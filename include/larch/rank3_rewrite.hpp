#pragma once

#include <larch/chart_spr.hpp>
#include <larch/merge.hpp>
#include <larch/thread_pool.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

// Phase 8 / Option A: safe materialization of a sidecar rewrite by generating
// one or more concrete trees, assigning compact genomes with the existing
// Fitch machinery, merging those trees back with the trusted merge path, and
// rebuilding every sidecar index from scratch.
//
// This is intentionally not an in-place bulk DAG rewrite.  The collapsed
// grammar still ignores compact-genome labels; concrete history nodes are
// created only in the generated tree(s), then normalized by merge.

struct rank3_rewrite_rule {
  production_id before = no_production;
  grammar_production after;
  std::vector<std::size_t> witness_nodes;
  std::vector<std::size_t> witness_edges;
  spr_score_result score;
};

struct rank3_topology {
  // selected_production_by_clade[c] is the production chosen for non-leaf clade
  // c. Leaf clades and unreachable clades contain no_production.
  std::vector<production_id> selected_production_by_clade;
  std::vector<bool> used_production;
};

struct rank3_production_taxa_key {
  std::vector<taxon_id> parent;
  std::vector<std::vector<taxon_id>> children;

  bool operator==(rank3_production_taxa_key const&) const = default;
  bool operator<(rank3_production_taxa_key const& other) const {
    return std::tie(parent, children) < std::tie(other.parent, other.children);
  }
};

struct rank3_option_a_options {
  // true: add generated trees to the current history DAG. false: build a new
  // DAG from only the generated tree(s).  The true default mirrors the current
  // optimize-and-merge pipeline and avoids destructive rewrites.
  bool include_original_dag = true;

  // Run validate_dag on generated trees and the merged DAG.  Tests keep this
  // enabled; callers doing large exploratory batches may disable it and rely on
  // existing merge/rebuild checks.
  bool validate = true;

  // Generated trees are fed through merge, which keeps the minimum stored
  // edge_weight for duplicate edge labels.  A very large default prevents a
  // generated duplicate edge from lowering an existing edge-weight objective.
  // Callers that intentionally want neutral generated edge weights can set
  // this to 0 explicitly.
  float generated_edge_weight = std::numeric_limits<float>::max();

  // Treat absence of any intended rewrite production after the full grammar
  // rebuild as a hard validation failure.  This is the safe Phase-8 default.
  bool require_intended_productions_present = true;

  // Rebuild options used after merge.  The Phase-1 defaults remain strict.
  clade_grammar_options rebuild_grammar_options = {};
};

struct rank3_option_a_result {
  phylo_dag dag;
  clade_grammar_build_result rebuilt;
  std::size_t materialized_tree_count = 0;

  // Intended productions are represented by taxon sets in the grammar used to
  // generate the concrete trees.  With deterministic sample-id taxon ordering,
  // these keys are stable across the full grammar rebuild.
  std::vector<rank3_production_taxa_key> intended_productions;
  std::vector<bool> intended_production_present;

  [[nodiscard]] bool all_intended_productions_present() const {
    return std::all_of(intended_production_present.begin(),
                       intended_production_present.end(),
                       [](bool present) { return present; });
  }
};

namespace rank3_detail {

inline void normalize_production_key(rank3_production_taxa_key& key) {
  std::sort(key.parent.begin(), key.parent.end());
  key.parent.erase(std::unique(key.parent.begin(), key.parent.end()),
                   key.parent.end());
  for (auto& child : key.children) {
    std::sort(child.begin(), child.end());
    child.erase(std::unique(child.begin(), child.end()), child.end());
  }
  std::sort(key.children.begin(), key.children.end());
}

inline void validate_same_taxa_for_rebuild(clade_grammar const& expected,
                                           clade_grammar const& rebuilt) {
  if (expected.taxa.id_to_sample_id.size() !=
      rebuilt.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(
        "rank3 option A: rebuilt grammar has different taxon count");
  }
  for (std::size_t i = 0; i < expected.taxa.id_to_sample_id.size(); ++i) {
    if (expected.taxa.id_to_sample_id[i] != rebuilt.taxa.id_to_sample_id[i]) {
      throw std::runtime_error(
          "rank3 option A: rebuilt grammar taxon order changed at index " +
          std::to_string(i) + " (expected '" +
          expected.taxa.id_to_sample_id[i] + "', got '" +
          rebuilt.taxa.id_to_sample_id[i] + "')");
    }
  }
}

inline rank3_production_taxa_key production_key_from_id(
    clade_grammar const& grammar, production_id pid) {
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error("rank3 option A: production id out of range");
  }
  auto const& prod = grammar.productions[pid];
  if (prod.parent == no_clade || prod.parent >= grammar.clades.size()) {
    throw std::runtime_error("rank3 option A: production parent out of range");
  }

  rank3_production_taxa_key key;
  key.parent = grammar.clades[prod.parent].taxa;
  key.children.reserve(prod.children.size());
  for (auto child : prod.children) {
    if (child == no_clade || child >= grammar.clades.size()) {
      throw std::runtime_error("rank3 option A: production child out of range");
    }
    key.children.push_back(grammar.clades[child].taxa);
  }
  normalize_production_key(key);
  return key;
}

inline bool has_production_key(clade_grammar const& grammar,
                               rank3_production_taxa_key key) {
  normalize_production_key(key);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (production_key_from_id(grammar, static_cast<production_id>(pid)) == key)
      return true;
  }
  return false;
}

inline void append_unique_key(std::vector<rank3_production_taxa_key>& keys,
                              rank3_production_taxa_key key) {
  normalize_production_key(key);
  if (std::find(keys.begin(), keys.end(), key) == keys.end())
    keys.push_back(std::move(key));
}

inline void validate_topology_shapes(clade_grammar const& grammar,
                                     rank3_topology const& topology) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error("rank3 option A: root clade out of range");
  }
  if (topology.selected_production_by_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "rank3 option A: topology clade-choice vector has wrong size");
  }
  if (topology.used_production.size() != grammar.productions.size()) {
    throw std::runtime_error(
        "rank3 option A: topology production-use vector has wrong size");
  }
}

inline void collect_reachable_topology_productions(
    clade_grammar const& grammar, rank3_topology const& topology,
    clade_id clade, std::vector<std::uint8_t>& clade_state,
    std::vector<bool>& reachable_production) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("rank3 option A: topology clade out of range");
  }
  if (clade_state[clade] == 1) {
    throw std::runtime_error("rank3 option A: cycle in selected topology");
  }
  if (clade_state[clade] == 2) {
    throw std::runtime_error(
        "rank3 option A: selected topology reuses a clade in two places; "
        "expected a concrete tree");
  }
  clade_state[clade] = 1;

  if (grammar.clades[clade].taxa.size() == 1) {
    if (topology.selected_production_by_clade[clade] != no_production) {
      throw std::runtime_error(
          "rank3 option A: singleton clade has a selected production");
    }
    clade_state[clade] = 2;
    return;
  }

  auto pid = topology.selected_production_by_clade[clade];
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error(
        "rank3 option A: selected topology is incomplete at clade " +
        std::to_string(clade));
  }
  auto const& prod = grammar.productions[pid];
  if (prod.parent != clade) {
    throw std::runtime_error(
        "rank3 option A: selected production parent mismatch at clade " +
        std::to_string(clade));
  }
  if (prod.children.size() != 2) {
    throw std::runtime_error(
        "rank3 option A: selected production " + std::to_string(pid) +
        " has arity " + std::to_string(prod.children.size()) +
        "; Option A materializes binary concrete trees only");
  }
  chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
  reachable_production[pid] = true;

  for (auto child : prod.children) {
    collect_reachable_topology_productions(grammar, topology, child,
                                           clade_state,
                                           reachable_production);
  }
  clade_state[clade] = 2;
}

inline std::vector<bool> validate_topology(clade_grammar const& grammar,
                                           rank3_topology const& topology) {
  validate_topology_shapes(grammar, topology);
  std::vector<std::uint8_t> clade_state(grammar.clades.size(), 0);
  std::vector<bool> reachable_production(grammar.productions.size(), false);
  collect_reachable_topology_productions(grammar, topology, grammar.root_clade,
                                         clade_state, reachable_production);

  for (std::size_t clade = 0; clade < clade_state.size(); ++clade) {
    if (clade_state[clade] == 0 &&
        topology.selected_production_by_clade[clade] != no_production) {
      throw std::runtime_error(
          "rank3 option A: unreachable clade " + std::to_string(clade) +
          " has a selected production");
    }
  }

  for (std::size_t pid = 0; pid < reachable_production.size(); ++pid) {
    if (topology.used_production[pid] != reachable_production[pid]) {
      throw std::runtime_error(
          "rank3 option A: used_production does not match reachable "
          "selected productions at production " +
          std::to_string(pid));
    }
  }
  return reachable_production;
}

inline std::vector<compact_genome> collect_leaf_compact_genomes(
    phylo_dag& source, clade_grammar const& grammar) {
  std::vector<compact_genome> result(grammar.taxa.id_to_sample_id.size());
  std::vector<bool> seen(grammar.taxa.id_to_sample_id.size(), false);

  auto reachable = larch::detail::collect_reachable(source);
  for (auto node_idx : reachable.nodes) {
    auto nv = source.get_node(node_idx);
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            std::string sample_id{node.sample_id()};
            auto it = grammar.taxa.sample_id_to_id.find(sample_id);
            if (it == grammar.taxa.sample_id_to_id.end()) return;
            auto tid = it->second;
            if (tid >= result.size()) {
              throw std::runtime_error(
                  "rank3 option A: taxon id out of range for sample '" +
                  sample_id + "'");
            }
            if (seen[tid] && !(result[tid] == node.cg())) {
              throw std::runtime_error(
                  "rank3 option A: conflicting compact genomes for duplicate "
                  "sample_id '" +
                  sample_id + "'");
            }
            result[tid] = node.cg();
            seen[tid] = true;
          }
        },
        nv);
  }

  for (std::size_t tid = 0; tid < seen.size(); ++tid) {
    if (!seen[tid]) {
      throw std::runtime_error(
          "rank3 option A: source DAG is missing compact genome for taxon '" +
          grammar.taxa.id_to_sample_id[tid] + "'");
    }
  }
  return result;
}

inline std::size_t add_tree_edge(phylo_dag& tree, std::size_t parent_idx,
                                 std::size_t child_idx,
                                 std::size_t clade_index,
                                 float edge_weight) {
  auto edge = tree.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_index;
  edge.edge_weight() = edge_weight;
  std::visit([&](auto parent) { edge.set_parent(parent); },
             tree.get_node(parent_idx));
  std::visit([&](auto child) { edge.set_child(child); },
             tree.get_node(child_idx));
  return edge.index();
}

inline std::size_t build_tree_subtree(
    phylo_dag& tree, clade_grammar const& grammar,
    rank3_topology const& topology,
    std::vector<compact_genome> const& leaf_cgs, clade_id clade,
    std::vector<std::uint8_t>& state, float generated_edge_weight) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("rank3 option A: subtree clade out of range");
  }
  if (state[clade] == 1) {
    throw std::runtime_error("rank3 option A: cycle in selected topology");
  }
  if (state[clade] == 2) {
    throw std::runtime_error(
        "rank3 option A: selected topology reuses a clade in two places; "
        "expected a concrete tree");
  }
  state[clade] = 1;

  auto const& key = grammar.clades[clade];
  std::size_t node_idx = no_idx;
  if (key.taxa.size() == 1) {
    auto taxon = key.taxa.front();
    if (taxon >= grammar.taxa.id_to_sample_id.size() ||
        taxon >= leaf_cgs.size()) {
      throw std::runtime_error("rank3 option A: leaf taxon out of range");
    }
    auto leaf = tree.append_node<node_kind::leaf>();
    leaf.sample_id() = grammar.taxa.id_to_sample_id[taxon];
    leaf.cg() = leaf_cgs[taxon];
    node_idx = leaf.index();
  } else {
    auto pid = topology.selected_production_by_clade[clade];
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "rank3 option A: selected topology has no production for "
          "non-singleton clade " +
          std::to_string(clade));
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "rank3 option A: selected production parent mismatch");
    }
    if (prod.children.size() != 2) {
      throw std::runtime_error(
          "rank3 option A: selected production is not a binary concrete "
          "branching");
    }
    auto inner = tree.append_node<node_kind::inner>();
    node_idx = inner.index();
    for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
      auto child_idx = build_tree_subtree(tree, grammar, topology, leaf_cgs,
                                          prod.children[child_i], state,
                                          generated_edge_weight);
      add_tree_edge(tree, node_idx, child_idx, child_i,
                    generated_edge_weight);
    }
  }

  state[clade] = 2;
  return node_idx;
}

inline void validate_tree_if_requested(phylo_dag& tree,
                                       std::string_view label,
                                       bool validate) {
  if (!validate) return;
  build_clade_offsets(tree);
  if (!is_tree(tree)) {
    throw std::runtime_error("rank3 option A: materialized DAG is not a tree");
  }
  validate_dag(tree, label, thread_pool::get_default());
}

inline std::vector<rank3_production_taxa_key> intended_keys_from_topologies(
    clade_grammar const& grammar,
    std::vector<rank3_topology> const& topologies) {
  std::vector<rank3_production_taxa_key> keys;
  for (auto const& topology : topologies) {
    auto reachable = validate_topology(grammar, topology);
    for (std::size_t pid = 0; pid < reachable.size(); ++pid) {
      if (!reachable[pid]) continue;
      append_unique_key(keys, production_key_from_id(
                                  grammar, static_cast<production_id>(pid)));
    }
  }
  return keys;
}

inline void append_intended_temp_overlay_keys(
    overlay_materialization_result const& materialized,
    std::vector<rank3_production_taxa_key>& keys) {
  for (auto dense_pid : materialized.temp_production_to_dense) {
    if (dense_pid == no_production) continue;
    append_unique_key(keys,
                      production_key_from_id(materialized.grammar, dense_pid));
  }
}

inline std::vector<bool> production_key_presence(
    clade_grammar const& grammar,
    std::vector<rank3_production_taxa_key> const& keys) {
  std::vector<bool> present;
  present.reserve(keys.size());
  for (auto key : keys) present.push_back(has_production_key(grammar, key));
  return present;
}

inline std::string production_key_to_string(rank3_production_taxa_key const& key) {
  auto taxa_to_string = [](std::vector<taxon_id> const& taxa) {
    std::string out{"{"};
    for (std::size_t i = 0; i < taxa.size(); ++i) {
      if (i != 0) out += ",";
      out += std::to_string(taxa[i]);
    }
    out += "}";
    return out;
  };

  std::string out = taxa_to_string(key.parent) + "->";
  for (std::size_t i = 0; i < key.children.size(); ++i) {
    if (i != 0) out += "|";
    out += taxa_to_string(key.children[i]);
  }
  return out;
}

inline void enforce_intended_productions_present(
    rank3_option_a_result const& result, rank3_option_a_options const& options) {
  if (!options.require_intended_productions_present) return;
  if (result.intended_productions.size() !=
      result.intended_production_present.size()) {
    throw std::runtime_error(
        "rank3 option A: internal intended-production presence size mismatch");
  }
  for (std::size_t i = 0; i < result.intended_productions.size(); ++i) {
    if (result.intended_production_present[i]) continue;
    throw std::runtime_error(
        "rank3 option A: intended production missing after grammar rebuild: " +
        production_key_to_string(result.intended_productions[i]));
  }
}

}  // namespace rank3_detail

inline rank3_topology make_empty_rank3_topology(
    clade_grammar const& grammar) {
  rank3_topology topology;
  topology.selected_production_by_clade.assign(grammar.clades.size(),
                                               no_production);
  topology.used_production.assign(grammar.productions.size(), false);
  return topology;
}

inline rank3_topology rank3_topology_from_productions(
    clade_grammar const& grammar,
    std::vector<production_id> const& productions) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);

  auto topology = make_empty_rank3_topology(grammar);
  for (auto pid : productions) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("rank3 option A: traceback production out of range");
    }
    auto parent = grammar.productions[pid].parent;
    if (parent == no_clade || parent >= grammar.clades.size()) {
      throw std::runtime_error("rank3 option A: traceback parent out of range");
    }
    auto& selected = topology.selected_production_by_clade[parent];
    if (selected != no_production && selected != pid) {
      throw std::runtime_error(
          "rank3 option A: conflicting production choices for clade " +
          std::to_string(parent));
    }
    selected = pid;
    topology.used_production[pid] = true;
  }
  return topology;
}

inline rank3_topology rank3_topology_from_traceback(
    clade_grammar const& grammar, chart_traceback_result const& trace) {
  return rank3_topology_from_productions(grammar, trace.productions);
}

inline void fill_first_rank3_topology(clade_grammar const& grammar,
                                      clade_id clade,
                                      rank3_topology& topology) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("rank3 option A: clade out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1) return;
  auto const& productions = grammar.productions_by_parent[clade];
  if (productions.empty()) {
    throw std::runtime_error(
        "rank3 option A: non-singleton clade has no production");
  }
  auto pid = productions.front();
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error("rank3 option A: first production out of range");
  }
  auto const& prod = grammar.productions[pid];
  if (prod.parent != clade) {
    throw std::runtime_error("rank3 option A: first production parent mismatch");
  }
  topology.selected_production_by_clade[clade] = pid;
  topology.used_production[pid] = true;
  for (auto child : prod.children) fill_first_rank3_topology(grammar, child, topology);
}

inline rank3_topology first_rank3_topology(clade_grammar const& grammar) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  auto topology = make_empty_rank3_topology(grammar);
  fill_first_rank3_topology(grammar, grammar.root_clade, topology);
  (void)rank3_detail::validate_topology(grammar, topology);
  return topology;
}

inline rank3_topology rank3_topology_preferring_productions(
    clade_grammar const& grammar,
    std::vector<production_id> const& preferred_productions) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);

  std::vector<bool> preferred(grammar.productions.size(), false);
  for (auto pid : preferred_productions) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "rank3 option A: preferred production out of range");
    }
    preferred[pid] = true;
  }

  auto topology = make_empty_rank3_topology(grammar);
  auto fill = [&](auto&& self, clade_id clade) -> void {
    if (clade == no_clade || clade >= grammar.clades.size()) {
      throw std::runtime_error("rank3 option A: clade out of range");
    }
    if (grammar.clades[clade].taxa.size() == 1) return;

    auto const& productions = grammar.productions_by_parent[clade];
    if (productions.empty()) {
      throw std::runtime_error(
          "rank3 option A: non-singleton clade has no production");
    }

    production_id chosen = no_production;
    for (auto pid : productions) {
      if (!preferred[pid]) continue;
      if (chosen != no_production && chosen != pid) {
        throw std::runtime_error(
            "rank3 option A: multiple preferred productions share parent "
            "clade " +
            std::to_string(clade) +
            "; supply an explicit concrete topology");
      }
      chosen = pid;
    }
    if (chosen == no_production) chosen = productions.front();

    auto const& prod = grammar.productions[chosen];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "rank3 option A: preferred production parent mismatch");
    }
    topology.selected_production_by_clade[clade] = chosen;
    topology.used_production[chosen] = true;
    for (auto child : prod.children) self(self, child);
  };

  fill(fill, grammar.root_clade);
  auto reachable = rank3_detail::validate_topology(grammar, topology);
  for (std::size_t pid = 0; pid < preferred.size(); ++pid) {
    if (!preferred[pid]) continue;
    if (!reachable[pid]) {
      throw std::runtime_error(
          "rank3 option A: preferred production " + std::to_string(pid) +
          " is not reachable in one concrete topology; supply an explicit "
          "topology");
    }
  }
  return topology;
}

inline rank3_topology deterministic_optimal_rank3_topology(
    clade_grammar const& grammar, leaf_site_states const& leaf_states,
    chart_options options = {}) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "rank3 option A: reference state is required when score_ua_edge is true");
  }
  auto chart = build_single_site_chart(grammar, leaf_states, options);
  auto outside = build_single_site_outside_chart(grammar, chart, options);
  return rank3_topology_from_traceback(
      grammar, deterministic_optimal_single_site_traceback(grammar, chart, outside));
}

inline rank3_topology deterministic_optimal_rank3_topology(
    clade_grammar const& grammar, leaf_site_states const& leaf_states,
    chart_options options, std::uint8_t reference_state) {
  auto chart = build_single_site_chart(grammar, leaf_states, options);
  auto outside = build_single_site_outside_chart(grammar, chart, options,
                                                reference_state);
  return rank3_topology_from_traceback(
      grammar, deterministic_optimal_single_site_traceback(grammar, chart, outside));
}

inline phylo_dag materialize_rank3_tree_from_topology(
    phylo_dag& source, clade_grammar const& grammar,
    rank3_topology const& topology, bool validate = true,
    float generated_edge_weight = std::numeric_limits<float>::max()) {
  (void)rank3_detail::validate_topology(grammar, topology);
  auto leaf_cgs = rank3_detail::collect_leaf_compact_genomes(source, grammar);

  phylo_dag tree;
  auto ua = tree.append_node<node_kind::ua>();
  ua.reference_sequence() = get_reference_sequence(source);
  tree.set_root(ua);

  std::vector<std::uint8_t> state(grammar.clades.size(), 0);
  auto root_child = rank3_detail::build_tree_subtree(
      tree, grammar, topology, leaf_cgs, grammar.root_clade, state,
      generated_edge_weight);
  rank3_detail::add_tree_edge(tree, ua.index(), root_child, 0,
                              generated_edge_weight);

  build_clade_offsets(tree);
  fitch_assign_compact_genomes(tree);
  recompute_edge_mutations(tree);
  build_clade_offsets(tree);
  rank3_detail::validate_tree_if_requested(tree, "rank3 materialized tree",
                                           validate);
  return tree;
}

inline std::vector<phylo_dag> materialize_rank3_trees_from_topologies(
    phylo_dag& source, clade_grammar const& grammar,
    std::vector<rank3_topology> const& topologies, bool validate = true,
    float generated_edge_weight = std::numeric_limits<float>::max()) {
  if (topologies.empty()) {
    throw std::runtime_error("rank3 option A: no topologies to materialize");
  }
  std::vector<phylo_dag> trees;
  trees.reserve(topologies.size());
  for (auto const& topology : topologies) {
    trees.push_back(materialize_rank3_tree_from_topology(
        source, grammar, topology, validate, generated_edge_weight));
  }
  return trees;
}

inline rank3_option_a_result merge_rank3_trees_option_a(
    phylo_dag& source, clade_grammar const& topology_grammar,
    std::vector<rank3_topology> const& topologies,
    rank3_option_a_options const& options = {}) {
  if (topologies.empty()) {
    throw std::runtime_error("rank3 option A: no topologies to merge");
  }

  auto trees = materialize_rank3_trees_from_topologies(
      source, topology_grammar, topologies, options.validate,
      options.generated_edge_weight);

  merge merger{get_reference_sequence(source)};
  if (options.include_original_dag) {
    build_clade_offsets(source);
    merger.add_dag(source);
  }
  for (auto& tree : trees) {
    if (get_reference_sequence(tree) != get_reference_sequence(source)) {
      throw std::runtime_error("rank3 option A: generated tree reference mismatch");
    }
    build_clade_offsets(tree);
    merger.add_dag(tree);
  }

  rank3_option_a_result result;
  result.materialized_tree_count = trees.size();
  result.intended_productions =
      rank3_detail::intended_keys_from_topologies(topology_grammar, topologies);
  result.dag = std::move(merger.get_result());
  build_clade_offsets(result.dag);
  if (options.validate)
    validate_dag(result.dag, "rank3 option A merged DAG",
                 thread_pool::get_default());
  result.rebuilt = build_clade_grammar_with_audit(
      result.dag, options.rebuild_grammar_options);
  rank3_detail::validate_same_taxa_for_rebuild(topology_grammar,
                                               result.rebuilt.grammar);
  result.intended_production_present = rank3_detail::production_key_presence(
      result.rebuilt.grammar, result.intended_productions);
  rank3_detail::enforce_intended_productions_present(result, options);
  return result;
}

inline rank3_option_a_result materialize_rank3_option_a(
    phylo_dag& source, clade_grammar const& topology_grammar,
    rank3_topology const& topology,
    rank3_option_a_options const& options = {}) {
  return merge_rank3_trees_option_a(source, topology_grammar,
                                    std::vector<rank3_topology>{topology},
                                    options);
}

inline rank3_option_a_result materialize_rank3_option_a(
    phylo_dag& source, clade_grammar const& topology_grammar,
    std::vector<rank3_topology> const& topologies,
    rank3_option_a_options const& options = {}) {
  return merge_rank3_trees_option_a(source, topology_grammar, topologies,
                                    options);
}

inline rank3_option_a_result materialize_rank3_option_a(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate,
    std::vector<rank3_topology> const& dense_overlay_topologies,
    rank3_option_a_options const& options = {}) {
  auto overlay = overlay_from_candidate(base_grammar, candidate);
  auto materialized = materialize_overlay_grammar(overlay);

  auto topologies = dense_overlay_topologies;
  if (topologies.empty()) {
    std::vector<production_id> preferred_temp_productions;
    preferred_temp_productions.reserve(materialized.temp_production_to_dense.size());
    for (auto dense_pid : materialized.temp_production_to_dense) {
      if (dense_pid != no_production)
        preferred_temp_productions.push_back(dense_pid);
    }
    if (preferred_temp_productions.empty()) {
      topologies.push_back(first_rank3_topology(materialized.grammar));
    } else {
      topologies.push_back(rank3_topology_preferring_productions(
          materialized.grammar, preferred_temp_productions));
    }
  }

  auto result = merge_rank3_trees_option_a(source, materialized.grammar,
                                           topologies, options);
  // A candidate's temporary productions are the productions introduced by the
  // rewrite.  Keep them in the validation list even if the chosen concrete
  // topology has additional pre-existing productions.
  rank3_detail::append_intended_temp_overlay_keys(materialized,
                                                  result.intended_productions);
  result.intended_production_present = rank3_detail::production_key_presence(
      result.rebuilt.grammar, result.intended_productions);
  rank3_detail::enforce_intended_productions_present(result, options);
  return result;
}

inline rank3_option_a_result materialize_rank3_option_a(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate,
    rank3_option_a_options const& options = {}) {
  return materialize_rank3_option_a(source, base_grammar, candidate,
                                    std::vector<rank3_topology>{}, options);
}

inline rank3_option_a_result materialize_rank3_option_a_single_site(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate, leaf_site_states const& leaf_states,
    chart_options chart_opts = {},
    rank3_option_a_options const& options = {}) {
  if (chart_opts.score_ua_edge) {
    throw std::runtime_error(
        "rank3 option A: reference state overload is required when "
        "score_ua_edge is true");
  }
  auto overlay = overlay_from_candidate(base_grammar, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  auto topology = deterministic_optimal_rank3_topology(materialized.grammar,
                                                       leaf_states,
                                                       chart_opts);
  return materialize_rank3_option_a(source, base_grammar, candidate,
                                    std::vector<rank3_topology>{topology},
                                    options);
}

inline rank3_option_a_result materialize_rank3_option_a_single_site(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate, leaf_site_states const& leaf_states,
    chart_options chart_opts, std::uint8_t reference_state,
    rank3_option_a_options const& options = {}) {
  auto overlay = overlay_from_candidate(base_grammar, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  auto topology = deterministic_optimal_rank3_topology(
      materialized.grammar, leaf_states, chart_opts, reference_state);
  return materialize_rank3_option_a(source, base_grammar, candidate,
                                    std::vector<rank3_topology>{topology},
                                    options);
}

}  // namespace larch

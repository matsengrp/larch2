#pragma once

#include <larch/chart_spr.hpp>
#include <larch/merge.hpp>
#include <larch/overlay_spr.hpp>
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

// Phase 8 rank-3 rewrite prototypes.
//
// Option A: safe materialization of a sidecar rewrite by generating one or
// more concrete trees, assigning compact genomes with the existing Fitch
// machinery, merging those trees back with the trusted merge path, and
// rebuilding every sidecar index from scratch.
//
// Option B: stage a topology rewrite in an overlay_dag/overlay_spr sidecar and
// materialize only accepted/validated overlay changes.  This is still not the
// direct in-place bulk rewrite from Option C; materialized overlay trees are
// normalized through the same merge and grammar-rebuild validation gates.

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

struct rank3_option_b_options {
  // Like Option A, the conservative default adds the accepted overlay tree to
  // the current history DAG via merge.  Set false to materialize a replacement
  // DAG from only the overlay tree.
  bool include_original_dag = true;

  // Validate the staged/materialized tree and final merged DAG.
  bool validate = true;

  // New overlay edges are synthetic; make their merge weight conservative so
  // they do not lower an existing edge-weight objective by default.
  float added_edge_weight = std::numeric_limits<float>::max();

  // A grammar_spr_candidate materialized through Option B must carry a
  // source_tree_move: grammar-native Option B staging is not implemented yet.
  // When enabled, re-project that concrete move against the supplied base
  // grammar and require it to describe the same source-tree-local rewrite as
  // the candidate.  This deliberately does not compare against the full
  // materialized collapsed-DAG overlay, which may contain unrelated alternative
  // productions from the base grammar.
  bool validate_projected_candidate = true;

  // Treat absence of intended overlay-added productions after the full grammar
  // rebuild as a hard validation failure.
  bool require_intended_productions_present = true;

  clade_grammar_options rebuild_grammar_options = {};
};

struct rank3_option_b_result {
  phylo_dag dag;
  clade_grammar_build_result rebuilt;
  std::size_t materialized_tree_count = 0;
  bool staged_in_overlay = false;
  bool used_source_tree_move = false;

  std::vector<rank3_production_taxa_key> intended_productions;
  std::vector<bool> intended_production_present;

  [[nodiscard]] bool all_intended_productions_present() const {
    return std::all_of(intended_production_present.begin(),
                       intended_production_present.end(),
                       [](bool present) { return present; });
  }
};

class rank3_option_b_stage {
 public:
  phylo_dag* base = nullptr;
  overlay_dag<phylo_dag> overlay;
  spr_move move;
  std::size_t fragment_root = no_idx;
  float added_edge_weight = std::numeric_limits<float>::max();

  rank3_option_b_stage(phylo_dag& base_, spr_move move_,
                       float added_edge_weight_)
      : base(&base_),
        overlay(base_),
        move(move_),
        added_edge_weight(added_edge_weight_) {
    fragment_root = apply_spr_topology(overlay, base_, move_);
    for (auto ev : overlay.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            if (overlay.is_added_edge(edge.index()))
              edge.edge_weight() = added_edge_weight;
          },
          ev);
    }
  }

  rank3_option_b_stage(rank3_option_b_stage const&) = delete;
  rank3_option_b_stage& operator=(rank3_option_b_stage const&) = delete;
  rank3_option_b_stage(rank3_option_b_stage&&) = default;
  rank3_option_b_stage& operator=(rank3_option_b_stage&&) = delete;
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

inline void validate_same_taxa_for_rank3(std::string_view context,
                                          clade_grammar const& expected,
                                          clade_grammar const& rebuilt) {
  if (expected.taxa.id_to_sample_id.size() !=
      rebuilt.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(std::string{context} +
                             ": rebuilt grammar has different taxon count");
  }
  for (std::size_t i = 0; i < expected.taxa.id_to_sample_id.size(); ++i) {
    if (expected.taxa.id_to_sample_id[i] != rebuilt.taxa.id_to_sample_id[i]) {
      throw std::runtime_error(
          std::string{context} +
          ": rebuilt grammar taxon order changed at index " +
          std::to_string(i) + " (expected '" +
          expected.taxa.id_to_sample_id[i] + "', got '" +
          rebuilt.taxa.id_to_sample_id[i] + "')");
    }
  }
}

inline void validate_same_taxa_for_rebuild(clade_grammar const& expected,
                                           clade_grammar const& rebuilt) {
  validate_same_taxa_for_rank3("rank3 option A", expected, rebuilt);
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

inline std::set<rank3_production_taxa_key> production_key_set(
    clade_grammar const& grammar) {
  std::set<rank3_production_taxa_key> keys;
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    keys.insert(production_key_from_id(grammar,
                                       static_cast<production_id>(pid)));
  }
  return keys;
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

inline void validate_same_production_key_set(
    std::string_view context, clade_grammar const& expected,
    clade_grammar const& observed) {
  validate_same_taxa_for_rank3(context, expected, observed);
  auto expected_keys = production_key_set(expected);
  auto observed_keys = production_key_set(observed);
  if (expected_keys == observed_keys) return;

  for (auto const& key : expected_keys) {
    if (!observed_keys.contains(key)) {
      throw std::runtime_error(std::string{context} +
                               ": missing expected production " +
                               production_key_to_string(key));
    }
  }
  for (auto const& key : observed_keys) {
    if (!expected_keys.contains(key)) {
      throw std::runtime_error(std::string{context} +
                               ": observed unexpected production " +
                               production_key_to_string(key));
    }
  }
  throw std::runtime_error(std::string{context} +
                           ": production key sets differ");
}

inline std::vector<taxon_id> candidate_taxa_for_ref(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_clade_ref ref, std::string_view label) {
  std::vector<taxon_id> taxa;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= base.clades.size()) {
      throw std::runtime_error(std::string{label} +
                               ": base clade ref out of range");
    }
    taxa = base.clades[ref.id].taxa;
  } else {
    if (ref.id == no_clade || ref.id >= candidate.added_clades.size()) {
      throw std::runtime_error(std::string{label} +
                               ": temp clade ref out of range");
    }
    taxa = candidate.added_clades[ref.id].taxa;
  }
  std::sort(taxa.begin(), taxa.end());
  taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
  return taxa;
}

inline std::set<std::vector<taxon_id>> candidate_added_clade_taxa_set(
    grammar_spr_candidate const& candidate) {
  std::set<std::vector<taxon_id>> keys;
  for (auto const& clade : candidate.added_clades) {
    auto taxa = clade.taxa;
    std::sort(taxa.begin(), taxa.end());
    taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
    keys.insert(std::move(taxa));
  }
  return keys;
}

inline std::set<production_id> candidate_removed_base_production_set(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::string_view context) {
  std::set<production_id> removed;
  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(std::string{context} +
                               ": candidate tombstones a temp production");
    }
    if (ref.id == no_production || ref.id >= base.productions.size()) {
      throw std::runtime_error(std::string{context} +
                               ": removed base production out of range");
    }
    removed.insert(ref.id);
  }
  return removed;
}

inline rank3_production_taxa_key candidate_added_production_key(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_grammar_production const& prod, std::string_view context) {
  rank3_production_taxa_key key;
  key.parent = candidate_taxa_for_ref(base, candidate, prod.parent, context);
  key.children.reserve(prod.children.size());
  for (auto child : prod.children) {
    key.children.push_back(
        candidate_taxa_for_ref(base, candidate, child, context));
  }
  normalize_production_key(key);
  return key;
}

inline std::set<rank3_production_taxa_key> candidate_added_production_key_set(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::string_view context) {
  std::set<rank3_production_taxa_key> keys;
  for (auto const& prod : candidate.added_productions) {
    keys.insert(candidate_added_production_key(base, candidate, prod, context));
  }
  return keys;
}

inline void validate_candidate_matches_projected_move(
    std::string_view context, clade_grammar const& base,
    grammar_spr_candidate const& candidate,
    grammar_spr_candidate const& projected) {
  auto same_clade_field = [&](overlay_clade_ref lhs_ref,
                              grammar_spr_candidate const& lhs_candidate,
                              overlay_clade_ref rhs_ref,
                              grammar_spr_candidate const& rhs_candidate,
                              std::string_view field) {
    auto lhs = candidate_taxa_for_ref(base, lhs_candidate, lhs_ref, field);
    auto rhs = candidate_taxa_for_ref(base, rhs_candidate, rhs_ref, field);
    if (lhs != rhs) {
      throw std::runtime_error(std::string{context} +
                               ": candidate " + std::string{field} +
                               " does not match source_tree_move projection");
    }
  };

  same_clade_field(candidate.moved_clade, candidate, projected.moved_clade,
                   projected, "moved_clade");
  same_clade_field(candidate.old_parent, candidate, projected.old_parent,
                   projected, "old_parent");
  same_clade_field(candidate.old_sibling, candidate, projected.old_sibling,
                   projected, "old_sibling");
  same_clade_field(candidate.new_sibling_or_target, candidate,
                   projected.new_sibling_or_target, projected,
                   "new_sibling_or_target");

  auto candidate_removed = candidate_removed_base_production_set(
      base, candidate, context);
  auto projected_removed = candidate_removed_base_production_set(
      base, projected, context);
  if (candidate_removed != projected_removed) {
    throw std::runtime_error(std::string{context} +
                             ": removed productions do not match "
                             "source_tree_move projection");
  }

  if (candidate_added_clade_taxa_set(candidate) !=
      candidate_added_clade_taxa_set(projected)) {
    throw std::runtime_error(std::string{context} +
                             ": added clades do not match "
                             "source_tree_move projection");
  }

  auto candidate_added = candidate_added_production_key_set(base, candidate,
                                                           context);
  auto projected_added = candidate_added_production_key_set(base, projected,
                                                           context);
  if (candidate_added != projected_added) {
    throw std::runtime_error(std::string{context} +
                             ": added productions do not match "
                             "source_tree_move projection");
  }
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

inline void enforce_intended_productions_present(
    rank3_option_b_result const& result, rank3_option_b_options const& options) {
  if (!options.require_intended_productions_present) return;
  if (result.intended_productions.size() !=
      result.intended_production_present.size()) {
    throw std::runtime_error(
        "rank3 option B: internal intended-production presence size mismatch");
  }
  for (std::size_t i = 0; i < result.intended_productions.size(); ++i) {
    if (result.intended_production_present[i]) continue;
    throw std::runtime_error(
        "rank3 option B: intended production missing after grammar rebuild: " +
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

inline spr_move rank3_option_b_validated_move(phylo_dag& source,
                                              spr_move move) {
  build_clade_offsets(source);
  if (!is_tree(source)) {
    throw std::runtime_error(
        "rank3 option B: overlay_spr materialization requires a concrete "
        "source tree");
  }
  tree_index index{source};
  if (!index.is_valid(move.src) || !index.is_valid(move.dst)) {
    throw std::runtime_error("rank3 option B: source SPR move node out of range");
  }
  if (move.src == move.dst || move.src == index.get_tree_root()) {
    throw std::runtime_error("rank3 option B: invalid source SPR move");
  }
  if (index.is_ancestor(move.src, move.dst)) {
    throw std::runtime_error(
        "rank3 option B: source SPR move would attach inside moved subtree");
  }
  move.lca = compute_lca(source, move.src, move.dst);
  return move;
}

inline rank3_option_b_stage stage_rank3_option_b_overlay(
    phylo_dag& source, spr_move move,
    rank3_option_b_options const& options = {}) {
  move = rank3_option_b_validated_move(source, move);
  return rank3_option_b_stage{source, move, options.added_edge_weight};
}

inline rank3_option_b_stage stage_rank3_option_b_overlay(
    phylo_dag&, grammar_spr_candidate const&,
    rank3_option_b_options const& = {}) {
  throw std::runtime_error(
      "rank3 option B: staging a grammar_spr_candidate requires the base "
      "grammar for source_tree_move projection validation; call "
      "stage_rank3_option_b_overlay(source_tree, base_grammar, candidate)");
}

inline spr_move rank3_option_b_validated_candidate_move(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate,
    rank3_option_b_options const& options = {}) {
  if (!candidate.source_tree_move.has_value()) {
    throw std::runtime_error(
        "rank3 option B: grammar_spr_candidate must carry source_tree_move; "
        "grammar-native Option B materialization is not implemented yet; use "
        "Option A or bootstrap/project a concrete tree SPR move first");
  }

  auto move = rank3_option_b_validated_move(source,
                                            *candidate.source_tree_move);
  if (options.validate_projected_candidate) {
    auto projected = project_tree_spr_move_to_candidate(base_grammar, source,
                                                       move);
    if (!projected.has_value()) {
      throw std::runtime_error(
          "rank3 option B: source_tree_move cannot be projected into the "
          "supplied base grammar");
    }
    rank3_detail::validate_candidate_matches_projected_move(
        "rank3 option B candidate projection validation", base_grammar,
        candidate, *projected);
  }
  return move;
}

inline rank3_option_b_stage stage_rank3_option_b_overlay(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate,
    rank3_option_b_options const& options = {}) {
  auto move = rank3_option_b_validated_candidate_move(source, base_grammar,
                                                      candidate, options);
  return stage_rank3_option_b_overlay(source, move, options);
}

inline phylo_dag materialize_rank3_option_b_overlay_tree(
    rank3_option_b_stage& stage, bool validate = true) {
  if (stage.base == nullptr) {
    throw std::runtime_error("rank3 option B: staged overlay has no base DAG");
  }

  scoped_arena<4096> arena;
  auto* mr = arena.get();
  auto root_idx = std::visit([](auto node) { return node.index(); },
                             stage.overlay.get_root());
  auto ua_children = overlay_detail::get_child_edges_of(stage.overlay,
                                                        root_idx, mr);
  if (ua_children.empty()) {
    throw std::runtime_error(
        "rank3 option B: staged overlay root has no materializable child");
  }
  auto tree_root = overlay_detail::get_child_idx_of(stage.overlay,
                                                    ua_children[0]);
  auto tree = copy_subtree_from_overlay(stage.overlay, *stage.base, tree_root,
                                        mr);
  for (auto ev : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto parent_idx = get_parent_idx(tree, edge.index());
          if (is_ua(tree, parent_idx))
            edge.edge_weight() = stage.added_edge_weight;
        },
        ev);
  }

  build_clade_offsets(tree);
  fitch_assign_compact_genomes(tree);
  recompute_edge_mutations(tree);
  build_clade_offsets(tree);
  rank3_detail::validate_tree_if_requested(
      tree, "rank3 option B materialized overlay tree", validate);
  return tree;
}

inline rank3_option_b_result merge_rank3_overlay_tree_option_b(
    phylo_dag& source, clade_grammar const& base_grammar,
    phylo_dag& overlay_tree,
    std::vector<rank3_production_taxa_key> intended_productions,
    rank3_option_b_options const& options = {}) {
  if (get_reference_sequence(overlay_tree) != get_reference_sequence(source)) {
    throw std::runtime_error(
        "rank3 option B: materialized overlay tree reference mismatch");
  }

  merge merger{get_reference_sequence(source)};
  if (options.include_original_dag) {
    build_clade_offsets(source);
    merger.add_dag(source);
  }
  build_clade_offsets(overlay_tree);
  merger.add_dag(overlay_tree);

  rank3_option_b_result result;
  result.materialized_tree_count = 1;
  result.staged_in_overlay = true;
  result.dag = std::move(merger.get_result());
  build_clade_offsets(result.dag);
  if (options.validate) {
    validate_dag(result.dag, "rank3 option B merged DAG",
                 thread_pool::get_default());
  }
  result.rebuilt = build_clade_grammar_with_audit(
      result.dag, options.rebuild_grammar_options);
  rank3_detail::validate_same_taxa_for_rank3("rank3 option B", base_grammar,
                                             result.rebuilt.grammar);
  result.intended_productions = std::move(intended_productions);
  result.intended_production_present = rank3_detail::production_key_presence(
      result.rebuilt.grammar, result.intended_productions);
  rank3_detail::enforce_intended_productions_present(result, options);
  return result;
}

inline rank3_option_b_result materialize_rank3_option_b(
    phylo_dag& source, clade_grammar const& base_grammar, spr_move move,
    rank3_option_b_options const& options = {}) {
  auto stage = stage_rank3_option_b_overlay(source, move, options);
  auto tree = materialize_rank3_option_b_overlay_tree(stage, options.validate);
  auto tree_grammar = build_clade_grammar(tree, options.rebuild_grammar_options);

  std::vector<rank3_production_taxa_key> intended;
  for (std::size_t pid = 0; pid < tree_grammar.productions.size(); ++pid) {
    rank3_detail::append_unique_key(
        intended, rank3_detail::production_key_from_id(
                      tree_grammar, static_cast<production_id>(pid)));
  }

  auto result = merge_rank3_overlay_tree_option_b(
      source, base_grammar, tree, std::move(intended), options);
  result.used_source_tree_move = true;
  return result;
}

inline rank3_option_b_result materialize_rank3_option_b(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate,
    rank3_option_b_options const& options = {}) {
  auto stage = stage_rank3_option_b_overlay(source, base_grammar, candidate,
                                            options);
  auto tree = materialize_rank3_option_b_overlay_tree(stage, options.validate);

  auto candidate_overlay = overlay_from_candidate(base_grammar, candidate);
  auto expected_overlay = materialize_overlay_grammar(candidate_overlay);

  std::vector<rank3_production_taxa_key> intended;
  rank3_detail::append_intended_temp_overlay_keys(expected_overlay, intended);
  if (intended.empty()) {
    auto tree_grammar = build_clade_grammar(tree,
                                            options.rebuild_grammar_options);
    for (std::size_t pid = 0; pid < tree_grammar.productions.size(); ++pid) {
      rank3_detail::append_unique_key(
          intended, rank3_detail::production_key_from_id(
                        tree_grammar, static_cast<production_id>(pid)));
    }
  }

  auto result = merge_rank3_overlay_tree_option_b(
      source, base_grammar, tree, std::move(intended), options);
  result.used_source_tree_move = true;
  return result;
}

}  // namespace larch

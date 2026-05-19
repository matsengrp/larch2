#pragma once

#include <larch/chart_spr.hpp>
#include <larch/grammar_topology.hpp>
#include <larch/merge.hpp>
#include <larch/overlay_spr.hpp>
#include <larch/thread_pool.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
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

using rank3_topology = grammar_topology;

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

enum class grammar_native_selection_kind {
  heuristic,
  exact,
  caller_supplied,
};

struct grammar_native_selection_diagnostics {
  std::size_t reachable_temp_production_count = 0;
  std::vector<std::size_t> selected_temp_production_count_by_topology;
  std::vector<production_id> uncovered_temp_productions;
  std::vector<production_id> uncovered_required_productions;
  grammar_native_selection_kind selection_kind =
      grammar_native_selection_kind::heuristic;
  bool topology_cap_truncated = false;
  std::size_t selected_topology_count = 0;
  std::size_t max_materialized_topologies = 0;

  std::uint64_t multisite_optimum = multisite_score_inf;
  std::uint64_t composite_lower_bound = multisite_score_inf;
  std::uint64_t initial_upper_bound = multisite_score_inf;
  std::vector<std::size_t> frontier_sizes_by_clade;
  std::size_t equality_deduplicated = 0;
  std::size_t dominance_pruned = 0;
  std::size_t bound_pruned = 0;
  bool exact_bnb_certificate = false;
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

  // Candidate overloads use the grammar-native materializer by default.  Set
  // this true to explicitly replay candidate.source_tree_move through the old
  // overlay_spr staging path.
  bool use_source_tree_overlay_staging = false;

  // When source-tree overlay staging is requested, re-project that concrete
  // move against the supplied base grammar and require it to describe the same
  // source-tree-local rewrite as the candidate.  This deliberately does not
  // compare against the full materialized collapsed-DAG overlay, which may
  // contain unrelated alternative productions from the base grammar.
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

  // Populated when this result came from grammar-native materialization.
  std::optional<grammar_native_selection_diagnostics> selection_diagnostics;

  [[nodiscard]] bool all_intended_productions_present() const {
    return std::all_of(intended_production_present.begin(),
                       intended_production_present.end(),
                       [](bool present) { return present; });
  }
};

enum class grammar_native_topology_policy {
  // Default safe mode: choose deterministic concrete topologies that include
  // reachable temporary productions when they are mutually compatible.
  prefer_temp_deterministic,
  // Caller supplies dense-overlay rank3_topology values.
  explicit_topologies,
  // Use a single-site chart/traceback over the dense overlay grammar.
  single_site_optimal,
  // Use Phase-5 exact multi-site B&B over the dense overlay grammar.
  multisite_bnb_optimal,
  // Enumerate compatible concrete topologies up to a hard cap.
  bounded_enumeration,
};

class grammar_native_selection_error : public std::runtime_error {
 public:
  grammar_native_selection_diagnostics diagnostics;

  grammar_native_selection_error(
      std::string const& message,
      grammar_native_selection_diagnostics diagnostics_)
      : std::runtime_error(message), diagnostics(std::move(diagnostics_)) {}
};

class grammar_native_bnb_selection_error
    : public grammar_native_selection_error {
 public:
  using grammar_native_selection_error::grammar_native_selection_error;
};

using grammar_native_topology_selector = std::function<std::vector<rank3_topology>(
    overlay_materialization_result const&)>;

struct grammar_native_materialization_options {
  bool include_original_dag = true;
  bool validate = true;
  float generated_edge_weight = std::numeric_limits<float>::max();
  bool require_intended_productions_present = true;
  bool require_all_reachable_temp_productions_selected = true;
  // 0 means unlimited.  The default materializes one deterministic topology.
  std::size_t max_materialized_topologies = 1;
  grammar_native_topology_policy topology_policy =
      grammar_native_topology_policy::prefer_temp_deterministic;
  clade_grammar_options rebuild_grammar_options = {};

  // Used only when topology_policy == explicit_topologies.  The IDs are dense
  // overlay grammar IDs returned by materialize_overlay_grammar().
  std::vector<rank3_topology> explicit_topologies;

  // Optional selector hook for optimal policies whose caller already has a
  // chart/B&B traceback.  Returned topologies use dense overlay grammar IDs.
  grammar_native_topology_selector topology_selector = {};

  // Built-in single_site_optimal inputs.  When score_ua_edge is true, provide
  // single_site_reference_state for the scored site.
  std::optional<leaf_site_states> single_site_leaf_states = std::nullopt;
  chart_options single_site_chart_options = {};
  std::optional<std::uint8_t> single_site_reference_state = std::nullopt;

  // Built-in multisite_bnb_optimal inputs.  The site patterns must use the
  // same taxon registry as the dense overlay grammar.
  site_pattern_set const* multisite_patterns = nullptr;
  chart_options multisite_chart_options = {};
  multisite_topology_trace_options multisite_trace_options = {};
};

struct grammar_native_materialization_result {
  overlay_materialization_result overlay;
  std::vector<rank3_topology> selected_topologies;
  std::vector<phylo_dag> materialized_trees;
  rank3_option_b_result merged;
  std::vector<rank3_production_taxa_key> intended_temp_productions;
  grammar_native_selection_diagnostics selection_diagnostics;
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

inline std::vector<taxon_id> overlay_taxa_for_ref(
    overlay_clade_grammar const& overlay, overlay_clade_ref ref,
    std::string_view context) {
  try {
    return chart_spr_detail::clade_key_for_ref(overlay, ref).taxa;
  } catch (std::runtime_error const& e) {
    throw std::runtime_error(std::string{context} + ": " + e.what());
  }
}

inline std::string taxa_vector_to_string(std::vector<taxon_id> const& taxa) {
  std::string out{"{"};
  for (std::size_t i = 0; i < taxa.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(taxa[i]);
  }
  out += "}";
  return out;
}

inline void validate_temp_production_partition(
    overlay_clade_grammar const& overlay, std::size_t temp_pid) {
  if (temp_pid >= overlay.temp_productions.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: temp production out of range");
  }
  auto const& prod = overlay.temp_productions[temp_pid];
  if (prod.children.size() != 2) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: temp production " +
        std::to_string(temp_pid) + " has arity " +
        std::to_string(prod.children.size()) +
        "; grammar-native materialization is binary-only");
  }

  auto parent_taxa = overlay_taxa_for_ref(
      overlay, prod.parent,
      "rank3 grammar-native materialization temp production parent");
  std::vector<taxon_id> covered;
  for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
    auto child_taxa = overlay_taxa_for_ref(
        overlay, prod.children[child_i],
        "rank3 grammar-native materialization temp production child");
    if (child_taxa == parent_taxa) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: temp production " +
          std::to_string(temp_pid) + " child " + std::to_string(child_i) +
          " has the same taxon set as its parent");
    }

    std::vector<taxon_id> overlap;
    std::set_intersection(covered.begin(), covered.end(), child_taxa.begin(),
                          child_taxa.end(), std::back_inserter(overlap));
    if (!overlap.empty()) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: temp production " +
          std::to_string(temp_pid) +
          " children overlap on taxa " + taxa_vector_to_string(overlap));
    }

    std::vector<taxon_id> next;
    std::set_union(covered.begin(), covered.end(), child_taxa.begin(),
                   child_taxa.end(), std::back_inserter(next));
    covered = std::move(next);
  }

  if (covered != parent_taxa) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: temp production " +
        std::to_string(temp_pid) + " children union " +
        taxa_vector_to_string(covered) + " does not equal parent " +
        taxa_vector_to_string(parent_taxa));
  }
}

inline overlay_materialization_result validate_and_dense_materialize_candidate_impl(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    grammar_native_materialization_options const& options) {
  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: candidate tombstones a temp "
          "production");
    }
  }

  auto overlay = overlay_from_candidate(base, candidate);
  chart_spr_detail::validate_clade_ref(overlay, candidate.moved_clade,
                                       "candidate moved_clade");
  chart_spr_detail::validate_clade_ref(overlay, candidate.old_parent,
                                       "candidate old_parent");
  chart_spr_detail::validate_clade_ref(overlay, candidate.old_sibling,
                                       "candidate old_sibling");
  chart_spr_detail::validate_clade_ref(overlay,
                                       candidate.new_sibling_or_target,
                                       "candidate new_sibling_or_target");
  for (std::size_t i = 0; i < overlay.temp_productions.size(); ++i) {
    validate_temp_production_partition(overlay, i);
  }

  auto materialized = materialize_overlay_grammar(overlay);
  parsimony_chart_detail::validate_chart_grammar(materialized.grammar);
  chart_trim_detail::validate_production_indices(materialized.grammar);

  for (std::size_t i = 0; i < materialized.temp_production_to_dense.size();
       ++i) {
    auto dense_pid = materialized.temp_production_to_dense[i];
    if (dense_pid == no_production) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: temp production " +
          std::to_string(i) + " is unreachable from the overlay root");
    }
    if (dense_pid >= materialized.grammar.productions.size()) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: temp production dense id out "
          "of range");
    }
  }
  (void)options;
  return materialized;
}

inline std::vector<production_id> reachable_temp_dense_productions(
    overlay_materialization_result const& materialized) {
  std::vector<production_id> result;
  result.reserve(materialized.temp_production_to_dense.size());
  for (auto dense_pid : materialized.temp_production_to_dense) {
    if (dense_pid == no_production) continue;
    if (dense_pid >= materialized.grammar.productions.size()) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: temp dense production out of "
          "range");
    }
    result.push_back(dense_pid);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

inline bool dense_production_is_temp(
    overlay_materialization_result const& materialized, production_id pid) {
  if (pid == no_production ||
      pid >= materialized.dense_production_to_ref.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: dense production ref out of "
        "range");
  }
  return materialized.dense_production_to_ref[pid].space ==
         overlay_id_space::temp;
}

inline void append_selected_temp_overlay_keys(
    overlay_materialization_result const& materialized,
    std::vector<rank3_topology> const& topologies,
    std::vector<rank3_production_taxa_key>& keys) {
  for (auto const& topology : topologies) {
    auto reachable = validate_grammar_topology(materialized.grammar, topology);
    for (std::size_t pid = 0; pid < reachable.size(); ++pid) {
      if (!reachable[pid]) continue;
      auto dense_pid = static_cast<production_id>(pid);
      if (!dense_production_is_temp(materialized, dense_pid)) continue;
      append_unique_key(keys, production_key_from_id(materialized.grammar,
                                                     dense_pid));
    }
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
  return make_empty_grammar_topology(grammar);
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

namespace rank3_detail {

inline std::vector<std::vector<taxon_id>> production_child_taxa_for_sort(
    clade_grammar const& grammar, production_id pid) {
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: production id out of range");
  }
  std::vector<std::vector<taxon_id>> child_taxa;
  child_taxa.reserve(grammar.productions[pid].children.size());
  for (auto child : grammar.productions[pid].children) {
    if (child == no_clade || child >= grammar.clades.size()) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: production child out of "
          "range");
    }
    child_taxa.push_back(grammar.clades[child].taxa);
  }
  std::sort(child_taxa.begin(), child_taxa.end());
  return child_taxa;
}

inline std::vector<production_id> sorted_productions_for_native_selection(
    overlay_materialization_result const& materialized, clade_id clade) {
  auto const& grammar = materialized.grammar;
  if (clade == no_clade || clade >= grammar.productions_by_parent.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: parent clade out of range");
  }
  auto productions = grammar.productions_by_parent[clade];
  std::stable_sort(productions.begin(), productions.end(),
                   [&](production_id lhs, production_id rhs) {
                     auto lhs_temp = dense_production_is_temp(materialized,
                                                              lhs);
                     auto rhs_temp = dense_production_is_temp(materialized,
                                                              rhs);
                     if (lhs_temp != rhs_temp) return lhs_temp > rhs_temp;
                     if (lhs != rhs) return lhs < rhs;
                     return production_child_taxa_for_sort(grammar, lhs) <
                            production_child_taxa_for_sort(grammar, rhs);
                   });
  return productions;
}

inline void merge_topology_into(rank3_topology& dst,
                                rank3_topology const& src) {
  if (dst.selected_production_by_clade.size() !=
          src.selected_production_by_clade.size() ||
      dst.used_production.size() != src.used_production.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: topology shape mismatch");
  }
  for (std::size_t cid = 0; cid < src.selected_production_by_clade.size();
       ++cid) {
    auto pid = src.selected_production_by_clade[cid];
    if (pid == no_production) continue;
    auto& selected = dst.selected_production_by_clade[cid];
    if (selected != no_production && selected != pid) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: incompatible child topology "
          "choices for clade " +
          std::to_string(cid));
    }
    selected = pid;
  }
  for (std::size_t pid = 0; pid < src.used_production.size(); ++pid) {
    dst.used_production[pid] = dst.used_production[pid] ||
                               src.used_production[pid];
  }
}

inline rank3_topology combine_native_child_topologies(
    clade_grammar const& grammar, clade_id parent, production_id pid,
    rank3_topology const& left, rank3_topology const& right) {
  auto topology = make_empty_rank3_topology(grammar);
  merge_topology_into(topology, left);
  merge_topology_into(topology, right);
  if (parent == no_clade || parent >= topology.selected_production_by_clade.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: parent clade out of range");
  }
  auto& selected = topology.selected_production_by_clade[parent];
  if (selected != no_production && selected != pid) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: incompatible parent topology "
        "choice");
  }
  selected = pid;
  if (pid == no_production || pid >= topology.used_production.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: selected production out of "
        "range");
  }
  topology.used_production[pid] = true;
  return topology;
}

inline std::vector<rank3_topology> enumerate_native_topologies_from_clade(
    overlay_materialization_result const& materialized, clade_id clade,
    std::size_t max_topologies) {
  auto const& grammar = materialized.grammar;
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: clade out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1) {
    return {make_empty_rank3_topology(grammar)};
  }

  std::vector<rank3_topology> result;
  auto productions = sorted_productions_for_native_selection(materialized,
                                                             clade);
  if (productions.empty()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: non-singleton clade has no "
        "production");
  }

  for (auto pid : productions) {
    if (max_topologies != 0 && result.size() >= max_topologies) break;
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: production parent mismatch");
    }
    chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
    auto left_topologies = enumerate_native_topologies_from_clade(
        materialized, prod.children[0], max_topologies);
    auto right_topologies = enumerate_native_topologies_from_clade(
        materialized, prod.children[1], max_topologies);

    for (auto const& left : left_topologies) {
      for (auto const& right : right_topologies) {
        if (max_topologies != 0 && result.size() >= max_topologies) break;
        auto topology = combine_native_child_topologies(grammar, clade, pid,
                                                        left, right);
        result.push_back(std::move(topology));
      }
      if (max_topologies != 0 && result.size() >= max_topologies) break;
    }
  }
  return result;
}

inline std::vector<bool> union_used_productions(
    clade_grammar const& grammar, std::vector<rank3_topology> const& topologies) {
  std::vector<bool> used(grammar.productions.size(), false);
  for (auto const& topology : topologies) {
    auto reachable = validate_topology(grammar, topology);
    for (std::size_t pid = 0; pid < reachable.size(); ++pid)
      used[pid] = used[pid] || reachable[pid];
  }
  return used;
}

inline std::string production_id_vector_to_string(
    std::vector<production_id> const& ids) {
  std::string out{"["};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(ids[i]);
  }
  out += "]";
  return out;
}

struct native_topology_selection_result {
  std::vector<rank3_topology> topologies;
  grammar_native_selection_diagnostics diagnostics;
};

inline grammar_native_selection_diagnostics build_selection_diagnostics(
    overlay_materialization_result const& materialized,
    std::vector<rank3_topology> const& topologies,
    grammar_native_selection_kind selection_kind, bool cap_truncated,
    std::size_t max_materialized_topologies) {
  auto temp_dense = reachable_temp_dense_productions(materialized);
  std::vector<bool> covered(materialized.grammar.productions.size(), false);

  grammar_native_selection_diagnostics diagnostics;
  diagnostics.reachable_temp_production_count = temp_dense.size();
  diagnostics.selection_kind = selection_kind;
  diagnostics.topology_cap_truncated = cap_truncated;
  diagnostics.selected_topology_count = topologies.size();
  diagnostics.max_materialized_topologies = max_materialized_topologies;
  diagnostics.selected_temp_production_count_by_topology.reserve(
      topologies.size());

  for (auto const& topology : topologies) {
    auto reachable = validate_topology(materialized.grammar, topology);
    std::size_t selected_temp_count = 0;
    for (auto dense_pid : temp_dense) {
      if (dense_pid >= reachable.size()) {
        throw std::runtime_error(
            "rank3 grammar-native materialization: temp production dense id "
            "out of topology range");
      }
      if (!reachable[dense_pid]) continue;
      ++selected_temp_count;
      covered[dense_pid] = true;
    }
    diagnostics.selected_temp_production_count_by_topology.push_back(
        selected_temp_count);
  }

  for (auto dense_pid : temp_dense) {
    if (dense_pid >= covered.size() || !covered[dense_pid])
      diagnostics.uncovered_temp_productions.push_back(dense_pid);
  }
  return diagnostics;
}

inline void copy_bnb_trace_diagnostics(
    grammar_native_selection_diagnostics& diagnostics,
    multisite_topology_trace_result const& trace) {
  diagnostics.multisite_optimum = trace.optimum;
  diagnostics.composite_lower_bound = trace.composite_lower_bound;
  diagnostics.initial_upper_bound = trace.initial_upper_bound;
  diagnostics.frontier_sizes_by_clade = trace.frontier_sizes_by_clade;
  diagnostics.equality_deduplicated = trace.equality_deduplicated;
  diagnostics.dominance_pruned = trace.dominance_pruned;
  diagnostics.bound_pruned = trace.bound_pruned;
  diagnostics.uncovered_required_productions =
      trace.uncovered_required_productions;
  diagnostics.exact_bnb_certificate = true;
  diagnostics.topology_cap_truncated = diagnostics.topology_cap_truncated ||
                                       trace.topology_cap_truncated;
}

inline void require_reachable_temp_productions_selected(
    grammar_native_materialization_options const& options,
    grammar_native_selection_diagnostics const& diagnostics) {
  if (!options.require_all_reachable_temp_productions_selected) return;
  if (diagnostics.uncovered_temp_productions.empty()) return;
  throw grammar_native_selection_error(
      "rank3 grammar-native materialization: reachable temp productions " +
          production_id_vector_to_string(
              diagnostics.uncovered_temp_productions) +
          " are not selected by the materialized topology set",
      diagnostics);
}

inline void enforce_native_selection_cap(
    std::vector<rank3_topology> const& topologies,
    grammar_native_materialization_options const& options,
    std::string_view context) {
  if (options.max_materialized_topologies == 0) return;
  if (topologies.size() <= options.max_materialized_topologies) return;
  throw std::runtime_error(
      "rank3 grammar-native materialization: " + std::string{context} +
      " topology count exceeds max_materialized_topologies");
}

inline rank3_topology single_site_native_topology(
    clade_grammar const& grammar,
    grammar_native_materialization_options const& options) {
  if (!options.single_site_leaf_states.has_value()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: single_site_optimal requires "
        "single_site_leaf_states or topology_selector");
  }

  auto const& chart_options = options.single_site_chart_options;
  auto chart = build_single_site_chart(grammar,
                                       *options.single_site_leaf_states,
                                       chart_options);
  if (chart_options.score_ua_edge) {
    if (!options.single_site_reference_state.has_value()) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: single_site_optimal with "
          "score_ua_edge requires single_site_reference_state");
    }
    auto outside = build_single_site_outside_chart(
        grammar, chart, chart_options, *options.single_site_reference_state);
    return rank3_topology_from_traceback(
        grammar, deterministic_optimal_single_site_traceback(grammar, chart,
                                                             outside));
  }

  auto outside = build_single_site_outside_chart(grammar, chart,
                                                chart_options);
  return rank3_topology_from_traceback(
      grammar, deterministic_optimal_single_site_traceback(grammar, chart,
                                                           outside));
}

inline native_topology_selection_result select_native_topologies(
    overlay_materialization_result const& materialized,
    grammar_native_materialization_options const& options) {
  auto const& grammar = materialized.grammar;
  native_topology_selection_result result;
  auto selection_kind = grammar_native_selection_kind::heuristic;
  bool cap_truncated = false;
  std::optional<multisite_topology_trace_result> bnb_trace;
  std::vector<production_id> bnb_caller_required_productions;

  switch (options.topology_policy) {
    case grammar_native_topology_policy::explicit_topologies:
      if (options.explicit_topologies.empty()) {
        throw std::runtime_error(
            "rank3 grammar-native materialization: explicit topology policy "
            "requires at least one topology");
      }
      result.topologies = options.explicit_topologies;
      enforce_native_selection_cap(result.topologies, options, "explicit");
      selection_kind = grammar_native_selection_kind::caller_supplied;
      break;

    case grammar_native_topology_policy::prefer_temp_deterministic:
    case grammar_native_topology_policy::bounded_enumeration: {
      auto requested = options.max_materialized_topologies;
      auto enumeration_cap = requested;
      if (requested != 0 &&
          requested < std::numeric_limits<std::size_t>::max()) {
        enumeration_cap = requested + 1;
      }
      result.topologies = enumerate_native_topologies_from_clade(
          materialized, grammar.root_clade, enumeration_cap);
      if (requested != 0 && result.topologies.size() > requested) {
        cap_truncated = true;
        result.topologies.resize(requested);
      }
      if (result.topologies.empty()) {
        throw std::runtime_error(
            "rank3 grammar-native materialization: no concrete topologies "
            "selected from dense overlay grammar");
      }
      selection_kind = grammar_native_selection_kind::heuristic;
      break;
    }

    case grammar_native_topology_policy::single_site_optimal:
      if (options.topology_selector) {
        result.topologies = options.topology_selector(materialized);
        selection_kind = grammar_native_selection_kind::caller_supplied;
      } else {
        result.topologies.push_back(single_site_native_topology(grammar,
                                                                options));
        selection_kind = grammar_native_selection_kind::exact;
      }
      enforce_native_selection_cap(result.topologies, options, "single-site");
      break;

    case grammar_native_topology_policy::multisite_bnb_optimal:
      if (options.topology_selector) {
        result.topologies = options.topology_selector(materialized);
        enforce_native_selection_cap(result.topologies, options, "multi-site");
        selection_kind = grammar_native_selection_kind::caller_supplied;
        break;
      }

      if (options.multisite_patterns == nullptr) {
        auto diagnostics = build_selection_diagnostics(
            materialized, {}, grammar_native_selection_kind::heuristic, false,
            options.max_materialized_topologies);
        throw grammar_native_bnb_selection_error(
            "rank3 grammar-native materialization: multisite_bnb_optimal "
            "requires multisite_patterns when no topology_selector is "
            "provided",
            diagnostics);
      }

      try {
        auto trace_options = options.multisite_trace_options;
        bnb_caller_required_productions = trace_options.required_productions;
        std::sort(bnb_caller_required_productions.begin(),
                  bnb_caller_required_productions.end());
        bnb_caller_required_productions.erase(
            std::unique(bnb_caller_required_productions.begin(),
                        bnb_caller_required_productions.end()),
            bnb_caller_required_productions.end());
        auto required = bnb_caller_required_productions;
        auto temp_dense = reachable_temp_dense_productions(materialized);
        required.insert(required.end(), temp_dense.begin(), temp_dense.end());
        std::sort(required.begin(), required.end());
        required.erase(std::unique(required.begin(), required.end()),
                       required.end());
        trace_options.required_productions = std::move(required);
        // Let this materialization layer turn uncovered temp productions into
        // grammar_native_bnb_selection_error with overlay-selection diagnostics.
        trace_options.require_required_production_coverage = false;
        if (trace_options.max_optimal_topologies == 1) {
          trace_options.max_optimal_topologies =
              options.max_materialized_topologies;
        }

        bnb_trace = build_multisite_optimal_topologies(
            grammar, *options.multisite_patterns,
            options.multisite_chart_options, trace_options);
      } catch (grammar_native_bnb_selection_error const&) {
        throw;
      } catch (std::runtime_error const& e) {
        auto diagnostics = build_selection_diagnostics(
            materialized, {}, grammar_native_selection_kind::exact, false,
            options.max_materialized_topologies);
        throw grammar_native_bnb_selection_error(
            std::string{"rank3 grammar-native materialization: built-in "
                        "multi-site B&B topology selection failed: "} +
                e.what(),
            diagnostics);
      }

      result.topologies = bnb_trace->topologies;
      enforce_native_selection_cap(result.topologies, options,
                                   "multi-site B&B");
      cap_truncated = bnb_trace->topology_cap_truncated;
      selection_kind = grammar_native_selection_kind::exact;
      break;
  }

  if (result.topologies.empty()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: topology selector returned no "
        "topologies");
  }

  for (auto const& topology : result.topologies)
    (void)validate_topology(grammar, topology);
  result.diagnostics = build_selection_diagnostics(
      materialized, result.topologies, selection_kind, cap_truncated,
      options.max_materialized_topologies);
  if (bnb_trace.has_value()) {
    copy_bnb_trace_diagnostics(result.diagnostics, *bnb_trace);
    std::vector<production_id> uncovered_caller_required;
    for (auto pid : bnb_caller_required_productions) {
      if (std::find(bnb_trace->uncovered_required_productions.begin(),
                    bnb_trace->uncovered_required_productions.end(),
                    pid) != bnb_trace->uncovered_required_productions.end()) {
        uncovered_caller_required.push_back(pid);
      }
    }
    if (!uncovered_caller_required.empty()) {
      throw grammar_native_bnb_selection_error(
          "rank3 grammar-native materialization: built-in multi-site B&B "
          "found no emitted exact-optimal topology set covering "
          "caller-required productions " +
              production_id_vector_to_string(uncovered_caller_required),
          result.diagnostics);
    }
  }
  if (bnb_trace.has_value() &&
      options.require_all_reachable_temp_productions_selected &&
      !result.diagnostics.uncovered_temp_productions.empty()) {
    throw grammar_native_bnb_selection_error(
        "rank3 grammar-native materialization: built-in multi-site B&B found "
        "no emitted exact-optimal topology set covering reachable temp "
        "productions " +
            production_id_vector_to_string(
                result.diagnostics.uncovered_temp_productions),
        result.diagnostics);
  }
  require_reachable_temp_productions_selected(options, result.diagnostics);
  return result;
}

}  // namespace rank3_detail

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

inline overlay_materialization_result validate_and_dense_materialize_candidate(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    grammar_native_materialization_options const& options = {}) {
  return rank3_detail::validate_and_dense_materialize_candidate_impl(
      base, candidate, options);
}

inline rank3_option_b_result merge_rank3_native_trees_option_b(
    phylo_dag& source, clade_grammar const& base_grammar,
    std::vector<phylo_dag>& trees,
    std::vector<rank3_production_taxa_key> intended_productions,
    grammar_native_materialization_options const& options = {}) {
  if (trees.empty()) {
    throw std::runtime_error(
        "rank3 grammar-native materialization: no generated trees to merge");
  }

  merge merger{get_reference_sequence(source)};
  if (options.include_original_dag) {
    build_clade_offsets(source);
    merger.add_dag(source);
  }
  for (auto& tree : trees) {
    if (get_reference_sequence(tree) != get_reference_sequence(source)) {
      throw std::runtime_error(
          "rank3 grammar-native materialization: generated tree reference "
          "mismatch");
    }
    build_clade_offsets(tree);
    merger.add_dag(tree);
  }

  rank3_option_b_result result;
  result.materialized_tree_count = trees.size();
  result.staged_in_overlay = false;
  result.used_source_tree_move = false;
  result.intended_productions = std::move(intended_productions);
  result.dag = std::move(merger.get_result());
  build_clade_offsets(result.dag);
  if (options.validate) {
    validate_dag(result.dag,
                 "rank3 grammar-native materialized/merged DAG",
                 thread_pool::get_default());
  }
  result.rebuilt = build_clade_grammar_with_audit(
      result.dag, options.rebuild_grammar_options);
  rank3_detail::validate_same_taxa_for_rank3(
      "rank3 grammar-native materialization", base_grammar,
      result.rebuilt.grammar);
  result.intended_production_present = rank3_detail::production_key_presence(
      result.rebuilt.grammar, result.intended_productions);

  if (options.require_intended_productions_present) {
    for (std::size_t i = 0; i < result.intended_productions.size(); ++i) {
      if (result.intended_production_present[i]) continue;
      throw std::runtime_error(
          "rank3 grammar-native materialization: intended production missing "
          "after grammar rebuild: " +
          rank3_detail::production_key_to_string(
              result.intended_productions[i]));
    }
  }
  return result;
}

inline grammar_native_materialization_result materialize_grammar_native_overlay(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate,
    grammar_native_materialization_options const& options = {}) {
  // Validate source leaf compact genomes before selecting/materializing trees;
  // materialize_rank3_tree_from_topology repeats this check for the dense
  // grammar, but doing it here produces a grammar-native diagnostic before any
  // generated tree work starts.
  try {
    (void)rank3_detail::collect_leaf_compact_genomes(source, base_grammar);
  } catch (std::runtime_error const& e) {
    throw std::runtime_error(
        std::string{"rank3 grammar-native materialization: "} + e.what());
  }

  grammar_native_materialization_result result;
  result.overlay = validate_and_dense_materialize_candidate(base_grammar,
                                                            candidate,
                                                            options);

  auto selection = rank3_detail::select_native_topologies(result.overlay,
                                                          options);
  result.selected_topologies = std::move(selection.topologies);
  result.selection_diagnostics = std::move(selection.diagnostics);

  if (options.require_all_reachable_temp_productions_selected) {
    rank3_detail::append_intended_temp_overlay_keys(
        result.overlay, result.intended_temp_productions);
  } else {
    rank3_detail::append_selected_temp_overlay_keys(
        result.overlay, result.selected_topologies,
        result.intended_temp_productions);
  }

  result.materialized_trees = materialize_rank3_trees_from_topologies(
      source, result.overlay.grammar, result.selected_topologies,
      options.validate, options.generated_edge_weight);

  result.merged = merge_rank3_native_trees_option_b(
      source, base_grammar, result.materialized_trees,
      result.intended_temp_productions, options);
  result.merged.selection_diagnostics = result.selection_diagnostics;
  return result;
}

inline rank3_option_b_result materialize_rank3_option_b_grammar_native(
    phylo_dag& source, clade_grammar const& base_grammar,
    grammar_spr_candidate const& candidate,
    rank3_option_b_options const& options = {}) {
  grammar_native_materialization_options native_options;
  native_options.include_original_dag = options.include_original_dag;
  native_options.validate = options.validate;
  native_options.generated_edge_weight = options.added_edge_weight;
  native_options.require_intended_productions_present =
      options.require_intended_productions_present;
  native_options.rebuild_grammar_options = options.rebuild_grammar_options;
  auto result = materialize_grammar_native_overlay(source, base_grammar,
                                                   candidate, native_options);
  result.merged.selection_diagnostics = result.selection_diagnostics;
  return std::move(result.merged);
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
        "rank3 option B: overlay_spr staging requires source_tree_move; use "
        "materialize_rank3_option_b for grammar-native materialization");
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
  if (!options.use_source_tree_overlay_staging) {
    return materialize_rank3_option_b_grammar_native(source, base_grammar,
                                                     candidate, options);
  }

  auto stage = stage_rank3_option_b_overlay(source, base_grammar, candidate,
                                            options);
  auto tree = materialize_rank3_option_b_overlay_tree(stage, options.validate);

  auto expected_overlay = validate_and_dense_materialize_candidate(
      base_grammar, candidate);

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

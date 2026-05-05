#pragma once

#include <larch/compute.hpp>
#include <larch/indep_rs_cnn_model.hpp>
#include <larch/likelihood_score_ops.hpp>
#include <larch/rs_fivemer_model.hpp>
#include <larch/spr_move.hpp>
#include <larch/yaml_reader.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

// Type-erased model wrapper holding either model type.
using ml_model = std::variant<rs_fivemer_model, indep_rs_cnn_model>;

// Auto-detect model type from YAML and load weights.
inline ml_model load_ml_model(std::string_view dir, std::string_view name) {
  auto yml_path = std::string{dir} + "/" + std::string{name} + ".yml";
  auto doc = read_yaml(yml_path);
  auto model_class = doc.at("model_class").as_string();
  if (model_class == "RSFivemerModel") return rs_fivemer_model::load(dir, name);
  if (model_class == "IndepRSCNNModel")
    return indep_rs_cnn_model::load(dir, name);
  throw std::runtime_error{"load_ml_model: unknown model_class: " +
                            model_class};
}

// Compute log_likelihood through variant dispatch.
inline double ml_log_likelihood(ml_model const& model, std::string_view parent,
                                std::string_view child) {
  return std::visit(
      [&](auto const& m) { return m.log_likelihood(parent, child); }, model);
}

// WeightOps specialization for minimum-NLL tree sampling with an ml_model
// variant.
template <>
struct likelihood_score_ops<ml_model> {
  using weight_type = double;

  ml_model const& model;
  std::string const& reference;
  bool ignore_ua_edge = true;
  mutable likelihood_score_cache cache;

  void clear_cache() const { cache.clear(); }

  std::size_t cached_sequence_count() const {
    return cache.cached_sequence_count();
  }

  std::size_t cached_edge_score_count() const {
    return cache.cached_edge_score_count();
  }

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return 0.0;
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t edge_idx) const {
    auto parent_idx = get_parent_idx(dag, edge_idx);
    if (ignore_ua_edge && is_ua(dag, parent_idx)) return 0.0;

    auto& cached = cache.edge_score_slot(dag, edge_idx);
    if (cached) return *cached;

    auto ev = dag.get_edge(edge_idx);
    double score = std::visit(
        [&](auto edge) -> double {
          if (edge.mutations().empty()) return 0.0;

          auto child_idx = std::visit([](auto child) { return child.index(); },
                                      edge.get_child());
          auto const& parent_seq =
              cache.sequence_for(dag, parent_idx, reference);
          auto const& child_seq = cache.sequence_for(dag, child_idx, reference);

          return -ml_log_likelihood(model, parent_seq, child_seq);
        },
        ev);
    cached = score;
    return score;
  }

  std::pair<weight_type, std::vector<std::size_t>> within_clade_accum(
      std::vector<weight_type> const& weights) const {
    double best = std::numeric_limits<double>::max();
    for (auto w : weights) best = std::min(best, w);
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < weights.size(); ++i)
      if (weights[i] <= best + 1e-10) indices.push_back(i);
    return {best, indices};
  }

  weight_type between_clades(std::vector<weight_type> const& weights) const {
    double sum = 0.0;
    for (auto w : weights) sum += w;
    return sum;
  }

  weight_type above_node(weight_type edge_w, weight_type child_w) const {
    return edge_w + child_w;
  }
};

using ml_model_likelihood_score_ops = likelihood_score_ops<ml_model>;

// Adjust rate bias through variant dispatch.
inline void ml_adjust_rate_bias(ml_model& model, double log_factor) {
  std::visit([&](auto& m) { m.adjust_rate_bias_by(log_factor); }, model);
}

// Compute total negative log-likelihood for all non-UA mutated edges in a DAG.
inline double compute_dag_ml_nll(ml_model const& model, phylo_dag& dag) {
  auto const& ref = get_reference_sequence(dag);
  ml_model_likelihood_score_ops ops{.model = model,
                                    .reference = ref,
                                    .ignore_ua_edge = true};
  double total_nll = 0.0;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) { total_nll += ops.compute_edge(dag, edge.index()); },
        ev);
  }
  return total_nll;
}

// ============================================================================
// Changed-edges-only delta LL scoring
// ============================================================================

// Collect the set of nodes whose parent edges are affected by an SPR move.
// These are nodes on the path from src to lca and from dst to lca,
// plus src itself and dst itself (whose parent edges change).
inline std::unordered_set<std::size_t> collect_affected_nodes(
    phylo_dag& tree, spr_move const& move) {
  std::unordered_set<std::size_t> affected;

  // Walk src → lca: every node on this path has its parent edge changed.
  auto cur = move.src;
  while (cur != move.lca) {
    affected.insert(cur);
    auto pe = get_parent_edges(tree, cur);
    if (pe.empty()) break;
    cur = get_parent_idx(tree, pe[0]);
  }

  // Walk dst → lca: same.
  cur = move.dst;
  while (cur != move.lca) {
    affected.insert(cur);
    auto pe = get_parent_edges(tree, cur);
    if (pe.empty()) break;
    cur = get_parent_idx(tree, pe[0]);
  }

  return affected;
}

// Compute NLL for a single edge given parent/child node indices in a tree.
inline double compute_edge_nll(ml_model const& model, phylo_dag& tree,
                               std::size_t parent_idx,
                               std::size_t child_idx) {
  auto const& ref = get_reference_sequence(tree);
  auto parent_seq = reconstruct_sequence(tree, parent_idx, ref);
  auto child_seq = reconstruct_sequence(tree, child_idx, ref);
  return -ml_log_likelihood(model, parent_seq, child_seq);
}

// Compute delta ML score for an SPR move by scoring only the changed edges.
//
// Identifies nodes on the src→lca and dst→lca paths in the original tree.
// For each such node, scores the parent edge before and after the move.
// Returns old_NLL - new_NLL (positive = move improved likelihood).
//
// The fragment contains the new topology with Fitch-assigned CGs.
// The original_tree provides the old edge topology.
inline double compute_delta_ml_score(ml_model const& model,
                                     phylo_dag& original_tree,
                                     phylo_dag& fragment,
                                     spr_move const& move) {
  auto affected = collect_affected_nodes(original_tree, move);
  if (affected.empty()) return 0.0;

  auto const& ref = get_reference_sequence(original_tree);

  // Old NLL: score parent edges of affected nodes in original tree.
  double old_nll = 0.0;
  for (auto nidx : affected) {
    auto pe = get_parent_edges(original_tree, nidx);
    if (pe.empty()) continue;
    auto parent_idx = get_parent_idx(original_tree, pe[0]);
    if (is_ua(original_tree, parent_idx)) continue;
    old_nll += compute_edge_nll(model, original_tree, parent_idx, nidx);
  }

  // New NLL: score all non-UA edges in the fragment.
  // The fragment is the full affected subtree with new topology, so this
  // captures all new edges including the new inner node created by the SPR.
  double new_nll = compute_dag_ml_nll(model, fragment);

  return old_nll - new_nll;
}

// Backward-compatible alias.
inline double compute_fragment_ml_score(ml_model const& model,
                                        phylo_dag& fragment) {
  return compute_dag_ml_nll(model, fragment);
}

// ============================================================================
// MLScoringConfig: encapsulates ML model + coefficient for move scoring.
// ============================================================================

struct ml_scoring_config {
  ml_model* model = nullptr;
  double coeff = 0.0;

  // Adjust a parsimony-based score with ML delta LL.
  // base_score: parsimony score (lower = better).
  // original_tree: the tree before the SPR move.
  // fragment: the post-move subtree DAG.
  // move: the SPR move that produced the fragment.
  //
  // Returns: base_score - coeff * delta_LL
  // When delta_LL > 0 (move improved ML), effective score decreases.
  double adjust_score(double base_score, phylo_dag& original_tree,
                      phylo_dag& fragment, spr_move const& move) const {
    if (model != nullptr && coeff != 0.0) {
      double delta_ll =
          compute_delta_ml_score(*model, original_tree, fragment, move);
      return base_score - coeff * delta_ll;
    }
    return base_score;
  }
};

}  // namespace larch

#pragma once

#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

// Reconstruct the full DNA sequence for a DAG node by applying its
// compact_genome mutations to the reference.  UA nodes return the reference.
inline std::string reconstruct_sequence(phylo_dag& dag, std::size_t node_idx,
                                        std::string_view reference) {
  std::string seq{reference};
  auto nv = dag.get_node(node_idx);
  std::visit(
      [&](auto node) {
        if constexpr (requires { node.cg(); }) {
          for (auto& [pos, base] : node.cg()) seq[pos - 1] = base.to_char();
        }
      },
      nv);
  return seq;
}

// Per-WeightOps cache for likelihood scoring.
//
// The cache is intentionally tied to the phylo_dag object address used by
// compute_edge().  Use a fresh ops object, or call clear_cache(), after
// mutating the DAG, changing the referenced model state (for example via
// ml_adjust_rate_bias()), or changing scoring options that affect edge scores.
//
// The cache mutates inside const compute_edge(); this is intended for the
// current serial subtree_weight use.  A shared ops object is not thread-safe
// for parallel scoring.  Sequence caching also trades memory for speed: for
// large DAGs it may hold one full reconstructed sequence per scored node.
struct likelihood_score_cache {
  mutable phylo_dag const* dag = nullptr;
  mutable std::vector<std::optional<std::string>> sequences;
  mutable std::vector<std::optional<double>> edge_scores;

  void clear() const {
    dag = nullptr;
    sequences.clear();
    edge_scores.clear();
  }

  void ensure_for(phylo_dag& d) const {
    if (dag != &d) {
      dag = &d;
      sequences.clear();
      edge_scores.clear();
    }
    if (sequences.size() < d.node_high_mark())
      sequences.resize(d.node_high_mark());
    if (edge_scores.size() < d.edge_high_mark())
      edge_scores.resize(d.edge_high_mark());
  }

  std::string const& sequence_for(phylo_dag& d, std::size_t node_idx,
                                  std::string_view reference) const {
    ensure_for(d);
    if (!sequences[node_idx])
      sequences[node_idx] = reconstruct_sequence(d, node_idx, reference);
    return *sequences[node_idx];
  }

  std::optional<double>& edge_score_slot(phylo_dag& d,
                                         std::size_t edge_idx) const {
    ensure_for(d);
    return edge_scores[edge_idx];
  }

  std::size_t cached_sequence_count() const {
    std::size_t count = 0;
    for (auto const& seq : sequences)
      if (seq) ++count;
    return count;
  }

  std::size_t cached_edge_score_count() const {
    std::size_t count = 0;
    for (auto const& score : edge_scores)
      if (score) ++count;
    return count;
  }
};

// WeightOps for maximum-likelihood tree scoring using a neural network model.
// Weight = negative log-likelihood (lower = better, matching parsimony
// semantics where min weight = optimal tree).
//
// Model must provide: double log_likelihood(string_view parent, string_view
// child).  The artificial UA->root edge is ignored by default; set
// ignore_ua_edge=false to score it.
template <typename Model>
struct likelihood_score_ops {
  using weight_type = double;

  Model const& model;
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

          auto child_idx =
              std::visit([](auto c) { return c.index(); }, edge.get_child());

          auto const& parent_seq =
              cache.sequence_for(dag, parent_idx, reference);
          auto const& child_seq = cache.sequence_for(dag, child_idx, reference);

          return -model.log_likelihood(parent_seq, child_seq);
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

}  // namespace larch

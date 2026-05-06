#pragma once

#include <larch/bigint.hpp>
#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>
#include <larch/simple_weight_ops.hpp>
#include <larch/tie_tolerance.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

// Concept for weight operations
template <typename Ops>
concept WeightOps = requires(Ops const& ops, phylo_dag& dag, std::size_t idx,
                             std::vector<typename Ops::weight_type> ws) {
  typename Ops::weight_type;
  {
    ops.compute_leaf(dag, idx)
  } -> std::convertible_to<typename Ops::weight_type>;
  {
    ops.compute_edge(dag, idx)
  } -> std::convertible_to<typename Ops::weight_type>;
  {
    ops.within_clade_accum(ws)
  } -> std::same_as<
      std::pair<typename Ops::weight_type, std::vector<std::size_t>>>;
  { ops.between_clades(ws) } -> std::convertible_to<typename Ops::weight_type>;
  {
    ops.above_node(ws[0], ws[0])
  } -> std::convertible_to<typename Ops::weight_type>;
};

// Parsimony scoring: weight = mutation count (min across clades)
struct parsimony_score_ops {
  using weight_type = std::size_t;

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return 0;
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t edge_idx) const {
    auto ev = dag.get_edge(edge_idx);
    return std::visit(
        [](auto edge) -> std::size_t { return edge.mutations().size(); }, ev);
  }

  // Within a clade: pick the minimum weight edges
  std::pair<weight_type, std::vector<std::size_t>> within_clade_accum(
      std::vector<weight_type> const& weights) const {
    assert(!weights.empty());
    weight_type best = *std::min_element(weights.begin(), weights.end());
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < weights.size(); ++i)
      if (weights[i] == best) indices.push_back(i);
    return {best, indices};
  }

  // Between clades: sum weights (independent subtrees)
  weight_type between_clades(std::vector<weight_type> const& weights) const {
    weight_type sum = 0;
    for (auto w : weights) sum += w;
    return sum;
  }

  // Above node: edge_weight + child_weight
  weight_type above_node(weight_type edge_weight,
                         weight_type child_weight) const {
    return edge_weight + child_weight;
  }
};

// Tree counting: weight = number of distinct trees below
struct tree_count_ops {
  using weight_type = bigint;

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return bigint{1};
  }

  weight_type compute_edge(phylo_dag& /*dag*/, std::size_t /*edge_idx*/) const {
    return bigint{1};
  }

  // Within a clade: sum all weights (every edge is an alternative)
  std::pair<weight_type, std::vector<std::size_t>> within_clade_accum(
      std::vector<weight_type> const& weights) const {
    assert(!weights.empty());
    weight_type sum{0};
    for (auto const& w : weights) sum += w;
    std::vector<std::size_t> all_indices;
    for (std::size_t i = 0; i < weights.size(); ++i) all_indices.push_back(i);
    return {sum, all_indices};
  }

  // Between clades: product (cartesian product of choices)
  weight_type between_clades(std::vector<weight_type> const& weights) const {
    weight_type prod{1};
    for (auto const& w : weights) prod *= w;
    return prod;
  }

  // Above node: child_weight (edge weight is ignored for counting)
  weight_type above_node(weight_type /*edge_weight*/,
                         weight_type child_weight) const {
    return child_weight;
  }
};

// Parsimony scoring that ignores the UA->root edge (for
// --ignore-root-edge-mutations)
struct ua_free_parsimony_score_ops {
  using weight_type = std::size_t;

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return 0;
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t edge_idx) const {
    auto parent_idx = get_parent_idx(dag, edge_idx);
    if (is_ua(dag, parent_idx)) return 0;
    auto ev = dag.get_edge(edge_idx);
    return std::visit(
        [](auto edge) -> std::size_t { return edge.mutations().size(); }, ev);
  }

  std::pair<weight_type, std::vector<std::size_t>> within_clade_accum(
      std::vector<weight_type> const& weights) const {
    assert(!weights.empty());
    weight_type best = *std::min_element(weights.begin(), weights.end());
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < weights.size(); ++i)
      if (weights[i] == best) indices.push_back(i);
    return {best, indices};
  }

  weight_type between_clades(std::vector<weight_type> const& weights) const {
    weight_type sum = 0;
    for (auto w : weights) sum += w;
    return sum;
  }

  weight_type above_node(weight_type edge_weight,
                         weight_type child_weight) const {
    return edge_weight + child_weight;
  }
};

// Stored protobuf/in-memory edge_weight scoring.  Lower total stored edge
// weight is better, matching subtree_weight's minimum-weight semantics.
struct edge_weight_score_ops {
  using weight_type = double;

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return 0.0;
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t edge_idx) const {
    auto ev = dag.get_edge(edge_idx);
    return std::visit(
        [](auto edge) -> double {
          return static_cast<double>(edge.edge_weight());
        },
        ev);
  }

  std::pair<weight_type, std::vector<std::size_t>> within_clade_accum(
      std::vector<weight_type> const& weights) const {
    assert(!weights.empty());
    double best = *std::min_element(weights.begin(), weights.end());
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < weights.size(); ++i)
      if (within_min_weight_tie(weights[i], best)) indices.push_back(i);
    return {best, indices};
  }

  weight_type between_clades(std::vector<weight_type> const& weights) const {
    double sum = 0.0;
    for (auto w : weights) sum += w;
    return sum;
  }

  weight_type above_node(weight_type edge_weight,
                         weight_type child_weight) const {
    return edge_weight + child_weight;
  }
};

// Max parsimony binary ops: for computing maximum parsimony score (worst tree)
struct max_parsimony_binary_ops {
  using weight_type = std::size_t;
  static inline weight_type const identity{0};

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return 0;
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t edge_idx) const {
    auto ev = dag.get_edge(edge_idx);
    return std::visit(
        [](auto edge) -> std::size_t { return edge.mutations().size(); }, ev);
  }

  bool compare(weight_type lhs, weight_type rhs) const {
    return lhs > rhs;  // maximize
  }

  bool compare_equal(weight_type lhs, weight_type rhs) const {
    return lhs == rhs;
  }

  weight_type combine(weight_type lhs, weight_type rhs) const {
    return lhs + rhs;
  }
};

using max_parsimony_score_ops = simple_weight_ops<max_parsimony_binary_ops>;

}  // namespace larch

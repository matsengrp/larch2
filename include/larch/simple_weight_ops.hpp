#pragma once

#include <larch/phylo_dag.hpp>

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace larch {

template <typename BinaryOps>
struct simple_weight_ops {
  using weight_type = typename BinaryOps::weight_type;

  simple_weight_ops() = default;
  explicit simple_weight_ops(BinaryOps ops) : ops_{std::move(ops)} {}

  weight_type compute_leaf(phylo_dag& dag, std::size_t node_idx) const {
    return ops_.compute_leaf(dag, node_idx);
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t edge_idx) const {
    return ops_.compute_edge(dag, edge_idx);
  }

  std::pair<weight_type, std::vector<std::size_t>> within_clade_accum(
      std::vector<weight_type> const& weights) const {
    assert(!weights.empty());
    weight_type optimal = weights.at(0);
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < weights.size(); ++i) {
      if (ops_.compare(weights[i], optimal)) {
        optimal = weights[i];
        indices.clear();
        indices.push_back(i);
      } else if (ops_.compare_equal(weights[i], optimal)) {
        indices.push_back(i);
      }
    }
    return {ops_.combine(BinaryOps::identity, optimal), indices};
  }

  weight_type between_clades(std::vector<weight_type> const& weights) const {
    weight_type result = BinaryOps::identity;
    for (auto const& w : weights) result = ops_.combine(result, w);
    return result;
  }

  weight_type above_node(weight_type edge_w, weight_type child_w) const {
    return ops_.combine(edge_w, child_w);
  }

  BinaryOps const& get_ops() const { return ops_; }

 private:
  BinaryOps ops_{};
};

}  // namespace larch

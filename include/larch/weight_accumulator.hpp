#pragma once

#include <larch/weight_counter.hpp>
#include <larch/phylo_dag.hpp>

#include <cassert>
#include <numeric>
#include <utility>
#include <vector>

namespace larch {

template <typename Ops>
struct weight_accumulator {
  using weight_type = weight_counter<Ops>;

  weight_accumulator() = default;
  explicit weight_accumulator(Ops ops) : ops_{std::move(ops)} {}

  weight_type compute_leaf(phylo_dag& dag, std::size_t idx) const {
    return weight_type(
        std::vector<typename Ops::weight_type>{ops_.compute_leaf(dag, idx)});
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t idx) const {
    return weight_type(
        std::vector<typename Ops::weight_type>{ops_.compute_edge(dag, idx)});
  }

  // Sum all counters (union) and return all indices
  std::pair<weight_type, std::vector<std::size_t>> within_clade_accum(
      std::vector<weight_type> const& weights) const {
    std::vector<std::size_t> indices(weights.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    return {std::accumulate(weights.begin(), weights.end(), weight_type{}),
            indices};
  }

  // Multiply all counters (cartesian product)
  weight_type between_clades(std::vector<weight_type> const& weights) const {
    if (weights.size() == 1) return weights[0];
    return std::accumulate(
        std::next(weights.begin()), weights.end(), weights.front(),
        [](auto&& lhs, auto const& rhs) { return lhs * rhs; });
  }

  // For each child weight, apply inner above_node and accumulate counts
  weight_type above_node(weight_type edge_w, weight_type child_w) const {
    assert(edge_w.get_weights().size() == 1);
    auto edge_it = edge_w.get_weights().begin();
    typename weight_type::map_type result;
    for (auto const& [child_weight, child_count] : child_w.get_weights())
      result[ops_.above_node(edge_it->first, child_weight)] += child_count;
    return weight_type(std::move(result));
  }

 private:
  Ops ops_{};
};

}  // namespace larch

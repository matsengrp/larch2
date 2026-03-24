#pragma once

#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>

#include <limits>
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

// WeightOps for maximum-likelihood tree scoring using a neural network model.
// Weight = negative log-likelihood (lower = better, matching parsimony
// semantics where min weight = optimal tree).
//
// Model must provide: double log_likelihood(string_view parent, string_view
// child)
template <typename Model>
struct likelihood_score_ops {
  using weight_type = double;

  Model const& model;
  std::string const& reference;

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return 0.0;
  }

  weight_type compute_edge(phylo_dag& dag, std::size_t edge_idx) const {
    auto ev = dag.get_edge(edge_idx);
    return std::visit(
        [&](auto edge) -> double {
          if (edge.mutations().empty()) return 0.0;

          auto parent_idx =
              std::visit([](auto p) { return p.index(); }, edge.get_parent());
          auto child_idx =
              std::visit([](auto c) { return c.index(); }, edge.get_child());

          auto parent_seq = reconstruct_sequence(dag, parent_idx, reference);
          auto child_seq = reconstruct_sequence(dag, child_idx, reference);

          return -model.log_likelihood(parent_seq, child_seq);
        },
        ev);
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

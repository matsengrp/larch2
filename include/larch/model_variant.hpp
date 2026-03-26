#pragma once

#include <larch/compute.hpp>
#include <larch/indep_rs_cnn_model.hpp>
#include <larch/likelihood_score_ops.hpp>
#include <larch/rs_fivemer_model.hpp>
#include <larch/spr_move.hpp>
#include <larch/yaml_reader.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
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

// Adjust rate bias through variant dispatch.
inline void ml_adjust_rate_bias(ml_model& model, double log_factor) {
  std::visit([&](auto& m) { m.adjust_rate_bias_by(log_factor); }, model);
}

// Compute total negative log-likelihood for all non-UA mutated edges in a DAG.
inline double compute_dag_ml_nll(ml_model const& model, phylo_dag& dag) {
  auto const& ref = get_reference_sequence(dag);
  double total_nll = 0.0;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (edge.mutations().empty()) return;
          auto parent_idx =
              std::visit([](auto p) { return p.index(); }, edge.get_parent());
          if (is_ua(dag, parent_idx)) return;
          auto child_idx =
              std::visit([](auto c) { return c.index(); }, edge.get_child());
          auto parent_seq = reconstruct_sequence(dag, parent_idx, ref);
          auto child_seq = reconstruct_sequence(dag, child_idx, ref);
          total_nll += -ml_log_likelihood(model, parent_seq, child_seq);
        },
        ev);
  }
  return total_nll;
}

// Compute total NLL for all edges in a subtree of a tree, starting from
// subtree_root_idx downward.  Skips the UA→root edge.
inline double compute_subtree_ml_nll(ml_model const& model, phylo_dag& tree,
                                     std::size_t subtree_root_idx) {
  auto const& ref = get_reference_sequence(tree);
  double total_nll = 0.0;

  // DFS walk from subtree_root_idx.
  std::vector<std::size_t> stack = {subtree_root_idx};
  while (!stack.empty()) {
    auto nidx = stack.back();
    stack.pop_back();
    if (is_leaf(tree, nidx)) continue;

    auto clades = get_clades(tree, nidx);
    for (auto const& edges : clades) {
      for (auto edge_idx : edges) {
        auto child_idx = get_child_idx(tree, edge_idx);
        // Score this edge.
        auto ev = tree.get_edge(edge_idx);
        std::visit(
            [&](auto edge) {
              if (!edge.mutations().empty()) {
                auto parent_seq = reconstruct_sequence(tree, nidx, ref);
                auto child_seq = reconstruct_sequence(tree, child_idx, ref);
                total_nll +=
                    -ml_log_likelihood(model, parent_seq, child_seq);
              }
            },
            ev);
        stack.push_back(child_idx);
      }
    }
  }
  return total_nll;
}

// Compute delta ML score for an SPR move: old_NLL - new_NLL.
// Positive means the move improved the likelihood (new tree has lower NLL).
// The fragment contains the new topology; the original tree provides old edges.
inline double compute_delta_ml_score(ml_model const& model,
                                     phylo_dag& original_tree,
                                     phylo_dag& fragment,
                                     spr_move const& move) {
  double new_nll = compute_dag_ml_nll(model, fragment);
  double old_nll = compute_subtree_ml_nll(model, original_tree, move.lca);
  return old_nll - new_nll;
}

// Backward-compatible alias.
inline double compute_fragment_ml_score(ml_model const& model,
                                        phylo_dag& fragment) {
  return compute_dag_ml_nll(model, fragment);
}

}  // namespace larch

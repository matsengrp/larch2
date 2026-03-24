#pragma once

#include <larch/compute.hpp>
#include <larch/indep_rs_cnn_model.hpp>
#include <larch/likelihood_score_ops.hpp>
#include <larch/rs_fivemer_model.hpp>
#include <larch/yaml_reader.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

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

// Compute the total negative log-likelihood for all mutated edges in a
// fragment DAG.  Used to re-score SPR moves with ML.
inline double compute_fragment_ml_score(ml_model const& model,
                                        phylo_dag& fragment) {
  auto const& ref = get_reference_sequence(fragment);
  double total_nll = 0.0;
  for (auto ev : fragment.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (edge.mutations().empty()) return;
          auto parent_idx =
              std::visit([](auto p) { return p.index(); }, edge.get_parent());
          // Skip edges from UA node.
          if (is_ua(fragment, parent_idx)) return;
          auto child_idx =
              std::visit([](auto c) { return c.index(); }, edge.get_child());
          auto parent_seq = reconstruct_sequence(fragment, parent_idx, ref);
          auto child_seq = reconstruct_sequence(fragment, child_idx, ref);
          total_nll += -ml_log_likelihood(model, parent_seq, child_seq);
        },
        ev);
  }
  return total_nll;
}

}  // namespace larch

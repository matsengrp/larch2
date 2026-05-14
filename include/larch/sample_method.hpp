#pragma once

#include <optional>
#include <string_view>

namespace larch {

enum class sample_method {
  random,
  parsimony,
  ml,
  edge_weight,
  rf_minsum,
  rf_maxsum,
};

enum class objective_kind {
  generic,
  parsimony,
  ml_nll,
  edge_weight,
  sum_rf,
};

inline std::optional<sample_method> parse_sample_method(
    std::string_view method, bool allow_rf = false) noexcept {
  if (method == "random") return sample_method::random;
  if (method == "parsimony") return sample_method::parsimony;
  if (method == "ml" || method == "thrifty") return sample_method::ml;
  if (method == "edge-weight" || method == "edge_weight")
    return sample_method::edge_weight;
  if (allow_rf && method == "rf-minsum") return sample_method::rf_minsum;
  if (allow_rf && method == "rf-maxsum") return sample_method::rf_maxsum;
  return std::nullopt;
}

inline std::string_view format_sample_method(sample_method method) noexcept {
  switch (method) {
    case sample_method::random:
      return "random";
    case sample_method::parsimony:
      return "parsimony";
    case sample_method::ml:
      return "ml";
    case sample_method::edge_weight:
      return "edge-weight";
    case sample_method::rf_minsum:
      return "rf-minsum";
    case sample_method::rf_maxsum:
      return "rf-maxsum";
  }
  return "unknown";
}

inline bool is_ml_sample_method(sample_method method) noexcept {
  return method == sample_method::ml;
}

inline bool is_edge_weight_sample_method(sample_method method) noexcept {
  return method == sample_method::edge_weight;
}

inline bool is_rf_sample_method(sample_method method) noexcept {
  return method == sample_method::rf_minsum ||
         method == sample_method::rf_maxsum;
}

inline bool is_ml_sample_method(std::string_view method) {
  auto parsed = parse_sample_method(method, true);
  return parsed && is_ml_sample_method(*parsed);
}

inline bool is_edge_weight_sample_method(std::string_view method) {
  auto parsed = parse_sample_method(method, true);
  return parsed && is_edge_weight_sample_method(*parsed);
}

inline bool is_rf_sample_method(std::string_view method) {
  auto parsed = parse_sample_method(method, true);
  return parsed && is_rf_sample_method(*parsed);
}

inline bool is_known_sample_method(std::string_view method,
                                   bool allow_rf = false) {
  return parse_sample_method(method, allow_rf).has_value();
}

inline std::optional<objective_kind> parse_objective_kind(
    std::string_view kind) noexcept {
  if (kind == "objective") return objective_kind::generic;
  if (kind == "parsimony") return objective_kind::parsimony;
  if (kind == "ML NLL" || kind == "ml" || kind == "ml-nll")
    return objective_kind::ml_nll;
  if (kind == "edge_weight" || kind == "edge-weight")
    return objective_kind::edge_weight;
  if (kind == "sum RF" || kind == "sum-rf") return objective_kind::sum_rf;
  return std::nullopt;
}

inline std::string_view format_objective_kind(objective_kind kind) noexcept {
  switch (kind) {
    case objective_kind::generic:
      return "objective";
    case objective_kind::parsimony:
      return "parsimony";
    case objective_kind::ml_nll:
      return "ML NLL";
    case objective_kind::edge_weight:
      return "edge_weight";
    case objective_kind::sum_rf:
      return "sum RF";
  }
  return "objective";
}

}  // namespace larch

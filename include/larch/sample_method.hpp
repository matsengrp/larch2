#pragma once

#include <string_view>

namespace larch {

inline bool is_ml_sample_method(std::string_view method) {
  return method == "ml" || method == "thrifty";
}

inline bool is_edge_weight_sample_method(std::string_view method) {
  return method == "edge-weight" || method == "edge_weight";
}

inline bool is_rf_sample_method(std::string_view method) {
  return method == "rf-minsum" || method == "rf-maxsum";
}

inline bool is_known_sample_method(std::string_view method,
                                   bool allow_rf = false) {
  return method == "random" || method == "parsimony" ||
         is_ml_sample_method(method) || is_edge_weight_sample_method(method) ||
         (allow_rf && is_rf_sample_method(method));
}

}  // namespace larch

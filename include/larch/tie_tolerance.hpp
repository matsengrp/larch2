#pragma once

#include <algorithm>
#include <cmath>

namespace larch {

inline constexpr double min_weight_abs_tie_tolerance = 1e-10;
inline constexpr double min_weight_rel_tie_tolerance = 1e-10;

inline double min_weight_tie_tolerance(
    double best, double abs_tol = min_weight_abs_tie_tolerance,
    double rel_tol = min_weight_rel_tie_tolerance) {
  return abs_tol + rel_tol * std::max(1.0, std::abs(best));
}

inline bool within_min_weight_tie(
    double value, double best, double abs_tol = min_weight_abs_tie_tolerance,
    double rel_tol = min_weight_rel_tie_tolerance) {
  return value <= best + min_weight_tie_tolerance(best, abs_tol, rel_tol);
}

}  // namespace larch

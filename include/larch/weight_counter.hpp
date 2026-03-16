#pragma once

#include <larch/bigint.hpp>

#include <flat_map>
#include <initializer_list>
#include <ostream>
#include <utility>
#include <vector>

namespace larch {

template <typename Ops>
class weight_counter {
 public:
  using map_type = std::flat_map<typename Ops::weight_type, bigint>;

  weight_counter() = default;

  explicit weight_counter(
      std::vector<typename Ops::weight_type> const& weights) {
    for (auto const& w : weights) ++weights_[w];
  }

  explicit weight_counter(map_type weights) : weights_(std::move(weights)) {}

  weight_counter(
      std::initializer_list<std::pair<typename Ops::weight_type, bigint>>
          init) {
    for (auto const& [k, v] : init) weights_[k] = v;
  }

  // Union of multisets: add counts for matching weights
  weight_counter operator+(weight_counter const& rhs) const {
    map_type result = weights_;
    for (auto const& [key, count] : rhs.weights_) result[key] += count;
    return weight_counter(std::move(result));
  }

  // Cartesian product: apply ops.between_clades to each pair
  weight_counter operator*(weight_counter const& rhs) const {
    Ops ops;
    map_type result;
    for (auto const& [lw, lc] : weights_)
      for (auto const& [rw, rc] : rhs.weights_)
        result[ops.between_clades({lw, rw})] += lc * rc;
    return weight_counter(std::move(result));
  }

  bool operator==(weight_counter const& rhs) const = default;

  map_type const& get_weights() const { return weights_; }

  friend std::ostream& operator<<(std::ostream& os, weight_counter const& wc) {
    auto const& w = wc.weights_;
    if (w.empty()) return os << "{}";
    os << "{";
    bool first = true;
    for (auto const& [key, count] : w) {
      if (!first) os << ", ";
      os << key << ": " << count;
      first = false;
    }
    return os << "}";
  }

 private:
  map_type weights_;
};

}  // namespace larch

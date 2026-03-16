#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace larch {

class leaf_set {
  std::vector<std::vector<std::size_t>> clades_;
  std::size_t hash_ = 0;

  static std::size_t compute_hash(
      std::vector<std::vector<std::size_t>> const& clades) {
    std::size_t h = 0;
    for (auto const& clade : clades) {
      for (auto idx : clade) {
        h ^= std::hash<std::size_t>{}(idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
      }
    }
    return h;
  }

 public:
  leaf_set() = default;

  explicit leaf_set(std::vector<std::vector<std::size_t>> clades)
      : clades_{std::move(clades)}, hash_{compute_hash(clades_)} {}

  std::vector<std::size_t> to_parent_clade() const {
    std::vector<std::size_t> result;
    for (auto const& clade : clades_) {
      result.insert(result.end(), clade.begin(), clade.end());
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }

  bool empty() const { return clades_.empty(); }
  std::size_t size() const { return clades_.size(); }
  std::size_t hash() const { return hash_; }

  auto const& clades() const { return clades_; }
  auto begin() const { return clades_.begin(); }
  auto end() const { return clades_.end(); }

  bool operator==(leaf_set const& rhs) const { return clades_ == rhs.clades_; }
};

}  // namespace larch

template <>
struct std::hash<larch::leaf_set> {
  std::size_t operator()(larch::leaf_set const& ls) const noexcept {
    return ls.hash();
  }
};

template <>
struct std::equal_to<larch::leaf_set> {
  bool operator()(larch::leaf_set const& a,
                  larch::leaf_set const& b) const noexcept {
    return a == b;
  }
};

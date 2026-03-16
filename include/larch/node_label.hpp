#pragma once

#include <larch/common.hpp>

#include <cstddef>
#include <functional>

namespace larch {

struct node_label {
  std::size_t cg_idx = no_idx;
  std::size_t ls_idx = no_idx;
  std::size_t leaf_id_idx =
      no_idx;  // unique leaf identity index (for leaf_set construction)
  std::string sample_id;

  bool empty() const {
    return cg_idx == no_idx && ls_idx == no_idx && sample_id.empty();
  }

  bool operator==(node_label const& rhs) const {
    if (ls_idx != rhs.ls_idx) return false;
    if (!sample_id.empty()) {
      return sample_id == rhs.sample_id;
    }
    return cg_idx == rhs.cg_idx;
  }

  std::size_t hash() const {
    std::size_t unique = sample_id.empty()
                             ? std::hash<std::size_t>{}(cg_idx)
                             : std::hash<std::string>{}(sample_id);
    return unique ^ (std::hash<std::size_t>{}(ls_idx) + 0x9e3779b9 +
                     (unique << 6) + (unique >> 2));
  }
};

}  // namespace larch

template <>
struct std::hash<larch::node_label> {
  std::size_t operator()(larch::node_label const& nl) const noexcept {
    return nl.hash();
  }
};

template <>
struct std::equal_to<larch::node_label> {
  bool operator()(larch::node_label const& a,
                  larch::node_label const& b) const noexcept {
    return a == b;
  }
};

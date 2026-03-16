#pragma once

#include <larch/node_label.hpp>
#include <larch/leaf_set.hpp>

#include <cstddef>
#include <functional>
#include <vector>

namespace larch {

struct edge_label {
  node_label parent;
  node_label child;

  bool operator==(edge_label const& rhs) const {
    return parent == rhs.parent && child == rhs.child;
  }

  std::size_t hash() const {
    auto h1 = parent.hash();
    auto h2 = child.hash();
    return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
  }

  // Compute which clade of the parent's leaf set contains the child's leaves
  std::size_t compute_clade_index(
      std::vector<std::vector<std::size_t>> const& parent_clades,
      leaf_set const& child_ls) const {
    // Get child's parent clade (all leaves reachable from child, flattened)
    std::vector<std::size_t> child_parent_clade;
    if (child_ls.empty()) {
      // Leaf node: parent clade is just itself (by leaf_id_idx)
      child_parent_clade.push_back(child.leaf_id_idx);
    } else {
      child_parent_clade = child_ls.to_parent_clade();
    }

    for (std::size_t i = 0; i < parent_clades.size(); ++i) {
      if (parent_clades[i] == child_parent_clade) {
        return i;
      }
    }
    // Should not reach here
    return 0;
  }
};

}  // namespace larch

template <>
struct std::hash<larch::edge_label> {
  std::size_t operator()(larch::edge_label const& el) const noexcept {
    return el.hash();
  }
};

template <>
struct std::equal_to<larch::edge_label> {
  bool operator()(larch::edge_label const& a,
                  larch::edge_label const& b) const noexcept {
    return a == b;
  }
};

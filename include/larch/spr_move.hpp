#pragma once

#include <larch/compute.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_set>

namespace larch {

struct spr_move {
  std::size_t src;
  std::size_t dst;
  std::size_t lca;
  std::optional<int> score_change;
};

using move_callback = std::function<void(spr_move const&)>;

// Compute LCA by walking up from both nodes to find intersection.
// For optimizers that don't compute LCA themselves.
inline std::size_t compute_lca(phylo_dag& tree, std::size_t a, std::size_t b) {
  std::unordered_set<std::size_t> ancestors_a;
  std::size_t cur = a;
  while (true) {
    ancestors_a.insert(cur);
    auto pe = get_parent_edges(tree, cur);
    if (pe.empty()) break;
    cur = get_parent_idx(tree, pe[0]);
  }

  cur = b;
  while (true) {
    if (ancestors_a.contains(cur)) return cur;
    auto pe = get_parent_edges(tree, cur);
    if (pe.empty()) break;
    cur = get_parent_idx(tree, pe[0]);
  }

  return cur;
}

}  // namespace larch

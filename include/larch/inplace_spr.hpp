#pragma once

#include <larch/native_optimize.hpp>

#include <cstddef>
#include <cstdint>

namespace larch {

// ============================================================================
// Phase 1: spr_result — captures all topology changes from an in-place SPR
// ============================================================================

struct spr_result {
  std::size_t src;            // moved node
  std::size_t dst;            // destination (original child of dst_parent)
  std::size_t src_parent;     // original parent of src
  std::size_t dst_parent;     // parent of dst (now parent of new_inner)
  std::size_t new_inner;      // freshly created inner node
  std::size_t lca;            // lowest common ancestor of src and dst

  bool src_parent_collapsed;  // true if src_parent was binary and removed
  std::size_t grandparent;    // parent of collapsed src_parent (valid iff collapsed)
  std::size_t remaining_child;  // sibling of src that was reparented (valid iff collapsed)
  std::size_t collapsed_node;   // index of the removed node (valid iff collapsed)
};

// ============================================================================
// Phase 2: tree_state — mutable tree + index + running score for multi-step loop
// ============================================================================

struct tree_state {
  phylo_dag& tree;
  tree_index index;
  int parsimony_score;     // running total
  std::size_t step_count;  // moves applied so far

  // Non-copyable/movable: index holds a reference to the same tree,
  // and accidental copies would create a stale index sharing the same DAG.
  tree_state(tree_state const&) = delete;
  tree_state& operator=(tree_state const&) = delete;
  tree_state(tree_state&&) = delete;
  tree_state& operator=(tree_state&&) = delete;

  explicit tree_state(phylo_dag& t)
      : tree{t}, index{t}, parsimony_score{0}, step_count{0} {
    parsimony_score = compute_parsimony_from_index(index);
  }

  tree_state(phylo_dag& t, thread_pool& pool)
      : tree{t}, index{t, pool}, parsimony_score{0}, step_count{0} {
    parsimony_score = compute_parsimony_from_index(index);
  }

  // Compute total parsimony score by summing Fitch costs across all inner
  // nodes and variable sites.  A Fitch cost of 1 at a site means the node's
  // children disagree (no single allele is present in all children).
  static int compute_parsimony_from_index(tree_index const& idx) {
    int score = 0;
    auto root = idx.get_tree_root();
    score = compute_parsimony_below(idx, root);
    return score;
  }

 private:
  // Recursive bottom-up Fitch cost accumulation.
  static int compute_parsimony_below(tree_index const& idx, std::size_t node) {
    int cost = 0;
    auto const& children = idx.get_children(node);

    // Leaf nodes have zero Fitch cost.
    if (children.empty()) return 0;

    // Recurse into children first.
    for (auto child : children) {
      cost += compute_parsimony_below(idx, child);
    }

    // Add this node's Fitch cost: count sites where children disagree.
    auto n_sites = idx.num_variable_sites();
    uint8_t nc = idx.get_num_children(node);
    for (std::size_t i = 0; i < n_sites; i++) {
      auto const& counts = idx.get_child_counts(node, i);
      cost += fitch_cost_from_counts(counts, nc);
    }

    return cost;
  }
};

}  // namespace larch

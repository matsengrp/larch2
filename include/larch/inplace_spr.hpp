#pragma once

#include <larch/native_optimize.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

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

// ============================================================================
// Phase 4-5 helpers
// ============================================================================

// Result of collapsing a binary parent node.
struct collapse_info {
  std::size_t grandparent;       // parent of the collapsed node
  std::size_t remaining_child;   // sibling that was reparented to grandparent
  std::size_t collapsed_node;    // index of the removed node
};

// Return the single parent edge index for a tree node, without heap-allocating
// the std::vector that get_parent_edges() returns.
// Precondition: node has exactly 1 parent edge.
inline std::size_t get_parent_edge(phylo_dag& d, std::size_t node_idx) {
  std::size_t result = 0;
  bool found = false;
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_parents()) {
          std::visit(
              [&](auto edge) {
                result = edge.index();
                found = true;
              },
              ev);
          break;
        }
      },
      nv);
  assert(found && "node has no parent edge");
  return result;
}

// ============================================================================
// Phase 4: Detach source from parent
// ============================================================================

// Count the number of child edges for a node (does not rely on clade offsets).
inline std::size_t child_count(phylo_dag& d, std::size_t node_idx) {
  std::size_t count = 0;
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          (void)ev;
          ++count;
        }
      },
      nv);
  return count;
}

// Find the remaining child of a node that has exactly one child edge.
// Precondition: node has exactly 1 child.
inline std::size_t find_remaining_child(phylo_dag& d, std::size_t node_idx) {
  std::size_t result = 0;
  bool found = false;
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          std::visit(
              [&](auto edge) {
                auto cv = edge.get_child();
                result = std::visit([](auto c) { return c.index(); }, cv);
              },
              ev);
          found = true;
          break;  // only need the first (and only) child
        }
      },
      nv);
  assert(found && "expected exactly one child in find_remaining_child");
  return result;
}

// Detach src from its parent by removing the parent edge of src.
// Returns the number of children src_parent had *before* detach.
// After this call, src has 0 parent edges and src_parent has one fewer child.
inline std::size_t detach_source(phylo_dag& d, std::size_t src,
                                 std::size_t src_parent) {
  std::size_t old_child_count = child_count(d, src_parent);

  // Find and remove the parent edge of src
  auto pe = get_parent_edge(d, src);
  assert(get_parent_idx(d, pe) == src_parent && "src_parent does not match actual parent");
  auto ev = d.get_edge(pe);
  std::visit([](auto edge) { edge.remove(); }, ev);

  return old_child_count;
}

// ============================================================================
// Phase 5: Collapse binary source parent
// ============================================================================

// If src_parent had exactly 2 children before detach (now 1), collapse it:
// wire its remaining child directly to its grandparent, then remove src_parent.
//
// Returns collapse_info if collapse happened, std::nullopt otherwise.
inline std::optional<collapse_info> collapse_binary_parent(
    phylo_dag& d, std::size_t src_parent, std::size_t old_child_count) {
  if (old_child_count != 2) return std::nullopt;

  // src_parent now has exactly 1 child — find it
  std::size_t remaining_child = find_remaining_child(d, src_parent);

  // Find grandparent (parent of src_parent) and the clade index of that edge
  auto sp_pe = get_parent_edge(d, src_parent);
  std::size_t grandparent = get_parent_idx(d, sp_pe);
  std::size_t gp_clade = get_clade_idx(d, sp_pe);

  // Remove src_parent node (removes all its edges — the remaining child edge
  // and the parent edge to grandparent)
  auto sp_nv = d.get_node(src_parent);
  std::visit([](auto n) { n.remove(); }, sp_nv);

  // Reconnect: grandparent -> remaining_child with the same clade index
  auto e = d.append_edge<edge_kind::clade>();
  e.clade_index() = gp_clade;
  auto gpv = d.get_node(grandparent);
  std::visit([&](auto p) { e.set_parent(p); }, gpv);
  auto rcv = d.get_node(remaining_child);
  std::visit([&](auto c) { e.set_child(c); }, rcv);

  return collapse_info{grandparent, remaining_child, src_parent};
}

}  // namespace larch

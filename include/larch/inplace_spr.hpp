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

// Check whether a node has at least one parent edge, without heap-allocating
// the std::vector that get_parent_edges() returns.
inline bool has_parent_edge(phylo_dag& d, std::size_t node_idx) {
  bool found = false;
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_parents()) {
          (void)ev;
          found = true;
          break;
        }
      },
      nv);
  return found;
}

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
          break;  // only need the first (and only) parent edge
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

// ============================================================================
// Phase 6: Reattach at destination
// ============================================================================

// Result of reattach_at_destination: the new inner node and the parent it was
// inserted under (i.e., the effective dst_parent at reattach time).
struct reattach_result {
  std::size_t new_inner;   // freshly created inner node
  std::size_t dst_parent;  // parent of new_inner (= dst's parent before reattach)
};

// Create a new inner node between dst and its parent, then attach src as a
// sibling of dst under this new inner node.
//
// Preconditions:
//   - src has no parent edge (already detached via detach_source).
//   - dst has exactly one parent edge.
//   - dst is not the UA node.
//   - src != dst.
//
// Note: dst_parent is resolved to a chain index (stable across appends), so
// the index remains valid after append_node/append_edge calls below.
//
// Returns the new inner node index and the dst_parent it was inserted under.
inline reattach_result reattach_at_destination(phylo_dag& d, std::size_t src,
                                               std::size_t dst) {
  assert(!has_parent_edge(d, src) && "src must have no parent edge (already detached)");
  assert(!is_ua(d, dst) && "dst cannot be the UA node");
  assert(src != dst && "src and dst must be different nodes");

  // 1. Find dst_parent and the clade index of the dst_parent -> dst edge
  auto dst_pe = get_parent_edge(d, dst);
  std::size_t dst_parent = get_parent_idx(d, dst_pe);
  std::size_t dst_clade = get_clade_idx(d, dst_pe);

  // 2. Remove the dst_parent -> dst edge
  auto ev = d.get_edge(dst_pe);
  std::visit([](auto edge) { edge.remove(); }, ev);

  // 3. Append a new inner node
  auto new_inner = d.append_node<node_kind::inner>();
  auto ni_idx = new_inner.index();

  // 4. Add edge: dst_parent -> new_inner (same clade index as removed edge)
  {
    auto e = d.append_edge<edge_kind::clade>();
    e.clade_index() = dst_clade;
    auto pv = d.get_node(dst_parent);
    std::visit([&](auto p) { e.set_parent(p); }, pv);
    auto cv = d.get_node(ni_idx);
    std::visit([&](auto c) { e.set_child(c); }, cv);
  }

  // 5. Add edge: new_inner -> dst (clade index 0)
  {
    auto e = d.append_edge<edge_kind::clade>();
    e.clade_index() = 0;
    auto pv = d.get_node(ni_idx);
    std::visit([&](auto p) { e.set_parent(p); }, pv);
    auto cv = d.get_node(dst);
    std::visit([&](auto c) { e.set_child(c); }, cv);
  }

  // 6. Add edge: new_inner -> src (clade index 1)
  {
    auto e = d.append_edge<edge_kind::clade>();
    e.clade_index() = 1;
    auto pv = d.get_node(ni_idx);
    std::visit([&](auto p) { e.set_parent(p); }, pv);
    auto cv = d.get_node(src);
    std::visit([&](auto c) { e.set_child(c); }, cv);
  }

  return reattach_result{ni_idx, dst_parent};
}

// ============================================================================
// Phase 7: apply_spr_inplace — assemble phases 4–6 into a single function
// ============================================================================

// Compute the lowest common ancestor of two nodes using tree_index DFS
// intervals.  Must be called before any topology changes, since detach/collapse
// invalidates the ancestor relationship.
inline std::size_t compute_lca(tree_index const& index, std::size_t a,
                               std::size_t b) {
  if (index.is_ancestor(a, b)) return a;
  if (index.is_ancestor(b, a)) return b;
  // Walk up from a until we find an ancestor of b.
  auto cur = index.get_parent(a);
  while (!index.is_ancestor(cur, b)) {
    cur = index.get_parent(cur);
  }
  return cur;
}

// Apply an SPR move in-place on the phylo_dag: detach src from its parent,
// optionally collapse the binary parent, then reattach src as a sibling of dst
// under a freshly created inner node.
//
// The tree_index is used only to compute the LCA before topology changes.
// This function does NOT recompute CGs, edge mutations, or Fitch sets.
inline spr_result apply_spr_inplace(phylo_dag& tree, tree_index const& index,
                                    std::size_t src, std::size_t dst) {
  // 1. Record src_parent and child count.
  std::size_t src_parent_val =
      get_parent_idx(tree, get_parent_edge(tree, src));

  // 2. Compute LCA before any topology changes.
  std::size_t lca = compute_lca(index, src, dst);

  // 3. Detach src (Phase 4).
  auto old_child_count = detach_source(tree, src, src_parent_val);

  // 4. If binary, collapse src_parent (Phase 5).
  auto cinfo = collapse_binary_parent(tree, src_parent_val, old_child_count);

  // 5. Reattach at dst (Phase 6).  This also resolves dst_parent after
  //    collapse (may have changed if src_parent == dst_parent collapsed).
  auto [new_inner, dst_parent_val] = reattach_at_destination(tree, src, dst);

  // 6. Populate and return spr_result.
  return spr_result{
      .src = src,
      .dst = dst,
      .src_parent = src_parent_val,
      .dst_parent = dst_parent_val,
      .new_inner = new_inner,
      .lca = lca,
      .src_parent_collapsed = cinfo.has_value(),
      .grandparent = cinfo ? cinfo->grandparent : 0,
      .remaining_child = cinfo ? cinfo->remaining_child : 0,
      .collapsed_node = cinfo ? cinfo->collapsed_node : 0,
  };
}

}  // namespace larch

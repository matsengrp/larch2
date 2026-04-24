#pragma once

#include <larch/compute.hpp>
#include <larch/merge.hpp>
#include <larch/random_optimize.hpp>
#include <larch/spr_move.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace larch {

// ============================================================================
// Data structures
// ============================================================================

struct dfs_info {
  std::size_t dfs_index;
  std::size_t dfs_end_index;
  std::size_t level;
};

struct profitable_move {
  std::size_t src;
  std::size_t dst;
  std::size_t lca;
  int score_change;
};

struct move_coefficients {
  int pscore_coeff = 1;
  int node_coeff = 0;  // 0 = no node penalty (default matches current behavior)

  bool has_node_penalty() const { return node_coeff != 0; }

  int apply(int parsimony_change, int novel_node_count) const {
    return pscore_coeff * parsimony_change + node_coeff * novel_node_count;
  }
};

struct src_removal_result {
  int score_change{0};
  std::vector<uint8_t> new_fitch;
  std::vector<uint8_t> old_fitch;
  int lca_nc_adjustment{0};

  void resize(std::size_t n_sites) {
    new_fitch.resize(n_sites);
    old_fitch.resize(n_sites);
  }
};

struct scratch_buffers {
  std::vector<uint8_t> new_node_fitch;
  std::vector<uint8_t> prev_old_fitch;
  std::vector<uint8_t> prev_new_fitch;
  src_removal_result removal;

  void resize(std::size_t n_sites) {
    new_node_fitch.resize(n_sites);
    prev_old_fitch.resize(n_sites);
    prev_new_fitch.resize(n_sites);
    removal.resize(n_sites);
  }
};

// ============================================================================
// Free helper functions
// ============================================================================
// base_to_one_hot and fitch_set_from_counts are defined in compute.hpp

inline int fitch_cost_from_counts(std::array<uint32_t, 4> const& counts,
                                  uint32_t num_children) {
  if (num_children <= 1) return 0;
  for (int i = 0; i < 4; i++) {
    if (counts[i] == num_children) return 0;
  }
  return 1;
}

inline std::size_t compute_tree_max_depth(phylo_dag& d) {
  auto root_idx = get_root_idx(d);
  // BFS to find the child of UA (tree root)
  std::size_t tree_root = 0;
  auto clades = get_clades(d, root_idx);
  if (!clades.empty() && !clades[0].empty())
    tree_root = get_child_idx(d, clades[0][0]);

  // BFS for max depth
  std::size_t max_depth = 0;
  std::queue<std::pair<std::size_t, std::size_t>> q;
  q.push({tree_root, 0});
  while (!q.empty()) {
    auto [idx, depth] = q.front();
    q.pop();
    if (depth > max_depth) max_depth = depth;
    auto c = get_clades(d, idx);
    for (auto& clade : c)
      for (auto eidx : clade) q.push({get_child_idx(d, eidx), depth + 1});
  }
  return max_depth;
}

// ============================================================================
// spr_result: captures all topology changes from an in-place SPR
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
// tree_index: Precomputed tree data for move enumeration
// ============================================================================

class tree_index {
 public:
  explicit tree_index(phylo_dag& d) : d_{d} { init(); }

  tree_index(phylo_dag& d, thread_pool& pool) : d_{d}, pool_{&pool} { init(); }

  std::vector<std::size_t> const& get_variable_sites() const {
    return variable_sites_;
  }
  std::vector<std::size_t> const& get_searchable_nodes() const {
    return searchable_nodes_;
  }

  bool has_dfs_info(std::size_t node) const {
    return node < num_nodes_ && has_dfs_info_[node];
  }
  dfs_info const& get_dfs_info(std::size_t node) const {
    return dfs_info_[node];
  }

  bool is_ancestor(std::size_t ancestor, std::size_t descendant) const {
    if (ancestor >= num_nodes_ || descendant >= num_nodes_) return false;
    if (!has_dfs_info_[ancestor] || !has_dfs_info_[descendant]) return false;
    return dfs_info_[ancestor].dfs_index <= dfs_info_[descendant].dfs_index &&
           dfs_info_[descendant].dfs_index < dfs_info_[ancestor].dfs_end_index;
  }

  uint8_t get_fitch_set(std::size_t node, std::size_t site_idx) const {
    return fitch_sets_[node * num_variable_sites_ + site_idx];
  }

  uint8_t const* get_fitch_set_ptr(std::size_t node) const {
    return &fitch_sets_[node * num_variable_sites_];
  }

  std::array<uint32_t, 4> const& get_child_counts(std::size_t node,
                                                  std::size_t site_idx) const {
    return child_counts_[node * num_variable_sites_ + site_idx];
  }

  bool has_child_counts(std::size_t node) const {
    return node < num_nodes_ && has_child_counts_[node];
  }

  uint32_t get_num_children(std::size_t node) const {
    return num_children_[node];
  }

  uint8_t get_allele_union(std::size_t node, std::size_t site_idx) const {
    return allele_union_[node * num_variable_sites_ + site_idx];
  }

  phylo_dag& get_dag() { return d_; }
  std::size_t get_tree_root() const { return tree_root_; }
  std::size_t get_parent(std::size_t node) const { return parent_[node]; }
  std::vector<std::size_t> const& get_children(std::size_t node) const {
    return children_[node];
  }
  std::size_t num_variable_sites() const { return num_variable_sites_; }
  std::size_t num_condensed_leaves() const { return condensed_count_; }
  bool is_condensed(std::size_t node) const {
    return node < num_nodes_ && is_condensed_[node];
  }
  std::size_t num_nodes() const { return num_nodes_; }
  uint8_t const* get_ref_alleles_ptr() const { return ref_alleles_.data(); }
  std::size_t get_subtree_size(std::size_t node) const { return subtree_size_[node]; }

  // Compute total parsimony score from tree_index flat arrays.
  // Iterates all valid inner nodes and counts sites where children disagree
  // (Fitch cost = 1).  O(N × sites), no recursion.
  int compute_parsimony_score() const {
    int score = 0;
    for (std::size_t nid = 0; nid < num_nodes_; nid++) {
      if (!is_valid_[nid] || !has_child_counts_[nid]) continue;
      uint32_t nc = num_children_[nid];
      if (nc == 0) continue;
      std::size_t base = nid * num_variable_sites_;
      for (std::size_t i = 0; i < num_variable_sites_; i++) {
        score += fitch_cost_from_counts(child_counts_[base + i], nc);
      }
    }
    return score;
  }

  bool is_valid(std::size_t node) const {
    return node < num_nodes_ && is_valid_[node];
  }

  // Grow all flat arrays so that node indices up to new_high_mark-1
  // are valid slots.  Called by update_topology when apply_spr_inplace appends
  // a node beyond the current capacity.
  void ensure_capacity(std::size_t new_high_mark) {
    if (new_high_mark <= num_nodes_) return;
    auto const site_slots = new_high_mark * num_variable_sites_;
    dfs_info_.resize(new_high_mark);
    has_dfs_info_.resize(new_high_mark, 0);
    is_valid_.resize(new_high_mark, 0);
    fitch_sets_.resize(site_slots, 0);
    child_counts_.resize(site_slots, {0, 0, 0, 0});
    has_child_counts_.resize(new_high_mark, 0);
    num_children_.resize(new_high_mark, 0);
    allele_union_.resize(site_slots, 0);
    parent_.resize(new_high_mark, 0);
    children_.resize(new_high_mark);
    is_condensed_.resize(new_high_mark, 0);
    subtree_size_.resize(new_high_mark, 0);
    num_nodes_ = new_high_mark;
  }

  // Patch parent_[], children_[], and is_valid_[] to reflect the
  // topology changes described by an spr_result.  Must be called after
  // apply_spr_inplace().
  //
  // Only parent_[], children_[], and is_valid_[] are updated here.
  // The following remain stale and must be refreshed by their dedicated
  // update helpers:
  //   - searchable_nodes_    (call update_searchable_nodes())
  //   - subtree_size_[]      (call update_subtree_sizes())
  //   - tree_root_           (call update_tree_root())
  //   - is_condensed_[]      (call update_condensed_nodes())
  //   - dfs_info_[]          (call recompute_dfs())
  void update_topology(spr_result const& r) {
    ensure_capacity(d_.node_high_mark());

    if (r.src_parent_collapsed) {
      // Replace src_parent with remaining_child in grandparent's children.
      auto& gp_kids = children_[r.grandparent];
      for (auto& kid : gp_kids) {
        if (kid == r.src_parent) {
          kid = r.remaining_child;
          break;
        }
      }
      parent_[r.remaining_child] = r.grandparent;

      // Invalidate collapsed node.  Preserve Fitch bookkeeping
      // (num_children_, has_child_counts_, child_counts_) so that
      // compute_node_fitch_cost can retrieve the old cost for delta
      // tracking.  When the DAG recycles the slot for new_inner,
      // recompute_node_fitch_tracked sees the old data as its "before"
      // state and correctly computes the transition delta.
      is_valid_[r.collapsed_node] = 0;
      children_[r.collapsed_node].clear();
    } else {
      // Remove src from src_parent's children.
      auto& sp_kids = children_[r.src_parent];
      sp_kids.erase(std::remove(sp_kids.begin(), sp_kids.end(), r.src),
                    sp_kids.end());
    }

    // Replace dst with new_inner in dst_parent's children.
    auto& dp_kids = children_[r.dst_parent];
    for (auto& kid : dp_kids) {
      if (kid == r.dst) {
        kid = r.new_inner;
        break;
      }
    }

    // Set up new_inner.
    children_[r.new_inner] = {r.dst, r.src};
    parent_[r.new_inner] = r.dst_parent;
    is_valid_[r.new_inner] = 1;

    // Update parent pointers for moved nodes.
    parent_[r.dst] = r.new_inner;
    parent_[r.src] = r.new_inner;
  }

  // Patch searchable_nodes_ after an SPR.
  // - Add new_inner (always a valid inner node, never tree root or UA).
  // - Remove collapsed_node if src_parent was binary and got collapsed.
  // Must be called after update_topology().
  // Note: src and dst stay in the list (already present). Condensation may
  // further adjust searchable_nodes_ if status changes for
  // leaves near the SPR source or destination.
  void update_searchable_nodes(spr_result const& r) {
    assert(is_valid(r.new_inner) && "update_topology must be called first");
    if (r.src_parent_collapsed) {
      // Note: if the DAG recycles collapsed_node's slot for new_inner
      // (collapsed_node == new_inner), this removes then re-adds the same
      // index — net effect is correct.
      auto& sn = searchable_nodes_;
      sn.erase(std::remove(sn.begin(), sn.end(), r.collapsed_node), sn.end());
    }
    searchable_nodes_.push_back(r.new_inner);
  }

  // Incrementally recompute subtree_size_[] after an SPR.
  // Walk up from the insertion point (new_inner) and the removal point
  // (src_parent or grandparent if collapsed) to tree_root_, recomputing
  // subtree_size_[node] = 1 + sum(subtree_size_[child]) at each step.
  // Both paths merge at LCA so total work is bounded by 2 × depth.
  // Must be called after update_topology().
  //
  // Note: subtree_size_ must be correct before any incremental Fitch update
  // that uses the parallel path, since fitch_bottom_up_async checks
  // subtree_size_[child_id] >= kFitchParallelThreshold per node.
  void update_subtree_sizes(spr_result const& r) {
    assert(is_valid(r.new_inner) && !children_[r.new_inner].empty() &&
           "update_topology must be called before update_subtree_sizes");

    // Recompute subtree_size for new_inner (freshly created, not yet sized).
    recompute_subtree_size(r.new_inner);

    // Walk up from dst_parent to tree_root_ (insertion side).
    walk_up_recompute(r.dst_parent);

    // Walk up from removal side to tree_root_.  Skip when:
    // - removal start equals dst_parent (sibling move or LCA-collapse case —
    //   the insertion walk already covered the entire path), or
    // - removal start is invalid (root-collapse case — grandparent is the UA
    //   node; the insertion walk already covers the path to the effective root).
    std::size_t rem_start = r.src_parent_collapsed ? r.grandparent : r.src_parent;
    if (rem_start != r.dst_parent && is_valid(rem_start)) {
      walk_up_recompute(rem_start);
    }
  }

  // Re-derive tree_root_ after an SPR that changes which node is
  // the direct child of UA.  Two cases:
  //   (a) Removal collapse: src_parent was binary and was the old tree root —
  //       the surviving child takes over.  Resolved via DAG query because
  //       children_[] is never populated for the UA node (init() skips UA).
  //   (b) Insertion above root: dst was the tree root, so new_inner is now
  //       the direct child of UA and becomes the new tree root.
  // Must be called after update_topology() and before recompute_dfs().
  void update_tree_root(spr_result const& r) {
    // (a) Removal collapse: old root was collapsed during src extraction.
    if (r.src_parent_collapsed && r.collapsed_node == tree_root_) {
      auto ua_idx = get_root_idx(d_);
      auto ua_clades = get_clades(d_, ua_idx);
      assert(!ua_clades.empty() && !ua_clades[0].empty());
      tree_root_ = get_child_idx(d_, ua_clades[0][0]);
    }
    // (b) Insertion above root: new_inner placed above the current tree root.
    if (r.dst == tree_root_) {
      tree_root_ = r.new_inner;
    }
  }

  // Re-check condensation for leaf children near the SPR.
  // An SPR can create or break CG-identical sibling pairs, changing which
  // leaves are condensed.  Re-check only the children of the parent that
  // lost src (source side) and the children of new_inner (destination side).
  // Must be called after update_topology() and update_searchable_nodes().
  void update_condensed_nodes(spr_result const& r) {
    // Source side: the parent that lost src as a child.
    std::size_t source_parent =
        r.src_parent_collapsed ? r.grandparent : r.src_parent;
    if (is_valid(source_parent)) {
      recheck_condensation(source_parent);
    }
    // Destination side: new_inner has children {dst, src}.
    recheck_condensation(r.new_inner);
    // dst_parent lost dst as a direct child (replaced by new_inner).
    // If dst had identical siblings under dst_parent, their condensation
    // status is now stale — recheck.
    if (is_valid(r.dst_parent)) {
      recheck_condensation(r.dst_parent);
    }
  }

  // Recompute dfs_info_[] for the entire tree.
  // DFS indices (dfs_index, dfs_end_index, level) cannot be cheaply updated
  // incrementally — an SPR can change the DFS interval of nodes far from the
  // move.  Re-traverse the entire tree from tree_root_.
  // This is O(N) with no per-site work, so negligible vs Fitch updates.
  // Must be called after update_topology() (so children_[] is correct) and
  // after update_tree_root() (so tree_root_ is valid).
  void recompute_dfs() {
    assert(is_valid_[tree_root_] &&
           "recompute_dfs: tree_root_ is invalid — call update_tree_root() "
           "before recompute_dfs() when the SPR may have collapsed the root");
    std::fill(has_dfs_info_.begin(), has_dfs_info_.end(), 0);
    std::size_t counter = 0;
    dfs_visit(tree_root_, counter, 0);
    assert(has_dfs_info_[tree_root_] &&
           "recompute_dfs: tree_root_ unreachable after DFS traversal");
  }

  // Recompute a single node's Fitch data from its current children.
  // Fused single-pass loop (sites-outer, children-inner) to avoid separate
  // init/accumulate/finalize passes over num_variable_sites_.
  void recompute_node_fitch(std::size_t node_id) {
    assert(!is_leaf(d_, node_id) &&
           "recompute_node_fitch must not be called on a leaf node");
    auto& children = children_[node_id];
    uint32_t nc = static_cast<uint32_t>(children.size());
    num_children_[node_id] = nc;
    has_child_counts_[node_id] = true;
    std::size_t base = node_id * num_variable_sites_;

    for (std::size_t i = 0; i < num_variable_sites_; i++) {
      std::array<uint32_t, 4> counts = {0, 0, 0, 0};
      uint8_t au = 0;
      for (auto child : children) {
        uint8_t cf = fitch_sets_[child * num_variable_sites_ + i];
        for (int j = 0; j < 4; j++)
          if (cf & (1 << j)) counts[j]++;
        au |= allele_union_[child * num_variable_sites_ + i];
      }
      child_counts_[base + i] = counts;
      fitch_sets_[base + i] = fitch_set_from_counts(counts, nc);
      allele_union_[base + i] = au;
    }
  }

  // Result of a tracked Fitch recomputation.
  struct fitch_recompute_result {
    bool changed;  // true if fitch_sets_ or allele_union_ differs from previous
    int delta;     // parsimony cost change at this node
  };

  // Fused recompute with change detection (both fitch_sets_ and allele_union_)
  // and parsimony delta tracking.
  fitch_recompute_result recompute_node_fitch_tracked(std::size_t node_id) {
    assert(!is_leaf(d_, node_id) &&
           "recompute_node_fitch_tracked must not be called on a leaf node");
    auto& children = children_[node_id];
    uint32_t nc = static_cast<uint32_t>(children.size());
    uint32_t old_nc = num_children_[node_id];
    num_children_[node_id] = nc;
    has_child_counts_[node_id] = true;
    std::size_t base = node_id * num_variable_sites_;

    bool changed = false;
    int delta = 0;

    for (std::size_t i = 0; i < num_variable_sites_; i++) {
      uint8_t old_fitch = fitch_sets_[base + i];
      uint8_t old_au = allele_union_[base + i];
      int old_cost = fitch_cost_from_counts(child_counts_[base + i], old_nc);

      std::array<uint32_t, 4> counts = {0, 0, 0, 0};
      uint8_t au = 0;
      for (auto child : children) {
        uint8_t cf = fitch_sets_[child * num_variable_sites_ + i];
        for (int j = 0; j < 4; j++)
          if (cf & (1 << j)) counts[j]++;
        au |= allele_union_[child * num_variable_sites_ + i];
      }

      uint8_t new_fitch = fitch_set_from_counts(counts, nc);
      int new_cost = fitch_cost_from_counts(counts, nc);

      child_counts_[base + i] = counts;
      fitch_sets_[base + i] = new_fitch;
      allele_union_[base + i] = au;

      if (new_fitch != old_fitch || au != old_au) changed = true;
      delta += (new_cost - old_cost);
    }

    return {changed, delta};
  }

  // Walk from start to tree_root_, recomputing Fitch at each step.
  // Stop early if neither fitch_sets_ nor allele_union_ change at a node (all
  // ancestors are already correct).  Returns number of nodes updated.
  // delta_out accumulates the parsimony cost change across all recomputed nodes.
  std::size_t propagate_fitch_upward(std::size_t start, int& delta_out) {
    std::size_t count = 0;
    std::size_t node = start;
    delta_out = 0;
    while (node != tree_root_ && is_valid_[node]) {
      auto result = recompute_node_fitch_tracked(node);
      count++;
      delta_out += result.delta;
      if (!result.changed) break;
      node = parent_[node];
    }
    // Recompute root if propagation reached it (changes propagated all the way).
    if (node == tree_root_) {
      auto result = recompute_node_fitch_tracked(node);
      count++;
      delta_out += result.delta;
    }
    return count;
  }

  // Convenience overload without delta tracking.
  std::size_t propagate_fitch_upward(std::size_t start) {
    int delta = 0;
    return propagate_fitch_upward(start, delta);
  }

  // Propagate Fitch upward from start, stopping before stop_before (exclusive).
  // The stop_before node is NOT recomputed — the caller handles it.
  // Returns number of nodes updated.
  std::size_t propagate_fitch_up_to(std::size_t start, std::size_t stop_before,
                                    int& delta_out) {
    std::size_t count = 0;
    std::size_t node = start;
    delta_out = 0;
    while (node != stop_before && node != tree_root_ && is_valid_[node]) {
      auto result = recompute_node_fitch_tracked(node);
      count++;
      delta_out += result.delta;
      if (!result.changed) break;
      node = parent_[node];
    }
    // If we reached tree_root_ (and it's not the stop point), recompute it.
    if (node == tree_root_ && node != stop_before) {
      auto result = recompute_node_fitch_tracked(node);
      count++;
      delta_out += result.delta;
    }
    return count;
  }

  // Convenience overload without delta tracking.
  std::size_t propagate_fitch_up_to(std::size_t start,
                                    std::size_t stop_before) {
    int delta = 0;
    return propagate_fitch_up_to(start, stop_before, delta);
  }

  // Initialize Fitch data for a newly created inner node.
  // When a new inner node is created, its children (dst and src)
  // already have correct Fitch sets — only their parent changed.  This
  // computes the new node's Fitch sets from scratch.
  // Returns the parsimony cost delta introduced by this node.  For a new
  // node (num_children_ == 0 before recompute), old_cost is 0 so delta
  // equals the new cost — the number of variable sites where the children's
  // Fitch sets are disjoint (union sites).
  int init_new_node_fitch(std::size_t new_inner) {
    assert(new_inner < num_nodes_ &&
           "init_new_node_fitch: ensure_capacity must be called first");
    auto result = recompute_node_fitch_tracked(new_inner);
    return result.delta;
  }

  // Compute total Fitch cost at a node from its current child_counts_ arrays.
  // Used to obtain the old cost of a node about to be removed (e.g., a
  // collapsed src_parent) so the delta tracker can account for its deletion.
  int compute_node_fitch_cost(std::size_t node_id) const {
    if (!has_child_counts_[node_id]) return 0;
    uint32_t nc = num_children_[node_id];
    if (nc <= 1) return 0;
    int cost = 0;
    std::size_t base = node_id * num_variable_sites_;
    for (std::size_t i = 0; i < num_variable_sites_; i++) {
      cost += fitch_cost_from_counts(child_counts_[base + i], nc);
    }
    return cost;
  }

  // Fitch update for the source removal path.
  // After an SPR detaches src from src_parent, the Fitch data along the
  // removal path is stale.  If src_parent was collapsed, start from
  // grandparent (which gained remaining_child as a direct child); otherwise
  // start from src_parent (which lost one child).  Propagate upward to root
  // with early termination when Fitch sets stabilize.
  //
  // delta_out accumulates the parsimony score change along the removal path,
  // including the cost removed by collapsing src_parent (if applicable).
  // Must be called after update_topology() and (typically) init_new_node_fitch().
  void update_fitch_removal(spr_result const& r, int& delta_out) {
    delta_out = 0;
    // If src_parent was collapsed, its Fitch cost is removed from the tree.
    // When collapsed_node == new_inner (slot recycled by the DAG),
    // init_new_node_fitch's tracked recompute already captures the
    // old→new transition, so don't subtract separately.
    if (r.src_parent_collapsed && r.collapsed_node != r.new_inner) {
      delta_out -= compute_node_fitch_cost(r.collapsed_node);
    }
    std::size_t start = r.src_parent_collapsed ? r.grandparent : r.src_parent;
    int propagation_delta = 0;
    propagate_fitch_upward(start, propagation_delta);
    delta_out += propagation_delta;
  }

  // Convenience overload without delta tracking.
  void update_fitch_removal(spr_result const& r) {
    int delta = 0;
    update_fitch_removal(r, delta);
  }

  // Fitch update for the destination insertion path.
  // After reattaching src under new_inner next to dst, recompute Fitch
  // along the insertion path: first initialize new_inner's Fitch from its
  // children (src and dst), then propagate upward from dst_parent (which
  // now has new_inner as child instead of dst).
  //
  // delta_out accumulates the parsimony score change: new_inner's cost
  // (always non-negative — it's a fresh node) plus propagation changes.
  // Must be called after update_topology().
  void update_fitch_insertion(spr_result const& r, int& delta_out) {
    delta_out = init_new_node_fitch(r.new_inner);
    int propagation_delta = 0;
    propagate_fitch_upward(r.dst_parent, propagation_delta);
    delta_out += propagation_delta;
  }

  // Convenience overload without delta tracking.
  void update_fitch_insertion(spr_result const& r) {
    int delta = 0;
    update_fitch_insertion(r, delta);
  }

  // Combined incremental Fitch update.
  // Both the removal path (src_parent → root) and the insertion path
  // (dst_parent → root) converge at the LCA of src and dst.  Instead of
  // propagating both paths independently to root (doubling work on the
  // LCA→root segment), propagate each path up to the LCA separately,
  // then propagate once from LCA to root.
  //
  // Edge cases:
  //   - LCA collapsed (src_parent == lca, binary): use grandparent as
  //     effective LCA — the original LCA node no longer exists.
  //   - src_parent == LCA, not collapsed: removal-side propagation is a
  //     no-op (start == stop); the shared LCA→root pass handles it.
  //   - dst_parent == LCA: insertion-side propagation is a no-op.
  //
  // delta_out accumulates the total parsimony score change.
  // Must be called after update_topology() and update_tree_root().
  void update_fitch(spr_result const& r, int& delta_out) {
    delta_out = 0;

    // Account for collapsed node's Fitch cost removal.
    // When collapsed_node == new_inner (slot recycled by the DAG),
    // init_new_node_fitch's tracked recompute already captures the
    // old→new transition, so don't subtract separately.
    if (r.src_parent_collapsed && r.collapsed_node != r.new_inner) {
      delta_out -= compute_node_fitch_cost(r.collapsed_node);
    }

    // Effective LCA: if src_parent was the LCA and got collapsed,
    // the LCA node no longer exists; use grandparent instead.
    std::size_t effective_lca = r.lca;
    if (r.src_parent_collapsed && r.src_parent == r.lca) {
      effective_lca = r.grandparent;
    }

    // 1. Removal side: propagate up to (but not past) effective_lca.
    std::size_t rem_start =
        r.src_parent_collapsed ? r.grandparent : r.src_parent;
    int rem_delta = 0;
    propagate_fitch_up_to(rem_start, effective_lca, rem_delta);
    delta_out += rem_delta;

    // 2. Insertion side: init new_inner, then up to effective_lca.
    delta_out += init_new_node_fitch(r.new_inner);
    int ins_delta = 0;
    propagate_fitch_up_to(r.dst_parent, effective_lca, ins_delta);
    delta_out += ins_delta;

    // 3. Shared segment: effective_lca to root.
    int shared_delta = 0;
    propagate_fitch_upward(effective_lca, shared_delta);
    delta_out += shared_delta;
  }

  // Convenience overload without delta tracking.
  void update_fitch(spr_result const& r) {
    int delta = 0;
    update_fitch(r, delta);
  }

 private:
  static constexpr std::size_t kFitchParallelThreshold = 64;


  void init() {
    auto ua_idx = get_root_idx(d_);
    auto ua_clades = get_clades(d_, ua_idx);
    if (!ua_clades.empty() && !ua_clades[0].empty())
      tree_root_ = get_child_idx(d_, ua_clades[0][0]);

    // Collect variable sites from edge mutations
    scoped_arena<8192> arena;
    auto* mr = arena.get();
    std::pmr::set<std::size_t> var_sites_set(mr);
    for (auto ev : d_.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            for (auto& [pos, mut] : edge.mutations()) var_sites_set.insert(pos);
          },
          ev);
    }
    for (auto site : var_sites_set) variable_sites_.push_back(site);
    num_variable_sites_ = variable_sites_.size();

    num_nodes_ = d_.node_high_mark();

    // Allocate flat arrays
    dfs_info_.resize(num_nodes_);
    has_dfs_info_.assign(num_nodes_, false);
    is_valid_.assign(num_nodes_, 0);
    fitch_sets_.resize(num_nodes_ * num_variable_sites_, 0);
    child_counts_.resize(num_nodes_ * num_variable_sites_, {0, 0, 0, 0});
    has_child_counts_.assign(num_nodes_, false);
    num_children_.assign(num_nodes_, 0);
    allele_union_.resize(num_nodes_ * num_variable_sites_, 0);
    parent_.resize(num_nodes_, 0);
    children_.resize(num_nodes_);

    // Build topology
    for (auto nv : d_.get_all_nodes()) {
      std::visit(
          [&](auto node) {
            auto nid = node.index();
            if (is_ua(d_, nid)) return;
            is_valid_[nid] = 1;

            // Parent
            auto pes = get_parent_edges(d_, nid);
            if (!pes.empty()) parent_[nid] = get_parent_idx(d_, pes[0]);

            // Children
            auto cls = get_clades(d_, nid);
            for (auto& clade : cls)
              for (auto eidx : clade)
                children_[nid].push_back(get_child_idx(d_, eidx));
          },
          nv);
    }

    // Condense identical sibling leaves: group leaves sharing the same
    // parent and CompactGenome, keeping only one representative per group.
    // This matches larch's full CG equality check.
    is_condensed_.assign(num_nodes_, false);
    for (std::size_t nid = 0; nid < num_nodes_; nid++) {
      auto& kids = children_[nid];
      if (kids.size() <= 1) continue;

      std::vector<bool> condensed(kids.size(), false);
      bool any_condensed = false;
      for (std::size_t i = 0; i < kids.size(); i++) {
        if (condensed[i]) continue;
        if (!is_leaf(d_, kids[i])) continue;
        auto cg_i = get_node_cg(kids[i]);
        for (std::size_t j = i + 1; j < kids.size(); j++) {
          if (condensed[j]) continue;
          if (!is_leaf(d_, kids[j])) continue;
          auto cg_j = get_node_cg(kids[j]);
          if (cg_i == cg_j) {
            condensed[j] = true;
            is_condensed_[kids[j]] = true;
            any_condensed = true;
            condensed_count_++;
          }
        }
      }
      // Note: condensed leaves stay in children_ to preserve correct child
      // counts.  They are excluded from searchable_nodes_ below.
    }

    // Collect searchable nodes (non-UA, non-tree-root, non-condensed)
    for (auto nv : d_.get_all_nodes()) {
      std::visit(
          [&](auto node) {
            auto nid = node.index();
            if (is_ua(d_, nid)) return;
            if (nid == tree_root_) return;
            if (is_condensed_[nid]) return;
            searchable_nodes_.push_back(nid);
          },
          nv);
    }

    // Compute subtree sizes (needed for parallel threshold)
    subtree_size_.resize(num_nodes_, 0);
    compute_subtree_sizes(tree_root_);

    compute_dfs_indices();
    compute_fitch_sets();
  }

  nuc_base get_node_base(std::size_t nid, std::size_t site,
                         std::string const& ref) {
    auto nv = d_.get_node(nid);
    nuc_base result;
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.cg(); }) {
            result = node.cg().get_base(site, ref);
          } else {
            result = nuc_base::from_char(ref.at(site - 1));
          }
        },
        nv);
    return result;
  }

  compact_genome get_node_cg(std::size_t nid) {
    auto nv = d_.get_node(nid);
    compact_genome result;
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.cg(); }) {
            result = node.cg();
          }
        },
        nv);
    return result;
  }

  // Iterative bottom-up traversal to avoid stack overflow on degenerate trees.
  void compute_subtree_sizes(std::size_t root) {
    // BFS to collect all nodes, then process in reverse for bottom-up order.
    std::vector<std::size_t> order;
    order.push_back(root);
    for (std::size_t i = 0; i < order.size(); ++i) {
      for (auto child : children_[order[i]]) {
        order.push_back(child);
      }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      std::size_t size = 1;
      for (auto child : children_[*it]) {
        size += subtree_size_[child];
      }
      subtree_size_[*it] = size;
    }
  }

  // Recompute subtree_size for a single node from its children.
  void recompute_subtree_size(std::size_t node) {
    std::size_t size = 1;
    for (auto child : children_[node]) {
      size += subtree_size_[child];
    }
    subtree_size_[node] = size;
  }

  // Walk up from node to tree_root_, recomputing subtree_size at each step.
  // Guards against stale tree_root_ (e.g., after root collapse before
  // updates tree_root_): stops if the next parent is invalid (such as the UA
  // node).  Iteration bound prevents infinite loops on corrupted parent_ chains.
  void walk_up_recompute(std::size_t node) {
    auto cur = node;
    for (std::size_t steps = 0; steps < num_nodes_; ++steps) {
      recompute_subtree_size(cur);
      if (cur == tree_root_) break;
      auto next = parent_[cur];
      if (next >= num_nodes_ || !is_valid_[next]) break;
      cur = next;
    }
  }

  // Re-check condensation for leaf children of a given parent node.
  // Clears old condensed flags for all leaf children, re-runs the O(siblings²)
  // CG-equality check, and patches searchable_nodes_ and condensed_count_.
  void recheck_condensation(std::size_t parent_node) {
    auto& kids = children_[parent_node];

    // Collect leaf children (use children_[] to detect leaves — no DAG query).
    std::vector<std::size_t> leaf_kids;
    for (auto kid : kids) {
      if (children_[kid].empty()) leaf_kids.push_back(kid);
    }

    if (leaf_kids.size() <= 1) {
      // At most one leaf — clear any stale condensed flag.
      for (auto kid : leaf_kids) {
        if (is_condensed_[kid]) {
          is_condensed_[kid] = 0;
          condensed_count_--;
          searchable_nodes_.push_back(kid);
        }
      }
      return;
    }

    // Remember which leaves were condensed, then clear all.
    std::vector<uint8_t> was_condensed(leaf_kids.size());
    for (std::size_t i = 0; i < leaf_kids.size(); i++) {
      was_condensed[i] = is_condensed_[leaf_kids[i]];
      if (is_condensed_[leaf_kids[i]]) {
        is_condensed_[leaf_kids[i]] = 0;
        condensed_count_--;
      }
    }

    // Re-run condensation (same algorithm as init()).
    std::vector<bool> now_condensed(leaf_kids.size(), false);
    for (std::size_t i = 0; i < leaf_kids.size(); i++) {
      if (now_condensed[i]) continue;
      auto cg_i = get_node_cg(leaf_kids[i]);
      for (std::size_t j = i + 1; j < leaf_kids.size(); j++) {
        if (now_condensed[j]) continue;
        auto cg_j = get_node_cg(leaf_kids[j]);
        if (cg_i == cg_j) {
          now_condensed[j] = true;
          is_condensed_[leaf_kids[j]] = 1;
          condensed_count_++;
        }
      }
    }

    // Patch searchable_nodes_ for status changes.
    for (std::size_t i = 0; i < leaf_kids.size(); i++) {
      if (!was_condensed[i] && now_condensed[i]) {
        // Newly condensed — remove from searchable_nodes_ (swap-and-pop: O(1)
        // mutation instead of O(N) shift from std::remove; order is irrelevant).
        auto& sn = searchable_nodes_;
        auto it = std::find(sn.begin(), sn.end(), leaf_kids[i]);
        if (it != sn.end()) {
          *it = sn.back();
          sn.pop_back();
        }
      } else if (was_condensed[i] && !now_condensed[i]) {
        // Newly uncondensed — add to searchable_nodes_.
        searchable_nodes_.push_back(leaf_kids[i]);
      }
    }
  }

  // Iterative DFS to avoid stack overflow on degenerate trees.
  // Assigns pre-order dfs_index and post-order dfs_end_index.
  void dfs_visit(std::size_t root, std::size_t& counter,
                 std::size_t root_level) {
    struct frame {
      std::size_t node;
      std::size_t level;
      bool entered;
    };
    std::vector<frame> stack;
    stack.push_back({root, root_level, false});

    while (!stack.empty()) {
      if (!stack.back().entered) {
        auto node = stack.back().node;
        auto level = stack.back().level;
        stack.back().entered = true;

        dfs_info_[node].dfs_index = counter++;
        dfs_info_[node].level = level;

        // Push children in reverse order so the first child is visited first.
        auto& ch = children_[node];
        for (std::size_t i = ch.size(); i > 0; --i) {
          stack.push_back({ch[i - 1], level + 1, false});
        }
      } else {
        auto node = stack.back().node;
        dfs_info_[node].dfs_end_index = counter;
        has_dfs_info_[node] = true;
        stack.pop_back();
      }
    }
  }

  void compute_dfs_indices() {
    std::size_t counter = 0;
    dfs_visit(tree_root_, counter, 0);
  }

  void fitch_bottom_up_impl(std::size_t node_id, std::string const& ref) {
    std::size_t base_offset = node_id * num_variable_sites_;

    if (is_leaf(d_, node_id)) {
      for (std::size_t i = 0; i < num_variable_sites_; i++) {
        auto base = get_node_base(node_id, variable_sites_[i], ref);
        uint8_t singleton = base_to_one_hot(base);
        fitch_sets_[base_offset + i] = singleton;
        allele_union_[base_offset + i] = singleton;
      }
    } else {
      auto& children = children_[node_id];
      for (auto child_id : children) fitch_bottom_up_impl(child_id, ref);

      has_child_counts_[node_id] = true;
      uint32_t nc = static_cast<uint32_t>(children.size());
      num_children_[node_id] = nc;

      for (std::size_t i = 0; i < num_variable_sites_; i++) {
        auto& c = child_counts_[base_offset + i];
        c = {0, 0, 0, 0};
        uint8_t au = 0;

        for (auto child_id : children) {
          std::size_t child_offset = child_id * num_variable_sites_ + i;
          uint8_t child_fitch = fitch_sets_[child_offset];
          for (int j = 0; j < 4; j++) {
            if (child_fitch & (1 << j)) c[j]++;
          }
          au |= allele_union_[child_offset];
        }

        fitch_sets_[base_offset + i] = fitch_set_from_counts(c, nc);
        allele_union_[base_offset + i] = au;
      }
    }
  }

  void fitch_bottom_up(std::size_t node_id) {
    fitch_bottom_up_impl(node_id, get_reference_sequence(d_));
  }

  task<void> fitch_bottom_up_async(std::size_t node_id,
                                   std::string const& ref) {
    std::size_t base_offset = node_id * num_variable_sites_;

    if (is_leaf(d_, node_id)) {
      for (std::size_t i = 0; i < num_variable_sites_; i++) {
        auto base = get_node_base(node_id, variable_sites_[i], ref);
        uint8_t singleton = base_to_one_hot(base);
        fitch_sets_[base_offset + i] = singleton;
        allele_union_[base_offset + i] = singleton;
      }
      co_return;
    }

    auto& children = children_[node_id];

    // Recurse into children, parallelizing large subtrees
    bool any_large = false;
    for (auto child_id : children) {
      if (subtree_size_[child_id] >= kFitchParallelThreshold) {
        any_large = true;
        break;
      }
    }

    if (any_large) {
      co_await async_for_each(
          *pool_, children, [&](std::size_t child_id) -> task<void> {
            if (subtree_size_[child_id] >= kFitchParallelThreshold) {
              co_await fitch_bottom_up_async(child_id, ref);
            } else {
              fitch_bottom_up_impl(child_id, ref);
            }
          });
    } else {
      for (auto child_id : children) fitch_bottom_up_impl(child_id, ref);
    }

    // Compute this node's Fitch data from children
    has_child_counts_[node_id] = true;
    uint32_t nc = static_cast<uint32_t>(children.size());
    num_children_[node_id] = nc;

    for (std::size_t i = 0; i < num_variable_sites_; i++) {
      auto& c = child_counts_[base_offset + i];
      c = {0, 0, 0, 0};
      uint8_t au = 0;

      for (auto child_id : children) {
        std::size_t child_offset = child_id * num_variable_sites_ + i;
        uint8_t child_fitch = fitch_sets_[child_offset];
        for (int j = 0; j < 4; j++) {
          if (child_fitch & (1 << j)) c[j]++;
        }
        au |= allele_union_[child_offset];
      }

      fitch_sets_[base_offset + i] = fitch_set_from_counts(c, nc);
      allele_union_[base_offset + i] = au;
    }
  }

  void compute_fitch_sets() {
    auto const& ref = get_reference_sequence(d_);
    if (pool_ && subtree_size_[tree_root_] >= kFitchParallelThreshold) {
      sync_wait<void>(*pool_, fitch_bottom_up_async(tree_root_, ref));
    } else {
      fitch_bottom_up(tree_root_);
    }
    ref_alleles_.resize(num_variable_sites_);
    for (std::size_t i = 0; i < num_variable_sites_; i++) {
      auto base = nuc_base::from_char(ref.at(variable_sites_[i] - 1));
      ref_alleles_[i] = base_to_one_hot(base);
    }
  }

  phylo_dag& d_;
  thread_pool* pool_{nullptr};
  std::size_t tree_root_{0};
  std::vector<std::size_t> variable_sites_;
  std::vector<std::size_t> searchable_nodes_;

  std::size_t num_nodes_{0};
  std::size_t num_variable_sites_{0};

  std::vector<dfs_info> dfs_info_;
  std::vector<uint8_t> has_dfs_info_;
  std::vector<uint8_t> is_valid_;
  std::vector<uint8_t> fitch_sets_;
  std::vector<std::array<uint32_t, 4>> child_counts_;
  std::vector<uint8_t> has_child_counts_;
  std::vector<uint32_t> num_children_;
  std::vector<uint8_t> allele_union_;
  std::vector<std::size_t> parent_;
  std::vector<std::vector<std::size_t>> children_;
  std::vector<uint8_t> is_condensed_;
  std::vector<std::size_t> subtree_size_;
  std::vector<uint8_t> ref_alleles_;
  std::size_t condensed_count_{0};
};

// ============================================================================
// move_enumerator: Bounded search for profitable SPR moves
// ============================================================================

class move_enumerator {
 public:
  using callback_t = std::function<void(profitable_move const&)>;

  explicit move_enumerator(tree_index const& idx, int score_threshold = -1)
      : index_{idx}, score_threshold_{score_threshold} {}

  int compute_move_score(std::size_t src, std::size_t dst,
                         std::size_t lca) const {
    std::size_t n_sites = index_.num_variable_sites();
    if (n_sites == 0) return 0;

    auto src_parent = index_.get_parent(src);
    auto dst_parent = index_.get_parent(dst);
    bool src_parent_is_binary = (index_.get_num_children(src_parent) == 2);

    std::size_t src_sibling = 0;
    if (src_parent_is_binary) {
      for (auto child : index_.get_children(src_parent)) {
        if (child != src) {
          src_sibling = child;
          break;
        }
      }
    }

    std::vector<std::size_t> src_path;
    {
      auto cur = src_parent;
      while (true) {
        src_path.push_back(cur);
        if (cur == lca) break;
        cur = index_.get_parent(cur);
      }
    }

    std::vector<std::size_t> dst_path;
    {
      auto cur = dst_parent;
      while (true) {
        dst_path.push_back(cur);
        if (cur == lca) break;
        cur = index_.get_parent(cur);
      }
    }

    std::unordered_set<std::size_t> affected_set;
    for (auto n : src_path) affected_set.insert(n);
    for (auto n : dst_path) affected_set.insert(n);

    std::vector<std::size_t> affected(affected_set.begin(), affected_set.end());
    std::sort(
        affected.begin(), affected.end(), [&](std::size_t a, std::size_t b) {
          return index_.get_dfs_info(a).level > index_.get_dfs_info(b).level;
        });

    std::unordered_set<std::size_t> on_src_path(src_path.begin(),
                                                src_path.end());
    std::unordered_set<std::size_t> on_dst_path(dst_path.begin(),
                                                dst_path.end());

    int total_score_change = 0;

    for (std::size_t si = 0; si < n_sites; si++) {
      uint8_t src_fitch = index_.get_fitch_set(src, si);
      uint8_t dst_fitch = index_.get_fitch_set(dst, si);

      uint8_t n_inter = src_fitch & dst_fitch;
      uint8_t new_node_fitch = n_inter ? n_inter : (src_fitch | dst_fitch);
      int new_node_cost = n_inter ? 0 : 1;
      total_score_change += new_node_cost;

      if (src_parent_is_binary && index_.has_child_counts(src_parent)) {
        auto counts = index_.get_child_counts(src_parent, si);
        uint32_t nc = index_.get_num_children(src_parent);
        total_score_change -= fitch_cost_from_counts(counts, nc);
      }

      std::unordered_map<std::size_t, uint8_t> new_fitch_map;

      if (src_parent_is_binary)
        new_fitch_map[src_parent] = index_.get_fitch_set(src_sibling, si);

      for (auto node : affected) {
        if (src_parent_is_binary && node == src_parent) continue;
        if (!index_.has_child_counts(node)) continue;

        auto counts = index_.get_child_counts(node, si);
        uint32_t nc = index_.get_num_children(node);
        int old_cost = fitch_cost_from_counts(counts, nc);
        uint32_t new_nc = nc;

        if (on_src_path.count(node)) {
          if (node == src_parent && !src_parent_is_binary) {
            for (int j = 0; j < 4; j++) {
              if (src_fitch & (1 << j)) {
                if (counts[j] > 0) counts[j]--;
              }
            }
            new_nc--;
          } else {
            std::size_t src_side_child = 0;
            bool found = false;
            for (std::size_t pi = 0; pi + 1 < src_path.size(); pi++) {
              if (src_path[pi + 1] == node) {
                src_side_child = src_path[pi];
                found = true;
                break;
              }
            }
            if (found) {
              auto it = new_fitch_map.find(src_side_child);
              if (it != new_fitch_map.end()) {
                uint8_t old_f = index_.get_fitch_set(src_side_child, si);
                uint8_t new_f = it->second;
                for (int j = 0; j < 4; j++) {
                  if (old_f & (1 << j)) {
                    if (counts[j] > 0) counts[j]--;
                  }
                  if (new_f & (1 << j)) counts[j]++;
                }
              }
            }
          }
        }

        if (on_dst_path.count(node)) {
          if (node == dst_parent) {
            for (int j = 0; j < 4; j++) {
              if (dst_fitch & (1 << j)) {
                if (counts[j] > 0) counts[j]--;
              }
              if (new_node_fitch & (1 << j)) counts[j]++;
            }
          } else {
            std::size_t dst_side_child = 0;
            bool found = false;
            for (std::size_t pi = 0; pi + 1 < dst_path.size(); pi++) {
              if (dst_path[pi + 1] == node) {
                dst_side_child = dst_path[pi];
                found = true;
                break;
              }
            }
            if (found) {
              auto it = new_fitch_map.find(dst_side_child);
              if (it != new_fitch_map.end()) {
                uint8_t old_f = index_.get_fitch_set(dst_side_child, si);
                uint8_t new_f = it->second;
                for (int j = 0; j < 4; j++) {
                  if (old_f & (1 << j)) {
                    if (counts[j] > 0) counts[j]--;
                  }
                  if (new_f & (1 << j)) counts[j]++;
                }
              }
            }
          }
        }

        int new_cost = fitch_cost_from_counts(counts, new_nc);
        uint8_t new_fitch = fitch_set_from_counts(counts, new_nc);
        total_score_change += new_cost - old_cost;
        new_fitch_map[node] = new_fitch;
      }

      if (src_parent_is_binary) {
        if (new_fitch_map.count(src_sibling)) {
          new_fitch_map[lca] = new_fitch_map[src_sibling];
        } else if (dst == src_sibling) {
          new_fitch_map[lca] = new_node_fitch;
        }
      }

      uint8_t root_old_f = index_.get_fitch_set(index_.get_tree_root(), si);
      uint8_t root_new_f = root_old_f;

      auto lca_new_it = new_fitch_map.find(lca);
      if (lca_new_it != new_fitch_map.end()) {
        if (lca == index_.get_tree_root()) {
          root_new_f = lca_new_it->second;
        } else {
          uint8_t lca_orig = index_.get_fitch_set(lca, si);
          if (lca_new_it->second != lca_orig) {
            uint8_t prev_old_f = lca_orig;
            uint8_t prev_new_f = lca_new_it->second;
            auto above = index_.get_parent(lca);
            while (true) {
              if (!index_.has_child_counts(above)) break;

              auto counts = index_.get_child_counts(above, si);
              uint32_t nc = index_.get_num_children(above);
              int old_cost_above = fitch_cost_from_counts(counts, nc);

              for (int j = 0; j < 4; j++) {
                if (prev_old_f & (1 << j)) {
                  if (counts[j] > 0) counts[j]--;
                }
                if (prev_new_f & (1 << j)) counts[j]++;
              }

              int new_cost_above = fitch_cost_from_counts(counts, nc);
              total_score_change += new_cost_above - old_cost_above;

              uint8_t above_orig = index_.get_fitch_set(above, si);
              uint8_t above_new = fitch_set_from_counts(counts, nc);
              if (above_new == above_orig) break;

              prev_old_f = above_orig;
              prev_new_f = above_new;
              if (above == index_.get_tree_root()) {
                root_new_f = above_new;
                break;
              }
              above = index_.get_parent(above);
            }
          }
        }
      }

      if (root_old_f != root_new_f) {
        uint8_t ref_a = index_.get_ref_alleles_ptr()[si];
        int old_ua = (root_old_f & ref_a) ? 0 : 1;
        int new_ua = (root_new_f & ref_a) ? 0 : 1;
        total_score_change += new_ua - old_ua;
      }
    }

    return total_score_change;
  }

  void compute_initial_removal(std::size_t src,
                               src_removal_result& result) const {
    std::size_t n_sites = index_.num_variable_sites();
    result.score_change = 0;
    result.lca_nc_adjustment = 0;

    auto src_parent = index_.get_parent(src);
    bool src_parent_is_binary = (index_.get_num_children(src_parent) == 2);

    auto const* src_parent_fitch = index_.get_fitch_set_ptr(src_parent);

    if (src_parent_is_binary) {
      std::size_t sibling = 0;
      for (auto child : index_.get_children(src_parent)) {
        if (child != src) {
          sibling = child;
          break;
        }
      }

      auto const* sibling_fitch = index_.get_fitch_set_ptr(sibling);
      for (std::size_t si = 0; si < n_sites; si++) {
        auto& counts = index_.get_child_counts(src_parent, si);
        uint32_t nc = index_.get_num_children(src_parent);
        result.score_change -= fitch_cost_from_counts(counts, nc);
        result.old_fitch[si] = src_parent_fitch[si];
        result.new_fitch[si] = sibling_fitch[si];
      }
    } else {
      auto const* src_fitch = index_.get_fitch_set_ptr(src);
      for (std::size_t si = 0; si < n_sites; si++) {
        auto counts = index_.get_child_counts(src_parent, si);
        uint32_t nc = index_.get_num_children(src_parent);
        int old_cost = fitch_cost_from_counts(counts, nc);

        uint8_t sf = src_fitch[si];
        for (int j = 0; j < 4; j++) {
          if (sf & (1 << j)) {
            if (counts[j] > 0) counts[j]--;
          }
        }
        uint32_t new_nc = nc - 1;
        int new_cost = fitch_cost_from_counts(counts, new_nc);
        uint8_t new_fitch = fitch_set_from_counts(counts, new_nc);

        result.score_change += new_cost - old_cost;
        result.old_fitch[si] = src_parent_fitch[si];
        result.new_fitch[si] = new_fitch;
      }
    }
  }

  src_removal_result compute_initial_removal(std::size_t src) const {
    src_removal_result result;
    result.resize(index_.num_variable_sites());
    compute_initial_removal(src, result);
    return result;
  }

  void propagate_removal_upward(std::size_t current_lca,
                                src_removal_result& removal) const {
    std::size_t n_sites = index_.num_variable_sites();
    auto const* lca_fitch = index_.get_fitch_set_ptr(current_lca);
    uint32_t nc = index_.get_num_children(current_lca);

    for (std::size_t si = 0; si < n_sites; si++) {
      auto counts = index_.get_child_counts(current_lca, si);
      int old_cost = fitch_cost_from_counts(counts, nc);

      uint8_t old_child_f = removal.old_fitch[si];
      uint8_t new_child_f = removal.new_fitch[si];

      for (int j = 0; j < 4; j++) {
        if (old_child_f & (1 << j)) {
          if (counts[j] > 0) counts[j]--;
        }
        if (new_child_f & (1 << j)) counts[j]++;
      }

      int new_cost = fitch_cost_from_counts(counts, nc);
      uint8_t new_fitch = fitch_set_from_counts(counts, nc);

      removal.score_change += new_cost - old_cost;
      removal.old_fitch[si] = lca_fitch[si];
      removal.new_fitch[si] = new_fitch;
    }
  }

  int compute_lower_bound(std::size_t src,
                          src_removal_result const& /*removal*/,
                          std::size_t subtree_root) const {
    std::size_t n_sites = index_.num_variable_sites();
    auto const* src_fitch = index_.get_fitch_set_ptr(src);
    int forced_new_node_cost = 0;
    for (std::size_t si = 0; si < n_sites; si++) {
      uint8_t subtree_alleles = index_.get_allele_union(subtree_root, si);
      if (!(src_fitch[si] & subtree_alleles)) forced_new_node_cost += 1;
    }
    return forced_new_node_cost;
  }

  int compute_move_score_cached(std::size_t src, std::size_t dst,
                                std::size_t lca,
                                src_removal_result const& removal,
                                scratch_buffers& scratch) const {
    std::size_t n_sites = index_.num_variable_sites();
    if (n_sites == 0) return 0;

    auto dst_parent = index_.get_parent(dst);

    int nodes_remaining = 0;
    {
      auto cur = dst_parent;
      while (cur != lca) {
        nodes_remaining++;
        cur = index_.get_parent(cur);
      }
      nodes_remaining++;
      nodes_remaining +=
          static_cast<int>(index_.get_dfs_info(lca).level);
    }

    auto const* src_fitch_ptr = index_.get_fitch_set_ptr(src);
    auto const* dst_fitch_ptr = index_.get_fitch_set_ptr(dst);
    auto const* removal_old = removal.old_fitch.data();
    auto const* removal_new = removal.new_fitch.data();

    int total = removal.score_change;

    auto& new_node_fitch = scratch.new_node_fitch;
    for (std::size_t si = 0; si < n_sites; si++) {
      uint8_t sf = src_fitch_ptr[si];
      uint8_t df = dst_fitch_ptr[si];
      uint8_t inter = sf & df;
      new_node_fitch[si] = inter ? inter : (sf | df);
      total += inter ? 0 : 1;
    }

    auto& prev_old_fitch = scratch.prev_old_fitch;
    auto& prev_new_fitch = scratch.prev_new_fitch;

    auto node = dst_parent;
    bool is_first = true;
    while (true) {
      if (index_.has_child_counts(node)) {
        auto const* node_fitch_ptr = index_.get_fitch_set_ptr(node);
        uint32_t nc = index_.get_num_children(node);
        bool is_lca = (node == lca);
        uint32_t effective_nc =
            is_lca ? static_cast<uint32_t>(static_cast<int>(nc) +
                                           removal.lca_nc_adjustment)
                   : nc;

        for (std::size_t si = 0; si < n_sites; si++) {
          auto counts = index_.get_child_counts(node, si);
          int old_cost = fitch_cost_from_counts(counts, nc);

          uint8_t old_child_f;
          uint8_t new_child_f;
          if (is_first) {
            old_child_f = dst_fitch_ptr[si];
            new_child_f = new_node_fitch[si];
          } else {
            old_child_f = prev_old_fitch[si];
            new_child_f = prev_new_fitch[si];
          }

          for (int j = 0; j < 4; j++) {
            if (old_child_f & (1 << j)) {
              if (counts[j] > 0) counts[j]--;
            }
            if (new_child_f & (1 << j)) counts[j]++;
          }

          if (is_lca) {
            uint8_t src_old_f = removal_old[si];
            uint8_t src_new_f = removal_new[si];
            for (int j = 0; j < 4; j++) {
              if (src_old_f & (1 << j)) {
                if (counts[j] > 0) counts[j]--;
              }
              if (src_new_f & (1 << j)) counts[j]++;
            }
          }

          int new_cost = fitch_cost_from_counts(counts, effective_nc);
          total += new_cost - old_cost;

          prev_old_fitch[si] = node_fitch_ptr[si];
          prev_new_fitch[si] = fitch_set_from_counts(counts, effective_nc);
        }
      }

      nodes_remaining--;

      if (total > 0 && nodes_remaining > 0 &&
          total > static_cast<int>(n_sites) * nodes_remaining) {
        return total;
      }

      bool fitch_changed = false;
      for (std::size_t si = 0; si < n_sites; si++) {
        if (prev_old_fitch[si] != prev_new_fitch[si]) {
          fitch_changed = true;
          break;
        }
      }

      bool is_current_lca = (node == lca);

      if (is_current_lca && !fitch_changed) break;
      if (!is_current_lca && nodes_remaining < 0 && !fitch_changed) break;

      if (node == index_.get_tree_root()) {
        auto const* ref_alleles = index_.get_ref_alleles_ptr();
        for (std::size_t si = 0; si < n_sites; si++) {
          uint8_t old_f = prev_old_fitch[si];
          uint8_t new_f = prev_new_fitch[si];
          if (old_f != new_f) {
            int old_ua = (old_f & ref_alleles[si]) ? 0 : 1;
            int new_ua = (new_f & ref_alleles[si]) ? 0 : 1;
            total += new_ua - old_ua;
          }
        }
        break;
      }
      node = index_.get_parent(node);
      is_first = false;
    }

    return total;
  }

  int compute_move_score_cached(std::size_t src, std::size_t dst,
                                std::size_t lca,
                                src_removal_result const& removal) const {
    scratch_buffers scratch;
    scratch.resize(index_.num_variable_sites());
    return compute_move_score_cached(src, dst, lca, removal, scratch);
  }

  void find_moves_for_source(std::size_t src, std::size_t radius,
                             callback_t callback,
                             std::size_t* scored_count = nullptr) const {
    scratch_buffers scratch;
    scratch.resize(index_.num_variable_sites());
    upward_traversal(src, radius, callback, scratch, scored_count);
  }

  void find_all_moves(std::size_t radius, callback_t callback) const {
    auto& searchable = index_.get_searchable_nodes();
    for (auto src : searchable) find_moves_for_source(src, radius, callback);
  }

  void find_all_moves_parallel(std::size_t radius, callback_t callback,
                               thread_pool& pool) const {
    auto& searchable = index_.get_searchable_nodes();
    std::vector<std::vector<profitable_move>> per_source(searchable.size());

    std::vector<std::size_t> indices(searchable.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});

    parallel_for_each(pool, indices, [&](std::size_t i) {
      find_moves_for_source(
          searchable[i], radius,
          [&](profitable_move const& m) { per_source[i].push_back(m); });
    });

    for (auto& v : per_source)
      for (auto& m : v) callback(m);
  }

 private:
  void search_subtree_with_bound(std::size_t node, std::size_t src,
                                 std::size_t lca, std::size_t radius_left,
                                 src_removal_result const& removal,
                                 int& best_score, callback_t& callback,
                                 scratch_buffers& scratch,
                                 std::size_t* scored_count) const {
    if (index_.is_ancestor(src, node) || node == src) return;

    int score = compute_move_score_cached(src, node, lca, removal, scratch);
    if (scored_count) ++(*scored_count);
    if (score <= score_threshold_) {
      callback(profitable_move{src, node, lca, score});
      if (score < best_score) best_score = score;
    }

    if (radius_left == 0) return;

    for (auto child : index_.get_children(node)) {
      search_subtree_with_bound(child, src, lca, radius_left - 1, removal,
                                best_score, callback, scratch, scored_count);
    }
  }

  void upward_traversal(std::size_t src, std::size_t radius,
                        callback_t& callback, scratch_buffers& scratch,
                        std::size_t* scored_count) const {
    if (src == index_.get_tree_root()) return;

    std::size_t n_sites = index_.num_variable_sites();

    src_removal_result& removal = scratch.removal;
    removal.score_change = 0;
    removal.lca_nc_adjustment = -1;
    auto const* src_fitch = index_.get_fitch_set_ptr(src);
    for (std::size_t si = 0; si < n_sites; si++) {
      removal.old_fitch[si] = src_fitch[si];
      removal.new_fitch[si] = 0;
    }

    auto current = index_.get_parent(src);
    auto prev = src;
    std::size_t levels_up = 0;
    int best_score = 0;

    while (levels_up < radius) {
      levels_up++;

      auto& children = index_.get_children(current);
      std::size_t remaining_radius = radius - levels_up;

      for (auto child : children) {
        if (child == prev) continue;
        search_subtree_with_bound(child, src, current, remaining_radius,
                                  removal, best_score, callback, scratch,
                                  scored_count);
      }

      if (current == index_.get_tree_root()) break;

      if (levels_up == 1)
        compute_initial_removal(src, removal);
      else
        propagate_removal_upward(current, removal);

      prev = current;
      current = index_.get_parent(current);
    }
  }

  // Need mutable ref for is_ua check in upward_traversal
  tree_index const& index_;
  int score_threshold_ = -1;
};

// ============================================================================
// Tree cloning and SPR move application
// ============================================================================

inline std::pair<phylo_dag, std::vector<std::size_t>> clone_tree(
    phylo_dag& src) {
  phylo_dag dst;
  std::vector<std::size_t> old_to_new(src.node_high_mark(), 0);

  // Clone all nodes
  for (auto nv : src.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          using NV = std::remove_cvref_t<decltype(node)>;
          if constexpr (std::is_same_v<NV,
                                       node_view<phylo_dag, node_kind::ua>>) {
            auto n = dst.append_node<node_kind::ua>();
            n.reference_sequence() = node.reference_sequence();
            old_to_new[node.index()] = n.index();
          } else if constexpr (std::is_same_v<
                                   NV,
                                   node_view<phylo_dag, node_kind::inner>>) {
            auto n = dst.append_node<node_kind::inner>();
            n.cg() = node.cg();
            old_to_new[node.index()] = n.index();
          } else if constexpr (std::is_same_v<
                                   NV, node_view<phylo_dag, node_kind::leaf>>) {
            auto n = dst.append_node<node_kind::leaf>();
            n.cg() = node.cg();
            n.sample_id() = node.sample_id();
            old_to_new[node.index()] = n.index();
          }
        },
        nv);
  }

  // Clone all edges
  for (auto ev : src.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto e = dst.append_edge<edge_kind::clade>();
          e.mutations() = edge.mutations();
          e.clade_index() = edge.clade_index();
          auto parent_idx = old_to_new[get_parent_idx(src, edge.index())];
          auto child_idx = old_to_new[get_child_idx(src, edge.index())];
          auto pv = dst.get_node(parent_idx);
          std::visit([&](auto p) { e.set_parent(p); }, pv);
          auto cv = dst.get_node(child_idx);
          std::visit([&](auto c) { e.set_child(c); }, cv);
        },
        ev);
  }

  // Set root
  auto old_root_idx = get_root_idx(src);
  auto rv = dst.get_node(old_to_new[old_root_idx]);
  std::visit([&](auto r) { dst.set_root(r); }, rv);

  return {std::move(dst), std::move(old_to_new)};
}

// fitch_assign_compact_genomes is now defined in compute.hpp
// (moved there so dagutil can use it without pulling in the full optimizer)

inline phylo_dag apply_spr_move(phylo_dag& tree, std::size_t src_idx,
                                std::size_t dst_idx) {
  auto [result, old_to_new] = clone_tree(tree);
  auto new_src = old_to_new[src_idx];
  auto new_dst = old_to_new[dst_idx];

  // Find src's parent edge and parent node
  auto src_pe = get_parent_edges(result, new_src);
  auto src_parent_edge = src_pe[0];
  auto src_parent = get_parent_idx(result, src_parent_edge);

  // Find dst's parent edge and parent node
  auto dst_pe = get_parent_edges(result, new_dst);
  auto dst_parent_edge = dst_pe[0];
  auto dst_parent = get_parent_idx(result, dst_parent_edge);

  // Get src_parent's child count
  auto src_parent_clades = get_clades(result, src_parent);
  std::size_t src_parent_child_count = 0;
  for (auto& c : src_parent_clades) src_parent_child_count += c.size();

  // 1. Remove edge from src_parent to src
  auto ev1 = result.get_edge(src_parent_edge);
  std::visit([](auto edge) { edge.remove(); }, ev1);

  // 2. If src_parent was binary, collapse it
  if (src_parent_child_count == 2) {
    // After removing the src edge, src_parent has one remaining child
    auto remaining_clades = get_clades(result, src_parent);
    std::size_t remaining_child = 0;
    for (auto& cl : remaining_clades) {
      if (!cl.empty()) {
        remaining_child = get_child_idx(result, cl[0]);
        break;
      }
    }

    // Find grandparent
    auto sp_parent_edges = get_parent_edges(result, src_parent);
    if (!sp_parent_edges.empty()) {
      auto grandparent = get_parent_idx(result, sp_parent_edges[0]);
      auto gp_clade = get_clade_idx(result, sp_parent_edges[0]);

      // Remove src_parent node (removes all its edges)
      auto sp_nv = result.get_node(src_parent);
      std::visit([](auto n) { n.remove(); }, sp_nv);

      // Connect grandparent to remaining child
      auto e = result.append_edge<edge_kind::clade>();
      e.clade_index() = gp_clade;
      auto gpv = result.get_node(grandparent);
      std::visit([&](auto p) { e.set_parent(p); }, gpv);
      auto rcv = result.get_node(remaining_child);
      std::visit([&](auto c) { e.set_child(c); }, rcv);
    }
  }

  // 3. Remove edge from dst_parent to dst
  // Re-fetch dst parent edge since indices may have shifted after node removal
  auto dst_pe2 = get_parent_edges(result, new_dst);
  auto dst_parent_edge2 = dst_pe2[0];
  std::size_t dst_clade = get_clade_idx(result, dst_parent_edge2);
  // Also re-fetch dst_parent in case it changed
  dst_parent = get_parent_idx(result, dst_parent_edge2);
  auto ev3 = result.get_edge(dst_parent_edge2);
  std::visit([](auto edge) { edge.remove(); }, ev3);

  // 4. Create new inner node
  auto new_inner = result.append_node<node_kind::inner>();

  // 5. Connect dst_parent -> new_inner
  {
    auto e = result.append_edge<edge_kind::clade>();
    e.clade_index() = dst_clade;
    auto dpv = result.get_node(dst_parent);
    std::visit([&](auto p) { e.set_parent(p); }, dpv);
    e.set_child(new_inner);
  }

  // 6. Connect new_inner -> dst
  {
    auto e = result.append_edge<edge_kind::clade>();
    e.clade_index() = 0;
    e.set_parent(new_inner);
    auto dv = result.get_node(new_dst);
    std::visit([&](auto c) { e.set_child(c); }, dv);
  }

  // 7. Connect new_inner -> src
  {
    auto e = result.append_edge<edge_kind::clade>();
    e.clade_index() = 1;
    e.set_parent(new_inner);
    auto sv = result.get_node(new_src);
    std::visit([&](auto c) { e.set_child(c); }, sv);
  }

  // 8. Assign optimal CGs and recompute
  fitch_assign_compact_genomes(result);
  recompute_edge_mutations(result);
  set_sample_ids_from_cg(result);

  return result;
}

// ============================================================================
// native_strategy: MoveStrategy for optimize_dag
// ============================================================================

struct native_strategy {
  std::size_t max_moves;
  std::size_t radius{0};  // 0 = auto (depth * 2)
  thread_pool& pool = thread_pool::get_default();

  std::vector<phylo_dag> operator()(phylo_dag& tree, std::mt19937& /*rng*/) {
    tree_index idx{tree, pool};
    move_enumerator enumerator{idx};
    auto radius =
        this->radius > 0 ? this->radius : compute_tree_max_depth(tree) * 2;
    if (radius == 0) radius = 1;

    std::vector<profitable_move> moves;
    enumerator.find_all_moves_parallel(
        radius, [&](auto& m) { moves.push_back(m); }, pool);

    std::sort(moves.begin(), moves.end(),
              [](auto& a, auto& b) { return a.score_change < b.score_change; });

    if (moves.size() > max_moves) moves.resize(max_moves);

    std::vector<phylo_dag> result(moves.size());
    std::vector<std::size_t> indices(moves.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});

    parallel_for_each(pool, indices, [&](std::size_t i) {
      result[i] = apply_spr_move(tree, moves[i].src, moves[i].dst);
    });

    return result;
  }
};

// ============================================================================
// native_move_producer: MoveProducer for optimize_dag_v2
// ============================================================================

struct native_move_producer {
  std::size_t max_moves;
  std::size_t radius{0};  // 0 = auto (depth * 2)
  thread_pool& pool = thread_pool::get_default();

  void operator()(phylo_dag& tree, move_callback cb, std::mt19937& /*rng*/) {
    tree_index idx{tree, pool};
    move_enumerator enumerator{idx};
    auto radius =
        this->radius > 0 ? this->radius : compute_tree_max_depth(tree) * 2;
    if (radius == 0) radius = 1;

    std::vector<profitable_move> moves;
    enumerator.find_all_moves_parallel(
        radius, [&](auto& m) { moves.push_back(m); }, pool);

    std::sort(moves.begin(), moves.end(),
              [](auto& a, auto& b) { return a.score_change < b.score_change; });
    if (moves.size() > max_moves) moves.resize(max_moves);

    for (auto& m : moves)
      cb(spr_move{.src = m.src,
                  .dst = m.dst,
                  .lca = m.lca,
                  .score_change = m.score_change});
  }
};

}  // namespace larch

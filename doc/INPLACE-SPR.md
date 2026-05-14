# In-Place SPR: Implementation Plan

## Motivation

The current optimizer hits a wall when no single SPR move improves parsimony.
The existing `drift_escape()` in `larch2.cpp:1080-1206` attempts to work around
this by applying 1–5 neutral moves, then searching for improving moves on the
drifted tree. But each drift step pays the full cost of
`tree_index` construction — O(N × sites) — and clones the tree via
`apply_spr_move`. For a 100K-node tree with 30K variable sites, this is ~3
billion operations per step, making multi-step exploration impractical.

The core problem: the search is trapped in a local minimum, and the only way out
is a **sequence** of moves where intermediate steps may worsen parsimony but the
overall path leads to a better state. To find such paths efficiently, we need to
apply moves in-place to a single mutable tree, incrementally update the scoring
index, and re-search — all within a single iteration.

## Overview

```
                          ┌──────────────────────────────────┐
                          │     Current: clone per move       │
                          │                                   │
                          │  sample ──► tree_index (full)     │
                          │               │                   │
                          │          enumerate moves          │
                          │               │                   │
                          │    ┌──────────┼──────────┐        │
                          │    ▼          ▼          ▼        │
                          │  clone₁    clone₂    clone₃      │
                          │  fitch₁    fitch₂    fitch₃      │
                          │    │          │          │        │
                          │    └──────────┼──────────┘        │
                          │               ▼                   │
                          │           merge all               │
                          └──────────────────────────────────┘

                          ┌──────────────────────────────────┐
                          │     Proposed: in-place loop       │
                          │                                   │
                          │  sample ──► tree_index (full)     │
                          │               │                   │
                          │          ┌────┴────┐              │
                          │          ▼         │              │
                          │     enumerate      │              │
                          │          │         │              │
                          │     pick move      │ repeat K     │
                          │          │         │ times        │
                          │     apply in-place │              │
                          │          │         │              │
                          │     update index   │              │
                          │     (incremental)  │              │
                          │          │         │              │
                          │     queue fragment │              │
                          │          └─────────┘              │
                          │               │                   │
                          │     extract & merge fragments     │
                          └──────────────────────────────────┘
```

**Key invariant**: the sampled tree is the single mutable working copy for the
duration of the multi-step loop. Fragments are extracted at the end (or
periodically) for DAG merge. No conflict resolution is needed because moves are
applied sequentially.

**Per-step cost comparison**:

| Operation              | Current (clone)          | Proposed (in-place)         |
|------------------------|--------------------------|-----------------------------|
| Scoring next move      | O(N × sites) full rebuild | O(path × sites) incremental |
| Topology change        | O(N) clone + mutate       | O(1) mutate directly        |
| CG/Fitch for merge     | O(N × sites) per clone    | Deferred to end             |
| Thread safety          | Trivial (isolation)       | Trivial (sequential)        |

For a 100K-node tree (path ~20), the per-step index update is ~5000× cheaper
than a full rebuild.

### Files to create or modify

| File | Action |
|------|--------|
| `include/larch/inplace_spr.hpp` | **New** — in-place SPR, spr_result, tree_state |
| `include/larch/native_optimize.hpp` | **Modify** — add incremental update methods to tree_index |
| `include/larch/spr_pipeline.hpp` | **Modify** — add inplace_move_producer |
| `tools/larch2.cpp` | **Modify** — CLI flags, integrate with run_native |
| `test/inplace_spr_test.cpp` | **New** — all test cases |
| `CMakeLists.txt` | **Modify** — add test target |

---

## Part 1 — Data Structures (Phases 1–3)

### Phase 1: `spr_result` struct

Define a struct that captures all topology changes made by an in-place SPR, so
the incremental index update knows exactly what changed.

```cpp
// inplace_spr.hpp
struct spr_result {
  std::size_t src;               // moved node
  std::size_t dst;               // destination (original child of dst_parent)
  std::size_t src_parent;        // original parent of src
  std::size_t dst_parent;        // parent of dst (now parent of new_inner)
  std::size_t new_inner;         // freshly created inner node
  std::size_t lca;               // lowest common ancestor of src and dst

  bool src_parent_collapsed;     // true if src_parent was binary and removed
  std::size_t grandparent;       // parent of collapsed src_parent (valid iff collapsed)
  std::size_t remaining_child;   // sibling of src that was reparented (valid iff collapsed)
  std::size_t collapsed_node;    // index of the removed node (valid iff collapsed)
};
```

**Files**: `include/larch/inplace_spr.hpp` (new)

**Tests**:
- Construct `spr_result` with various field combinations.
- Verify `src_parent_collapsed == false` implies `collapsed_node` is unused.

---

### Phase 2: `tree_state` wrapper

A lightweight wrapper that bundles a mutable `phylo_dag` reference with a
`tree_index` and tracks the running parsimony score. This is the object passed
through the multi-step loop.

```cpp
struct tree_state {
  phylo_dag& tree;
  tree_index index;
  int parsimony_score;       // running total
  std::size_t step_count;    // moves applied so far

  explicit tree_state(phylo_dag& t, thread_pool& pool);
  // parsimony_score initialized from tree_index root Fitch cost
};
```

Computing the initial parsimony score: walk the tree_index bottom-up, counting
sites where a node's Fitch set does not contain any single allele present in all
children (i.e., the Fitch cost). This is the same value
`compute_move_score` produces for the root.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Build `tree_state` on a 6-leaf test tree; verify `parsimony_score` matches
  `subtree_weight<parsimony_score_ops>::compute_weight_below()`.
- Verify `step_count` starts at 0.

---

### Phase 3: Add `tree_index` accessor for node validity

The `tree_index` uses flat arrays indexed by node ID. After removing a node
(collapsed binary parent), its slot becomes invalid. Add an `is_valid_` vector
so incremental updates can skip dead nodes.

Add to `tree_index`:
```cpp
std::vector<uint8_t> is_valid_;   // 1 if node is live in the tree
```

Initialize in `init()`: `is_valid_[nid] = 1` for every node visited, `0`
otherwise. Use `is_valid_` only in incremental update code (e.g.,
`compute_parsimony_score`, `update_searchable_nodes`) — **not** in general
accessors like `is_ancestor()` or `get_fitch_set()`. Those hot-path functions
are only called during tree traversals that naturally skip invalid nodes via
topology (`children_[]`, `parent_[]`), and the existing `has_dfs_info_` /
bounds checks in `is_ancestor()` already handle out-of-range indices. Adding
a branch to every accessor would penalize the common case for a cold-path
benefit.

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Build tree_index; all tree nodes have `is_valid_ == 1`.
- Verify `is_valid_[ua_idx] == 0` (UA is not part of the tree_index tree).
- Manually set `is_valid_[n] = 0`; verify `compute_parsimony_score` skips
  node `n` (it is excluded from the Fitch cost sum).

---

## Part 2 — In-Place SPR on `phylo_dag` (Phases 4–9)

These phases implement `apply_spr_inplace()`, which modifies the `phylo_dag`
directly (no clone) and returns an `spr_result`.

### Phase 4: Detach source from parent

Remove the edge from `src_parent` to `src`. This is the first step of every SPR.

After this, `src` has no parent edge and `src_parent` has one fewer child.

Implementation: call `edge_view::remove()` on the parent edge of `src`.
This internally calls `unlink_child` on `src_parent` and `unlink_parent` on
`src`, then deallocates the edge.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Build a tree: UA → R → {A, B, C}. Detach A. Verify:
  - A has 0 parent edges.
  - R has 2 children (B, C).
  - Edge count decreased by 1.
  - Node count unchanged.

---

### Phase 5: Collapse binary source parent

If `src_parent` had exactly 2 children before detach (now 1), it is a
degenerate unary node and must be collapsed: wire its remaining child directly
to its grandparent.

Steps:
1. Find the remaining child of `src_parent`.
2. Find the grandparent (parent of `src_parent`).
3. Record the clade index of the `grandparent → src_parent` edge.
4. Remove `src_parent` (calls `node_view::remove()`, which removes all its
   edges and deallocates the node topology, creating a hole in the chain).
5. Add a new edge: `grandparent → remaining_child` with the recorded clade
   index.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Tree: UA → R → {P → {A, B}, C}. Detach A from P (binary). Verify:
  - P is removed (`node_count` decreased by 1).
  - B is now a direct child of R.
  - R has children {B, C}.
  - The clade index on the R→B edge matches the old R→P edge.
- Tree: UA → R → {P → {A, B}, C, D}. Detach A from P (binary). Same collapse.
- Tree: UA → R → {A, B}. Detach A from R (binary, R is tree root). Verify:
  - R is removed.
  - B is now direct child of UA.
  - Note: `tree_index::tree_root_` must be updated to B. This is tested
    fully in Phase 15; here, verify only the DAG topology.

---

### Phase 6: Reattach at destination

Create a new inner node between `dst` and its parent, then attach `src` as a
sibling of `dst` under this new inner node.

Steps:
1. Find `dst_parent` and the clade index of the `dst_parent → dst` edge.
2. Remove the `dst_parent → dst` edge.
3. Append a new `node_kind::inner` node (`new_inner`).
4. Add edge: `dst_parent → new_inner` (same clade index as removed edge).
5. Add edge: `new_inner → dst` (clade index 0).
6. Add edge: `new_inner → src` (clade index 1).

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Tree: UA → R → {A, B, C}. Move A to be sibling of C. Verify:
  - `new_inner` exists with children {C, A}.
  - R has children {B, new_inner}.
  - `new_inner`'s parent is R.
  - Total node count = original + 1 (new_inner) − 0 (no collapse since R had 3 children).
  - Total edge count = original + 2 (new edges) − 1 (removed dst edge) = original + 1.

---

### Phase 7: Assemble `apply_spr_inplace()`

Combine phases 4–6 into a single function:

```cpp
spr_result apply_spr_inplace(phylo_dag& tree, tree_index const& index,
                             std::size_t src, std::size_t dst);
```

The `tree_index` is needed to compute the LCA via `is_ancestor()` before
topology changes invalidate DFS intervals.

This function:
1. Records `src_parent`, `dst_parent`, child count of `src_parent`.
2. **Computes LCA of `src` and `dst`** using the tree_index's DFS intervals
   (`is_ancestor`). This must happen before any topology changes, because
   detach/collapse invalidates the ancestor relationship. Store the result
   in `spr_result.lca`.
3. Detaches src (Phase 4).
4. If binary, collapses `src_parent` (Phase 5).
5. Reattaches at dst (Phase 6).
6. Populates and returns `spr_result`.

Does **not** recompute CGs, edge mutations, or Fitch sets — that is handled
by the incremental tree_index update.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Apply SPR in-place on a 7-node tree. Verify `spr_result` fields:
  - `new_inner` is a valid node index with correct children.
  - `src_parent_collapsed` is correct.
  - If collapsed, `collapsed_node` was actually removed from the DAG.
- Apply SPR where src and dst share the same parent (sibling move). Verify
  correctness even when `src_parent == dst_parent`.

---

### Phase 8: Validate in-place vs clone-based SPR

The topology produced by `apply_spr_inplace` must match `apply_spr_move` (the
existing clone-based version). Write a test that applies the same move both
ways and compares the resulting tree structures.

**Tests**:
- For 10 different (src, dst) pairs on a 12-leaf test tree:
  1. Clone the tree.
  2. Apply `apply_spr_move(clone, src, dst)` → `clone_result`.
  3. Apply `apply_spr_inplace(original, src, dst)`.
  4. Run `fitch_assign_compact_genomes` + `recompute_edge_mutations` on
     `original`.
  5. Compare leaf sets: same leaves in both results.
  6. Compare parsimony score (edge mutation count): identical.
  7. Compare topology: same parent-child relationships (modulo node index
     renumbering).

---

### Phase 9: Edge cases and robustness

Test degenerate SPR configurations.

**Tests**:
- **src is direct child of dst_parent**: src becomes sibling of dst under
  new_inner; original parent collapses (if binary). Verify no double-removal.
- **dst is child of src_parent** (sibling swap): topology should still be
  valid after move.
- **LCA is tree root**: move spans the entire tree. Verify root is handled.
- **LCA collapses** (src_parent == LCA and was binary): grandparent becomes
  new LCA. Verify `spr_result.grandparent` is correct.
- **Tree has polytomies**: src_parent has >2 children, no collapse needed.
  Verify no crash.
- **Move to adjacent node** (dst is src's sibling): effectively a no-op
  topology change (src detaches and reattaches in same neighborhood). Verify
  tree is still valid.

---

## Part 3 — Incremental tree_index Topology (Phases 10–16)

After `apply_spr_inplace()` modifies the `phylo_dag`, the `tree_index`'s
cached arrays are stale. These phases add an incremental update method.

### Phase 10: `tree_index::update_topology(spr_result const& r)`

Add a method that patches `parent_[]` and `children_[]` given the `spr_result`.

Changes needed:
- **src_parent** (or grandparent if collapsed): remove src from
  `children_[src_parent]`. If collapsed, replace src_parent with
  remaining_child in `children_[grandparent]`.
- **new_inner**: create entry `children_[new_inner] = {dst, src}`,
  `parent_[new_inner] = dst_parent`.
- **dst_parent**: replace dst with new_inner in `children_[dst_parent]`.
- **dst**: `parent_[dst] = new_inner`.
- **src**: `parent_[src] = new_inner`.
- **collapsed_node**: mark `is_valid_[collapsed_node] = 0`;
  clear `num_children_[collapsed_node] = 0` and
  `has_child_counts_[collapsed_node] = false` so a future reuse of the slot
  sees a clean Fitch baseline (`old_cost = 0`).
- **new_inner**: mark `is_valid_[new_inner] = 1`.

Grow flat arrays if `new_inner >= num_nodes_` (the chain may have allocated
at `high_mark`).

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Apply in-place SPR, then `update_topology`. Verify `parent_[]` and
  `children_[]` match a freshly built `tree_index` on the same tree.
- Verify `is_valid_[collapsed_node] == 0` after collapse.
- Verify `is_valid_[new_inner] == 1`.

---

### Phase 11: Grow flat arrays for new nodes

When `apply_spr_inplace` appends a new inner node, its index may be ≥
`num_nodes_`. The tree_index must resize all flat arrays:

```cpp
void ensure_capacity(std::size_t new_high_mark) {
  if (new_high_mark <= num_nodes_) return;
  dfs_info_.resize(new_high_mark);
  has_dfs_info_.resize(new_high_mark, 0);
  is_valid_.resize(new_high_mark, 0);
  fitch_sets_.resize(new_high_mark * num_variable_sites_, 0);
  child_counts_.resize(new_high_mark * num_variable_sites_, {0,0,0,0});
  has_child_counts_.resize(new_high_mark, 0);
  num_children_.resize(new_high_mark, 0);
  allele_union_.resize(new_high_mark * num_variable_sites_, 0);
  parent_.resize(new_high_mark, 0);
  children_.resize(new_high_mark);
  is_condensed_.resize(new_high_mark, 0);
  subtree_size_.resize(new_high_mark, 0);
  ref_alleles_ stays the same (sites don't change).
  num_nodes_ = new_high_mark;
}
```

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Apply SPR that creates new_inner at index `num_nodes_`. Call
  `ensure_capacity`. Verify all arrays have correct size and new slots are
  zero-initialized.
- Apply two SPRs in sequence; verify capacity grows monotonically.

---

### Phase 12: Update `searchable_nodes_`

After an SPR:
- Add `new_inner` to `searchable_nodes_` (it is a valid search source).
- Remove `collapsed_node` from `searchable_nodes_` (if collapse happened).
- `src` and `dst` remain searchable (they were already in the list).

The simplest implementation: maintain `searchable_nodes_` as an
`unordered_set` for O(1) insert/erase, and convert to a vector only when
`find_all_moves_parallel` needs it.

Alternatively, keep the vector and accept O(N) removal (N is small, typically
≤2 removals per SPR).

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- After SPR with collapse: `collapsed_node` not in `searchable_nodes_`,
  `new_inner` is in `searchable_nodes_`.
- After SPR without collapse: `new_inner` added, nothing removed.

---

### Phase 13: Incremental `subtree_size_[]` update

Walk up from both affected insertion and removal points to the root, recomputing
`subtree_size_[node] = 1 + sum(subtree_size_[child])` for each node on the
path.

Affected nodes: `new_inner`, `dst_parent`, then up to root along parent chain.
On the removal side: `src_parent` (or `grandparent` if collapsed), then up to
root.

Since both paths merge at LCA, the total path length is bounded by 2 × depth.

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- After SPR + topology update + subtree_size update: compare every
  `subtree_size_[n]` against a freshly computed value (recursive recount).
  All must match.

---

### Phase 14: Full DFS re-traverse for `dfs_info_[]`

DFS indices (`dfs_index`, `dfs_end_index`, `level`) cannot be cheaply
updated incrementally — an SPR can change the DFS interval of nodes far
from the move. Re-traverse the entire tree.

This is O(N) with no per-site work, so for 100K nodes it is ~100K
operations — negligible compared to O(path × sites) Fitch update.

```cpp
void recompute_dfs() {
  std::fill(has_dfs_info_.begin(), has_dfs_info_.end(), 0);
  std::size_t counter = 0;
  dfs_visit(tree_root_, counter, 0);
}
```

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- After SPR + DFS re-traverse: `dfs_info_` matches a freshly built
  `tree_index`.
- `is_ancestor(a, b)` returns correct results for 20 random pairs.

---

### Phase 15: Handle tree root change

Two cases can change which node is the direct child of UA:

**(a) Removal collapse:** `src_parent` was binary and was the tree root. After
collapse, `remaining_child` (or `new_inner`, if dst was the remaining child)
becomes the direct child of UA. Re-derive from the DAG:
```cpp
tree_root_ = first child of UA in the modified phylo_dag
```

**(b) Insertion above root:** `dst == tree_root_`. `new_inner` is inserted
between UA and the old root, so `new_inner` becomes the new tree root:
```cpp
if (r.dst == tree_root_) tree_root_ = r.new_inner;
```

Both cases are checked in sequence; (a) fires first so that (b) compares
against the already-updated `tree_root_`.

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Tree: UA → R → {A, B}. Move A next to some other subtree such that R
  collapses. Verify `tree_root_` is updated correctly.
- After root change: `dfs_info_[tree_root_].level == 0`.
- Insertion above root: tree with ternary inner node, move leaf to root as
  dst. Verify `tree_root_` becomes `new_inner`.

---

### Phase 16: Condensed leaf re-check

If the SPR moved a leaf that was condensed (identical CG to a sibling under
the same parent), the condensation status may change: the moved leaf now has
different siblings.

Minimal update: re-check condensation only for the children of `new_inner`
(where src landed) and the children of `src_parent`/`grandparent` (where src
left). This is O(siblings²) per affected node, typically small.

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Build tree with two identical leaves under the same parent (one condensed).
  Move the non-condensed copy to a different parent. Verify the previously
  condensed sibling is now the sole representative and not condensed.
- Move a leaf next to an identical leaf. Verify one becomes condensed.

---

## Part 4 — Incremental Fitch Sets (Phases 17–23)

The most performance-critical part. After topology changes, update
`fitch_sets_[]`, `child_counts_[]`, `allele_union_[]`, and `num_children_[]`
only for affected nodes.

### Phase 17: Single-node Fitch recomputation

Add a method to recompute a single node's Fitch data from its current children:

```cpp
void recompute_node_fitch(std::size_t node_id) {
  auto& children = children_[node_id];
  uint8_t nc = static_cast<uint8_t>(children.size());
  num_children_[node_id] = nc;
  has_child_counts_[node_id] = true;
  std::size_t base = node_id * num_variable_sites_;

  for (std::size_t i = 0; i < num_variable_sites_; i++) {
    std::array<uint8_t, 4> c = {0, 0, 0, 0};
    uint8_t au = 0;
    for (auto child : children) {
      uint8_t cf = fitch_sets_[child * num_variable_sites_ + i];
      for (int j = 0; j < 4; j++)
        if (cf & (1 << j)) c[j]++;
      au |= allele_union_[child * num_variable_sites_ + i];
    }
    child_counts_[base + i] = c;
    fitch_sets_[base + i] = fitch_set_from_counts(c, nc);
    allele_union_[base + i] = au;
  }
}
```

Cost: O(num_children × num_variable_sites) per node.

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Build tree_index. Manually call `recompute_node_fitch(root)`. Verify Fitch
  sets unchanged (idempotent).
- Add a child to a node (in `children_[]` only), call `recompute_node_fitch`.
  Verify Fitch set now reflects the new child.

---

### Phase 18: Upward Fitch propagation

Walk from a starting node to the root, recomputing Fitch at each step.
Stop early if the Fitch set at a node doesn't change (all ancestors are
already correct).

```cpp
// Returns number of nodes updated.
std::size_t propagate_fitch_upward(std::size_t start) {
  std::size_t count = 0;
  std::size_t node = start;
  while (node != tree_root_ && is_valid_[node]) {
    auto old_fitch = get_fitch_snapshot(node);   // copy of fitch row
    recompute_node_fitch(node);
    count++;
    if (fitch_unchanged(node, old_fitch)) break; // early termination
    node = parent_[node];
  }
  // Always recompute root
  if (node == tree_root_) {
    recompute_node_fitch(node);
    count++;
  }
  return count;
}
```

Early termination is the key optimization: most SPR moves only change Fitch
sets for ~5–10 nodes before the change is absorbed.

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- On a 20-node tree, corrupt one leaf's Fitch set, then propagate upward.
  Verify all ancestors are corrected and early termination happens at the
  right level.
- Propagate from root: should recompute only root (1 node).

---

### Phase 19: Initialize Fitch for new inner node

When a new inner node is created (Phase 6), it has children `{dst, src}`.
Its Fitch data must be computed from scratch:

```cpp
void init_new_node_fitch(std::size_t new_inner) {
  // Leaf Fitch sets for src and dst are already correct
  // (they didn't change — only their parent changed)
  recompute_node_fitch(new_inner);
}
```

This is trivially a call to `recompute_node_fitch`. Separated as a phase for
clarity — it is the starting point of the insertion-path propagation.

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- After creating `new_inner` with children {A, B} where A has Fitch `{A}` and
  B has Fitch `{C}` at some site: verify `new_inner`'s Fitch is `{A, C}`
  (union, since A ∩ C = ∅).
- Children with matching Fitch: verify intersection.

---

### Phase 20: Fitch update for source removal path

After detaching `src` from `src_parent`:
1. If `src_parent` was collapsed, start from `grandparent`. Recompute its
   Fitch (it gained `remaining_child` as a direct child, lost `src_parent`).
2. If not collapsed, start from `src_parent`. Recompute its Fitch (one fewer
   child).
3. Propagate upward to root.

```cpp
void update_fitch_removal(spr_result const& r) {
  std::size_t start = r.src_parent_collapsed ? r.grandparent : r.src_parent;
  propagate_fitch_upward(start);
}
```

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Tree: R → {P → {A, B, C}, D}. Remove A from P (not binary). Verify
  P's Fitch reflects only {B, C}. Verify R's Fitch reflects P's new Fitch.
- Tree: R → {P → {A, B}, D}. Remove A, P collapses. Verify R's Fitch reflects
  {B, D} directly.

---

### Phase 21: Fitch update for destination insertion path

After reattaching `src` under `new_inner` next to `dst`:
1. Compute `new_inner`'s Fitch (Phase 19).
2. Propagate upward from `dst_parent` (which now has `new_inner` as child
   instead of `dst`).

```cpp
void update_fitch_insertion(spr_result const& r) {
  init_new_node_fitch(r.new_inner);
  propagate_fitch_upward(r.dst_parent);
}
```

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Move a leaf with unique mutation next to a leaf without it. Verify
  `new_inner` Fitch reflects the union. Verify `dst_parent` Fitch changes
  accordingly.

---

### Phase 22: Combined incremental Fitch update ✓ (session 32)

Combine removal and insertion path updates. Both paths converge at LCA and
share the LCA→root segment. Optimize: propagate both paths up to LCA
separately, then propagate once from LCA to root.

```cpp
void update_fitch(spr_result const& r) {
  // 1. Removal side: propagate up to (but not past) LCA
  std::size_t rem_start = r.src_parent_collapsed ? r.grandparent : r.src_parent;
  propagate_fitch_up_to(rem_start, r.lca);

  // 2. Insertion side: new_inner, then up to (but not past) LCA
  init_new_node_fitch(r.new_inner);
  propagate_fitch_up_to(r.dst_parent, r.lca);

  // 3. LCA to root (shared segment)
  propagate_fitch_upward(r.lca);  // includes root
}
```

Where `propagate_fitch_up_to(start, stop)` recomputes from `start` upward
but stops before `stop` (exclusive).

Edge cases:
- **LCA collapsed** (`r.src_parent == r.lca && r.src_parent_collapsed`): use
  `r.grandparent` as the effective LCA.
- **src_parent == LCA, not collapsed** (src_parent had >2 children):
  `rem_start == r.lca`, so `propagate_fitch_up_to(rem_start, r.lca)` is a
  no-op (start == stop). This is correct: step 3 (`propagate_fitch_upward(r.lca)`)
  recomputes the LCA's Fitch from its current children, which already reflect
  both the removal (src detached) and insertion (new_inner added below).

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Apply SPR, run combined Fitch update. Compare every node's `fitch_sets_`
  against a freshly built `tree_index`. All must match.
- Repeat for 20 different SPR moves on a 30-leaf tree. All must match.
- Verify early termination: count nodes updated; confirm < N for local moves.

---

### Phase 23: Validate incremental Fitch vs full rebuild ✓ (session 32)

Comprehensive cross-validation: for many SPR moves, compare incremental
update against a full tree_index rebuild.

**Tests**:
- Load `data/test_5_trees/tree_0.pb.gz`. Sample a min-weight tree.
- For each of the first 50 searchable nodes as `src`:
  - Pick a valid `dst` (not in src's subtree, not src's parent).
  - Clone the tree. Apply clone-based SPR → full tree_index → Fitch snapshot.
  - Apply in-place SPR on original → incremental update → Fitch snapshot.
  - Compare both Fitch snapshots: must be identical.
- This test is O(50 × N × sites) — acceptable for CI.

---

## Part 5 — Incremental Score Tracking (Phases 24–26)

### Phase 24: Compute parsimony score from tree_index ✓ (session 32)

Add a method to compute the tree's total parsimony (number of Fitch-cost
changes) directly from the tree_index's Fitch arrays at the root:

```cpp
int tree_index::compute_parsimony_score() const {
  int score = 0;
  // Standard Fitch cost: for each inner node and each variable site,
  // cost is 0 if the intersection of children's Fitch sets is non-empty,
  // 1 otherwise. We detect non-empty intersection by checking whether
  // any allele's child_count equals num_children.
  for (std::size_t nid = 0; nid < num_nodes_; nid++) {
    if (!is_valid_[nid] || !has_child_counts_[nid]) continue;
    uint8_t nc = num_children_[nid];
    if (nc == 0) continue;
    for (std::size_t i = 0; i < num_variable_sites_; i++) {
      auto& c = child_counts_[nid * num_variable_sites_ + i];
      bool has_intersection = false;
      for (int j = 0; j < 4; j++) {
        if (c[j] == nc) { has_intersection = true; break; }
      }
      if (!has_intersection) score++;
    }
  }
  return score;
}
```

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- On the 6-leaf test tree: verify score matches
  `subtree_weight<parsimony_score_ops>::compute_weight_below()`.
- On a tree where all leaves are identical (parsimony = 0): verify score = 0.
- On a star tree with all-different leaves: verify score = (num_leaves − 1)
  × num_variable_sites... actually this depends on the tree structure.
  Verify against known hand-computed value.

---

### Phase 25: Incremental score delta from Fitch update ✓ (session 32, via Phase 18/22)

Instead of recomputing the full score, track the delta during the upward
propagation. At each recomputed node, compare old vs new Fitch cost:

```cpp
int delta = 0;
// Before recomputing node:
int old_cost = count_union_sites(node);  // sites where fitch is union
recompute_node_fitch(node);
int new_cost = count_union_sites(node);
delta += (new_cost - old_cost);
```

Accumulate deltas along both affected paths. The total delta is the parsimony
score change.

Add this to `propagate_fitch_upward`:
```cpp
std::size_t propagate_fitch_upward(std::size_t start, int& delta_out);
```

**Files**: `include/larch/native_optimize.hpp`

**Tests**:
- Apply SPR in-place, track delta. Verify `old_score + delta ==
  compute_parsimony_score()` (full recomputation).
- Apply a known-improving move. Verify delta < 0.
- Apply a known-worsening move. Verify delta > 0.

---

### Phase 26: Update `tree_state` running score ✓ (session 32)

Wire the delta tracking into `tree_state`:

```cpp
void tree_state::apply_move(std::size_t src, std::size_t dst) {
  auto result = apply_spr_inplace(tree, src, dst);
  index.ensure_capacity(tree.node_high_mark());  // grow arrays BEFORE writing
  index.update_topology(result);
  index.recompute_dfs();
  index.update_subtree_sizes(result);
  index.update_searchable_nodes(result);
  int delta = 0;
  index.update_fitch(result, delta);
  parsimony_score += delta;
  step_count++;
}
```

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Apply 5 sequential moves to a tree via `tree_state::apply_move`. After
  each, verify `parsimony_score` matches full recomputation.
- Verify `step_count == 5` at the end.

---

## Part 6 — Fragment Extraction (Phases 27–29)

### Phase 27: Copy full tree from modified `phylo_dag` ✓ (session 32)

After in-place modification, the `phylo_dag`'s inner-node CGs and edge
mutations are stale (only the tree_index Fitch arrays are current). To produce
a fragment for merge, clone the tree and run full Fitch:

```cpp
phylo_dag extract_fragment(phylo_dag& tree) {
  auto [fragment, _] = clone_tree(tree);
  fitch_assign_compact_genomes(fragment);
  recompute_edge_mutations(fragment);
  set_sample_ids_from_cg(fragment);
  return fragment;
}
```

This is O(N + N × sites) — acceptable because fragment extraction is not on
the hot path of the multi-step loop. It happens K times per iteration (once
per step), or once at the end, depending on the extraction strategy.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Apply 3 in-place SPRs, extract fragment. Verify:
  - Fragment is a valid tree (`verify_is_tree`).
  - Fragment has same leaf set as original.
  - Fragment's parsimony score matches `tree_state.parsimony_score`.

---

### Phase 28: Deferred fragment extraction ✓ (session 33)

For the multi-step loop, we have two strategies:

**A. Extract after every step** (maximum DAG diversity):
```cpp
for step in 0..K:
    apply_move(...)
    fragments.push_back(extract_fragment(tree))
```
Cost: K × O(N × sites).

**B. Extract only at the end** (minimum overhead):
```cpp
for step in 0..K:
    apply_move(...)
fragments.push_back(extract_fragment(tree))
```
Cost: 1 × O(N × sites).

**C. Record moves and replay** (deferred extraction) — **Deferred**:
Store the sequence of `spr_move` descriptors and replay on the original tree.
This is non-trivial because after multiple in-place SPRs, node indices diverge:
new inner nodes are created and collapsed nodes are removed. Replaying a
work_tree move `(src, dst)` on the original tree would reference wrong or
nonexistent node indices. Making this work requires either (a) storing moves
in original-tree coordinates from the start via the `old_to_new` map from
`clone_tree`, or (b) maintaining a bidirectional index map through each SPR.
**Not implemented in v1; revisit if Strategy A proves too expensive.**

Strategy B is simplest and is recommended as the default. Strategy A adds
diversity but at proportional cost.

Implement A and B behind a configuration enum:
```cpp
enum class fragment_strategy { every_step, final_only };
```

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- **every_step**: K fragments produced, each a valid tree with correct leaves.
- **final_only**: 1 fragment produced.

---

### Phase 29: Rebuild clade offsets before extraction ✓ (session 33, handled by extract_fragment)

The in-place SPR invalidates clade offsets on affected nodes. Before cloning
(which uses `get_clades()`), rebuild clade offsets:

```cpp
tree.build_all_clade_offsets(
    [&](std::size_t eidx) { return get_clade_idx(tree, eidx); });
```

This is O(N × max_children) — cheap.

Note: during the multi-step loop, clade offsets are NOT needed (the tree_index
uses its own `children_[]` array, not `get_clades()`). Only rebuild before
fragment extraction.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- After 3 in-place SPRs, rebuild clade offsets. Verify `get_clades(tree, n)`
  returns correct children grouped by clade index for every node.
- Verify fragment extraction works without assertion failures.

---

## Part 7 — Multi-Step Iteration Loop (Phases 30–36)

### Phase 30: `inplace_move_producer` struct ✓ (session 33)

The MoveProducer concept from `spr_pipeline.hpp` takes a tree and callback.
Define a new producer that runs the multi-step loop internally:

```cpp
struct inplace_move_producer {
  std::size_t max_moves;
  std::size_t max_steps;          // K: steps per iteration
  std::size_t radius{0};
  int accept_threshold{0};        // accept moves with delta <= threshold
  fragment_strategy frag_mode{fragment_strategy::final_only};
  thread_pool& pool = thread_pool::get_default();

  void operator()(phylo_dag& tree, dag_callback frag_cb, std::mt19937& rng);
};
```

This is a `MoveProducer` that can be passed to `optimize_dag_v2`.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- Verify `inplace_move_producer` satisfies the `MoveProducer` concept.
- Call with a trivial tree; verify callback is invoked with valid `spr_move`s.

---

### Phase 31: Core loop — score, select, apply, extract ✓ (session 33)

Implement the operator():

```cpp
void inplace_move_producer::operator()(
    phylo_dag& tree, move_callback cb, std::mt19937& rng) {
  // Clone the tree so we can mutate it without affecting the caller's copy
  auto [work_tree, idx_map] = clone_tree(tree);
  fitch_assign_compact_genomes(work_tree);
  recompute_edge_mutations(work_tree);
  set_sample_ids_from_cg(work_tree);

  tree_state state{work_tree, pool};
  std::vector<spr_move> recorded_moves;

  for (std::size_t step = 0; step < max_steps; ++step) {
    // 1. Enumerate moves on current tree
    move_enumerator enumerator{state.index, accept_threshold};
    auto r = radius > 0 ? radius : compute_tree_max_depth(work_tree) * 2;
    if (r == 0) r = 1;

    std::vector<profitable_move> candidates;
    // Note: candidates.push_back in the callback is safe because
    // find_all_moves_parallel collects into per-source vectors internally
    // and invokes the callback sequentially from the main thread after
    // the parallel phase completes (see native_optimize.hpp:974-990).
    enumerator.find_all_moves_parallel(r,
        [&](profitable_move const& m) { candidates.push_back(m); }, pool);

    if (candidates.empty()) break;  // stuck

    // 2. Select move (best improving, or random if accepting worsening)
    std::sort(candidates.begin(), candidates.end(),
              [](auto& a, auto& b) { return a.score_change < b.score_change; });
    auto& chosen = select_move(candidates, rng);

    // 3. Record for fragment extraction
    recorded_moves.push_back(spr_move{
        .src = chosen.src, .dst = chosen.dst, .lca = chosen.lca,
        .score_change = chosen.score_change});

    // 4. Apply in-place
    state.apply_move(chosen.src, chosen.dst);

    // 5. Optionally extract fragment
    if (frag_mode == fragment_strategy::every_step)
      cb(recorded_moves.back());
  }

  // 6. Extract final fragment
  if (frag_mode == fragment_strategy::final_only && !recorded_moves.empty())
    cb(recorded_moves.back());
}
```

Note: the callback receives `spr_move` descriptors. The actual fragment
generation (clone + Fitch) happens in `optimize_dag_v2` or a wrapper, using
`apply_spr_as_fragment` on the `work_tree` state at each step. This requires
rethinking the callback — see Phase 33.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- On a tree at a local minimum (no improving single moves): verify the loop
  runs multiple steps (accepting worsening moves) and eventually finds an
  improving state.
- On a tree with clear improving moves: verify the loop picks the best move
  and runs 1 step.

---

### Phase 32: Move selection policy ✓ (session 33)

Separate move selection into a pluggable function:

```cpp
using move_selector = std::function<
    profitable_move const&(std::vector<profitable_move> const&, std::mt19937&)>;
```

Default selectors:

**best_improving**: Pick the most negative score_change. If none are negative,
pick the least-worsening.

**random_weighted**: Weight by exp(-score_change / temperature). Boltzmann
selection.

**random_uniform**: Pick uniformly at random from all candidates.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- `best_improving` always returns the move with smallest score_change.
- `random_weighted` with temperature→0 converges to `best_improving`.
- `random_weighted` with temperature→∞ converges to `random_uniform`.
- `random_uniform` returns each candidate with roughly equal probability
  (chi-squared test over 10000 calls).

---

### Phase 33: Fragment generation integration ✓ (session 33)

The `move_callback` from `spr_pipeline.hpp` receives `spr_move` descriptors.
The `optimize_dag_v2` pipeline then calls `apply_spr_as_fragment(sampled, move)`
on the original sampled tree. But for the in-place producer, the moves are
relative to the evolving `work_tree`, not the original.

**Decision: Option A — separate `FragmentProducer` concept.**

The `inplace_move_producer` cannot satisfy the existing `MoveProducer` concept
(3 parameters: tree, move_callback, rng) because its moves are relative to an
evolving internal work_tree, not the caller's sampled tree. Calling
`apply_spr_as_fragment(sampled, move)` with work_tree-relative moves would
produce wrong topologies.

Instead, define a new concept for producers that emit complete fragments:

```cpp
using dag_callback = std::function<void(phylo_dag)>;

template <typename F>
concept FragmentProducer =
    requires(F& f, phylo_dag& tree, dag_callback cb, std::mt19937& rng) {
      { f(tree, cb, rng) };
    };
```

The `inplace_move_producer` satisfies `FragmentProducer`:
```cpp
void operator()(phylo_dag& tree, dag_callback frag_cb, std::mt19937& rng);
```

It handles extraction internally: clone `work_tree`, run Fitch, and call
`frag_cb` directly. The caller merges the received fragments without
needing `apply_spr_as_fragment`.

Extend `optimize_dag_v2` to accept both `MoveProducer`s and
`FragmentProducer`s. `MoveProducer`s go through the existing collect-then-
fragment path; `FragmentProducer`s call `m.add_dag()` on each emitted
fragment directly.

**Files**: `include/larch/inplace_spr.hpp`, `include/larch/spr_pipeline.hpp`

**Tests**:
- `inplace_move_producer` produces fragments via `frag_cb`. Each fragment is
  a valid tree with the correct leaf set.
- Fragments are mergeable: `merge.add_dag(fragment)` succeeds.
- DAG grows after merging fragments from a multi-step run.

---

### Phase 34: Configurable step count and radius ✓ (session 33)

Add parameters to control the multi-step loop behavior:

- `max_steps`: maximum number of in-place moves per iteration (default: 10).
- `radius`: SPR search radius (default: auto = 2 × depth).
- `max_moves_per_step`: limit on move candidates evaluated per step
  (default: 100).
- `worsening_budget`: maximum cumulative parsimony worsening allowed
  before aborting the loop (default: unlimited).
- `improvement_target`: stop early if score improved by this much
  (default: 0 = no early stop on improvement).

```cpp
struct inplace_params {
  std::size_t max_steps{10};
  std::size_t radius{0};
  std::size_t max_moves_per_step{100};
  std::optional<int> worsening_budget;
  std::optional<int> improvement_target;
  move_selector selector{best_improving};
  fragment_strategy frag_mode{fragment_strategy::final_only};  // every_step or final_only
};
```

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- `max_steps = 1`: loop runs exactly one step.
- `worsening_budget = 5`: loop aborts when cumulative worsening exceeds 5.
- `improvement_target = -3`: loop stops early when score improves by 3+.

---

### Phase 35: Escape detection ✓ (session 33)

The loop should detect when it has found a genuine escape from the local
minimum:

```cpp
bool escaped = (state.parsimony_score < initial_parsimony_score);
```

If escaped, extract the current tree as a high-priority fragment. If not
escaped after `max_steps`, the loop was unsuccessful — optionally still
extract the final tree (it adds diversity to the DAG even if not improving).

Log the escape: step count, initial score, final score, delta.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- On a tree at a known local minimum with a 2-step escape path: verify
  `escaped == true` after the loop.
- On a tree already at the global optimum: verify `escaped == false`.

---

### Phase 36: Integration with `optimize_dag_v2` ✓ (session 33)

Add `inplace_move_producer` as an optional additional producer in the pipeline:

```cpp
// Example usage:
native_move_producer native{.max_moves = 50};
inplace_move_producer inplace{.params = {.max_steps = 10}};
auto results = optimize_dag_v2(m, num_iters, seed, native, inplace);
```

The `optimize_dag_v2` template already supports variadic producers. The
inplace producer runs after the native producer on the same sampled tree.

If the native producer found improving moves, the inplace producer may be
skipped (no need to escape — not stuck). Add a flag:
```cpp
bool only_when_stuck{true};  // skip if native found improving moves
```

Detecting "stuck": the native producer's callback was not invoked (no
improving moves found).

**Files**: `include/larch/spr_pipeline.hpp`

**Tests**:
- With `only_when_stuck = true`: when native finds moves, inplace doesn't
  run. When native finds nothing, inplace runs.
- Full pipeline: load 5 test trees, merge, run `optimize_dag_v2` with both
  producers for 5 iterations. Verify DAG grows and parsimony doesn't worsen.

---

## Part 8 — Acceptance Policies (Phases 37–38)

### Phase 37: Bounded worsening (greedy with slack) ✓ (session 33, via accept_threshold + worsening_budget)

The simplest policy: accept any move with `score_change <= threshold`.

- `threshold = -1` (default): only strictly improving moves. This is the
  current behavior and won't escape minima.
- `threshold = 0`: neutral + improving. Same as current drift.
- `threshold = +2`: allow up to 2-mutation worsening per step. Enables
  escaping shallow minima.

The `worsening_budget` parameter (Phase 34) caps cumulative worsening,
preventing unbounded degradation.

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- threshold=-1: only improving moves accepted.
- threshold=0: neutral moves accepted, worsening rejected.
- threshold=+2: moves with delta ≤ +2 accepted.
- worsening_budget=3 with threshold=+2: loop stops after cumulative delta
  exceeds +3, even if individual moves are within threshold.

---

### Phase 38: Simulated annealing schedule ✓ (session 33, via random_weighted selector + temperature/cooling_rate)

For more sophisticated exploration, accept worsening moves with probability
`exp(-delta / T)`, where T decreases over steps:

```cpp
struct annealing_selector {
  double initial_temperature;
  double cooling_rate;  // T *= cooling_rate each step

  profitable_move const& operator()(
      std::vector<profitable_move> const& moves,
      std::mt19937& rng, std::size_t step) {
    double T = initial_temperature * std::pow(cooling_rate, step);
    // Compute acceptance probabilities
    std::vector<double> weights(moves.size());
    for (std::size_t i = 0; i < moves.size(); i++) {
      if (moves[i].score_change <= 0)
        weights[i] = 1.0;  // always accept improving
      else
        weights[i] = std::exp(-moves[i].score_change / T);
    }
    std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
    return moves[dist(rng)];
  }
};
```

**Files**: `include/larch/inplace_spr.hpp`

**Tests**:
- High temperature: worsening moves accepted frequently. Over 1000 trials,
  acceptance rate for delta=+1 should be > 50%.
- Low temperature: worsening moves rarely accepted. Acceptance rate for
  delta=+1 should be < 5%.
- Zero temperature: only improving moves accepted (degenerate to greedy).
- Cooling: acceptance rate decreases monotonically with step number.

---

## Part 9 — CLI Integration and End-to-End (Phases 39–42)

### Phase 39: CLI flags ✓ (session 33)

Add command-line options to `larch2.cpp`:

```
--inplace-steps N       Number of in-place SPR steps per iteration (default: 0 = disabled)
--inplace-threshold T   Accept moves with score_change <= T (default: 0)
--inplace-budget B      Maximum cumulative worsening allowed (default: unlimited)
--inplace-temperature F Simulated annealing initial temperature (default: 0 = disabled)
--inplace-cooling F     Cooling rate (default: 0.9)
--inplace-fragments S   Fragment strategy: every|final (default: final)
```

When `--inplace-steps > 0`, add an `inplace_move_producer` to the pipeline
alongside the native producer. Wire through `run_native` and/or replace
the existing `drift_escape` mechanism.

**Files**: `tools/larch2.cpp`, `CMakeLists.txt`

**Tests**:
- `--inplace-steps 5`: runs without crash, produces valid output.
- `--inplace-steps 0`: no change from baseline behavior.
- Argument validation: `--inplace-steps -1` → error.
- `--inplace-temperature 2.0 --inplace-cooling 0.95`: annealing mode activates.

---

### Phase 40: Replace drift_escape with inplace loop ✓ (session 33)

The existing `drift_escape()` in `larch2.cpp:1080-1206` does essentially
what the inplace loop does, but inefficiently (full tree_index rebuild + clone
per step). Replace it:

```cpp
static bool drift_escape(merge& m, args const& a, std::mt19937& rng,
                          progress& prog) {
  if (!a.inplace_steps || *a.inplace_steps == 0) {
    return drift_escape_legacy(m, a, rng, prog);  // old behavior
  }
  // New: use inplace loop
  auto& dag = m.get_result();
  auto tree = sample_min_weight_tree(dag, ...);
  inplace_params params{
    .max_steps = *a.inplace_steps,
    .accept_threshold = a.inplace_threshold.value_or(0),
    ...
  };
  inplace_move_producer producer{params};
  // Run and merge fragments
  ...
}
```

**Files**: `tools/larch2.cpp`

**Tests**:
- Existing drift tests still pass with legacy path.
- With `--inplace-steps 10 --drift 5`: drift uses inplace loop, escapes
  known local minimum faster than legacy drift.
- Timing comparison: inplace drift < legacy drift wall-clock time on
  `data/test_5_trees`.

---

### Phase 41: End-to-end local minimum escape ✓ (session 33)

Construct a test case where the optimizer is provably stuck at a local
minimum with no single improving SPR move, but a 2-step path exists to a
better tree.

**Construction**: Build a tree where:
1. Leaf sequences are designed so that the optimal tree requires topology X.
2. The initial tree has topology Y, which differs from X by exactly 2 SPR
   moves.
3. Every single SPR from Y either worsens parsimony or is neutral.
4. The 2-step path Y → Z → X improves parsimony (Z is worse than Y, but X
   is better than both).

This requires careful sequence design. **Note**: 4 leaves are insufficient —
with only 3 unrooted topologies, any topology reaches any other in a single
SPR move, so no non-global local minimum exists under single SPR.

Use **6 leaves** with carefully designed sequences so that the suboptimal
topology is a local minimum under single SPR. The exact sequences should be
determined by exhaustive enumeration: for each candidate suboptimal topology,
verify that all possible single SPR moves produce equal or worse parsimony
scores. This verification can be done programmatically using `find_all_moves`
on the candidate tree.

Construction approach:
1. Pick a 6-leaf optimal topology and compute its parsimony cost.
2. Pick a topology that differs by exactly 2 SPR moves.
3. Design sequences such that every single SPR from topology 2 is neutral
   or worsening.
4. **Verify by exhaustive search** (there are O(N²) possible SPR moves for
   6 leaves — small enough to check all).

Verify:
- With `--inplace-steps 0`: optimizer stays stuck after many iterations.
- With `--inplace-steps 3 --inplace-threshold 1`: optimizer escapes within
  a few iterations.

**Files**: `test/inplace_spr_test.cpp`

**Tests**:
- Verify the constructed tree is at a local minimum (no single improving move).
- Verify the 2-step escape path exists (manual SPR sequence improves score).
- Run `inplace_move_producer` with `max_steps=3`. Verify it finds the escape.
- Run 10 times with different seeds. Verify escape found in ≥ 8/10 runs.

---

### Phase 42: Performance validation ✓ (session 33)

Benchmark incremental vs full tree_index rebuild on realistic data.

**Test protocol**:
1. Load `data/test_5_trees/tree_0.pb.gz`, sample a tree.
2. Enumerate 20 profitable moves.
3. For each move:
   a. Time full tree_index construction (baseline).
   b. Time incremental topology + Fitch update.
   c. Verify results are identical.
4. Report speedup ratio.

Expected: incremental update should be at least 10× faster for these small
trees, and much more for larger trees (the ratio scales with N/path_length).

**Files**: `test/inplace_spr_test.cpp`

**Tests**:
- Incremental update produces same Fitch arrays as full rebuild (correctness).
- Incremental update is measurably faster (≥5× on test data).
- No memory leaks: run 100 sequential in-place moves; check that
  `node_high_mark()` doesn't grow unboundedly (holes are reused or growth
  is bounded).

---

## Known Limitations

**Move enumeration dominates per-step cost.** Each step calls
`find_all_moves_parallel`, which is O(searchable_nodes × radius_subtree_size
× sites). For N=100K and radius=40, this is expensive. The plan saves
O(path × sites) on Fitch scoring via incremental updates, but the
enumeration cost is unchanged. For K=10 steps, the total enumeration cost is
O(K × N × radius × sites), which may dominate the Fitch savings. Future work
could explore incremental move pruning — only re-searching nodes near the
affected paths — but this is out of scope for v1.

**Memory growth is unbounded across steps.** Each in-place SPR appends a new
inner node (incrementing `node_high_mark()`) and may collapse one (creating a
hole below `high_mark`). Holes are not reclaimed by `append_node`. After K
steps, `high_mark` grows by K. For K=10 this is trivial (~10 extra slots).
For long-running scenarios (K=1000+), this could waste memory. Phase 42 tests
verify bounded growth for practical K values. Compaction (renumbering nodes to
close holes) is a possible future optimization.

---

## Appendix: Test Infrastructure

### New test file: `test/inplace_spr_test.cpp`

```
CMakeLists.txt addition:
  foreach(test_name ... inplace_spr_test)
      add_executable(${test_name} test/${test_name}.cpp)
      target_link_libraries(${test_name} PRIVATE larch)
      add_test(NAME ${test_name} COMMAND ${test_name}
               WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
```

The test file follows the project's existing pattern:
- `main()` calls individual test functions.
- Uses `cassert` for validation.
- Uses `std::println` for progress output.
- Test helpers: `cg_from_sequence`, `add_edge`, `verify_is_tree`,
  `get_leaf_ids` (copied from existing test files or factored into a shared
  header).

### Test data requirements

- Existing fixtures (`data/test_5_trees/`) sufficient for most tests.
- Phase 41 requires a hand-crafted local minimum tree (built in code, not a
  fixture file).
- No new protobuf fixtures needed.

### Test execution order

Tests should be runnable independently but are logically ordered by phase:
```
test_spr_result_struct()           // Phase 1
test_tree_state_construction()     // Phase 2
test_node_validity_tracking()      // Phase 3
test_detach_source()               // Phase 4
test_collapse_binary()             // Phase 5
test_reattach_destination()        // Phase 6
test_apply_spr_inplace()           // Phase 7
test_inplace_vs_clone()            // Phase 8
test_edge_cases()                  // Phase 9
test_topology_update()             // Phase 10-12
test_subtree_size_update()         // Phase 13
test_dfs_retraverse()              // Phase 14
test_root_change()                 // Phase 15
test_condensed_recheck()           // Phase 16
test_single_node_fitch()           // Phase 17
test_upward_propagation()          // Phase 18
test_new_node_fitch()              // Phase 19
test_removal_path_fitch()          // Phase 20
test_insertion_path_fitch()        // Phase 21
test_combined_fitch()              // Phase 22
test_fitch_vs_full_rebuild()       // Phase 23
test_parsimony_from_index()        // Phase 24
test_incremental_delta()           // Phase 25
test_running_score()               // Phase 26
test_fragment_extraction()         // Phase 27
test_deferred_extraction()         // Phase 28
test_clade_offset_rebuild()        // Phase 29
test_move_producer_concept()       // Phase 30
test_core_loop()                   // Phase 31
test_move_selection()              // Phase 32
test_fragment_integration()        // Phase 33
test_configurable_params()         // Phase 34
test_escape_detection()            // Phase 35
test_pipeline_integration()        // Phase 36
test_bounded_worsening()           // Phase 37
test_simulated_annealing()         // Phase 38
test_local_minimum_escape()        // Phase 41
test_performance()                 // Phase 42
```

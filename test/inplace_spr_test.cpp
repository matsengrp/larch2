#include <larch/inplace_spr.hpp>
#include <larch/load_proto_dag.hpp>

#include <cassert>
#include <map>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace larch;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static compact_genome cg_from_sequence(std::string_view seq,
                                       std::string_view ref) {
  std::map<mutation_position, nuc_base> muts;
  for (std::size_t i = 0; i < seq.size(); ++i) {
    if (seq[i] != ref[i]) muts[i + 1] = nuc_base::from_char(seq[i]);
  }
  return compact_genome{std::move(muts)};
}

static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx, std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
}

// Build a 6-leaf suboptimal tree (same as native_optimize_test).
//       root
//      /    \
//    i1      i2
//   / \     / \
//  i3  L4  L3  i4
//  /\         /\
// L1 L2     L5  L6
static phylo_dag make_suboptimal_tree() {
  constexpr std::string_view ref = "AAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from_sequence("TAAA", ref);
  l1.sample_id() = l1.cg().to_string();
  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from_sequence("TAGA", ref);
  l2.sample_id() = l2.cg().to_string();
  auto l3 = d.append_node<node_kind::leaf>();
  l3.cg() = cg_from_sequence("TAAG", ref);
  l3.sample_id() = l3.cg().to_string();
  auto l4 = d.append_node<node_kind::leaf>();
  l4.cg() = cg_from_sequence("ACAA", ref);
  l4.sample_id() = l4.cg().to_string();
  auto l5 = d.append_node<node_kind::leaf>();
  l5.cg() = cg_from_sequence("ACGA", ref);
  l5.sample_id() = l5.cg().to_string();
  auto l6 = d.append_node<node_kind::leaf>();
  l6.cg() = cg_from_sequence("ACAG", ref);
  l6.sample_id() = l6.cg().to_string();

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);
  auto i1 = d.append_node<node_kind::inner>();
  i1.cg() = cg_from_sequence("AAAA", ref);
  auto i2 = d.append_node<node_kind::inner>();
  i2.cg() = cg_from_sequence("AAAA", ref);
  auto i3 = d.append_node<node_kind::inner>();
  i3.cg() = cg_from_sequence("TAAA", ref);
  auto i4 = d.append_node<node_kind::inner>();
  i4.cg() = cg_from_sequence("ACAA", ref);

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), i1.index(), 0);
  add_edge(d, root.index(), i2.index(), 1);
  add_edge(d, i1.index(), i3.index(), 0);
  add_edge(d, i1.index(), l4.index(), 1);
  add_edge(d, i3.index(), l1.index(), 0);
  add_edge(d, i3.index(), l2.index(), 1);
  add_edge(d, i2.index(), l3.index(), 0);
  add_edge(d, i2.index(), i4.index(), 1);
  add_edge(d, i4.index(), l5.index(), 0);
  add_edge(d, i4.index(), l6.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// Build a small 3-leaf tree: UA -> R -> {A, B, C}
static phylo_dag make_simple_tree() {
  constexpr std::string_view ref = "AAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto la = d.append_node<node_kind::leaf>();
  la.cg() = cg_from_sequence("TAA", ref);
  la.sample_id() = la.cg().to_string();
  auto lb = d.append_node<node_kind::leaf>();
  lb.cg() = cg_from_sequence("ATA", ref);
  lb.sample_id() = lb.cg().to_string();
  auto lc = d.append_node<node_kind::leaf>();
  lc.cg() = cg_from_sequence("AAT", ref);
  lc.sample_id() = lc.cg().to_string();

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAA", ref);

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), la.index(), 0);
  add_edge(d, root.index(), lb.index(), 1);
  add_edge(d, root.index(), lc.index(), 2);

  recompute_edge_mutations(d);
  return d;
}

// Compute parsimony score by summing edge mutations (ground truth).
static int ground_truth_parsimony(phylo_dag& d) {
  int total = 0;
  auto ua_idx = get_root_idx(d);
  for (auto ev : d.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          // Skip the UA->root edge: tree_index Fitch scoring does not
          // include the UA->root edge because it scores internal Fitch costs
          // only.  But for a tree, parsimony = sum of all edge mutations
          // below the root.  The UA->root edge carries the cost of the root
          // CG vs reference, which is the same as the Fitch cost at the root
          // (bits not matching reference).  So we count all edges except
          // UA edges.
          auto pidx = get_parent_idx(d, edge.index());
          if (is_ua(d, pidx)) return;
          total += static_cast<int>(edge.mutations().size());
        },
        ev);
  }
  return total;
}

// Count total edges in a DAG.
static std::size_t count_edges(phylo_dag& d) {
  std::size_t n = 0;
  for (auto ev : d.get_all_edges()) {
    (void)ev;
    ++n;
  }
  return n;
}

// Count total nodes in a DAG.
static std::size_t count_nodes(phylo_dag& d) {
  std::size_t n = 0;
  for (auto nv : d.get_all_nodes()) {
    (void)nv;
    ++n;
  }
  return n;
}

// Build a tree: UA -> R -> {P -> {A, B}, C}
// P is a binary inner node.
static phylo_dag make_binary_parent_tree() {
  constexpr std::string_view ref = "AAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto la = d.append_node<node_kind::leaf>();
  la.cg() = cg_from_sequence("TAAA", ref);
  la.sample_id() = la.cg().to_string();
  auto lb = d.append_node<node_kind::leaf>();
  lb.cg() = cg_from_sequence("ATAA", ref);
  lb.sample_id() = lb.cg().to_string();
  auto lc = d.append_node<node_kind::leaf>();
  lc.cg() = cg_from_sequence("AATA", ref);
  lc.sample_id() = lc.cg().to_string();

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);
  auto p = d.append_node<node_kind::inner>();
  p.cg() = cg_from_sequence("AAAA", ref);

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), p.index(), 0);
  add_edge(d, root.index(), lc.index(), 1);
  add_edge(d, p.index(), la.index(), 0);
  add_edge(d, p.index(), lb.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// Build a tree: UA -> R -> {P -> {A, B, C}, D}
// P is a ternary inner node (no collapse on single detach).
static phylo_dag make_ternary_parent_tree() {
  constexpr std::string_view ref = "AAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto la = d.append_node<node_kind::leaf>();
  la.cg() = cg_from_sequence("TAAA", ref);
  la.sample_id() = la.cg().to_string();
  auto lb = d.append_node<node_kind::leaf>();
  lb.cg() = cg_from_sequence("ATAA", ref);
  lb.sample_id() = lb.cg().to_string();
  auto lc = d.append_node<node_kind::leaf>();
  lc.cg() = cg_from_sequence("AATA", ref);
  lc.sample_id() = lc.cg().to_string();
  auto ld = d.append_node<node_kind::leaf>();
  ld.cg() = cg_from_sequence("AAAT", ref);
  ld.sample_id() = ld.cg().to_string();

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);
  auto p = d.append_node<node_kind::inner>();
  p.cg() = cg_from_sequence("AAAA", ref);

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), p.index(), 0);
  add_edge(d, root.index(), ld.index(), 1);
  add_edge(d, p.index(), la.index(), 0);
  add_edge(d, p.index(), lb.index(), 1);
  add_edge(d, p.index(), lc.index(), 2);

  recompute_edge_mutations(d);
  return d;
}

// Build a binary root tree: UA -> R -> {A, B}
static phylo_dag make_binary_root_tree() {
  constexpr std::string_view ref = "AAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto la = d.append_node<node_kind::leaf>();
  la.cg() = cg_from_sequence("TAAA", ref);
  la.sample_id() = la.cg().to_string();
  auto lb = d.append_node<node_kind::leaf>();
  lb.cg() = cg_from_sequence("ATAA", ref);
  lb.sample_id() = lb.cg().to_string();

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), la.index(), 0);
  add_edge(d, root.index(), lb.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// ---------------------------------------------------------------------------
// Phase 1 tests: spr_result
// ---------------------------------------------------------------------------

static void test_spr_result_construction() {
  std::println("test_spr_result_construction");

  // Test basic construction with no collapse
  spr_result r1{
      .src = 1,
      .dst = 2,
      .src_parent = 3,
      .dst_parent = 4,
      .new_inner = 5,
      .lca = 6,
      .src_parent_collapsed = false,
      .grandparent = 0,
      .remaining_child = 0,
      .collapsed_node = 0,
  };
  assert(r1.src == 1);
  assert(r1.dst == 2);
  assert(r1.src_parent == 3);
  assert(r1.dst_parent == 4);
  assert(r1.new_inner == 5);
  assert(r1.lca == 6);
  assert(!r1.src_parent_collapsed);

  // Test construction with collapse
  spr_result r2{
      .src = 10,
      .dst = 20,
      .src_parent = 30,
      .dst_parent = 40,
      .new_inner = 50,
      .lca = 60,
      .src_parent_collapsed = true,
      .grandparent = 70,
      .remaining_child = 80,
      .collapsed_node = 30,
  };
  assert(r2.src_parent_collapsed);
  assert(r2.grandparent == 70);
  assert(r2.remaining_child == 80);
  assert(r2.collapsed_node == 30);

  // Verify that when collapsed is false, collapse fields are unused
  // (they exist but should not be relied upon)
  assert(!r1.src_parent_collapsed);
  // collapsed fields are present but semantically unused — this is by design

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 2 tests: tree_state
// ---------------------------------------------------------------------------

static void test_tree_state_construction() {
  std::println("test_tree_state_construction");

  auto tree = make_suboptimal_tree();
  tree_state state{tree};

  assert(state.step_count == 0);
  assert(state.parsimony_score >= 0);

  // Verify parsimony score matches ground truth (edge mutation sum).
  int gt = ground_truth_parsimony(tree);
  std::println("  tree_state parsimony_score = {}", state.parsimony_score);
  std::println("  ground truth (edge mutations) = {}", gt);
  assert(state.parsimony_score == gt);

  std::println("  PASS");
}

static void test_tree_state_simple_tree() {
  std::println("test_tree_state_simple_tree");

  auto tree = make_simple_tree();
  tree_state state{tree};

  assert(state.step_count == 0);

  int gt = ground_truth_parsimony(tree);
  std::println("  parsimony_score = {}, ground truth = {}", state.parsimony_score,
               gt);
  assert(state.parsimony_score == gt);

  // Simple tree with 3 leaves, each differing at one site from ref.
  // R has 3 children: no allele is shared by all 3 at any site,
  // so Fitch cost = 3 (one per variable site).
  assert(state.parsimony_score == 3);

  std::println("  PASS");
}

static void test_tree_state_with_pool() {
  std::println("test_tree_state_with_pool");

  auto tree = make_suboptimal_tree();
  auto& pool = thread_pool::get_default();
  tree_state state{tree, pool};

  assert(state.step_count == 0);

  int gt = ground_truth_parsimony(tree);
  assert(state.parsimony_score == gt);

  std::println("  parsimony_score = {}", state.parsimony_score);
  std::println("  PASS");
}

static void test_tree_state_zero_parsimony() {
  std::println("test_tree_state_zero_parsimony");

  // All leaves identical to reference — zero parsimony.
  constexpr std::string_view ref = "AAA";
  phylo_dag d;
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from_sequence("AAA", ref);
  l1.sample_id() = "leaf1";
  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from_sequence("AAA", ref);
  l2.sample_id() = "leaf2";

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAA", ref);

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), l1.index(), 0);
  add_edge(d, root.index(), l2.index(), 1);

  recompute_edge_mutations(d);

  tree_state state{d};
  std::println("  parsimony_score = {}", state.parsimony_score);
  assert(state.parsimony_score == 0);
  assert(state.step_count == 0);

  std::println("  PASS");
}

static void test_tree_state_single_leaf() {
  std::println("test_tree_state_single_leaf");

  // Degenerate tree: UA -> root (leaf).
  constexpr std::string_view ref = "ACG";
  phylo_dag d;
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto leaf = d.append_node<node_kind::leaf>();
  leaf.cg() = cg_from_sequence("ACG", ref);
  leaf.sample_id() = "only_leaf";

  add_edge(d, ua.index(), leaf.index(), 0);

  recompute_edge_mutations(d);

  tree_state state{d};
  std::println("  parsimony_score = {}", state.parsimony_score);
  // Single leaf — no inner nodes, no Fitch cost.
  assert(state.parsimony_score == 0);
  assert(state.step_count == 0);

  std::println("  PASS");
}

static void test_tree_state_no_variable_sites() {
  std::println("test_tree_state_no_variable_sites");

  // Two leaves identical to ref — no variable sites at all.
  constexpr std::string_view ref = "GG";
  phylo_dag d;
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from_sequence("GG", ref);
  l1.sample_id() = "leaf1";
  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from_sequence("GG", ref);
  l2.sample_id() = "leaf2";

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("GG", ref);

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), l1.index(), 0);
  add_edge(d, root.index(), l2.index(), 1);

  recompute_edge_mutations(d);

  tree_index idx{d};
  std::println("  variable_sites = {}", idx.num_variable_sites());
  assert(idx.num_variable_sites() == 0);

  tree_state state{d};
  std::println("  parsimony_score = {}", state.parsimony_score);
  assert(state.parsimony_score == 0);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 3 tests: is_valid_
// ---------------------------------------------------------------------------

static void test_is_valid_initialization() {
  std::println("test_is_valid_initialization");

  auto tree = make_suboptimal_tree();
  tree_index idx{tree};

  auto ua_idx = get_root_idx(tree);

  // All tree nodes should be valid
  auto tree_root = idx.get_tree_root();
  assert(idx.is_valid(tree_root));

  for (auto node : idx.get_searchable_nodes()) {
    assert(idx.is_valid(node));
  }

  // UA node should not be valid (it's skipped in init)
  assert(!idx.is_valid(ua_idx));

  std::println("  PASS");
}

static void test_is_valid_all_tree_nodes() {
  std::println("test_is_valid_all_tree_nodes");

  auto tree = make_suboptimal_tree();
  tree_index idx{tree};

  auto ua_idx = get_root_idx(tree);

  // Check every node in the tree
  std::size_t valid_count = 0;
  std::size_t invalid_count = 0;
  for (auto nv : tree.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          auto nid = node.index();
          if (is_ua(tree, nid)) {
            assert(!idx.is_valid(nid));
            invalid_count++;
          } else {
            assert(idx.is_valid(nid));
            valid_count++;
          }
        },
        nv);
  }

  // 6 leaves + 4 inner + 1 root = 11 valid, 1 UA = invalid
  std::println("  valid: {}, invalid: {}", valid_count, invalid_count);
  assert(valid_count == 11);
  assert(invalid_count == 1);

  std::println("  PASS");
}

static void test_is_valid_out_of_range() {
  std::println("test_is_valid_out_of_range");

  auto tree = make_simple_tree();
  tree_index idx{tree};

  // Out-of-range indices should return false
  assert(!idx.is_valid(999999));
  assert(!idx.is_valid(static_cast<std::size_t>(-1)));

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 4 tests: detach_source
// ---------------------------------------------------------------------------

static void test_detach_from_ternary_parent() {
  std::println("test_detach_from_ternary_parent");

  // Tree: UA -> R -> {A, B, C}.  Detach A from R (ternary parent).
  auto d = make_simple_tree();
  auto ua_idx = get_root_idx(d);

  // Find R (child of UA) and its children
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);

  // R has 3 clades, each with 1 child: A, B, C
  auto a_idx = get_child_idx(d, root_clades[0][0]);

  auto orig_edge_count = count_edges(d);
  auto orig_node_count = count_nodes(d);

  auto old_cc = detach_source(d, a_idx, root_idx);

  // A had parent R, R had 3 children
  assert(old_cc == 3);

  // A now has 0 parent edges
  auto a_pes = get_parent_edges(d, a_idx);
  assert(a_pes.empty());

  // R now has 2 children
  assert(child_count(d, root_idx) == 2);

  // Edge count decreased by 1
  assert(count_edges(d) == orig_edge_count - 1);

  // Node count unchanged
  assert(count_nodes(d) == orig_node_count);

  std::println("  PASS");
}

static void test_detach_from_binary_parent() {
  std::println("test_detach_from_binary_parent");

  // Tree: UA -> R -> {P -> {A, B}, C}.  Detach A from P (binary).
  auto d = make_binary_parent_tree();
  auto ua_idx = get_root_idx(d);

  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto p_idx = get_child_idx(d, root_clades[0][0]);
  auto p_clades = get_clades(d, p_idx);
  auto a_idx = get_child_idx(d, p_clades[0][0]);

  auto orig_edge_count = count_edges(d);
  auto orig_node_count = count_nodes(d);

  auto old_cc = detach_source(d, a_idx, p_idx);

  // P had 2 children
  assert(old_cc == 2);

  // A now has 0 parent edges
  auto a_pes = get_parent_edges(d, a_idx);
  assert(a_pes.empty());

  // P now has 1 child
  assert(child_count(d, p_idx) == 1);

  // Edge count decreased by 1
  assert(count_edges(d) == orig_edge_count - 1);

  // Node count unchanged
  assert(count_nodes(d) == orig_node_count);

  std::println("  PASS");
}

static void test_detach_node_count_unchanged() {
  std::println("test_detach_node_count_unchanged");

  // Detach from the suboptimal tree (larger tree)
  auto d = make_suboptimal_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto i1_idx = get_child_idx(d, root_clades[0][0]);
  auto i1_clades = get_clades(d, i1_idx);
  auto i3_idx = get_child_idx(d, i1_clades[0][0]);
  auto i3_clades = get_clades(d, i3_idx);
  auto l1_idx = get_child_idx(d, i3_clades[0][0]);

  auto orig_node_count = count_nodes(d);
  detach_source(d, l1_idx, i3_idx);
  assert(count_nodes(d) == orig_node_count);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 5 tests: collapse_binary_parent
// ---------------------------------------------------------------------------

static void test_collapse_binary_inner() {
  std::println("test_collapse_binary_inner");

  // Tree: UA -> R -> {P -> {A, B}, C}.  Detach A from P, then collapse P.
  auto d = make_binary_parent_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto p_idx = get_child_idx(d, root_clades[0][0]);
  auto c_idx = get_child_idx(d, root_clades[1][0]);
  auto p_clades = get_clades(d, p_idx);
  auto a_idx = get_child_idx(d, p_clades[0][0]);
  auto b_idx = get_child_idx(d, p_clades[1][0]);

  // Record the clade index of the R->P edge for later verification
  auto rp_clade = get_clade_idx(d, root_clades[0][0]);

  auto orig_node_count = count_nodes(d);

  // Phase 4: detach A from P
  auto old_cc = detach_source(d, a_idx, p_idx);
  assert(old_cc == 2);

  // Phase 5: collapse P (binary parent)
  std::size_t grandparent = 0, remaining_child = 0, collapsed_node = 0;
  bool collapsed =
      collapse_binary_parent(d, p_idx, old_cc, grandparent, remaining_child,
                             collapsed_node);

  assert(collapsed);
  assert(grandparent == root_idx);
  assert(remaining_child == b_idx);
  assert(collapsed_node == p_idx);

  // P is removed: node count decreased by 1
  assert(count_nodes(d) == orig_node_count - 1);

  // B is now a direct child of R
  auto b_pes = get_parent_edges(d, b_idx);
  assert(!b_pes.empty());
  assert(get_parent_idx(d, b_pes[0]) == root_idx);

  // The clade index on the R->B edge matches the old R->P edge
  assert(get_clade_idx(d, b_pes[0]) == rp_clade);

  // R now has 2 children: B and C
  assert(child_count(d, root_idx) == 2);

  // Verify the children of R are B and C
  std::set<std::size_t> r_children;
  auto nv = d.get_node(root_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          std::visit(
              [&](auto edge) {
                auto cv = edge.get_child();
                r_children.insert(
                    std::visit([](auto c) { return c.index(); }, cv));
              },
              ev);
        }
      },
      nv);
  assert(r_children.count(b_idx) == 1);
  assert(r_children.count(c_idx) == 1);

  std::println("  PASS");
}

static void test_collapse_binary_with_ternary_grandparent() {
  std::println("test_collapse_binary_with_ternary_grandparent");

  // Tree: UA -> R -> {P -> {A, B}, C, D}.  Detach A from P, collapse P.
  // Same as above but R has 3 children initially.
  auto d = make_ternary_parent_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);

  // P is the first child of R, it has children A, B, C
  // But we only detach one child from P and need P to be binary.
  // Let's build a specific tree for this case.
  // Actually, make_ternary_parent_tree has P with 3 children {A,B,C}.
  // We want P binary. Let me use make_binary_parent_tree but add D to R.

  // Re-do: build UA -> R -> {P->{A,B}, C, D}
  constexpr std::string_view ref = "AAAA";
  phylo_dag d2;
  auto ua2 = d2.append_node<node_kind::ua>();
  ua2.reference_sequence() = std::string{ref};
  d2.set_root(ua2);

  auto la = d2.append_node<node_kind::leaf>();
  la.cg() = cg_from_sequence("TAAA", ref);
  la.sample_id() = la.cg().to_string();
  auto lb = d2.append_node<node_kind::leaf>();
  lb.cg() = cg_from_sequence("ATAA", ref);
  lb.sample_id() = lb.cg().to_string();
  auto lc = d2.append_node<node_kind::leaf>();
  lc.cg() = cg_from_sequence("AATA", ref);
  lc.sample_id() = lc.cg().to_string();
  auto ld = d2.append_node<node_kind::leaf>();
  ld.cg() = cg_from_sequence("AAAT", ref);
  ld.sample_id() = ld.cg().to_string();

  auto root2 = d2.append_node<node_kind::inner>();
  root2.cg() = cg_from_sequence("AAAA", ref);
  auto p2 = d2.append_node<node_kind::inner>();
  p2.cg() = cg_from_sequence("AAAA", ref);

  add_edge(d2, ua2.index(), root2.index(), 0);
  add_edge(d2, root2.index(), p2.index(), 0);
  add_edge(d2, root2.index(), lc.index(), 1);
  add_edge(d2, root2.index(), ld.index(), 2);
  add_edge(d2, p2.index(), la.index(), 0);
  add_edge(d2, p2.index(), lb.index(), 1);

  recompute_edge_mutations(d2);

  auto p2_idx = p2.index();
  auto a_idx = la.index();
  auto b_idx = lb.index();
  auto root2_idx = root2.index();

  auto orig_node_count = count_nodes(d2);

  // Detach A from P (binary)
  auto old_cc = detach_source(d2, a_idx, p2_idx);
  assert(old_cc == 2);

  // Collapse P
  std::size_t gp = 0, rc = 0, cn = 0;
  bool collapsed = collapse_binary_parent(d2, p2_idx, old_cc, gp, rc, cn);

  assert(collapsed);
  assert(gp == root2_idx);
  assert(rc == b_idx);
  assert(cn == p2_idx);

  // P removed
  assert(count_nodes(d2) == orig_node_count - 1);

  // B is direct child of R
  auto b_pes = get_parent_edges(d2, b_idx);
  assert(get_parent_idx(d2, b_pes[0]) == root2_idx);

  // R now has 3 children: B, C, D
  assert(child_count(d2, root2_idx) == 3);

  std::println("  PASS");
}

static void test_no_collapse_ternary_parent() {
  std::println("test_no_collapse_ternary_parent");

  // Tree: UA -> R -> {P -> {A, B, C}, D}.  Detach A from P (ternary).
  // No collapse should happen.
  auto d = make_ternary_parent_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto p_idx = get_child_idx(d, root_clades[0][0]);
  auto p_clades = get_clades(d, p_idx);
  auto a_idx = get_child_idx(d, p_clades[0][0]);

  auto orig_node_count = count_nodes(d);

  // Detach A from P (ternary)
  auto old_cc = detach_source(d, a_idx, p_idx);
  assert(old_cc == 3);

  // Try collapse — should not happen
  std::size_t gp = 0, rc = 0, cn = 0;
  bool collapsed = collapse_binary_parent(d, p_idx, old_cc, gp, rc, cn);

  assert(!collapsed);

  // Node count unchanged (no collapse)
  assert(count_nodes(d) == orig_node_count);

  // P still has 2 children
  assert(child_count(d, p_idx) == 2);

  std::println("  PASS");
}

static void test_collapse_binary_root() {
  std::println("test_collapse_binary_root");

  // Tree: UA -> R -> {A, B}.  Detach A from R (binary root).
  // R should collapse, B becomes direct child of UA.
  auto d = make_binary_root_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);

  auto orig_node_count = count_nodes(d);

  // Detach A from R
  auto old_cc = detach_source(d, a_idx, root_idx);
  assert(old_cc == 2);

  // Collapse R (binary root)
  std::size_t gp = 0, rc = 0, cn = 0;
  bool collapsed = collapse_binary_parent(d, root_idx, old_cc, gp, rc, cn);

  assert(collapsed);
  assert(gp == ua_idx);
  assert(rc == b_idx);
  assert(cn == root_idx);

  // R is removed
  assert(count_nodes(d) == orig_node_count - 1);

  // B is now direct child of UA
  auto b_pes = get_parent_edges(d, b_idx);
  assert(!b_pes.empty());
  assert(get_parent_idx(d, b_pes[0]) == ua_idx);

  std::println("  PASS");
}

static void test_detach_and_collapse_suboptimal_tree() {
  std::println("test_detach_and_collapse_suboptimal_tree");

  // Use the 6-leaf suboptimal tree:
  //       root
  //      /    \          (root is binary)
  //    i1      i2
  //   / \     / \
  //  i3  L4  L3  i4
  //  /\         /\
  // L1 L2     L5  L6
  //
  // Detach L1 from i3 (binary). i3 should collapse, L2 becomes child of i1.
  auto d = make_suboptimal_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto i1_idx = get_child_idx(d, root_clades[0][0]);
  auto i1_clades = get_clades(d, i1_idx);
  auto i3_idx = get_child_idx(d, i1_clades[0][0]);
  auto l4_idx = get_child_idx(d, i1_clades[1][0]);
  auto i3_clades = get_clades(d, i3_idx);
  auto l1_idx = get_child_idx(d, i3_clades[0][0]);
  auto l2_idx = get_child_idx(d, i3_clades[1][0]);

  auto orig_node_count = count_nodes(d);

  // Detach L1 from i3
  auto old_cc = detach_source(d, l1_idx, i3_idx);
  assert(old_cc == 2);

  // Collapse i3
  std::size_t gp = 0, rc = 0, cn = 0;
  bool collapsed = collapse_binary_parent(d, i3_idx, old_cc, gp, rc, cn);

  assert(collapsed);
  assert(gp == i1_idx);
  assert(rc == l2_idx);
  assert(cn == i3_idx);

  // i3 removed
  assert(count_nodes(d) == orig_node_count - 1);

  // L2 is child of i1
  auto l2_pes = get_parent_edges(d, l2_idx);
  assert(get_parent_idx(d, l2_pes[0]) == i1_idx);

  // i1 still has 2 children: L2 and L4
  assert(child_count(d, i1_idx) == 2);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // Phase 1: spr_result
  test_spr_result_construction();

  // Phase 2: tree_state
  test_tree_state_construction();
  test_tree_state_simple_tree();
  test_tree_state_with_pool();
  test_tree_state_zero_parsimony();
  test_tree_state_single_leaf();
  test_tree_state_no_variable_sites();

  // Phase 3: is_valid_
  test_is_valid_initialization();
  test_is_valid_all_tree_nodes();
  test_is_valid_out_of_range();

  // Phase 4: detach_source
  test_detach_from_ternary_parent();
  test_detach_from_binary_parent();
  test_detach_node_count_unchanged();

  // Phase 5: collapse_binary_parent
  test_collapse_binary_inner();
  test_collapse_binary_with_ternary_grandparent();
  test_no_collapse_ternary_parent();
  test_collapse_binary_root();
  test_detach_and_collapse_suboptimal_tree();

  std::println("All inplace SPR phase 1-5 tests passed!");
  return 0;
}

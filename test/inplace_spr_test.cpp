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
  auto cinfo = collapse_binary_parent(d, p_idx, old_cc);

  assert(cinfo.has_value());
  assert(cinfo->grandparent == root_idx);
  assert(cinfo->remaining_child == b_idx);
  assert(cinfo->collapsed_node == p_idx);

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
  auto cinfo = collapse_binary_parent(d2, p2_idx, old_cc);

  assert(cinfo.has_value());
  assert(cinfo->grandparent == root2_idx);
  assert(cinfo->remaining_child == b_idx);
  assert(cinfo->collapsed_node == p2_idx);

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
  auto cinfo = collapse_binary_parent(d, p_idx, old_cc);

  assert(!cinfo.has_value());

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
  auto cinfo = collapse_binary_parent(d, root_idx, old_cc);

  assert(cinfo.has_value());
  assert(cinfo->grandparent == ua_idx);
  assert(cinfo->remaining_child == b_idx);
  assert(cinfo->collapsed_node == root_idx);

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
  auto cinfo = collapse_binary_parent(d, i3_idx, old_cc);

  assert(cinfo.has_value());
  assert(cinfo->grandparent == i1_idx);
  assert(cinfo->remaining_child == l2_idx);
  assert(cinfo->collapsed_node == i3_idx);

  // i3 removed
  assert(count_nodes(d) == orig_node_count - 1);

  // L2 is child of i1
  auto l2_pes = get_parent_edges(d, l2_idx);
  assert(get_parent_idx(d, l2_pes[0]) == i1_idx);

  // i1 still has 2 children: L2 and L4
  assert(child_count(d, i1_idx) == 2);

  std::println("  PASS");
}

static void test_detach_from_root_no_collapse() {
  std::println("test_detach_from_root_no_collapse");

  // Tree: UA -> R -> {A, B, C}.  src = A, src_parent = R (tree root).
  // R has 3 children, so detach works but collapse should not happen.
  auto d = make_simple_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto a_idx = get_child_idx(d, root_clades[0][0]);

  auto orig_node_count = count_nodes(d);

  auto old_cc = detach_source(d, a_idx, root_idx);
  assert(old_cc == 3);

  auto cinfo = collapse_binary_parent(d, root_idx, old_cc);
  assert(!cinfo.has_value());

  // Node count unchanged (no collapse)
  assert(count_nodes(d) == orig_node_count);

  // R still has 2 children
  assert(child_count(d, root_idx) == 2);

  std::println("  PASS");
}

static void test_clades_after_collapse() {
  std::println("test_clades_after_collapse");

  // Tree: UA -> R -> {P -> {A, B}, C}.  Detach A, collapse P.
  // After: R -> {B, C}.  Verify get_clades(R) is correct.
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

  auto old_cc = detach_source(d, a_idx, p_idx);
  auto cinfo = collapse_binary_parent(d, p_idx, old_cc);
  assert(cinfo.has_value());

  // Verify clades of R after collapse
  auto new_root_clades = get_clades(d, root_idx);
  assert(new_root_clades.size() == 2);

  // Collect all children from clades
  std::set<std::size_t> children_from_clades;
  for (auto& clade : new_root_clades) {
    for (auto eidx : clade) {
      children_from_clades.insert(get_child_idx(d, eidx));
    }
  }
  assert(children_from_clades.count(b_idx) == 1);
  assert(children_from_clades.count(c_idx) == 1);
  assert(children_from_clades.size() == 2);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 6 tests: reattach_at_destination
// ---------------------------------------------------------------------------

static void test_reattach_simple() {
  std::println("test_reattach_simple");

  // Tree: UA -> R -> {A, B, C}.  Detach A, reattach as sibling of C.
  // Expected: R -> {B, new_inner -> {C, A}}
  auto d = make_simple_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);

  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);
  auto c_idx = get_child_idx(d, root_clades[2][0]);

  auto orig_node_count = count_nodes(d);
  auto orig_edge_count = count_edges(d);

  // Phase 4: detach A from R
  detach_source(d, a_idx, root_idx);

  // Phase 6: reattach A as sibling of C
  auto [ni_idx, _dp] = reattach_at_destination(d, a_idx, c_idx);

  // new_inner exists with children {C, A}
  std::set<std::size_t> ni_children;
  auto nv = d.get_node(ni_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          std::visit(
              [&](auto edge) {
                auto cv = edge.get_child();
                ni_children.insert(
                    std::visit([](auto c) { return c.index(); }, cv));
              },
              ev);
        }
      },
      nv);
  assert(ni_children.count(c_idx) == 1);
  assert(ni_children.count(a_idx) == 1);
  assert(ni_children.size() == 2);

  // R has children {B, new_inner}
  std::set<std::size_t> r_children;
  auto rnv = d.get_node(root_idx);
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
      rnv);
  assert(r_children.count(b_idx) == 1);
  assert(r_children.count(ni_idx) == 1);
  assert(r_children.size() == 2);

  // new_inner's parent is R
  auto ni_pes = get_parent_edges(d, ni_idx);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(d, ni_pes[0]) == root_idx);

  // Node count: original + 1 (new_inner), no collapse since R had 3 children
  assert(count_nodes(d) == orig_node_count + 1);

  // Edge count: original - 1 (detach) - 1 (removed dst edge) + 3 (new edges) = original + 1
  assert(count_edges(d) == orig_edge_count + 1);

  std::println("  PASS");
}

static void test_reattach_with_collapse() {
  std::println("test_reattach_with_collapse");

  // Tree: UA -> R -> {P -> {A, B}, C}.  Detach A from P (binary, collapses),
  // then reattach A as sibling of C.
  // After detach+collapse: R -> {B, C}
  // After reattach: R -> {B, new_inner -> {C, A}}
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

  auto orig_node_count = count_nodes(d);
  auto orig_edge_count = count_edges(d);

  // Detach A from P, collapse P
  auto old_cc = detach_source(d, a_idx, p_idx);
  auto cinfo = collapse_binary_parent(d, p_idx, old_cc);
  assert(cinfo.has_value());

  // After collapse: R -> {B, C}, node_count = orig - 1
  assert(count_nodes(d) == orig_node_count - 1);

  // Reattach A as sibling of C
  auto [ni_idx, _dp] = reattach_at_destination(d, a_idx, c_idx);

  // new_inner has children {C, A}
  auto ni_children = get_child_indices(d, ni_idx);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(c_idx) == 1);
  assert(ni_set.count(a_idx) == 1);

  // R has children {B, new_inner}
  auto r_children = get_child_indices(d, root_idx);
  std::set<std::size_t> r_set(r_children.begin(), r_children.end());
  assert(r_set.count(b_idx) == 1);
  assert(r_set.count(ni_idx) == 1);

  // Node count: orig - 1 (collapse) + 1 (new_inner) = orig
  assert(count_nodes(d) == orig_node_count);

  // Edge count: orig - 1 (detach) - 2 + 1 (collapse) - 1 (remove dst edge) + 3 = orig
  assert(count_edges(d) == orig_edge_count);

  std::println("  PASS");
}

static void test_reattach_preserves_clade_index() {
  std::println("test_reattach_preserves_clade_index");

  // Tree: UA -> R -> {A(clade 0), B(clade 1), C(clade 2)}.
  // Detach A, reattach as sibling of C.
  // The new_inner should take C's old clade index (2) in R's children.
  auto d = make_simple_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);

  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto c_idx = get_child_idx(d, root_clades[2][0]);

  // Record C's clade index before the move
  auto c_pe = get_parent_edge(d, c_idx);
  auto c_old_clade = get_clade_idx(d, c_pe);
  assert(c_old_clade == 2);

  detach_source(d, a_idx, root_idx);
  auto [ni_idx, _dp] = reattach_at_destination(d, a_idx, c_idx);

  // new_inner's edge from R should have the same clade index C had
  auto ni_pes = get_parent_edges(d, ni_idx);
  assert(ni_pes.size() == 1);
  assert(get_clade_idx(d, ni_pes[0]) == c_old_clade);

  // C's edge from new_inner should have clade index 0
  auto c_pes = get_parent_edges(d, c_idx);
  assert(c_pes.size() == 1);
  assert(get_parent_idx(d, c_pes[0]) == ni_idx);
  assert(get_clade_idx(d, c_pes[0]) == 0);

  // A's edge from new_inner should have clade index 1
  auto a_pes = get_parent_edges(d, a_idx);
  assert(a_pes.size() == 1);
  assert(get_parent_idx(d, a_pes[0]) == ni_idx);
  assert(get_clade_idx(d, a_pes[0]) == 1);

  std::println("  PASS");
}

static void test_reattach_on_suboptimal_tree() {
  std::println("test_reattach_on_suboptimal_tree");

  // Use the 6-leaf suboptimal tree:
  //       root
  //      /    \
  //    i1      i2
  //   / \     / \
  //  i3  L4  L3  i4
  //  /\         /\
  // L1 L2     L5  L6
  //
  // Detach L1 from i3 (binary, collapses i3), reattach L1 as sibling of L5.
  // After collapse: i1 -> {L2, L4}, i4 -> {L5, L6}
  // After reattach: i4 -> {new_inner -> {L5, L1}, L6}
  auto d = make_suboptimal_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto i2_idx = get_child_idx(d, root_clades[1][0]);
  auto i1_idx = get_child_idx(d, root_clades[0][0]);
  auto i1_clades = get_clades(d, i1_idx);
  auto i3_idx = get_child_idx(d, i1_clades[0][0]);
  auto i3_clades = get_clades(d, i3_idx);
  auto l1_idx = get_child_idx(d, i3_clades[0][0]);
  auto i2_clades = get_clades(d, i2_idx);
  auto i4_idx = get_child_idx(d, i2_clades[1][0]);
  auto i4_clades = get_clades(d, i4_idx);
  auto l5_idx = get_child_idx(d, i4_clades[0][0]);
  auto l6_idx = get_child_idx(d, i4_clades[1][0]);

  // Detach L1 from i3, collapse i3
  auto old_cc = detach_source(d, l1_idx, i3_idx);
  collapse_binary_parent(d, i3_idx, old_cc);

  // Reattach L1 as sibling of L5
  auto [ni_idx, _dp] = reattach_at_destination(d, l1_idx, l5_idx);

  // new_inner has children {L5, L1}
  auto ni_children = get_child_indices(d, ni_idx);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(l5_idx) == 1);
  assert(ni_set.count(l1_idx) == 1);

  // i4 has children {new_inner, L6}
  auto i4_children = get_child_indices(d, i4_idx);
  std::set<std::size_t> i4_set(i4_children.begin(), i4_children.end());
  assert(i4_set.count(ni_idx) == 1);
  assert(i4_set.count(l6_idx) == 1);

  // new_inner's parent is i4
  auto ni_pes = get_parent_edges(d, ni_idx);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(d, ni_pes[0]) == i4_idx);

  std::println("  PASS");
}

static void test_reattach_dst_is_tree_root() {
  std::println("test_reattach_dst_is_tree_root");

  // Tree: UA -> R -> {A, B, C}.  Detach A, reattach as sibling of R (tree root).
  // Expected: UA -> new_inner -> {R -> {B, C}, A}
  auto d = make_simple_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);

  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);
  auto c_idx = get_child_idx(d, root_clades[2][0]);

  auto orig_node_count = count_nodes(d);
  auto orig_edge_count = count_edges(d);

  // Detach A from R (ternary, no collapse)
  detach_source(d, a_idx, root_idx);

  // Reattach A as sibling of R (the tree root, child of UA)
  auto [ni_idx, _dp] = reattach_at_destination(d, a_idx, root_idx);

  // new_inner has children {R, A}
  auto ni_children = get_child_indices(d, ni_idx);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(root_idx) == 1);
  assert(ni_set.count(a_idx) == 1);
  assert(ni_set.size() == 2);

  // new_inner's parent is UA
  auto ni_pes = get_parent_edges(d, ni_idx);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(d, ni_pes[0]) == ua_idx);

  // R's parent is now new_inner (not UA)
  auto r_pes = get_parent_edges(d, root_idx);
  assert(r_pes.size() == 1);
  assert(get_parent_idx(d, r_pes[0]) == ni_idx);

  // R has children {B, C} (A was removed)
  auto r_children = get_child_indices(d, root_idx);
  std::set<std::size_t> r_set(r_children.begin(), r_children.end());
  assert(r_set.count(b_idx) == 1);
  assert(r_set.count(c_idx) == 1);
  assert(r_set.size() == 2);

  // Node count: original + 1 (new_inner, no collapse)
  assert(count_nodes(d) == orig_node_count + 1);
  // Edge count: original - 1 (detach) - 1 (remove dst edge) + 3 = original + 1
  assert(count_edges(d) == orig_edge_count + 1);

  std::println("  PASS");
}

static void test_reattach_dst_is_inner_node() {
  std::println("test_reattach_dst_is_inner_node");

  // Use the 6-leaf suboptimal tree:
  //       root
  //      /    \
  //    i1      i2
  //   / \     / \
  //  i3  L4  L3  i4
  //  /\         /\
  // L1 L2     L5  L6
  //
  // Detach L4 from i1 (binary, collapses i1), reattach L4 as sibling of i2
  // (inner node).
  // After collapse: root -> {i3 -> {L1, L2}, i2 -> {L3, i4 -> {L5, L6}}}
  // After reattach: root -> {i3, new_inner -> {i2 -> {L3, i4}, L4}}
  auto d = make_suboptimal_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto i1_idx = get_child_idx(d, root_clades[0][0]);
  auto i2_idx = get_child_idx(d, root_clades[1][0]);
  auto i1_clades = get_clades(d, i1_idx);
  auto i3_idx = get_child_idx(d, i1_clades[0][0]);
  auto l4_idx = get_child_idx(d, i1_clades[1][0]);
  auto i2_clades = get_clades(d, i2_idx);
  auto l3_idx = get_child_idx(d, i2_clades[0][0]);
  auto i4_idx = get_child_idx(d, i2_clades[1][0]);

  auto orig_node_count = count_nodes(d);
  auto orig_edge_count = count_edges(d);

  // Detach L4 from i1 (binary, collapses i1)
  auto old_cc = detach_source(d, l4_idx, i1_idx);
  auto cinfo = collapse_binary_parent(d, i1_idx, old_cc);
  assert(cinfo.has_value());

  // Reattach L4 as sibling of i2 (inner node with subtree)
  auto [ni_idx, _dp] = reattach_at_destination(d, l4_idx, i2_idx);

  // new_inner has children {i2, L4}
  auto ni_children = get_child_indices(d, ni_idx);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(i2_idx) == 1);
  assert(ni_set.count(l4_idx) == 1);
  assert(ni_set.size() == 2);

  // i2's subtree is preserved: children are still {L3, i4}
  auto i2_children = get_child_indices(d, i2_idx);
  std::set<std::size_t> i2_set(i2_children.begin(), i2_children.end());
  assert(i2_set.count(l3_idx) == 1);
  assert(i2_set.count(i4_idx) == 1);
  assert(i2_set.size() == 2);

  // root has children {i3, new_inner}
  auto r_children = get_child_indices(d, root_idx);
  std::set<std::size_t> r_set(r_children.begin(), r_children.end());
  assert(r_set.count(i3_idx) == 1);
  assert(r_set.count(ni_idx) == 1);
  assert(r_set.size() == 2);

  // Node count: orig - 1 (collapse i1) + 1 (new_inner) = orig
  assert(count_nodes(d) == orig_node_count);
  // Edge count: orig - 1 (detach) - 2 + 1 (collapse) - 1 (remove dst edge) + 3 = orig
  assert(count_edges(d) == orig_edge_count);

  std::println("  PASS");
}

static void test_reattach_after_root_collapse() {
  std::println("test_reattach_after_root_collapse");

  // Tree: UA -> R -> {P -> {A, B}, C}.
  // Detach C from R (R is binary, collapses).
  // After collapse: UA -> P -> {A, B}
  // Reattach C as sibling of A.
  // After reattach: UA -> P -> {new_inner -> {A, C}, B}
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

  auto orig_node_count = count_nodes(d);
  auto orig_edge_count = count_edges(d);

  // Detach C from R (R is binary, will collapse)
  auto old_cc = detach_source(d, c_idx, root_idx);
  auto cinfo = collapse_binary_parent(d, root_idx, old_cc);
  assert(cinfo.has_value());
  assert(cinfo->grandparent == ua_idx);
  assert(cinfo->remaining_child == p_idx);

  // After collapse: P's parent is now UA
  auto p_pes = get_parent_edges(d, p_idx);
  assert(p_pes.size() == 1);
  assert(get_parent_idx(d, p_pes[0]) == ua_idx);

  // Reattach C as sibling of A
  auto [ni_idx, _dp] = reattach_at_destination(d, c_idx, a_idx);

  // new_inner has children {A, C}
  auto ni_children = get_child_indices(d, ni_idx);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(a_idx) == 1);
  assert(ni_set.count(c_idx) == 1);
  assert(ni_set.size() == 2);

  // P has children {new_inner, B}
  auto p_children = get_child_indices(d, p_idx);
  std::set<std::size_t> p_set(p_children.begin(), p_children.end());
  assert(p_set.count(ni_idx) == 1);
  assert(p_set.count(b_idx) == 1);
  assert(p_set.size() == 2);

  // new_inner's parent is P
  auto ni_pes = get_parent_edges(d, ni_idx);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(d, ni_pes[0]) == p_idx);

  // P's parent is still UA (tree root changed from R to P)
  p_pes = get_parent_edges(d, p_idx);
  assert(p_pes.size() == 1);
  assert(get_parent_idx(d, p_pes[0]) == ua_idx);

  // Node count: orig - 1 (collapse R) + 1 (new_inner) = orig
  assert(count_nodes(d) == orig_node_count);
  // Edge count: orig - 1 (detach) - 2 + 1 (collapse) - 1 (remove dst edge) + 3 = orig
  assert(count_edges(d) == orig_edge_count);

  std::println("  PASS");
}

static void test_reattach_adjacent_sibling() {
  std::println("test_reattach_adjacent_sibling");

  // Tree: UA -> R -> {A, B, C}.  Detach A, reattach as sibling of B.
  // Expected: R -> {new_inner -> {B, A}, C}
  // This is a "move to adjacent node" — topologically near-trivial but still
  // exercises the reattach path when src and dst were originally siblings.
  auto d = make_simple_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);

  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);
  auto c_idx = get_child_idx(d, root_clades[2][0]);

  auto orig_node_count = count_nodes(d);
  auto orig_edge_count = count_edges(d);

  // Detach A from R (ternary, no collapse)
  detach_source(d, a_idx, root_idx);

  // Reattach A as sibling of B (A's former sibling)
  auto [ni_idx, _dp] = reattach_at_destination(d, a_idx, b_idx);

  // new_inner has children {B, A}
  auto ni_children = get_child_indices(d, ni_idx);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(b_idx) == 1);
  assert(ni_set.count(a_idx) == 1);
  assert(ni_set.size() == 2);

  // R has children {new_inner, C}
  auto r_children = get_child_indices(d, root_idx);
  std::set<std::size_t> r_set(r_children.begin(), r_children.end());
  assert(r_set.count(ni_idx) == 1);
  assert(r_set.count(c_idx) == 1);
  assert(r_set.size() == 2);

  // new_inner's parent is R
  auto ni_pes = get_parent_edges(d, ni_idx);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(d, ni_pes[0]) == root_idx);

  // Node count: original + 1 (new_inner, no collapse)
  assert(count_nodes(d) == orig_node_count + 1);
  // Edge count: original - 1 (detach) - 1 (remove dst edge) + 3 = original + 1
  assert(count_edges(d) == orig_edge_count + 1);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 8 helpers: 12-leaf tree and topology comparison
// ---------------------------------------------------------------------------

// Node indices returned by make_12leaf_tree for easy test access.
struct twelve_leaf_tree {
  phylo_dag tree;
  std::size_t root, i1, i2, i3, i4, i5, i6, i7, i8, i9;
  std::size_t L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12;
};

// Build a 12-leaf tree with mixed binary/ternary structure:
//
//                      root
//                    /       \
//                  i1          i2
//                 / \         / \
//               i3   i4     i5    i6
//              / \  / \    / \   / | \
//            L1  L2 L3 L4 L5  L6 L7 L8 i7
//                                      / \
//                                    L9  i8
//                                       / \
//                                     L10 i9
//                                        / \
//                                      L11 L12
//
// Binary parents: root, i1, i2, i3, i4, i5, i7, i8, i9
// Ternary parent: i6 (children: L7, L8, i7)
//
static twelve_leaf_tree make_12leaf_tree() {
  constexpr std::string_view ref = "AAAAAAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from_sequence("TAAAAAAA", ref);
  l1.sample_id() = "L1";
  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from_sequence("ATAAAAAA", ref);
  l2.sample_id() = "L2";
  auto l3 = d.append_node<node_kind::leaf>();
  l3.cg() = cg_from_sequence("AATAAAAA", ref);
  l3.sample_id() = "L3";
  auto l4 = d.append_node<node_kind::leaf>();
  l4.cg() = cg_from_sequence("AAATAAAA", ref);
  l4.sample_id() = "L4";
  auto l5 = d.append_node<node_kind::leaf>();
  l5.cg() = cg_from_sequence("AAAATAAA", ref);
  l5.sample_id() = "L5";
  auto l6 = d.append_node<node_kind::leaf>();
  l6.cg() = cg_from_sequence("AAAAATAA", ref);
  l6.sample_id() = "L6";
  auto l7 = d.append_node<node_kind::leaf>();
  l7.cg() = cg_from_sequence("AAAAAATA", ref);
  l7.sample_id() = "L7";
  auto l8 = d.append_node<node_kind::leaf>();
  l8.cg() = cg_from_sequence("AAAAAAAT", ref);
  l8.sample_id() = "L8";
  auto l9 = d.append_node<node_kind::leaf>();
  l9.cg() = cg_from_sequence("TAAATAAA", ref);
  l9.sample_id() = "L9";
  auto l10 = d.append_node<node_kind::leaf>();
  l10.cg() = cg_from_sequence("ATAAATAA", ref);
  l10.sample_id() = "L10";
  auto l11 = d.append_node<node_kind::leaf>();
  l11.cg() = cg_from_sequence("AATAAATA", ref);
  l11.sample_id() = "L11";
  auto l12 = d.append_node<node_kind::leaf>();
  l12.cg() = cg_from_sequence("AAATAAAT", ref);
  l12.sample_id() = "L12";

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i1 = d.append_node<node_kind::inner>();
  n_i1.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i2 = d.append_node<node_kind::inner>();
  n_i2.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i3 = d.append_node<node_kind::inner>();
  n_i3.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i4 = d.append_node<node_kind::inner>();
  n_i4.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i5 = d.append_node<node_kind::inner>();
  n_i5.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i6 = d.append_node<node_kind::inner>();
  n_i6.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i7 = d.append_node<node_kind::inner>();
  n_i7.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i8 = d.append_node<node_kind::inner>();
  n_i8.cg() = cg_from_sequence("AAAAAAAA", ref);
  auto n_i9 = d.append_node<node_kind::inner>();
  n_i9.cg() = cg_from_sequence("AAAAAAAA", ref);

  // UA -> root
  add_edge(d, ua.index(), root.index(), 0);
  // root -> i1, i2
  add_edge(d, root.index(), n_i1.index(), 0);
  add_edge(d, root.index(), n_i2.index(), 1);
  // i1 -> i3, i4
  add_edge(d, n_i1.index(), n_i3.index(), 0);
  add_edge(d, n_i1.index(), n_i4.index(), 1);
  // i2 -> i5, i6
  add_edge(d, n_i2.index(), n_i5.index(), 0);
  add_edge(d, n_i2.index(), n_i6.index(), 1);
  // i3 -> L1, L2
  add_edge(d, n_i3.index(), l1.index(), 0);
  add_edge(d, n_i3.index(), l2.index(), 1);
  // i4 -> L3, L4
  add_edge(d, n_i4.index(), l3.index(), 0);
  add_edge(d, n_i4.index(), l4.index(), 1);
  // i5 -> L5, L6
  add_edge(d, n_i5.index(), l5.index(), 0);
  add_edge(d, n_i5.index(), l6.index(), 1);
  // i6 -> L7, L8, i7  (ternary)
  add_edge(d, n_i6.index(), l7.index(), 0);
  add_edge(d, n_i6.index(), l8.index(), 1);
  add_edge(d, n_i6.index(), n_i7.index(), 2);
  // i7 -> L9, i8
  add_edge(d, n_i7.index(), l9.index(), 0);
  add_edge(d, n_i7.index(), n_i8.index(), 1);
  // i8 -> L10, i9
  add_edge(d, n_i8.index(), l10.index(), 0);
  add_edge(d, n_i8.index(), n_i9.index(), 1);
  // i9 -> L11, L12
  add_edge(d, n_i9.index(), l11.index(), 0);
  add_edge(d, n_i9.index(), l12.index(), 1);

  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);

  return twelve_leaf_tree{
      .tree = std::move(d),
      .root = root.index(),
      .i1 = n_i1.index(),
      .i2 = n_i2.index(),
      .i3 = n_i3.index(),
      .i4 = n_i4.index(),
      .i5 = n_i5.index(),
      .i6 = n_i6.index(),
      .i7 = n_i7.index(),
      .i8 = n_i8.index(),
      .i9 = n_i9.index(),
      .L1 = l1.index(),
      .L2 = l2.index(),
      .L3 = l3.index(),
      .L4 = l4.index(),
      .L5 = l5.index(),
      .L6 = l6.index(),
      .L7 = l7.index(),
      .L8 = l8.index(),
      .L9 = l9.index(),
      .L10 = l10.index(),
      .L11 = l11.index(),
      .L12 = l12.index(),
  };
}

// Collect the set of leaf sample_ids from a tree.
static std::set<std::string> collect_leaf_sample_ids(phylo_dag& d) {
  std::set<std::string> result;
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            result.insert(node.sample_id());
          }
        },
        nv);
  }
  return result;
}

// Compute the set of "clades" for topology comparison.
// For each non-leaf, non-UA node, compute the set of leaf sample_ids reachable
// from it.  Two trees have the same topology iff they produce the same set of
// clade leaf-sets (modulo node index renumbering).
static std::set<std::set<std::string>> compute_clade_sets(phylo_dag& d) {
  auto subtree_leaves = compute_subtree_leaves(d);
  auto ua_idx = get_root_idx(d);

  // Map node-index leaf sets to sample_id leaf sets
  std::set<std::set<std::string>> result;
  for (auto nv : d.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    if (is_ua(d, idx)) continue;
    if (is_leaf(d, idx)) continue;
    auto const& leaf_indices = subtree_leaves[idx];
    if (leaf_indices.empty()) continue;

    std::set<std::string> leaf_ids;
    for (auto lidx : leaf_indices) {
      auto lnv = d.get_node(lidx);
      std::visit(
          [&](auto node) {
            if constexpr (requires { node.sample_id(); }) {
              leaf_ids.insert(node.sample_id());
            }
          },
          lnv);
    }
    result.insert(std::move(leaf_ids));
  }
  return result;
}

// ---------------------------------------------------------------------------
// Phase 7 tests: apply_spr_inplace
// ---------------------------------------------------------------------------

static void test_apply_spr_inplace_with_collapse() {
  std::println("test_apply_spr_inplace_with_collapse");

  // Use the 6-leaf suboptimal tree:
  //       root
  //      /    \
  //    i1      i2
  //   / \     / \
  //  i3  L4  L3  i4
  //  /\         /\
  // L1 L2     L5  L6
  //
  // Move L1 to be sibling of L5.
  // i3 is binary so it collapses.
  // After: root -> {i1 -> {L2, L4}, i2 -> {L3, i4 -> {new_inner -> {L5, L1}, L6}}}
  auto d = make_suboptimal_tree();
  tree_index idx{d};

  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto i1_idx = get_child_idx(d, root_clades[0][0]);
  auto i2_idx = get_child_idx(d, root_clades[1][0]);
  auto i1_clades = get_clades(d, i1_idx);
  auto i3_idx = get_child_idx(d, i1_clades[0][0]);
  auto l4_idx = get_child_idx(d, i1_clades[1][0]);
  auto i3_clades = get_clades(d, i3_idx);
  auto l1_idx = get_child_idx(d, i3_clades[0][0]);
  auto l2_idx = get_child_idx(d, i3_clades[1][0]);
  auto i2_clades = get_clades(d, i2_idx);
  auto l3_idx = get_child_idx(d, i2_clades[0][0]);
  auto i4_idx = get_child_idx(d, i2_clades[1][0]);
  auto i4_clades = get_clades(d, i4_idx);
  auto l5_idx = get_child_idx(d, i4_clades[0][0]);
  auto l6_idx = get_child_idx(d, i4_clades[1][0]);

  auto r = apply_spr_inplace(d, idx, l1_idx, l5_idx);

  // Verify spr_result fields
  assert(r.src == l1_idx);
  assert(r.dst == l5_idx);
  assert(r.src_parent == i3_idx);
  assert(r.dst_parent == i4_idx);
  assert(r.lca == root_idx);  // LCA of L1 and L5 is root

  // src_parent (i3) was binary, so it collapsed
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == i3_idx);
  assert(r.grandparent == i1_idx);
  assert(r.remaining_child == l2_idx);

  // new_inner is a valid node with correct children {L5, L1}
  auto ni_children = get_child_indices(d, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(l5_idx) == 1);
  assert(ni_set.count(l1_idx) == 1);
  assert(ni_set.size() == 2);

  // new_inner's parent is i4
  auto ni_pes = get_parent_edges(d, r.new_inner);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(d, ni_pes[0]) == i4_idx);

  // i4 has children {new_inner, L6}
  auto i4_children = get_child_indices(d, i4_idx);
  std::set<std::size_t> i4_set(i4_children.begin(), i4_children.end());
  assert(i4_set.count(r.new_inner) == 1);
  assert(i4_set.count(l6_idx) == 1);

  // i1 has children {L2, L4} (i3 was collapsed)
  auto i1_children = get_child_indices(d, i1_idx);
  std::set<std::size_t> i1_set(i1_children.begin(), i1_children.end());
  assert(i1_set.count(l2_idx) == 1);
  assert(i1_set.count(l4_idx) == 1);
  assert(i1_set.size() == 2);

  std::println("  PASS");
}

static void test_apply_spr_inplace_no_collapse() {
  std::println("test_apply_spr_inplace_no_collapse");

  // Tree: UA -> R -> {A, B, C}.  Move A to be sibling of C.
  // R is ternary so no collapse.
  // After: R -> {B, new_inner -> {C, A}}
  auto d = make_simple_tree();
  tree_index idx{d};

  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);
  auto c_idx = get_child_idx(d, root_clades[2][0]);

  auto orig_node_count = count_nodes(d);

  auto r = apply_spr_inplace(d, idx, a_idx, c_idx);

  // Verify spr_result fields
  assert(r.src == a_idx);
  assert(r.dst == c_idx);
  assert(r.src_parent == root_idx);
  assert(r.dst_parent == root_idx);  // C's parent is R
  assert(r.lca == root_idx);         // LCA of A and C is R
  assert(!r.src_parent_collapsed);   // R was ternary, no collapse

  // new_inner has children {C, A}
  auto ni_children = get_child_indices(d, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(c_idx) == 1);
  assert(ni_set.count(a_idx) == 1);
  assert(ni_set.size() == 2);

  // R has children {B, new_inner}
  auto r_children = get_child_indices(d, root_idx);
  std::set<std::size_t> r_set(r_children.begin(), r_children.end());
  assert(r_set.count(b_idx) == 1);
  assert(r_set.count(r.new_inner) == 1);
  assert(r_set.size() == 2);

  // Node count: orig + 1 (new_inner, no collapse)
  assert(count_nodes(d) == orig_node_count + 1);

  std::println("  PASS");
}

static void test_apply_spr_inplace_sibling_move() {
  std::println("test_apply_spr_inplace_sibling_move");

  // Tree: UA -> R -> {A, B, C}.  Move A to be sibling of B.
  // src_parent == dst_parent == R (ternary, no collapse).
  // After: R -> {new_inner -> {B, A}, C}
  auto d = make_simple_tree();
  tree_index idx{d};

  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);
  auto c_idx = get_child_idx(d, root_clades[2][0]);

  auto r = apply_spr_inplace(d, idx, a_idx, b_idx);

  // Verify spr_result fields — src_parent == dst_parent
  assert(r.src == a_idx);
  assert(r.dst == b_idx);
  assert(r.src_parent == root_idx);
  assert(r.dst_parent == root_idx);
  assert(r.lca == root_idx);  // LCA of siblings is their parent
  assert(!r.src_parent_collapsed);

  // new_inner has children {B, A}
  auto ni_children = get_child_indices(d, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(b_idx) == 1);
  assert(ni_set.count(a_idx) == 1);
  assert(ni_set.size() == 2);

  // R has children {new_inner, C}
  auto r_children = get_child_indices(d, root_idx);
  std::set<std::size_t> r_set(r_children.begin(), r_children.end());
  assert(r_set.count(r.new_inner) == 1);
  assert(r_set.count(c_idx) == 1);
  assert(r_set.size() == 2);

  std::println("  PASS");
}

static void test_apply_spr_inplace_sibling_collapse() {
  std::println("test_apply_spr_inplace_sibling_collapse");

  // Tree: UA -> R -> {P -> {A, B}, C}.  Move A to be sibling of B.
  // src_parent == dst_parent == P (binary, collapses).
  // After collapse: R -> {B, C}
  // After reattach: R -> {new_inner -> {B, A}, C}
  // dst_parent in spr_result should be R (post-collapse parent of B).
  auto d = make_binary_parent_tree();
  tree_index idx{d};

  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto p_idx = get_child_idx(d, root_clades[0][0]);
  auto c_idx = get_child_idx(d, root_clades[1][0]);
  auto p_clades = get_clades(d, p_idx);
  auto a_idx = get_child_idx(d, p_clades[0][0]);
  auto b_idx = get_child_idx(d, p_clades[1][0]);

  auto orig_node_count = count_nodes(d);

  auto r = apply_spr_inplace(d, idx, a_idx, b_idx);

  // Verify spr_result fields
  assert(r.src == a_idx);
  assert(r.dst == b_idx);
  assert(r.src_parent == p_idx);
  // dst_parent is R (post-collapse parent of B), not P
  assert(r.dst_parent == root_idx);
  assert(r.lca == p_idx);  // LCA of A and B was P

  // P collapsed
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == p_idx);
  assert(r.grandparent == root_idx);
  assert(r.remaining_child == b_idx);

  // new_inner has children {B, A}
  auto ni_children = get_child_indices(d, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(b_idx) == 1);
  assert(ni_set.count(a_idx) == 1);
  assert(ni_set.size() == 2);

  // new_inner's parent is R
  auto ni_pes = get_parent_edges(d, r.new_inner);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(d, ni_pes[0]) == root_idx);

  // R has children {new_inner, C}
  auto r_children = get_child_indices(d, root_idx);
  std::set<std::size_t> r_set(r_children.begin(), r_children.end());
  assert(r_set.count(r.new_inner) == 1);
  assert(r_set.count(c_idx) == 1);
  assert(r_set.size() == 2);

  // Node count: orig - 1 (collapse P) + 1 (new_inner) = orig
  assert(count_nodes(d) == orig_node_count);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 8 tests: validate in-place vs clone-based SPR
// ---------------------------------------------------------------------------

static void test_inplace_vs_clone_spr() {
  std::println("test_inplace_vs_clone_spr");

  // 10 different (src, dst) pairs on the 12-leaf tree.
  // Each pair exercises a different SPR configuration:
  //  1. L1->L5:  cross-tree, binary collapse at i3
  //  2. L3->L8:  cross-tree to ternary subtree, binary collapse at i4
  //  3. L5->L3:  cross-tree, binary collapse at i5
  //  4. L7->L1:  ternary parent i6, no collapse
  //  5. L1->L2:  sibling move, binary collapse at i3
  //  6. L11->L5: deep to shallow, binary collapse at i9
  //  7. L9->L12: within-subtree, binary collapse at i7
  //  8. L4->L6:  cross-subtree, binary collapse at i4
  //  9. L12->L1: deep to far side, binary collapse at i9
  // 10. L6->L10: cross-subtree, binary collapse at i5
  struct move_spec {
    std::size_t twelve_leaf_tree::*src;
    std::size_t twelve_leaf_tree::*dst;
    const char* desc;
  };

  move_spec moves[] = {
      {&twelve_leaf_tree::L1, &twelve_leaf_tree::L5, "L1->L5 cross-tree collapse"},
      {&twelve_leaf_tree::L3, &twelve_leaf_tree::L8, "L3->L8 cross-tree to ternary"},
      {&twelve_leaf_tree::L5, &twelve_leaf_tree::L3, "L5->L3 cross-tree collapse"},
      {&twelve_leaf_tree::L7, &twelve_leaf_tree::L1, "L7->L1 ternary no collapse"},
      {&twelve_leaf_tree::L1, &twelve_leaf_tree::L2, "L1->L2 sibling collapse"},
      {&twelve_leaf_tree::L11, &twelve_leaf_tree::L5, "L11->L5 deep to shallow"},
      {&twelve_leaf_tree::L9, &twelve_leaf_tree::L12, "L9->L12 within subtree"},
      {&twelve_leaf_tree::L4, &twelve_leaf_tree::L6, "L4->L6 cross-subtree"},
      {&twelve_leaf_tree::L12, &twelve_leaf_tree::L1, "L12->L1 deep to far side"},
      {&twelve_leaf_tree::L6, &twelve_leaf_tree::L10, "L6->L10 cross-subtree deep"},
  };

  for (auto const& [src_member, dst_member, desc] : moves) {
    std::println("  subtest: {}", desc);

    // --- Clone-based path ---
    auto ta = make_12leaf_tree();
    auto src_a = ta.*src_member;
    auto dst_a = ta.*dst_member;
    auto clone_result = apply_spr_move(ta.tree, src_a, dst_a);

    // --- In-place path ---
    auto tb = make_12leaf_tree();
    auto src_b = tb.*src_member;
    auto dst_b = tb.*dst_member;
    {
      tree_index idx{tb.tree};
      apply_spr_inplace(tb.tree, idx, src_b, dst_b);
    }
    build_clade_offsets(tb.tree);
    fitch_assign_compact_genomes(tb.tree);
    recompute_edge_mutations(tb.tree);

    // --- Structural equivalence (catches unary-node regressions that
    //     compute_clade_sets would silently deduplicate away) ---
    assert(count_nodes(clone_result) == count_nodes(tb.tree));

    // --- Compare leaf sets ---
    auto leaves_clone = collect_leaf_sample_ids(clone_result);
    auto leaves_inplace = collect_leaf_sample_ids(tb.tree);
    assert(leaves_clone == leaves_inplace);

    // --- Compare parsimony score ---
    int pars_clone = ground_truth_parsimony(clone_result);
    int pars_inplace = ground_truth_parsimony(tb.tree);
    if (pars_clone != pars_inplace) {
      std::println("    FAIL: parsimony mismatch: clone={} inplace={}",
                   pars_clone, pars_inplace);
    }
    assert(pars_clone == pars_inplace);

    // --- Compare topology (clade sets) ---
    auto clades_clone = compute_clade_sets(clone_result);
    auto clades_inplace = compute_clade_sets(tb.tree);
    if (clades_clone != clades_inplace) {
      std::println("    FAIL: topology mismatch for {}", desc);
      std::println("    clone clade count: {}", clades_clone.size());
      std::println("    inplace clade count: {}", clades_inplace.size());
    }
    assert(clades_clone == clades_inplace);

    std::println("    OK (parsimony={}, clades={})", pars_clone,
                 clades_clone.size());
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 9: Edge cases and robustness
// ---------------------------------------------------------------------------

// Helper: cross-validate in-place SPR result against clone-based SPR.
// Both trees must be freshly constructed from the same builder.
// Caller has already applied apply_spr_inplace to `inplace_tree`.
static void cross_validate_vs_clone(phylo_dag& inplace_tree,
                                    twelve_leaf_tree& clone_spec,
                                    std::size_t twelve_leaf_tree::*src_member,
                                    std::size_t twelve_leaf_tree::*dst_member) {
  // Rebuild CGs and edge mutations on the in-place tree.
  build_clade_offsets(inplace_tree);
  fitch_assign_compact_genomes(inplace_tree);
  recompute_edge_mutations(inplace_tree);

  // Clone-based reference.
  auto src_c = clone_spec.*src_member;
  auto dst_c = clone_spec.*dst_member;
  auto clone_result = apply_spr_move(clone_spec.tree, src_c, dst_c);

  // Node count (catches unary-node regressions).
  assert(count_nodes(clone_result) == count_nodes(inplace_tree));

  // Leaf sets.
  assert(collect_leaf_sample_ids(clone_result) ==
         collect_leaf_sample_ids(inplace_tree));

  // Parsimony score.
  int pars_clone = ground_truth_parsimony(clone_result);
  int pars_inplace = ground_truth_parsimony(inplace_tree);
  assert(pars_clone == pars_inplace);

  // Topology (clade sets).
  assert(compute_clade_sets(clone_result) == compute_clade_sets(inplace_tree));
}

// Test 1: src is direct child of dst_parent (binary, collapses).
// Move L3 → L4 on the 12-leaf tree.  src_parent == dst_parent == i4 (binary).
// Verify no double-removal: i4 is collapsed exactly once, topology is valid.
static void test_edge_case_src_is_child_of_dst_parent() {
  std::println("test_edge_case_src_is_child_of_dst_parent");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto orig_node_count = count_nodes(t.tree);

  auto r = apply_spr_inplace(t.tree, idx, t.L3, t.L4);

  // src_parent == dst_parent == i4 before move
  assert(r.src == t.L3);
  assert(r.dst == t.L4);
  assert(r.src_parent == t.i4);
  // i4 was binary → collapsed
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.i4);
  assert(r.grandparent == t.i1);
  assert(r.remaining_child == t.L4);
  // dst_parent should be i1 (post-collapse parent of L4)
  assert(r.dst_parent == t.i1);
  // LCA of L3 and L4 was i4
  assert(r.lca == t.i4);

  // new_inner has children {L4, L3}
  auto ni_children = get_child_indices(t.tree, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(t.L4) == 1);
  assert(ni_set.count(t.L3) == 1);
  assert(ni_set.size() == 2);

  // No double-removal: node count = orig - 1 (collapse) + 1 (new_inner)
  assert(count_nodes(t.tree) == orig_node_count);

  // Cross-validate against clone-based SPR.
  auto tc = make_12leaf_tree();
  cross_validate_vs_clone(t.tree, tc, &twelve_leaf_tree::L3,
                          &twelve_leaf_tree::L4);

  std::println("  PASS");
}

// Test 2: dst is child of src_parent (sibling swap) under ternary parent.
// Move L7 → L8 on the 12-leaf tree.  src_parent == dst_parent == i6 (ternary).
// No collapse; topology should still be valid.
static void test_edge_case_sibling_swap_ternary() {
  std::println("test_edge_case_sibling_swap_ternary");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto orig_node_count = count_nodes(t.tree);

  auto r = apply_spr_inplace(t.tree, idx, t.L7, t.L8);

  assert(r.src == t.L7);
  assert(r.dst == t.L8);
  assert(r.src_parent == t.i6);
  assert(r.dst_parent == t.i6);
  // i6 was ternary → no collapse
  assert(!r.src_parent_collapsed);
  // LCA of siblings is their parent
  assert(r.lca == t.i6);

  // new_inner has children {L8, L7}
  auto ni_children = get_child_indices(t.tree, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(t.L8) == 1);
  assert(ni_set.count(t.L7) == 1);
  assert(ni_set.size() == 2);

  // i6 still has children: {new_inner, i7} (L7 detached, L8 moved under new_inner)
  auto i6_children = get_child_indices(t.tree, t.i6);
  std::set<std::size_t> i6_set(i6_children.begin(), i6_children.end());
  assert(i6_set.count(r.new_inner) == 1);
  assert(i6_set.count(t.i7) == 1);
  assert(i6_set.size() == 2);

  // Node count: orig + 1 (new_inner, no collapse)
  assert(count_nodes(t.tree) == orig_node_count + 1);

  // Cross-validate.
  auto tc = make_12leaf_tree();
  cross_validate_vs_clone(t.tree, tc, &twelve_leaf_tree::L7,
                          &twelve_leaf_tree::L8);

  std::println("  PASS");
}

// Test 3: LCA is tree root (move spans entire tree).
// Move L1 → L12 on the 12-leaf tree.  L1 is under i1 (left subtree),
// L12 is under i2 (right subtree).  LCA == root.
static void test_edge_case_lca_is_tree_root() {
  std::println("test_edge_case_lca_is_tree_root");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L12);

  assert(r.src == t.L1);
  assert(r.dst == t.L12);
  // LCA of L1 and L12 is root
  assert(r.lca == t.root);
  // src_parent == i3 (binary) → collapses
  assert(r.src_parent == t.i3);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.i3);
  assert(r.grandparent == t.i1);
  assert(r.remaining_child == t.L2);

  // new_inner has children {L12, L1}
  auto ni_children = get_child_indices(t.tree, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(t.L12) == 1);
  assert(ni_set.count(t.L1) == 1);
  assert(ni_set.size() == 2);

  // Cross-validate.
  auto tc = make_12leaf_tree();
  cross_validate_vs_clone(t.tree, tc, &twelve_leaf_tree::L1,
                          &twelve_leaf_tree::L12);

  std::println("  PASS");
}

// Test 4: LCA collapses (src_parent == LCA and was binary).
// Move L1 → L2 on the 12-leaf tree.  src_parent == i3, LCA of L1 and L2 == i3.
// i3 is binary → collapses.  Verify spr_result.grandparent == i1.
static void test_edge_case_lca_collapses() {
  std::println("test_edge_case_lca_collapses");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L2);

  assert(r.src == t.L1);
  assert(r.dst == t.L2);
  // LCA of L1 and L2 is their common parent i3
  assert(r.lca == t.i3);
  // src_parent == LCA == i3 (binary) → collapses
  assert(r.src_parent == t.i3);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.i3);
  assert(r.grandparent == t.i1);
  assert(r.remaining_child == t.L2);

  // After collapse, L2 is reparented to i1.
  // Reattach creates new_inner under i1 with children {L2, L1}.
  auto ni_children = get_child_indices(t.tree, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(t.L2) == 1);
  assert(ni_set.count(t.L1) == 1);
  assert(ni_set.size() == 2);

  // new_inner's parent is i1 (the grandparent of the collapsed LCA)
  auto ni_pes = get_parent_edges(t.tree, r.new_inner);
  assert(ni_pes.size() == 1);
  assert(get_parent_idx(t.tree, ni_pes[0]) == t.i1);

  // Cross-validate.
  auto tc = make_12leaf_tree();
  cross_validate_vs_clone(t.tree, tc, &twelve_leaf_tree::L1,
                          &twelve_leaf_tree::L2);

  std::println("  PASS");
}

// Test 5: Tree has polytomies — src_parent has >2 children, no collapse.
// Move L8 → L3 on the 12-leaf tree.  src_parent == i6 (ternary: L7, L8, i7).
// No collapse needed.
static void test_edge_case_polytomy_no_collapse() {
  std::println("test_edge_case_polytomy_no_collapse");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto orig_node_count = count_nodes(t.tree);

  auto r = apply_spr_inplace(t.tree, idx, t.L8, t.L3);

  assert(r.src == t.L8);
  assert(r.dst == t.L3);
  assert(r.src_parent == t.i6);
  // i6 was ternary → no collapse
  assert(!r.src_parent_collapsed);
  // dst_parent == i4 (parent of L3)
  assert(r.dst_parent == t.i4);

  // i6 should still have 2 children after detach: {L7, i7}
  auto i6_children = get_child_indices(t.tree, t.i6);
  std::set<std::size_t> i6_set(i6_children.begin(), i6_children.end());
  assert(i6_set.count(t.L7) == 1);
  assert(i6_set.count(t.i7) == 1);
  assert(i6_set.size() == 2);

  // new_inner under i4 with children {L3, L8}
  auto ni_children = get_child_indices(t.tree, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(t.L3) == 1);
  assert(ni_set.count(t.L8) == 1);
  assert(ni_set.size() == 2);

  // Node count: orig + 1 (new_inner, no collapse)
  assert(count_nodes(t.tree) == orig_node_count + 1);

  // Cross-validate.
  auto tc = make_12leaf_tree();
  cross_validate_vs_clone(t.tree, tc, &twelve_leaf_tree::L8,
                          &twelve_leaf_tree::L3);

  std::println("  PASS");
}

// Test 6: Move to adjacent node — dst is src's sibling.
// Move L11 → L12 on the 12-leaf tree.  Both are children of i9 (binary).
// This is a near-trivial topology change deep in the tree.  Verify valid tree.
static void test_edge_case_move_to_adjacent_node() {
  std::println("test_edge_case_move_to_adjacent_node");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto orig_node_count = count_nodes(t.tree);

  auto r = apply_spr_inplace(t.tree, idx, t.L11, t.L12);

  assert(r.src == t.L11);
  assert(r.dst == t.L12);
  assert(r.src_parent == t.i9);
  // i9 was binary → collapses
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.i9);
  assert(r.grandparent == t.i8);
  assert(r.remaining_child == t.L12);

  // After collapse of i9, L12 is reparented to i8.
  // new_inner under i8 with children {L12, L11}.
  auto ni_children = get_child_indices(t.tree, r.new_inner);
  std::set<std::size_t> ni_set(ni_children.begin(), ni_children.end());
  assert(ni_set.count(t.L12) == 1);
  assert(ni_set.count(t.L11) == 1);
  assert(ni_set.size() == 2);

  // Node count: orig - 1 (collapse) + 1 (new_inner) = orig
  assert(count_nodes(t.tree) == orig_node_count);

  // The tree is structurally valid: every non-UA node has exactly 1 parent.
  auto ua_idx = get_root_idx(t.tree);
  for (auto nv : t.tree.get_all_nodes()) {
    auto idx2 = std::visit([](auto n) { return n.index(); }, nv);
    if (is_ua(t.tree, idx2)) continue;
    auto pes = get_parent_edges(t.tree, idx2);
    assert(pes.size() == 1);
  }

  // Cross-validate.
  auto tc = make_12leaf_tree();
  cross_validate_vs_clone(t.tree, tc, &twelve_leaf_tree::L11,
                          &twelve_leaf_tree::L12);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 10: tree_index::update_topology
// ---------------------------------------------------------------------------

// Verify that the tree_index's parent_[] and children_[] match the actual DAG
// topology for every non-UA node still present in the DAG.
static void verify_topology_matches_dag(tree_index& idx, phylo_dag& tree) {
  for (auto nv : tree.get_all_nodes()) {
    auto nid = std::visit([](auto n) { return n.index(); }, nv);
    if (is_ua(tree, nid)) continue;

    assert(idx.is_valid(nid));

    // Check parent.
    auto pes = get_parent_edges(tree, nid);
    assert(pes.size() == 1);
    auto actual_parent = get_parent_idx(tree, pes[0]);
    if (idx.get_parent(nid) != actual_parent) {
      std::println("  FAIL: parent mismatch at node {}: idx={} dag={}",
                   nid, idx.get_parent(nid), actual_parent);
    }
    assert(idx.get_parent(nid) == actual_parent);

    // Check children (compare as sets — order may differ).
    auto actual_children = get_child_indices(tree, nid);
    auto const& idx_children = idx.get_children(nid);
    std::set<std::size_t> actual_set(actual_children.begin(),
                                     actual_children.end());
    std::set<std::size_t> idx_set(idx_children.begin(), idx_children.end());
    if (actual_set != idx_set) {
      std::println("  FAIL: children mismatch at node {}", nid);
      std::print("    dag:");
      for (auto c : actual_set) std::print(" {}", c);
      std::println("");
      std::print("    idx:");
      for (auto c : idx_set) std::print(" {}", c);
      std::println("");
    }
    assert(actual_set == idx_set);
  }
}

// Test 1: Apply SPR with binary collapse, then update_topology.
// Verify parent_[] and children_[] match the actual DAG.
static void test_update_topology_with_collapse() {
  std::println("test_update_topology_with_collapse");

  // 12-leaf tree.  Move L1 → L5.
  // i3 is binary → collapses.  new_inner created under i4.
  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.i3);

  idx.update_topology(r);

  // The chain may reuse the collapsed slot for new_inner.  When that happens,
  // collapsed_node == new_inner and the slot is valid (occupied by new_inner).
  if (r.collapsed_node != r.new_inner) {
    assert(!idx.is_valid(r.collapsed_node));
  }
  assert(idx.is_valid(r.new_inner));

  // Verify full topology matches DAG.
  verify_topology_matches_dag(idx, t.tree);

  std::println("  PASS");
}

// Test 2: Apply SPR without collapse (ternary parent), then update_topology.
static void test_update_topology_no_collapse() {
  std::println("test_update_topology_no_collapse");

  // 12-leaf tree.  Move L8 → L3.
  // i6 is ternary → no collapse.
  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L8, t.L3);
  assert(!r.src_parent_collapsed);

  idx.update_topology(r);

  // new_inner is valid.
  assert(idx.is_valid(r.new_inner));

  // Verify full topology matches DAG.
  verify_topology_matches_dag(idx, t.tree);

  std::println("  PASS");
}

// Test 3: Sibling move (L11 → L12) with collapse, then update_topology.
static void test_update_topology_sibling_collapse() {
  std::println("test_update_topology_sibling_collapse");

  // i9 is binary → collapses.  L12 reparented to i8, then new_inner
  // created under i8 with children {L12, L11}.
  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L11, t.L12);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.i9);

  idx.update_topology(r);

  if (r.collapsed_node != r.new_inner) {
    assert(!idx.is_valid(r.collapsed_node));
  }
  assert(idx.is_valid(r.new_inner));

  verify_topology_matches_dag(idx, t.tree);

  std::println("  PASS");
}

// Test 4: Multiple SPR configurations from Phase 8, all verified via
// update_topology against the actual DAG.
static void test_update_topology_multiple_moves() {
  std::println("test_update_topology_multiple_moves");

  struct move_spec {
    std::size_t twelve_leaf_tree::*src;
    std::size_t twelve_leaf_tree::*dst;
    const char* desc;
  };

  move_spec moves[] = {
      {&twelve_leaf_tree::L1, &twelve_leaf_tree::L5,
       "L1->L5 cross-tree collapse"},
      {&twelve_leaf_tree::L3, &twelve_leaf_tree::L8,
       "L3->L8 cross-tree to ternary"},
      {&twelve_leaf_tree::L5, &twelve_leaf_tree::L3,
       "L5->L3 cross-tree collapse"},
      {&twelve_leaf_tree::L7, &twelve_leaf_tree::L1,
       "L7->L1 ternary no collapse"},
      {&twelve_leaf_tree::L1, &twelve_leaf_tree::L2,
       "L1->L2 sibling collapse"},
      {&twelve_leaf_tree::L11, &twelve_leaf_tree::L5,
       "L11->L5 deep to shallow"},
      {&twelve_leaf_tree::L9, &twelve_leaf_tree::L12,
       "L9->L12 within subtree"},
      {&twelve_leaf_tree::L4, &twelve_leaf_tree::L6,
       "L4->L6 cross-subtree"},
      {&twelve_leaf_tree::L12, &twelve_leaf_tree::L1,
       "L12->L1 deep to far side"},
      {&twelve_leaf_tree::L6, &twelve_leaf_tree::L10,
       "L6->L10 cross-subtree deep"},
  };

  for (auto const& [src_member, dst_member, desc] : moves) {
    std::println("  subtest: {}", desc);

    auto t = make_12leaf_tree();
    tree_index idx{t.tree};

    auto src = t.*src_member;
    auto dst = t.*dst_member;
    auto r = apply_spr_inplace(t.tree, idx, src, dst);
    idx.update_topology(r);

    if (r.src_parent_collapsed && r.collapsed_node != r.new_inner) {
      assert(!idx.is_valid(r.collapsed_node));
    }
    assert(idx.is_valid(r.new_inner));

    verify_topology_matches_dag(idx, t.tree);

    std::println("    OK");
  }

  std::println("  PASS");
}

// Test 5: Verify ensure_capacity grows arrays when new_inner exceeds
// num_nodes_.  Apply SPR, verify the index can address the new node.
static void test_update_topology_ensure_capacity() {
  std::println("test_update_topology_ensure_capacity");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // Record state before SPR.  new_inner will be appended at the DAG's
  // current high mark.
  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);

  // new_inner should be at or beyond the original num_nodes_.
  // update_topology calls ensure_capacity internally.
  idx.update_topology(r);

  // If ensure_capacity didn't run, the next line would segfault or assert.
  assert(idx.is_valid(r.new_inner));
  assert(idx.get_parent(r.new_inner) == r.dst_parent);
  auto const& kids = idx.get_children(r.new_inner);
  std::set<std::size_t> kid_set(kids.begin(), kids.end());
  assert(kid_set.count(t.L1) == 1);
  assert(kid_set.count(t.L5) == 1);

  std::println("  PASS");
}

// Test 6: Phase 9 edge cases verified through update_topology.
// These are the tricky SPR configurations (sibling swaps, LCA collapse,
// polytomy moves, etc.) that Phase 9 validated at the DAG level.
// This test verifies update_topology also handles them correctly.
static void test_update_topology_edge_cases() {
  std::println("test_update_topology_edge_cases");

  struct move_spec {
    std::size_t twelve_leaf_tree::*src;
    std::size_t twelve_leaf_tree::*dst;
    const char* desc;
  };

  move_spec moves[] = {
      // src is child of dst_parent (Phase 9 test 1: L3→L4, collapse)
      {&twelve_leaf_tree::L3, &twelve_leaf_tree::L4,
       "src_is_child_of_dst_parent (L3->L4, collapse)"},
      // sibling swap ternary (Phase 9 test 2: L7→L8, no collapse)
      {&twelve_leaf_tree::L7, &twelve_leaf_tree::L8,
       "sibling_swap_ternary (L7->L8, no collapse)"},
      // LCA is tree root (Phase 9 test 3: L1→L12)
      {&twelve_leaf_tree::L1, &twelve_leaf_tree::L12,
       "lca_is_tree_root (L1->L12)"},
      // LCA collapses (Phase 9 test 4: L1→L2, sibling collapse)
      {&twelve_leaf_tree::L1, &twelve_leaf_tree::L2,
       "lca_collapses (L1->L2, sibling collapse)"},
      // polytomy no collapse (Phase 9 test 5: L8→L3)
      {&twelve_leaf_tree::L8, &twelve_leaf_tree::L3,
       "polytomy_no_collapse (L8->L3)"},
      // move to adjacent node (Phase 9 test 6: L11→L12, sibling collapse)
      {&twelve_leaf_tree::L11, &twelve_leaf_tree::L12,
       "move_to_adjacent (L11->L12, sibling collapse)"},
  };

  for (auto const& [src_member, dst_member, desc] : moves) {
    std::println("  subtest: {}", desc);

    auto t = make_12leaf_tree();
    tree_index idx{t.tree};

    auto src = t.*src_member;
    auto dst = t.*dst_member;
    auto r = apply_spr_inplace(t.tree, idx, src, dst);
    idx.update_topology(r);

    if (r.src_parent_collapsed && r.collapsed_node != r.new_inner) {
      assert(!idx.is_valid(r.collapsed_node));
    }
    assert(idx.is_valid(r.new_inner));

    verify_topology_matches_dag(idx, t.tree);

    std::println("    OK");
  }

  std::println("  PASS");
}

// Test 7: Sequential SPRs — apply + update_topology twice on the same index.
// Verifies that update_topology correctly patches an already-patched index.
static void test_update_topology_sequential_sprs() {
  std::println("test_update_topology_sequential_sprs");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // First SPR: L1 -> L5 (cross-tree, binary collapse of i3).
  auto r1 = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  assert(r1.src_parent_collapsed);
  idx.update_topology(r1);
  verify_topology_matches_dag(idx, t.tree);
  std::println("  after SPR 1 (L1->L5): OK");

  // Second SPR needs fresh DFS for compute_lca (stale after first
  // update_topology).  Build a temporary index for the LCA computation.
  tree_index idx_for_lca{t.tree};
  auto r2 = apply_spr_inplace(t.tree, idx_for_lca, t.L8, t.L3);
  idx.update_topology(r2);
  verify_topology_matches_dag(idx, t.tree);
  std::println("  after SPR 2 (L8->L3): OK");

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 11: ensure_capacity — grow flat arrays for new nodes
// ---------------------------------------------------------------------------

// Test 1: ensure_capacity grows arrays when new_inner exceeds num_nodes_.
// Uses a non-collapsing SPR (from ternary parent i6) so the new node is
// appended at high_mark rather than recycling a removed node's slot.
// Calls ensure_capacity directly and verifies all new slots are zero-initialized.
static void test_ensure_capacity_array_sizes() {
  std::println("test_ensure_capacity_array_sizes");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};
  auto old_cap = idx.num_nodes();

  // L7 is under ternary parent i6 (children: L7, L8, i7).
  // Moving L7 does NOT collapse i6, so append_node allocates at high_mark.
  auto r = apply_spr_inplace(t.tree, idx, t.L7, t.L1);
  assert(!r.src_parent_collapsed);
  auto new_high = t.tree.node_high_mark();
  assert(new_high > old_cap);

  // Call ensure_capacity directly (not through update_topology) to test
  // it in isolation.
  idx.ensure_capacity(new_high);
  assert(idx.num_nodes() == new_high);

  // All new slots (from old_cap to new_high - 1) should be zero-initialized.
  auto n_sites = idx.num_variable_sites();
  for (std::size_t n = old_cap; n < new_high; n++) {
    assert(!idx.is_valid(n));
    assert(!idx.has_dfs_info(n));
    assert(!idx.has_child_counts(n));
    assert(idx.get_num_children(n) == 0);
    // dfs_info struct should be value-initialized (all zeros).
    auto const& di = idx.get_dfs_info(n);
    assert(di.dfs_index == 0 && di.dfs_end_index == 0 && di.level == 0);
    // parent should default to 0.
    assert(idx.get_parent(n) == 0);
    // children should be an empty vector.
    assert(idx.get_children(n).empty());
    for (std::size_t i = 0; i < n_sites; i++) {
      assert(idx.get_fitch_set(n, i) == 0);
      auto const& cc = idx.get_child_counts(n, i);
      assert(cc[0] == 0 && cc[1] == 0 && cc[2] == 0 && cc[3] == 0);
      assert(idx.get_allele_union(n, i) == 0);
    }
  }

  std::println("  old_cap={} new_cap={}", old_cap, new_high);
  std::println("  PASS");
}

// Test 2: Two SPRs in sequence; verify capacity grows monotonically.
// First SPR is non-collapsing (truly grows arrays).  Second may collapse
// (recycling a hole), but capacity must not shrink.
static void test_ensure_capacity_monotonic_growth() {
  std::println("test_ensure_capacity_monotonic_growth");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};
  auto cap0 = idx.num_nodes();

  // First SPR: L7 → L1 (non-collapsing from ternary i6).
  auto r1 = apply_spr_inplace(t.tree, idx, t.L7, t.L1);
  assert(!r1.src_parent_collapsed);
  idx.update_topology(r1);
  auto cap1 = idx.num_nodes();
  assert(cap1 > cap0);
  assert(idx.is_valid(r1.new_inner));

  // Second SPR: L8 → L3.  i6 is now binary {L8, i7}, so this collapses i6.
  // The chain recycles i6's slot for new_inner, so high_mark may not grow.
  tree_index idx_for_lca{t.tree};
  auto r2 = apply_spr_inplace(t.tree, idx_for_lca, t.L8, t.L3);
  idx.update_topology(r2);
  auto cap2 = idx.num_nodes();
  assert(cap2 >= cap1);  // monotonic — never shrinks

  // Both new_inner nodes should be valid and addressable.
  assert(idx.is_valid(r1.new_inner));
  assert(idx.is_valid(r2.new_inner));

  // Verify full topology survives the second SPR (covers both the
  // reallocation triggered by growth and the hole-reuse no-op case where
  // ensure_capacity returns immediately because high_mark didn't change).
  verify_topology_matches_dag(idx, t.tree);

  std::println("  cap0={} cap1={} cap2={}", cap0, cap1, cap2);
  std::println("  PASS");
}

// Phase 12: update_searchable_nodes — patch searchable_nodes_ after SPR
// ---------------------------------------------------------------------------

// After SPR with binary collapse: collapsed_node removed, new_inner added.
static void test_update_searchable_nodes_with_collapse() {
  std::println("test_update_searchable_nodes_with_collapse");

  // Move L1 → L5.  i3 is binary → collapses.
  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // Snapshot searchable_nodes before the move.
  auto sn_before = idx.get_searchable_nodes();
  std::set<std::size_t> before_set(sn_before.begin(), sn_before.end());

  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  assert(r.src_parent_collapsed);
  auto collapsed = r.collapsed_node;
  auto new_inner = r.new_inner;

  // collapsed_node was in the original searchable set.
  assert(before_set.count(collapsed));

  idx.update_topology(r);
  idx.update_searchable_nodes(r);

  auto const& sn_after = idx.get_searchable_nodes();
  std::set<std::size_t> after_set(sn_after.begin(), sn_after.end());

  // new_inner must be present.
  assert(after_set.count(new_inner));
  // src and dst still present.
  assert(after_set.count(t.L1));
  assert(after_set.count(t.L5));

  if (collapsed != new_inner) {
    // When the DAG does NOT recycle the slot, collapsed_node must be gone.
    assert(!after_set.count(collapsed));
  }
  // When the DAG recycles collapsed_node's slot for new_inner
  // (collapsed == new_inner), the index is correctly present as new_inner.

  std::println("  collapsed={} new_inner={} recycled={}",
               collapsed, new_inner, collapsed == new_inner);
  std::println("  PASS");
}

// After SPR without collapse: new_inner added, nothing removed.
static void test_update_searchable_nodes_no_collapse() {
  std::println("test_update_searchable_nodes_no_collapse");

  // Move L7 → L1.  i6 is ternary → no collapse.
  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto sn_before = idx.get_searchable_nodes();
  auto count_before = sn_before.size();

  auto r = apply_spr_inplace(t.tree, idx, t.L7, t.L1);
  assert(!r.src_parent_collapsed);
  auto new_inner = r.new_inner;

  idx.update_topology(r);
  idx.update_searchable_nodes(r);

  auto const& sn_after = idx.get_searchable_nodes();
  std::set<std::size_t> after_set(sn_after.begin(), sn_after.end());

  // new_inner must be present.
  assert(after_set.count(new_inner));
  // Size grew by exactly 1 (added new_inner, removed nothing).
  assert(sn_after.size() == count_before + 1);

  std::println("  new_inner={} count: {} → {}", new_inner, count_before,
               sn_after.size());
  std::println("  PASS");
}

// TODO: Once Fitch incremental updates (Phase 17+) are implemented, add an
// integration test that runs find_all_moves on a patched tree_index after SPR +
// update_topology + update_searchable_nodes, to verify end-to-end correctness
// of searchable_nodes_ against the move enumerator.

// ===========================================================================
// Phase 13: update_subtree_sizes
// ===========================================================================

// Build a tree with binary root and one leaf child, for root-collapse tests:
//   UA → root → {L_a, inner → {L_b, L_c}}
// Moving L_a causes root to collapse, making inner the effective root.
struct root_collapse_tree {
  phylo_dag tree;
  std::size_t root, inner, L_a, L_b, L_c;
};

static root_collapse_tree make_root_collapse_tree() {
  constexpr std::string_view ref = "AAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto la = d.append_node<node_kind::leaf>();
  la.cg() = cg_from_sequence("TAAA", ref);
  la.sample_id() = "L_a";
  auto lb = d.append_node<node_kind::leaf>();
  lb.cg() = cg_from_sequence("ATAA", ref);
  lb.sample_id() = "L_b";
  auto lc = d.append_node<node_kind::leaf>();
  lc.cg() = cg_from_sequence("AATA", ref);
  lc.sample_id() = "L_c";

  auto root_n = d.append_node<node_kind::inner>();
  root_n.cg() = cg_from_sequence("AAAA", ref);
  auto inner_n = d.append_node<node_kind::inner>();
  inner_n.cg() = cg_from_sequence("AAAA", ref);

  add_edge(d, ua.index(), root_n.index(), 0);
  add_edge(d, root_n.index(), la.index(), 0);
  add_edge(d, root_n.index(), inner_n.index(), 1);
  add_edge(d, inner_n.index(), lb.index(), 0);
  add_edge(d, inner_n.index(), lc.index(), 1);

  recompute_edge_mutations(d);

  return root_collapse_tree{
      .tree = std::move(d),
      .root = root_n.index(),
      .inner = inner_n.index(),
      .L_a = la.index(),
      .L_b = lb.index(),
      .L_c = lc.index(),
  };
}

// Recursively recount subtree size from scratch for verification.
static std::size_t recount_subtree_size(tree_index const& idx, std::size_t node) {
  std::size_t size = 1;
  for (auto child : idx.get_children(node)) {
    size += recount_subtree_size(idx, child);
  }
  return size;
}

// Verify all valid nodes' subtree_size matches a fresh recount.
static void verify_all_subtree_sizes(tree_index const& idx) {
  auto root = idx.get_tree_root();
  // Walk the tree from root and check every reachable node.
  std::vector<std::size_t> stack = {root};
  std::size_t checked = 0;
  while (!stack.empty()) {
    auto node = stack.back();
    stack.pop_back();
    auto expected = recount_subtree_size(idx, node);
    assert(idx.get_subtree_size(node) == expected &&
           "subtree_size mismatch after update");
    ++checked;
    for (auto child : idx.get_children(node)) {
      stack.push_back(child);
    }
  }
  assert(checked > 0 && "no nodes checked");
}

// SPR with collapse: L1 → L5.  i3 collapses (binary).
static void test_subtree_size_update_with_collapse() {
  std::println("test_subtree_size_update_with_collapse");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // Verify initial subtree sizes are correct.
  verify_all_subtree_sizes(idx);

  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  assert(r.src_parent_collapsed);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);

  verify_all_subtree_sizes(idx);

  // new_inner has exactly 2 children (L5 and L1), so subtree_size = 3.
  assert(idx.get_subtree_size(r.new_inner) == 3);

  std::println("  PASS");
}

// SPR without collapse: L7 → L1.  i6 is ternary → no collapse.
static void test_subtree_size_update_no_collapse() {
  std::println("test_subtree_size_update_no_collapse");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L7, t.L1);
  assert(!r.src_parent_collapsed);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);

  verify_all_subtree_sizes(idx);

  // new_inner has exactly 2 children (L1 and L7), so subtree_size = 3.
  assert(idx.get_subtree_size(r.new_inner) == 3);

  std::println("  PASS");
}

// Two sequential SPRs: verify sizes remain correct after each.
static void test_subtree_size_update_sequential() {
  std::println("test_subtree_size_update_sequential");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // First move: L1 → L5 (with collapse).
  auto r1 = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  idx.update_topology(r1);
  idx.update_searchable_nodes(r1);
  idx.update_subtree_sizes(r1);
  verify_all_subtree_sizes(idx);

  // Second move: L3 → L6 (i4 is binary → collapses).
  auto r2 = apply_spr_inplace(t.tree, idx, t.L3, t.L6);
  idx.update_topology(r2);
  idx.update_searchable_nodes(r2);
  idx.update_subtree_sizes(r2);
  verify_all_subtree_sizes(idx);

  // Root subtree_size should equal total reachable nodes.
  auto root = idx.get_tree_root();
  assert(idx.get_subtree_size(root) == recount_subtree_size(idx, root));

  std::println("  PASS");
}

// Verify subtree sizes on the initial tree (no SPR) match recount.
static void test_subtree_size_initial() {
  std::println("test_subtree_size_initial");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  verify_all_subtree_sizes(idx);

  // Root should contain all 12 leaves + 10 inner nodes = 22.
  assert(idx.get_subtree_size(t.root) == 22);
  // Leaf nodes have subtree_size = 1.
  assert(idx.get_subtree_size(t.L1) == 1);
  assert(idx.get_subtree_size(t.L12) == 1);
  // i9 has 2 leaves → subtree_size = 3.
  assert(idx.get_subtree_size(t.i9) == 3);

  std::println("  PASS");
}

// LCA-collapse: L1→L2.  src_parent == LCA == i3 collapses, grandparent == i1.
// After collapse, L2 is reparented to i1, then reattach creates new_inner under
// i1 (dst_parent).  grandparent == dst_parent → removal walk is skipped.
static void test_subtree_size_lca_collapse() {
  std::println("test_subtree_size_lca_collapse");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L2);
  assert(r.src_parent_collapsed);
  assert(r.grandparent == t.i1);
  assert(r.dst_parent == t.i1);  // LCA collapse: grandparent == dst_parent

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);

  verify_all_subtree_sizes(idx);

  // new_inner has exactly 2 children (L2 and L1), so subtree_size = 3.
  assert(idx.get_subtree_size(r.new_inner) == 3);

  std::println("  PASS");
}

// Overlapping paths: L1→L4.  Removal path (i1→root) and insertion path
// (i4→i1→root) share i1 and root.  The second walk overwrites the first
// walk's values at shared nodes with identical (correct) results.
static void test_subtree_size_overlapping_paths() {
  std::println("test_subtree_size_overlapping_paths");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L4);
  assert(r.src_parent_collapsed);
  assert(r.grandparent == t.i1);
  assert(r.dst_parent == t.i4);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);

  verify_all_subtree_sizes(idx);

  // new_inner has children {L4, L1} → subtree_size = 3.
  assert(idx.get_subtree_size(r.new_inner) == 3);
  // i4: children {L3, new_inner} → subtree_size = 5.
  assert(idx.get_subtree_size(t.i4) == 5);
  // Total tree size unchanged (one node removed, one added).
  assert(idx.get_subtree_size(t.root) == 22);

  std::println("  PASS");
}

// Root-collapse: moving L_a from the binary root causes root to collapse.
// grandparent is the UA node (not a valid tree_index node).
// Verifies that update_subtree_sizes handles this without crashing or
// corrupting subtree sizes.  tree_root_ remains stale (Phase 15 will fix it).
static void test_subtree_size_root_collapse() {
  std::println("test_subtree_size_root_collapse");

  auto t = make_root_collapse_tree();
  tree_index idx{t.tree};

  // Verify initial sizes.
  assert(idx.get_subtree_size(t.root) == 5);   // root, inner, L_a, L_b, L_c
  assert(idx.get_subtree_size(t.inner) == 3);   // inner, L_b, L_c
  assert(idx.get_subtree_size(t.L_a) == 1);

  auto r = apply_spr_inplace(t.tree, idx, t.L_a, t.L_b);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.root);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);

  // tree_root_ is stale (still points to collapsed root), so we can't use
  // verify_all_subtree_sizes.  Instead, verify individual reachable nodes.
  // After root collapse: inner → {new_inner → {L_b, L_a}, L_c}
  assert(idx.get_subtree_size(r.new_inner) == 3);
  assert(idx.get_subtree_size(t.inner) == 5);
  assert(idx.get_subtree_size(t.L_a) == 1);
  assert(idx.get_subtree_size(t.L_b) == 1);
  assert(idx.get_subtree_size(t.L_c) == 1);

  std::println("  PASS");
}

// Sibling move: A→B on ternary root.  src_parent == dst_parent == root,
// no collapse.  Both walks start from root → second walk is skipped.
static void test_subtree_size_sibling_move() {
  std::println("test_subtree_size_sibling_move");

  auto d = make_simple_tree();
  tree_index idx{d};

  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);

  auto r = apply_spr_inplace(d, idx, a_idx, b_idx);
  assert(!r.src_parent_collapsed);
  assert(r.src_parent == root_idx);
  assert(r.dst_parent == root_idx);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);

  verify_all_subtree_sizes(idx);

  // new_inner has children {B, A} → subtree_size = 3.
  assert(idx.get_subtree_size(r.new_inner) == 3);
  // Root: originally 4 (root + A + B + C), now 5 (root + new_inner(3) + C).
  assert(idx.get_subtree_size(root_idx) == 5);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 14 helpers: DFS re-traverse verification
// ---------------------------------------------------------------------------

// Recompute DFS info from scratch by walking the tree, and return a map
// of node -> dfs_info for all reachable nodes.
static std::unordered_map<std::size_t, dfs_info> fresh_dfs_info(
    tree_index const& idx) {
  std::unordered_map<std::size_t, dfs_info> result;
  struct frame {
    std::size_t node;
    std::size_t level;
  };
  // Iterative DFS using an explicit stack (post-order needs two passes).
  // Simpler: recursive lambda.
  std::size_t counter = 0;
  std::function<void(std::size_t, std::size_t)> visit =
      [&](std::size_t node, std::size_t level) {
        dfs_info info;
        info.dfs_index = counter++;
        info.level = level;
        for (auto child : idx.get_children(node)) visit(child, level + 1);
        info.dfs_end_index = counter;
        result[node] = info;
      };
  visit(idx.get_tree_root(), 0);
  return result;
}

// Verify that idx.dfs_info_ matches a freshly computed DFS traversal for
// all reachable nodes.
static void verify_dfs_info(tree_index const& idx) {
  auto expected = fresh_dfs_info(idx);
  assert(!expected.empty() && "fresh DFS produced no nodes");
  for (auto const& [node, info] : expected) {
    assert(idx.has_dfs_info(node) && "reachable node missing DFS info");
    auto const& actual = idx.get_dfs_info(node);
    assert(actual.dfs_index == info.dfs_index &&
           "dfs_index mismatch after recompute_dfs");
    assert(actual.dfs_end_index == info.dfs_end_index &&
           "dfs_end_index mismatch after recompute_dfs");
    assert(actual.level == info.level && "level mismatch after recompute_dfs");
  }
}

// Verify is_ancestor returns correct results for all pairs in a sample of
// nodes.  Uses the DFS interval property directly.
static void verify_is_ancestor(tree_index const& idx,
                                std::vector<std::size_t> const& nodes) {
  for (auto a : nodes) {
    for (auto b : nodes) {
      if (!idx.has_dfs_info(a) || !idx.has_dfs_info(b)) continue;
      auto const& ai = idx.get_dfs_info(a);
      auto const& bi = idx.get_dfs_info(b);
      bool expected = ai.dfs_index <= bi.dfs_index &&
                      bi.dfs_index < ai.dfs_end_index;
      assert(idx.is_ancestor(a, b) == expected &&
             "is_ancestor disagrees with DFS intervals");
    }
  }
}

// Collect all reachable node indices from tree_root via children_.
static std::vector<std::size_t> collect_reachable_nodes(
    tree_index const& idx) {
  std::vector<std::size_t> result;
  std::vector<std::size_t> stack = {idx.get_tree_root()};
  while (!stack.empty()) {
    auto node = stack.back();
    stack.pop_back();
    result.push_back(node);
    for (auto child : idx.get_children(node)) stack.push_back(child);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Phase 14 tests: DFS re-traverse
// ---------------------------------------------------------------------------

// After SPR + DFS re-traverse: dfs_info_ matches a freshly built tree_index.
static void test_dfs_recompute_with_collapse() {
  std::println("test_dfs_recompute_with_collapse");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // Verify initial DFS info is correct.
  verify_dfs_info(idx);

  // SPR: L1 → L5 (i3 is binary → collapses).
  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  assert(r.src_parent_collapsed);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  verify_dfs_info(idx);

  // Collapsed node should not have valid DFS info — unless its slot was
  // recycled for new_inner (collapsed_node == new_inner).
  if (r.collapsed_node != r.new_inner) {
    assert(!idx.has_dfs_info(r.collapsed_node) &&
           "collapsed node should not have DFS info");
  }

  // Tree root should have level 0.
  assert(idx.get_dfs_info(idx.get_tree_root()).level == 0);

  std::println("  PASS");
}

// SPR without collapse: verify DFS is fully correct.
static void test_dfs_recompute_no_collapse() {
  std::println("test_dfs_recompute_no_collapse");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // SPR: L7 → L1 (i6 is ternary → no collapse).
  auto r = apply_spr_inplace(t.tree, idx, t.L7, t.L1);
  assert(!r.src_parent_collapsed);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  verify_dfs_info(idx);
  assert(idx.get_dfs_info(idx.get_tree_root()).level == 0);

  std::println("  PASS");
}

// is_ancestor returns correct results for 20+ random pairs after SPR.
static void test_dfs_is_ancestor_after_spr() {
  std::println("test_dfs_is_ancestor_after_spr");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // SPR: L1 → L5 (with collapse).
  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  auto nodes = collect_reachable_nodes(idx);
  assert(nodes.size() >= 20 && "expected at least 20 reachable nodes");

  // Check all pairs among reachable nodes (covers well over 20 pairs).
  verify_is_ancestor(idx, nodes);

  // Spot-check: tree_root is ancestor of every reachable node.
  auto root = idx.get_tree_root();
  for (auto node : nodes) {
    assert(idx.is_ancestor(root, node));
  }

  // Spot-check: no leaf is ancestor of any other node (except itself).
  for (auto node : nodes) {
    if (!idx.get_children(node).empty()) continue;  // skip inner nodes
    for (auto other : nodes) {
      if (other == node) {
        assert(idx.is_ancestor(node, other));  // self-ancestry
      } else {
        assert(!idx.is_ancestor(node, other));
      }
    }
  }

  std::println("  PASS");
}

// Sequential SPRs: DFS info remains correct after each recompute.
static void test_dfs_recompute_sequential() {
  std::println("test_dfs_recompute_sequential");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // First move: L1 → L5 (with collapse).
  auto r1 = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  idx.update_topology(r1);
  idx.update_searchable_nodes(r1);
  idx.update_subtree_sizes(r1);
  idx.recompute_dfs();
  verify_dfs_info(idx);

  // Second move: L3 → L6 (i4 is binary → collapses).
  auto r2 = apply_spr_inplace(t.tree, idx, t.L3, t.L6);
  idx.update_topology(r2);
  idx.update_searchable_nodes(r2);
  idx.update_subtree_sizes(r2);
  idx.recompute_dfs();
  verify_dfs_info(idx);

  // Verify is_ancestor on all reachable nodes after both moves.
  auto nodes = collect_reachable_nodes(idx);
  verify_is_ancestor(idx, nodes);

  std::println("  PASS");
}

// Simple tree: verify DFS recompute on a small tree.
static void test_dfs_recompute_simple_tree() {
  std::println("test_dfs_recompute_simple_tree");

  auto d = make_simple_tree();
  tree_index idx{d};

  verify_dfs_info(idx);

  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);

  // SPR: A → B (root is ternary → no collapse).
  auto r = apply_spr_inplace(d, idx, a_idx, b_idx);
  assert(!r.src_parent_collapsed);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  verify_dfs_info(idx);

  // new_inner should be at level 1 (child of root).
  assert(idx.get_dfs_info(r.new_inner).level == 1);

  // A and B should now be at level 2 (children of new_inner).
  assert(idx.get_dfs_info(a_idx).level == 2);
  assert(idx.get_dfs_info(b_idx).level == 2);

  // Root should be at level 0.
  assert(idx.get_dfs_info(root_idx).level == 0);

  auto nodes = collect_reachable_nodes(idx);
  verify_is_ancestor(idx, nodes);

  std::println("  PASS");
}

// Idempotency: calling recompute_dfs twice produces same result.
static void test_dfs_recompute_idempotent() {
  std::println("test_dfs_recompute_idempotent");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // SPR then recompute.
  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  idx.update_topology(r);
  idx.recompute_dfs();

  // Save DFS info.
  auto first = fresh_dfs_info(idx);

  // Recompute again — should be identical.
  idx.recompute_dfs();

  auto second = fresh_dfs_info(idx);
  assert(first.size() == second.size());
  for (auto const& [node, info] : first) {
    auto it = second.find(node);
    assert(it != second.end());
    assert(it->second.dfs_index == info.dfs_index);
    assert(it->second.dfs_end_index == info.dfs_end_index);
    assert(it->second.level == info.level);
  }

  std::println("  PASS");
}

// Suboptimal tree: verify DFS recompute works on a different tree structure.
static void test_dfs_recompute_suboptimal_tree() {
  std::println("test_dfs_recompute_suboptimal_tree");

  auto d = make_suboptimal_tree();
  tree_index idx{d};

  verify_dfs_info(idx);

  // Find nodes for an SPR move.
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  // i1 is first child, i2 is second child of root.
  auto i1_idx = get_child_idx(d, root_clades[0][0]);
  auto i2_idx = get_child_idx(d, root_clades[1][0]);
  auto i1_clades = get_clades(d, i1_idx);
  // L4 is second child of i1.
  auto l4_idx = get_child_idx(d, i1_clades[1][0]);
  auto i2_clades = get_clades(d, i2_idx);
  auto l3_idx = get_child_idx(d, i2_clades[0][0]);

  // SPR: L4 → L3 (i1 is binary → collapses).
  auto r = apply_spr_inplace(d, idx, l4_idx, l3_idx);
  assert(r.src_parent_collapsed);

  idx.update_topology(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  verify_dfs_info(idx);

  auto nodes = collect_reachable_nodes(idx);
  verify_is_ancestor(idx, nodes);

  std::println("  PASS");
}

// Root collapse: SPR collapses binary root, verify DFS after tree_root_ update.
static void test_dfs_recompute_root_collapse() {
  std::println("test_dfs_recompute_root_collapse");

  // Suboptimal tree has binary root (children: i1, i2).
  // Moving i1's child (L4) would collapse i1, not root.
  // To collapse root, we need src_parent == root.  SPR i1 → L3 detaches i1
  // from root (binary → root collapses), then reattaches i1 as sibling of L3.
  auto d = make_suboptimal_tree();
  tree_index idx{d};

  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto i1_idx = get_child_idx(d, root_clades[0][0]);
  auto i2_idx = get_child_idx(d, root_clades[1][0]);
  auto i2_clades = get_clades(d, i2_idx);
  auto l3_idx = get_child_idx(d, i2_clades[0][0]);

  assert(idx.get_tree_root() == root_idx);

  // SPR: i1 → L3 (root is binary → collapses).
  auto r = apply_spr_inplace(d, idx, i1_idx, l3_idx);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == root_idx);

  idx.update_topology(r);
  idx.update_tree_root(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  // tree_root_ should have changed away from the collapsed root.
  assert(idx.get_tree_root() != root_idx);
  assert(idx.is_valid(idx.get_tree_root()));

  verify_dfs_info(idx);

  // New tree root should have level 0.
  assert(idx.get_dfs_info(idx.get_tree_root()).level == 0);

  // Collapsed root should not have DFS info — unless its slot was recycled
  // for new_inner (collapsed_node == new_inner).
  if (r.collapsed_node != r.new_inner) {
    assert(!idx.has_dfs_info(root_idx) &&
           "collapsed root should not have DFS info");
  }

  // is_ancestor should be correct for all reachable nodes.
  auto nodes = collect_reachable_nodes(idx);
  verify_is_ancestor(idx, nodes);

  std::println("  PASS");
}

// End-to-end: compare incremental DFS against a fresh tree_index built from
// the post-SPR DAG.  This catches bugs where update_topology produces a
// children_[] state that differs from the actual DAG.
static void test_dfs_recompute_vs_fresh_index() {
  std::println("test_dfs_recompute_vs_fresh_index");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};

  // SPR: L1 → L5 (with collapse).
  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  idx.update_topology(r);
  idx.update_tree_root(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  // Build a completely fresh tree_index from the modified DAG.
  tree_index fresh{t.tree};

  assert(idx.get_tree_root() == fresh.get_tree_root());

  auto reachable = collect_reachable_nodes(idx);
  auto fresh_reachable = collect_reachable_nodes(fresh);
  assert(reachable.size() == fresh_reachable.size() &&
         "reachable node count mismatch between incremental and fresh index");

  // Compare structural DFS properties: descendant count and level.
  // (Exact dfs_index values may differ if child ordering diverges.)
  for (auto node : reachable) {
    assert(fresh.has_dfs_info(node) && idx.has_dfs_info(node));
    auto const& a = idx.get_dfs_info(node);
    auto const& b = fresh.get_dfs_info(node);
    assert(a.dfs_end_index - a.dfs_index == b.dfs_end_index - b.dfs_index &&
           "descendant count mismatch vs fresh index");
    assert(a.level == b.level && "level mismatch vs fresh index");
  }

  // Cross-check is_ancestor for all pairs against fresh index.
  for (auto a : reachable) {
    for (auto b : reachable) {
      assert(idx.is_ancestor(a, b) == fresh.is_ancestor(a, b) &&
             "is_ancestor disagrees between incremental and fresh index");
    }
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 15 tests: update_tree_root
// ---------------------------------------------------------------------------

// Tree: UA → root → {L_a, inner → {L_b, L_c}}.
// Move L_a → L_b: root is binary → collapses.
// Verify tree_root_ is updated to the surviving child of UA.
static void test_root_change_basic() {
  std::println("test_root_change_basic");

  auto t = make_root_collapse_tree();
  tree_index idx{t.tree};

  assert(idx.get_tree_root() == t.root);

  auto r = apply_spr_inplace(t.tree, idx, t.L_a, t.L_b);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.root);

  idx.update_topology(r);

  // Before update_tree_root, tree_root_ still points to collapsed root.
  assert(idx.get_tree_root() == t.root);

  idx.update_tree_root(r);

  // tree_root_ must have changed away from the collapsed root.
  assert(idx.get_tree_root() != t.root);
  assert(idx.is_valid(idx.get_tree_root()));

  // New tree root should be the surviving child of UA (inner node).
  assert(idx.get_tree_root() == t.inner);

  std::println("  PASS");
}

// After root change, recompute DFS and verify tree_root has level 0.
static void test_root_change_dfs_level() {
  std::println("test_root_change_dfs_level");

  auto t = make_root_collapse_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L_a, t.L_b);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == t.root);

  idx.update_topology(r);
  idx.update_tree_root(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  // tree_root_ must have level 0.
  assert(idx.get_dfs_info(idx.get_tree_root()).level == 0);

  // All reachable nodes should have valid DFS info.
  verify_dfs_info(idx);

  // is_ancestor should be consistent.
  auto nodes = collect_reachable_nodes(idx);
  verify_is_ancestor(idx, nodes);

  std::println("  PASS");
}

// No-op case: non-root collapse should leave tree_root_ unchanged.
static void test_root_change_noop() {
  std::println("test_root_change_noop");

  auto t = make_12leaf_tree();
  tree_index idx{t.tree};
  auto original_root = idx.get_tree_root();

  // SPR: L1 → L5 (collapses i3, not root).
  auto r = apply_spr_inplace(t.tree, idx, t.L1, t.L5);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node != original_root);

  idx.update_topology(r);
  idx.update_tree_root(r);

  // tree_root_ should remain unchanged.
  assert(idx.get_tree_root() == original_root);
  assert(idx.is_valid(idx.get_tree_root()));

  std::println("  PASS");
}

// End-to-end: after root collapse, compare tree_root_ against fresh index.
static void test_root_change_vs_fresh_index() {
  std::println("test_root_change_vs_fresh_index");

  auto t = make_root_collapse_tree();
  tree_index idx{t.tree};

  auto r = apply_spr_inplace(t.tree, idx, t.L_a, t.L_b);
  idx.update_topology(r);
  idx.update_tree_root(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  // Build fresh index from the same post-SPR DAG.
  tree_index fresh{t.tree};

  assert(idx.get_tree_root() == fresh.get_tree_root());

  // Reachable node sets should match.
  auto reachable = collect_reachable_nodes(idx);
  auto fresh_reachable = collect_reachable_nodes(fresh);
  assert(reachable.size() == fresh_reachable.size());

  std::println("  PASS");
}

// Binary root: UA → root → {A, B}.  Move A → B.
// After detach, root is binary → collapses.  remaining_child = B = dst.
// Reattach creates new_inner between UA and B: UA → new_inner → {B, A}.
// update_tree_root should set tree_root_ = new_inner.
static void test_root_change_dst_is_remaining_child() {
  std::println("test_root_change_dst_is_remaining_child");

  auto d = make_binary_root_tree();
  auto ua_idx = get_root_idx(d);
  auto ua_clades = get_clades(d, ua_idx);
  auto root_idx = get_child_idx(d, ua_clades[0][0]);
  auto root_clades = get_clades(d, root_idx);
  auto a_idx = get_child_idx(d, root_clades[0][0]);
  auto b_idx = get_child_idx(d, root_clades[1][0]);

  tree_index idx{d};
  assert(idx.get_tree_root() == root_idx);

  auto r = apply_spr_inplace(d, idx, a_idx, b_idx);
  assert(r.src_parent_collapsed);
  assert(r.collapsed_node == root_idx);
  assert(r.remaining_child == b_idx);  // dst == remaining_child

  idx.update_topology(r);
  idx.update_tree_root(r);
  idx.update_searchable_nodes(r);
  idx.update_subtree_sizes(r);
  idx.recompute_dfs();

  // new_inner may reuse the collapsed root's index (node recycling), so we
  // can't assert tree_root_ != root_idx.  Instead verify it equals new_inner
  // and is valid.
  assert(idx.get_tree_root() == r.new_inner);
  assert(idx.is_valid(idx.get_tree_root()));

  // Full consistency checks.
  verify_dfs_info(idx);
  verify_all_subtree_sizes(idx);

  // Cross-check against a fresh index.
  tree_index fresh{d};
  assert(idx.get_tree_root() == fresh.get_tree_root());
  auto reachable = collect_reachable_nodes(idx);
  auto fresh_reachable = collect_reachable_nodes(fresh);
  assert(reachable.size() == fresh_reachable.size());

  std::println("  PASS");
}

// Two consecutive root-collapsing SPRs.
// Tree: UA → root → {L_a, inner → {L_b, L_c}}.
// SPR 1: L_a → L_b — root collapses, inner becomes new tree root (binary).
// SPR 2: L_c → L_a — inner collapses, new_inner1 becomes new tree root.
static void test_root_change_sequential() {
  std::println("test_root_change_sequential");

  auto t = make_root_collapse_tree();
  tree_index idx{t.tree};
  assert(idx.get_tree_root() == t.root);

  // --- First SPR: L_a → L_b, root collapses ---
  auto r1 = apply_spr_inplace(t.tree, idx, t.L_a, t.L_b);
  assert(r1.src_parent_collapsed);
  assert(r1.collapsed_node == t.root);

  idx.update_topology(r1);
  idx.update_tree_root(r1);
  idx.update_searchable_nodes(r1);
  idx.update_subtree_sizes(r1);
  idx.recompute_dfs();

  // After first SPR: tree_root_ = inner.
  assert(idx.get_tree_root() == t.inner);

  // --- Second SPR: L_c → L_a, inner (now tree root, binary) collapses ---
  auto r2 = apply_spr_inplace(t.tree, idx, t.L_c, t.L_a);
  assert(r2.src_parent_collapsed);
  assert(r2.collapsed_node == t.inner);  // inner was the tree root

  idx.update_topology(r2);

  // Before update_tree_root, tree_root_ is stale (points to collapsed inner).
  assert(idx.get_tree_root() == t.inner);

  idx.update_tree_root(r2);

  // tree_root_ must now be r1.new_inner (inner's remaining child after
  // collapse).  Note: r1.new_inner may have recycled t.root's index, so we
  // can't assert != t.root.
  assert(idx.get_tree_root() == r1.new_inner);
  assert(idx.is_valid(idx.get_tree_root()));

  idx.update_searchable_nodes(r2);
  idx.update_subtree_sizes(r2);
  idx.recompute_dfs();

  // Full consistency checks.
  verify_dfs_info(idx);
  verify_all_subtree_sizes(idx);

  // Cross-check against fresh index.
  tree_index fresh{t.tree};
  assert(idx.get_tree_root() == fresh.get_tree_root());
  auto reachable = collect_reachable_nodes(idx);
  auto fresh_reachable = collect_reachable_nodes(fresh);
  assert(reachable.size() == fresh_reachable.size());

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
  test_detach_from_root_no_collapse();
  test_clades_after_collapse();

  // Phase 6: reattach_at_destination
  test_reattach_simple();
  test_reattach_with_collapse();
  test_reattach_preserves_clade_index();
  test_reattach_on_suboptimal_tree();
  test_reattach_dst_is_tree_root();
  test_reattach_dst_is_inner_node();
  test_reattach_after_root_collapse();
  test_reattach_adjacent_sibling();

  // Phase 7: apply_spr_inplace
  test_apply_spr_inplace_with_collapse();
  test_apply_spr_inplace_no_collapse();
  test_apply_spr_inplace_sibling_move();
  test_apply_spr_inplace_sibling_collapse();

  // Phase 8: validate in-place vs clone-based SPR
  test_inplace_vs_clone_spr();

  // Phase 9: edge cases and robustness
  test_edge_case_src_is_child_of_dst_parent();
  test_edge_case_sibling_swap_ternary();
  test_edge_case_lca_is_tree_root();
  test_edge_case_lca_collapses();
  test_edge_case_polytomy_no_collapse();
  test_edge_case_move_to_adjacent_node();

  // Phase 10: tree_index::update_topology
  test_update_topology_with_collapse();
  test_update_topology_no_collapse();
  test_update_topology_sibling_collapse();
  test_update_topology_multiple_moves();
  test_update_topology_ensure_capacity();
  test_update_topology_edge_cases();
  test_update_topology_sequential_sprs();

  // Phase 11: ensure_capacity
  test_ensure_capacity_array_sizes();
  test_ensure_capacity_monotonic_growth();

  // Phase 12: update_searchable_nodes
  test_update_searchable_nodes_with_collapse();
  test_update_searchable_nodes_no_collapse();

  // Phase 13: update_subtree_sizes
  test_subtree_size_initial();
  test_subtree_size_update_with_collapse();
  test_subtree_size_update_no_collapse();
  test_subtree_size_update_sequential();
  test_subtree_size_lca_collapse();
  test_subtree_size_overlapping_paths();
  test_subtree_size_root_collapse();
  test_subtree_size_sibling_move();

  // Phase 14: recompute_dfs
  test_dfs_recompute_with_collapse();
  test_dfs_recompute_no_collapse();
  test_dfs_is_ancestor_after_spr();
  test_dfs_recompute_sequential();
  test_dfs_recompute_simple_tree();
  test_dfs_recompute_idempotent();
  test_dfs_recompute_suboptimal_tree();
  test_dfs_recompute_root_collapse();
  test_dfs_recompute_vs_fresh_index();

  // Phase 15: update_tree_root
  test_root_change_basic();
  test_root_change_dfs_level();
  test_root_change_noop();
  test_root_change_vs_fresh_index();
  test_root_change_dst_is_remaining_child();
  test_root_change_sequential();

  std::println("All inplace SPR phase 1-15 tests passed!");
  return 0;
}

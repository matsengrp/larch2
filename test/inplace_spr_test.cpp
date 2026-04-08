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

  // Phase 3: is_valid_
  test_is_valid_initialization();
  test_is_valid_all_tree_nodes();
  test_is_valid_out_of_range();

  std::println("All inplace SPR phase 1-3 tests passed!");
  return 0;
}

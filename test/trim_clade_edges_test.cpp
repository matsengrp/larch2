#include <larch/compute.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/merge.hpp>
#include <larch/subtree_weight.hpp>

#include "test_util.hpp"

#include <cassert>
#include <cstring>
#include <print>
#include <set>
#include <string>

using namespace larch;
using larch::test::cg_from_sequence;

static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx, std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
}

static std::set<std::string> get_leaf_ids(phylo_dag& d) {
  std::set<std::string> ids;
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); })
            ids.insert(node.sample_id());
        },
        nv);
  }
  return ids;
}

struct inconsistent_dag {
  phylo_dag d;
  std::size_t inner1_idx;
  std::size_t inner2_idx;
};

// Build a small DAG where one UA alternative is missing a leaf.
//
//   UA ─── clade 0 ──→ inner1 ──→ {leaf1, leaf2, leaf3}   (good)
//   UA ─── clade 0 ──→ inner2 ──→ {leaf1, leaf2}          (bad)
//
static inconsistent_dag make_inconsistent_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto inner1 = d.append_node<node_kind::inner>();
  inner1.cg() = cg_from_sequence("GAA", ref);

  auto inner2 = d.append_node<node_kind::inner>();
  inner2.cg() = cg_from_sequence("GAA", ref);

  auto leaf1 = d.append_node<node_kind::leaf>();
  leaf1.cg() = cg_from_sequence("ACC", ref);
  leaf1.sample_id() = "leaf1";

  auto leaf2 = d.append_node<node_kind::leaf>();
  leaf2.cg() = cg_from_sequence("GTT", ref);
  leaf2.sample_id() = "leaf2";

  auto leaf3 = d.append_node<node_kind::leaf>();
  leaf3.cg() = cg_from_sequence("AAA", ref);
  leaf3.sample_id() = "leaf3";

  add_edge(d, ua.index(), inner1.index(), 0);
  add_edge(d, ua.index(), inner2.index(), 0);

  add_edge(d, inner1.index(), leaf1.index(), 0);
  add_edge(d, inner1.index(), leaf2.index(), 1);
  add_edge(d, inner1.index(), leaf3.index(), 2);

  add_edge(d, inner2.index(), leaf1.index(), 0);
  add_edge(d, inner2.index(), leaf2.index(), 1);

  recompute_edge_mutations(d);
  return {std::move(d), inner1.index(), inner2.index()};
}

// Build a DAG where ALL alternatives in a clade are incomplete (neither
// has the full leaf set).
//
//   UA ─── clade 0 ──→ inner1 ──→ {leaf1, leaf2}   (missing leaf3)
//   UA ─── clade 0 ──→ inner2 ──→ {leaf1, leaf3}   (missing leaf2)
//
static phylo_dag make_all_bad_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto inner1 = d.append_node<node_kind::inner>();
  inner1.cg() = cg_from_sequence("GAA", ref);

  auto inner2 = d.append_node<node_kind::inner>();
  inner2.cg() = cg_from_sequence("GAA", ref);

  auto leaf1 = d.append_node<node_kind::leaf>();
  leaf1.cg() = cg_from_sequence("ACC", ref);
  leaf1.sample_id() = "leaf1";

  auto leaf2 = d.append_node<node_kind::leaf>();
  leaf2.cg() = cg_from_sequence("GTT", ref);
  leaf2.sample_id() = "leaf2";

  auto leaf3 = d.append_node<node_kind::leaf>();
  leaf3.cg() = cg_from_sequence("AAA", ref);
  leaf3.sample_id() = "leaf3";

  add_edge(d, ua.index(), inner1.index(), 0);
  add_edge(d, ua.index(), inner2.index(), 0);

  add_edge(d, inner1.index(), leaf1.index(), 0);
  add_edge(d, inner1.index(), leaf2.index(), 1);

  add_edge(d, inner2.index(), leaf1.index(), 0);
  add_edge(d, inner2.index(), leaf3.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

static void test_compute_subtree_leaves() {
  std::println("test_compute_subtree_leaves");

  auto [d, inner1_idx, inner2_idx] = make_inconsistent_dag();
  auto subtree_leaves = compute_subtree_leaves(d);

  auto root_idx = get_root_idx(d);
  assert(subtree_leaves[root_idx].size() == 3);
  assert(subtree_leaves[inner1_idx].size() == 3);
  assert(subtree_leaves[inner2_idx].size() == 2);

  std::println("  PASS");
}

static void test_trim_removes_bad_edge() {
  std::println("test_trim_removes_bad_edge");

  auto [d, inner1_idx, inner2_idx] = make_inconsistent_dag();
  auto tr = trim_inconsistent_clade_edges(d);
  assert(tr.edges_removed == 1);
  assert(tr.unresolvable_clades == 0);

  auto remaining = get_leaf_ids(d);
  assert(remaining == (std::set<std::string>{"leaf1", "leaf2", "leaf3"}));

  std::println("  PASS");
}

static void test_trim_idempotent() {
  std::println("test_trim_idempotent");

  auto [d, inner1_idx, inner2_idx] = make_inconsistent_dag();
  auto tr1 = trim_inconsistent_clade_edges(d);
  assert(tr1.edges_removed == 1);

  auto tr2 = trim_inconsistent_clade_edges(d);
  assert(tr2.edges_removed == 0);
  assert(tr2.unresolvable_clades == 0);

  std::println("  PASS");
}

static void test_trim_all_bad_reports_unresolvable() {
  std::println("test_trim_all_bad_reports_unresolvable");

  auto d = make_all_bad_dag();
  auto tr = trim_inconsistent_clade_edges(d);
  assert(tr.edges_removed == 0);
  assert(tr.unresolvable_clades == 1);

  std::println("  PASS");
}

static void test_validate_catches_inconsistency() {
  std::println("test_validate_catches_inconsistency");

  auto [d, inner1_idx, inner2_idx] = make_inconsistent_dag();
  bool caught = false;
  try {
    validate_dag(d, "inconsistent");
  } catch (std::runtime_error const& e) {
    caught = true;
    assert(std::strstr(e.what(), "different leaf sets") != nullptr);
  }
  assert(caught);

  std::println("  PASS");
}

static void test_trim_then_validate() {
  std::println("test_trim_then_validate");

  auto [d, inner1_idx, inner2_idx] = make_inconsistent_dag();
  trim_inconsistent_clade_edges(d);
  validate_dag(d, "after trim");

  std::println("  PASS");
}

static void test_trim_then_sample() {
  std::println("test_trim_then_sample");

  auto [d, inner1_idx, inner2_idx] = make_inconsistent_dag();
  trim_inconsistent_clade_edges(d);
  auto expected_leaves = get_leaf_ids(d);

  for (std::uint32_t seed = 0; seed < 20; ++seed) {
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(d, seed);
    auto tree = sw.sample_tree(pops);
    assert(is_tree(tree));
    assert(get_leaf_ids(tree) == expected_leaves);
  }

  std::println("  PASS");
}

// Shared test logic for real-world reproducer DAGs.
static void run_real_dag_trim_test(std::string_view path,
                                   std::size_t expected_leaves) {
  auto d = load_proto_dag(path);

  auto num_leaves = leaf_count(d);
  std::println("  DAG: {} nodes, {} edges, {} leaves", node_count(d),
               edge_count(d), num_leaves);
  assert(num_leaves == expected_leaves);

  // Before trim: some min-weight samples should be incomplete.
  {
    bool found_incomplete = false;
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
      parsimony_score_ops pops;
      subtree_weight<parsimony_score_ops> sw(d, seed);
      auto tree = sw.min_weight_sample_tree(pops);
      if (leaf_count(tree) < num_leaves) {
        found_incomplete = true;
        break;
      }
    }
    assert(found_incomplete);
    std::println("  confirmed: untrimmed DAG produces incomplete trees");
  }

  // Trim and verify.
  auto tr = trim_inconsistent_clade_edges(d);
  std::println("  trimmed {} edges ({} unresolvable clades)", tr.edges_removed,
               tr.unresolvable_clades);
  assert(tr.edges_removed > 0);
  assert(tr.unresolvable_clades == 0);

  // After trim: all min-weight samples should be complete.
  auto expected_ids = get_leaf_ids(d);
  for (std::uint32_t seed = 0; seed < 100; ++seed) {
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(d, seed);
    auto tree = sw.min_weight_sample_tree(pops);
    assert(is_tree(tree));
    assert(get_leaf_ids(tree) == expected_ids);
  }
  std::println("  100 min-weight samples after trim: all have {} leaves",
               num_leaves);
}

static void test_fluC_PB2_7taxa() {
  std::println("test_fluC_PB2_7taxa (reproducer DAG)");
  run_real_dag_trim_test("data/madag/fluC_PB2_7taxa.pb", 7);
  std::println("  PASS");
}

static void test_fluC_PB2_10taxa() {
  std::println("test_fluC_PB2_10taxa (10-taxa reproducer DAG)");
  run_real_dag_trim_test("data/madag/fluC_PB2_10taxa.pb", 10);
  std::println("  PASS");
}

// Verify that merge::get_result() returns a DAG where min-weight sampling
// always produces trees with the correct leaf count (i.e., inconsistent
// clade edges were already trimmed inside get_result()).
static void test_merge_get_result_trims_inconsistent() {
  std::println("test_merge_get_result_trims_inconsistent");

  // Load a DAG known to produce inconsistent edges after merge+SPR.
  // We use one of the reproducer DAGs which was specifically crafted to
  // have incomplete-leaf-set alternatives.
  auto d = load_proto_dag("data/madag/fluC_PB2_7taxa.pb");
  auto num_leaves = leaf_count(d);
  auto ref = get_reference_sequence(d);

  // Merge the DAG through the merge object (same path as larch2 optimizer).
  merge m{ref};
  m.add_dag(d);
  auto& result = m.get_result();

  // All min-weight sampled trees should have the full leaf count.
  for (std::uint32_t seed = 0; seed < 50; ++seed) {
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(result, seed);
    auto tree = sw.min_weight_sample_tree(pops);
    assert(is_tree(tree));
    auto tree_leaves = leaf_count(tree);
    if (tree_leaves != num_leaves) {
      std::println("  FAIL: seed {} produced tree with {} leaves (expected {})",
                   seed, tree_leaves, num_leaves);
      assert(false);
    }
  }
  std::println("  50 samples from merge result: all have {} leaves", num_leaves);
  std::println("  PASS");
}

int main() {
  test_compute_subtree_leaves();
  test_trim_removes_bad_edge();
  test_trim_idempotent();
  test_trim_all_bad_reports_unresolvable();
  test_validate_catches_inconsistency();
  test_trim_then_validate();
  test_trim_then_sample();
  test_fluC_PB2_7taxa();
  test_fluC_PB2_10taxa();
  test_merge_get_result_trims_inconsistent();

  std::println("All trim_clade_edges tests passed!");
  return 0;
}

#include <larch/compute.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/subtree_weight.hpp>

#include <cassert>
#include <map>
#include <print>
#include <set>
#include <string>

using namespace larch;

static compact_genome cg_from_sequence(std::string_view seq,
                                       std::string_view ref) {
  std::map<mutation_position, nuc_base> muts;
  for (std::size_t i = 0; i < seq.size(); ++i) {
    if (seq[i] != ref[i]) {
      muts[i + 1] = nuc_base::from_char(seq[i]);
    }
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

// Build a small DAG where one UA alternative is missing a leaf.
//
//   UA ─── clade 0 ──→ inner1 ──→ {leaf1, leaf2, leaf3}   (good)
//   UA ─── clade 0 ──→ inner2 ──→ {leaf1, leaf2}          (bad: missing leaf3)
//
static phylo_dag make_inconsistent_dag() {
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

  // UA -> inner1 (clade 0, alternative 1)
  add_edge(d, ua.index(), inner1.index(), 0);
  // UA -> inner2 (clade 0, alternative 2) — bad subtree
  add_edge(d, ua.index(), inner2.index(), 0);

  // inner1 -> leaf1, leaf2, leaf3 (complete)
  add_edge(d, inner1.index(), leaf1.index(), 0);
  add_edge(d, inner1.index(), leaf2.index(), 1);
  add_edge(d, inner1.index(), leaf3.index(), 2);

  // inner2 -> leaf1, leaf2 only (missing leaf3)
  add_edge(d, inner2.index(), leaf1.index(), 0);
  add_edge(d, inner2.index(), leaf2.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

static void test_compute_subtree_leaves() {
  std::println("test_compute_subtree_leaves");

  auto d = make_inconsistent_dag();
  auto subtree_leaves = compute_subtree_leaves(d);

  // UA should reach all 3 leaves.
  auto root_idx = get_root_idx(d);
  assert(subtree_leaves[root_idx].size() == 3);

  // inner1 should reach 3 leaves, inner2 should reach 2.
  std::size_t inner1_idx = 0, inner2_idx = 0;
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.cg(); }) {
            if constexpr (!requires { node.sample_id(); }) {
              if (subtree_leaves[node.index()].size() == 3)
                inner1_idx = node.index();
              else if (subtree_leaves[node.index()].size() == 2)
                inner2_idx = node.index();
            }
          }
        },
        nv);
  }

  assert(subtree_leaves[inner1_idx].size() == 3);
  assert(subtree_leaves[inner2_idx].size() == 2);

  std::println("  PASS");
}

static void test_trim_removes_bad_edge() {
  std::println("test_trim_removes_bad_edge");

  auto d = make_inconsistent_dag();
  auto removed = trim_inconsistent_clade_edges(d);
  assert(removed == 1);  // The UA->inner2 edge should be removed.

  // All remaining nodes should be reachable and valid.
  assert(leaf_count(d) == 3);

  std::println("  PASS");
}

static void test_validate_catches_inconsistency() {
  std::println("test_validate_catches_inconsistency");

  auto d = make_inconsistent_dag();
  bool caught = false;
  try {
    validate_dag(d, "inconsistent");
  } catch (std::runtime_error const& e) {
    caught = true;
    std::println("  caught: {}", e.what());
  }
  assert(caught);

  std::println("  PASS");
}

static void test_trim_then_validate() {
  std::println("test_trim_then_validate");

  auto d = make_inconsistent_dag();
  trim_inconsistent_clade_edges(d);
  validate_dag(d, "after trim");  // Should not throw.

  std::println("  PASS");
}

static void test_trim_then_sample() {
  std::println("test_trim_then_sample");

  auto d = make_inconsistent_dag();
  trim_inconsistent_clade_edges(d);
  auto expected_leaves = get_leaf_ids(d);

  parsimony_score_ops pops;
  subtree_weight<parsimony_score_ops> sw(d, 42u);

  for (int i = 0; i < 20; ++i) {
    auto tree = sw.sample_tree(pops);
    auto tree_leaves = get_leaf_ids(tree);
    assert(tree_leaves == expected_leaves);
  }

  std::println("  PASS");
}

static void test_fluC_PB2_7taxa() {
  std::println("test_fluC_PB2_7taxa (reproducer DAG)");

  auto d = load_proto_dag("data/madag/fluC_PB2_7taxa.pb");

  auto num_leaves = leaf_count(d);
  std::println("  DAG: {} nodes, {} edges, {} leaves", node_count(d),
               edge_count(d), num_leaves);
  assert(num_leaves == 7);

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
  auto removed = trim_inconsistent_clade_edges(d);
  std::println("  trimmed {} edges", removed);
  assert(removed > 0);

  // After trim: all min-weight samples should be complete.
  for (std::uint32_t seed = 0; seed < 100; ++seed) {
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(d, seed);
    auto tree = sw.min_weight_sample_tree(pops);
    assert(leaf_count(tree) == num_leaves);
  }
  std::println("  100 min-weight samples after trim: all have {} leaves",
               num_leaves);

  std::println("  PASS");
}

static void test_fluC_PB2_10taxa() {
  std::println("test_fluC_PB2_10taxa (10-taxa reproducer DAG)");

  auto d = load_proto_dag("data/madag/fluC_PB2_10taxa.pb");

  auto num_leaves = leaf_count(d);
  std::println("  DAG: {} nodes, {} edges, {} leaves", node_count(d),
               edge_count(d), num_leaves);
  assert(num_leaves == 10);

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
  auto removed = trim_inconsistent_clade_edges(d);
  std::println("  trimmed {} edges", removed);
  assert(removed > 0);

  // After trim: all min-weight samples should be complete.
  for (std::uint32_t seed = 0; seed < 100; ++seed) {
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(d, seed);
    auto tree = sw.min_weight_sample_tree(pops);
    assert(leaf_count(tree) == num_leaves);
  }
  std::println("  100 min-weight samples after trim: all have {} leaves",
               num_leaves);

  std::println("  PASS");
}

int main() {
  test_compute_subtree_leaves();
  test_trim_removes_bad_edge();
  test_validate_catches_inconsistency();
  test_trim_then_validate();
  test_trim_then_sample();
  test_fluC_PB2_7taxa();
  test_fluC_PB2_10taxa();

  std::println("All trim_clade_edges tests passed!");
  return 0;
}

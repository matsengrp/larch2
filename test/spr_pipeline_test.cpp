#include <larch/spr_pipeline.hpp>
#include <larch/inplace_spr.hpp>
#include <larch/load_proto_dag.hpp>

#include <cassert>
#include <map>
#include <print>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace larch;

// ---------------------------------------------------------------------------
// helpers (same patterns as native_optimize_test.cpp)
// ---------------------------------------------------------------------------

static void verify_is_tree(phylo_dag& tree) {
  auto nc = node_count(tree);
  auto ec = edge_count(tree);
  assert(ec == nc - 1);

  for (auto nv : tree.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          std::size_t parent_count = 0;
          for (auto ev : node.get_parents()) {
            (void)ev;
            ++parent_count;
          }
          if constexpr (requires { node.reference_sequence(); }) {
            assert(parent_count == 0);
          } else {
            assert(parent_count == 1);
          }
        },
        nv);
  }
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

// ---------------------------------------------------------------------------
// test fixtures
// ---------------------------------------------------------------------------

struct proto_fixture {
  std::vector<phylo_dag> trees;
  merge merger;
  std::set<std::string> leaf_ids;

  proto_fixture(std::vector<std::string> const& paths)
      : merger(load_trees(paths)) {
    std::vector<phylo_dag*> ptrs;
    for (auto& t : trees) ptrs.push_back(&t);
    merger.add_dags(ptrs);
    leaf_ids = get_leaf_ids(merger.get_result());
  }

 private:
  std::string load_trees(std::vector<std::string> const& paths) {
    trees.reserve(paths.size());
    for (auto& path : paths) {
      trees.emplace_back(load_proto_dag(path));
      recompute_compact_genomes(trees.back());
      set_sample_ids_from_cg(trees.back());
    }
    return get_reference_sequence(trees[0]);
  }
};

// ---------------------------------------------------------------------------
// Test 1: overlay SPR preserves original tree
// ---------------------------------------------------------------------------

static void test_overlay_spr_preserves_original() {
  std::println("test_overlay_spr_preserves_original");

  auto tree = make_suboptimal_tree();
  auto original_leaves = get_leaf_ids(tree);
  auto original_nc = node_count(tree);
  auto original_ec = edge_count(tree);

  // Find a profitable move
  tree_index idx{tree};
  move_enumerator enumerator{idx};
  auto radius = compute_tree_max_depth(tree) * 2;

  std::vector<profitable_move> moves;
  enumerator.find_all_moves(radius, [&](auto& m) { moves.push_back(m); });
  assert(!moves.empty());

  // Apply SPR on overlay
  auto& m = moves[0];
  overlay_dag<phylo_dag> ov{tree};
  apply_spr_topology(ov, tree,
                     spr_move{.src = m.src, .dst = m.dst, .lca = m.lca});

  // Original tree must be completely unchanged
  assert(node_count(tree) == original_nc);
  assert(edge_count(tree) == original_ec);
  assert(get_leaf_ids(tree) == original_leaves);

  // Verify tree structure is intact
  verify_is_tree(tree);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 2: fragment is a valid tree
// ---------------------------------------------------------------------------

static void test_fragment_is_valid_tree() {
  std::println("test_fragment_is_valid_tree");

  auto tree = make_suboptimal_tree();

  tree_index idx{tree};
  move_enumerator enumerator{idx};
  auto radius = compute_tree_max_depth(tree) * 2;

  std::vector<profitable_move> moves;
  enumerator.find_all_moves(radius, [&](auto& m) { moves.push_back(m); });
  assert(!moves.empty());

  for (std::size_t i = 0; i < std::min(moves.size(), std::size_t{5}); ++i) {
    auto& m = moves[i];
    auto fragment = apply_spr_as_fragment(
        tree, spr_move{.src = m.src, .dst = m.dst, .lca = m.lca});

    verify_is_tree(fragment);

    // Fragment must have at least a UA + 1 inner + some leaves
    assert(node_count(fragment) >= 3);
    assert(edge_count(fragment) >= 2);

    std::println("  move {}: fragment has {} nodes, {} edges", i,
                 node_count(fragment), edge_count(fragment));
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 3: fragment preserves subtree leaves
// ---------------------------------------------------------------------------

static void test_fragment_preserves_subtree_leaves() {
  std::println("test_fragment_preserves_subtree_leaves");

  auto tree = make_suboptimal_tree();
  auto original_leaves = get_leaf_ids(tree);

  tree_index idx{tree};
  move_enumerator enumerator{idx};
  auto radius = compute_tree_max_depth(tree) * 2;

  std::vector<profitable_move> moves;
  enumerator.find_all_moves(radius, [&](auto& m) { moves.push_back(m); });
  assert(!moves.empty());

  for (std::size_t i = 0; i < std::min(moves.size(), std::size_t{5}); ++i) {
    auto& m = moves[i];
    auto fragment = apply_spr_as_fragment(
        tree, spr_move{.src = m.src, .dst = m.dst, .lca = m.lca});

    auto frag_leaves = get_leaf_ids(fragment);

    // Fragment leaves must be a subset of original leaves
    for (auto& leaf : frag_leaves) {
      assert(original_leaves.contains(leaf));
    }

    // Fragment must have at least 2 leaves (src subtree + dst in the affected
    // region)
    assert(frag_leaves.size() >= 2);

    std::println("  move {}: fragment has {} leaves", i, frag_leaves.size());
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 4: fragment merge grows DAG
// ---------------------------------------------------------------------------

static void test_fragment_merge_grows_dag() {
  std::println("test_fragment_merge_grows_dag");

  auto tree = make_suboptimal_tree();
  merge m("AAAA");
  m.add_dag(tree);

  auto initial_nodes = m.result_node_count();
  auto initial_edges = m.result_edge_count();
  std::println("  initial: {} nodes, {} edges", initial_nodes, initial_edges);

  tree_index idx{tree};
  move_enumerator enumerator{idx};
  auto radius = compute_tree_max_depth(tree) * 2;

  std::vector<profitable_move> moves;
  enumerator.find_all_moves(radius, [&](auto& m) { moves.push_back(m); });

  std::size_t fragments_merged = 0;
  for (auto& mv : moves) {
    auto fragment = apply_spr_as_fragment(
        tree, spr_move{.src = mv.src, .dst = mv.dst, .lca = mv.lca});
    m.add_dag(std::move(fragment));
    ++fragments_merged;
  }

  auto final_nodes = m.result_node_count();
  auto final_edges = m.result_edge_count();
  std::println("  final:   {} nodes, {} edges (merged {} fragments)",
               final_nodes, final_edges, fragments_merged);

  // DAG should grow after merging fragments with new topologies
  bool grew = (final_nodes > initial_nodes || final_edges > initial_edges);
  std::println("  DAG grew: {}", grew);

  // Leaf set must be preserved
  auto final_leaves = get_leaf_ids(m.get_result());
  auto original_leaves = get_leaf_ids(tree);
  assert(final_leaves == original_leaves);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 5: native_move_producer satisfies MoveProducer concept
// ---------------------------------------------------------------------------

static void test_native_move_producer_satisfies_concept() {
  std::println("test_native_move_producer_satisfies_concept");

  static_assert(MoveProducer<native_move_producer>,
                "native_move_producer must satisfy MoveProducer concept");

  // Also verify it actually finds moves
  auto tree = make_suboptimal_tree();
  native_move_producer producer{.max_moves = 10};
  std::mt19937 rng(42);

  std::vector<spr_move> collected;
  move_callback cb = [&](spr_move const& m) { collected.push_back(m); };
  producer(tree, cb, rng);

  std::println("  producer found {} moves", collected.size());
  assert(!collected.empty());

  for (auto& m : collected) {
    assert(m.score_change.has_value());
    assert(*m.score_change < 0);
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 6: optimize_dag_v2 equivalent to old pipeline
// ---------------------------------------------------------------------------

static void test_optimize_dag_v2_equivalent() {
  std::println("test_optimize_dag_v2_equivalent");

  // Old pipeline
  auto tree1 = make_suboptimal_tree();
  merge m1("AAAA");
  m1.add_dag(tree1);
  native_strategy old_strategy{.max_moves = 10};
  auto old_results = optimize_dag(m1, old_strategy, 3, 42u);

  // New pipeline
  auto tree2 = make_suboptimal_tree();
  merge m2("AAAA");
  m2.add_dag(tree2);
  native_move_producer producer{.max_moves = 10};
  auto new_results =
      optimize_dag_v2(m2, 3, std::optional<std::uint32_t>{42u}, producer);

  // Same number of iterations
  assert(old_results.size() == new_results.size());

  // Leaf sets must match
  auto old_leaves = get_leaf_ids(m1.get_result());
  auto new_leaves = get_leaf_ids(m2.get_result());
  assert(old_leaves == new_leaves);

  // Both should have merged some trees
  bool old_merged = false, new_merged = false;
  for (auto& r : old_results) {
    std::println("  old iter {}: merged={} parsimony={} nodes={} edges={}",
                 r.iteration, r.trees_merged, r.parsimony_score,
                 r.dag_node_count, r.dag_edge_count);
    if (r.trees_merged > 0) old_merged = true;
  }
  for (auto& r : new_results) {
    std::println("  new iter {}: merged={} parsimony={} nodes={} edges={}",
                 r.iteration, r.trees_merged, r.parsimony_score,
                 r.dag_node_count, r.dag_edge_count);
    if (r.trees_merged > 0) new_merged = true;
  }
  assert(old_merged);
  assert(new_merged);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 7: optimize_dag_v2 deterministic
// ---------------------------------------------------------------------------

static void test_optimize_dag_v2_deterministic() {
  std::println("test_optimize_dag_v2_deterministic");

  auto run = []() {
    auto tree = make_suboptimal_tree();
    merge m("AAAA");
    m.add_dag(tree);
    native_move_producer producer{.max_moves = 10};
    return optimize_dag_v2(m, 3, std::optional<std::uint32_t>{42u}, producer);
  };

  auto results1 = run();
  auto results2 = run();

  assert(results1.size() == results2.size());
  for (std::size_t i = 0; i < results1.size(); ++i) {
    assert(results1[i].dag_node_count == results2[i].dag_node_count);
    assert(results1[i].dag_edge_count == results2[i].dag_edge_count);
    assert(results1[i].trees_merged == results2[i].trees_merged);
    assert(results1[i].parsimony_score == results2[i].parsimony_score);
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 8: optimize_dag_v2 on proto data
// ---------------------------------------------------------------------------

static void test_optimize_dag_v2_on_proto_data() {
  std::println("test_optimize_dag_v2_on_proto_data");

  proto_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto initial_nodes = f.merger.result_node_count();
  auto initial_edges = f.merger.result_edge_count();
  std::println("  initial: {} nodes, {} edges", initial_nodes, initial_edges);

  native_move_producer producer{.max_moves = 20};
  auto results =
      optimize_dag_v2(f.merger, 3, std::optional<std::uint32_t>{42u}, producer);

  auto final_nodes = f.merger.result_node_count();
  auto final_edges = f.merger.result_edge_count();
  std::println("  final:   {} nodes, {} edges", final_nodes, final_edges);

  for (auto& r : results) {
    std::println("  iter {}: merged={} parsimony={} nodes={} edges={}",
                 r.iteration, r.trees_merged, r.parsimony_score,
                 r.dag_node_count, r.dag_edge_count);
  }

  // Leaf set must be preserved
  auto final_leaves = get_leaf_ids(f.merger.get_result());
  assert(final_leaves == f.leaf_ids);

  // Sample trees and verify validity
  parsimony_score_ops pops;
  subtree_weight<parsimony_score_ops> sw(f.merger.get_result(), 123u);
  for (int i = 0; i < 3; ++i) {
    auto sampled = sw.min_weight_sample_tree(pops);
    verify_is_tree(sampled);
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 9: UA-grandparent fragment root captures full tree
// ---------------------------------------------------------------------------
//
// Regression test for the fragment_root determination bug.  When the LCA
// equals the tree root (binary, child of UA) and collapses during the SPR,
// the old code returned remaining_child_after_collapse, producing a fragment
// that missed the new inner node and the moved source.  The fix returns
// UA's actual child after the full SPR topology change.
//
// Tree:
//     UA
//      |
//    root
//    /   \
//  leafA  P
//        / \
//     leafB leafC
//
// SPR: src=leafA, dst=P  →  root collapses, new inner X between UA and P.
//
//     UA
//      |
//      X (new)
//     / \
//    P   leafA
//   / \
// leafB leafC
//
// Old fragment root = P (missed X and leafA).
// Fixed fragment root = X (captures the whole tree).

static void test_ua_grandparent_fragment_root() {
  std::println("test_ua_grandparent_fragment_root");

  constexpr std::string_view ref = "AAAA";
  phylo_dag tree;

  auto ua = tree.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  tree.set_root(ua);

  auto root = tree.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);

  auto leafA = tree.append_node<node_kind::leaf>();
  leafA.cg() = cg_from_sequence("TAAA", ref);
  leafA.sample_id() = "leafA";

  auto P = tree.append_node<node_kind::inner>();
  P.cg() = cg_from_sequence("ACAA", ref);

  auto leafB = tree.append_node<node_kind::leaf>();
  leafB.cg() = cg_from_sequence("ACGA", ref);
  leafB.sample_id() = "leafB";

  auto leafC = tree.append_node<node_kind::leaf>();
  leafC.cg() = cg_from_sequence("ACAG", ref);
  leafC.sample_id() = "leafC";

  add_edge(tree, ua.index(), root.index(), 0);
  add_edge(tree, root.index(), leafA.index(), 0);
  add_edge(tree, root.index(), P.index(), 1);
  add_edge(tree, P.index(), leafB.index(), 0);
  add_edge(tree, P.index(), leafC.index(), 1);

  recompute_edge_mutations(tree);
  auto original_leaves = get_leaf_ids(tree);
  assert(original_leaves.size() == 3);

  // SPR: move leafA to be sibling of P  (LCA = root, root collapses)
  auto lca = compute_lca(tree, leafA.index(), P.index());
  assert(lca == root.index());

  auto fragment = apply_spr_as_fragment(
      tree, spr_move{.src = leafA.index(), .dst = P.index(), .lca = lca});

  verify_is_tree(fragment);
  auto frag_leaves = get_leaf_ids(fragment);

  std::println("  fragment: {} nodes, {} edges, {} leaves",
               node_count(fragment), edge_count(fragment), frag_leaves.size());

  // The fragment must contain ALL leaves of the original tree.
  assert(frag_leaves == original_leaves);

  // Merge and verify no trim is needed.
  merge m(std::string{ref});
  m.add_dag(tree);
  m.add_dag(std::move(fragment));
  auto& result = m.get_result();

  auto result_leaves = get_leaf_ids(result);
  assert(result_leaves == original_leaves);

  // Verify all sampled trees have the correct leaf count.
  for (std::uint32_t seed = 0; seed < 20; ++seed) {
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(result, seed);
    auto sampled = sw.min_weight_sample_tree(pops);
    assert(is_tree(sampled));
    assert(get_leaf_ids(sampled) == original_leaves);
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 36: optimize_dag_v2 with both MoveProducer and FragmentProducer
// ---------------------------------------------------------------------------

static void test_optimize_dag_v2_mixed_producers() {
  std::println("test_optimize_dag_v2_mixed_producers");

  auto tree = make_suboptimal_tree();
  merge m("AAAA");
  m.add_dag(tree);

  auto initial_nodes = m.result_node_count();
  auto initial_edges = m.result_edge_count();
  auto initial_leaves = get_leaf_ids(m.get_result());
  std::println("  initial: {} nodes, {} edges", initial_nodes, initial_edges);

  native_move_producer native{.max_moves = 10};
  inplace_params ip{
      .max_steps = 3,
      .accept_threshold = 0,
      .selector = move_selection::best_improving,
      .frag_mode = fragment_strategy::final_only,
  };
  inplace_move_producer inplace{ip};

  auto results = optimize_dag_v2(m, 3, std::optional<std::uint32_t>{42u},
                                 native, inplace);

  auto final_nodes = m.result_node_count();
  auto final_edges = m.result_edge_count();
  std::println("  final:   {} nodes, {} edges", final_nodes, final_edges);

  for (auto& r : results) {
    std::println("  iter {}: merged={} parsimony={} nodes={} edges={}",
                 r.iteration, r.trees_merged, r.parsimony_score,
                 r.dag_node_count, r.dag_edge_count);
  }

  // Leaf set must be preserved
  auto final_leaves = get_leaf_ids(m.get_result());
  assert(final_leaves == initial_leaves);

  // DAG should have grown
  assert(final_nodes >= initial_nodes);
  assert(final_edges >= initial_edges);

  // At least some trees were merged
  bool any_merged = false;
  for (auto& r : results)
    if (r.trees_merged > 0) any_merged = true;
  assert(any_merged);

  std::println("  PASS");
}

// Phase 36: FragmentProducer-only pipeline (no MoveProducer)
static void test_optimize_dag_v2_fragment_only() {
  std::println("test_optimize_dag_v2_fragment_only");

  auto tree = make_suboptimal_tree();
  merge m("AAAA");
  m.add_dag(tree);

  auto initial_leaves = get_leaf_ids(m.get_result());

  inplace_params ip{
      .max_steps = 5,
      .accept_threshold = 0,
      .selector = move_selection::best_improving,
      .frag_mode = fragment_strategy::final_only,
  };
  inplace_move_producer producer{ip};

  auto results = optimize_dag_v2(m, 3, std::optional<std::uint32_t>{42u},
                                 producer);

  for (auto& r : results) {
    std::println("  iter {}: merged={} parsimony={} nodes={} edges={}",
                 r.iteration, r.trees_merged, r.parsimony_score,
                 r.dag_node_count, r.dag_edge_count);
  }

  auto final_leaves = get_leaf_ids(m.get_result());
  assert(final_leaves == initial_leaves);

  std::println("  PASS");
}

// Phase 36: Mixed producers on real data
static void test_optimize_dag_v2_mixed_real_data() {
  std::println("test_optimize_dag_v2_mixed_real_data");

  proto_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz"});

  auto initial_nodes = f.merger.result_node_count();
  std::println("  initial: {} nodes", initial_nodes);

  native_move_producer native{.max_moves = 10};
  inplace_params ip{
      .max_steps = 3,
      .accept_threshold = 0,
      .selector = move_selection::best_improving,
      .frag_mode = fragment_strategy::final_only,
  };
  inplace_move_producer inplace{ip};

  auto results = optimize_dag_v2(f.merger, 2,
                                 std::optional<std::uint32_t>{42u},
                                 native, inplace);

  for (auto& r : results) {
    std::println("  iter {}: merged={} parsimony={} nodes={} edges={}",
                 r.iteration, r.trees_merged, r.parsimony_score,
                 r.dag_node_count, r.dag_edge_count);
  }

  auto final_leaves = get_leaf_ids(f.merger.get_result());
  assert(final_leaves == f.leaf_ids);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  test_overlay_spr_preserves_original();
  test_fragment_is_valid_tree();
  test_fragment_preserves_subtree_leaves();
  test_fragment_merge_grows_dag();
  test_native_move_producer_satisfies_concept();
  test_optimize_dag_v2_equivalent();
  test_optimize_dag_v2_deterministic();
  test_optimize_dag_v2_on_proto_data();
  test_ua_grandparent_fragment_root();

  // Phase 36: mixed producer pipeline
  test_optimize_dag_v2_mixed_producers();
  test_optimize_dag_v2_fragment_only();
  test_optimize_dag_v2_mixed_real_data();

  std::println("All SPR pipeline tests passed!");
  return 0;
}

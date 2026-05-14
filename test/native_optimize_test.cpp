#include <larch/native_optimize.hpp>
#include <larch/load_proto_dag.hpp>

#include "test_util.hpp"

#include <cassert>
#include <print>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace larch;
using larch::test::cg_from_sequence;

// ---------------------------------------------------------------------------
// helpers (same patterns as optimize_test.cpp)
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

static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx, std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
}

// Build a deliberately suboptimal tree so moves can be found.
// 6 leaves with a ref of length 4. The topology places leaves suboptimally.
static phylo_dag make_suboptimal_tree() {
  constexpr std::string_view ref = "AAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  // Leaves: group {L1,L2,L3} share mutation at pos 1 (A->T)
  //         group {L4,L5,L6} share mutation at pos 2 (A->C)
  // But we'll build a topology that mixes them, making it suboptimal.
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

  // Suboptimal topology: mix the two groups
  //       root
  //      /    \
  //    i1      i2
  //   / \     / \
  //  i3  L4  L3  i4
  //  /\         /\
  // L1 L2     L5  L6
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
  add_edge(d, i1.index(), l4.index(), 1);  // L4 is in wrong group
  add_edge(d, i3.index(), l1.index(), 0);
  add_edge(d, i3.index(), l2.index(), 1);
  add_edge(d, i2.index(), l3.index(), 0);  // L3 is in wrong group
  add_edge(d, i2.index(), i4.index(), 1);
  add_edge(d, i4.index(), l5.index(), 0);
  add_edge(d, i4.index(), l6.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// Same 6-leaf tree from optimize_test
static phylo_dag make_sample_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto n1 = d.append_node<node_kind::leaf>();
  n1.cg() = cg_from_sequence("ACC", ref);
  n1.sample_id() = n1.cg().to_string();
  auto n2 = d.append_node<node_kind::leaf>();
  n2.cg() = cg_from_sequence("GTT", ref);
  n2.sample_id() = n2.cg().to_string();
  auto n3 = d.append_node<node_kind::leaf>();
  n3.cg() = cg_from_sequence("AGG", ref);
  n3.sample_id() = n3.cg().to_string();
  auto n4 = d.append_node<node_kind::leaf>();
  n4.cg() = cg_from_sequence("ACG", ref);
  n4.sample_id() = n4.cg().to_string();
  auto n5 = d.append_node<node_kind::leaf>();
  n5.cg() = cg_from_sequence("CTT", ref);
  n5.sample_id() = n5.cg().to_string();
  auto n6 = d.append_node<node_kind::leaf>();
  n6.cg() = cg_from_sequence("TCC", ref);
  n6.sample_id() = n6.cg().to_string();

  auto n7 = d.append_node<node_kind::inner>();
  n7.cg() = cg_from_sequence("TGG", ref);
  auto n8 = d.append_node<node_kind::inner>();
  n8.cg() = cg_from_sequence("CTC", ref);
  auto n9 = d.append_node<node_kind::inner>();
  n9.cg() = cg_from_sequence("AGT", ref);
  auto n10 = d.append_node<node_kind::inner>();
  n10.cg() = cg_from_sequence("GAA", ref);

  add_edge(d, ua.index(), n10.index(), 0);
  add_edge(d, n10.index(), n8.index(), 0);
  add_edge(d, n10.index(), n9.index(), 1);
  add_edge(d, n8.index(), n3.index(), 0);
  add_edge(d, n8.index(), n4.index(), 1);
  add_edge(d, n8.index(), n7.index(), 2);
  add_edge(d, n7.index(), n1.index(), 0);
  add_edge(d, n7.index(), n2.index(), 1);
  add_edge(d, n9.index(), n5.index(), 0);
  add_edge(d, n9.index(), n6.index(), 1);

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
// Test 1: tree_index construction
// ---------------------------------------------------------------------------

static void test_tree_index_construction() {
  std::println("test_tree_index_construction");

  auto tree = make_suboptimal_tree();
  tree_index idx{tree};

  auto& var_sites = idx.get_variable_sites();
  std::println("  variable sites: {}", var_sites.size());
  assert(!var_sites.empty());

  auto& searchable = idx.get_searchable_nodes();
  std::println("  searchable nodes: {}", searchable.size());
  assert(!searchable.empty());

  // DFS must be consistent: parent's interval contains child's
  auto tree_root = idx.get_tree_root();
  assert(idx.has_dfs_info(tree_root));

  // Check ancestor relationships
  for (auto node : searchable) {
    assert(idx.is_ancestor(tree_root, node));
    assert(!idx.is_ancestor(node, tree_root) || node == tree_root);
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 2: enumerator finds moves on suboptimal tree
// ---------------------------------------------------------------------------

static void test_enumerator_finds_moves() {
  std::println("test_enumerator_finds_moves");

  auto tree = make_suboptimal_tree();
  tree_index idx{tree};
  move_enumerator enumerator{idx};

  auto radius = compute_tree_max_depth(tree) * 2;
  std::println("  radius: {}", radius);

  std::vector<profitable_move> moves;
  enumerator.find_all_moves(radius, [&](auto& m) { moves.push_back(m); });

  std::println("  found {} moves", moves.size());
  for (auto& m : moves) {
    std::println("    src={} dst={} lca={} score_change={}", m.src, m.dst,
                 m.lca, m.score_change);
    assert(m.score_change < 0);
  }
  assert(!moves.empty());

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 3: cached vs independent scoring
// ---------------------------------------------------------------------------

static void test_cached_vs_independent() {
  std::println("test_cached_vs_independent");

  auto tree = make_suboptimal_tree();
  tree_index idx{tree};
  move_enumerator enumerator{idx};

  auto radius = compute_tree_max_depth(tree) * 2;
  std::size_t checked = 0;

  // Find all moves and verify cached == independent for each
  enumerator.find_all_moves(radius, [&](auto& m) {
    int independent = enumerator.compute_move_score(m.src, m.dst, m.lca);

    if (independent != m.score_change) {
      std::println(
          "  MISMATCH: src={} dst={} lca={} reported={} independent={}", m.src,
          m.dst, m.lca, m.score_change, independent);
    }
    assert(independent == m.score_change);
    checked++;
  });

  std::println("  checked {} moves", checked);
  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 4: pruned vs exhaustive (brute force all src,dst,lca triples)
// ---------------------------------------------------------------------------

static void test_pruned_vs_exhaustive() {
  std::println("test_pruned_vs_exhaustive");

  auto tree = make_suboptimal_tree();
  tree_index idx{tree};
  move_enumerator enumerator{idx};

  auto radius = compute_tree_max_depth(tree) * 2;

  // Collect all moves via find_all_moves
  std::set<std::pair<std::size_t, std::size_t>> pruned_moves;
  enumerator.find_all_moves(
      radius, [&](auto& m) { pruned_moves.insert({m.src, m.dst}); });

  // Brute force: for each searchable src, try every non-ancestor dst
  auto& searchable = idx.get_searchable_nodes();
  std::size_t brute_profitable = 0;

  for (auto src : searchable) {
    // Walk up from src to find all potential LCAs
    std::vector<std::size_t> ancestors;
    auto cur = idx.get_parent(src);
    while (idx.has_dfs_info(cur)) {
      ancestors.push_back(cur);
      if (cur == idx.get_tree_root()) break;
      cur = idx.get_parent(cur);
    }

    // For each potential dst (all nodes except src and src's
    // ancestors/descendants)
    for (auto dst_nv : tree.get_all_nodes()) {
      std::visit(
          [&](auto dst_node) {
            auto dst = dst_node.index();
            if (is_ua(tree, dst)) return;
            if (dst == idx.get_tree_root()) return;
            if (dst == src) return;
            if (idx.is_ancestor(src, dst)) return;  // dst is descendant of src
            if (idx.is_ancestor(dst, src)) return;  // dst is ancestor of src
            if (!idx.has_dfs_info(dst)) return;

            // Find LCA
            for (auto lca : ancestors) {
              if (idx.is_ancestor(lca, dst)) {
                int score = enumerator.compute_move_score(src, dst, lca);
                if (score < 0) {
                  brute_profitable++;
                  if (!pruned_moves.count({src, dst})) {
                    std::println("  MISSED: src={} dst={} lca={} score={}", src,
                                 dst, lca, score);
                  }
                }
                break;
              }
            }
          },
          dst_nv);
    }
  }

  std::println("  pruned found: {}, brute force found: {}", pruned_moves.size(),
               brute_profitable);
  // pruned search should find at least as many as brute force
  assert(pruned_moves.size() >= brute_profitable);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 5: DAG grows with native strategy
// ---------------------------------------------------------------------------

static void test_dag_grows() {
  std::println("test_dag_grows");

  auto tree = make_suboptimal_tree();
  merge m("AAAA");
  m.add_dag(tree);

  auto initial_nodes = m.result_node_count();
  auto initial_edges = m.result_edge_count();
  std::println("  initial: {} nodes, {} edges", initial_nodes, initial_edges);

  native_strategy strategy{.max_moves = 10};
  auto results = optimize_dag(m, strategy, 2, 42u);

  auto final_nodes = m.result_node_count();
  auto final_edges = m.result_edge_count();
  std::println("  final:   {} nodes, {} edges", final_nodes, final_edges);

  bool grew = (final_nodes > initial_nodes || final_edges > initial_edges);
  std::println("  DAG grew: {}", grew);

  // At least one iteration should have merged new trees
  bool any_merged = false;
  for (auto& r : results) {
    std::println("  iter {}: merged={} parsimony={} nodes={} edges={}",
                 r.iteration, r.trees_merged, r.parsimony_score,
                 r.dag_node_count, r.dag_edge_count);
    if (r.trees_merged > 0) any_merged = true;
  }
  assert(any_merged);

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test: verify predicted score change matches ground truth (mutation count)
// ---------------------------------------------------------------------------

static std::size_t count_tree_mutations(phylo_dag& tree) {
  std::size_t total = 0;
  for (auto ev : tree.get_all_edges()) {
    std::visit([&](auto edge) { total += edge.mutations().size(); }, ev);
  }
  return total;
}

static void test_score_vs_ground_truth() {
  std::println("test_score_vs_ground_truth");

  auto tree = make_suboptimal_tree();
  auto original_score = count_tree_mutations(tree);
  std::println("  original score: {}", original_score);

  tree_index idx{tree};
  move_enumerator enumerator{idx};
  auto radius = compute_tree_max_depth(tree) * 2;

  std::vector<profitable_move> all_moves;
  enumerator.find_all_moves(radius, [&](auto& m) { all_moves.push_back(m); });

  // Also check all src/dst pairs (brute force) for completeness
  auto& searchable = idx.get_searchable_nodes();
  std::vector<profitable_move> brute_moves;

  for (auto src : searchable) {
    std::vector<std::size_t> ancestors;
    auto cur = idx.get_parent(src);
    while (idx.has_dfs_info(cur)) {
      ancestors.push_back(cur);
      if (cur == idx.get_tree_root()) break;
      cur = idx.get_parent(cur);
    }

    for (auto dst_nv : tree.get_all_nodes()) {
      std::visit(
          [&](auto dst_node) {
            auto dst = dst_node.index();
            if (is_ua(tree, dst)) return;
            if (dst == idx.get_tree_root()) return;
            if (dst == src) return;
            if (idx.is_ancestor(src, dst)) return;
            if (idx.is_ancestor(dst, src)) return;
            if (!idx.has_dfs_info(dst)) return;

            for (auto lca : ancestors) {
              if (idx.is_ancestor(lca, dst)) {
                int score = enumerator.compute_move_score(src, dst, lca);
                if (score < 0) {
                  brute_moves.push_back(
                      profitable_move{src, dst, lca, score});
                }
                break;
              }
            }
          },
          dst_nv);
    }
  }

  std::size_t mismatches = 0;
  auto check_move = [&](profitable_move const& m, char const* label) {
    auto result = apply_spr_move(tree, m.src, m.dst);
    auto actual_score = static_cast<int>(count_tree_mutations(result));
    int actual_change = actual_score - static_cast<int>(original_score);
    if (actual_change != m.score_change) {
      std::println("  {} MISMATCH: src={} dst={} lca={} predicted={} actual={}",
                   label, m.src, m.dst, m.lca, m.score_change, actual_change);
      mismatches++;
    }
  };

  for (auto& m : all_moves) check_move(m, "enumerated");
  for (auto& m : brute_moves) check_move(m, "brute");

  std::println("  checked {} enumerated + {} brute moves, {} mismatches",
               all_moves.size(), brute_moves.size(), mismatches);
  assert(mismatches == 0);
  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 6: sampled trees are valid after optimization
// ---------------------------------------------------------------------------

static void test_valid_dag() {
  std::println("test_valid_dag");

  auto tree = make_suboptimal_tree();
  auto original_leaves = get_leaf_ids(tree);

  // Verify each SPR move produces a valid tree with correct leaves
  tree_index idx{tree};
  move_enumerator enumerator{idx};
  auto radius = compute_tree_max_depth(tree) * 2;

  std::vector<profitable_move> moves;
  enumerator.find_all_moves(radius, [&](auto& m) { moves.push_back(m); });

  for (std::size_t i = 0; i < std::min(moves.size(), std::size_t{5}); ++i) {
    auto result = apply_spr_move(tree, moves[i].src, moves[i].dst);
    verify_is_tree(result);
    auto result_leaves = get_leaf_ids(result);
    assert(result_leaves == original_leaves);
  }

  // Verify optimization produces valid sampled trees
  merge m("AAAA");
  m.add_dag(tree);
  native_strategy strategy{.max_moves = 5};
  optimize_dag(m, strategy, 2, 42u);

  parsimony_score_ops pops;
  subtree_weight<parsimony_score_ops> sw(m.get_result(), 99u);
  for (int i = 0; i < 5; ++i) {
    auto sampled = sw.min_weight_sample_tree(pops);
    verify_is_tree(sampled);
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Test 7: native optimization on test_5_trees proto data
// ---------------------------------------------------------------------------

static void test_native_5_trees() {
  std::println("test_native_5_trees");

  proto_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto initial_nodes = f.merger.result_node_count();
  auto initial_edges = f.merger.result_edge_count();
  std::println("  initial: {} nodes, {} edges", initial_nodes, initial_edges);

  native_strategy strategy{.max_moves = 20};
  auto results = optimize_dag(f.merger, strategy, 3, 42u);

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
// Test 8: parallel tree_index produces bit-identical Fitch sets
// ---------------------------------------------------------------------------

static void test_parallel_tree_index() {
  std::println("test_parallel_tree_index");

  auto tree = make_suboptimal_tree();

  // Sequential construction
  tree_index seq_idx{tree};

  // Parallel construction
  thread_pool pool{4};
  tree_index par_idx{tree, pool};

  auto n_sites = seq_idx.num_variable_sites();
  assert(n_sites == par_idx.num_variable_sites());

  auto& seq_searchable = seq_idx.get_searchable_nodes();
  auto& par_searchable = par_idx.get_searchable_nodes();
  assert(seq_searchable.size() == par_searchable.size());

  // Check every node's Fitch sets and child counts
  auto root = seq_idx.get_tree_root();
  assert(root == par_idx.get_tree_root());

  std::size_t nodes_checked = 0;
  std::function<void(std::size_t)> check_node = [&](std::size_t nid) {
    for (std::size_t i = 0; i < n_sites; i++) {
      assert(seq_idx.get_fitch_set(nid, i) == par_idx.get_fitch_set(nid, i));
      assert(seq_idx.get_allele_union(nid, i) ==
             par_idx.get_allele_union(nid, i));
      if (seq_idx.has_child_counts(nid)) {
        assert(par_idx.has_child_counts(nid));
        auto& sc = seq_idx.get_child_counts(nid, i);
        auto& pc = par_idx.get_child_counts(nid, i);
        assert(sc == pc);
      }
    }
    assert(seq_idx.get_num_children(nid) == par_idx.get_num_children(nid));
    nodes_checked++;

    for (auto child : seq_idx.get_children(nid)) check_node(child);
  };
  check_node(root);

  std::println("  checked {} nodes, {} sites — bit-identical", nodes_checked,
               n_sites);
  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  // Disable stdout buffering to prevent hangs with std::println
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  test_tree_index_construction();
  test_enumerator_finds_moves();
  test_cached_vs_independent();
  test_pruned_vs_exhaustive();
  test_score_vs_ground_truth();
  test_dag_grows();
  test_valid_dag();
  test_native_5_trees();
  test_parallel_tree_index();

  std::println("All native optimize tests passed!");
  return 0;
}

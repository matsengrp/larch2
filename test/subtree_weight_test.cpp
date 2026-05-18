#include <larch/subtree_weight.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/merge.hpp>
#include <larch/protobuf.hpp>
#include <larch/io_util.hpp>

#include "test_util.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using larch::test::cg_from_sequence;

// Verify that a phylo_dag is a valid tree:
// - Every non-root node has exactly one parent
// - Tree with N nodes has N-1 edges
static void verify_is_tree(larch::phylo_dag& tree) {
  auto nc = larch::node_count(tree);
  auto ec = larch::edge_count(tree);

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

static std::size_t add_weighted_edge(larch::phylo_dag& d,
                                     std::size_t parent_idx,
                                     std::size_t child_idx,
                                     std::size_t clade_idx,
                                     float edge_weight) {
  auto edge = d.append_edge<larch::edge_kind::clade>();
  edge.clade_index() = clade_idx;
  edge.edge_weight() = edge_weight;
  std::visit([&](auto p) { edge.set_parent(p); }, d.get_node(parent_idx));
  std::visit([&](auto c) { edge.set_child(c); }, d.get_node(child_idx));
  return edge.index();
}

static larch::phylo_dag make_edge_weight_alternative_dag() {
  constexpr std::string_view ref = "AAAA";
  larch::phylo_dag d;

  auto ua = d.append_node<larch::node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto root = d.append_node<larch::node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);

  auto alt1 = d.append_node<larch::node_kind::inner>();
  alt1.cg() = cg_from_sequence("CAAA", ref);

  auto alt2 = d.append_node<larch::node_kind::inner>();
  alt2.cg() = cg_from_sequence("ACAA", ref);

  auto l1 = d.append_node<larch::node_kind::leaf>();
  l1.cg() = cg_from_sequence("CCAA", ref);
  l1.sample_id() = "L1";

  auto l2 = d.append_node<larch::node_kind::leaf>();
  l2.cg() = cg_from_sequence("CACA", ref);
  l2.sample_id() = "L2";

  add_weighted_edge(d, ua.index(), root.index(), 0, 0.5f);
  add_weighted_edge(d, root.index(), alt1.index(), 0, 1.0f);
  add_weighted_edge(d, root.index(), alt2.index(), 0, 10.0f);
  add_weighted_edge(d, alt1.index(), l1.index(), 0, 1.0f);
  add_weighted_edge(d, alt1.index(), l2.index(), 1, 1.0f);
  add_weighted_edge(d, alt2.index(), l1.index(), 0, 10.0f);
  add_weighted_edge(d, alt2.index(), l2.index(), 1, 10.0f);

  larch::recompute_edge_mutations(d);
  larch::build_clade_offsets(d);
  return d;
}

static larch::phylo_dag make_edge_weight_tie_dag() {
  constexpr std::string_view ref = "AAAA";
  larch::phylo_dag d;

  auto ua = d.append_node<larch::node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto root = d.append_node<larch::node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);

  auto alt1 = d.append_node<larch::node_kind::leaf>();
  alt1.cg() = cg_from_sequence("CAAA", ref);
  alt1.sample_id() = "L";

  auto alt2 = d.append_node<larch::node_kind::leaf>();
  alt2.cg() = cg_from_sequence("ACAA", ref);
  alt2.sample_id() = "L";

  add_weighted_edge(d, ua.index(), root.index(), 0, 0.5f);
  add_weighted_edge(d, root.index(), alt1.index(), 0, 1.0f);
  add_weighted_edge(d, root.index(), alt2.index(), 0, 1.0f);

  larch::recompute_edge_mutations(d);
  larch::build_clade_offsets(d);
  return d;
}

static double sum_edge_weights(larch::phylo_dag& d) {
  double total = 0.0;
  for (auto ev : d.get_all_edges())
    std::visit([&](auto edge) { total += edge.edge_weight(); }, ev);
  return total;
}

static std::set<std::string> get_leaf_ids(larch::phylo_dag& d) {
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

static std::string selected_leaf_sequence(larch::phylo_dag& sampled,
                                          std::string_view alt1,
                                          std::string_view alt2) {
  std::string chosen_seq;
  std::size_t chosen_count = 0;
  for (auto nv : sampled.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            auto seq = larch::test::node_sequence(sampled, node.index());
            if (seq == alt1 || seq == alt2) {
              chosen_seq = std::move(seq);
              ++chosen_count;
            }
          }
        },
        nv);
  }
  assert(chosen_count == 1);
  return chosen_seq;
}

struct test_fixture {
  std::vector<larch::phylo_dag> trees;
  larch::merge merger;
  std::set<std::string> leaf_ids;

  test_fixture(std::vector<std::string> const& paths)
      : merger(load_trees(paths)) {
    std::vector<larch::phylo_dag*> ptrs;
    for (auto& t : trees) ptrs.push_back(&t);
    merger.add_dags(ptrs);
    leaf_ids = get_leaf_ids(merger.get_result());
  }

 private:
  std::string load_trees(std::vector<std::string> const& paths) {
    trees.reserve(paths.size());
    for (auto& path : paths) {
      trees.emplace_back(
          larch::load_proto_dag(larch::test::source_path_string(path)));
      larch::recompute_compact_genomes(trees.back());
      larch::set_sample_ids_from_cg(trees.back());
    }
    return larch::get_reference_sequence(trees[0]);
  }
};

static void test_sample_tree() {
  std::println("test_sample_tree");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  for (int i = 0; i < 5; ++i) {
    auto tree = sw.sample_tree(pops);
    verify_is_tree(tree);
    auto tree_leaves = get_leaf_ids(tree);
    assert(tree_leaves == f.leaf_ids);
  }
  std::println("  PASS");
}

static void test_tree_count() {
  std::println("test_tree_count");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::tree_count_ops tops;
  larch::subtree_weight<larch::tree_count_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto count = sw.compute_weight_below(root_idx, tops);

  std::println("  tree count: {}", count.to_string());
  assert(count > larch::bigint{0});
  std::println("  PASS");
}

static void test_uniform_sample_tree() {
  std::println("test_uniform_sample_tree");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::tree_count_ops tops;
  larch::subtree_weight<larch::tree_count_ops> sw(dag, 42u);

  for (int i = 0; i < 5; ++i) {
    auto tree = sw.uniform_sample_tree(tops);
    verify_is_tree(tree);
    auto tree_leaves = get_leaf_ids(tree);
    assert(tree_leaves == f.leaf_ids);
  }
  std::println("  PASS");
}

static void test_min_weight_sample_tree() {
  std::println("test_min_weight_sample_tree");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto min_score = sw.compute_weight_below(root_idx, pops);
  std::println("  min parsimony score: {}", min_score);

  for (int i = 0; i < 5; ++i) {
    auto tree = sw.min_weight_sample_tree(pops);
    verify_is_tree(tree);
    auto tree_leaves = get_leaf_ids(tree);
    assert(tree_leaves == f.leaf_ids);

    std::size_t total_mutations = 0;
    for (auto ev : tree.get_all_edges()) {
      std::visit([&](auto edge) { total_mutations += edge.mutations().size(); },
                 ev);
    }
    assert(total_mutations == min_score);
  }
  std::println("  PASS");
}

static void test_min_weight_count() {
  std::println("test_min_weight_count");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto count = sw.min_weight_count(root_idx, pops);

  std::println("  min-weight tree count: {}", count.to_string());
  assert(count > larch::bigint{0});
  std::println("  PASS");
}

static void test_min_weight_uniform_sample() {
  std::println("test_min_weight_uniform_sample");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto min_score = sw.compute_weight_below(root_idx, pops);

  for (int i = 0; i < 5; ++i) {
    auto tree = sw.min_weight_uniform_sample_tree(pops);
    verify_is_tree(tree);
    auto tree_leaves = get_leaf_ids(tree);
    assert(tree_leaves == f.leaf_ids);

    std::size_t total_mutations = 0;
    for (auto ev : tree.get_all_edges()) {
      std::visit([&](auto edge) { total_mutations += edge.mutations().size(); },
                 ev);
    }
    assert(total_mutations == min_score);
  }
  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Edge parsimony tests
// ---------------------------------------------------------------------------

static void test_edge_parsimony_consistency() {
  std::println("test_edge_parsimony_consistency");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto global_min = sw.compute_weight_below(root_idx, pops);
  auto scores = sw.compute_edge_min_global_scores(pops);

  std::println("  global_min: {}", global_min);

  // Every edge score must be >= global minimum
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto eidx = edge.index();
          assert(scores[eidx] >= global_min);
        },
        ev);
  }

  // Edges on min-weight trees should have score == global_min
  // (at least one such edge must exist -- the UA->root edge)
  bool found_optimal = false;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (scores[edge.index()] == global_min) found_optimal = true;
        },
        ev);
  }
  assert(found_optimal);

  std::println("  PASS");
}

static void test_edge_parsimony_single_tree() {
  std::println("test_edge_parsimony_single_tree");

  // Single tree: every edge's min global score equals the tree's parsimony
  test_fixture f({"data/test_5_trees/tree_0.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto global_min = sw.compute_weight_below(root_idx, pops);
  auto scores = sw.compute_edge_min_global_scores(pops);

  std::println("  global_min: {}, edges: {}", global_min,
               larch::edge_count(dag));

  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto eidx = edge.index();
          assert(scores[eidx] == global_min);
        },
        ev);
  }

  std::println("  PASS");
}

static void test_edge_parsimony_ua_root_edge() {
  std::println("test_edge_parsimony_ua_root_edge");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto global_min = sw.compute_weight_below(root_idx, pops);
  auto scores = sw.compute_edge_min_global_scores(pops);

  // UA->root edge appears in every tree, so penalty must be 0
  auto root_nv = dag.get_root();
  std::visit(
      [&](auto ua) {
        for (auto ev : ua.get_children()) {
          std::visit(
              [&](auto edge) {
                auto eidx = edge.index();
                std::println("  UA->root edge {} score: {} (global_min: {})",
                             eidx, scores[eidx], global_min);
                assert(scores[eidx] == global_min);
              },
              ev);
        }
      },
      root_nv);

  std::println("  PASS");
}

static void test_edge_parsimony_rerooting_identity() {
  std::println("test_edge_parsimony_rerooting_identity");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto global_min = sw.compute_weight_below(root_idx, pops);
  auto above = sw.compute_weight_above(pops);
  auto scores = sw.compute_edge_min_global_scores(pops);

  // For every leaf node: above[leaf] + below[leaf] == global_min.
  // Every tree contains every leaf, so the min global score of any tree
  // containing a leaf is just the global minimum. Since below[leaf] = 0,
  // above[leaf] must equal global_min.
  std::size_t leaf_count = 0;
  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            auto idx = node.index();
            auto below = sw.compute_weight_below(idx, pops);
            assert(below == 0);
            assert(above[idx] == global_min);
            ++leaf_count;
          }
        },
        nv);
  }
  std::println("  verified above[leaf] == global_min for {} leaves",
               leaf_count);
  assert(leaf_count > 0);

  // For every edge: min_global_score(e) == above[parent] + (total_clade -
  // clade_j) + edge_w + below[child]. Verify this decomposition by checking
  // that for edges in the min-weight tree, scores[e] == global_min.
  std::size_t optimal_edges = 0;
  std::size_t total_edges = 0;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          ++total_edges;
          assert(scores[edge.index()] >= global_min);
          if (scores[edge.index()] == global_min) ++optimal_edges;
        },
        ev);
  }
  std::println("  {}/{} edges are optimal (penalty=0)", optimal_edges,
               total_edges);
  assert(optimal_edges > 0);

  // For every node: above[node] + below[node] >= global_min
  for (auto nv : dag.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    auto below = sw.compute_weight_below(idx, pops);
    assert(above[idx] + below >= global_min);
  }

  std::println("  PASS");
}

static void test_weight_above() {
  std::println("test_weight_above");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto global_min = sw.compute_weight_below(root_idx, pops);
  auto above = sw.compute_weight_above(pops);

  // Root should have weight_above = 0
  assert(above[root_idx] == 0);

  // All reachable nodes should have finite weight_above
  auto constexpr max_w = std::numeric_limits<std::size_t>::max() / 2;
  for (auto nv : dag.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    assert(above[idx] < max_w);
  }

  // Re-rooting identity: for every leaf, above[leaf] == global_min
  // (since below[leaf] = 0 and every tree contains every leaf)
  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            assert(above[node.index()] == global_min);
          }
        },
        nv);
  }

  // For all nodes: above[node] + below[node] >= global_min
  for (auto nv : dag.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    auto below = sw.compute_weight_below(idx, pops);
    assert(above[idx] + below >= global_min);
  }

  std::println("  root above: {}, global_min: {}", above[root_idx], global_min);
  std::println("  PASS");
}

static void test_edge_parsimony_exhaustive_ground_truth() {
  std::println("test_edge_parsimony_exhaustive_ground_truth");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  sw.compute_weight_below(root_idx, pops);
  auto scores = sw.compute_edge_min_global_scores(pops);

  // Sample many trees, tracking which DAG edges appear in each and each
  // tree's parsimony score. Then verify: for each edge, the minimum score
  // among trees containing that edge equals the computed min_global_score.
  auto edge_hm = dag.edge_high_mark();
  std::vector<std::size_t> min_score_seen(
      edge_hm, std::numeric_limits<std::size_t>::max());

  int const num_samples = 500;
  larch::subtree_weight<larch::parsimony_score_ops> sampler(dag, 7777u);
  for (int i = 0; i < num_samples; ++i) {
    auto chosen_edges = sampler.sample_tree_edges(pops);

    // Compute this tree's total parsimony score
    std::size_t tree_score = 0;
    for (auto eidx : chosen_edges) tree_score += pops.compute_edge(dag, eidx);

    // Update min score seen for each edge in this tree
    for (auto eidx : chosen_edges) {
      if (tree_score < min_score_seen[eidx]) min_score_seen[eidx] = tree_score;
    }
  }

  // For every edge that was seen in at least one sampled tree:
  // min_score_seen[e] >= scores[e] (computed is a lower bound).
  // With enough samples, they should be equal for most edges.
  std::size_t edges_seen = 0;
  std::size_t exact_matches = 0;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto eidx = edge.index();
          if (min_score_seen[eidx] < std::numeric_limits<std::size_t>::max()) {
            ++edges_seen;
            // Computed score must be <= any observed score
            assert(scores[eidx] <= min_score_seen[eidx]);
            if (scores[eidx] == min_score_seen[eidx]) ++exact_matches;
          }
        },
        ev);
  }

  std::println("  sampled {} trees, observed {}/{} edges, {} exact matches",
               num_samples, edges_seen, larch::edge_count(dag), exact_matches);
  // We expect most edges to have been observed
  assert(edges_seen > larch::edge_count(dag) / 2);
  // We expect most observed edges to have exact matches
  assert(exact_matches > edges_seen / 2);

  std::println("  PASS");
}

static void test_edge_weight_score_ops_selects_min_alternative() {
  std::println("test_edge_weight_score_ops_selects_min_alternative");

  auto dag = make_edge_weight_alternative_dag();
  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  larch::edge_weight_score_ops ops;
  larch::subtree_weight<larch::edge_weight_score_ops> sw(dag, 42u);

  auto score = sw.compute_weight_below(root_idx, ops);
  assert(std::abs(score - 3.5) < 1e-10);

  auto sampled = sw.min_weight_sample_tree(ops);
  verify_is_tree(sampled);
  assert(std::abs(sum_edge_weights(sampled) - 3.5) < 1e-10);

  std::println("  PASS");
}

static void test_edge_weight_score_ops_tie_selection() {
  std::println("test_edge_weight_score_ops_tie_selection");

  auto dag = make_edge_weight_tie_dag();
  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  larch::edge_weight_score_ops ops;
  larch::subtree_weight<larch::edge_weight_score_ops> sw(dag, 42u);

  auto score = sw.compute_weight_below(root_idx, ops);
  assert(std::abs(score - 1.5) < 1e-10);
  assert(sw.min_weight_count(root_idx, ops) == larch::bigint{2});

  std::println("  PASS");
}

static void test_edge_weight_uniform_tie_sampling_balance() {
  std::println("test_edge_weight_uniform_tie_sampling_balance");

  auto dag = make_edge_weight_tie_dag();
  larch::edge_weight_score_ops ops;
  larch::subtree_weight<larch::edge_weight_score_ops> sw(dag, 271828u);
  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  assert(sw.min_weight_count(root_idx, ops) == larch::bigint{2});

  int alt1_count = 0;
  int alt2_count = 0;
  constexpr int trials = 2000;
  for (int i = 0; i < trials; ++i) {
    auto sampled = sw.min_weight_uniform_sample_tree(ops);
    auto chosen = selected_leaf_sequence(sampled, "CAAA", "ACAA");
    if (chosen == "CAAA")
      ++alt1_count;
    else if (chosen == "ACAA")
      ++alt2_count;
    else
      assert(false && "unexpected edge-weight sampled alternative");
  }

  assert(alt1_count + alt2_count == trials);
  assert(alt1_count > trials * 0.4 && alt1_count < trials * 0.6);
  assert(alt2_count > trials * 0.4 && alt2_count < trials * 0.6);

  std::println("  balanced edge-weight samples: alt1={}, alt2={}",
               alt1_count, alt2_count);
  std::println("  PASS");
}

static void test_edge_weight_min_global_scores() {
  std::println("test_edge_weight_min_global_scores");

  auto dag = make_edge_weight_alternative_dag();
  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  larch::edge_weight_score_ops ops;
  larch::subtree_weight<larch::edge_weight_score_ops> sw(dag, 42u);

  auto global_min = sw.compute_weight_below(root_idx, ops);
  auto scores = sw.compute_edge_min_global_scores(ops);
  assert(std::abs(global_min - 3.5) < 1e-10);

  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto eidx = edge.index();
          assert(scores[eidx] + 1e-10 >= global_min);
          if (edge.edge_weight() < 2.0f) {
            assert(std::abs(scores[eidx] - 3.5) < 1e-10);
          } else {
            assert(std::abs(scores[eidx] - 30.5) < 1e-10);
          }
        },
        ev);
  }

  std::println("  PASS");
}

static void test_edge_weight_round_trip_and_merge() {
  std::println("test_edge_weight_round_trip_and_merge");

  auto dag = make_edge_weight_alternative_dag();
  auto original_sum = sum_edge_weights(dag);
  assert(std::abs(original_sum - 33.5) < 1e-10);

  auto tmp_path =
      larch::test::unique_temp_path("subtree_weight_edge_weight_rt", ".pb");
  larch::save_proto_dag(dag, tmp_path.string());

  auto loaded = larch::load_proto_dag(tmp_path.string());
  assert(std::abs(sum_edge_weights(loaded) - original_sum) < 1e-10);

  larch::edge_weight_score_ops ops;
  auto loaded_root_idx =
      std::visit([](auto n) { return n.index(); }, loaded.get_root());
  larch::subtree_weight<larch::edge_weight_score_ops> loaded_sw(loaded, 42u);
  assert(std::abs(loaded_sw.compute_weight_below(loaded_root_idx, ops) - 3.5) <
         1e-10);

  larch::merge m{larch::get_reference_sequence(loaded)};
  m.add_dag(loaded);
  auto& merged = m.get_result();
  assert(std::abs(sum_edge_weights(merged) - original_sum) < 1e-10);
  auto merged_root_idx =
      std::visit([](auto n) { return n.index(); }, merged.get_root());
  larch::subtree_weight<larch::edge_weight_score_ops> merged_sw(merged, 42u);
  assert(std::abs(merged_sw.compute_weight_below(merged_root_idx, ops) - 3.5) <
         1e-10);

  std::filesystem::remove(tmp_path);
  std::println("  PASS");
}

static void test_edge_parsimony_round_trip() {
  std::println("test_edge_parsimony_round_trip");

  test_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto& dag = f.merger.get_result();
  larch::parsimony_score_ops pops;
  larch::subtree_weight<larch::parsimony_score_ops> sw(dag, 42u);

  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto global_min = sw.compute_weight_below(root_idx, pops);
  auto scores = sw.compute_edge_min_global_scores(pops);

  // Convert to penalty floats
  std::vector<float> penalties(scores.size());
  for (std::size_t i = 0; i < scores.size(); ++i)
    penalties[i] = static_cast<float>(scores[i] - global_min);

  // Save with edge weights
  auto tmp_path = larch::test::unique_temp_path(
      "subtree_weight_edge_parsimony_rt", ".pb");
  larch::save_proto_dag(dag, tmp_path.string(), penalties);

  // Load raw protobuf and check edge_weight values
  auto file_bytes = larch::read_file(tmp_path.string());
  std::span<const uint8_t> data{
      reinterpret_cast<const uint8_t*>(file_bytes.data()), file_bytes.size()};
  auto edge_spans = pb::collect_field_spans(data, 1);
  std::vector<larch::dag_edge> loaded_edges(edge_spans.size());
  for (std::size_t i = 0; i < edge_spans.size(); ++i)
    loaded_edges[i] = pb::decode<larch::dag_edge>(edge_spans[i]);

  assert(loaded_edges.size() == larch::edge_count(dag));

  std::size_t nonzero_weights = 0;
  for (auto& e : loaded_edges) {
    // edge_weight should be a valid float (not NaN)
    assert(!std::isnan(e.edge_weight));
    assert(e.edge_weight >= 0.0f);
    if (e.edge_weight > 0.0f) ++nonzero_weights;
  }

  std::println("  saved {} edges, {} with nonzero penalty, global_min={}",
               loaded_edges.size(), nonzero_weights, global_min);
  // We know from the CLI test that 112 edges have nonzero penalty
  assert(nonzero_weights > 0);

  // Clean up
  std::filesystem::remove(tmp_path);

  std::println("  PASS");
}

int main() {
  test_sample_tree();
  test_tree_count();
  test_uniform_sample_tree();
  test_min_weight_sample_tree();
  test_min_weight_count();
  test_min_weight_uniform_sample();
  test_weight_above();
  test_edge_parsimony_consistency();
  test_edge_parsimony_single_tree();
  test_edge_parsimony_ua_root_edge();
  test_edge_parsimony_rerooting_identity();
  test_edge_parsimony_exhaustive_ground_truth();
  test_edge_weight_score_ops_selects_min_alternative();
  test_edge_weight_score_ops_tie_selection();
  test_edge_weight_uniform_tie_sampling_balance();
  test_edge_weight_min_global_scores();
  test_edge_weight_round_trip_and_merge();
  test_edge_parsimony_round_trip();

  std::println("All subtree_weight tests passed!");
  return 0;
}

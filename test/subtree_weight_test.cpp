#include <larch/subtree_weight.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/merge.hpp>
#include <larch/protobuf.hpp>
#include <larch/io_util.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <print>
#include <set>
#include <string>
#include <variant>
#include <vector>

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
      trees.emplace_back(larch::load_proto_dag(path));
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
    for (auto eidx : chosen_edges)
      tree_score += pops.compute_edge(dag, eidx);

    // Update min score seen for each edge in this tree
    for (auto eidx : chosen_edges) {
      if (tree_score < min_score_seen[eidx])
        min_score_seen[eidx] = tree_score;
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
  std::string tmp_path = "/tmp/test_edge_parsimony_rt.pb";
  larch::save_proto_dag(dag, tmp_path, penalties);

  // Load raw protobuf and check edge_weight values
  auto file_bytes = larch::read_file(tmp_path);
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
  std::remove(tmp_path.c_str());

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
  test_edge_parsimony_round_trip();

  std::println("All subtree_weight tests passed!");
  return 0;
}

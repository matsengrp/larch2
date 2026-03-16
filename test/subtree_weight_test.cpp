#include <larch/subtree_weight.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/merge.hpp>

#include <cassert>
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

int main() {
  test_sample_tree();
  test_tree_count();
  test_uniform_sample_tree();
  test_min_weight_sample_tree();
  test_min_weight_count();
  test_min_weight_uniform_sample();

  std::println("All subtree_weight tests passed!");
  return 0;
}

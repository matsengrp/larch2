#include <larch/random_optimize.hpp>
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
// helpers (same patterns as subtree_weight_test / rf_distance_test)
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

// Same 6-leaf tree used in rf_distance_test
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

// Same leaves, different topology
static phylo_dag make_nonintersecting_sample_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto n1 = d.append_node<node_kind::leaf>();
  n1.cg() = cg_from_sequence("TCC", ref);
  n1.sample_id() = n1.cg().to_string();
  auto n2 = d.append_node<node_kind::leaf>();
  n2.cg() = cg_from_sequence("ACG", ref);
  n2.sample_id() = n2.cg().to_string();
  auto n3 = d.append_node<node_kind::leaf>();
  n3.cg() = cg_from_sequence("GTT", ref);
  n3.sample_id() = n3.cg().to_string();
  auto n4 = d.append_node<node_kind::leaf>();
  n4.cg() = cg_from_sequence("AGG", ref);
  n4.sample_id() = n4.cg().to_string();
  auto n5 = d.append_node<node_kind::leaf>();
  n5.cg() = cg_from_sequence("ACC", ref);
  n5.sample_id() = n5.cg().to_string();
  auto n6 = d.append_node<node_kind::leaf>();
  n6.cg() = cg_from_sequence("CTT", ref);
  n6.sample_id() = n6.cg().to_string();

  auto n7 = d.append_node<node_kind::inner>();
  n7.cg() = cg_from_sequence("AAA", ref);
  auto n8 = d.append_node<node_kind::inner>();
  n8.cg() = cg_from_sequence("AAA", ref);
  auto n9 = d.append_node<node_kind::inner>();
  n9.cg() = cg_from_sequence("AAA", ref);
  auto n10 = d.append_node<node_kind::inner>();
  n10.cg() = cg_from_sequence("GAA", ref);

  add_edge(d, ua.index(), n10.index(), 0);
  add_edge(d, n10.index(), n3.index(), 0);
  add_edge(d, n10.index(), n8.index(), 1);
  add_edge(d, n10.index(), n9.index(), 2);
  add_edge(d, n8.index(), n1.index(), 0);
  add_edge(d, n8.index(), n2.index(), 1);
  add_edge(d, n9.index(), n6.index(), 0);
  add_edge(d, n9.index(), n7.index(), 1);
  add_edge(d, n7.index(), n4.index(), 0);
  add_edge(d, n7.index(), n5.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// ---------------------------------------------------------------------------
// move strategies for testing
// ---------------------------------------------------------------------------

struct noop_strategy {
  std::vector<phylo_dag> operator()(phylo_dag& /*tree*/,
                                    std::mt19937& /*rng*/) {
    return {};
  }
};

// Returns the alternative topology once, then nothing.
struct one_shot_alternative_strategy {
  bool fired = false;

  std::vector<phylo_dag> operator()(phylo_dag& /*tree*/,
                                    std::mt19937& /*rng*/) {
    if (fired) return {};
    fired = true;
    std::vector<phylo_dag> result;
    result.push_back(make_nonintersecting_sample_dag());
    return result;
  }
};

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
// tests
// ---------------------------------------------------------------------------

// No-op strategy: re-merging sampled trees that are already in the DAG should
// be idempotent thanks to hash-chain deduplication.
static void test_noop_optimizer() {
  std::println("test_noop_optimizer");

  proto_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  auto initial_nodes = f.merger.result_node_count();
  auto initial_edges = f.merger.result_edge_count();
  std::println("  initial: {} nodes, {} edges", initial_nodes, initial_edges);

  noop_strategy strategy;
  auto results = optimize_dag(f.merger, strategy, 3, 42u);

  auto final_nodes = f.merger.result_node_count();
  auto final_edges = f.merger.result_edge_count();
  std::println("  final:   {} nodes, {} edges", final_nodes, final_edges);

  assert(final_nodes == initial_nodes);
  assert(final_edges == initial_edges);

  // Leaf set must be preserved.
  auto final_leaves = get_leaf_ids(f.merger.get_result());
  assert(final_leaves == f.leaf_ids);

  // All iterations should have merged 0 extra trees.
  for (auto& r : results) assert(r.trees_merged == 0);

  std::println("  PASS");
}

// Merging an alternative topology should grow the DAG.
static void test_optimizer_with_alternative_trees() {
  std::println("test_optimizer_with_alternative_trees");

  auto dag1 = make_sample_dag();
  merge m("GAA");
  m.add_dag(dag1);

  auto initial_nodes = m.result_node_count();
  auto initial_edges = m.result_edge_count();
  std::println("  initial: {} nodes, {} edges", initial_nodes, initial_edges);

  one_shot_alternative_strategy strategy;
  auto results = optimize_dag(m, strategy, 3, 42u);

  auto final_nodes = m.result_node_count();
  auto final_edges = m.result_edge_count();
  std::println("  final:   {} nodes, {} edges", final_nodes, final_edges);

  // The alternative topology should have added new nodes/edges.
  assert(final_nodes > initial_nodes || final_edges > initial_edges);

  // Leaf set must be preserved.
  auto original_leaves = get_leaf_ids(dag1);
  auto final_leaves = get_leaf_ids(m.get_result());
  assert(final_leaves == original_leaves);

  // First iteration should have merged 1 tree.
  assert(results[0].trees_merged == 1);
  // Subsequent iterations: 0 (one_shot fires only once).
  for (std::size_t i = 1; i < results.size(); ++i)
    assert(results[i].trees_merged == 0);

  std::println("  PASS");
}

// After optimisation, sampled trees must be structurally valid.
static void test_optimizer_dag_validity() {
  std::println("test_optimizer_dag_validity");

  auto dag1 = make_sample_dag();
  auto dag2 = make_nonintersecting_sample_dag();
  merge m("GAA");
  m.add_dag(dag1);
  m.add_dag(dag2);

  noop_strategy strategy;
  optimize_dag(m, strategy, 2, 42u);

  auto original_leaves = get_leaf_ids(dag1);

  parsimony_score_ops pops;
  subtree_weight<parsimony_score_ops> sw(m.get_result(), 99u);

  for (int i = 0; i < 5; ++i) {
    auto tree = sw.min_weight_sample_tree(pops);
    verify_is_tree(tree);
    auto tree_leaves = get_leaf_ids(tree);
    assert(tree_leaves == original_leaves);
  }

  std::println("  PASS");
}

// Parsimony scores in optimize_result must be valid.
static void test_optimizer_parsimony_scores() {
  std::println("test_optimizer_parsimony_scores");

  proto_fixture f(
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"});

  noop_strategy strategy;
  auto results = optimize_dag(f.merger, strategy, 3, 42u);

  for (auto& r : results) {
    std::println("  iter {}: parsimony={} nodes={} edges={}", r.iteration,
                 r.parsimony_score, r.dag_node_count, r.dag_edge_count);
    assert(r.parsimony_score > 0);
  }

  // Verify by sampling: mutation count on min-weight tree should match.
  parsimony_score_ops pops;
  subtree_weight<parsimony_score_ops> sw(f.merger.get_result(), 123u);
  auto root_idx = get_root_idx(f.merger.get_result());
  auto expected_score = sw.compute_weight_below(root_idx, pops);

  auto tree = sw.min_weight_sample_tree(pops);
  std::size_t total_mutations = 0;
  for (auto ev : tree.get_all_edges()) {
    std::visit([&](auto edge) { total_mutations += edge.mutations().size(); },
               ev);
  }
  assert(total_mutations == expected_score);
  std::println("  verified min parsimony = {}", expected_score);

  std::println("  PASS");
}

// Same seed should produce identical results.
static void test_optimizer_determinism() {
  std::println("test_optimizer_determinism");

  auto run = [](std::uint32_t seed) {
    auto dag = make_sample_dag();
    merge m("GAA");
    m.add_dag(dag);
    one_shot_alternative_strategy strategy;
    return optimize_dag(m, strategy, 3, seed);
  };

  auto r1 = run(42u);
  auto r2 = run(42u);

  assert(r1.size() == r2.size());
  for (std::size_t i = 0; i < r1.size(); ++i) {
    assert(r1[i].dag_node_count == r2[i].dag_node_count);
    assert(r1[i].dag_edge_count == r2[i].dag_edge_count);
    assert(r1[i].trees_merged == r2[i].trees_merged);
    assert(r1[i].parsimony_score == r2[i].parsimony_score);
  }

  std::println("  PASS");
}

int main() {
  test_noop_optimizer();
  test_optimizer_with_alternative_trees();
  test_optimizer_dag_validity();
  test_optimizer_parsimony_scores();
  test_optimizer_determinism();

  std::println("All optimize tests passed!");
  return 0;
}

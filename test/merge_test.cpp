#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/load_parsimony.hpp>
#include <larch/merge.hpp>
#include <larch/thread_pool.hpp>

#include <larch/io_util.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

static void test_protobuf(std::string name,
                          std::vector<std::string> const& paths,
                          std::size_t expected_nodes,
                          std::size_t expected_edges) {
  std::println("{}", name);

  std::vector<larch::phylo_dag> trees;
  trees.reserve(paths.size());
  for (auto& path : paths) {
    trees.emplace_back(larch::load_proto_dag(path));
    larch::recompute_compact_genomes(trees.back());
    larch::set_sample_ids_from_cg(trees.back(), /*coerce=*/true);
  }

  auto ref = larch::get_reference_sequence(trees[0]);
  larch::merge m{ref};
  std::vector<larch::phylo_dag*> ptrs;
  for (auto& t : trees) ptrs.push_back(&t);
  m.add_dags(ptrs);

  auto got_nodes = m.result_node_count();
  auto got_edges = m.result_edge_count();

  assert(expected_nodes == got_nodes);
  assert(expected_edges == got_edges);
  std::println("  PASS");
}

static void test_case_2() {
  test_protobuf("test_case_2",
                {"data/testcase2/tree_0.pb.gz", "data/testcase2/tree_1.pb.gz",
                 "data/testcase2/tree_2.pb.gz", "data/testcase2/tree_3.pb.gz",
                 "data/testcase2/tree_4.pb.gz"},
                130, 201);
}

static void test_five_trees() {
  test_protobuf(
      "test_five_trees",
      {"data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
       "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
       "data/test_5_trees/tree_4.pb.gz"},
      124, 179);
}

static void test_case_ref() {
  test_protobuf(
      "test_case_ref",
      {"data/testcaseref/tree_0.pb.gz", "data/testcaseref/tree_1.pb.gz",
       "data/testcaseref/tree_2.pb.gz", "data/testcaseref/tree_3.pb.gz",
       "data/testcaseref/tree_4.pb.gz"},
      130, 201);
}

static void test_add_trees() {
  std::println("test_add_trees");

  std::vector<std::string> paths1 = {"data/test_5_trees/tree_0.pb.gz",
                                     "data/test_5_trees/tree_1.pb.gz"};
  std::vector<std::string> paths2 = {"data/test_5_trees/tree_2.pb.gz",
                                     "data/test_5_trees/tree_3.pb.gz",
                                     "data/test_5_trees/tree_4.pb.gz"};

  std::vector<larch::phylo_dag> trees1, trees2;
  for (auto& path : paths1) {
    trees1.emplace_back(larch::load_proto_dag(path));
    larch::recompute_compact_genomes(trees1.back());
    larch::set_sample_ids_from_cg(trees1.back(), /*coerce=*/true);
  }
  for (auto& path : paths2) {
    trees2.emplace_back(larch::load_proto_dag(path));
    larch::recompute_compact_genomes(trees2.back());
    larch::set_sample_ids_from_cg(trees2.back(), /*coerce=*/true);
  }

  auto ref = larch::get_reference_sequence(trees1[0]);
  larch::merge m{ref};

  std::vector<larch::phylo_dag*> ptrs1;
  for (auto& t : trees1) ptrs1.push_back(&t);
  m.add_dags(ptrs1);

  std::vector<larch::phylo_dag*> ptrs2;
  for (auto& t : trees2) ptrs2.push_back(&t);
  m.add_dags(ptrs2);

  auto got_nodes = m.result_node_count();
  auto got_edges = m.result_edge_count();

  assert(124 == got_nodes);
  assert(179 == got_edges);
  std::println("  PASS");
}

static void test_subtree() {
  std::println("test_subtree");

  std::vector<std::string> paths = {
      "data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
      "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
      "data/test_5_trees/tree_4.pb.gz"};

  auto first = larch::load_proto_dag(paths[0]);
  larch::recompute_compact_genomes(first);
  larch::set_sample_ids_from_cg(first, /*coerce=*/true);
  auto ref = larch::get_reference_sequence(first);

  larch::merge m{ref};
  m.add_dag(first);
  for (std::size_t i = 1; i < paths.size(); ++i) {
    auto tree = larch::load_proto_dag(paths[i]);
    larch::recompute_compact_genomes(tree);
    larch::set_sample_ids_from_cg(tree, /*coerce=*/true);
    m.add_dag(std::move(tree));
  }

  auto got_nodes = m.result_node_count();
  auto got_edges = m.result_edge_count();

  assert(124 == got_nodes);
  assert(179 == got_edges);
  std::println("  PASS");
}

static void test_case_20d() {
  std::println("test_case_20d (800 parsimony trees)");

  // Read reference sequence from file
  auto refseq_bytes = larch::read_file("data/20D_from_fasta/refseq.txt");
  std::string ref{refseq_bytes.begin(), refseq_bytes.end()};

  // Collect all parsimony tree paths
  std::vector<std::string> paths;
  for (auto& entry :
       std::filesystem::directory_iterator{"data/20D_from_fasta"}) {
    auto name = entry.path().filename().string();
    if (name.starts_with("1final-tree-"))
      paths.push_back(entry.path().string());
  }
  std::sort(paths.begin(), paths.end());
  std::println("  loading {} trees...", paths.size());

  // Load all parsimony trees in parallel
  auto& pool = larch::thread_pool::get_default();
  std::vector<std::future<larch::phylo_dag>> futures;
  futures.reserve(paths.size());
  for (auto& path : paths) {
    futures.push_back(pool.submit([&path, &ref] {
      auto tree = larch::load_parsimony_tree(path, ref);
      larch::recompute_compact_genomes(tree);
      return tree;
    }));
  }
  std::vector<larch::phylo_dag> trees;
  trees.reserve(paths.size());
  for (auto& f : futures) {
    trees.push_back(f.get());
  }

  // Merge
  std::println("  merging...");
  auto t0 = std::chrono::steady_clock::now();
  larch::merge m{ref};
  std::vector<larch::phylo_dag*> ptrs;
  for (auto& t : trees) ptrs.push_back(&t);
  m.add_dags(ptrs);

  auto got_nodes = m.result_node_count();
  auto got_edges = m.result_edge_count();
  auto t1 = std::chrono::steady_clock::now();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  std::println("  merged in {} ms: {} nodes, {} edges", ms, got_nodes,
               got_edges);
  assert(5802 == got_nodes);
  assert(8615 == got_edges);
  std::println("  PASS");
}

int main(int argc, char** argv) {
  bool run_slow = argc > 1 && std::string_view{argv[1]} == "--slow";

  test_case_2();
  test_five_trees();
  test_case_ref();
  test_add_trees();
  test_subtree();

  if (run_slow) {
    test_case_20d();
  } else {
    std::println("(skipping test_case_20d, pass --slow to run)");
  }

  std::println("All tests passed!");
  return 0;
}

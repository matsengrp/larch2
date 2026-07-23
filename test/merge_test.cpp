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
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

static void test_compact_genome_add_parent_edge_ordered_scan() {
  std::println("test_compact_genome_add_parent_edge_ordered_scan");

  auto base = [](char value) { return larch::nuc_base::from_char(value); };
  larch::compact_genome parent{
      std::map<larch::mutation_position, larch::nuc_base>{
          {2, base('C')}, {4, base('G')}, {6, base('T')}}};
  larch::compact_genome actual{
      std::map<larch::mutation_position, larch::nuc_base>{
          {1, base('G')}, {4, base('C')}, {7, base('T')}}};
  larch::edge_mutations edge{
      {1, {base('G'), base('A')}}, {2, {base('C'), base('T')}},
      {3, {base('A'), base('C')}}, {4, {base('G'), base('A')}},
      {5, {base('A'), base('A')}}, {6, {base('T'), base('G')}},
      {8, {base('A'), base('C')}}};

  actual.add_parent_edge(edge, parent, "AAAAAAAA");
  larch::compact_genome const expected{
      std::map<larch::mutation_position, larch::nuc_base>{
          {2, base('T')}, {3, base('C')}, {6, base('G')},
          {7, base('T')}, {8, base('C')}}};
  if (actual != expected) {
    throw std::runtime_error(
        "ordered add_parent_edge scan changed successful semantics");
  }

  larch::compact_genome empty_actual;
  empty_actual.add_parent_edge(edge, parent, "AAAAAAAA");
  larch::compact_genome const expected_from_empty{
      std::map<larch::mutation_position, larch::nuc_base>{
          {2, base('T')}, {3, base('C')}, {6, base('G')}, {8, base('C')}}};
  if (empty_actual != expected_from_empty) {
    throw std::runtime_error(
        "empty-target add_parent_edge clone changed successful semantics");
  }

  larch::compact_genome self_parent = parent;
  self_parent.add_parent_edge(
      larch::edge_mutations{{2, {base('C'), base('A')}},
                            {3, {base('A'), base('T')}}},
      self_parent, "AAAAAAAA");
  larch::compact_genome const expected_self_parent{
      std::map<larch::mutation_position, larch::nuc_base>{
          {3, base('T')}, {4, base('G')}, {6, base('T')}}};
  if (self_parent != expected_self_parent) {
    throw std::runtime_error(
        "add_parent_edge changed self-parent alias semantics");
  }

  larch::compact_genome partial{
      std::map<larch::mutation_position, larch::nuc_base>{{7, base('T')}}};
  larch::edge_mutations invalid_edge{
      {2, {base('C'), base('A')}}, {4, {base('A'), base('T')}},
      {9, {base('A'), base('C')}}};
  bool threw = false;
  try {
    partial.add_parent_edge(invalid_edge, parent, "AAAAAAAA");
  } catch (std::out_of_range const&) {
    threw = true;
  }
  if (!threw || partial.to_string() != "<4T,6T,7T,>") {
    throw std::runtime_error(
        "ordered add_parent_edge scan changed reference validation order");
  }

  larch::compact_genome empty_partial;
  threw = false;
  try {
    empty_partial.add_parent_edge(invalid_edge, parent, "AAAAAAAA");
  } catch (std::out_of_range const&) {
    threw = true;
  }
  if (!threw || empty_partial.to_string() != "<4T,6T,>") {
    throw std::runtime_error(
        "empty-target add_parent_edge clone changed validation order");
  }
  larch::compact_genome const expected_empty_partial{
      std::map<larch::mutation_position, larch::nuc_base>{
          {4, base('T')}, {6, base('T')}}};
  larch::compact_genome recovered_partial;
  recovered_partial.add_parent_edge({}, empty_partial, "AAAAAAAA");
  if (recovered_partial != expected_empty_partial ||
      recovered_partial.hash() != expected_empty_partial.hash()) {
    throw std::runtime_error(
        "shared empty-edge inheritance propagated a stale failed hash");
  }
  empty_partial.add_parent_edge({}, empty_partial, "AAAAAAAA");
  if (empty_partial != expected_empty_partial ||
      empty_partial.hash() != expected_empty_partial.hash()) {
    throw std::runtime_error(
        "self-parent empty-edge recovery did not recompute a stale hash");
  }
  larch::compact_genome const expected_parent{
      std::map<larch::mutation_position, larch::nuc_base>{
          {2, base('C')}, {4, base('G')}, {6, base('T')}}};
  if (parent != expected_parent || parent.hash() != expected_parent.hash()) {
    throw std::runtime_error(
        "copy-on-write compact-genome mutation changed its shared parent");
  }

  std::println("  PASS");
}

static void test_recompute_edge_mutations_from_immutable_genomes() {
  std::println("test_recompute_edge_mutations_from_immutable_genomes");

  larch::phylo_dag dag;
  auto ua = dag.append_node<larch::node_kind::ua>();
  ua.reference_sequence() = "AAAAAAAA";
  dag.set_root(ua);
  auto inner = dag.append_node<larch::node_kind::inner>();
  auto leaf = dag.append_node<larch::node_kind::leaf>();
  leaf.sample_id() = "sample";
  auto inherited_leaf = dag.append_node<larch::node_kind::leaf>();
  inherited_leaf.sample_id() = "inherited-sample";

  auto base = [](char value) { return larch::nuc_base::from_char(value); };
  inner.cg() = larch::compact_genome{
      std::map<larch::mutation_position, larch::nuc_base>{
          {2, base('C')}, {4, base('G')}, {6, base('T')}, {8, base('C')}}};
  leaf.cg() = larch::compact_genome{
      std::map<larch::mutation_position, larch::nuc_base>{
          {1, base('G')}, {2, base('C')}, {3, base('T')}, {4, base('T')},
          {7, base('C')}}};
  inherited_leaf.cg() = inner.cg();

  auto ua_edge = ua.append_child<larch::edge_kind::clade>();
  ua_edge.set_child(inner);
  ua_edge.clade_index() = 0;
  auto leaf_edge = inner.append_child<larch::edge_kind::clade>();
  leaf_edge.set_child(leaf);
  leaf_edge.clade_index() = 0;
  auto inherited_edge = inner.append_child<larch::edge_kind::clade>();
  inherited_edge.set_child(inherited_leaf);
  inherited_edge.clade_index() = 1;
  ua_edge.mutations()[1] = {base('A'), base('T')};
  leaf_edge.mutations()[2] = {base('A'), base('G')};

  larch::recompute_edge_mutations(dag);

  larch::edge_mutations const expected_ua{
      {2, {base('A'), base('C')}},
      {4, {base('A'), base('G')}},
      {6, {base('A'), base('T')}},
      {8, {base('A'), base('C')}},
  };
  larch::edge_mutations const expected_leaf{
      {1, {base('A'), base('G')}},
      {3, {base('A'), base('T')}},
      {4, {base('G'), base('T')}},
      {6, {base('T'), base('A')}},
      {7, {base('A'), base('C')}},
      {8, {base('C'), base('A')}},
  };
  if (ua_edge.mutations() != expected_ua ||
      leaf_edge.mutations() != expected_leaf ||
      !inherited_edge.mutations().empty()) {
    throw std::runtime_error(
        "recompute_edge_mutations produced an incorrect mutation map");
  }

  auto const expected_inner_cg = inner.cg();
  auto const expected_leaf_cg = leaf.cg();
  auto make_recompute_input = [&] {
    larch::phylo_dag input;
    auto input_ua = input.append_node<larch::node_kind::ua>();
    input_ua.reference_sequence() = "AAAAAAAA";
    input.set_root(input_ua);
    auto input_inner = input.append_node<larch::node_kind::inner>();
    auto input_leaf = input.append_node<larch::node_kind::leaf>();
    input_leaf.sample_id() = "sample";
    auto input_inherited_leaf = input.append_node<larch::node_kind::leaf>();
    input_inherited_leaf.sample_id() = "inherited-sample";
    input_inner.cg() = larch::compact_genome{};
    input_leaf.cg() = larch::compact_genome{
        std::map<larch::mutation_position, larch::nuc_base>{{8, base('T')}}};
    input_inherited_leaf.cg() = larch::compact_genome{
        std::map<larch::mutation_position, larch::nuc_base>{{7, base('G')}}};
    auto input_ua_edge = input_ua.append_child<larch::edge_kind::clade>();
    input_ua_edge.set_child(input_inner);
    input_ua_edge.clade_index() = 0;
    input_ua_edge.mutations() = expected_ua;
    auto input_leaf_edge =
        input_inner.append_child<larch::edge_kind::clade>();
    input_leaf_edge.set_child(input_leaf);
    input_leaf_edge.clade_index() = 0;
    input_leaf_edge.mutations() = expected_leaf;
    auto input_inherited_edge =
        input_inner.append_child<larch::edge_kind::clade>();
    input_inherited_edge.set_child(input_inherited_leaf);
    input_inherited_edge.clade_index() = 1;
    return input;
  };

  auto serial_dag = make_recompute_input();
  auto parallel_dag = make_recompute_input();
  auto worker_one_dag = make_recompute_input();
  larch::recompute_compact_genomes(serial_dag);
  if (!larch::recompute_compact_genomes_parallel_tree(
          parallel_dag, 4, larch::thread_pool::get_default())) {
    throw std::runtime_error(
        "proper tree did not use parallel compact-genome recomputation");
  }
  if (larch::recompute_compact_genomes_parallel_tree(
          worker_one_dag, 1, larch::thread_pool::get_default())) {
    throw std::runtime_error(
        "one worker unexpectedly reported parallel CG recomputation");
  }
  auto compact_genome_at = [](larch::phylo_dag& input,
                              std::size_t node_index) {
    larch::compact_genome result;
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.cg(); }) result = node.cg();
        },
        input.get_node(node_index));
    return result;
  };
  for (auto node_index :
       {inner.index(), leaf.index(), inherited_leaf.index()}) {
    if (compact_genome_at(parallel_dag, node_index) !=
            compact_genome_at(serial_dag, node_index) ||
        compact_genome_at(worker_one_dag, node_index) !=
            compact_genome_at(serial_dag, node_index)) {
      throw std::runtime_error(
          "parallel compact-genome tree recomputation changed a node");
    }
  }
  auto const serial_inner_cg = compact_genome_at(serial_dag, inner.index());
  auto const serial_leaf_cg = compact_genome_at(serial_dag, leaf.index());
  auto const serial_inherited_cg =
      compact_genome_at(serial_dag, inherited_leaf.index());
  if (serial_inner_cg != expected_inner_cg ||
      serial_leaf_cg != expected_leaf_cg ||
      serial_inherited_cg != expected_inner_cg ||
      serial_inherited_cg.hash() != expected_inner_cg.hash()) {
    throw std::runtime_error(
        "recompute_compact_genomes produced an incorrect compact genome");
  }

  std::println("  PASS");
}

static void test_recompute_compact_genomes_multiparent_bfs_choice() {
  std::println("test_recompute_compact_genomes_multiparent_bfs_choice");

  larch::phylo_dag dag;
  auto ua = dag.append_node<larch::node_kind::ua>();
  ua.reference_sequence() = "AAAAAAAA";
  dag.set_root(ua);
  auto parent_a = dag.append_node<larch::node_kind::inner>();
  auto middle = dag.append_node<larch::node_kind::inner>();
  auto parent_b = dag.append_node<larch::node_kind::inner>();
  auto child = dag.append_node<larch::node_kind::leaf>();
  child.sample_id() = "multiparent-child";
  auto base = [](char value) { return larch::nuc_base::from_char(value); };

  auto connect = [&](auto parent, auto destination) {
    auto edge = parent.template append_child<larch::edge_kind::clade>();
    edge.set_child(destination);
    edge.clade_index() = 0;
    return edge;
  };
  auto ua_a = connect(ua, parent_a);
  auto ua_middle = connect(ua, middle);
  auto middle_b = connect(middle, parent_b);
  // Put the not-yet-processed deeper parent first in the child's parent list.
  // The historical BFS must skip it and select the already processed A path.
  auto b_child = connect(parent_b, child);
  auto a_child = connect(parent_a, child);

  ua_a.mutations() = {
      {2, {base('A'), base('C')}}, {5, {base('A'), base('G')}}};
  ua_middle.mutations() = {{3, {base('A'), base('T')}}};
  middle_b.mutations() = {
      {3, {base('T'), base('C')}}, {6, {base('A'), base('G')}}};
  b_child.mutations() = {{1, {base('A'), base('G')}}};
  a_child.mutations() = {
      {2, {base('C'), base('A')}}, {4, {base('A'), base('T')}},
      {5, {base('G'), base('C')}}};

  auto wrong = larch::compact_genome{
      std::map<larch::mutation_position, larch::nuc_base>{{8, base('T')}}};
  parent_a.cg() = wrong;
  middle.cg() = wrong;
  parent_b.cg() = wrong;
  child.cg() = wrong;

  if (larch::recompute_compact_genomes_parallel_tree(
          dag, 4, larch::thread_pool::get_default())) {
    throw std::runtime_error(
        "multiparent DAG unexpectedly used tree CG specialization");
  }

  larch::compact_genome const expected_a{
      std::map<larch::mutation_position, larch::nuc_base>{{2, base('C')},
                                                          {5, base('G')}}};
  larch::compact_genome const expected_middle{
      std::map<larch::mutation_position, larch::nuc_base>{{3, base('T')}}};
  larch::compact_genome const expected_b{
      std::map<larch::mutation_position, larch::nuc_base>{{3, base('C')},
                                                          {6, base('G')}}};
  larch::compact_genome const expected_child{
      std::map<larch::mutation_position, larch::nuc_base>{{4, base('T')},
                                                          {5, base('C')}}};
  if (parent_a.cg() != expected_a || middle.cg() != expected_middle ||
      parent_b.cg() != expected_b || child.cg() != expected_child) {
    throw std::runtime_error(
        "recompute_compact_genomes changed its processed-parent choice");
  }

  std::println("  PASS");
}

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

  test_compact_genome_add_parent_edge_ordered_scan();
  test_recompute_edge_mutations_from_immutable_genomes();
  test_recompute_compact_genomes_multiparent_bfs_choice();
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

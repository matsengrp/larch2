#include <larch/rf_distance.hpp>
#include <larch/weight_accumulator.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/compute.hpp>
#include <larch/merge.hpp>

#include "test_util.hpp"

#include <cassert>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <print>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace larch;
using larch::test::cg_from_sequence;

// Helper: add edge between parent and child with given clade index
static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx, std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
}

// Build topology and set CGs from a sequence map, then recompute edge mutations
struct dag_builder {
  phylo_dag dag;
  std::vector<std::size_t> node_indices;  // maps logical id -> dag index

  dag_builder(std::string_view ref, std::size_t num_nodes) {
    // Node 0 = UA
    auto ua = dag.append_node<node_kind::ua>();
    ua.reference_sequence() = std::string{ref};
    dag.set_root(ua);
    node_indices.push_back(ua.index());

    // Remaining nodes: we need to know which are leaves.
    // We'll add them as inner first, then fix leaves later.
    for (std::size_t i = 1; i < num_nodes; ++i) {
      auto n = dag.append_node<node_kind::inner>();
      node_indices.push_back(n.index());
    }
  }

  std::size_t idx(std::size_t logical) const { return node_indices[logical]; }
};

// Larch sample DAG topology and sequences:
//   UA(0) -> 10(GAA)
//   10 -> 8(CTC) [clade 0], 10 -> 9(AGT) [clade 1]
//   8 -> 3(AGG) [clade 0], 8 -> 4(ACG) [clade 1], 8 -> 7(TGG) [clade 2]
//   7 -> 1(ACC) [clade 0], 7 -> 2(GTT) [clade 1]
//   9 -> 5(CTT) [clade 0], 9 -> 6(TCC) [clade 1]
// Leaves: 1,2,3,4,5,6
static phylo_dag make_sample_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  // Create UA
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  // Create leaf nodes: 1-6
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

  // Create inner nodes: 7,8,9,10
  auto n7 = d.append_node<node_kind::inner>();
  n7.cg() = cg_from_sequence("TGG", ref);
  auto n8 = d.append_node<node_kind::inner>();
  n8.cg() = cg_from_sequence("CTC", ref);
  auto n9 = d.append_node<node_kind::inner>();
  n9.cg() = cg_from_sequence("AGT", ref);
  auto n10 = d.append_node<node_kind::inner>();
  n10.cg() = cg_from_sequence("GAA", ref);  // root = ref

  // Edges: UA -> 10
  add_edge(d, ua.index(), n10.index(), 0);
  // 10 -> 8 [clade 0], 10 -> 9 [clade 1]
  add_edge(d, n10.index(), n8.index(), 0);
  add_edge(d, n10.index(), n9.index(), 1);
  // 8 -> 3 [clade 0], 8 -> 4 [clade 1], 8 -> 7 [clade 2]
  add_edge(d, n8.index(), n3.index(), 0);
  add_edge(d, n8.index(), n4.index(), 1);
  add_edge(d, n8.index(), n7.index(), 2);
  // 7 -> 1 [clade 0], 7 -> 2 [clade 1]
  add_edge(d, n7.index(), n1.index(), 0);
  add_edge(d, n7.index(), n2.index(), 1);
  // 9 -> 5 [clade 0], 9 -> 6 [clade 1]
  add_edge(d, n9.index(), n5.index(), 0);
  add_edge(d, n9.index(), n6.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// Nonintersecting sample DAG - same leaf set, different topology:
//   UA(0) -> 10(GAA)
//   10 -> 3(GTT) [clade 0], 10 -> 8(AAA) [clade 1], 10 -> 9(AAA) [clade 2]
//   8 -> 1(TCC) [clade 0], 8 -> 2(ACG) [clade 1]
//   9 -> 6(CTT) [clade 0], 9 -> 7(AAA) [clade 1]
//   7 -> 4(AGG) [clade 0], 7 -> 5(ACC) [clade 1]
// Leaves: 1,2,3,4,5,6
static phylo_dag make_nonintersecting_sample_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  // Leaf nodes - same 6 sequences as sample_dag but different positions
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

  // Inner nodes
  auto n7 = d.append_node<node_kind::inner>();
  n7.cg() = cg_from_sequence("AAA", ref);
  auto n8 = d.append_node<node_kind::inner>();
  n8.cg() = cg_from_sequence("AAA", ref);
  auto n9 = d.append_node<node_kind::inner>();
  n9.cg() = cg_from_sequence("AAA", ref);
  auto n10 = d.append_node<node_kind::inner>();
  n10.cg() = cg_from_sequence("GAA", ref);  // root = ref

  // Edges: UA -> 10
  add_edge(d, ua.index(), n10.index(), 0);
  // 10 -> 3 [clade 0], 10 -> 8 [clade 1], 10 -> 9 [clade 2]
  add_edge(d, n10.index(), n3.index(), 0);
  add_edge(d, n10.index(), n8.index(), 1);
  add_edge(d, n10.index(), n9.index(), 2);
  // 8 -> 1 [clade 0], 8 -> 2 [clade 1]
  add_edge(d, n8.index(), n1.index(), 0);
  add_edge(d, n8.index(), n2.index(), 1);
  // 9 -> 6 [clade 0], 9 -> 7 [clade 1]
  add_edge(d, n9.index(), n6.index(), 0);
  add_edge(d, n9.index(), n7.index(), 1);
  // 7 -> 4 [clade 0], 7 -> 5 [clade 1]
  add_edge(d, n7.index(), n4.index(), 0);
  add_edge(d, n7.index(), n5.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// Base sample DAG - same topology as sample_dag, different sequence map
static phylo_dag make_base_sample_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  // Leaf nodes with "base" sequence map
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

  // Inner nodes with "base" sequence map
  auto n7 = d.append_node<node_kind::inner>();
  n7.cg() = cg_from_sequence("TGG", ref);
  auto n8 = d.append_node<node_kind::inner>();
  n8.cg() = cg_from_sequence("CTC", ref);
  auto n9 = d.append_node<node_kind::inner>();
  n9.cg() = cg_from_sequence("AGT", ref);
  auto n10 = d.append_node<node_kind::inner>();
  n10.cg() = cg_from_sequence("GAA", ref);

  // Same topology as sample_dag
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

// Sample DAG with one unique node - 12 nodes, extra inner node 10 (GGA)
//   UA(0) -> 11(GAA)
//   11 -> 10(GGA) [clade 0], 11 -> 9(AGT) [clade 1]
//   10 -> 8(CTC) [clade 0], 10 -> 7(TGG) [clade 1]
//   8 -> 3(AGG) [clade 0], 8 -> 4(ACG) [clade 1]
//   7 -> 1(ACC) [clade 0], 7 -> 2(GTT) [clade 1]
//   9 -> 5(CTT) [clade 0], 9 -> 6(TCC) [clade 1]
static phylo_dag make_sample_dag_with_one_unique_node() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  // Leaf nodes (same 6 as sample_dag)
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

  // Inner nodes
  auto n7 = d.append_node<node_kind::inner>();
  n7.cg() = cg_from_sequence("TGG", ref);
  auto n8 = d.append_node<node_kind::inner>();
  n8.cg() = cg_from_sequence("CTC", ref);
  auto n9 = d.append_node<node_kind::inner>();
  n9.cg() = cg_from_sequence("AGT", ref);
  auto n10 = d.append_node<node_kind::inner>();  // New node
  n10.cg() = cg_from_sequence("GGA", ref);
  auto n11 = d.append_node<node_kind::inner>();
  n11.cg() = cg_from_sequence("GAA", ref);  // root = ref

  // Edges
  add_edge(d, ua.index(), n11.index(), 0);
  add_edge(d, n11.index(), n10.index(), 0);
  add_edge(d, n11.index(), n9.index(), 1);
  add_edge(d, n10.index(), n8.index(), 0);
  add_edge(d, n10.index(), n7.index(), 1);
  add_edge(d, n8.index(), n3.index(), 0);
  add_edge(d, n8.index(), n4.index(), 1);
  add_edge(d, n7.index(), n1.index(), 0);
  add_edge(d, n7.index(), n2.index(), 1);
  add_edge(d, n9.index(), n5.index(), 0);
  add_edge(d, n9.index(), n6.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

// RF distance types
enum class rf_distance_type { Min, MinSum, Max, MaxSum };

static bigint get_rf_distance(merge& comp_merge, merge& ref_merge,
                              rf_distance_type type = rf_distance_type::MinSum,
                              bool print_info = true) {
  auto& dag1 = comp_merge.get_result();
  bigint shift_sum, result;

  if (type == rf_distance_type::Min) {
    subtree_weight<rf_distance> count(dag1);
    rf_distance weight_ops{ref_merge, comp_merge};
    shift_sum = weight_ops.get_ops().get_shift_sum();
    result =
        count.compute_weight_below(get_root_idx(dag1), weight_ops) + shift_sum;
  } else if (type == rf_distance_type::MinSum) {
    subtree_weight<sum_rf_distance> count(dag1);
    sum_rf_distance weight_ops{sum_rf_distance_ops{ref_merge, comp_merge}};
    shift_sum = weight_ops.get_ops().get_shift_sum();
    result =
        count.compute_weight_below(get_root_idx(dag1), weight_ops) + shift_sum;
  } else if (type == rf_distance_type::Max) {
    subtree_weight<max_rf_distance> count(dag1);
    max_rf_distance weight_ops{ref_merge, comp_merge};
    shift_sum = weight_ops.get_ops().get_shift_sum();
    result =
        count.compute_weight_below(get_root_idx(dag1), weight_ops) + shift_sum;
  } else {
    subtree_weight<max_sum_rf_distance> count(dag1);
    max_sum_rf_distance weight_ops{
        max_sum_rf_distance_ops{ref_merge, comp_merge}};
    shift_sum = weight_ops.get_ops().get_shift_sum();
    result =
        count.compute_weight_below(get_root_idx(dag1), weight_ops) + shift_sum;
  }

  if (is_tree(ref_merge.get_result())) {
    assert(shift_sum == bigint(node_count(ref_merge.get_result()) - 1));
  }
  if (print_info) {
    std::cout << "shift_sum: " << shift_sum << "\n";
    std::cout << "rf_distance: " << result << "\n";
  }
  return result;
}

// Test 1: DAG vs itself = 0
static void test_zero_rf_distance() {
  std::println("test_zero_rf_distance");
  auto dag = make_sample_dag();
  merge m("GAA");
  m.add_dag(dag);
  auto dist = get_rf_distance(m, m);
  assert(dist == bigint{0});
  std::println("  PASS");
}

// Test 2: same topology, different CGs = 0
static void test_rf_on_two_identical_topologies() {
  std::println("test_rf_on_two_identical_topologies");
  auto dag1 = make_sample_dag();
  auto dag2 = make_sample_dag();
  recompute_compact_genomes(dag1);
  recompute_compact_genomes(dag2);

  // Change internal node CGs of dag2 to "AAA" (relative to ref "GAA")
  auto aaa_cg = cg_from_sequence("AAA", "GAA");
  for (auto nv : dag2.get_all_nodes()) {
    std::visit(
        [&](auto n) {
          if constexpr (requires { n.cg(); }) {
            if constexpr (!requires { n.sample_id(); }) {
              n.cg() = aaa_cg;
            }
          }
        },
        nv);
  }
  recompute_edge_mutations(dag2);

  merge merge1("GAA");
  merge1.add_dag(dag1);
  merge merge2("GAA");
  merge2.add_dag(dag1);
  merge2.add_dag(dag2);

  auto dist = get_rf_distance(merge1, merge2);
  assert(dist == bigint{0});
  std::println("  PASS");
}

// Test 3: two distinct topologies in single merge
static void test_rf_two_distinct_topologies_single_merge() {
  std::println("test_rf_two_distinct_topologies_single_merge");
  auto dag1 = make_sample_dag();
  auto dag2 = make_nonintersecting_sample_dag();

  auto ec1 = edge_count(dag1);
  auto ec2 = edge_count(dag2);
  auto lc1 = leaf_count(dag1);
  auto lc2 = leaf_count(dag2);

  merge m("GAA");
  m.add_dag(dag1);
  m.add_dag(dag2);
  auto dist = get_rf_distance(m, m);
  auto truedist = bigint(ec1 + ec2 - lc1 - lc2 - 2);
  if (dist != truedist) {
    std::cout << "expected distance of " << truedist
              << " but computed distance was " << dist << "\n";
    assert(false);
  }
  std::println("  PASS");
}

// Test 4: hand-computed example
static void test_rf_distance_hand_computed_example() {
  std::println("test_rf_distance_hand_computed_example");
  auto dag1 = make_sample_dag();
  auto dag2 = make_nonintersecting_sample_dag();

  auto ec1 = edge_count(dag1);
  auto ec2 = edge_count(dag2);
  auto lc1 = leaf_count(dag1);
  auto lc2 = leaf_count(dag2);
  auto true_dist = bigint(ec1 + ec2 - lc1 - lc2 - 2);

  merge merge1("GAA");
  merge1.add_dag(dag1);
  merge merge2("GAA");
  merge2.add_dag(dag1);
  merge2.add_dag(dag2);
  assert(get_rf_distance(merge1, merge2) == true_dist);

  merge m("GAA");
  m.add_dag(dag1);
  m.add_dag(dag2);
  assert(get_rf_distance(merge1, merge2) == get_rf_distance(m, m));
  std::println("  PASS");
}

// Test 5: RF counter (weight_accumulator)
static void test_rf_counter() {
  std::println("test_rf_counter");
  auto dag1 = make_sample_dag();
  auto dag2 = make_nonintersecting_sample_dag();
  auto dag3 = make_sample_dag();

  recompute_compact_genomes(dag3);
  // Change internal CGs of dag3 to "AAA" (relative to ref "GAA")
  auto aaa_cg = cg_from_sequence("AAA", "GAA");
  for (auto nv : dag3.get_all_nodes()) {
    std::visit(
        [&](auto n) {
          if constexpr (requires { n.cg(); }) {
            if constexpr (!requires { n.sample_id(); }) {
              n.cg() = aaa_cg;
            }
          }
        },
        nv);
  }
  recompute_edge_mutations(dag3);

  merge merge1("GAA");
  merge1.add_dag(dag1);
  merge1.add_dag(dag2);
  merge1.add_dag(dag3);
  merge merge2("GAA");
  merge2.add_dag(dag1);
  merge2.add_dag(dag2);
  merge2.add_dag(dag3);

  using accum_ops = weight_accumulator<sum_rf_distance>;
  sum_rf_distance inner_ops{sum_rf_distance_ops{merge2, merge1}};
  auto shift_sum = inner_ops.get_ops().get_shift_sum();
  accum_ops accum{std::move(inner_ops)};
  subtree_weight<accum_ops> count(merge1.get_result());
  auto scores =
      count.compute_weight_below(get_root_idx(merge1.get_result()), accum);

  bigint total_count{0};
  std::cout << " " << std::right << std::setw(11) << "RF Distance";
  std::cout << " | " << std::left << std::setw(11) << "count" << "\n";
  std::cout << "-------------------------\n";
  for (auto [rfd, rfcount] : scores.get_weights()) {
    std::cout << " " << std::right << std::setw(11) << rfd + shift_sum;
    std::cout << " | " << std::left << std::setw(11) << rfcount << "\n";
    total_count += rfcount;
  }
  std::cout << "total count: " << total_count << "\n";
  assert(total_count == bigint{3});
  std::println("  PASS");
}

// Test 6: different weight ops (Min, MinSum, Max, MaxSum)
static void test_rf_distance_different_weight_ops() {
  std::println("test_rf_distance_different_weight_ops");
  auto dag1 = make_sample_dag();
  auto dag2 = make_nonintersecting_sample_dag();
  auto dag3 = make_sample_dag_with_one_unique_node();

  merge merge1("GAA");
  merge1.add_dag(dag1);
  merge merge2("GAA");
  merge2.add_dag(dag2);
  merge merge3("GAA");
  merge3.add_dag(dag3);

  merge merge1_2("GAA");
  merge1_2.add_dag(dag1);
  merge1_2.add_dag(dag2);
  merge merge1_3("GAA");
  merge1_3.add_dag(dag1);
  merge1_3.add_dag(dag3);
  merge merge2_3("GAA");
  merge2_3.add_dag(dag2);
  merge2_3.add_dag(dag3);
  merge merge1_2_3("GAA");
  merge1_2_3.add_dag(dag1);
  merge1_2_3.add_dag(dag2);
  merge1_2_3.add_dag(dag3);

  // Hand-computed single-tree RF distances
  std::map<std::pair<int, int>, bigint> true_dist_map;
  true_dist_map[{1, 1}] = true_dist_map[{2, 2}] = true_dist_map[{3, 3}] =
      bigint{0};
  true_dist_map[{1, 2}] = true_dist_map[{2, 1}] = bigint{6};
  true_dist_map[{1, 3}] = true_dist_map[{3, 1}] = bigint{1};
  true_dist_map[{2, 3}] = true_dist_map[{3, 2}] = bigint{7};

  auto compute_true_dist = [&](std::vector<int> compute_ids,
                               std::vector<int> ref_ids, rf_distance_type type,
                               bool do_print = false) -> bigint {
    std::vector<bigint> vec;
    for (auto compute_id : compute_ids) {
      std::vector<bigint> subvec;
      for (auto ref_id : ref_ids)
        subvec.push_back(true_dist_map[{compute_id, ref_id}]);
      if (type == rf_distance_type::Min) {
        vec.push_back(*std::min_element(subvec.begin(), subvec.end()));
      } else if (type == rf_distance_type::Max) {
        vec.push_back(*std::max_element(subvec.begin(), subvec.end()));
      } else {
        vec.push_back(std::accumulate(subvec.begin(), subvec.end(), bigint{0}));
      }
    }
    bigint total;
    if (type == rf_distance_type::Min || type == rf_distance_type::MinSum) {
      total = *std::min_element(vec.begin(), vec.end());
    } else {
      total = *std::max_element(vec.begin(), vec.end());
    }
    if (do_print) std::cout << "true_rf_distance: " << total << "\n";
    return total;
  };

  // Single MATs
  bigint dist;
  for (auto type : {rf_distance_type::Min, rf_distance_type::Max,
                    rf_distance_type::MinSum, rf_distance_type::MaxSum}) {
    bool p = true;
    dist = get_rf_distance(merge1, merge2, type, p);
    assert(dist == compute_true_dist({1}, {2}, type, p));
    dist = get_rf_distance(merge2, merge1, type, p);
    assert(dist == compute_true_dist({2}, {1}, type, p));
    dist = get_rf_distance(merge1, merge3, type, p);
    assert(dist == compute_true_dist({1}, {3}, type, p));
    dist = get_rf_distance(merge3, merge1, type, p);
    assert(dist == compute_true_dist({3}, {1}, type, p));
    dist = get_rf_distance(merge2, merge3, type, p);
    assert(dist == compute_true_dist({2}, {3}, type, p));
    dist = get_rf_distance(merge3, merge2, type, p);
    assert(dist == compute_true_dist({3}, {2}, type, p));
  }

  // MATs to two-MAT MADAGs
  {
    bool p = true;
    dist = get_rf_distance(merge1_2, merge1, rf_distance_type::Min, p);
    assert(dist == compute_true_dist({1, 2}, {1}, rf_distance_type::Min, p));
    dist = get_rf_distance(merge1_2, merge1, rf_distance_type::Max, p);
    assert(dist == compute_true_dist({1, 2}, {1}, rf_distance_type::Max, p));
    dist = get_rf_distance(merge1_2, merge1, rf_distance_type::MinSum, p);
    assert(dist == compute_true_dist({1, 2}, {1}, rf_distance_type::MinSum, p));
    dist = get_rf_distance(merge1_2, merge1, rf_distance_type::MaxSum, p);
    assert(dist == compute_true_dist({1, 2}, {1}, rf_distance_type::MaxSum, p));

    dist = get_rf_distance(merge2_3, merge1, rf_distance_type::Min, p);
    assert(dist == compute_true_dist({2, 3}, {1}, rf_distance_type::Min, p));
    dist = get_rf_distance(merge2_3, merge1, rf_distance_type::Max, p);
    assert(dist == compute_true_dist({2, 3}, {1}, rf_distance_type::Max, p));
    dist = get_rf_distance(merge2_3, merge1, rf_distance_type::MinSum, p);
    assert(dist == compute_true_dist({2, 3}, {1}, rf_distance_type::MinSum, p));
    dist = get_rf_distance(merge2_3, merge1, rf_distance_type::MaxSum, p);
    assert(dist == compute_true_dist({2, 3}, {1}, rf_distance_type::MaxSum, p));
  }

  // MATs to three-MAT MADAGs
  {
    bool p = true;
    dist = get_rf_distance(merge1_2_3, merge1, rf_distance_type::Min, p);
    assert(dist == compute_true_dist({1, 2, 3}, {1}, rf_distance_type::Min, p));
    dist = get_rf_distance(merge1_2_3, merge1, rf_distance_type::Max, p);
    assert(dist == compute_true_dist({1, 2, 3}, {1}, rf_distance_type::Max, p));
    dist = get_rf_distance(merge1_2_3, merge1, rf_distance_type::MinSum, p);
    assert(dist ==
           compute_true_dist({1, 2, 3}, {1}, rf_distance_type::MinSum, p));
    dist = get_rf_distance(merge1_2_3, merge1, rf_distance_type::MaxSum, p);
    assert(dist ==
           compute_true_dist({1, 2, 3}, {1}, rf_distance_type::MaxSum, p));
  }

  // Sum RF-Distances over MADAGs
  {
    bool p = true;
    dist = get_rf_distance(merge1_2_3, merge1_2, rf_distance_type::MinSum, p);
    assert(dist ==
           compute_true_dist({1, 2, 3}, {1, 2}, rf_distance_type::MinSum, p));
    dist = get_rf_distance(merge1_2_3, merge1_2, rf_distance_type::MaxSum, p);
    assert(dist ==
           compute_true_dist({1, 2, 3}, {1, 2}, rf_distance_type::MaxSum, p));
    dist = get_rf_distance(merge1_2, merge1_2_3, rf_distance_type::MinSum, p);
    assert(dist ==
           compute_true_dist({1, 2}, {1, 2, 3}, rf_distance_type::MinSum, p));
    dist = get_rf_distance(merge1_2, merge1_2_3, rf_distance_type::MaxSum, p);
    assert(dist ==
           compute_true_dist({1, 2}, {1, 2, 3}, rf_distance_type::MaxSum, p));
    dist = get_rf_distance(merge1_2_3, merge1_2_3, rf_distance_type::MinSum, p);
    assert(dist == compute_true_dist({1, 2, 3}, {1, 2, 3},
                                     rf_distance_type::MinSum, p));
    dist = get_rf_distance(merge1_2_3, merge1_2_3, rf_distance_type::MaxSum, p);
    assert(dist == compute_true_dist({1, 2, 3}, {1, 2, 3},
                                     rf_distance_type::MaxSum, p));
  }

  std::println("  PASS");
}

int main() {
  test_zero_rf_distance();
  test_rf_on_two_identical_topologies();
  test_rf_two_distinct_topologies_single_merge();
  test_rf_distance_hand_computed_example();
  test_rf_counter();
  test_rf_distance_different_weight_ops();

  std::println("All RF distance tests passed!");
  return 0;
}

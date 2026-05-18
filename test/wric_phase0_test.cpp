#include <larch/clade_grammar.hpp>
#include <larch/load_proto_dag.hpp>

#include "test_util.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <print>
#include <string>
#include <vector>

static larch::test::tiny_tree_node five_taxon_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAAA",
      {tiny_inner("AB", "AAAA",
                  {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA")}),
       tiny_inner("CDE", "AAAA",
                  {tiny_leaf("C", "ACAA"),
                   tiny_inner("DE", "AAAA",
                              {tiny_leaf("D", "AAGA"),
                               tiny_leaf("E", "AAAT")})})});
}

static larch::test::tiny_tree_node alternate_four_taxon_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAAA",
      {tiny_inner("AC", "AAAA",
                  {tiny_leaf("A", "AAAA"), tiny_leaf("C", "AAGA")}),
       tiny_inner("BD", "AAAA",
                  {tiny_leaf("B", "CAAA"), tiny_leaf("D", "AAAT")})});
}

static larch::test::tiny_tree_node base_four_taxon_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAAA",
      {tiny_inner("AB", "AAAA",
                  {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA")}),
       tiny_inner("CD", "AAAA",
                  {tiny_leaf("C", "AAGA"), tiny_leaf("D", "AAAT")})});
}

static void test_tiny_tree_build_enumerate_and_score() {
  std::println("test_tiny_tree_build_enumerate_and_score");

  auto tree = larch::test::make_tiny_labelled_tree("AAAA", five_taxon_tree_spec());
  larch::validate_dag(tree, "wric_phase0_tiny_tree");
  assert(larch::leaf_count(tree) == 5);

  auto edge_sets = larch::test::enumerate_represented_tree_edges(tree);
  assert(edge_sets.size() == 1);
  assert(larch::test::score_edge_set_parsimony(tree, edge_sets.front(), false) ==
         4);
  assert(larch::test::score_tree_parsimony(tree, false) == 4);
  assert(larch::test::score_tree_fitch_parsimony(tree, false) == 4);

  auto sampled = larch::test::sample_represented_tree(tree, 17u);
  larch::validate_dag(sampled, "wric_phase0_sampled_tree");
  assert(larch::test::score_tree_parsimony(sampled, false) == 4);

  std::println("  PASS");
}

static void test_tiny_dag_builder_and_merge_helpers() {
  std::println("test_tiny_dag_builder_and_merge_helpers");

  auto direct = larch::test::make_tiny_labelled_dag(
      "AAAA", "root",
      {{"root", "AAAA", ""},
       {"left", "AAAA", ""},
       {"right", "AAAA", ""},
       {"A", "AAAA", "A"},
       {"B", "CAAA", "B"},
       {"C", "AAGA", "C"},
       {"D", "AAAT", "D"}},
      {{"root", "left", 0},
       {"root", "right", 1},
       {"left", "A", 0},
       {"left", "B", 1},
       {"right", "C", 0},
       {"right", "D", 1}});
  larch::validate_dag(direct, "wric_phase0_direct_tiny_dag");
  assert(larch::test::score_tree_parsimony(direct, false) == 3);

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAAA", base_four_taxon_tree_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAAA", alternate_four_taxon_tree_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  larch::validate_dag(merged, "wric_phase0_merged_tiny_trees");

  auto merged_edge_sets = larch::test::enumerate_represented_tree_edges(merged);
  assert(merged_edge_sets.size() >= 2);
  auto sampled_edges = larch::test::sample_represented_tree_edges(merged, 23u);
  assert(!sampled_edges.empty());

  std::println("  merged represented trees: {}", merged_edge_sets.size());
  std::println("  PASS");
}

static void test_strict_nucleotide_audit() {
  std::println("test_strict_nucleotide_audit");

  auto tiny = larch::test::make_tiny_labelled_tree("AAAA", five_taxon_tree_spec());
  auto tiny_audit = larch::test::audit_strict_nucleotides(tiny);
  assert(tiny_audit.reference_bases == 4);
  assert(tiny_audit.leaf_count == 5);
  assert(tiny_audit.leaf_bases == 20);

  for (int i = 0; i < 5; ++i) {
    auto path = larch::test::source_path_string(
        "data/test_5_trees/tree_" + std::to_string(i) + ".pb.gz");
    auto dag = larch::load_proto_dag(path);
    auto audit = larch::test::audit_strict_nucleotides(dag);
    assert(audit.reference_bases > 0);
    assert(audit.leaf_count > 0);
    assert(audit.leaf_bases == audit.reference_bases * audit.leaf_count);
    std::println("  tree_{}: reference_bases={}, leaves={}", i,
                 audit.reference_bases, audit.leaf_count);
  }

  bool rejected_ambiguous_reference = false;
  try {
    auto bad = larch::test::make_tiny_labelled_tree(
        "AANA", larch::test::tiny_leaf("bad", "AAAA"));
    (void)larch::test::audit_strict_nucleotides(bad);
  } catch (std::runtime_error const&) {
    rejected_ambiguous_reference = true;
  }
  assert(rejected_ambiguous_reference);

  bool rejected_invalid_nuc_base = false;
  try {
    (void)larch::test::strict_decode_acgt(larch::nuc_base{7});
  } catch (std::runtime_error const&) {
    rejected_invalid_nuc_base = true;
  }
  assert(rejected_invalid_nuc_base);

  std::println("  PASS");
}

int main() {
  static_assert(larch::no_clade ==
                std::numeric_limits<larch::clade_id>::max());
  static_assert(larch::no_production ==
                std::numeric_limits<larch::production_id>::max());

  test_tiny_tree_build_enumerate_and_score();
  test_tiny_dag_builder_and_merge_helpers();
  test_strict_nucleotide_audit();

  std::println("All WRIC phase 0 tests passed!");
  return 0;
}

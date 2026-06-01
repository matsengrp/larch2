#include <larch/chart_bnb_trim_apply.hpp>
#include <larch/chart_trim.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/site_patterns.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr)                                    \
  do {                                                 \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

static bool throws_runtime_error_containing(auto&& f, std::string needle) {
  try {
    f();
  } catch (std::runtime_error const& e) {
    return std::string{e.what()}.find(needle) != std::string::npos;
  }
  return false;
}

static larch::test::tiny_tree_node paper_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("C", "AC"),
                   tiny_inner("DE", "AA",
                              {tiny_leaf("D", "CA"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node paper_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("D", "CA"),
                   tiny_inner("CE", "AA",
                              {tiny_leaf("C", "AC"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node concordant_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("C", "AA"),
                   tiny_inner("DE", "AA",
                              {tiny_leaf("D", "CC"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node concordant_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("D", "CC"),
                   tiny_inner("CE", "AA",
                              {tiny_leaf("C", "AA"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node three_taxon_star_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner("root", "AAAA",
                    {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA"),
                     tiny_leaf("C", "AAGA")});
}

static larch::polytomy_refinement_result binary_refinement(larch::phylo_dag& dag) {
  larch::clade_grammar_options grammar_opts;
  larch::polytomy_refinement_options refinement_opts;
  refinement_opts.mode = larch::polytomy_mode::reject;
  return larch::build_polytomy_refined_clade_grammar(dag, grammar_opts,
                                                     refinement_opts);
}

static larch::chart_bnb_trim_apply_result apply_mask(larch::phylo_dag& dag) {
  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);
  larch::chart_bnb_trim_apply_options options;
  options.mode = larch::chart_bnb_trim_application_mode::production_mask_superset;
  return larch::apply_chart_bnb_trim(dag, refinement, patterns, {}, trim,
                                     options);
}

static void test_production_mask_apply_writes_valid_superset() {
  std::println("test_production_mask_apply_writes_valid_superset");
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", concordant_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", concordant_tree2_spec()));
  auto dag = larch::test::merge_tiny_trees(trees);

  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);

  larch::chart_bnb_trim_apply_options options;
  options.mode = larch::chart_bnb_trim_application_mode::production_mask_superset;
  auto applied = larch::apply_chart_bnb_trim(dag, refinement, patterns, {}, trim,
                                             options);

  CHECK(applied.production_mask_superset);
  CHECK(!applied.grammar_topology_exact);
  CHECK(applied.kept_productions_requested ==
        static_cast<std::size_t>(std::count(trim.keep_production.begin(),
                                            trim.keep_production.end(), true)));
  CHECK(applied.validated_output_parsimony_min == trim.optimum);
  CHECK(applied.validation_oracle == "brute_force_topology_enumeration_fitch");
  CHECK(applied.validation_strength == "exact_small_fixture_independent");
  CHECK(applied.validation_status == "success");
  CHECK(applied.validation_succeeded);
  larch::validate_dag(applied.dag, "chart_bnb_trim_apply_test mask",
                      larch::thread_pool::get_default());
}

static void test_optimal_topology_materialize_identity_tree_set() {
  std::println("test_optimal_topology_materialize_identity_tree_set");
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto dag = larch::test::merge_tiny_trees(trees);

  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);

  larch::chart_bnb_trim_apply_options options;
  options.mode =
      larch::chart_bnb_trim_application_mode::optimal_topology_materialize;
  options.max_exact_topologies_to_materialize = 10;
  auto applied = larch::apply_chart_bnb_trim(dag, refinement, patterns, {}, trim,
                                             options);

  CHECK(applied.identity_preserving_tree_set);
  CHECK(applied.grammar_topology_exact);
  CHECK(!applied.source_history_topology_exact);
  CHECK(applied.materialized_topologies > 0);
  CHECK(applied.validated_output_parsimony_min == trim.optimum);
  CHECK(applied.validation_oracle ==
        "brute_force_topology_enumeration_fitch_and_topology_set_compare");
  CHECK(applied.validation_status == "success");

  larch::tree_count_ops ops;
  larch::subtree_weight<larch::tree_count_ops> counter(applied.dag);
  auto count = counter.compute_weight_below(larch::get_root_idx(applied.dag), ops);
  CHECK(count == larch::bigint{applied.materialized_topologies});
}

static void test_sparse_source_indices_apply_in_source_witness_space() {
  std::println("test_sparse_source_indices_apply_in_source_witness_space");
  auto dag = larch::test::make_tiny_labelled_tree("AA", concordant_tree1_spec());

  std::size_t target_edge = larch::no_idx;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (target_edge != larch::no_idx) return;
          if (!larch::is_ua(dag, larch::get_parent_idx(dag, edge.index()))) {
            target_edge = edge.index();
          }
        },
        ev);
  }
  CHECK(target_edge != larch::no_idx);
  auto parent = larch::get_parent_idx(dag, target_edge);
  auto child = larch::get_child_idx(dag, target_edge);
  auto src_edge = dag.get_edge_as<larch::edge_kind::clade>(target_edge);
  auto duplicate = dag.append_edge<larch::edge_kind::clade>();
  duplicate.clade_index() = src_edge.clade_index();
  duplicate.edge_weight() = src_edge.edge_weight();
  duplicate.mutations() = src_edge.mutations();
  std::visit([&](auto node) { duplicate.set_parent(node); }, dag.get_node(parent));
  std::visit([&](auto node) { duplicate.set_child(node); }, dag.get_node(child));
  src_edge.remove();
  larch::build_clade_offsets(dag);
  CHECK(dag.edge_high_mark() > larch::edge_count(dag));

  auto applied = apply_mask(dag);
  CHECK(applied.validation_succeeded);
  larch::validate_dag(applied.dag, "chart_bnb sparse source apply",
                      larch::thread_pool::get_default());
}

static void test_optimal_topology_materialize_cap_truncation_fails() {
  std::println("test_optimal_topology_materialize_cap_truncation_fails");
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto dag = larch::test::merge_tiny_trees(trees);
  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);

  larch::chart_bnb_trim_apply_options options;
  options.mode =
      larch::chart_bnb_trim_application_mode::optimal_topology_materialize;
  options.max_exact_topologies_to_materialize = 1;
  CHECK(throws_runtime_error_containing(
      [&] { (void)larch::apply_chart_bnb_trim(dag, refinement, patterns, {}, trim,
                                               options); },
      "cap was truncated"));
}

static void test_production_mask_rejects_synthetic_polytomy_provenance() {
  std::println("test_production_mask_rejects_synthetic_polytomy_provenance");
  auto dag = larch::test::make_tiny_labelled_tree("AAAA", three_taxon_star_spec());
  larch::polytomy_refinement_options refinement_opts;
  refinement_opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, refinement_opts);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);
  larch::chart_bnb_trim_apply_options options;
  options.mode = larch::chart_bnb_trim_application_mode::production_mask_superset;
  CHECK(throws_runtime_error_containing(
      [&] { (void)larch::apply_chart_bnb_trim(dag, refinement, patterns, {}, trim,
                                               options); },
      "synthetic polytomy-refinement provenance"));
}

static void test_score_ua_edge_single_taxon_validation() {
  std::println("test_score_ua_edge_single_taxon_validation");
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_leaf("A", "C"));
  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  larch::chart_options chart_opts;
  chart_opts.score_ua_edge = true;
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns,
                                          chart_opts);
  CHECK(trim.optimum == 1);
  larch::chart_bnb_trim_apply_options options;
  options.mode = larch::chart_bnb_trim_application_mode::production_mask_superset;
  auto applied = larch::apply_chart_bnb_trim(dag, refinement, patterns,
                                             chart_opts, trim, options);
  CHECK(applied.kept_productions_requested == 0);
  CHECK(applied.validated_output_parsimony_min == 1);
  CHECK(applied.validation_oracle == "brute_force_topology_enumeration_fitch");
  CHECK(applied.validation_succeeded);
}

static void test_production_mask_round_trips_through_protobuf() {
  std::println("test_production_mask_round_trips_through_protobuf");
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", concordant_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", concordant_tree2_spec()));
  auto dag = larch::test::merge_tiny_trees(trees);
  auto applied = apply_mask(dag);

  auto path = larch::test::unique_temp_path(
      "chart_bnb_trim_apply_roundtrip", ".pb.gz");
  larch::save_proto_dag(applied.dag, path.string());
  auto loaded = larch::load_proto_dag(path.string());
  std::filesystem::remove(path);
  larch::validate_dag(loaded, "chart_bnb protobuf roundtrip",
                      larch::thread_pool::get_default());
  auto refinement = binary_refinement(loaded);
  auto patterns = larch::build_site_patterns(loaded, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);
  CHECK(trim.optimum == applied.bnb_optimum);
}

static void test_production_mask_reports_recombination_superset() {
  std::println("test_production_mask_reports_recombination_superset");
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto tree0 = tiny_inner(
      "I4", "AAAA",
      {tiny_inner("I3", "AAAA",
                  {tiny_inner("I1", "AAAA",
                              {tiny_leaf("B", "CCCC"),
                               tiny_inner("I0", "AAAA",
                                          {tiny_leaf("C", "CCAA"),
                                           tiny_leaf("D", "AAAA")})}),
                   tiny_inner("I2", "AAAA",
                              {tiny_leaf("A", "CCAC"),
                               tiny_leaf("F", "CCCA")})}),
       tiny_leaf("E", "AACA")});
  auto tree1 = tiny_inner(
      "I4", "AAAA",
      {tiny_inner("I3", "AAAA",
                  {tiny_inner("I0", "AAAA",
                              {tiny_leaf("F", "CCCA"),
                               tiny_leaf("B", "CCCC")}),
                   tiny_inner("I2", "AAAA",
                              {tiny_leaf("C", "CCAA"),
                               tiny_inner("I1", "AAAA",
                                          {tiny_leaf("D", "AAAA"),
                                           tiny_leaf("A", "CCAC")})})}),
       tiny_leaf("E", "AACA")});
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AAAA", tree0));
  trees.push_back(larch::test::make_tiny_labelled_tree("AAAA", tree1));
  auto dag = larch::test::merge_tiny_trees(trees);
  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);
  larch::chart_bnb_trim_apply_options options;
  options.mode = larch::chart_bnb_trim_application_mode::production_mask_superset;
  auto applied = larch::apply_chart_bnb_trim(dag, refinement, patterns, {}, trim,
                                             options);

  CHECK(applied.production_mask_superset);
  CHECK(!applied.grammar_topology_exact);
  CHECK(applied.validated_output_parsimony_min == trim.optimum);
  CHECK(applied.output_contains_only_optimal_topologies == "false");
  CHECK(applied.validation_oracle == "brute_force_topology_enumeration_fitch");

  options.mode = larch::chart_bnb_trim_application_mode::annotated_optimal_trim;
  auto annotated = larch::apply_chart_bnb_trim(dag, refinement, patterns, {},
                                               trim, options);
  CHECK(annotated.annotated_optimal_trim);
  CHECK(annotated.coupled_frontier_exact);
  CHECK(!annotated.production_mask_superset);
  CHECK(!annotated.output_dag_available);
  CHECK(annotated.output_artifact_kind == "coupled_frontier_annotation");
  CHECK(annotated.output_contains_only_optimal_topologies == "true");
  CHECK(annotated.validated_output_parsimony_min == trim.optimum);
  CHECK(annotated.validation_succeeded);
  CHECK(annotated.coupled_frontier_entries > 0);
  CHECK(annotated.coupled_provenance_choices > 0);

  auto enumerated = larch::enumerate_multisite_coupled_frontier_topologies(
      refinement.grammar, annotated.coupled_frontier_annotation, 0);
  CHECK(!enumerated.topology_cap_truncated);
  auto brute = larch::brute_force_multisite_topologies(refinement.grammar,
                                                       patterns);
  CHECK(enumerated.topologies.size() == brute.optimal_topology_count);
  for (auto const& topology : enumerated.topologies) {
    CHECK(larch::score_selected_topology(refinement.grammar, patterns,
                                         topology) == trim.optimum);
  }
}

static void test_annotated_validation_cap_truncation_reported() {
  std::println("test_annotated_validation_cap_truncation_reported");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto dag = larch::test::merge_tiny_trees(trees);

  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto brute = larch::brute_force_multisite_topologies(refinement.grammar,
                                                       patterns);
  CHECK(brute.optimal_topology_count > 1);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);

  larch::chart_bnb_trim_apply_options options;
  options.mode =
      larch::chart_bnb_trim_application_mode::annotated_optimal_trim;
  options.max_exact_topologies_to_materialize = 1;
  auto applied = larch::apply_chart_bnb_trim(dag, refinement, patterns, {},
                                             trim, options);
  CHECK(applied.annotated_optimal_trim);
  CHECK(applied.coupled_frontier_exact);
  CHECK(!applied.topology_cap_truncated);
  CHECK(applied.validation_topology_cap_truncated);
  CHECK(applied.validation_oracle ==
        "coupled_frontier_annotation_structural_reachability");
  CHECK(applied.validation_strength ==
        "exact_by_bnb_frontier_annotation_not_independent");
  CHECK(applied.validation_succeeded);
}

static void test_missing_and_bad_witnesses_fail_clearly() {
  std::println("test_missing_and_bad_witnesses_fail_clearly");
  auto dag = larch::test::make_tiny_labelled_tree("AA", concordant_tree1_spec());
  auto refinement = binary_refinement(dag);
  auto patterns = larch::build_site_patterns(dag, refinement.grammar);
  auto trim = larch::build_multisite_trim(refinement.grammar, patterns);
  larch::chart_bnb_trim_apply_options options;
  options.mode = larch::chart_bnb_trim_application_mode::production_mask_superset;

  auto missing = refinement;
  CHECK(!missing.grammar.productions.empty());
  missing.grammar.productions.front().witnesses.clear();
  CHECK(throws_runtime_error_containing(
      [&] { (void)larch::apply_chart_bnb_trim(dag, missing, patterns, {}, trim,
                                               options); },
      "has no source DAG witnesses"));

  auto bad_edge = refinement;
  auto& witness = bad_edge.grammar.productions.front().witnesses.front();
  auto witness_parent = witness.parent_node;
  std::size_t replacement_edge = larch::no_idx;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (replacement_edge != larch::no_idx) return;
          if (larch::get_parent_idx(dag, edge.index()) != witness_parent) {
            replacement_edge = edge.index();
          }
        },
        ev);
  }
  CHECK(replacement_edge != larch::no_idx);
  witness.children.front().edge_alternatives.front() = replacement_edge;
  CHECK(throws_runtime_error_containing(
      [&] { (void)larch::apply_chart_bnb_trim(dag, bad_edge, patterns, {}, trim,
                                               options); },
      "witness edge parent mismatch"));
}

int main() {
  test_production_mask_apply_writes_valid_superset();
  test_optimal_topology_materialize_identity_tree_set();
  test_sparse_source_indices_apply_in_source_witness_space();
  test_optimal_topology_materialize_cap_truncation_fails();
  test_production_mask_rejects_synthetic_polytomy_provenance();
  test_score_ua_edge_single_taxon_validation();
  test_production_mask_round_trips_through_protobuf();
  test_production_mask_reports_recombination_superset();
  test_annotated_validation_cap_truncation_reported();
  test_missing_and_bad_witnesses_fail_clearly();
  return 0;
}

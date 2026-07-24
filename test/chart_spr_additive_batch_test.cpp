#include <larch/chart_spr_search.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/rank3_rewrite.hpp>
#include <larch/save_proto_dag.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

[[noreturn]] void test_fail(char const* expression, char const* file,
                            int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expression);
}

#define CHECK(expression)                                          \
  do {                                                             \
    if (!(expression)) test_fail(#expression, __FILE__, __LINE__); \
  } while (false)

larch::test::tiny_tree_node arity4_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_leaf("B", "A"), tiny_leaf("D", "C"), tiny_leaf("E", "C")});
}

std::uint64_t external_dag_score(larch::phylo_dag& dag) {
  larch::parsimony_score_ops ops;
  larch::subtree_weight<larch::parsimony_score_ops> scorer{dag,
                                                           std::uint32_t{1}};
  return scorer.compute_weight_below(larch::get_root_idx(dag), ops);
}

std::size_t grammar_max_arity(larch::clade_grammar const& grammar) {
  std::size_t result = 0;
  for (auto const& production : grammar.productions) {
    result = std::max(result, production.children.size());
  }
  return result;
}

std::pair<std::uint64_t, std::uint64_t>
independently_rescore_minimum_output_tree(larch::phylo_dag& dag) {
  larch::parsimony_score_ops ops;
  larch::subtree_weight<larch::parsimony_score_ops> sampler{dag,
                                                            std::uint32_t{7}};
  (void)sampler.compute_weight_below(larch::get_root_idx(dag), ops);
  auto tree = sampler.min_weight_sample_tree(ops);
  larch::fitch_assign_compact_genomes(tree);
  larch::recompute_edge_mutations(tree);
  larch::build_clade_offsets(tree);

  larch::clade_grammar_options grammar_options;
  grammar_options.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(tree, grammar_options);
  auto patterns = larch::build_site_patterns(tree, grammar);
  auto topology = larch::first_rank3_topology(grammar);
  larch::chart_options chart_options;
  chart_options.score_ua_edge = true;
  auto chart_score = larch::score_selected_topology(grammar, patterns, topology,
                                                    chart_options);
  auto independent_score = external_dag_score(tree);
  return {chart_score, independent_score};
}

larch::chart_spr_search_options additive_options() {
  larch::chart_spr_search_options options;
  options.additive_batch_union = true;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 3;
  options.worker_count = 1;
  options.seed = 1;
  options.chart.score_ua_edge = true;
  options.enumeration.sampled_tree_spr_radius = 8;
  options.additive_batch_max_moves_per_radius = 50;
  options.additive_batch_score_threshold = -1;
  options.rebuild_after_accept = true;
  options.materialize_accepted_moves = true;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::off;
  return options;
}

void test_kary_additive_batch_accepts_rebuilds_and_materializes() {
  std::println("test_kary_additive_batch_accepts_rebuilds_and_materializes");

  auto dag = larch::test::make_tiny_labelled_tree("A", arity4_misplaced_tree());
  larch::clade_grammar_options grammar_options;
  grammar_options.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, grammar_options);
  CHECK(grammar_max_arity(grammar) == 4);
  auto const independently_measured_initial_score = external_dag_score(dag);
  CHECK(independently_measured_initial_score == 3);

  auto search =
      larch::run_chart_spr_search(std::move(dag), grammar, additive_options());

  CHECK(search.summary.initial_score == independently_measured_initial_score);
  CHECK(search.summary.final_score < search.summary.initial_score);
  CHECK(search.summary.final_score <= 2);
  CHECK(search.counters.accepted_moves >= 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept ==
        search.counters.accepted_moves);
  CHECK(search.counters.grammar_rebuilds >= search.counters.accepted_moves);
  CHECK(search.counters.pattern_rebuilds >= search.counters.accepted_moves);
  CHECK(search.iterations.size() >= 2);
  CHECK(search.iterations.size() <= 3);

  std::size_t observed_commits = 0;
  auto previous_score = search.summary.initial_score;
  for (auto const& iteration : search.iterations) {
    CHECK(iteration.additive_batch_union);
    CHECK(iteration.state_score_before == previous_score);
    CHECK(iteration.batch_sampled_tree_exact_chart_score ==
          iteration.batch_sampled_tree_external_score);
    if (iteration.accepted_move_committed) {
      ++observed_commits;
      CHECK(iteration.state_score_after < iteration.state_score_before);
      CHECK(iteration.batch_exact_witness_external_score <
            iteration.batch_sampled_tree_external_score);
      CHECK(iteration.batch_exact_witness_score_parity);
      CHECK(iteration.batch_exact_witness_chart_score ==
            iteration.batch_exact_witness_external_score);
      CHECK(iteration.batch_output_external_score ==
            iteration.state_score_after);
      CHECK(iteration.batch_output_grammar_max_arity >= 4);
      previous_score = iteration.state_score_after;
    } else {
      CHECK(iteration.state_score_after == iteration.state_score_before);
    }
  }
  CHECK(observed_commits == search.counters.accepted_moves);
  CHECK(previous_score == search.summary.final_score);

  auto const& accepted = search.iterations.front();
  CHECK(accepted.accepted_move_committed);
  CHECK(accepted.state_score_after < accepted.state_score_before);
  CHECK(accepted.batch_moves_projected > 0);
  CHECK(accepted.batch_fragments_materialized ==
        accepted.batch_moves_projected);
  CHECK(accepted.batch_candidate_certificates_materialized ==
        accepted.batch_moves_projected);
  CHECK(accepted.batch_multifurcating_moves_projected > 0);
  CHECK(accepted.batch_max_source_parent_arity >= 4);
  CHECK(accepted.batch_output_grammar_max_arity >= 4);
  CHECK(accepted.batch_exact_witness_multifurcation_productions > 0);
  CHECK(accepted.batch_exact_witness_score_parity);
  CHECK(accepted.batch_sampled_tree_exact_chart_score ==
        accepted.batch_sampled_tree_external_score);
  CHECK(accepted.batch_exact_witness_external_score <
        accepted.batch_sampled_tree_external_score);
  CHECK(accepted.batch_tentative_union_external_score <
        accepted.state_score_before);
  CHECK(accepted.batch_exact_witness_chart_score ==
        accepted.batch_exact_witness_external_score);
  CHECK(accepted.batch_output_external_score <=
        accepted.batch_exact_witness_external_score);
  CHECK(accepted.batch_output_external_score == accepted.state_score_after);

  // max_iterations > 1 must operate on each rebuilt accepted DAG.  This
  // one-site fixture can improve at most twice (3 -> 2 -> 1), so three
  // iterations leave room for a terminal non-improving union.
  auto const& terminal = search.iterations.back();
  CHECK(!terminal.accepted_move_committed);
  CHECK(terminal.state_score_before == search.summary.final_score);
  CHECK(terminal.state_score_after == terminal.state_score_before);
  CHECK(!terminal.no_accept_reason.empty());

  auto output_grammar = larch::build_clade_grammar(search.dag, grammar_options);
  CHECK(grammar_max_arity(output_grammar) >= 4);
  CHECK(external_dag_score(search.dag) == search.summary.final_score);
  larch::validate_dag(search.dag, "chart SPR additive batch test output");

  auto [chart_score, independent_score] =
      independently_rescore_minimum_output_tree(search.dag);
  CHECK(chart_score == independent_score);
  CHECK(chart_score == search.summary.final_score);

  auto const output_path =
      larch::test::unique_temp_path("chart_spr_additive_batch", ".pb.gz");
  larch::save_proto_dag(search.dag, output_path.string());
  auto loaded = larch::load_proto_dag(output_path.string());
  std::filesystem::remove(output_path);
  larch::validate_dag(loaded, "chart SPR additive batch round-trip output");
  CHECK(external_dag_score(loaded) == search.summary.final_score);
  auto loaded_grammar = larch::build_clade_grammar(loaded, grammar_options);
  CHECK(grammar_max_arity(loaded_grammar) >= 4);

  std::println("  PASS");
}

void test_binary_default_path_remains_selected_without_opt_in() {
  std::println("test_binary_default_path_remains_selected_without_opt_in");

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto dag = larch::test::make_tiny_labelled_tree(
      "A",
      tiny_inner(
          "root", "A",
          {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
           tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})}));
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  CHECK(!options.additive_batch_union);
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.max_iterations = 1;
  options.worker_count = 1;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.iterations.size() == 1);
  CHECK(!search.iterations.front().additive_batch_union);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted->exact.has_value());
  CHECK(search.iterations.front().accepted->exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);

  std::println("  PASS");
}

}  // namespace

int main() {
  test_kary_additive_batch_accepts_rebuilds_and_materializes();
  test_binary_default_path_remains_selected_without_opt_in();
  std::println("chart_spr_additive_batch_test PASS");
  return 0;
}

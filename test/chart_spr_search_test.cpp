#include <larch/chart_spr_search.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <print>
#include <stdexcept>
#include <string>

[[noreturn]] static void test_fail(char const* expr, char const* file, int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr) \
  do { \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

static larch::test::tiny_tree_node four_taxon_base_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

struct tiny_chart_spr_fixture {
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
  larch::site_pattern_set patterns;
  std::vector<larch::grammar_spr_candidate> candidates;
};

static tiny_chart_spr_fixture make_fixture() {
  tiny_chart_spr_fixture fixture;
  fixture.dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  fixture.grammar = larch::build_clade_grammar(fixture.dag);
  fixture.patterns = larch::build_site_patterns(fixture.dag, fixture.grammar);
  fixture.candidates = larch::enumerate_grammar_spr_candidates(fixture.grammar);
  CHECK(!fixture.candidates.empty());
  return fixture;
}

static void test_lower_bound_oracle_counters_show_full_rebuild_cost() {
  std::println("test_lower_bound_oracle_counters_show_full_rebuild_cost");

  auto fixture = make_fixture();
  larch::chart_spr_search_counters counters;
  auto score = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front(), {}, &counters);

  CHECK(score.old_score > 0);
  CHECK(counters.full_overlay_materializations == 1);
  CHECK(counters.overlay_materializations_for_oracle == 1);
  CHECK(counters.overlay_materializations_for_exact_verification == 0);
  CHECK(counters.overlay_materializations_for_accept_materialization == 0);
  CHECK(counters.full_composite_rebuilds == 2);
  CHECK(counters.local_candidate_scores == 0);

  std::println("  PASS");
}

static void test_local_rejected_candidate_counter_guardrail() {
  std::println("test_local_rejected_candidate_counter_guardrail");

  auto fixture = make_fixture();
  auto states = larch::leaf_site_states{
      .state_by_taxon = fixture.patterns.patterns.front().state_by_taxon};
  auto base_chart = larch::build_single_site_chart(fixture.grammar, states);
  auto overlay = larch::overlay_from_candidate(fixture.grammar,
                                               fixture.candidates.front());

  larch::chart_spr_search_counters counters;
  auto local = larch::score_rejected_candidate_with_local_recompute_oracle(
      overlay, base_chart, states, {}, &counters);
  larch::record_chart_spr_rejected_candidate(counters);

  CHECK(local.affected_clade_count > 0);
  CHECK(counters.local_candidate_scores == 1);
  CHECK(counters.rejected_moves == 1);
  CHECK(counters.full_composite_rebuilds == 0);
  CHECK(counters.full_overlay_materializations == 1);
  CHECK(counters.overlay_materializations_for_oracle == 1);

  std::println("  PASS");
}

static void test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute() {
  std::println("test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_candidates = 1;
  larch::chart_spr_search_counters counters;
  auto result = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options, &counters);

  CHECK(result.candidates.size() == 1);
  CHECK(result.stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(result.stats.upward_paths_completed > result.candidates.size());
  CHECK(result.stats.upward_path_iterator_steps > 0);
  CHECK(result.stats.candidates_constructed >= result.candidates.size());
  CHECK(counters.candidate_cap_cutoffs == 1);
  CHECK(counters.candidates_generated_after_dedup == result.candidates.size());
  CHECK(counters.upward_paths_completed == result.stats.upward_paths_completed);
  CHECK(counters.path_pairs_considered == result.stats.path_pairs_considered);

  std::println("  PASS");
}

int main() {
  test_lower_bound_oracle_counters_show_full_rebuild_cost();
  test_local_rejected_candidate_counter_guardrail();
  test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute();
  std::println("chart_spr_search_test PASS");
  return 0;
}

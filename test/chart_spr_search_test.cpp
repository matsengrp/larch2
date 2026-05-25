#include <larch/chart_spr_search.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

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

static larch::test::tiny_tree_node four_taxon_offset_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "CCAA",
      {tiny_inner("AB", "CCAA", {tiny_leaf("A", "CCAA"),
                                    tiny_leaf("B", "CCAA")}),
       tiny_inner("CD", "CCGA", {tiny_leaf("C", "CCGA"),
                                    tiny_leaf("D", "CCGA")})});
}

static larch::test::tiny_tree_node four_taxon_repeated_reference_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"),
                                  tiny_leaf("B", "AA")}),
       tiny_inner("CD", "AC", {tiny_leaf("C", "AA"),
                                  tiny_leaf("D", "CC")})});
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

static std::vector<std::string> candidate_taxon_signatures(
    larch::clade_grammar const& grammar,
    std::vector<larch::grammar_spr_candidate> const& candidates) {
  std::vector<std::string> signatures;
  signatures.reserve(candidates.size());
  for (auto const& candidate : candidates) {
    signatures.push_back(
        larch::chart_spr_candidate_taxon_signature(grammar, candidate));
  }
  return signatures;
}

static void test_streaming_and_eager_candidate_apis_match() {
  std::println("test_streaming_and_eager_candidate_apis_match");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.include_root_moves = true;

  std::vector<larch::grammar_spr_candidate> streamed;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const& candidate) {
        streamed.push_back(candidate);
        return true;
      });
  auto eager = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options);

  CHECK(stats.stop_reason == larch::chart_spr_candidate_stop_reason::exhausted);
  CHECK(stats.candidates_generated_after_dedup == streamed.size());
  CHECK(candidate_taxon_signatures(fixture.grammar, streamed) ==
        candidate_taxon_signatures(fixture.grammar, eager.candidates));

  std::println("  PASS");
}

static void test_search_state_local_score_entry_point_matches_oracle() {
  std::println("test_search_state_local_score_entry_point_matches_oracle");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto scored = larch::score_candidate_locally(state, fixture.candidates.front());

  larch::chart_spr_search_counters oracle_counters;
  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front(), {},
      &oracle_counters);

  CHECK(scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(scored.lower_bound.value.delta == oracle.delta);
  CHECK(scored.affected_clade_count > 0);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);
  CHECK(state.counters.local_candidate_scores == 1);
  CHECK(state.counters.full_composite_rebuilds == 0);

  std::println("  PASS");
}

static void test_persistent_active_pattern_cache_matches_full_composite() {
  std::println("test_persistent_active_pattern_cache_matches_full_composite");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto full_patterns = larch::build_site_patterns(dag, grammar);
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto active_build = larch::make_active_search_patterns(full_patterns, opts);
  CHECK(active_build.skipped_invariant_site_count == 3);
  CHECK(active_build.invariant_constant_offset == 2);
  active_build.active_patterns.assert_no_skipped_invariant_metadata();

  auto active_composite = larch::build_composite_chart_score_active(
      grammar, active_build.active_patterns, opts);
  auto full_composite = larch::build_composite_chart_score(
      grammar, full_patterns, opts);

  auto state = larch::build_chart_spr_search_state(
      dag, grammar, full_patterns, opts);
  CHECK(state.pattern_charts.size() ==
        active_build.active_patterns.patterns.patterns.size());
  CHECK(state.skipped_invariant_site_count == 3);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(state.composite_lower_bound_without_invariants ==
        active_composite.weighted_lower_bound);
  CHECK(state.composite_lower_bound_with_invariants ==
        full_composite.weighted_lower_bound);
  CHECK(state.composite_lower_bound_with_invariants ==
        state.composite_lower_bound_without_invariants +
            state.invariant_constant_offset);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);

  std::println("  PASS");
}

static void test_root_reference_counts_preserved_in_cache() {
  std::println("test_root_reference_counts_preserved_in_cache");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AC", four_taxon_repeated_reference_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  CHECK(patterns.patterns.size() == 1);
  CHECK(patterns.patterns.front().weight == 2);
  CHECK(patterns.patterns.front()
            .reference_state_counts[larch::nuc_base::A] == 1);
  CHECK(patterns.patterns.front()
            .reference_state_counts[larch::nuc_base::C] == 1);

  larch::chart_options opts;
  opts.score_ua_edge = true;
  auto state = larch::build_chart_spr_search_state(
      dag, grammar, patterns, opts);
  CHECK(state.pattern_charts.size() == 1);
  auto const& entry = state.pattern_charts.front();
  CHECK(entry.reference_state_counts[larch::nuc_base::A] == 1);
  CHECK(entry.reference_state_counts[larch::nuc_base::C] == 1);
  CHECK(entry.root_min_excluding_ua == 1);
  CHECK(entry.weighted_root_score == 3);
  CHECK(entry.weighted_root_score >
        patterns.patterns.front().weight * entry.root_min_excluding_ua);
  CHECK(state.composite_lower_bound_without_invariants == 3);
  CHECK(state.composite_lower_bound_with_invariants == 3);

  std::println("  PASS");
}

static void test_active_pattern_assertions_reject_skipped_metadata() {
  std::println("test_active_pattern_assertions_reject_skipped_metadata");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::site_pattern_options pattern_opts;
  pattern_opts.skip_invariant_sites = true;
  auto skipped = larch::build_site_patterns(dag, grammar, pattern_opts);
  CHECK(skipped.skipped_invariant_site_count == 3);

  larch::active_site_pattern_set bad{skipped};
  bool threw = false;
  try {
    bad.assert_no_skipped_invariant_metadata();
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_state_builder_from_dag_rebuilds_patterns_once() {
  std::println("test_state_builder_from_dag_rebuilds_patterns_once");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto state = larch::build_chart_spr_search_state(dag, grammar, opts);
  CHECK(state.counters.pattern_rebuilds == 1);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);
  CHECK(state.skipped_invariant_site_count == 3);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(larch::chart_spr_pattern_source_fingerprint_matches(
      dag, grammar, state.pattern_source_fingerprint));
  state.active_patterns.assert_no_skipped_invariant_metadata();

  std::println("  PASS");
}

static void test_non_default_cache_options_throw_until_batching_exists() {
  std::println("test_non_default_cache_options_throw_until_batching_exists");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.cache.max_cached_patterns = 1;

  bool threw = false;
  try {
    (void)larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, options);
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_unchartable_grammar_rejected_with_empty_active_patterns() {
  std::println("test_unchartable_grammar_rejected_with_empty_active_patterns");

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", tiny_inner("root", "A", {tiny_leaf("A", "A"),
                                      tiny_leaf("B", "A")}));
  auto grammar = larch::build_clade_grammar(dag);
  grammar.productions.clear();
  grammar.productions_by_parent.assign(grammar.clades.size(), {});
  grammar.productions_by_child.assign(grammar.clades.size(), {});
  auto patterns = larch::build_site_patterns(dag, grammar);

  bool threw = false;
  try {
    (void)larch::build_chart_spr_search_state(dag, grammar, patterns);
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_unsupported_enumeration_options_fail_explicitly() {
  std::println("test_unsupported_enumeration_options_fail_explicitly");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.include_neutral_or_reversal_candidates = true;

  bool threw = false;
  try {
    (void)larch::for_each_grammar_spr_candidate(
        fixture.grammar, options,
        [](larch::grammar_spr_candidate const&) { return true; });
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_max_affected_estimate_prunes_before_construction() {
  std::println("test_max_affected_estimate_prunes_before_construction");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_estimated_affected_clades = 1;

  std::size_t callbacks = 0;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const&) {
        ++callbacks;
        return true;
      });

  CHECK(callbacks == 0);
  CHECK(stats.candidates_constructed == 0);
  CHECK(stats.candidates_pruned_before_construction > 0);

  std::println("  PASS");
}

static void test_streaming_candidate_cap_stops_before_eager_path_precompute() {
  std::println("test_streaming_candidate_cap_stops_before_eager_path_precompute");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_candidates = 1;

  std::vector<larch::grammar_spr_candidate> streamed;
  auto streaming_stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const& candidate) {
        streamed.push_back(candidate);
        return true;
      });
  auto eager = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options);

  CHECK(streamed.size() == 1);
  CHECK(streaming_stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(streaming_stats.candidates_generated_after_dedup == 1);
  CHECK(streaming_stats.candidates_constructed == 1);
  CHECK(streaming_stats.upward_paths_completed <
        eager.stats.upward_paths_completed);

  std::println("  PASS");
}

static void test_streaming_path_pair_budget_stops_early() {
  std::println("test_streaming_path_pair_budget_stops_early");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_path_pairs_considered = 1;

  std::size_t callbacks = 0;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const&) {
        ++callbacks;
        return true;
      });

  CHECK(stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::path_budget);
  CHECK(stats.path_pairs_considered == 1);
  CHECK(stats.candidates_constructed <= 1);
  CHECK(callbacks <= 1);

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
  test_streaming_and_eager_candidate_apis_match();
  test_search_state_local_score_entry_point_matches_oracle();
  test_persistent_active_pattern_cache_matches_full_composite();
  test_root_reference_counts_preserved_in_cache();
  test_active_pattern_assertions_reject_skipped_metadata();
  test_state_builder_from_dag_rebuilds_patterns_once();
  test_non_default_cache_options_throw_until_batching_exists();
  test_unchartable_grammar_rejected_with_empty_active_patterns();
  test_unsupported_enumeration_options_fail_explicitly();
  test_max_affected_estimate_prunes_before_construction();
  test_streaming_candidate_cap_stops_before_eager_path_precompute();
  test_streaming_path_pair_budget_stops_early();
  test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute();
  std::println("chart_spr_search_test PASS");
  return 0;
}

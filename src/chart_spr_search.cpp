#include <larch/chart_spr_search.hpp>
#include <larch/rank3_rewrite.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace larch {
namespace {

double chart_spr_elapsed_ms(std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point stop) {
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

void validate_chart_spr_search_loop_options(
    chart_spr_search_options const& options) {
  validate_supported_chart_cache_options(options.cache);
  if (!options.materialize_accepted_moves) {
    throw std::runtime_error(
        "chart SPR search: sidecar-only accepted moves are not implemented "
        "yet; materialize_accepted_moves must remain true");
  }
  if (!options.rebuild_after_accept) {
    throw std::runtime_error(
        "chart SPR search: accepted-state local cache updates are not "
        "implemented yet; rebuild_after_accept must remain true");
  }
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::fixed_topology_exact) {
    throw std::runtime_error(
        "chart SPR Phase-5 search: fixed_topology_exact acceptance is not "
        "supported until the post-materialization commit gate can rescore the "
        "same selected topology certificate on the rebuilt grammar");
  }
}

bool chart_spr_rebuild_after_accept_needs_exact_trim(
    chart_spr_search_options const& options) {
  return options.acceptance_mode !=
         chart_spr_acceptance_mode::lower_bound_heuristic;
}

void chart_spr_add_search_state_rebuild_counters(
    chart_spr_search_counters& accumulated,
    chart_spr_search_counters const& rebuild_counters,
    bool update_current_skipped_invariant_sites) {
  accumulated.grammar_rebuilds += rebuild_counters.grammar_rebuilds;
  accumulated.pattern_rebuilds += rebuild_counters.pattern_rebuilds;
  accumulated.base_chart_cache_rebuilds +=
      rebuild_counters.base_chart_cache_rebuilds;
  accumulated.pattern_batch_cache_builds +=
      rebuild_counters.pattern_batch_cache_builds;
  if (update_current_skipped_invariant_sites) {
    accumulated.skipped_invariant_sites =
        rebuild_counters.skipped_invariant_sites;
  }
}

chart_spr_search_state rebuild_chart_spr_search_state_after_accept(
    chart_spr_search_state const& previous_state, phylo_dag& rebuilt_dag,
    clade_grammar rebuilt_grammar, chart_spr_search_options const& options,
    bool& reused_patterns) {
  validate_supported_chart_cache_options(options.cache);
  bool build_exact = chart_spr_rebuild_after_accept_needs_exact_trim(options);
  reused_patterns =
      !options.force_pattern_fingerprint_mismatch_for_tests &&
      chart_spr_pattern_source_fingerprint_matches(
          rebuilt_dag, rebuilt_grammar,
          previous_state.pattern_source_fingerprint);

  if (reused_patterns) {
    chart_spr_active_pattern_build_result active_build;
    active_build.active_patterns = previous_state.active_patterns;
    active_build.pattern_source_fingerprint =
        previous_state.pattern_source_fingerprint;
    active_build.invariant_constant_offset =
        previous_state.invariant_constant_offset;
    active_build.skipped_invariant_site_count =
        previous_state.skipped_invariant_site_count;
    return build_chart_spr_search_state_from_active(
        rebuilt_dag, std::move(rebuilt_grammar), std::move(active_build),
        options.chart, build_exact, options.exact_trim, options.cache);
  }

  auto active_build = make_active_search_patterns(
      rebuilt_dag, rebuilt_grammar, options.chart);
  auto state = build_chart_spr_search_state_from_active(
      rebuilt_dag, std::move(rebuilt_grammar), std::move(active_build),
      options.chart, build_exact, options.exact_trim, options.cache);
  ++state.counters.pattern_rebuilds;
  return state;
}

std::uint64_t chart_spr_post_materialization_objective_score(
    chart_spr_search_state const& rebuilt_state,
    chart_spr_search_options const& options) {
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::lower_bound_heuristic) {
    return rebuilt_state.composite_lower_bound_with_invariants;
  }
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::fixed_topology_exact) {
    throw std::runtime_error(
        "chart SPR Phase-5 search: fixed_topology_exact rebuilt-objective "
        "gate is not implemented");
  }
  return chart_spr_state_exact_score_with_invariants(rebuilt_state,
                                                     options.exact_trim);
}

rank3_option_b_result materialize_chart_spr_accepted_candidate(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& accepted) {
  if (state.dag == nullptr) {
    throw std::runtime_error(
        "chart SPR search: current state has no source DAG");
  }

  rank3_option_b_options option_b;
  try {
    return materialize_rank3_option_b(*state.dag, state.grammar,
                                      accepted.candidate, option_b);
  } catch (std::exception const& option_b_error) {
    rank3_option_a_options option_a;
    option_a.include_original_dag = option_b.include_original_dag;
    option_a.validate = option_b.validate;
    option_a.generated_edge_weight = option_b.added_edge_weight;
    option_a.require_intended_productions_present =
        option_b.require_intended_productions_present;
    option_a.rebuild_grammar_options = option_b.rebuild_grammar_options;
    try {
      auto fallback = materialize_rank3_option_a(
          *state.dag, state.grammar, accepted.candidate, option_a);
      rank3_option_b_result converted;
      converted.dag = std::move(fallback.dag);
      converted.rebuilt = std::move(fallback.rebuilt);
      converted.materialized_tree_count = fallback.materialized_tree_count;
      converted.staged_in_overlay = false;
      converted.used_source_tree_move = false;
      converted.intended_productions =
          std::move(fallback.intended_productions);
      converted.intended_production_present =
          std::move(fallback.intended_production_present);
      return converted;
    } catch (std::exception const& option_a_error) {
      throw std::runtime_error(
          std::string{"chart SPR search: accepted candidate materialization "
                      "failed with Option B ('"} +
          option_b_error.what() + "') and Option A ('" +
          option_a_error.what() + "')");
    }
  }
}

void chart_spr_refresh_search_summary_from_counters(
    chart_spr_search_summary& summary,
    chart_spr_search_counters const& counters) {
  summary.accepted_moves = counters.accepted_moves;
  summary.candidates_locally_scored = counters.local_candidate_scores;
  summary.local_rows_recomputed = counters.local_rows_recomputed;
  summary.candidate_batches_scored = counters.candidate_batches_scored;
  summary.pattern_batch_cache_builds = counters.pattern_batch_cache_builds;
  summary.exact_verifications = counters.exact_verifications;
  summary.overlay_materializations_for_exact_verification =
      counters.overlay_materializations_for_exact_verification;
  summary.overlay_materializations_for_accept_materialization =
      counters.overlay_materializations_for_accept_materialization;
  summary.sidecar_rebuilds_after_accept =
      counters.sidecar_rebuilds_after_accept;
  summary.candidate_accepts_attempted = counters.candidate_accepts_attempted;
  summary.post_materialization_rejections =
      counters.post_materialization_rejections;
  summary.full_search_state_rebuilds =
      summary.initial_search_state_rebuilds +
      summary.sidecar_rebuilds_after_accept;
}

}  // namespace

chart_spr_search_result run_chart_spr_search(
    phylo_dag initial_dag, clade_grammar initial_grammar,
    chart_spr_search_options options) {
  validate_chart_spr_search_loop_options(options);

  auto total_start = std::chrono::steady_clock::now();
  chart_spr_search_result result;
  result.dag = std::move(initial_dag);
  result.summary.acceptance_mode = options.acceptance_mode;
  result.summary.candidate_selection = options.candidate_selection;
  result.summary.initial_search_state_rebuilds = 1;

  auto cache_start = std::chrono::steady_clock::now();
  auto state = build_chart_spr_search_state(
      result.dag, std::move(initial_grammar), options);
  ++state.counters.grammar_rebuilds;
  result.summary.cache_build_ms = chart_spr_elapsed_ms(
      cache_start, std::chrono::steady_clock::now());
  result.summary.initial_score =
      chart_spr_iteration_state_score_before(state, options);
  result.summary.final_score = result.summary.initial_score;
  result.summary.active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  result.summary.chart_cache_estimated_full_bytes =
      state.estimated_full_pattern_cache_bytes;
  result.summary.chart_cache_resident_bytes =
      state.resident_pattern_cache_bytes;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
  result.summary.local_score_worker_count =
      chart_spr_search_detail::normalize_chart_spr_worker_count(
          options.local_score_worker_count);
  std::vector<std::size_t> aggregate_affected_counts;

  std::string immediate_reversal_key_to_skip;
  for (std::size_t iter = 0; iter < options.max_iterations; ++iter) {
    auto iteration_options = options;
    iteration_options.enumeration.immediate_reversal_candidate_key_to_skip =
        immediate_reversal_key_to_skip;
    auto iteration_seed = options.seed + static_cast<std::uint32_t>(iter);
    iteration_options.seed = iteration_seed;
    iteration_options.enumeration.seed = iteration_seed;
    auto iteration = run_chart_spr_acceptance_iteration(state,
                                                        iteration_options,
                                                        iter);
    result.summary.candidates_generated += iteration.candidates_generated;
    result.summary.local_scoring_ms += iteration.local_scoring_ms;
    result.summary.exact_verification_ms += iteration.exact_verification_ms;
    aggregate_affected_counts.insert(
        aggregate_affected_counts.end(), iteration.affected_clade_counts.begin(),
        iteration.affected_clade_counts.end());

    if (!iteration.accepted) {
      result.summary.final_score = iteration.state_score_after;
      result.iterations.push_back(std::move(iteration));
      break;
    }

    auto attempt_counters = state.counters;
    auto materialize_start = std::chrono::steady_clock::now();
    try {
      auto materialized = materialize_chart_spr_accepted_candidate(
          state, *iteration.accepted);
      ++attempt_counters.full_overlay_materializations;
      ++attempt_counters.overlay_materializations_for_accept_materialization;
      ++attempt_counters.grammar_rebuilds;
      ++attempt_counters.sidecar_rebuilds_after_accept;

      bool reused_patterns = false;
      auto tentative_state = rebuild_chart_spr_search_state_after_accept(
          state, materialized.dag, std::move(materialized.rebuilt.grammar),
          options, reused_patterns);
      chart_spr_add_search_state_rebuild_counters(
          attempt_counters, tentative_state.counters, true);
      tentative_state.counters = attempt_counters;
      result.summary.accepted_rebuild_ms += chart_spr_elapsed_ms(
          materialize_start, std::chrono::steady_clock::now());

      auto check_start = std::chrono::steady_clock::now();
      auto rebuilt_score = chart_spr_post_materialization_objective_score(
          tentative_state, options);
      if (options.override_post_materialization_rebuilt_score_for_tests) {
        rebuilt_score =
            *options.override_post_materialization_rebuilt_score_for_tests;
      }
      result.summary.post_materialization_check_ms += chart_spr_elapsed_ms(
          check_start, std::chrono::steady_clock::now());
      iteration.post_materialization_rebuilt_score = rebuilt_score;
      iteration.reused_patterns_after_accept = reused_patterns;

      if (rebuilt_score > iteration.state_score_before) {
        ++attempt_counters.post_materialization_rejections;
        state.counters = attempt_counters;
        iteration.post_materialization_rejected = true;
        iteration.accepted_move_committed = false;
        iteration.state_score_after = iteration.state_score_before;
        iteration.no_accept_reason =
            "post-materialization rebuilt objective worsened";
        iteration.post_materialization_rejection_reason =
            "rebuilt objective " + std::to_string(rebuilt_score) +
            " exceeds pre-accept objective " +
            std::to_string(iteration.state_score_before);
        result.summary.final_score = iteration.state_score_after;
        result.iterations.push_back(std::move(iteration));
        break;
      }

      immediate_reversal_key_to_skip = chart_spr_candidate_immediate_reverse_key(
          state.grammar, iteration.accepted->candidate);
      ++attempt_counters.accepted_moves;
      result.dag = std::move(materialized.dag);
      tentative_state.dag = &result.dag;
      tentative_state.counters = attempt_counters;
      state = std::move(tentative_state);
      iteration.accepted_move_committed = true;
      iteration.state_score_after = rebuilt_score;
      result.summary.final_score = rebuilt_score;
      result.iterations.push_back(std::move(iteration));
    } catch (std::exception const& e) {
      result.summary.accepted_rebuild_ms += chart_spr_elapsed_ms(
          materialize_start, std::chrono::steady_clock::now());
      ++attempt_counters.post_materialization_rejections;
      state.counters = attempt_counters;
      iteration.post_materialization_rejected = true;
      iteration.accepted_move_committed = false;
      iteration.state_score_after = iteration.state_score_before;
      iteration.no_accept_reason =
          "accepted candidate failed materialization/rebuild";
      iteration.post_materialization_rejection_reason = e.what();
      result.summary.final_score = iteration.state_score_after;
      result.iterations.push_back(std::move(iteration));
      break;
    }
  }

  result.counters = state.counters;
  result.summary.iterations = result.iterations.size();
  chart_spr_refresh_search_summary_from_counters(result.summary,
                                                 result.counters);
  result.summary.active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  result.summary.chart_cache_estimated_full_bytes =
      state.estimated_full_pattern_cache_bytes;
  result.summary.chart_cache_resident_bytes =
      state.resident_pattern_cache_bytes;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
  result.summary.total_ms = chart_spr_elapsed_ms(
      total_start, std::chrono::steady_clock::now());
  result.summary.effective_candidate_batch_size =
      state.effective_candidate_batch_size;
  if (result.summary.local_scoring_ms > 0.0) {
    auto seconds = result.summary.local_scoring_ms / 1000.0;
    result.summary.local_candidates_per_second =
        static_cast<double>(result.summary.candidates_locally_scored) /
        seconds;
    result.summary.local_rows_recomputed_per_second =
        static_cast<double>(result.summary.local_rows_recomputed) / seconds;
  }
  result.summary.affected_distribution =
      summarize_affected_clade_counts(std::move(aggregate_affected_counts));
  return result;
}

}  // namespace larch

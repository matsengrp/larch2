#include <larch/chart_spr_search.hpp>

#include "test_util.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

[[noreturn]] static void test_fail(char const* expr, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr)                                    \
  do {                                                 \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

namespace {

using namespace std::chrono_literals;

larch::test::tiny_tree_node four_taxon_base_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

larch::test::tiny_tree_node four_taxon_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})});
}

larch::test::tiny_tree_node four_taxon_multisite_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAAA",
      {tiny_inner("AB", "AAAA",
                  {tiny_leaf("A", "AAAA"), tiny_leaf("B", "ACAA")}),
       tiny_inner("CD", "AAAA",
                  {tiny_leaf("C", "AACA"), tiny_leaf("D", "AAAC")})});
}

struct fixture {
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
};

fixture make_fixture(bool misplaced = false) {
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", misplaced ? four_taxon_misplaced_tree() : four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  return fixture{std::move(dag), std::move(grammar)};
}

fixture make_multisite_fixture() {
  auto dag =
      larch::test::make_tiny_labelled_tree("AAAA", four_taxon_multisite_tree());
  auto grammar = larch::build_clade_grammar(dag);
  return fixture{std::move(dag), std::move(grammar)};
}

larch::chart_spr_search_options pipeline_options(std::size_t workers = 4) {
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.max_iterations = 1;
  options.max_candidates_per_iteration = 32;
  options.enumeration.max_candidates = 32;
  options.enumeration.max_candidates_is_post_dedup = true;
  options.cache.candidate_batch_size = 1;
  options.worker_count = workers;
  options.local_score_worker_count = workers;
  options.enable_candidate_generation_pipeline = true;
  return options;
}

void check_scheduler_quiescent(larch::chart_scheduler const& scheduler) {
  auto const metrics = scheduler.metrics();
  CHECK(metrics.rejected_concurrent_operations == 0);
  CHECK(metrics.tasks_submitted == metrics.tasks_completed);
  CHECK(metrics.tasks_submitted == metrics.tasks_joined);
  CHECK(metrics.pending_tasks == 0);
}

std::array<larch::chart_spr_scheduler_axis_metrics const*, 13> axes(
    larch::chart_spr_scheduler_axis_counters const& counters) {
  return {
      &counters.initial_chart_patterns,
      &counters.candidate_generation,
      &counters.exact_setup_patterns,
      &counters.exact_frontier_clades,
      &counters.exact_candidates,
      &counters.lazy_inside_clades,
      &counters.lazy_outside_clades,
      &counters.inside_cache_patterns,
      &counters.outside_cache_patterns,
      &counters.fixed_topology_patterns,
      &counters.local_score_candidates,
      &counters.local_score_candidate_patterns,
      &counters.other,
  };
}

void check_scheduler_axis_reconciliation(
    larch::chart_scheduler_metrics const& scheduler,
    larch::chart_spr_scheduler_axis_counters const& counters) {
  std::uint64_t operations = 0;
  std::uint64_t ranges = 0;
  std::uint64_t tasks = 0;
  std::size_t active = 0;
  for (auto const* axis : axes(counters)) {
    operations += axis->operations;
    ranges += axis->ranges;
    tasks += axis->worker_tasks;
    active = std::max(active, axis->active_worker_high_water);
  }
  CHECK(operations == scheduler.operations);
  CHECK(ranges == scheduler.ranges_created);
  CHECK(tasks == scheduler.tasks_submitted);
  CHECK(active == scheduler.active_worker_high_water);
}

larch::chart_spr_search_options semantic_case_options(
    larch::chart_spr_candidate_source source, std::uint32_t seed,
    std::size_t workers) {
  auto options = pipeline_options(workers);
  options.cache.candidate_batch_size = 2;
  options.max_candidates_per_iteration = 12;
  options.enumeration.max_candidates = 12;
  options.enumeration.source = source;
  options.enumeration.sampled_tree_count = 2;
  options.enumeration.randomize_order = true;
  options.enumeration.reservoir_sample = true;
  options.seed = seed;
  options.enumeration.seed = seed;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
  return options;
}

larch::chart_spr_search_result run_semantic_case(
    larch::chart_spr_candidate_source source, std::uint32_t seed,
    std::size_t workers) {
  auto input = make_fixture(true);
  auto options = semantic_case_options(source, seed, workers);
  return larch::run_chart_spr_search(std::move(input.dag), input.grammar,
                                     options);
}

larch::chart_spr_search_result run_exact_hybrid_semantic_case(
    std::uint32_t seed, std::size_t workers) {
  auto input = make_fixture(true);
  auto options = semantic_case_options(
      larch::chart_spr_candidate_source::hybrid, seed, workers);
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 4;
  return larch::run_chart_spr_search(std::move(input.dag), input.grammar,
                                     options);
}

void test_worker_source_seed_semantics() {
  std::println("test_worker_source_seed_semantics");
  constexpr std::array sources{
      larch::chart_spr_candidate_source::grammar,
      larch::chart_spr_candidate_source::sampled_tree,
      larch::chart_spr_candidate_source::hybrid,
  };
  constexpr std::array<std::uint32_t, 3> seeds{1, 7, 19};
  constexpr std::array<std::size_t, 4> workers{1, 2, 4, 8};

  for (auto source : sources) {
    for (auto seed : seeds) {
      std::optional<std::string> oracle_digest;
      std::optional<std::string> oracle_sidecar;
      for (auto worker_count : workers) {
        auto search = run_semantic_case(source, seed, worker_count);
        CHECK(search.canonical_digest.has_value());
        auto digest = larch::emit_chart_spr_semantic_digest_json(
            *search.canonical_digest);
        if (worker_count == 1) {
          oracle_digest = std::move(digest);
          oracle_sidecar = search.canonical_digest->full_sidecar;
          CHECK(search.summary.candidate_pipeline_batches_generated == 0);
        } else {
          CHECK(digest == *oracle_digest);
          CHECK(search.canonical_digest->full_sidecar == *oracle_sidecar);
          CHECK(search.summary.candidate_pipeline_batches_generated > 0);
          CHECK(search.summary.candidate_pipeline_batches_scored > 0);
          CHECK(search.summary
                    .candidate_pipeline_scheduler_projection_overlap_batches ==
                0);
        }
        CHECK(search.summary.scheduler.rejected_concurrent_operations == 0);
        check_scheduler_axis_reconciliation(search.summary.scheduler,
                                            search.summary.scheduler_axes);
      }
    }
  }
  std::println("  PASS");
}

void test_exact_hybrid_worker_seed_semantics() {
  std::println("test_exact_hybrid_worker_seed_semantics");
  constexpr std::array<std::uint32_t, 3> seeds{1, 7, 19};
  constexpr std::array<std::size_t, 4> workers{1, 2, 4, 8};

  for (auto seed : seeds) {
    std::optional<std::string> w1_digest;
    std::optional<std::string> w1_sidecar;
    for (auto worker_count : workers) {
      auto search = run_exact_hybrid_semantic_case(seed, worker_count);

      CHECK(search.summary.requested_worker_count == worker_count);
      CHECK(search.summary.resolved_worker_count == worker_count);
      CHECK(search.summary.initial_score == 2);
      CHECK(search.summary.final_score == 1);
      CHECK(search.summary.accepted_moves == 1);
      CHECK(search.iterations.size() == 1);
      auto const& iteration = search.iterations.front();
      CHECK(iteration.candidates_generated == 12);
      CHECK(iteration.candidates_scored == 12);
      CHECK(iteration.locally_ranked_candidates_retained == 4);
      CHECK(iteration.candidates_exact_verified == 4);
      CHECK(iteration.accepted.has_value());
      CHECK(iteration.accepted_move_committed);
      CHECK(iteration.state_score_before == 2);
      CHECK(iteration.state_score_after == 1);
      CHECK(iteration.accepted->exact.has_value());
      CHECK(iteration.accepted->exact->kind ==
            larch::chart_spr_score_kind::grammar_exact);

      CHECK(search.canonical_report.has_value());
      auto const& report = *search.canonical_report;
      CHECK(report.contract.acceptance == "exact_multisite");
      CHECK(report.contract.objective == "grammar_exact");
      CHECK(report.contract.candidate_selection == "lower_bound_top_k");
      CHECK(report.contract.candidate_source == "hybrid");
      CHECK(report.contract.max_candidates == 12);
      CHECK(report.contract.top_k_exact == 4);
      CHECK(report.contract.sampled_tree_count == 2);
      CHECK(report.contract.randomize_order);
      CHECK(report.contract.reservoir_sample);
      CHECK(report.contract.seed == seed);
      CHECK(report.iterations.size() == 1);
      auto const& canonical_iteration = report.iterations.front();
      CHECK(canonical_iteration.candidates.size() == 12);
      CHECK(canonical_iteration.exact_verified_stream_indices.size() == 4);
      CHECK(canonical_iteration.accepted_move_present);
      CHECK(canonical_iteration.accepted_move_committed);
      CHECK(canonical_iteration.selected_stream_index.has_value());
      CHECK(!canonical_iteration.selected_signature.empty());

      std::size_t exact_evidence_records = 0;
      for (auto stream_index :
           canonical_iteration.exact_verified_stream_indices) {
        CHECK(stream_index < canonical_iteration.candidates.size());
        auto const& candidate =
            canonical_iteration.candidates[stream_index];
        CHECK(candidate.valid);
        CHECK(candidate.invalid_reason.empty());
        CHECK(candidate.exact.has_value());
        CHECK(candidate.exact_evidence.has_value());
        CHECK(candidate.exact->kind == "grammar_exact");
        CHECK(candidate.exact->exact_multisite);
        auto const& evidence = *candidate.exact_evidence;
        CHECK(evidence.evidence_kind ==
              "grammar_exact_frontier_provenance_companion");
        CHECK(evidence.keep_production_exact);
        CHECK(evidence.keep_mask_kind ==
              "exact_optimal_production_union");
        CHECK(!evidence.kept_production_keys.empty());
        CHECK(!evidence.frontier_sizes.empty());
        CHECK(!evidence.optimal_root_provenance_classes.empty());
        ++exact_evidence_records;
      }
      CHECK(exact_evidence_records == 4);
      CHECK(report.final_exact.has_value());
      CHECK(report.final_exact->keep_production_exact);

      CHECK(search.canonical_digest.has_value());
      CHECK(search.canonical_digest->candidate_count == 12);
      CHECK(search.canonical_digest->exact_candidate_count == 4);
      CHECK(!search.canonical_digest->full_sidecar.empty());
      auto digest = larch::emit_chart_spr_semantic_digest_json(
          *search.canonical_digest);
      if (worker_count == 1) {
        w1_digest = digest;
        w1_sidecar = search.canonical_digest->full_sidecar;
        CHECK(search.summary.candidate_pipeline_batches_generated == 0);
        CHECK(search.summary.candidate_pipeline_batches_scored == 0);
      } else {
        CHECK(w1_digest.has_value());
        CHECK(w1_sidecar.has_value());
        CHECK(digest == *w1_digest);
        CHECK(search.canonical_digest->full_sidecar == *w1_sidecar);
        CHECK(search.summary.candidate_pipeline_batches_generated > 0);
        CHECK(search.summary.candidate_pipeline_batches_scored > 0);
        CHECK(search.summary.exact_candidate_parallel_batches > 0);
        CHECK(search.summary.scheduler_axes.exact_candidates
                  .parallel_operations > 0);
        CHECK(search.summary
                  .candidate_pipeline_scheduler_projection_overlap_batches ==
              0);
      }
      CHECK(search.summary.scheduler.rejected_concurrent_operations == 0);
      check_scheduler_axis_reconciliation(search.summary.scheduler,
                                          search.summary.scheduler_axes);
    }
  }
  std::println("  PASS");
}

void test_full_slot_cancellation_drains_and_recovers() {
  std::println("test_full_slot_cancellation_drains_and_recovers");
  auto input = make_fixture();
  auto options = pipeline_options();
  options.force_candidate_pipeline_cancel_after_scored_batches_for_tests = 1;
  options.before_candidate_pipeline_score_batch_for_tests =
      [](std::size_t batch) {
        if (batch == 0) std::this_thread::sleep_for(30ms);
      };
  auto state = larch::build_chart_spr_search_state(input.dag, input.grammar);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;

  auto cancelled = larch::run_chart_spr_acceptance_iteration(
      state, options, 0, workspace, scheduler);
  CHECK(cancelled.candidate_generation.candidate_pipeline_cancellations == 1);
  CHECK(cancelled.candidate_generation.candidate_pipeline_producer_stalls > 0);
  CHECK(
      cancelled.candidate_generation.candidate_pipeline_serial_overlap_batches >
      0);
  CHECK(cancelled.candidate_generation
            .candidate_pipeline_scheduler_projection_overlap_batches == 0);
  CHECK(cancelled.candidate_generation
            .candidate_pipeline_stale_candidates_discarded > 0);
  CHECK(cancelled.candidates_generated == cancelled.candidates_scored);
  CHECK(cancelled.candidate_generation.stop_reason ==
        larch::chart_spr_candidate_stop_reason::callback_stop);
  CHECK(workspace.active_candidate_count(0) == 0);
  CHECK(workspace.active_candidate_count(1) == 0);
  CHECK(workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(scheduler);

  auto recovery_options = pipeline_options();
  recovery_options.enable_candidate_generation_pipeline = false;
  auto recovered = larch::run_chart_spr_acceptance_iteration(
      state, recovery_options, 1, workspace, scheduler);
  CHECK(recovered.candidates_scored > 0);
  CHECK(workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(scheduler);

  scheduler.shutdown();
  check_scheduler_axis_reconciliation(scheduler.metrics(),
                                      state.counters.scheduler_axes);
  std::println("  PASS");
}

void test_early_acceptance_discards_speculation() {
  std::println("test_early_acceptance_discards_speculation");
  auto input = make_fixture(true);
  auto options = pipeline_options();
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_first_improvement;
  options.before_candidate_pipeline_score_batch_for_tests = [](std::size_t) {
    std::this_thread::sleep_for(10ms);
  };
  auto state = larch::build_chart_spr_search_state(input.dag, input.grammar);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  auto iteration = larch::run_chart_spr_acceptance_iteration(
      state, options, 0, workspace, scheduler);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->lower_bound.value.improves());
  CHECK(iteration.candidate_generation.candidate_pipeline_cancellations == 1);
  CHECK(iteration.candidate_generation
            .candidate_pipeline_stale_candidates_discarded > 0);
  CHECK(iteration.candidates_generated == iteration.candidates_scored);
  check_scheduler_quiescent(scheduler);
  scheduler.shutdown();
  std::println("  PASS");
}

void test_stale_stamp_never_scores() {
  std::println("test_stale_stamp_never_scores");
  auto input = make_fixture();
  auto options = pipeline_options();
  options.force_candidate_pipeline_stale_buffer_after_batches_for_tests = 0;
  auto state = larch::build_chart_spr_search_state(input.dag, input.grammar);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  std::string failure;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(state, options, 0,
                                                    workspace, scheduler);
  } catch (larch::chart_execution_plan_mismatch const& error) {
    failure = error.what();
  }
  CHECK(failure.find("stale state generation/epoch") != std::string::npos);
  CHECK(state.counters.local_candidate_scores == 0);
  CHECK(state.counters.candidate_pipeline_state_epoch_rejections == 1);
  CHECK(workspace.active_candidate_count(0) == 0);
  CHECK(workspace.active_candidate_count(1) == 0);
  CHECK(workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(scheduler);

  auto recovery = pipeline_options();
  auto recovered = larch::run_chart_spr_acceptance_iteration(
      state, recovery, 1, workspace, scheduler);
  CHECK(recovered.candidates_scored > 0);
  CHECK(workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(scheduler);
  scheduler.shutdown();
  check_scheduler_axis_reconciliation(scheduler.metrics(),
                                      state.counters.scheduler_axes);
  std::println("  PASS");
}

void test_error_precedence_drain_and_recovery() {
  std::println("test_error_precedence_drain_and_recovery");
  auto input = make_fixture();
  auto options = pipeline_options();
  options.force_candidate_pipeline_generation_failure_after_for_tests = 1;
  options.force_candidate_pipeline_scoring_failure_after_batches_for_tests = 0;
  options.before_candidate_pipeline_score_batch_for_tests = [](std::size_t) {
    std::this_thread::sleep_for(20ms);
  };
  auto state = larch::build_chart_spr_search_state(input.dag, input.grammar);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  std::string failure;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(state, options, 0,
                                                    workspace, scheduler);
  } catch (std::runtime_error const& error) {
    failure = error.what();
  }
  CHECK(failure.find("forced scoring failure") != std::string::npos);
  CHECK(state.counters.candidate_pipeline_generation_errors == 1);
  CHECK(workspace.active_candidate_count(0) == 0);
  CHECK(workspace.active_candidate_count(1) == 0);
  CHECK(workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(scheduler);

  auto recovery = pipeline_options();
  auto iteration = larch::run_chart_spr_acceptance_iteration(
      state, recovery, 1, workspace, scheduler);
  CHECK(iteration.candidates_scored > 0);
  check_scheduler_quiescent(scheduler);
  scheduler.shutdown();
  check_scheduler_axis_reconciliation(scheduler.metrics(),
                                      state.counters.scheduler_axes);
  std::println("  PASS");
}

void test_generation_error_drains() {
  std::println("test_generation_error_drains");
  auto input = make_fixture();
  auto options = pipeline_options();
  options.force_candidate_pipeline_generation_failure_after_for_tests = 1;
  auto state = larch::build_chart_spr_search_state(input.dag, input.grammar);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  std::string failure;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(state, options, 0,
                                                    workspace, scheduler);
  } catch (std::runtime_error const& error) {
    failure = error.what();
  }
  CHECK(failure.find("forced generation failure") != std::string::npos);
  CHECK(state.counters.candidate_pipeline_generation_errors == 1);
  CHECK(workspace.active_candidate_count(0) == 0);
  CHECK(workspace.active_candidate_count(1) == 0);
  CHECK(workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(scheduler);

  auto recovery = pipeline_options();
  auto recovered = larch::run_chart_spr_acceptance_iteration(
      state, recovery, 1, workspace, scheduler);
  CHECK(recovered.candidates_scored > 0);
  CHECK(workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(scheduler);
  scheduler.shutdown();
  check_scheduler_axis_reconciliation(scheduler.metrics(),
                                      state.counters.scheduler_axes);
  std::println("  PASS");
}

void test_finite_sampled_source_adaptive_planning() {
  std::println("test_finite_sampled_source_adaptive_planning");

  auto input = make_fixture();
  auto state = larch::build_chart_spr_search_state(input.dag, input.grammar);
  larch::chart_scheduler scheduler{
      larch::chart_scheduler_options{.requested_workers = 8}};

  larch::grammar_spr_enumeration_options options;
  options.source = larch::chart_spr_candidate_source::sampled_tree;
  options.sampled_tree_source_dag = &input.dag;
  options.sampled_tree_count = 1;
  options.max_candidates = 32;
  options.max_candidates_is_post_dedup = true;

  auto estimate = [&](std::size_t requested_source_wave) {
    return larch::chart_spr_search_detail::
        estimate_grammar_spr_finite_iteration_memory_envelope(
            state, 32, 1, 1, false, scheduler, 1,
            &options, 1, false, requested_source_wave, 0, 0);
  };

  auto const maximum = estimate(0);
  // Finite admission remains conservative at the complete worker width. The
  // runtime policy narrows only its first active source wave, so the retained
  // slots and memory envelope still bound a later full-width wave.
  CHECK(maximum.planned_sampled_source_wave_size == 8);
  CHECK(maximum.planned_sampled_projection_wave_size >
        maximum.planned_sampled_source_wave_size);
  CHECK(estimate(8).planned_sampled_source_wave_size == 8);
  CHECK(estimate(4).planned_sampled_source_wave_size == 4);
  CHECK(estimate(1).planned_sampled_source_wave_size == 1);
  CHECK(larch::chart_spr_detail::sampled_tree_source_initial_runtime_wave_size(
            8, true, true, 0) == 4);
  CHECK(larch::chart_spr_detail::sampled_tree_source_initial_runtime_wave_size(
            8, true, false, 0) == 8);
  CHECK(larch::chart_spr_detail::sampled_tree_source_initial_runtime_wave_size(
            8, true, true, 2) == 2);
  CHECK(larch::chart_spr_detail::sampled_tree_source_initial_runtime_wave_size(
            4, true, true, 8) == 4);

  // Zero is the exhaustive/reservoir-expanded policy and remains at the
  // resolved worker width in the same allocation-free envelope.
  options.max_candidates = 0;
  auto const uncapped = estimate(0);
  CHECK(uncapped.planned_sampled_source_wave_size == 8);
  CHECK(uncapped.planned_sampled_projection_wave_size == 32);

  check_scheduler_quiescent(scheduler);
  scheduler.shutdown();
  std::println("  PASS");
}

void test_finite_admission_exact_boundary() {
  std::println("test_finite_admission_exact_boundary");
  for (bool use_lazy : {true, false}) {
    for (auto source : {larch::chart_spr_candidate_source::grammar,
                        larch::chart_spr_candidate_source::sampled_tree,
                        larch::chart_spr_candidate_source::hybrid}) {
      auto make_options = [&] {
        auto options = pipeline_options();
        options.cache.use_lazy_multisite_chart = use_lazy;
        options.enumeration.source = source;
        options.enumeration.sampled_tree_count = 2;
        return options;
      };
      auto make_state = [&](fixture& input,
                            larch::chart_spr_search_options const& options) {
        auto state_options = options;
        state_options.cache.memory_budget_bytes = 0;
        auto state = larch::build_chart_spr_search_state(
            input.dag, input.grammar, state_options);
        CHECK(state.cache_strategy ==
              (use_lazy
                   ? larch::chart_spr_cache_strategy::lazy_multisite_chart
                   : larch::chart_spr_cache_strategy::all_active_patterns));
        return state;
      };

      auto discovery_input = make_fixture();
      auto discovery_options = make_options();
      std::atomic<std::size_t> discovery_starts{0};
      std::atomic<std::size_t> discovery_projection_allocations{0};
      discovery_options.before_candidate_pipeline_start_for_tests = [&] {
        discovery_starts.fetch_add(1, std::memory_order_relaxed);
      };
      discovery_options.enumeration
          .before_sampled_tree_projection_workspace_allocation_for_tests = [&] {
        discovery_projection_allocations.fetch_add(1,
                                                   std::memory_order_relaxed);
      };
      discovery_options.cache.memory_budget_bytes = 1;
      auto discovery_state = make_state(discovery_input, discovery_options);
      larch::chart_scheduler discovery_scheduler{
          larch::chart_scheduler_options{.requested_workers = 4}};
      larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
          discovery_workspace;
      std::size_t envelope = 0;
      bool discovery_rejected = false;
      try {
        (void)larch::run_chart_spr_acceptance_iteration(
            discovery_state, discovery_options, 0, discovery_workspace,
            discovery_scheduler);
      } catch (larch::chart_spr_search_detail::
                   chart_spr_lazy_local_budget_error const& error) {
        discovery_rejected = true;
        CHECK(error.available_bytes() == 1);
        envelope = error.required_bytes();
      }
      CHECK(discovery_rejected);
      CHECK(envelope > 1);
      CHECK(discovery_starts.load(std::memory_order_relaxed) == 0);
      CHECK(discovery_projection_allocations.load(std::memory_order_relaxed) ==
            0);
      CHECK(discovery_workspace.candidate_slots.capacity() == 0);
      CHECK(discovery_workspace.pipeline_candidate_slots.capacity() == 0);
      CHECK(discovery_scheduler.metrics().operations == 0);
      discovery_scheduler.shutdown();

      auto exact_input = make_fixture();
      auto exact_options = make_options();
      std::atomic<std::size_t> exact_starts{0};
      std::atomic<std::size_t> exact_projection_allocations{0};
      exact_options.before_candidate_pipeline_start_for_tests = [&] {
        exact_starts.fetch_add(1, std::memory_order_relaxed);
      };
      exact_options.enumeration
          .before_sampled_tree_projection_workspace_allocation_for_tests = [&] {
        exact_projection_allocations.fetch_add(1, std::memory_order_relaxed);
      };
      exact_options.cache.memory_budget_bytes = envelope;
      auto exact_state = make_state(exact_input, exact_options);
      larch::chart_scheduler exact_scheduler{
          larch::chart_scheduler_options{.requested_workers = 4}};
      larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
          exact_workspace;
      auto envelope_options = exact_options.enumeration;
      if (source != larch::chart_spr_candidate_source::grammar) {
        envelope_options.sampled_tree_source_dag = exact_state.dag;
      }
      auto const rank_limit = larch::chart_spr_rank_buffer_limit(exact_options);
      auto const ranked_reserve_limit =
          rank_limit == larch::chart_spr_rank_unlimited
              ? exact_options.enumeration.max_candidates
              : std::min(exact_options.enumeration.max_candidates, rank_limit);
      auto const planned = larch::chart_spr_search_detail::
          estimate_grammar_spr_finite_iteration_memory_envelope(
              exact_state, exact_options.enumeration.max_candidates, 1,
              ranked_reserve_limit, false, exact_scheduler, 1,
              &envelope_options, 2, true,
              source == larch::chart_spr_candidate_source::grammar ? 0 : 1,
              source == larch::chart_spr_candidate_source::grammar ? 0 : 1,
              source == larch::chart_spr_candidate_source::sampled_tree ? 0
                                                                        : 1);
      CHECK(planned.planned_required_bytes == envelope);
      auto selected_grammar_plan = planned;
      if (source == larch::chart_spr_candidate_source::grammar) {
        selected_grammar_plan = larch::chart_spr_search_detail::
            estimate_grammar_spr_finite_iteration_memory_envelope(
                exact_state, exact_options.enumeration.max_candidates, 1,
                ranked_reserve_limit, false, exact_scheduler, 1,
                &envelope_options, 2, true, 0, 0, 0);
        if (selected_grammar_plan.planned_required_bytes > envelope) {
          std::size_t low = 1;
          std::size_t high =
              selected_grammar_plan.planned_grammar_candidate_wave_size;
          selected_grammar_plan = planned;
          while (low < high) {
            auto const midpoint = low + (high - low + 1) / 2;
            auto candidate = larch::chart_spr_search_detail::
                estimate_grammar_spr_finite_iteration_memory_envelope(
                    exact_state, exact_options.enumeration.max_candidates, 1,
                    ranked_reserve_limit, false, exact_scheduler, 1,
                    &envelope_options, 2, true, 0, 0, midpoint);
            if (candidate.planned_required_bytes <= envelope) {
              low = midpoint;
              selected_grammar_plan = std::move(candidate);
            } else {
              high = midpoint - 1;
            }
          }
        }
        CHECK(selected_grammar_plan.planned_required_bytes <= envelope);
      }
      CHECK(planned.planned_cache_strategy == exact_state.cache_strategy);
      CHECK(planned.planned_candidate_batch_size == 1);
      CHECK(planned.candidate_buffer_count == 2);
      if (source != larch::chart_spr_candidate_source::grammar) {
        CHECK(planned.planned_sampled_source_wave_size == 1);
        CHECK(planned.planned_sampled_projection_wave_size == 1);
        CHECK(planned.planned_sampled_source_count_bound > 0);
        CHECK(planned.planned_sampled_destination_bound_per_source > 0);
        CHECK(planned.planned_sampled_source_admitted_peak_bytes > 0);
      }
      if (source != larch::chart_spr_candidate_source::sampled_tree) {
        CHECK(planned.planned_grammar_candidate_wave_size == 1);
        CHECK(planned.planned_grammar_candidate_wave_owned_bytes > 0);
        CHECK(planned.planned_grammar_candidate_admitted_wave_bytes ==
              planned.planned_grammar_candidate_wave_owned_bytes);
      }
      if (use_lazy) {
        CHECK(planned.planned_local_prepared_slots == 1);
        CHECK(planned.planned_local_worker_slots == 1);
      } else {
        CHECK(planned.planned_local_prepared_slots == 4);
        CHECK(planned.planned_local_worker_slots == 4);
      }
      auto exact = larch::run_chart_spr_acceptance_iteration(
          exact_state, exact_options, 0, exact_workspace, exact_scheduler);
      CHECK(exact.candidates_scored > 0);
      CHECK(exact_starts.load(std::memory_order_relaxed) == 1);
      CHECK(exact.candidate_generation.candidate_pipeline_batches_generated >
            0);
      CHECK(exact_state.counters.lazy_local_iteration_envelope_bytes_max ==
            envelope);
      if (source == larch::chart_spr_candidate_source::grammar) {
        // A tight finite envelope may admit a single grammar construction per
        // wave.  In that case the scheduler handoff correctly serializes the
        // next construction behind scoring, so overlap is not an admission
        // invariant.  test_full_slot_cancellation_drains_and_recovers proves
        // non-vacuous producer/consumer overlap independently.
        CHECK(
            exact.candidate_generation.grammar_candidate_admitted_wave_width ==
            selected_grammar_plan.planned_grammar_candidate_wave_size);
        CHECK(exact_projection_allocations.load(std::memory_order_relaxed) ==
              0);
      } else {
        CHECK(exact_projection_allocations.load(std::memory_order_relaxed) > 0);
        CHECK(exact.candidate_generation
                  .sampled_tree_source_admitted_wave_width == 1);
        CHECK(
            exact.candidate_generation.sampled_tree_projection_peak_wave_size ==
            1);
      }
      exact_scheduler.shutdown();

      auto rejected_input = make_fixture();
      auto rejected_options = make_options();
      std::atomic<std::size_t> rejected_starts{0};
      std::atomic<std::size_t> rejected_projection_allocations{0};
      rejected_options.before_candidate_pipeline_start_for_tests = [&] {
        rejected_starts.fetch_add(1, std::memory_order_relaxed);
      };
      rejected_options.enumeration
          .before_sampled_tree_projection_workspace_allocation_for_tests = [&] {
        rejected_projection_allocations.fetch_add(1, std::memory_order_relaxed);
      };
      rejected_options.cache.memory_budget_bytes = envelope - 1;
      auto rejected_state = make_state(rejected_input, rejected_options);
      larch::chart_scheduler rejected_scheduler{
          larch::chart_scheduler_options{.requested_workers = 4}};
      larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
          rejected_workspace;
      bool rejected = false;
      try {
        (void)larch::run_chart_spr_acceptance_iteration(
            rejected_state, rejected_options, 0, rejected_workspace,
            rejected_scheduler);
      } catch (larch::chart_spr_search_detail::
                   chart_spr_lazy_local_budget_error const& error) {
        rejected = true;
        CHECK(error.required_bytes() == envelope);
        CHECK(error.available_bytes() == envelope - 1);
      }
      CHECK(rejected);
      CHECK(rejected_starts.load(std::memory_order_relaxed) == 0);
      CHECK(rejected_projection_allocations.load(std::memory_order_relaxed) ==
            0);
      CHECK(rejected_workspace.candidate_slots.capacity() == 0);
      CHECK(rejected_workspace.pipeline_candidate_slots.capacity() == 0);
      CHECK(rejected_scheduler.metrics().operations == 0);
      rejected_scheduler.shutdown();

      if (source == larch::chart_spr_candidate_source::sampled_tree) {
        // The unified shape contract remains active after its outer E
        // preflight. Simulate an underestimated realized container capacity;
        // it must fail after the reserve hook but before RNG-driven source
        // enumeration, scheduler submission, projection, or gather.
        auto seam_input = make_fixture();
        auto seam_options = make_options();
        seam_options.cache.memory_budget_bytes = envelope;
        seam_options.enumeration
            .sampled_tree_source_wave_actual_capacity_extra_bytes_for_tests =
            planned.planned_sampled_source_admitted_peak_bytes;
        std::atomic<std::size_t> seam_allocations{0};
        std::atomic<std::size_t> seam_sources{0};
        std::atomic<std::size_t> seam_projections{0};
        seam_options.enumeration
            .before_sampled_tree_projection_workspace_allocation_for_tests =
            [&] { seam_allocations.fetch_add(1, std::memory_order_relaxed); };
        seam_options.enumeration.before_sampled_tree_source_enumeration_for_tests =
            [&](std::size_t, std::size_t) {
              seam_sources.fetch_add(1, std::memory_order_relaxed);
            };
        seam_options.enumeration.before_sampled_tree_projection_for_tests =
            [&](std::size_t) {
              seam_projections.fetch_add(1, std::memory_order_relaxed);
            };
        auto seam_state = make_state(seam_input, seam_options);
        larch::chart_scheduler seam_scheduler{
            larch::chart_scheduler_options{.requested_workers = 4}};
        larch::chart_spr_search_detail::
            chart_spr_acceptance_iteration_workspace seam_workspace;
        bool seam_rejected = false;
        try {
          (void)larch::run_chart_spr_acceptance_iteration(
              seam_state, seam_options, 0, seam_workspace, seam_scheduler);
        } catch (larch::sampled_tree_projection_budget_error const& error) {
          seam_rejected = true;
          CHECK(error.required_bytes() > error.budget_bytes());
          CHECK(error.budget_bytes() ==
                planned.planned_sampled_source_admitted_peak_bytes);
        }
        CHECK(seam_rejected);
        CHECK(seam_allocations.load(std::memory_order_relaxed) == 1);
        CHECK(seam_sources.load(std::memory_order_relaxed) == 0);
        CHECK(seam_projections.load(std::memory_order_relaxed) == 0);
        CHECK(seam_scheduler.metrics().operations == 0);
        seam_scheduler.shutdown();
      }
      if (source == larch::chart_spr_candidate_source::grammar) {
        auto seam_input = make_fixture();
        auto seam_options = make_options();
        seam_options.cache.memory_budget_bytes = envelope;
        seam_options.enumeration
            .grammar_candidate_wave_actual_capacity_extra_bytes_for_tests =
            selected_grammar_plan.planned_grammar_candidate_admitted_wave_bytes;
        std::atomic<std::size_t> seam_allocations{0};
        std::atomic<std::size_t> seam_constructions{0};
        seam_options.enumeration
            .before_grammar_candidate_workspace_allocation_for_tests = [&] {
          seam_allocations.fetch_add(1, std::memory_order_relaxed);
        };
        seam_options.enumeration
            .before_grammar_candidate_construction_for_tests =
            [&](std::size_t) {
              seam_constructions.fetch_add(1, std::memory_order_relaxed);
            };
        auto seam_state = make_state(seam_input, seam_options);
        larch::chart_scheduler seam_scheduler{
            larch::chart_scheduler_options{.requested_workers = 4}};
        larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
            seam_workspace;
        bool seam_rejected = false;
        try {
          (void)larch::run_chart_spr_acceptance_iteration(
              seam_state, seam_options, 0, seam_workspace, seam_scheduler);
        } catch (larch::sampled_tree_projection_budget_error const& error) {
          seam_rejected = true;
          CHECK(error.required_bytes() > error.budget_bytes());
          CHECK(error.budget_bytes() ==
                selected_grammar_plan
                    .planned_grammar_candidate_admitted_wave_bytes);
        }
        CHECK(seam_rejected);
        CHECK(seam_allocations.load(std::memory_order_relaxed) == 1);
        CHECK(seam_constructions.load(std::memory_order_relaxed) == 0);
        CHECK(seam_scheduler.metrics().operations == 0);
        seam_scheduler.shutdown();
      }
    }
  }
  std::println("  PASS");
}

void test_dense_partial_final_batch_uses_admitted_tile_shape() {
  std::println("test_dense_partial_final_batch_uses_admitted_tile_shape");
  std::string oversized_diagnostic(
      4 * larch::chart_spr_search_detail::lazy_local_invalid_reason_max_size,
      'x');
  auto make_options = [&] {
    auto options = pipeline_options();
    options.cache.use_lazy_multisite_chart = false;
    options.cache.candidate_batch_size = 4;
    options.max_candidates_per_iteration = 10;
    options.enumeration.max_candidates = 10;
    options.enumeration.source =
        larch::chart_spr_candidate_source::sampled_tree;
    options.enumeration.sampled_tree_count = 2;
    options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
    options.force_dense_invalid_reason_for_tests = oversized_diagnostic;
    return options;
  };
  auto make_state = [](fixture& input,
                       larch::chart_spr_search_options const& options) {
    auto state_options = options;
    state_options.cache.memory_budget_bytes = 0;
    auto state = larch::build_chart_spr_search_state(input.dag, input.grammar,
                                                     state_options);
    CHECK(state.cache_strategy ==
          larch::chart_spr_cache_strategy::all_active_patterns);
    CHECK(state.active_patterns.patterns.patterns.size() > 1);
    return state;
  };

  auto unlimited_input = make_multisite_fixture();
  auto unlimited_options = make_options();
  auto unlimited_state = make_state(unlimited_input, unlimited_options);
  larch::chart_scheduler unlimited_scheduler{
      larch::chart_scheduler_options{.requested_workers = 4}};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      unlimited_workspace;
  auto unlimited = larch::run_chart_spr_acceptance_iteration(
      unlimited_state, unlimited_options, 0, unlimited_workspace,
      unlimited_scheduler);
  CHECK(unlimited.canonical_candidates.size() == 10);
  for (auto const& record : unlimited.canonical_candidates) {
    CHECK(record.invalid_reason == oversized_diagnostic);
  }
  bool retained_unlimited_diagnostic = false;
  for (auto const& result : unlimited_workspace.local_results) {
    if (result.invalid_reason.empty()) continue;
    retained_unlimited_diagnostic = true;
    CHECK(result.invalid_reason == oversized_diagnostic);
    CHECK(result.invalid_reason.capacity() + 1 >
          larch::chart_spr_search_detail::
              lazy_local_invalid_reason_owned_capacity_bound());
  }
  CHECK(retained_unlimited_diagnostic);
  check_scheduler_quiescent(unlimited_scheduler);
  unlimited_scheduler.shutdown();

  auto discovery_input = make_multisite_fixture();
  auto discovery_options = make_options();
  discovery_options.cache.memory_budget_bytes = 1;
  auto discovery_state = make_state(discovery_input, discovery_options);
  larch::chart_scheduler discovery_scheduler{
      larch::chart_scheduler_options{.requested_workers = 4}};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      discovery_workspace;
  std::size_t envelope = 0;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        discovery_state, discovery_options, 0, discovery_workspace,
        discovery_scheduler);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          error) {
    CHECK(error.available_bytes() == 1);
    envelope = error.required_bytes();
  }
  CHECK(envelope > 1);
  CHECK(discovery_workspace.candidate_slots.capacity() == 0);
  CHECK(discovery_workspace.pipeline_candidate_slots.capacity() == 0);
  CHECK(discovery_scheduler.metrics().operations == 0);
  discovery_scheduler.shutdown();

  auto exact_input = make_multisite_fixture();
  auto exact_options = make_options();
  std::atomic<std::size_t> scored_batches_completed{0};
  std::atomic<std::size_t> projections_after_scoring{0};
  exact_options.after_candidate_pipeline_score_batch_for_tests =
      [&](std::size_t) {
        scored_batches_completed.fetch_add(1, std::memory_order_release);
      };
  exact_options.enumeration.before_sampled_tree_projection_for_tests =
      [&](std::size_t) {
        if (scored_batches_completed.load(std::memory_order_acquire) != 0) {
          projections_after_scoring.fetch_add(1, std::memory_order_relaxed);
        }
      };
  exact_options.cache.memory_budget_bytes = envelope;
  auto exact_state = make_state(exact_input, exact_options);
  larch::chart_scheduler exact_scheduler{
      larch::chart_scheduler_options{.requested_workers = 4}};
  auto const rank_limit = larch::chart_spr_rank_buffer_limit(exact_options);
  auto const ranked_reserve_limit =
      rank_limit == larch::chart_spr_rank_unlimited
          ? exact_options.enumeration.max_candidates
          : std::min(exact_options.enumeration.max_candidates, rank_limit);
  auto envelope_options = exact_options.enumeration;
  envelope_options.sampled_tree_source_dag = exact_state.dag;
  auto const planned = larch::chart_spr_search_detail::
      estimate_grammar_spr_finite_iteration_memory_envelope(
          exact_state, exact_options.enumeration.max_candidates, 4,
          ranked_reserve_limit, true, exact_scheduler, 4, &envelope_options, 2,
          true, 1, 1, 1);
  CHECK(planned.planned_required_bytes == envelope);
  CHECK(planned.planned_local_tile_result_slots > 0);
  CHECK(planned.planned_local_weighted_candidate_order_bytes > 0);
  CHECK(planned.planned_local_untiled_concurrent_preparation_peak_bytes > 0);
  CHECK(planned.planned_local_retained_stable_capacity_bytes > 0);
  CHECK(
      planned.planned_generation_wave_with_retained_local_peak_bytes ==
      std::max(
          {planned.planned_sampled_source_active_scratch_bytes +
               planned.planned_sampled_source_scheduler_operation_peak_bytes,
           planned.planned_sampled_projection_active_scratch_bytes +
               planned
                   .planned_sampled_projection_scheduler_operation_peak_bytes,
           planned.planned_grammar_candidate_scheduler_operation_peak_bytes}) +
          planned.planned_local_retained_stable_capacity_bytes);

  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      exact_workspace;
  auto exact = larch::run_chart_spr_acceptance_iteration(
      exact_state, exact_options, 0, exact_workspace, exact_scheduler);
  CHECK(exact.candidates_generated == 10);
  CHECK(exact.candidates_scored == 10);
  CHECK(exact.candidate_score_failures == 10);
  CHECK(exact.candidate_generation.candidate_pipeline_batches_generated == 3);
  CHECK(exact.candidate_generation.candidate_pipeline_batches_scored == 3);
  CHECK(exact.candidate_generation.sampled_tree_projection_peak_wave_size == 1);
  CHECK(projections_after_scoring.load(std::memory_order_relaxed) > 0);
  CHECK(exact_state.counters.scheduler_axes.local_score_candidate_patterns
            .operations > 0);
  CHECK(exact_state.counters.lazy_local_pre_submit_budget_failures == 0);
  CHECK(exact.canonical_candidates.size() == 10);
  for (auto const& record : exact.canonical_candidates) {
    CHECK(record.invalid_reason ==
          larch::chart_spr_search_detail::lazy_local_oversized_invalid_reason);
    CHECK(record.invalid_reason.capacity() + 1 <=
          larch::chart_spr_search_detail::
              lazy_local_invalid_reason_owned_capacity_bound());
  }
  CHECK(exact_workspace.local_score.operation_boundary_clean());
  check_scheduler_quiescent(exact_scheduler);
  exact_scheduler.shutdown();
  std::println("  PASS");
}

}  // namespace

int main() {
  test_worker_source_seed_semantics();
  test_exact_hybrid_worker_seed_semantics();
  test_full_slot_cancellation_drains_and_recovers();
  test_early_acceptance_discards_speculation();
  test_stale_stamp_never_scores();
  test_error_precedence_drain_and_recovery();
  test_generation_error_drains();
  test_finite_sampled_source_adaptive_planning();
  test_finite_admission_exact_boundary();
  test_dense_partial_final_batch_uses_admitted_tile_shape();
  std::println("chart_spr_pipeline_test PASS");
  return 0;
}

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

larch::chart_spr_search_result run_semantic_case(
    larch::chart_spr_candidate_source source, std::uint32_t seed,
    std::size_t workers) {
  auto input = make_fixture(true);
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

void test_finite_admission_exact_boundary() {
  std::println("test_finite_admission_exact_boundary");
  auto exact_input = make_fixture();
  auto exact_options = pipeline_options();
  std::atomic<std::size_t> exact_starts{0};
  exact_options.before_candidate_pipeline_start_for_tests = [&] {
    exact_starts.fetch_add(1, std::memory_order_relaxed);
  };
  auto exact_state =
      larch::build_chart_spr_search_state(exact_input.dag, exact_input.grammar);
  larch::chart_scheduler exact_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  auto const estimate = larch::chart_spr_search_detail::
      estimate_chart_spr_candidate_pipeline_memory(exact_state, exact_options,
                                                   1, exact_scheduler);
  CHECK(estimate.safely_bounded);
  CHECK(estimate.published_state_bytes > 0);
  CHECK(estimate.double_buffer_bytes > 0);
  CHECK(estimate.simultaneous_local_scoring_bytes > 0);
  CHECK(estimate.scheduler_resident_bytes > 0);
  CHECK(estimate.cancellation_error_and_handoff_bytes > 0);
  exact_options.cache.memory_budget_bytes = estimate.required_peak_bytes;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      exact_workspace;
  auto exact = larch::run_chart_spr_acceptance_iteration(
      exact_state, exact_options, 0, exact_workspace, exact_scheduler);
  CHECK(exact.candidates_scored > 0);
  CHECK(exact_starts.load(std::memory_order_relaxed) == 1);
  exact_scheduler.shutdown();

  auto rejected_input = make_fixture();
  auto rejected_options = pipeline_options();
  std::atomic<std::size_t> rejected_starts{0};
  rejected_options.before_candidate_pipeline_start_for_tests = [&] {
    rejected_starts.fetch_add(1, std::memory_order_relaxed);
  };
  rejected_options.cache.memory_budget_bytes = estimate.required_peak_bytes - 1;
  auto rejected_state = larch::build_chart_spr_search_state(
      rejected_input.dag, rejected_input.grammar);
  larch::chart_scheduler rejected_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      rejected_workspace;
  bool rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        rejected_state, rejected_options, 0, rejected_workspace,
        rejected_scheduler);
  } catch (larch::chart_spr_exact_state_budget_error const& error) {
    rejected = true;
    CHECK(error.required_bytes() == estimate.required_peak_bytes);
  }
  CHECK(rejected);
  CHECK(rejected_starts.load(std::memory_order_relaxed) == 0);
  CHECK(rejected_workspace.candidate_slots.capacity() == 0);
  CHECK(rejected_workspace.pipeline_candidate_slots.capacity() == 0);
  CHECK(rejected_scheduler.metrics().operations == 0);
  rejected_scheduler.shutdown();
  std::println("  PASS");
}

}  // namespace

int main() {
  test_worker_source_seed_semantics();
  test_full_slot_cancellation_drains_and_recovers();
  test_early_acceptance_discards_speculation();
  test_stale_stamp_never_scores();
  test_error_precedence_drain_and_recovery();
  test_generation_error_drains();
  test_finite_admission_exact_boundary();
  std::println("chart_spr_pipeline_test PASS");
  return 0;
}

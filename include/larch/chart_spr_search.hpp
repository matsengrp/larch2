#pragma once

#include <larch/chart_spr.hpp>
#include <larch/chart_scheduler.hpp>
#include <larch/chart_spr_semantic_report.hpp>
#include <larch/lazy_chart.hpp>
#include <larch/lazy_key_grouping.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

class overlay_chain;

struct chart_spr_scheduler_axis_metrics {
  std::uint64_t operations = 0;
  std::uint64_t parallel_operations = 0;
  std::uint64_t items = 0;
  std::uint64_t ranges = 0;
  std::uint64_t worker_tasks = 0;
  std::size_t active_worker_high_water = 0;
  std::size_t minimum_effective_grain = 0;
  std::size_t maximum_effective_grain = 0;

  bool operator==(chart_spr_scheduler_axis_metrics const&) const = default;
};

struct chart_spr_scheduler_axis_counters {
  chart_spr_scheduler_axis_metrics initial_chart_patterns;
  chart_spr_scheduler_axis_metrics candidate_generation;
  chart_spr_scheduler_axis_metrics exact_setup_patterns;
  chart_spr_scheduler_axis_metrics exact_frontier_clades;
  chart_spr_scheduler_axis_metrics exact_candidates;
  chart_spr_scheduler_axis_metrics lazy_inside_clades;
  chart_spr_scheduler_axis_metrics lazy_outside_clades;
  chart_spr_scheduler_axis_metrics inside_cache_patterns;
  chart_spr_scheduler_axis_metrics outside_cache_patterns;
  chart_spr_scheduler_axis_metrics fixed_topology_patterns;
  chart_spr_scheduler_axis_metrics local_score_candidates;
  chart_spr_scheduler_axis_metrics local_score_candidate_patterns;
  chart_spr_scheduler_axis_metrics other;

  bool operator==(chart_spr_scheduler_axis_counters const&) const = default;
};

inline void record_chart_spr_scheduler_axis_run(
    chart_spr_scheduler_axis_metrics& axis,
    chart_scheduler_run_summary const& run) {
  ++axis.operations;
  if (run.used_parallel_workers()) ++axis.parallel_operations;
  axis.items += run.item_count;
  axis.ranges += run.range_count;
  axis.worker_tasks += run.worker_tasks_submitted;
  axis.active_worker_high_water =
      std::max(axis.active_worker_high_water, run.active_workers);
  // The scheduler deliberately excludes empty operations from its lifetime
  // grain extrema.  Axis accounting must use the identical convention so the
  // two surfaces reconcile exactly.
  if (run.range_count != 0) {
    if (axis.minimum_effective_grain == 0) {
      axis.minimum_effective_grain = run.effective_grain;
    } else {
      axis.minimum_effective_grain =
          std::min(axis.minimum_effective_grain, run.effective_grain);
    }
    axis.maximum_effective_grain =
        std::max(axis.maximum_effective_grain, run.effective_grain);
  }
}

// A submission failure has no returned run summary, but the scheduler has
// already joined every accepted runner and published the complete operation
// delta. Preserve that delta on the semantic axis before propagating the
// infrastructure error so a recovered search still reconciles exactly with
// the search-lifetime scheduler.
inline void record_chart_spr_scheduler_axis_failed_run(
    chart_spr_scheduler_axis_metrics& axis,
    chart_indexed_range_plan const& plan,
    chart_scheduler_metrics const& before,
    chart_scheduler_metrics const& after) {
  if (after.operations < before.operations ||
      after.parallel_operations < before.parallel_operations ||
      after.ranges_created < before.ranges_created ||
      after.tasks_submitted < before.tasks_submitted) {
    throw std::logic_error(
        "chart SPR scheduler-axis failure accounting regressed");
  }
  auto const operation_delta = after.operations - before.operations;
  if (operation_delta == 0) {
    // Rejection before operation publication (for example use after shutdown
    // or unrelated concurrent use) has no semantic-axis work to classify.
    return;
  }
  if (operation_delta != 1) {
    throw std::logic_error(
        "chart SPR scheduler-axis failure accounting saw multiple operations");
  }
  axis.operations += operation_delta;
  axis.parallel_operations +=
      after.parallel_operations - before.parallel_operations;
  axis.items += plan.item_count;
  axis.ranges += after.ranges_created - before.ranges_created;
  axis.worker_tasks += after.tasks_submitted - before.tasks_submitted;
  axis.active_worker_high_water =
      std::max(axis.active_worker_high_water, after.last_active_workers);
  if (plan.range_count != 0) {
    if (axis.minimum_effective_grain == 0) {
      axis.minimum_effective_grain = plan.effective_grain;
    } else {
      axis.minimum_effective_grain =
          std::min(axis.minimum_effective_grain, plan.effective_grain);
    }
    axis.maximum_effective_grain =
        std::max(axis.maximum_effective_grain, plan.effective_grain);
  }
}

inline chart_indexed_range_options chart_spr_phase4_pattern_range_options(
    std::size_t item_count, std::size_t resolved_workers) noexcept {
  if (resolved_workers <= 1) {
    return chart_indexed_range_options{
        .minimum_grain = std::max<std::size_t>(1, item_count),
        .target_ranges_per_worker = 1,
    };
  }
  return chart_indexed_range_options{
      .minimum_grain = 1,
      .target_ranges_per_worker = 4,
  };
}

// Accepted-candidate rebuild failures are ordinary rejections. If such a
// rebuild throws after completing scheduler work, retain the completed axes
// so the eventual successful search report still reconciles with the one
// search-lifetime scheduler. Normal returns publish through the returned
// state's counters and therefore leave this destination untouched.
class chart_spr_failed_scheduler_axes_publisher {
 public:
  chart_spr_failed_scheduler_axes_publisher(
      chart_spr_scheduler_axis_counters const& source,
      chart_spr_scheduler_axis_counters* destination) noexcept
      : source_(&source),
        destination_(destination),
        uncaught_on_entry_(std::uncaught_exceptions()) {}

  ~chart_spr_failed_scheduler_axes_publisher() noexcept {
    if (destination_ != nullptr &&
        std::uncaught_exceptions() > uncaught_on_entry_) {
      *destination_ = *source_;
    }
  }

 private:
  chart_spr_scheduler_axis_counters const* source_;
  chart_spr_scheduler_axis_counters* destination_;
  int uncaught_on_entry_;
};

class chart_spr_scheduler_run_axis_publisher {
 public:
  chart_spr_scheduler_run_axis_publisher(
      chart_spr_scheduler_axis_metrics& axis,
      std::vector<chart_scheduler_run_summary> const& runs) noexcept
      : axis_(&axis), runs_(&runs) {}

  ~chart_spr_scheduler_run_axis_publisher() noexcept {
    for (auto const& run : *runs_) {
      record_chart_spr_scheduler_axis_run(*axis_, run);
    }
  }

 private:
  chart_spr_scheduler_axis_metrics* axis_;
  std::vector<chart_scheduler_run_summary> const* runs_;
};

inline void add_chart_spr_scheduler_axis_metrics(
    chart_spr_scheduler_axis_metrics& dst,
    chart_spr_scheduler_axis_metrics const& src) {
  dst.operations += src.operations;
  dst.parallel_operations += src.parallel_operations;
  dst.items += src.items;
  dst.ranges += src.ranges;
  dst.worker_tasks += src.worker_tasks;
  dst.active_worker_high_water =
      std::max(dst.active_worker_high_water, src.active_worker_high_water);
  if (src.minimum_effective_grain != 0) {
    dst.minimum_effective_grain = dst.minimum_effective_grain == 0
                                      ? src.minimum_effective_grain
                                      : std::min(dst.minimum_effective_grain,
                                                 src.minimum_effective_grain);
  }
  dst.maximum_effective_grain =
      std::max(dst.maximum_effective_grain, src.maximum_effective_grain);
}

inline void add_chart_spr_scheduler_axis_counters(
    chart_spr_scheduler_axis_counters& dst,
    chart_spr_scheduler_axis_counters const& src) {
  add_chart_spr_scheduler_axis_metrics(dst.initial_chart_patterns,
                                       src.initial_chart_patterns);
  add_chart_spr_scheduler_axis_metrics(dst.candidate_generation,
                                       src.candidate_generation);
  add_chart_spr_scheduler_axis_metrics(dst.exact_setup_patterns,
                                       src.exact_setup_patterns);
  add_chart_spr_scheduler_axis_metrics(dst.exact_frontier_clades,
                                       src.exact_frontier_clades);
  add_chart_spr_scheduler_axis_metrics(dst.exact_candidates,
                                       src.exact_candidates);
  add_chart_spr_scheduler_axis_metrics(dst.lazy_inside_clades,
                                       src.lazy_inside_clades);
  add_chart_spr_scheduler_axis_metrics(dst.lazy_outside_clades,
                                       src.lazy_outside_clades);
  add_chart_spr_scheduler_axis_metrics(dst.inside_cache_patterns,
                                       src.inside_cache_patterns);
  add_chart_spr_scheduler_axis_metrics(dst.outside_cache_patterns,
                                       src.outside_cache_patterns);
  add_chart_spr_scheduler_axis_metrics(dst.fixed_topology_patterns,
                                       src.fixed_topology_patterns);
  add_chart_spr_scheduler_axis_metrics(dst.local_score_candidates,
                                       src.local_score_candidates);
  add_chart_spr_scheduler_axis_metrics(dst.local_score_candidate_patterns,
                                       src.local_score_candidate_patterns);
  add_chart_spr_scheduler_axis_metrics(dst.other, src.other);
}

inline std::uint64_t chart_spr_scheduler_axis_operation_count(
    chart_spr_scheduler_axis_counters const& axes) noexcept {
  return axes.initial_chart_patterns.operations +
         axes.candidate_generation.operations +
         axes.exact_setup_patterns.operations +
         axes.exact_frontier_clades.operations +
         axes.exact_candidates.operations + axes.lazy_inside_clades.operations +
         axes.lazy_outside_clades.operations +
         axes.inside_cache_patterns.operations +
         axes.outside_cache_patterns.operations +
         axes.fixed_topology_patterns.operations +
         axes.local_score_candidates.operations +
         axes.local_score_candidate_patterns.operations + axes.other.operations;
}

// Phase-0 instrumentation for the DAG-native chart-SPR search path.  These
// counters deliberately separate accepted-state rebuilds from diagnostic/oracle
// overlay materializations and exact-verification materializations so benchmark
// output can detect accidental "full rebuild per rejected candidate" behavior.
struct chart_spr_search_counters {
  // Phase-4 scheduler work is classified by semantic axis. The sum of axis
  // operation/range/task counts must reconcile with the search-lifetime
  // scheduler totals; no parallel work is hidden behind a generic bucket.
  chart_spr_scheduler_axis_counters scheduler_axes;
  std::size_t grammar_rebuilds = 0;
  std::size_t pattern_rebuilds = 0;
  std::size_t base_chart_cache_rebuilds = 0;
  // Phase-1 immutable structural-plan accounting.  Validation and ordering
  // happen at the checked generation boundary; the candidate-pattern region
  // must leave its three scoped counters at zero.
  std::size_t chart_execution_plan_builds = 0;
  std::size_t chart_execution_plan_cache_hits = 0;
  std::size_t candidate_execution_plan_builds = 0;
  std::size_t candidate_execution_plan_cache_hits = 0;
  std::size_t full_grammar_validations = 0;
  std::size_t production_index_validations = 0;
  std::size_t production_partition_validations = 0;
  // Candidate/chain temp productions validated before dense reachability
  // filtering.  Kept distinct from output-plan and candidate-pattern work.
  std::size_t dynamic_overlay_payload_partition_validations = 0;
  std::size_t candidate_partition_validations = 0;
  std::size_t clade_order_sorts = 0;
  std::size_t production_descriptors_compiled = 0;
  std::size_t plan_mismatch_rejections = 0;
  std::size_t candidate_pattern_full_grammar_validations = 0;
  std::size_t candidate_pattern_partition_validations = 0;
  std::size_t candidate_pattern_clade_order_sorts = 0;
  // Total overlay materializations plus reason-coded splits.  Exact
  // verification materializations are expensive, but they are not accepted-
  // state sidecar rebuilds and must be reported separately.
  std::size_t full_overlay_materializations = 0;
  std::size_t overlay_materializations_for_oracle = 0;
  // Legacy Phase-1 bridge path counter.  The Phase-3 overlay-delta scorer
  // should leave this at zero; keep the field so diagnostics can catch
  // accidental regressions to dense overlay materialization in local scoring.
  std::size_t overlay_materializations_for_local_scoring_bridge = 0;
  std::size_t overlay_materializations_for_exact_verification = 0;
  std::size_t overlay_materializations_for_accept_materialization = 0;
  // Phase 5 grammar-valued final compaction: one dense overlay-chain
  // materialization at the end of a local-commit run, counted separately from
  // per-accept materialization and per-candidate exact verification.
  std::size_t overlay_materializations_for_final_compaction = 0;
  // Wall time spent in the materialization call spans made by the production
  // search loop.  The three buckets are disjoint and their sum is reported as
  // `materialization_ms`; a call span is charged even when the call throws and
  // the search catches that failure to report an invalid candidate/rejected
  // accept.  Diagnostic materialization performed only by the local-scoring
  // oracle is intentionally excluded.  The accepted-update bucket deliberately
  // measures the whole accepted Option A/B materializer call (which also
  // merges, validates, and audits the resulting grammar), or just the local-
  // commit tip materialization call in local-commit mode.
  double materialization_exact_verification_ms = 0.0;
  double materialization_accepted_update_ms = 0.0;
  double materialization_final_compaction_ms = 0.0;
  std::size_t sidecar_rebuilds_after_accept = 0;
  std::size_t full_composite_rebuilds = 0;
  std::size_t local_candidate_scores = 0;
  std::size_t local_rows_recomputed = 0;
  // Candidate-local production rows completed by the specialized
  // all-four-parent-state unit-Fitch recurrence. The provider is read once per
  // child, and this advances only after all four contributions are available.
  std::size_t local_unit_fitch_fast_path_productions_scored = 0;
  // Allocation-sensitive local-kernel contract.  Every dense
  // candidate-pattern visit borrows the resident pattern's immutable leaf
  // states.  The owned-copy counter is a regression sentinel: production
  // local scoring must leave it at zero, while exact candidate x pattern
  // view-use assertions make that zero non-vacuous.  Row scratch records only
  // explicit capacity growth, not ordinary resize/fill resets within retained
  // capacity.
  std::size_t local_leaf_state_view_uses = 0;
  std::size_t local_leaf_state_owned_copies = 0;
  std::size_t local_row_scratch_capacity_growths = 0;
  // Non-lazy local-commit scoring reads the authoritative persistent cache
  // through an immutable dense-tip view. This makes the zero-copy projection
  // contract non-vacuous without counting each child-row lookup.
  std::size_t local_commit_inside_row_view_pattern_visits = 0;
  // Cross-cutting WRIC arity counter: non-binary production rows scored by dense
  // chart builds, overlay-delta local rows, and persistent cache recomputes.
  std::size_t multifurcation_productions_scored = 0;
  std::size_t local_score_parallel_batches = 0;
  std::size_t local_score_worker_tasks = 0;
  // Lazy resident-cache scoring admits a bounded stable-prefix wave of
  // candidate tasks after their descriptors and grouping scratch have been
  // prepared on the coordinator.  These diagnostics make the resulting
  // concurrency and unified-memory decisions observable.
  std::size_t lazy_local_admission_waves = 0;
  std::size_t lazy_local_parallel_waves = 0;
  std::size_t lazy_local_memory_limited_waves = 0;
  std::size_t lazy_local_admitted_concurrency_max = 0;
  std::size_t lazy_local_prepared_tasks = 0;
  std::size_t lazy_local_reused_prepared_tasks = 0;
  std::size_t lazy_local_pre_submit_budget_failures = 0;
  std::size_t lazy_local_peak_admitted_bytes = 0;
  std::size_t lazy_local_peak_projected_resident_bytes = 0;
  std::size_t lazy_local_preparation_peak_bytes = 0;
  std::size_t lazy_local_result_output_resident_bytes_max = 0;
  std::size_t lazy_local_retained_exact_trim_bytes_max = 0;
  std::size_t lazy_local_canonical_exact_evidence_resident_bytes_max = 0;
  std::size_t
      lazy_local_canonical_exact_evidence_construction_peak_bytes_max = 0;
  // Fixed-size reservation held for every simultaneously admitted worker so
  // scheduler exception transport cannot escape the finite wave envelope.
  std::size_t lazy_local_runtime_transient_reservation_bytes_max = 0;
  std::size_t lazy_local_iteration_envelope_bytes_max = 0;
  std::size_t lazy_local_iteration_generation_phase_bytes_max = 0;
  std::size_t lazy_local_iteration_evidence_phase_bytes_max = 0;
  std::size_t lazy_local_ranked_candidate_exact_evidence_bytes_max = 0;
  std::size_t lazy_local_iteration_task_stable_bytes_max = 0;
  std::size_t lazy_local_iteration_task_preparation_peak_bytes_max = 0;
  std::size_t candidate_batches_scored = 0;
  std::size_t pattern_batch_cache_builds = 0;
  // Dense single-site inside charts built by the state's ordinary cache
  // representation.  Exact-setup and persistent-inside-cache recurrence
  // owners have distinct counters below, so their sum can enforce the
  // one-build contract without double counting.  Lazy structural classes and
  // resident chart copies do not advance this field.
  std::size_t initial_state_inside_charts_built = 0;
  // Cold construction of the persistent local-commit inside cache.  A
  // resident construction must report zero builds and one consume per active
  // pattern; these counters are populated by the substrate wiring.
  std::size_t inside_cache_inside_charts_built = 0;
  std::size_t inside_cache_resident_inside_charts_consumed = 0;
  // Phase-2B finalized exact-setup accounting.  These are logical work counts
  // copied from each successful multisite trim.  In particular, an all-active
  // search-state trim must consume the resident single-site charts and leave
  // `exact_setup_inside_charts_built` at zero; pattern-batched cold trims make
  // their unavoidable rebuilds explicit.  The lazy multisite representation
  // has its own counter because it does not construct a multisite_exact_setup.
  std::size_t exact_setup_builds = 0;
  std::size_t exact_setup_inside_charts_built = 0;
  std::size_t exact_setup_resident_inside_charts_consumed = 0;
  std::size_t exact_setup_active_leaf_state_vectors_copied = 0;
  std::size_t exact_setup_active_leaf_states_copied = 0;
  std::size_t exact_setup_outside_boundary_charts_built = 0;
  std::size_t exact_setup_upper_bound_topologies_generated = 0;
  std::size_t exact_setup_upper_bound_topologies_unique = 0;
  std::size_t exact_setup_frontier_passes = 0;
  std::size_t exact_bnb_levels = 0;
  std::size_t exact_bnb_clades = 0;
  std::size_t exact_bnb_product_combinations = 0;
  std::size_t exact_bnb_frontier_entries = 0;
  double exact_bnb_ms = 0.0;
  std::size_t exact_trim_lazy_chart_uses = 0;
  // Phase-2B local-commit cold outside-cache construction.  The production
  // path must build the inside cache once, then report zero additional inside
  // builds and one inside reuse/outside build per active pattern here.
  std::size_t outside_cache_inside_charts_built = 0;
  std::size_t outside_cache_inside_charts_reused = 0;
  std::size_t outside_cache_outside_charts_built = 0;
  std::size_t exact_verifications = 0;
  std::size_t accepted_exact_trims_reused = 0;
  std::size_t accepted_exact_trim_reuse_rejections = 0;
  // Phase-6 stable-rank exact-candidate admission. Candidate-parallel waves
  // disable every inner scheduler axis; singleton waves may instead use the
  // existing exact-frontier or fixed-topology-pattern axis. These counters
  // make that exclusivity and the unified-memory admission decision visible.
  std::size_t exact_candidate_admission_batches = 0;
  std::size_t exact_candidate_parallel_batches = 0;
  std::size_t exact_candidate_inner_parallel_batches = 0;
  std::size_t exact_candidate_memory_limited_batches = 0;
  std::size_t exact_candidate_peak_admitted_bytes = 0;
  std::size_t exact_candidate_peak_projected_resident_bytes = 0;
  double exact_candidate_queued_for_memory_ms = 0.0;
  std::size_t accepted_moves = 0;
  std::size_t candidate_accepts_attempted = 0;
  std::size_t rejected_moves = 0;
  std::size_t post_materialization_rejections = 0;
  std::size_t skipped_invariant_sites = 0;

  // Candidate-generation/path-explosion counters.
  std::size_t candidate_source_productions_considered = 0;
  std::size_t upward_path_iterator_steps = 0;
  std::size_t upward_paths_completed = 0;
  std::size_t path_pairs_considered = 0;
  std::size_t candidates_constructed = 0;
  std::size_t candidates_pruned_before_construction = 0;
  std::size_t candidates_pruned_after_construction = 0;
  std::size_t candidates_generated_after_dedup = 0;
  std::size_t candidates_pruned_root_or_trivial = 0;
  std::size_t candidates_pruned_moved_size = 0;
  std::size_t candidates_pruned_target_size = 0;
  std::size_t candidates_pruned_overlap = 0;
  std::size_t candidates_pruned_affected_estimate = 0;
  std::size_t candidates_pruned_immediate_reversal = 0;
  std::size_t candidates_pruned_duplicate = 0;
  std::size_t candidates_pruned_invalid = 0;
  std::size_t spr_multifurcation_moves_generated = 0;
  std::size_t candidate_cap_cutoffs = 0;
  std::size_t path_budget_cutoffs = 0;
  std::size_t candidate_pipeline_batches_generated = 0;
  std::size_t candidate_pipeline_batches_scored = 0;
  std::size_t candidate_pipeline_serial_overlap_batches = 0;
  std::size_t candidate_pipeline_scheduler_projection_overlap_batches = 0;
  std::size_t candidate_pipeline_producer_stalls = 0;
  std::size_t candidate_pipeline_consumer_stalls = 0;
  std::uint64_t candidate_pipeline_producer_stall_nanoseconds = 0;
  std::uint64_t candidate_pipeline_consumer_stall_nanoseconds = 0;
  std::size_t candidate_pipeline_cancellations = 0;
  std::size_t candidate_pipeline_stale_batches_discarded = 0;
  std::size_t candidate_pipeline_stale_candidates_discarded = 0;
  std::size_t candidate_pipeline_state_epoch_rejections = 0;
  std::size_t candidate_pipeline_generation_errors = 0;
  std::size_t candidate_pipeline_estimated_peak_bytes = 0;

  // Overlay validation/reachability counters.
  std::size_t overlay_reachability_validations = 0;
  std::size_t reachable_clades_traversed = 0;
  std::size_t reachable_productions_traversed = 0;
  std::size_t reachable_temp_clades_traversed = 0;
  std::size_t reachable_temp_productions_traversed = 0;
  std::size_t reachability_full_grammar_like_passes = 0;

  // Phase 4 (Work item 1+3 integration) local-commit counters.  Accepted
  // SPR moves commit to the overlay chain + persistent inside/outside caches
  // instead of dense-materializing per accept.  These make the cross-cutting
  // counter contract visible: a regression to "full chart rebuild per accept"
  // shows up as `inside_rows_recomputed_on_commit` /
  // `outside_rows_recomputed_on_commit` recovers the whole grammar, while a
  // regression to "dense materialize per accept" shows up as a nonzero
  // `overlay_materializations_for_accept_materialization` (the conservative
  // per-accept counter, which the Phase-4 local-commit path leaves at zero).
  //
  // Local commits performed (only exact-gate accepts may commit locally).
  std::size_t local_commit_accepted_moves = 0;
  // Labelled, counted skips of accepted candidates whose tombstones do not
  // resolve to frozen-base productions (the Phase-4 tombstone-scope gate,
  // resolution (a) in the plan).  Distinct from an accept and never a silent
  // no-op.
  std::size_t local_commit_tombstone_scope_skips = 0;
  // (pattern, clade) inside / outside row recomputations across all local
  // commits, mirrored from the persistent caches so the contract is visible
  // in the search counters / summary without exposing the cache objects.
  std::size_t inside_rows_recomputed_on_commit = 0;
  std::size_t outside_rows_recomputed_on_commit = 0;
  // Lazy local-commit recomputations count class rows, not dense
  // (pattern, clade) rows.  They are kept separate so a lazy run can report
  // the affected persistent-cache rows and the smaller lazy work surface.
  std::size_t lazy_inside_rows_computed = 0;
  std::size_t lazy_outside_rows_computed = 0;
  std::size_t lazy_patterns_merged_max = 0;
  std::size_t lazy_remerge_collisions = 0;
  std::size_t lazy_inside_rows_recomputed_on_commit = 0;
  std::size_t lazy_outside_rows_recomputed_on_commit = 0;
  std::size_t lazy_incremental_rows_recomputed = 0;
  std::size_t lazy_structural_class_count_max = 0;
  // Phase-7 finite state-build admission. The inside/outside maxima expose the
  // actual scratch-slot concurrency selected under the unified live envelope;
  // peaks include retained state, resident chart/output, coordinator arrays,
  // and measured scratch capacities.
  std::size_t lazy_chart_memory_budget_bytes = 0;
  std::size_t lazy_chart_inside_max_admitted_slots = 0;
  std::size_t lazy_chart_outside_max_admitted_slots = 0;
  std::size_t lazy_chart_inside_admission_waves = 0;
  std::size_t lazy_chart_outside_admission_waves = 0;
  std::size_t lazy_chart_inside_memory_limited_levels = 0;
  std::size_t lazy_chart_outside_memory_limited_levels = 0;
  std::size_t lazy_chart_inside_reused_slot_waves = 0;
  std::size_t lazy_chart_outside_reused_slot_waves = 0;
  std::size_t lazy_chart_inside_workspace_evictions = 0;
  std::size_t lazy_chart_outside_workspace_evictions = 0;
  std::size_t lazy_chart_preflight_peak_bytes = 0;
  std::size_t lazy_chart_actual_peak_bytes = 0;
  std::size_t lazy_chart_pre_submit_rejections = 0;
  // Automatic-policy probes are serial, bounded, and distinct from the
  // scheduled publication builders above. Frozen accepted-state rebuilds
  // reuse only the policy decision, never stale memory measurements.
  std::size_t lazy_policy_pilot_runs = 0;
  std::size_t lazy_policy_frozen_reuses = 0;
  // Number of times the Phase-4 self-check two-chart oracle ran after a local
  // commit (only when verify_local_commit_two_chart_oracle_for_tests is set).
  std::size_t local_commit_two_chart_oracle_runs = 0;
  // Number of times the tip grammar view was refreshed from the chain for
  // candidate generation.  This is a grammar-only materialization (no chart
  // rescoring -- the charts come from the persistent caches); it is reported
  // separately so it is never hidden behind `full_overlay_materializations`.
  // Eliminating it entirely is the Phase 6/7 (Option C splice) scope.
  std::size_t local_commit_tip_grammar_refreshes = 0;

  // Phase 8 selected-topology row cache used by the local-commit
  // fixed_topology_exact verifier.  Misses compute one structural selected
  // subtree for all active patterns; hits mean an unchanged selected subtree
  // was read from the persistent topology cache.  These counters are reported
  // separately from the persistent-cache verification counters so selected-
  // topology work cannot hide behind the "cache path" label.
  std::size_t fixed_topology_selected_cache_hits = 0;
  std::size_t fixed_topology_selected_cache_misses = 0;
  std::size_t fixed_topology_selected_rows_computed = 0;
  std::size_t selected_topology_class_rows_computed = 0;
  std::size_t selected_topology_multifurcation_rows = 0;
  // Persistent-cache fixed_topology_exact verifier diagnostics.  A nonzero
  // fallback count means the production path could not serve the selected
  // topology from the persistent fixed-topology cache and had to use the
  // conservative from-scratch direct scorer; Phase 8 tests assert this remains
  // zero on the fixture matrix so fallback frequency cannot be silent.
  std::size_t fixed_topology_persistent_cache_verifications = 0;
  std::size_t fixed_topology_persistent_cache_fallbacks = 0;
  std::size_t fixed_topology_persistent_cache_oracle_mismatches = 0;
  // Phase-8 production per-pattern gate.  The persistent-cache verifier always
  // cross-checks its selected-topology score against the independent direct
  // overlay selected-topology scorer (no materialization); this counts the
  // candidates where that gate found a per-pattern mismatch and the cache
  // value could not be labelled exact from the cache path alone.  Distinct
  // from the (test-only) materialized oracle mismatch counter so a regression
  // in the production gate is visible on its own.
  std::size_t
      fixed_topology_persistent_cache_direct_oracle_mismatches = 0;
  // Phase-8 affected-row participation of the persistent inside cache.  Base
  // clade refs consulted in the selected-topology recurrence are cross-checked
  // against icache; matching rows are reused from the persistent cache
  // (unaffected), mismatching rows and temp clades are recomputed (affected).
  // This makes the delta "from persistent inside+outside cache, restricted to
  // affected rows" observable: a regression to "recompute everything" is
  // visible as zero reused rows.
  std::size_t fixed_topology_icache_rows_reused = 0;
  std::size_t fixed_topology_icache_rows_recomputed_affected = 0;
  // Phase-8 chain-objective gate (Work item 1 exactness contract).  Counts
  // accepted candidates whose selected before-topology score did not equal the
  // recorded chain objective (the previous accepted after-topology).  Such a
  // candidate is still gated against the chain objective, never against its
  // own before-topology score; a nonzero count is a diagnostic that the
  // candidate's before certificate is not the chain tip's topology.
  std::size_t fixed_topology_chain_objective_before_mismatches = 0;
  // Test-only diagnostics for the Phase-8 independent-s_M corruption hook.
  // The real-witness counter proves the hook exercised the actual bug class;
  // the perturbation counter is retained as a regression guard and should stay
  // zero (the hook must not rely on arbitrary score changes).
  std::size_t fixed_topology_independent_sm_bug_witnesses_for_tests = 0;
  std::size_t fixed_topology_independent_sm_bug_perturbations_for_tests = 0;

  // Phase 9 (Work item 4a, technique 2) transient chain extension for
  // grammar-exact verification.  The exact_multisite gate verifies an
  // unaccepted candidate by transiently extending the overlay chain in
  // reader-local scratch storage (never mutating the shared cache, bypassing
  // the Phase 4 commit barrier), reading the exact frontier on the extended
  // grammar, and discarding.  It is counted here, never folded into
  // `full_overlay_materializations` (a regression to "dense materialize per
  // candidate" must be visible, not hidden behind a renamed counter).
  //
  // Scratch caches are constructed only for the test/diagnostic two-chart
  // oracle and are counted separately below; production B&B rebuilds its exact
  // setup from the materialized extended grammar.
  std::size_t transient_chain_extensions_for_verification = 0;
  // Scratch inside/outside caches are useful only for the opt-in two-chart
  // diagnostic.  Production transient verification materializes the extended
  // chain and runs exact B&B without copying or advancing those unused caches.
  std::size_t transient_chain_diagnostic_cache_extensions = 0;
  // Test-only diagnostic: transient extensions that fell back to the
  // from-scratch cold path because the per-candidate oracle found a mismatch.
  // A nonzero count means the transient result could not be trusted from the
  // transient path alone; the cold-path result is authoritative.  Should stay
  // zero on the fixture matrix (a nonzero count without a forced corruption
  // hook is a correctness regression).
  std::size_t transient_chain_extension_fallbacks = 0;
  // Test-only diagnostic: transient extensions whose per-candidate
  // from-scratch oracle (both charts + exact optimum) disagreed with the
  // transient result.  Should stay zero on the fixture matrix.
  std::size_t transient_chain_extension_oracle_mismatches = 0;
  // Test-only diagnostic: number of (pattern, clade) scratch rows checked by
  // the per-candidate two-chart oracle when the oracle flag is on.  Reported
  // separately so a regression to "no oracle" is visible.
  std::size_t transient_chain_extension_oracle_rows_checked_for_tests = 0;
};

// All mutable state owned by one exact-verification attempt. The search
// coordinator supplies a private counter sink for candidate-parallel work and
// publishes the immutable old exact trim before launching readers. A nullable
// inner scheduler selects exactly one parallel axis per admission wave.
struct chart_spr_exact_verification_context {
  chart_spr_search_counters& counters;
  multisite_trim_result const* published_old_trim = nullptr;
  chart_scheduler* inner_scheduler = nullptr;
};

inline void record_chart_execution_plan_build_stats(
    chart_spr_search_counters& counters,
    chart_execution_plan_build_stats const& stats) {
  counters.chart_execution_plan_builds += stats.plan_builds;
  counters.full_grammar_validations += stats.full_grammar_validations;
  counters.production_index_validations += stats.production_index_validations;
  counters.production_partition_validations +=
      stats.production_partition_validations;
  counters.clade_order_sorts += stats.clade_order_sorts;
  counters.production_descriptors_compiled +=
      stats.production_descriptors_compiled;
}

inline void record_planned_overlay_materialization_stats(
    chart_spr_search_counters& counters,
    planned_overlay_materialization_result const& planned) {
  record_chart_execution_plan_build_stats(counters,
                                          planned.execution_plan.build_stats());
  counters.dynamic_overlay_payload_partition_validations +=
      planned.payload_validation_stats.production_partition_validations;
}

inline void record_overlay_payload_validation_stats(
    chart_spr_search_counters& counters,
    overlay_payload_validation_stats const& stats) {
  counters.dynamic_overlay_payload_partition_validations +=
      stats.production_partition_validations;
}

inline void record_multisite_exact_setup_work_stats(
    chart_spr_search_counters& counters,
    multisite_exact_setup_work_stats const& stats) {
  counters.exact_setup_builds += stats.setup_builds;
  counters.exact_setup_inside_charts_built += stats.inside_charts_built;
  counters.exact_setup_resident_inside_charts_consumed +=
      stats.resident_inside_charts_consumed;
  counters.exact_setup_active_leaf_state_vectors_copied +=
      stats.active_leaf_state_vectors_copied;
  counters.exact_setup_active_leaf_states_copied +=
      stats.active_leaf_states_copied;
  counters.exact_setup_outside_boundary_charts_built +=
      stats.outside_boundary_charts_built;
  counters.exact_setup_upper_bound_topologies_generated +=
      stats.upper_bound_topologies_generated;
  counters.exact_setup_upper_bound_topologies_unique +=
      stats.upper_bound_topologies_unique;
  counters.exact_setup_frontier_passes += stats.frontier_passes;
}

inline void record_multisite_exact_trim_work(
    chart_spr_search_counters& counters,
    multisite_trim_result const& trim) {
  record_multisite_exact_setup_work_stats(counters, trim.exact_setup_work);
  counters.exact_bnb_levels += trim.exact_bnb_levels;
  counters.exact_bnb_clades += trim.exact_bnb_clades;
  counters.exact_bnb_product_combinations +=
      trim.exact_bnb_product_combinations;
  counters.exact_bnb_frontier_entries += trim.exact_bnb_frontier_entries;
  counters.exact_bnb_ms += trim.exact_bnb_ms;
}

inline void record_inside_chart_cache_build_work(
    chart_spr_search_counters& counters, std::size_t inside_charts_built,
    std::size_t resident_inside_charts_consumed) {
  counters.inside_cache_inside_charts_built += inside_charts_built;
  counters.inside_cache_resident_inside_charts_consumed +=
      resident_inside_charts_consumed;
}

// Acceptance modes describe the objective used to accept a candidate.  They
// are intentionally independent from chart_spr_candidate_selection_mode below,
// which describes how many locally ranked candidates receive exact
// verification.
//
// exact_multisite (default): grammar-exact multi-site B&B over the modified
// grammar/topology set.  The current state's old exact score is cached; top-K
// verification materializes only candidate overlays and is counted separately
// from accepted-state rebuilds.
//
// fixed_topology_exact: exact only for one complete before/after topology
// certificate or deterministic selector recorded in the result.  A bare
// grammar_spr_candidate is not enough because it leaves grammar production
// choices open.
//
// lower_bound_heuristic: opt-in exploratory mode that accepts a composite
// lower-bound improvement.  It is not a coupled multi-site parsimony proof and
// reports must label it as heuristic.
enum class chart_spr_acceptance_mode {
  exact_multisite,
  fixed_topology_exact,
  lower_bound_heuristic,
};

inline char const* chart_spr_acceptance_mode_name(
    chart_spr_acceptance_mode mode) {
  switch (mode) {
    case chart_spr_acceptance_mode::exact_multisite:
      return "exact_multisite";
    case chart_spr_acceptance_mode::fixed_topology_exact:
      return "fixed_topology_exact";
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return "lower_bound_heuristic";
  }
  return "unknown";
}

// Candidate-selection modes describe which generated/scored candidates are
// exact-verified.  They do not change the acceptance objective above.  For
// example, exact_multisite + lower_bound_top_k means the accepted move (if any)
// passed grammar-exact verification, but unverified candidates may still hide
// improvements; exhaustive_exact is the fully exhaustive policy.
enum class chart_spr_candidate_selection_mode {
  exhaustive_exact,
  lower_bound_top_k,
  lower_bound_first_improvement,
  sampled_or_randomized,
};

inline char const* chart_spr_candidate_selection_mode_name(
    chart_spr_candidate_selection_mode mode) {
  switch (mode) {
    case chart_spr_candidate_selection_mode::exhaustive_exact:
      return "exhaustive_exact";
    case chart_spr_candidate_selection_mode::lower_bound_top_k:
      return "lower_bound_top_k";
    case chart_spr_candidate_selection_mode::lower_bound_first_improvement:
      return "lower_bound_first_improvement";
    case chart_spr_candidate_selection_mode::sampled_or_randomized:
      return "sampled_or_randomized";
  }
  return "unknown";
}

enum class chart_spr_topology_selection_kind {
  none,
  explicit_certificate,
  deterministic_selector,
};

inline char const* chart_spr_topology_selection_kind_name(
    chart_spr_topology_selection_kind kind) {
  switch (kind) {
    case chart_spr_topology_selection_kind::none:
      return "none";
    case chart_spr_topology_selection_kind::explicit_certificate:
      return "explicit_certificate";
    case chart_spr_topology_selection_kind::deterministic_selector:
      return "deterministic_selector";
  }
  return "unknown";
}

// Phase 10 (cross-cutting CLI / report / identity surface).  These two enums
// name the accepted-state commit path and the exact-verification path a run
// selects, so the Phase-10 report surfaces them as labelled, distinct modes
// (defaults unchanged) and the counter contract can be read off the report.
//
// `chart_spr_commit_mode` names how an accepted move commits.  The default
// `overlay_delta` is the Phase-4 local-commit path: append the accepted SPR
// overlay to the base-plus-overlay chain and refresh the persistent inside /
// outside caches.  `option_c` names a rank-3 Option-C rewrite committed through
// `option_c_commit_via_chain` (Phase 6/7), which is reachable ONLY through that
// library API: the search loop's candidate generator always produces SPR
// overlay-delta commits, so it cannot honor `option_c`.  Selecting `option_c`
// for `run_chart_spr_search` therefore throws a labelled error from
// `validate_chart_spr_search_loop_options` rather than being silently reported
// with a label the loop did not honor (no silent fallback).  The Option-C
// commit machinery stays a library API, exercised by `option_c_chain_commit_test`.
enum class chart_spr_commit_mode {
  overlay_delta,
  option_c,
};

inline char const* chart_spr_commit_mode_name(chart_spr_commit_mode mode) {
  switch (mode) {
    case chart_spr_commit_mode::overlay_delta:
      return "overlay_delta";
    case chart_spr_commit_mode::option_c:
      return "option_c";
  }
  return "unknown";
}

// `chart_spr_verification_mode` names how an unaccepted candidate is exact-
// verified.  The default `transient` is the Phase-9 path: transiently extend
// the chain in reader-local scratch (counted under
// `transient_chain_extensions_for_verification`, never under
// `full_overlay_materializations`).  `cold` skips installing the transient
// verifier so the from-scratch `verify_candidate_exact_against_state` path
// runs (a dense materialization per verified candidate, counted under
// `overlay_materializations_for_exact_verification`).  This is the named
// verification-mode choice the Phase-10 report surfaces.  Diagnostic scratch
// caches are built only when the opt-in two-chart oracle requests them.
enum class chart_spr_verification_mode {
  transient,
  cold,
};

inline char const* chart_spr_verification_mode_name(
    chart_spr_verification_mode mode) {
  switch (mode) {
    case chart_spr_verification_mode::transient:
      return "transient";
    case chart_spr_verification_mode::cold:
      return "cold";
  }
  return "unknown";
}

struct chart_spr_production_signature {
  // Stable across dense overlay materialization/rebuild.  In-process reports
  // use taxon IDs; cross-run reports can translate them to sample IDs.
  std::vector<taxon_id> parent_taxa;
  std::vector<std::vector<taxon_id>> child_taxa;

  bool operator==(chart_spr_production_signature const&) const = default;
};

struct chart_spr_topology_certificate {
  // Overlay refs are stable inside one candidate overlay and avoid tying the
  // certificate to dense materialized production IDs.
  std::vector<overlay_production_ref> before_overlay_productions;
  std::vector<overlay_production_ref> after_overlay_productions;

  // Taxon-key signatures let the certificate survive overlay materialization,
  // accepted-state rebuilds, and diagnostic JSON round trips.
  std::vector<chart_spr_production_signature> before_signatures;
  std::vector<chart_spr_production_signature> after_signatures;
};

struct chart_spr_topology_selection {
  chart_spr_topology_selection_kind kind =
      chart_spr_topology_selection_kind::none;
  std::string selector_name;
  std::optional<chart_spr_topology_certificate> certificate;
};

inline chart_spr_production_signature chart_spr_production_signature_for_ref(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_production_ref ref) {
  chart_spr_production_signature signature;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_production || ref.id >= base.productions.size()) {
      throw std::runtime_error(
          "chart SPR topology certificate: base production ref out of range");
    }
    auto const& prod = base.productions[ref.id];
    signature.parent_taxa = base.clades[prod.parent].taxa;
    signature.child_taxa.reserve(prod.children.size());
    for (auto child : prod.children) {
      signature.child_taxa.push_back(base.clades[child].taxa);
    }
  } else {
    if (ref.id == no_production ||
        ref.id >= candidate.added_productions.size()) {
      throw std::runtime_error(
          "chart SPR topology certificate: temp production ref out of range");
    }
    auto const& prod = candidate.added_productions[ref.id];
    signature.parent_taxa =
        chart_spr_clade_taxa_for_ref(base, candidate, prod.parent);
    signature.child_taxa.reserve(prod.children.size());
    for (auto child : prod.children) {
      signature.child_taxa.push_back(
          chart_spr_clade_taxa_for_ref(base, candidate, child));
    }
  }
  std::sort(signature.parent_taxa.begin(), signature.parent_taxa.end());
  for (auto& child : signature.child_taxa) {
    std::sort(child.begin(), child.end());
  }
  std::sort(signature.child_taxa.begin(), signature.child_taxa.end());
  return signature;
}

inline chart_spr_topology_certificate make_chart_spr_topology_certificate(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::vector<overlay_production_ref> before,
    std::vector<overlay_production_ref> after) {
  chart_spr_topology_certificate certificate;
  certificate.before_overlay_productions = std::move(before);
  certificate.after_overlay_productions = std::move(after);
  certificate.before_signatures.reserve(
      certificate.before_overlay_productions.size());
  for (auto ref : certificate.before_overlay_productions) {
    certificate.before_signatures.push_back(
        chart_spr_production_signature_for_ref(base, candidate, ref));
  }
  certificate.after_signatures.reserve(
      certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    certificate.after_signatures.push_back(
        chart_spr_production_signature_for_ref(base, candidate, ref));
  }
  return certificate;
}

enum class chart_spr_lazy_policy {
  off,
  on,
  automatic,
};

inline char const* chart_spr_lazy_policy_name(chart_spr_lazy_policy policy) {
  switch (policy) {
    case chart_spr_lazy_policy::off:
      return "off";
    case chart_spr_lazy_policy::on:
      return "on";
    case chart_spr_lazy_policy::automatic:
      return "auto";
  }
  return "unknown";
}

enum class chart_spr_lazy_policy_reason {
  explicit_off,
  explicit_on,
  no_active_patterns,
  full_lazy_budget_exceeded,
  pilot_budget_exceeded,
  no_internal_structural_classes,
  structural_and_strong_row_ratios_exceeded,
  row_ratio_above_one_half,
  key_work_above_eight_dense_rows,
  compression_thresholds_and_budget_safe,
};

inline char const* chart_spr_lazy_policy_reason_name(
    chart_spr_lazy_policy_reason reason) noexcept {
  switch (reason) {
    case chart_spr_lazy_policy_reason::explicit_off:
      return "explicit_off";
    case chart_spr_lazy_policy_reason::explicit_on:
      return "explicit_on";
    case chart_spr_lazy_policy_reason::no_active_patterns:
      return "no_active_patterns";
    case chart_spr_lazy_policy_reason::full_lazy_budget_exceeded:
      return "full_lazy_budget_exceeded";
    case chart_spr_lazy_policy_reason::pilot_budget_exceeded:
      return "pilot_budget_exceeded";
    case chart_spr_lazy_policy_reason::no_internal_structural_classes:
      return "no_internal_structural_classes";
    case chart_spr_lazy_policy_reason::
        structural_and_strong_row_ratios_exceeded:
      return "structural_and_strong_row_ratios_exceeded";
    case chart_spr_lazy_policy_reason::row_ratio_above_one_half:
      return "row_ratio_above_one_half";
    case chart_spr_lazy_policy_reason::key_work_above_eight_dense_rows:
      return "key_work_above_eight_dense_rows";
    case chart_spr_lazy_policy_reason::compression_thresholds_and_budget_safe:
      return "compression_thresholds_and_budget_safe";
  }
  return "unknown";
}

// Phase-7 policy diagnostics are integer observations.  Floating-point ratios
// are presentation-only so the representation choice is reproducible across
// machines and never depends on wall-clock samples.
struct chart_spr_lazy_policy_diagnostics {
  static constexpr std::uint32_t current_version = 1;

  std::uint32_t version = current_version;
  chart_spr_lazy_policy requested = chart_spr_lazy_policy::off;
  chart_spr_lazy_policy resolved = chart_spr_lazy_policy::off;
  chart_spr_lazy_policy_reason reason =
      chart_spr_lazy_policy_reason::explicit_off;
  bool frozen = false;
  // False only when a lightweight standalone diagnostic deliberately avoids
  // constructing the full pattern set needed to populate the numeric fields.
  // Core policy resolution always publishes complete measurements.
  bool measurements_available = true;

  std::size_t active_pattern_count = 0;
  std::size_t pilot_pattern_count = 0;
  std::uint64_t pilot_pattern_index_hash = 0;
  std::size_t pilot_inside_chart_builds = 0;
  std::size_t pilot_outside_chart_builds = 0;
  std::size_t pilot_exact_builds = 0;
  std::size_t pilot_scheduler_submissions = 0;
  std::size_t pilot_internal_structural_class_count_max = 0;
  std::size_t pilot_structural_ratio_numerator = 0;
  std::size_t pilot_structural_ratio_denominator = 0;
  std::size_t pilot_inside_rows = 0;
  std::size_t pilot_dense_rows = 0;
  std::size_t pilot_row_ratio_numerator = 0;
  std::size_t pilot_row_ratio_denominator = 0;
  std::size_t pilot_key_words = 0;
  std::size_t pilot_dense_row_work = 0;
  std::size_t pilot_estimated_allocation_bytes = 0;
  std::size_t estimated_lazy_cache_bytes = 0;
  // Full active-pattern dense row surface, independent of any subsequently
  // selected pattern-batch size.
  std::size_t estimated_dense_cache_bytes = 0;

  bool operator==(chart_spr_lazy_policy_diagnostics const&) const = default;
};

struct chart_cache_options {
  std::size_t max_cached_patterns = 0;  // 0 = all patterns
  std::size_t memory_budget_bytes = 0;  // 0 = no explicit budget
  std::size_t candidate_batch_size = 0; // 0 = choose from memory budget
  std::size_t pattern_batch_size = 0;   // 0 = derive from cache budget
  // Source-compatible explicit-on alias.  New callers should set lazy_policy;
  // a true legacy value always retains its historical forced-lazy meaning.
  bool use_lazy_multisite_chart = false;
  chart_spr_lazy_policy lazy_policy = chart_spr_lazy_policy::off;
};

namespace chart_spr_search_detail {

// Defined only in chart_spr_search.cpp. Public state builders can name but
// cannot construct this token; accepted-state rebuilds use it to carry one
// automatic decision without exposing caller-forgeable frozen diagnostics in
// chart_cache_options.
class chart_spr_lazy_policy_rebuild_token;

chart_spr_lazy_policy_diagnostics const&
chart_spr_lazy_policy_from_rebuild_token(
    chart_spr_lazy_policy_rebuild_token const& token) noexcept;

}  // namespace chart_spr_search_detail

struct chart_spr_search_state;

using chart_spr_topology_selection_provider = std::function<
    std::optional<chart_spr_topology_selection>(
        chart_spr_search_state const&, grammar_spr_candidate const&)>;

struct chart_spr_topology_selection_memory_estimate {
  std::size_t scratch_bytes = 0;
  std::size_t retained_result_bytes = 0;
  bool safely_bounded = true;
};

// Estimators are preflight metadata callbacks: they must not allocate material
// scratch or invoke the selector itself. Their returned scratch covers the
// subsequent provider invocation, not evaluation of this callback.
using chart_spr_topology_selection_memory_estimator = std::function<
    chart_spr_topology_selection_memory_estimate(
        chart_spr_search_state const&, grammar_spr_candidate const&)>;

struct chart_spr_search_options {
  std::size_t max_iterations = 1;
  std::size_t max_candidates_per_iteration = 0;  // 0 = unlimited
  std::size_t top_k_exact_verify = 16;
  chart_spr_acceptance_mode acceptance_mode =
      chart_spr_acceptance_mode::exact_multisite;
  chart_spr_candidate_selection_mode candidate_selection =
      chart_spr_candidate_selection_mode::lower_bound_top_k;

  grammar_spr_enumeration_options enumeration = {};
  chart_options chart = {};
  multisite_trim_options exact_trim = {};
  chart_cache_options cache = {};

  // Opt-in canonical semantic oracle.  `off` preserves the historical
  // retention and performance behavior.  `digest` captures semantic records
  // long enough to hash them; `full` also retains the exact NDJSON byte stream
  // whose SHA-256 is reported by the compact result.
  chart_spr_semantic_capture_mode semantic_capture =
      chart_spr_semantic_capture_mode::off;
  std::string semantic_polytomy_mode;
  std::string semantic_refinement_exactness;
  std::size_t semantic_polytomy_max_exact_arity = 0;
  std::size_t semantic_polytomy_max_shapes = 0;
  std::size_t semantic_polytomy_max_productions = 0;
  std::size_t semantic_polytomy_max_clades = 0;

  // Unified chart-search worker budget.  This is the forward-looking budget
  // for every parallel chart phase.  Phase 0 maps it to the already-parallel
  // local scorer; later phases consume the same budget without adding
  // phase-specific CLI knobs.  Zero chooses the affinity-restricted physical
  // core count when available, then affinity logical CPUs, then the portable
  // hardware-concurrency/serial fallback reported by chart_scheduler.
  std::size_t worker_count = 1;

  // Parallel local lower-bound scoring.  1 is deterministic serial scoring;
  // 0 chooses the same topology-aware automatic policy as worker_count.
  // Kept as a source-compatible library alias.  Command-line callers set this
  // from worker_count; direct callers that set only this field retain the
  // historical behavior.
  std::size_t local_score_worker_count = 1;

  // Optional hook for fixed_topology_exact callers that already have a
  // complete before/after topology certificate. If absent,
  // fixed_topology_exact first consumes a projected sampled-tree candidate's
  // source topology certificate when present; otherwise it uses the
  // deterministic selector named below and records the resulting certificate on
  // each scored candidate.
  chart_spr_topology_selection_provider topology_selection_provider = {};
  std::string fixed_topology_selector_name =
      "first_reachable_overlay_topology";

  bool materialize_accepted_moves = true;
  // true: conservative path materializes an accepted move and rebuilds the
  // grammar/pattern/chart sidecar before the next iteration. false: Phase 4
  // local-commit mode appends the accepted overlay to the base-plus-overlay
  // chain and updates the persistent inside+outside chart caches, avoiding
  // per-accept dense materialization and sidecar rebuilds.
  //
  // Phase 4/5 limitations: local commit requires an exact acceptance gate
  // (exact_multisite or fixed_topology_exact) and currently requires
  // chart.score_ua_edge == false because the persistent outside cache needs a
  // documented per-pattern reference-state convention.  Final compaction is
  // grammar-valued: the materialized chain DAG is scored by exact B&B over the
  // output grammar and every accepted overlay production key is checked as a
  // witness in the compacted DAG.
  bool rebuild_after_accept = true;

  // Phase 10 cross-cutting surface.  `commit_mode` names the accepted-state
  // commit path a run reports (overlay_delta default; option_c for rank-3
  // Option-C commits via the library API).  `verification_mode` names the
  // exact-verification path: `transient` (default) installs the Phase-9
  // transient-extension exact_multisite verifier; `cold` skips it so the
  // from-scratch `verify_candidate_exact_against_state` path runs.  Both are
  // labelled distinctly in the report; defaults are unchanged.  See the enums
  // above and the Phase-10 doc.
  chart_spr_commit_mode commit_mode = chart_spr_commit_mode::overlay_delta;
  chart_spr_verification_mode verification_mode =
      chart_spr_verification_mode::transient;

  // Test/diagnostic hooks for validating expensive guardrails without needing
  // pathological input DAGs.
  bool verify_local_against_full_for_tests = false;
  bool force_pattern_fingerprint_mismatch_for_tests = false;
  // Phase 4 self-check: after every local commit, recompute BOTH the inside and
  // the outside chart from scratch on the materialized chain (Phase 0's
  // two-chart oracle) and assert the persistent caches agree, per Work item
  // 3's correctness invariant.  This is the load-bearing guard against
  // affected-set under-inclusion (the most likely silent bug).  Off by default;
  // the Phase 4 tests enable it to satisfy the "two-chart oracle green after
  // every accept" exit criterion without exposing the cache objects.
  bool verify_local_commit_two_chart_oracle_for_tests = false;
  // Phase 8 self-check: during local-commit fixed_topology_exact verification,
  // materialize each candidate overlay and compare the cache score against a
  // from-scratch selected-topology score on the same extended grammar, per
  // active pattern.  Expensive and test/diagnostic-only; the production path
  // does not run a hidden from-scratch selected-topology oracle per candidate.
  bool verify_fixed_topology_materialized_oracle_for_tests = false;
  // Phase 8 corruption hook: when a candidate/pattern exposes the real
  // independent-s_M bug class, make the persistent fixed-topology scorer return
  // that test-only under-count before oracle comparison.  The per-pattern
  // oracle must catch witnessed corruptions and route through the fallback.
  bool force_fixed_topology_independent_sm_bug_for_tests = false;
  // Phase 8 hard-error hook: corrupt the local-commit fixed-topology cache
  // epoch before verification.  Cache/substrate invariant failures must be
  // rethrown as labelled correctness failures, not converted into invalid
  // candidates / "no exact-improving verified candidate".
  bool force_fixed_topology_cache_epoch_mismatch_for_tests = false;
  // Phase 9 self-check: during local-commit exact_multisite verification,
  // materialize each candidate overlay via the cold from-scratch path and
  // compare the transient-extension result against it (both charts + exact
  // optimum), per Work item 4a's correctness oracle.  Expensive and
  // test/diagnostic-only; the production path trusts the transient result and
  // never runs a hidden from-scratch oracle per candidate.
  bool verify_transient_chain_extension_oracle_for_tests = false;
  // Phase 9 corruption hook: perturb the transient extension's scratch
  // outside cache so the per-candidate two-chart oracle catches the
  // disagreement and the verifier falls back to the cold path.  The oracle
  // must catch witnessed corruptions; a nonzero fallback count proves the
  // gate fires rather than silently trusting a wrong transient result.
  bool force_transient_chain_extension_oracle_mismatch_for_tests = false;
  // Semantic-oracle isolation hook.  When capture is enabled, fail while
  // constructing exact evidence *after* the verifier has established its
  // algorithmic outcome.  Tests use this to prove report failures propagate
  // as hard errors instead of being converted into invalid candidates.
  bool force_canonical_evidence_failure_for_tests = false;
  // Exact-verifier hard-error hook.  Arithmetic overflow is an integrity
  // failure, not a property of one biological candidate; tests inject it at
  // the candidate B&B boundary in both cold and transient verification modes.
  bool force_candidate_exact_bnb_overflow_for_tests = false;
  // Test-only injection point for the Phase 4 hard-error contract: after the
  // overlay-chain append succeeds, force a post-append failure and verify the
  // search propagates it instead of converting it into an ordinary rejection.
  bool force_local_commit_post_append_failure_for_tests = false;
  std::optional<std::uint64_t>
      override_post_materialization_rebuilt_score_for_tests;
  // Legacy Phase-4 tree-valued final-compaction override.  The Phase-5
  // grammar-valued local-compaction oracle ignores this hook; it remains in
  // the options struct so tests can assert the old tree-level score override no
  // longer controls the reported output-DAG parsimony.
  std::optional<std::uint64_t>
      override_final_compaction_rebuilt_score_for_tests;
  // Test-only corruption hook for the Phase-5 recorded-chain-objective check:
  // local compaction must compare the output DAG oracle to the exact objective
  // recorded at accept time, not to a fresh final-state recomputation that
  // could mask a bad recorded score.
  std::optional<std::uint64_t>
      override_local_commit_recorded_objective_for_tests;

  std::uint32_t seed = 1;

  // Phase-6 additions deliberately follow the historical aggregate prefix so
  // positional initializers compiled against the pre-Phase-6 options layout
  // retain their source meaning.
  // Additional provider-specific memory beyond the built-in certificate and
  // selector envelope. A finite unified budget fails closed before invoking a
  // custom provider unless this contract is supplied.
  chart_spr_topology_selection_memory_estimator
      topology_selection_additional_memory_estimator = {};
  // Stable-rank hook invoked inside each admitted exact-candidate task after
  // concurrency tracking begins and before verifier entry. Tests use it to
  // force overlap and reverse completion/failure order without weakening
  // production scheduling or exception semantics.
  std::function<void(std::size_t)>
      before_exact_candidate_verification_for_tests = {};
  // Exact-axis-only infrastructure injection. The first candidate-parallel
  // wave asks the shared scheduler to fail after this many successful runner
  // submissions. This is deliberately separate from local scoring so tests
  // can prove partial exact-wave join/accounting and deterministic failure
  // precedence without perturbing earlier scheduler operations.
  std::optional<std::size_t>
      force_exact_candidate_submit_failure_after_for_tests;

  // Phase-8 bounded candidate-generation pipeline.  Explicit W1 always keeps
  // the historical serial generator/scorer order.  Multi-worker callers may
  // opt out to retain that path for profiling or stop-rule fallback.
  bool enable_candidate_generation_pipeline = true;
  std::optional<std::size_t>
      force_candidate_pipeline_generation_failure_after_for_tests;
  std::optional<std::size_t>
      force_candidate_pipeline_scoring_failure_after_batches_for_tests;
  std::optional<std::size_t>
      force_candidate_pipeline_cancel_after_scored_batches_for_tests;
  std::optional<std::size_t>
      force_candidate_pipeline_stale_buffer_after_batches_for_tests;
  std::function<void()> before_candidate_pipeline_start_for_tests = {};
  std::function<void(std::size_t)>
      before_candidate_pipeline_score_batch_for_tests = {};
  std::function<void(std::size_t)>
      after_candidate_pipeline_score_batch_for_tests = {};
  // Non-owning oversized-diagnostic seam for finite dense-score admission
  // tests. The view must outlive the synchronous acceptance iteration.
  std::string_view force_dense_invalid_reason_for_tests = {};
};

struct chart_spr_exact_candidate_memory_estimate {
  // Peak task-local storage when the candidate itself is the parallel axis.
  std::size_t serial_scratch_bytes = 0;
  // Peak task-local storage for a singleton candidate allowed to use the
  // Phase-5 inner scheduler. This includes serial_scratch_bytes.
  std::size_t inner_parallel_scratch_bytes = 0;
  // Scheduler-owned operation/run-summary storage for that singleton inner
  // path. It is temporally alternative to the candidate-parallel outer
  // scheduler operation and is populated from the actual search scheduler.
  std::size_t inner_scheduler_scratch_bytes = 0;
  // Storage that survives task completion until stable-rank aggregation.
  std::size_t retained_result_bytes = 0;
  // False means standard-container growth could not be bounded before the
  // finite-budget sentinel. Such a candidate is still runnable with the
  // historical unlimited budget, but finite admission fails closed.
  bool safely_bounded = true;
};

struct chart_spr_exact_candidate_admission_wave {
  std::size_t begin_rank = 0;
  std::size_t end_rank = 0;
  std::size_t admitted_bytes = 0;
  bool memory_limited = false;
  bool use_inner_parallelism = false;
};

class chart_spr_exact_candidate_budget_error : public std::runtime_error {
 public:
  chart_spr_exact_candidate_budget_error(std::size_t stable_rank,
                                         std::size_t required_bytes,
                                         std::size_t available_bytes)
      : std::runtime_error("chart SPR exact-candidate admission: stable rank " +
                           std::to_string(stable_rank) + " requires " +
                           std::to_string(required_bytes) + " bytes but only " +
                           std::to_string(available_bytes) +
                           " bytes are available"),
        stable_rank_(stable_rank),
        required_bytes_(required_bytes),
        available_bytes_(available_bytes) {}

  [[nodiscard]] std::size_t stable_rank() const noexcept {
    return stable_rank_;
  }
  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }
  [[nodiscard]] std::size_t available_bytes() const noexcept {
    return available_bytes_;
  }

 private:
  std::size_t stable_rank_;
  std::size_t required_bytes_;
  std::size_t available_bytes_;
};

class chart_spr_exact_state_budget_error : public std::runtime_error {
 public:
  chart_spr_exact_state_budget_error(std::size_t required_bytes,
                                     std::size_t budget_bytes)
      : std::runtime_error(
            "chart SPR exact-state admission requires " +
            std::to_string(required_bytes) +
            " bytes, exceeding configured unified chart/exact budget " +
            std::to_string(budget_bytes)),
        required_bytes_(required_bytes),
        budget_bytes_(budget_bytes) {}

  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }
  [[nodiscard]] std::size_t budget_bytes() const noexcept {
    return budget_bytes_;
  }

 private:
  std::size_t required_bytes_;
  std::size_t budget_bytes_;
};

inline std::size_t chart_spr_exact_candidate_checked_bytes_add(
    std::size_t lhs, std::size_t rhs, std::string_view context) {
  if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
    throw std::overflow_error(std::string{context} + " byte overflow");
  }
  return lhs + rhs;
}

inline std::size_t chart_spr_exact_candidate_checked_bytes_multiply(
    std::size_t lhs, std::size_t rhs, std::string_view context) {
  if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
    throw std::overflow_error(std::string{context} + " byte overflow");
  }
  return lhs * rhs;
}

inline std::size_t chart_spr_exact_candidate_saturating_bytes_add(
    std::size_t lhs, std::size_t rhs) noexcept {
  return rhs > (std::numeric_limits<std::size_t>::max)() - lhs
             ? (std::numeric_limits<std::size_t>::max)()
             : lhs + rhs;
}

// Deterministic maximal stable-prefix planner. A finite budget never skips an
// earlier large candidate to admit a later small one. The caller subtracts
// shared resident storage and retained results from available_bytes first.
inline chart_spr_exact_candidate_admission_wave
plan_chart_spr_exact_candidate_admission_wave(
    std::span<chart_spr_exact_candidate_memory_estimate const> estimates,
    std::size_t begin_rank, std::size_t worker_limit,
    std::size_t available_bytes, bool enforce_budget,
    chart_scheduler const* outer_scheduler = nullptr,
    chart_indexed_range_options outer_range_options =
        chart_indexed_range_options{.minimum_grain = 1,
                                    .target_ranges_per_worker = 1}) {
  if (begin_rank >= estimates.size()) {
    return chart_spr_exact_candidate_admission_wave{.begin_rank = begin_rank,
                                                    .end_rank = begin_rank};
  }
  worker_limit = std::max<std::size_t>(1, worker_limit);
  auto const candidate_limit =
      std::min(worker_limit, estimates.size() - begin_rank);
  auto const effective_available =
      enforce_budget ? available_bytes
                     : (std::numeric_limits<std::size_t>::max)();
  auto outer_operation_bytes = [&](std::size_t count) {
    if (count < 2 || outer_scheduler == nullptr) return std::size_t{0};
    return estimate_chart_scheduler_operation_peak_bytes(
        outer_scheduler->plan_indexed_ranges(count, outer_range_options));
  };
  auto singleton_inner_required =
      [&](chart_spr_exact_candidate_memory_estimate const& estimate) {
        auto required = chart_spr_exact_candidate_saturating_bytes_add(
            estimate.inner_parallel_scratch_bytes,
            estimate.inner_scheduler_scratch_bytes);
        return chart_spr_exact_candidate_saturating_bytes_add(
            required, estimate.retained_result_bytes);
      };

  if (!enforce_budget) {
    std::size_t admitted = 0;
    for (std::size_t offset = 0; offset < candidate_limit; ++offset) {
      auto const& estimate = estimates[begin_rank + offset];
      auto required = estimate.serial_scratch_bytes;
      if (candidate_limit == 1) {
        required = chart_spr_exact_candidate_saturating_bytes_add(
            estimate.inner_parallel_scratch_bytes,
            estimate.inner_scheduler_scratch_bytes);
      }
      if (required > (std::numeric_limits<std::size_t>::max)() -
                         estimate.retained_result_bytes) {
        admitted = (std::numeric_limits<std::size_t>::max)();
        break;
      }
      required += estimate.retained_result_bytes;
      admitted = required > (std::numeric_limits<std::size_t>::max)() - admitted
                     ? (std::numeric_limits<std::size_t>::max)()
                     : admitted + required;
    }
    if (candidate_limit >= 2) {
      admitted = chart_spr_exact_candidate_saturating_bytes_add(
          admitted, outer_operation_bytes(candidate_limit));
    }
    return chart_spr_exact_candidate_admission_wave{
        .begin_rank = begin_rank,
        .end_rank = begin_rank + candidate_limit,
        .admitted_bytes = admitted,
        .use_inner_parallelism = candidate_limit == 1,
    };
  }

  std::size_t admitted = 0;
  std::size_t count = 0;
  for (; count < candidate_limit; ++count) {
    auto const& estimate = estimates[begin_rank + count];
    if (!estimate.safely_bounded) break;
    auto const serial_overflow = estimate.serial_scratch_bytes >
                                 (std::numeric_limits<std::size_t>::max)() -
                                     estimate.retained_result_bytes;
    auto const required = chart_spr_exact_candidate_saturating_bytes_add(
        estimate.serial_scratch_bytes, estimate.retained_result_bytes);
    if (serial_overflow || required > effective_available - admitted) break;
    auto const candidate_sum = admitted + required;
    auto const operation = outer_operation_bytes(count + 1);
    if (operation > effective_available - candidate_sum) break;
    admitted = candidate_sum;
  }
  if (count == 0) {
    auto const& estimate = estimates[begin_rank];
    auto const required =
        estimate.safely_bounded
            ? chart_spr_exact_candidate_saturating_bytes_add(
                  estimate.serial_scratch_bytes, estimate.retained_result_bytes)
            : (std::numeric_limits<std::size_t>::max)();
    throw chart_spr_exact_candidate_budget_error(begin_rank, required,
                                                 effective_available);
  }

  chart_spr_exact_candidate_admission_wave wave{
      .begin_rank = begin_rank,
      .end_rank = begin_rank + count,
      .admitted_bytes = chart_spr_exact_candidate_saturating_bytes_add(
          admitted, outer_operation_bytes(count)),
      .memory_limited =
          count < candidate_limit && begin_rank + count < estimates.size(),
  };
  if (count == 1) {
    auto const& estimate = estimates[begin_rank];
    auto const inner_required = singleton_inner_required(estimate);
    if (inner_required != (std::numeric_limits<std::size_t>::max)() &&
        inner_required <= effective_available) {
      wave.admitted_bytes = inner_required;
      wave.use_inner_parallelism = true;
    } else {
      wave.memory_limited = true;
    }
  }
  return wave;
}

// Canonical exact evidence needs the equality-deduplicated optimal root
// frontier classes.  Capture them in the algorithmic trim itself so a
// semantic run does not repeat the entire exact B&B solely for reporting.
// Score-only trims intentionally remain unchanged: they cannot provide an
// exact production mask or exact tied-root provenance.
inline void configure_chart_spr_primary_exact_provenance(
    chart_spr_search_options& options) noexcept {
  if (options.semantic_capture == chart_spr_semantic_capture_mode::off ||
      options.acceptance_mode != chart_spr_acceptance_mode::exact_multisite ||
      chart_multisite_detail::keep_mask_kind_for_options(options.exact_trim) !=
          multisite_keep_mask_kind::exact_optimal_production_union) {
    return;
  }
  options.exact_trim.capture_optimal_root_provenance = true;
}

enum class chart_spr_score_kind {
  composite_lower_bound,
  fixed_topology_exact,
  grammar_exact,
};

inline char const* chart_spr_score_kind_name(chart_spr_score_kind kind) {
  switch (kind) {
    case chart_spr_score_kind::composite_lower_bound:
      return "composite_lower_bound";
    case chart_spr_score_kind::fixed_topology_exact:
      return "fixed_topology_exact";
    case chart_spr_score_kind::grammar_exact:
      return "grammar_exact";
  }
  return "unknown";
}

enum class chart_spr_score_convention {
  active_only,
  full_with_invariants,
};

inline char const* chart_spr_score_convention_name(
    chart_spr_score_convention convention) {
  switch (convention) {
    case chart_spr_score_convention::active_only:
      return "active_only";
    case chart_spr_score_convention::full_with_invariants:
      return "full_with_invariants";
  }
  return "unknown";
}

struct chart_spr_objective_score {
  spr_score_result value;
  chart_spr_score_kind kind = chart_spr_score_kind::composite_lower_bound;
  chart_spr_score_convention convention =
      chart_spr_score_convention::full_with_invariants;

  // Nonzero only when convention == full_with_invariants.  Keeping this field
  // beside the score makes active-only vs full-objective comparisons explicit.
  std::uint64_t invariant_offset_applied = 0;
};

inline chart_spr_canonical_score chart_spr_canonicalize_objective_score(
    chart_spr_objective_score const& score) {
  return {
      .kind = chart_spr_score_kind_name(score.kind),
      .convention = chart_spr_score_convention_name(score.convention),
      .delta = score.value.delta,
      .old_score = score.value.old_score,
      .new_score = score.value.new_score,
      .invariant_offset = score.invariant_offset_applied,
      .exact_multisite = score.value.exact_multisite,
  };
}

inline chart_spr_canonical_exact_evidence
chart_spr_canonicalize_trim_evidence(
    clade_grammar const& grammar, multisite_trim_result const& trim,
    std::uint64_t invariant_offset) {
  chart_spr_canonical_exact_evidence evidence;
  auto assign_exact = [](std::string& destination, std::string_view value) {
    destination.clear();
    destination.reserve(value.size());
    destination.assign(value);
  };
  if (trim.keep_production_exact) {
    evidence.evidence_kind.clear();
    evidence.evidence_kind.reserve(
        std::string_view{"grammar_exact_frontier_provenance_companion"}
            .size());
    evidence.evidence_kind.assign("grammar_exact_frontier");
  } else {
    assign_exact(evidence.evidence_kind,
                 "grammar_exact_score_only_frontier_statistics");
  }
  assign_exact(evidence.keep_mask_kind,
               multisite_keep_mask_kind_name(trim.keep_mask_kind));
  evidence.keep_production_exact = trim.keep_production_exact;
  evidence.optimum_active = trim.optimum;
  evidence.invariant_offset = invariant_offset;
  if (trim.keep_production_exact) {
    if (trim.keep_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          "chart-SPR canonical report: exact keep mask size mismatch");
    }
    evidence.kept_production_keys.reserve(
        static_cast<std::size_t>(std::count(trim.keep_production.begin(),
                                            trim.keep_production.end(), true)));
    for (std::size_t pid = 0; pid < trim.keep_production.size(); ++pid) {
      if (!trim.keep_production[pid]) continue;
      evidence.kept_production_keys.push_back(
          chart_spr_canonical_production_sample_key(
              grammar, static_cast<production_id>(pid)));
    }
  }
  if (trim.frontier_sizes_by_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "chart-SPR canonical report: frontier-size vector size mismatch");
  }
  evidence.frontier_sizes.reserve(static_cast<std::size_t>(std::count_if(
      grammar.clades.begin(), grammar.clades.end(),
      [](auto const& clade) { return !clade.taxa.empty(); })));
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa.empty()) continue;
    evidence.frontier_sizes.emplace_back(
        chart_spr_canonical_clade_sample_key(grammar,
                                             grammar.clades[cid].taxa),
        trim.frontier_sizes_by_clade[cid]);
  }
  evidence.optimal_root_provenance_classes.reserve(
      trim.optimal_root_provenance_classes.size());
  for (auto const& source : trim.optimal_root_provenance_classes) {
    if (source.used_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          "chart-SPR canonical report: root provenance mask size mismatch");
    }
    chart_spr_canonical_root_provenance_class canonical;
    canonical.cost.reserve(source.cost.size());
    for (auto value : source.cost) canonical.cost.push_back(value);
    canonical.production_keys.reserve(static_cast<std::size_t>(std::count(
        source.used_production.begin(), source.used_production.end(), true)));
    for (std::size_t pid = 0; pid < source.used_production.size(); ++pid) {
      if (!source.used_production[pid]) continue;
      canonical.production_keys.push_back(
          chart_spr_canonical_production_sample_key(
              grammar, static_cast<production_id>(pid)));
    }
    evidence.optimal_root_provenance_classes.push_back(
        std::move(canonical));
  }
  chart_spr_semantic_detail::sort_exact_evidence(evidence);
  return evidence;
}

inline chart_spr_canonical_exact_evidence
chart_spr_canonicalize_fixed_topology_evidence(
    clade_grammar const& grammar, chart_spr_topology_selection const& selection,
    std::uint64_t invariant_offset) {
  chart_spr_canonical_exact_evidence evidence;
  evidence.evidence_kind = "fixed_topology_certificate";
  evidence.keep_mask_kind = "not_applicable_fixed_topology";
  evidence.invariant_offset = invariant_offset;
  evidence.topology_selection_kind =
      chart_spr_topology_selection_kind_name(selection.kind);
  evidence.topology_selector = selection.selector_name;
  if (!selection.certificate) return evidence;
  auto append = [&](std::vector<chart_spr_production_signature> const& source,
                    std::vector<std::string>& destination) {
    for (auto const& signature : source) {
      destination.push_back(chart_spr_canonical_production_sample_key(
          grammar, signature.parent_taxa, signature.child_taxa));
    }
  };
  append(selection.certificate->before_signatures,
         evidence.before_topology_production_keys);
  append(selection.certificate->after_signatures,
         evidence.after_topology_production_keys);
  chart_spr_semantic_detail::sort_exact_evidence(evidence);
  return evidence;
}

struct chart_spr_reusable_exact_trim_payload;

struct chart_spr_candidate_score {
  grammar_spr_candidate candidate;
  chart_spr_objective_score lower_bound;
  std::optional<chart_spr_objective_score> exact;
  chart_spr_topology_selection topology_selection;
  std::size_t affected_clade_count = 0;
  double local_score_ms = 0.0;
  bool valid = true;
  std::string invalid_reason;
  std::size_t canonical_stream_index =
      (std::numeric_limits<std::size_t>::max)();
  // A pointer keeps the off-mode candidate footprint small and preserves the
  // value type's existing copy semantics.  It is allocated only for the small
  // exact-verified subset when semantic capture is enabled.
  std::shared_ptr<chart_spr_canonical_exact_evidence>
      canonical_exact_evidence;
  // Built-in grammar-exact verification already owns the complete exact
  // frontier result for the candidate grammar.  Retain that result through
  // stable winner selection so a compatible local commit can publish it as
  // the next state's old trim without running B&B again.  The structural and
  // option identities are checked against the independently materialized
  // committed tip before publication; custom verifiers may leave this empty.
  std::shared_ptr<chart_spr_reusable_exact_trim_payload> reusable_exact_trim;
  // Forces a throw at the exact materializer call boundary.  This exists only
  // to exercise exception-safe production accounting without relying on an
  // allocation failure or an impractically oversized overlay.
  bool force_exact_materializer_failure_for_tests = false;
  bool force_canonical_evidence_failure_for_tests = false;
  bool force_candidate_exact_bnb_overflow_for_tests = false;
};

inline bool chart_spr_exact_trim_options_equal(
    multisite_trim_options const& lhs,
    multisite_trim_options const& rhs) noexcept {
  return lhs.use_bound_pruning == rhs.use_bound_pruning &&
         lhs.dominance_mode == rhs.dominance_mode &&
         lhs.require_exact_keep_mask == rhs.require_exact_keep_mask &&
         lhs.upper_bound_override == rhs.upper_bound_override &&
         lhs.known_exact_optimum == rhs.known_exact_optimum &&
         lhs.max_frontier_entries_per_clade ==
             rhs.max_frontier_entries_per_clade &&
         lhs.capture_optimal_root_provenance ==
             rhs.capture_optimal_root_provenance &&
         lhs.force_optimal_root_provenance_capture_failure_for_tests ==
             rhs.force_optimal_root_provenance_capture_failure_for_tests;
}

inline bool chart_spr_chart_options_equal(chart_options const& lhs,
                                          chart_options const& rhs) noexcept {
  return lhs.keep_trace == rhs.keep_trace &&
         lhs.score_ua_edge == rhs.score_ua_edge &&
         lhs.max_trace_choices == rhs.max_trace_choices;
}

inline void chart_spr_force_canonical_evidence_failure_for_tests(
    chart_spr_candidate_score const& candidate) {
  if (candidate.force_canonical_evidence_failure_for_tests) {
    throw std::runtime_error(
        "chart-SPR canonical report: forced exact-evidence failure for test");
  }
}

inline void chart_spr_force_candidate_exact_bnb_overflow_for_tests(
    chart_spr_candidate_score const& candidate) {
  if (candidate.force_candidate_exact_bnb_overflow_for_tests) {
    throw std::overflow_error(
        "forced candidate exact B&B arithmetic overflow for tests");
  }
}

// Source-compatible custom-verifier surface retained from the pre-Phase-6
// API. Legacy callbacks are always invoked as singleton candidate waves: they
// have no task-local counter sink and therefore cannot safely participate in
// candidate-parallel execution.
using chart_spr_fixed_topology_verifier = std::function<
    chart_spr_candidate_score(chart_spr_search_state const&,
                              chart_spr_candidate_score)>;

// Phase-6 internal/context-aware surface. The explicit context supplies the
// task-private counter sink, immutable old trim, and optional inner scheduler
// needed to make a declared-parallel-safe callback candidate-parallel.
using chart_spr_contextual_fixed_topology_verifier =
    std::function<chart_spr_candidate_score(
        chart_spr_search_state const&, chart_spr_candidate_score,
        chart_spr_exact_verification_context&)>;

// Phase 9 (Work item 4a, technique 2) hook: when installed on the search
// state (by the local-commit substrate builder), the exact_multisite
// acceptance gate verifies each candidate by transiently extending the
// overlay chain in reader-local scratch storage, reading the exact frontier on
// the extended grammar, and discarding.
// The hook is reader-local (never mutates the shared cache) and therefore
// bypasses the Phase 4 commit barrier, so transient extensions can run in
// parallel with other scoring readers under the epoch/snapshot model.  When
// absent, the exact_multisite gate falls back to the cold from-scratch path
// (`verify_candidate_exact_against_state`).
//
// The production B&B rebuilds its exact setup from the materialized grammar;
// scratch inside/outside caches are therefore constructed only when the
// test/diagnostic two-chart oracle requests them.
using chart_spr_exact_multisite_verifier =
    std::function<chart_spr_candidate_score(
        chart_spr_search_state const&, chart_spr_candidate_score,
        checked_chart_execution_plan_ref const&, chart_scheduler&,
        multisite_trim_options const&)>;

using chart_spr_contextual_exact_multisite_verifier =
    std::function<chart_spr_candidate_score(
        chart_spr_search_state const&, chart_spr_candidate_score,
        checked_chart_execution_plan_ref const&,
        chart_spr_exact_verification_context&, multisite_trim_options const&)>;

// Owner-neutral source for a finalized current-state exact setup.  A
// local-commit substrate can project its immutable persistent inside rows into
// an owning setup without exposing cache types in this header.  The returned
// setup must retain no state/cache/provider borrow; common trim construction
// and work-counter recording remain authoritative at the call site.
using chart_spr_exact_setup_provider = std::function<multisite_exact_setup(
    chart_spr_search_state const&, checked_chart_execution_plan_ref const&)>;
using chart_spr_scheduled_exact_setup_provider =
    std::function<multisite_exact_setup(
        chart_spr_search_state const&, checked_chart_execution_plan_ref const&,
        chart_scheduler&, std::vector<chart_scheduler_run_summary>*)>;

struct affected_clade_distribution {
  double mean = 0.0;
  std::size_t p50 = 0;
  std::size_t p95 = 0;
  std::size_t max = 0;
};

struct chart_spr_iteration_result {
  std::size_t iteration = 0;
  chart_spr_acceptance_mode acceptance_mode =
      chart_spr_acceptance_mode::exact_multisite;
  chart_spr_candidate_selection_mode candidate_selection =
      chart_spr_candidate_selection_mode::lower_bound_top_k;
  chart_spr_candidate_generation_stats candidate_generation;
  std::size_t candidates_generated = 0;
  std::size_t candidates_scored = 0;
  std::size_t candidates_exact_verified = 0;
  std::size_t candidate_score_failures = 0;
  std::size_t local_improving_candidates = 0;
  std::size_t locally_ranked_candidates_retained = 0;
  bool unverified_candidates_may_contain_improvements = false;
  std::string no_accept_reason;
  affected_clade_distribution affected_distribution;
  std::vector<std::size_t> affected_clade_counts;
  std::optional<chart_spr_candidate_score> accepted;
  bool accepted_move_committed = false;
  // Product-level evidence for the single-writer accepted-state transaction.
  // These are deltas of the persistent dense (pattern, clade) cache counters
  // across this iteration's successful local commit.  They remain zero for
  // no-accept/rejected iterations and for conservative materialize-rebuild
  // mode, whose rebuild accounting has a separate counter contract.
  std::size_t accepted_inside_rows_recomputed = 0;
  std::size_t accepted_outside_rows_recomputed = 0;
  // Captured against the pre-commit grammar so every committed iteration can
  // report the accepted candidate after the mutable tip advances.
  std::string accepted_candidate_signature;
  bool post_materialization_rejected = false;
  std::string post_materialization_rejection_reason;
  bool reused_patterns_after_accept = false;
  std::uint64_t post_materialization_rebuilt_score = 0;
  std::uint64_t state_score_before = 0;
  std::uint64_t state_score_after = 0;
  double candidate_generation_ms = 0.0;
  double local_scoring_ms = 0.0;
  double exact_verification_ms = 0.0;
  // One entry per exact-verification attempt, in stable ranked-candidate
  // order.  This is diagnostic accounting and is excluded from canonical
  // semantic comparisons.
  std::vector<double> exact_candidate_verification_ms;

  // Populated only when chart_spr_search_options::semantic_capture is not
  // off.  The stream includes invalid and non-retained candidates; the normal
  // search result historically retains only the accepted candidate.
  std::uint32_t canonical_seed = 0;
  std::vector<chart_spr_canonical_candidate_record> canonical_candidates;
  std::vector<std::size_t> canonical_ranked_stream_indices;
  std::vector<std::size_t> canonical_exact_verified_stream_indices;
  std::optional<chart_spr_canonical_exact_evidence>
      canonical_state_exact_before;
};

enum class chart_spr_cache_strategy {
  all_active_patterns,
  pattern_batches,
  lazy_multisite_chart,
};

namespace chart_spr_search_detail {

class chart_spr_candidate_invalid_error : public std::runtime_error {
 public:
  explicit chart_spr_candidate_invalid_error(std::string_view reason)
      : std::runtime_error(std::string{reason}) {}
};

// Finite lazy-local admission supports a bounded diagnostic surface. Current
// internal candidate-validation messages are substantially shorter than this
// policy limit; bounding future diagnostics by policy makes their resident
// string and exception-message overlap provable instead of relying on an
// arbitrary per-record byte allowance. Oversized future diagnostics are
// replaced by the fixed message below, preserving a deterministic fail-closed
// result without growing task storage past the admitted envelope.
inline constexpr std::size_t lazy_local_invalid_reason_max_size = 255;
inline constexpr std::string_view lazy_local_oversized_invalid_reason =
    "chart SPR lazy local score: diagnostic exceeded finite admission bound";
static_assert(lazy_local_oversized_invalid_reason.size() <=
              lazy_local_invalid_reason_max_size);

inline std::string_view bounded_lazy_local_invalid_reason(
    std::string_view reason) noexcept {
  return reason.size() <= lazy_local_invalid_reason_max_size
             ? reason
             : lazy_local_oversized_invalid_reason;
}

inline std::size_t local_capacity_checked_add(std::size_t lhs, std::size_t rhs,
                                              std::string_view context) {
  if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
    throw std::overflow_error(std::string{context} + " byte overflow");
  }
  return lhs + rhs;
}

inline std::size_t local_capacity_checked_multiply(std::size_t lhs,
                                                   std::size_t rhs,
                                                   std::string_view context) {
  if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
    throw std::overflow_error(std::string{context} + " byte overflow");
  }
  return lhs * rhs;
}

inline std::size_t lazy_local_invalid_reason_owned_capacity_bound() {
  return local_capacity_checked_add(
      std::max(lazy_local_invalid_reason_max_size, std::string{}.capacity()), 1,
      "chart SPR lazy-local diagnostic capacity");
}

// A thrown ordinary internal diagnostic can keep its runtime_error payload
// alive in the scheduler exception slot until the join. The candidate result
// string is pre-reserved separately; this charge covers the exception object,
// allocator/control metadata, and one bounded message payload. Hard memory
// exceptions propagate and are never converted into candidate invalidation.
inline std::size_t lazy_local_worker_error_transient_capacity_bound() {
  return local_capacity_checked_add(
      lazy_local_invalid_reason_owned_capacity_bound(), 16 * sizeof(void*),
      "chart SPR lazy-local worker diagnostic transient");
}

enum class lazy_local_worker_failure_kind : std::uint8_t {
  none,
  plan_identity,
  missing_lazy_chart,
  scratch_invariant,
  arithmetic_overflow,
  length_failure,
  allocation_failure,
  forced_for_tests,
};

// Worker-side invariant failures are fixed-size status, never biological
// candidate invalidity. The coordinator throws the lowest-slot failure after
// the join, preserving deterministic hard-failure selection without retaining
// worker exception strings.
class lazy_local_worker_failure_error : public std::exception {
 public:
  lazy_local_worker_failure_error(lazy_local_worker_failure_kind kind,
                                  std::size_t slot) noexcept
      : kind_(kind), slot_(slot) {}

  [[nodiscard]] char const* what() const noexcept override {
    return "chart SPR lazy-local worker invariant failure";
  }
  [[nodiscard]] lazy_local_worker_failure_kind kind() const noexcept {
    return kind_;
  }
  [[nodiscard]] std::size_t slot() const noexcept { return slot_; }

 private:
  lazy_local_worker_failure_kind kind_;
  std::size_t slot_;
};

template <class T>
inline std::size_t local_owned_dynamic_capacity_bytes(T const&) noexcept {
  return 0;
}

template <class T, class Allocator>
inline std::size_t local_vector_dynamic_capacity_bytes(
    std::vector<T, Allocator> const& values) {
  return local_capacity_checked_multiply(
      values.capacity(), sizeof(T), "chart SPR lazy-local vector capacity");
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    std::string const& value) {
  // This intentionally counts SSO capacity too. It is conservative, stable on
  // the frozen toolchain, and avoids depending on an implementation-specific
  // SSO threshold in the admission contract.
  return local_capacity_checked_add(value.capacity(), 1,
                                    "chart SPR lazy-local string capacity");
}

inline std::size_t local_owned_dynamic_capacity_bytes(clade_key const&);
inline std::size_t local_owned_dynamic_capacity_bytes(
    production_child_witness const&);
inline std::size_t local_owned_dynamic_capacity_bytes(
    production_witness const&);
inline std::size_t local_owned_dynamic_capacity_bytes(
    grammar_production const&);
inline std::size_t local_owned_dynamic_capacity_bytes(
    overlay_grammar_production const&);
inline std::size_t local_owned_dynamic_capacity_bytes(taxon_registry const&);
inline std::size_t local_owned_dynamic_capacity_bytes(clade_grammar const&);
inline std::size_t local_owned_dynamic_capacity_bytes(
    overlay_materialization_result const&);
inline std::size_t local_owned_dynamic_capacity_bytes(
    grammar_spr_candidate const&);

template <class T, class Allocator>
inline std::size_t local_owned_dynamic_capacity_bytes(
    std::vector<T, Allocator> const& values) {
  auto total = local_vector_dynamic_capacity_bytes(values);
  for (auto const& value : values) {
    total = local_capacity_checked_add(
        total, local_owned_dynamic_capacity_bytes(value),
        "chart SPR lazy-local nested capacity");
  }
  return total;
}

template <class T>
inline std::size_t local_owned_dynamic_capacity_bytes(
    std::optional<T> const& value) {
  return value ? local_owned_dynamic_capacity_bytes(*value) : 0;
}

template <class Key, class Value, class Hash, class Equal, class Allocator>
inline std::size_t local_owned_dynamic_capacity_bytes(
    std::unordered_map<Key, Value, Hash, Equal, Allocator> const& values) {
  auto total =
      local_capacity_checked_multiply(values.bucket_count(), sizeof(void*),
                                      "chart SPR lazy-local hash buckets");
  total = local_capacity_checked_add(
      total,
      local_capacity_checked_multiply(
          values.size(),
          sizeof(typename std::remove_cvref_t<decltype(values)>::value_type) +
              2 * sizeof(void*),
          "chart SPR lazy-local hash nodes"),
      "chart SPR lazy-local hash capacity");
  for (auto const& [key, value] : values) {
    total = local_capacity_checked_add(total,
                                       local_owned_dynamic_capacity_bytes(key),
                                       "chart SPR lazy-local hash keys");
    total = local_capacity_checked_add(
        total, local_owned_dynamic_capacity_bytes(value),
        "chart SPR lazy-local hash values");
  }
  return total;
}

template <class... Ts>
inline std::size_t local_owned_dynamic_capacity_sum(Ts const&... values) {
  std::size_t total = 0;
  ((total = local_capacity_checked_add(
        total, local_owned_dynamic_capacity_bytes(values),
        "chart SPR lazy-local owned capacity")),
   ...);
  return total;
}

inline std::size_t local_owned_dynamic_capacity_bytes(clade_key const& value) {
  return local_owned_dynamic_capacity_bytes(value.taxa);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    production_child_witness const& value) {
  return local_owned_dynamic_capacity_bytes(value.edge_alternatives);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    production_witness const& value) {
  return local_owned_dynamic_capacity_bytes(value.children);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    grammar_production const& value) {
  return local_owned_dynamic_capacity_sum(value.children, value.witnesses);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    overlay_grammar_production const& value) {
  return local_owned_dynamic_capacity_sum(value.children, value.witnesses);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    taxon_registry const& value) {
  return local_owned_dynamic_capacity_sum(value.id_to_sample_id,
                                          value.sample_id_to_id);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    clade_grammar const& value) {
  return local_owned_dynamic_capacity_sum(
      value.taxa, value.clades, value.productions, value.productions_by_parent,
      value.productions_by_child, value.node_to_clade);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    overlay_materialization_result const& value) {
  return local_owned_dynamic_capacity_sum(
      value.grammar, value.dense_clade_to_ref, value.dense_production_to_ref,
      value.base_clade_to_dense, value.temp_clade_to_dense,
      value.base_production_to_dense, value.temp_production_to_dense);
}

inline std::size_t local_owned_dynamic_capacity_bytes(
    grammar_spr_candidate const& value) {
  return local_owned_dynamic_capacity_sum(
      value.removed_productions, value.added_clades, value.added_productions,
      value.source_tree_move, value.source_before_topology_productions,
      value.source_after_topology_productions);
}

// Allocation-free residence visible through a borrowed candidate span. The
// logical candidate objects and every nested owned capacity remain live while
// their prepared descriptors are built and scored. A span cannot reveal
// surplus capacity in its owner's outer vector (or unrelated owner/control
// allocations); direct finite callers must report that invisible slack via
// local_spr_score_options::admission_additional_resident_bytes.
inline std::size_t estimate_lazy_local_candidate_input_resident_bytes(
    std::span<grammar_spr_candidate const> candidates) {
  auto total = local_capacity_checked_multiply(
      candidates.size(), sizeof(grammar_spr_candidate),
      "chart SPR lazy-local candidate input objects");
  for (auto const& candidate : candidates) {
    total = local_capacity_checked_add(
        total, local_owned_dynamic_capacity_bytes(candidate),
        "chart SPR lazy-local candidate input capacity");
  }
  return total;
}

// Internal two-stage publication policy used by local-commit orchestration.
// Public builders retain the completed-state default. When a resolved
// non-lazy state is deferred, the local substrate becomes the sole owner of
// the initial dense recurrence and must finalize the state before any score or
// exact-trim consumer is called. The historical field name is retained for
// source compatibility.
struct chart_spr_state_build_policy {
  bool defer_pattern_batch_bootstrap_to_local_cache = false;
};

}  // namespace chart_spr_search_detail

inline char const* chart_spr_cache_strategy_name(
    chart_spr_cache_strategy strategy) {
  switch (strategy) {
    case chart_spr_cache_strategy::all_active_patterns:
      return "all_active_patterns";
    case chart_spr_cache_strategy::pattern_batches:
      return "pattern_batches";
    case chart_spr_cache_strategy::lazy_multisite_chart:
      return "lazy_multisite_chart";
  }
  return "unknown";
}

struct chart_spr_search_summary {
  std::size_t iterations = 0;
  std::size_t accepted_moves = 0;
  std::size_t candidates_generated = 0;
  std::size_t candidates_locally_scored = 0;
  std::size_t local_rows_recomputed = 0;
  std::size_t local_unit_fitch_fast_path_productions_scored = 0;
  std::size_t local_leaf_state_view_uses = 0;
  std::size_t local_leaf_state_owned_copies = 0;
  std::size_t local_row_scratch_capacity_growths = 0;
  std::size_t local_commit_inside_row_view_pattern_visits = 0;
  std::size_t multifurcation_productions_scored = 0;
  std::size_t lazy_local_admission_waves = 0;
  std::size_t lazy_local_parallel_waves = 0;
  std::size_t lazy_local_memory_limited_waves = 0;
  std::size_t lazy_local_admitted_concurrency_max = 0;
  std::size_t lazy_local_prepared_tasks = 0;
  std::size_t lazy_local_reused_prepared_tasks = 0;
  std::size_t lazy_local_pre_submit_budget_failures = 0;
  std::size_t lazy_local_peak_admitted_bytes = 0;
  std::size_t lazy_local_peak_projected_resident_bytes = 0;
  std::size_t lazy_local_preparation_peak_bytes = 0;
  std::size_t lazy_local_result_output_resident_bytes_max = 0;
  std::size_t lazy_local_retained_exact_trim_bytes_max = 0;
  std::size_t lazy_local_canonical_exact_evidence_resident_bytes_max = 0;
  std::size_t
      lazy_local_canonical_exact_evidence_construction_peak_bytes_max = 0;
  std::size_t lazy_local_runtime_transient_reservation_bytes_max = 0;
  std::size_t lazy_local_iteration_envelope_bytes_max = 0;
  std::size_t lazy_local_iteration_generation_phase_bytes_max = 0;
  std::size_t lazy_local_iteration_evidence_phase_bytes_max = 0;
  std::size_t lazy_local_ranked_candidate_exact_evidence_bytes_max = 0;
  std::size_t lazy_local_iteration_task_stable_bytes_max = 0;
  std::size_t lazy_local_iteration_task_preparation_peak_bytes_max = 0;
  std::size_t candidate_batches_scored = 0;
  std::size_t candidate_pipeline_batches_generated = 0;
  std::size_t candidate_pipeline_batches_scored = 0;
  std::size_t candidate_pipeline_serial_overlap_batches = 0;
  std::size_t candidate_pipeline_scheduler_projection_overlap_batches = 0;
  std::size_t candidate_pipeline_producer_stalls = 0;
  std::size_t candidate_pipeline_consumer_stalls = 0;
  double candidate_pipeline_producer_stall_ms = 0.0;
  double candidate_pipeline_consumer_stall_ms = 0.0;
  std::size_t candidate_pipeline_cancellations = 0;
  std::size_t candidate_pipeline_stale_batches_discarded = 0;
  std::size_t candidate_pipeline_stale_candidates_discarded = 0;
  std::size_t candidate_pipeline_state_epoch_rejections = 0;
  std::size_t candidate_pipeline_generation_errors = 0;
  std::size_t candidate_pipeline_estimated_peak_bytes = 0;
  std::size_t pattern_batch_cache_builds = 0;
  std::size_t initial_state_inside_charts_built = 0;
  std::size_t inside_cache_inside_charts_built = 0;
  std::size_t inside_cache_resident_inside_charts_consumed = 0;
  std::size_t exact_setup_builds = 0;
  std::size_t exact_setup_inside_charts_built = 0;
  std::size_t exact_setup_resident_inside_charts_consumed = 0;
  std::size_t exact_setup_active_leaf_state_vectors_copied = 0;
  std::size_t exact_setup_active_leaf_states_copied = 0;
  std::size_t exact_setup_outside_boundary_charts_built = 0;
  std::size_t exact_setup_upper_bound_topologies_generated = 0;
  std::size_t exact_setup_upper_bound_topologies_unique = 0;
  std::size_t exact_setup_frontier_passes = 0;
  std::size_t exact_bnb_levels = 0;
  std::size_t exact_bnb_clades = 0;
  std::size_t exact_bnb_product_combinations = 0;
  std::size_t exact_bnb_frontier_entries = 0;
  double exact_bnb_ms = 0.0;
  std::size_t exact_trim_lazy_chart_uses = 0;
  std::size_t outside_cache_inside_charts_built = 0;
  std::size_t outside_cache_inside_charts_reused = 0;
  std::size_t outside_cache_outside_charts_built = 0;
  std::size_t chart_execution_plan_builds = 0;
  std::size_t chart_execution_plan_cache_hits = 0;
  std::size_t candidate_execution_plan_builds = 0;
  std::size_t candidate_execution_plan_cache_hits = 0;
  std::size_t full_grammar_validations = 0;
  std::size_t production_index_validations = 0;
  std::size_t production_partition_validations = 0;
  std::size_t dynamic_overlay_payload_partition_validations = 0;
  std::size_t candidate_partition_validations = 0;
  std::size_t clade_order_sorts = 0;
  std::size_t production_descriptors_compiled = 0;
  std::size_t plan_mismatch_rejections = 0;
  std::size_t candidate_pattern_full_grammar_validations = 0;
  std::size_t candidate_pattern_partition_validations = 0;
  std::size_t candidate_pattern_clade_order_sorts = 0;
  std::size_t exact_verifications = 0;
  std::size_t accepted_exact_trims_reused = 0;
  std::size_t accepted_exact_trim_reuse_rejections = 0;
  std::size_t overlay_materializations_for_exact_verification = 0;
  std::size_t overlay_materializations_for_accept_materialization = 0;
  std::size_t overlay_materializations_for_final_compaction = 0;
  std::size_t sidecar_rebuilds_after_accept = 0;
  std::size_t initial_search_state_rebuilds = 0;
  std::size_t full_search_state_rebuilds = 0;
  // Local-commit final compaction performs one safety rebuild from the output
  // DAG and scores it with the Phase-5 grammar-valued exact oracle. Keep it
  // separate from per-accepted-move sidecar rebuilds so benchmark reports can
  // distinguish amortized final verification cost.
  std::size_t final_compaction_rebuilds = 0;
  std::size_t candidate_accepts_attempted = 0;
  std::size_t post_materialization_rejections = 0;
  // Phase 4 local-commit visibility (mirror of the counter-contract fields so
  // a regression to "full rebuild per accept" is visible in benchmark/CI
  // tables without drilling into the counters struct).
  std::size_t local_commit_accepted_moves = 0;
  std::size_t local_commit_tombstone_scope_skips = 0;
  std::size_t inside_rows_recomputed_on_commit = 0;
  std::size_t outside_rows_recomputed_on_commit = 0;
  std::size_t lazy_inside_rows_computed = 0;
  std::size_t lazy_outside_rows_computed = 0;
  std::size_t lazy_patterns_merged_max = 0;
  std::size_t lazy_remerge_collisions = 0;
  std::size_t lazy_inside_rows_recomputed_on_commit = 0;
  std::size_t lazy_outside_rows_recomputed_on_commit = 0;
  std::size_t lazy_incremental_rows_recomputed = 0;
  std::size_t lazy_structural_class_count_max = 0;
  std::size_t lazy_chart_memory_budget_bytes = 0;
  std::size_t lazy_chart_inside_max_admitted_slots = 0;
  std::size_t lazy_chart_outside_max_admitted_slots = 0;
  std::size_t lazy_chart_inside_admission_waves = 0;
  std::size_t lazy_chart_outside_admission_waves = 0;
  std::size_t lazy_chart_inside_memory_limited_levels = 0;
  std::size_t lazy_chart_outside_memory_limited_levels = 0;
  std::size_t lazy_chart_inside_reused_slot_waves = 0;
  std::size_t lazy_chart_outside_reused_slot_waves = 0;
  std::size_t lazy_chart_inside_workspace_evictions = 0;
  std::size_t lazy_chart_outside_workspace_evictions = 0;
  std::size_t lazy_chart_preflight_peak_bytes = 0;
  std::size_t lazy_chart_actual_peak_bytes = 0;
  std::size_t lazy_chart_pre_submit_rejections = 0;
  std::size_t lazy_policy_pilot_runs = 0;
  std::size_t lazy_policy_frozen_reuses = 0;
  std::size_t lazy_internal_structural_class_count_max = 0;
  double lazy_merge_ratio = 0.0;
  double lazy_internal_structural_class_ratio = 0.0;
  std::size_t local_commit_two_chart_oracle_runs = 0;
  std::size_t local_commit_tip_grammar_refreshes = 0;
  std::size_t fixed_topology_selected_cache_hits = 0;
  std::size_t fixed_topology_selected_cache_misses = 0;
  std::size_t fixed_topology_selected_rows_computed = 0;
  std::size_t selected_topology_class_rows_computed = 0;
  std::size_t selected_topology_multifurcation_rows = 0;
  std::size_t spr_multifurcation_moves_generated = 0;
  std::size_t fixed_topology_persistent_cache_verifications = 0;
  std::size_t fixed_topology_persistent_cache_fallbacks = 0;
  std::size_t fixed_topology_persistent_cache_oracle_mismatches = 0;
  std::size_t fixed_topology_persistent_cache_direct_oracle_mismatches = 0;
  std::size_t fixed_topology_icache_rows_reused = 0;
  std::size_t fixed_topology_icache_rows_recomputed_affected = 0;
  std::size_t fixed_topology_chain_objective_before_mismatches = 0;
  // Phase 9 transient chain extension for grammar-exact verification.
  // Mirrored from the counters so a regression to "dense materialize per
  // candidate" (full_overlay_materializations > 0 on an exact local-commit
  // run) is visible in CI/benchmark tables without drilling into counters.
  std::size_t transient_chain_extensions_for_verification = 0;
  std::size_t transient_chain_diagnostic_cache_extensions = 0;
  std::size_t transient_chain_extension_fallbacks = 0;
  std::size_t transient_chain_extension_oracle_mismatches = 0;
  chart_spr_candidate_selection_mode candidate_selection =
      chart_spr_candidate_selection_mode::lower_bound_top_k;
  chart_spr_acceptance_mode acceptance_mode =
      chart_spr_acceptance_mode::exact_multisite;
  std::uint64_t initial_score = 0;
  std::uint64_t final_score = 0;
  double total_ms = 0.0;
  double cache_build_ms = 0.0;
  // Local-commit substrate construction is reported separately from the
  // search-state representation and exact frontier.  These spans are
  // disjoint: the inside span owns any deferred non-lazy recurrence, and
  // the outside span consumes the completed inside cache.
  double local_inside_cache_initialization_ms = 0.0;
  double local_outside_cache_initialization_ms = 0.0;
  // The actual initial dense (all-active or pattern-batched) or lazy
  // inside+outside chart construction span.  Unlike cache_build_ms this does
  // not include pattern extraction, grammar validation, cache sizing, or
  // exact-trim initialization.
  double initial_chart_construction_ms = 0.0;
  double candidate_generation_ms = 0.0;
  double exact_initialization_ms = 0.0;
  double local_scoring_ms = 0.0;
  double local_candidates_per_second = 0.0;
  double local_rows_recomputed_per_second = 0.0;
  double exact_verification_ms = 0.0;
  // Disjoint materialization spans.  materialization_ms is exactly their sum;
  // see the counter fields above for the inclusion/exclusion contract.
  double materialization_ms = 0.0;
  double materialization_exact_verification_ms = 0.0;
  double materialization_accepted_update_ms = 0.0;
  double materialization_final_compaction_ms = 0.0;
  // Observed high-water mark from real verifier entry/exit tracking.  It is
  // zero when no verifier runs and may exceed one for candidate-parallel
  // admission waves.
  std::size_t peak_concurrent_exact_verifiers = 0;
  std::size_t exact_candidate_admission_batches = 0;
  std::size_t exact_candidate_parallel_batches = 0;
  std::size_t exact_candidate_inner_parallel_batches = 0;
  std::size_t exact_candidate_memory_limited_batches = 0;
  std::size_t exact_candidate_peak_admitted_bytes = 0;
  std::size_t exact_candidate_peak_projected_resident_bytes = 0;
  double exact_candidate_queued_for_memory_ms = 0.0;
  std::size_t exact_candidate_timing_count = 0;
  double exact_candidate_verification_ms_min = 0.0;
  double exact_candidate_verification_ms_mean = 0.0;
  double exact_candidate_verification_ms_max = 0.0;
  double accepted_rebuild_ms = 0.0;
  double final_compaction_ms = 0.0;
  multisite_keep_mask_kind final_compaction_exactness_kind =
      multisite_keep_mask_kind::none;
  double post_materialization_check_ms = 0.0;
  std::size_t active_pattern_count = 0;
  std::size_t initial_grammar_clade_count = 0;
  std::size_t initial_grammar_production_count = 0;
  std::size_t final_grammar_clade_count = 0;
  std::size_t final_grammar_production_count = 0;
  std::size_t chart_cache_estimated_full_bytes = 0;
  std::size_t chart_cache_resident_bytes = 0;
  chart_spr_cache_strategy cache_strategy =
      chart_spr_cache_strategy::all_active_patterns;
  chart_spr_lazy_policy_diagnostics lazy_policy;
  std::size_t effective_pattern_batch_size = 0;
  std::size_t effective_candidate_batch_size = 0;
  std::size_t requested_worker_count = 1;
  std::size_t resolved_worker_count = 1;
  std::size_t local_score_worker_count = 1;
  // Search-lifetime orchestration metrics are kept out of the mutable search
  // state because accepted-state rebuilds replace that state.  The scheduler
  // is shut down before this snapshot is published, so pending tasks and live
  // pool threads must both be zero in every returned result.
  chart_scheduler_metrics scheduler;
  chart_spr_scheduler_axis_counters scheduler_axes;
  affected_clade_distribution affected_distribution;
  // Phase 10 cross-cutting surface.  These mirror the selected commit /
  // verification modes and the chain's per-accept exactness label so a run's
  // emitted report carries the contracted mode labels and every accepted
  // move's exactness kind (Work item 1 exactness contract).  The per-accept
  // label equals the acceptance mode for local-commit runs (fixed_topology_exact
  // / exact_multisite); for the conservative materialize-rebuild path it is the
  // constant `none_conservative_materialize_rebuild` (there is no overlay chain,
  // so there is no per-accept chain exactness to report).  Note this label is the
  // CHAIN's per-accept label, not the objective's exactness kind: a
  // lower_bound_heuristic acceptance gate still reports its score with kind
  // `composite_lower_bound` (see `chart_spr_score_kind`); it is simply never
  // admitted to local commit (see validate_chart_spr_search_loop_options).
  chart_spr_commit_mode commit_mode = chart_spr_commit_mode::overlay_delta;
  chart_spr_verification_mode verification_mode =
      chart_spr_verification_mode::transient;
  std::string chain_per_accept_exactness_label;
};

struct chart_spr_search_result {
  phylo_dag dag;
  std::vector<chart_spr_iteration_result> iterations;
  chart_spr_search_counters counters;
  chart_spr_search_summary summary;
  // Phase 10 identity surface: the JSON identity report of the overlay chain
  // (one entry per accepted delta, with taxon-set keys + commit-source label),
  // emitted when the run used local-commit mode (`rebuild_after_accept =
  // false`) so the chain existed.  Empty for conservative materialize-rebuild
  // runs and for local-commit runs that accepted nothing.  Built from
  // `build_phase10_chain_identity_report` (phase10_report.hpp); the keys are
  // stable across materialize / rebuild / report round trips.
  std::string chain_identity_report_json;
  std::optional<chart_spr_canonical_report> canonical_report;
  std::optional<chart_spr_semantic_digest_report> canonical_digest;
};

// Search-cache objective convention:
//
// * chart_spr_search_state stores only active/topology-informative patterns in
//   active_site_pattern_set.  Invariant sites are omitted from the hot chart
//   cache and represented by invariant_constant_offset plus
//   skipped_invariant_site_count on the state.
// * Internal cache/B&B/oracle values are active-only unless a field name or
//   chart_spr_score_convention explicitly says full_with_invariants.
// * The invariant offset is added exactly once at report/comparison/commit
//   boundaries.
// * The active wrapper below mechanically rejects any site_pattern_set carrying
//   skipped-invariant metadata or invariant patterns.
// * Root rows are scored through chart_spr_weighted_root_score_from_row() so
//   score_ua_edge=true compressed patterns use per-reference-state counts.
struct active_site_pattern_set {
  site_pattern_set patterns;

  // Search-state internals are active/topology-informative only.  Invariant
  // sites and their topology-independent score contribution live on
  // chart_spr_search_state, so this wrapper must not carry skipped-invariant
  // metadata or invariant patterns into active-only chart/B&B calls.
  void assert_no_skipped_invariant_metadata() const {
    if (patterns.skipped_invariant_site_count != 0) {
      throw std::runtime_error(
          "chart SPR active patterns: skipped invariant site metadata must be "
          "owned by the search state");
    }
    if (patterns.skipped_invariant_constant_score_with_reference_edge != 0) {
      throw std::runtime_error(
          "chart SPR active patterns: skipped invariant score metadata must be "
          "owned by the search state");
    }
    if (patterns.invariant_site_count != 0 ||
        patterns.invariant_constant_score_excluding_ua != 0 ||
        patterns.invariant_constant_score_with_reference_edge != 0) {
      throw std::runtime_error(
          "chart SPR active patterns: invariant-site metadata must be owned "
          "by the search state");
    }
    for (std::size_t i = 0; i < patterns.patterns.size(); ++i) {
      if (is_invariant_site_pattern(patterns.patterns[i])) {
        throw std::runtime_error(
            "chart SPR active patterns: invariant pattern at active index " +
            std::to_string(i));
      }
    }
  }
};

struct inside_chart_cache_active_pattern_fingerprint {
  static constexpr std::uint32_t current_schema_version = 1;

  std::array<std::uint64_t, 2> structure{};
  std::uint32_t schema_version = current_schema_version;

  bool operator==(inside_chart_cache_active_pattern_fingerprint const&) const =
      default;
};

namespace inside_chart_cache_detail {

// Shared identity for persistent-cache provenance and accepted exact-frontier
// reuse. Include every semantic field so neither rows nor a trim can survive
// an in-place mutation of the active pattern set.
inline inside_chart_cache_active_pattern_fingerprint
fingerprint_active_pattern_set(active_site_pattern_set const& active) {
  inside_chart_cache_active_pattern_fingerprint result;
  auto& first = result.structure[0];
  auto& second = result.structure[1];
  first = 0x6a09e667f3bcc909ULL;
  second = 0xbb67ae8584caa73bULL;

  auto mix = [&](std::uint64_t value) {
    chart_execution_plan_detail::fingerprint_mix(first, value);
    chart_execution_plan_detail::fingerprint_mix(second,
                                                 value ^ 0xd6e8feb86659fd93ULL);
  };
  auto mix_state_map = [&](normalized_binary_state_map const& map) {
    mix(map.exact_pattern);
    mix(map.normalized_binary_pattern);
    for (auto state : map.normalized_to_original) mix(state);
    for (auto state : map.original_to_normalized) mix(state);
  };
  auto const& patterns = active.patterns;

  mix(result.schema_version);
  mix(patterns.taxon_count);
  mix(patterns.patterns.size());
  for (auto const& pattern : patterns.patterns) {
    mix(pattern.state_by_taxon.size());
    for (auto state : pattern.state_by_taxon) mix(state);
    mix(pattern.positions.size());
    for (auto position : pattern.positions) mix(position);
    mix(pattern.weight);
    for (auto count : pattern.reference_state_counts) mix(count);
  }

  mix(patterns.original_site_to_pattern.size());
  for (auto pattern : patterns.original_site_to_pattern) mix(pattern);

  mix(patterns.normalized_binary_patterns.size());
  for (auto const& pattern : patterns.normalized_binary_patterns) {
    mix(pattern.state_by_taxon.size());
    for (auto state : pattern.state_by_taxon) mix(state);
    mix(pattern.positions.size());
    for (auto position : pattern.positions) mix(position);
    mix(pattern.weight);
    mix(pattern.exact_pattern_indices.size());
    for (auto pattern_index : pattern.exact_pattern_indices) {
      mix(pattern_index);
    }
    mix(pattern.exact_state_maps.size());
    for (auto const& map : pattern.exact_state_maps) mix_state_map(map);
  }

  mix(patterns.exact_pattern_to_normalized_binary_pattern.size());
  for (auto pattern : patterns.exact_pattern_to_normalized_binary_pattern) {
    mix(pattern);
  }
  mix(patterns.exact_pattern_to_normalized_binary_state_map.size());
  for (auto const& map :
       patterns.exact_pattern_to_normalized_binary_state_map) {
    mix_state_map(map);
  }

  mix(patterns.total_site_count);
  mix(patterns.invariant_site_count);
  mix(patterns.variable_site_count);
  mix(patterns.binary_variable_site_count);
  mix(patterns.nonbinary_variable_site_count);
  mix(patterns.skipped_invariant_site_count);
  mix(patterns.invariant_constant_score_excluding_ua);
  mix(patterns.invariant_constant_score_with_reference_edge);
  mix(patterns.skipped_invariant_constant_score_with_reference_edge);
  return result;
}

}  // namespace inside_chart_cache_detail

struct chart_spr_reusable_exact_trim_payload {
  chart_plan_fingerprint target_execution_fingerprint;
  inside_chart_cache_active_pattern_fingerprint active_pattern_fingerprint;
  chart_options chart_opts;
  multisite_trim_options trim_options;
  std::uint64_t invariant_constant_offset = 0;
  multisite_trim_result trim;
};

inline std::shared_ptr<chart_spr_reusable_exact_trim_payload>
make_chart_spr_reusable_exact_trim(
    chart_execution_plan const& target_plan,
    active_site_pattern_set const& active_patterns,
    chart_options const& chart_opts,
    multisite_trim_options const& trim_options,
    std::uint64_t invariant_constant_offset, multisite_trim_result trim) {
  return std::make_shared<chart_spr_reusable_exact_trim_payload>(
      chart_spr_reusable_exact_trim_payload{
          .target_execution_fingerprint = target_plan.fingerprint(),
          .active_pattern_fingerprint =
              inside_chart_cache_detail::fingerprint_active_pattern_set(
                  active_patterns),
          .chart_opts = chart_opts,
          .trim_options = trim_options,
          .invariant_constant_offset = invariant_constant_offset,
          .trim = std::move(trim)});
}

struct chart_spr_pattern_source_fingerprint {
  std::uint64_t reference_hash = 0;
  std::uint64_t sample_id_hash = 0;
  std::uint64_t compact_genome_hash = 0;
  std::uint64_t taxon_registry_hash = 0;

  bool operator==(chart_spr_pattern_source_fingerprint const&) const = default;
};

struct pattern_chart_cache_entry {
  single_site_chart chart;  // no trace in hot path

  // Keep the whole root row, not just a scalar root minimum.  With
  // score_ua_edge=true, one compressed leaf-state pattern can contain
  // positions with different UA/reference states.
  std::array<chart_cost, nuc_state_count> root_row =
      parsimony_chart_detail::make_inf_row();
  chart_cost root_min_excluding_ua = chart_inf;
  std::array<chart_cost, nuc_state_count> root_min_by_reference_state =
      parsimony_chart_detail::make_inf_row();
  std::array<std::uint64_t, nuc_state_count> reference_state_counts{};
  std::uint64_t weighted_root_score = 0;
};

// Non-owning current-tip projection over the persistent local-commit inside
// cache. The search loop keeps the backing substrate alive for every scoring
// read; readers are admitted only between commit barriers, and the complete tip
// / active-pattern identity is checked once at each scoring boundary. This view
// replaces a second owning [pattern][dense-clade] copy for non-lazy local mode.
struct chart_spr_inside_row_view {
  using row_type = std::array<chart_cost, nuc_state_count>;
  using reader_type = row_type const& (*)(void const*, std::size_t, clade_id);

  void const* context = nullptr;
  reader_type reader = nullptr;
  std::size_t pattern_count = 0;
  std::size_t clade_count = 0;
  std::uint64_t execution_generation = 0;
  chart_plan_fingerprint execution_fingerprint;
  inside_chart_cache_active_pattern_fingerprint active_pattern_fingerprint;

  [[nodiscard]] bool valid() const noexcept {
    return context != nullptr && reader != nullptr;
  }

  void assert_compatible(chart_execution_plan const& plan,
                         active_site_pattern_set const& active_patterns) const {
    if (!valid()) {
      throw std::runtime_error(
          "chart SPR inside-row view: missing local-commit row provider");
    }
    if (execution_generation != plan.grammar_generation() ||
        execution_fingerprint != plan.fingerprint() ||
        pattern_count != active_patterns.patterns.patterns.size() ||
        clade_count != plan.clades().size() ||
        active_pattern_fingerprint !=
            inside_chart_cache_detail::fingerprint_active_pattern_set(
                active_patterns)) {
      throw std::runtime_error(
          "chart SPR inside-row view: stale tip or active-pattern identity");
    }
  }

  [[nodiscard]] row_type const& row(std::size_t pattern, clade_id clade) const {
    if (!valid() || pattern >= pattern_count || clade == no_clade ||
        clade >= clade_count) {
      throw std::runtime_error(
          "chart SPR inside-row view: row index out of range");
    }
    return reader(context, pattern, clade);
  }
};

struct chart_spr_active_pattern_build_result {
  active_site_pattern_set active_patterns;
  chart_spr_pattern_source_fingerprint pattern_source_fingerprint;
  std::uint64_t invariant_constant_offset = 0;
  std::size_t skipped_invariant_site_count = 0;
};

namespace chart_spr_search_detail {

inline std::uint64_t mix_u64(std::uint64_t seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  return seed;
}

inline std::uint64_t fnv1a_append_byte(std::uint64_t seed,
                                        std::uint8_t byte) {
  return (seed ^ static_cast<std::uint64_t>(byte)) * 1099511628211ULL;
}

inline std::uint64_t hash_string_u64(std::string const& value) {
  std::uint64_t seed = 1469598103934665603ULL;
  for (unsigned char byte : value) seed = fnv1a_append_byte(seed, byte);
  return seed;
}

inline bool chart_spr_grammar_has_multifurcation(
    clade_grammar const& grammar) {
  return std::any_of(grammar.productions.begin(), grammar.productions.end(),
                     [](auto const& prod) {
                       return prod.children.size() != 2;
                     });
}

inline void validate_chart_spr_exact_multisite_multifurcation_gate(
    clade_grammar const& grammar, std::string_view context) {
  if (!chart_spr_grammar_has_multifurcation(grammar)) return;
  parsimony_chart_detail::record_arity_gate_throw(
      arity_gate_consumer::chart_spr_exact_multisite);
  throw std::runtime_error(
      std::string{context} +
      ": WI6 exact_multisite does not support multifurcating productions; "
      "use fixed_topology_exact or lower_bound_heuristic for DAG-native "
      "multifurcation SPR search");
}

inline void validate_chart_spr_search_grammar(clade_grammar const& grammar) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error("chart SPR search: root clade out of range");
  }
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto const& clade = grammar.clades[cid];
    auto const& productions = grammar.productions_by_parent[cid];
    if (clade.taxa.size() == 1) {
      if (!productions.empty()) {
        throw std::runtime_error(
            "chart SPR search: singleton clade " + std::to_string(cid) +
            " has productions; expected leaf clades to be productionless");
      }
    } else if (productions.empty()) {
      throw std::runtime_error(
          "chart SPR search: non-singleton clade " + std::to_string(cid) +
          " has no productions; DAG-native chart-SPR requires a "
          "chart-compatible grammar");
    }
  }

  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& prod = grammar.productions[pid];
    parsimony_chart_detail::validate_production_inside_row_inputs(
        grammar, prod, static_cast<production_id>(pid),
        "chart SPR search");
  }
}

inline void validate_active_patterns_for_execution_plan(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options) {
  plan.assert_valid();
  if (patterns.taxon_count != plan.taxon_count()) {
    throw std::runtime_error(
        "chart SPR search state: site-pattern set taxon count mismatch");
  }
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (pattern.state_by_taxon.size() != plan.taxon_count()) {
      throw std::runtime_error(
          "chart SPR search state: site-pattern taxon count mismatch");
    }
    for (auto state : pattern.state_by_taxon) {
      parsimony_chart_detail::validate_state(state, "site-pattern state");
    }
    if (options.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(pattern,
                                                                pattern_index);
    }
  }
}

inline std::uint64_t invariant_pattern_reference_edge_offset(
    site_pattern const& pattern) {
  if (pattern.state_by_taxon.empty()) {
    throw std::runtime_error(
        "chart SPR active patterns: invariant pattern has no taxa");
  }
  auto invariant_state = pattern.state_by_taxon.front();
  parsimony_chart_detail::validate_state(invariant_state,
                                         "invariant pattern state");
  std::uint64_t total = 0;
  std::uint64_t reference_count_sum = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    auto count = static_cast<std::uint64_t>(
        pattern.reference_state_counts[reference_state]);
    reference_count_sum += count;
    if (count == 0) continue;
    auto cost = parsimony_chart_detail::transition_cost(reference_state,
                                                        invariant_state);
    total = chart_multisite_detail::checked_add_u64(
        total,
        chart_multisite_detail::checked_mul_cost(
            count, cost, "chart-SPR invariant UA-edge offset"),
        "chart-SPR invariant UA-edge offset total");
  }
  if (reference_count_sum != pattern.weight) {
    throw std::runtime_error(
        "chart SPR active patterns: invariant reference-state counts do not "
        "sum to pattern weight");
  }
  return total;
}

inline void append_active_pattern_metadata(site_pattern_set& active,
                                           site_pattern const& pattern) {
  active.patterns.push_back(pattern);
  active.total_site_count += pattern.weight;
  active.variable_site_count += pattern.weight;
  if (is_binary_variable_site_pattern(pattern)) {
    active.binary_variable_site_count += pattern.weight;
  } else {
    active.nonbinary_variable_site_count += pattern.weight;
  }
}

inline pattern_chart_cache_entry build_pattern_chart_cache_entry(
    clade_grammar const& grammar, site_pattern const& pattern,
    chart_options const& chart_opts, chart_options const& chart_build_opts) {
  auto states = view_leaf_site_states(pattern.state_by_taxon);
  pattern_chart_cache_entry entry;
  entry.chart = build_single_site_chart(grammar, states, chart_build_opts);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= entry.chart.inside.size()) {
    throw std::runtime_error(
        "chart SPR search state: root clade out of chart range");
  }
  entry.root_row = entry.chart.inside[grammar.root_clade];
  entry.root_min_excluding_ua =
      entry.chart.root_min_excluding_ua(grammar.root_clade);
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    entry.root_min_by_reference_state[reference_state] =
        entry.chart.root_min_with_reference_edge(grammar.root_clade,
                                                 reference_state);
    entry.reference_state_counts[reference_state] =
        pattern.reference_state_counts[reference_state];
  }
  entry.weighted_root_score = chart_spr_weighted_root_score_from_row(
      entry.root_row, pattern, chart_opts);
  return entry;
}

inline pattern_chart_cache_entry build_pattern_chart_cache_entry(
    chart_execution_plan const& plan, site_pattern const& pattern,
    chart_options const& chart_opts, chart_options const& chart_build_opts) {
  auto states = view_leaf_site_states(pattern.state_by_taxon);
  pattern_chart_cache_entry entry;
  entry.chart = build_single_site_chart(plan, states, chart_build_opts);
  auto const root = plan.root_clade();
  if (root == no_clade || root >= entry.chart.inside.size()) {
    throw std::runtime_error(
        "chart SPR search state: root clade out of chart range");
  }
  entry.root_row = entry.chart.inside[root];
  entry.root_min_excluding_ua = entry.chart.root_min_excluding_ua(root);
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    entry.root_min_by_reference_state[reference_state] =
        entry.chart.root_min_with_reference_edge(root,
                                                 reference_state);
    entry.reference_state_counts[reference_state] =
        pattern.reference_state_counts[reference_state];
  }
  entry.weighted_root_score = chart_spr_weighted_root_score_from_row(
      entry.root_row, pattern, chart_opts);
  return entry;
}

inline std::size_t multifurcation_productions_scored_for_entries(
    std::vector<pattern_chart_cache_entry> const& entries) {
  std::size_t total = 0;
  for (auto const& entry : entries) {
    total += entry.chart.multifurcation_productions_scored;
  }
  return total;
}

}  // namespace chart_spr_search_detail

inline void validate_supported_chart_cache_options(
    chart_cache_options const& cache) {
  // Phase 7 supports all-active-pattern caches and explicit pattern-batch
  // scoring.  Zero values keep the historical defaults: all active patterns
  // cached when no budget is supplied, and an automatically chosen bounded
  // candidate batch when pattern batching is required for deterministic grammar
  // enumeration.  Extremely small byte budgets are treated as advisory and
  // still allow a one-pattern batch so tests and diagnostics fail by score, not
  // by allocator policy.
  switch (cache.lazy_policy) {
    case chart_spr_lazy_policy::off:
    case chart_spr_lazy_policy::on:
    case chart_spr_lazy_policy::automatic:
      break;
    default:
      throw std::invalid_argument(
          "chart SPR cache options: invalid lazy policy");
  }
}

inline chart_spr_pattern_source_fingerprint
build_chart_spr_pattern_source_fingerprint(phylo_dag& dag,
                                           clade_grammar const& grammar) {
  using chart_spr_search_detail::hash_string_u64;
  using chart_spr_search_detail::mix_u64;

  chart_spr_pattern_source_fingerprint fp;
  fp.reference_hash = hash_string_u64(get_reference_sequence(dag));

  std::uint64_t sample_seed = grammar.taxa.id_to_sample_id.size();
  for (auto const& sample_id : grammar.taxa.id_to_sample_id) {
    sample_seed = mix_u64(sample_seed, hash_string_u64(sample_id));
  }
  fp.sample_id_hash = sample_seed;

  std::vector<std::pair<std::string, std::uint64_t>> leaf_hashes;
  auto reachable = detail::collect_reachable(dag);
  for (auto node_idx : reachable.nodes) {
    auto nv = dag.get_node(node_idx);
    if (!detail::is_leaf_node(nv)) continue;
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            std::uint64_t cg_seed = 1469598103934665603ULL;
            for (auto const& [pos, base] : node.cg()) {
              cg_seed = mix_u64(cg_seed, static_cast<std::uint64_t>(pos));
              cg_seed = mix_u64(cg_seed,
                                static_cast<std::uint64_t>(base.raw()));
            }
            leaf_hashes.emplace_back(std::string{node.sample_id()}, cg_seed);
          } else {
            throw std::runtime_error(
                "chart SPR pattern fingerprint: reachable leaf node lacks "
                "sample_id/cg annotations");
          }
        },
        nv);
  }
  std::sort(leaf_hashes.begin(), leaf_hashes.end());
  std::uint64_t cg_seed = leaf_hashes.size();
  for (auto const& [sample_id, hash] : leaf_hashes) {
    cg_seed = mix_u64(cg_seed, hash_string_u64(sample_id));
    cg_seed = mix_u64(cg_seed, hash);
  }
  fp.compact_genome_hash = cg_seed;

  std::uint64_t registry_seed = grammar.taxa.id_to_sample_id.size();
  for (std::size_t id = 0; id < grammar.taxa.id_to_sample_id.size(); ++id) {
    registry_seed = mix_u64(registry_seed, static_cast<std::uint64_t>(id));
    registry_seed = mix_u64(registry_seed,
                            hash_string_u64(grammar.taxa.id_to_sample_id[id]));
  }
  std::vector<std::pair<std::string, taxon_id>> taxon_map_entries;
  taxon_map_entries.reserve(grammar.taxa.sample_id_to_id.size());
  for (auto const& [sample_id, id] : grammar.taxa.sample_id_to_id) {
    taxon_map_entries.emplace_back(sample_id, id);
  }
  std::sort(taxon_map_entries.begin(), taxon_map_entries.end());
  for (auto const& [sample_id, id] : taxon_map_entries) {
    registry_seed = mix_u64(registry_seed, hash_string_u64(sample_id));
    registry_seed = mix_u64(registry_seed, static_cast<std::uint64_t>(id));
  }
  fp.taxon_registry_hash = registry_seed;
  return fp;
}

inline bool chart_spr_pattern_source_fingerprint_matches(
    phylo_dag& dag, clade_grammar const& grammar,
    chart_spr_pattern_source_fingerprint const& expected) {
  return build_chart_spr_pattern_source_fingerprint(dag, grammar) == expected;
}

inline chart_spr_active_pattern_build_result make_active_search_patterns(
    site_pattern_set const& source_patterns,
    chart_options const& chart_opts = {}) {
  chart_spr_active_pattern_build_result result;
  auto& active = result.active_patterns.patterns;
  active.taxon_count = source_patterns.taxon_count;

  result.skipped_invariant_site_count =
      source_patterns.skipped_invariant_site_count;
  if (chart_opts.score_ua_edge) {
    result.invariant_constant_offset =
        chart_multisite_detail::checked_add_u64(
            result.invariant_constant_offset,
            source_patterns
                .skipped_invariant_constant_score_with_reference_edge,
            "chart-SPR skipped invariant UA-edge offset");
  }

  active.patterns.reserve(source_patterns.patterns.size());
  for (std::size_t pattern_index = 0;
       pattern_index < source_patterns.patterns.size(); ++pattern_index) {
    auto const& pattern = source_patterns.patterns[pattern_index];
    if (chart_opts.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(pattern,
                                                                pattern_index);
    }
    if (is_invariant_site_pattern(pattern)) {
      result.skipped_invariant_site_count += pattern.weight;
      if (chart_opts.score_ua_edge) {
        result.invariant_constant_offset =
            chart_multisite_detail::checked_add_u64(
                result.invariant_constant_offset,
                chart_spr_search_detail::
                    invariant_pattern_reference_edge_offset(pattern),
                "chart-SPR invariant pattern UA-edge offset");
      }
      continue;
    }
    chart_spr_search_detail::append_active_pattern_metadata(active, pattern);
  }

  active.exact_pattern_to_normalized_binary_pattern.assign(
      active.patterns.size(), no_site_pattern);
  active.exact_pattern_to_normalized_binary_state_map.assign(
      active.patterns.size(), normalized_binary_state_map{});

  result.active_patterns.assert_no_skipped_invariant_metadata();
  return result;
}

inline chart_spr_active_pattern_build_result make_active_search_patterns(
    phylo_dag& dag, clade_grammar const& grammar,
    chart_options const& chart_opts = {},
    site_pattern_options pattern_opts = {}) {
  pattern_opts.skip_invariant_sites = true;
  auto raw_patterns = build_site_patterns(dag, grammar, pattern_opts);
  auto result = make_active_search_patterns(raw_patterns, chart_opts);
  result.pattern_source_fingerprint =
      build_chart_spr_pattern_source_fingerprint(dag, grammar);
  return result;
}

inline composite_chart_score build_composite_chart_score_active(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_options const& options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_composite_chart_score(grammar, patterns.patterns, options);
}

inline composite_chart_score build_composite_chart_score_active(
    chart_execution_plan const& plan, active_site_pattern_set const& patterns,
    chart_options const& options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_composite_chart_score(plan, patterns.patterns, options);
}

inline multisite_trim_result build_multisite_trim_active(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_multisite_trim(grammar, patterns.patterns, options,
                              trim_options);
}

inline multisite_trim_result build_multisite_trim_active(
    chart_execution_plan const& plan, active_site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_multisite_trim(plan, patterns.patterns, options, trim_options);
}

inline multisite_trim_result build_multisite_trim_active(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    lazy_multisite_chart const& lazy_chart, chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_multisite_trim(grammar, patterns.patterns, lazy_chart, options,
                              trim_options);
}

inline multisite_trim_result build_multisite_trim_active(
    chart_execution_plan const& plan, active_site_pattern_set const& patterns,
    lazy_multisite_chart const& lazy_chart,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_multisite_trim(plan, patterns.patterns, lazy_chart, options,
                              trim_options);
}

inline multisite_trim_result build_multisite_trim_active(
    clade_grammar const& grammar, chart_execution_plan const& plan,
    active_site_pattern_set const& patterns,
    lazy_multisite_chart const& lazy_chart,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_multisite_trim(grammar, plan, patterns.patterns, lazy_chart,
                              options, trim_options);
}

// Prefer tied-root provenance captured by the algorithmic trim.  Search runs
// configure that capture before any exact work, eliminating the former second
// full B&B from the timed canonical path.  The companion remains a checked
// compatibility fallback for direct library callers that supply an exact trim
// built without capture; it still runs outside semantic verifier/commit
// catches, so a report failure cannot change candidate validity or acceptance.
//
// Score-only trims intentionally have no exact keep mask.  Their scalar and
// frontier statistics are already the strongest honest evidence available,
// so they must not be upgraded to a different algorithmic mode merely for the
// report.
inline chart_spr_canonical_exact_evidence
chart_spr_canonicalize_search_trim_evidence(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_options const& chart_opts,
    multisite_trim_options const& algorithm_trim_options,
    multisite_trim_result const& algorithm_trim,
    std::uint64_t invariant_offset) {
  if (algorithm_trim.keep_production_exact) {
    if (algorithm_trim_options.capture_optimal_root_provenance) {
      if (algorithm_trim.optimal_root_provenance_classes.empty()) {
        throw std::runtime_error(
            "chart-SPR canonical report: primary exact trim was configured "
            "to capture root provenance but published no classes");
      }
      auto evidence = chart_spr_canonicalize_trim_evidence(
          grammar, algorithm_trim, invariant_offset);
      // Preserve the frozen semantic spelling while changing only where the
      // evidence was computed.
      evidence.evidence_kind = "grammar_exact_frontier_provenance_companion";
      return evidence;
    }
    auto companion_options = algorithm_trim_options;
    companion_options.capture_optimal_root_provenance = true;
    auto companion_trim = build_multisite_trim_active(
        grammar, patterns, chart_opts, companion_options);
    if (companion_trim.optimum != algorithm_trim.optimum ||
        companion_trim.keep_mask_kind != algorithm_trim.keep_mask_kind ||
        companion_trim.keep_production_exact !=
            algorithm_trim.keep_production_exact ||
        companion_trim.keep_production != algorithm_trim.keep_production ||
        companion_trim.frontier_sizes_by_clade !=
            algorithm_trim.frontier_sizes_by_clade ||
        companion_trim.active_pattern_count !=
            algorithm_trim.active_pattern_count ||
        companion_trim.invariant_constant_offset !=
            algorithm_trim.invariant_constant_offset) {
      throw std::runtime_error(
          "chart-SPR canonical report: provenance companion disagrees with "
          "algorithmic exact trim optimum, keep mask, frontier sizes, or "
          "pattern metadata");
    }

    auto evidence = chart_spr_canonicalize_trim_evidence(
        grammar, companion_trim, invariant_offset);
    evidence.evidence_kind =
        "grammar_exact_frontier_provenance_companion";
    return evidence;
  }

  return chart_spr_canonicalize_trim_evidence(
      grammar, algorithm_trim, invariant_offset);
}

namespace chart_spr_search_detail {

inline chart_spr_canonical_exact_evidence
canonicalize_search_trim_evidence_with_plan(
    clade_grammar const& grammar, chart_execution_plan const& plan,
    active_site_pattern_set const& patterns,
    chart_options const& chart_opts,
    multisite_trim_options const& algorithm_trim_options,
    multisite_trim_result const& algorithm_trim,
    std::uint64_t invariant_offset) {
  if (algorithm_trim.keep_production_exact) {
    if (algorithm_trim_options.capture_optimal_root_provenance) {
      if (algorithm_trim.optimal_root_provenance_classes.empty()) {
        throw std::runtime_error(
            "chart-SPR canonical report: primary exact trim was configured "
            "to capture root provenance but published no classes");
      }
      auto evidence = chart_spr_canonicalize_trim_evidence(
          grammar, algorithm_trim, invariant_offset);
      evidence.evidence_kind = "grammar_exact_frontier_provenance_companion";
      return evidence;
    }
    auto companion_options = algorithm_trim_options;
    companion_options.capture_optimal_root_provenance = true;
    auto companion_trim = build_multisite_trim_active(
        plan, patterns, chart_opts, companion_options);
    if (companion_trim.optimum != algorithm_trim.optimum ||
        companion_trim.keep_mask_kind != algorithm_trim.keep_mask_kind ||
        companion_trim.keep_production_exact !=
            algorithm_trim.keep_production_exact ||
        companion_trim.keep_production != algorithm_trim.keep_production ||
        companion_trim.frontier_sizes_by_clade !=
            algorithm_trim.frontier_sizes_by_clade ||
        companion_trim.active_pattern_count !=
            algorithm_trim.active_pattern_count ||
        companion_trim.invariant_constant_offset !=
            algorithm_trim.invariant_constant_offset) {
      throw std::runtime_error(
          "chart-SPR canonical report: provenance companion disagrees with "
          "algorithmic exact trim optimum, keep mask, frontier sizes, or "
          "pattern metadata");
    }

    auto evidence = chart_spr_canonicalize_trim_evidence(
        grammar, companion_trim, invariant_offset);
    evidence.evidence_kind =
        "grammar_exact_frontier_provenance_companion";
    return evidence;
  }

  return chart_spr_canonicalize_trim_evidence(
      grammar, algorithm_trim, invariant_offset);
}

}  // namespace chart_spr_search_detail

inline chart_spr_canonical_exact_evidence
chart_spr_canonicalize_search_trim_evidence(
    clade_grammar const& grammar, chart_execution_plan const& plan,
    active_site_pattern_set const& patterns,
    chart_options const& chart_opts,
    multisite_trim_options const& algorithm_trim_options,
    multisite_trim_result const& algorithm_trim,
    std::uint64_t invariant_offset) {
  auto checked = check_chart_execution_plan(grammar, plan);
  return chart_spr_search_detail::canonicalize_search_trim_evidence_with_plan(
      grammar, checked.plan(), patterns, chart_opts, algorithm_trim_options,
      algorithm_trim, invariant_offset);
}

inline chart_spr_canonical_exact_evidence
chart_spr_canonicalize_search_trim_evidence(
    clade_grammar const& grammar,
    checked_chart_execution_plan_ref const& checked,
    active_site_pattern_set const& patterns,
    chart_options const& chart_opts,
    multisite_trim_options const& algorithm_trim_options,
    multisite_trim_result const& algorithm_trim,
    std::uint64_t invariant_offset) {
  checked.assert_same(grammar, checked.plan());
  return chart_spr_search_detail::canonicalize_search_trim_evidence_with_plan(
      grammar, checked.plan(), patterns, chart_opts, algorithm_trim_options,
      algorithm_trim, invariant_offset);
}

inline chart_spr_canonical_exact_evidence
chart_spr_canonicalize_search_trim_evidence(
    planned_overlay_materialization_result const& planned,
    active_site_pattern_set const& patterns,
    chart_options const& chart_opts,
    multisite_trim_options const& algorithm_trim_options,
    multisite_trim_result const& algorithm_trim,
    std::uint64_t invariant_offset) {
  return chart_spr_search_detail::canonicalize_search_trim_evidence_with_plan(
      planned.materialized.grammar, planned.execution_plan, patterns,
      chart_opts, algorithm_trim_options, algorithm_trim, invariant_offset);
}

inline multisite_trim_result build_lazy_multisite_trim_active_from_scratch(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  lazy_chart_options lazy_options;
  lazy_options.chart = options;
  lazy_options.chart.keep_trace = false;
  lazy_options.chart.max_trace_choices = 0;
  auto lazy_chart =
      build_lazy_inside_chart(grammar, patterns.patterns, lazy_options);
  return build_multisite_trim_active(grammar, patterns, lazy_chart, options,
                                     trim_options);
}

inline multisite_trim_result build_lazy_multisite_trim_active_from_scratch(
    clade_grammar const& grammar, chart_execution_plan const& plan,
    active_site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  auto checked = check_chart_execution_plan(grammar, plan);
  patterns.assert_no_skipped_invariant_metadata();
  lazy_chart_options lazy_options;
  lazy_options.chart = options;
  lazy_options.chart.keep_trace = false;
  lazy_options.chart.max_trace_choices = 0;
  auto lazy_chart =
      build_lazy_inside_chart(checked.plan(), patterns.patterns, lazy_options);
  return build_multisite_trim_active(checked.plan(), patterns, lazy_chart, options,
                                     trim_options);
}

inline multisite_trim_result build_lazy_multisite_trim_active_from_scratch(
    planned_overlay_materialization_result const& planned,
    active_site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  lazy_chart_options lazy_options;
  lazy_options.chart = options;
  lazy_options.chart.keep_trace = false;
  lazy_options.chart.max_trace_choices = 0;
  auto lazy_chart = build_lazy_inside_chart(
      planned.execution_plan, patterns.patterns, lazy_options);
  return build_multisite_trim_active(planned.execution_plan, patterns,
                                     lazy_chart, options, trim_options);
}

inline std::size_t estimate_chart_spr_pattern_row_cache_bytes(
    clade_grammar const& grammar) {
  constexpr auto bytes_per_row = nuc_state_count * sizeof(chart_cost);
  if (grammar.clades.size() >
      (std::numeric_limits<std::size_t>::max)() / bytes_per_row) {
    throw std::overflow_error("chart SPR pattern-cache row byte overflow");
  }
  return grammar.clades.size() * bytes_per_row;
}

inline std::size_t estimate_chart_spr_pattern_entry_cache_bytes(
    clade_grammar const& grammar) {
  auto const row_bytes = estimate_chart_spr_pattern_row_cache_bytes(grammar);
  if (row_bytes > (std::numeric_limits<std::size_t>::max)() -
                      sizeof(pattern_chart_cache_entry)) {
    throw std::overflow_error("chart SPR pattern-cache entry byte overflow");
  }
  return row_bytes + sizeof(pattern_chart_cache_entry);
}

inline std::size_t estimate_chart_spr_pattern_batch_cache_bytes(
    clade_grammar const& grammar, std::size_t pattern_count) {
  auto const entry_bytes =
      estimate_chart_spr_pattern_entry_cache_bytes(grammar);
  if (entry_bytes != 0 &&
      pattern_count > (std::numeric_limits<std::size_t>::max)() / entry_bytes) {
    throw std::overflow_error("chart SPR pattern-cache batch byte overflow");
  }
  return pattern_count * entry_bytes;
}

inline std::size_t estimate_chart_spr_full_pattern_cache_bytes(
    clade_grammar const& grammar, active_site_pattern_set const& patterns) {
  patterns.assert_no_skipped_invariant_metadata();
  auto const row_bytes = estimate_chart_spr_pattern_row_cache_bytes(grammar);
  auto const pattern_count = patterns.patterns.patterns.size();
  if (row_bytes != 0 &&
      pattern_count > (std::numeric_limits<std::size_t>::max)() / row_bytes) {
    throw std::overflow_error("chart SPR full pattern-cache byte overflow");
  }
  return pattern_count * row_bytes;
}

// Conservative frozen-toolchain admission bound for a fully materialized
// lazy inside/outside cache.  This is intentionally available to the inline
// state builder so a finite unified budget can reject before any lazy rows or
// class maps are allocated.
std::size_t estimate_chart_spr_lazy_cache_admission_bytes(
    std::size_t clade_count, std::size_t pattern_count);

namespace chart_spr_search_detail {

inline chart_spr_lazy_policy requested_chart_spr_lazy_policy(
    chart_cache_options const& cache) {
  return cache.use_lazy_multisite_chart ? chart_spr_lazy_policy::on
                                        : cache.lazy_policy;
}

inline std::size_t checked_lazy_policy_add(std::size_t lhs, std::size_t rhs,
                                           char const* context) {
  return chart_spr_exact_candidate_checked_bytes_add(lhs, rhs, context);
}

inline std::size_t checked_lazy_policy_multiply(std::size_t lhs,
                                                std::size_t rhs,
                                                char const* context) {
  return chart_spr_exact_candidate_checked_bytes_multiply(lhs, rhs, context);
}

inline std::size_t lazy_policy_stratum_midpoint(std::size_t population,
                                                std::size_t strata,
                                                std::size_t stratum) {
  auto const base = population / strata;
  auto const remainder = population % strata;
  auto const begin = stratum * base + std::min(stratum, remainder);
  auto const width = base + (stratum < remainder ? 1 : 0);
  return begin + (width - 1) / 2;
}

inline std::size_t lazy_policy_allocator_rounded_staged_vector_bytes(
    std::size_t count, std::size_t element_size, char const* context) {
  if (count == 0) return 0;
  auto logical = checked_lazy_policy_multiply(count, element_size, context);
  // The frozen allocator rounds requests below 16 bytes to its minimum
  // quantum.  Charge old+new staging as twice the rounded allocation even for
  // the current empty-to-sized construction, so this remains safe if a pilot
  // vector is later reused instead of freshly constructed.
  auto const rounded = std::max<std::size_t>(16, logical);
  return checked_lazy_policy_multiply(rounded, 2, context);
}

inline std::size_t estimate_lazy_policy_pilot_allocation_bytes(
    chart_execution_plan const& plan, std::size_t pilot_pattern_count,
    std::size_t taxon_count) {
  // The ordinary lazy admission bound includes both row surfaces and all
  // retained maps, so it conservatively covers the inside-only pilot chart.
  auto const chart_resident = estimate_chart_spr_lazy_cache_admission_bytes(
      plan.clades().size(), pilot_pattern_count);
  // Resident lazy admission already models doubled vector capacities.  Charge
  // a second whole chart envelope for allocator old+new publication peaks and
  // for inside-only coordinator containers that coexist while a clade is
  // published.
  auto total = checked_lazy_policy_multiply(
      chart_resident, 2, "chart SPR lazy-policy pilot chart old-new bytes");
  auto const pattern_objects =
      lazy_policy_allocator_rounded_staged_vector_bytes(
          pilot_pattern_count, sizeof(site_pattern),
          "chart SPR lazy-policy pilot pattern-object bytes");
  auto const state_vector_bytes =
      lazy_policy_allocator_rounded_staged_vector_bytes(
          taxon_count, sizeof(std::uint8_t),
          "chart SPR lazy-policy pilot state-vector bytes");
  auto const pattern_states =
      checked_lazy_policy_multiply(pilot_pattern_count, state_vector_bytes,
                                   "chart SPR lazy-policy pilot state bytes");
  total = checked_lazy_policy_add(
      total, pattern_objects, "chart SPR lazy-policy pilot allocation bytes");
  total = checked_lazy_policy_add(
      total, pattern_states, "chart SPR lazy-policy pilot allocation bytes");

  // Packed grouping publishes coordinator index vectors in addition to its
  // packed-word/workspace/result envelope below. Charge allocator-rounded
  // old+new storage for the three per-pattern coordinator indices here; the
  // 3x largest packed-key envelope below separately covers staged packed
  // words, sorting scratch, and both grouping results.
  total = checked_lazy_policy_add(
      total,
      lazy_policy_allocator_rounded_staged_vector_bytes(
          pilot_pattern_count, sizeof(std::size_t) * 3,
          "chart SPR lazy-policy pilot coordinator-vector bytes"),
      "chart SPR lazy-policy pilot allocation bytes");

  // Also charge the largest packed-key workspace.  Twice the logical estimate
  // bounds the frozen libstdc++ vector growth from empty for the pilot sizes.
  std::size_t max_key_workspace = 0;
  for (auto clade : plan.bottom_up_order()) {
    if (plan.clade(clade).is_leaf()) continue;
    auto const productions = plan.productions_for_parent(clade);
    if (productions.empty()) continue;
    auto const structural_width = plan.children(productions.front()).size();
    std::size_t row_width = 0;
    for (auto production : productions) {
      row_width =
          checked_lazy_policy_add(row_width, plan.children(production).size(),
                                  "chart SPR lazy-policy pilot row-key width");
    }
    auto const logical = lazy_chart_detail::
        estimate_plan_parent_key_grouping_logical_resident_bytes(
            pilot_pattern_count, structural_width, pilot_pattern_count,
            row_width, pilot_pattern_count);
    max_key_workspace = std::max(max_key_workspace, logical);
  }
  return checked_lazy_policy_add(
      total,
      checked_lazy_policy_multiply(
          max_key_workspace, 3,
          "chart SPR lazy-policy pilot key-workspace bytes"),
      "chart SPR lazy-policy pilot allocation bytes");
}

inline std::size_t lazy_policy_key_word_work(
    chart_execution_plan const& plan, lazy_multisite_chart const& pilot) {
  std::size_t total = 0;
  for (auto clade : plan.bottom_up_order()) {
    if (plan.clade(clade).is_leaf()) continue;
    auto const productions = plan.productions_for_parent(clade);
    if (productions.empty()) continue;
    auto const structural_width = plan.children(productions.front()).size();
    std::size_t row_width = 0;
    for (auto production : productions) {
      row_width =
          checked_lazy_policy_add(row_width, plan.children(production).size(),
                                  "chart SPR lazy-policy row-key width");
    }
    auto const structural_words = checked_lazy_policy_multiply(
        pilot.pattern_count, structural_width,
        "chart SPR lazy-policy structural key work");
    auto const row_words = checked_lazy_policy_multiply(
        pilot.structural_class_count(clade), row_width,
        "chart SPR lazy-policy row key work");
    total = checked_lazy_policy_add(
        total,
        checked_lazy_policy_add(structural_words, row_words,
                                "chart SPR lazy-policy clade key work"),
        "chart SPR lazy-policy total key work");
  }
  return total;
}

inline bool lazy_policy_at_most_multiple(std::size_t value, std::size_t base,
                                         std::size_t multiplier) {
  if (base == 0) return value == 0;
  if (base > (std::numeric_limits<std::size_t>::max)() / multiplier) {
    return true;
  }
  return value <= base * multiplier;
}

}  // namespace chart_spr_search_detail

// Run the deterministic Phase-7 policy pilot.  The pilot samples at most 32
// midpoint-stratified active-pattern indices, constructs only a lazy inside
// chart, and resolves with integer comparisons.  It never builds outside rows,
// a full chart, an exact setup, or an exact frontier.
inline chart_spr_lazy_policy_diagnostics resolve_chart_spr_lazy_policy(
    chart_execution_plan const& plan,
    active_site_pattern_set const& active_patterns,
    chart_options const& chart_options, chart_cache_options const& cache,
    std::size_t estimated_dense_cache_bytes) {
  using namespace chart_spr_search_detail;

  validate_supported_chart_cache_options(cache);

  auto const requested = requested_chart_spr_lazy_policy(cache);
  auto const active_pattern_count = active_patterns.patterns.patterns.size();

  chart_spr_lazy_policy_diagnostics result;
  result.requested = requested;
  result.resolved = result.requested;
  result.active_pattern_count = active_pattern_count;
  result.estimated_dense_cache_bytes = estimated_dense_cache_bytes;
  result.estimated_lazy_cache_bytes =
      estimate_chart_spr_lazy_cache_admission_bytes(
          plan.clades().size(), result.active_pattern_count);
  if (result.requested == chart_spr_lazy_policy::off) {
    result.reason = chart_spr_lazy_policy_reason::explicit_off;
    return result;
  }
  if (result.requested == chart_spr_lazy_policy::on) {
    result.reason = chart_spr_lazy_policy_reason::explicit_on;
    return result;
  }

  result.resolved = chart_spr_lazy_policy::off;
  if (result.active_pattern_count == 0) {
    result.reason = chart_spr_lazy_policy_reason::no_active_patterns;
    return result;
  }

  constexpr std::size_t max_pilot_patterns = 32;
  auto const pilot_count =
      std::min(max_pilot_patterns, result.active_pattern_count);
  result.pilot_estimated_allocation_bytes =
      estimate_lazy_policy_pilot_allocation_bytes(
          plan, pilot_count, active_patterns.patterns.taxon_count);

  // Fail closed before allocating the pilot pattern copies or chart.  The
  // selected full lazy representation must fit too; otherwise a promising
  // pilot would only defer the same finite-budget rejection.
  if (cache.memory_budget_bytes != 0 &&
      result.estimated_lazy_cache_bytes > cache.memory_budget_bytes) {
    result.reason = chart_spr_lazy_policy_reason::full_lazy_budget_exceeded;
    return result;
  }
  if (cache.memory_budget_bytes != 0 &&
      result.pilot_estimated_allocation_bytes > cache.memory_budget_bytes) {
    result.reason = chart_spr_lazy_policy_reason::pilot_budget_exceeded;
    return result;
  }

  site_pattern_set pilot_patterns;
  pilot_patterns.taxon_count = active_patterns.patterns.taxon_count;
  pilot_patterns.patterns.reserve(pilot_count);
  std::uint64_t index_hash = 1469598103934665603ULL;
  for (std::size_t stratum = 0; stratum < pilot_count; ++stratum) {
    auto const index = lazy_policy_stratum_midpoint(result.active_pattern_count,
                                                    pilot_count, stratum);
    auto const& source = active_patterns.patterns.patterns[index];
    site_pattern selected;
    selected.state_by_taxon = source.state_by_taxon;
    selected.weight = source.weight;
    selected.reference_state_counts = source.reference_state_counts;
    pilot_patterns.patterns.push_back(std::move(selected));
    index_hash = mix_u64(index_hash, index);
  }
  result.pilot_pattern_count = pilot_patterns.patterns.size();
  result.pilot_pattern_index_hash = index_hash;

  lazy_chart_options pilot_options;
  pilot_options.chart = chart_options;
  pilot_options.chart.keep_trace = false;
  pilot_options.chart.max_trace_choices = 0;
  pilot_options.retain_all_inside_class_maps = true;
  auto pilot = build_lazy_inside_chart(plan, pilot_patterns, pilot_options);
  result.pilot_inside_chart_builds = 1;

  for (auto clade : plan.bottom_up_order()) {
    if (clade == plan.root_clade() || plan.clade(clade).is_leaf()) continue;
    result.pilot_internal_structural_class_count_max =
        std::max(result.pilot_internal_structural_class_count_max,
                 pilot.structural_class_count(clade));
  }
  result.pilot_structural_ratio_numerator =
      result.pilot_internal_structural_class_count_max;
  result.pilot_structural_ratio_denominator = result.pilot_pattern_count;
  result.pilot_inside_rows = pilot.lazy_inside_rows_computed;
  result.pilot_dense_rows = checked_lazy_policy_multiply(
      result.pilot_pattern_count, plan.clades().size(),
      "chart SPR lazy-policy pilot dense rows");
  result.pilot_row_ratio_numerator = result.pilot_inside_rows;
  result.pilot_row_ratio_denominator = result.pilot_dense_rows;
  result.pilot_key_words = lazy_policy_key_word_work(plan, pilot);
  result.pilot_dense_row_work = result.pilot_dense_rows;

  // V1 policy, deliberately simple and inspectable:
  //   * maximum internal structural-class ratio <= 1/3, OR an especially
  //     strong total retained inside-row ratio <= 1/8 (the stratified pilot
  //     can deliberately sample one member from many otherwise large classes);
  //   * total retained inside-row ratio <= 1/2;
  //   * packed-key words <= 8x dense row visits;
  //   * both pilot and full conservative lazy estimates fit a finite budget.
  // These comparisons are all integer; reported decimal ratios do not feed
  // the decision.
  if (result.pilot_structural_ratio_denominator == 0 ||
      result.pilot_internal_structural_class_count_max == 0) {
    result.reason =
        chart_spr_lazy_policy_reason::no_internal_structural_classes;
    return result;
  }
  auto const structural_compression =
      result.pilot_internal_structural_class_count_max <=
      result.pilot_pattern_count / 3;
  auto const strong_row_compression =
      result.pilot_inside_rows <= result.pilot_dense_rows / 8;
  if (!structural_compression && !strong_row_compression) {
    result.reason =
        chart_spr_lazy_policy_reason::structural_and_strong_row_ratios_exceeded;
    return result;
  }
  if (result.pilot_inside_rows > result.pilot_dense_rows / 2) {
    result.reason = chart_spr_lazy_policy_reason::row_ratio_above_one_half;
    return result;
  }
  if (!lazy_policy_at_most_multiple(result.pilot_key_words,
                                    result.pilot_dense_row_work, 8)) {
    result.reason =
        chart_spr_lazy_policy_reason::key_work_above_eight_dense_rows;
    return result;
  }

  result.resolved = chart_spr_lazy_policy::on;
  result.reason =
      chart_spr_lazy_policy_reason::compression_thresholds_and_budget_safe;
  return result;
}

inline std::size_t choose_chart_spr_pattern_batch_size(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_cache_options const& cache) {
  auto active_count = patterns.patterns.patterns.size();
  if (active_count == 0) return 0;
  if (cache.pattern_batch_size != 0) {
    return std::max<std::size_t>(1,
                                 std::min(cache.pattern_batch_size,
                                          active_count));
  }

  std::size_t limit = active_count;
  if (cache.max_cached_patterns != 0) {
    limit = std::min(limit, cache.max_cached_patterns);
  }
  auto entry_bytes = estimate_chart_spr_pattern_entry_cache_bytes(grammar);
  if (cache.memory_budget_bytes != 0 && entry_bytes != 0) {
    auto by_budget = cache.memory_budget_bytes / entry_bytes;
    if (by_budget == 0) by_budget = 1;
    limit = std::min(limit, by_budget);
  }
  return std::max<std::size_t>(1, std::min(limit, active_count));
}

inline chart_spr_cache_strategy choose_chart_spr_cache_strategy(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_cache_options const& cache) {
  validate_supported_chart_cache_options(cache);
  auto const lazy_policy =
      chart_spr_search_detail::requested_chart_spr_lazy_policy(cache);
  if (lazy_policy == chart_spr_lazy_policy::automatic) {
    throw std::runtime_error(
        "chart SPR cache strategy: unresolved auto lazy policy");
  }
  if (lazy_policy == chart_spr_lazy_policy::on) {
    return chart_spr_cache_strategy::lazy_multisite_chart;
  }
  auto active_count = patterns.patterns.patterns.size();
  if (active_count == 0) return chart_spr_cache_strategy::all_active_patterns;
  auto batch_size = choose_chart_spr_pattern_batch_size(grammar, patterns,
                                                       cache);
  return batch_size >= active_count
             ? chart_spr_cache_strategy::all_active_patterns
             : chart_spr_cache_strategy::pattern_batches;
}

// One high-water domain is shared by every search-state rebuild in a run.
// Atomics make this measurement truthful once exact verification is
// parallelized; today the observed peak is one because the verifier loop is
// serial.  The guard provides exception-safe entry/exit accounting.
class chart_spr_exact_verifier_concurrency_tracker {
 public:
  void enter() noexcept {
    auto const now = active_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto previous = peak_.load(std::memory_order_relaxed);
    while (previous < now &&
           !peak_.compare_exchange_weak(previous, now,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
    }
  }

  void leave() noexcept {
    active_.fetch_sub(1, std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t peak() const noexcept {
    return peak_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<std::size_t> active_{0};
  std::atomic<std::size_t> peak_{0};
};

class chart_spr_exact_verifier_activity {
 public:
  explicit chart_spr_exact_verifier_activity(
      std::shared_ptr<chart_spr_exact_verifier_concurrency_tracker> tracker)
      : tracker_(std::move(tracker)) {
    if (tracker_) tracker_->enter();
  }

  chart_spr_exact_verifier_activity(
      chart_spr_exact_verifier_activity const&) = delete;
  chart_spr_exact_verifier_activity& operator=(
      chart_spr_exact_verifier_activity const&) = delete;

  ~chart_spr_exact_verifier_activity() {
    if (tracker_) tracker_->leave();
  }

 private:
  std::shared_ptr<chart_spr_exact_verifier_concurrency_tracker> tracker_;
};

// Adds one precisely scoped wall-time span to an instrumentation bucket on
// every exit path.  In particular, verifier and accepted-update materializers
// deliberately convert some exceptions into reportable candidate/accept
// failures; those calls still consumed materialization time and must not vanish
// from the Phase-0 profile.  This guard changes accounting only, never control
// flow.
class chart_spr_elapsed_accumulator {
 public:
  explicit chart_spr_elapsed_accumulator(double& destination) noexcept
      : destination_(&destination), start_(std::chrono::steady_clock::now()) {}

  chart_spr_elapsed_accumulator(chart_spr_elapsed_accumulator const&) = delete;
  chart_spr_elapsed_accumulator& operator=(
      chart_spr_elapsed_accumulator const&) = delete;

  void finish() noexcept {
    if (destination_ == nullptr) return;
    *destination_ += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start_)
                         .count();
    destination_ = nullptr;
  }

  ~chart_spr_elapsed_accumulator() noexcept { finish(); }

 private:
  double* destination_;
  std::chrono::steady_clock::time_point start_;
};

// State-resident callbacks need mutation tracking in addition to
// std::function's callable interface.  A public assignment receives a unique
// mutation token while whole-state copy/move preserves it, allowing a declared
// aggregate target-ownership contract to detect even same-count replacement.
template <class Function>
class chart_spr_tracked_state_callback {
 public:
  chart_spr_tracked_state_callback() = default;
  chart_spr_tracked_state_callback(chart_spr_tracked_state_callback const&) =
      default;
  chart_spr_tracked_state_callback(chart_spr_tracked_state_callback&&) =
      default;
  chart_spr_tracked_state_callback& operator=(
      chart_spr_tracked_state_callback const&) = default;
  chart_spr_tracked_state_callback& operator=(
      chart_spr_tracked_state_callback&&) = default;

  template <class Callable>
    requires(!std::same_as<std::remove_cvref_t<Callable>,
                           chart_spr_tracked_state_callback> &&
             std::is_assignable_v<Function&, Callable>)
  chart_spr_tracked_state_callback& operator=(Callable&& callable) {
    function_ = std::forward<Callable>(callable);
    revision_ = next_revision_.fetch_add(1, std::memory_order_relaxed);
    if (revision_ == 0) {
      // Reserve zero for never-assigned wrappers across integer wraparound.
      revision_ = next_revision_.fetch_add(1, std::memory_order_relaxed);
    }
    return *this;
  }

  explicit operator bool() const noexcept {
    return static_cast<bool>(function_);
  }

  template <class... Args>
  decltype(auto) operator()(Args&&... args) const {
    return std::invoke(function_, std::forward<Args>(args)...);
  }

  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

 private:
  inline static std::atomic<std::uint64_t> next_revision_{1};
  Function function_;
  std::uint64_t revision_ = 0;
};

struct chart_spr_search_state {
  phylo_dag* dag = nullptr;
  clade_grammar grammar;
  chart_execution_plan execution_plan;

  // Active/topology-informative patterns only.  Invariant-site metadata is
  // stored as invariant_constant_offset/skipped_invariant_site_count below and
  // added exactly once at objective/reporting boundaries.
  active_site_pattern_set active_patterns;
  chart_spr_pattern_source_fingerprint pattern_source_fingerprint;
  std::uint64_t invariant_constant_offset = 0;
  std::size_t skipped_invariant_site_count = 0;

  chart_options chart_opts;
  chart_cache_options cache_opts;
  chart_spr_cache_strategy cache_strategy =
      chart_spr_cache_strategy::all_active_patterns;
  chart_spr_lazy_policy_diagnostics lazy_policy;
  std::size_t estimated_full_pattern_cache_bytes = 0;
  std::size_t resident_pattern_cache_bytes = 0;
  // Persistent local-commit inside+outside row caches are additional to the
  // lazy scoring representation, but replace the non-lazy scoring projection.
  // Keep their contribution separate while substrate construction is in
  // flight so admission never under-reports the two full cache surfaces.
  std::size_t local_commit_persistent_cache_bytes = 0;
  // Source-compatible diagnostic retained from the pre-Phase-6 shared
  // selected-cache implementation. Selected caches are task-local now, so no
  // bytes remain admitted after an operation and this value stays zero.
  mutable std::size_t selected_topology_cache_admitted_bytes = 0;
  std::size_t effective_pattern_batch_size = 0;
  mutable std::size_t effective_candidate_batch_size = 0;
  std::vector<pattern_chart_cache_entry> pattern_charts;
  std::optional<lazy_multisite_chart> lazy_chart;
  chart_spr_inside_row_view local_commit_inside_rows;
  // A deferred non-lazy local state is an internal, two-stage publication: its
  // grammar/plan/pattern identity is complete, but its initial composite must
  // be supplied by the local persistent inside cache before the state can be
  // scored or exactly trimmed. Public builders never return this state. The
  // historical field name is retained for source compatibility.
  bool pattern_batch_bootstrap_deferred = false;
  std::uint64_t composite_lower_bound_without_invariants = 0;
  std::uint64_t composite_lower_bound_with_invariants = 0;
  double chart_construction_ms = 0.0;
  double exact_initialization_ms = 0.0;
  std::shared_ptr<chart_spr_exact_verifier_concurrency_tracker>
      exact_verifier_concurrency =
          std::make_shared<chart_spr_exact_verifier_concurrency_tracker>();

  // Active-pattern-only exact result.  Add invariant_constant_offset exactly
  // once when comparing/reporting the full objective.  This is mutable so the
  // Phase-4 exact acceptance gate can lazily build and then reuse the current
  // state's old exact score even when verification APIs take a const state.
  mutable std::optional<multisite_trim_result> exact_trim_active_only;
  // Internal local-commit orchestration enables candidate-result retention
  // only while it owns a commit substrate capable of consuming the payload.
  // Conservative rebuild and standalone verification must not retain one full
  // trim per reported iteration.
  bool retain_verified_exact_trim_for_local_commit = false;

  // Optional owning exact-setup source installed by local-commit
  // orchestration. Non-lazy local states consume the persistent inside cache;
  // public/conservative states continue to use their selected representation.
  chart_spr_tracked_state_callback<chart_spr_exact_setup_provider>
      exact_setup_provider;
  chart_spr_tracked_state_callback<chart_spr_scheduled_exact_setup_provider>
      scheduled_exact_setup_provider;
  // A custom setup provider may retain the ordinary finalized setup covered
  // by the generic estimator. Any provider-specific scratch must be returned
  // here. Finite-budget exact initialization fails closed when a provider is
  // installed without this matching contract. Like every admission metadata
  // callback, the estimator itself must not allocate material scratch.
  chart_spr_tracked_state_callback<
      std::function<std::size_t(multisite_trim_options const&, std::size_t)>>
      exact_setup_provider_additional_memory_estimator;

  // Optional Phase-8 fixed-topology verifier supplied by the local-commit
  // substrate.  When present, fixed_topology_exact verification reads the
  // persistent inside/outside caches owned by the substrate.  Conservative
  // rebuild mode leaves this empty and uses the legacy direct selected-topology
  // fallback below.
  chart_spr_tracked_state_callback<chart_spr_fixed_topology_verifier>
      fixed_topology_exact_verifier;
  // Context-aware callbacks can opt in to candidate-parallel invocation.
  // Legacy callbacks above remain singleton regardless of this flag because
  // their historical signature has no task-private counter sink.
  chart_spr_tracked_state_callback<chart_spr_contextual_fixed_topology_verifier>
      contextual_fixed_topology_exact_verifier;
  bool fixed_topology_exact_verifier_parallel_safe = false;
  // Provider-specific scratch beyond the generic fixed-topology verifier
  // estimate. A finite unified budget fails closed if a custom verifier is
  // installed without this matching estimator; the estimator itself must be
  // allocation-free apart from trivial stack/SSO state.
  chart_spr_tracked_state_callback<
      std::function<std::size_t(grammar_spr_candidate const&)>>
      fixed_topology_exact_additional_memory_estimator;
  // Provider-created owning output (for example an enlarged invalid-reason or
  // evidence payload) survives task completion and every later wave until
  // stable aggregation.  It is distinct from scratch and must therefore be
  // carried in the admission planner.  Finite custom-verifier runs require
  // both estimators.
  chart_spr_tracked_state_callback<
      std::function<std::size_t(grammar_spr_candidate const&)>>
      fixed_topology_exact_additional_retained_memory_estimator;

  // Optional Phase-9 transient-extension verifier supplied by the local-commit
  // substrate.  When present, exact_multisite verification transiently extends
  // the chain in reader-local scratch (never mutating the shared cache) and
  // reads the exact frontier on the extended grammar.  Conservative
  // rebuild mode leaves this empty and uses the cold from-scratch path
  // (`verify_candidate_exact_against_state`).
  //
  // Diagnostic scratch caches are gated to the optional two-chart oracle; the
  // production B&B consumes only the materialized extended grammar.
  chart_spr_tracked_state_callback<chart_spr_exact_multisite_verifier>
      exact_multisite_verifier;
  chart_spr_tracked_state_callback<
      chart_spr_contextual_exact_multisite_verifier>
      contextual_exact_multisite_verifier;
  bool exact_multisite_verifier_parallel_safe = false;

  // Operational frozen-toolchain bound for scratch owned specifically by the
  // transient verifier (chain copy, append/tip folding, and any diagnostic
  // cache copies). The generic candidate estimator adds materialization and
  // B&B storage separately. A finite budget fails closed when a transient
  // verifier is installed without this matching estimator. Both estimator
  // callbacks are admission metadata and must not allocate material scratch.
  chart_spr_tracked_state_callback<
      std::function<std::size_t(grammar_spr_candidate const&)>>
      exact_multisite_transient_memory_estimator;
  chart_spr_tracked_state_callback<
      std::function<std::size_t(grammar_spr_candidate const&)>>
      exact_multisite_transient_retained_memory_estimator;

  // std::function stores its wrapper inline above, but a callable target may
  // own an opaque heap allocation (and may itself retain further ownership).
  // Public/custom callback installation must declare one aggregate bound for
  // those persistent targets.  Internal production callbacks are frozen-
  // toolchain SBO callables and declare zero through the guarded installer.
  // The count and tracked assignment revisions prevent later additions,
  // removals, or same-count replacements from silently inheriting a stale
  // declaration made for an earlier set of targets.
  std::size_t callback_target_resident_bytes = 0;
  std::size_t callback_target_resident_contract_function_count = 0;
  std::array<std::uint64_t, 11> callback_target_resident_contract_revisions{};
  bool callback_target_resident_safely_bounded = false;

  mutable chart_spr_search_counters counters;
};

inline std::size_t chart_spr_state_callback_function_count(
    chart_spr_search_state const& state) noexcept {
  return static_cast<std::size_t>(
             static_cast<bool>(state.exact_setup_provider)) +
         static_cast<std::size_t>(
             static_cast<bool>(state.scheduled_exact_setup_provider)) +
         static_cast<std::size_t>(static_cast<bool>(
             state.exact_setup_provider_additional_memory_estimator)) +
         static_cast<std::size_t>(
             static_cast<bool>(state.fixed_topology_exact_verifier)) +
         static_cast<std::size_t>(static_cast<bool>(
             state.contextual_fixed_topology_exact_verifier)) +
         static_cast<std::size_t>(static_cast<bool>(
             state.fixed_topology_exact_additional_memory_estimator)) +
         static_cast<std::size_t>(static_cast<bool>(
             state.fixed_topology_exact_additional_retained_memory_estimator)) +
         static_cast<std::size_t>(
             static_cast<bool>(state.exact_multisite_verifier)) +
         static_cast<std::size_t>(
             static_cast<bool>(state.contextual_exact_multisite_verifier)) +
         static_cast<std::size_t>(static_cast<bool>(
             state.exact_multisite_transient_memory_estimator)) +
         static_cast<std::size_t>(static_cast<bool>(
             state.exact_multisite_transient_retained_memory_estimator));
}

inline std::array<std::uint64_t, 11> chart_spr_state_callback_revisions(
    chart_spr_search_state const& state) noexcept {
  return {
      state.exact_setup_provider.revision(),
      state.scheduled_exact_setup_provider.revision(),
      state.exact_setup_provider_additional_memory_estimator.revision(),
      state.fixed_topology_exact_verifier.revision(),
      state.contextual_fixed_topology_exact_verifier.revision(),
      state.fixed_topology_exact_additional_memory_estimator.revision(),
      state.fixed_topology_exact_additional_retained_memory_estimator
          .revision(),
      state.exact_multisite_verifier.revision(),
      state.contextual_exact_multisite_verifier.revision(),
      state.exact_multisite_transient_memory_estimator.revision(),
      state.exact_multisite_transient_retained_memory_estimator.revision(),
  };
}

// Declare the aggregate persistent ownership of every callable target
// currently installed in the state.  Re-declare after adding/replacing a
// custom callback.  The bound excludes std::function wrappers, which are
// already inline in sizeof(chart_spr_search_state).
inline void declare_chart_spr_state_callback_target_resident_bytes(
    chart_spr_search_state& state, std::size_t resident_bytes) noexcept {
  state.callback_target_resident_bytes = resident_bytes;
  state.callback_target_resident_contract_function_count =
      chart_spr_state_callback_function_count(state);
  state.callback_target_resident_contract_revisions =
      chart_spr_state_callback_revisions(state);
  state.callback_target_resident_safely_bounded = true;
}

// Capacity-based resident accounting for the coordinator-published old trim,
// plus a conservative preflight estimate for one candidate task. Definitions
// live with the materialization/cache implementations in chart_spr_search.cpp.
std::size_t estimate_chart_spr_trim_resident_bytes(
    multisite_trim_result const& trim);

// Dynamic owning storage only.  The fixed trim object is already inline in
// chart_spr_search_state::exact_trim_active_only.
std::size_t estimate_chart_spr_trim_dynamic_resident_bytes(
    multisite_trim_result const& trim);

// Cache-independent owning state.  This includes sizeof(state), whose inline
// optionals already contain the fixed lazy-chart and exact-trim objects, but
// excludes every selected-cache allocation (including pattern_charts slots).
inline std::size_t estimate_chart_spr_state_core_resident_bytes(
    chart_spr_search_state const& state);

// Selected scoring-cache admitted bound with fixed inline lazy-chart storage
// removed. Lazy/all-active values describe capacity ownership; pattern-batch
// deliberately contributes its configured future batch-workspace reservation
// even while pattern_charts is empty. resident_pattern_cache_bytes already
// includes the persistent local-commit cache when one is published; do not add
// that field separately.
inline std::size_t estimate_chart_spr_selected_cache_dynamic_resident_bytes(
    chart_spr_search_state const& state);

inline std::size_t estimate_chart_spr_retained_exact_trim_bytes(
    chart_spr_search_state const& state) {
  return state.exact_trim_active_only
             ? estimate_chart_spr_trim_resident_bytes(
                   *state.exact_trim_active_only)
             : 0;
}

// Unified published-state admission bound. It is allocator-capacity exact for
// the core, lazy/all-active selected cache, and trim components; pattern-batch
// intentionally substitutes its reserved next-batch scoring envelope.
inline std::size_t estimate_chart_spr_published_state_resident_bytes(
    chart_spr_search_state const& state) {
  auto total = chart_spr_exact_candidate_checked_bytes_add(
      estimate_chart_spr_state_core_resident_bytes(state),
      estimate_chart_spr_selected_cache_dynamic_resident_bytes(state),
      "chart SPR published state core/cache resident bytes");
  if (state.exact_trim_active_only) {
    total = chart_spr_exact_candidate_checked_bytes_add(
        total,
        estimate_chart_spr_trim_dynamic_resident_bytes(
            *state.exact_trim_active_only),
        "chart SPR published state exact-trim resident bytes");
  }
  return total;
}

inline void require_chart_spr_retained_exact_state_memory_budget(
    chart_spr_search_state const& state, multisite_trim_result const& trim,
    std::size_t memory_budget_bytes) {
  if (memory_budget_bytes == 0) return;
  auto required = chart_spr_exact_candidate_checked_bytes_add(
      estimate_chart_spr_state_core_resident_bytes(state),
      estimate_chart_spr_selected_cache_dynamic_resident_bytes(state),
      "chart SPR retained exact-state core/cache resident bytes");
  required = chart_spr_exact_candidate_checked_bytes_add(
      required, estimate_chart_spr_trim_dynamic_resident_bytes(trim),
      "chart SPR retained exact-state exact-trim resident bytes");
  if (required > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(required, memory_budget_bytes);
  }
}

inline void require_chart_spr_retained_exact_state_memory_budget(
    chart_spr_search_state const& state, multisite_trim_result const& trim) {
  require_chart_spr_retained_exact_state_memory_budget(
      state, trim, state.cache_opts.memory_budget_bytes);
}

std::size_t estimate_chart_spr_canonical_exact_evidence_resident_bytes(
    chart_spr_canonical_exact_evidence const& evidence);

struct chart_spr_canonical_exact_evidence_memory_estimate {
  std::size_t retained_bytes = 0;
  std::size_t construction_peak_bytes = 0;
};

// Allocation-free exact-size preflight for grammar trim evidence. Canonical
// key builders reserve these encoded lengths before publication; the retained
// estimate therefore also serves as a strict post-build capacity backstop.
chart_spr_canonical_exact_evidence_memory_estimate
estimate_chart_spr_canonical_exact_evidence_memory(
    clade_grammar const& grammar, multisite_trim_result const& trim);

chart_spr_topology_selection_memory_estimate
estimate_chart_spr_topology_selection_memory(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_options const& options);

namespace chart_spr_search_detail {

// Frozen-toolchain capacity walk for storage already live when the exact
// candidate loop begins. This includes every ranked candidate's owning
// overlay/certificate payload, current iteration canonical/affected storage,
// and enough additional payload for one accepted-candidate copy.
std::size_t estimate_exact_loop_resident_input_bytes(
    std::vector<chart_spr_candidate_score> const& ranked,
    chart_spr_iteration_result const& iteration);
std::size_t estimate_exact_loop_accepted_candidate_dynamic_bytes(
    std::span<chart_spr_candidate_score const> ranked);

// Conservative live envelope for the grammar-stream enumerator while it calls
// a scoring-batch callback, plus the measured frozen-toolchain node/string
// charge added for each equality-deduplicated signature retained so far.
std::size_t estimate_grammar_spr_enumeration_fixed_live_bytes(
    clade_grammar const& grammar, chart_execution_plan const& plan);
std::size_t estimate_grammar_spr_enumeration_signature_live_bytes(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate);

struct grammar_spr_finite_iteration_memory_envelope {
  std::size_t planned_required_bytes = 0;
  std::size_t planned_generation_phase_required_bytes = 0;
  std::size_t planned_evidence_phase_required_bytes = 0;
  std::size_t future_dynamic_bytes = 0;
  std::size_t planned_post_release_result_bytes = 0;
  std::size_t planned_ranked_candidate_exact_evidence_bytes = 0;
  std::size_t planned_accepted_candidate_dynamic_bytes = 0;
  std::size_t planned_accepted_candidate_signature_bytes = 0;
  std::size_t planned_local_workspace_resident_bytes = 0;
  std::size_t planned_signature_node_bytes = 0;
  std::size_t planned_candidate_live_bytes = 0;
  std::size_t planned_canonical_record_dynamic_bytes = 0;
  std::size_t planned_canonical_state_exact_evidence_resident_bytes = 0;
  std::size_t planned_canonical_state_exact_evidence_construction_peak_bytes =
      0;
  std::size_t planned_scheduler_operation_peak_bytes = 0;
  std::size_t planned_scheduler_resident_bytes = 0;
  std::size_t planned_local_task_stable_bytes = 0;
  std::size_t planned_local_task_preparation_peak_bytes = 0;
  std::size_t planned_local_weighted_candidate_order_bytes = 0;
  std::size_t planned_pattern_batch_construction_scratch_bytes = 0;
  std::size_t planned_local_untiled_concurrent_preparation_peak_bytes = 0;
  std::size_t planned_local_retained_stable_capacity_bytes = 0;
  std::size_t planned_generation_wave_with_retained_local_peak_bytes = 0;
  chart_spr_cache_strategy planned_cache_strategy =
      chart_spr_cache_strategy::all_active_patterns;
  std::size_t planned_candidate_batch_size = 0;
  std::size_t planned_local_prepared_slots = 0;
  std::size_t planned_local_worker_slots = 0;
  std::size_t planned_local_tile_result_slots = 0;
  std::size_t planned_source_owned_bytes = 0;
  std::size_t planned_enumeration_callback_concurrent_bytes = 0;
  std::size_t planned_candidate_buffer_owned_bytes = 0;
  std::size_t planned_pipeline_control_bytes = 0;
  std::size_t planned_sampled_source_waiting_bytes = 0;
  std::size_t planned_sampled_source_active_scratch_bytes = 0;
  std::size_t planned_sampled_source_scheduler_operation_peak_bytes = 0;
  std::size_t planned_sampled_projection_active_scratch_bytes = 0;
  std::size_t planned_sampled_projection_scheduler_operation_peak_bytes = 0;
  std::size_t planned_sampled_source_admitted_peak_bytes = 0;
  std::size_t planned_sampled_source_count_bound = 0;
  std::size_t planned_sampled_destination_bound_per_source = 0;
  std::size_t planned_sampled_source_wave_size = 0;
  std::size_t planned_sampled_projection_wave_size = 0;
  std::size_t planned_grammar_candidate_wave_owned_bytes = 0;
  std::size_t planned_grammar_candidate_scheduler_operation_peak_bytes = 0;
  std::size_t planned_grammar_candidate_admitted_wave_bytes = 0;
  std::size_t planned_grammar_candidate_wave_size = 0;
  std::size_t candidate_buffer_count = 1;
};

// Allocation-free pre-enumeration envelope for a bounded grammar-native
// stream. The planned surface is the temporal maximum of generation/local/
// exact preparation and post-release canonical-evidence construction.
// future_dynamic_bytes is generation-only and is reused with measured
// capacities for the pre-enumeration reserve backstop.
grammar_spr_finite_iteration_memory_envelope
estimate_grammar_spr_finite_iteration_memory_envelope(
    chart_spr_search_state const& state, std::size_t candidate_limit,
    std::size_t candidate_batch_size, std::size_t ranked_limit,
    bool capture_semantics, chart_scheduler const& scheduler,
    std::size_t local_task_slots,
    grammar_spr_enumeration_options const* source_options = nullptr,
    std::size_t candidate_buffer_count = 1,
    bool include_pipeline_control = false, std::size_t source_wave_size = 0,
    std::size_t projection_wave_size = 0,
    std::size_t grammar_candidate_wave_size = 0);

// Peak coordinator scratch used while producing the per-candidate admission
// estimates. Estimation is serial, so only the largest candidate is charged.
std::size_t estimate_exact_loop_estimator_peak_scratch_bytes(
    chart_spr_search_state const& state,
    std::span<chart_spr_candidate_score const> ranked);

inline std::size_t estimate_chart_spr_scheduler_resident_bytes(
    chart_scheduler const& scheduler) {
  return chart_spr_exact_candidate_checked_bytes_add(
      estimate_chart_scheduler_implementation_resident_bytes(),
      estimate_chart_scheduler_pool_owning_heap_bytes(
          scheduler.worker_resolution().resolved_workers),
      "chart SPR scheduler resident bytes");
}

inline std::size_t estimate_chart_spr_scheduler_operation_peak_for_any_items(
    chart_scheduler const& scheduler, std::size_t maximum_item_count,
    chart_indexed_range_options options) {
  if (maximum_item_count == 0) return 0;
  if (options.minimum_grain != 1 || options.target_ranges_per_worker == 0) {
    throw std::logic_error(
        "chart SPR scheduler-operation bound requires unit grain and an "
        "explicit range target");
  }
  // With unit minimum grain, the operation estimator is largest at the
  // greatest attainable range count. Planning an arbitrarily large N can
  // increase adaptive grain and make ceil(N/grain) fall, so plan the exact
  // saturation pivot instead of assuming monotonicity in N.
  auto const workers = scheduler.worker_resolution().resolved_workers;
  auto const target =
      std::max<std::size_t>(1, options.target_ranges_per_worker);
  auto const range_saturation =
      workers > (std::numeric_limits<std::size_t>::max)() / target
          ? (std::numeric_limits<std::size_t>::max)()
          : workers * target;
  auto const pivot = std::min(maximum_item_count, range_saturation);
  return estimate_chart_scheduler_operation_peak_bytes(
      scheduler.plan_indexed_ranges(pivot, options));
}

inline std::size_t
estimate_chart_spr_state_exact_scheduler_operation_peak_bytes(
    chart_spr_search_state const& state, chart_scheduler const& scheduler) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    // Both lazy state-exact routes are deliberately serial.
    return 0;
  }
  auto const pattern_count = state.active_patterns.patterns.patterns.size();
  std::size_t peak = 0;
  auto observe_actual = [&](std::size_t item_count,
                            chart_indexed_range_options options) {
    peak =
        std::max(peak, estimate_chart_scheduler_operation_peak_bytes(
                           scheduler.plan_indexed_ranges(item_count, options)));
  };
  auto observe_any = [&](std::size_t maximum_item_count,
                         chart_indexed_range_options options) {
    peak = std::max(peak,
                    estimate_chart_spr_scheduler_operation_peak_for_any_items(
                        scheduler, maximum_item_count, options));
  };

  auto const setup_options =
      chart_multisite_detail::multisite_exact_setup_range_options();
  auto const setup_is_scheduled =
      state.cache_strategy == chart_spr_cache_strategy::all_active_patterns ||
      static_cast<bool>(state.scheduled_exact_setup_provider) ||
      !state.exact_setup_provider;
  if (setup_is_scheduled) {
    observe_actual(pattern_count, setup_options);
    auto const topology_count_bound =
        chart_spr_exact_candidate_checked_bytes_add(
            pattern_count, 1, "chart SPR state exact upper-topology count");
    auto const upper_bound_items =
        chart_spr_exact_candidate_checked_bytes_multiply(
            pattern_count, topology_count_bound,
            "chart SPR state exact upper-topology scheduler items");
    observe_any(upper_bound_items, setup_options);
  }

  auto const frontier_options =
      chart_multisite_detail::multisite_frontier_clade_range_options();
  auto const& offsets = state.execution_plan.bottom_up_level_offsets();
  for (std::size_t level = 0; level + 1 < offsets.size(); ++level) {
    observe_actual(offsets[level + 1] - offsets[level], frontier_options);
  }
  return peak;
}

inline std::size_t estimate_chart_spr_state_exact_scheduler_summary_bytes(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    return 0;
  }
  auto const& offsets = state.execution_plan.bottom_up_level_offsets();
  auto const level_count = offsets.empty() ? 0 : offsets.size() - 1;
  auto const frontier_pass_count =
      trim_options.dominance_mode ==
              multisite_dominance_mode::two_pass_exact_mask
          ? std::size_t{2}
          : std::size_t{1};
  auto const frontier_summary_count =
      chart_spr_exact_candidate_checked_bytes_multiply(
          level_count, frontier_pass_count,
          "chart SPR state exact frontier summary count");
  auto const summary_count = chart_spr_exact_candidate_checked_bytes_add(
      2, frontier_summary_count,
      "chart SPR state exact scheduler summary count");
  return chart_spr_exact_candidate_checked_bytes_multiply(
      summary_count, sizeof(chart_scheduler_run_summary),
      "chart SPR state exact scheduler summary bytes");
}

std::size_t estimate_chart_spr_exact_candidate_inner_scheduler_scratch_bytes(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_options const& options, chart_scheduler const& scheduler);

}  // namespace chart_spr_search_detail

chart_spr_exact_candidate_memory_estimate
estimate_chart_spr_exact_candidate_memory(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_options const& options, std::size_t resolved_workers);

chart_spr_exact_candidate_memory_estimate estimate_chart_spr_state_exact_memory(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options, std::size_t resolved_workers);

chart_spr_exact_candidate_memory_estimate estimate_chart_spr_state_exact_memory(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options, std::size_t resolved_workers,
    std::size_t memory_budget_bytes);

inline void require_chart_spr_state_exact_memory_budget(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options, std::size_t resolved_workers,
    std::size_t memory_budget_bytes) {
  if (memory_budget_bytes == 0) return;
  auto const resident_state =
      estimate_chart_spr_published_state_resident_bytes(state);
  if (resident_state > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(resident_state,
                                             memory_budget_bytes);
  }
  // The state estimator uses the same virtual-topology memo/visit/removed
  // vectors as candidate admission. Pre-admit that allocation-free envelope
  // before invoking the allocation-heavy estimator itself.
  chart_spr_candidate_score identity_candidate;
  auto const estimator_scratch =
      chart_spr_search_detail::
          estimate_exact_loop_estimator_peak_scratch_bytes(
              state,
              std::span<chart_spr_candidate_score const>{
                  &identity_candidate, 1});
  auto const estimator_required = chart_spr_exact_candidate_checked_bytes_add(
      resident_state, estimator_scratch,
      "chart SPR exact-state estimator preflight bytes");
  if (estimator_required > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(estimator_required,
                                             memory_budget_bytes);
  }
  auto const estimate = estimate_chart_spr_state_exact_memory(
      state, trim_options, resolved_workers, memory_budget_bytes);
  auto const scratch = resolved_workers > 1
                           ? estimate.inner_parallel_scratch_bytes
                           : estimate.serial_scratch_bytes;
  auto const overflow =
      resident_state > (std::numeric_limits<std::size_t>::max)() - scratch;
  auto const required = overflow || !estimate.safely_bounded
                            ? (std::numeric_limits<std::size_t>::max)()
                            : resident_state + scratch;
  if (required > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(required, memory_budget_bytes);
  }
}

inline void require_chart_spr_state_exact_memory_budget(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options, std::size_t resolved_workers) {
  auto const budget = state.cache_opts.memory_budget_bytes;
  require_chart_spr_state_exact_memory_budget(state, trim_options,
                                              resolved_workers, budget);
}

inline void require_chart_spr_state_exact_memory_budget(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options,
    chart_scheduler const& scheduler, std::size_t memory_budget_bytes) {
  if (memory_budget_bytes == 0) return;
  auto resident_base = estimate_chart_spr_published_state_resident_bytes(state);
  resident_base = chart_spr_exact_candidate_checked_bytes_add(
      resident_base,
      chart_spr_search_detail::estimate_chart_spr_scheduler_resident_bytes(
          scheduler),
      "chart SPR exact-state scheduler ownership");
  if (resident_base > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(resident_base,
                                             memory_budget_bytes);
  }

  // Estimation and construction are sequential phases. Charge the
  // allocation-heavy virtual-topology estimator against the same resident
  // state/pool base, then release it before composing exact setup/frontier
  // scratch with the largest actual scheduler operation.
  chart_spr_candidate_score identity_candidate;
  auto const estimator_scratch =
      chart_spr_search_detail::estimate_exact_loop_estimator_peak_scratch_bytes(
          state,
          std::span<chart_spr_candidate_score const>{&identity_candidate, 1});
  auto const estimator_required = chart_spr_exact_candidate_checked_bytes_add(
      resident_base, estimator_scratch,
      "chart SPR scheduled exact-state estimator preflight bytes");
  if (estimator_required > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(estimator_required,
                                             memory_budget_bytes);
  }

  auto const workers = scheduler.worker_resolution().resolved_workers;
  auto const estimate = estimate_chart_spr_state_exact_memory(
      state, trim_options, workers, memory_budget_bytes);
  auto construction_scratch = estimate.inner_parallel_scratch_bytes;
  construction_scratch = chart_spr_exact_candidate_checked_bytes_add(
      construction_scratch,
      chart_spr_search_detail::
          estimate_chart_spr_state_exact_scheduler_operation_peak_bytes(
              state, scheduler),
      "chart SPR scheduled exact-state operation scratch");
  construction_scratch = chart_spr_exact_candidate_checked_bytes_add(
      construction_scratch,
      chart_spr_search_detail::
          estimate_chart_spr_state_exact_scheduler_summary_bytes(state,
                                                                 trim_options),
      "chart SPR scheduled exact-state run summaries");
  auto const required =
      estimate.safely_bounded
          ? chart_spr_exact_candidate_checked_bytes_add(
                resident_base, construction_scratch,
                "chart SPR scheduled exact-state construction bytes")
          : (std::numeric_limits<std::size_t>::max)();
  if (required > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(required, memory_budget_bytes);
  }
}

inline void require_chart_spr_state_exact_memory_budget(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options,
    chart_scheduler const& scheduler) {
  require_chart_spr_state_exact_memory_budget(
      state, trim_options, scheduler, state.cache_opts.memory_budget_bytes);
}

inline void require_chart_spr_retained_exact_state_memory_budget(
    chart_spr_search_state const& state, multisite_trim_result const& trim,
    chart_scheduler const& scheduler, std::size_t memory_budget_bytes) {
  if (memory_budget_bytes == 0) return;
  auto required = chart_spr_exact_candidate_checked_bytes_add(
      estimate_chart_spr_state_core_resident_bytes(state),
      estimate_chart_spr_selected_cache_dynamic_resident_bytes(state),
      "chart SPR retained scheduled exact-state core/cache bytes");
  required = chart_spr_exact_candidate_checked_bytes_add(
      required, estimate_chart_spr_trim_dynamic_resident_bytes(trim),
      "chart SPR retained scheduled exact-state trim bytes");
  required = chart_spr_exact_candidate_checked_bytes_add(
      required,
      chart_spr_search_detail::estimate_chart_spr_scheduler_resident_bytes(
          scheduler),
      "chart SPR retained scheduled exact-state scheduler ownership");
  if (required > memory_budget_bytes) {
    throw chart_spr_exact_state_budget_error(required, memory_budget_bytes);
  }
}

inline void require_chart_spr_retained_exact_state_memory_budget(
    chart_spr_search_state const& state, multisite_trim_result const& trim,
    chart_scheduler const& scheduler) {
  require_chart_spr_retained_exact_state_memory_budget(
      state, trim, scheduler, state.cache_opts.memory_budget_bytes);
}

namespace chart_spr_search_detail {

inline void require_completed_chart_spr_state_bootstrap(
    chart_spr_search_state const& state, std::string_view consumer) {
  if (state.pattern_batch_bootstrap_deferred) {
    throw std::runtime_error(
        std::string{consumer} +
        ": deferred local-cache bootstrap has not been finalized by the "
        "local persistent inside cache");
  }
}

// Complete the private non-lazy publication after the local persistent cache
// has built the one authoritative set of dense inside charts. The cache
// composite includes the state's invariant constant exactly once.
inline void finalize_deferred_pattern_batch_bootstrap(
    chart_spr_search_state& state,
    std::uint64_t composite_lower_bound_with_invariants,
    double chart_construction_ms = 0.0) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart ||
      !state.pattern_batch_bootstrap_deferred) {
    throw std::runtime_error(
        "chart SPR search state: no deferred local-cache bootstrap to "
        "finalize");
  }
  if (!state.pattern_charts.empty() || state.lazy_chart) {
    throw std::runtime_error(
        "chart SPR search state: deferred local-cache bootstrap acquired an "
        "unexpected resident chart representation");
  }
  if (composite_lower_bound_with_invariants < state.invariant_constant_offset) {
    throw std::runtime_error(
        "chart SPR search state: deferred local-cache composite is below "
        "the invariant offset");
  }
  state.composite_lower_bound_with_invariants =
      composite_lower_bound_with_invariants;
  state.composite_lower_bound_without_invariants =
      composite_lower_bound_with_invariants - state.invariant_constant_offset;
  state.chart_construction_ms = chart_construction_ms;
  state.pattern_batch_bootstrap_deferred = false;
}

}  // namespace chart_spr_search_detail

namespace chart_spr_search_detail {

inline std::size_t estimate_owned_string_capacity_bytes(
    std::string const& value, std::string_view context) {
  // The terminating null is owned storage too. Charging cap+1 is also a
  // conservative allowance for SSO strings whose inline object bytes are
  // already present in the surrounding vector/node capacity.
  return chart_spr_exact_candidate_checked_bytes_add(value.capacity(), 1,
                                                     context);
}

}  // namespace chart_spr_search_detail

inline std::size_t estimate_chart_spr_pattern_cache_bytes(
    chart_spr_search_state const& state) {
  auto checked_add = [](std::size_t lhs, std::size_t rhs) {
    if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
      throw std::overflow_error("chart SPR pattern-cache byte overflow");
    }
    return lhs + rhs;
  };
  auto capacity_bytes = [](auto const& values) {
    using vector_type = std::remove_cvref_t<decltype(values)>;
    if (values.capacity() != 0 &&
        sizeof(typename vector_type::value_type) >
            (std::numeric_limits<std::size_t>::max)() / values.capacity()) {
      throw std::overflow_error("chart SPR pattern-cache byte overflow");
    }
    return values.capacity() * sizeof(typename vector_type::value_type);
  };
  auto add_nested_capacity = [&](std::size_t total, auto const& nested) {
    total = checked_add(total, capacity_bytes(nested));
    for (auto const& values : nested) {
      total = checked_add(total, capacity_bytes(values));
    }
    return total;
  };
  auto add_optional_map_capacity = [&](std::size_t total, auto const& maps) {
    total = checked_add(total, capacity_bytes(maps));
    for (auto const& map : maps) {
      if (map) total = checked_add(total, capacity_bytes(*map));
    }
    return total;
  };

  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    if (!state.lazy_chart) return 0;
    auto const& chart = *state.lazy_chart;
    std::size_t total = sizeof(lazy_multisite_chart);
    total = add_nested_capacity(total, chart.inside_rows_by_clade);
    total = add_nested_capacity(total, chart.outside_rows_by_clade);
    total =
        add_optional_map_capacity(total, chart.class_index_by_pattern_by_clade);
    total = add_optional_map_capacity(
        total, chart.structural_class_index_by_pattern_by_clade);
    total = add_optional_map_capacity(
        total, chart.outside_class_index_by_pattern_by_clade);
    total = checked_add(total,
                        capacity_bytes(chart.structural_class_count_by_clade));
    total = add_nested_capacity(total, chart.class_weight_by_clade);
    total = add_nested_capacity(total, chart.outside_class_weight_by_clade);
    total =
        checked_add(total, capacity_bytes(chart.outside_global_min_by_pattern));
    return total;
  }
  std::size_t total = capacity_bytes(state.pattern_charts);
  for (auto const& entry : state.pattern_charts) {
    total = checked_add(total, capacity_bytes(entry.chart.inside));
    total = checked_add(total, capacity_bytes(entry.chart.optimal_choices));
    for (auto const& choices_by_state : entry.chart.optimal_choices) {
      for (auto const& choices : choices_by_state) {
        total = checked_add(total, capacity_bytes(choices));
      }
    }
  }
  return total;
}

inline std::size_t estimate_chart_spr_selected_cache_dynamic_resident_bytes(
    chart_spr_search_state const& state) {
  auto total = state.resident_pattern_cache_bytes;
  if (state.cache_strategy != chart_spr_cache_strategy::lazy_multisite_chart ||
      total == 0) {
    return total;
  }
  // optional<lazy_multisite_chart>'s fixed object storage is already part of
  // sizeof(chart_spr_search_state).  The resident cache aggregate may also
  // contain local_commit_persistent_cache_bytes, so remove exactly the one
  // overlapped fixed object rather than reconstructing or adding components.
  if (total < sizeof(lazy_multisite_chart)) {
    throw std::logic_error(
        "chart SPR lazy resident cache is smaller than its fixed object");
  }
  return total - sizeof(lazy_multisite_chart);
}

// Resident storage already owned by a partially constructed search state and
// overlapping lazy inside/outside construction.  The DAG is non-owning here;
// all owning grammar, immutable-plan, active-pattern, and state buffers are
// charged from allocator-selected capacities.
inline std::size_t estimate_chart_spr_state_core_resident_bytes(
    chart_spr_search_state const& state) {
  auto add = [](std::size_t lhs, std::size_t rhs, std::string_view context) {
    return chart_spr_exact_candidate_checked_bytes_add(lhs, rhs, context);
  };
  auto capacity_bytes = [](auto const& values, std::string_view context) {
    using vector_type = std::remove_cvref_t<decltype(values)>;
    return chart_spr_exact_candidate_checked_bytes_multiply(
        values.capacity(), sizeof(typename vector_type::value_type), context);
  };
  std::size_t total = sizeof(state);
  auto const callback_count = chart_spr_state_callback_function_count(state);
  if (callback_count != 0 &&
      (!state.callback_target_resident_safely_bounded ||
       state.callback_target_resident_contract_function_count !=
           callback_count ||
       state.callback_target_resident_contract_revisions !=
           chart_spr_state_callback_revisions(state))) {
    throw std::runtime_error(
        "chart SPR state resident estimate: installed callback targets lack "
        "a complete persistent-resident contract");
  }
  if (callback_count != 0) {
    total = add(total, state.callback_target_resident_bytes,
                "chart SPR retained callback targets");
  }
  auto add_vector = [&](auto const& values, std::string_view context) {
    total = add(total, capacity_bytes(values, context),
                "chart SPR lazy retained state");
  };

  auto const& grammar = state.grammar;
  add_vector(grammar.taxa.id_to_sample_id, "chart SPR retained taxon names");
  for (auto const& name : grammar.taxa.id_to_sample_id) {
    total = add(total,
                chart_spr_search_detail::estimate_owned_string_capacity_bytes(
                    name, "chart SPR retained taxon strings"),
                "chart SPR retained taxon strings");
  }
  // unordered_map node/bucket allocation is not exposed directly by the
  // standard library.  Charge the observable bucket array plus a conservative
  // node envelope and each stored key's selected capacity.
  total = add(total,
              chart_spr_exact_candidate_checked_bytes_multiply(
                  grammar.taxa.sample_id_to_id.bucket_count(), sizeof(void*),
                  "chart SPR retained taxon hash buckets"),
              "chart SPR lazy retained state");
  total = add(
      total,
      chart_spr_exact_candidate_checked_bytes_multiply(
          grammar.taxa.sample_id_to_id.size(),
          sizeof(std::pair<std::string const, taxon_id>) + 2 * sizeof(void*),
          "chart SPR retained taxon hash nodes"),
      "chart SPR lazy retained state");
  for (auto const& [name, unused_taxon] : grammar.taxa.sample_id_to_id) {
    (void)unused_taxon;
    total = add(total,
                chart_spr_search_detail::estimate_owned_string_capacity_bytes(
                    name, "chart SPR retained taxon hash strings"),
                "chart SPR retained taxon hash strings");
  }
  add_vector(grammar.clades, "chart SPR retained clades");
  for (auto const& clade : grammar.clades) {
    total =
        add(total, capacity_bytes(clade.taxa, "chart SPR retained clade taxa"),
            "chart SPR lazy retained state");
  }
  add_vector(grammar.productions, "chart SPR retained productions");
  for (auto const& production : grammar.productions) {
    total = add(total,
                capacity_bytes(production.children,
                               "chart SPR retained production children"),
                "chart SPR lazy retained state");
    total = add(total,
                capacity_bytes(production.witnesses,
                               "chart SPR retained production witnesses"),
                "chart SPR lazy retained state");
    for (auto const& witness : production.witnesses) {
      total = add(total,
                  capacity_bytes(witness.children,
                                 "chart SPR retained witness children"),
                  "chart SPR lazy retained state");
      for (auto const& child : witness.children) {
        total = add(total,
                    capacity_bytes(child.edge_alternatives,
                                   "chart SPR retained witness edges"),
                    "chart SPR lazy retained state");
      }
    }
  }
  auto add_nested = [&](auto const& nested, std::string_view context) {
    add_vector(nested, context);
    for (auto const& values : nested) {
      total = add(total, capacity_bytes(values, context),
                  "chart SPR lazy retained state");
    }
  };
  add_nested(grammar.productions_by_parent,
             "chart SPR retained productions by parent");
  add_nested(grammar.productions_by_child,
             "chart SPR retained productions by child");
  add_vector(grammar.node_to_clade, "chart SPR retained node map");
  total = add(total, state.execution_plan.dynamic_capacity_bytes(),
              "chart SPR retained execution plan");

  auto const& patterns = state.active_patterns.patterns;
  add_vector(patterns.patterns, "chart SPR retained patterns");
  for (auto const& pattern : patterns.patterns) {
    total = add(total,
                capacity_bytes(pattern.state_by_taxon,
                               "chart SPR retained pattern states"),
                "chart SPR lazy retained state");
    total = add(total,
                capacity_bytes(pattern.positions,
                               "chart SPR retained pattern positions"),
                "chart SPR lazy retained state");
  }
  add_vector(patterns.original_site_to_pattern,
             "chart SPR retained site-pattern map");
  add_vector(patterns.normalized_binary_patterns,
             "chart SPR retained normalized patterns");
  for (auto const& pattern : patterns.normalized_binary_patterns) {
    total = add(total,
                capacity_bytes(pattern.state_by_taxon,
                               "chart SPR retained normalized states"),
                "chart SPR lazy retained state");
    total = add(total,
                capacity_bytes(pattern.positions,
                               "chart SPR retained normalized positions"),
                "chart SPR lazy retained state");
    total = add(total,
                capacity_bytes(pattern.exact_pattern_indices,
                               "chart SPR retained normalized indices"),
                "chart SPR lazy retained state");
    total = add(total,
                capacity_bytes(pattern.exact_state_maps,
                               "chart SPR retained normalized state maps"),
                "chart SPR lazy retained state");
  }
  add_vector(patterns.exact_pattern_to_normalized_binary_pattern,
             "chart SPR retained normalized-pattern map");
  add_vector(patterns.exact_pattern_to_normalized_binary_state_map,
             "chart SPR retained normalized-state map");
  // make_shared co-allocates the tracker with a frozen-libstdc++ control
  // block. The shared_ptr object itself is already inside sizeof(state); this
  // allowance covers the tracker, strong/weak atomics, vptr/control metadata,
  // and allocator alignment/header.
  total = add(total,
              sizeof(chart_spr_exact_verifier_concurrency_tracker) +
                  4 * sizeof(void*) + 2 * sizeof(std::size_t),
              "chart SPR retained verifier tracker/control block");
  return total;
}

// State construction can own a partially filled pattern_charts buffer before
// resident_pattern_cache_bytes has been replaced by its post-build capacity
// walk.  Keep that build-only surface outside the cache-independent published
// state core.
inline std::size_t estimate_chart_spr_lazy_state_build_retained_bytes(
    chart_spr_search_state const& state) {
  auto const pattern_chart_slots =
      chart_spr_exact_candidate_checked_bytes_multiply(
          state.pattern_charts.capacity(), sizeof(pattern_chart_cache_entry),
          "chart SPR retained pattern-chart slots");
  return chart_spr_exact_candidate_checked_bytes_add(
      estimate_chart_spr_state_core_resident_bytes(state), pattern_chart_slots,
      "chart SPR retained state build bytes");
}

// The automatic-policy pilot and the selected representation are disjoint
// temporal phases.  The state core (plus an already-published state during a
// frozen rebuild) survives across both; an externally supplied scheduler is
// also live during the serial pilot even though the pilot submits no work.
inline std::size_t estimate_chart_spr_lazy_policy_pilot_required_bytes(
    chart_spr_search_state const& state, std::size_t pilot_transient_bytes,
    chart_scheduler const* live_scheduler = nullptr,
    std::size_t overlapping_published_state_bytes = 0) {
  auto base = chart_spr_exact_candidate_checked_bytes_add(
      overlapping_published_state_bytes,
      estimate_chart_spr_state_core_resident_bytes(state),
      "chart SPR lazy-policy pilot live state");
  if (live_scheduler != nullptr) {
    base = chart_spr_exact_candidate_checked_bytes_add(
        base,
        chart_spr_search_detail::estimate_chart_spr_scheduler_resident_bytes(
            *live_scheduler),
        "chart SPR lazy-policy pilot live scheduler");
  }
  return chart_spr_exact_candidate_checked_bytes_add(
      base, pilot_transient_bytes,
      "chart SPR lazy-policy pilot required bytes");
}

// Allocation-free upper bound shared by the state-level preflight and strict
// E/E-1 tests.  selected_dynamic_resident_bytes is the chosen lazy chart plus
// any representation-coupled reservation that will coexist with it.  The
// scheduled builders independently enforce the same envelope from measured
// capacities and remain the post-projection backstop.
inline std::size_t estimate_chart_spr_lazy_state_build_required_bytes(
    chart_spr_search_state const& state,
    std::size_t selected_dynamic_resident_bytes,
    chart_scheduler const* scheduler = nullptr,
    std::size_t overlapping_published_state_bytes = 0) {
  auto retained = chart_spr_exact_candidate_checked_bytes_add(
      overlapping_published_state_bytes,
      estimate_chart_spr_lazy_state_build_retained_bytes(state),
      "chart SPR lazy state-build overlapping published state");
  auto const clade_count = state.execution_plan.clades().size();
  auto maximum_inside_width = std::size_t{0};
  for (auto clade : state.execution_plan.bottom_up_order()) {
    if (!state.execution_plan.clade(clade).is_leaf()) {
      maximum_inside_width =
          std::max(maximum_inside_width,
                   lazy_chart_detail::plan_inside_clade_row_key_width(
                       state.execution_plan, clade));
    }
  }
  auto const pattern_count = state.active_patterns.patterns.patterns.size();
  auto inside_transient =
      lazy_chart_detail::logical_plan_inside_coordinator_bytes(
          clade_count, false,
          static_cast<std::vector<chart_scheduler_run_summary> const*>(
              nullptr));
  inside_transient = chart_spr_exact_candidate_checked_bytes_add(
      inside_transient,
      lazy_chart_detail::logical_plan_inside_slot_resident_bytes(
          pattern_count, maximum_inside_width),
      "chart SPR lazy inside singleton preflight");
  inside_transient = chart_spr_exact_candidate_checked_bytes_add(
      inside_transient,
      lazy_chart_detail::logical_plan_inside_slot_preparation_extra_bytes(
          pattern_count),
      "chart SPR lazy inside preparation preflight");
  inside_transient = chart_spr_exact_candidate_checked_bytes_add(
      inside_transient,
      lazy_chart_detail::logical_plan_output_preparation_extra_bytes(),
      "chart SPR lazy inside output preparation preflight");
  inside_transient = chart_spr_exact_candidate_checked_bytes_add(
      inside_transient,
      sizeof(lazy_chart_detail::plan_lazy_chart_scheduler_workspace),
      "chart SPR lazy inside workspace preflight");
  inside_transient = chart_spr_exact_candidate_checked_bytes_add(
      inside_transient,
      chart_spr_exact_candidate_checked_bytes_multiply(
          clade_count, sizeof(chart_scheduler_run_summary),
          "chart SPR lazy inside run-summary preflight"),
      "chart SPR lazy inside run-summary preflight");

  auto outside_transient =
      lazy_chart_detail::logical_plan_outside_coordinator_bytes(
          clade_count, state.execution_plan.top_down_level_offsets().size() - 1,
          static_cast<std::vector<chart_scheduler_run_summary> const*>(
              nullptr));
  outside_transient = chart_spr_exact_candidate_checked_bytes_add(
      outside_transient,
      lazy_chart_detail::logical_plan_outside_slot_resident_bytes(
          pattern_count, state.execution_plan.max_arity() + 1,
          state.execution_plan.max_arity()),
      "chart SPR lazy outside singleton preflight");
  outside_transient = chart_spr_exact_candidate_checked_bytes_add(
      outside_transient,
      lazy_chart_detail::logical_plan_outside_slot_preparation_extra_bytes(),
      "chart SPR lazy outside preparation preflight");
  outside_transient = chart_spr_exact_candidate_checked_bytes_add(
      outside_transient,
      lazy_chart_detail::logical_plan_output_preparation_extra_bytes(),
      "chart SPR lazy outside output preparation preflight");
  outside_transient = chart_spr_exact_candidate_checked_bytes_add(
      outside_transient,
      sizeof(lazy_chart_detail::plan_lazy_chart_scheduler_workspace),
      "chart SPR lazy outside workspace preflight");
  // Outside construction overlaps its own summaries and the retained inside
  // summaries published by the preceding phase.
  outside_transient = chart_spr_exact_candidate_checked_bytes_add(
      outside_transient,
      chart_spr_exact_candidate_checked_bytes_multiply(
          clade_count, 2 * sizeof(chart_scheduler_run_summary),
          "chart SPR lazy outside run-summary overlap preflight"),
      "chart SPR lazy outside run-summary overlap preflight");

  auto transient = state.chart_opts.score_ua_edge
                       ? inside_transient
                       : std::max(inside_transient, outside_transient);
  // Finite direct builds instantiate the same W1 scheduler immediately below;
  // a null external scheduler therefore means one owned worker, not no pool.
  auto const worker_count =
      scheduler != nullptr ? scheduler->worker_resolution().resolved_workers
                           : std::size_t{1};
  transient = chart_spr_exact_candidate_checked_bytes_add(
      transient,
      chart_spr_exact_candidate_checked_bytes_add(
          estimate_chart_scheduler_implementation_resident_bytes(),
          estimate_chart_scheduler_pool_owning_heap_bytes(worker_count),
          "chart SPR lazy scheduler ownership preflight"),
      "chart SPR lazy scheduler ownership preflight");
  return chart_spr_exact_candidate_checked_bytes_add(
      retained,
      chart_spr_exact_candidate_checked_bytes_add(
          selected_dynamic_resident_bytes, transient,
          "chart SPR lazy state-build selected representation"),
      "chart SPR lazy state-build required bytes");
}

inline void add_lazy_chart_build_counters(chart_spr_search_counters& counters,
                                          lazy_multisite_chart const& chart) {
  counters.lazy_inside_rows_computed += chart.lazy_inside_rows_computed;
  counters.lazy_outside_rows_computed += chart.lazy_outside_rows_computed;
  counters.lazy_patterns_merged_max =
      std::max(counters.lazy_patterns_merged_max,
               chart.lazy_patterns_merged_max);
  counters.lazy_remerge_collisions += chart.lazy_remerge_collisions;
  counters.lazy_structural_class_count_max =
      std::max(counters.lazy_structural_class_count_max,
               chart.lazy_structural_class_count_max);
}

inline void add_lazy_chart_memory_report(
    chart_spr_search_counters& counters,
    lazy_chart_detail::plan_lazy_chart_memory_report const& report) {
  counters.lazy_chart_memory_budget_bytes = report.memory_budget_bytes;
  counters.lazy_chart_inside_max_admitted_slots =
      report.inside_max_admitted_slots;
  counters.lazy_chart_outside_max_admitted_slots =
      report.outside_max_admitted_slots;
  counters.lazy_chart_inside_admission_waves = report.inside_admission_waves;
  counters.lazy_chart_outside_admission_waves = report.outside_admission_waves;
  counters.lazy_chart_inside_memory_limited_levels =
      report.inside_memory_limited_levels;
  counters.lazy_chart_outside_memory_limited_levels =
      report.outside_memory_limited_levels;
  counters.lazy_chart_inside_reused_slot_waves =
      report.inside_reused_slot_waves;
  counters.lazy_chart_outside_reused_slot_waves =
      report.outside_reused_slot_waves;
  counters.lazy_chart_inside_workspace_evictions =
      report.inside_workspace_evictions;
  counters.lazy_chart_outside_workspace_evictions =
      report.outside_workspace_evictions;
  counters.lazy_chart_preflight_peak_bytes =
      report.preflight_peak_capacity_resident_bytes;
  counters.lazy_chart_actual_peak_bytes =
      report.actual_peak_capacity_resident_bytes;
  counters.lazy_chart_pre_submit_rejections = report.pre_submit_rejections;
}

inline std::size_t estimate_chart_spr_full_pattern_cache_bytes(
    chart_spr_search_state const& state) {
  return estimate_chart_spr_full_pattern_cache_bytes(state.grammar,
                                                    state.active_patterns);
}

// Build the current state's active-pattern exact trim using the representation
// already selected for local scoring.  All setup sources return one owning
// finalized setup, and common trim construction/counter recording stays here.
// All-active states consume resident pattern charts, lazy states retain their
// class-compressed bridge, and pattern-batch states consume an installed
// persistent-cache setup provider when available (otherwise they use the cold
// conservative path).
inline multisite_trim_result build_chart_spr_state_exact_trim(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    multisite_trim_options const& trim_options = {}) {
  chart_spr_search_detail::require_completed_chart_spr_state_bootstrap(
      state, "chart SPR exact trim");
  state.active_patterns.assert_no_skipped_invariant_metadata();
  checked_state.assert_same(state.grammar, state.execution_plan);
  require_chart_spr_state_exact_memory_budget(state, trim_options, 1);

  multisite_trim_result trim;
  if (state.local_commit_inside_rows.valid()) {
    if (!state.exact_setup_provider) {
      throw std::runtime_error(
          "chart SPR exact trim: inside-row view has no exact setup provider");
    }
    auto setup = state.exact_setup_provider(state, checked_state);
    trim = build_multisite_trim_from_exact_setup(
        state.execution_plan, setup, state.chart_opts, trim_options);
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::all_active_patterns) {
    auto const pattern_count =
        state.active_patterns.patterns.patterns.size();
    if (state.pattern_charts.size() != pattern_count) {
      throw std::runtime_error(
          "chart SPR exact trim: all-active resident chart count mismatch");
    }
    auto setup = build_multisite_exact_setup_from_resident_inside(
        state.execution_plan, state.active_patterns.patterns,
        [&](std::size_t pattern_index,
            site_pattern const&) -> single_site_chart const& {
          if (pattern_index >= state.pattern_charts.size()) {
            throw std::runtime_error(
                "chart SPR exact trim: resident pattern index out of range");
          }
          return state.pattern_charts[pattern_index].chart;
        },
        state.chart_opts);
    trim = build_multisite_trim_from_exact_setup(
        state.execution_plan, setup, state.chart_opts, trim_options);
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::lazy_multisite_chart) {
    if (!state.lazy_chart) {
      throw std::runtime_error(
          "chart SPR exact trim: lazy cache strategy without lazy chart");
    }
    trim = build_multisite_trim_active(
        state.execution_plan, state.active_patterns, *state.lazy_chart,
        state.chart_opts, trim_options);
    ++state.counters.exact_trim_lazy_chart_uses;
  } else if (state.exact_setup_provider) {
    auto setup = state.exact_setup_provider(state, checked_state);
    trim = build_multisite_trim_from_exact_setup(
        state.execution_plan, setup, state.chart_opts, trim_options);
  } else {
    trim = build_multisite_trim_active(
        state.execution_plan, state.active_patterns, state.chart_opts,
        trim_options);
  }
  record_multisite_exact_trim_work(state.counters, trim);
  return trim;
}

inline multisite_trim_result build_chart_spr_state_exact_trim(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options = {}) {
  auto checked =
      check_chart_execution_plan(state.grammar, state.execution_plan);
  return build_chart_spr_state_exact_trim(state, checked, trim_options);
}

inline multisite_trim_result build_chart_spr_state_exact_trim(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    chart_scheduler& scheduler,
    multisite_trim_options const& trim_options = {}) {
  chart_spr_search_detail::require_completed_chart_spr_state_bootstrap(
      state, "chart SPR exact trim");
  state.active_patterns.assert_no_skipped_invariant_metadata();
  checked_state.assert_same(state.grammar, state.execution_plan);
  require_chart_spr_state_exact_memory_budget(state, trim_options, scheduler);

  multisite_trim_scheduler_run_summaries runs;
  // Both current scheduled setup providers issue at most the active-pattern
  // and upper-bound-topology operations. Reserve before either can run so an
  // allocation failure cannot lose an already completed run summary.
  if (state.cache_strategy != chart_spr_cache_strategy::lazy_multisite_chart) {
    runs.exact_setup.reserve(2);
  }
  chart_spr_scheduler_run_axis_publisher publish_setup_runs{
      state.counters.scheduler_axes.exact_setup_patterns, runs.exact_setup};
  chart_spr_scheduler_run_axis_publisher publish_frontier_runs{
      state.counters.scheduler_axes.exact_frontier_clades,
      runs.frontier_clades};
  multisite_trim_result trim;
  if (state.local_commit_inside_rows.valid()) {
    if (state.scheduled_exact_setup_provider) {
      auto setup = state.scheduled_exact_setup_provider(
          state, checked_state, scheduler, &runs.exact_setup);
      trim = build_multisite_trim_from_exact_setup(state.execution_plan, setup,
                                                   scheduler, state.chart_opts,
                                                   trim_options, &runs);
    } else if (state.exact_setup_provider) {
      auto setup = state.exact_setup_provider(state, checked_state);
      trim = build_multisite_trim_from_exact_setup(state.execution_plan, setup,
                                                   scheduler, state.chart_opts,
                                                   trim_options, &runs);
    } else {
      throw std::runtime_error(
          "chart SPR exact trim: inside-row view has no exact setup provider");
    }
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::all_active_patterns) {
    auto const pattern_count = state.active_patterns.patterns.patterns.size();
    if (state.pattern_charts.size() != pattern_count) {
      throw std::runtime_error(
          "chart SPR exact trim: all-active resident chart count mismatch");
    }
    auto setup = build_multisite_exact_setup_from_resident_inside(
        state.execution_plan, state.active_patterns.patterns, scheduler,
        [&](std::size_t pattern_index, site_pattern const&,
            std::size_t) -> single_site_chart const& {
          if (pattern_index >= state.pattern_charts.size()) {
            throw std::runtime_error(
                "chart SPR exact trim: resident pattern index out of range");
          }
          return state.pattern_charts[pattern_index].chart;
        },
        state.chart_opts, &runs.exact_setup);
    trim = build_multisite_trim_from_exact_setup(state.execution_plan, setup,
                                                 scheduler, state.chart_opts,
                                                 trim_options, &runs);
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::lazy_multisite_chart) {
    if (!state.lazy_chart) {
      throw std::runtime_error(
          "chart SPR exact trim: lazy cache strategy without lazy chart");
    }
    trim = build_multisite_trim_active(state.execution_plan,
                                       state.active_patterns, *state.lazy_chart,
                                       state.chart_opts, trim_options);
    ++state.counters.exact_trim_lazy_chart_uses;
  } else if (state.scheduled_exact_setup_provider) {
    auto setup = state.scheduled_exact_setup_provider(
        state, checked_state, scheduler, &runs.exact_setup);
    trim = build_multisite_trim_from_exact_setup(state.execution_plan, setup,
                                                 scheduler, state.chart_opts,
                                                 trim_options, &runs);
  } else if (state.exact_setup_provider) {
    auto setup = state.exact_setup_provider(state, checked_state);
    trim = build_multisite_trim_from_exact_setup(state.execution_plan, setup,
                                                 scheduler, state.chart_opts,
                                                 trim_options, &runs);
  } else {
    auto setup = build_multisite_exact_setup(
        state.execution_plan, state.active_patterns.patterns, scheduler,
        state.chart_opts, &runs.exact_setup);
    trim = build_multisite_trim_from_exact_setup(state.execution_plan, setup,
                                                 scheduler, state.chart_opts,
                                                 trim_options, &runs);
  }
  record_multisite_exact_trim_work(state.counters, trim);
  return trim;
}

inline chart_spr_search_state build_chart_spr_search_state_from_active(
    phylo_dag& dag, clade_grammar grammar,
    chart_spr_active_pattern_build_result active_build,
    chart_options options = {}, bool build_exact_trim = false,
    multisite_trim_options const& trim_options = {},
    chart_cache_options cache = {},
    chart_spr_search_detail::chart_spr_state_build_policy build_policy = {},
    chart_scheduler* scheduler = nullptr,
    chart_spr_scheduler_axis_counters* failed_scheduler_axes = nullptr,
    chart_spr_search_detail::chart_spr_lazy_policy_rebuild_token const*
        lazy_policy_rebuild_token = nullptr,
    std::size_t overlapping_published_state_bytes = 0) {
  validate_supported_chart_cache_options(cache);
  active_build.active_patterns.assert_no_skipped_invariant_metadata();

  chart_spr_search_state state;
  chart_spr_failed_scheduler_axes_publisher failed_axes_publisher{
      state.counters.scheduler_axes, failed_scheduler_axes};
  state.dag = &dag;
  state.grammar = std::move(grammar);
  if (state.grammar.execution_generation == 0) {
    state.grammar.execution_generation =
        detail::allocate_clade_grammar_execution_generation();
  }
  state.execution_plan = build_chart_execution_plan(state.grammar);
  record_chart_execution_plan_build_stats(state.counters,
                                          state.execution_plan.build_stats());
  if (build_exact_trim) {
    chart_spr_search_detail::
        validate_chart_spr_exact_multisite_multifurcation_gate(
            state.grammar, "chart SPR search state");
  }
  chart_spr_search_detail::validate_active_patterns_for_execution_plan(
      state.execution_plan, active_build.active_patterns.patterns, options);
  state.active_patterns = std::move(active_build.active_patterns);
  state.pattern_source_fingerprint =
      active_build.pattern_source_fingerprint;
  state.invariant_constant_offset = active_build.invariant_constant_offset;
  state.skipped_invariant_site_count =
      active_build.skipped_invariant_site_count;
  state.chart_opts = options;
  state.estimated_full_pattern_cache_bytes =
      estimate_chart_spr_full_pattern_cache_bytes(state);
  auto cache_selection_options = cache;
  auto const state_core_resident_bytes =
      estimate_chart_spr_state_core_resident_bytes(state);
  auto const state_build_live_base_bytes =
      chart_spr_exact_candidate_checked_bytes_add(
          overlapping_published_state_bytes, state_core_resident_bytes,
          "chart SPR state-build overlapping published state");
  if (cache.memory_budget_bytes != 0) {
    if (state_build_live_base_bytes > cache.memory_budget_bytes) {
      if (build_exact_trim) {
        throw chart_spr_exact_state_budget_error(state_build_live_base_bytes,
                                                 cache.memory_budget_bytes);
      }
      throw std::runtime_error(
          "chart SPR search state: cache-independent state core requires " +
          std::to_string(state_build_live_base_bytes) +
          " bytes including any overlapping published state, exceeding "
          "configured budget " +
          std::to_string(cache.memory_budget_bytes));
    }
    cache_selection_options.memory_budget_bytes =
        cache.memory_budget_bytes - state_build_live_base_bytes;
  }
  // The serial bounded pilot does not overlap future local-cache reservations
  // or the chosen representation. It does overlap the new state core, any
  // still-published old state during a frozen rebuild, and an externally live
  // scheduler (despite submitting no pilot work).
  auto policy_resolution_options = cache;
  if (cache.memory_budget_bytes != 0) {
    auto policy_live_base_bytes = state_build_live_base_bytes;
    if (scheduler != nullptr &&
        chart_spr_search_detail::requested_chart_spr_lazy_policy(cache) ==
            chart_spr_lazy_policy::automatic) {
      policy_live_base_bytes = chart_spr_exact_candidate_checked_bytes_add(
          policy_live_base_bytes,
          chart_spr_search_detail::estimate_chart_spr_scheduler_resident_bytes(
              *scheduler),
          "chart SPR lazy-policy pilot live scheduler");
    }
    if (policy_live_base_bytes > cache.memory_budget_bytes) {
      throw std::runtime_error(
          "chart SPR search state: lazy-policy live base requires " +
          std::to_string(policy_live_base_bytes) +
          " bytes, exceeding configured budget " +
          std::to_string(cache.memory_budget_bytes));
    }
    policy_resolution_options.memory_budget_bytes =
        cache.memory_budget_bytes - policy_live_base_bytes;
  }
  std::size_t reserved_local_commit_cache_bytes = 0;
  if (build_policy.defer_pattern_batch_bootstrap_to_local_cache &&
      cache.memory_budget_bytes != 0) {
    auto const full_bytes = state.estimated_full_pattern_cache_bytes;
    if (full_bytes > (std::numeric_limits<std::size_t>::max)() / 2) {
      throw std::overflow_error(
          "chart SPR local-commit mandatory cache byte overflow");
    }
    auto const mandatory_pair_bytes = full_bytes * 2;
    reserved_local_commit_cache_bytes = mandatory_pair_bytes;
    if (mandatory_pair_bytes > cache_selection_options.memory_budget_bytes) {
      throw std::runtime_error(
          "chart SPR local commit: configured cache budget cannot hold the "
          "state core and mandatory full inside/outside caches");
    }
    // The strategy selector owns only the remainder. A literal zero means
    // "unbounded" in the public cache options, so use one byte as the finite
    // zero-remainder sentinel; it deterministically selects the smallest
    // non-lazy policy, whose bootstrap is then supplied by the persistent
    // cache (including the one-pattern all-active corner).
    auto const scoring_remainder =
        cache_selection_options.memory_budget_bytes - mandatory_pair_bytes;
    cache_selection_options.memory_budget_bytes =
        std::max<std::size_t>(1, scoring_remainder);
  }
  bool lazy_policy_pilot_ran_this_build = false;
  if (lazy_policy_rebuild_token != nullptr) {
    auto const& frozen =
        chart_spr_search_detail::chart_spr_lazy_policy_from_rebuild_token(
            *lazy_policy_rebuild_token);
    if (chart_spr_search_detail::requested_chart_spr_lazy_policy(cache) !=
            chart_spr_lazy_policy::automatic ||
        frozen.version != chart_spr_lazy_policy_diagnostics::current_version ||
        frozen.requested != chart_spr_lazy_policy::automatic ||
        frozen.resolved == chart_spr_lazy_policy::automatic || !frozen.frozen) {
      throw std::logic_error(
          "chart SPR lazy policy: invalid internal rebuild token");
    }
    state.lazy_policy = frozen;
    ++state.counters.lazy_policy_frozen_reuses;
  } else {
    state.lazy_policy = resolve_chart_spr_lazy_policy(
        state.execution_plan, state.active_patterns, options,
        policy_resolution_options, state.estimated_full_pattern_cache_bytes);
    state.counters.lazy_policy_pilot_runs +=
        state.lazy_policy.pilot_inside_chart_builds;
    lazy_policy_pilot_ran_this_build =
        state.lazy_policy.pilot_inside_chart_builds != 0;
  }
  state.lazy_policy.frozen = true;
  // The selector consumes an already-resolved representation. The original
  // request remains in state.cache_opts and is the only public policy input.
  cache_selection_options.use_lazy_multisite_chart = false;
  cache_selection_options.lazy_policy = state.lazy_policy.resolved;
  state.cache_opts = cache;
  state.effective_pattern_batch_size = choose_chart_spr_pattern_batch_size(
      state.grammar, state.active_patterns, cache_selection_options);
  state.cache_strategy = choose_chart_spr_cache_strategy(
      state.grammar, state.active_patterns, cache_selection_options);
  auto const defer_pattern_batch_bootstrap =
      state.cache_strategy != chart_spr_cache_strategy::lazy_multisite_chart &&
      build_policy.defer_pattern_batch_bootstrap_to_local_cache;
  // The one-pattern minimum belongs to the selected non-lazy publication
  // phase. Applying it before automatic resolution would incorrectly make a
  // forced/automatic lazy pilot pay for a dense representation it never owns.
  if (cache.memory_budget_bytes != 0 &&
      state.cache_strategy != chart_spr_cache_strategy::lazy_multisite_chart &&
      !defer_pattern_batch_bootstrap &&
      !state.active_patterns.patterns.patterns.empty()) {
    auto const minimum_scoring_bytes =
        estimate_chart_spr_pattern_entry_cache_bytes(state.grammar);
    if (cache_selection_options.memory_budget_bytes < minimum_scoring_bytes) {
      auto const required = chart_spr_exact_candidate_checked_bytes_add(
          state_build_live_base_bytes,
          chart_spr_exact_candidate_checked_bytes_add(
              reserved_local_commit_cache_bytes, minimum_scoring_bytes,
              "chart SPR minimum selected cache admission"),
          "chart SPR minimum published state admission");
      if (build_exact_trim) {
        throw chart_spr_exact_state_budget_error(required,
                                                 cache.memory_budget_bytes);
      }
      throw std::runtime_error(
          "chart SPR search state: live state core plus one scoring pattern "
          "requires " +
          std::to_string(required) + " bytes, exceeding configured budget " +
          std::to_string(cache.memory_budget_bytes));
    }
  }
  if (defer_pattern_batch_bootstrap && build_exact_trim) {
    throw std::runtime_error(
        "chart SPR search state: deferred local-cache bootstrap requires "
        "deferred exact initialization");
  }
  state.pattern_batch_bootstrap_deferred = defer_pattern_batch_bootstrap;
  auto const exact_owns_pattern_batch_bootstrap =
      state.cache_strategy == chart_spr_cache_strategy::pattern_batches &&
      build_exact_trim;

  // Selection deliberately rounds an undersized pattern budget up to one
  // entry so the zero-budget compatibility policy remains useful. Under an
  // explicit finite budget, however, that must not turn into an allocate-then-
  // reject path. Charge the selected scoring representation, unless the local
  // persistent cache owns that recurrence, plus the mandatory local-cache
  // surfaces before constructing any chart payload.
  auto projected_scoring_cache_bytes =
      defer_pattern_batch_bootstrap ? std::size_t{0}
      : state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart
          ? estimate_chart_spr_lazy_cache_admission_bytes(
                state.grammar.clades.size(),
                state.active_patterns.patterns.patterns.size())
          : estimate_chart_spr_pattern_batch_cache_bytes(
                state.grammar, state.effective_pattern_batch_size);
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      projected_scoring_cache_bytes < sizeof(lazy_multisite_chart)) {
    throw std::logic_error(
        "chart SPR lazy cache projection omitted its fixed object");
  }
  auto const projected_scoring_cache_dynamic_bytes =
      defer_pattern_batch_bootstrap ? std::size_t{0}
      : state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart
          ? projected_scoring_cache_bytes - sizeof(lazy_multisite_chart)
          : projected_scoring_cache_bytes;
  auto const projected_initial_resident_bytes =
      chart_spr_exact_candidate_checked_bytes_add(
          projected_scoring_cache_bytes, reserved_local_commit_cache_bytes,
          "chart SPR initial resident-cache admission");
  auto const projected_initial_dynamic_resident_bytes =
      chart_spr_exact_candidate_checked_bytes_add(
          projected_scoring_cache_dynamic_bytes,
          reserved_local_commit_cache_bytes,
          "chart SPR initial dynamic resident-cache admission");
  std::size_t lazy_state_build_retained_bytes = 0;
  auto projected_initial_live_bytes =
      chart_spr_exact_candidate_checked_bytes_add(
          state_build_live_base_bytes, projected_initial_dynamic_resident_bytes,
          "chart SPR initial published-state admission");
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      cache.memory_budget_bytes != 0) {
    lazy_state_build_retained_bytes =
        chart_spr_exact_candidate_checked_bytes_add(
            overlapping_published_state_bytes,
            estimate_chart_spr_lazy_state_build_retained_bytes(state),
            "chart SPR lazy state-build retained live state");
    projected_initial_live_bytes =
        estimate_chart_spr_lazy_state_build_required_bytes(
            state, projected_initial_dynamic_resident_bytes, scheduler,
            overlapping_published_state_bytes);
  }
  if (lazy_policy_pilot_ran_this_build) {
    auto const pilot_required =
        estimate_chart_spr_lazy_policy_pilot_required_bytes(
            state, state.lazy_policy.pilot_estimated_allocation_bytes,
            scheduler, overlapping_published_state_bytes);
    // The pilot chart is destroyed before representation publication.  The
    // finite envelope is the temporal maximum, never the sum of both phases.
    projected_initial_live_bytes =
        std::max(projected_initial_live_bytes, pilot_required);
  }
  if (cache.memory_budget_bytes != 0 &&
      projected_initial_live_bytes > cache.memory_budget_bytes) {
    if (build_exact_trim) {
      throw chart_spr_exact_state_budget_error(projected_initial_live_bytes,
                                               cache.memory_budget_bytes);
    }
    throw std::runtime_error(
        "chart SPR search state: estimated live chart state build requires " +
        std::to_string(projected_initial_live_bytes) +
        " bytes, exceeding configured budget " +
        std::to_string(cache.memory_budget_bytes));
  }
  state.resident_pattern_cache_bytes = projected_initial_resident_bytes;
  auto const publication_memory_budget_bytes =
      cache.memory_budget_bytes == 0
          ? std::size_t{0}
          : cache.memory_budget_bytes - overlapping_published_state_bytes;
  if (build_exact_trim) {
    if (scheduler != nullptr) {
      require_chart_spr_state_exact_memory_budget(
          state, trim_options, *scheduler, publication_memory_budget_bytes);
    } else {
      require_chart_spr_state_exact_memory_budget(
          state, trim_options, 1, publication_memory_budget_bytes);
    }
  }
  // Only a non-deferred scoring representation is resident at this point. The
  // reserved local-commit surfaces are constructed later and added from their
  // actual capacities; keeping the reservation out of the durable state
  // avoids counting them twice.
  state.resident_pattern_cache_bytes = projected_scoring_cache_bytes;
  state.effective_candidate_batch_size = cache.candidate_batch_size;
  ++state.counters.base_chart_cache_rebuilds;
  state.counters.skipped_invariant_sites =
      state.skipped_invariant_site_count;

  auto chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  std::uint64_t active_total = 0;
  auto const chart_construction_start = std::chrono::steady_clock::now();
  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    if (!defer_pattern_batch_bootstrap) {
      auto const& patterns = state.active_patterns.patterns.patterns;
      if (scheduler == nullptr) {
        state.pattern_charts.reserve(patterns.size());
        for (auto const& pattern : patterns) {
          auto entry = chart_spr_search_detail::build_pattern_chart_cache_entry(
              state.execution_plan, pattern, options, chart_build_options);
          ++state.counters.chart_execution_plan_cache_hits;
          active_total = chart_multisite_detail::checked_add_u64(
              active_total, entry.weighted_root_score,
              "chart-SPR cached active-pattern lower bound");
          state.counters.multifurcation_productions_scored +=
              entry.chart.multifurcation_productions_scored;
          state.pattern_charts.push_back(std::move(entry));
          ++state.counters.initial_state_inside_charts_built;
        }
      } else {
        state.pattern_charts.resize(patterns.size());
        std::vector<std::exception_ptr> errors(patterns.size());
        auto range_options = chart_spr_phase4_pattern_range_options(
            patterns.size(), scheduler->worker_resolution().resolved_workers);
        auto run = scheduler->for_each_indexed_range(
            patterns.size(), range_options,
            [&](chart_indexed_range const& range, std::size_t,
                chart_scheduler_cancellation_token const&) {
              for (std::size_t pattern_index = range.begin;
                   pattern_index < range.end; ++pattern_index) {
                try {
                  state.pattern_charts[pattern_index] =
                      chart_spr_search_detail::build_pattern_chart_cache_entry(
                          state.execution_plan, patterns[pattern_index],
                          options, chart_build_options);
                } catch (...) {
                  errors[pattern_index] = std::current_exception();
                  break;
                }
              }
            });
        record_chart_spr_scheduler_axis_run(
            state.counters.scheduler_axes.initial_chart_patterns, run);
        for (auto const& error : errors) {
          if (error) std::rethrow_exception(error);
        }
        for (auto const& entry : state.pattern_charts) {
          ++state.counters.chart_execution_plan_cache_hits;
          active_total = chart_multisite_detail::checked_add_u64(
              active_total, entry.weighted_root_score,
              "chart-SPR cached active-pattern lower bound");
          state.counters.multifurcation_productions_scored +=
              entry.chart.multifurcation_productions_scored;
          ++state.counters.initial_state_inside_charts_built;
        }
      }
      state.chart_construction_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - chart_construction_start)
              .count();
      state.resident_pattern_cache_bytes =
          estimate_chart_spr_pattern_cache_bytes(state);
    } else {
      state.resident_pattern_cache_bytes = 0;
    }
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::lazy_multisite_chart) {
    lazy_chart_options lazy_options;
    lazy_options.chart = chart_build_options;
    lazy_options.retain_all_inside_class_maps = true;
    std::vector<chart_scheduler_run_summary> lazy_inside_runs;
    std::vector<chart_scheduler_run_summary> lazy_outside_runs;
    lazy_chart_detail::plan_lazy_chart_scheduler_workspace
        lazy_scheduler_workspace;
    lazy_chart_detail::plan_lazy_chart_memory_options lazy_memory_options{
        .memory_budget_bytes = cache.memory_budget_bytes,
        .retained_resident_bytes = lazy_state_build_retained_bytes,
    };
    lazy_chart_detail::plan_lazy_chart_memory_report lazy_memory_report;
    chart_spr_scheduler_run_axis_publisher lazy_inside_axis_publisher{
        state.counters.scheduler_axes.lazy_inside_clades, lazy_inside_runs};
    chart_spr_scheduler_run_axis_publisher lazy_outside_axis_publisher{
        state.counters.scheduler_axes.lazy_outside_clades, lazy_outside_runs};
    std::optional<chart_scheduler> finite_w1_scheduler;
    auto* lazy_scheduler = scheduler;
    if (lazy_scheduler == nullptr && cache.memory_budget_bytes != 0) {
      finite_w1_scheduler.emplace(
          chart_scheduler_options{.requested_workers = 1,
                                  .default_minimum_grain = 1,
                                  .default_target_ranges_per_worker = 4});
      lazy_scheduler = &*finite_w1_scheduler;
    }
    if (lazy_scheduler == nullptr) {
      state.lazy_chart = build_lazy_inside_chart(
          state.execution_plan, state.active_patterns.patterns, lazy_options);
    } else {
      state.lazy_chart = build_lazy_inside_chart_scheduled(
          state.execution_plan, state.active_patterns.patterns, lazy_options,
          *lazy_scheduler, &lazy_inside_runs, nullptr,
          &lazy_scheduler_workspace, lazy_memory_options, &lazy_memory_report);
      lazy_scheduler_workspace.release_inside();
    }
    ++state.counters.chart_execution_plan_cache_hits;
    if (!options.score_ua_edge) {
      if (lazy_scheduler == nullptr) {
        build_lazy_outside_chart_in_place(state.execution_plan,
                                          state.active_patterns.patterns,
                                          *state.lazy_chart, options);
      } else {
        auto outside_memory_options = lazy_memory_options;
        if (outside_memory_options.memory_budget_bytes != 0) {
          outside_memory_options.retained_resident_bytes =
              chart_spr_exact_candidate_checked_bytes_add(
                  outside_memory_options.retained_resident_bytes,
                  chart_spr_exact_candidate_checked_bytes_multiply(
                      lazy_inside_runs.capacity(),
                      sizeof(chart_scheduler_run_summary),
                      "chart SPR retained lazy-inside run summaries"),
                  "chart SPR retained lazy-inside run summaries");
        }
        build_lazy_outside_chart_in_place_scheduled(
            state.execution_plan, state.active_patterns.patterns,
            *state.lazy_chart, options, *lazy_scheduler, &lazy_outside_runs,
            nullptr, &lazy_scheduler_workspace, outside_memory_options,
            &lazy_memory_report);
        lazy_scheduler_workspace.release_outside();
      }
      ++state.counters.chart_execution_plan_cache_hits;
    }
    state.chart_construction_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - chart_construction_start)
            .count();
    active_total = lazy_composite_lower_bound(
        state.execution_plan, state.active_patterns.patterns, *state.lazy_chart,
        options);
    ++state.counters.chart_execution_plan_cache_hits;
    state.counters.multifurcation_productions_scored +=
        state.lazy_chart->multifurcation_productions_scored +
        state.lazy_chart->outside_multifurcation_productions_scored;
    add_lazy_chart_build_counters(state.counters, *state.lazy_chart);
    add_lazy_chart_memory_report(state.counters, lazy_memory_report);
    state.resident_pattern_cache_bytes =
        estimate_chart_spr_pattern_cache_bytes(state);
  } else {
    auto const& patterns = state.active_patterns.patterns.patterns;
    auto batch_size = std::max<std::size_t>(
        1, state.effective_pattern_batch_size);
    // Exact conservative initialization already has to build one cold inside
    // chart per active pattern.  Let that setup own the initial composite too,
    // rather than first constructing and discarding an identical batch.  A
    // deferred local state similarly leaves this recurrence to its persistent
    // inside cache.
    if (!exact_owns_pattern_batch_bootstrap && !defer_pattern_batch_bootstrap) {
      if (scheduler == nullptr) {
        for (std::size_t begin = 0; begin < patterns.size();
             begin += batch_size) {
          ++state.counters.pattern_batch_cache_builds;
          auto end = std::min(patterns.size(), begin + batch_size);
          for (std::size_t i = begin; i < end; ++i) {
            auto entry =
                chart_spr_search_detail::build_pattern_chart_cache_entry(
                    state.execution_plan, patterns[i], options,
                    chart_build_options);
            ++state.counters.chart_execution_plan_cache_hits;
            active_total = chart_multisite_detail::checked_add_u64(
                active_total, entry.weighted_root_score,
                "chart-SPR batched active-pattern lower bound");
            state.counters.multifurcation_productions_scored +=
                entry.chart.multifurcation_productions_scored;
            ++state.counters.initial_state_inside_charts_built;
          }
        }
      } else {
        // Pattern-batch mode is an RSS contract, not only a scoring policy.
        // Construct and release exactly one configured batch at a time; the
        // coordinator folds its pre-sized slots in increasing pattern order.
        for (std::size_t begin = 0; begin < patterns.size();
             begin += batch_size) {
          auto const count = std::min(batch_size, patterns.size() - begin);
          auto const range_options = chart_spr_phase4_pattern_range_options(
              count, scheduler->worker_resolution().resolved_workers);
          std::vector<pattern_chart_cache_entry> pattern_slots(count);
          std::vector<std::exception_ptr> pattern_errors(count);
          auto run = scheduler->for_each_indexed_range(
              count, range_options,
              [&](chart_indexed_range const& range, std::size_t,
                  chart_scheduler_cancellation_token const&) {
                for (std::size_t local = range.begin; local < range.end;
                     ++local) {
                  try {
                    pattern_slots[local] = chart_spr_search_detail::
                        build_pattern_chart_cache_entry(
                            state.execution_plan, patterns[begin + local],
                            options, chart_build_options);
                  } catch (...) {
                    pattern_errors[local] = std::current_exception();
                  }
                }
              });
          record_chart_spr_scheduler_axis_run(
              state.counters.scheduler_axes.initial_chart_patterns, run);
          for (std::size_t local = 0; local < count; ++local) {
            if (pattern_errors[local]) {
              std::rethrow_exception(pattern_errors[local]);
            }
          }
          ++state.counters.pattern_batch_cache_builds;
          for (std::size_t local = 0; local < count; ++local) {
            auto const& entry = pattern_slots[local];
            ++state.counters.chart_execution_plan_cache_hits;
            active_total = chart_multisite_detail::checked_add_u64(
                active_total, entry.weighted_root_score,
                "chart-SPR batched active-pattern lower bound");
            state.counters.multifurcation_productions_scored +=
                entry.chart.multifurcation_productions_scored;
            ++state.counters.initial_state_inside_charts_built;
          }
        }
      }
    }
    if (!exact_owns_pattern_batch_bootstrap && !defer_pattern_batch_bootstrap) {
      state.chart_construction_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - chart_construction_start)
              .count();
    }
    state.resident_pattern_cache_bytes =
        defer_pattern_batch_bootstrap
            ? std::size_t{0}
            : estimate_chart_spr_pattern_batch_cache_bytes(
                  state.grammar, state.effective_pattern_batch_size);
  }

  // Capacity-based post-build accounting is the backstop for the conservative
  // preflight above.  It must still fail before exact construction if this
  // frozen allocator/toolchain retained more storage than the projection.
  auto const actual_initial_resident_bytes =
      chart_spr_exact_candidate_checked_bytes_add(
          overlapping_published_state_bytes,
          chart_spr_exact_candidate_checked_bytes_add(
              estimate_chart_spr_published_state_resident_bytes(state),
              reserved_local_commit_cache_bytes,
              "chart SPR initial resident-cache capacity check"),
          "chart SPR overlapping resident-state capacity check");
  if (cache.memory_budget_bytes != 0 &&
      actual_initial_resident_bytes > cache.memory_budget_bytes) {
    if (build_exact_trim) {
      throw chart_spr_exact_state_budget_error(
          actual_initial_resident_bytes, cache.memory_budget_bytes);
    }
    throw std::runtime_error(
        "chart SPR search state: resident chart-cache capacity requires " +
        std::to_string(actual_initial_resident_bytes) +
        " bytes, exceeding configured budget " +
        std::to_string(cache.memory_budget_bytes));
  }

  if (build_exact_trim) {
    auto const exact_initialization_start =
        std::chrono::steady_clock::now();
    if (scheduler != nullptr) {
      require_chart_spr_state_exact_memory_budget(
          state, trim_options, *scheduler, publication_memory_budget_bytes);
    } else {
      require_chart_spr_state_exact_memory_budget(
          state, trim_options, 1, publication_memory_budget_bytes);
    }
    if (scheduler != nullptr) {
      auto checked =
          check_chart_execution_plan(state.grammar, state.execution_plan);
      auto built_trim = build_chart_spr_state_exact_trim(
          state, checked, *scheduler, trim_options);
      require_chart_spr_retained_exact_state_memory_budget(
          state, built_trim, *scheduler, publication_memory_budget_bytes);
      state.exact_trim_active_only = std::move(built_trim);
    } else {
      auto built_trim = build_chart_spr_state_exact_trim(state, trim_options);
      require_chart_spr_retained_exact_state_memory_budget(
          state, built_trim, publication_memory_budget_bytes);
      state.exact_trim_active_only = std::move(built_trim);
    }
    ++state.counters.chart_execution_plan_cache_hits;
    if (exact_owns_pattern_batch_bootstrap) {
      if (state.exact_trim_active_only->invariant_constant_offset != 0) {
        throw std::runtime_error(
            "chart SPR search state: active-only exact setup unexpectedly "
            "carried an invariant offset");
      }
      active_total = state.exact_trim_active_only->composite_lower_bound;
    }
    state.exact_initialization_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - exact_initialization_start)
            .count();
  }
  if (!state.pattern_batch_bootstrap_deferred) {
    state.composite_lower_bound_without_invariants = active_total;
    state.composite_lower_bound_with_invariants =
        chart_multisite_detail::checked_add_u64(
            active_total, state.invariant_constant_offset,
            "chart-SPR cached lower bound invariant offset");
  }
  return state;
}

inline chart_spr_search_state build_chart_spr_search_state(
    phylo_dag& dag, clade_grammar grammar, site_pattern_set patterns,
    chart_options options = {}) {
  auto active_build = make_active_search_patterns(patterns, options);
  active_build.pattern_source_fingerprint =
      build_chart_spr_pattern_source_fingerprint(dag, grammar);
  return build_chart_spr_search_state_from_active(
      dag, std::move(grammar), std::move(active_build), options);
}

inline chart_spr_search_state build_chart_spr_search_state(
    phylo_dag& dag, clade_grammar grammar, chart_options options = {}) {
  auto active_build = make_active_search_patterns(dag, grammar, options);
  auto state = build_chart_spr_search_state_from_active(
      dag, std::move(grammar), std::move(active_build), options);
  ++state.counters.pattern_rebuilds;
  return state;
}

inline chart_spr_search_state build_chart_spr_search_state(
    phylo_dag& dag, clade_grammar grammar,
    chart_spr_search_options const& options) {
  validate_supported_chart_cache_options(options.cache);
  auto active_build = make_active_search_patterns(dag, grammar, options.chart);
  bool build_exact =
      options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite;
  auto state = build_chart_spr_search_state_from_active(
      dag, std::move(grammar), std::move(active_build), options.chart,
      build_exact, options.exact_trim, options.cache);
  ++state.counters.pattern_rebuilds;
  return state;
}

namespace chart_spr_search_detail {

// Narrow test surface for the production accepted-state rebuild path.  It
// keeps the frozen token non-constructible while allowing strict finite-budget
// tests to observe its pilot-skip and old/new publication-overlap contract.
chart_spr_search_state rebuild_chart_spr_search_state_after_accept_for_tests(
    chart_spr_search_state const& previous_state, phylo_dag& rebuilt_dag,
    clade_grammar rebuilt_grammar, chart_spr_search_options const& options,
    chart_scheduler& scheduler);

}  // namespace chart_spr_search_detail

inline constexpr std::size_t chart_spr_overlay_row_npos =
    std::numeric_limits<std::size_t>::max();

struct overlay_reachability_stats {
  std::size_t reachable_clades = 0;
  std::size_t reachable_productions = 0;
  std::size_t reachable_temp_clades = 0;
  std::size_t reachable_temp_productions = 0;
  bool full_grammar_like = false;

  bool operator==(overlay_reachability_stats const&) const = default;
};

struct candidate_chart_execution_plan_build_stats {
  std::size_t candidate_partition_validations = 0;
  std::size_t clade_order_sorts = 0;
  std::size_t production_descriptors_compiled = 0;

  bool operator==(
      candidate_chart_execution_plan_build_stats const&) const = default;
};

struct candidate_chart_production_descriptor {
  overlay_production_ref source;
  std::size_t child_begin = 0;
  std::size_t child_count = 0;

  [[nodiscard]] bool is_binary() const noexcept { return child_count == 2; }

  bool operator==(
      candidate_chart_production_descriptor const&) const = default;
};

struct candidate_chart_child_descriptor {
  overlay_clade_ref clade;
  std::size_t local_row_slot = chart_spr_overlay_row_npos;
  clade_id base_clade = no_clade;
  taxon_id leaf_taxon = chart_plan_no_taxon;

  [[nodiscard]] bool has_local_row() const noexcept {
    return local_row_slot != chart_spr_overlay_row_npos;
  }

  bool operator==(candidate_chart_child_descriptor const&) const = default;
};

struct candidate_chart_row_descriptor {
  overlay_clade_ref clade;
  taxon_id leaf_taxon = chart_plan_no_taxon;
  std::size_t production_begin = 0;
  std::size_t production_count = 0;

  [[nodiscard]] bool is_leaf() const noexcept {
    return leaf_taxon != chart_plan_no_taxon;
  }

  bool operator==(candidate_chart_row_descriptor const&) const = default;
};

struct spr_overlay_delta {
  clade_grammar const* base = nullptr;

  // Candidate seeds are copied because prepared candidates can be moved or
  // retained after the caller-owned grammar_spr_candidate has gone out of
  // scope.  The old implementation borrowed a candidate pointer solely for
  // these two affected-closure seeds.
  overlay_clade_ref candidate_old_parent;
  overlay_clade_ref candidate_new_sibling_or_target;

  std::vector<clade_key> temp_clades;
  std::vector<overlay_grammar_production> temp_productions;
  std::vector<production_id> removed_base_productions;

  // Phase 10 (cross-cutting identity surface).  Optional commit-source label
  // that names which commit path produced this delta.  Empty (the default)
  // means an SPR overlay delta produced by `build_spr_overlay_delta` (the
  // Phase-4 local-commit path / Option A/B materialize-and-merge);
  // `"option_c_chain_commit"` means a rank-3 Option-C rewrite committed via
  // `option_c_as_overlay_delta` (Phase 6/7).  This is the label the Phase-10
  // JSON identity report carries per chain entry so Option-C rewrite
  // identities survive materialize -> rebuild -> report round trips; it is
  // not used by any chart/cache logic and carries no behavioral contract.
  std::string commit_source;

  // Affected base clades plus temp clades, sorted bottom-up.
  std::vector<overlay_clade_ref> affected_order;
  std::vector<bool> affected_base_clade;
  std::vector<bool> affected_temp_clade;
  std::vector<std::size_t> affected_base_row_slot;
  std::vector<std::size_t> affected_temp_row_slot;

  overlay_clade_ref root;
  overlay_reachability_stats reachability_stats;

  // Identity of the immutable base plan used to compile the candidate-local
  // descriptor.  Legacy callers that do not supply a plan leave generation at
  // zero; production search always supplies its resident plan.
  std::uint64_t base_plan_generation = 0;
  chart_plan_fingerprint base_plan_fingerprint;

  // Flat, self-contained recurrence descriptors compiled once per candidate.
  // Rows are aligned with affected_order.  Base productions precede temp
  // productions exactly as in the legacy recurrence, and both production and
  // child order are preserved.
  candidate_chart_execution_plan_build_stats candidate_plan_build_stats;
  std::vector<candidate_chart_row_descriptor> compiled_rows;
  std::vector<candidate_chart_production_descriptor> compiled_productions;
  std::vector<candidate_chart_child_descriptor> compiled_children;

  // Candidate-local indices built once and reused across all active patterns.
  std::vector<bool> removed_base_production;
  std::vector<bool> reachable_base_clade;
  std::vector<bool> reachable_temp_clade;
  std::vector<std::vector<production_id>> temp_productions_by_base_parent;
  std::vector<std::vector<production_id>> temp_productions_by_temp_parent;
  std::vector<std::vector<production_id>> temp_productions_by_base_child;
  std::vector<std::vector<production_id>> temp_productions_by_temp_child;
};

// Caller-owned high-water storage for rebuilding an overlay descriptor in
// place.  Traversal buffers are borrowed only while an `_into` build is
// active.  The spare-object pools retain nested vector capacities that would
// otherwise be discarded when a later candidate has fewer temporary clades,
// productions, witnesses, or index rows.
struct spr_overlay_delta_build_scratch {
  std::vector<overlay_clade_ref> reachability_stack;
  std::vector<overlay_clade_ref> affected_queue;

  std::vector<clade_key> spare_temp_clades;
  std::vector<overlay_grammar_production> spare_temp_productions;
  std::vector<production_witness> spare_production_witnesses;
  std::vector<production_child_witness> spare_child_witnesses;

  std::vector<std::vector<production_id>> spare_temp_productions_by_base_parent;
  std::vector<std::vector<production_id>> spare_temp_productions_by_temp_parent;
  std::vector<std::vector<production_id>> spare_temp_productions_by_base_child;
  std::vector<std::vector<production_id>> spare_temp_productions_by_temp_child;

  void clear_operation_borrows() noexcept {
    reachability_stack.clear();
    affected_queue.clear();
  }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return reachability_stack.empty() && affected_queue.empty();
  }
};

struct local_overlay_chart_rows {
  static constexpr std::size_t npos = chart_spr_overlay_row_npos;

  // Rows only for affected base clades and reachable temp clades.  Slot maps
  // are candidate-delta owned, not rebuilt for every active pattern.
  std::vector<std::array<chart_cost, nuc_state_count>> rows;
  std::span<std::size_t const> base_row_slot;
  std::span<std::size_t const> temp_row_slot;

  [[nodiscard]] std::size_t slot_for(overlay_clade_ref ref) const {
    if (ref.space == overlay_id_space::base) {
      if (base_row_slot.empty()) {
        throw std::runtime_error(
            "chart SPR overlay-delta row: missing base row slot map");
      }
      auto const slots = base_row_slot;
      if (ref.id == no_clade || ref.id >= slots.size()) {
        throw std::runtime_error(
            "chart SPR overlay-delta row: base clade ref out of range");
      }
      return slots[ref.id];
    }
    if (temp_row_slot.empty()) {
      throw std::runtime_error(
          "chart SPR overlay-delta row: missing temp row slot map");
    }
    auto const slots = temp_row_slot;
    if (ref.id == no_clade || ref.id >= slots.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta row: temp clade ref out of range");
    }
    return slots[ref.id];
  }

  [[nodiscard]] bool has_local_row(overlay_clade_ref ref) const {
    return slot_for(ref) != npos;
  }
};

struct local_score_worker_barrier_for_tests {
  std::atomic<std::size_t> started = 0;
  std::atomic<bool> release = false;
};

struct local_spr_score_options {
  bool verify_against_full_overlay = false;  // tests/debug only
  bool exact_multisite = false;              // false = composite/lower bound
  bool validate_cached_chart_shapes = false; // tests/debug only

  // Deterministic test-only injection for the parallel operation-boundary
  // contract.  When set, throw immediately before submitting the task with
  // this zero-based successful-submission index.  Production callers leave it
  // disengaged.
  std::optional<std::size_t> force_worker_submit_failure_after_for_tests;

  // Parallel operation-boundary test hooks.  The barrier makes every queued
  // task announce itself and wait until the coordinator has submitted (or
  // failed to submit) the complete deterministic task set.  The optional
  // worker index then throws from inside that task.  Production callers leave
  // both fields disengaged/null.
  local_score_worker_barrier_for_tests* worker_barrier_for_tests = nullptr;
  std::optional<std::size_t> force_worker_failure_for_tests;
  // Lazy-wave integrity hooks stage fixed failure codes inside one/all task
  // slots; the coordinator must choose the lowest failed slot after joining.
  std::optional<std::size_t> force_lazy_worker_invariant_failure_for_tests;
  bool force_all_lazy_worker_invariant_failures_for_tests = false;
  // Coordinator finalization hook used to prove no result is published before
  // every admitted slot has completed throw-capable score finalization.
  std::optional<std::size_t> force_lazy_finish_overflow_for_tests;
  std::optional<std::size_t> force_lazy_finish_allocation_for_tests;

  // Optional tighter unified budget for local-task admission. Zero inherits
  // the search state's cache budget (and is unlimited when that is also zero).
  // The additional resident value lets an orchestration owner charge storage
  // that remains live across this call without making the local workspace own
  // or inspect it. A checked finite call with a supplied scheduler charges
  // that scheduler's persistent ownership and each actual range operation
  // internally; callers must not include either in this external value.
  std::size_t admission_memory_budget_bytes = 0;
  std::size_t admission_additional_resident_bytes = 0;
  // Production acceptance already charges the owning result-vector outer
  // capacity. The checked direct `_into` boundary otherwise charges the
  // caller span's logical object storage itself. Both paths always charge and
  // pre-reserve each invalid-reason string below.
  bool admission_additional_resident_includes_result_objects = false;
  // The checked direct `_into` boundary also charges each visible candidate
  // object and its nested owned capacities. Set this only when
  // admission_additional_resident_bytes already includes that same residence,
  // as the production acceptance workspace does. Surplus capacity outside the
  // visible span must always be supplied through the additional-resident
  // value because a span cannot inspect its owner.
  bool admission_additional_resident_includes_candidate_inputs = false;

  // The finite acceptance-iteration owner supplies the allocation-free
  // frozen-toolchain bounds for dense scorer temporaries.  The scorer checks
  // the actual vector capacities after allocation and before the first
  // scheduler submission.  Zero keeps the historical unchecked direct-call
  // contract; it never weakens the finite iteration envelope, which always
  // supplies both applicable bounds.
  std::size_t admission_weighted_candidate_order_capacity_bytes = 0;
  std::size_t admission_pattern_batch_construction_scratch_bytes = 0;

  // Non-owning test seam for exercising diagnostics produced after dense
  // preparation.  Production callers leave it empty.  The view must outlive
  // the synchronous local-score operation.
  std::string_view force_dense_invalid_reason_for_tests = {};

  // Mark reachability validation as full-grammar-like when visited base
  // clades or productions exceed this fraction of the base grammar.  0
  // disables the flag.
  double full_grammar_like_reachability_fraction = 0.50;
};

namespace chart_spr_search_detail {

inline bool use_finite_local_score_diagnostics(
    chart_spr_search_state const& state,
    local_spr_score_options const& options) noexcept {
  return options.admission_memory_budget_bytes != 0 ||
         state.cache_opts.memory_budget_bytes != 0;
}

inline void reserve_finite_local_score_invalid_reason(std::string& reason) {
  reason.clear();
  reason.reserve(lazy_local_invalid_reason_max_size);
  if (local_owned_dynamic_capacity_bytes(reason) >
      lazy_local_invalid_reason_owned_capacity_bound()) {
    throw std::logic_error(
        "chart SPR finite local score: diagnostic string capacity exceeded "
        "the unified admission envelope");
  }
}

inline std::size_t local_overlay_delta_dynamic_capacity_bytes(
    spr_overlay_delta const& delta) {
  return local_owned_dynamic_capacity_sum(
      delta.temp_clades, delta.temp_productions, delta.removed_base_productions,
      delta.commit_source, delta.affected_order, delta.affected_base_clade,
      delta.affected_temp_clade, delta.affected_base_row_slot,
      delta.affected_temp_row_slot, delta.compiled_rows,
      delta.compiled_productions, delta.compiled_children,
      delta.removed_base_production, delta.reachable_base_clade,
      delta.reachable_temp_clade, delta.temp_productions_by_base_parent,
      delta.temp_productions_by_temp_parent,
      delta.temp_productions_by_base_child,
      delta.temp_productions_by_temp_child);
}

inline std::size_t local_overlay_build_scratch_dynamic_capacity_bytes(
    spr_overlay_delta_build_scratch const& scratch) {
  return local_owned_dynamic_capacity_sum(
      scratch.reachability_stack, scratch.affected_queue,
      scratch.spare_temp_clades, scratch.spare_temp_productions,
      scratch.spare_production_witnesses, scratch.spare_child_witnesses,
      scratch.spare_temp_productions_by_base_parent,
      scratch.spare_temp_productions_by_temp_parent,
      scratch.spare_temp_productions_by_base_child,
      scratch.spare_temp_productions_by_temp_child);
}

inline clade_key const& overlay_delta_clade_key(spr_overlay_delta const& delta,
                                                overlay_clade_ref ref) {
  if (delta.base == nullptr) {
    throw std::runtime_error("chart SPR overlay-delta: missing base grammar");
  }
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= delta.base->clades.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: base clade ref out of range");
    }
    return delta.base->clades[ref.id];
  }
  if (ref.id == no_clade || ref.id >= delta.temp_clades.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: temp clade ref out of range");
  }
  return delta.temp_clades[ref.id];
}

inline std::size_t overlay_delta_clade_size(spr_overlay_delta const& delta,
                                            overlay_clade_ref ref) {
  return overlay_delta_clade_key(delta, ref).taxa.size();
}

inline bool overlay_delta_base_production_removed(
    spr_overlay_delta const& delta, production_id pid) {
  if (pid == no_production || pid >= delta.removed_base_production.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: base production id out of range");
  }
  return delta.removed_base_production[pid];
}

inline bool overlay_delta_ref_is_reachable(spr_overlay_delta const& delta,
                                           overlay_clade_ref ref) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= delta.reachable_base_clade.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: base reachability ref out of range");
    }
    return delta.reachable_base_clade[ref.id];
  }
  if (ref.id == no_clade || ref.id >= delta.reachable_temp_clade.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: temp reachability ref out of range");
  }
  return delta.reachable_temp_clade[ref.id];
}

inline std::array<chart_cost, nuc_state_count> overlay_delta_leaf_row(
    taxon_id taxon, leaf_site_states_view leaf_states) {
  if (taxon == chart_plan_no_taxon) {
    throw std::runtime_error(
        "chart SPR overlay-delta: missing compiled leaf taxon");
  }
  if (taxon >= leaf_states.state_by_taxon.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: leaf taxon out of state range");
  }
  auto observed = leaf_states.state_by_taxon[taxon];
  parsimony_chart_detail::validate_state(observed,
                                         "overlay-delta leaf state");
  auto row = parsimony_chart_detail::make_inf_row();
  row[observed] = 0;
  return row;
}

inline std::array<chart_cost, nuc_state_count> overlay_delta_leaf_row(
    taxon_id taxon, leaf_site_states const& leaf_states) {
  return overlay_delta_leaf_row(taxon, view_leaf_site_states(leaf_states));
}

template <class T>
inline void resize_reusing_nested_storage(std::vector<T>& values,
                                          std::size_t size,
                                          std::vector<T>& spares) {
  while (values.size() > size) {
    spares.push_back(std::move(values.back()));
    values.pop_back();
  }
  while (values.size() < size) {
    if (spares.empty()) {
      values.emplace_back();
    } else {
      values.push_back(std::move(spares.back()));
      spares.pop_back();
    }
  }
}

inline void copy_production_child_witness_reusing_storage(
    production_child_witness& destination,
    production_child_witness const& source) {
  destination.child = source.child;
  destination.edge_alternatives.assign(source.edge_alternatives.begin(),
                                       source.edge_alternatives.end());
}

inline void copy_production_witness_reusing_storage(
    production_witness& destination, production_witness const& source,
    spr_overlay_delta_build_scratch& scratch) {
  destination.parent_node = source.parent_node;
  resize_reusing_nested_storage(destination.children, source.children.size(),
                                scratch.spare_child_witnesses);
  for (std::size_t i = 0; i < source.children.size(); ++i) {
    copy_production_child_witness_reusing_storage(destination.children[i],
                                                  source.children[i]);
  }
}

inline void copy_overlay_production_reusing_storage(
    overlay_grammar_production& destination,
    overlay_grammar_production const& source,
    spr_overlay_delta_build_scratch& scratch) {
  destination.parent = source.parent;
  destination.children.assign(source.children.begin(), source.children.end());
  resize_reusing_nested_storage(destination.witnesses, source.witnesses.size(),
                                scratch.spare_production_witnesses);
  for (std::size_t i = 0; i < source.witnesses.size(); ++i) {
    copy_production_witness_reusing_storage(destination.witnesses[i],
                                            source.witnesses[i], scratch);
  }
  destination.multiplicity = source.multiplicity;
}

inline void copy_candidate_payload_reusing_storage(
    spr_overlay_delta& delta, grammar_spr_candidate const& candidate,
    spr_overlay_delta_build_scratch& scratch) {
  resize_reusing_nested_storage(delta.temp_clades,
                                candidate.added_clades.size(),
                                scratch.spare_temp_clades);
  for (std::size_t i = 0; i < candidate.added_clades.size(); ++i) {
    delta.temp_clades[i].taxa.assign(candidate.added_clades[i].taxa.begin(),
                                     candidate.added_clades[i].taxa.end());
  }

  resize_reusing_nested_storage(delta.temp_productions,
                                candidate.added_productions.size(),
                                scratch.spare_temp_productions);
  for (std::size_t i = 0; i < candidate.added_productions.size(); ++i) {
    copy_overlay_production_reusing_storage(
        delta.temp_productions[i], candidate.added_productions[i], scratch);
  }
}

inline void reset_production_index_reusing_storage(
    std::vector<std::vector<production_id>>& index, std::size_t size,
    std::vector<std::vector<production_id>>& spares) {
  resize_reusing_nested_storage(index, size, spares);
  for (auto& row : index) row.clear();
}

inline void validate_overlay_delta_temp_clade_key(clade_key const& key,
                                                  std::size_t taxon_count,
                                                  std::size_t index) {
  auto const invalid =
      key.taxa.empty() || !std::is_sorted(key.taxa.begin(), key.taxa.end()) ||
      std::adjacent_find(key.taxa.begin(), key.taxa.end()) != key.taxa.end() ||
      std::any_of(key.taxa.begin(), key.taxa.end(),
                  [&](taxon_id taxon) { return taxon >= taxon_count; });
  if (!invalid) return;

  // Formatting the indexed diagnostic is deliberately confined to the error
  // path.  Valid candidate preparation is allocation-sensitive.
  chart_spr_detail::validate_clade_key(
      key, taxon_count, "overlay-delta temp clade " + std::to_string(index));
}

inline bool sorted_taxon_ranges_intersect(std::vector<taxon_id> const& lhs,
                                          std::vector<taxon_id> const& rhs) {
  auto left = lhs.begin();
  auto right = rhs.begin();
  while (left != lhs.end() && right != rhs.end()) {
    if (*left < *right) {
      ++left;
    } else if (*right < *left) {
      ++right;
    } else {
      return true;
    }
  }
  return false;
}

inline void validate_overlay_delta_production_partition(
    spr_overlay_delta const& delta, overlay_grammar_production const& prod,
    production_id pid) {
  parsimony_chart_detail::record_production_partition_validation();
  if (prod.children.size() < 2) {
    throw std::runtime_error(
        "chart SPR overlay-delta: temp production " +
        std::to_string(pid) + " has arity " +
        std::to_string(prod.children.size()) +
        "; local scoring requires at least 2 children");
  }

  auto const& parent_taxa = overlay_delta_clade_key(delta, prod.parent).taxa;
  std::size_t covered_size = 0;
  for (std::size_t child_index = 0; child_index < prod.children.size();
       ++child_index) {
    auto child = prod.children[child_index];
    auto const& child_taxa = overlay_delta_clade_key(delta, child).taxa;
    if (child_taxa.size() >= parent_taxa.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: temp production child is not smaller "
          "than parent");
    }
    if (!std::includes(parent_taxa.begin(), parent_taxa.end(),
                       child_taxa.begin(), child_taxa.end())) {
      throw std::runtime_error(
          "chart SPR overlay-delta: temp production child is not a subset "
          "of parent");
    }

    for (std::size_t previous = 0; previous < child_index; ++previous) {
      auto const& previous_taxa =
          overlay_delta_clade_key(delta, prod.children[previous]).taxa;
      if (sorted_taxon_ranges_intersect(previous_taxa, child_taxa)) {
        throw std::runtime_error(
            "chart SPR overlay-delta: temp production children overlap");
      }
    }
    covered_size += child_taxa.size();
  }
  if (covered_size != parent_taxa.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: temp production children do not union to "
        "the parent clade");
  }
}

inline void append_temp_production_index(
    std::vector<std::vector<production_id>>& base_index,
    std::vector<std::vector<production_id>>& temp_index, overlay_clade_ref ref,
    production_id pid) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= base_index.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: base ref out of temp index range");
    }
    base_index[ref.id].push_back(pid);
  } else {
    if (ref.id == no_clade || ref.id >= temp_index.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: temp ref out of temp index range");
    }
    temp_index[ref.id].push_back(pid);
  }
}

inline std::vector<production_id> const& temp_productions_for_parent(
    spr_overlay_delta const& delta, overlay_clade_ref parent) {
  if (parent.space == overlay_id_space::base) {
    if (parent.id == no_clade ||
        parent.id >= delta.temp_productions_by_base_parent.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: base parent out of temp index range");
    }
    return delta.temp_productions_by_base_parent[parent.id];
  }
  if (parent.id == no_clade ||
      parent.id >= delta.temp_productions_by_temp_parent.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: temp parent out of temp index range");
  }
  return delta.temp_productions_by_temp_parent[parent.id];
}

inline std::size_t available_production_count_for_parent(
    spr_overlay_delta const& delta, overlay_clade_ref parent) {
  auto const& base = *delta.base;
  std::size_t count = 0;
  if (parent.space == overlay_id_space::base) {
    if (parent.id == no_clade || parent.id >= base.productions_by_parent.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: base parent out of range");
    }
    for (auto pid : base.productions_by_parent[parent.id]) {
      if (!overlay_delta_base_production_removed(delta, pid)) ++count;
    }
  }
  count += temp_productions_for_parent(delta, parent).size();
  return count;
}

inline void validate_reachable_base_production(
    spr_overlay_delta const& delta, production_id pid) {
  auto const& base = *delta.base;
  if (pid == no_production || pid >= base.productions.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: reachable base production out of range");
  }
  auto const& prod = base.productions[pid];
  parsimony_chart_detail::validate_production_inside_row_inputs(
      base, prod, pid, "chart SPR overlay-delta");
}

inline void mark_overlay_delta_affected(
    spr_overlay_delta const& delta, std::vector<bool>& affected_base,
    std::vector<bool>& affected_temp, std::vector<overlay_clade_ref>& queue,
    overlay_clade_ref ref) {
  (void)overlay_delta_clade_key(delta, ref);
  if (ref.space == overlay_id_space::base) {
    if (!affected_base[ref.id]) {
      affected_base[ref.id] = true;
      queue.push_back(ref);
    }
  } else {
    if (!affected_temp[ref.id]) {
      affected_temp[ref.id] = true;
      queue.push_back(ref);
    }
  }
}

inline bool overlay_delta_ref_present(overlay_clade_ref ref) {
  return ref.id != no_clade;
}

inline void mark_overlay_delta_affected_if_present(
    spr_overlay_delta const& delta, std::vector<bool>& affected_base,
    std::vector<bool>& affected_temp, std::vector<overlay_clade_ref>& queue,
    overlay_clade_ref ref) {
  if (!overlay_delta_ref_present(ref)) return;
  mark_overlay_delta_affected(delta, affected_base, affected_temp, queue, ref);
}

inline void validate_reachable_overlay_clade(
    spr_overlay_delta const& delta, overlay_clade_ref ref) {
  auto size = overlay_delta_clade_size(delta, ref);
  auto available = available_production_count_for_parent(delta, ref);
  if (size == 1) {
    if (available != 0) {
      throw std::runtime_error(
          "chart SPR overlay-delta: reachable singleton clade has "
          "available productions");
    }
    return;
  }
  if (available == 0) {
    throw std::runtime_error(
        "chart SPR overlay-delta: reachable non-singleton clade has no "
        "available productions");
  }
}

inline void build_overlay_delta_temp_indices(
    spr_overlay_delta& delta, spr_overlay_delta_build_scratch& scratch,
    candidate_chart_execution_plan_build_stats* external_stats = nullptr) {
  auto const& base = *delta.base;
  reset_production_index_reusing_storage(
      delta.temp_productions_by_base_parent, base.clades.size(),
      scratch.spare_temp_productions_by_base_parent);
  reset_production_index_reusing_storage(
      delta.temp_productions_by_temp_parent, delta.temp_clades.size(),
      scratch.spare_temp_productions_by_temp_parent);
  reset_production_index_reusing_storage(
      delta.temp_productions_by_base_child, base.clades.size(),
      scratch.spare_temp_productions_by_base_child);
  reset_production_index_reusing_storage(
      delta.temp_productions_by_temp_child, delta.temp_clades.size(),
      scratch.spare_temp_productions_by_temp_child);

  for (std::size_t i = 0; i < delta.temp_productions.size(); ++i) {
    auto pid = static_cast<production_id>(i);
    auto const& prod = delta.temp_productions[i];
    ++delta.candidate_plan_build_stats.candidate_partition_validations;
    if (external_stats != nullptr) {
      ++external_stats->candidate_partition_validations;
    }
    validate_overlay_delta_production_partition(delta, prod, pid);
    append_temp_production_index(delta.temp_productions_by_base_parent,
                                 delta.temp_productions_by_temp_parent,
                                 prod.parent, pid);

    // Partition validation above proves that valid child clades are pairwise
    // disjoint, hence duplicate child refs are impossible.  Index the
    // immutable payload directly instead of copying/sorting it.
    for (auto child : prod.children) {
      append_temp_production_index(delta.temp_productions_by_base_child,
                                   delta.temp_productions_by_temp_child,
                                   child, pid);
    }
  }
}

inline void compute_overlay_delta_reachability(
    spr_overlay_delta& delta, spr_overlay_delta_build_scratch& scratch,
    local_spr_score_options const& options,
    bool base_productions_already_validated = false) {
  auto const& base = *delta.base;
  delta.reachable_base_clade.assign(base.clades.size(), false);
  delta.reachable_temp_clade.assign(delta.temp_clades.size(), false);
  delta.reachability_stats = overlay_reachability_stats{};

  std::size_t reachable_base_clades = 0;
  std::size_t reachable_base_productions = 0;
  auto& stack = scratch.reachability_stack;
  stack.clear();
  stack.push_back(delta.root);
  while (!stack.empty()) {
    auto ref = stack.back();
    stack.pop_back();
    (void)overlay_delta_clade_key(delta, ref);

    bool newly_reachable = false;
    if (ref.space == overlay_id_space::base) {
      newly_reachable = !delta.reachable_base_clade[ref.id];
      if (newly_reachable) {
        delta.reachable_base_clade[ref.id] = true;
        ++reachable_base_clades;
      }
    } else {
      newly_reachable = !delta.reachable_temp_clade[ref.id];
      if (newly_reachable) {
        delta.reachable_temp_clade[ref.id] = true;
        ++delta.reachability_stats.reachable_temp_clades;
      }
    }
    if (!newly_reachable) continue;

    validate_reachable_overlay_clade(delta, ref);

    if (ref.space == overlay_id_space::base) {
      for (auto pid : base.productions_by_parent[ref.id]) {
        if (overlay_delta_base_production_removed(delta, pid)) continue;
        if (!base_productions_already_validated) {
          validate_reachable_base_production(delta, pid);
        }
        ++reachable_base_productions;
        for (auto child : base.productions[pid].children) {
          stack.push_back(base_clade_ref(child));
        }
      }
    }

    for (auto temp_pid : temp_productions_for_parent(delta, ref)) {
      if (temp_pid == no_production ||
          temp_pid >= delta.temp_productions.size()) {
        throw std::runtime_error(
            "chart SPR overlay-delta: temp production id out of range");
      }
      ++delta.reachability_stats.reachable_temp_productions;
      auto const& prod = delta.temp_productions[temp_pid];
      for (auto child : prod.children) stack.push_back(child);
    }
  }

  delta.reachability_stats.reachable_clades =
      reachable_base_clades + delta.reachability_stats.reachable_temp_clades;
  delta.reachability_stats.reachable_productions =
      reachable_base_productions +
      delta.reachability_stats.reachable_temp_productions;

  auto fraction = options.full_grammar_like_reachability_fraction;
  if (fraction > 0.0) {
    bool base_clade_like =
        !base.clades.empty() &&
        (static_cast<double>(reachable_base_clades) /
             static_cast<double>(base.clades.size()) >
         fraction);
    bool base_production_like =
        !base.productions.empty() &&
        (static_cast<double>(reachable_base_productions) /
             static_cast<double>(base.productions.size()) >
         fraction);
    delta.reachability_stats.full_grammar_like =
        base_clade_like || base_production_like;
  }
  stack.clear();
}

inline void compute_overlay_delta_affected_order(
    spr_overlay_delta& delta, spr_overlay_delta_build_scratch& scratch,
    candidate_chart_execution_plan_build_stats* external_stats = nullptr) {
  auto const& base = *delta.base;
  delta.affected_base_clade.assign(base.clades.size(), false);
  delta.affected_temp_clade.assign(delta.temp_clades.size(), false);
  auto& queue = scratch.affected_queue;
  queue.clear();

  for (std::size_t i = 0; i < delta.temp_clades.size(); ++i) {
    mark_overlay_delta_affected(delta, delta.affected_base_clade,
                                delta.affected_temp_clade, queue,
                                temp_clade_ref(static_cast<clade_id>(i)));
  }
  for (auto pid : delta.removed_base_productions) {
    if (pid == no_production || pid >= base.productions.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: removed production out of range");
    }
    mark_overlay_delta_affected(delta, delta.affected_base_clade,
                                delta.affected_temp_clade, queue,
                                base_clade_ref(base.productions[pid].parent));
  }
  for (std::size_t i = 0; i < delta.temp_productions.size(); ++i) {
    mark_overlay_delta_affected(delta, delta.affected_base_clade,
                                delta.affected_temp_clade, queue,
                                delta.temp_productions[i].parent);
  }
  mark_overlay_delta_affected_if_present(delta, delta.affected_base_clade,
                                         delta.affected_temp_clade, queue,
                                         delta.candidate_old_parent);
  mark_overlay_delta_affected_if_present(delta, delta.affected_base_clade,
                                         delta.affected_temp_clade, queue,
                                         delta.candidate_new_sibling_or_target);

  for (std::size_t head = 0; head < queue.size(); ++head) {
    auto child = queue[head];
    if (child.space == overlay_id_space::base) {
      for (auto pid : base.productions_by_child[child.id]) {
        if (overlay_delta_base_production_removed(delta, pid)) continue;
        auto parent = base.productions[pid].parent;
        mark_overlay_delta_affected(delta, delta.affected_base_clade,
                                    delta.affected_temp_clade, queue,
                                    base_clade_ref(parent));
      }
      for (auto temp_pid : delta.temp_productions_by_base_child[child.id]) {
        mark_overlay_delta_affected(delta, delta.affected_base_clade,
                                    delta.affected_temp_clade, queue,
                                    delta.temp_productions[temp_pid].parent);
      }
    } else {
      for (auto temp_pid : delta.temp_productions_by_temp_child[child.id]) {
        mark_overlay_delta_affected(delta, delta.affected_base_clade,
                                    delta.affected_temp_clade, queue,
                                    delta.temp_productions[temp_pid].parent);
      }
    }
  }

  delta.affected_base_row_slot.assign(base.clades.size(),
                                      chart_spr_overlay_row_npos);
  delta.affected_temp_row_slot.assign(delta.temp_clades.size(),
                                      chart_spr_overlay_row_npos);
  delta.affected_order.clear();
  for (clade_id cid = 0; cid < base.clades.size(); ++cid) {
    if (!delta.affected_base_clade[cid] || !delta.reachable_base_clade[cid]) {
      delta.affected_base_clade[cid] = false;
      continue;
    }
    delta.affected_order.push_back(base_clade_ref(cid));
  }
  for (clade_id cid = 0; cid < delta.temp_clades.size(); ++cid) {
    if (!delta.affected_temp_clade[cid] || !delta.reachable_temp_clade[cid]) {
      delta.affected_temp_clade[cid] = false;
      continue;
    }
    delta.affected_order.push_back(temp_clade_ref(cid));
  }

  parsimony_chart_detail::record_clade_order_sort();
  ++delta.candidate_plan_build_stats.clade_order_sorts;
  if (external_stats != nullptr) ++external_stats->clade_order_sorts;
  std::sort(delta.affected_order.begin(), delta.affected_order.end(),
            [&](overlay_clade_ref lhs, overlay_clade_ref rhs) {
              auto lsize = overlay_delta_clade_size(delta, lhs);
              auto rsize = overlay_delta_clade_size(delta, rhs);
              if (lsize != rsize) return lsize < rsize;
              return lhs < rhs;
            });

  for (std::size_t slot = 0; slot < delta.affected_order.size(); ++slot) {
    auto ref = delta.affected_order[slot];
    if (ref.space == overlay_id_space::base) {
      delta.affected_base_clade[ref.id] = true;
      delta.affected_base_row_slot[ref.id] = slot;
    } else {
      delta.affected_temp_clade[ref.id] = true;
      delta.affected_temp_row_slot[ref.id] = slot;
    }
  }
  queue.clear();
}

inline candidate_chart_child_descriptor compile_candidate_chart_child(
    spr_overlay_delta const& delta, overlay_clade_ref child,
    std::size_t parent_row_slot, chart_execution_plan const* base_plan) {
  candidate_chart_child_descriptor descriptor;
  descriptor.clade = child;
  if (child.space == overlay_id_space::base) {
    if (child.id == no_clade ||
        child.id >= delta.affected_base_row_slot.size()) {
      throw std::runtime_error(
          "chart SPR candidate execution plan: base child out of range");
    }
    descriptor.base_clade = child.id;
    descriptor.local_row_slot = delta.affected_base_row_slot[child.id];
    if (base_plan != nullptr) {
      descriptor.leaf_taxon = base_plan->clade(child.id).leaf_taxon;
    } else {
      auto const& key = overlay_delta_clade_key(delta, child);
      if (key.taxa.size() == 1) descriptor.leaf_taxon = key.taxa.front();
    }
  } else {
    if (child.id == no_clade ||
        child.id >= delta.affected_temp_row_slot.size()) {
      throw std::runtime_error(
          "chart SPR candidate execution plan: temp child out of range");
    }
    descriptor.local_row_slot = delta.affected_temp_row_slot[child.id];
    auto const& key = overlay_delta_clade_key(delta, child);
    if (key.taxa.size() == 1) descriptor.leaf_taxon = key.taxa.front();
    if (!descriptor.has_local_row()) {
      throw std::runtime_error(
          "chart SPR candidate execution plan: reachable temp child has no "
          "local row");
    }
  }
  if (descriptor.has_local_row() &&
      descriptor.local_row_slot >= parent_row_slot) {
    throw std::runtime_error(
        "chart SPR candidate execution plan: child row is not before parent "
        "row");
  }
  return descriptor;
}

inline void append_candidate_chart_production_descriptor(
    spr_overlay_delta& delta, overlay_production_ref source,
    std::span<overlay_clade_ref const> children, std::size_t parent_row_slot,
    chart_execution_plan const* base_plan,
    candidate_chart_execution_plan_build_stats* external_stats) {
  if (children.size() < 2) {
    throw std::runtime_error(
        "chart SPR candidate execution plan: production has fewer than 2 "
        "children");
  }
  candidate_chart_production_descriptor descriptor;
  descriptor.source = source;
  descriptor.child_begin = delta.compiled_children.size();
  descriptor.child_count = children.size();
  for (auto child : children) {
    delta.compiled_children.push_back(compile_candidate_chart_child(
        delta, child, parent_row_slot, base_plan));
  }
  delta.compiled_productions.push_back(descriptor);
  ++delta.candidate_plan_build_stats.production_descriptors_compiled;
  if (external_stats != nullptr) {
    ++external_stats->production_descriptors_compiled;
  }
}

inline void compile_overlay_delta_execution_rows(
    spr_overlay_delta& delta, chart_execution_plan const* base_plan = nullptr,
    candidate_chart_execution_plan_build_stats* external_stats = nullptr) {
  auto const& base = *delta.base;
  delta.compiled_rows.clear();
  delta.compiled_productions.clear();
  delta.compiled_children.clear();
  delta.compiled_rows.reserve(delta.affected_order.size());

  for (std::size_t row_slot = 0; row_slot < delta.affected_order.size();
       ++row_slot) {
    auto ref = delta.affected_order[row_slot];
    candidate_chart_row_descriptor row;
    row.clade = ref;
    row.production_begin = delta.compiled_productions.size();
    auto const& key = overlay_delta_clade_key(delta, ref);
    if (key.taxa.size() == 1) {
      row.leaf_taxon = key.taxa.front();
    }

    if (ref.space == overlay_id_space::base) {
      auto production_ids = base_plan != nullptr
                                ? base_plan->productions_for_parent(ref.id)
                                : std::span<production_id const>{
                                      base.productions_by_parent[ref.id]};
      for (auto pid : production_ids) {
        if (overlay_delta_base_production_removed(delta, pid)) continue;
        auto base_children =
            base_plan != nullptr
                ? base_plan->children(pid)
                : std::span<clade_id const>{base.productions[pid].children};
        std::size_t begin = delta.compiled_children.size();
        for (auto child : base_children) {
          delta.compiled_children.push_back(compile_candidate_chart_child(
              delta, base_clade_ref(child), row_slot, base_plan));
        }
        candidate_chart_production_descriptor descriptor;
        descriptor.source = base_production_ref(pid);
        descriptor.child_begin = begin;
        descriptor.child_count = base_children.size();
        delta.compiled_productions.push_back(descriptor);
        ++delta.candidate_plan_build_stats.production_descriptors_compiled;
        if (external_stats != nullptr) {
          ++external_stats->production_descriptors_compiled;
        }
      }
    }

    for (auto temp_pid : temp_productions_for_parent(delta, ref)) {
      if (temp_pid == no_production ||
          temp_pid >= delta.temp_productions.size()) {
        throw std::runtime_error(
            "chart SPR candidate execution plan: temp production id out of "
            "range");
      }
      auto const& production = delta.temp_productions[temp_pid];
      append_candidate_chart_production_descriptor(
          delta, temp_production_ref(temp_pid), production.children, row_slot,
          base_plan, external_stats);
    }

    row.production_count =
        delta.compiled_productions.size() - row.production_begin;
    if (row.is_leaf()) {
      if (row.production_count != 0) {
        throw std::runtime_error(
            "chart SPR candidate execution plan: singleton clade has "
            "productions");
      }
    } else if (row.production_count == 0) {
      throw std::runtime_error(
          "chart SPR candidate execution plan: non-singleton clade has no "
          "productions");
    }
    delta.compiled_rows.push_back(row);
  }
}

inline candidate_chart_row_descriptor const& candidate_chart_row_for_ref(
    spr_overlay_delta const& delta, overlay_clade_ref ref) {
  std::size_t slot = chart_spr_overlay_row_npos;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= delta.affected_base_row_slot.size()) {
      throw std::runtime_error(
          "chart SPR candidate execution plan: base row ref out of range");
    }
    slot = delta.affected_base_row_slot[ref.id];
  } else {
    if (ref.id == no_clade || ref.id >= delta.affected_temp_row_slot.size()) {
      throw std::runtime_error(
          "chart SPR candidate execution plan: temp row ref out of range");
    }
    slot = delta.affected_temp_row_slot[ref.id];
  }
  if (slot == chart_spr_overlay_row_npos ||
      slot >= delta.compiled_rows.size() ||
      delta.compiled_rows[slot].clade != ref) {
    throw std::runtime_error(
        "chart SPR candidate execution plan: missing compiled row");
  }
  return delta.compiled_rows[slot];
}

inline std::span<candidate_chart_production_descriptor const>
candidate_chart_productions_for_row(spr_overlay_delta const& delta,
                                    candidate_chart_row_descriptor const& row) {
  return std::span<candidate_chart_production_descriptor const>{
      delta.compiled_productions}
      .subspan(row.production_begin, row.production_count);
}

inline std::span<candidate_chart_child_descriptor const>
candidate_chart_children(
    spr_overlay_delta const& delta,
    candidate_chart_production_descriptor const& production) {
  return std::span<candidate_chart_child_descriptor const>{
      delta.compiled_children}
      .subspan(production.child_begin, production.child_count);
}

inline void assert_overlay_delta_execution_plan_compatible(
    spr_overlay_delta const& delta, chart_execution_plan const& plan) {
  if (delta.base_plan_generation == 0 ||
      delta.base_plan_generation != plan.grammar_generation() ||
      delta.base_plan_fingerprint != plan.fingerprint()) {
    throw chart_execution_plan_mismatch(
        "chart SPR candidate execution plan: base plan mismatch");
  }
}

inline void record_overlay_delta_reachability_counters(
    chart_spr_search_counters& counters,
    overlay_reachability_stats const& stats) {
  ++counters.overlay_reachability_validations;
  counters.reachable_clades_traversed += stats.reachable_clades;
  counters.reachable_productions_traversed += stats.reachable_productions;
  counters.reachable_temp_clades_traversed += stats.reachable_temp_clades;
  counters.reachable_temp_productions_traversed +=
      stats.reachable_temp_productions;
  if (stats.full_grammar_like) ++counters.reachability_full_grammar_like_passes;
}

}  // namespace chart_spr_search_detail

namespace chart_spr_search_detail {

inline void fail_closed_overlay_delta_build(
    spr_overlay_delta& delta,
    spr_overlay_delta_build_scratch& scratch) noexcept {
  delta.base = nullptr;
  delta.candidate_old_parent = {};
  delta.candidate_new_sibling_or_target = {};
  delta.commit_source.clear();
  delta.root = {};
  delta.reachability_stats = {};
  delta.base_plan_generation = 0;
  delta.base_plan_fingerprint = {};
  delta.candidate_plan_build_stats = {};
  delta.removed_base_productions.clear();
  delta.affected_order.clear();
  delta.affected_base_clade.clear();
  delta.affected_temp_clade.clear();
  delta.affected_base_row_slot.clear();
  delta.affected_temp_row_slot.clear();
  delta.compiled_rows.clear();
  delta.compiled_productions.clear();
  delta.compiled_children.clear();
  delta.removed_base_production.clear();
  delta.reachable_base_clade.clear();
  delta.reachable_temp_clade.clear();
  scratch.clear_operation_borrows();
}

inline void build_spr_overlay_delta_impl_into(
    clade_grammar const& base, chart_execution_plan const* base_plan,
    grammar_spr_candidate const& candidate,
    local_spr_score_options const& options,
    checked_chart_execution_plan_ref const* checked_base,
    spr_overlay_delta& delta, spr_overlay_delta_build_scratch& scratch,
    candidate_chart_execution_plan_build_stats* external_stats = nullptr) {
  fail_closed_overlay_delta_build(delta, scratch);
  try {
    if (base.root_clade == no_clade || base.root_clade >= base.clades.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: root clade out of range");
    }
    if (base_plan != nullptr) {
      if (checked_base == nullptr) {
        throw chart_execution_plan_mismatch(
            "chart SPR candidate execution plan: missing checked base token");
      }
      checked_base->assert_same(base, *base_plan);
    }

    try {
      delta.base = &base;
      delta.candidate_old_parent = candidate.old_parent;
      delta.candidate_new_sibling_or_target =
          candidate.new_sibling_or_target;
      delta.commit_source.clear();
      delta.root = base_clade_ref(base.root_clade);
      delta.reachability_stats = {};
      delta.base_plan_generation = 0;
      delta.base_plan_fingerprint = {};
      delta.candidate_plan_build_stats = {};
      delta.affected_order.clear();
      delta.compiled_rows.clear();
      delta.compiled_productions.clear();
      delta.compiled_children.clear();
      copy_candidate_payload_reusing_storage(delta, candidate, scratch);
      if (base_plan != nullptr) {
        delta.base_plan_generation = base_plan->grammar_generation();
        delta.base_plan_fingerprint = base_plan->fingerprint();
      }

      auto taxon_count = base.taxa.id_to_sample_id.size();
      for (std::size_t i = 0; i < delta.temp_clades.size(); ++i) {
        validate_overlay_delta_temp_clade_key(delta.temp_clades[i], taxon_count,
                                              i);
      }

      delta.removed_base_productions.clear();
      delta.removed_base_productions.reserve(
          candidate.removed_productions.size());
      for (auto ref : candidate.removed_productions) {
        if (ref.space != overlay_id_space::base) {
          throw std::runtime_error(
              "chart SPR overlay-delta: candidates may tombstone only base "
              "productions");
        }
        if (ref.id == no_production || ref.id >= base.productions.size()) {
          throw std::runtime_error(
              "chart SPR overlay-delta: removed production out of range");
        }
        delta.removed_base_productions.push_back(ref.id);
      }
      std::sort(delta.removed_base_productions.begin(),
                delta.removed_base_productions.end());
      delta.removed_base_productions.erase(
          std::unique(delta.removed_base_productions.begin(),
                      delta.removed_base_productions.end()),
          delta.removed_base_productions.end());

      delta.removed_base_production.assign(base.productions.size(), false);
      for (auto pid : delta.removed_base_productions) {
        delta.removed_base_production[pid] = true;
      }

      build_overlay_delta_temp_indices(delta, scratch, external_stats);
      compute_overlay_delta_reachability(delta, scratch, options,
                                         base_plan != nullptr);
      compute_overlay_delta_affected_order(delta, scratch, external_stats);
    } catch (chart_execution_plan_mismatch const&) {
      throw;
    } catch (std::bad_alloc const&) {
      throw;
    } catch (std::overflow_error const&) {
      throw;
    } catch (std::length_error const&) {
      throw;
    } catch (std::runtime_error const& error) {
      throw chart_spr_candidate_invalid_error(error.what());
    }
    // Once candidate validation/reachability succeeds, compiled-descriptor
    // failures are internal invariants and must remain hard errors.
    compile_overlay_delta_execution_rows(delta, base_plan, external_stats);
    scratch.clear_operation_borrows();
  } catch (...) {
    fail_closed_overlay_delta_build(delta, scratch);
    throw;
  }
}

inline spr_overlay_delta build_spr_overlay_delta_impl(
    clade_grammar const& base, chart_execution_plan const* base_plan,
    grammar_spr_candidate const& candidate,
    local_spr_score_options const& options,
    checked_chart_execution_plan_ref const* checked_base,
    candidate_chart_execution_plan_build_stats* external_stats = nullptr) {
  spr_overlay_delta delta;
  spr_overlay_delta_build_scratch scratch;
  build_spr_overlay_delta_impl_into(base, base_plan, candidate, options,
                                    checked_base, delta, scratch,
                                    external_stats);
  return delta;
}

}  // namespace chart_spr_search_detail

inline spr_overlay_delta build_spr_overlay_delta(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    local_spr_score_options const& options = {}) {
  return chart_spr_search_detail::build_spr_overlay_delta_impl(
      base, nullptr, candidate, options, nullptr);
}

inline spr_overlay_delta build_spr_overlay_delta(
    clade_grammar const& base, chart_execution_plan const& base_plan,
    grammar_spr_candidate const& candidate,
    local_spr_score_options const& options = {}) {
  auto checked = check_chart_execution_plan(base, base_plan);
  return chart_spr_search_detail::build_spr_overlay_delta_impl(
      base, &base_plan, candidate, options, &checked);
}

// Production search owns grammar and plan as one immutable published state.
// Publication performs the full structural fingerprint check exactly once;
// candidate preparation then uses this O(1) generation/shape check.  Callers
// outside that ownership contract must use the checked overload above.
inline spr_overlay_delta build_spr_overlay_delta_from_resident_plan(
    clade_grammar const& base,
    checked_chart_execution_plan_ref const& checked_base,
    grammar_spr_candidate const& candidate,
    local_spr_score_options const& options = {},
    candidate_chart_execution_plan_build_stats* build_stats = nullptr) {
  auto const& base_plan = checked_base.plan();
  return chart_spr_search_detail::build_spr_overlay_delta_impl(
      base, &base_plan, candidate, options, &checked_base, build_stats);
}

inline void build_spr_overlay_delta_from_resident_plan_into(
    clade_grammar const& base,
    checked_chart_execution_plan_ref const& checked_base,
    grammar_spr_candidate const& candidate, spr_overlay_delta& delta,
    spr_overlay_delta_build_scratch& scratch,
    local_spr_score_options const& options = {},
    candidate_chart_execution_plan_build_stats* build_stats = nullptr) {
  auto const& base_plan = checked_base.plan();
  chart_spr_search_detail::build_spr_overlay_delta_impl_into(
      base, &base_plan, candidate, options, &checked_base, delta, scratch,
      build_stats);
}

inline std::array<chart_cost, nuc_state_count> const&
local_overlay_chart_row(local_overlay_chart_rows const& rows,
                        single_site_chart const& base_chart,
                        overlay_clade_ref ref) {
  auto slot = rows.slot_for(ref);
  if (slot != local_overlay_chart_rows::npos) {
    if (slot >= rows.rows.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta row: local row slot out of range");
    }
    return rows.rows[slot];
  }
  if (ref.space != overlay_id_space::base) {
    throw std::runtime_error(
        "chart SPR overlay-delta row: reachable temp clade has no local row");
  }
  if (ref.id == no_clade || ref.id >= base_chart.inside.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta row: base chart row out of range");
  }
  return base_chart.inside[ref.id];
}

inline std::array<chart_cost, nuc_state_count> const& local_overlay_chart_row(
    local_overlay_chart_rows const& rows,
    chart_spr_inside_row_view const& base_rows, std::size_t pattern,
    overlay_clade_ref ref) {
  auto slot = rows.slot_for(ref);
  if (slot != local_overlay_chart_rows::npos) {
    if (slot >= rows.rows.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta row view: local row slot out of range");
    }
    return rows.rows[slot];
  }
  if (ref.space != overlay_id_space::base) {
    throw std::runtime_error(
        "chart SPR overlay-delta row view: reachable temp clade has no local "
        "row");
  }
  return base_rows.row(pattern, ref.id);
}

struct overlay_row_provider {
  spr_overlay_delta const& delta;
  single_site_chart const& base_chart;
  local_overlay_chart_rows const& local_rows;

  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      overlay_clade_ref ref) const {
    (void)delta;
    return local_overlay_chart_row(local_rows, base_chart, ref);
  }

  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      candidate_chart_child_descriptor const& child) const {
    if (child.has_local_row()) {
      if (child.local_row_slot >= local_rows.rows.size()) {
        throw std::runtime_error(
            "chart SPR overlay-delta row: compiled local child slot out of "
            "range");
      }
      return local_rows.rows[child.local_row_slot];
    }
    if (child.base_clade == no_clade ||
        child.base_clade >= base_chart.inside.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta row: compiled base child out of range");
    }
    return base_chart.inside[child.base_clade];
  }
};

struct overlay_row_view_provider {
  spr_overlay_delta const& delta;
  chart_spr_inside_row_view const& base_rows;
  std::size_t pattern = 0;
  local_overlay_chart_rows const& local_rows;

  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      overlay_clade_ref ref) const {
    (void)delta;
    return local_overlay_chart_row(local_rows, base_rows, pattern, ref);
  }

  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      candidate_chart_child_descriptor const& child) const {
    if (child.has_local_row()) {
      if (child.local_row_slot >= local_rows.rows.size()) {
        throw std::runtime_error(
            "chart SPR overlay-delta row view: compiled local child slot out "
            "of range");
      }
      return local_rows.rows[child.local_row_slot];
    }
    return base_rows.row(pattern, child.base_clade);
  }
};

namespace chart_spr_search_detail {

template <class RowProvider>
inline void accumulate_overlay_production_row(
    std::array<chart_cost, nuc_state_count>& row,
    std::span<candidate_chart_child_descriptor const> children,
    RowProvider const& provider,
    chart_spr_search_counters* counters = nullptr) {
  if (children.size() < 2) {
    throw std::runtime_error(
        "chart SPR overlay-delta: local row recompute requires at least 2 "
        "children");
  }
  if (children.size() != 2 && counters != nullptr) {
    ++counters->multifurcation_productions_scored;
  }

  struct production_view {
    std::span<candidate_chart_child_descriptor const> children;
  } prod{children};
    auto row_provider =
        [&](candidate_chart_child_descriptor const& child) -> auto const& {
      return provider.row(child);
    };
    auto const totals =
        parsimony_chart_detail::combine_production_inside_rows_unit_fitch(
            prod, row_provider);
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      row[parent_state] = std::min(row[parent_state], totals[parent_state]);
    }
    if (counters != nullptr) {
      ++counters->local_unit_fitch_fast_path_productions_scored;
    }
}

template <class RowProvider>
inline std::array<chart_cost, nuc_state_count> recompute_overlay_delta_row(
    spr_overlay_delta const& delta, leaf_site_states_view leaf_states,
    RowProvider const& provider,
    candidate_chart_row_descriptor const& compiled_row,
    chart_spr_search_counters* counters = nullptr) {
  if (compiled_row.is_leaf()) {
    return overlay_delta_leaf_row(compiled_row.leaf_taxon, leaf_states);
  }

  auto row = parsimony_chart_detail::make_inf_row();
  auto productions = candidate_chart_productions_for_row(delta, compiled_row);
  if (productions.empty()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: non-singleton clade has no productions "
        "during local row recompute");
  }
  for (auto const& production : productions) {
    accumulate_overlay_production_row(
        row, candidate_chart_children(delta, production), provider, counters);
  }
  return row;
}

}  // namespace chart_spr_search_detail

inline void build_local_overlay_chart_rows_into(
    clade_grammar const& base, spr_overlay_delta const& delta,
    single_site_chart const& base_chart,
    leaf_site_states_view leaf_states, local_overlay_chart_rows& rows,
    chart_options const& options = {},
    bool validate_base_chart_shapes = false,
    chart_spr_search_counters* counters = nullptr) {
  if (options.keep_trace) {
    throw std::runtime_error(
        "chart SPR overlay-delta: local row scorer does not support trace "
        "storage");
  }
  if (validate_base_chart_shapes) {
    chart_trim_detail::validate_chart_shapes(base, base_chart);
  }
  if (base_chart.inside.size() != base.clades.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: cached base chart clade count mismatch");
  }
  if (leaf_states.state_by_taxon.size() != base.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: leaf state count mismatch");
  }
  if (delta.affected_base_row_slot.size() != base.clades.size() ||
      delta.affected_temp_row_slot.size() != delta.temp_clades.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: affected row slot map size mismatch");
  }
  if (delta.compiled_rows.size() != delta.affected_order.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta: compiled row count mismatch");
  }

  rows.base_row_slot = delta.affected_base_row_slot;
  rows.temp_row_slot = delta.affected_temp_row_slot;
  auto const required_rows = delta.affected_order.size();
  if (rows.rows.capacity() < required_rows) {
    rows.rows.reserve(required_rows);
    if (counters != nullptr) {
      ++counters->local_row_scratch_capacity_growths;
    }
  }
  rows.rows.resize(required_rows);

  overlay_row_provider provider{delta, base_chart, rows};
  for (std::size_t i = 0; i < delta.compiled_rows.size(); ++i) {
    rows.rows[i] = chart_spr_search_detail::recompute_overlay_delta_row(
        delta, leaf_states, provider, delta.compiled_rows[i], counters);
  }
}

inline void build_local_overlay_chart_rows_into(
    clade_grammar const& base, spr_overlay_delta const& delta,
    chart_spr_inside_row_view const& base_rows, std::size_t pattern,
    leaf_site_states_view leaf_states, local_overlay_chart_rows& rows,
    chart_options const& options = {},
    chart_spr_search_counters* counters = nullptr) {
  if (options.keep_trace) {
    throw std::runtime_error(
        "chart SPR overlay-delta row view does not support trace storage");
  }
  if (!base_rows.valid() || base_rows.clade_count != base.clades.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta row view: cached base clade count mismatch");
  }
  if (leaf_states.state_by_taxon.size() != base.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta row view: leaf state count mismatch");
  }
  if (delta.affected_base_row_slot.size() != base.clades.size() ||
      delta.affected_temp_row_slot.size() != delta.temp_clades.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta row view: affected slot map size mismatch");
  }
  if (delta.compiled_rows.size() != delta.affected_order.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta row view: compiled row count mismatch");
  }

  rows.base_row_slot = delta.affected_base_row_slot;
  rows.temp_row_slot = delta.affected_temp_row_slot;
  auto const required_rows = delta.affected_order.size();
  if (rows.rows.capacity() < required_rows) {
    rows.rows.reserve(required_rows);
    if (counters != nullptr) {
      ++counters->local_row_scratch_capacity_growths;
    }
  }
  rows.rows.resize(required_rows);

  overlay_row_view_provider provider{delta, base_rows, pattern, rows};
  for (std::size_t i = 0; i < delta.compiled_rows.size(); ++i) {
    rows.rows[i] = chart_spr_search_detail::recompute_overlay_delta_row(
        delta, leaf_states, provider, delta.compiled_rows[i], counters);
  }
}

inline void build_local_overlay_chart_rows_into(
    clade_grammar const& base, spr_overlay_delta const& delta,
    single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, local_overlay_chart_rows& rows,
    chart_options const& options = {},
    bool validate_base_chart_shapes = false,
    chart_spr_search_counters* counters = nullptr) {
  build_local_overlay_chart_rows_into(
      base, delta, base_chart, view_leaf_site_states(leaf_states), rows,
      options, validate_base_chart_shapes, counters);
}

// Legacy/direct wrapper.  Prepared production scoring uses the overload above
// with its checked resident grammar, so its immutable descriptor carries no
// borrowed grammar pointer.
inline void build_local_overlay_chart_rows_into(
    spr_overlay_delta const& delta, single_site_chart const& base_chart,
    leaf_site_states_view leaf_states, local_overlay_chart_rows& rows,
    chart_options const& options = {},
    bool validate_base_chart_shapes = false,
    chart_spr_search_counters* counters = nullptr) {
  if (delta.base == nullptr) {
    throw std::runtime_error("chart SPR overlay-delta: missing base grammar");
  }
  build_local_overlay_chart_rows_into(
      *delta.base, delta, base_chart, leaf_states, rows, options,
      validate_base_chart_shapes, counters);
}

inline void build_local_overlay_chart_rows_into(
    spr_overlay_delta const& delta, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, local_overlay_chart_rows& rows,
    chart_options const& options = {},
    bool validate_base_chart_shapes = false,
    chart_spr_search_counters* counters = nullptr) {
  build_local_overlay_chart_rows_into(
      delta, base_chart, view_leaf_site_states(leaf_states), rows, options,
      validate_base_chart_shapes, counters);
}

inline local_overlay_chart_rows build_local_overlay_chart_rows(
    clade_grammar const& base, spr_overlay_delta const& delta,
    single_site_chart const& base_chart,
    leaf_site_states_view leaf_states, chart_options const& options = {},
    bool validate_base_chart_shapes = false) {
  local_overlay_chart_rows rows;
  build_local_overlay_chart_rows_into(base, delta, base_chart, leaf_states,
                                      rows, options,
                                      validate_base_chart_shapes);
  return rows;
}

inline local_overlay_chart_rows build_local_overlay_chart_rows(
    clade_grammar const& base, spr_overlay_delta const& delta,
    single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, chart_options const& options = {},
    bool validate_base_chart_shapes = false) {
  return build_local_overlay_chart_rows(
      base, delta, base_chart, view_leaf_site_states(leaf_states), options,
      validate_base_chart_shapes);
}

inline local_overlay_chart_rows build_local_overlay_chart_rows(
    clade_grammar const&, spr_overlay_delta&&, single_site_chart const&,
    leaf_site_states_view, chart_options const& = {}, bool = false) = delete;
inline local_overlay_chart_rows build_local_overlay_chart_rows(
    clade_grammar const&, spr_overlay_delta&&, single_site_chart const&,
    leaf_site_states const&, chart_options const& = {}, bool = false) = delete;
inline local_overlay_chart_rows build_local_overlay_chart_rows(
    clade_grammar const&, spr_overlay_delta const&&, single_site_chart const&,
    leaf_site_states_view, chart_options const& = {}, bool = false) = delete;
inline local_overlay_chart_rows build_local_overlay_chart_rows(
    clade_grammar const&, spr_overlay_delta const&&, single_site_chart const&,
    leaf_site_states const&, chart_options const& = {}, bool = false) = delete;

inline local_overlay_chart_rows build_local_overlay_chart_rows(
    spr_overlay_delta const& delta, single_site_chart const& base_chart,
    leaf_site_states_view leaf_states, chart_options const& options = {},
    bool validate_base_chart_shapes = false) {
  local_overlay_chart_rows rows;
  build_local_overlay_chart_rows_into(delta, base_chart, leaf_states, rows,
                                      options, validate_base_chart_shapes);
  return rows;
}

inline local_overlay_chart_rows build_local_overlay_chart_rows(
    spr_overlay_delta const& delta, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, chart_options const& options = {},
    bool validate_base_chart_shapes = false) {
  return build_local_overlay_chart_rows(
      delta, base_chart, view_leaf_site_states(leaf_states), options,
      validate_base_chart_shapes);
}

inline local_overlay_chart_rows build_local_overlay_chart_rows(
    spr_overlay_delta&&, single_site_chart const&, leaf_site_states_view,
    chart_options const& = {}, bool = false) = delete;
inline local_overlay_chart_rows build_local_overlay_chart_rows(
    spr_overlay_delta&&, single_site_chart const&, leaf_site_states const&,
    chart_options const& = {}, bool = false) = delete;
inline local_overlay_chart_rows build_local_overlay_chart_rows(
    spr_overlay_delta const&&, single_site_chart const&, leaf_site_states_view,
    chart_options const& = {}, bool = false) = delete;
inline local_overlay_chart_rows build_local_overlay_chart_rows(
    spr_overlay_delta const&&, single_site_chart const&,
    leaf_site_states const&, chart_options const& = {}, bool = false) = delete;

inline chart_spr_candidate_score make_invalid_local_candidate_score(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    std::string reason) {
  auto old_score = state.composite_lower_bound_with_invariants;
  chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.lower_bound.value = spr_score_result{0, old_score, old_score, false};
  scored.lower_bound.kind = chart_spr_score_kind::composite_lower_bound;
  scored.lower_bound.convention =
      chart_spr_score_convention::full_with_invariants;
  scored.lower_bound.invariant_offset_applied =
      state.invariant_constant_offset;
  scored.valid = false;
  scored.invalid_reason = std::move(reason);
  return scored;
}

inline void verify_local_overlay_rows_against_full(
    spr_overlay_delta const& delta, local_overlay_chart_rows const& local_rows,
    single_site_chart const& base_chart,
    overlay_materialization_result const& materialized,
    leaf_site_states_view leaf_states, chart_options options) {
  (void)delta;
  options.keep_trace = false;
  options.max_trace_choices = 0;
  auto full = build_single_site_chart(materialized.grammar, leaf_states,
                                      options);
  if (full.inside.size() != materialized.dense_clade_to_ref.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta verification: dense clade map size mismatch");
  }
  for (std::size_t dense = 0; dense < materialized.dense_clade_to_ref.size();
       ++dense) {
    auto ref = materialized.dense_clade_to_ref[dense];
    auto const& local = local_overlay_chart_row(local_rows, base_chart, ref);
    if (local != full.inside[dense]) {
      throw std::runtime_error(
          "chart SPR overlay-delta verification: local row differs from "
          "full overlay chart");
    }
  }
}

inline void verify_local_overlay_rows_against_full(
    spr_overlay_delta const& delta, local_overlay_chart_rows const& local_rows,
    chart_spr_inside_row_view const& base_rows, std::size_t pattern,
    overlay_materialization_result const& materialized,
    leaf_site_states_view leaf_states, chart_options options) {
  (void)delta;
  options.keep_trace = false;
  options.max_trace_choices = 0;
  auto full =
      build_single_site_chart(materialized.grammar, leaf_states, options);
  if (full.inside.size() != materialized.dense_clade_to_ref.size()) {
    throw std::runtime_error(
        "chart SPR overlay-delta row-view verification: dense clade map size "
        "mismatch");
  }
  for (std::size_t dense = 0; dense < materialized.dense_clade_to_ref.size();
       ++dense) {
    auto ref = materialized.dense_clade_to_ref[dense];
    auto const& local =
        local_overlay_chart_row(local_rows, base_rows, pattern, ref);
    if (local != full.inside[dense]) {
      throw std::runtime_error(
          "chart SPR overlay-delta row-view verification: local row differs "
          "from full overlay chart");
    }
  }
}

inline void verify_local_overlay_rows_against_full(
    spr_overlay_delta const& delta, local_overlay_chart_rows const& local_rows,
    single_site_chart const& base_chart,
    overlay_materialization_result const& materialized,
    leaf_site_states const& leaf_states, chart_options options) {
  verify_local_overlay_rows_against_full(
      delta, local_rows, base_chart, materialized,
      view_leaf_site_states(leaf_states), options);
}

inline void add_chart_spr_search_counters(
    chart_spr_search_counters& dst, chart_spr_search_counters const& src) {
  add_chart_spr_scheduler_axis_counters(dst.scheduler_axes, src.scheduler_axes);
  dst.grammar_rebuilds += src.grammar_rebuilds;
  dst.pattern_rebuilds += src.pattern_rebuilds;
  dst.base_chart_cache_rebuilds += src.base_chart_cache_rebuilds;
  dst.chart_execution_plan_builds += src.chart_execution_plan_builds;
  dst.chart_execution_plan_cache_hits += src.chart_execution_plan_cache_hits;
  dst.candidate_execution_plan_builds += src.candidate_execution_plan_builds;
  dst.candidate_execution_plan_cache_hits +=
      src.candidate_execution_plan_cache_hits;
  dst.full_grammar_validations += src.full_grammar_validations;
  dst.production_index_validations += src.production_index_validations;
  dst.production_partition_validations += src.production_partition_validations;
  dst.dynamic_overlay_payload_partition_validations +=
      src.dynamic_overlay_payload_partition_validations;
  dst.candidate_partition_validations += src.candidate_partition_validations;
  dst.clade_order_sorts += src.clade_order_sorts;
  dst.production_descriptors_compiled += src.production_descriptors_compiled;
  dst.plan_mismatch_rejections += src.plan_mismatch_rejections;
  dst.candidate_pattern_full_grammar_validations +=
      src.candidate_pattern_full_grammar_validations;
  dst.candidate_pattern_partition_validations +=
      src.candidate_pattern_partition_validations;
  dst.candidate_pattern_clade_order_sorts +=
      src.candidate_pattern_clade_order_sorts;
  dst.full_overlay_materializations += src.full_overlay_materializations;
  dst.overlay_materializations_for_oracle +=
      src.overlay_materializations_for_oracle;
  dst.overlay_materializations_for_local_scoring_bridge +=
      src.overlay_materializations_for_local_scoring_bridge;
  dst.overlay_materializations_for_exact_verification +=
      src.overlay_materializations_for_exact_verification;
  dst.overlay_materializations_for_accept_materialization +=
      src.overlay_materializations_for_accept_materialization;
  dst.overlay_materializations_for_final_compaction +=
      src.overlay_materializations_for_final_compaction;
  dst.materialization_exact_verification_ms +=
      src.materialization_exact_verification_ms;
  dst.materialization_accepted_update_ms +=
      src.materialization_accepted_update_ms;
  dst.materialization_final_compaction_ms +=
      src.materialization_final_compaction_ms;
  dst.sidecar_rebuilds_after_accept += src.sidecar_rebuilds_after_accept;
  dst.full_composite_rebuilds += src.full_composite_rebuilds;
  dst.local_candidate_scores += src.local_candidate_scores;
  dst.local_rows_recomputed += src.local_rows_recomputed;
  dst.local_unit_fitch_fast_path_productions_scored +=
      src.local_unit_fitch_fast_path_productions_scored;
  dst.local_leaf_state_view_uses += src.local_leaf_state_view_uses;
  dst.local_leaf_state_owned_copies += src.local_leaf_state_owned_copies;
  dst.local_row_scratch_capacity_growths +=
      src.local_row_scratch_capacity_growths;
  dst.local_commit_inside_row_view_pattern_visits +=
      src.local_commit_inside_row_view_pattern_visits;
  dst.multifurcation_productions_scored +=
      src.multifurcation_productions_scored;
  dst.local_score_parallel_batches += src.local_score_parallel_batches;
  dst.local_score_worker_tasks += src.local_score_worker_tasks;
  dst.lazy_local_admission_waves += src.lazy_local_admission_waves;
  dst.lazy_local_parallel_waves += src.lazy_local_parallel_waves;
  dst.lazy_local_memory_limited_waves += src.lazy_local_memory_limited_waves;
  dst.lazy_local_admitted_concurrency_max =
      std::max(dst.lazy_local_admitted_concurrency_max,
               src.lazy_local_admitted_concurrency_max);
  dst.lazy_local_prepared_tasks += src.lazy_local_prepared_tasks;
  dst.lazy_local_reused_prepared_tasks += src.lazy_local_reused_prepared_tasks;
  dst.lazy_local_pre_submit_budget_failures +=
      src.lazy_local_pre_submit_budget_failures;
  dst.lazy_local_peak_admitted_bytes = std::max(
      dst.lazy_local_peak_admitted_bytes, src.lazy_local_peak_admitted_bytes);
  dst.lazy_local_peak_projected_resident_bytes =
      std::max(dst.lazy_local_peak_projected_resident_bytes,
               src.lazy_local_peak_projected_resident_bytes);
  dst.lazy_local_preparation_peak_bytes =
      std::max(dst.lazy_local_preparation_peak_bytes,
               src.lazy_local_preparation_peak_bytes);
  dst.lazy_local_result_output_resident_bytes_max = std::max(
      dst.lazy_local_result_output_resident_bytes_max,
      src.lazy_local_result_output_resident_bytes_max);
  dst.lazy_local_retained_exact_trim_bytes_max = std::max(
      dst.lazy_local_retained_exact_trim_bytes_max,
      src.lazy_local_retained_exact_trim_bytes_max);
  dst.lazy_local_canonical_exact_evidence_resident_bytes_max = std::max(
      dst.lazy_local_canonical_exact_evidence_resident_bytes_max,
      src.lazy_local_canonical_exact_evidence_resident_bytes_max);
  dst.lazy_local_canonical_exact_evidence_construction_peak_bytes_max =
      std::max(
          dst.lazy_local_canonical_exact_evidence_construction_peak_bytes_max,
          src.lazy_local_canonical_exact_evidence_construction_peak_bytes_max);
  dst.lazy_local_runtime_transient_reservation_bytes_max = std::max(
      dst.lazy_local_runtime_transient_reservation_bytes_max,
      src.lazy_local_runtime_transient_reservation_bytes_max);
  dst.lazy_local_iteration_envelope_bytes_max =
      std::max(dst.lazy_local_iteration_envelope_bytes_max,
               src.lazy_local_iteration_envelope_bytes_max);
  dst.lazy_local_iteration_generation_phase_bytes_max =
      std::max(dst.lazy_local_iteration_generation_phase_bytes_max,
               src.lazy_local_iteration_generation_phase_bytes_max);
  dst.lazy_local_iteration_evidence_phase_bytes_max =
      std::max(dst.lazy_local_iteration_evidence_phase_bytes_max,
               src.lazy_local_iteration_evidence_phase_bytes_max);
  dst.lazy_local_ranked_candidate_exact_evidence_bytes_max =
      std::max(dst.lazy_local_ranked_candidate_exact_evidence_bytes_max,
               src.lazy_local_ranked_candidate_exact_evidence_bytes_max);
  dst.lazy_local_iteration_task_stable_bytes_max =
      std::max(dst.lazy_local_iteration_task_stable_bytes_max,
               src.lazy_local_iteration_task_stable_bytes_max);
  dst.lazy_local_iteration_task_preparation_peak_bytes_max =
      std::max(dst.lazy_local_iteration_task_preparation_peak_bytes_max,
               src.lazy_local_iteration_task_preparation_peak_bytes_max);
  dst.candidate_batches_scored += src.candidate_batches_scored;
  dst.pattern_batch_cache_builds += src.pattern_batch_cache_builds;
  dst.initial_state_inside_charts_built +=
      src.initial_state_inside_charts_built;
  dst.inside_cache_inside_charts_built += src.inside_cache_inside_charts_built;
  dst.inside_cache_resident_inside_charts_consumed +=
      src.inside_cache_resident_inside_charts_consumed;
  dst.exact_setup_builds += src.exact_setup_builds;
  dst.exact_setup_inside_charts_built +=
      src.exact_setup_inside_charts_built;
  dst.exact_setup_resident_inside_charts_consumed +=
      src.exact_setup_resident_inside_charts_consumed;
  dst.exact_setup_active_leaf_state_vectors_copied +=
      src.exact_setup_active_leaf_state_vectors_copied;
  dst.exact_setup_active_leaf_states_copied +=
      src.exact_setup_active_leaf_states_copied;
  dst.exact_setup_outside_boundary_charts_built +=
      src.exact_setup_outside_boundary_charts_built;
  dst.exact_setup_upper_bound_topologies_generated +=
      src.exact_setup_upper_bound_topologies_generated;
  dst.exact_setup_upper_bound_topologies_unique +=
      src.exact_setup_upper_bound_topologies_unique;
  dst.exact_setup_frontier_passes += src.exact_setup_frontier_passes;
  dst.exact_bnb_levels += src.exact_bnb_levels;
  dst.exact_bnb_clades += src.exact_bnb_clades;
  dst.exact_bnb_product_combinations += src.exact_bnb_product_combinations;
  dst.exact_bnb_frontier_entries += src.exact_bnb_frontier_entries;
  dst.exact_bnb_ms += src.exact_bnb_ms;
  dst.exact_trim_lazy_chart_uses += src.exact_trim_lazy_chart_uses;
  dst.outside_cache_inside_charts_built +=
      src.outside_cache_inside_charts_built;
  dst.outside_cache_inside_charts_reused +=
      src.outside_cache_inside_charts_reused;
  dst.outside_cache_outside_charts_built +=
      src.outside_cache_outside_charts_built;
  dst.exact_verifications += src.exact_verifications;
  dst.accepted_exact_trims_reused += src.accepted_exact_trims_reused;
  dst.accepted_exact_trim_reuse_rejections +=
      src.accepted_exact_trim_reuse_rejections;
  dst.exact_candidate_admission_batches +=
      src.exact_candidate_admission_batches;
  dst.exact_candidate_parallel_batches += src.exact_candidate_parallel_batches;
  dst.exact_candidate_inner_parallel_batches +=
      src.exact_candidate_inner_parallel_batches;
  dst.exact_candidate_memory_limited_batches +=
      src.exact_candidate_memory_limited_batches;
  dst.exact_candidate_peak_admitted_bytes =
      std::max(dst.exact_candidate_peak_admitted_bytes,
               src.exact_candidate_peak_admitted_bytes);
  dst.exact_candidate_peak_projected_resident_bytes =
      std::max(dst.exact_candidate_peak_projected_resident_bytes,
               src.exact_candidate_peak_projected_resident_bytes);
  dst.exact_candidate_queued_for_memory_ms +=
      src.exact_candidate_queued_for_memory_ms;
  dst.accepted_moves += src.accepted_moves;
  dst.candidate_accepts_attempted += src.candidate_accepts_attempted;
  dst.rejected_moves += src.rejected_moves;
  dst.post_materialization_rejections += src.post_materialization_rejections;
  if (src.skipped_invariant_sites != 0) {
    dst.skipped_invariant_sites = src.skipped_invariant_sites;
  }
  dst.candidate_source_productions_considered +=
      src.candidate_source_productions_considered;
  dst.upward_path_iterator_steps += src.upward_path_iterator_steps;
  dst.upward_paths_completed += src.upward_paths_completed;
  dst.path_pairs_considered += src.path_pairs_considered;
  dst.candidates_constructed += src.candidates_constructed;
  dst.candidates_pruned_before_construction +=
      src.candidates_pruned_before_construction;
  dst.candidates_pruned_after_construction +=
      src.candidates_pruned_after_construction;
  dst.candidates_generated_after_dedup +=
      src.candidates_generated_after_dedup;
  dst.candidates_pruned_root_or_trivial +=
      src.candidates_pruned_root_or_trivial;
  dst.candidates_pruned_moved_size += src.candidates_pruned_moved_size;
  dst.candidates_pruned_target_size += src.candidates_pruned_target_size;
  dst.candidates_pruned_overlap += src.candidates_pruned_overlap;
  dst.candidates_pruned_affected_estimate +=
      src.candidates_pruned_affected_estimate;
  dst.candidates_pruned_immediate_reversal +=
      src.candidates_pruned_immediate_reversal;
  dst.candidates_pruned_duplicate += src.candidates_pruned_duplicate;
  dst.candidates_pruned_invalid += src.candidates_pruned_invalid;
  dst.spr_multifurcation_moves_generated +=
      src.spr_multifurcation_moves_generated;
  dst.candidate_cap_cutoffs += src.candidate_cap_cutoffs;
  dst.path_budget_cutoffs += src.path_budget_cutoffs;
  dst.candidate_pipeline_batches_generated +=
      src.candidate_pipeline_batches_generated;
  dst.candidate_pipeline_batches_scored +=
      src.candidate_pipeline_batches_scored;
  dst.candidate_pipeline_serial_overlap_batches +=
      src.candidate_pipeline_serial_overlap_batches;
  dst.candidate_pipeline_scheduler_projection_overlap_batches +=
      src.candidate_pipeline_scheduler_projection_overlap_batches;
  dst.candidate_pipeline_producer_stalls +=
      src.candidate_pipeline_producer_stalls;
  dst.candidate_pipeline_consumer_stalls +=
      src.candidate_pipeline_consumer_stalls;
  dst.candidate_pipeline_producer_stall_nanoseconds +=
      src.candidate_pipeline_producer_stall_nanoseconds;
  dst.candidate_pipeline_consumer_stall_nanoseconds +=
      src.candidate_pipeline_consumer_stall_nanoseconds;
  dst.candidate_pipeline_cancellations += src.candidate_pipeline_cancellations;
  dst.candidate_pipeline_stale_batches_discarded +=
      src.candidate_pipeline_stale_batches_discarded;
  dst.candidate_pipeline_stale_candidates_discarded +=
      src.candidate_pipeline_stale_candidates_discarded;
  dst.candidate_pipeline_state_epoch_rejections +=
      src.candidate_pipeline_state_epoch_rejections;
  dst.candidate_pipeline_generation_errors +=
      src.candidate_pipeline_generation_errors;
  dst.candidate_pipeline_estimated_peak_bytes =
      std::max(dst.candidate_pipeline_estimated_peak_bytes,
               src.candidate_pipeline_estimated_peak_bytes);
  dst.overlay_reachability_validations +=
      src.overlay_reachability_validations;
  dst.reachable_clades_traversed += src.reachable_clades_traversed;
  dst.reachable_productions_traversed +=
      src.reachable_productions_traversed;
  dst.reachable_temp_clades_traversed +=
      src.reachable_temp_clades_traversed;
  dst.reachable_temp_productions_traversed +=
      src.reachable_temp_productions_traversed;
  dst.reachability_full_grammar_like_passes +=
      src.reachability_full_grammar_like_passes;
  dst.local_commit_accepted_moves += src.local_commit_accepted_moves;
  dst.local_commit_tombstone_scope_skips +=
      src.local_commit_tombstone_scope_skips;
  dst.inside_rows_recomputed_on_commit +=
      src.inside_rows_recomputed_on_commit;
  dst.outside_rows_recomputed_on_commit +=
      src.outside_rows_recomputed_on_commit;
  dst.lazy_inside_rows_recomputed_on_commit +=
      src.lazy_inside_rows_recomputed_on_commit;
  dst.lazy_outside_rows_recomputed_on_commit +=
      src.lazy_outside_rows_recomputed_on_commit;
  dst.lazy_incremental_rows_recomputed +=
      src.lazy_incremental_rows_recomputed;
  dst.lazy_inside_rows_computed += src.lazy_inside_rows_computed;
  dst.lazy_outside_rows_computed += src.lazy_outside_rows_computed;
  dst.lazy_patterns_merged_max =
      std::max(dst.lazy_patterns_merged_max, src.lazy_patterns_merged_max);
  dst.lazy_remerge_collisions += src.lazy_remerge_collisions;
  dst.lazy_structural_class_count_max =
      std::max(dst.lazy_structural_class_count_max,
               src.lazy_structural_class_count_max);
  dst.lazy_chart_memory_budget_bytes = std::max(
      dst.lazy_chart_memory_budget_bytes, src.lazy_chart_memory_budget_bytes);
  dst.lazy_chart_inside_max_admitted_slots =
      std::max(dst.lazy_chart_inside_max_admitted_slots,
               src.lazy_chart_inside_max_admitted_slots);
  dst.lazy_chart_outside_max_admitted_slots =
      std::max(dst.lazy_chart_outside_max_admitted_slots,
               src.lazy_chart_outside_max_admitted_slots);
  dst.lazy_chart_inside_admission_waves +=
      src.lazy_chart_inside_admission_waves;
  dst.lazy_chart_outside_admission_waves +=
      src.lazy_chart_outside_admission_waves;
  dst.lazy_chart_inside_memory_limited_levels +=
      src.lazy_chart_inside_memory_limited_levels;
  dst.lazy_chart_outside_memory_limited_levels +=
      src.lazy_chart_outside_memory_limited_levels;
  dst.lazy_chart_inside_reused_slot_waves +=
      src.lazy_chart_inside_reused_slot_waves;
  dst.lazy_chart_outside_reused_slot_waves +=
      src.lazy_chart_outside_reused_slot_waves;
  dst.lazy_chart_inside_workspace_evictions +=
      src.lazy_chart_inside_workspace_evictions;
  dst.lazy_chart_outside_workspace_evictions +=
      src.lazy_chart_outside_workspace_evictions;
  dst.lazy_chart_preflight_peak_bytes = std::max(
      dst.lazy_chart_preflight_peak_bytes, src.lazy_chart_preflight_peak_bytes);
  dst.lazy_chart_actual_peak_bytes = std::max(dst.lazy_chart_actual_peak_bytes,
                                              src.lazy_chart_actual_peak_bytes);
  dst.lazy_chart_pre_submit_rejections += src.lazy_chart_pre_submit_rejections;
  dst.lazy_policy_pilot_runs += src.lazy_policy_pilot_runs;
  dst.lazy_policy_frozen_reuses += src.lazy_policy_frozen_reuses;
  dst.local_commit_two_chart_oracle_runs +=
      src.local_commit_two_chart_oracle_runs;
  dst.local_commit_tip_grammar_refreshes +=
      src.local_commit_tip_grammar_refreshes;
  dst.fixed_topology_selected_cache_hits +=
      src.fixed_topology_selected_cache_hits;
  dst.fixed_topology_selected_cache_misses +=
      src.fixed_topology_selected_cache_misses;
  dst.fixed_topology_selected_rows_computed +=
      src.fixed_topology_selected_rows_computed;
  dst.selected_topology_class_rows_computed +=
      src.selected_topology_class_rows_computed;
  dst.selected_topology_multifurcation_rows +=
      src.selected_topology_multifurcation_rows;
  dst.fixed_topology_persistent_cache_verifications +=
      src.fixed_topology_persistent_cache_verifications;
  dst.fixed_topology_persistent_cache_fallbacks +=
      src.fixed_topology_persistent_cache_fallbacks;
  dst.fixed_topology_persistent_cache_oracle_mismatches +=
      src.fixed_topology_persistent_cache_oracle_mismatches;
  dst.fixed_topology_persistent_cache_direct_oracle_mismatches +=
      src.fixed_topology_persistent_cache_direct_oracle_mismatches;
  dst.fixed_topology_icache_rows_reused += src.fixed_topology_icache_rows_reused;
  dst.fixed_topology_icache_rows_recomputed_affected +=
      src.fixed_topology_icache_rows_recomputed_affected;
  dst.fixed_topology_chain_objective_before_mismatches +=
      src.fixed_topology_chain_objective_before_mismatches;
  dst.fixed_topology_independent_sm_bug_witnesses_for_tests +=
      src.fixed_topology_independent_sm_bug_witnesses_for_tests;
  dst.fixed_topology_independent_sm_bug_perturbations_for_tests +=
      src.fixed_topology_independent_sm_bug_perturbations_for_tests;
  dst.transient_chain_extensions_for_verification +=
      src.transient_chain_extensions_for_verification;
  dst.transient_chain_diagnostic_cache_extensions +=
      src.transient_chain_diagnostic_cache_extensions;
  dst.transient_chain_extension_fallbacks +=
      src.transient_chain_extension_fallbacks;
  dst.transient_chain_extension_oracle_mismatches +=
      src.transient_chain_extension_oracle_mismatches;
  dst.transient_chain_extension_oracle_rows_checked_for_tests +=
      src.transient_chain_extension_oracle_rows_checked_for_tests;
}

// Candidate-owner-neutral Phase-3 output.  In particular, this deliberately
// excludes the grammar_spr_candidate and every Phase-4/5 attachment carried by
// chart_spr_candidate_score.  Callers that need the owning form promote the
// input candidate only after the local-scoring operation has completed.
struct chart_spr_local_score_result {
  chart_spr_objective_score lower_bound;
  std::size_t affected_clade_count = 0;
  double local_score_ms = 0.0;
  bool valid = true;
  std::string invalid_reason;
};

struct lazy_overlay_context_accumulator {
  std::size_t representative = no_site_pattern;
  std::uint64_t weight = 0;
  std::array<std::uint64_t, nuc_state_count> reference_state_counts{};
};

struct chart_spr_local_score_scratch {
  local_overlay_chart_rows rows;
  std::vector<lazy_key_grouping_detail::packed_key_word> lazy_context_key_words;
  lazy_key_grouping_detail::packed_key_grouping_workspace
      lazy_context_grouping_workspace;
  lazy_key_grouping_detail::packed_key_grouping_result
      lazy_context_grouping_result;
  std::vector<lazy_overlay_context_accumulator> lazy_contexts;
  std::size_t lazy_prepared_key_width = 0;
  std::size_t lazy_prepared_grouping_resident_bytes = 0;
  lazy_key_grouping_detail::packed_key_grouping_prepared_status
      lazy_grouping_status;
  chart_spr_search_detail::lazy_local_worker_failure_kind lazy_worker_failure =
      chart_spr_search_detail::lazy_local_worker_failure_kind::none;

  void stage_lazy_worker_failure(
      chart_spr_search_detail::lazy_local_worker_failure_kind kind) noexcept {
    if (lazy_worker_failure ==
        chart_spr_search_detail::lazy_local_worker_failure_kind::none) {
      lazy_worker_failure = kind;
    }
  }

  [[nodiscard]] bool has_lazy_worker_failure() const noexcept {
    return lazy_worker_failure !=
           chart_spr_search_detail::lazy_local_worker_failure_kind::none;
  }

  void clear_borrows() noexcept {
    rows.base_row_slot = {};
    rows.temp_row_slot = {};
    lazy_context_key_words.clear();
    lazy_context_grouping_workspace.clear_sizes();
    lazy_context_grouping_result.clear_sizes();
    lazy_contexts.clear();
    lazy_prepared_key_width = 0;
    lazy_prepared_grouping_resident_bytes = 0;
    lazy_grouping_status = {};
    lazy_worker_failure =
        chart_spr_search_detail::lazy_local_worker_failure_kind::none;
  }

  void release_retained_storage() noexcept {
    clear_borrows();
    std::vector<std::array<chart_cost, nuc_state_count>>{}.swap(rows.rows);
    std::vector<lazy_key_grouping_detail::packed_key_word>{}.swap(
        lazy_context_key_words);
    lazy_context_grouping_workspace.release();
    lazy_context_grouping_result = {};
    std::vector<lazy_overlay_context_accumulator>{}.swap(lazy_contexts);
  }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return rows.base_row_slot.empty() && rows.temp_row_slot.empty() &&
           lazy_context_key_words.empty() && lazy_contexts.empty() &&
           lazy_prepared_key_width == 0 &&
           lazy_prepared_grouping_resident_bytes == 0 &&
           lazy_worker_failure ==
               chart_spr_search_detail::lazy_local_worker_failure_kind::none &&
           lazy_grouping_status.succeeded() &&
           lazy_context_grouping_workspace.sizes_empty() &&
           lazy_context_grouping_result.class_by_input.empty() &&
           lazy_context_grouping_result.representative_by_class.empty() &&
           lazy_context_grouping_result.member_offsets_by_class.empty() &&
           lazy_context_grouping_result.members_by_class.empty() &&
           lazy_context_grouping_result.lexicographic_class_order.empty();
  }
};

namespace chart_spr_search_detail {

inline std::size_t local_score_scratch_dynamic_capacity_bytes(
    chart_spr_local_score_scratch const& scratch) {
  std::size_t total = local_vector_dynamic_capacity_bytes(scratch.rows.rows);
  total = local_capacity_checked_add(
      total,
      lazy_key_grouping_detail::packed_key_word_buffer_dynamic_capacity_bytes(
          scratch.lazy_context_key_words),
      "chart SPR lazy-local scratch key capacity");
  total = local_capacity_checked_add(
      total, scratch.lazy_context_grouping_workspace.dynamic_capacity_bytes(),
      "chart SPR lazy-local scratch grouping workspace");
  total = local_capacity_checked_add(
      total, scratch.lazy_context_grouping_result.dynamic_capacity_bytes(),
      "chart SPR lazy-local scratch grouping result");
  total = local_capacity_checked_add(
      total, local_vector_dynamic_capacity_bytes(scratch.lazy_contexts),
      "chart SPR lazy-local scratch contexts");
  return total;
}

// Published candidate-local execution descriptor.  Construction consumes the
// mutable build aggregate, clears its now-unneeded borrowed base pointer, and
// exposes only a const view thereafter.  Raw overlay payload and compiled rows
// therefore cannot diverge while pattern workers share the descriptor.
class compiled_spr_candidate_execution {
 public:
  compiled_spr_candidate_execution() = default;

  void build_and_publish(
      clade_grammar const& base,
      checked_chart_execution_plan_ref const& checked_base,
      grammar_spr_candidate const& candidate,
      local_spr_score_options const& options,
      candidate_chart_execution_plan_build_stats* build_stats) {
    release_operation_borrows();
    try {
      build_spr_overlay_delta_from_resident_plan_into(
          base, checked_base, candidate, descriptor_, build_scratch_, options,
          build_stats);
    } catch (...) {
      descriptor_.base = nullptr;
      published_ = false;
      throw;
    }
    descriptor_.base = nullptr;
    published_ = true;
  }

  [[nodiscard]] spr_overlay_delta const& descriptor() const noexcept {
    return descriptor_;
  }

  void release_operation_borrows() noexcept {
    // The descriptor is self-contained after publication.  Retain its owning
    // high-water storage for the forthcoming in-place builder, but make it
    // impossible for a later operation to consume stale prepared state.
    descriptor_.base = nullptr;
    build_scratch_.clear_operation_borrows();
    published_ = false;
  }

  void release_retained_storage() noexcept {
    release_operation_borrows();
    descriptor_ = {};
    build_scratch_ = {};
  }

  [[nodiscard]] bool published() const noexcept { return published_; }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return descriptor_.base == nullptr && !published_ &&
           build_scratch_.operation_boundary_clean();
  }

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    return local_capacity_checked_add(
        local_overlay_delta_dynamic_capacity_bytes(descriptor_),
        local_overlay_build_scratch_dynamic_capacity_bytes(build_scratch_),
        "chart SPR lazy-local compiled execution capacity");
  }

 private:
  spr_overlay_delta descriptor_;
  spr_overlay_delta_build_scratch build_scratch_;
  bool published_ = false;
};

class prepared_local_candidate_score {
 public:
  std::optional<overlay_materialization_result> verification_materialized;
  std::uint64_t new_active_score = 0;
  chart_spr_local_score_result result;
  bool valid_for_accumulation = false;

  [[nodiscard]] spr_overlay_delta const& delta() const {
    if (!execution.published()) {
      throw std::runtime_error(
          "chart SPR local score: missing compiled candidate execution");
    }
    return execution.descriptor();
  }

  void reset_for_prepare() noexcept {
    verification_materialized.reset();
    new_active_score = 0;
    result.lower_bound = {};
    result.affected_clade_count = 0;
    result.local_score_ms = 0.0;
    result.valid = true;
    result.invalid_reason.clear();
    valid_for_accumulation = false;
    execution.release_operation_borrows();
  }

  void reserve_finite_invalid_reason() {
    reserve_finite_local_score_invalid_reason(result.invalid_reason);
  }

  void configure_finite_invalid_reason(bool finite) {
    finite_invalid_reason_ = finite;
    if (finite) reserve_finite_invalid_reason();
  }

  [[nodiscard]] bool finite_invalid_reason() const noexcept {
    return finite_invalid_reason_;
  }

  void release_operation_borrows() noexcept {
    verification_materialized.reset();
    valid_for_accumulation = false;
    execution.release_operation_borrows();
  }

  void release_retained_storage() noexcept {
    verification_materialized.reset();
    std::string{}.swap(result.invalid_reason);
    execution.release_retained_storage();
    reset_for_prepare();
  }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return !valid_for_accumulation && execution.operation_boundary_clean();
  }

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    auto total = execution.dynamic_capacity_bytes();
    total = local_capacity_checked_add(
        total, local_owned_dynamic_capacity_bytes(result.invalid_reason),
        "chart SPR lazy-local prepared result capacity");
    if (verification_materialized) {
      total = local_capacity_checked_add(
          total, local_owned_dynamic_capacity_bytes(*verification_materialized),
          "chart SPR lazy-local verification overlay capacity");
    }
    return total;
  }

 private:
  compiled_spr_candidate_execution execution;
  bool finite_invalid_reason_ = false;

  void build_and_publish_execution(
      clade_grammar const& base,
      checked_chart_execution_plan_ref const& checked_base,
      grammar_spr_candidate const& candidate,
      local_spr_score_options const& options,
      candidate_chart_execution_plan_build_stats* build_stats) {
    execution.build_and_publish(base, checked_base, candidate, options,
                                build_stats);
  }

  friend void prepare_local_candidate_score_into(
      chart_spr_search_state const& state,
      grammar_spr_candidate const& candidate,
      local_spr_score_options const& options,
      chart_spr_search_counters* counters,
      checked_chart_execution_plan_ref const& checked_state,
      prepared_local_candidate_score& prepared);
};

struct local_score_worker_workspace {
  chart_spr_local_score_scratch scratch;
  chart_spr_search_counters counters;

  void reset_for_operation() noexcept {
    scratch.clear_borrows();
    counters = {};
  }

  void release_retained_storage() noexcept {
    scratch.release_retained_storage();
    counters = {};
  }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return scratch.operation_boundary_clean();
  }

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    return local_score_scratch_dynamic_capacity_bytes(scratch);
  }
};

// One deterministic candidate x pattern tile.  Workers publish only to the
// tile's stable slot; the coordinator folds slots in candidate-major,
// increasing-pattern order after the scheduler joins.  Candidate-level input
// failures remain ordinary invalid candidates rather than cancelling unrelated
// tiles.
struct local_score_tile_result {
  std::uint64_t active_score = 0;
  bool valid = true;
  std::string invalid_reason;

  void reset_for_operation() noexcept {
    active_score = 0;
    valid = true;
    invalid_reason.clear();
  }

  void configure_finite_invalid_reason(bool finite) {
    if (finite) reserve_finite_local_score_invalid_reason(invalid_reason);
  }
};

struct local_score_workspace_access;

}  // namespace chart_spr_search_detail

// Caller-owned high-water storage for Phase-3 scoring.  A workspace is
// intentionally single-operation-at-a-time. Resident-cache scoring retains
// one prepared descriptor per effective worker; pattern-batch scoring retains
// one per candidate because descriptors cross pattern batches. Every parallel
// worker receives disjoint row scratch. No grammar, plan, candidate, or output
// borrow survives a public operation boundary.
class chart_spr_local_score_workspace {
 public:
  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    if (operation_active_ || prepared_active_ != 0 || worker_active_ != 0 ||
        tile_active_ != 0 || !serial_scratch_.operation_boundary_clean()) {
      return false;
    }
    for (auto const& prepared : prepared_) {
      if (!prepared.operation_boundary_clean()) return false;
    }
    for (auto const& worker : workers_) {
      if (!worker.operation_boundary_clean()) return false;
    }
    return true;
  }

  void release_retained_storage() {
    if (!operation_boundary_clean()) {
      throw std::logic_error(
          "chart SPR local score workspace: cannot release active storage");
    }
    std::vector<chart_spr_search_detail::prepared_local_candidate_score>{}
        .swap(prepared_);
    std::vector<chart_spr_search_detail::local_score_worker_workspace>{}.swap(
        workers_);
    std::vector<chart_spr_search_detail::local_score_tile_result>{}.swap(
        tile_results_);
    serial_scratch_ = {};
  }

 private:
  friend struct chart_spr_search_detail::local_score_workspace_access;

  std::vector<chart_spr_search_detail::prepared_local_candidate_score>
      prepared_;
  std::vector<chart_spr_search_detail::local_score_worker_workspace> workers_;
  std::vector<chart_spr_search_detail::local_score_tile_result> tile_results_;
  chart_spr_local_score_scratch serial_scratch_;
  std::size_t prepared_active_ = 0;
  std::size_t worker_active_ = 0;
  std::size_t tile_active_ = 0;
  bool operation_active_ = false;
};

namespace chart_spr_search_detail {

class chart_spr_lazy_local_budget_error : public std::exception {
 public:
  chart_spr_lazy_local_budget_error(std::size_t candidate_index,
                                    std::size_t required_bytes,
                                    std::size_t available_bytes) noexcept
      : candidate_index_(candidate_index),
        required_bytes_(required_bytes),
        available_bytes_(available_bytes) {}

  [[nodiscard]] char const* what() const noexcept override {
    return "chart SPR lazy-local admission budget exceeded";
  }

  [[nodiscard]] std::size_t candidate_index() const noexcept {
    return candidate_index_;
  }
  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }
  [[nodiscard]] std::size_t available_bytes() const noexcept {
    return available_bytes_;
  }

 private:
  std::size_t candidate_index_;
  std::size_t required_bytes_;
  std::size_t available_bytes_;
};

// Finite lazy-local admission currently has a complete live-storage envelope
// only for the grammar-native streaming enumerator.  Reject the other source
// implementations before they allocate source-specific state rather than
// silently applying the grammar envelope to a different ownership graph.
class chart_spr_lazy_local_enumeration_budget_error : public std::exception {
 public:
  explicit chart_spr_lazy_local_enumeration_budget_error(
      chart_spr_candidate_source source) noexcept
      : source_(source) {}

  [[nodiscard]] char const* what() const noexcept override {
    return "chart SPR finite lazy-local budget does not support this "
           "candidate source or enumeration mode";
  }

  [[nodiscard]] chart_spr_candidate_source source() const noexcept {
    return source_;
  }

 private:
  chart_spr_candidate_source source_;
};

class chart_spr_canonical_exact_evidence_budget_error
    : public std::exception {
 public:
  [[nodiscard]] char const* what() const noexcept override {
    return "chart SPR finite canonical exact evidence requires primary root "
           "provenance capture";
  }
};

enum class chart_spr_lazy_local_finite_api : std::uint8_t {
  unchecked_scheduler_into,
  unchecked_worker_count_into,
  checked_worker_count_parallel_into,
  checked_owning,
  unchecked_owning,
  unchecked_singleton,
};

// Finite lazy-local admission is deliberately restricted to the production
// path and the checked caller-owned `_into` seam with a caller-owned
// scheduler. Compatibility wrappers that allocate a scheduler, result vector,
// promotion, or plan-fingerprint scratch outside that seam fail before doing
// so until they own a complete outer envelope.
class chart_spr_lazy_local_finite_api_error : public std::exception {
 public:
  explicit chart_spr_lazy_local_finite_api_error(
      chart_spr_lazy_local_finite_api api) noexcept
      : api_(api) {}

  [[nodiscard]] char const* what() const noexcept override {
    return "chart SPR finite lazy-local admission requires checked caller-owned "
           "into storage and scheduler";
  }

  [[nodiscard]] chart_spr_lazy_local_finite_api api() const noexcept {
    return api_;
  }

 private:
  chart_spr_lazy_local_finite_api api_;
};

struct lazy_local_admission_wave {
  std::size_t admitted_count = 0;
  std::size_t admitted_bytes = 0;
  bool memory_limited = false;
};

// Pure maximal stable-prefix planner. The runtime can feed measured deep
// capacities one at a time without allocating another estimates buffer; tests
// exercise the same function with a complete wave to pin prefix and failure
// semantics.
inline lazy_local_admission_wave plan_lazy_local_admission_wave(
    std::span<std::size_t const> task_bytes, std::size_t begin_candidate,
    std::size_t task_limit, std::size_t available_bytes, bool enforce_budget) {
  task_limit = std::min(task_limit, task_bytes.size());
  lazy_local_admission_wave wave;
  for (; wave.admitted_count < task_limit; ++wave.admitted_count) {
    auto const required = task_bytes[wave.admitted_count];
    if (enforce_budget && required > available_bytes - wave.admitted_bytes) {
      if (wave.admitted_count == 0) {
        throw chart_spr_lazy_local_budget_error(begin_candidate, required,
                                                available_bytes);
      }
      wave.memory_limited = true;
      break;
    }
    wave.admitted_bytes = local_capacity_checked_add(
        wave.admitted_bytes, required, "chart SPR lazy-local admission prefix");
  }
  return wave;
}

struct local_score_workspace_access {
  static void bound_lazy_task_slots(chart_spr_local_score_workspace& workspace,
                                    std::size_t task_count,
                                    bool release_all_retained_storage) {
    if (!workspace.operation_boundary_clean()) {
      throw std::logic_error(
          "chart SPR lazy-local workspace: cannot bound active task slots");
    }
    auto hard_bound = [task_count, release_all_retained_storage](auto& slots) {
      // A finite-budget operation needs the exact outer size before begin(),
      // both to avoid a hidden cold-workspace resize and to make the
      // old+replacement overlap independently preflightable. Unlimited runs
      // preserve the historical grow-on-begin behavior.
      if (release_all_retained_storage
              ? (slots.size() == task_count && slots.capacity() <= task_count)
              : (slots.size() <= task_count &&
                 slots.capacity() <= task_count)) {
        return;
      }
      std::remove_cvref_t<decltype(slots)> replacement;
      replacement.resize(task_count);
      slots.swap(replacement);
    };
    hard_bound(workspace.prepared_);
    hard_bound(workspace.workers_);
    // Lazy candidate waves never publish candidate x pattern tiles.
    std::vector<local_score_tile_result>{}.swap(workspace.tile_results_);
    if (release_all_retained_storage) {
      for (auto& prepared : workspace.prepared_) {
        prepared.release_retained_storage();
      }
      for (auto& worker : workspace.workers_) {
        worker.release_retained_storage();
      }
      workspace.serial_scratch_.release_retained_storage();
    }
  }

  static void begin(chart_spr_local_score_workspace& workspace,
                    std::size_t prepared_count, std::size_t worker_count,
                    std::size_t tile_count = 0,
                    bool finite_invalid_reasons = false) {
    if (!workspace.operation_boundary_clean()) {
      throw std::logic_error(
          "chart SPR local score workspace: overlapping operation or stale "
          "borrow");
    }
    // Grow before publishing an active operation so allocation failure leaves
    // the caller-visible boundary clean.
    if (workspace.prepared_.size() < prepared_count) {
      workspace.prepared_.resize(prepared_count);
    }
    if (workspace.workers_.size() < worker_count) {
      workspace.workers_.resize(worker_count);
    }
    if (workspace.tile_results_.size() < tile_count) {
      workspace.tile_results_.resize(tile_count);
    }
    // Reserve every worker-published diagnostic before the operation becomes
    // active and, critically, before any scheduler submission.  A retained
    // oversized capacity from an earlier unlimited call fails closed here;
    // the finite iteration's outer actual-capacity preflight normally rejects
    // that reuse even earlier.
    for (std::size_t i = 0; i < prepared_count; ++i) {
      workspace.prepared_[i].configure_finite_invalid_reason(
          finite_invalid_reasons);
    }
    for (std::size_t i = 0; i < tile_count; ++i) {
      workspace.tile_results_[i].configure_finite_invalid_reason(
          finite_invalid_reasons);
    }
    workspace.prepared_active_ = prepared_count;
    workspace.worker_active_ = worker_count;
    workspace.tile_active_ = tile_count;
    workspace.operation_active_ = true;
    workspace.serial_scratch_.clear_borrows();
    for (std::size_t i = 0; i < prepared_count; ++i) {
      workspace.prepared_[i].reset_for_prepare();
    }
    for (std::size_t i = 0; i < worker_count; ++i) {
      workspace.workers_[i].reset_for_operation();
    }
    for (std::size_t i = 0; i < tile_count; ++i) {
      workspace.tile_results_[i].reset_for_operation();
    }
  }

  static void finish(chart_spr_local_score_workspace& workspace) noexcept {
    for (std::size_t i = 0; i < workspace.prepared_active_; ++i) {
      workspace.prepared_[i].release_operation_borrows();
    }
    for (std::size_t i = 0; i < workspace.worker_active_; ++i) {
      workspace.workers_[i].reset_for_operation();
    }
    workspace.serial_scratch_.clear_borrows();
    workspace.prepared_active_ = 0;
    workspace.worker_active_ = 0;
    workspace.tile_active_ = 0;
    workspace.operation_active_ = false;
  }

  static prepared_local_candidate_score& prepared(
      chart_spr_local_score_workspace& workspace, std::size_t index) {
    return workspace.prepared_[index];
  }

  static local_score_worker_workspace& worker(
      chart_spr_local_score_workspace& workspace, std::size_t index) {
    return workspace.workers_[index];
  }

  static chart_spr_local_score_scratch& serial_scratch(
      chart_spr_local_score_workspace& workspace) {
    return workspace.serial_scratch_;
  }

  static local_score_tile_result& tile_result(
      chart_spr_local_score_workspace& workspace, std::size_t index) {
    return workspace.tile_results_[index];
  }

  static void release_task_retained_storage(
      chart_spr_local_score_workspace& workspace, std::size_t index) noexcept {
    workspace.prepared_[index].release_retained_storage();
    workspace.workers_[index].release_retained_storage();
  }

  static std::size_t task_dynamic_capacity_bytes(
      chart_spr_local_score_workspace const& workspace, std::size_t index) {
    return local_capacity_checked_add(
        workspace.prepared_[index].dynamic_capacity_bytes(),
        workspace.workers_[index].dynamic_capacity_bytes(),
        "chart SPR lazy-local task capacity");
  }

  static std::size_t fixed_resident_capacity_bytes(
      chart_spr_local_score_workspace const& workspace) {
    std::size_t total = sizeof(workspace);
    total = local_capacity_checked_add(
        total, local_vector_dynamic_capacity_bytes(workspace.prepared_),
        "chart SPR lazy-local workspace prepared slots");
    total = local_capacity_checked_add(
        total, local_vector_dynamic_capacity_bytes(workspace.workers_),
        "chart SPR lazy-local workspace worker slots");
    total = local_capacity_checked_add(
        total, local_vector_dynamic_capacity_bytes(workspace.tile_results_),
        "chart SPR lazy-local workspace tile slots");
    total = local_capacity_checked_add(
        total,
        local_score_scratch_dynamic_capacity_bytes(workspace.serial_scratch_),
        "chart SPR lazy-local workspace serial scratch");
    return total;
  }

  static std::size_t retained_task_dynamic_capacity_bytes(
      chart_spr_local_score_workspace const& workspace) {
    std::size_t total = 0;
    for (auto const& prepared : workspace.prepared_) {
      total = local_capacity_checked_add(
          total, prepared.dynamic_capacity_bytes(),
          "chart SPR lazy-local retained prepared capacity");
    }
    for (auto const& worker : workspace.workers_) {
      total = local_capacity_checked_add(
          total, worker.dynamic_capacity_bytes(),
          "chart SPR lazy-local retained worker capacity");
    }
    return total;
  }

  static std::size_t full_resident_capacity_bytes(
      chart_spr_local_score_workspace const& workspace) {
    return local_capacity_checked_add(
        fixed_resident_capacity_bytes(workspace),
        retained_task_dynamic_capacity_bytes(workspace),
        "chart SPR lazy-local full workspace capacity");
  }

  // Allocation-free frozen-toolchain envelope for the exact-size replacements
  // performed by finite bound_lazy_task_slots(). Both replacement buffers are
  // charged together, which conservatively dominates their sequential
  // old+new overlap and the following no-growth begin().
  static std::size_t finite_outer_bound_peak_capacity_bytes(
      chart_spr_local_score_workspace const& workspace,
      std::size_t task_count) {
    auto total = full_resident_capacity_bytes(workspace);
    auto const prepared_replaced = workspace.prepared_.size() != task_count ||
                                   workspace.prepared_.capacity() > task_count;
    auto const workers_replaced = workspace.workers_.size() != task_count ||
                                  workspace.workers_.capacity() > task_count;
    if (prepared_replaced) {
      total = local_capacity_checked_add(
          total,
          local_capacity_checked_multiply(
              task_count, sizeof(prepared_local_candidate_score),
              "chart SPR lazy-local prepared replacement"),
          "chart SPR lazy-local prepared replacement overlap");
      prepared_local_candidate_score empty_prepared;
      total = local_capacity_checked_add(
          total,
          local_capacity_checked_multiply(
              task_count, empty_prepared.dynamic_capacity_bytes(),
              "chart SPR lazy-local prepared replacement defaults"),
          "chart SPR lazy-local prepared replacement default overlap");
    }
    if (workers_replaced) {
      total = local_capacity_checked_add(
          total,
          local_capacity_checked_multiply(
              task_count, sizeof(local_score_worker_workspace),
              "chart SPR lazy-local worker replacement"),
          "chart SPR lazy-local worker replacement overlap");
      local_score_worker_workspace empty_worker;
      total = local_capacity_checked_add(
          total,
          local_capacity_checked_multiply(
              task_count, empty_worker.dynamic_capacity_bytes(),
              "chart SPR lazy-local worker replacement defaults"),
          "chart SPR lazy-local worker replacement default overlap");
    }
    return total;
  }

  static std::pair<std::size_t, std::size_t> task_slot_capacities(
      chart_spr_local_score_workspace const& workspace) noexcept {
    return {workspace.prepared_.capacity(), workspace.workers_.capacity()};
  }
};

inline void set_invalid_local_score_result(chart_spr_search_state const& state,
                                           chart_spr_local_score_result& result,
                                           std::string_view reason) {
  auto old_score = state.composite_lower_bound_with_invariants;
  result.lower_bound.value = spr_score_result{0, old_score, old_score, false};
  result.lower_bound.kind = chart_spr_score_kind::composite_lower_bound;
  result.lower_bound.convention =
      chart_spr_score_convention::full_with_invariants;
  result.lower_bound.invariant_offset_applied = state.invariant_constant_offset;
  result.affected_clade_count = 0;
  result.valid = false;
  result.invalid_reason.assign(reason);
}

inline chart_spr_local_score_result make_invalid_local_score_result(
    chart_spr_search_state const& state, std::string_view reason) {
  chart_spr_local_score_result result;
  set_invalid_local_score_result(state, result, reason);
  return result;
}

inline void prepare_local_candidate_score_into(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    local_spr_score_options const& options, chart_spr_search_counters* counters,
    checked_chart_execution_plan_ref const& checked_state,
    prepared_local_candidate_score& prepared) {
  if (options.exact_multisite) {
    throw std::runtime_error(
        "chart SPR local score: exact_multisite belongs to the Phase-4 "
        "verification gate, not the Phase-3 composite local scorer");
  }

  if (counters != nullptr) {
    ++counters->local_candidate_scores;
  }

  prepared.configure_finite_invalid_reason(
      use_finite_local_score_diagnostics(state, options));
  prepared.reset_for_prepare();
  candidate_chart_execution_plan_build_stats candidate_plan_stats;
  auto record_candidate_plan_stats = [&] {
    if (counters == nullptr) return;
    counters->candidate_partition_validations +=
        candidate_plan_stats.candidate_partition_validations;
    counters->clade_order_sorts += candidate_plan_stats.clade_order_sorts;
    counters->production_descriptors_compiled +=
        candidate_plan_stats.production_descriptors_compiled;
  };
  auto invalidate = [&](std::string_view reason) {
    if (options.admission_memory_budget_bytes != 0 ||
        state.cache_opts.memory_budget_bytes != 0) {
      reason = bounded_lazy_local_invalid_reason(reason);
    }
    set_invalid_local_score_result(state, prepared.result, reason);
    prepared.valid_for_accumulation = false;
  };
  try {
    checked_state.assert_same(state.grammar, state.execution_plan);
    prepared.build_and_publish_execution(state.grammar, checked_state,
                                         candidate, options,
                                         &candidate_plan_stats);
  } catch (chart_execution_plan_mismatch const&) {
    record_candidate_plan_stats();
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    throw;
  } catch (chart_spr_candidate_invalid_error const& error) {
    record_candidate_plan_stats();
    invalidate(error.what());
    return;
  } catch (chart_spr_lazy_local_budget_error const&) {
    record_candidate_plan_stats();
    throw;
  } catch (chart_spr_lazy_local_enumeration_budget_error const&) {
    record_candidate_plan_stats();
    throw;
  } catch (chart_spr_exact_candidate_budget_error const&) {
    record_candidate_plan_stats();
    throw;
  } catch (chart_spr_exact_state_budget_error const&) {
    record_candidate_plan_stats();
    throw;
  } catch (std::bad_alloc const&) {
    record_candidate_plan_stats();
    throw;
  } catch (std::overflow_error const&) {
    record_candidate_plan_stats();
    throw;
  } catch (std::length_error const&) {
    record_candidate_plan_stats();
    throw;
  } catch (std::exception const&) {
    record_candidate_plan_stats();
    throw;
  }
  record_candidate_plan_stats();
  if (counters != nullptr) {
    // These are successful-build/use counters, not attempt counters.  Invalid
    // candidate descriptors and stale resident plans must not masquerade as a
    // completed build or cache hit.
    ++counters->chart_execution_plan_cache_hits;
    ++counters->candidate_execution_plan_builds;
    record_overlay_delta_reachability_counters(
        *counters, prepared.delta().reachability_stats);
  }

  if (options.verify_against_full_overlay) {
    auto verification_overlay = overlay_from_candidate(state.grammar,
                                                       candidate);
    prepared.verification_materialized =
        materialize_overlay_grammar(verification_overlay);
    if (counters != nullptr) {
      ++counters->full_overlay_materializations;
      ++counters->overlay_materializations_for_oracle;
    }
  }

  prepared.valid_for_accumulation = true;
}

inline prepared_local_candidate_score prepare_local_candidate_score(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    local_spr_score_options const& options, chart_spr_search_counters* counters,
    checked_chart_execution_plan_ref const& checked_state) {
  prepared_local_candidate_score prepared;
  prepare_local_candidate_score_into(state, candidate, options, counters,
                                     checked_state, prepared);
  return prepared;
}

// Direct/detail callers retain a checked standalone boundary.  Production
// acceptance batches call the capability overload above so every candidate in
// the immutable epoch shares the same one fingerprint scan.
inline prepared_local_candidate_score prepare_local_candidate_score(
    chart_spr_search_state const& state,
    grammar_spr_candidate const& candidate,
    local_spr_score_options const& options,
    chart_spr_search_counters* counters) {
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    return prepare_local_candidate_score(state, candidate, options, counters,
                                         checked);
  } catch (chart_execution_plan_mismatch const&) {
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    throw;
  }
}

inline void invalidate_prepared_local_candidate(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared, std::string_view reason) {
  if (prepared.finite_invalid_reason()) {
    reason = bounded_lazy_local_invalid_reason(reason);
  }
  set_invalid_local_score_result(state, prepared.result, reason);
  prepared.valid_for_accumulation = false;
}

inline bool validate_prepared_candidate_plan_identity(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    chart_spr_search_counters* counters,
    checked_chart_execution_plan_ref const& checked_state) {
  try {
    checked_state.assert_same(state.grammar, state.execution_plan);
    assert_overlay_delta_execution_plan_compatible(prepared.delta(),
                                                   state.execution_plan);
    return true;
  } catch (std::exception const& e) {
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    invalidate_prepared_local_candidate(state, prepared, e.what());
    return false;
  }
}


inline bool validate_prepared_candidate_plan_identity(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    chart_spr_search_counters* counters) {
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    return validate_prepared_candidate_plan_identity(
        state, prepared, counters, checked);
  } catch (std::exception const& e) {
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    invalidate_prepared_local_candidate(state, prepared, e.what());
    return false;
  }
}

inline void accumulate_prepared_local_candidate_row_view(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared, std::size_t pattern_begin,
    std::size_t pattern_end, local_spr_score_options const& options,
    chart_spr_search_counters* counters, chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state) {
  if (!prepared.valid_for_accumulation) return;
  if (!options.force_dense_invalid_reason_for_tests.empty()) {
    invalidate_prepared_local_candidate(
        state, prepared, options.force_dense_invalid_reason_for_tests);
    return;
  }
  if (!validate_prepared_candidate_plan_identity(state, prepared, counters,
                                                 checked_state)) {
    return;
  }

  auto const& patterns = state.active_patterns.patterns.patterns;
  if (pattern_begin > pattern_end || pattern_end > patterns.size()) {
    invalidate_prepared_local_candidate(
        state, prepared,
        "chart SPR local score: inside-row view pattern range out of bounds");
    return;
  }
  try {
    auto const& delta = prepared.delta();
    parsimony_chart_detail::structural_work_observer hot_work_observer{
        .full_grammar_validations =
            counters != nullptr
                ? &counters->candidate_pattern_full_grammar_validations
                : nullptr,
        .production_partition_validations =
            counters != nullptr
                ? &counters->candidate_pattern_partition_validations
                : nullptr,
        .clade_order_sorts =
            counters != nullptr ? &counters->candidate_pattern_clade_order_sorts
                                : nullptr};
    parsimony_chart_detail::structural_work_observer_scope hot_work_scope{
        counters != nullptr ? &hot_work_observer : nullptr};
    auto chart_build_options = state.chart_opts;
    chart_build_options.keep_trace = false;
    chart_build_options.max_trace_choices = 0;
    for (std::size_t pattern_index = pattern_begin; pattern_index < pattern_end;
         ++pattern_index) {
      if (counters != nullptr) {
        ++counters->candidate_execution_plan_cache_hits;
        ++counters->local_leaf_state_view_uses;
        ++counters->local_commit_inside_row_view_pattern_visits;
      }
      auto const& pattern = patterns[pattern_index];
      auto states = view_leaf_site_states(pattern.state_by_taxon);
      build_local_overlay_chart_rows_into(
          state.grammar, delta, state.local_commit_inside_rows, pattern_index,
          states, scratch.rows, chart_build_options, counters);
      if (counters != nullptr) {
        counters->local_rows_recomputed += delta.affected_order.size();
      }
      if (prepared.verification_materialized) {
        verify_local_overlay_rows_against_full(
            delta, scratch.rows, state.local_commit_inside_rows, pattern_index,
            *prepared.verification_materialized, states, state.chart_opts);
      }
      auto const& root_row =
          local_overlay_chart_row(scratch.rows, state.local_commit_inside_rows,
                                  pattern_index, delta.root);
      prepared.new_active_score = chart_multisite_detail::checked_add_u64(
          prepared.new_active_score,
          chart_spr_weighted_root_score_from_row(root_row, pattern,
                                                 state.chart_opts),
          "chart-SPR local candidate active lower bound");
    }
  } catch (std::exception const& e) {
    invalidate_prepared_local_candidate(state, prepared, e.what());
  }
}

inline void accumulate_prepared_local_candidate_patterns(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    std::size_t pattern_offset,
    std::vector<pattern_chart_cache_entry> const& entries,
    local_spr_score_options const& options,
    chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state) {
  if (!prepared.valid_for_accumulation) return;
  if (!options.force_dense_invalid_reason_for_tests.empty()) {
    invalidate_prepared_local_candidate(
        state, prepared, options.force_dense_invalid_reason_for_tests);
    return;
  }
  if (!validate_prepared_candidate_plan_identity(
          state, prepared, counters, checked_state)) {
    return;
  }
  auto const& delta = prepared.delta();
  parsimony_chart_detail::structural_work_observer hot_work_observer{
      .full_grammar_validations =
          counters != nullptr
              ? &counters->candidate_pattern_full_grammar_validations
              : nullptr,
      .production_partition_validations =
          counters != nullptr
              ? &counters->candidate_pattern_partition_validations
              : nullptr,
      .clade_order_sorts = counters != nullptr
                               ? &counters->candidate_pattern_clade_order_sorts
                               : nullptr};
  parsimony_chart_detail::structural_work_observer_scope hot_work_scope{
      counters != nullptr ? &hot_work_observer : nullptr};
  auto const& patterns = state.active_patterns.patterns.patterns;
  auto chart_build_options = state.chart_opts;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;
  try {
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (counters != nullptr) {
        ++counters->candidate_execution_plan_cache_hits;
      }
      auto pattern_index = pattern_offset + i;
      if (pattern_index >= patterns.size()) {
        throw std::runtime_error(
            "chart SPR local score: pattern batch offset out of range");
      }
      auto const& pattern = patterns[pattern_index];
      auto const& cache_entry = entries[i];
      // The pattern set is immutable for the whole scoring epoch.  Borrow its
      // leaf row directly; constructing leaf_site_states here would allocate
      // and copy once for every candidate x pattern visit.
      auto states = view_leaf_site_states(pattern.state_by_taxon);
      if (counters != nullptr) {
        ++counters->local_leaf_state_view_uses;
      }
      build_local_overlay_chart_rows_into(
          state.grammar, delta, cache_entry.chart, states, scratch.rows,
          chart_build_options, options.validate_cached_chart_shapes, counters);
      if (counters != nullptr) {
        counters->local_rows_recomputed +=
            delta.affected_order.size();
      }
      if (prepared.verification_materialized) {
        verify_local_overlay_rows_against_full(
            delta, scratch.rows, cache_entry.chart,
            *prepared.verification_materialized, states, state.chart_opts);
      }
      auto const& root_row = local_overlay_chart_row(
          scratch.rows, cache_entry.chart, delta.root);
      prepared.new_active_score = chart_multisite_detail::checked_add_u64(
          prepared.new_active_score,
          chart_spr_weighted_root_score_from_row(root_row, pattern,
                                                 state.chart_opts),
          "chart-SPR local candidate active lower bound");
    }
  } catch (std::exception const& e) {
    invalidate_prepared_local_candidate(state, prepared, e.what());
  }
}

inline void accumulate_prepared_local_candidate_patterns(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    std::size_t pattern_offset,
    std::vector<pattern_chart_cache_entry> const& entries,
    local_spr_score_options const& options,
    chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch) {
  if (!prepared.valid_for_accumulation) return;
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    accumulate_prepared_local_candidate_patterns(
        state, prepared, pattern_offset, entries, options, counters, scratch,
        checked);
  } catch (std::exception const& e) {
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    invalidate_prepared_local_candidate(state, prepared, e.what());
  }
}

inline void score_prepared_local_candidate_pattern_tile(
    chart_spr_search_state const& state,
    prepared_local_candidate_score const& prepared, std::size_t pattern_begin,
    std::size_t pattern_end, local_spr_score_options const& options,
    chart_spr_search_counters* counters, chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state,
    local_score_tile_result& tile) {
  tile.reset_for_operation();
  scratch.clear_borrows();
  if (!prepared.valid_for_accumulation) return;
  if (!options.force_dense_invalid_reason_for_tests.empty()) {
    tile.valid = false;
    auto reason = options.force_dense_invalid_reason_for_tests;
    if (use_finite_local_score_diagnostics(state, options)) {
      reason = bounded_lazy_local_invalid_reason(reason);
    }
    tile.invalid_reason.assign(reason);
    return;
  }

  try {
    checked_state.assert_same(state.grammar, state.execution_plan);
    auto const& delta = prepared.delta();
    assert_overlay_delta_execution_plan_compatible(delta, state.execution_plan);
    auto const& patterns = state.active_patterns.patterns.patterns;
    if (pattern_begin > pattern_end || pattern_end > patterns.size() ||
        (!state.local_commit_inside_rows.valid() &&
         state.pattern_charts.size() != patterns.size())) {
      throw std::runtime_error(
          "chart SPR local score: candidate-pattern tile out of range");
    }
    parsimony_chart_detail::structural_work_observer hot_work_observer{
        .full_grammar_validations =
            counters != nullptr
                ? &counters->candidate_pattern_full_grammar_validations
                : nullptr,
        .production_partition_validations =
            counters != nullptr
                ? &counters->candidate_pattern_partition_validations
                : nullptr,
        .clade_order_sorts =
            counters != nullptr ? &counters->candidate_pattern_clade_order_sorts
                                : nullptr};
    parsimony_chart_detail::structural_work_observer_scope hot_work_scope{
        counters != nullptr ? &hot_work_observer : nullptr};
    auto chart_build_options = state.chart_opts;
    chart_build_options.keep_trace = false;
    chart_build_options.max_trace_choices = 0;

    for (std::size_t pattern_index = pattern_begin; pattern_index < pattern_end;
         ++pattern_index) {
      if (counters != nullptr) {
        ++counters->candidate_execution_plan_cache_hits;
        ++counters->local_leaf_state_view_uses;
      }
      auto const& pattern = patterns[pattern_index];
      auto states = view_leaf_site_states(pattern.state_by_taxon);
      if (state.local_commit_inside_rows.valid()) {
        if (counters != nullptr) {
          ++counters->local_commit_inside_row_view_pattern_visits;
        }
        build_local_overlay_chart_rows_into(
            state.grammar, delta, state.local_commit_inside_rows, pattern_index,
            states, scratch.rows, chart_build_options, counters);
      } else {
        auto const& cache_entry = state.pattern_charts[pattern_index];
        build_local_overlay_chart_rows_into(
            state.grammar, delta, cache_entry.chart, states, scratch.rows,
            chart_build_options, options.validate_cached_chart_shapes,
            counters);
      }
      if (counters != nullptr) {
        counters->local_rows_recomputed += delta.affected_order.size();
      }
      if (prepared.verification_materialized) {
        if (state.local_commit_inside_rows.valid()) {
          verify_local_overlay_rows_against_full(
              delta, scratch.rows, state.local_commit_inside_rows,
              pattern_index, *prepared.verification_materialized, states,
              state.chart_opts);
        } else {
          verify_local_overlay_rows_against_full(
              delta, scratch.rows, state.pattern_charts[pattern_index].chart,
              *prepared.verification_materialized, states, state.chart_opts);
        }
      }
      auto const& root_row =
          state.local_commit_inside_rows.valid()
              ? local_overlay_chart_row(scratch.rows,
                                        state.local_commit_inside_rows,
                                        pattern_index, delta.root)
              : local_overlay_chart_row(
                    scratch.rows, state.pattern_charts[pattern_index].chart,
                    delta.root);
      tile.active_score = chart_multisite_detail::checked_add_u64(
          tile.active_score,
          chart_spr_weighted_root_score_from_row(root_row, pattern,
                                                 state.chart_opts),
          "chart-SPR local candidate active lower bound");
    }
  } catch (std::exception const& e) {
    tile.valid = false;
    auto reason = std::string_view{e.what()};
    if (use_finite_local_score_diagnostics(state, options)) {
      reason = bounded_lazy_local_invalid_reason(reason);
    }
    tile.invalid_reason.assign(reason);
  }
  scratch.clear_borrows();
}

inline void score_prepared_local_candidate_pattern_cache_tile(
    chart_spr_search_state const& state,
    prepared_local_candidate_score const& prepared, std::size_t pattern_offset,
    std::vector<pattern_chart_cache_entry> const& entries,
    std::size_t entry_begin, std::size_t entry_end,
    local_spr_score_options const& options, chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state,
    local_score_tile_result& tile) {
  tile.reset_for_operation();
  scratch.clear_borrows();
  if (!prepared.valid_for_accumulation) return;
  if (!options.force_dense_invalid_reason_for_tests.empty()) {
    tile.valid = false;
    auto reason = options.force_dense_invalid_reason_for_tests;
    if (use_finite_local_score_diagnostics(state, options)) {
      reason = bounded_lazy_local_invalid_reason(reason);
    }
    tile.invalid_reason.assign(reason);
    return;
  }

  try {
    checked_state.assert_same(state.grammar, state.execution_plan);
    auto const& delta = prepared.delta();
    assert_overlay_delta_execution_plan_compatible(delta, state.execution_plan);
    auto const& patterns = state.active_patterns.patterns.patterns;
    if (entry_begin > entry_end || entry_end > entries.size() ||
        pattern_offset > patterns.size() ||
        entries.size() > patterns.size() - pattern_offset) {
      throw std::runtime_error(
          "chart SPR local score: cached candidate-pattern tile out of "
          "range");
    }

    parsimony_chart_detail::structural_work_observer hot_work_observer{
        .full_grammar_validations =
            counters != nullptr
                ? &counters->candidate_pattern_full_grammar_validations
                : nullptr,
        .production_partition_validations =
            counters != nullptr
                ? &counters->candidate_pattern_partition_validations
                : nullptr,
        .clade_order_sorts =
            counters != nullptr ? &counters->candidate_pattern_clade_order_sorts
                                : nullptr};
    parsimony_chart_detail::structural_work_observer_scope hot_work_scope{
        counters != nullptr ? &hot_work_observer : nullptr};
    auto chart_build_options = state.chart_opts;
    chart_build_options.keep_trace = false;
    chart_build_options.max_trace_choices = 0;

    for (std::size_t entry_index = entry_begin; entry_index < entry_end;
         ++entry_index) {
      if (counters != nullptr) {
        ++counters->candidate_execution_plan_cache_hits;
        ++counters->local_leaf_state_view_uses;
      }
      auto const pattern_index = pattern_offset + entry_index;
      auto const& pattern = patterns[pattern_index];
      auto const& cache_entry = entries[entry_index];
      auto states = view_leaf_site_states(pattern.state_by_taxon);
      build_local_overlay_chart_rows_into(
          state.grammar, delta, cache_entry.chart, states, scratch.rows,
          chart_build_options, options.validate_cached_chart_shapes, counters);
      if (counters != nullptr) {
        counters->local_rows_recomputed += delta.affected_order.size();
      }
      if (prepared.verification_materialized) {
        verify_local_overlay_rows_against_full(
            delta, scratch.rows, cache_entry.chart,
            *prepared.verification_materialized, states, state.chart_opts);
      }
      auto const& root_row =
          local_overlay_chart_row(scratch.rows, cache_entry.chart, delta.root);
      tile.active_score = chart_multisite_detail::checked_add_u64(
          tile.active_score,
          chart_spr_weighted_root_score_from_row(root_row, pattern,
                                                 state.chart_opts),
          "chart-SPR local candidate active lower bound");
    }
  } catch (std::exception const& e) {
    tile.valid = false;
    auto reason = std::string_view{e.what()};
    if (use_finite_local_score_diagnostics(state, options)) {
      reason = bounded_lazy_local_invalid_reason(reason);
    }
    tile.invalid_reason.assign(reason);
  }
  scratch.clear_borrows();
}

inline void finalize_prepared_local_candidate_score(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared) {
  if (!prepared.valid_for_accumulation) return;

  auto new_score = chart_multisite_detail::checked_add_u64(
      prepared.new_active_score, state.invariant_constant_offset,
      "chart-SPR local candidate invariant offset");
  auto old_score = state.composite_lower_bound_with_invariants;
  auto& result = prepared.result;
  result.lower_bound.value =
      spr_score_result{chart_spr_detail::signed_delta(old_score, new_score),
                       old_score, new_score, false};
  result.lower_bound.kind = chart_spr_score_kind::composite_lower_bound;
  result.lower_bound.convention =
      chart_spr_score_convention::full_with_invariants;
  result.lower_bound.invariant_offset_applied = state.invariant_constant_offset;
  result.affected_clade_count = prepared.delta().affected_order.size();
  result.valid = true;
  result.invalid_reason.clear();
}

inline void finish_prepared_local_candidate_score_into(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    chart_spr_local_score_result& output) {
  finalize_prepared_local_candidate_score(state, prepared);
  output = prepared.result;
}

inline chart_spr_local_score_result finish_prepared_local_candidate_score(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared) {
  chart_spr_local_score_result result;
  finish_prepared_local_candidate_score_into(state, prepared, result);
  return result;
}

inline std::vector<pattern_chart_cache_entry>
build_pattern_chart_cache_entries_for_range(
    chart_spr_search_state const& state, std::size_t begin,
    std::size_t count,
    chart_spr_search_counters* counters) {
  auto const& patterns = state.active_patterns.patterns.patterns;
  if (begin > patterns.size() || count > patterns.size() - begin) {
    throw std::runtime_error(
        "chart SPR local score: pattern cache batch range out of range");
  }
  auto chart_build_options = state.chart_opts;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;
  std::vector<pattern_chart_cache_entry> entries;
  entries.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    entries.push_back(build_pattern_chart_cache_entry(
        state.execution_plan, patterns[begin + i], state.chart_opts,
        chart_build_options));
    if (counters != nullptr) ++counters->chart_execution_plan_cache_hits;
  }
  return entries;
}

inline std::size_t lazy_overlay_inside_class_index(
    lazy_multisite_chart const& lazy, clade_id clade, std::size_t pattern) {
  if (clade == no_clade || clade >= lazy.class_index_by_pattern_by_clade.size()) {
    throw std::runtime_error(
        "chart SPR lazy local score: base clade class map out of range");
  }
  if (pattern >= lazy.pattern_count) {
    throw std::runtime_error(
        "chart SPR lazy local score: pattern index out of range");
  }
  auto const& map = lazy.class_index_by_pattern_by_clade[clade];
  if (!map) {
    throw std::runtime_error(
        "chart SPR lazy local score: inside class map was not retained");
  }
  auto class_index = (*map)[pattern];
  if (clade >= lazy.inside_rows_by_clade.size() ||
      class_index >= lazy.inside_rows_by_clade[clade].size()) {
    throw std::runtime_error(
        "chart SPR lazy local score: inside class index out of range");
  }
  return class_index;
}

inline void append_lazy_overlay_leaf_state(
    std::vector<site_pattern> const& patterns, taxon_id taxon,
    std::size_t pattern,
    std::vector<lazy_key_grouping_detail::packed_key_word>& key) {
  if (taxon == chart_plan_no_taxon) return;
  if (pattern >= patterns.size() ||
      taxon >= patterns[pattern].state_by_taxon.size()) {
    throw std::runtime_error(
        "chart SPR lazy local score: leaf state key out of range");
  }
  key.push_back(lazy_key_grouping_detail::checked_packed_key_word(
      patterns[pattern].state_by_taxon[taxon],
      "chart SPR lazy local leaf-state key"));
}

inline void append_lazy_overlay_base_class(
    lazy_multisite_chart const& lazy, overlay_clade_ref ref,
    std::size_t pattern,
    std::vector<lazy_key_grouping_detail::packed_key_word>& key) {
  if (ref.space != overlay_id_space::base) return;
  key.push_back(lazy_key_grouping_detail::checked_packed_key_word(
      lazy_overlay_inside_class_index(lazy, ref.id, pattern),
      "chart SPR lazy local inside-class key"));
}

inline std::size_t lazy_overlay_context_key_width(
    spr_overlay_delta const& delta) {
  using lazy_key_grouping_detail::checked_packed_key_count_add;
  std::size_t width = 1;  // Base root class.
  auto add_optional_components = [&](overlay_clade_ref clade,
                                     taxon_id leaf_taxon) {
    if (clade.space == overlay_id_space::base) {
      width = checked_packed_key_count_add(
          width, 1, "chart SPR lazy local context-key width");
    }
    if (leaf_taxon != chart_plan_no_taxon) {
      width = checked_packed_key_count_add(
          width, 1, "chart SPR lazy local context-key width");
    }
  };
  for (auto const& row : delta.compiled_rows) {
    add_optional_components(row.clade, row.leaf_taxon);
    for (auto const& production :
         candidate_chart_productions_for_row(delta, row)) {
      for (auto const& child : candidate_chart_children(delta, production)) {
        add_optional_components(child.clade, child.leaf_taxon);
      }
    }
  }
  return width;
}

struct lazy_local_scratch_preparation_report {
  std::size_t observed_prepublication_peak_dynamic_capacity_bytes = 0;
};

// Coordinator-side preparation boundary for the lazy candidate kernel. Every
// vector touched by the scheduled grouping/recurrence path is brought to its
// worst-case size before admission. The worker subsequently changes sizes and
// contents only; it performs no reserve or grouping-storage preparation.
inline lazy_local_scratch_preparation_report
prepare_lazy_local_score_scratch_for_candidate(
    chart_spr_search_state const& state, spr_overlay_delta const& delta,
    chart_spr_local_score_scratch& scratch,
    chart_spr_search_counters* counters) {
  scratch.clear_borrows();
  auto const patterns = state.active_patterns.patterns.patterns.size();
  auto const key_width = lazy_overlay_context_key_width(delta);
  auto const word_count =
      lazy_key_grouping_detail::checked_packed_key_count_multiply(
          patterns, key_width, "chart SPR lazy local packed context keys");

  auto const word_report =
      lazy_key_grouping_detail::prepare_packed_key_word_buffer(
          word_count, scratch.lazy_context_key_words);
  auto const grouping_report =
      lazy_key_grouping_detail::prepare_packed_key_grouping_storage(
          patterns, scratch.lazy_context_grouping_workspace,
          scratch.lazy_context_grouping_result);
  auto const old_row_capacity = scratch.rows.rows.capacity();
  scratch.lazy_contexts.reserve(patterns);
  scratch.rows.rows.reserve(delta.affected_order.size());
  if (counters != nullptr && scratch.rows.rows.capacity() != old_row_capacity) {
    ++counters->local_row_scratch_capacity_growths;
  }

  scratch.lazy_prepared_key_width = key_width;
  scratch.lazy_prepared_grouping_resident_bytes =
      grouping_report.prepared_owned_capacity_resident_bytes;
  scratch.lazy_grouping_status = {};
  auto const prepared = local_score_scratch_dynamic_capacity_bytes(scratch);
  // Two copies of final capacity conservatively dominate vector reserve's
  // old+new transient; the primitive reports dominate their own staging.
  auto peak = local_capacity_checked_multiply(
      prepared, 2, "chart SPR lazy-local scratch preparation peak");
  peak = local_capacity_checked_add(
      peak, word_report.observed_prepublication_peak_capacity_resident_bytes,
      "chart SPR lazy-local word preparation peak");
  peak = local_capacity_checked_add(
      peak,
      grouping_report
          .observed_prepublication_peak_owned_capacity_resident_bytes,
      "chart SPR lazy-local grouping preparation peak");
  return lazy_local_scratch_preparation_report{
      .observed_prepublication_peak_dynamic_capacity_bytes = peak,
  };
}

inline void append_lazy_overlay_context_key(
    chart_spr_search_state const& state, spr_overlay_delta const& delta,
    std::size_t pattern,
    std::vector<lazy_key_grouping_detail::packed_key_word>& key_words) {
  if (!state.lazy_chart) {
    throw std::runtime_error("chart SPR lazy local score: missing lazy chart");
  }
  auto const& lazy = *state.lazy_chart;
  auto const& patterns = state.active_patterns.patterns.patterns;
  key_words.push_back(lazy_key_grouping_detail::checked_packed_key_word(
      lazy_overlay_inside_class_index(lazy, state.grammar.root_clade, pattern),
      "chart SPR lazy local root-class key"));

  auto append_children =
      [&](std::span<candidate_chart_child_descriptor const> children) {
        for (auto const& child : children) {
          append_lazy_overlay_base_class(lazy, child.clade, pattern, key_words);
          append_lazy_overlay_leaf_state(patterns, child.leaf_taxon, pattern,
                                         key_words);
        }
      };

  for (auto const& row : delta.compiled_rows) {
    auto ref = row.clade;
    append_lazy_overlay_base_class(lazy, ref, pattern, key_words);
    append_lazy_overlay_leaf_state(patterns, row.leaf_taxon, pattern,
                                   key_words);
    for (auto const& production :
         candidate_chart_productions_for_row(delta, row)) {
      append_children(candidate_chart_children(delta, production));
    }
  }
}

struct lazy_overlay_row_provider {
  spr_overlay_delta const& delta;
  lazy_multisite_chart const& lazy;
  std::size_t pattern = no_site_pattern;
  local_overlay_chart_rows const& local_rows;

  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      overlay_clade_ref ref) const {
    auto slot = local_rows.slot_for(ref);
    if (slot != local_overlay_chart_rows::npos) {
      if (slot >= local_rows.rows.size()) {
        throw std::runtime_error(
            "chart SPR lazy local score: local row slot out of range");
      }
      return local_rows.rows[slot];
    }
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(
          "chart SPR lazy local score: reachable temp clade has no local row");
    }
    auto class_index = lazy_overlay_inside_class_index(lazy, ref.id, pattern);
    return lazy.inside_rows_by_clade[ref.id][class_index];
  }

  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      candidate_chart_child_descriptor const& child) const {
    if (child.has_local_row()) {
      if (child.local_row_slot >= local_rows.rows.size()) {
        throw std::runtime_error(
            "chart SPR lazy local score: compiled local child slot out of "
            "range");
      }
      return local_rows.rows[child.local_row_slot];
    }
    if (child.base_clade == no_clade) {
      throw std::runtime_error(
          "chart SPR lazy local score: compiled child has no base row");
    }
    auto class_index =
        lazy_overlay_inside_class_index(lazy, child.base_clade, pattern);
    return lazy.inside_rows_by_clade[child.base_clade][class_index];
  }
};

inline std::uint64_t lazy_overlay_weighted_root_score(
    std::array<chart_cost, nuc_state_count> const& root_row,
    lazy_overlay_context_accumulator const& context,
    chart_options const& options) {
  if (!options.score_ua_edge) {
    return chart_multisite_detail::checked_mul_cost(
        context.weight, chart_multisite_detail::row_min(root_row),
        "chart SPR lazy local score root cost");
  }
  std::uint64_t total = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    auto count = context.reference_state_counts[reference_state];
    if (count == 0) continue;
    chart_cost best = chart_inf;
    for (std::uint8_t root_state = 0; root_state < nuc_state_count;
         ++root_state) {
      best = std::min(best, parsimony_chart_detail::saturated_add(
                                root_row[root_state],
                                parsimony_chart_detail::transition_cost(
                                    reference_state, root_state)));
    }
    total = chart_multisite_detail::checked_add_u64(
        total,
        chart_multisite_detail::checked_mul_cost(
            count, best, "chart SPR lazy local score root-edge cost"),
        "chart SPR lazy local score root-edge total");
  }
  return total;
}

inline void accumulate_prepared_local_candidate_lazy_prepared(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    local_spr_score_options const& options, chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state) {
  if (!prepared.valid_for_accumulation) return;
  try {
    checked_state.assert_same(state.grammar, state.execution_plan);
    assert_overlay_delta_execution_plan_compatible(prepared.delta(),
                                                   state.execution_plan);
  } catch (std::bad_alloc const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            allocation_failure);
  } catch (std::overflow_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            arithmetic_overflow);
  } catch (std::length_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            length_failure);
  } catch (std::exception const&) {
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::plan_identity);
  }
  if (scratch.has_lazy_worker_failure()) {
    prepared.valid_for_accumulation = false;
    return;
  }
  auto const& delta = prepared.delta();
  parsimony_chart_detail::structural_work_observer hot_work_observer{
      .full_grammar_validations =
          counters != nullptr
              ? &counters->candidate_pattern_full_grammar_validations
              : nullptr,
      .production_partition_validations =
          counters != nullptr
              ? &counters->candidate_pattern_partition_validations
              : nullptr,
      .clade_order_sorts = counters != nullptr
                               ? &counters->candidate_pattern_clade_order_sorts
                               : nullptr};
  parsimony_chart_detail::structural_work_observer_scope hot_work_scope{
      counters != nullptr ? &hot_work_observer : nullptr};
  if (!state.lazy_chart) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            missing_lazy_chart);
    prepared.valid_for_accumulation = false;
    return;
  }

  auto const& patterns = state.active_patterns.patterns.patterns;
  try {
    auto const key_width = lazy_overlay_context_key_width(delta);
    if (scratch.lazy_prepared_key_width != key_width) {
      throw std::logic_error(
          "chart SPR lazy local score: scratch was not prepared for candidate");
    }
    auto const word_count =
        lazy_key_grouping_detail::checked_packed_key_count_multiply(
            patterns.size(), key_width,
            "chart SPR lazy local packed context keys");
    if (scratch.lazy_context_key_words.capacity() < word_count ||
        scratch.lazy_contexts.capacity() < patterns.size() ||
        scratch.rows.rows.capacity() < delta.affected_order.size()) {
      throw std::logic_error(
          "chart SPR lazy local score: prepared scratch capacity is too small");
    }
    scratch.lazy_context_key_words.clear();
    for (std::size_t pattern_index = 0; pattern_index < patterns.size();
         ++pattern_index) {
      if (counters != nullptr) {
        ++counters->candidate_execution_plan_cache_hits;
      }
      append_lazy_overlay_context_key(state, delta, pattern_index,
                                      scratch.lazy_context_key_words);
      auto const expected_words = (pattern_index + 1) * key_width;
      if (scratch.lazy_context_key_words.size() != expected_words) {
        throw std::logic_error(
            "chart SPR lazy local score: inconsistent context-key width");
      }
    }

    auto const grouping_status =
        lazy_key_grouping_detail::try_group_packed_keys_prepared(
            lazy_key_grouping_detail::packed_key_matrix_view{
                .key_count = patterns.size(),
                .key_width = key_width,
                .words = scratch.lazy_context_key_words,
            },
            scratch.lazy_context_grouping_workspace,
            scratch.lazy_context_grouping_result,
            scratch.lazy_prepared_grouping_resident_bytes);
    scratch.lazy_grouping_status = grouping_status;
    if (!grouping_status.succeeded()) {
      return;
    }
    auto const& grouping = scratch.lazy_context_grouping_result;
    scratch.lazy_contexts.resize(grouping.class_count());
    std::fill(scratch.lazy_contexts.begin(), scratch.lazy_contexts.end(),
              lazy_overlay_context_accumulator{});
    for (std::size_t pattern_index = 0; pattern_index < patterns.size();
         ++pattern_index) {
      auto const class_id = grouping.class_by_input[pattern_index];
      if (class_id >= scratch.lazy_contexts.size()) {
        throw std::logic_error(
            "chart SPR lazy local score: context class out of range");
      }
      auto& context = scratch.lazy_contexts[class_id];
      if (context.representative == no_site_pattern) {
        context.representative = pattern_index;
      }
      auto const& pattern = patterns[pattern_index];
      context.weight = chart_multisite_detail::checked_add_u64(
          context.weight, pattern.weight,
          "chart SPR lazy local score context weight");
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        context.reference_state_counts[reference_state] =
            chart_multisite_detail::checked_add_u64(
                context.reference_state_counts[reference_state],
                pattern.reference_state_counts[reference_state],
                "chart SPR lazy local score context reference count");
      }
    }

    for (auto class_id : grouping.lexicographic_class_order) {
      if (class_id >= scratch.lazy_contexts.size()) {
        throw std::logic_error(
            "chart SPR lazy local score: lexicographic class out of range");
      }
      auto const& context = scratch.lazy_contexts[class_id];
      if (context.representative >= patterns.size()) {
        throw std::runtime_error(
            "chart SPR lazy local score: context representative out of range");
      }
      auto& rows = scratch.rows;
      rows.base_row_slot = delta.affected_base_row_slot;
      rows.temp_row_slot = delta.affected_temp_row_slot;
      auto const required_rows = delta.affected_order.size();
      rows.rows.resize(required_rows);
      std::fill(rows.rows.begin(), rows.rows.end(),
                parsimony_chart_detail::make_inf_row());

      lazy_overlay_row_provider provider{delta, *state.lazy_chart,
                                         context.representative, rows};
      auto states = view_leaf_site_states(
          patterns[context.representative].state_by_taxon);
      for (std::size_t i = 0; i < delta.compiled_rows.size(); ++i) {
        rows.rows[i] = recompute_overlay_delta_row(
            delta, states, provider, delta.compiled_rows[i], counters);
      }
      if (counters != nullptr) {
        counters->local_rows_recomputed += delta.affected_order.size();
      }
      if (prepared.verification_materialized) {
        auto chart_build_options = state.chart_opts;
        chart_build_options.keep_trace = false;
        chart_build_options.max_trace_choices = 0;
        auto base_chart =
            build_single_site_chart(state.grammar, states, chart_build_options);
        verify_local_overlay_rows_against_full(
            delta, rows, base_chart, *prepared.verification_materialized,
            states, state.chart_opts);
      }
      auto const& root_row = provider.row(delta.root);
      auto contribution =
          lazy_overlay_weighted_root_score(root_row, context, state.chart_opts);
      prepared.new_active_score = chart_multisite_detail::checked_add_u64(
          prepared.new_active_score, contribution,
          "chart SPR lazy local candidate active lower bound");
    }
  } catch (chart_spr_lazy_local_budget_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            scratch_invariant);
  } catch (chart_spr_lazy_local_enumeration_budget_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            scratch_invariant);
  } catch (chart_spr_exact_candidate_budget_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            scratch_invariant);
  } catch (chart_spr_exact_state_budget_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            scratch_invariant);
  } catch (std::bad_alloc const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            allocation_failure);
  } catch (std::overflow_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            arithmetic_overflow);
  } catch (std::length_error const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            length_failure);
  } catch (std::exception const&) {
    scratch.stage_lazy_worker_failure(
        chart_spr_search_detail::lazy_local_worker_failure_kind::
            scratch_invariant);
  }
  if (scratch.has_lazy_worker_failure()) {
    prepared.valid_for_accumulation = false;
  }
}

inline void finalize_lazy_local_grouping_status(
    chart_spr_search_state const&, prepared_local_candidate_score&,
    chart_spr_local_score_scratch& scratch, std::size_t slot = 0) {
  if (scratch.has_lazy_worker_failure()) {
    throw lazy_local_worker_failure_error(scratch.lazy_worker_failure, slot);
  }
  if (scratch.lazy_grouping_status.succeeded()) return;
  // Shape/storage/budget statuses indicate a violated admitted invariant, not
  // an ordinary biological candidate rejection. Materialize the exception on
  // the coordinator after the join so no worker allocates exception storage.
  lazy_key_grouping_detail::throw_packed_key_grouping_prepared_failure(
      scratch.lazy_grouping_status);
}

inline void accumulate_prepared_local_candidate_lazy(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    local_spr_score_options const& options, chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state) {
  if (!prepared.valid_for_accumulation) return;
  prepare_lazy_local_score_scratch_for_candidate(state, prepared.delta(),
                                                 scratch, counters);
  accumulate_prepared_local_candidate_lazy_prepared(
      state, prepared, options, counters, scratch, checked_state);
  finalize_lazy_local_grouping_status(state, prepared, scratch);
}

inline void accumulate_prepared_local_candidate_lazy(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    local_spr_score_options const& options,
    chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch) {
  if (!prepared.valid_for_accumulation) return;
  auto checked = check_chart_execution_plan(state.grammar,
                                            state.execution_plan);
  accumulate_prepared_local_candidate_lazy(state, prepared, options, counters,
                                           scratch, checked);
}

inline void score_candidate_locally_counted_into(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    local_spr_score_options const& options, chart_spr_search_counters* counters,
    prepared_local_candidate_score& prepared,
    chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_local_score_result& output) {
  scratch.clear_borrows();
  prepare_local_candidate_score_into(state, candidate, options, counters,
                                     checked_state, prepared);
  auto finish = [&] {
    finish_prepared_local_candidate_score_into(state, prepared, output);
    scratch.clear_borrows();
    prepared.release_operation_borrows();
  };
  if (!prepared.valid_for_accumulation) {
    finish();
    return;
  }

  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    accumulate_prepared_local_candidate_lazy(
        state, prepared, options, counters, scratch, checked_state);
    finish();
    return;
  }

  if (state.local_commit_inside_rows.valid()) {
    accumulate_prepared_local_candidate_row_view(
        state, prepared, 0, state.active_patterns.patterns.patterns.size(),
        options, counters, scratch, checked_state);
    finish();
    return;
  }

  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    if (state.pattern_charts.size() !=
        state.active_patterns.patterns.patterns.size()) {
      throw std::runtime_error(
          "chart SPR local score: pattern chart cache size mismatch");
    }
    accumulate_prepared_local_candidate_patterns(
        state, prepared, 0, state.pattern_charts, options, counters, scratch,
        checked_state);
    finish();
    return;
  }

  auto const& patterns = state.active_patterns.patterns.patterns;
  auto batch_size = std::max<std::size_t>(
      1, state.effective_pattern_batch_size);
  for (std::size_t begin = 0; begin < patterns.size(); begin += batch_size) {
    auto count = std::min(batch_size, patterns.size() - begin);
    auto entries = build_pattern_chart_cache_entries_for_range(state, begin,
                                                               count, counters);
    if (counters != nullptr) ++counters->pattern_batch_cache_builds;
    if (counters != nullptr) {
      counters->multifurcation_productions_scored +=
          multifurcation_productions_scored_for_entries(entries);
    }
    accumulate_prepared_local_candidate_patterns(
        state, prepared, begin, entries, options, counters, scratch,
        checked_state);
    if (!prepared.valid_for_accumulation) break;
  }
  finish();
}

inline std::size_t normalize_chart_spr_worker_count(
    std::size_t requested) {
  // Keep the explicit path allocation- and probe-free.  Automatic requests
  // share the scheduler's affinity/topology-aware resolver.
  if (requested != 0) return requested;
  return resolve_chart_worker_count(0).resolved_workers;
}

inline std::size_t requested_chart_spr_worker_count(
    chart_spr_search_options const& options) {
  // Preserve direct library callers of the legacy field.  A non-default
  // unified value wins; otherwise a non-default legacy value is the request.
  // The CLI rejects conflicting aliases before constructing options.
  return options.worker_count != 1 ? options.worker_count
                                   : options.local_score_worker_count;
}

inline chart_scheduler_options chart_spr_search_scheduler_options(
    std::size_t requested_workers) {
  chart_scheduler_options scheduler_options;
  scheduler_options.requested_workers = requested_workers;
  // Phase 3 deliberately leaves the frozen 64-candidate case serial.  Phases
  // 4--6 replace this orchestration-only grain with work-axis estimates.
  scheduler_options.default_minimum_grain = 64;
  scheduler_options.default_target_ranges_per_worker = 4;
  return scheduler_options;
}

inline chart_scheduler_options chart_spr_compatibility_scheduler_options(
    std::size_t requested_workers) {
  chart_scheduler_options scheduler_options;
  scheduler_options.requested_workers = requested_workers;
  // Preserve direct library callers' historical eager parallel behavior and
  // static ceil partition task count.  The production search uses the adaptive
  // Phase-3 policy above and retains its scheduler across every batch.
  scheduler_options.default_minimum_grain = 1;
  scheduler_options.default_target_ranges_per_worker = 1;
  return scheduler_options;
}

inline void maybe_force_local_score_submit_failure_for_tests(
    local_spr_score_options const& options,
    std::size_t successful_submissions) {
  if (options.force_worker_submit_failure_after_for_tests ==
      successful_submissions) {
    if (auto* barrier = options.worker_barrier_for_tests;
        barrier != nullptr && successful_submissions != 0) {
      while (barrier->started.load(std::memory_order_acquire) <
             successful_submissions) {
        std::this_thread::yield();
      }
    }
    throw std::runtime_error(
        "chart SPR local score: forced worker submission failure");
  }
}

inline void local_score_worker_arrive_and_wait_for_tests(
    local_spr_score_options const& options, std::size_t worker) {
  if (auto* barrier = options.worker_barrier_for_tests; barrier != nullptr) {
    barrier->started.fetch_add(1, std::memory_order_release);
    while (!barrier->release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  if (options.force_worker_failure_for_tests == worker) {
    throw std::runtime_error(
        "chart SPR local score: forced worker task failure");
  }
}

inline void release_local_score_workers_for_tests(
    local_spr_score_options const& options,
    std::size_t expected_started_workers) noexcept {
  auto* barrier = options.worker_barrier_for_tests;
  if (barrier == nullptr) return;
  while (barrier->started.load(std::memory_order_acquire) <
         expected_started_workers) {
    std::this_thread::yield();
  }
  barrier->release.store(true, std::memory_order_release);
}

struct local_candidate_pattern_tile_plan {
  std::size_t pattern_count = 0;
  std::size_t pattern_grain = 0;
  std::size_t tiles_per_candidate = 0;
  std::size_t total_tiles = 0;

  [[nodiscard]] bool enabled() const noexcept { return total_tiles > 1; }
};

inline local_candidate_pattern_tile_plan plan_local_candidate_pattern_tiles(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    chart_scheduler const* scheduler) {
  local_candidate_pattern_tile_plan plan;
  auto const candidate_count = candidates.size();
  if (scheduler == nullptr || candidate_count == 0 ||
      (state.cache_strategy != chart_spr_cache_strategy::all_active_patterns &&
       !state.local_commit_inside_rows.valid())) {
    return plan;
  }
  auto const workers = scheduler->worker_resolution().resolved_workers;
  auto const patterns = state.active_patterns.patterns.patterns.size();
  if (workers <= 1 || patterns <= 1 || candidate_count >= workers) return plan;
  if (patterns > (std::numeric_limits<std::size_t>::max)() / candidate_count) {
    throw std::overflow_error(
        "chart SPR local score: candidate-pattern work size overflow");
  }
  auto const work_items = patterns * candidate_count;
  std::size_t affected_units = 0;
  for (auto const& candidate : candidates) {
    auto const weight = std::max<std::size_t>(
        1, chart_spr_detail::estimate_candidate_affected_clades(candidate));
    if (weight > (std::numeric_limits<std::size_t>::max)() / patterns ||
        affected_units >
            (std::numeric_limits<std::size_t>::max)() - weight * patterns) {
      affected_units = (std::numeric_limits<std::size_t>::max)();
      break;
    }
    affected_units += weight * patterns;
  }
  auto const worthwhile_units =
      workers > (std::numeric_limits<std::size_t>::max)() / 2
          ? (std::numeric_limits<std::size_t>::max)()
          : workers * 2;
  if (work_items < workers && affected_units < worthwhile_units) return plan;

  auto const target_total =
      workers > (std::numeric_limits<std::size_t>::max)() / 4
          ? (std::numeric_limits<std::size_t>::max)()
          : workers * 4;
  auto const target_per_candidate =
      std::max<std::size_t>(1, target_total / candidate_count);
  plan.pattern_count = patterns;
  plan.pattern_grain = std::max<std::size_t>(
      1, (patterns + target_per_candidate - 1) / target_per_candidate);
  plan.tiles_per_candidate =
      (patterns + plan.pattern_grain - 1) / plan.pattern_grain;
  if (plan.tiles_per_candidate >
      (std::numeric_limits<std::size_t>::max)() / candidate_count) {
    throw std::overflow_error(
        "chart SPR local score: candidate-pattern tile count overflow");
  }
  plan.total_tiles = plan.tiles_per_candidate * candidate_count;
  return plan;
}

inline std::vector<std::size_t> local_score_weighted_candidate_order(
    std::span<grammar_spr_candidate const> candidates,
    std::size_t resolved_workers) {
  std::vector<std::size_t> order(candidates.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  if (resolved_workers <= 1 || candidates.size() <= 1) return order;
  std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
    auto const lhs_weight =
        chart_spr_detail::estimate_candidate_affected_clades(candidates[lhs]);
    auto const rhs_weight =
        chart_spr_detail::estimate_candidate_affected_clades(candidates[rhs]);
    return lhs_weight != rhs_weight ? lhs_weight > rhs_weight : lhs < rhs;
  });
  return order;
}

inline void require_local_score_admitted_temporary_capacity(
    std::size_t actual_bytes, std::size_t admitted_bytes,
    std::string_view temporary_name) {
  if (admitted_bytes == 0 || actual_bytes <= admitted_bytes) return;
  throw std::logic_error("chart SPR finite local score: observed " +
                         std::string{temporary_name} +
                         " capacity exceeded the unified admission envelope");
}

inline chart_indexed_range_options local_score_candidate_range_options(
    std::size_t candidate_count, std::size_t resolved_workers) noexcept {
  if (resolved_workers <= 1) {
    return chart_spr_phase4_pattern_range_options(candidate_count,
                                                  resolved_workers);
  }
  auto const ranges_per_worker =
      candidate_count / resolved_workers +
      static_cast<std::size_t>(candidate_count % resolved_workers != 0);
  return chart_indexed_range_options{
      .minimum_grain = 1,
      // Unit candidate ranges through the frozen 64-candidate workload keep
      // dynamic claiming responsive to affected-set skew without creating
      // one owning pool task per range (the scheduler submits only workers).
      .target_ranges_per_worker =
          std::clamp<std::size_t>(ranges_per_worker, 1, 32),
  };
}

struct local_pattern_batch_fusion_plan {
  std::size_t pattern_stride = 0;
  std::size_t total_tiles = 0;

  [[nodiscard]] bool enabled() const noexcept {
    return pattern_stride != 0 && total_tiles != 0;
  }
};

inline local_pattern_batch_fusion_plan plan_local_pattern_batch_fusion(
    chart_spr_search_state const& state, std::size_t candidate_count,
    chart_scheduler const* scheduler) {
  local_pattern_batch_fusion_plan plan;
  if (scheduler == nullptr || candidate_count == 0 ||
      state.local_commit_inside_rows.valid() ||
      state.cache_strategy != chart_spr_cache_strategy::pattern_batches) {
    return plan;
  }
  auto const workers = scheduler->worker_resolution().resolved_workers;
  auto const pattern_count = state.active_patterns.patterns.patterns.size();
  auto const batch_size =
      std::max<std::size_t>(1, state.effective_pattern_batch_size);
  auto const max_batch_patterns = std::min(pattern_count, batch_size);
  // Candidate ranges already fill the pool when there are at least as many
  // candidates as stable slots.  Fuse cold chart construction with pattern
  // scoring only for the one/few-candidate shape that otherwise collapses.
  if (workers <= 1 || candidate_count >= workers || max_batch_patterns <= 1) {
    return plan;
  }
  auto const range_plan = scheduler->plan_indexed_ranges(
      max_batch_patterns,
      chart_spr_phase4_pattern_range_options(max_batch_patterns, workers));
  if (range_plan.range_count <= 1) return plan;
  if (max_batch_patterns >
      (std::numeric_limits<std::size_t>::max)() / candidate_count) {
    throw std::overflow_error(
        "chart SPR local score: pattern-batch tile count overflow");
  }
  plan.pattern_stride = max_batch_patterns;
  plan.total_tiles = max_batch_patterns * candidate_count;
  return plan;
}

template <typename Operation>
chart_scheduler_run_summary run_local_score_scheduler_operation(
    chart_scheduler& scheduler, local_spr_score_options const& options,
    Operation&& operation) {
  bool const needs_hooks =
      options.force_worker_submit_failure_after_for_tests.has_value() ||
      options.worker_barrier_for_tests != nullptr;
  if (!needs_hooks) return std::forward<Operation>(operation)();

  chart_scheduler_test_detail::access::set_submission_hooks(
      scheduler,
      [&options](std::size_t successful_submissions) {
        maybe_force_local_score_submit_failure_for_tests(
            options, successful_submissions);
      },
      [&options](std::size_t successful_submissions, std::size_t,
                 bool) noexcept {
        release_local_score_workers_for_tests(options,
                                              successful_submissions);
      });
  try {
    auto summary = std::forward<Operation>(operation)();
    chart_scheduler_test_detail::access::clear_submission_hooks(scheduler);
    return summary;
  } catch (...) {
    chart_scheduler_test_detail::access::clear_submission_hooks(scheduler);
    throw;
  }
}

inline std::vector<pattern_chart_cache_entry>
build_pattern_chart_cache_entries_for_range(
    chart_spr_search_state const& state, std::size_t begin, std::size_t count,
    chart_spr_search_counters* counters, chart_scheduler& scheduler,
    local_spr_score_options const& options) {
  auto const& patterns = state.active_patterns.patterns.patterns;
  if (begin > patterns.size() || count > patterns.size() - begin) {
    throw std::runtime_error(
        "chart SPR local score: pattern cache batch range out of range");
  }
  auto chart_build_options = state.chart_opts;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;
  std::vector<pattern_chart_cache_entry> entries(count);
  std::vector<std::exception_ptr> errors(count);
  std::vector<std::uint8_t> built(count, 0);
  auto const construction_scratch_bytes = local_capacity_checked_add(
      local_owned_dynamic_capacity_bytes(errors),
      local_owned_dynamic_capacity_bytes(built),
      "chart SPR pattern-batch construction scratch capacity");
  require_local_score_admitted_temporary_capacity(
      construction_scratch_bytes,
      options.admission_pattern_batch_construction_scratch_bytes,
      "pattern-batch construction scratch");
  auto const range_options = chart_spr_phase4_pattern_range_options(
      count, scheduler.worker_resolution().resolved_workers);
  auto const range_plan = scheduler.plan_indexed_ranges(count, range_options);
  auto const parallel_cold_build =
      scheduler.worker_resolution().resolved_workers > 1 &&
      range_plan.range_count > 1 && !range_plan.force_serial;
  auto run = run_local_score_scheduler_operation(scheduler, options, [&] {
    return scheduler.for_each_indexed_range(
        count, range_options,
        [&](chart_indexed_range const& range, std::size_t worker,
            chart_scheduler_cancellation_token const&) {
          // A one-range scheduler fallback runs on the coordinator and
          // has no after-submissions hook to release the test barrier.
          // Leave hooks for the following genuinely parallel score axis.
          if (parallel_cold_build) {
            local_score_worker_arrive_and_wait_for_tests(options, worker);
          }
          for (std::size_t local = range.begin; local < range.end; ++local) {
            try {
              entries[local] = build_pattern_chart_cache_entry(
                  state.execution_plan, patterns[begin + local],
                  state.chart_opts, chart_build_options);
              built[local] = 1;
            } catch (...) {
              errors[local] = std::current_exception();
            }
          }
        });
  });
  if (counters != nullptr) {
    record_chart_spr_scheduler_axis_run(
        counters->scheduler_axes.local_score_candidate_patterns, run);
    counters->chart_execution_plan_cache_hits += static_cast<std::size_t>(
        std::count(built.begin(), built.end(), std::uint8_t{1}));
  }
  for (auto const& error : errors) {
    if (error) std::rethrow_exception(error);
  }
  return entries;
}

std::size_t effective_lazy_local_admission_budget_bytes(
    chart_spr_search_state const& state,
    local_spr_score_options const& options) noexcept;

void score_candidates_locally_lazy_waves_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, chart_scheduler* scheduler,
    std::size_t task_limit,
    checked_chart_execution_plan_ref const& checked_state);

inline void score_candidates_locally_all_cache_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, chart_scheduler* scheduler,
    std::size_t worker_count,
    checked_chart_execution_plan_ref const& checked_state,
    local_candidate_pattern_tile_plan const& tile_plan) {
  chart_spr_search_counters aggregate;
  if (candidates.empty()) return;
  ++aggregate.candidate_batches_scored;

  // The source-compatible direct W1 seam bypasses scheduler type erasure and
  // construction so the Phase-2 warmed allocation gate remains exactly zero.
  if (scheduler == nullptr) {
    auto& worker = local_score_workspace_access::worker(workspace, 0);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      auto start = std::chrono::steady_clock::now();
      score_candidate_locally_counted_into(
          state, candidates[i], options, &aggregate,
          local_score_workspace_access::prepared(workspace, 0), worker.scratch,
          checked_state, results[i]);
      results[i].local_score_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
    }
    add_chart_spr_search_counters(state.counters, aggregate);
    return;
  }

  auto const candidate_order =
      local_score_weighted_candidate_order(candidates, worker_count);
  require_local_score_admitted_temporary_capacity(
      local_owned_dynamic_capacity_bytes(candidate_order),
      options.admission_weighted_candidate_order_capacity_bytes,
      "weighted candidate order");

  if (tile_plan.enabled()) {
    auto const batch_start = std::chrono::steady_clock::now();
    for (std::size_t candidate = 0; candidate < candidates.size();
         ++candidate) {
      prepare_local_candidate_score_into(
          state, candidates[candidate], options, &aggregate, checked_state,
          local_score_workspace_access::prepared(workspace, candidate));
    }

    auto const range_options = chart_spr_phase4_pattern_range_options(
        tile_plan.total_tiles, worker_count);
    auto run = run_local_score_scheduler_operation(*scheduler, options, [&] {
      return scheduler->for_each_indexed_range(
          tile_plan.total_tiles, range_options,
          [&](chart_indexed_range const& range, std::size_t worker,
              chart_scheduler_cancellation_token const&) {
            local_score_worker_arrive_and_wait_for_tests(options, worker);
            auto& worker_workspace =
                local_score_workspace_access::worker(workspace, worker);
            for (std::size_t work_index = range.begin; work_index < range.end;
                 ++work_index) {
              auto const candidate_rank = work_index % candidates.size();
              auto const candidate = candidate_order[candidate_rank];
              auto const candidate_tile = work_index / candidates.size();
              auto const tile_index =
                  candidate * tile_plan.tiles_per_candidate + candidate_tile;
              auto const pattern_begin =
                  candidate_tile * tile_plan.pattern_grain;
              auto const pattern_end =
                  std::min(tile_plan.pattern_count,
                           pattern_begin + tile_plan.pattern_grain);
              score_prepared_local_candidate_pattern_tile(
                  state,
                  local_score_workspace_access::prepared(workspace, candidate),
                  pattern_begin, pattern_end, options,
                  &worker_workspace.counters, worker_workspace.scratch,
                  checked_state,
                  local_score_workspace_access::tile_result(workspace,
                                                            tile_index));
            }
          });
    });
    record_chart_spr_scheduler_axis_run(
        aggregate.scheduler_axes.local_score_candidate_patterns, run);
    if (run.used_parallel_workers()) {
      ++aggregate.local_score_parallel_batches;
      aggregate.local_score_worker_tasks += run.worker_tasks_submitted;
    }
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      add_chart_spr_search_counters(
          aggregate,
          local_score_workspace_access::worker(workspace, worker).counters);
    }

    auto const batch_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - batch_start)
                              .count();
    auto const per_candidate_ms =
        batch_ms / static_cast<double>(candidates.size());
    for (std::size_t candidate = 0; candidate < candidates.size();
         ++candidate) {
      auto& prepared =
          local_score_workspace_access::prepared(workspace, candidate);
      if (prepared.valid_for_accumulation) {
        try {
          for (std::size_t candidate_tile = 0;
               candidate_tile < tile_plan.tiles_per_candidate;
               ++candidate_tile) {
            auto const tile_index =
                candidate * tile_plan.tiles_per_candidate + candidate_tile;
            auto const& tile = local_score_workspace_access::tile_result(
                workspace, tile_index);
            if (!tile.valid) {
              invalidate_prepared_local_candidate(state, prepared,
                                                  tile.invalid_reason);
              break;
            }
            prepared.new_active_score = chart_multisite_detail::checked_add_u64(
                prepared.new_active_score, tile.active_score,
                "chart-SPR local candidate active lower bound");
          }
        } catch (std::exception const& e) {
          invalidate_prepared_local_candidate(state, prepared, e.what());
        }
      }
      finish_prepared_local_candidate_score_into(state, prepared,
                                                 results[candidate]);
      results[candidate].local_score_ms = per_candidate_ms;
    }
    add_chart_spr_search_counters(state.counters, aggregate);
    return;
  }

  auto const range_options =
      local_score_candidate_range_options(candidates.size(), worker_count);
  auto run =
      run_local_score_scheduler_operation(*scheduler, options, [&] {
        return scheduler->for_each_indexed_range(
            candidates.size(), range_options,
            [&](chart_indexed_range const& range, std::size_t worker,
                chart_scheduler_cancellation_token const&) {
              local_score_worker_arrive_and_wait_for_tests(options, worker);
              auto& worker_workspace =
                  local_score_workspace_access::worker(workspace, worker);
              for (std::size_t work_index = range.begin; work_index < range.end;
                   ++work_index) {
                auto const i = candidate_order[work_index];
                auto start = std::chrono::steady_clock::now();
                score_candidate_locally_counted_into(
                    state, candidates[i], options, &worker_workspace.counters,
                    local_score_workspace_access::prepared(workspace, worker),
                    worker_workspace.scratch, checked_state, results[i]);
                results[i].local_score_ms =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
              }
            });
      });
  record_chart_spr_scheduler_axis_run(
      aggregate.scheduler_axes.local_score_candidates, run);
  if (run.used_parallel_workers()) {
    ++aggregate.local_score_parallel_batches;
    aggregate.local_score_worker_tasks += run.worker_tasks_submitted;
  }
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    add_chart_spr_search_counters(
        aggregate,
        local_score_workspace_access::worker(workspace, worker).counters);
  }
  add_chart_spr_search_counters(state.counters, aggregate);
}

inline void score_candidates_locally_pattern_batches_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, chart_scheduler* scheduler,
    std::size_t worker_count,
    checked_chart_execution_plan_ref const& checked_state,
    local_pattern_batch_fusion_plan const& fusion_plan) {
  chart_spr_search_counters aggregate;
  if (candidates.empty()) return;
  auto batch_start = std::chrono::steady_clock::now();
  ++aggregate.candidate_batches_scored;

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    prepare_local_candidate_score_into(
        state, candidates[i], options, &aggregate, checked_state,
        local_score_workspace_access::prepared(workspace, i));
  }
  auto const candidate_order =
      scheduler == nullptr
          ? std::vector<std::size_t>{}
          : local_score_weighted_candidate_order(candidates, worker_count);
  require_local_score_admitted_temporary_capacity(
      local_owned_dynamic_capacity_bytes(candidate_order),
      options.admission_weighted_candidate_order_capacity_bytes,
      "weighted candidate order");

  auto const& patterns = state.active_patterns.patterns.patterns;
  auto batch_size = std::max<std::size_t>(
      1, state.effective_pattern_batch_size);
  // Serial pattern batches share one row buffer for the whole candidate batch.
  // Keeping this outside the pattern loop retains the high-water capacity
  // instead of allocating the first affected row again for every batch.
  auto& serial_scratch =
      local_score_workspace_access::serial_scratch(workspace);
  bool used_parallel_workers = false;
  bool worker_counters_folded = false;
  auto fold_worker_counters = [&] {
    if (scheduler == nullptr || worker_counters_folded) return;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      add_chart_spr_search_counters(
          aggregate,
          local_score_workspace_access::worker(workspace, worker).counters);
    }
    worker_counters_folded = true;
  };

  try {
    for (std::size_t begin = 0; begin < patterns.size(); begin += batch_size) {
      auto const count = std::min(batch_size, patterns.size() - begin);

      if (fusion_plan.enabled()) {
        // One synchronous pattern-axis operation owns this cold batch from
        // construction through candidate scoring.  Chart entries and tile
        // results are pre-sized disjoint slots; no worker performs a nested
        // scheduler wait, and the next batch is not retained concurrently.
        std::vector<pattern_chart_cache_entry> entries(count);
        std::vector<std::exception_ptr> build_errors(count);
        std::vector<std::uint8_t> built(count, 0);
        auto const construction_scratch_bytes = local_capacity_checked_add(
            local_owned_dynamic_capacity_bytes(build_errors),
            local_owned_dynamic_capacity_bytes(built),
            "chart SPR fused pattern-batch construction scratch capacity");
        require_local_score_admitted_temporary_capacity(
            construction_scratch_bytes,
            options.admission_pattern_batch_construction_scratch_bytes,
            "fused pattern-batch construction scratch");
        auto chart_build_options = state.chart_opts;
        chart_build_options.keep_trace = false;
        chart_build_options.max_trace_choices = 0;
        auto const range_options = chart_spr_phase4_pattern_range_options(
            count, scheduler->worker_resolution().resolved_workers);
        auto const range_plan =
            scheduler->plan_indexed_ranges(count, range_options);
        auto const parallel_fused_batch =
            scheduler->worker_resolution().resolved_workers > 1 &&
            range_plan.range_count > 1 && !range_plan.force_serial;
        auto run =
            run_local_score_scheduler_operation(*scheduler, options, [&] {
              return scheduler->for_each_indexed_range(
                  count, range_options,
                  [&](chart_indexed_range const& range, std::size_t worker,
                      chart_scheduler_cancellation_token const&) {
                    if (parallel_fused_batch) {
                      local_score_worker_arrive_and_wait_for_tests(options,
                                                                   worker);
                    }
                    auto& worker_workspace =
                        local_score_workspace_access::worker(workspace, worker);
                    for (std::size_t local = range.begin; local < range.end;
                         ++local) {
                      try {
                        entries[local] = build_pattern_chart_cache_entry(
                            state.execution_plan, patterns[begin + local],
                            state.chart_opts, chart_build_options);
                        built[local] = 1;
                      } catch (...) {
                        build_errors[local] = std::current_exception();
                        continue;
                      }
                      for (std::size_t candidate = 0;
                           candidate < candidates.size(); ++candidate) {
                        auto const tile_index =
                            candidate * fusion_plan.pattern_stride + local;
                        score_prepared_local_candidate_pattern_cache_tile(
                            state,
                            local_score_workspace_access::prepared(workspace,
                                                                   candidate),
                            begin, entries, local, local + 1, options,
                            &worker_workspace.counters,
                            worker_workspace.scratch, checked_state,
                            local_score_workspace_access::tile_result(
                                workspace, tile_index));
                      }
                    }
                  });
            });
        record_chart_spr_scheduler_axis_run(
            aggregate.scheduler_axes.local_score_candidate_patterns, run);
        if (run.used_parallel_workers()) {
          used_parallel_workers = true;
          aggregate.local_score_worker_tasks += run.worker_tasks_submitted;
        }
        aggregate.chart_execution_plan_cache_hits += static_cast<std::size_t>(
            std::count(built.begin(), built.end(), std::uint8_t{1}));
        for (auto const& error : build_errors) {
          if (error) std::rethrow_exception(error);
        }
        ++aggregate.pattern_batch_cache_builds;
        aggregate.multifurcation_productions_scored +=
            multifurcation_productions_scored_for_entries(entries);

        // Publish candidate totals only after the operation joins, in the same
        // candidate-major and increasing-pattern order as the serial loop.
        for (std::size_t candidate = 0; candidate < candidates.size();
             ++candidate) {
          auto& prepared =
              local_score_workspace_access::prepared(workspace, candidate);
          if (!prepared.valid_for_accumulation) continue;
          try {
            for (std::size_t local = 0; local < count; ++local) {
              auto const tile_index =
                  candidate * fusion_plan.pattern_stride + local;
              auto const& tile = local_score_workspace_access::tile_result(
                  workspace, tile_index);
              if (!tile.valid) {
                invalidate_prepared_local_candidate(state, prepared,
                                                    tile.invalid_reason);
                break;
              }
              prepared.new_active_score =
                  chart_multisite_detail::checked_add_u64(
                      prepared.new_active_score, tile.active_score,
                      "chart-SPR local candidate active lower bound");
            }
          } catch (std::exception const& e) {
            invalidate_prepared_local_candidate(state, prepared, e.what());
          }
        }
        continue;
      }

      auto entries =
          scheduler == nullptr
              ? build_pattern_chart_cache_entries_for_range(state, begin, count,
                                                            &aggregate)
              : build_pattern_chart_cache_entries_for_range(
                    state, begin, count, &aggregate, *scheduler, options);
      ++aggregate.pattern_batch_cache_builds;
      aggregate.multifurcation_productions_scored +=
          multifurcation_productions_scored_for_entries(entries);

      if (scheduler == nullptr) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
          accumulate_prepared_local_candidate_patterns(
              state, local_score_workspace_access::prepared(workspace, i),
              begin, entries, options, &aggregate, serial_scratch,
              checked_state);
        }
      } else {
        // `entries` is loop-local while the scheduler outlives the search.  A
        // synchronous join keeps its borrow valid through failure recovery.
        auto const range_options = local_score_candidate_range_options(
            candidates.size(), scheduler->worker_resolution().resolved_workers);
        auto run =
            run_local_score_scheduler_operation(*scheduler, options, [&] {
              return scheduler->for_each_indexed_range(
                  candidates.size(), range_options,
                  [&](chart_indexed_range const& range, std::size_t worker,
                      chart_scheduler_cancellation_token const&) {
                    local_score_worker_arrive_and_wait_for_tests(options,
                                                                 worker);
                    auto& worker_workspace =
                        local_score_workspace_access::worker(workspace, worker);
                    for (std::size_t work_index = range.begin;
                         work_index < range.end; ++work_index) {
                      auto const i = candidate_order[work_index];
                      accumulate_prepared_local_candidate_patterns(
                          state,
                          local_score_workspace_access::prepared(workspace, i),
                          begin, entries, options, &worker_workspace.counters,
                          worker_workspace.scratch, checked_state);
                    }
                  });
            });
        record_chart_spr_scheduler_axis_run(
            aggregate.scheduler_axes.local_score_candidates, run);
        if (run.used_parallel_workers()) {
          used_parallel_workers = true;
          aggregate.local_score_worker_tasks += run.worker_tasks_submitted;
        }
      }
    }
  } catch (...) {
    fold_worker_counters();
    if (used_parallel_workers) ++aggregate.local_score_parallel_batches;
    add_chart_spr_search_counters(state.counters, aggregate);
    throw;
  }

  if (used_parallel_workers) ++aggregate.local_score_parallel_batches;
  fold_worker_counters();

  auto batch_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - batch_start)
                      .count();
  auto per_candidate_ms = batch_ms / static_cast<double>(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    finish_prepared_local_candidate_score_into(
        state, local_score_workspace_access::prepared(workspace, i),
        results[i]);
    results[i].local_score_ms = per_candidate_ms;
  }
  add_chart_spr_search_counters(state.counters, aggregate);
}

inline void score_candidates_locally_into_impl(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, chart_scheduler* scheduler,
    checked_chart_execution_plan_ref const& checked_state) {
  require_completed_chart_spr_state_bootstrap(
      state, "chart SPR local score");
  checked_state.assert_same(state.grammar, state.execution_plan);
  if (state.local_commit_inside_rows.valid()) {
    // The pattern payload is immutable for this complete scoring operation.
    // Fingerprint it once at the reader boundary, never once per candidate or
    // candidate-pattern tile.
    state.local_commit_inside_rows.assert_compatible(state.execution_plan,
                                                     state.active_patterns);
  }
  if (candidates.size() != results.size()) {
    throw std::invalid_argument(
        "chart SPR local score: candidate/result span size mismatch");
  }

  auto const resolved_workers =
      scheduler == nullptr
          ? std::size_t{1}
          : scheduler->worker_resolution().resolved_workers;
  // A same-scheduler nested operation inherits its outer stable slot, which
  // can exceed this operation's range/candidate count.  Keep one workspace per
  // resolved slot so the advertised nested fallback can never index past the
  // scratch array.  The source-compatible worker-count wrapper caps its
  // temporary scheduler to candidates.size(), while the persistent search is
  // bounded by its explicit/auto worker budget.
  auto const effective_worker_count =
      candidates.empty() ? 0 : resolved_workers;
  auto const lazy_resident_cache =
      state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart;
  auto const lazy_task_count =
      candidates.empty() ? 0
                         : std::min(effective_worker_count, candidates.size());
  auto const resident_cache =
      state.cache_strategy == chart_spr_cache_strategy::all_active_patterns ||
      lazy_resident_cache || state.local_commit_inside_rows.valid();
  auto const tile_plan =
      plan_local_candidate_pattern_tiles(state, candidates, scheduler);
  auto const pattern_batch_fusion =
      plan_local_pattern_batch_fusion(state, candidates.size(), scheduler);
  auto const prepared_count =
      lazy_resident_cache ? lazy_task_count
      : resident_cache
          ? (tile_plan.enabled() ? candidates.size() : effective_worker_count)
          : candidates.size();
  auto const workspace_worker_count =
      lazy_resident_cache ? lazy_task_count : effective_worker_count;
  auto operation_options = options;
  if (lazy_resident_cache) {
    auto const admission_budget =
        effective_lazy_local_admission_budget_bytes(state, options);
    if (admission_budget != 0) {
      if (scheduler != nullptr) {
        operation_options.admission_additional_resident_bytes =
            local_capacity_checked_add(
                operation_options.admission_additional_resident_bytes,
                estimate_chart_spr_scheduler_resident_bytes(*scheduler),
                "chart SPR lazy-local scheduler ownership");
      }
      std::size_t candidate_input_resident = 0;
      if (!options
               .admission_additional_resident_includes_candidate_inputs) {
        candidate_input_resident =
            estimate_lazy_local_candidate_input_resident_bytes(candidates);
      }
      std::size_t output_current = 0;
      if (!options
               .admission_additional_resident_includes_result_objects) {
        output_current = local_capacity_checked_multiply(
            results.size(), sizeof(chart_spr_local_score_result),
            "chart SPR lazy-local direct result objects");
      }
      std::size_t output_replacement = 0;
      auto const reason_bound =
          lazy_local_invalid_reason_owned_capacity_bound();
      for (auto const& result : results) {
        auto const current =
            local_owned_dynamic_capacity_bytes(result.invalid_reason);
        output_current = local_capacity_checked_add(
            output_current, current,
            "chart SPR lazy-local direct result capacity");
        if (result.invalid_reason.capacity() <
            lazy_local_invalid_reason_max_size) {
          output_replacement = local_capacity_checked_add(
              output_replacement, reason_bound,
              "chart SPR lazy-local direct result replacement");
        }
      }
      auto resident_before_begin = local_capacity_checked_add(
          estimate_chart_spr_published_state_resident_bytes(state),
          operation_options.admission_additional_resident_bytes,
          "chart SPR lazy-local outer shared resident capacity");
      resident_before_begin = local_capacity_checked_add(
          resident_before_begin,
          local_score_workspace_access::finite_outer_bound_peak_capacity_bytes(
              workspace, lazy_task_count),
          "chart SPR lazy-local outer replacement peak");
      resident_before_begin = local_capacity_checked_add(
          resident_before_begin, candidate_input_resident,
          "chart SPR lazy-local candidate input resident peak");
      resident_before_begin = local_capacity_checked_add(
          resident_before_begin, output_current,
          "chart SPR lazy-local output resident peak");
      resident_before_begin = local_capacity_checked_add(
          resident_before_begin, output_replacement,
          "chart SPR lazy-local output replacement peak");
      if (resident_before_begin > admission_budget) {
        ++state.counters.lazy_local_pre_submit_budget_failures;
        throw chart_spr_lazy_local_budget_error(0, resident_before_begin,
                                                admission_budget);
      }
      // Publication later copies only policy-bounded genuine candidate
      // diagnostics. Reserve every destination before begin()/submission so a
      // failure cannot expose a partially published result span.
      for (auto& result : results) {
        result.invalid_reason.reserve(lazy_local_invalid_reason_max_size);
      }
      std::size_t output_resident = 0;
      if (!options
               .admission_additional_resident_includes_result_objects) {
        output_resident = local_capacity_checked_multiply(
            results.size(), sizeof(chart_spr_local_score_result),
            "chart SPR lazy-local direct result objects");
      }
      for (auto const& result : results) {
        output_resident = local_capacity_checked_add(
            output_resident,
            local_owned_dynamic_capacity_bytes(result.invalid_reason),
            "chart SPR lazy-local direct result capacity");
      }
      state.counters.lazy_local_result_output_resident_bytes_max = std::max(
          state.counters.lazy_local_result_output_resident_bytes_max,
          output_resident);
      operation_options.admission_additional_resident_bytes =
          local_capacity_checked_add(
              local_capacity_checked_add(
                  operation_options.admission_additional_resident_bytes,
                  candidate_input_resident,
                  "chart SPR lazy-local candidate input resident capacity"),
              output_resident, "chart SPR lazy-local output resident capacity");
    }
    local_score_workspace_access::bound_lazy_task_slots(
        workspace, lazy_task_count, admission_budget != 0);
    if (admission_budget != 0) {
      auto resident_after_bound = local_capacity_checked_add(
          estimate_chart_spr_published_state_resident_bytes(state),
          operation_options.admission_additional_resident_bytes,
          "chart SPR lazy-local bounded shared resident capacity");
      resident_after_bound = local_capacity_checked_add(
          resident_after_bound,
          local_score_workspace_access::full_resident_capacity_bytes(workspace),
          "chart SPR lazy-local bounded workspace capacity");
      if (resident_after_bound > admission_budget) {
        ++state.counters.lazy_local_pre_submit_budget_failures;
        throw chart_spr_lazy_local_budget_error(0, resident_after_bound,
                                                admission_budget);
      }
    }
  }
  auto const finite_invalid_reasons =
      use_finite_local_score_diagnostics(state, operation_options);
  if (finite_invalid_reasons) {
    for (auto& result : results) {
      reserve_finite_local_score_invalid_reason(result.invalid_reason);
    }
  }
  local_score_workspace_access::begin(
      workspace, prepared_count, workspace_worker_count,
      lazy_resident_cache
          ? 0
          : std::max(tile_plan.total_tiles, pattern_batch_fusion.total_tiles),
      finite_invalid_reasons);
  try {
    for (auto& result : results) {
      result.lower_bound = {};
      result.affected_clade_count = 0;
      result.local_score_ms = 0.0;
      result.valid = true;
      result.invalid_reason.clear();
    }
    if (lazy_resident_cache &&
        (scheduler != nullptr ||
         effective_lazy_local_admission_budget_bytes(state, options) != 0)) {
      score_candidates_locally_lazy_waves_into(state, candidates, results,
                                               workspace, operation_options,
                                               scheduler, lazy_task_count,
                                               checked_state);
    } else if (resident_cache) {
      score_candidates_locally_all_cache_into(
          state, candidates, results, workspace, options, scheduler,
          effective_worker_count, checked_state, tile_plan);
    } else {
      score_candidates_locally_pattern_batches_into(
          state, candidates, results, workspace, options, scheduler,
          effective_worker_count, checked_state, pattern_batch_fusion);
    }
  } catch (...) {
    local_score_workspace_access::finish(workspace);
    throw;
  }
  local_score_workspace_access::finish(workspace);
}

}  // namespace chart_spr_search_detail

// Caller-owned Phase-3 scoring boundary.  The candidate and result spans are
// borrowed only for the duration of this call; the reusable workspace is clean
// both before entry and after every normal or exceptional exit.  Production
// search callers pass their one search-lifetime scheduler here.
inline void score_candidates_locally_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, chart_scheduler& scheduler,
    checked_chart_execution_plan_ref const& checked_state) {
  chart_spr_search_detail::score_candidates_locally_into_impl(
      state, candidates, results, workspace, options, &scheduler,
      checked_state);
}

// Source-compatible worker-count boundary.  Explicit W1 deliberately executes
// the Phase-2 serial implementation directly: constructing/type-erasing a
// temporary scheduler here would violate the warmed zero-allocation contract.
// Other direct calls receive one temporary scheduler for this public operation;
// the full search overload above retains one scheduler across all batches.
inline void score_candidates_locally_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, std::size_t worker_count,
    checked_chart_execution_plan_ref const& checked_state) {
  if (candidates.empty() || worker_count == 1) {
    chart_spr_search_detail::score_candidates_locally_into_impl(
        state, candidates, results, workspace, options, nullptr,
        checked_state);
    return;
  }
  // Preserve the legacy checked boundary's exception ordering: invalid state,
  // plan identity, or span sizes fail before scheduler allocation/thread
  // setup.  The implementation repeats these O(1) guards after construction
  // so scheduler-taking callers retain one self-contained checked boundary.
  chart_spr_search_detail::require_completed_chart_spr_state_bootstrap(
      state, "chart SPR local score");
  checked_state.assert_same(state.grammar, state.execution_plan);
  if (candidates.size() != results.size()) {
    throw std::invalid_argument(
        "chart SPR local score: candidate/result span size mismatch");
  }
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      chart_spr_search_detail::effective_lazy_local_admission_budget_bytes(
          state, options) != 0) {
    throw chart_spr_search_detail::chart_spr_lazy_local_finite_api_error(
        chart_spr_search_detail::chart_spr_lazy_local_finite_api::
            checked_worker_count_parallel_into);
  }
  auto const resolved =
      chart_spr_search_detail::normalize_chart_spr_worker_count(worker_count);
  auto const effective = std::min(resolved, candidates.size());
  if (effective <= 1) {
    chart_spr_search_detail::score_candidates_locally_into_impl(
        state, candidates, results, workspace, options, nullptr,
        checked_state);
    return;
  }
  chart_scheduler scheduler{
      chart_spr_search_detail::chart_spr_compatibility_scheduler_options(
          effective)};
  score_candidates_locally_into(state, candidates, results, workspace, options,
                                scheduler, checked_state);
  scheduler.shutdown();
}

inline void score_candidates_locally_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, chart_scheduler& scheduler) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      chart_spr_search_detail::effective_lazy_local_admission_budget_bytes(
          state, options) != 0) {
    throw chart_spr_search_detail::chart_spr_lazy_local_finite_api_error(
        chart_spr_search_detail::chart_spr_lazy_local_finite_api::
            unchecked_scheduler_into);
  }
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    score_candidates_locally_into(state, candidates, results, workspace,
                                  options, scheduler, checked);
  } catch (chart_execution_plan_mismatch const&) {
    ++state.counters.plan_mismatch_rejections;
    throw;
  }
}

inline void score_candidates_locally_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options = {}, std::size_t worker_count = 1) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      chart_spr_search_detail::effective_lazy_local_admission_budget_bytes(
          state, options) != 0) {
    throw chart_spr_search_detail::chart_spr_lazy_local_finite_api_error(
        chart_spr_search_detail::chart_spr_lazy_local_finite_api::
            unchecked_worker_count_into);
  }
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    score_candidates_locally_into(state, candidates, results, workspace,
                                  options, worker_count, checked);
  } catch (chart_execution_plan_mismatch const&) {
    ++state.counters.plan_mismatch_rejections;
    throw;
  }
}

inline chart_spr_candidate_score promote_chart_spr_local_score(
    grammar_spr_candidate const& candidate,
    chart_spr_local_score_result const& local) {
  chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.lower_bound = local.lower_bound;
  scored.affected_clade_count = local.affected_clade_count;
  scored.local_score_ms = local.local_score_ms;
  scored.valid = local.valid;
  scored.invalid_reason = local.invalid_reason;
  return scored;
}

// Owning compatibility API.  Candidate promotion is deliberately outside the
// `_into` operation so the measured/reusable region never deep-copies a
// grammar_spr_candidate or constructs Phase-4/5 candidate payload.
inline std::vector<chart_spr_candidate_score> score_candidates_locally(
    chart_spr_search_state const& state,
    std::vector<grammar_spr_candidate> const& candidates,
    local_spr_score_options const& options, std::size_t worker_count,
    checked_chart_execution_plan_ref const& checked_state) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      chart_spr_search_detail::effective_lazy_local_admission_budget_bytes(
          state, options) != 0) {
    throw chart_spr_search_detail::chart_spr_lazy_local_finite_api_error(
        chart_spr_search_detail::chart_spr_lazy_local_finite_api::
            checked_owning);
  }
  std::vector<chart_spr_local_score_result> local_results(candidates.size());
  chart_spr_local_score_workspace workspace;
  score_candidates_locally_into(state, candidates, local_results, workspace,
                                options, worker_count, checked_state);
  std::vector<chart_spr_candidate_score> scores;
  scores.reserve(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    auto promotion_start = std::chrono::steady_clock::now();
    scores.push_back(
        promote_chart_spr_local_score(candidates[i], local_results[i]));
    scores.back().local_score_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - promotion_start)
            .count();
  }
  return scores;
}

inline std::vector<chart_spr_candidate_score> score_candidates_locally(
    chart_spr_search_state const& state,
    std::vector<grammar_spr_candidate> const& candidates,
    local_spr_score_options const& options = {},
    std::size_t worker_count = 1) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      chart_spr_search_detail::effective_lazy_local_admission_budget_bytes(
          state, options) != 0) {
    throw chart_spr_search_detail::chart_spr_lazy_local_finite_api_error(
        chart_spr_search_detail::chart_spr_lazy_local_finite_api::
            unchecked_owning);
  }
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    return score_candidates_locally(state, candidates, options, worker_count,
                                    checked);
  } catch (chart_execution_plan_mismatch const&) {
    ++state.counters.plan_mismatch_rejections;
    throw;
  }
}

// Phase-3 local scoring entry point for one candidate.  Pattern-batch cache
// mode intentionally requires the batch scorer so callers do not accidentally
// rebuild pattern chart batches once per candidate.
inline chart_spr_candidate_score score_candidate_locally(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    local_spr_score_options const& options = {}) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      chart_spr_search_detail::effective_lazy_local_admission_budget_bytes(
          state, options) != 0) {
    throw chart_spr_search_detail::chart_spr_lazy_local_finite_api_error(
        chart_spr_search_detail::chart_spr_lazy_local_finite_api::
            unchecked_singleton);
  }
  if (state.cache_strategy == chart_spr_cache_strategy::pattern_batches) {
    throw std::runtime_error(
        "chart SPR local score: score_candidate_locally is disabled for "
        "pattern-batch cache mode; use score_candidates_locally with a "
        "bounded candidate batch");
  }
  chart_spr_local_score_result local;
  chart_spr_local_score_workspace workspace;
  score_candidates_locally_into(
      state, std::span<grammar_spr_candidate const>{&candidate, 1},
      std::span<chart_spr_local_score_result>{&local, 1}, workspace, options,
      1);
  auto promotion_start = std::chrono::steady_clock::now();
  auto scored = promote_chart_spr_local_score(candidate, local);
  scored.local_score_ms +=
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - promotion_start)
          .count();
  return scored;
}

inline affected_clade_distribution summarize_affected_clade_counts(
    std::vector<std::size_t> counts) {
  affected_clade_distribution summary;
  if (counts.empty()) return summary;
  std::sort(counts.begin(), counts.end());
  std::uint64_t sum = 0;
  for (auto count : counts) sum += count;
  summary.mean = static_cast<double>(sum) / static_cast<double>(counts.size());
  summary.p50 = counts[counts.size() / 2];
  auto p95_index = (counts.size() * 95 + 99) / 100;
  if (p95_index == 0) p95_index = 1;
  summary.p95 = counts[p95_index - 1];
  summary.max = counts.back();
  return summary;
}

inline void record_chart_spr_candidate_generation_stats(
    chart_spr_candidate_generation_stats const& stats,
    chart_spr_search_counters& counters) {
  counters.upward_path_iterator_steps += stats.upward_path_iterator_steps;
  counters.upward_paths_completed += stats.upward_paths_completed;
  counters.path_pairs_considered += stats.path_pairs_considered;
  counters.candidates_constructed += stats.candidates_constructed;
  counters.candidates_pruned_before_construction +=
      stats.candidates_pruned_before_construction;
  counters.candidates_pruned_after_construction +=
      stats.candidates_pruned_after_construction;
  counters.candidates_generated_after_dedup +=
      stats.candidates_generated_after_dedup;
  counters.candidates_pruned_root_or_trivial +=
      stats.candidates_pruned_root_or_trivial;
  counters.candidates_pruned_moved_size += stats.candidates_pruned_moved_size;
  counters.candidates_pruned_target_size += stats.candidates_pruned_target_size;
  counters.candidates_pruned_overlap += stats.candidates_pruned_overlap;
  counters.candidates_pruned_affected_estimate +=
      stats.candidates_pruned_affected_estimate;
  counters.candidates_pruned_immediate_reversal +=
      stats.candidates_pruned_immediate_reversal;
  counters.candidates_pruned_duplicate += stats.candidates_pruned_duplicate;
  counters.candidates_pruned_invalid += stats.candidates_pruned_invalid;
  counters.spr_multifurcation_moves_generated +=
      stats.spr_multifurcation_moves_generated;
  if (stats.stop_reason == chart_spr_candidate_stop_reason::candidate_cap) {
    ++counters.candidate_cap_cutoffs;
  }
  if (stats.stop_reason == chart_spr_candidate_stop_reason::path_budget) {
    ++counters.path_budget_cutoffs;
  }
  counters.candidate_pipeline_batches_generated +=
      stats.candidate_pipeline_batches_generated;
  counters.candidate_pipeline_batches_scored +=
      stats.candidate_pipeline_batches_scored;
  counters.candidate_pipeline_serial_overlap_batches +=
      stats.candidate_pipeline_serial_overlap_batches;
  counters.candidate_pipeline_scheduler_projection_overlap_batches +=
      stats.candidate_pipeline_scheduler_projection_overlap_batches;
  counters.candidate_pipeline_producer_stalls +=
      stats.candidate_pipeline_producer_stalls;
  counters.candidate_pipeline_consumer_stalls +=
      stats.candidate_pipeline_consumer_stalls;
  counters.candidate_pipeline_producer_stall_nanoseconds +=
      stats.candidate_pipeline_producer_stall_nanoseconds;
  counters.candidate_pipeline_consumer_stall_nanoseconds +=
      stats.candidate_pipeline_consumer_stall_nanoseconds;
  counters.candidate_pipeline_cancellations +=
      stats.candidate_pipeline_cancellations;
  counters.candidate_pipeline_stale_batches_discarded +=
      stats.candidate_pipeline_stale_batches_discarded;
  counters.candidate_pipeline_stale_candidates_discarded +=
      stats.candidate_pipeline_stale_candidates_discarded;
  counters.candidate_pipeline_state_epoch_rejections +=
      stats.candidate_pipeline_state_epoch_rejections;
  counters.candidate_pipeline_generation_errors +=
      stats.candidate_pipeline_generation_errors;
  counters.candidate_pipeline_estimated_peak_bytes =
      std::max(counters.candidate_pipeline_estimated_peak_bytes,
               stats.candidate_pipeline_estimated_peak_bytes);
}

inline std::uint64_t chart_spr_add_invariant_offset(
    std::uint64_t active_score, chart_spr_search_state const& state,
    std::string const& label) {
  return chart_multisite_detail::checked_add_u64(
      active_score, state.invariant_constant_offset, label);
}

inline chart_spr_objective_score make_chart_spr_objective_score(
    spr_score_result value, chart_spr_score_kind kind,
    chart_spr_score_convention convention,
    std::uint64_t invariant_offset_applied = 0) {
  chart_spr_objective_score score;
  score.value = value;
  score.kind = kind;
  score.convention = convention;
  score.invariant_offset_applied = invariant_offset_applied;
  return score;
}

inline multisite_trim_result const& ensure_chart_spr_state_exact_trim(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    multisite_trim_options const& trim_options = {}) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  checked_state.assert_same(state.grammar, state.execution_plan);
  if (!state.exact_trim_active_only) {
    require_chart_spr_state_exact_memory_budget(state, trim_options, 1);
    auto built_trim =
        build_chart_spr_state_exact_trim(state, checked_state, trim_options);
    require_chart_spr_retained_exact_state_memory_budget(state, built_trim);
    state.exact_trim_active_only = std::move(built_trim);
    ++state.counters.chart_execution_plan_cache_hits;
  }
  return *state.exact_trim_active_only;
}

inline multisite_trim_result const& ensure_chart_spr_state_exact_trim(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options = {}) {
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    return ensure_chart_spr_state_exact_trim(state, checked, trim_options);
  } catch (chart_execution_plan_mismatch const&) {
    ++state.counters.plan_mismatch_rejections;
    throw;
  }
}

inline multisite_trim_result const& ensure_chart_spr_state_exact_trim(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    chart_scheduler& scheduler,
    multisite_trim_options const& trim_options = {}) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  checked_state.assert_same(state.grammar, state.execution_plan);
  if (!state.exact_trim_active_only) {
    require_chart_spr_state_exact_memory_budget(state, trim_options, scheduler);
    auto built_trim = build_chart_spr_state_exact_trim(
        state, checked_state, scheduler, trim_options);
    require_chart_spr_retained_exact_state_memory_budget(state, built_trim,
                                                         scheduler);
    state.exact_trim_active_only = std::move(built_trim);
    ++state.counters.chart_execution_plan_cache_hits;
  }
  return *state.exact_trim_active_only;
}

inline std::uint64_t chart_spr_state_exact_score_with_invariants(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    multisite_trim_options const& trim_options = {}) {
  auto const& trim =
      ensure_chart_spr_state_exact_trim(state, checked_state, trim_options);
  return chart_spr_add_invariant_offset(
      trim.optimum, state, "chart-SPR exact state invariant offset");
}

inline std::uint64_t chart_spr_state_exact_score_with_invariants(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options = {}) {
  auto const& trim = ensure_chart_spr_state_exact_trim(state, trim_options);
  return chart_spr_add_invariant_offset(
      trim.optimum, state, "chart-SPR exact state invariant offset");
}

// Phase-4 exact verification gate (the COLD / from-scratch path).  This
// intentionally does not call the legacy diagnostic exact multisite helper: the
// current state's old exact active-pattern score is read from (or lazily built
// into) state.exact_trim_active_only, and only the candidate's new materialized
// overlay grammar is trimmed here.  Overlay materialization is counted in the
// exact-verification bucket, not as local scoring and not as accepted-state
// sidecar rebuilding.
inline chart_spr_candidate_score verify_candidate_exact_against_state_impl(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_exact_verification_context& context,
    multisite_trim_options const& trim_options = {}) {
  if (!candidate.valid) return candidate;
  auto& counters = context.counters;
  auto* scheduler = context.inner_scheduler;
  ++counters.exact_verifications;

  planned_overlay_materialization_result planned;
  multisite_trim_result new_trim;
  multisite_trim_scheduler_run_summaries scheduler_runs;
  chart_spr_scheduler_run_axis_publisher publish_setup_runs{
      counters.scheduler_axes.exact_setup_patterns, scheduler_runs.exact_setup};
  chart_spr_scheduler_run_axis_publisher publish_frontier_runs{
      counters.scheduler_axes.exact_frontier_clades,
      scheduler_runs.frontier_clades};
  bool dense_materialization_completed = false;
  bool materialization_counted = false;
  bool payload_validation_counted = false;
  overlay_payload_validation_stats completed_payload_validation_stats;
  try {
    auto const& old_trim =
        context.published_old_trim != nullptr ? *context.published_old_trim
        : scheduler != nullptr
            ? ensure_chart_spr_state_exact_trim(state, checked_state,
                                                *scheduler, trim_options)
            : ensure_chart_spr_state_exact_trim(state, checked_state,
                                                trim_options);
    {
      chart_spr_elapsed_accumulator materialization_timer{
          counters.materialization_exact_verification_ms};
      if (candidate.force_exact_materializer_failure_for_tests) {
        throw std::runtime_error(
            "forced exact materializer failure for tests");
      }
      planned = materialize_candidate_overlay_grammar_with_plan(
          state.grammar, checked_state, candidate.candidate,
          &dense_materialization_completed,
          [&] { materialization_timer.finish(); },
          &completed_payload_validation_stats);
    }
    ++counters.full_overlay_materializations;
    ++counters.overlay_materializations_for_exact_verification;
    materialization_counted = true;
    record_planned_overlay_materialization_stats(counters, planned);
    payload_validation_counted = true;

    if (state.cache_strategy ==
        chart_spr_cache_strategy::lazy_multisite_chart) {
      chart_spr_force_candidate_exact_bnb_overflow_for_tests(candidate);
      new_trim = build_lazy_multisite_trim_active_from_scratch(
          planned, state.active_patterns, state.chart_opts, trim_options);
    } else if (scheduler != nullptr) {
      state.active_patterns.assert_no_skipped_invariant_metadata();
      chart_spr_force_candidate_exact_bnb_overflow_for_tests(candidate);
      new_trim = build_multisite_trim(
          planned.execution_plan, state.active_patterns.patterns, *scheduler,
          state.chart_opts, trim_options, &scheduler_runs);
    } else {
      chart_spr_force_candidate_exact_bnb_overflow_for_tests(candidate);
      new_trim = build_multisite_trim_active(planned.execution_plan,
                                             state.active_patterns,
                                             state.chart_opts, trim_options);
    }
    if (state.cache_strategy ==
        chart_spr_cache_strategy::lazy_multisite_chart) {
      ++counters.exact_trim_lazy_chart_uses;
    }
    record_multisite_exact_trim_work(counters, new_trim);
    ++counters.chart_execution_plan_cache_hits;

    auto old_full = chart_spr_add_invariant_offset(
        old_trim.optimum, state,
        "chart-SPR exact old-score invariant offset");
    auto new_full = chart_spr_add_invariant_offset(
        new_trim.optimum, state,
        "chart-SPR exact new-score invariant offset");
    candidate.exact = make_chart_spr_objective_score(
        spr_score_result{chart_spr_detail::signed_delta(old_full, new_full),
                         old_full, new_full, true},
        chart_spr_score_kind::grammar_exact,
        chart_spr_score_convention::full_with_invariants,
        state.invariant_constant_offset);
  } catch (chart_scheduler_submit_error const&) {
    // Scheduler infrastructure failure is not a biological/semantic invalid
    // candidate. Propagate it so the search cannot continue with an
    // unclassified scheduler operation or silently reduced exactness.
    throw;
  } catch (multisite_optimal_root_provenance_capture_error const&) {
    // Root provenance is report-only even when captured by the primary B&B.
    // Its failure must abort semantic capture, never select a different
    // candidate by converting this one into an ordinary rejection.
    throw;
  } catch (std::bad_alloc const&) {
    throw;
  } catch (std::overflow_error const&) {
    // Score/size arithmetic overflow invalidates the computation, not one
    // candidate.  Never turn it into an ordinary invalid-candidate outcome.
    throw;
  } catch (std::logic_error const&) {
    // Includes scheduler lifecycle/concurrent-use failures.  These are
    // infrastructure/programming errors, not candidate invalidity.
    throw;
  } catch (std::exception const& e) {
    // Before this refactor the dense materializer returned (and its success
    // counters advanced) before the separately built plan could reject the
    // output.  Preserve that reason-coded accounting for the combined call.
    if (dense_materialization_completed && !materialization_counted) {
      ++counters.full_overlay_materializations;
      ++counters.overlay_materializations_for_exact_verification;
    }
    if (!payload_validation_counted) {
      record_overlay_payload_validation_stats(
          counters, completed_payload_validation_stats);
    }
    candidate.valid = false;
    candidate.invalid_reason = e.what();
  }
  // Canonical evidence is report-only.  Construct it outside the verifier's
  // semantic catch so a report bug cannot silently invalidate a candidate or
  // change acceptance; it must abort the correctness-only canonical run.
  if (candidate.valid && candidate.exact &&
      candidate.canonical_stream_index !=
          (std::numeric_limits<std::size_t>::max)()) {
    chart_spr_force_canonical_evidence_failure_for_tests(candidate);
    candidate.canonical_exact_evidence =
        std::make_shared<chart_spr_canonical_exact_evidence>(
            chart_spr_canonicalize_search_trim_evidence(
                planned, state.active_patterns, state.chart_opts,
                trim_options, new_trim,
                state.invariant_constant_offset));
    if (new_trim.keep_production_exact) {
      ++counters.chart_execution_plan_cache_hits;
    }
  }
  if (candidate.valid && candidate.exact &&
      state.retain_verified_exact_trim_for_local_commit) {
    candidate.reusable_exact_trim = make_chart_spr_reusable_exact_trim(
        planned.execution_plan, state.active_patterns, state.chart_opts,
        trim_options, state.invariant_constant_offset, std::move(new_trim));
  }
  return candidate;
}

inline chart_spr_candidate_score verify_candidate_exact_against_state(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    multisite_trim_options const& trim_options = {}) {
  chart_spr_exact_verification_context context{state.counters};
  return verify_candidate_exact_against_state_impl(
      state, std::move(candidate), checked_state, context, trim_options);
}

inline chart_spr_candidate_score verify_candidate_exact_against_state(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    chart_scheduler& scheduler,
    multisite_trim_options const& trim_options = {}) {
  chart_spr_exact_verification_context context{state.counters, nullptr,
                                               &scheduler};
  return verify_candidate_exact_against_state_impl(
      state, std::move(candidate), checked_state, context, trim_options);
}

inline chart_spr_candidate_score verify_candidate_exact_against_state(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    multisite_trim_options const& trim_options = {}) {
  if (!candidate.valid) return candidate;
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    return verify_candidate_exact_against_state(
        state, std::move(candidate), checked, trim_options);
  } catch (chart_execution_plan_mismatch const& e) {
    ++state.counters.exact_verifications;
    ++state.counters.plan_mismatch_rejections;
    candidate.valid = false;
    candidate.invalid_reason = e.what();
    return candidate;
  }
}

inline bool chart_spr_topology_selection_has_certificate_or_selector(
    chart_spr_topology_selection const& selection) {
  if (selection.kind == chart_spr_topology_selection_kind::none) return false;
  if (selection.certificate) return true;
  return !selection.selector_name.empty();
}

inline std::vector<production_id> chart_spr_base_production_ids_from_refs(
    std::vector<overlay_production_ref> const& refs) {
  std::vector<production_id> ids;
  ids.reserve(refs.size());
  for (auto ref : refs) {
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(
          "fixed_topology_exact certificate: before-topology production "
          "must refer to the base grammar");
    }
    ids.push_back(ref.id);
  }
  return ids;
}

inline production_id chart_spr_dense_production_id_for_ref(
    overlay_materialization_result const& materialized,
    overlay_production_ref ref) {
  production_id dense = no_production;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_production ||
        ref.id >= materialized.base_production_to_dense.size()) {
      throw std::runtime_error(
          "fixed_topology_exact certificate: base production ref out of "
          "materialized range");
    }
    dense = materialized.base_production_to_dense[ref.id];
  } else {
    if (ref.id == no_production ||
        ref.id >= materialized.temp_production_to_dense.size()) {
      throw std::runtime_error(
          "fixed_topology_exact certificate: temp production ref out of "
          "materialized range");
    }
    dense = materialized.temp_production_to_dense[ref.id];
  }
  if (dense == no_production ||
      dense >= materialized.grammar.productions.size()) {
    throw std::runtime_error(
        "fixed_topology_exact certificate: selected after-topology "
        "production is not reachable in the materialized overlay grammar");
  }
  return dense;
}

inline void validate_chart_spr_topology_certificate_signatures(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    chart_spr_topology_certificate const& certificate) {
  if (certificate.before_signatures.size() !=
      certificate.before_overlay_productions.size()) {
    throw std::runtime_error(
        "fixed_topology_exact certificate: before signature count does not "
        "match before production refs");
  }
  for (std::size_t i = 0; i < certificate.before_signatures.size(); ++i) {
    auto expected = chart_spr_production_signature_for_ref(
        base, candidate, certificate.before_overlay_productions[i]);
    if (!(certificate.before_signatures[i] == expected)) {
      throw std::runtime_error(
          "fixed_topology_exact certificate: before production signature "
          "does not match its overlay ref");
    }
  }
  if (certificate.after_signatures.size() !=
      certificate.after_overlay_productions.size()) {
    throw std::runtime_error(
        "fixed_topology_exact certificate: after signature count does not "
        "match after production refs");
  }
  for (std::size_t i = 0; i < certificate.after_signatures.size(); ++i) {
    auto expected = chart_spr_production_signature_for_ref(
        base, candidate, certificate.after_overlay_productions[i]);
    if (!(certificate.after_signatures[i] == expected)) {
      throw std::runtime_error(
          "fixed_topology_exact certificate: after production signature "
          "does not match its overlay ref");
    }
  }
}

inline bool chart_spr_candidate_removes_base_production(
    grammar_spr_candidate const& candidate, production_id pid) {
  for (auto ref : candidate.removed_productions) {
    if (ref.space == overlay_id_space::base && ref.id == pid) return true;
  }
  return false;
}

inline overlay_clade_ref chart_spr_overlay_production_parent(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_production_ref ref) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_production || ref.id >= base.productions.size()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: base production ref out of range");
    }
    return base_clade_ref(base.productions[ref.id].parent);
  }
  if (ref.id == no_production || ref.id >= candidate.added_productions.size()) {
    throw std::runtime_error(
        "fixed_topology_exact selector: temp production ref out of range");
  }
  return candidate.added_productions[ref.id].parent;
}

inline std::vector<overlay_clade_ref> chart_spr_overlay_production_children(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_production_ref ref) {
  std::vector<overlay_clade_ref> children;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_production || ref.id >= base.productions.size()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: base production ref out of range");
    }
    auto const& prod = base.productions[ref.id];
    children.reserve(prod.children.size());
    for (auto child : prod.children) children.push_back(base_clade_ref(child));
    return children;
  }
  if (ref.id == no_production || ref.id >= candidate.added_productions.size()) {
    throw std::runtime_error(
        "fixed_topology_exact selector: temp production ref out of range");
  }
  return candidate.added_productions[ref.id].children;
}

inline void validate_chart_spr_selected_overlay_production_for_fixed_topology(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_production_ref ref, std::string const& context) {
  auto parent = chart_spr_overlay_production_parent(base, candidate, ref);
  auto children = chart_spr_overlay_production_children(base, candidate, ref);
  if (children.size() < 2) {
    throw std::runtime_error(context +
                             ": selected production has fewer than 2 "
                             "children");
  }

  auto parent_taxa = chart_spr_clade_taxa_for_ref(base, candidate, parent);
  std::sort(parent_taxa.begin(), parent_taxa.end());
  std::vector<taxon_id> covered;
  for (auto child : children) {
    auto child_taxa = chart_spr_clade_taxa_for_ref(base, candidate, child);
    std::sort(child_taxa.begin(), child_taxa.end());
    if (child_taxa.empty()) {
      throw std::runtime_error(context +
                               ": selected production child has no taxa");
    }
    if (child_taxa.size() >= parent_taxa.size()) {
      throw std::runtime_error(
          context + ": selected production child is not smaller than parent");
    }
    if (!std::includes(parent_taxa.begin(), parent_taxa.end(),
                       child_taxa.begin(), child_taxa.end())) {
      throw std::runtime_error(
          context + ": selected production child is not a subset of parent");
    }
    std::vector<taxon_id> overlap;
    std::set_intersection(covered.begin(), covered.end(),
                          child_taxa.begin(), child_taxa.end(),
                          std::back_inserter(overlap));
    if (!overlap.empty()) {
      throw std::runtime_error(
          context + ": selected production children overlap");
    }
    std::vector<taxon_id> next;
    std::set_union(covered.begin(), covered.end(), child_taxa.begin(),
                   child_taxa.end(), std::back_inserter(next));
    covered = std::move(next);
  }
  if (covered != parent_taxa) {
    throw std::runtime_error(
        context + ": selected production children do not union to parent");
  }
}

inline bool chart_spr_try_collect_first_base_topology_refs(
    clade_grammar const& base, clade_id clade,
    std::vector<std::uint8_t>& clade_state,
    std::vector<overlay_production_ref>& refs) {
  if (clade == no_clade || clade >= base.clades.size()) {
    throw std::runtime_error(
        "fixed_topology_exact selector: base clade out of range");
  }
  if (clade_state[clade] != 0) return false;
  clade_state[clade] = 1;

  if (base.clades[clade].taxa.size() == 1) {
    if (!base.productions_by_parent[clade].empty()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: singleton clade has productions");
    }
    clade_state[clade] = 2;
    return true;
  }

  auto choices = base.productions_by_parent[clade];
  std::sort(choices.begin(), choices.end());
  for (auto pid : choices) {
    if (pid == no_production || pid >= base.productions.size()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: base production out of range");
    }
    auto const& prod = base.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "fixed_topology_exact selector: production parent mismatch");
    }
    auto state_snapshot = clade_state;
    auto ref_size = refs.size();
    refs.push_back(base_production_ref(pid));
    bool ok = true;
    for (auto child : prod.children) {
      if (!chart_spr_try_collect_first_base_topology_refs(
              base, child, clade_state, refs)) {
        ok = false;
        break;
      }
    }
    if (ok) {
      clade_state[clade] = 2;
      return true;
    }
    refs.resize(ref_size);
    clade_state = std::move(state_snapshot);
  }

  clade_state[clade] = 0;
  return false;
}

inline std::vector<overlay_production_ref> chart_spr_first_base_topology_refs(
    clade_grammar const& base) {
  std::vector<std::uint8_t> clade_state(base.clades.size(), 0);
  std::vector<overlay_production_ref> refs;
  if (!chart_spr_try_collect_first_base_topology_refs(
          base, base.root_clade, clade_state, refs)) {
    throw std::runtime_error(
        "fixed_topology_exact selector: no complete base topology found");
  }
  return refs;
}

inline std::uint8_t chart_spr_overlay_clade_state(
    std::vector<std::uint8_t> const& base_state,
    std::vector<std::uint8_t> const& temp_state, overlay_clade_ref ref) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= base_state.size()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: base clade state out of range");
    }
    return base_state[ref.id];
  }
  if (ref.id == no_clade || ref.id >= temp_state.size()) {
    throw std::runtime_error(
        "fixed_topology_exact selector: temp clade state out of range");
  }
  return temp_state[ref.id];
}

inline void chart_spr_set_overlay_clade_state(
    std::vector<std::uint8_t>& base_state,
    std::vector<std::uint8_t>& temp_state, overlay_clade_ref ref,
    std::uint8_t value) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= base_state.size()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: base clade state out of range");
    }
    base_state[ref.id] = value;
    return;
  }
  if (ref.id == no_clade || ref.id >= temp_state.size()) {
    throw std::runtime_error(
        "fixed_topology_exact selector: temp clade state out of range");
  }
  temp_state[ref.id] = value;
}

inline std::vector<overlay_production_ref>
chart_spr_available_overlay_productions_for_parent(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_clade_ref parent) {
  std::vector<overlay_production_ref> choices;

  // Prefer candidate-added productions when building the deterministic
  // after-topology witness.  On tree-like inputs this forces the selected
  // topology through the SPR rewrite rather than silently falling back to an
  // unaffected base alternative when both are reachable in a DAG grammar.
  for (std::size_t i = 0; i < candidate.added_productions.size(); ++i) {
    if (candidate.added_productions[i].parent == parent) {
      choices.push_back(temp_production_ref(static_cast<production_id>(i)));
    }
  }

  if (parent.space == overlay_id_space::base) {
    if (parent.id == no_clade || parent.id >= base.productions_by_parent.size()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: base parent out of range");
    }
    auto base_choices = base.productions_by_parent[parent.id];
    std::sort(base_choices.begin(), base_choices.end());
    for (auto pid : base_choices) {
      if (!chart_spr_candidate_removes_base_production(candidate, pid)) {
        choices.push_back(base_production_ref(pid));
      }
    }
  }
  return choices;
}

inline bool chart_spr_try_collect_first_overlay_topology_refs(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_clade_ref clade, std::vector<std::uint8_t>& base_state,
    std::vector<std::uint8_t>& temp_state,
    std::vector<overlay_production_ref>& refs) {
  (void)chart_spr_clade_taxa_for_ref(base, candidate, clade);
  if (chart_spr_overlay_clade_state(base_state, temp_state, clade) != 0) {
    return false;
  }
  chart_spr_set_overlay_clade_state(base_state, temp_state, clade, 1);

  if (chart_spr_clade_taxa_for_ref(base, candidate, clade).size() == 1) {
    auto choices = chart_spr_available_overlay_productions_for_parent(
        base, candidate, clade);
    if (!choices.empty()) {
      throw std::runtime_error(
          "fixed_topology_exact selector: singleton overlay clade has "
          "productions");
    }
    chart_spr_set_overlay_clade_state(base_state, temp_state, clade, 2);
    return true;
  }

  auto choices = chart_spr_available_overlay_productions_for_parent(
      base, candidate, clade);
  for (auto prod_ref : choices) {
    auto base_snapshot = base_state;
    auto temp_snapshot = temp_state;
    auto ref_size = refs.size();
    refs.push_back(prod_ref);
    bool ok = true;
    for (auto child : chart_spr_overlay_production_children(
             base, candidate, prod_ref)) {
      if (!chart_spr_try_collect_first_overlay_topology_refs(
              base, candidate, child, base_state, temp_state, refs)) {
        ok = false;
        break;
      }
    }
    if (ok) {
      chart_spr_set_overlay_clade_state(base_state, temp_state, clade, 2);
      return true;
    }
    refs.resize(ref_size);
    base_state = std::move(base_snapshot);
    temp_state = std::move(temp_snapshot);
  }

  chart_spr_set_overlay_clade_state(base_state, temp_state, clade, 0);
  return false;
}

inline std::vector<overlay_production_ref>
chart_spr_first_overlay_topology_refs(clade_grammar const& base,
                                      grammar_spr_candidate const& candidate) {
  std::vector<std::uint8_t> base_state(base.clades.size(), 0);
  std::vector<std::uint8_t> temp_state(candidate.added_clades.size(), 0);
  std::vector<overlay_production_ref> refs;
  if (!chart_spr_try_collect_first_overlay_topology_refs(
          base, candidate, base_clade_ref(base.root_clade), base_state,
          temp_state, refs)) {
    throw std::runtime_error(
        "fixed_topology_exact selector: no complete overlay topology found");
  }
  return refs;
}

inline bool chart_spr_builtin_fixed_topology_selector_name(
    std::string const& name) {
  return name.empty() || name == "first" ||
         name == "first-reachable-overlay-topology" ||
         name == "first_reachable_overlay_topology";
}

inline chart_spr_topology_selection
make_chart_spr_builtin_fixed_topology_selection(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::string selector_name = "first_reachable_overlay_topology") {
  if (!chart_spr_builtin_fixed_topology_selector_name(selector_name)) {
    throw std::runtime_error(
        "fixed_topology_exact selector: unsupported deterministic selector '" +
        selector_name + "'");
  }
  if (selector_name.empty() || selector_name == "first" ||
      selector_name == "first-reachable-overlay-topology") {
    selector_name = "first_reachable_overlay_topology";
  }

  chart_spr_topology_selection selection;
  selection.kind = chart_spr_topology_selection_kind::deterministic_selector;
  selection.selector_name = std::move(selector_name);
  selection.certificate = make_chart_spr_topology_certificate(
      base, candidate, chart_spr_first_base_topology_refs(base),
      chart_spr_first_overlay_topology_refs(base, candidate));
  return selection;
}

inline std::optional<chart_spr_topology_selection>
make_chart_spr_source_tree_topology_selection(
    clade_grammar const& base, grammar_spr_candidate const& candidate) {
  if (!candidate.source_before_topology_productions ||
      !candidate.source_after_topology_productions) {
    return std::nullopt;
  }
  chart_spr_topology_selection selection;
  selection.kind = chart_spr_topology_selection_kind::explicit_certificate;
  selection.selector_name = "source_tree_move_certificate";
  selection.certificate = make_chart_spr_topology_certificate(
      base, candidate, *candidate.source_before_topology_productions,
      *candidate.source_after_topology_productions);
  return selection;
}

inline void attach_fixed_topology_selection_for_acceptance(
    chart_spr_search_state const& state, chart_spr_candidate_score& scored,
    chart_spr_search_options const& options) {
  if (!scored.valid || options.acceptance_mode !=
                           chart_spr_acceptance_mode::fixed_topology_exact) {
    return;
  }
  try {
    std::optional<chart_spr_topology_selection> selection;
    if (options.topology_selection_provider) {
      selection = options.topology_selection_provider(state, scored.candidate);
    } else {
      selection = make_chart_spr_source_tree_topology_selection(
          state.grammar, scored.candidate);
      if (!selection) {
        selection = make_chart_spr_builtin_fixed_topology_selection(
            state.grammar, scored.candidate,
            options.fixed_topology_selector_name);
      }
    }
    if (!selection) {
      scored.valid = false;
      scored.invalid_reason =
          "fixed_topology_exact acceptance requires topology selection; "
          "the configured provider returned none";
      return;
    }
    scored.topology_selection = std::move(*selection);
  } catch (std::exception const& e) {
    scored.valid = false;
    scored.invalid_reason = e.what();
  }
}

inline void chart_spr_collect_reachable_selected_overlay_topology(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref clade, std::vector<std::uint8_t>& base_state,
    std::vector<std::uint8_t>& temp_state,
    std::set<overlay_production_ref>& reached,
    std::string const& context) {
  auto state = chart_spr_overlay_clade_state(base_state, temp_state, clade);
  if (state == 1) {
    throw std::runtime_error(context + ": cycle in selected topology");
  }
  if (state == 2) {
    throw std::runtime_error(
        context + ": selected topology reuses a clade in multiple places");
  }
  chart_spr_set_overlay_clade_state(base_state, temp_state, clade, 1);

  auto taxa = chart_spr_clade_taxa_for_ref(base, candidate, clade);
  if (taxa.size() == 1) {
    if (selected.find(clade) != selected.end()) {
      throw std::runtime_error(context +
                               ": selected leaf clade has a production");
    }
    chart_spr_set_overlay_clade_state(base_state, temp_state, clade, 2);
    return;
  }

  auto it = selected.find(clade);
  if (it == selected.end()) {
    throw std::runtime_error(
        context + ": selected topology missing production for non-singleton "
                  "clade");
  }
  auto parent = chart_spr_overlay_production_parent(base, candidate,
                                                    it->second);
  if (parent != clade) {
    throw std::runtime_error(context +
                             ": selected production parent mismatch");
  }
  validate_chart_spr_selected_overlay_production_for_fixed_topology(
      base, candidate, it->second, context);
  reached.insert(it->second);
  for (auto child : chart_spr_overlay_production_children(base, candidate,
                                                          it->second)) {
    chart_spr_collect_reachable_selected_overlay_topology(
        base, candidate, selected, child, base_state, temp_state, reached,
        context);
  }
  chart_spr_set_overlay_clade_state(base_state, temp_state, clade, 2);
}

inline void validate_chart_spr_selected_overlay_topology_complete(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    std::string const& context) {
  std::vector<std::uint8_t> base_state(base.clades.size(), 0);
  std::vector<std::uint8_t> temp_state(candidate.added_clades.size(), 0);
  std::set<overlay_production_ref> reached;
  chart_spr_collect_reachable_selected_overlay_topology(
      base, candidate, selected, base_clade_ref(base.root_clade), base_state,
      temp_state, reached, context);
  for (auto const& [parent, ref] : selected) {
    (void)parent;
    if (reached.find(ref) == reached.end()) {
      throw std::runtime_error(
          context + ": unreachable selected production in topology "
                    "certificate");
    }
  }
}

inline std::map<overlay_clade_ref, overlay_production_ref>
chart_spr_overlay_selected_production_by_parent(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::vector<overlay_production_ref> const& production_refs) {
  std::map<overlay_clade_ref, overlay_production_ref> selected;
  for (auto ref : production_refs) {
    if (ref.space == overlay_id_space::base &&
        chart_spr_candidate_removes_base_production(candidate, ref.id)) {
      throw std::runtime_error(
          "fixed_topology_exact certificate: selected after-topology base "
          "production is removed by the candidate");
    }
    validate_chart_spr_selected_overlay_production_for_fixed_topology(
        base, candidate, ref, "fixed_topology_exact certificate");
    auto parent = chart_spr_overlay_production_parent(base, candidate, ref);
    auto [it, inserted] = selected.emplace(parent, ref);
    if (!inserted && it->second != ref) {
      throw std::runtime_error(
          "fixed_topology_exact certificate: conflicting after-topology "
          "production choices for one clade");
    }
  }
  validate_chart_spr_selected_overlay_topology_complete(
      base, candidate, selected, "fixed_topology_exact certificate");
  return selected;
}

inline std::map<overlay_clade_ref, overlay_production_ref>
chart_spr_before_selected_production_by_parent(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::vector<overlay_production_ref> const& production_refs) {
  std::map<overlay_clade_ref, overlay_production_ref> selected;
  for (auto ref : production_refs) {
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: before-topology "
          "production must refer to the current base grammar");
    }
    validate_chart_spr_selected_overlay_production_for_fixed_topology(
        base, candidate, ref,
        "fixed_topology_exact selected-topology cache before");
    auto parent = chart_spr_overlay_production_parent(base, candidate, ref);
    auto [it, inserted] = selected.emplace(parent, ref);
    if (!inserted && it->second != ref) {
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: conflicting "
          "before-topology production choices for one clade");
    }
  }
  validate_chart_spr_selected_overlay_topology_complete(
      base, candidate, selected,
      "fixed_topology_exact selected-topology cache before");
  return selected;
}

inline std::array<chart_cost, nuc_state_count>
chart_spr_restricted_overlay_topology_row_impl(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    site_pattern const& pattern,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref clade,
    std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>&
        base_memo,
    std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>&
        temp_memo,
    std::vector<std::uint8_t>& base_state,
    std::vector<std::uint8_t>& temp_state,
    chart_spr_search_counters* counters = nullptr) {
  auto& state_slot = clade.space == overlay_id_space::base
                         ? base_state.at(clade.id)
                         : temp_state.at(clade.id);
  auto& memo_slot = clade.space == overlay_id_space::base
                        ? base_memo.at(clade.id)
                        : temp_memo.at(clade.id);
  if (state_slot == 1) {
    throw std::runtime_error(
        "fixed_topology_exact cache scorer: cycle in selected overlay "
        "topology");
  }
  if (state_slot == 2) {
    if (!memo_slot) {
      throw std::runtime_error(
          "fixed_topology_exact cache scorer: completed clade missing memo");
    }
    return *memo_slot;
  }
  state_slot = 1;

  auto taxa = chart_spr_clade_taxa_for_ref(base, candidate, clade);
  std::array<chart_cost, nuc_state_count> row;
  if (taxa.size() == 1) {
    auto taxon = taxa.front();
    if (taxon >= pattern.state_by_taxon.size()) {
      throw std::runtime_error(
          "fixed_topology_exact cache scorer: leaf taxon out of pattern "
          "state range");
    }
    auto observed = pattern.state_by_taxon[taxon];
    parsimony_chart_detail::validate_state(
        observed, "fixed_topology_exact cache scorer leaf state");
    row = chart_multisite_detail::make_inf_row();
    row[observed] = 0;
  } else {
    auto it = selected.find(clade);
    if (it == selected.end()) {
      throw std::runtime_error(
          "fixed_topology_exact cache scorer: selected topology missing "
          "production for non-singleton overlay clade");
    }
    auto parent = chart_spr_overlay_production_parent(base, candidate,
                                                      it->second);
    if (parent != clade) {
      throw std::runtime_error(
          "fixed_topology_exact cache scorer: selected production parent "
          "mismatch");
    }
    validate_chart_spr_selected_overlay_production_for_fixed_topology(
        base, candidate, it->second,
        "fixed_topology_exact cache scorer");
    auto children = chart_spr_overlay_production_children(base, candidate,
                                                          it->second);
    if (children.size() != 2 && counters != nullptr) {
      ++counters->selected_topology_multifurcation_rows;
    }
    std::vector<chart_multisite_detail::chart_row> child_rows;
    child_rows.reserve(children.size());
    for (auto child : children) {
      child_rows.push_back(chart_spr_restricted_overlay_topology_row_impl(
          base, candidate, pattern, selected, child, base_memo, temp_memo,
          base_state, temp_state, counters));
    }
    row = chart_multisite_detail::combine_rows(
        std::span<chart_multisite_detail::chart_row const>{
            child_rows.data(), child_rows.size()});
  }

  memo_slot = row;
  state_slot = 2;
  return row;
}

inline std::array<chart_cost, nuc_state_count>
chart_spr_restricted_overlay_topology_row(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    site_pattern const& pattern,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    chart_spr_search_counters* counters = nullptr) {
  std::vector<std::optional<std::array<chart_cost, nuc_state_count>>> base_memo(
      base.clades.size());
  std::vector<std::optional<std::array<chart_cost, nuc_state_count>>> temp_memo(
      candidate.added_clades.size());
  std::vector<std::uint8_t> base_state(base.clades.size(), 0);
  std::vector<std::uint8_t> temp_state(candidate.added_clades.size(), 0);
  return chart_spr_restricted_overlay_topology_row_impl(
      base, candidate, pattern, selected, base_clade_ref(base.root_clade),
      base_memo, temp_memo, base_state, temp_state, counters);
}

struct chart_spr_lazy_selected_topology_entry {
  std::vector<std::array<chart_cost, nuc_state_count>> rows;
  std::vector<std::size_t> class_index_by_pattern;
};

// Selected-topology child-class contexts use the same one-stage prepared
// packed storage contract as lazy outside contexts: one pattern-major word
// buffer, one grouping workspace, and one grouping result. The alias makes the
// scratch lifetime explicit while retaining a single audited accounting path.
using chart_spr_lazy_selected_topology_key_workspace =
    lazy_chart_detail::outside_context_key_workspace;
using chart_spr_packed_lazy_selected_topology_keys =
    lazy_chart_detail::packed_outside_context_keys;

inline std::size_t
estimate_chart_spr_lazy_selected_topology_key_grouping_logical_resident_bytes(
    std::size_t pattern_count, std::size_t child_count,
    std::size_t class_count) {
  return lazy_chart_detail::
      estimate_outside_context_key_grouping_logical_resident_bytes(
          pattern_count, child_count, class_count);
}

inline chart_spr_packed_lazy_selected_topology_keys
chart_spr_collect_lazy_selected_topology_context_keys(
    std::size_t pattern_count,
    std::vector<chart_spr_lazy_selected_topology_entry const*> const&
        child_entries,
    chart_spr_lazy_selected_topology_key_workspace& workspace) {
  using namespace lazy_key_grouping_detail;
  return lazy_chart_detail::collect_packed_outside_context_keys(
      pattern_count, child_entries.size(), workspace,
      [&](std::span<packed_key_word> words) {
        auto output = words.begin();
        for (std::size_t pattern = 0; pattern < pattern_count; ++pattern) {
          for (auto const* child_entry : child_entries) {
            *output++ = checked_packed_key_word(
                child_entry->class_index_by_pattern[pattern],
                "fixed_topology_exact lazy selected-topology child class "
                "index");
          }
        }
        if (output != words.end()) {
          throw std::logic_error(
              "fixed_topology_exact lazy selected-topology context-key width "
              "mismatch");
        }
      });
}

inline void chart_spr_assign_lazy_selected_topology_internal_entry(
    chart_spr_lazy_selected_topology_entry& entry, std::size_t pattern_count,
    std::vector<chart_spr_lazy_selected_topology_entry const*> const&
        child_entries,
    chart_spr_lazy_selected_topology_key_workspace& workspace,
    chart_spr_search_counters& counters) {
  entry.class_index_by_pattern.assign(pattern_count, 0);
  auto const context_keys =
      chart_spr_collect_lazy_selected_topology_context_keys(
          pattern_count, child_entries, workspace);
  auto const& context_classes = context_keys.classes();
  auto const words = context_keys.key_words();

  std::map<std::array<chart_cost, nuc_state_count>, std::size_t> class_by_row;
  // The former ordered vector-key map computed rows in lexicographic context
  // order. Packed IDs are first-occurrence IDs, so the explicit lexicographic
  // class order is required for row numbering and partial-failure semantics.
  for (auto context_class : context_classes.lexicographic_class_order) {
    auto const representative =
        context_classes.representative_by_class[context_class];
    auto const key_begin = representative * context_keys.key_width;
    std::vector<chart_multisite_detail::chart_row> child_rows;
    child_rows.reserve(child_entries.size());
    for (std::size_t child_i = 0; child_i < child_entries.size(); ++child_i) {
      auto const* child_entry = child_entries[child_i];
      auto const class_index =
          static_cast<std::size_t>(words[key_begin + child_i]);
      if (class_index >= child_entry->rows.size()) {
        throw std::runtime_error(
            "fixed_topology_exact lazy selected-topology scorer: child "
            "class index out of range");
      }
      child_rows.push_back(child_entry->rows[class_index]);
    }
    auto const row = chart_multisite_detail::combine_rows(
        std::span<chart_multisite_detail::chart_row const>{child_rows.data(),
                                                           child_rows.size()});
    auto [row_it, inserted] = class_by_row.emplace(row, class_by_row.size());
    if (inserted) entry.rows.push_back(row);
    for (auto pattern : context_classes.members_for_class(context_class)) {
      entry.class_index_by_pattern[pattern] = row_it->second;
    }
  }
  if (child_entries.size() != 2) {
    counters.selected_topology_multifurcation_rows +=
        context_classes.class_count();
  }
}

inline chart_spr_lazy_selected_topology_entry const&
chart_spr_lazy_selected_topology_rows_for_clade(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref clade,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        base_memo,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        temp_memo,
    std::vector<std::uint8_t>& base_state,
    std::vector<std::uint8_t>& temp_state, chart_spr_search_counters& counters,
    chart_spr_lazy_selected_topology_key_workspace& key_workspace) {
  auto& state_slot = clade.space == overlay_id_space::base
                         ? base_state.at(clade.id)
                         : temp_state.at(clade.id);
  auto& memo_slot = clade.space == overlay_id_space::base
                        ? base_memo.at(clade.id)
                        : temp_memo.at(clade.id);
  if (state_slot == 1) {
    throw std::runtime_error(
        "fixed_topology_exact lazy selected-topology scorer: cycle in "
        "selected topology");
  }
  if (state_slot == 2) {
    if (!memo_slot) {
      throw std::runtime_error(
          "fixed_topology_exact lazy selected-topology scorer: completed "
          "clade missing memo");
    }
    return *memo_slot;
  }
  state_slot = 1;

  auto const& active = state.active_patterns.patterns.patterns;
  chart_spr_lazy_selected_topology_entry entry;
  entry.class_index_by_pattern.assign(active.size(), 0);
  auto taxa = chart_spr_clade_taxa_for_ref(state.grammar, candidate, clade);
  if (taxa.empty()) {
    throw std::runtime_error(
        "fixed_topology_exact lazy selected-topology scorer: selected clade "
        "has no taxa");
  }

  if (taxa.size() == 1) {
    auto taxon = taxa.front();
    std::map<std::uint8_t, std::size_t> class_by_state;
    for (std::size_t p = 0; p < active.size(); ++p) {
      if (taxon >= active[p].state_by_taxon.size()) {
        throw std::runtime_error(
            "fixed_topology_exact lazy selected-topology scorer: leaf taxon "
            "out of state range");
      }
      auto observed = active[p].state_by_taxon[taxon];
      parsimony_chart_detail::validate_state(
          observed, "fixed_topology_exact lazy selected-topology leaf state");
      auto [it, inserted] =
          class_by_state.emplace(observed, class_by_state.size());
      if (inserted) {
        auto row = parsimony_chart_detail::make_inf_row();
        row[observed] = 0;
        entry.rows.push_back(row);
      }
      entry.class_index_by_pattern[p] = it->second;
    }
  } else {
    auto it = selected.find(clade);
    if (it == selected.end()) {
      throw std::runtime_error(
          "fixed_topology_exact lazy selected-topology scorer: selected "
          "topology missing production for non-singleton clade");
    }
    validate_chart_spr_selected_overlay_production_for_fixed_topology(
        state.grammar, candidate, it->second,
        "fixed_topology_exact lazy selected-topology scorer");
    auto children = chart_spr_overlay_production_children(state.grammar,
                                                          candidate,
                                                          it->second);
    std::vector<chart_spr_lazy_selected_topology_entry const*> child_entries;
    child_entries.reserve(children.size());
    for (auto child : children) {
      child_entries.push_back(&chart_spr_lazy_selected_topology_rows_for_clade(
          state, candidate, selected, child, base_memo, temp_memo, base_state,
          temp_state, counters, key_workspace));
    }
    chart_spr_assign_lazy_selected_topology_internal_entry(
        entry, active.size(), child_entries, key_workspace, counters);
  }

  counters.fixed_topology_selected_rows_computed += entry.rows.size();
  counters.selected_topology_class_rows_computed += entry.rows.size();
  memo_slot = std::move(entry);
  state_slot = 2;
  return *memo_slot;
}

inline chart_spr_lazy_selected_topology_entry const&
chart_spr_lazy_selected_topology_root_rows(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        base_memo,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        temp_memo,
    chart_spr_search_counters& counters,
    chart_spr_lazy_selected_topology_key_workspace& key_workspace) {
  std::vector<std::uint8_t> base_state(state.grammar.clades.size(), 0);
  std::vector<std::uint8_t> temp_state(candidate.added_clades.size(), 0);
  return chart_spr_lazy_selected_topology_rows_for_clade(
      state, candidate, selected, base_clade_ref(state.grammar.root_clade),
      base_memo, temp_memo, base_state, temp_state, counters, key_workspace);
}

struct chart_spr_fixed_topology_pattern_scores {
  std::vector<std::uint64_t> old_pattern_scores;
  std::vector<std::uint64_t> new_pattern_scores;
  std::uint64_t old_active_total = 0;
  std::uint64_t new_active_total = 0;
};

inline chart_spr_fixed_topology_pattern_scores
fixed_topology_lazy_selected_pattern_scores(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!state.lazy_chart) {
    throw std::runtime_error(
        "fixed_topology_exact lazy selected-topology scorer: missing lazy "
        "chart");
  }
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "fixed_topology_exact lazy selected-topology scorer requires a "
        "complete topology certificate");
  }
  auto const& certificate = *candidate.topology_selection.certificate;
  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate, certificate);

  auto before_selected = chart_spr_before_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.before_overlay_productions);
  auto after_selected = chart_spr_overlay_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.after_overlay_productions);

  std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>
      before_base_memo(state.grammar.clades.size());
  std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>
      before_temp_memo(candidate.candidate.added_clades.size());
  std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>
      after_base_memo(state.grammar.clades.size());
  std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>
      after_temp_memo(candidate.candidate.added_clades.size());
  chart_spr_lazy_selected_topology_key_workspace key_workspace;
  auto const& before_root = chart_spr_lazy_selected_topology_root_rows(
      state, candidate.candidate, before_selected, before_base_memo,
      before_temp_memo, counters, key_workspace);
  auto const& after_root = chart_spr_lazy_selected_topology_root_rows(
      state, candidate.candidate, after_selected, after_base_memo,
      after_temp_memo, counters, key_workspace);

  chart_spr_fixed_topology_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto old_class = before_root.class_index_by_pattern[p];
    auto new_class = after_root.class_index_by_pattern[p];
    if (old_class >= before_root.rows.size() ||
        new_class >= after_root.rows.size()) {
      throw std::runtime_error(
          "fixed_topology_exact lazy selected-topology scorer: root class "
          "index out of range");
    }
    auto old_score = chart_spr_weighted_root_score_from_row(
        before_root.rows[old_class], active[p], state.chart_opts);
    auto new_score = chart_spr_weighted_root_score_from_row(
        after_root.rows[new_class], active[p], state.chart_opts);
    scores.old_pattern_scores.push_back(old_score);
    scores.new_pattern_scores.push_back(new_score);
    scores.old_active_total = chart_multisite_detail::checked_add_u64(
        scores.old_active_total, old_score,
        "fixed_topology_exact lazy selected old active total");
    scores.new_active_total = chart_multisite_detail::checked_add_u64(
        scores.new_active_total, new_score,
        "fixed_topology_exact lazy selected new active total");
  }
  return scores;
}

inline chart_spr_fixed_topology_pattern_scores
fixed_topology_direct_selected_pattern_scores(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    return fixed_topology_lazy_selected_pattern_scores(state, candidate,
                                                       counters);
  }
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "fixed_topology_exact direct scorer requires a complete topology "
        "certificate");
  }
  auto const& certificate = *candidate.topology_selection.certificate;
  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate, certificate);

  auto before_ids = chart_spr_base_production_ids_from_refs(
      certificate.before_overlay_productions);
  auto before_topology = grammar_topology_from_productions(state.grammar,
                                                          before_ids);
  (void)validate_grammar_topology(state.grammar, before_topology);
  auto selected_after = chart_spr_overlay_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.after_overlay_productions);

  chart_spr_fixed_topology_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  for (std::size_t pattern_index = 0; pattern_index < active.size();
       ++pattern_index) {
    auto const& pattern = active[pattern_index];
    if (state.chart_opts.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(
          pattern, pattern_index);
    }
    auto old_row = chart_multisite_detail::restricted_topology_row(
        state.grammar, pattern, before_topology);
    auto new_row = chart_spr_restricted_overlay_topology_row(
        state.grammar, candidate.candidate, pattern, selected_after, &counters);
    auto old_score = chart_spr_weighted_root_score_from_row(old_row, pattern,
                                                            state.chart_opts);
    auto new_score = chart_spr_weighted_root_score_from_row(new_row, pattern,
                                                            state.chart_opts);
    scores.old_pattern_scores.push_back(old_score);
    scores.new_pattern_scores.push_back(new_score);
    scores.old_active_total = chart_multisite_detail::checked_add_u64(
        scores.old_active_total, old_score,
        "fixed_topology_exact direct old active total");
    scores.new_active_total = chart_multisite_detail::checked_add_u64(
        scores.new_active_total, new_score,
        "fixed_topology_exact direct new active total");
  }
  return scores;
}

inline chart_spr_fixed_topology_pattern_scores
fixed_topology_direct_selected_pattern_scores(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters, chart_scheduler& scheduler) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    return fixed_topology_lazy_selected_pattern_scores(state, candidate,
                                                       counters);
  }
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "fixed_topology_exact direct scorer requires a complete topology "
        "certificate");
  }
  auto const& certificate = *candidate.topology_selection.certificate;
  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate, certificate);
  auto before_ids = chart_spr_base_production_ids_from_refs(
      certificate.before_overlay_productions);
  auto before_topology =
      grammar_topology_from_productions(state.grammar, before_ids);
  (void)validate_grammar_topology(state.grammar, before_topology);
  auto selected_after = chart_spr_overlay_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.after_overlay_productions);

  auto const& active = state.active_patterns.patterns.patterns;
  if (state.chart_opts.score_ua_edge) {
    for (std::size_t pattern_index = 0; pattern_index < active.size();
         ++pattern_index) {
      chart_multisite_detail::validate_pattern_reference_counts(
          active[pattern_index], pattern_index);
    }
  }
  chart_spr_fixed_topology_pattern_scores scores;
  scores.old_pattern_scores.resize(active.size());
  scores.new_pattern_scores.resize(active.size());
  auto const resolved_workers = scheduler.worker_resolution().resolved_workers;
  std::vector<chart_spr_search_counters> counters_by_slot(resolved_workers);
  std::vector<std::exception_ptr> errors(active.size());
  auto range_options =
      chart_spr_phase4_pattern_range_options(active.size(), resolved_workers);
  chart_scheduler_run_summary run;
  try {
    run = scheduler.for_each_indexed_range(
        active.size(), range_options,
        [&](chart_indexed_range const& range, std::size_t stable_slot,
            chart_scheduler_cancellation_token const&) {
          for (std::size_t pattern_index = range.begin;
               pattern_index < range.end; ++pattern_index) {
            try {
              auto const& pattern = active[pattern_index];
              auto old_row = chart_multisite_detail::restricted_topology_row(
                  state.grammar, pattern, before_topology);
              auto new_row = chart_spr_restricted_overlay_topology_row(
                  state.grammar, candidate.candidate, pattern, selected_after,
                  &counters_by_slot[stable_slot]);
              scores.old_pattern_scores[pattern_index] =
                  chart_spr_weighted_root_score_from_row(old_row, pattern,
                                                         state.chart_opts);
              scores.new_pattern_scores[pattern_index] =
                  chart_spr_weighted_root_score_from_row(new_row, pattern,
                                                         state.chart_opts);
            } catch (...) {
              errors[pattern_index] = std::current_exception();
              break;
            }
          }
        });
  } catch (chart_scheduler_submit_error const&) {
    throw;
  } catch (std::exception const& error) {
    // Every pattern callback captures its own failure above. Any exception
    // escaping the scheduler therefore occurred before a run summary could be
    // returned (pool construction, submission, or scheduler bookkeeping).
    throw chart_scheduler_submit_error(
        std::string{"fixed_topology_exact scheduler operation failed: "} +
        error.what());
  } catch (...) {
    throw chart_scheduler_submit_error(
        "fixed_topology_exact scheduler operation failed with a non-standard "
        "exception");
  }
  record_chart_spr_scheduler_axis_run(
      counters.scheduler_axes.fixed_topology_patterns, run);
  for (auto const& slot_counters : counters_by_slot) {
    add_chart_spr_search_counters(counters, slot_counters);
  }
  for (auto const& error : errors) {
    if (error) std::rethrow_exception(error);
  }
  for (std::size_t pattern_index = 0; pattern_index < active.size();
       ++pattern_index) {
    scores.old_active_total = chart_multisite_detail::checked_add_u64(
        scores.old_active_total, scores.old_pattern_scores[pattern_index],
        "fixed_topology_exact direct old active total");
    scores.new_active_total = chart_multisite_detail::checked_add_u64(
        scores.new_active_total, scores.new_pattern_scores[pattern_index],
        "fixed_topology_exact direct new active total");
  }
  return scores;
}

inline chart_spr_fixed_topology_pattern_scores
fixed_topology_direct_selected_pattern_scores(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  return fixed_topology_direct_selected_pattern_scores(state, candidate,
                                                       state.counters);
}

inline chart_spr_fixed_topology_pattern_scores
fixed_topology_direct_selected_pattern_scores(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate, chart_scheduler& scheduler) {
  return fixed_topology_direct_selected_pattern_scores(
      state, candidate, state.counters, scheduler);
}

inline std::vector<std::array<chart_cost, nuc_state_count>>
chart_spr_selected_overlay_child_outside_rows(
    std::array<chart_cost, nuc_state_count> const& parent_outside,
    std::vector<std::array<chart_cost, nuc_state_count>> const&
        child_inside_rows) {
  if (child_inside_rows.empty()) {
    throw std::runtime_error(
        "fixed_topology_exact selected outside scorer: selected production "
        "has no children");
  }
  std::vector<std::array<chart_cost, nuc_state_count>> result(
      child_inside_rows.size(), parsimony_chart_detail::make_inf_row());

  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    auto base_cost = parent_outside[parent_state];
    if (base_cost >= chart_inf) continue;

    std::vector<chart_cost> child_best(child_inside_rows.size(), chart_inf);
    for (std::size_t child_i = 0; child_i < child_inside_rows.size();
         ++child_i) {
      auto const& child_inside = child_inside_rows[child_i];
      for (std::uint8_t child_state = 0; child_state < nuc_state_count;
           ++child_state) {
        child_best[child_i] = std::min(
            child_best[child_i],
            parsimony_chart_detail::saturated_add(
                child_inside[child_state],
                parsimony_chart_detail::transition_cost(parent_state,
                                                        child_state)));
      }
    }

    std::vector<chart_cost> prefix(child_inside_rows.size() + 1,
                                   chart_cost{0});
    std::vector<chart_cost> suffix(child_inside_rows.size() + 1,
                                   chart_cost{0});
    for (std::size_t child_i = 0; child_i < child_inside_rows.size();
         ++child_i) {
      prefix[child_i + 1] = parsimony_chart_detail::saturated_add(
          prefix[child_i], child_best[child_i]);
    }
    for (std::size_t child_i = child_inside_rows.size(); child_i-- > 0;) {
      suffix[child_i] = parsimony_chart_detail::saturated_add(
          child_best[child_i], suffix[child_i + 1]);
    }

    for (std::size_t child_i = 0; child_i < child_inside_rows.size();
         ++child_i) {
      auto sibling_context = parsimony_chart_detail::saturated_add(
          base_cost, parsimony_chart_detail::saturated_add(
                         prefix[child_i], suffix[child_i + 1]));
      if (sibling_context >= chart_inf) continue;
      for (std::uint8_t child_state = 0; child_state < nuc_state_count;
           ++child_state) {
        auto candidate = parsimony_chart_detail::saturated_add(
            sibling_context,
            parsimony_chart_detail::transition_cost(parent_state,
                                                    child_state));
        if (candidate < result[child_i][child_state]) {
          result[child_i][child_state] = candidate;
        }
      }
    }
  }
  return result;
}

// Legacy/conservative fixed-topology exact path.  This is the from-scratch
// selected-topology fallback: it does not materialize an overlay grammar, but
// it intentionally recomputes the selected before/after topology rows.  The
// Phase-8 local-commit path installs
// `state.contextual_fixed_topology_exact_verifier`,
// which serves production verification from the persistent inside/outside
// caches and uses this direct scorer only as a per-pattern oracle/fallback.
inline spr_score_result fixed_topology_delta_direct_selected_topology(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters) {
  auto scores =
      fixed_topology_direct_selected_pattern_scores(state, candidate, counters);
  auto old_full = chart_spr_add_invariant_offset(
      scores.old_active_total, state,
      "chart-SPR fixed-topology old-score invariant offset");
  auto new_full = chart_spr_add_invariant_offset(
      scores.new_active_total, state,
      "chart-SPR fixed-topology new-score invariant offset");
  return spr_score_result{chart_spr_detail::signed_delta(old_full, new_full),
                          old_full, new_full, true};
}

inline spr_score_result fixed_topology_delta_direct_selected_topology(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters, chart_scheduler& scheduler) {
  auto scores = fixed_topology_direct_selected_pattern_scores(
      state, candidate, counters, scheduler);
  auto old_full = chart_spr_add_invariant_offset(
      scores.old_active_total, state,
      "chart-SPR fixed-topology old-score invariant offset");
  auto new_full = chart_spr_add_invariant_offset(
      scores.new_active_total, state,
      "chart-SPR fixed-topology new-score invariant offset");
  return spr_score_result{chart_spr_detail::signed_delta(old_full, new_full),
                          old_full, new_full, true};
}

inline spr_score_result fixed_topology_delta_direct_selected_topology(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  return fixed_topology_delta_direct_selected_topology(state, candidate,
                                                       state.counters);
}

inline spr_score_result fixed_topology_delta_direct_selected_topology(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate, chart_scheduler& scheduler) {
  return fixed_topology_delta_direct_selected_topology(
      state, candidate, state.counters, scheduler);
}

chart_spr_fixed_topology_pattern_scores
fixed_topology_selected_cache_pattern_scores_for_tests(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate);

// Exercises the production lazy local-commit projection/refresh path without
// running candidate selection.  This is intentionally narrow: regression
// tests use it to construct rebasing/reactivation chains whose exact commit
// order is otherwise difficult to force through the public search policy.
void refresh_chart_spr_lazy_chart_after_local_commit_for_tests(
    chart_spr_search_state& state, overlay_chain const& chain,
    overlay_materialization_result const& materialized,
    chart_execution_plan const& execution_plan,
    std::vector<overlay_clade_ref> const& previous_dense_clade_to_ref);

inline chart_spr_candidate_score verify_candidate_fixed_topology_exact_impl(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_spr_exact_verification_context& context) {
  if (!candidate.valid) return candidate;
  if (!chart_spr_topology_selection_has_certificate_or_selector(
          candidate.topology_selection)) {
    candidate.valid = false;
    candidate.invalid_reason =
        "fixed_topology_exact acceptance requires an explicit complete "
        "topology certificate or a recorded deterministic topology selector; "
        "a bare grammar_spr_candidate is not exact";
    return candidate;
  }
  if (!candidate.topology_selection.certificate) {
    candidate.valid = false;
    candidate.invalid_reason =
        "fixed_topology_exact deterministic selectors must be resolved to a "
        "complete topology certificate before verification";
    return candidate;
  }

  ++context.counters.exact_verifications;
  try {
    // Phase 8: score the complete fixed before/after topology certificate
    // directly in overlay space.  This is exact for the selected topology and
    // performs no dense overlay materialization; the exact-verification
    // materialization counters therefore remain unchanged for the
    // fixed_topology_exact gate.
    auto delta =
        context.inner_scheduler != nullptr
            ? fixed_topology_delta_direct_selected_topology(
                  state, candidate, context.counters, *context.inner_scheduler)
            : fixed_topology_delta_direct_selected_topology(state, candidate,
                                                            context.counters);
    candidate.exact = make_chart_spr_objective_score(
        delta, chart_spr_score_kind::fixed_topology_exact,
        chart_spr_score_convention::full_with_invariants,
        state.invariant_constant_offset);
  } catch (chart_scheduler_submit_error const&) {
    // Infrastructure failures return no run summary. They must abort the
    // search rather than masquerade as a biologically invalid candidate and
    // leave semantic-axis accounting unable to reconcile with the scheduler.
    throw;
  } catch (std::exception const& e) {
    candidate.valid = false;
    candidate.invalid_reason = e.what();
  }
  if (candidate.valid && candidate.exact &&
      candidate.canonical_stream_index !=
          (std::numeric_limits<std::size_t>::max)()) {
    chart_spr_force_canonical_evidence_failure_for_tests(candidate);
    candidate.canonical_exact_evidence =
        std::make_shared<chart_spr_canonical_exact_evidence>(
            chart_spr_canonicalize_fixed_topology_evidence(
                state.grammar, candidate.topology_selection,
                state.invariant_constant_offset));
  }
  return candidate;
}

inline chart_spr_candidate_score verify_candidate_fixed_topology_exact(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate) {
  chart_spr_exact_verification_context context{state.counters};
  return verify_candidate_fixed_topology_exact_impl(state, std::move(candidate),
                                                    context);
}

inline chart_spr_candidate_score verify_candidate_fixed_topology_exact(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_scheduler& scheduler) {
  chart_spr_exact_verification_context context{state.counters, nullptr,
                                               &scheduler};
  return verify_candidate_fixed_topology_exact_impl(state, std::move(candidate),
                                                    context);
}

inline chart_spr_candidate_score verify_candidate_for_acceptance(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_search_options const& options,
    chart_spr_exact_verification_context& context) {
  checked_state.assert_same(state.grammar, state.execution_plan);
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return candidate;
    case chart_spr_acceptance_mode::exact_multisite:
      // Phase 9: when a local-commit substrate is active it installs
      // `state.contextual_exact_multisite_verifier`, which verifies the
      // candidate by
      // transiently extending the chain in reader-local scratch (avoiding a
      // fresh materialize_overlay_grammar, never mutating the
      // shared cache).  When absent (conservative rebuild mode, or a bare
      // state built without a substrate), the cold from-scratch path below is
      // used.  The cold path is also the correctness oracle the transient
      // verifier cross-checks when its test-only oracle flag is set.
      if (options.verification_mode ==
              chart_spr_verification_mode::transient &&
          state.contextual_exact_multisite_verifier) {
        if (state.exact_multisite_verifier) {
          throw std::logic_error(
              "chart SPR exact verification: both legacy and contextual "
              "multisite verifiers are installed");
        }
        return state.contextual_exact_multisite_verifier(
            state, std::move(candidate), checked_state, context,
            options.exact_trim);
      }
      if (options.verification_mode ==
              chart_spr_verification_mode::transient &&
          state.exact_multisite_verifier &&
          context.inner_scheduler != nullptr) {
        return state.exact_multisite_verifier(
            state, std::move(candidate), checked_state,
            *context.inner_scheduler, options.exact_trim);
      }
      return verify_candidate_exact_against_state_impl(
          state, std::move(candidate), checked_state, context,
          options.exact_trim);
    case chart_spr_acceptance_mode::fixed_topology_exact: {
      chart_spr_candidate_score verified;
      if (state.contextual_fixed_topology_exact_verifier) {
        if (state.fixed_topology_exact_verifier) {
          throw std::logic_error(
              "chart SPR exact verification: both legacy and contextual "
              "fixed-topology verifiers are installed");
        }
        verified = state.contextual_fixed_topology_exact_verifier(
            state, std::move(candidate), context);
      } else if (state.fixed_topology_exact_verifier) {
        verified = state.fixed_topology_exact_verifier(state,
                                                       std::move(candidate));
      } else {
        verified = verify_candidate_fixed_topology_exact_impl(
            state, std::move(candidate), context);
      }
      if (verified.canonical_stream_index !=
              (std::numeric_limits<std::size_t>::max)() &&
          verified.exact &&
          !verified.canonical_exact_evidence) {
        chart_spr_force_canonical_evidence_failure_for_tests(verified);
        verified.canonical_exact_evidence =
            std::make_shared<chart_spr_canonical_exact_evidence>(
                chart_spr_canonicalize_fixed_topology_evidence(
                    state.grammar, verified.topology_selection,
                    state.invariant_constant_offset));
      }
      return verified;
      }
  }
  return candidate;
}

inline chart_spr_candidate_score verify_candidate_for_acceptance(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_search_options const& options,
    chart_scheduler* scheduler = nullptr) {
  chart_spr_exact_verification_context context{state.counters, nullptr,
                                               scheduler};
  return verify_candidate_for_acceptance(state, std::move(candidate),
                                         checked_state, options, context);
}

inline chart_spr_candidate_score verify_candidate_for_acceptance(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_spr_search_options const& options) {
  auto checked = check_chart_execution_plan(state.grammar,
                                            state.execution_plan);
  return verify_candidate_for_acceptance(state, std::move(candidate), checked,
                                         options);
}

inline bool chart_spr_candidate_has_accepting_improvement(
    chart_spr_candidate_score const& candidate,
    chart_spr_acceptance_mode mode) {
  if (!candidate.valid) return false;
  switch (mode) {
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return candidate.lower_bound.value.improves();
    case chart_spr_acceptance_mode::exact_multisite:
    case chart_spr_acceptance_mode::fixed_topology_exact:
      return candidate.exact && candidate.exact->value.improves();
  }
  return false;
}

inline spr_score_result const& chart_spr_candidate_acceptance_score(
    chart_spr_candidate_score const& candidate,
    chart_spr_acceptance_mode mode) {
  if (mode == chart_spr_acceptance_mode::lower_bound_heuristic) {
    return candidate.lower_bound.value;
  }
  if (!candidate.exact) {
    throw std::runtime_error(
        "chart SPR acceptance score requested before exact verification");
  }
  return candidate.exact->value;
}

inline bool chart_spr_ranked_candidate_better(
    clade_grammar const& grammar, chart_spr_candidate_score const& lhs,
    chart_spr_candidate_score const& rhs) {
  if (lhs.valid != rhs.valid) return lhs.valid;
  if (lhs.lower_bound.value.delta != rhs.lower_bound.value.delta) {
    return lhs.lower_bound.value.delta < rhs.lower_bound.value.delta;
  }
  if (lhs.affected_clade_count != rhs.affected_clade_count) {
    return lhs.affected_clade_count < rhs.affected_clade_count;
  }
  return chart_spr_candidate_taxon_signature(grammar, lhs.candidate) <
         chart_spr_candidate_taxon_signature(grammar, rhs.candidate);
}

inline bool chart_spr_acceptance_candidate_better(
    chart_spr_acceptance_mode mode, clade_grammar const& grammar,
    chart_spr_candidate_score const& lhs,
    chart_spr_candidate_score const& rhs) {
  auto const& lscore = chart_spr_candidate_acceptance_score(lhs, mode);
  auto const& rscore = chart_spr_candidate_acceptance_score(rhs, mode);
  if (lscore.new_score != rscore.new_score) {
    return lscore.new_score < rscore.new_score;
  }
  if (lscore.delta != rscore.delta) return lscore.delta < rscore.delta;
  return chart_spr_ranked_candidate_better(grammar, lhs, rhs);
}

inline constexpr std::size_t chart_spr_rank_unlimited =
    std::numeric_limits<std::size_t>::max();

inline void chart_spr_insert_ranked_candidate(
    clade_grammar const& grammar, std::vector<chart_spr_candidate_score>& ranked,
    chart_spr_candidate_score candidate, std::size_t max_ranked) {
  if (!candidate.valid || max_ranked == 0) return;
  auto pos = std::lower_bound(
      ranked.begin(), ranked.end(), candidate,
      [&](chart_spr_candidate_score const& existing,
          chart_spr_candidate_score const& value) {
        return chart_spr_ranked_candidate_better(grammar, existing, value);
      });
  ranked.insert(pos, std::move(candidate));
  if (max_ranked != chart_spr_rank_unlimited && ranked.size() > max_ranked) {
    ranked.pop_back();
  }
}

inline std::size_t chart_spr_rank_buffer_limit(
    chart_spr_search_options const& options) {
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return 1;
    case chart_spr_acceptance_mode::fixed_topology_exact:
    case chart_spr_acceptance_mode::exact_multisite:
      break;
  }
  switch (options.candidate_selection) {
    case chart_spr_candidate_selection_mode::exhaustive_exact:
      return chart_spr_rank_unlimited;
    case chart_spr_candidate_selection_mode::lower_bound_top_k:
      return options.top_k_exact_verify;
    case chart_spr_candidate_selection_mode::lower_bound_first_improvement:
      return 1;
    case chart_spr_candidate_selection_mode::sampled_or_randomized:
      return options.top_k_exact_verify;
  }
  return options.top_k_exact_verify;
}

inline grammar_spr_enumeration_options chart_spr_iteration_enumeration_options(
    chart_spr_search_options const& options) {
  auto enumeration = options.enumeration;
  if (options.candidate_selection ==
      chart_spr_candidate_selection_mode::sampled_or_randomized) {
    enumeration.randomize_order = true;
    enumeration.seed = options.seed;
  }
  if (options.max_candidates_per_iteration != 0) {
    enumeration.max_candidates = options.max_candidates_per_iteration;
    enumeration.max_candidates_is_post_dedup = true;
  }
  return enumeration;
}

inline bool chart_spr_enumeration_replayable_without_candidate_batch(
    grammar_spr_enumeration_options const& enumeration) {
  return enumeration.source == chart_spr_candidate_source::grammar &&
         !enumeration.randomize_order && !enumeration.reservoir_sample;
}

inline std::size_t chart_spr_effective_candidate_batch_size(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    std::size_t already_resolved_workers = 0) {
  if (options.candidate_selection ==
      chart_spr_candidate_selection_mode::lower_bound_first_improvement) {
    return 1;
  }
  if (options.cache.candidate_batch_size != 0) {
    return std::max<std::size_t>(1, options.cache.candidate_batch_size);
  }
  auto workers = already_resolved_workers;
  if (workers == 0) {
    workers = chart_spr_search_detail::normalize_chart_spr_worker_count(
        chart_spr_search_detail::requested_chart_spr_worker_count(options));
  }
  if (state.cache_strategy == chart_spr_cache_strategy::pattern_batches) {
    return std::max<std::size_t>(1, workers * 4);
  }
  return workers > 1 ? std::max<std::size_t>(1, workers * 4) : 1;
}

inline void validate_chart_spr_pattern_batch_replay_strategy(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    grammar_spr_enumeration_options const& enumeration,
    std::size_t effective_candidate_batch_size = 0) {
  if (state.cache_strategy != chart_spr_cache_strategy::pattern_batches) {
    return;
  }
  // Phase 7 scores pattern batches against a stored, bounded candidate batch.
  // A zero explicit cache.candidate_batch_size therefore means "use the
  // computed default", not "replay an unbounded candidate stream".
  auto bounded_batch_size = effective_candidate_batch_size != 0
                                ? effective_candidate_batch_size
                                : chart_spr_effective_candidate_batch_size(
                                      state, options);
  if (bounded_batch_size != 0) return;
  if (chart_spr_enumeration_replayable_without_candidate_batch(enumeration)) {
    return;
  }
  throw std::runtime_error(
      "chart SPR pattern-batch scoring: non-replayable candidate source/order "
      "requires a bounded candidate batch");
}

inline std::uint64_t chart_spr_iteration_state_score_before(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_search_options const& options) {
  chart_spr_search_detail::require_completed_chart_spr_state_bootstrap(
      state, "chart SPR iteration state score");
  checked_state.assert_same(state.grammar, state.execution_plan);
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    return chart_spr_state_exact_score_with_invariants(
        state, checked_state, options.exact_trim);
  }
  return state.composite_lower_bound_with_invariants;
}

inline std::uint64_t chart_spr_iteration_state_score_before(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_search_options const& options, chart_scheduler& scheduler) {
  chart_spr_search_detail::require_completed_chart_spr_state_bootstrap(
      state, "chart SPR iteration state score");
  checked_state.assert_same(state.grammar, state.execution_plan);
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    auto const& trim = ensure_chart_spr_state_exact_trim(
        state, checked_state, scheduler, options.exact_trim);
    return chart_spr_add_invariant_offset(
        trim.optimum, state, "chart-SPR exact state invariant offset");
  }
  return state.composite_lower_bound_with_invariants;
}

inline std::uint64_t chart_spr_iteration_state_score_before(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options) {
  chart_spr_search_detail::require_completed_chart_spr_state_bootstrap(
      state, "chart SPR iteration state score");
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    return chart_spr_state_exact_score_with_invariants(state,
                                                       options.exact_trim);
  }
  return state.composite_lower_bound_with_invariants;
}

namespace chart_spr_search_detail {

struct grammar_spr_candidate_copy_scratch {
  spr_overlay_delta_build_scratch payload;
  std::vector<overlay_production_ref> source_before_topology;
  std::vector<overlay_production_ref> source_after_topology;
};

inline void copy_optional_production_refs_reusing_storage(
    std::optional<std::vector<overlay_production_ref>>& destination,
    std::optional<std::vector<overlay_production_ref>> const& source,
    std::vector<overlay_production_ref>& spare) {
  if (!source) {
    if (destination) {
      destination->clear();
      destination->swap(spare);
      destination.reset();
    }
    return;
  }
  if (!destination) {
    destination.emplace();
    destination->swap(spare);
  }
  destination->assign(source->begin(), source->end());
}

inline void copy_grammar_spr_candidate_reusing_storage(
    grammar_spr_candidate& destination, grammar_spr_candidate const& source,
    grammar_spr_candidate_copy_scratch& scratch) {
  destination.moved_clade = source.moved_clade;
  destination.old_parent = source.old_parent;
  destination.old_sibling = source.old_sibling;
  destination.new_sibling_or_target = source.new_sibling_or_target;
  destination.removed_productions.assign(source.removed_productions.begin(),
                                         source.removed_productions.end());

  resize_reusing_nested_storage(destination.added_clades,
                                source.added_clades.size(),
                                scratch.payload.spare_temp_clades);
  for (std::size_t i = 0; i < source.added_clades.size(); ++i) {
    destination.added_clades[i].taxa.assign(source.added_clades[i].taxa.begin(),
                                            source.added_clades[i].taxa.end());
  }
  resize_reusing_nested_storage(destination.added_productions,
                                source.added_productions.size(),
                                scratch.payload.spare_temp_productions);
  for (std::size_t i = 0; i < source.added_productions.size(); ++i) {
    copy_overlay_production_reusing_storage(destination.added_productions[i],
                                            source.added_productions[i],
                                            scratch.payload);
  }

  destination.source_tree_move = source.source_tree_move;
  copy_optional_production_refs_reusing_storage(
      destination.source_before_topology_productions,
      source.source_before_topology_productions,
      scratch.source_before_topology);
  copy_optional_production_refs_reusing_storage(
      destination.source_after_topology_productions,
      source.source_after_topology_productions, scratch.source_after_topology);
}

// Search-lifetime acceptance storage. Candidate slots use high-water copies
// so shrinking a nested clade/production/witness or toggling optional topology
// provenance does not free its storage before a later batch or accepted-state
// generation needs it again. The local scorer retains no pointer into these
// slots after each `_into` call.
struct chart_spr_acceptance_iteration_workspace {
  std::vector<grammar_spr_candidate> candidate_slots;
  std::vector<grammar_spr_candidate_copy_scratch> candidate_copy_scratch;
  std::vector<grammar_spr_candidate> pipeline_candidate_slots;
  std::vector<grammar_spr_candidate_copy_scratch>
      pipeline_candidate_copy_scratch;
  std::vector<chart_spr_local_score_result> local_results;
  chart_spr_local_score_workspace local_score;
  std::size_t active_candidates = 0;
  std::size_t pipeline_active_candidates = 0;

  void begin_iteration() {
    if (!local_score.operation_boundary_clean()) {
      throw std::logic_error(
          "chart SPR acceptance workspace: local scorer crossed an operation "
          "boundary with a stale borrow");
    }
    active_candidates = 0;
    pipeline_active_candidates = 0;
  }

  void reserve_batch(std::size_t candidate_count,
                     bool reserve_pipeline_buffer = false) {
    if (candidate_slots.capacity() < candidate_count) {
      candidate_slots.reserve(candidate_count);
    }
    if (candidate_copy_scratch.capacity() < candidate_count) {
      candidate_copy_scratch.reserve(candidate_count);
    }
    if (local_results.capacity() < candidate_count) {
      local_results.reserve(candidate_count);
    }
    if (reserve_pipeline_buffer) {
      if (pipeline_candidate_slots.capacity() < candidate_count) {
        pipeline_candidate_slots.reserve(candidate_count);
      }
      if (pipeline_candidate_copy_scratch.capacity() < candidate_count) {
        pipeline_candidate_copy_scratch.reserve(candidate_count);
      }
    }
  }

  void append_candidate_to_buffer(std::size_t buffer,
                                  grammar_spr_candidate const& candidate) {
    auto& slots = buffer == 0 ? candidate_slots : pipeline_candidate_slots;
    auto& scratch =
        buffer == 0 ? candidate_copy_scratch : pipeline_candidate_copy_scratch;
    auto& active = buffer == 0 ? active_candidates : pipeline_active_candidates;
    if (buffer > 1) {
      throw std::out_of_range(
          "chart SPR acceptance workspace: pipeline buffer out of range");
    }
    if (active == slots.size()) {
      slots.emplace_back();
      try {
        scratch.emplace_back();
      } catch (...) {
        slots.pop_back();
        throw;
      }
    }
    copy_grammar_spr_candidate_reusing_storage(slots[active], candidate,
                                               scratch[active]);
    ++active;
  }

  void append_candidate(grammar_spr_candidate const& candidate) {
    append_candidate_to_buffer(0, candidate);
  }

  [[nodiscard]] std::span<grammar_spr_candidate const> candidates(
      std::size_t buffer = 0) const {
    if (buffer > 1) {
      throw std::out_of_range(
          "chart SPR acceptance workspace: pipeline buffer out of range");
    }
    auto const& slots =
        buffer == 0 ? candidate_slots : pipeline_candidate_slots;
    auto active = buffer == 0 ? active_candidates : pipeline_active_candidates;
    return {slots.data(), active};
  }

  [[nodiscard]] std::size_t local_admission_additional_resident_bytes(
      bool include_local_results = true) const {
    std::size_t total =
        sizeof(chart_spr_acceptance_iteration_workspace) - sizeof(local_score);
    total = local_capacity_checked_add(
        total, candidate_buffer_owned_capacity_bytes(0),
        "chart SPR lazy-local candidate buffer 0 ownership");
    total = local_capacity_checked_add(
        total, candidate_buffer_owned_capacity_bytes(1),
        "chart SPR lazy-local candidate buffer 1 ownership");
    total = local_capacity_checked_add(
        total, local_result_owned_capacity_bytes(include_local_results),
        "chart SPR lazy-local result ownership");
    return total;
  }

  [[nodiscard]] std::size_t candidate_buffer_owned_capacity_bytes(
      std::size_t buffer) const {
    if (buffer > 1) {
      throw std::out_of_range(
          "chart SPR acceptance workspace: pipeline buffer out of range");
    }
    auto const& slots =
        buffer == 0 ? candidate_slots : pipeline_candidate_slots;
    auto const& copy_scratch =
        buffer == 0 ? candidate_copy_scratch : pipeline_candidate_copy_scratch;
    auto total = local_owned_dynamic_capacity_bytes(slots);
    total = local_capacity_checked_add(
        total, local_vector_dynamic_capacity_bytes(copy_scratch),
        "chart SPR lazy-local candidate-copy slots");
    for (auto const& scratch : copy_scratch) {
      total = local_capacity_checked_add(
          total,
          local_owned_dynamic_capacity_sum(
              scratch.payload.reachability_stack,
              scratch.payload.affected_queue, scratch.payload.spare_temp_clades,
              scratch.payload.spare_temp_productions,
              scratch.payload.spare_production_witnesses,
              scratch.payload.spare_child_witnesses,
              scratch.payload.spare_temp_productions_by_base_parent,
              scratch.payload.spare_temp_productions_by_temp_parent,
              scratch.payload.spare_temp_productions_by_base_child,
              scratch.payload.spare_temp_productions_by_temp_child,
              scratch.source_before_topology, scratch.source_after_topology),
          "chart SPR lazy-local candidate-copy scratch");
    }
    return total;
  }

  [[nodiscard]] std::size_t local_result_owned_capacity_bytes(
      bool include_elements = true) const {
    auto total = local_vector_dynamic_capacity_bytes(local_results);
    if (include_elements) {
      for (auto const& result : local_results) {
        total = local_capacity_checked_add(
            total, local_owned_dynamic_capacity_bytes(result.invalid_reason),
            "chart SPR lazy-local result reasons");
      }
    }
    return total;
  }

  [[nodiscard]] std::size_t local_admission_non_candidate_buffer_resident_bytes(
      bool include_local_results = true) const {
    auto total =
        sizeof(chart_spr_acceptance_iteration_workspace) - sizeof(local_score);
    total = local_capacity_checked_add(
        total, local_result_owned_capacity_bytes(include_local_results),
        "chart SPR lazy-local result ownership");
    return total;
  }

  [[nodiscard]] std::span<chart_spr_local_score_result> results(
      std::size_t buffer = 0) {
    if (buffer > 1) {
      throw std::out_of_range(
          "chart SPR acceptance workspace: pipeline buffer out of range");
    }
    auto active = buffer == 0 ? active_candidates : pipeline_active_candidates;
    if (local_results.size() < active) {
      local_results.resize(active);
    }
    return {local_results.data(), active};
  }

  [[nodiscard]] std::size_t active_candidate_count(
      std::size_t buffer) const noexcept {
    return buffer == 0 ? active_candidates : pipeline_active_candidates;
  }

  void finish_batch(std::size_t buffer = 0) noexcept {
    if (buffer == 0) {
      active_candidates = 0;
    } else {
      pipeline_active_candidates = 0;
    }
  }

  void release_retained_storage_before_exact() {
    if (active_candidates != 0 || pipeline_active_candidates != 0 ||
        !local_score.operation_boundary_clean()) {
      throw std::logic_error(
          "chart SPR acceptance workspace: cannot release active storage");
    }
    std::vector<grammar_spr_candidate>{}.swap(candidate_slots);
    std::vector<grammar_spr_candidate_copy_scratch>{}.swap(
        candidate_copy_scratch);
    std::vector<grammar_spr_candidate>{}.swap(pipeline_candidate_slots);
    std::vector<grammar_spr_candidate_copy_scratch>{}.swap(
        pipeline_candidate_copy_scratch);
    std::vector<chart_spr_local_score_result>{}.swap(local_results);
    local_score.release_retained_storage();
  }
};

struct chart_spr_candidate_pipeline_snapshot {
  std::uint64_t grammar_generation = 0;
  std::uint64_t plan_generation = 0;
  chart_plan_fingerprint plan_fingerprint;
  chart_spr_pattern_source_fingerprint pattern_fingerprint;
  std::size_t active_pattern_count = 0;
  std::uint64_t state_score = 0;

  bool operator==(chart_spr_candidate_pipeline_snapshot const&) const = default;
};

inline chart_spr_candidate_pipeline_snapshot
capture_chart_spr_candidate_pipeline_snapshot(
    chart_spr_search_state const& state) {
  return chart_spr_candidate_pipeline_snapshot{
      .grammar_generation = state.grammar.execution_generation,
      .plan_generation = state.execution_plan.grammar_generation(),
      .plan_fingerprint = state.execution_plan.fingerprint(),
      .pattern_fingerprint = state.pattern_source_fingerprint,
      .active_pattern_count = state.active_patterns.patterns.patterns.size(),
      .state_score = state.composite_lower_bound_with_invariants,
  };
}

inline void validate_chart_spr_candidate_pipeline_snapshot(
    chart_spr_search_state const& state,
    chart_spr_candidate_pipeline_snapshot const& expected) {
  auto const actual = capture_chart_spr_candidate_pipeline_snapshot(state);
  if (actual != expected) {
    throw chart_execution_plan_mismatch(
        "chart SPR candidate pipeline: stale state generation/epoch");
  }
  auto checked =
      check_chart_execution_plan(state.grammar, state.execution_plan);
  checked.assert_same(state.grammar, state.execution_plan);
}

enum class chart_spr_candidate_pipeline_slot_status {
  free,
  filling,
  ready,
  scoring,
};

struct chart_spr_candidate_pipeline_slot_state {
  chart_spr_candidate_pipeline_slot_status status =
      chart_spr_candidate_pipeline_slot_status::free;
  std::size_t sequence = 0;
  chart_spr_candidate_pipeline_snapshot snapshot;
};

struct chart_spr_candidate_pipeline_controller {
  std::mutex state_mutex;
  std::condition_variable state_changed;
  std::mutex scheduler_handoff;
  std::array<chart_spr_candidate_pipeline_slot_state, 2> slots;
  std::atomic<bool> cancel_requested{false};
  bool producer_done = false;
  bool scoring_active = false;
  std::exception_ptr producer_failure;
  chart_spr_candidate_generation_stats generation;
  chart_spr_candidate_generation_stats pipeline_diagnostics;
  sampled_tree_projection_scheduler_diagnostics projection_scheduler;
  double candidate_generation_ms = 0.0;
  std::size_t stale_multifurcation_candidates = 0;
};

inline void record_chart_spr_candidate_generation_scheduler_axis(
    chart_spr_scheduler_axis_metrics& axis,
    sampled_tree_projection_scheduler_diagnostics const& diagnostics) {
  axis.operations += diagnostics.operations;
  axis.parallel_operations += diagnostics.parallel_operations;
  axis.items += diagnostics.items;
  axis.ranges += diagnostics.ranges;
  axis.worker_tasks += diagnostics.worker_tasks;
  axis.active_worker_high_water = std::max(
      axis.active_worker_high_water, diagnostics.active_worker_high_water);
  if (diagnostics.minimum_effective_grain != 0) {
    axis.minimum_effective_grain =
        axis.minimum_effective_grain == 0
            ? diagnostics.minimum_effective_grain
            : std::min(axis.minimum_effective_grain,
                       diagnostics.minimum_effective_grain);
  }
  axis.maximum_effective_grain = std::max(axis.maximum_effective_grain,
                                          diagnostics.maximum_effective_grain);
}

struct chart_spr_exact_candidate_slot {
  std::optional<chart_spr_candidate_score> result;
  chart_spr_search_counters counters;
  std::exception_ptr failure;
  double elapsed_ms = 0.0;
};

}  // namespace chart_spr_search_detail

// Score candidates with the Phase-3 local lower-bound scorer, retain the best
// candidates according to the configured candidate-selection policy, and apply
// the Phase-4 acceptance gate.  This function does not mutate/materialize the
// DAG; Phase 5 is responsible for applying an accepted candidate and rebuilding
// sidecar state.  Rejected candidates are locally scored only, while exact
// verification materializes overlays only for the retained verified set.
inline chart_spr_iteration_result run_chart_spr_acceptance_iteration(
    chart_spr_search_state const& state, chart_spr_search_options options,
    std::size_t iteration,
    chart_spr_search_detail::chart_spr_acceptance_iteration_workspace&
        workspace,
    chart_scheduler& scheduler) {
  workspace.begin_iteration();
  validate_supported_chart_cache_options(options.cache);
  auto exact_memory_budget = options.cache.memory_budget_bytes;
  if (state.cache_opts.memory_budget_bytes != 0) {
    exact_memory_budget =
        exact_memory_budget == 0
            ? state.cache_opts.memory_budget_bytes
            : std::min(exact_memory_budget,
                       state.cache_opts.memory_budget_bytes);
  }
  if (options.candidate_selection ==
      chart_spr_candidate_selection_mode::sampled_or_randomized) {
    options.enumeration.randomize_order = true;
    options.enumeration.seed = options.seed;
  }

  // This function owns options by value, so moving its enumeration state
  // avoids copying a potentially heap-backed reversal key before finite
  // admission has had a chance to reject the iteration.
  auto enumeration = std::move(options.enumeration);
  if (options.candidate_selection ==
      chart_spr_candidate_selection_mode::sampled_or_randomized) {
    enumeration.randomize_order = true;
    enumeration.seed = options.seed;
  }
  if (options.max_candidates_per_iteration != 0) {
    enumeration.max_candidates = options.max_candidates_per_iteration;
    enumeration.max_candidates_is_post_dedup = true;
  }
  if (enumeration.source != chart_spr_candidate_source::grammar &&
      state.dag != nullptr) {
    enumeration.sampled_tree_source_dag = state.dag;
  }
  auto const finite_lazy_local_admission =
      state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart &&
      exact_memory_budget != 0;
  auto const worker_count = scheduler.worker_resolution().resolved_workers;
  auto const pipeline_requested =
      options.enable_candidate_generation_pipeline && worker_count > 1;
  auto const finite_pipeline_admission =
      pipeline_requested && exact_memory_budget != 0;
  auto const finite_iteration_admission =
      finite_lazy_local_admission || finite_pipeline_admission;
  if (finite_iteration_admission &&
      (enumeration.reservoir_sample || enumeration.max_candidates == 0)) {
    // Reservoir retains the full underlying source visit graph as well as its
    // candidate vector. Uncapped enumeration likewise has no finite signature
    // bound. Both remain fail-closed before rank, buffer, or source allocation.
    throw chart_spr_search_detail::
        chart_spr_lazy_local_enumeration_budget_error(enumeration.source);
  }
  auto const use_pipeline = pipeline_requested;
  auto const candidate_buffer_count =
      use_pipeline ? std::size_t{2} : std::size_t{1};
  auto candidate_batch_size =
      chart_spr_effective_candidate_batch_size(state, options, worker_count);
  if (finite_iteration_admission) {
    candidate_batch_size =
        std::min(candidate_batch_size, enumeration.max_candidates);
  }
  candidate_batch_size = std::max<std::size_t>(1, candidate_batch_size);
  auto const rank_limit = chart_spr_rank_buffer_limit(options);
  auto const ranked_reserve_limit =
      finite_iteration_admission
          ? std::min(enumeration.max_candidates,
                     rank_limit == chart_spr_rank_unlimited
                         ? enumeration.max_candidates
                         : rank_limit)
          : (rank_limit != chart_spr_rank_unlimited ? rank_limit : 0);
  bool const capture_semantics =
      options.semantic_capture != chart_spr_semantic_capture_mode::off;
  auto const local_task_slots =
      std::max<std::size_t>(1, std::min(worker_count, candidate_batch_size));
  std::optional<
      chart_spr_search_detail::grammar_spr_finite_iteration_memory_envelope>
      finite_iteration_envelope;
  auto const finite_scheduler_resident =
      finite_iteration_admission
          ? chart_spr_search_detail::
                estimate_chart_spr_scheduler_resident_bytes(scheduler)
          : std::size_t{0};
  auto fail_finite_iteration_budget = [&](std::size_t candidate_index,
                                          std::size_t required_bytes,
                                          std::size_t available_bytes) -> void {
    ++state.counters.lazy_local_pre_submit_budget_failures;
    throw chart_spr_search_detail::chart_spr_lazy_local_budget_error(
        candidate_index, required_bytes, available_bytes);
  };
  if (finite_iteration_admission) {
    // Establish an empty acceptance ownership graph before either a cold exact
    // trim or the iteration reserves are allowed to allocate.
    workspace.release_retained_storage_before_exact();
  }

  // Schema-2 fingerprinting performs only forward-ordered reverse-map lookups,
  // so checked publication is allocation-free and can retain mismatch
  // precedence over every finite memory rejection.
  auto checked_state = [&] {
    try {
      return check_chart_execution_plan(state.grammar, state.execution_plan);
    } catch (chart_execution_plan_mismatch const&) {
      ++state.counters.plan_mismatch_rejections;
      throw;
    }
  }();
  if (finite_iteration_admission && capture_semantics &&
      options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite &&
      chart_multisite_detail::keep_mask_kind_for_options(options.exact_trim) ==
          multisite_keep_mask_kind::exact_optimal_production_union &&
      !options.exact_trim.capture_optimal_root_provenance) {
    // The compatibility companion keeps a second complete B&B live beside
    // the primary trim. Finite runs require the production primary-capture
    // contract instead of guessing at that overlap.
    throw chart_spr_search_detail::
        chart_spr_canonical_exact_evidence_budget_error{};
  }
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    chart_spr_search_detail::validate_chart_spr_exact_multisite_multifurcation_gate(
        state.grammar, "chart SPR acceptance iteration");
    if (state.exact_trim_active_only) {
      // An iteration may tighten (but never loosen) the state budget.  Reject
      // a cache+published-old-trim overrun before candidate generation grows
      // its workspace, even though no exact-state rebuild is needed.
      require_chart_spr_retained_exact_state_memory_budget(
          state, *state.exact_trim_active_only, scheduler, exact_memory_budget);
    } else {
      require_chart_spr_state_exact_memory_budget(
          state, options.exact_trim, scheduler, exact_memory_budget);
      (void)ensure_chart_spr_state_exact_trim(
          state, checked_state, scheduler, options.exact_trim);
      require_chart_spr_retained_exact_state_memory_budget(
          state, *state.exact_trim_active_only, scheduler, exact_memory_budget);
    }
    if (finite_iteration_admission && capture_semantics &&
        state.exact_trim_active_only->keep_production_exact &&
        state.exact_trim_active_only->optimal_root_provenance_classes.empty()) {
      throw chart_spr_search_detail::
          chart_spr_canonical_exact_evidence_budget_error{};
    }
  } else if (options.acceptance_mode ==
                 chart_spr_acceptance_mode::fixed_topology_exact &&
             exact_memory_budget != 0 &&
             estimate_chart_spr_published_state_resident_bytes(state) >
                 exact_memory_budget) {
    // Fixed-topology verification needs no published exact trim, but a caller
    // may tighten the iteration budget below the already-resident chart/cache
    // generation. Reject that state before candidate generation allocates its
    // batch/rank workspace.
    throw chart_spr_exact_state_budget_error(
        estimate_chart_spr_published_state_resident_bytes(state),
        exact_memory_budget);
  }
  state.counters.lazy_local_retained_exact_trim_bytes_max = std::max(
      state.counters.lazy_local_retained_exact_trim_bytes_max,
      estimate_chart_spr_retained_exact_trim_bytes(state));

  if (finite_iteration_admission) {
    auto estimate_envelope = [&](std::size_t source_wave_size,
                                 std::size_t projection_wave_size,
                                 std::size_t grammar_wave_size) {
      return chart_spr_search_detail::
          estimate_grammar_spr_finite_iteration_memory_envelope(
              state, enumeration.max_candidates, candidate_batch_size,
              ranked_reserve_limit, capture_semantics, scheduler,
              local_task_slots, &enumeration, candidate_buffer_count,
              use_pipeline, source_wave_size, projection_wave_size,
              grammar_wave_size);
    };
    try {
      auto selected = estimate_envelope(0, 0, 0);
      if (selected.planned_required_bytes > exact_memory_budget) {
        auto const has_sampled_source =
            enumeration.source != chart_spr_candidate_source::grammar;
        auto const has_parallel_grammar_source =
            enumeration.source != chart_spr_candidate_source::sampled_tree &&
            selected.planned_grammar_candidate_wave_size != 0;
        auto const minimum_source =
            has_sampled_source ? std::size_t{1} : std::size_t{0};
        auto const minimum_projection = minimum_source;
        auto const minimum_grammar =
            has_parallel_grammar_source ? std::size_t{1} : std::size_t{0};
        auto minimum = estimate_envelope(minimum_source, minimum_projection,
                                         minimum_grammar);
        selected = minimum;
        if (minimum.planned_required_bytes <= exact_memory_budget) {
          auto selected_source = minimum_source;
          auto selected_projection = minimum_projection;

          // Admit sampled source concurrency at the irreducible one-projection
          // and one-grammar-construction working set.
          if (has_sampled_source) {
            auto const maximum_source_wave =
                estimate_envelope(0, 1, minimum_grammar)
                    .planned_sampled_source_wave_size;
            std::size_t low = 1;
            std::size_t high = maximum_source_wave;
            while (low < high) {
              auto const midpoint = low + (high - low + 1) / 2;
              auto candidate = estimate_envelope(midpoint, 1, minimum_grammar);
              if (candidate.planned_required_bytes <= exact_memory_budget) {
                low = midpoint;
                selected = std::move(candidate);
              } else {
                high = midpoint - 1;
              }
            }
            selected_source = low;

            auto const maximum_projection_wave =
                estimate_envelope(selected_source, 0, minimum_grammar)
                    .planned_sampled_projection_wave_size;
            low = 1;
            high = maximum_projection_wave;
            selected = estimate_envelope(selected_source, 1, minimum_grammar);
            while (low < high) {
              auto const midpoint = low + (high - low + 1) / 2;
              auto candidate =
                  estimate_envelope(selected_source, midpoint, minimum_grammar);
              if (candidate.planned_required_bytes <= exact_memory_budget) {
                low = midpoint;
                selected = std::move(candidate);
              } else {
                high = midpoint - 1;
              }
            }
            selected_projection = low;
          }

          // Grammar construction is a third independent dimension and is
          // admitted only after the sampled source/projection widths have been
          // fixed. All three stages are scheduler-handoff alternatives.
          if (has_parallel_grammar_source) {
            auto const maximum_grammar_wave =
                estimate_envelope(selected_source, selected_projection, 0)
                    .planned_grammar_candidate_wave_size;
            std::size_t low = 1;
            std::size_t high = maximum_grammar_wave;
            selected =
                estimate_envelope(selected_source, selected_projection, 1);
            while (low < high) {
              auto const midpoint = low + (high - low + 1) / 2;
              auto candidate = estimate_envelope(selected_source,
                                                 selected_projection, midpoint);
              if (candidate.planned_required_bytes <= exact_memory_budget) {
                low = midpoint;
                selected = std::move(candidate);
              } else {
                high = midpoint - 1;
              }
            }
          }
        }
      }
      finite_iteration_envelope = std::move(selected);
    } catch (std::overflow_error const&) {
      fail_finite_iteration_budget(0, (std::numeric_limits<std::size_t>::max)(),
                                   exact_memory_budget);
    }
    state.counters.lazy_local_iteration_envelope_bytes_max =
        std::max(state.counters.lazy_local_iteration_envelope_bytes_max,
                 finite_iteration_envelope->planned_required_bytes);
    state.counters.lazy_local_iteration_generation_phase_bytes_max = std::max(
        state.counters.lazy_local_iteration_generation_phase_bytes_max,
        finite_iteration_envelope->planned_generation_phase_required_bytes);
    state.counters.lazy_local_iteration_evidence_phase_bytes_max = std::max(
        state.counters.lazy_local_iteration_evidence_phase_bytes_max,
        finite_iteration_envelope->planned_evidence_phase_required_bytes);
    state.counters.lazy_local_iteration_task_stable_bytes_max =
        std::max(state.counters.lazy_local_iteration_task_stable_bytes_max,
                 finite_iteration_envelope->planned_local_task_stable_bytes);
    state.counters
        .lazy_local_iteration_task_preparation_peak_bytes_max = std::max(
        state.counters.lazy_local_iteration_task_preparation_peak_bytes_max,
        finite_iteration_envelope->planned_local_task_preparation_peak_bytes);
    state.counters
        .lazy_local_canonical_exact_evidence_construction_peak_bytes_max =
        std::max(
            state.counters
                .lazy_local_canonical_exact_evidence_construction_peak_bytes_max,
            finite_iteration_envelope
                ->planned_canonical_state_exact_evidence_construction_peak_bytes);
    if (finite_iteration_envelope->planned_required_bytes >
        exact_memory_budget) {
      fail_finite_iteration_budget(
          0, finite_iteration_envelope->planned_required_bytes,
          exact_memory_budget);
    }
    if (enumeration.source != chart_spr_candidate_source::grammar) {
      enumeration.sampled_tree_source_maximum_wave_size =
          finite_iteration_envelope->planned_sampled_source_wave_size;
      enumeration.sampled_tree_projection_maximum_wave_size =
          finite_iteration_envelope->planned_sampled_projection_wave_size;
      enumeration.sampled_tree_source_admitted_source_count_bound =
          finite_iteration_envelope->planned_sampled_source_count_bound;
      enumeration.sampled_tree_source_admitted_destination_bound =
          finite_iteration_envelope
              ->planned_sampled_destination_bound_per_source;
      enumeration.sampled_tree_source_admitted_peak_bytes =
          finite_iteration_envelope
              ->planned_sampled_source_admitted_peak_bytes;
    }
    if (enumeration.source != chart_spr_candidate_source::sampled_tree) {
      enumeration.grammar_candidate_maximum_wave_size =
          finite_iteration_envelope->planned_grammar_candidate_wave_size;
      enumeration.grammar_candidate_admitted_wave_bytes =
          finite_iteration_envelope
              ->planned_grammar_candidate_admitted_wave_bytes;
    }
  }
  validate_chart_spr_pattern_batch_replay_strategy(state, options, enumeration,
                                                   candidate_batch_size);

  // Every owning reserve below is covered by the allocation-free finite
  // envelope above. Actual capacities are checked before the enumerator is
  // allowed to allocate its lookup/signature state.
  chart_spr_iteration_result result;
  result.iteration = iteration;
  result.acceptance_mode = options.acceptance_mode;
  result.candidate_selection = options.candidate_selection;
  std::vector<chart_spr_candidate_score> ranked;
  if (ranked_reserve_limit != 0) ranked.reserve(ranked_reserve_limit);
  std::vector<std::size_t> affected_counts;
  if (finite_iteration_admission) {
    affected_counts.reserve(enumeration.max_candidates);
    if (capture_semantics) {
      result.canonical_candidates.reserve(enumeration.max_candidates);
      result.canonical_ranked_stream_indices.reserve(ranked_reserve_limit);
    }
  }
  workspace.reserve_batch(candidate_batch_size, use_pipeline);

  if (finite_iteration_admission) {
    auto actual_with_future =
        estimate_chart_spr_published_state_resident_bytes(state);
    actual_with_future = chart_spr_search_detail::local_capacity_checked_add(
        actual_with_future, finite_scheduler_resident,
        "chart SPR finite grammar scheduler ownership");
    actual_with_future = chart_spr_search_detail::local_capacity_checked_add(
        actual_with_future,
        workspace.local_admission_additional_resident_bytes(),
        "chart SPR finite grammar actual acceptance capacity");
    actual_with_future = chart_spr_search_detail::local_capacity_checked_add(
        actual_with_future,
        chart_spr_search_detail::estimate_exact_loop_resident_input_bytes(
            ranked, result),
        "chart SPR finite grammar actual result capacity");
    actual_with_future = chart_spr_search_detail::local_capacity_checked_add(
        actual_with_future,
        chart_spr_search_detail::local_owned_dynamic_capacity_bytes(
            affected_counts),
        "chart SPR finite grammar actual affected capacity");
    actual_with_future = chart_spr_search_detail::local_capacity_checked_add(
        actual_with_future,
        finite_iteration_envelope->planned_local_workspace_resident_bytes,
        "chart SPR finite grammar planned local workspace");
    actual_with_future = chart_spr_search_detail::local_capacity_checked_add(
        actual_with_future,
        finite_iteration_envelope->planned_pipeline_control_bytes,
        "chart SPR finite source planned pipeline control");
    actual_with_future = chart_spr_search_detail::local_capacity_checked_add(
        actual_with_future, finite_iteration_envelope->future_dynamic_bytes,
        "chart SPR finite grammar future live capacity");
    if (actual_with_future > exact_memory_budget ||
        actual_with_future >
            finite_iteration_envelope->planned_required_bytes) {
      fail_finite_iteration_budget(0, actual_with_future, exact_memory_budget);
    }
  }

  result.state_score_before = chart_spr_iteration_state_score_before(
      state, checked_state, options, scheduler);
  result.state_score_after = result.state_score_before;
  if (capture_semantics) {
    result.canonical_seed = options.seed;
  }
  // Candidate construction and sampled-tree projection share the one
  // search-lifetime scheduler; no source may construct a per-wave pool.  The
  // historical option name is retained because both generation paths use the
  // same scheduler handoff/cancellation contract.
  enumeration.sampled_tree_projection_scheduler = &scheduler;
  if (enumeration.source != chart_spr_candidate_source::grammar) {
    if (exact_memory_budget != 0) {
      if (finite_iteration_admission) {
        // The source-aware iteration envelope is the sole admission owner and
        // has already selected/bound the projection wave. The direct-library
        // wrapper must not re-add published state, buffers, scoring, or the
        // scheduler core.
        enumeration.sampled_tree_projection_memory_budget_bytes = 0;
        enumeration.sampled_tree_projection_external_resident_bytes = 0;
      } else {
        enumeration.sampled_tree_projection_memory_budget_bytes =
            enumeration.sampled_tree_projection_memory_budget_bytes == 0
                ? exact_memory_budget
                : std::min(
                      exact_memory_budget,
                      enumeration.sampled_tree_projection_memory_budget_bytes);
        auto projection_external_resident =
            estimate_chart_spr_published_state_resident_bytes(state);
        projection_external_resident =
            chart_spr_search_detail::local_capacity_checked_add(
                projection_external_resident,
                workspace.local_admission_additional_resident_bytes(),
                "chart SPR sampled projection acceptance capacity");
        projection_external_resident =
            chart_spr_search_detail::local_capacity_checked_add(
                projection_external_resident,
                chart_spr_search_detail::
                    estimate_exact_loop_resident_input_bytes(ranked, result),
                "chart SPR sampled projection result capacity");
        projection_external_resident =
            chart_spr_search_detail::local_capacity_checked_add(
                projection_external_resident,
                chart_spr_search_detail::local_owned_dynamic_capacity_bytes(
                    affected_counts),
                "chart SPR sampled projection affected capacity");
        enumeration.sampled_tree_projection_external_resident_bytes = std::max(
            enumeration.sampled_tree_projection_external_resident_bytes,
            projection_external_resident);
      }
    }
  }
  state.effective_candidate_batch_size = candidate_batch_size;
  auto local_options = local_spr_score_options{};
  local_options.verify_against_full_overlay =
      options.verify_local_against_full_for_tests;
  local_options.force_dense_invalid_reason_for_tests =
      options.force_dense_invalid_reason_for_tests;
  auto const enumeration_fixed_resident =
      finite_iteration_admission
          ? (enumeration.source == chart_spr_candidate_source::grammar &&
                     !use_pipeline
                 ? chart_spr_search_detail::local_capacity_checked_add(
                       chart_spr_search_detail::
                           estimate_grammar_spr_enumeration_fixed_live_bytes(
                               state.grammar, state.execution_plan),
                       finite_iteration_envelope
                           ->planned_grammar_candidate_wave_owned_bytes,
                       "chart SPR finite grammar construction wave")
                 : finite_iteration_envelope->planned_source_owned_bytes)
          : std::size_t{0};
  auto const pipeline_control_resident =
      finite_iteration_admission
          ? finite_iteration_envelope->planned_pipeline_control_bytes
          : std::size_t{0};
  std::size_t enumeration_seen_resident = 0;
  std::size_t enumeration_seen_count = 0;
  std::size_t enumeration_current_candidate_resident = 0;
  bool enumeration_active = true;

  auto finite_workspace_resident = [&](bool include_local_results = true) {
    if (!use_pipeline || !enumeration_active) {
      // While the producer is live, its two buffers cannot be inspected
      // concurrently and are charged at their admitted capacities below. Once
      // it has joined, release_retained_storage_before_exact() makes the
      // ordinary capacity walker authoritative; carrying the admitted buffers
      // into the evidence phase would double-count storage that no longer
      // exists and spuriously reject exact E.
      return workspace.local_admission_additional_resident_bytes(
          include_local_results);
    }
    auto total = workspace.local_admission_non_candidate_buffer_resident_bytes(
        include_local_results);
    total = chart_spr_search_detail::local_capacity_checked_add(
        total,
        chart_spr_search_detail::local_capacity_checked_multiply(
            candidate_buffer_count,
            finite_iteration_envelope->planned_candidate_buffer_owned_bytes,
            "chart SPR finite pipeline admitted candidate buffers"),
        "chart SPR finite pipeline workspace ownership");
    return total;
  };

  auto finite_actual_live_bytes = [&]() {
    auto total = estimate_chart_spr_published_state_resident_bytes(state);
    total = chart_spr_search_detail::local_capacity_checked_add(
        total, finite_scheduler_resident,
        "chart SPR finite grammar live scheduler ownership");
    total = chart_spr_search_detail::local_capacity_checked_add(
        total, finite_workspace_resident(),
        "chart SPR finite grammar live acceptance capacity");
    total = chart_spr_search_detail::local_capacity_checked_add(
        total,
        chart_spr_search_detail::local_score_workspace_access::
            full_resident_capacity_bytes(workspace.local_score),
        "chart SPR finite grammar live local workspace");
    total = chart_spr_search_detail::local_capacity_checked_add(
        total,
        chart_spr_search_detail::estimate_exact_loop_resident_input_bytes(
            ranked, result),
        "chart SPR finite grammar live result capacity");
    total = chart_spr_search_detail::local_capacity_checked_add(
        total,
        chart_spr_search_detail::local_owned_dynamic_capacity_bytes(
            affected_counts),
        "chart SPR finite grammar live affected capacity");
    if (enumeration_active) {
      total = chart_spr_search_detail::local_capacity_checked_add(
          total, enumeration_fixed_resident,
          "chart SPR finite live source capacity");
      total = chart_spr_search_detail::local_capacity_checked_add(
          total, pipeline_control_resident,
          "chart SPR finite live pipeline control capacity");
      total = chart_spr_search_detail::local_capacity_checked_add(
          total, enumeration_seen_resident,
          "chart SPR finite grammar live signature capacity");
      total = chart_spr_search_detail::local_capacity_checked_add(
          total,
          finite_iteration_envelope
              ->planned_enumeration_callback_concurrent_bytes,
          "chart SPR finite live source callback scratch");
    }
    return total;
  };

  auto check_finite_actual_live = [&](std::size_t candidate_index) {
    if (!finite_iteration_admission) return;
    auto const actual = finite_actual_live_bytes();
    if (actual > exact_memory_budget ||
        actual > finite_iteration_envelope->planned_required_bytes) {
      fail_finite_iteration_budget(candidate_index, actual,
                                   exact_memory_budget);
    }
  };
  check_finite_actual_live(0);

  bool stop_after_batch = false;

  auto process_candidate_batch = [&](std::size_t buffer) {
    if (workspace.active_candidate_count(buffer) == 0) return;
    auto candidate_batch = workspace.candidates(buffer);
    if (finite_iteration_admission) {
      auto const tile_plan =
          chart_spr_search_detail::plan_local_candidate_pattern_tiles(
              state, candidate_batch, &scheduler);
      auto const fusion_plan =
          chart_spr_search_detail::plan_local_pattern_batch_fusion(
              state, candidate_batch.size(), &scheduler);
      auto const lazy_local = state.cache_strategy ==
                              chart_spr_cache_strategy::lazy_multisite_chart;
      auto const resident_local =
          state.cache_strategy ==
              chart_spr_cache_strategy::all_active_patterns ||
          lazy_local || state.local_commit_inside_rows.valid();
      auto const runtime_prepared_slots =
          lazy_local ? std::min(worker_count, candidate_batch.size())
          : resident_local
              ? (tile_plan.enabled() ? candidate_batch.size() : worker_count)
              : candidate_batch.size();
      auto const runtime_worker_slots =
          lazy_local ? std::min(worker_count, candidate_batch.size())
                     : worker_count;
      auto const runtime_tile_result_slots =
          std::max(tile_plan.total_tiles, fusion_plan.total_tiles);
      if (finite_iteration_envelope->planned_cache_strategy !=
              state.cache_strategy ||
          candidate_batch.size() >
              finite_iteration_envelope->planned_candidate_batch_size ||
          runtime_prepared_slots >
              finite_iteration_envelope->planned_local_prepared_slots ||
          runtime_worker_slots >
              finite_iteration_envelope->planned_local_worker_slots ||
          runtime_tile_result_slots >
              finite_iteration_envelope->planned_local_tile_result_slots) {
        throw std::logic_error(
            "chart SPR finite pipeline: runtime local scoring plan exceeded "
            "unified admission");
      }
    }
    // Grow caller-owned result storage before the timed `_into` region.
    auto local_results = workspace.results(buffer);
    auto local_start = std::chrono::steady_clock::now();
    auto batch_local_options = local_options;
    batch_local_options.admission_memory_budget_bytes = exact_memory_budget;
    std::size_t additional_resident = 0;
    if (finite_iteration_admission) {
      additional_resident =
          finite_workspace_resident(/*include_local_results=*/false);
      additional_resident = chart_spr_search_detail::local_capacity_checked_add(
          additional_resident,
          chart_spr_search_detail::estimate_exact_loop_resident_input_bytes(
              ranked, result),
          "chart SPR lazy-local retained iteration inputs");
      additional_resident = chart_spr_search_detail::local_capacity_checked_add(
          additional_resident,
          chart_spr_search_detail::local_owned_dynamic_capacity_bytes(
              affected_counts),
          "chart SPR lazy-local retained affected counts");
    }
    if (finite_iteration_admission && enumeration_active) {
      additional_resident = chart_spr_search_detail::local_capacity_checked_add(
          additional_resident, enumeration_fixed_resident,
          "chart SPR lazy-local live source storage");
      additional_resident = chart_spr_search_detail::local_capacity_checked_add(
          additional_resident,
          finite_iteration_envelope
              ->planned_enumeration_callback_concurrent_bytes,
          "chart SPR lazy-local live source callback scratch");
      additional_resident = chart_spr_search_detail::local_capacity_checked_add(
          additional_resident, pipeline_control_resident,
          "chart SPR lazy-local live pipeline control storage");
      additional_resident = chart_spr_search_detail::local_capacity_checked_add(
          additional_resident, enumeration_seen_resident,
          "chart SPR lazy-local live enumeration signatures");
    }
    batch_local_options.admission_additional_resident_bytes =
        additional_resident;
    batch_local_options
        .admission_additional_resident_includes_result_objects = true;
    batch_local_options
        .admission_additional_resident_includes_candidate_inputs = true;
    if (finite_iteration_admission) {
      batch_local_options.admission_weighted_candidate_order_capacity_bytes =
          finite_iteration_envelope
              ->planned_local_weighted_candidate_order_bytes;
      batch_local_options.admission_pattern_batch_construction_scratch_bytes =
          finite_iteration_envelope
              ->planned_pattern_batch_construction_scratch_bytes;
    }
    score_candidates_locally_into(state, candidate_batch, local_results,
                                  workspace.local_score, batch_local_options,
                                  scheduler, checked_state);
    result.local_scoring_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - local_start)
            .count();
    // Candidate ownership is promoted only after the allocation-sensitive
    // `_into` region and after its workspace is clean.  Promotion was inside
    // the historical owning scorer's aggregate timer (and, for resident-cache
    // scoring, inside each candidate timer), so attribute it explicitly while
    // keeping canonical capture/ranking outside local-scoring time.
    for (std::size_t i = 0; i < candidate_batch.size(); ++i) {
      auto promotion_start = std::chrono::steady_clock::now();
      auto scored =
          promote_chart_spr_local_score(candidate_batch[i], local_results[i]);
      auto promotion_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - promotion_start)
              .count();
      result.local_scoring_ms += promotion_ms;
      scored.local_score_ms += promotion_ms;
      scored.force_candidate_exact_bnb_overflow_for_tests =
          options.force_candidate_exact_bnb_overflow_for_tests;
      ++result.candidates_scored;
      if (capture_semantics) {
        auto const stream_index = result.canonical_candidates.size();
        scored.canonical_stream_index = stream_index;
        scored.force_canonical_evidence_failure_for_tests =
            options.force_canonical_evidence_failure_for_tests;
        chart_spr_canonical_candidate_record record;
        record.stream_index = stream_index;
        record.signature = chart_spr_candidate_sample_signature(
            state.grammar, scored.candidate);
        record.valid = scored.valid;
        record.invalid_reason = scored.invalid_reason;
        record.affected_clade_count = scored.affected_clade_count;
        record.lower_bound =
            chart_spr_canonicalize_objective_score(scored.lower_bound);
        if (finite_iteration_admission) {
          auto canonical_dynamic =
              chart_spr_search_detail::local_owned_dynamic_capacity_sum(
                  record.signature, record.invalid_reason,
                  record.lower_bound.kind, record.lower_bound.convention);
          if (canonical_dynamic >
              finite_iteration_envelope
                  ->planned_canonical_record_dynamic_bytes) {
            fail_finite_iteration_budget(stream_index, canonical_dynamic,
                                         exact_memory_budget);
          }
        }
        result.canonical_candidates.push_back(std::move(record));
      }
      if (!scored.valid) {
        ++result.candidate_score_failures;
        check_finite_actual_live(i);
        continue;
      }
      affected_counts.push_back(scored.affected_clade_count);
      if (scored.lower_bound.value.improves()) {
        ++result.local_improving_candidates;
      }
      chart_spr_insert_ranked_candidate(state.grammar, ranked,
                                        std::move(scored), rank_limit);
      check_finite_actual_live(i);
    }
  };

  chart_spr_candidate_generation_stats generation;
  sampled_tree_projection_scheduler_diagnostics serial_projection_scheduler;
  if (!use_pipeline) {
    enumeration.sampled_tree_projection_scheduler_diagnostics_sink =
        &serial_projection_scheduler;
    double generation_callback_ms = 0.0;
    auto const generation_start = std::chrono::steady_clock::now();
    try {
      generation = for_each_grammar_spr_candidate(
          state.grammar, checked_state, enumeration,
          [&](grammar_spr_candidate const& candidate) {
            auto const callback_start = std::chrono::steady_clock::now();
            if (finite_iteration_admission &&
                enumeration.source == chart_spr_candidate_source::grammar) {
              if (enumeration_seen_count >= enumeration.max_candidates) {
                fail_finite_iteration_budget(
                    enumeration_seen_count,
                    chart_spr_search_detail::local_capacity_checked_add(
                        enumeration_seen_count, 1,
                        "chart SPR finite grammar generated candidate count"),
                    enumeration.max_candidates);
              }
              auto const signature_live = chart_spr_search_detail::
                  estimate_grammar_spr_enumeration_signature_live_bytes(
                      state.grammar, candidate);
              if (signature_live >
                  finite_iteration_envelope->planned_signature_node_bytes) {
                fail_finite_iteration_budget(
                    enumeration_seen_count, signature_live,
                    finite_iteration_envelope->planned_signature_node_bytes);
              }
              enumeration_seen_resident =
                  chart_spr_search_detail::local_capacity_checked_add(
                      enumeration_seen_resident, signature_live,
                      "chart SPR lazy-local live enumeration signatures");
              ++enumeration_seen_count;
              enumeration_current_candidate_resident =
                  chart_spr_search_detail::local_capacity_checked_add(
                      sizeof(candidate),
                      chart_spr_search_detail::
                          local_owned_dynamic_capacity_bytes(candidate),
                      "chart SPR lazy-local live enumeration candidate");
              if (enumeration_current_candidate_resident >
                  finite_iteration_envelope->planned_candidate_live_bytes) {
                fail_finite_iteration_budget(
                    enumeration_seen_count - 1,
                    enumeration_current_candidate_resident,
                    finite_iteration_envelope->planned_candidate_live_bytes);
              }
              check_finite_actual_live(enumeration_seen_count - 1);
            } else if (finite_iteration_admission) {
              ++enumeration_seen_count;
            }
            workspace.append_candidate(candidate);
            check_finite_actual_live(
                enumeration_seen_count == 0 ? 0 : enumeration_seen_count - 1);
            if (workspace.active_candidates >= candidate_batch_size) {
              process_candidate_batch(0);
              workspace.finish_batch(0);
              enumeration_current_candidate_resident = 0;
              if (options.candidate_selection ==
                      chart_spr_candidate_selection_mode::
                          lower_bound_first_improvement &&
                  result.local_improving_candidates > 0) {
                stop_after_batch = true;
                generation_callback_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - callback_start)
                        .count();
                return false;
              }
            }
            generation_callback_ms +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - callback_start)
                    .count();
            return true;
          });
    } catch (...) {
      chart_spr_search_detail::
          record_chart_spr_candidate_generation_scheduler_axis(
              state.counters.scheduler_axes.candidate_generation,
              serial_projection_scheduler);
      workspace.finish_batch(0);
      throw;
    }
    auto const generation_total_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - generation_start)
            .count();
    result.candidate_generation_ms =
        std::max(0.0, generation_total_ms - generation_callback_ms);
    if (!stop_after_batch) {
      process_candidate_batch(0);
      workspace.finish_batch(0);
    }
    chart_spr_search_detail::
        record_chart_spr_candidate_generation_scheduler_axis(
            state.counters.scheduler_axes.candidate_generation,
            serial_projection_scheduler);
  } else {
    if (options.before_candidate_pipeline_start_for_tests) {
      options.before_candidate_pipeline_start_for_tests();
    }

    chart_spr_search_detail::chart_spr_candidate_pipeline_controller pipeline;
    auto const pipeline_snapshot =
        chart_spr_search_detail::capture_chart_spr_candidate_pipeline_snapshot(
            state);
    enumeration.sampled_tree_projection_scheduler_handoff_mutex =
        &pipeline.scheduler_handoff;
    enumeration.sampled_tree_projection_cancel_requested =
        &pipeline.cancel_requested;
    enumeration.sampled_tree_projection_scheduler_diagnostics_sink =
        &pipeline.projection_scheduler;

    std::jthread producer{[&] {
      std::optional<std::size_t> filling_buffer;
      std::size_t next_sequence = 0;
      std::size_t callbacks_seen = 0;
      double callback_ms = 0.0;
      auto const generation_start = std::chrono::steady_clock::now();
      auto publish_filling_buffer = [&] {
        if (!filling_buffer) return;
        auto const buffer = *filling_buffer;
        auto& slot = pipeline.slots[buffer];
        slot.sequence = next_sequence;
        slot.snapshot = pipeline_snapshot;
        if (options
                .force_candidate_pipeline_stale_buffer_after_batches_for_tests ==
            next_sequence) {
          ++slot.snapshot.grammar_generation;
        }
        {
          std::lock_guard lock{pipeline.state_mutex};
          slot.status = chart_spr_search_detail::
              chart_spr_candidate_pipeline_slot_status::ready;
          ++pipeline.pipeline_diagnostics.candidate_pipeline_batches_generated;
          if (pipeline.scoring_active) {
            ++pipeline.pipeline_diagnostics
                  .candidate_pipeline_serial_overlap_batches;
          }
        }
        ++next_sequence;
        filling_buffer.reset();
        pipeline.state_changed.notify_all();
      };
      auto acquire_filling_buffer = [&]() -> bool {
        if (filling_buffer) return true;
        auto const buffer = next_sequence % 2;
        auto const wait_start = std::chrono::steady_clock::now();
        std::unique_lock lock{pipeline.state_mutex};
        auto const stalled = pipeline.slots[buffer].status !=
                             chart_spr_search_detail::
                                 chart_spr_candidate_pipeline_slot_status::free;
        pipeline.state_changed.wait(lock, [&] {
          return pipeline.cancel_requested.load(std::memory_order_acquire) ||
                 pipeline.slots[buffer].status ==
                     chart_spr_search_detail::
                         chart_spr_candidate_pipeline_slot_status::free;
        });
        if (stalled) {
          ++pipeline.pipeline_diagnostics.candidate_pipeline_producer_stalls;
          pipeline.pipeline_diagnostics
              .candidate_pipeline_producer_stall_nanoseconds +=
              static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - wait_start)
                      .count());
        }
        if (pipeline.cancel_requested.load(std::memory_order_acquire)) {
          return false;
        }
        pipeline.slots[buffer].status = chart_spr_search_detail::
            chart_spr_candidate_pipeline_slot_status::filling;
        workspace.finish_batch(buffer);
        filling_buffer = buffer;
        return true;
      };

      try {
        pipeline.generation = for_each_grammar_spr_candidate(
            state.grammar, checked_state, enumeration,
            [&](grammar_spr_candidate const& candidate) {
              auto const callback_start = std::chrono::steady_clock::now();
              if (pipeline.cancel_requested.load(std::memory_order_acquire) ||
                  !acquire_filling_buffer()) {
                ++pipeline.pipeline_diagnostics
                      .candidate_pipeline_stale_candidates_discarded;
                if (chart_spr_detail::
                        grammar_spr_candidate_involves_multifurcation(
                            state.grammar, candidate)) {
                  ++pipeline.stale_multifurcation_candidates;
                }
                callback_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - callback_start)
                        .count();
                return false;
              }
              if (options
                      .force_candidate_pipeline_generation_failure_after_for_tests ==
                  callbacks_seen) {
                throw std::runtime_error(
                    "chart SPR candidate pipeline: forced generation failure");
              }
              ++callbacks_seen;
              workspace.append_candidate_to_buffer(*filling_buffer, candidate);
              if (finite_iteration_admission) {
                auto const buffer_owned =
                    workspace.candidate_buffer_owned_capacity_bytes(
                        *filling_buffer);
                if (buffer_owned > finite_iteration_envelope
                                       ->planned_candidate_buffer_owned_bytes) {
                  throw chart_spr_search_detail::
                      chart_spr_lazy_local_budget_error(
                          callbacks_seen - 1, buffer_owned,
                          finite_iteration_envelope
                              ->planned_candidate_buffer_owned_bytes);
                }
              }
              if (workspace.active_candidate_count(*filling_buffer) >=
                  candidate_batch_size) {
                publish_filling_buffer();
              }
              callback_ms +=
                  std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - callback_start)
                      .count();
              return !pipeline.cancel_requested.load(std::memory_order_acquire);
            });
        if (!pipeline.cancel_requested.load(std::memory_order_acquire) &&
            filling_buffer &&
            workspace.active_candidate_count(*filling_buffer) != 0) {
          publish_filling_buffer();
        }
      } catch (...) {
        ++pipeline.pipeline_diagnostics.candidate_pipeline_generation_errors;
        pipeline.producer_failure = std::current_exception();
        pipeline.cancel_requested.store(true, std::memory_order_release);
      }
      pipeline.candidate_generation_ms =
          std::max(0.0, std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - generation_start)
                                .count() -
                            callback_ms);
      {
        std::lock_guard lock{pipeline.state_mutex};
        pipeline.producer_done = true;
      }
      pipeline.state_changed.notify_all();
    }};

    std::exception_ptr consumer_failure;
    std::size_t next_sequence = 0;
    std::size_t scored_batches = 0;
    try {
      for (;;) {
        auto const buffer = next_sequence % 2;
        auto const wait_start = std::chrono::steady_clock::now();
        std::unique_lock state_lock{pipeline.state_mutex};
        auto const stalled =
            pipeline.slots[buffer].status !=
            chart_spr_search_detail::chart_spr_candidate_pipeline_slot_status::
                ready;
        pipeline.state_changed.wait(state_lock, [&] {
          return (pipeline.slots[buffer].status ==
                      chart_spr_search_detail::
                          chart_spr_candidate_pipeline_slot_status::ready &&
                  pipeline.slots[buffer].sequence == next_sequence) ||
                 pipeline.producer_done;
        });
        auto& slot = pipeline.slots[buffer];
        if (slot.status !=
                chart_spr_search_detail::
                    chart_spr_candidate_pipeline_slot_status::ready ||
            slot.sequence != next_sequence) {
          break;
        }
        if (stalled) {
          ++pipeline.pipeline_diagnostics.candidate_pipeline_consumer_stalls;
          pipeline.pipeline_diagnostics
              .candidate_pipeline_consumer_stall_nanoseconds +=
              static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - wait_start)
                      .count());
        }
        slot.status = chart_spr_search_detail::
            chart_spr_candidate_pipeline_slot_status::scoring;
        auto const buffer_snapshot = slot.snapshot;
        state_lock.unlock();

        auto const handoff_start = std::chrono::steady_clock::now();
        std::unique_lock scheduler_handoff{pipeline.scheduler_handoff};
        auto const handoff_wait = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - handoff_start)
                .count());
        if (handoff_wait != 0) {
          ++pipeline.pipeline_diagnostics.candidate_pipeline_consumer_stalls;
          pipeline.pipeline_diagnostics
              .candidate_pipeline_consumer_stall_nanoseconds += handoff_wait;
        }
        {
          std::lock_guard lock{pipeline.state_mutex};
          pipeline.scoring_active = true;
        }
        try {
          if (options.before_candidate_pipeline_score_batch_for_tests) {
            options.before_candidate_pipeline_score_batch_for_tests(
                scored_batches);
          }
          chart_spr_search_detail::
              validate_chart_spr_candidate_pipeline_snapshot(state,
                                                             buffer_snapshot);
          if (options
                  .force_candidate_pipeline_scoring_failure_after_batches_for_tests ==
              scored_batches) {
            throw std::runtime_error(
                "chart SPR candidate pipeline: forced scoring failure");
          }
          process_candidate_batch(buffer);
          if (options.after_candidate_pipeline_score_batch_for_tests) {
            options.after_candidate_pipeline_score_batch_for_tests(
                scored_batches);
          }
        } catch (chart_execution_plan_mismatch const&) {
          ++pipeline.pipeline_diagnostics
                .candidate_pipeline_state_epoch_rejections;
          ++state.counters.plan_mismatch_rejections;
          throw;
        }
        {
          std::lock_guard lock{pipeline.state_mutex};
          pipeline.scoring_active = false;
        }
        scheduler_handoff.unlock();

        ++scored_batches;
        ++pipeline.pipeline_diagnostics.candidate_pipeline_batches_scored;
        auto const early_improvement = options.candidate_selection ==
                                           chart_spr_candidate_selection_mode::
                                               lower_bound_first_improvement &&
                                       result.local_improving_candidates > 0;
        auto const forced_cancel =
            options
                .force_candidate_pipeline_cancel_after_scored_batches_for_tests ==
            scored_batches;
        if (early_improvement || forced_cancel) {
          stop_after_batch = true;
          ++pipeline.pipeline_diagnostics.candidate_pipeline_cancellations;
          pipeline.cancel_requested.store(true, std::memory_order_release);
        }

        workspace.finish_batch(buffer);
        {
          std::lock_guard lock{pipeline.state_mutex};
          slot.status = chart_spr_search_detail::
              chart_spr_candidate_pipeline_slot_status::free;
        }
        ++next_sequence;
        pipeline.state_changed.notify_all();
        if (stop_after_batch) break;
      }
    } catch (...) {
      {
        std::lock_guard lock{pipeline.state_mutex};
        pipeline.scoring_active = false;
      }
      pipeline.cancel_requested.store(true, std::memory_order_release);
      pipeline.state_changed.notify_all();
      consumer_failure = std::current_exception();
    }

    if (stop_after_batch || consumer_failure) {
      pipeline.cancel_requested.store(true, std::memory_order_release);
      pipeline.state_changed.notify_all();
    }
    producer.join();

    // No state publication or accepted commit can occur before this drain.
    // Invalidate both buffers after the producer has stopped touching them.
    for (std::size_t buffer = 0; buffer < 2; ++buffer) {
      auto const active = workspace.active_candidate_count(buffer);
      auto const status = pipeline.slots[buffer].status;
      if (active != 0 &&
          status != chart_spr_search_detail::
                        chart_spr_candidate_pipeline_slot_status::free) {
        ++pipeline.pipeline_diagnostics
              .candidate_pipeline_stale_batches_discarded;
        pipeline.pipeline_diagnostics
            .candidate_pipeline_stale_candidates_discarded += active;
        for (auto const& candidate : workspace.candidates(buffer)) {
          if (chart_spr_detail::grammar_spr_candidate_involves_multifurcation(
                  state.grammar, candidate)) {
            ++pipeline.stale_multifurcation_candidates;
          }
        }
      }
      workspace.finish_batch(buffer);
      pipeline.slots[buffer].status = chart_spr_search_detail::
          chart_spr_candidate_pipeline_slot_status::free;
    }
    pipeline.pipeline_diagnostics
        .candidate_pipeline_producer_stall_nanoseconds +=
        pipeline.generation
            .sampled_tree_projection_scheduler_handoff_stall_nanoseconds;
    if (pipeline.generation
            .sampled_tree_projection_scheduler_handoff_stall_nanoseconds != 0) {
      ++pipeline.pipeline_diagnostics.candidate_pipeline_producer_stalls;
    }
    pipeline.pipeline_diagnostics.candidate_pipeline_estimated_peak_bytes =
        std::max(
            finite_iteration_admission
                ? finite_iteration_envelope->planned_required_bytes
                : std::size_t{0},
            pipeline.generation.sampled_tree_projection_estimated_peak_bytes);
    pipeline.generation.candidate_pipeline_batches_generated =
        pipeline.pipeline_diagnostics.candidate_pipeline_batches_generated;
    pipeline.generation.candidate_pipeline_batches_scored =
        pipeline.pipeline_diagnostics.candidate_pipeline_batches_scored;
    pipeline.generation.candidate_pipeline_serial_overlap_batches =
        pipeline.pipeline_diagnostics.candidate_pipeline_serial_overlap_batches;
    pipeline.generation
        .candidate_pipeline_scheduler_projection_overlap_batches = 0;
    pipeline.generation.candidate_pipeline_producer_stalls =
        pipeline.pipeline_diagnostics.candidate_pipeline_producer_stalls;
    pipeline.generation.candidate_pipeline_consumer_stalls =
        pipeline.pipeline_diagnostics.candidate_pipeline_consumer_stalls;
    pipeline.generation.candidate_pipeline_producer_stall_nanoseconds =
        pipeline.pipeline_diagnostics
            .candidate_pipeline_producer_stall_nanoseconds;
    pipeline.generation.candidate_pipeline_consumer_stall_nanoseconds =
        pipeline.pipeline_diagnostics
            .candidate_pipeline_consumer_stall_nanoseconds;
    pipeline.generation.candidate_pipeline_cancellations =
        pipeline.pipeline_diagnostics.candidate_pipeline_cancellations;
    pipeline.generation.candidate_pipeline_stale_batches_discarded =
        pipeline.pipeline_diagnostics
            .candidate_pipeline_stale_batches_discarded;
    pipeline.generation.candidate_pipeline_stale_candidates_discarded =
        pipeline.pipeline_diagnostics
            .candidate_pipeline_stale_candidates_discarded;
    pipeline.generation.candidate_pipeline_state_epoch_rejections =
        pipeline.pipeline_diagnostics.candidate_pipeline_state_epoch_rejections;
    pipeline.generation.candidate_pipeline_generation_errors =
        pipeline.pipeline_diagnostics.candidate_pipeline_generation_errors;
    pipeline.generation.candidate_pipeline_estimated_peak_bytes =
        pipeline.pipeline_diagnostics.candidate_pipeline_estimated_peak_bytes;
    // The legacy post-dedup count names candidates visible to scoring.  Work
    // generated against this still-current snapshot but invalidated on an
    // early stop is reported only by the stale-work diagnostics.
    if (pipeline.generation.candidates_generated_after_dedup >=
        pipeline.generation.candidate_pipeline_stale_candidates_discarded) {
      pipeline.generation.candidates_generated_after_dedup -=
          pipeline.generation.candidate_pipeline_stale_candidates_discarded;
    }
    if (pipeline.generation.spr_multifurcation_moves_generated >=
        pipeline.stale_multifurcation_candidates) {
      pipeline.generation.spr_multifurcation_moves_generated -=
          pipeline.stale_multifurcation_candidates;
    }
    chart_spr_search_detail::
        record_chart_spr_candidate_generation_scheduler_axis(
            state.counters.scheduler_axes.candidate_generation,
            pipeline.projection_scheduler);
    result.candidate_generation_ms = pipeline.candidate_generation_ms;
    generation = std::move(pipeline.generation);
    if (consumer_failure) {
      record_chart_spr_candidate_generation_stats(generation, state.counters);
      std::rethrow_exception(consumer_failure);
    }
    if (pipeline.producer_failure) {
      record_chart_spr_candidate_generation_stats(generation, state.counters);
      try {
        std::rethrow_exception(pipeline.producer_failure);
      } catch (
          chart_spr_search_detail::chart_spr_lazy_local_budget_error const&) {
        ++state.counters.lazy_local_pre_submit_budget_failures;
        throw;
      }
    }
    chart_spr_search_detail::validate_chart_spr_candidate_pipeline_snapshot(
        state, pipeline_snapshot);
  }
  enumeration_active = false;

  // No local descriptor/candidate-batch capacity is needed after the final
  // generation callback. Under a finite budget, release it before result
  // vector finalization and accepted-candidate copying so those later growth
  // surfaces do not silently overlap a retained local high-water mark.
  if (finite_iteration_admission) {
    workspace.release_retained_storage_before_exact();
    check_finite_actual_live(
        enumeration_seen_count == 0 ? 0 : enumeration_seen_count - 1);
  }

  result.candidate_generation = generation;
  result.candidates_generated = generation.candidates_generated_after_dedup;
  result.locally_ranked_candidates_retained = ranked.size();
  result.affected_distribution =
      summarize_affected_clade_counts(affected_counts);
  result.affected_clade_counts = std::move(affected_counts);
  record_chart_spr_candidate_generation_stats(generation, state.counters);

  if (capture_semantics) {
    result.canonical_ranked_stream_indices.reserve(ranked.size());
    for (std::size_t rank = 0; rank < ranked.size(); ++rank) {
      if (ranked[rank].canonical_stream_index ==
          (std::numeric_limits<std::size_t>::max)()) {
        throw std::logic_error(
            "chart-SPR canonical report: retained candidate missing stream "
            "index");
      }
      auto stream_index = ranked[rank].canonical_stream_index;
      if (stream_index >= result.canonical_candidates.size()) {
        throw std::logic_error(
            "chart-SPR canonical report: ranked stream index out of range");
      }
      result.canonical_ranked_stream_indices.push_back(stream_index);
      result.canonical_candidates[stream_index].ranked_index = rank;
    }
    check_finite_actual_live(
        enumeration_seen_count == 0 ? 0 : enumeration_seen_count - 1);
  }

  auto const no_ranked_candidates_retained = ranked.empty();

  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::lower_bound_heuristic) {
    for (auto const& candidate : ranked) {
      if (chart_spr_candidate_has_accepting_improvement(
              candidate, options.acceptance_mode)) {
        result.accepted = candidate;
        result.state_score_before = candidate.lower_bound.value.old_score;
        result.state_score_after = candidate.lower_bound.value.new_score;
        ++state.counters.candidate_accepts_attempted;
        break;
      }
    }
    check_finite_actual_live(
        enumeration_seen_count == 0 ? 0 : enumeration_seen_count - 1);
  } else {
    // Include serial selector resolution, memory estimation/admission, and
    // coordinator aggregation in the exact-stage measurement. Otherwise new
    // Phase-6 overhead could disappear from the advertised scaling metric.
    auto const exact_stage_start = std::chrono::steady_clock::now();
    // Candidate generation/local scoring retain high-water slot and scratch
    // capacities for later iterations.  They are no longer useful while exact
    // results are resident, so release them before admission instead of
    // silently omitting a second large memory domain from the unified bound.
    // A later iteration may rebuild this storage after the exact/commit
    // barrier; no borrow crosses this boundary.
    workspace.release_retained_storage_before_exact();

    // Publish one immutable old trim before candidate readers.  If the state
    // did not already own it, its own unified preflight and retained-capacity
    // backstop run inside this call.
    multisite_trim_result const* published_old_trim = nullptr;
    if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite &&
        !ranked.empty()) {
      published_old_trim = &ensure_chart_spr_state_exact_trim(
          state, checked_state, scheduler, options.exact_trim);
    }
    auto const resolved_workers =
        scheduler.worker_resolution().resolved_workers;
    auto const finite_budget = exact_memory_budget != 0;
    if (state.fixed_topology_exact_verifier &&
        state.contextual_fixed_topology_exact_verifier) {
      throw std::logic_error(
          "chart SPR exact verification: both legacy and contextual "
          "fixed-topology verifiers are installed");
    }
    if (state.exact_multisite_verifier &&
        state.contextual_exact_multisite_verifier) {
      throw std::logic_error(
          "chart SPR exact verification: both legacy and contextual "
          "multisite verifiers are installed");
    }
    auto const legacy_multisite_verifier_active =
        options.acceptance_mode ==
            chart_spr_acceptance_mode::exact_multisite &&
        options.verification_mode ==
            chart_spr_verification_mode::transient &&
        static_cast<bool>(state.exact_multisite_verifier);
    auto exact_candidate_worker_limit = resolved_workers;
    if ((options.acceptance_mode ==
             chart_spr_acceptance_mode::fixed_topology_exact &&
         (state.fixed_topology_exact_verifier ||
          (state.contextual_fixed_topology_exact_verifier &&
           !state.fixed_topology_exact_verifier_parallel_safe))) ||
        (options.acceptance_mode ==
             chart_spr_acceptance_mode::exact_multisite &&
         options.verification_mode == chart_spr_verification_mode::transient &&
         (state.exact_multisite_verifier ||
          (state.contextual_exact_multisite_verifier &&
           !state.exact_multisite_verifier_parallel_safe)))) {
      exact_candidate_worker_limit = 1;
    }
    auto const scheduler_resident =
        chart_spr_search_detail::estimate_chart_spr_scheduler_resident_bytes(
            scheduler);

    // Topology selection precedes the ordinary candidate estimate because its
    // complete certificate becomes verifier input.  Under a finite budget,
    // pre-admit every serial selector's scratch and cumulative output before
    // invoking either the built-in selector or a user callback.  Custom
    // providers without a matching estimator fail closed here.
    if (finite_budget && options.acceptance_mode ==
                             chart_spr_acceptance_mode::fixed_topology_exact &&
        !ranked.empty()) {
      auto selector_resident =
          estimate_chart_spr_published_state_resident_bytes(state);
      selector_resident = chart_spr_exact_candidate_checked_bytes_add(
          selector_resident, scheduler_resident,
          "chart SPR topology-selector scheduler ownership");
      selector_resident = chart_spr_exact_candidate_checked_bytes_add(
          selector_resident,
          chart_spr_search_detail::estimate_exact_loop_resident_input_bytes(
              ranked, result),
          "chart SPR topology-selector resident live inputs");
      if (selector_resident > exact_memory_budget) {
        throw chart_spr_exact_candidate_budget_error(
            0, selector_resident, exact_memory_budget);
      }
      // Computing the structural selector estimate itself constructs the
      // virtual-topology memo/visit/removed vectors. Its allocation-free
      // envelope must fit before calling the allocation-heavy estimator; the
      // returned scratch below instead covers the later selector invocation.
      auto const selector_estimator_scratch =
          chart_spr_search_detail::
              estimate_exact_loop_estimator_peak_scratch_bytes(state,
                                                                ranked);
      std::size_t carried_selector_results = 0;
      for (std::size_t rank = 0; rank < ranked.size(); ++rank) {
        auto const already = chart_spr_exact_candidate_saturating_bytes_add(
            selector_resident, carried_selector_results);
        auto const available = already <= exact_memory_budget
                                   ? exact_memory_budget - already
                                   : std::size_t{0};
        if (already > exact_memory_budget ||
            selector_estimator_scratch > available) {
          throw chart_spr_exact_candidate_budget_error(
              rank, selector_estimator_scratch, available);
        }
        auto const estimate = estimate_chart_spr_topology_selection_memory(
            state, ranked[rank], options);
        auto const required =
            !estimate.safely_bounded
                ? (std::numeric_limits<std::size_t>::max)()
                : chart_spr_exact_candidate_saturating_bytes_add(
                      estimate.scratch_bytes,
                      estimate.retained_result_bytes);
        if (already > exact_memory_budget || required > available) {
          throw chart_spr_exact_candidate_budget_error(rank, required,
                                                       available);
        }
        carried_selector_results =
            chart_spr_exact_candidate_saturating_bytes_add(
                carried_selector_results, estimate.retained_result_bytes);
      }
    }
    // User-provided topology selectors have no thread-safety contract. Resolve
    // every certificate serially before publishing the ranked candidates to
    // exact-verification readers.
    for (auto& candidate : ranked) {
      attach_fixed_topology_selection_for_acceptance(state, candidate, options);
    }

    auto resident_exact_base =
        estimate_chart_spr_published_state_resident_bytes(state);
    resident_exact_base = chart_spr_exact_candidate_checked_bytes_add(
        resident_exact_base, scheduler_resident,
        "chart SPR exact-candidate scheduler ownership");
    resident_exact_base = chart_spr_exact_candidate_checked_bytes_add(
        resident_exact_base,
        chart_spr_search_detail::estimate_exact_loop_resident_input_bytes(
            ranked, result),
        "chart SPR exact-candidate resident live inputs");

    auto exact_loop_capacity = ranked.size();
    if (!ranked.empty()) {
      exact_loop_capacity = std::max<std::size_t>(
          4, chart_spr_exact_candidate_checked_bytes_multiply(
                 ranked.size(), 2,
                 "chart SPR exact-candidate coordinator capacity"));
    }
    auto exact_loop_fixed_bytes =
        chart_spr_exact_candidate_checked_bytes_multiply(
            exact_loop_capacity,
            sizeof(chart_spr_search_detail::chart_spr_exact_candidate_slot),
            "chart SPR exact-candidate result-slot storage");
    exact_loop_fixed_bytes = chart_spr_exact_candidate_checked_bytes_add(
        exact_loop_fixed_bytes,
        chart_spr_exact_candidate_checked_bytes_multiply(
            exact_loop_capacity,
            sizeof(chart_spr_exact_candidate_memory_estimate),
            "chart SPR exact-candidate estimate-vector storage"),
        "chart SPR exact-candidate fixed loop storage");
    exact_loop_fixed_bytes = chart_spr_exact_candidate_checked_bytes_add(
        exact_loop_fixed_bytes,
        chart_spr_exact_candidate_checked_bytes_multiply(
            exact_loop_capacity, sizeof(chart_spr_candidate_score),
            "chart SPR exact-candidate verified-vector storage"),
        "chart SPR exact-candidate fixed loop storage");
    exact_loop_fixed_bytes = chart_spr_exact_candidate_checked_bytes_add(
        exact_loop_fixed_bytes,
        chart_spr_exact_candidate_checked_bytes_multiply(
            exact_loop_capacity, sizeof(double),
            "chart SPR exact-candidate timing-vector storage"),
        "chart SPR exact-candidate fixed loop storage");
    if (capture_semantics) {
      exact_loop_fixed_bytes = chart_spr_exact_candidate_checked_bytes_add(
          exact_loop_fixed_bytes,
          chart_spr_exact_candidate_checked_bytes_multiply(
              exact_loop_capacity, sizeof(std::size_t),
              "chart SPR exact-candidate canonical-index storage"),
          "chart SPR exact-candidate fixed loop storage");
    }
    exact_loop_fixed_bytes = chart_spr_exact_candidate_checked_bytes_add(
        exact_loop_fixed_bytes,
        chart_spr_search_detail::
            estimate_exact_loop_estimator_peak_scratch_bytes(state, ranked),
        "chart SPR exact-candidate estimator scratch");
    auto const exact_range_options = chart_indexed_range_options{
        .minimum_grain = 1, .target_ranges_per_worker = 1};
    resident_exact_base = chart_spr_exact_candidate_checked_bytes_add(
        resident_exact_base, exact_loop_fixed_bytes,
        "chart SPR exact-candidate resident fixed storage");
    if (finite_budget &&
        resident_exact_base > exact_memory_budget) {
      throw chart_spr_exact_candidate_budget_error(
          0, resident_exact_base, exact_memory_budget);
    }

    std::vector<chart_spr_candidate_score> verified;
    verified.reserve(ranked.size());
    std::vector<chart_spr_search_detail::chart_spr_exact_candidate_slot> slots(
        ranked.size());
    std::vector<chart_spr_exact_candidate_memory_estimate> memory_estimates;
    memory_estimates.reserve(ranked.size());
    auto memory_options = options;
    memory_options.cache.memory_budget_bytes = exact_memory_budget;
    for (auto const& candidate : ranked) {
      auto estimate =
          candidate.valid
              ? estimate_chart_spr_exact_candidate_memory(
                    state, candidate, memory_options, resolved_workers)
              : chart_spr_exact_candidate_memory_estimate{};
      if (candidate.valid) {
        estimate.inner_scheduler_scratch_bytes = chart_spr_search_detail::
            estimate_chart_spr_exact_candidate_inner_scheduler_scratch_bytes(
                state, candidate, memory_options, scheduler);
      }
      memory_estimates.push_back(estimate);
    }

    if (finite_iteration_admission && capture_semantics) {
      std::size_t ranked_evidence_bytes = 0;
      for (auto const& estimate : memory_estimates) {
        if (!estimate.safely_bounded) {
          ranked_evidence_bytes = (std::numeric_limits<std::size_t>::max)();
          break;
        }
        ranked_evidence_bytes = chart_spr_exact_candidate_checked_bytes_add(
            ranked_evidence_bytes, estimate.retained_result_bytes,
            "chart SPR finite ranked exact-evidence bytes");
      }
      auto post_release_result =
          ranked_evidence_bytes == (std::numeric_limits<std::size_t>::max)()
              ? ranked_evidence_bytes
              : chart_spr_exact_candidate_checked_bytes_add(
                    finite_iteration_envelope
                        ->planned_post_release_result_bytes,
                    ranked_evidence_bytes,
                    "chart SPR finite post-release candidate evidence");
      auto const accepted_candidate_bytes = chart_spr_search_detail::
          estimate_exact_loop_accepted_candidate_dynamic_bytes(ranked);
      if (post_release_result != (std::numeric_limits<std::size_t>::max)()) {
        post_release_result = chart_spr_exact_candidate_checked_bytes_add(
            post_release_result, accepted_candidate_bytes,
            "chart SPR finite accepted candidate ownership");
      }
      auto evidence_phase = post_release_result;
      if (evidence_phase != (std::numeric_limits<std::size_t>::max)()) {
        evidence_phase = chart_spr_exact_candidate_checked_bytes_add(
            estimate_chart_spr_published_state_resident_bytes(state),
            finite_scheduler_resident,
            "chart SPR finite evidence scheduler ownership");
        evidence_phase = chart_spr_exact_candidate_checked_bytes_add(
            evidence_phase, post_release_result,
            "chart SPR finite evidence retained result");
        evidence_phase = chart_spr_exact_candidate_checked_bytes_add(
            evidence_phase,
            finite_iteration_envelope
                ->planned_canonical_state_exact_evidence_construction_peak_bytes,
            "chart SPR finite state-evidence construction");
      }
      finite_iteration_envelope->planned_ranked_candidate_exact_evidence_bytes =
          ranked_evidence_bytes;
      finite_iteration_envelope->planned_accepted_candidate_dynamic_bytes =
          accepted_candidate_bytes;
      finite_iteration_envelope->planned_post_release_result_bytes =
          post_release_result;
      finite_iteration_envelope->planned_evidence_phase_required_bytes =
          evidence_phase;
      finite_iteration_envelope->planned_required_bytes = std::max(
          finite_iteration_envelope->planned_generation_phase_required_bytes,
          evidence_phase);
      state.counters.lazy_local_iteration_envelope_bytes_max =
          std::max(state.counters.lazy_local_iteration_envelope_bytes_max,
                   finite_iteration_envelope->planned_required_bytes);
      state.counters.lazy_local_iteration_evidence_phase_bytes_max =
          std::max(state.counters.lazy_local_iteration_evidence_phase_bytes_max,
                   evidence_phase);
      state.counters
          .lazy_local_ranked_candidate_exact_evidence_bytes_max = std::max(
          state.counters.lazy_local_ranked_candidate_exact_evidence_bytes_max,
          ranked_evidence_bytes);
      if (finite_iteration_envelope->planned_required_bytes >
          exact_memory_budget) {
        fail_finite_iteration_budget(
            0, finite_iteration_envelope->planned_required_bytes,
            exact_memory_budget);
      }
    }

    auto run_candidate = [&](std::size_t rank,
                             chart_scheduler* inner_scheduler) noexcept {
      auto& slot = slots[rank];
      auto const exact_start = std::chrono::steady_clock::now();
      try {
        auto candidate = std::move(ranked[rank]);
        if (candidate.valid) {
          chart_spr_exact_verifier_activity verifier_activity{
              state.exact_verifier_concurrency};
          if (options.before_exact_candidate_verification_for_tests) {
            options.before_exact_candidate_verification_for_tests(rank);
          }
          chart_spr_exact_verification_context context{
              slot.counters, published_old_trim, inner_scheduler};
          slot.result = verify_candidate_for_acceptance(
              state, std::move(candidate), checked_state, options, context);
        } else {
          // Certificate attachment can invalidate a candidate before verifier
          // entry. Keep its stable timing/result slot without inflating the
          // real verifier-concurrency high-water mark.
          slot.result = std::move(candidate);
        }
      } catch (...) {
        slot.failure = std::current_exception();
      }
      slot.elapsed_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - exact_start)
                            .count();
    };

    std::size_t wave_begin = 0;
    std::size_t carried_retained_bytes = 0;
    while (wave_begin < ranked.size()) {
      auto const already_resident_overflow =
          resident_exact_base >
          (std::numeric_limits<std::size_t>::max)() - carried_retained_bytes;
      auto const already_resident =
          chart_spr_exact_candidate_saturating_bytes_add(
              resident_exact_base, carried_retained_bytes);
      auto available_bytes = (std::numeric_limits<std::size_t>::max)();
      if (finite_budget) {
        if (already_resident_overflow ||
            already_resident > exact_memory_budget) {
          throw chart_spr_exact_candidate_budget_error(
              wave_begin, already_resident, exact_memory_budget);
        }
        available_bytes = exact_memory_budget - already_resident;
      }
      auto wave = plan_chart_spr_exact_candidate_admission_wave(
          memory_estimates, wave_begin, exact_candidate_worker_limit,
          available_bytes, finite_budget, &scheduler, exact_range_options);
      // The historical transient callback receives a scheduler reference and
      // may use it. It therefore cannot take the serial-scratch fallback that
      // a context-aware callback can select under pressure. Its forced
      // singleton must fit the inner-memory estimate before invocation.
      if (legacy_multisite_verifier_active &&
          !wave.use_inner_parallelism) {
        auto const& estimate = memory_estimates[wave_begin];
        auto const required =
            !estimate.safely_bounded
                ? (std::numeric_limits<std::size_t>::max)()
                : chart_spr_exact_candidate_saturating_bytes_add(
                      chart_spr_exact_candidate_saturating_bytes_add(
                          estimate.inner_parallel_scratch_bytes,
                          estimate.inner_scheduler_scratch_bytes),
                      estimate.retained_result_bytes);
        throw chart_spr_exact_candidate_budget_error(
            wave_begin, required, available_bytes);
      }
      auto const wave_count = wave.end_rank - wave.begin_rank;
      auto const projected_resident_overflow =
          already_resident >
          (std::numeric_limits<std::size_t>::max)() - wave.admitted_bytes;
      auto const projected_resident =
          chart_spr_exact_candidate_saturating_bytes_add(already_resident,
                                                         wave.admitted_bytes);
      if (finite_budget &&
          (projected_resident_overflow ||
           projected_resident > exact_memory_budget)) {
        throw std::logic_error(
            "chart SPR exact-candidate admission projected resident bytes "
            "exceed the configured budget");
      }
      state.counters.exact_candidate_peak_admitted_bytes =
          std::max(state.counters.exact_candidate_peak_admitted_bytes,
                   wave.admitted_bytes);
      state.counters.exact_candidate_peak_projected_resident_bytes =
          std::max(state.counters.exact_candidate_peak_projected_resident_bytes,
                   projected_resident);
      ++state.counters.exact_candidate_admission_batches;
      if (wave.memory_limited) {
        ++state.counters.exact_candidate_memory_limited_batches;
      }
      auto const candidate_limit = std::min(
          exact_candidate_worker_limit, ranked.size() - wave_begin);
      auto const candidates_deferred_for_memory =
          finite_budget && wave_count < candidate_limit;
      auto const wave_start = std::chrono::steady_clock::now();
      if (wave_count >= 2) {
        ++state.counters.exact_candidate_parallel_batches;
        auto const range_plan =
            scheduler.plan_indexed_ranges(wave_count, exact_range_options);
        if (wave_begin == 0 &&
            options.force_exact_candidate_submit_failure_after_for_tests) {
          auto const fail_after =
              *options.force_exact_candidate_submit_failure_after_for_tests;
          if (fail_after >= range_plan.worker_task_limit) {
            throw std::logic_error(
                "chart SPR exact-candidate submit-failure hook does not "
                "select a submitted runner");
          }
          chart_scheduler_test_detail::access::fail_submission_after(
              scheduler, fail_after);
        }
        auto const scheduler_before = scheduler.metrics();
        try {
          auto run = scheduler.for_each_indexed_range(
              wave_count, exact_range_options,
              [&](chart_indexed_range const& range, std::size_t,
                  chart_scheduler_cancellation_token const&) {
                for (std::size_t offset = range.begin; offset < range.end;
                     ++offset) {
                  run_candidate(wave_begin + offset, nullptr);
                }
              });
          record_chart_spr_scheduler_axis_run(
              state.counters.scheduler_axes.exact_candidates, run);
        } catch (...) {
          record_chart_spr_scheduler_axis_failed_run(
              state.counters.scheduler_axes.exact_candidates, range_plan,
              scheduler_before, scheduler.metrics());
          for (std::size_t rank = wave_begin;
               rank < wave_begin + wave_count; ++rank) {
            add_chart_spr_search_counters(state.counters,
                                          slots[rank].counters);
          }
          // Scheduler infrastructure failure wins over failures from already
          // accepted lower-rank runners because the intended stable wave was
          // not returned as complete. The scheduler has joined every accepted
          // runner here.
          throw;
        }
      } else if (wave.use_inner_parallelism) {
        ++state.counters.exact_candidate_inner_parallel_batches;
        run_candidate(wave_begin, &scheduler);
      } else {
        run_candidate(wave_begin, nullptr);
      }

      // Join is complete here. Merge task-local diagnostics and select hard
      // failures only in stable lower-bound rank order; completion order can
      // affect neither counters nor which exception escapes.
      for (std::size_t rank = wave_begin; rank < wave_begin + wave_count;
           ++rank) {
        add_chart_spr_search_counters(state.counters, slots[rank].counters);
      }
      for (std::size_t rank = wave_begin; rank < wave_begin + wave_count;
           ++rank) {
        if (slots[rank].failure) {
          std::rethrow_exception(slots[rank].failure);
        }
      }
      if (candidates_deferred_for_memory) {
        state.counters.exact_candidate_queued_for_memory_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wave_start)
                .count();
      }
      for (std::size_t rank = wave_begin; rank < wave_begin + wave_count;
           ++rank) {
        carried_retained_bytes = chart_spr_exact_candidate_saturating_bytes_add(
            carried_retained_bytes,
            memory_estimates[rank].retained_result_bytes);
      }
      wave_begin += wave_count;
    }
    for (auto& slot : slots) {
      if (!slot.result) {
        throw std::logic_error(
            "chart-SPR exact candidate task completed without a result");
      }
      auto verified_candidate = std::move(*slot.result);
      result.exact_candidate_verification_ms.push_back(slot.elapsed_ms);
      if (slot.counters.exact_verifications != 0 || verified_candidate.exact) {
        ++result.candidates_exact_verified;
      }
      if (capture_semantics) {
        if (verified_candidate.canonical_stream_index ==
            (std::numeric_limits<std::size_t>::max)()) {
          throw std::logic_error(
              "chart-SPR canonical report: verified candidate missing "
              "stream index");
        }
        auto stream_index = verified_candidate.canonical_stream_index;
        if (stream_index >= result.canonical_candidates.size()) {
          throw std::logic_error(
              "chart-SPR canonical report: verified stream index out of "
              "range");
        }
        auto& record = result.canonical_candidates[stream_index];
        record.exact_verification_index =
            result.canonical_exact_verified_stream_indices.size();
        result.canonical_exact_verified_stream_indices.push_back(stream_index);
        record.valid = verified_candidate.valid;
        record.invalid_reason = verified_candidate.invalid_reason;
        if (verified_candidate.exact) {
          record.exact = chart_spr_canonicalize_objective_score(
              *verified_candidate.exact);
        }
        if (verified_candidate.canonical_exact_evidence) {
          record.exact_evidence =
              *verified_candidate.canonical_exact_evidence;
        }
      }
      verified.push_back(std::move(verified_candidate));
    }

    chart_spr_candidate_score const* accepted_candidate = nullptr;
    for (auto const& candidate : verified) {
      if (!chart_spr_candidate_has_accepting_improvement(
              candidate, options.acceptance_mode)) {
        continue;
      }
      if (accepted_candidate == nullptr ||
          chart_spr_acceptance_candidate_better(options.acceptance_mode,
                                                state.grammar, candidate,
                                                *accepted_candidate)) {
        accepted_candidate = &candidate;
      }
    }
    // Select by reference, then copy the winner once. Repeated assignment to
    // an engaged optional could otherwise allocate a new owning payload while
    // the previous winner's destination buffers were still live.
    if (accepted_candidate != nullptr) {
      result.accepted = *accepted_candidate;
    }
    if (result.accepted) {
      auto const& accepted_score = chart_spr_candidate_acceptance_score(
          *result.accepted, options.acceptance_mode);
      result.state_score_before = accepted_score.old_score;
      result.state_score_after = accepted_score.new_score;
      ++state.counters.candidate_accepts_attempted;
    }
    result.exact_verification_ms +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - exact_stage_start)
            .count();
    std::vector<chart_spr_candidate_score>{}.swap(ranked);
  }

  if (result.accepted) {
    // The accepted candidate's dense IDs are meaningful only against this
    // pre-commit grammar.  Retain its stable sample signature before the
    // caller advances the tip, with the allocation charged to the finite
    // iteration result envelope below.
    result.accepted_candidate_signature = chart_spr_candidate_sample_signature(
        state.grammar, result.accepted->candidate);
    if (finite_iteration_admission) {
      auto const signature_bytes =
          chart_spr_search_detail::local_owned_dynamic_capacity_bytes(
              result.accepted_candidate_signature);
      if (signature_bytes > finite_iteration_envelope
                                ->planned_accepted_candidate_signature_bytes) {
        fail_finite_iteration_budget(
            0, signature_bytes,
            finite_iteration_envelope
                ->planned_accepted_candidate_signature_bytes);
      }
      check_finite_actual_live(
          enumeration_seen_count == 0 ? 0 : enumeration_seen_count - 1);
    }
  }

  if (capture_semantics &&
      options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    // Build reporting evidence only after enumeration, local workspace, and
    // exact coordinator storage are gone. The estimator accounts for every
    // encoded sample-id occurrence and the retained trim remains charged in
    // the published-state base exactly once.
    auto const& old_trim = *state.exact_trim_active_only;
    if (finite_lazy_local_admission) {
      auto const actual_before_evidence = finite_actual_live_bytes();
      auto const required_with_construction =
          chart_spr_search_detail::local_capacity_checked_add(
              actual_before_evidence,
              finite_iteration_envelope
                  ->planned_canonical_state_exact_evidence_construction_peak_bytes,
              "chart SPR canonical exact-evidence preflight");
      if (required_with_construction > exact_memory_budget ||
          required_with_construction >
              finite_iteration_envelope
                  ->planned_evidence_phase_required_bytes) {
        fail_finite_iteration_budget(0, required_with_construction,
                                     exact_memory_budget);
      }
    }
    auto evidence = chart_spr_canonicalize_search_trim_evidence(
        state.grammar, checked_state, state.active_patterns, state.chart_opts,
        options.exact_trim, old_trim, state.invariant_constant_offset);
    auto const actual_evidence_bytes =
        estimate_chart_spr_canonical_exact_evidence_resident_bytes(evidence);
    state.counters.lazy_local_canonical_exact_evidence_resident_bytes_max =
        std::max(
            state.counters
                .lazy_local_canonical_exact_evidence_resident_bytes_max,
            actual_evidence_bytes);
    if (finite_lazy_local_admission &&
        actual_evidence_bytes >
            finite_iteration_envelope
                ->planned_canonical_state_exact_evidence_resident_bytes) {
      fail_finite_iteration_budget(
          0, actual_evidence_bytes,
          finite_iteration_envelope
              ->planned_canonical_state_exact_evidence_resident_bytes);
    }
    result.canonical_state_exact_before = std::move(evidence);
    if (old_trim.keep_production_exact) {
      ++state.counters.chart_execution_plan_cache_hits;
    }
    check_finite_actual_live(
        enumeration_seen_count == 0 ? 0 : enumeration_seen_count - 1);
  }

  if (!result.accepted) {
    if (result.candidates_scored == 0) {
      result.no_accept_reason = "no candidates scored";
    } else if (options.acceptance_mode ==
               chart_spr_acceptance_mode::lower_bound_heuristic) {
      result.no_accept_reason = "no lower-bound-improving candidate";
    } else if (no_ranked_candidates_retained) {
      result.no_accept_reason = "no valid locally scored candidates retained";
    } else {
      result.no_accept_reason = "no exact-improving verified candidate";
    }
  }
  if (finite_lazy_local_admission &&
      (capture_semantics ||
       options.acceptance_mode ==
           chart_spr_acceptance_mode::lower_bound_heuristic)) {
    check_finite_actual_live(
        enumeration_seen_count == 0 ? 0 : enumeration_seen_count - 1);
  }

  bool generation_truncated =
      result.candidate_generation.stop_reason !=
      chart_spr_candidate_stop_reason::exhausted;
  bool exact_selection_non_exhaustive =
      options.acceptance_mode !=
          chart_spr_acceptance_mode::lower_bound_heuristic &&
      options.candidate_selection !=
          chart_spr_candidate_selection_mode::exhaustive_exact &&
      result.candidates_scored > result.candidates_exact_verified;
  if (generation_truncated || exact_selection_non_exhaustive) {
    result.unverified_candidates_may_contain_improvements = true;
  }

  if (result.candidates_scored > (result.accepted ? 1U : 0U)) {
    state.counters.rejected_moves +=
        result.candidates_scored - (result.accepted ? 1U : 0U);
  }
  return result;
}

inline chart_spr_iteration_result run_chart_spr_acceptance_iteration(
    chart_spr_search_state const& state, chart_spr_search_options options,
    std::size_t iteration,
    chart_spr_search_detail::chart_spr_acceptance_iteration_workspace&
        workspace) {
  auto const requested =
      chart_spr_search_detail::requested_chart_spr_worker_count(options);
  chart_scheduler scheduler{
      chart_spr_search_detail::chart_spr_compatibility_scheduler_options(
          requested)};
  auto result = run_chart_spr_acceptance_iteration(
      state, std::move(options), iteration, workspace, scheduler);
  scheduler.shutdown();
  return result;
}

inline chart_spr_iteration_result run_chart_spr_acceptance_iteration(
    chart_spr_search_state const& state, chart_spr_search_options options = {},
    std::size_t iteration = 0) {
  chart_spr_search_detail::chart_spr_acceptance_iteration_workspace workspace;
  return run_chart_spr_acceptance_iteration(state, std::move(options),
                                            iteration, workspace);
}

// Phase-5 conservative DAG-native SPR search loop.  Candidate generation and
// broad ranking use the cached local overlay-delta scorer.  Only accepted
// candidates are materialized into a tentative DAG through the rank-3 grammar-
// native path, then the sidecar search state is rebuilt once and the rebuilt
// objective gates the commit.  Locally rejected candidates therefore do not
// trigger full search-state rebuilds.
chart_spr_search_result run_chart_spr_search(
    phylo_dag initial_dag, clade_grammar initial_grammar,
    chart_spr_search_options options = {});

inline void record_chart_spr_local_candidate_score(
    chart_spr_search_counters& counters) {
  ++counters.local_candidate_scores;
}

inline void record_chart_spr_rejected_candidate(
    chart_spr_search_counters& counters) {
  ++counters.rejected_moves;
}

// Diagnostic/oracle equivalent of the legacy lower-bound helper.  It
// materializes a dense overlay grammar and calls build_composite_chart_score()
// on both the base grammar and overlay grammar, counting each completed
// expensive operation so failed candidates do not over-report completed work.
inline spr_score_result score_multisite_spr_candidate_lower_bound_oracle(
    clade_grammar const& base, site_pattern_set const& patterns,
    grammar_spr_candidate const& candidate, chart_options const& options = {},
    chart_spr_search_counters* counters = nullptr) {
  auto overlay = overlay_from_candidate(base, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  if (counters != nullptr) {
    ++counters->full_overlay_materializations;
    ++counters->overlay_materializations_for_oracle;
  }

  auto old_composite = build_composite_chart_score(base, patterns, options);
  if (counters != nullptr) {
    counters->multifurcation_productions_scored +=
        old_composite.multifurcation_productions_scored;
  }
  auto old_score = old_composite.weighted_lower_bound;
  if (counters != nullptr) ++counters->full_composite_rebuilds;
  auto new_composite =
      build_composite_chart_score(materialized.grammar, patterns, options);
  if (counters != nullptr) {
    counters->multifurcation_productions_scored +=
        new_composite.multifurcation_productions_scored;
  }
  auto new_score = new_composite.weighted_lower_bound;
  if (counters != nullptr) ++counters->full_composite_rebuilds;
  return spr_score_result{
      chart_spr_detail::signed_delta(old_score, new_score), old_score,
      new_score, false};
}

// Diagnostic/oracle equivalent of the legacy exact helper.  It recomputes both
// old and new exact trims and materializes the overlay; it is suitable for small
// oracle checks, not production top-K exact verification.
inline spr_score_result score_multisite_spr_candidate_exact_oracle(
    clade_grammar const& base, site_pattern_set const& patterns,
    grammar_spr_candidate const& candidate, chart_options const& options = {},
    multisite_trim_options const& trim_options = {},
    chart_spr_search_counters* counters = nullptr) {
  auto overlay = overlay_from_candidate(base, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  if (counters != nullptr) {
    ++counters->full_overlay_materializations;
    ++counters->overlay_materializations_for_oracle;
  }

  auto old_trim = build_multisite_trim(base, patterns, options, trim_options);
  auto new_trim = build_multisite_trim(materialized.grammar, patterns, options,
                                       trim_options);
  if (counters != nullptr) {
    record_multisite_exact_trim_work(*counters, old_trim);
    record_multisite_exact_trim_work(*counters, new_trim);
  }
  auto old_score = old_trim.optimum;
  auto new_score = new_trim.optimum;
  return spr_score_result{
      chart_spr_detail::signed_delta(old_score, new_score), old_score,
      new_score, true};
}

// Phase-0 diagnostic local recompute path used by guardrail tests and dagutil
// reporting until the lightweight overlay-delta scorer lands.  It exercises the
// existing local recompute oracle and records local candidate-score accounting;
// it must not increment full_composite_rebuilds.
inline single_site_overlay_recompute_result
score_rejected_candidate_with_local_recompute_oracle(
    overlay_clade_grammar const& overlay, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, chart_options const& options = {},
    chart_spr_search_counters* counters = nullptr) {
  auto result = build_single_site_overlay_chart_locally(
      overlay, base_chart, leaf_states, options);
  if (counters != nullptr) {
    ++counters->local_candidate_scores;
    ++counters->full_overlay_materializations;
    ++counters->overlay_materializations_for_oracle;
    counters->multifurcation_productions_scored +=
        result.chart.multifurcation_productions_scored;
  }
  return result;
}

struct chart_spr_eager_candidate_enumeration_result {
  std::vector<grammar_spr_candidate> candidates;
  chart_spr_candidate_generation_stats stats;
};

namespace chart_spr_search_detail {

inline void add_path_stats(std::vector<chart_spr_detail::upward_path> const& paths,
                           chart_spr_candidate_generation_stats& stats,
                           chart_spr_search_counters* counters) {
  stats.upward_paths_completed += paths.size();
  if (counters != nullptr) counters->upward_paths_completed += paths.size();
  for (auto const& path : paths) {
    stats.upward_path_iterator_steps += path.size();
    if (counters != nullptr) counters->upward_path_iterator_steps += path.size();
  }
}

inline void note_pruned_before(chart_spr_candidate_generation_stats& stats,
                               chart_spr_search_counters* counters) {
  ++stats.candidates_pruned_before_construction;
  if (counters != nullptr) ++counters->candidates_pruned_before_construction;
}

inline void note_pruned_after(chart_spr_candidate_generation_stats& stats,
                              chart_spr_search_counters* counters) {
  ++stats.candidates_pruned_after_construction;
  if (counters != nullptr) ++counters->candidates_pruned_after_construction;
}

}  // namespace chart_spr_search_detail

// Phase-0 diagnostic enumerator that mirrors the current eager helper behavior
// while exposing counters.  It intentionally precomputes all upward paths before
// respecting max_candidates; benchmark output can therefore reveal the old
// source-path x destination-path hazard.  This is not the Phase-1 streaming API.
inline chart_spr_eager_candidate_enumeration_result
enumerate_grammar_spr_candidates_eager_diagnostic(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options options = {},
    chart_spr_search_counters* counters = nullptr) {
  using namespace chart_spr_detail;
  using namespace chart_spr_search_detail;
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);

  chart_spr_eager_candidate_enumeration_result result;
  auto base_lookup = build_clade_lookup(grammar);
  std::set<std::string> seen;

  std::vector<std::vector<upward_path>> paths_to_root(grammar.clades.size());
  for (clade_id cid = 0; cid < grammar.clades.size(); ++cid) {
    paths_to_root[cid] = enumerate_upward_paths_to_root(grammar, cid);
    add_path_stats(paths_to_root[cid], result.stats, counters);
  }

  for (std::size_t source_pid_raw = 0;
       source_pid_raw < grammar.productions.size(); ++source_pid_raw) {
    auto source_pid = static_cast<production_id>(source_pid_raw);
    if (counters != nullptr) ++counters->candidate_source_productions_considered;
    auto const& source_prod = grammar.productions[source_pid];
    if (source_prod.children.size() < 2) continue;

    for (std::size_t moved_i = 0; moved_i < source_prod.children.size();
         ++moved_i) {
      auto moved = source_prod.children[moved_i];
      auto source_cochildren = cochildren_of(grammar, source_pid, moved);
      if (!source_cochildren) continue;
      auto const& moved_taxa = grammar.clades[moved].taxa;

      for (clade_id target = 0; target < grammar.clades.size(); ++target) {
        if (target == moved ||
            (source_cochildren->size() == 1 &&
             target == source_cochildren->front()) ||
            target == source_prod.parent) {
          note_pruned_before(result.stats, counters);
          continue;
        }
        auto const& target_taxa = grammar.clades[target].taxa;
        if (!disjoint_taxa(moved_taxa, target_taxa)) {
          note_pruned_before(result.stats, counters);
          continue;
        }

        for (auto const& source_path : paths_to_root[source_prod.parent]) {
          for (auto const& dest_path : paths_to_root[target]) {
            ++result.stats.path_pairs_considered;
            if (counters != nullptr) ++counters->path_pairs_considered;

            auto candidate = make_general_spr_candidate(
                grammar, base_lookup, source_pid, moved, target, source_path,
                dest_path);
            if (!candidate) {
              note_pruned_after(result.stats, counters);
              continue;
            }
            ++result.stats.candidates_constructed;
            if (counters != nullptr) ++counters->candidates_constructed;

            auto signature = chart_spr_candidate_taxon_signature(
                grammar, *candidate);
            if (!seen.insert(std::move(signature)).second) {
              note_pruned_after(result.stats, counters);
              continue;
            }

            if (grammar_spr_candidate_involves_multifurcation(grammar,
                                                              *candidate)) {
              ++result.stats.spr_multifurcation_moves_generated;
              if (counters != nullptr) {
                ++counters->spr_multifurcation_moves_generated;
              }
            }
            result.candidates.push_back(std::move(*candidate));
            ++result.stats.candidates_generated_after_dedup;
            if (counters != nullptr) ++counters->candidates_generated_after_dedup;

            if (options.max_candidates != 0 &&
                result.candidates.size() >= options.max_candidates) {
              result.stats.stop_reason =
                  chart_spr_candidate_stop_reason::candidate_cap;
              if (counters != nullptr) ++counters->candidate_cap_cutoffs;
              return result;
            }
          }
        }
      }
    }
  }

  result.stats.stop_reason = chart_spr_candidate_stop_reason::exhausted;
  return result;
}

}  // namespace larch

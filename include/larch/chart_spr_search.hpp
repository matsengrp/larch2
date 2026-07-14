#pragma once

#include <larch/chart_spr.hpp>
#include <larch/chart_scheduler.hpp>
#include <larch/chart_spr_semantic_report.hpp>
#include <larch/lazy_chart.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
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
  chart_spr_scheduler_axis_metrics exact_setup_patterns;
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
  add_chart_spr_scheduler_axis_metrics(dst.exact_setup_patterns,
                                       src.exact_setup_patterns);
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
         axes.exact_setup_patterns.operations +
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
  // Cross-cutting WRIC arity counter: non-binary production rows scored by dense
  // chart builds, overlay-delta local rows, and persistent cache recomputes.
  std::size_t multifurcation_productions_scored = 0;
  std::size_t local_score_parallel_batches = 0;
  std::size_t local_score_worker_tasks = 0;
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
  std::size_t exact_trim_lazy_chart_uses = 0;
  // Phase-2B local-commit cold outside-cache construction.  The production
  // path must build the inside cache once, then report zero additional inside
  // builds and one inside reuse/outside build per active pattern here.
  std::size_t outside_cache_inside_charts_built = 0;
  std::size_t outside_cache_inside_charts_reused = 0;
  std::size_t outside_cache_outside_charts_built = 0;
  std::size_t exact_verifications = 0;
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

struct chart_cache_options {
  std::size_t max_cached_patterns = 0;  // 0 = all patterns
  std::size_t memory_budget_bytes = 0;  // 0 = no explicit budget
  std::size_t candidate_batch_size = 0; // 0 = choose from memory budget
  std::size_t pattern_batch_size = 0;   // 0 = derive from cache budget
  bool use_lazy_multisite_chart = false;
};

struct chart_spr_search_state;

using chart_spr_topology_selection_provider = std::function<
    std::optional<chart_spr_topology_selection>(
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
};

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
  evidence.evidence_kind =
      trim.keep_production_exact
          ? "grammar_exact_frontier"
          : "grammar_exact_score_only_frontier_statistics";
  evidence.keep_mask_kind =
      multisite_keep_mask_kind_name(trim.keep_mask_kind);
  evidence.keep_production_exact = trim.keep_production_exact;
  evidence.optimum_active = trim.optimum;
  evidence.invariant_offset = invariant_offset;
  if (trim.keep_production_exact) {
    if (trim.keep_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          "chart-SPR canonical report: exact keep mask size mismatch");
    }
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
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa.empty()) continue;
    evidence.frontier_sizes.emplace_back(
        chart_spr_canonical_clade_sample_key(grammar,
                                             grammar.clades[cid].taxa),
        trim.frontier_sizes_by_clade[cid]);
  }
  for (auto const& source : trim.optimal_root_provenance_classes) {
    if (source.used_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          "chart-SPR canonical report: root provenance mask size mismatch");
    }
    chart_spr_canonical_root_provenance_class canonical;
    canonical.cost.reserve(source.cost.size());
    for (auto value : source.cost) canonical.cost.push_back(value);
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
  // Forces a throw at the exact materializer call boundary.  This exists only
  // to exercise exception-safe production accounting without relying on an
  // allocation failure or an impractically oversized overlay.
  bool force_exact_materializer_failure_for_tests = false;
  bool force_canonical_evidence_failure_for_tests = false;
};

inline void chart_spr_force_canonical_evidence_failure_for_tests(
    chart_spr_candidate_score const& candidate) {
  if (candidate.force_canonical_evidence_failure_for_tests) {
    throw std::runtime_error(
        "chart-SPR canonical report: forced exact-evidence failure for test");
  }
}

using chart_spr_fixed_topology_verifier = std::function<
    chart_spr_candidate_score(chart_spr_search_state const&,
                              chart_spr_candidate_score)>;

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
using chart_spr_exact_multisite_verifier = std::function<
    chart_spr_candidate_score(chart_spr_search_state const&,
                              chart_spr_candidate_score,
                              checked_chart_execution_plan_ref const&,
                              multisite_trim_options const&)>;

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

// Internal two-stage publication policy used by local-commit orchestration.
// Public builders retain the completed-state default.  When a resolved
// pattern-batch state is deferred, the local substrate becomes the sole owner
// of the initial dense recurrence and must finalize the state before any score
// or exact-trim consumer is called.
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
  std::size_t multifurcation_productions_scored = 0;
  std::size_t candidate_batches_scored = 0;
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
  // disjoint: the inside span owns any deferred pattern-batch recurrence, and
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
  // zero when no verifier runs and one for today's serial verifier loop.
  std::size_t peak_concurrent_exact_verifiers = 0;
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
  (void)cache;
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

// Build report-only tied-root provenance without changing the trim options
// used by the search itself.  This companion runs only after the algorithmic
// trim has succeeded, and callers deliberately invoke it outside semantic
// verifier / commit catches.  A report failure therefore aborts a canonical
// correctness run instead of changing candidate validity or acceptance.
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
  if (cache.use_lazy_multisite_chart) {
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
  std::size_t estimated_full_pattern_cache_bytes = 0;
  std::size_t resident_pattern_cache_bytes = 0;
  // Persistent local-commit inside+outside row caches are additional to the
  // scoring representation above. Keep their contribution separate so tip
  // refreshes can replace the scoring estimate without under-reporting the
  // two full cache surfaces.
  std::size_t local_commit_persistent_cache_bytes = 0;
  // Fixed-topology local verification bounds its per-candidate structural row
  // cache, clears it at the operation boundary, and retains only this admitted
  // high-water estimate for budget/report reconciliation.
  mutable std::size_t selected_topology_cache_admitted_bytes = 0;
  std::size_t effective_pattern_batch_size = 0;
  mutable std::size_t effective_candidate_batch_size = 0;
  std::vector<pattern_chart_cache_entry> pattern_charts;
  std::optional<lazy_multisite_chart> lazy_chart;
  // A deferred pattern-batch state is an internal, two-stage publication: its
  // grammar/plan/pattern identity is complete, but its initial composite must
  // be supplied by the local persistent inside cache before the state can be
  // scored or exactly trimmed.  Public builders never return this state.
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

  // Optional owning exact-setup source installed by local-commit
  // orchestration.  It is consulted only for pattern-batch states, whose
  // bounded public representation intentionally owns no full pattern charts.
  // Lazy and all-active paths retain their selected representations.
  chart_spr_exact_setup_provider exact_setup_provider;
  chart_spr_scheduled_exact_setup_provider scheduled_exact_setup_provider;

  // Optional Phase-8 fixed-topology verifier supplied by the local-commit
  // substrate.  When present, fixed_topology_exact verification reads the
  // persistent inside/outside caches owned by the substrate.  Conservative
  // rebuild mode leaves this empty and uses the legacy direct selected-topology
  // fallback below.
  chart_spr_fixed_topology_verifier fixed_topology_exact_verifier;

  // Optional Phase-9 transient-extension verifier supplied by the local-commit
  // substrate.  When present, exact_multisite verification transiently extends
  // the chain in reader-local scratch (never mutating the shared cache) and
  // reads the exact frontier on the extended grammar.  Conservative
  // rebuild mode leaves this empty and uses the cold from-scratch path
  // (`verify_candidate_exact_against_state`).
  //
  // Diagnostic scratch caches are gated to the optional two-chart oracle; the
  // production B&B consumes only the materialized extended grammar.
  chart_spr_exact_multisite_verifier exact_multisite_verifier;

  mutable chart_spr_search_counters counters;
};

namespace chart_spr_search_detail {

inline void require_completed_chart_spr_state_bootstrap(
    chart_spr_search_state const& state, std::string_view consumer) {
  if (state.pattern_batch_bootstrap_deferred) {
    throw std::runtime_error(
        std::string{consumer} +
        ": deferred pattern-batch bootstrap has not been finalized by the "
        "local persistent inside cache");
  }
}

// Complete the private pattern-batch publication after the local persistent
// cache has built the one authoritative set of dense inside charts.  The cache
// composite includes the state's invariant constant exactly once.
inline void finalize_deferred_pattern_batch_bootstrap(
    chart_spr_search_state& state,
    std::uint64_t composite_lower_bound_with_invariants,
    double chart_construction_ms = 0.0) {
  if (state.cache_strategy != chart_spr_cache_strategy::pattern_batches ||
      !state.pattern_batch_bootstrap_deferred) {
    throw std::runtime_error(
        "chart SPR search state: no deferred pattern-batch bootstrap to "
        "finalize");
  }
  if (!state.pattern_charts.empty() || state.lazy_chart) {
    throw std::runtime_error(
        "chart SPR search state: deferred pattern-batch bootstrap acquired an "
        "unexpected resident chart representation");
  }
  if (composite_lower_bound_with_invariants < state.invariant_constant_offset) {
    throw std::runtime_error(
        "chart SPR search state: deferred pattern-batch composite is below "
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

  multisite_trim_result trim;
  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
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

  std::vector<chart_scheduler_run_summary> runs;
  // Both current scheduled setup providers issue at most the active-pattern
  // and upper-bound-topology operations. Reserve before either can run so an
  // allocation failure cannot lose an already completed run summary.
  runs.reserve(2);
  chart_spr_scheduler_run_axis_publisher publish_runs{
      state.counters.scheduler_axes.exact_setup_patterns, runs};
  multisite_trim_result trim;
  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
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
        state.chart_opts, &runs);
    trim = build_multisite_trim_from_exact_setup(
        state.execution_plan, setup, state.chart_opts, trim_options);
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
    auto setup = state.scheduled_exact_setup_provider(state, checked_state,
                                                      scheduler, &runs);
    trim = build_multisite_trim_from_exact_setup(
        state.execution_plan, setup, state.chart_opts, trim_options);
  } else if (state.exact_setup_provider) {
    auto setup = state.exact_setup_provider(state, checked_state);
    trim = build_multisite_trim_from_exact_setup(
        state.execution_plan, setup, state.chart_opts, trim_options);
  } else {
    auto setup = build_multisite_exact_setup(
        state.execution_plan, state.active_patterns.patterns, scheduler,
        state.chart_opts, &runs);
    trim = build_multisite_trim_from_exact_setup(
        state.execution_plan, setup, state.chart_opts, trim_options);
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
    chart_spr_scheduler_axis_counters* failed_scheduler_axes = nullptr) {
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
  state.cache_opts = cache;
  state.estimated_full_pattern_cache_bytes =
      estimate_chart_spr_full_pattern_cache_bytes(state);
  auto cache_selection_options = cache;
  if (build_policy.defer_pattern_batch_bootstrap_to_local_cache &&
      cache.memory_budget_bytes != 0) {
    auto const full_bytes = state.estimated_full_pattern_cache_bytes;
    auto const scoring_pattern_bytes =
        estimate_chart_spr_pattern_entry_cache_bytes(state.grammar);
    if (full_bytes > (std::numeric_limits<std::size_t>::max)() / 2) {
      throw std::overflow_error(
          "chart SPR local-commit mandatory cache byte overflow");
    }
    auto const mandatory_pair_bytes = full_bytes * 2;
    if (mandatory_pair_bytes > cache.memory_budget_bytes ||
        scoring_pattern_bytes >
            cache.memory_budget_bytes - mandatory_pair_bytes) {
      throw std::runtime_error(
          "chart SPR local commit: configured cache budget cannot hold the "
          "mandatory full inside/outside caches plus one scoring pattern");
    }
    // The strategy selector owns only the remainder. It may retain all active
    // scoring charts when three full surfaces fit, or shrink to a bounded
    // pattern batch while reserving the two local-commit cache surfaces.
    cache_selection_options.memory_budget_bytes =
        cache.memory_budget_bytes - mandatory_pair_bytes;
  }
  state.effective_pattern_batch_size = choose_chart_spr_pattern_batch_size(
      state.grammar, state.active_patterns, cache_selection_options);
  state.cache_strategy = choose_chart_spr_cache_strategy(
      state.grammar, state.active_patterns, cache_selection_options);
  auto const defer_pattern_batch_bootstrap =
      state.cache_strategy == chart_spr_cache_strategy::pattern_batches &&
      build_policy.defer_pattern_batch_bootstrap_to_local_cache;
  if (defer_pattern_batch_bootstrap && build_exact_trim) {
    throw std::runtime_error(
        "chart SPR search state: deferred pattern-batch bootstrap requires "
        "deferred exact initialization");
  }
  state.pattern_batch_bootstrap_deferred = defer_pattern_batch_bootstrap;
  auto const exact_owns_pattern_batch_bootstrap =
      state.cache_strategy == chart_spr_cache_strategy::pattern_batches &&
      build_exact_trim;
  state.effective_candidate_batch_size = cache.candidate_batch_size;
  ++state.counters.base_chart_cache_rebuilds;
  state.counters.skipped_invariant_sites =
      state.skipped_invariant_site_count;

  auto chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  std::uint64_t active_total = 0;
  auto const chart_construction_start = std::chrono::steady_clock::now();
  if (state.cache_strategy ==
      chart_spr_cache_strategy::all_active_patterns) {
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
                        state.execution_plan, patterns[pattern_index], options,
                        chart_build_options);
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
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::lazy_multisite_chart) {
    lazy_chart_options lazy_options;
    lazy_options.chart = chart_build_options;
    lazy_options.retain_all_inside_class_maps = true;
    state.lazy_chart = build_lazy_inside_chart(
        state.execution_plan, state.active_patterns.patterns, lazy_options);
    ++state.counters.chart_execution_plan_cache_hits;
    if (!options.score_ua_edge) {
      build_lazy_outside_chart_in_place(
          state.execution_plan, state.active_patterns.patterns,
          *state.lazy_chart,
          options);
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
        estimate_chart_spr_pattern_batch_cache_bytes(
            state.grammar, state.effective_pattern_batch_size);
  }

  if (build_exact_trim) {
    auto const exact_initialization_start =
        std::chrono::steady_clock::now();
    if (scheduler != nullptr) {
      auto checked =
          check_chart_execution_plan(state.grammar, state.execution_plan);
      state.exact_trim_active_only = build_chart_spr_state_exact_trim(
          state, checked, *scheduler, trim_options);
    } else {
      state.exact_trim_active_only =
          build_chart_spr_state_exact_trim(state, trim_options);
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

  // Mark reachability validation as full-grammar-like when visited base
  // clades or productions exceed this fraction of the base grammar.  0
  // disables the flag.
  double full_grammar_like_reachability_fraction = 0.50;
};

namespace chart_spr_search_detail {

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

    delta.base = &base;
    delta.candidate_old_parent = candidate.old_parent;
    delta.candidate_new_sibling_or_target = candidate.new_sibling_or_target;
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
  dst.multifurcation_productions_scored +=
      src.multifurcation_productions_scored;
  dst.local_score_parallel_batches += src.local_score_parallel_batches;
  dst.local_score_worker_tasks += src.local_score_worker_tasks;
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
  dst.exact_trim_lazy_chart_uses += src.exact_trim_lazy_chart_uses;
  dst.outside_cache_inside_charts_built +=
      src.outside_cache_inside_charts_built;
  dst.outside_cache_inside_charts_reused +=
      src.outside_cache_inside_charts_reused;
  dst.outside_cache_outside_charts_built +=
      src.outside_cache_outside_charts_built;
  dst.exact_verifications += src.exact_verifications;
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

struct chart_spr_local_score_scratch {
  local_overlay_chart_rows rows;

  void clear_borrows() noexcept {
    rows.base_row_slot = {};
    rows.temp_row_slot = {};
  }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return rows.base_row_slot.empty() && rows.temp_row_slot.empty();
  }
};

namespace chart_spr_search_detail {

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

  [[nodiscard]] bool published() const noexcept { return published_; }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return descriptor_.base == nullptr && !published_ &&
           build_scratch_.operation_boundary_clean();
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

  void release_operation_borrows() noexcept {
    verification_materialized.reset();
    valid_for_accumulation = false;
    execution.release_operation_borrows();
  }

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return !valid_for_accumulation && execution.operation_boundary_clean();
  }

 private:
  compiled_spr_candidate_execution execution;

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

  [[nodiscard]] bool operation_boundary_clean() const noexcept {
    return scratch.operation_boundary_clean();
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

struct local_score_workspace_access {
  static void begin(chart_spr_local_score_workspace& workspace,
                    std::size_t prepared_count, std::size_t worker_count,
                    std::size_t tile_count = 0) {
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
};

inline chart_spr_local_score_result make_invalid_local_score_result(
    chart_spr_search_state const& state, std::string reason) {
  auto old_score = state.composite_lower_bound_with_invariants;
  chart_spr_local_score_result result;
  result.lower_bound.value = spr_score_result{0, old_score, old_score, false};
  result.lower_bound.kind = chart_spr_score_kind::composite_lower_bound;
  result.lower_bound.convention =
      chart_spr_score_convention::full_with_invariants;
  result.lower_bound.invariant_offset_applied = state.invariant_constant_offset;
  result.valid = false;
  result.invalid_reason = std::move(reason);
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
  try {
    checked_state.assert_same(state.grammar, state.execution_plan);
    prepared.build_and_publish_execution(state.grammar, checked_state,
                                         candidate, options,
                                         &candidate_plan_stats);
  } catch (chart_execution_plan_mismatch const& e) {
    record_candidate_plan_stats();
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    prepared.result = make_invalid_local_score_result(state, e.what());
    return;
  } catch (std::exception const& e) {
    record_candidate_plan_stats();
    prepared.result = make_invalid_local_score_result(state, e.what());
    return;
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
  } catch (chart_execution_plan_mismatch const& e) {
    if (counters != nullptr) {
      ++counters->local_candidate_scores;
      ++counters->plan_mismatch_rejections;
    }
    prepared_local_candidate_score prepared;
    prepared.reset_for_prepare();
    prepared.result = make_invalid_local_score_result(state, e.what());
    return prepared;
  }
}

inline void invalidate_prepared_local_candidate(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared, std::string reason) {
  prepared.result = make_invalid_local_score_result(state, std::move(reason));
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

  try {
    checked_state.assert_same(state.grammar, state.execution_plan);
    auto const& delta = prepared.delta();
    assert_overlay_delta_execution_plan_compatible(delta, state.execution_plan);
    auto const& patterns = state.active_patterns.patterns.patterns;
    if (pattern_begin > pattern_end || pattern_end > patterns.size() ||
        state.pattern_charts.size() != patterns.size()) {
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
      auto const& cache_entry = state.pattern_charts[pattern_index];
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
    tile.invalid_reason = e.what();
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
    tile.invalid_reason = e.what();
  }
  scratch.clear_borrows();
}

inline void finish_prepared_local_candidate_score_into(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    chart_spr_local_score_result& output) {
  if (!prepared.valid_for_accumulation) {
    output = prepared.result;
    return;
  }

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
  output = result;
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
    std::size_t pattern, std::vector<std::size_t>& key) {
  if (taxon == chart_plan_no_taxon) return;
  if (pattern >= patterns.size() ||
      taxon >= patterns[pattern].state_by_taxon.size()) {
    throw std::runtime_error(
        "chart SPR lazy local score: leaf state key out of range");
  }
  key.push_back(patterns[pattern].state_by_taxon[taxon]);
}

inline void append_lazy_overlay_base_class(
    lazy_multisite_chart const& lazy, overlay_clade_ref ref,
    std::size_t pattern, std::vector<std::size_t>& key) {
  if (ref.space != overlay_id_space::base) return;
  key.push_back(lazy_overlay_inside_class_index(lazy, ref.id, pattern));
}

inline std::vector<std::size_t> lazy_overlay_context_key(
    chart_spr_search_state const& state, spr_overlay_delta const& delta,
    std::size_t pattern) {
  if (!state.lazy_chart) {
    throw std::runtime_error("chart SPR lazy local score: missing lazy chart");
  }
  auto const& lazy = *state.lazy_chart;
  auto const& patterns = state.active_patterns.patterns.patterns;
  std::vector<std::size_t> key;
  key.reserve(delta.affected_order.size() * 4 + 1);
  key.push_back(lazy_overlay_inside_class_index(
      lazy, state.grammar.root_clade, pattern));

  auto append_children =
      [&](std::span<candidate_chart_child_descriptor const> children) {
    for (auto const& child : children) {
      append_lazy_overlay_base_class(lazy, child.clade, pattern, key);
      append_lazy_overlay_leaf_state(patterns, child.leaf_taxon, pattern, key);
    }
  };

  for (auto const& row : delta.compiled_rows) {
    auto ref = row.clade;
    append_lazy_overlay_base_class(lazy, ref, pattern, key);
    append_lazy_overlay_leaf_state(patterns, row.leaf_taxon, pattern, key);
    for (auto const& production :
         candidate_chart_productions_for_row(delta, row)) {
      append_children(candidate_chart_children(delta, production));
    }
  }
  return key;
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

struct lazy_overlay_context_accumulator {
  std::size_t representative = no_site_pattern;
  std::uint64_t weight = 0;
  std::array<std::uint64_t, nuc_state_count> reference_state_counts{};
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
      best = std::min(
          best, parsimony_chart_detail::saturated_add(
                    root_row[root_state],
                    parsimony_chart_detail::transition_cost(reference_state,
                                                            root_state)));
    }
    total = chart_multisite_detail::checked_add_u64(
        total,
        chart_multisite_detail::checked_mul_cost(
            count, best, "chart SPR lazy local score root-edge cost"),
        "chart SPR lazy local score root-edge total");
  }
  return total;
}

inline void accumulate_prepared_local_candidate_lazy(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    local_spr_score_options const& options, chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch,
    checked_chart_execution_plan_ref const& checked_state) {
  if (!prepared.valid_for_accumulation) return;
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
  if (!state.lazy_chart) {
    invalidate_prepared_local_candidate(
        state, prepared, "chart SPR lazy local score: missing lazy chart");
    return;
  }

  auto const& patterns = state.active_patterns.patterns.patterns;
  std::map<std::vector<std::size_t>, lazy_overlay_context_accumulator>
      contexts;
  try {
    for (std::size_t pattern_index = 0; pattern_index < patterns.size();
         ++pattern_index) {
      if (counters != nullptr) {
        ++counters->candidate_execution_plan_cache_hits;
      }
      auto key = lazy_overlay_context_key(state, delta, pattern_index);
      auto [it, inserted] = contexts.emplace(
          std::move(key), lazy_overlay_context_accumulator{});
      auto& context = it->second;
      if (inserted) context.representative = pattern_index;
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

    for (auto const& [key, context] : contexts) {
      (void)key;
      if (context.representative >= patterns.size()) {
        throw std::runtime_error(
            "chart SPR lazy local score: context representative out of range");
      }
      auto& rows = scratch.rows;
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
      std::fill(rows.rows.begin(), rows.rows.end(),
                parsimony_chart_detail::make_inf_row());

      lazy_overlay_row_provider provider{delta, *state.lazy_chart,
                                         context.representative, rows};
      auto states = view_leaf_site_states(
          patterns[context.representative].state_by_taxon);
      for (std::size_t i = 0; i < delta.compiled_rows.size(); ++i) {
        rows.rows[i] = recompute_overlay_delta_row(
            delta, states, provider, delta.compiled_rows[i],
            counters);
      }
      if (counters != nullptr) {
        counters->local_rows_recomputed += delta.affected_order.size();
      }
      if (prepared.verification_materialized) {
        auto chart_build_options = state.chart_opts;
        chart_build_options.keep_trace = false;
        chart_build_options.max_trace_choices = 0;
        auto base_chart = build_single_site_chart(state.grammar, states,
                                                  chart_build_options);
        verify_local_overlay_rows_against_full(
            delta, rows, base_chart, *prepared.verification_materialized,
            states, state.chart_opts);
      }
      auto const& root_row = provider.row(delta.root);
      auto contribution = lazy_overlay_weighted_root_score(
          root_row, context, state.chart_opts);
      prepared.new_active_score = chart_multisite_detail::checked_add_u64(
          prepared.new_active_score, contribution,
          "chart SPR lazy local candidate active lower bound");
    }
  } catch (std::exception const& e) {
    invalidate_prepared_local_candidate(state, prepared, e.what());
  }
}

inline void accumulate_prepared_local_candidate_lazy(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared,
    local_spr_score_options const& options,
    chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch) {
  if (!prepared.valid_for_accumulation) return;
  try {
    auto checked =
        check_chart_execution_plan(state.grammar, state.execution_plan);
    accumulate_prepared_local_candidate_lazy(
        state, prepared, options, counters, scratch, checked);
  } catch (std::exception const& e) {
    if (counters != nullptr) ++counters->plan_mismatch_rejections;
    invalidate_prepared_local_candidate(state, prepared, e.what());
  }
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
      state.cache_strategy != chart_spr_cache_strategy::all_active_patterns) {
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
  auto const resident_cache =
      state.cache_strategy == chart_spr_cache_strategy::all_active_patterns ||
      state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart;
  auto const tile_plan =
      plan_local_candidate_pattern_tiles(state, candidates, scheduler);
  auto const pattern_batch_fusion =
      plan_local_pattern_batch_fusion(state, candidates.size(), scheduler);
  auto const prepared_count =
      resident_cache
          ? (tile_plan.enabled() ? candidates.size() : effective_worker_count)
          : candidates.size();
  local_score_workspace_access::begin(
      workspace, prepared_count, effective_worker_count,
      std::max(tile_plan.total_tiles, pattern_batch_fusion.total_tiles));
  try {
    for (auto& result : results) {
      result.lower_bound = {};
      result.affected_clade_count = 0;
      result.local_score_ms = 0.0;
      result.valid = true;
      result.invalid_reason.clear();
    }
    if (resident_cache) {
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
    state.exact_trim_active_only =
        build_chart_spr_state_exact_trim(state, checked_state, trim_options);
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
    state.exact_trim_active_only = build_chart_spr_state_exact_trim(
        state, checked_state, scheduler, trim_options);
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
inline chart_spr_candidate_score verify_candidate_exact_against_state(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    multisite_trim_options const& trim_options = {}) {
  if (!candidate.valid) return candidate;
  ++state.counters.exact_verifications;

  planned_overlay_materialization_result planned;
  multisite_trim_result new_trim;
  bool dense_materialization_completed = false;
  bool materialization_counted = false;
  bool payload_validation_counted = false;
  overlay_payload_validation_stats completed_payload_validation_stats;
  try {
    auto const& old_trim =
        ensure_chart_spr_state_exact_trim(state, checked_state, trim_options);
    {
      chart_spr_elapsed_accumulator materialization_timer{
          state.counters.materialization_exact_verification_ms};
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
    ++state.counters.full_overlay_materializations;
    ++state.counters.overlay_materializations_for_exact_verification;
    materialization_counted = true;
    record_planned_overlay_materialization_stats(state.counters, planned);
    payload_validation_counted = true;

    new_trim =
        state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart
            ? build_lazy_multisite_trim_active_from_scratch(
                  planned, state.active_patterns, state.chart_opts,
                  trim_options)
            : build_multisite_trim_active(planned.execution_plan,
                                          state.active_patterns,
                                          state.chart_opts, trim_options);
    if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
      ++state.counters.exact_trim_lazy_chart_uses;
    }
    record_multisite_exact_trim_work(state.counters, new_trim);
    ++state.counters.chart_execution_plan_cache_hits;

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
  } catch (std::exception const& e) {
    // Before this refactor the dense materializer returned (and its success
    // counters advanced) before the separately built plan could reject the
    // output.  Preserve that reason-coded accounting for the combined call.
    if (dense_materialization_completed && !materialization_counted) {
      ++state.counters.full_overlay_materializations;
      ++state.counters.overlay_materializations_for_exact_verification;
    }
    if (!payload_validation_counted) {
      record_overlay_payload_validation_stats(
          state.counters, completed_payload_validation_stats);
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
      ++state.counters.chart_execution_plan_cache_hits;
    }
  }
  return candidate;
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

inline chart_spr_lazy_selected_topology_entry const&
chart_spr_lazy_selected_topology_rows_for_clade(
    chart_spr_search_state const& state,
    grammar_spr_candidate const& candidate,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref clade,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        base_memo,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        temp_memo,
    std::vector<std::uint8_t>& base_state,
    std::vector<std::uint8_t>& temp_state) {
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
          state, candidate, selected, child, base_memo, temp_memo,
          base_state, temp_state));
    }

    std::map<std::vector<std::size_t>, std::vector<std::size_t>>
        patterns_by_context;
    for (std::size_t p = 0; p < active.size(); ++p) {
      std::vector<std::size_t> key;
      key.reserve(child_entries.size());
      for (auto const* child_entry : child_entries) {
        key.push_back(child_entry->class_index_by_pattern[p]);
      }
      patterns_by_context[std::move(key)].push_back(p);
    }

    std::map<std::array<chart_cost, nuc_state_count>, std::size_t>
        class_by_row;
    for (auto const& [key, members] : patterns_by_context) {
      std::vector<chart_multisite_detail::chart_row> child_rows;
      child_rows.reserve(child_entries.size());
      for (std::size_t child_i = 0; child_i < child_entries.size(); ++child_i) {
        auto const* child_entry = child_entries[child_i];
        auto class_index = key[child_i];
        if (class_index >= child_entry->rows.size()) {
          throw std::runtime_error(
              "fixed_topology_exact lazy selected-topology scorer: child "
              "class index out of range");
        }
        child_rows.push_back(child_entry->rows[class_index]);
      }
      auto row = chart_multisite_detail::combine_rows(
          std::span<chart_multisite_detail::chart_row const>{
              child_rows.data(), child_rows.size()});
      auto [row_it, inserted] = class_by_row.emplace(row, class_by_row.size());
      if (inserted) entry.rows.push_back(row);
      for (auto p : members) entry.class_index_by_pattern[p] = row_it->second;
    }
    if (children.size() != 2) {
      state.counters.selected_topology_multifurcation_rows +=
          patterns_by_context.size();
    }
  }

  state.counters.fixed_topology_selected_rows_computed += entry.rows.size();
  state.counters.selected_topology_class_rows_computed += entry.rows.size();
  memo_slot = std::move(entry);
  state_slot = 2;
  return *memo_slot;
}

inline chart_spr_lazy_selected_topology_entry const&
chart_spr_lazy_selected_topology_root_rows(
    chart_spr_search_state const& state,
    grammar_spr_candidate const& candidate,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        base_memo,
    std::vector<std::optional<chart_spr_lazy_selected_topology_entry>>&
        temp_memo) {
  std::vector<std::uint8_t> base_state(state.grammar.clades.size(), 0);
  std::vector<std::uint8_t> temp_state(candidate.added_clades.size(), 0);
  return chart_spr_lazy_selected_topology_rows_for_clade(
      state, candidate, selected, base_clade_ref(state.grammar.root_clade),
      base_memo, temp_memo, base_state, temp_state);
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
    chart_spr_candidate_score const& candidate) {
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
  auto const& before_root = chart_spr_lazy_selected_topology_root_rows(
      state, candidate.candidate, before_selected, before_base_memo,
      before_temp_memo);
  auto const& after_root = chart_spr_lazy_selected_topology_root_rows(
      state, candidate.candidate, after_selected, after_base_memo,
      after_temp_memo);

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
    chart_spr_candidate_score const& candidate) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    return fixed_topology_lazy_selected_pattern_scores(state, candidate);
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
        state.grammar, candidate.candidate, pattern, selected_after,
        &state.counters);
    auto old_score = chart_spr_weighted_root_score_from_row(
        old_row, pattern, state.chart_opts);
    auto new_score = chart_spr_weighted_root_score_from_row(
        new_row, pattern, state.chart_opts);
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
    chart_spr_candidate_score const& candidate, chart_scheduler& scheduler) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    return fixed_topology_lazy_selected_pattern_scores(state, candidate);
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
      state.counters.scheduler_axes.fixed_topology_patterns, run);
  for (auto const& counters : counters_by_slot) {
    add_chart_spr_search_counters(state.counters, counters);
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
// Phase-8 local-commit path installs `state.fixed_topology_exact_verifier`,
// which serves production verification from the persistent inside/outside
// caches and uses this direct scorer only as a per-pattern oracle/fallback.
inline spr_score_result fixed_topology_delta_direct_selected_topology(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  auto scores = fixed_topology_direct_selected_pattern_scores(state,
                                                             candidate);
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
    chart_spr_candidate_score const& candidate, chart_scheduler& scheduler) {
  auto scores = fixed_topology_direct_selected_pattern_scores(state, candidate,
                                                              scheduler);
  auto old_full = chart_spr_add_invariant_offset(
      scores.old_active_total, state,
      "chart-SPR fixed-topology old-score invariant offset");
  auto new_full = chart_spr_add_invariant_offset(
      scores.new_active_total, state,
      "chart-SPR fixed-topology new-score invariant offset");
  return spr_score_result{chart_spr_detail::signed_delta(old_full, new_full),
                          old_full, new_full, true};
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

inline chart_spr_candidate_score verify_candidate_fixed_topology_exact(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate) {
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

  ++state.counters.exact_verifications;
  try {
    // Phase 8: score the complete fixed before/after topology certificate
    // directly in overlay space.  This is exact for the selected topology and
    // performs no dense overlay materialization; the exact-verification
    // materialization counters therefore remain unchanged for the
    // fixed_topology_exact gate.
    auto delta = fixed_topology_delta_direct_selected_topology(state, candidate);
    candidate.exact = make_chart_spr_objective_score(
        delta, chart_spr_score_kind::fixed_topology_exact,
        chart_spr_score_convention::full_with_invariants,
        state.invariant_constant_offset);
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
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_scheduler& scheduler) {
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
  ++state.counters.exact_verifications;
  try {
    auto delta = fixed_topology_delta_direct_selected_topology(state, candidate,
                                                               scheduler);
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

inline chart_spr_candidate_score verify_candidate_for_acceptance(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_search_options const& options,
    chart_scheduler* scheduler = nullptr) {
  checked_state.assert_same(state.grammar, state.execution_plan);
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return candidate;
    case chart_spr_acceptance_mode::exact_multisite:
      // Phase 9: when a local-commit substrate is active it installs
      // `state.exact_multisite_verifier`, which verifies the candidate by
      // transiently extending the chain in reader-local scratch (avoiding a
      // fresh materialize_overlay_grammar, never mutating the
      // shared cache).  When absent (conservative rebuild mode, or a bare
      // state built without a substrate), the cold from-scratch path below is
      // used.  The cold path is also the correctness oracle the transient
      // verifier cross-checks when its test-only oracle flag is set.
      if (state.exact_multisite_verifier) {
        return state.exact_multisite_verifier(state, std::move(candidate),
                                              checked_state,
                                              options.exact_trim);
      }
      return verify_candidate_exact_against_state(
          state, std::move(candidate), checked_state, options.exact_trim);
    case chart_spr_acceptance_mode::fixed_topology_exact:
      {
      chart_spr_candidate_score verified;
      if (state.fixed_topology_exact_verifier) {
        verified = state.fixed_topology_exact_verifier(state,
                                                       std::move(candidate));
      } else {
        verified = scheduler != nullptr
                       ? verify_candidate_fixed_topology_exact(
                             state, std::move(candidate), *scheduler)
                       : verify_candidate_fixed_topology_exact(
                             state, std::move(candidate));
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
  std::vector<chart_spr_local_score_result> local_results;
  chart_spr_local_score_workspace local_score;
  std::size_t active_candidates = 0;

  void begin_iteration() {
    if (!local_score.operation_boundary_clean()) {
      throw std::logic_error(
          "chart SPR acceptance workspace: local scorer crossed an operation "
          "boundary with a stale borrow");
    }
    active_candidates = 0;
  }

  void reserve_batch(std::size_t candidate_count) {
    if (candidate_slots.capacity() < candidate_count) {
      candidate_slots.reserve(candidate_count);
    }
    if (candidate_copy_scratch.capacity() < candidate_count) {
      candidate_copy_scratch.reserve(candidate_count);
    }
    if (local_results.capacity() < candidate_count) {
      local_results.reserve(candidate_count);
    }
  }

  void append_candidate(grammar_spr_candidate const& candidate) {
    if (active_candidates == candidate_slots.size()) {
      candidate_slots.emplace_back();
      try {
        candidate_copy_scratch.emplace_back();
      } catch (...) {
        candidate_slots.pop_back();
        throw;
      }
    }
    copy_grammar_spr_candidate_reusing_storage(
        candidate_slots[active_candidates], candidate,
        candidate_copy_scratch[active_candidates]);
    ++active_candidates;
  }

  [[nodiscard]] std::span<grammar_spr_candidate const> candidates() const {
    return {candidate_slots.data(), active_candidates};
  }

  [[nodiscard]] std::span<chart_spr_local_score_result> results() {
    if (local_results.size() < active_candidates) {
      local_results.resize(active_candidates);
    }
    return {local_results.data(), active_candidates};
  }

  void finish_batch() noexcept { active_candidates = 0; }
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
  auto checked_state = [&] {
    try {
      return check_chart_execution_plan(state.grammar, state.execution_plan);
    } catch (chart_execution_plan_mismatch const&) {
      ++state.counters.plan_mismatch_rejections;
      throw;
    }
  }();
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    chart_spr_search_detail::validate_chart_spr_exact_multisite_multifurcation_gate(
        state.grammar, "chart SPR acceptance iteration");
  }
  if (options.candidate_selection ==
      chart_spr_candidate_selection_mode::sampled_or_randomized) {
    options.enumeration.randomize_order = true;
    options.enumeration.seed = options.seed;
  }

  chart_spr_iteration_result result;
  result.iteration = iteration;
  result.acceptance_mode = options.acceptance_mode;
  result.candidate_selection = options.candidate_selection;
  result.state_score_before = chart_spr_iteration_state_score_before(
      state, checked_state, options, scheduler);
  result.state_score_after = result.state_score_before;
  bool const capture_semantics =
      options.semantic_capture != chart_spr_semantic_capture_mode::off;
  if (capture_semantics) {
    result.canonical_seed = options.seed;
    if (options.acceptance_mode ==
        chart_spr_acceptance_mode::exact_multisite) {
      auto const& old_trim = ensure_chart_spr_state_exact_trim(
          state, checked_state, scheduler, options.exact_trim);
      result.canonical_state_exact_before =
          chart_spr_canonicalize_search_trim_evidence(
              state.grammar, checked_state, state.active_patterns,
              state.chart_opts,
              options.exact_trim, old_trim,
              state.invariant_constant_offset);
      if (old_trim.keep_production_exact) {
        ++state.counters.chart_execution_plan_cache_hits;
      }
    }
  }

  auto enumeration = chart_spr_iteration_enumeration_options(options);
  if (enumeration.source != chart_spr_candidate_source::grammar &&
      state.dag != nullptr) {
    enumeration.sampled_tree_source_dag = state.dag;
  }
  std::vector<chart_spr_candidate_score> ranked;
  auto rank_limit = chart_spr_rank_buffer_limit(options);
  if (rank_limit != 0 && rank_limit != chart_spr_rank_unlimited) {
    ranked.reserve(rank_limit);
  }
  std::vector<std::size_t> affected_counts;

  auto const worker_count = scheduler.worker_resolution().resolved_workers;
  auto candidate_batch_size = chart_spr_effective_candidate_batch_size(
      state, options, worker_count);
  workspace.reserve_batch(candidate_batch_size);
  state.effective_candidate_batch_size = candidate_batch_size;
  validate_chart_spr_pattern_batch_replay_strategy(
      state, options, enumeration, candidate_batch_size);
  auto local_options = local_spr_score_options{};
  local_options.verify_against_full_overlay =
      options.verify_local_against_full_for_tests;

  bool stop_after_batch = false;

  auto process_candidate_batch = [&]() {
    if (workspace.active_candidates == 0) return;
    auto candidate_batch = workspace.candidates();
    // Grow caller-owned result storage before the timed `_into` region.
    auto local_results = workspace.results();
    auto local_start = std::chrono::steady_clock::now();
    score_candidates_locally_into(state, candidate_batch, local_results,
                                  workspace.local_score, local_options,
                                  scheduler, checked_state);
    result.local_scoring_ms += std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() -
                                   local_start)
                                   .count();
    workspace.finish_batch();
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
        result.canonical_candidates.push_back(std::move(record));
      }
      if (!scored.valid) {
        ++result.candidate_score_failures;
        continue;
      }
      affected_counts.push_back(scored.affected_clade_count);
      if (scored.lower_bound.value.improves()) {
        ++result.local_improving_candidates;
      }
      chart_spr_insert_ranked_candidate(state.grammar, ranked,
                                        std::move(scored), rank_limit);
    }
  };

  double generation_callback_ms = 0.0;
  auto const generation_start = std::chrono::steady_clock::now();
  auto generation = for_each_grammar_spr_candidate(
      state.grammar, checked_state, enumeration,
      [&](grammar_spr_candidate const& candidate) {
        auto const callback_start = std::chrono::steady_clock::now();
        workspace.append_candidate(candidate);
        if (workspace.active_candidates >= candidate_batch_size) {
          process_candidate_batch();
          if (options.candidate_selection ==
                  chart_spr_candidate_selection_mode::lower_bound_first_improvement &&
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
  auto const generation_total_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - generation_start)
          .count();
  result.candidate_generation_ms =
      std::max(0.0, generation_total_ms - generation_callback_ms);
  if (!stop_after_batch) process_candidate_batch();

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
  }

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
  } else {
    std::vector<chart_spr_candidate_score> verified;
    verified.reserve(ranked.size());
    for (auto& candidate : ranked) {
      attach_fixed_topology_selection_for_acceptance(state, candidate, options);
      auto exact_verifications_before = state.counters.exact_verifications;
      auto exact_start = std::chrono::steady_clock::now();
      chart_spr_candidate_score verified_candidate;
      if (candidate.valid) {
        chart_spr_exact_verifier_activity verifier_activity{
            state.exact_verifier_concurrency};
        verified_candidate = verify_candidate_for_acceptance(
            state, std::move(candidate), checked_state, options, &scheduler);
      } else {
        // Selection/certificate attachment can invalidate a candidate before
        // verifier entry.  Such a candidate has timing diagnostics but must
        // not inflate the observed verifier-concurrency high-water mark.
        verified_candidate = std::move(candidate);
      }
      auto const exact_candidate_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - exact_start)
              .count();
      result.exact_verification_ms += exact_candidate_ms;
      result.exact_candidate_verification_ms.push_back(exact_candidate_ms);
      if (state.counters.exact_verifications > exact_verifications_before ||
          verified_candidate.exact) {
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

    for (auto const& candidate : verified) {
      if (!chart_spr_candidate_has_accepting_improvement(
              candidate, options.acceptance_mode)) {
        continue;
      }
      if (!result.accepted || chart_spr_acceptance_candidate_better(
                                  options.acceptance_mode, state.grammar,
                                  candidate, *result.accepted)) {
        result.accepted = candidate;
      }
    }
    if (result.accepted) {
      auto const& accepted_score = chart_spr_candidate_acceptance_score(
          *result.accepted, options.acceptance_mode);
      result.state_score_before = accepted_score.old_score;
      result.state_score_after = accepted_score.new_score;
      ++state.counters.candidate_accepts_attempted;
    }
  }

  if (!result.accepted) {
    if (result.candidates_scored == 0) {
      result.no_accept_reason = "no candidates scored";
    } else if (options.acceptance_mode ==
               chart_spr_acceptance_mode::lower_bound_heuristic) {
      result.no_accept_reason = "no lower-bound-improving candidate";
    } else if (ranked.empty()) {
      result.no_accept_reason = "no valid locally scored candidates retained";
    } else {
      result.no_accept_reason = "no exact-improving verified candidate";
    }
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

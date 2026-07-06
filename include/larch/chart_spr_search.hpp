#pragma once

#include <larch/chart_spr.hpp>
#include <larch/thread_pool.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

// Phase-0 instrumentation for the DAG-native chart-SPR search path.  These
// counters deliberately separate accepted-state rebuilds from diagnostic/oracle
// overlay materializations and exact-verification materializations so benchmark
// output can detect accidental "full rebuild per rejected candidate" behavior.
struct chart_spr_search_counters {
  std::size_t grammar_rebuilds = 0;
  std::size_t pattern_rebuilds = 0;
  std::size_t base_chart_cache_rebuilds = 0;
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
  std::size_t sidecar_rebuilds_after_accept = 0;
  std::size_t full_composite_rebuilds = 0;
  std::size_t local_candidate_scores = 0;
  std::size_t local_rows_recomputed = 0;
  std::size_t local_score_parallel_batches = 0;
  std::size_t local_score_worker_tasks = 0;
  std::size_t candidate_batches_scored = 0;
  std::size_t pattern_batch_cache_builds = 0;
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
  // unaccepted candidate by transiently extending the overlay chain +
  // persistent inside/outside caches in reader-local scratch storage (never
  // mutating the shared cache, bypassing the Phase 4 commit barrier), reading
  // the exact frontier on the extended grammar, and discarding.  By the
  // cross-cutting definition this is NOT a dense materialization: the
  // transient extension reuses the persistent cache machinery and updates only
  // affected rows in scratch.  It is counted here, never folded into
  // `full_overlay_materializations` (a regression to "dense materialize per
  // candidate" must be visible, not hidden behind a renamed counter).
  //
  // CAVEAT: as shipped, the transient scratch caches are read ONLY by the
  // test/diagnostic two-chart oracle; the production B&B scorer rebuilds the
  // charts from the materialized extended grammar and does not consume them.
  // So this counter measures the transient-extension SUBSTRATE being
  // exercised, not a wall-clock win over the cold path -- Phase 9 lands
  // substrate + oracle + counter discipline; the perf win awaits Phase 12
  // (warm-started B&B seeded from these scratch caches).  See the header
  // comment on `chart_spr_transient_extension` (chart_spr_search.cpp) and the
  // Phase-9 caveat in doc/WRIC-SPR-SEARCH.md.
  std::size_t transient_chain_extensions_for_verification = 0;
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
// the chain + caches in reader-local scratch (counted under
// `transient_chain_extensions_for_verification`, never under
// `full_overlay_materializations`).  `cold` skips installing the transient
// verifier so the from-scratch `verify_candidate_exact_against_state` path
// runs (a dense materialization per verified candidate, counted under
// `overlay_materializations_for_exact_verification`).  This is the named
// verification-mode choice the Phase-10 report surfaces; it is meaningful even
// though the Phase-9 scratch caches are not yet consumed by the production B&B
// scorer (see the Phase-9 caveat in doc/WRIC-SPR-SEARCH.md).
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

  // Parallel local lower-bound scoring.  1 is deterministic serial scoring;
  // 0 chooses std::thread::hardware_concurrency() for the local scoring batch.
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

// Single chart-SPR root-row scoring entry point.  It delegates to the checked
// multi-site chart helper so every search cache/local scorer handles
// score_ua_edge=true compressed patterns the same way: reference-state counts
// are applied to the full root row, not to a pre-collapsed scalar root minimum.
inline std::uint64_t chart_spr_weighted_root_score_from_row(
    std::array<chart_cost, nuc_state_count> const& root_row,
    site_pattern const& pattern, chart_options const& options) {
  return chart_multisite_detail::weighted_root_score_from_row(root_row, pattern,
                                                              options);
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

struct chart_spr_candidate_score {
  grammar_spr_candidate candidate;
  chart_spr_objective_score lower_bound;
  std::optional<chart_spr_objective_score> exact;
  chart_spr_topology_selection topology_selection;
  std::size_t affected_clade_count = 0;
  double local_score_ms = 0.0;
  bool valid = true;
  std::string invalid_reason;
};

using chart_spr_fixed_topology_verifier = std::function<
    chart_spr_candidate_score(chart_spr_search_state const&,
                              chart_spr_candidate_score)>;

// Phase 9 (Work item 4a, technique 2) hook: when installed on the search
// state (by the local-commit substrate builder), the exact_multisite
// acceptance gate verifies each candidate by transiently extending the
// overlay chain + persistent inside/outside caches in reader-local scratch
// storage, reading the exact frontier on the extended grammar, and discarding.
// The hook is reader-local (never mutates the shared cache) and therefore
// bypasses the Phase 4 commit barrier, so transient extensions can run in
// parallel with other scoring readers under the epoch/snapshot model.  When
// absent, the exact_multisite gate falls back to the cold from-scratch path
// (`verify_candidate_exact_against_state`).
//
// CAVEAT: as shipped this lands the transient-extension SUBSTRATE and the
// per-candidate two-chart ORACLE, not a wall-clock improvement -- the scratch
// caches are not consumed by the production B&B scorer (it rebuilds charts
// from the grammar), so they are dead work on the production path and read
// only by the test/diagnostic oracle.  The caches are forward-looking
// groundwork for Phase 12 (warm-started B&B seeded from them).  See the header
// comment on `chart_spr_transient_extension` in chart_spr_search.cpp and the
// Phase-9 caveat in doc/WRIC-SPR-SEARCH.md.
using chart_spr_exact_multisite_verifier = std::function<
    chart_spr_candidate_score(chart_spr_search_state const&,
                              chart_spr_candidate_score,
                              multisite_trim_options const&)>;

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
  double local_scoring_ms = 0.0;
  double exact_verification_ms = 0.0;
};

struct chart_spr_search_summary {
  std::size_t iterations = 0;
  std::size_t accepted_moves = 0;
  std::size_t candidates_generated = 0;
  std::size_t candidates_locally_scored = 0;
  std::size_t local_rows_recomputed = 0;
  std::size_t candidate_batches_scored = 0;
  std::size_t pattern_batch_cache_builds = 0;
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
  std::size_t local_commit_two_chart_oracle_runs = 0;
  std::size_t local_commit_tip_grammar_refreshes = 0;
  std::size_t fixed_topology_selected_cache_hits = 0;
  std::size_t fixed_topology_selected_cache_misses = 0;
  std::size_t fixed_topology_selected_rows_computed = 0;
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
  double local_scoring_ms = 0.0;
  double local_candidates_per_second = 0.0;
  double local_rows_recomputed_per_second = 0.0;
  double exact_verification_ms = 0.0;
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
  std::size_t effective_pattern_batch_size = 0;
  std::size_t effective_candidate_batch_size = 0;
  std::size_t local_score_worker_count = 1;
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
};

enum class chart_spr_cache_strategy {
  all_active_patterns,
  pattern_batches,
};

inline char const* chart_spr_cache_strategy_name(
    chart_spr_cache_strategy strategy) {
  switch (strategy) {
    case chart_spr_cache_strategy::all_active_patterns:
      return "all_active_patterns";
    case chart_spr_cache_strategy::pattern_batches:
      return "pattern_batches";
  }
  return "unknown";
}

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

inline void validate_binary_chart_compatible_grammar(
    clade_grammar const& grammar) {
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
          " has no productions; DAG-native chart-SPR requires a binary "
          "chart-compatible grammar");
    }
  }

  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& prod = grammar.productions[pid];
    if (prod.children.size() != 2) {
      throw std::runtime_error(
          "chart SPR search: production " + std::to_string(pid) +
          " has arity " + std::to_string(prod.children.size()) +
          "; DAG-native chart-SPR requires a binary chart-compatible grammar "
          "(run/refine polytomies first or choose reject mode)");
    }
    parsimony_chart_detail::validate_binary_production_partition(
        grammar, prod, static_cast<production_id>(pid));
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
  leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
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

inline multisite_trim_result build_multisite_trim_active(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_multisite_trim(grammar, patterns.patterns, options,
                              trim_options);
}

inline std::size_t estimate_chart_spr_pattern_row_cache_bytes(
    clade_grammar const& grammar) {
  return grammar.clades.size() * nuc_state_count * sizeof(chart_cost);
}

inline std::size_t estimate_chart_spr_full_pattern_cache_bytes(
    clade_grammar const& grammar, active_site_pattern_set const& patterns) {
  patterns.assert_no_skipped_invariant_metadata();
  return patterns.patterns.patterns.size() *
         estimate_chart_spr_pattern_row_cache_bytes(grammar);
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
  auto row_bytes = estimate_chart_spr_pattern_row_cache_bytes(grammar);
  if (cache.memory_budget_bytes != 0 && row_bytes != 0) {
    auto by_budget = cache.memory_budget_bytes / row_bytes;
    if (by_budget == 0) by_budget = 1;
    limit = std::min(limit, by_budget);
  }
  return std::max<std::size_t>(1, std::min(limit, active_count));
}

inline chart_spr_cache_strategy choose_chart_spr_cache_strategy(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_cache_options const& cache) {
  auto active_count = patterns.patterns.patterns.size();
  if (active_count == 0) return chart_spr_cache_strategy::all_active_patterns;
  auto batch_size = choose_chart_spr_pattern_batch_size(grammar, patterns,
                                                       cache);
  return batch_size >= active_count
             ? chart_spr_cache_strategy::all_active_patterns
             : chart_spr_cache_strategy::pattern_batches;
}

struct chart_spr_search_state {
  phylo_dag* dag = nullptr;
  clade_grammar grammar;

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
  std::size_t effective_pattern_batch_size = 0;
  mutable std::size_t effective_candidate_batch_size = 0;
  std::vector<pattern_chart_cache_entry> pattern_charts;
  std::uint64_t composite_lower_bound_without_invariants = 0;
  std::uint64_t composite_lower_bound_with_invariants = 0;

  // Active-pattern-only exact result.  Add invariant_constant_offset exactly
  // once when comparing/reporting the full objective.  This is mutable so the
  // Phase-4 exact acceptance gate can lazily build and then reuse the current
  // state's old exact score even when verification APIs take a const state.
  mutable std::optional<multisite_trim_result> exact_trim_active_only;

  // Optional Phase-8 fixed-topology verifier supplied by the local-commit
  // substrate.  When present, fixed_topology_exact verification reads the
  // persistent inside/outside caches owned by the substrate.  Conservative
  // rebuild mode leaves this empty and uses the legacy direct selected-topology
  // fallback below.
  chart_spr_fixed_topology_verifier fixed_topology_exact_verifier;

  // Optional Phase-9 transient-extension verifier supplied by the local-commit
  // substrate.  When present, exact_multisite verification transiently extends
  // the chain + caches in reader-local scratch (never mutating the shared
  // cache) and reads the exact frontier on the extended grammar.  Conservative
  // rebuild mode leaves this empty and uses the cold from-scratch path
  // (`verify_candidate_exact_against_state`).
  //
  // As shipped this is substrate + oracle, not a perf win (see the typedef
  // comment on `chart_spr_exact_multisite_verifier` and the Phase-9 caveat in
  // doc/WRIC-SPR-SEARCH.md): the scratch caches are forward-looking groundwork
  // for Phase 12 and are unconsumed by the production B&B scorer.
  chart_spr_exact_multisite_verifier exact_multisite_verifier;

  mutable chart_spr_search_counters counters;
};

inline std::size_t estimate_chart_spr_pattern_cache_bytes(
    chart_spr_search_state const& state) {
  std::size_t total = state.pattern_charts.size() *
                      sizeof(pattern_chart_cache_entry);
  for (auto const& entry : state.pattern_charts) {
    total += entry.chart.inside.size() * sizeof(entry.chart.inside.front());
    total += entry.chart.optimal_choices.size() *
             sizeof(decltype(entry.chart.optimal_choices)::value_type);
  }
  return total;
}

inline std::size_t estimate_chart_spr_full_pattern_cache_bytes(
    chart_spr_search_state const& state) {
  return estimate_chart_spr_full_pattern_cache_bytes(state.grammar,
                                                    state.active_patterns);
}

inline chart_spr_search_state build_chart_spr_search_state_from_active(
    phylo_dag& dag, clade_grammar grammar,
    chart_spr_active_pattern_build_result active_build,
    chart_options options = {}, bool build_exact_trim = false,
    multisite_trim_options const& trim_options = {},
    chart_cache_options cache = {}) {
  validate_supported_chart_cache_options(cache);
  chart_spr_search_detail::validate_binary_chart_compatible_grammar(grammar);
  active_build.active_patterns.assert_no_skipped_invariant_metadata();
  chart_multisite_detail::validate_multisite_inputs(
      grammar, active_build.active_patterns.patterns, options);

  chart_spr_search_state state;
  state.dag = &dag;
  state.grammar = std::move(grammar);
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
  state.effective_pattern_batch_size = choose_chart_spr_pattern_batch_size(
      state.grammar, state.active_patterns, cache);
  state.cache_strategy = choose_chart_spr_cache_strategy(
      state.grammar, state.active_patterns, cache);
  state.effective_candidate_batch_size = cache.candidate_batch_size;
  ++state.counters.base_chart_cache_rebuilds;
  state.counters.skipped_invariant_sites =
      state.skipped_invariant_site_count;

  auto chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  std::uint64_t active_total = 0;
  if (state.cache_strategy ==
      chart_spr_cache_strategy::all_active_patterns) {
    state.pattern_charts.reserve(
        state.active_patterns.patterns.patterns.size());
    for (auto const& pattern : state.active_patterns.patterns.patterns) {
      auto entry = chart_spr_search_detail::build_pattern_chart_cache_entry(
          state.grammar, pattern, options, chart_build_options);
      active_total = chart_multisite_detail::checked_add_u64(
          active_total, entry.weighted_root_score,
          "chart-SPR cached active-pattern lower bound");
      state.pattern_charts.push_back(std::move(entry));
    }
    state.resident_pattern_cache_bytes =
        estimate_chart_spr_pattern_cache_bytes(state);
  } else {
    auto const& patterns = state.active_patterns.patterns.patterns;
    auto batch_size = std::max<std::size_t>(
        1, state.effective_pattern_batch_size);
    for (std::size_t begin = 0; begin < patterns.size(); begin += batch_size) {
      ++state.counters.pattern_batch_cache_builds;
      auto end = std::min(patterns.size(), begin + batch_size);
      for (std::size_t i = begin; i < end; ++i) {
        auto entry = chart_spr_search_detail::build_pattern_chart_cache_entry(
            state.grammar, patterns[i], options, chart_build_options);
        active_total = chart_multisite_detail::checked_add_u64(
            active_total, entry.weighted_root_score,
            "chart-SPR batched active-pattern lower bound");
      }
    }
    state.resident_pattern_cache_bytes =
        state.effective_pattern_batch_size *
        estimate_chart_spr_pattern_row_cache_bytes(state.grammar);
  }

  state.composite_lower_bound_without_invariants = active_total;
  state.composite_lower_bound_with_invariants =
      chart_multisite_detail::checked_add_u64(
          active_total, state.invariant_constant_offset,
          "chart-SPR cached lower bound invariant offset");

  if (build_exact_trim) {
    state.exact_trim_active_only = build_multisite_trim_active(
        state.grammar, state.active_patterns, options, trim_options);
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
};

struct spr_overlay_delta {
  clade_grammar const* base = nullptr;
  grammar_spr_candidate const* candidate = nullptr;

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

  // Candidate-local indices built once and reused across all active patterns.
  std::vector<bool> removed_base_production;
  std::vector<bool> reachable_base_clade;
  std::vector<bool> reachable_temp_clade;
  std::vector<std::vector<production_id>> temp_productions_by_base_parent;
  std::vector<std::vector<production_id>> temp_productions_by_temp_parent;
  std::vector<std::vector<production_id>> temp_productions_by_base_child;
  std::vector<std::vector<production_id>> temp_productions_by_temp_child;
};

struct local_overlay_chart_rows {
  static constexpr std::size_t npos = chart_spr_overlay_row_npos;

  // Rows only for affected base clades and reachable temp clades.  Slot maps
  // are candidate-delta owned, not rebuilt for every active pattern.
  std::vector<std::array<chart_cost, nuc_state_count>> rows;
  std::vector<std::size_t> const* base_row_slot = nullptr;
  std::vector<std::size_t> const* temp_row_slot = nullptr;

  [[nodiscard]] std::size_t slot_for(overlay_clade_ref ref) const {
    if (ref.space == overlay_id_space::base) {
      if (base_row_slot == nullptr) {
        throw std::runtime_error(
            "chart SPR overlay-delta row: missing base row slot map");
      }
      auto const& slots = *base_row_slot;
      if (ref.id == no_clade || ref.id >= slots.size()) {
        throw std::runtime_error(
            "chart SPR overlay-delta row: base clade ref out of range");
      }
      return slots[ref.id];
    }
    if (temp_row_slot == nullptr) {
      throw std::runtime_error(
          "chart SPR overlay-delta row: missing temp row slot map");
    }
    auto const& slots = *temp_row_slot;
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

struct local_spr_score_options {
  bool verify_against_full_overlay = false;  // tests/debug only
  bool exact_multisite = false;              // false = composite/lower bound
  bool validate_cached_chart_shapes = false; // tests/debug only

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
    spr_overlay_delta const& delta, overlay_clade_ref ref,
    leaf_site_states const& leaf_states) {
  auto const& key = overlay_delta_clade_key(delta, ref);
  if (key.taxa.size() != 1) {
    throw std::runtime_error(
        "chart SPR overlay-delta: leaf row requested for non-leaf clade");
  }
  auto taxon = key.taxa.front();
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

inline void validate_overlay_delta_production_partition(
    spr_overlay_delta const& delta, overlay_grammar_production const& prod,
    production_id pid) {
  if (prod.children.size() < 2) {
    throw std::runtime_error(
        "chart SPR overlay-delta: temp production " +
        std::to_string(pid) + " has arity " +
        std::to_string(prod.children.size()) +
        "; local scoring requires at least 2 children");
  }

  auto const& parent_taxa = overlay_delta_clade_key(delta, prod.parent).taxa;
  std::vector<taxon_id> covered;
  for (auto child : prod.children) {
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

    std::vector<taxon_id> overlap;
    std::set_intersection(covered.begin(), covered.end(),
                          child_taxa.begin(), child_taxa.end(),
                          std::back_inserter(overlap));
    if (!overlap.empty()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: temp production children overlap");
    }

    std::vector<taxon_id> next;
    std::set_union(covered.begin(), covered.end(), child_taxa.begin(),
                   child_taxa.end(), std::back_inserter(next));
    covered = std::move(next);
  }
  if (covered != parent_taxa) {
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

inline void build_overlay_delta_temp_indices(spr_overlay_delta& delta) {
  auto const& base = *delta.base;
  delta.temp_productions_by_base_parent.assign(base.clades.size(), {});
  delta.temp_productions_by_temp_parent.assign(delta.temp_clades.size(), {});
  delta.temp_productions_by_base_child.assign(base.clades.size(), {});
  delta.temp_productions_by_temp_child.assign(delta.temp_clades.size(), {});

  for (std::size_t i = 0; i < delta.temp_productions.size(); ++i) {
    auto pid = static_cast<production_id>(i);
    auto const& prod = delta.temp_productions[i];
    validate_overlay_delta_production_partition(delta, prod, pid);
    append_temp_production_index(delta.temp_productions_by_base_parent,
                                 delta.temp_productions_by_temp_parent,
                                 prod.parent, pid);

    auto children = prod.children;
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()),
                   children.end());
    for (auto child : children) {
      append_temp_production_index(delta.temp_productions_by_base_child,
                                   delta.temp_productions_by_temp_child,
                                   child, pid);
    }
  }
}

inline void compute_overlay_delta_reachability(
    spr_overlay_delta& delta,
    local_spr_score_options const& options) {
  auto const& base = *delta.base;
  delta.reachable_base_clade.assign(base.clades.size(), false);
  delta.reachable_temp_clade.assign(delta.temp_clades.size(), false);
  delta.reachability_stats = overlay_reachability_stats{};

  std::size_t reachable_base_clades = 0;
  std::size_t reachable_base_productions = 0;
  std::vector<overlay_clade_ref> stack{delta.root};
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
        validate_reachable_base_production(delta, pid);
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
      validate_overlay_delta_production_partition(delta, prod, temp_pid);
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
}

inline void compute_overlay_delta_affected_order(spr_overlay_delta& delta) {
  auto const& base = *delta.base;
  std::vector<bool> affected_base(base.clades.size(), false);
  std::vector<bool> affected_temp(delta.temp_clades.size(), false);
  std::vector<overlay_clade_ref> queue;

  for (std::size_t i = 0; i < delta.temp_clades.size(); ++i) {
    mark_overlay_delta_affected(delta, affected_base, affected_temp, queue,
                                temp_clade_ref(static_cast<clade_id>(i)));
  }
  for (auto pid : delta.removed_base_productions) {
    if (pid == no_production || pid >= base.productions.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: removed production out of range");
    }
    mark_overlay_delta_affected(delta, affected_base, affected_temp, queue,
                                base_clade_ref(base.productions[pid].parent));
  }
  for (std::size_t i = 0; i < delta.temp_productions.size(); ++i) {
    mark_overlay_delta_affected(delta, affected_base, affected_temp, queue,
                                delta.temp_productions[i].parent);
  }
  if (delta.candidate != nullptr) {
    mark_overlay_delta_affected_if_present(delta, affected_base,
                                           affected_temp, queue,
                                           delta.candidate->old_parent);
    mark_overlay_delta_affected_if_present(
        delta, affected_base, affected_temp, queue,
        delta.candidate->new_sibling_or_target);
  }

  for (std::size_t head = 0; head < queue.size(); ++head) {
    auto child = queue[head];
    if (child.space == overlay_id_space::base) {
      for (auto pid : base.productions_by_child[child.id]) {
        if (overlay_delta_base_production_removed(delta, pid)) continue;
        auto parent = base.productions[pid].parent;
        mark_overlay_delta_affected(delta, affected_base, affected_temp, queue,
                                    base_clade_ref(parent));
      }
      for (auto temp_pid : delta.temp_productions_by_base_child[child.id]) {
        mark_overlay_delta_affected(delta, affected_base, affected_temp, queue,
                                    delta.temp_productions[temp_pid].parent);
      }
    } else {
      for (auto temp_pid : delta.temp_productions_by_temp_child[child.id]) {
        mark_overlay_delta_affected(delta, affected_base, affected_temp, queue,
                                    delta.temp_productions[temp_pid].parent);
      }
    }
  }

  delta.affected_base_clade.assign(base.clades.size(), false);
  delta.affected_temp_clade.assign(delta.temp_clades.size(), false);
  delta.affected_base_row_slot.assign(base.clades.size(),
                                      chart_spr_overlay_row_npos);
  delta.affected_temp_row_slot.assign(delta.temp_clades.size(),
                                      chart_spr_overlay_row_npos);
  delta.affected_order.clear();
  for (clade_id cid = 0; cid < base.clades.size(); ++cid) {
    if (!affected_base[cid] || !delta.reachable_base_clade[cid]) continue;
    delta.affected_order.push_back(base_clade_ref(cid));
  }
  for (clade_id cid = 0; cid < delta.temp_clades.size(); ++cid) {
    if (!affected_temp[cid] || !delta.reachable_temp_clade[cid]) continue;
    delta.affected_order.push_back(temp_clade_ref(cid));
  }

  std::stable_sort(delta.affected_order.begin(), delta.affected_order.end(),
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

inline spr_overlay_delta build_spr_overlay_delta(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    local_spr_score_options const& options = {}) {
  if (base.root_clade == no_clade || base.root_clade >= base.clades.size()) {
    throw std::runtime_error("chart SPR overlay-delta: root clade out of range");
  }

  spr_overlay_delta delta;
  delta.base = &base;
  delta.candidate = &candidate;
  delta.temp_clades = candidate.added_clades;
  delta.temp_productions = candidate.added_productions;
  delta.root = base_clade_ref(base.root_clade);

  auto taxon_count = base.taxa.id_to_sample_id.size();
  for (std::size_t i = 0; i < delta.temp_clades.size(); ++i) {
    chart_spr_detail::validate_clade_key(
        delta.temp_clades[i], taxon_count,
        "overlay-delta temp clade " + std::to_string(i));
  }

  delta.removed_base_productions.reserve(candidate.removed_productions.size());
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

  chart_spr_search_detail::build_overlay_delta_temp_indices(delta);
  chart_spr_search_detail::compute_overlay_delta_reachability(delta, options);
  chart_spr_search_detail::compute_overlay_delta_affected_order(delta);
  return delta;
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
};

namespace chart_spr_search_detail {

inline void accumulate_overlay_production_row(
    std::array<chart_cost, nuc_state_count>& row,
    std::vector<overlay_clade_ref> const& children,
    overlay_row_provider const& provider) {
  if (children.size() < 2) {
    throw std::runtime_error(
        "chart SPR overlay-delta: local row recompute requires at least 2 "
        "children");
  }

  struct production_view {
    std::vector<overlay_clade_ref> const& children;
  } prod{children};
  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    auto row_provider = [&](overlay_clade_ref child) -> auto const& {
      return provider.row(child);
    };
    auto total = parsimony_chart_detail::combine_production_inside_row(
        prod, parent_state, row_provider);
    row[parent_state] = std::min(row[parent_state], total);
  }
}

inline std::array<chart_cost, nuc_state_count> recompute_overlay_delta_row(
    spr_overlay_delta const& delta, leaf_site_states const& leaf_states,
    overlay_row_provider const& provider, overlay_clade_ref ref) {
  auto const& base = *delta.base;
  auto const& key = overlay_delta_clade_key(delta, ref);
  if (key.taxa.size() == 1) {
    if (available_production_count_for_parent(delta, ref) != 0) {
      throw std::runtime_error(
          "chart SPR overlay-delta: singleton clade has productions during "
          "local row recompute");
    }
    return overlay_delta_leaf_row(delta, ref, leaf_states);
  }

  auto row = parsimony_chart_detail::make_inf_row();
  bool saw_production = false;
  if (ref.space == overlay_id_space::base) {
    for (auto pid : base.productions_by_parent[ref.id]) {
      if (overlay_delta_base_production_removed(delta, pid)) continue;
      validate_reachable_base_production(delta, pid);
      std::vector<overlay_clade_ref> children;
      children.reserve(base.productions[pid].children.size());
      for (auto child : base.productions[pid].children) {
        children.push_back(base_clade_ref(child));
      }
      accumulate_overlay_production_row(row, children, provider);
      saw_production = true;
    }
  }

  for (auto temp_pid : temp_productions_for_parent(delta, ref)) {
    if (temp_pid == no_production ||
        temp_pid >= delta.temp_productions.size()) {
      throw std::runtime_error(
          "chart SPR overlay-delta: temp production id out of range during "
          "row recompute");
    }
    auto const& prod = delta.temp_productions[temp_pid];
    validate_overlay_delta_production_partition(delta, prod, temp_pid);
    accumulate_overlay_production_row(row, prod.children, provider);
    saw_production = true;
  }

  if (!saw_production) {
    throw std::runtime_error(
        "chart SPR overlay-delta: non-singleton clade has no productions "
        "during local row recompute");
  }
  return row;
}

}  // namespace chart_spr_search_detail

inline void build_local_overlay_chart_rows_into(
    spr_overlay_delta const& delta, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, local_overlay_chart_rows& rows,
    chart_options const& options = {},
    bool validate_base_chart_shapes = false) {
  if (options.keep_trace) {
    throw std::runtime_error(
        "chart SPR overlay-delta: local row scorer does not support trace "
        "storage");
  }
  if (delta.base == nullptr) {
    throw std::runtime_error("chart SPR overlay-delta: missing base grammar");
  }
  auto const& base = *delta.base;
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

  rows.base_row_slot = &delta.affected_base_row_slot;
  rows.temp_row_slot = &delta.affected_temp_row_slot;
  rows.rows.assign(delta.affected_order.size(),
                   parsimony_chart_detail::make_inf_row());

  overlay_row_provider provider{delta, base_chart, rows};
  for (std::size_t i = 0; i < delta.affected_order.size(); ++i) {
    auto ref = delta.affected_order[i];
    rows.rows[i] = chart_spr_search_detail::recompute_overlay_delta_row(
        delta, leaf_states, provider, ref);
  }
}

inline local_overlay_chart_rows build_local_overlay_chart_rows(
    spr_overlay_delta const& delta, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, chart_options const& options = {},
    bool validate_base_chart_shapes = false) {
  local_overlay_chart_rows rows;
  build_local_overlay_chart_rows_into(delta, base_chart, leaf_states, rows,
                                      options, validate_base_chart_shapes);
  return rows;
}

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
    leaf_site_states const& leaf_states, chart_options options) {
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

inline void add_chart_spr_search_counters(
    chart_spr_search_counters& dst, chart_spr_search_counters const& src) {
  dst.grammar_rebuilds += src.grammar_rebuilds;
  dst.pattern_rebuilds += src.pattern_rebuilds;
  dst.base_chart_cache_rebuilds += src.base_chart_cache_rebuilds;
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
  dst.sidecar_rebuilds_after_accept += src.sidecar_rebuilds_after_accept;
  dst.full_composite_rebuilds += src.full_composite_rebuilds;
  dst.local_candidate_scores += src.local_candidate_scores;
  dst.local_rows_recomputed += src.local_rows_recomputed;
  dst.local_score_parallel_batches += src.local_score_parallel_batches;
  dst.local_score_worker_tasks += src.local_score_worker_tasks;
  dst.candidate_batches_scored += src.candidate_batches_scored;
  dst.pattern_batch_cache_builds += src.pattern_batch_cache_builds;
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
  dst.transient_chain_extension_fallbacks +=
      src.transient_chain_extension_fallbacks;
  dst.transient_chain_extension_oracle_mismatches +=
      src.transient_chain_extension_oracle_mismatches;
  dst.transient_chain_extension_oracle_rows_checked_for_tests +=
      src.transient_chain_extension_oracle_rows_checked_for_tests;
}

struct chart_spr_local_score_scratch {
  local_overlay_chart_rows rows;
};

namespace chart_spr_search_detail {

struct prepared_local_candidate_score {
  grammar_spr_candidate const* candidate = nullptr;
  spr_overlay_delta delta;
  std::optional<overlay_materialization_result> verification_materialized;
  std::uint64_t new_active_score = 0;
  chart_spr_candidate_score scored;
  bool valid_for_accumulation = false;
};

inline prepared_local_candidate_score prepare_local_candidate_score(
    chart_spr_search_state const& state,
    grammar_spr_candidate const& candidate,
    local_spr_score_options const& options,
    chart_spr_search_counters* counters) {
  if (options.exact_multisite) {
    throw std::runtime_error(
        "chart SPR local score: exact_multisite belongs to the Phase-4 "
        "verification gate, not the Phase-3 composite local scorer");
  }

  if (counters != nullptr) ++counters->local_candidate_scores;

  prepared_local_candidate_score prepared;
  prepared.candidate = &candidate;
  prepared.scored.candidate = candidate;
  try {
    prepared.delta = build_spr_overlay_delta(state.grammar, candidate,
                                             options);
  } catch (std::exception const& e) {
    prepared.scored = make_invalid_local_candidate_score(state, candidate,
                                                         e.what());
    return prepared;
  }
  if (counters != nullptr) {
    record_overlay_delta_reachability_counters(
        *counters, prepared.delta.reachability_stats);
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
  return prepared;
}

inline void invalidate_prepared_local_candidate(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared, std::string reason) {
  prepared.scored = make_invalid_local_candidate_score(
      state, *prepared.candidate, std::move(reason));
  prepared.valid_for_accumulation = false;
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
  auto const& patterns = state.active_patterns.patterns.patterns;
  auto chart_build_options = state.chart_opts;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;
  try {
    for (std::size_t i = 0; i < entries.size(); ++i) {
      auto pattern_index = pattern_offset + i;
      if (pattern_index >= patterns.size()) {
        throw std::runtime_error(
            "chart SPR local score: pattern batch offset out of range");
      }
      auto const& pattern = patterns[pattern_index];
      auto const& cache_entry = entries[i];
      leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
      build_local_overlay_chart_rows_into(
          prepared.delta, cache_entry.chart, states, scratch.rows,
          chart_build_options, options.validate_cached_chart_shapes);
      if (counters != nullptr) {
        counters->local_rows_recomputed +=
            prepared.delta.affected_order.size();
      }
      if (prepared.verification_materialized) {
        verify_local_overlay_rows_against_full(
            prepared.delta, scratch.rows, cache_entry.chart,
            *prepared.verification_materialized, states, state.chart_opts);
      }
      auto const& root_row = local_overlay_chart_row(
          scratch.rows, cache_entry.chart, prepared.delta.root);
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

inline chart_spr_candidate_score finish_prepared_local_candidate_score(
    chart_spr_search_state const& state,
    prepared_local_candidate_score& prepared) {
  if (!prepared.valid_for_accumulation) return prepared.scored;

  auto new_score = chart_multisite_detail::checked_add_u64(
      prepared.new_active_score, state.invariant_constant_offset,
      "chart-SPR local candidate invariant offset");
  auto old_score = state.composite_lower_bound_with_invariants;
  auto& scored = prepared.scored;
  scored.lower_bound.value = spr_score_result{
      chart_spr_detail::signed_delta(old_score, new_score), old_score,
      new_score, false};
  scored.lower_bound.kind = chart_spr_score_kind::composite_lower_bound;
  scored.lower_bound.convention =
      chart_spr_score_convention::full_with_invariants;
  scored.lower_bound.invariant_offset_applied =
      state.invariant_constant_offset;
  scored.affected_clade_count = prepared.delta.affected_order.size();
  scored.valid = true;
  scored.invalid_reason.clear();
  return scored;
}

inline std::vector<pattern_chart_cache_entry>
build_pattern_chart_cache_entries_for_range(
    chart_spr_search_state const& state, std::size_t begin,
    std::size_t count) {
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
        state.grammar, patterns[begin + i], state.chart_opts,
        chart_build_options));
  }
  return entries;
}

inline chart_spr_candidate_score score_candidate_locally_counted(
    chart_spr_search_state const& state,
    grammar_spr_candidate const& candidate,
    local_spr_score_options const& options,
    chart_spr_search_counters* counters,
    chart_spr_local_score_scratch& scratch) {
  auto prepared = prepare_local_candidate_score(state, candidate, options,
                                                counters);
  if (!prepared.valid_for_accumulation) return prepared.scored;

  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    if (state.pattern_charts.size() !=
        state.active_patterns.patterns.patterns.size()) {
      throw std::runtime_error(
          "chart SPR local score: pattern chart cache size mismatch");
    }
    accumulate_prepared_local_candidate_patterns(
        state, prepared, 0, state.pattern_charts, options, counters, scratch);
    return finish_prepared_local_candidate_score(state, prepared);
  }

  auto const& patterns = state.active_patterns.patterns.patterns;
  auto batch_size = std::max<std::size_t>(
      1, state.effective_pattern_batch_size);
  for (std::size_t begin = 0; begin < patterns.size(); begin += batch_size) {
    auto count = std::min(batch_size, patterns.size() - begin);
    auto entries = build_pattern_chart_cache_entries_for_range(state, begin,
                                                               count);
    if (counters != nullptr) ++counters->pattern_batch_cache_builds;
    accumulate_prepared_local_candidate_patterns(
        state, prepared, begin, entries, options, counters, scratch);
    if (!prepared.valid_for_accumulation) break;
  }
  return finish_prepared_local_candidate_score(state, prepared);
}

inline std::size_t normalize_chart_spr_worker_count(
    std::size_t requested) {
  if (requested == 0) {
    requested = std::thread::hardware_concurrency();
  }
  return std::max<std::size_t>(1, requested);
}

inline std::vector<chart_spr_candidate_score> score_candidates_locally_all_cache(
    chart_spr_search_state const& state,
    std::vector<grammar_spr_candidate> const& candidates,
    local_spr_score_options const& options,
    std::size_t worker_count) {
  std::vector<chart_spr_candidate_score> scores(candidates.size());
  chart_spr_search_counters aggregate;
  if (candidates.empty()) return scores;
  ++aggregate.candidate_batches_scored;

  worker_count = std::min(normalize_chart_spr_worker_count(worker_count),
                          candidates.size());
  if (worker_count <= 1) {
    chart_spr_local_score_scratch scratch;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      auto start = std::chrono::steady_clock::now();
      scores[i] = score_candidate_locally_counted(
          state, candidates[i], options, &aggregate, scratch);
      scores[i].local_score_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
    }
    add_chart_spr_search_counters(state.counters, aggregate);
    return scores;
  }

  ++aggregate.local_score_parallel_batches;
  aggregate.local_score_worker_tasks += worker_count;
  std::vector<chart_spr_search_counters> worker_counters(worker_count);
  std::vector<std::future<void>> futures;
  futures.reserve(worker_count);
  thread_pool pool(worker_count);
  auto chunk = (candidates.size() + worker_count - 1) / worker_count;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    auto begin = worker * chunk;
    auto end = std::min(candidates.size(), begin + chunk);
    if (begin >= end) continue;
    futures.push_back(pool.submit([&, worker, begin, end] {
      chart_spr_local_score_scratch scratch;
      for (std::size_t i = begin; i < end; ++i) {
        auto start = std::chrono::steady_clock::now();
        scores[i] = score_candidate_locally_counted(
            state, candidates[i], options, &worker_counters[worker],
            scratch);
        scores[i].local_score_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start)
                .count();
      }
    }));
  }
  for (auto& future : futures) future.get();
  for (auto const& counters : worker_counters) {
    add_chart_spr_search_counters(aggregate, counters);
  }
  add_chart_spr_search_counters(state.counters, aggregate);
  return scores;
}

inline std::vector<chart_spr_candidate_score>
score_candidates_locally_pattern_batches(
    chart_spr_search_state const& state,
    std::vector<grammar_spr_candidate> const& candidates,
    local_spr_score_options const& options,
    std::size_t worker_count) {
  std::vector<chart_spr_candidate_score> scores(candidates.size());
  chart_spr_search_counters aggregate;
  if (candidates.empty()) return scores;
  auto batch_start = std::chrono::steady_clock::now();
  ++aggregate.candidate_batches_scored;

  std::vector<prepared_local_candidate_score> prepared;
  prepared.reserve(candidates.size());
  for (auto const& candidate : candidates) {
    prepared.push_back(prepare_local_candidate_score(state, candidate,
                                                     options, &aggregate));
  }

  worker_count = std::min(normalize_chart_spr_worker_count(worker_count),
                          candidates.size());
  std::optional<thread_pool> pool;
  if (worker_count > 1) {
    ++aggregate.local_score_parallel_batches;
    pool.emplace(worker_count);
  }

  auto const& patterns = state.active_patterns.patterns.patterns;
  auto batch_size = std::max<std::size_t>(
      1, state.effective_pattern_batch_size);
  for (std::size_t begin = 0; begin < patterns.size(); begin += batch_size) {
    auto count = std::min(batch_size, patterns.size() - begin);
    auto entries = build_pattern_chart_cache_entries_for_range(state, begin,
                                                               count);
    ++aggregate.pattern_batch_cache_builds;

    if (worker_count <= 1) {
      chart_spr_local_score_scratch scratch;
      for (auto& item : prepared) {
        accumulate_prepared_local_candidate_patterns(
            state, item, begin, entries, options, &aggregate, scratch);
      }
    } else {
      aggregate.local_score_worker_tasks += worker_count;
      std::vector<chart_spr_search_counters> worker_counters(worker_count);
      std::vector<std::future<void>> futures;
      futures.reserve(worker_count);
      auto chunk = (prepared.size() + worker_count - 1) / worker_count;
      for (std::size_t worker = 0; worker < worker_count; ++worker) {
        auto item_begin = worker * chunk;
        auto item_end = std::min(prepared.size(), item_begin + chunk);
        if (item_begin >= item_end) continue;
        futures.push_back(pool->submit([&, worker, item_begin, item_end] {
          chart_spr_local_score_scratch scratch;
          for (std::size_t i = item_begin; i < item_end; ++i) {
            accumulate_prepared_local_candidate_patterns(
                state, prepared[i], begin, entries, options,
                &worker_counters[worker], scratch);
          }
        }));
      }
      for (auto& future : futures) future.get();
      for (auto const& counters : worker_counters) {
        add_chart_spr_search_counters(aggregate, counters);
      }
    }
  }

  auto batch_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - batch_start)
                      .count();
  auto per_candidate_ms = batch_ms /
                          static_cast<double>(prepared.size());
  for (std::size_t i = 0; i < prepared.size(); ++i) {
    scores[i] = finish_prepared_local_candidate_score(state, prepared[i]);
    scores[i].local_score_ms = per_candidate_ms;
  }
  add_chart_spr_search_counters(state.counters, aggregate);
  return scores;
}

}  // namespace chart_spr_search_detail

// Batch local scorer used by Phase 7.  It parallelizes over candidates when a
// worker count greater than one is requested.  In pattern-batch cache mode,
// base charts are built once per pattern batch and reused across the bounded
// candidate batch; they are never rebuilt once per candidate by this API.
inline std::vector<chart_spr_candidate_score> score_candidates_locally(
    chart_spr_search_state const& state,
    std::vector<grammar_spr_candidate> const& candidates,
    local_spr_score_options const& options = {},
    std::size_t worker_count = 1) {
  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    return chart_spr_search_detail::score_candidates_locally_all_cache(
        state, candidates, options, worker_count);
  }
  return chart_spr_search_detail::score_candidates_locally_pattern_batches(
      state, candidates, options, worker_count);
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
  std::vector<grammar_spr_candidate> one{candidate};
  auto scores = score_candidates_locally(state, one, options, 1);
  return scores.empty() ? make_invalid_local_candidate_score(
                             state, candidate,
                             "chart SPR local score: empty score batch")
                        : std::move(scores.front());
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
    multisite_trim_options const& trim_options = {}) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!state.exact_trim_active_only) {
    state.exact_trim_active_only = build_multisite_trim_active(
        state.grammar, state.active_patterns, state.chart_opts,
        trim_options);
  }
  return *state.exact_trim_active_only;
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
    multisite_trim_options const& trim_options = {}) {
  if (!candidate.valid) return candidate;
  ++state.counters.exact_verifications;

  try {
    auto const& old_trim =
        ensure_chart_spr_state_exact_trim(state, trim_options);
    auto overlay = overlay_from_candidate(state.grammar, candidate.candidate);
    auto materialized = materialize_overlay_grammar(overlay);
    ++state.counters.full_overlay_materializations;
    ++state.counters.overlay_materializations_for_exact_verification;

    auto new_trim = build_multisite_trim_active(
        materialized.grammar, state.active_patterns, state.chart_opts,
        trim_options);

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
    candidate.valid = false;
    candidate.invalid_reason = e.what();
  }
  return candidate;
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
  if (children.size() != 2) {
    throw std::runtime_error(context + ": selected production is not binary");
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
    std::vector<std::uint8_t>& temp_state) {
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
    auto left = chart_spr_restricted_overlay_topology_row_impl(
        base, candidate, pattern, selected, children[0], base_memo, temp_memo,
        base_state, temp_state);
    auto right = chart_spr_restricted_overlay_topology_row_impl(
        base, candidate, pattern, selected, children[1], base_memo, temp_memo,
        base_state, temp_state);
    row = chart_multisite_detail::combine_binary_rows(left, right);
  }

  memo_slot = row;
  state_slot = 2;
  return row;
}

inline std::array<chart_cost, nuc_state_count>
chart_spr_restricted_overlay_topology_row(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    site_pattern const& pattern,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected) {
  std::vector<std::optional<std::array<chart_cost, nuc_state_count>>> base_memo(
      base.clades.size());
  std::vector<std::optional<std::array<chart_cost, nuc_state_count>>> temp_memo(
      candidate.added_clades.size());
  std::vector<std::uint8_t> base_state(base.clades.size(), 0);
  std::vector<std::uint8_t> temp_state(candidate.added_clades.size(), 0);
  return chart_spr_restricted_overlay_topology_row_impl(
      base, candidate, pattern, selected, base_clade_ref(base.root_clade),
      base_memo, temp_memo, base_state, temp_state);
}

struct chart_spr_fixed_topology_pattern_scores {
  std::vector<std::uint64_t> old_pattern_scores;
  std::vector<std::uint64_t> new_pattern_scores;
  std::uint64_t old_active_total = 0;
  std::uint64_t new_active_total = 0;
};

inline chart_spr_fixed_topology_pattern_scores
fixed_topology_direct_selected_pattern_scores(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
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
        state.grammar, candidate.candidate, pattern, selected_after);
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
  return candidate;
}

inline chart_spr_candidate_score verify_candidate_for_acceptance(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_spr_search_options const& options) {
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return candidate;
    case chart_spr_acceptance_mode::exact_multisite:
      // Phase 9: when a local-commit substrate is active it installs
      // `state.exact_multisite_verifier`, which verifies the candidate by
      // transiently extending the chain + caches in reader-local scratch
      // (avoiding a fresh materialize_overlay_grammar, never mutating the
      // shared cache).  When absent (conservative rebuild mode, or a bare
      // state built without a substrate), the cold from-scratch path below is
      // used.  The cold path is also the correctness oracle the transient
      // verifier cross-checks when its test-only oracle flag is set.
      if (state.exact_multisite_verifier) {
        return state.exact_multisite_verifier(state, std::move(candidate),
                                              options.exact_trim);
      }
      return verify_candidate_exact_against_state(
          state, std::move(candidate), options.exact_trim);
    case chart_spr_acceptance_mode::fixed_topology_exact:
      if (state.fixed_topology_exact_verifier) {
        return state.fixed_topology_exact_verifier(state, std::move(candidate));
      }
      return verify_candidate_fixed_topology_exact(state, std::move(candidate));
  }
  return candidate;
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
    chart_spr_search_options const& options) {
  if (options.candidate_selection ==
      chart_spr_candidate_selection_mode::lower_bound_first_improvement) {
    return 1;
  }
  if (options.cache.candidate_batch_size != 0) {
    return std::max<std::size_t>(1, options.cache.candidate_batch_size);
  }
  auto workers = chart_spr_search_detail::normalize_chart_spr_worker_count(
      options.local_score_worker_count);
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
    chart_spr_search_options const& options) {
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    return chart_spr_state_exact_score_with_invariants(state,
                                                       options.exact_trim);
  }
  return state.composite_lower_bound_with_invariants;
}

// Score candidates with the Phase-3 local lower-bound scorer, retain the best
// candidates according to the configured candidate-selection policy, and apply
// the Phase-4 acceptance gate.  This function does not mutate/materialize the
// DAG; Phase 5 is responsible for applying an accepted candidate and rebuilding
// sidecar state.  Rejected candidates are locally scored only, while exact
// verification materializes overlays only for the retained verified set.
inline chart_spr_iteration_result run_chart_spr_acceptance_iteration(
    chart_spr_search_state const& state,
    chart_spr_search_options options = {}, std::size_t iteration = 0) {
  validate_supported_chart_cache_options(options.cache);
  if (options.candidate_selection ==
      chart_spr_candidate_selection_mode::sampled_or_randomized) {
    options.enumeration.randomize_order = true;
    options.enumeration.seed = options.seed;
  }

  chart_spr_iteration_result result;
  result.iteration = iteration;
  result.acceptance_mode = options.acceptance_mode;
  result.candidate_selection = options.candidate_selection;
  result.state_score_before =
      chart_spr_iteration_state_score_before(state, options);
  result.state_score_after = result.state_score_before;

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

  auto worker_count = chart_spr_search_detail::normalize_chart_spr_worker_count(
      options.local_score_worker_count);
  auto candidate_batch_size = chart_spr_effective_candidate_batch_size(
      state, options);
  state.effective_candidate_batch_size = candidate_batch_size;
  validate_chart_spr_pattern_batch_replay_strategy(
      state, options, enumeration, candidate_batch_size);
  auto local_options = local_spr_score_options{};
  local_options.verify_against_full_overlay =
      options.verify_local_against_full_for_tests;

  std::vector<grammar_spr_candidate> candidate_batch;
  candidate_batch.reserve(candidate_batch_size);
  bool stop_after_batch = false;

  auto process_candidate_batch = [&]() {
    if (candidate_batch.empty()) return;
    auto local_start = std::chrono::steady_clock::now();
    auto scored_batch = score_candidates_locally(
        state, candidate_batch, local_options, worker_count);
    result.local_scoring_ms += std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() -
                                   local_start)
                                   .count();
    for (auto& scored : scored_batch) {
      ++result.candidates_scored;
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
    candidate_batch.clear();
  };

  auto generation = for_each_grammar_spr_candidate(
      state.grammar, enumeration,
      [&](grammar_spr_candidate const& candidate) {
        candidate_batch.push_back(candidate);
        if (candidate_batch.size() >= candidate_batch_size) {
          process_candidate_batch();
          if (options.candidate_selection ==
                  chart_spr_candidate_selection_mode::lower_bound_first_improvement &&
              result.local_improving_candidates > 0) {
            stop_after_batch = true;
            return false;
          }
        }
        return true;
      });
  if (!stop_after_batch) process_candidate_batch();

  result.candidate_generation = generation;
  result.candidates_generated = generation.candidates_generated_after_dedup;
  result.locally_ranked_candidates_retained = ranked.size();
  result.affected_distribution =
      summarize_affected_clade_counts(affected_counts);
  result.affected_clade_counts = std::move(affected_counts);
  record_chart_spr_candidate_generation_stats(generation, state.counters);

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
      auto verified_candidate = verify_candidate_for_acceptance(
          state, std::move(candidate), options);
      result.exact_verification_ms +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - exact_start)
              .count();
      if (state.counters.exact_verifications > exact_verifications_before ||
          verified_candidate.exact) {
        ++result.candidates_exact_verified;
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

  auto old_score =
      build_composite_chart_score(base, patterns, options).weighted_lower_bound;
  if (counters != nullptr) ++counters->full_composite_rebuilds;
  auto new_score = build_composite_chart_score(materialized.grammar, patterns,
                                               options)
                       .weighted_lower_bound;
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

  auto old_score = build_multisite_trim(base, patterns, options, trim_options)
                       .optimum;
  auto new_score = build_multisite_trim(materialized.grammar, patterns, options,
                                        trim_options)
                       .optimum;
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
    if (source_prod.children.size() != 2) continue;

    for (std::size_t moved_i = 0; moved_i < 2; ++moved_i) {
      auto moved = source_prod.children[moved_i];
      auto old_sibling = source_prod.children[1 - moved_i];
      auto const& moved_taxa = grammar.clades[moved].taxa;

      for (clade_id target = 0; target < grammar.clades.size(); ++target) {
        if (target == moved || target == old_sibling ||
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
                grammar, base_lookup, source_pid, moved, old_sibling, target,
                source_path, dest_path);
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

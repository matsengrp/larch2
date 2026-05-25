#pragma once

#include <larch/chart_spr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
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
  // Phase-1 bridge path: local scorer reuses cached base charts but still
  // materializes one dense overlay per candidate until Phase-3 overlay-delta
  // scoring replaces this implementation detail.  Count it separately from
  // oracle/debug materializations.
  std::size_t overlay_materializations_for_local_scoring_bridge = 0;
  std::size_t overlay_materializations_for_exact_verification = 0;
  std::size_t overlay_materializations_for_accept_materialization = 0;
  std::size_t sidecar_rebuilds_after_accept = 0;
  std::size_t full_composite_rebuilds = 0;
  std::size_t local_candidate_scores = 0;
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
  std::size_t candidate_cap_cutoffs = 0;
  std::size_t path_budget_cutoffs = 0;

  // Overlay validation/reachability counters.
  std::size_t overlay_reachability_validations = 0;
  std::size_t reachable_clades_traversed = 0;
  std::size_t reachable_productions_traversed = 0;
  std::size_t reachable_temp_clades_traversed = 0;
  std::size_t reachable_temp_productions_traversed = 0;
  std::size_t reachability_full_grammar_like_passes = 0;
};

enum class chart_spr_acceptance_mode {
  exact_multisite,
  fixed_topology_exact,
  lower_bound_heuristic,
};

enum class chart_spr_candidate_selection_mode {
  exhaustive_exact,
  lower_bound_top_k,
  lower_bound_first_improvement,
  sampled_or_randomized,
};

enum class chart_spr_topology_selection_kind {
  none,
  explicit_certificate,
  deterministic_selector,
};

struct chart_spr_production_signature {
  // Stable across dense overlay materialization/rebuild.  In-process reports
  // use taxon IDs; cross-run reports can translate them to sample IDs.
  std::vector<taxon_id> parent_taxa;
  std::vector<std::vector<taxon_id>> child_taxa;
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

struct chart_cache_options {
  std::size_t max_cached_patterns = 0;  // 0 = all patterns
  std::size_t memory_budget_bytes = 0;  // 0 = no explicit budget
  std::size_t candidate_batch_size = 0; // 0 = choose from memory budget
};

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

  bool materialize_accepted_moves = true;
  bool rebuild_after_accept = true;  // conservative first production default
  bool verify_local_against_full_for_tests = false;
  std::uint32_t seed = 1;
};

enum class chart_spr_score_kind {
  composite_lower_bound,
  fixed_topology_exact,
  grammar_exact,
};

enum class chart_spr_score_convention {
  active_only,
  full_with_invariants,
};

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
};

struct affected_clade_distribution {
  double mean = 0.0;
  std::size_t p50 = 0;
  std::size_t p95 = 0;
  std::size_t max = 0;
};

struct chart_spr_iteration_result {
  std::size_t iteration = 0;
  std::size_t candidates_generated = 0;
  std::size_t candidates_scored = 0;
  std::size_t candidates_exact_verified = 0;
  affected_clade_distribution affected_distribution;
  std::optional<chart_spr_candidate_score> accepted;
  std::uint64_t state_score_before = 0;
  std::uint64_t state_score_after = 0;
};

struct chart_spr_search_result {
  phylo_dag dag;
  std::vector<chart_spr_iteration_result> iterations;
  chart_spr_search_counters counters;
};

struct chart_spr_search_state {
  phylo_dag* dag = nullptr;
  clade_grammar grammar;
  site_pattern_set patterns;
  chart_options chart_opts;
  std::vector<single_site_chart> pattern_charts;
  std::uint64_t composite_lower_bound_with_invariants = 0;
  chart_spr_search_counters counters;
};

inline chart_spr_search_state build_chart_spr_search_state(
    phylo_dag& dag, clade_grammar grammar, site_pattern_set patterns,
    chart_options options = {}) {
  chart_spr_search_state state;
  state.dag = &dag;
  state.grammar = std::move(grammar);
  state.patterns = std::move(patterns);
  state.chart_opts = options;
  ++state.counters.base_chart_cache_rebuilds;
  state.counters.skipped_invariant_sites =
      state.patterns.skipped_invariant_site_count;

  auto chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;
  state.pattern_charts.reserve(state.patterns.patterns.size());

  std::uint64_t total = 0;
  for (auto const& pattern : state.patterns.patterns) {
    leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
    auto chart = build_single_site_chart(state.grammar, states,
                                         chart_build_options);
    total = chart_multisite_detail::checked_add_u64(
        total,
        chart_multisite_detail::weighted_root_score_from_row(
            chart.inside[state.grammar.root_clade], pattern, options),
        "chart-SPR cached composite lower bound");
    state.pattern_charts.push_back(std::move(chart));
  }
  if (options.score_ua_edge) {
    total = chart_multisite_detail::checked_add_u64(
        total,
        state.patterns.skipped_invariant_constant_score_with_reference_edge,
        "chart-SPR skipped invariant UA-edge offset");
  }
  state.composite_lower_bound_with_invariants = total;
  return state;
}

// Phase-1 local scoring entry point.  This uses the existing materialized
// overlay-local recompute as the implementation bridge until the Phase-3
// lightweight overlay-delta scorer replaces the materialization step.  It
// reuses the state's cached base charts and does not call
// build_composite_chart_score() per rejected candidate.  Bridge overlay
// materializations are counted as local-scoring bridge work, not oracle work.
inline chart_spr_candidate_score score_candidate_locally(
    chart_spr_search_state& state, grammar_spr_candidate const& candidate) {
  if (state.pattern_charts.size() != state.patterns.patterns.size()) {
    throw std::runtime_error(
        "chart SPR local score: pattern chart cache size mismatch");
  }

  auto overlay = overlay_from_candidate(state.grammar, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  ++state.counters.full_overlay_materializations;
  ++state.counters.overlay_materializations_for_local_scoring_bridge;
  ++state.counters.local_candidate_scores;

  std::uint64_t new_score = 0;
  std::size_t affected_count = 0;
  auto chart_build_options = state.chart_opts;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  for (std::size_t pattern_index = 0;
       pattern_index < state.patterns.patterns.size(); ++pattern_index) {
    auto const& pattern = state.patterns.patterns[pattern_index];
    leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
    auto local = build_single_site_overlay_chart_locally(
        overlay, materialized, state.pattern_charts[pattern_index], states,
        chart_build_options);
    affected_count = std::max(affected_count, local.affected_clade_count);
    new_score = chart_multisite_detail::checked_add_u64(
        new_score,
        chart_multisite_detail::weighted_root_score_from_row(
            local.chart.inside[materialized.grammar.root_clade], pattern,
            state.chart_opts),
        "chart-SPR local candidate lower bound");
  }
  if (state.chart_opts.score_ua_edge) {
    new_score = chart_multisite_detail::checked_add_u64(
        new_score,
        state.patterns.skipped_invariant_constant_score_with_reference_edge,
        "chart-SPR local skipped invariant UA-edge offset");
  }

  auto old_score = state.composite_lower_bound_with_invariants;
  chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.lower_bound.value = spr_score_result{
      chart_spr_detail::signed_delta(old_score, new_score), old_score,
      new_score, false};
  scored.lower_bound.kind = chart_spr_score_kind::composite_lower_bound;
  scored.lower_bound.convention =
      chart_spr_score_convention::full_with_invariants;
  if (state.chart_opts.score_ua_edge) {
    scored.lower_bound.invariant_offset_applied =
        state.patterns.skipped_invariant_constant_score_with_reference_edge;
  }
  scored.affected_clade_count = affected_count;
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

            auto signature = candidate_signature(*candidate);
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

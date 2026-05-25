#pragma once

#include <larch/chart_spr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

enum class chart_spr_candidate_stop_reason {
  exhausted,
  candidate_cap,
  path_budget,
  callback_stop,
};

inline char const* chart_spr_candidate_stop_reason_name(
    chart_spr_candidate_stop_reason reason) {
  switch (reason) {
    case chart_spr_candidate_stop_reason::exhausted:
      return "exhausted";
    case chart_spr_candidate_stop_reason::candidate_cap:
      return "candidate_cap";
    case chart_spr_candidate_stop_reason::path_budget:
      return "path_budget";
    case chart_spr_candidate_stop_reason::callback_stop:
      return "callback_stop";
  }
  return "unknown";
}

struct chart_spr_candidate_generation_stats {
  std::size_t upward_path_iterator_steps = 0;
  std::size_t upward_paths_completed = 0;
  std::size_t path_pairs_considered = 0;
  std::size_t candidates_constructed = 0;
  std::size_t candidates_pruned_before_construction = 0;
  std::size_t candidates_pruned_after_construction = 0;
  chart_spr_candidate_stop_reason stop_reason =
      chart_spr_candidate_stop_reason::exhausted;
};

struct affected_clade_distribution {
  double mean = 0.0;
  std::size_t p50 = 0;
  std::size_t p95 = 0;
  std::size_t max = 0;
};

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

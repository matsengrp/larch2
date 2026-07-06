#pragma once

// WRIC DAG-native SPR & rank-3 rewrite: Phase 7 -- Option C as a committed
// overlay delta (Work item 2 <-> Work items 1+3 composition).
//
// Phase 6 delivered Option C as a standalone direct in-place production splice
// on a `phylo_dag` (`option_c_splice_production`).  This header is the
// chart-search-mode entry point: it expresses the same binary production
// rewrite as a committed overlay delta, so an Option C move commits through
// the Phase 4 path (overlay-chain append + paired inside/outside cache
// commits) and recomputes only the affected rows instead of a full chart
// rebuild.  Standalone mode (Phase 6) and chart-search mode (Phase 7) produce
// merge-equivalent outputs at the taxon-set-key level (Phase 7 exit criterion
// 2), checked in the Phase 7 test.
//
// The commit is built entirely from existing substrates:
//   * `option_c_as_overlay_delta` (rank3_rewrite.hpp) builds the
//     `spr_overlay_delta` in the stable overlay vocabulary;
//   * `overlay_chain::append` folds it onto the frozen base;
//   * `apply_commit_to_inside_cache` / `apply_commit_to_outside_cache`
//     (Phase 2/3) update the persistent caches on the affected sets only.
// No new chart recurrence, no new overlay-vocabulary field, no DAG mutation:
// the chart-search path never touches a `phylo_dag` here (compaction of the
// committed chain into an output DAG is Phase 5's grammar-valued oracle, which
// already handles SPR-committed chains and treats Option-C-committed chains
// identically).
//
// Tombstone scope (inherited from Phase 4).  The overlay vocabulary has no
// removed-temp-productions field (a plan non-goal), so only a before
// production that resolves to a frozen-base production may commit.  An Option
// C rewrite whose before production was introduced by an earlier chain delta
// is rejected with a labelled error by `overlay_chain::append` (the same
// committability gate the Phase 4 SPR path uses); it is not a silent no-op.
// The common case (rewriting an original production of the search input, or
// the first commit on a fresh chain) always satisfies this.

#include <larch/chart_two_chart_oracle.hpp>   // recompute_both_charts_from_scratch
#include <larch/inside_chart_cache.hpp>      // apply_commit_to_inside_cache
#include <larch/outside_chart_cache.hpp>     // apply_commit_to_outside_cache
#include <larch/overlay_chain.hpp>           // overlay_chain, materialize_overlay_chain
#include <larch/rank3_rewrite.hpp>           // option_c_as_overlay_delta

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace larch {

struct option_c_chain_commit_options {
  // Conservative default mirrors Phase 6's generated_edge_weight: a rewritten
  // edge cannot lower an existing edge-weight objective by accident.  The
  // chain-commit path operates on the grammar/overlay and does not assign edge
  // weights directly; the field is retained so the documented policy is
  // visible at the commit API and a future DAG-aware compaction can read it.
  float generated_edge_weight = std::numeric_limits<float>::max();
  // `merge`: splice replaces every before-witness with the after structure
  // (tombstone before, add after).  `no_op_if_present`: if the after
  // production is already represented in the tip grammar, the commit is a
  // documented no-op (no append, no cache mutation).  Mirrors Phase 6.
  option_c_after_present_policy after_present =
      option_c_after_present_policy::merge;
  // Outside-chart affected-set policy.  The conservative superset (all clades
  // reachable from the root) is the documented default for every move class in
  // the initial scope; a tighter set is only adopted per move class after the
  // two-chart oracle confirms containment (Work item 3 adoption policy).
  outside_affected_policy outside_policy =
      outside_affected_policy::conservative_superset;
  // Phase 3 two-chart oracle self-check after the commit: recompute BOTH
  // charts from scratch on the materialized chain tip and assert the
  // persistent caches agree on every reachable clade, every active pattern.
  // Test/diagnostic only; the production path does not run a hidden
  // from-scratch oracle per commit.
  bool verify_two_chart_oracle_for_tests = false;
};

struct option_c_chain_commit_result {
  rank3_production_taxa_key before_key;
  rank3_production_taxa_key after_key;
  // The before production's id in the tip grammar the commit was built against
  // (resolved by taxon-set key).
  production_id before_tip_production = no_production;
  // Number of before-production witnesses removed by this commit.  Always 1
  // for a successful commit (exactly one base production is tombstoned); 0 for
  // a `no_op_if_present` short-circuit.  Named to match Phase 6's
  // `option_c_splice_result::witnesses_spliced` so reports comparing the two
  // modes read the same field.
  std::size_t witnesses_spliced = 0;
  // Set when `after_present == no_op_if_present` short-circuited the commit
  // because the after key was already represented in the tip grammar.
  bool after_already_present_no_op = false;
  // This commit's contribution to the persistent-cache counters (the caches
  // themselves are cumulative).  Bounded by the affected sets, not a full
  // rebuild, per the Phase 7 exit criterion.
  std::size_t inside_rows_recomputed = 0;
  std::size_t outside_rows_recomputed = 0;
  // Affected-set sizes (per-clade, before per-pattern fan-out) for this
  // commit, mirrored from `compute_chain_inside_affected_set` /
  // `compute_chain_outside_affected_set` so a "full rebuild" regression is
  // visible without reading the caches.
  std::size_t inside_affected_clade_count = 0;
  std::size_t outside_affected_clade_count = 0;
  // Named commit-mode label, distinct from Option A/B in reports (Phase 7 exit
  // criterion 3).  Option A/B materialize-and-merge paths do not commit
  // through the overlay chain; this label identifies an Option-C chain commit
  // unambiguously.
  static constexpr char const* commit_label = "option_c_chain_commit";
};

namespace option_c_chain_commit_detail {

// Phase 3 two-chart oracle: recompute BOTH charts from scratch on the
// materialized chain tip and assert the persistent caches agree on every
// reachable clade, every active pattern.  This is the load-bearing guard
// against inside/outside affected-set under-inclusion (the most likely silent
// bug).  Mirrors `chart_spr_assert_local_commit_two_chart_oracle` in
// chart_spr_search.cpp; kept here so the Option-C commit path's self-check and
// the Phase 7 test share one oracle definition.
inline void assert_two_chart_oracle(overlay_chain const& chain,
                                    inside_chart_cache const& icache,
                                    outside_chart_cache const& ocache,
                                    std::string const& context) {
  auto materialized = materialize_overlay_chain(chain);
  auto const& grammar = materialized.grammar;
  if (materialized.dense_clade_to_ref.size() != grammar.clades.size()) {
    throw std::runtime_error("option C chain commit two-chart oracle [" +
                             context + "]: dense clade map size mismatch");
  }
  for (std::size_t p = 0; p < icache.patterns.size(); ++p) {
    leaf_site_states states;
    states.state_by_taxon = icache.patterns[p].state_by_taxon;
    auto oracle =
        recompute_both_charts_from_scratch(grammar, states, icache.chart_opts);
    if (oracle.first.inside.size() != materialized.dense_clade_to_ref.size() ||
        oracle.second.outside.size() !=
            materialized.dense_clade_to_ref.size()) {
      throw std::runtime_error("option C chain commit two-chart oracle [" +
                               context + "]: oracle chart size mismatch");
    }
    for (std::size_t dense = 0; dense < materialized.dense_clade_to_ref.size();
         ++dense) {
      auto ref = materialized.dense_clade_to_ref[dense];
      if (icache.row(p, ref) != oracle.first.inside[dense]) {
        throw std::runtime_error(
            "option C chain commit two-chart oracle [" + context +
            "]: inside row mismatch at pattern " + std::to_string(p) +
            " dense clade " + std::to_string(dense));
      }
      if (ocache.row(p, ref) != oracle.second.outside[dense]) {
        throw std::runtime_error(
            "option C chain commit two-chart oracle [" + context +
            "]: outside row mismatch at pattern " + std::to_string(p) +
            " dense clade " + std::to_string(dense));
      }
    }
    auto cached_global = outside_cache_global_min(ocache, icache, p);
    if (cached_global != oracle.second.global_min) {
      throw std::runtime_error(
          "option C chain commit two-chart oracle [" + context +
          "]: global_min mismatch at pattern " + std::to_string(p));
    }
  }
}

}  // namespace option_c_chain_commit_detail

// Commit an Option C production rewrite to the overlay chain + persistent
// inside/outside caches.  `chain` / `icache` / `ocache` must share the same
// frozen base grammar and be at the same commit epoch (the post-state of the
// previous commit, or the cold build).  The before production is resolved by
// taxon-set key against the materialized chain tip; the after structure is
// expressed in taxon sets, so identity survives materialization and rebuild
// exactly as in Phase 6 / Option A/B.
//
// On a labelled throw (absent before, polytomy before/after, boundary
// mismatch, before-production not a frozen-base production, dangling clade
// reference) the chain and caches are left unchanged: every guard fires before
// `chain.append`, and `overlay_chain::append` provides a strong exception
// guarantee.
inline option_c_chain_commit_result option_c_commit_via_chain(
    overlay_chain& chain, inside_chart_cache& icache,
    outside_chart_cache& ocache, rank3_production_taxa_key before_key,
    option_c_after_production const& after,
    option_c_chain_commit_options const& options = {}) {
  if (icache.base != &chain.base()) {
    throw std::runtime_error(
        "option C chain commit: inside cache base does not match chain base");
  }
  if (ocache.base != &chain.base()) {
    throw std::runtime_error(
        "option C chain commit: outside cache base does not match chain base");
  }

  // Grammar-only materialization of the chain tip (no chart rescoring).  This
  // is the same cheap tip refresh the Phase 4 SPR local-commit path performs;
  // it is NOT a dense materialization in the cross-cutting-counter sense (no
  // fresh chart rebuild).  Kept alive until after `chain.append` because the
  // delta's `base` pointer aliases it.
  auto materialized = materialize_overlay_chain(chain);
  auto const& tip = materialized.grammar;

  auto delta_result = option_c_as_overlay_delta(tip, before_key, after);

  option_c_chain_commit_result result;
  result.before_key = delta_result.before_key;
  result.after_key = delta_result.after_key;
  result.before_tip_production = delta_result.before_tip_production;

  // no_op_if_present short-circuit: a documented no-op when the after key is
  // already represented.  Never a silent splice; the result is labelled.
  if (options.after_present ==
          option_c_after_present_policy::no_op_if_present &&
      delta_result.after_already_present) {
    result.after_already_present_no_op = true;
    return result;
  }

  // Capture cumulative counter baselines so the result reports this commit's
  // contribution (the caches are cumulative across commits).
  std::size_t inside_before = icache.inside_rows_recomputed_on_commit;
  std::size_t outside_before = ocache.outside_rows_recomputed_on_commit;

  // Commit: append + paired inside/outside cache commits.  All three provide a
  // strong exception guarantee up to the append; once the append succeeds the
  // cache commits are load-bearing and a failure there is a hard correctness
  // error (mirroring the Phase 4 contract in
  // chart_spr_commit_accepted_locally).
  chain.append(delta_result.delta);
  apply_commit_to_inside_cache(chain, icache);
  apply_commit_to_outside_cache(chain, ocache, icache, options.outside_policy);

  result.witnesses_spliced = 1;
  result.inside_rows_recomputed =
      icache.inside_rows_recomputed_on_commit - inside_before;
  result.outside_rows_recomputed =
      ocache.outside_rows_recomputed_on_commit - outside_before;

  // Affected-set sizes for this commit (the last-appended delta).
  // Informational only; recomputed here because the commit primitives do not
  // expose the set they used.
  auto inside_affected = compute_chain_inside_affected_set(chain);
  result.inside_affected_clade_count = inside_affected.size();
  auto outside_affected =
      compute_chain_outside_affected_set(chain, options.outside_policy);
  result.outside_affected_clade_count = outside_affected.size();

  if (options.verify_two_chart_oracle_for_tests) {
    option_c_chain_commit_detail::assert_two_chart_oracle(
        chain, icache, ocache,
        "after option C commit " + std::to_string(chain.size()));
  }

  return result;
}

}  // namespace larch

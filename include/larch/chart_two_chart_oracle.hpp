#pragma once

// WRIC DAG-native SPR & rank-3 rewrite: from-scratch two-chart oracle.
//
// This header is the shared oracle every incremental commit / cache-update
// path in the DAG-native SPR plan must agree with.  It owns *no* incremental
// logic: it recomputes both the inside chart (`ch[X][s]`) and the outside
// chart (`ch^uparrow[X][s]`) for one site from scratch by delegating to the
// existing dense construction primitives `build_single_site_chart` and
// `build_single_site_outside_chart`.
//
// The two-chart symmetry of this oracle is deliberate.  An inside-only oracle
// cannot catch under-inclusion in an incremental *outside*-chart affected set,
// which is the most likely silent correctness bug in Work item 3.  Later
// phases therefore call this oracle (both halves) after every commit and
// assert that the persisted incremental charts equal these from-scratch
// values.
//
// Phase 0 deliverable per the plan: the helper exists, is thin, and its tests
// confirm it reproduces the current dense inside+outside charts on the small
// committed fixtures before any incremental machinery is layered on top.

#include <larch/chart_trim.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/phylo_dag.hpp>

#include <cstdint>
#include <utility>

namespace larch {

// Recompute both the inside and the outside single-site chart for one site
// from scratch.  Returns a pair `(inside, outside)` so callers use the same
// accessors (`.first`, `.second`) the plan's later-phase exit criteria refer
// to.
//
// The `options` argument is forwarded verbatim to both dense builders, so the
// oracle inherits the existing score_ua_edge convention:
//   * `options.score_ua_edge == false` (UA-free inside recurrence, outside root
//     row initialized to zero): use the (grammar, leaf_states, options)
//     overload below.
//   * `options.score_ua_edge == true`: the outside chart requires a reference
//     state, so one of the reference-state overloads must be used.  The bare
//     (grammar, leaf_states, options) overload throws in that case, mirroring
//     `build_single_site_outside_chart`'s own contract — the oracle is never a
//     silent UA-edge-free fallback.
inline std::pair<single_site_chart, single_site_outside_chart>
recompute_both_charts_from_scratch(clade_grammar const& grammar,
                                   leaf_site_states const& leaf_states,
                                   chart_options const& options) {
  auto inside = build_single_site_chart(grammar, leaf_states, options);
  // Forwards the score_ua_edge=true "reference state required" contract.
  auto outside = build_single_site_outside_chart(grammar, inside, options);
  return {std::move(inside), std::move(outside)};
}

// score_ua_edge=true overload: score the UA/reference edge against one
// explicit reference nucleotide state.  Mirrors
// build_single_site_outside_chart(grammar, chart, options, reference_state).
inline std::pair<single_site_chart, single_site_outside_chart>
recompute_both_charts_from_scratch(clade_grammar const& grammar,
                                   leaf_site_states const& leaf_states,
                                   chart_options const& options,
                                   std::uint8_t reference_state) {
  auto inside = build_single_site_chart(grammar, leaf_states, options);
  auto outside = build_single_site_outside_chart(grammar, inside, options,
                                                 reference_state);
  return {std::move(inside), std::move(outside)};
}

// score_ua_edge=true overload: the reference state and leaf states are both
// read from the DAG at position `pos`.  This is the one-call form that mirrors
// the existing extract_leaf_site_states / extract_reference_site_state pair
// and is the natural entry point for per-site loop callers that already hold a
// DAG.  When options.score_ua_edge == false the reference state is unused and
// the UA-free oracle is returned.
inline std::pair<single_site_chart, single_site_outside_chart>
recompute_both_charts_from_scratch(clade_grammar const& grammar, phylo_dag& dag,
                                   mutation_position pos,
                                   chart_options const& options) {
  auto leaf_states = extract_leaf_site_states(dag, grammar, pos);
  if (!options.score_ua_edge) {
    return recompute_both_charts_from_scratch(grammar, leaf_states, options);
  }
  return recompute_both_charts_from_scratch(
      grammar, leaf_states, options, extract_reference_site_state(dag, pos));
}

}  // namespace larch

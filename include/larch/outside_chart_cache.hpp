#pragma once

// WRIC DAG-native SPR & rank-3 rewrite: persistent outside-chart cache on the
// overlay chain (Work item 3, outside half -- Phase 3, highest-risk item).
//
// This header persists outside chart rows keyed by (active pattern,
// overlay-clade-ref) across accepted commits, with incremental recomputation
// scoped to the outside-affected set.  It is the outside dual of Phase 2's
// `inside_chart_cache.hpp`: where the inside chart propagates upward (a commit
// touches clades and their ancestor closure), the outside chart propagates
// downward (a commit changes the outside rows of the touched clades'
// descendants and of the siblings of inside-affected clades).
//
// The outside recurrence is UNCHANGED from `build_single_site_outside_chart`:
// the row for a clade X is the minimum, over every production in the committed
// grammar where X is a child, of
//     outside[parent][parent_state]
//   + min_sibling_state ( inside[sibling][sibling_state]
//                         + transition_cost(parent_state, sibling_state) )
//   + transition_cost(parent_state, child_state)
// minimized over parent_state, with the root clade initialized to 0
// (score_ua_edge=false) or transition_cost(reference_state, child_state)
// (score_ua_edge=true).  `recompute_tip_outside_row` below is a faithful
// gather-form inlining of that recurrence (the dense builder is scatter-form:
// iterate productions_by_parent, write into outside[child]); it owns no new
// math.  The only new logic is (a) the cache store, (b) the affected-set
// scoping, and (c) reading sibling inside rows from Phase 2's inside cache.
//
// Correctness oracle.  Every commit must leave the persisted outside chart
// equal to the outside half of Phase 0's
// `recompute_both_charts_from_scratch` on the materialized chain.  The Phase 3
// test asserts the outside half after every commit (both charts, since an
// inside-only oracle cannot catch outside under-inclusion -- the most likely
// silent bug).
//
// Adoption policy (Work item 3).  The conservative superset (every clade
// reachable from the root) is the DEFAULT affected set for every move class:
// it is trivially correct (a full top-down pass over reachable clades, exactly
// what the dense builder does) and is what `apply_commit_to_outside_cache`
// uses unless a tighter policy is explicitly requested and validated.  The
// three-term tight set (descendants of production-touched parents  union
// siblings of inside-affected clades  union  descendants of those siblings) is
// the TARGET, computed by `compute_chain_outside_affected_set` under the
// `three_term_tight` policy; it is adopted for a move class only after the
// two-chart oracle confirms containment on that class's fixtures.  The
// descendant-closure-only shortcut (term 1 alone, exposed as
// `compute_chain_outside_descendant_closure_only`) is KNOWN UNSOUND in general
// and exists solely so the Phase 3 test can demonstrate that -- it must never
// be the default.

#include <larch/chart_spr.hpp>          // overlay vocabulary
#include <larch/chart_spr_search.hpp>   // active_site_pattern_set,
                                        // chart_spr_weighted_root_score_from_row
#include <larch/chart_trim.hpp>         // single_site_outside_chart recurrence,
                                        // chart_trim_detail::add3
#include <larch/inside_chart_cache.hpp>  // chain_tip_index (reused), inside_chart_cache
#include <larch/overlay_chain.hpp>      // overlay_chain
#include <larch/parsimony_chart.hpp>    // single_site_chart row layout, recurrence helpers

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {

// Policy selecting which outside-affected set `apply_commit_to_outside_cache`
// recomputes.  See the header comment for the adoption policy.
enum class outside_affected_policy {
  // Every clade reachable from the root.  Trivially correct (a full top-down
  // pass); the documented default for all move classes in the initial scope.
  conservative_superset,
  // The three-term tight set: descendants of production-touched parents union
  // siblings of inside-affected clades union descendants of those siblings.
  // The target tight set; adopt for a move class only after the two-chart
  // oracle confirms containment on that class's fixtures.
  three_term_tight,
};

namespace outside_chart_cache_detail {

// Reuse Phase 2's merged-tip index verbatim: a single source of truth for the
// tip grammar's temp-clade/temp-production layout, reachability, and
// production-by-parent/child indices.  The outside cache adds only
// production-by-child traversal (for sibling lookup) and descendant closure
// (for the affected set), both built on top of this index.
using inside_chart_cache_detail::chain_tip_index;
using inside_chart_cache_detail::build_chain_tip_index;
using inside_chart_cache_detail::chain_tip_clade_key;
using inside_chart_cache_detail::chain_tip_clade_size;
using inside_chart_cache_detail::chain_tip_base_production_removed;
using inside_chart_cache_detail::temp_prods_for_parent;

// Whether `ref` is reachable from the root in the committed tip grammar.
// `materialize_overlay_grammar` drops unreachable clades/productions when it
// builds the dense output grammar, so any per-clade recurrence that intends to
// match the dense path (the outside gather here, and the outside-affected-set
// sibling scan) must confine itself to the reachable subgraph.  A production
// whose parent is unreachable is not in the materialized grammar; considering
// it would import a spurious contribution (possibly against a stale finite
// outside row on the unreachable parent) and silently under-count.
inline bool chain_tip_reachable(chain_tip_index const& idx,
                                overlay_clade_ref ref) {
  if (ref.id == no_clade) return false;
  if (ref.space == overlay_id_space::base) {
    return ref.id < idx.reachable_base.size() && idx.reachable_base[ref.id];
  }
  return ref.id < idx.reachable_temp.size() && idx.reachable_temp[ref.id];
}

// Invoke `fn(parent, children)` for every production (base non-tombstoned or
// temp) in the committed tip grammar where `ref` appears as a child. This is the
// outside gather's production enumerator and mirrors the scatter loop in
// `build_single_site_outside_chart` (which iterates productions_by_parent and
// writes into outside[child]). Only productions whose parent is reachable are
// emitted, so the gather matches the dense materialized grammar exactly (see
// `chain_tip_reachable`).
template <typename Fn>
void for_each_tip_production_with_child(chain_tip_index const& idx,
                                        overlay_clade_ref ref, Fn&& fn) {
  auto const& base = *idx.tip_overlay.base;
  if (ref.space == overlay_id_space::base) {
    if (ref.id >= base.productions_by_child.size()) {
      throw std::runtime_error(
          "outside cache: base child ref out of productions_by_child range");
    }
    for (auto pid : base.productions_by_child[ref.id]) {
      if (chain_tip_base_production_removed(idx, pid)) continue;
      if (pid >= base.productions.size()) {
        throw std::runtime_error(
            "outside cache: base production id out of range (by child)");
      }
      auto const& prod = base.productions[pid];
      parsimony_chart_detail::validate_production_inside_row_inputs(
          base, prod, pid, "outside cache");
      auto parent_ref = base_clade_ref(prod.parent);
      if (!chain_tip_reachable(idx, parent_ref)) continue;
      std::vector<overlay_clade_ref> children;
      children.reserve(prod.children.size());
      for (auto child : prod.children) children.push_back(base_clade_ref(child));
      fn(parent_ref, children);
    }
    if (ref.id >= idx.temp_prod_by_base_child.size()) {
      throw std::runtime_error(
          "outside cache: base child ref out of temp-by-base-child range");
    }
    for (auto tpid : idx.temp_prod_by_base_child[ref.id]) {
      if (tpid >= idx.tip_overlay.temp_productions.size()) {
        throw std::runtime_error(
            "outside cache: temp production id out of range (by base child)");
      }
      auto const& prod = idx.tip_overlay.temp_productions[tpid];
      if (!chain_tip_reachable(idx, prod.parent)) continue;
      fn(prod.parent, prod.children);
    }
  } else {
    if (ref.id >= idx.temp_prod_by_temp_child.size()) {
      throw std::runtime_error(
          "outside cache: temp child ref out of temp-by-temp-child range");
    }
    for (auto tpid : idx.temp_prod_by_temp_child[ref.id]) {
      if (tpid >= idx.tip_overlay.temp_productions.size()) {
        throw std::runtime_error(
            "outside cache: temp production id out of range (by temp child)");
      }
      auto const& prod = idx.tip_overlay.temp_productions[tpid];
      if (!chain_tip_reachable(idx, prod.parent)) continue;
      fn(prod.parent, prod.children);
    }
  }
}

// Descendant closure over the committed tip grammar: returns a pair of
// (visited_base, visited_temp) bitsets covering `seed` plus every clade
// reachable from `seed` by following child edges (a clade is a "child" of Y if
// some production names it as a child of parent Y).  This is the propagation
// closure for the outside-affected set: if outside[Y] changes, every child of
// Y (in any production where Y is parent) has outside[parent] changed and so
// changes too.
struct descendant_closure_result {
  std::vector<bool> vis_base;
  std::vector<bool> vis_temp;
};

inline descendant_closure_result compute_descendant_closure(
    chain_tip_index const& idx, std::vector<bool> seed_base,
    std::vector<bool> seed_temp) {
  auto const& base = *idx.tip_overlay.base;
  if (seed_base.size() != base.clades.size()) {
    throw std::runtime_error(
        "outside cache: descendant seed base size mismatch");
  }
  if (seed_temp.size() != idx.temp_clade_count) {
    throw std::runtime_error(
        "outside cache: descendant seed temp size mismatch");
  }
  descendant_closure_result out{std::move(seed_base), std::move(seed_temp)};
  std::vector<overlay_clade_ref> stack;
  for (clade_id c = 0; c < base.clades.size(); ++c) {
    if (out.vis_base[c]) stack.push_back(base_clade_ref(c));
  }
  for (clade_id c = 0; c < idx.temp_clade_count; ++c) {
    if (out.vis_temp[c]) stack.push_back(temp_clade_ref(c));
  }
  auto push_child = [&](overlay_clade_ref child) {
    if (child.id == no_clade) return;
    if (child.space == overlay_id_space::base) {
      if (child.id >= out.vis_base.size()) {
        throw std::runtime_error(
            "outside cache: descendant child base ref out of range");
      }
      if (!out.vis_base[child.id]) {
        out.vis_base[child.id] = true;
        stack.push_back(child);
      }
    } else {
      if (child.id >= out.vis_temp.size()) {
        throw std::runtime_error(
            "outside cache: descendant child temp ref out of range");
      }
      if (!out.vis_temp[child.id]) {
        out.vis_temp[child.id] = true;
        stack.push_back(child);
      }
    }
  };
  while (!stack.empty()) {
    auto ref = stack.back();
    stack.pop_back();
    if (ref.space == overlay_id_space::base) {
      if (ref.id >= base.productions_by_parent.size()) {
        throw std::runtime_error(
            "outside cache: descendant base parent out of range");
      }
      for (auto pid : base.productions_by_parent[ref.id]) {
        if (chain_tip_base_production_removed(idx, pid)) continue;
        if (pid >= base.productions.size()) {
          throw std::runtime_error(
              "outside cache: descendant base production out of range");
        }
        for (auto child : base.productions[pid].children) {
          push_child(base_clade_ref(child));
        }
      }
    }
    for (auto tpid : temp_prods_for_parent(idx, ref)) {
      if (tpid >= idx.tip_overlay.temp_productions.size()) {
        throw std::runtime_error(
            "outside cache: descendant temp production out of range");
      }
      for (auto child : idx.tip_overlay.temp_productions[tpid].children) {
        push_child(child);
      }
    }
  }
  return out;
}

// Collect reachable overlay-clade-refs from a (vis_base, vis_temp) bitset
// pair, filtered to reachable clades and sorted by DECREASING clade size
// (top-down: parents before children).  Ties broken by ref ordering for
// determinism.  This is the order the outside recompute loop consumes.
inline std::vector<overlay_clade_ref> collect_refs_decreasing_size(
    chain_tip_index const& idx, std::vector<bool> const& vis_base,
    std::vector<bool> const& vis_temp) {
  std::vector<overlay_clade_ref> result;
  for (clade_id cid = 0; cid < vis_base.size(); ++cid) {
    if (vis_base[cid] && idx.reachable_base[cid]) {
      result.push_back(base_clade_ref(cid));
    }
  }
  for (clade_id tid = 0; tid < vis_temp.size(); ++tid) {
    if (vis_temp[tid] && idx.reachable_temp[tid]) {
      result.push_back(temp_clade_ref(tid));
    }
  }
  std::stable_sort(result.begin(), result.end(),
                   [&](overlay_clade_ref lhs, overlay_clade_ref rhs) {
                     auto lsize = chain_tip_clade_size(idx, lhs);
                     auto rsize = chain_tip_clade_size(idx, rhs);
                     if (lsize != rsize) return lsize > rsize;  // decreasing
                     return lhs < rhs;
                   });
  return result;
}

}  // namespace outside_chart_cache_detail

// Persistent outside-chart cache keyed by (active pattern, overlay-clade-ref).
// `base_rows[p][cid]` holds the outside row for frozen-base clade `cid` under
// active pattern `p`; `temp_rows[p][tid]` holds the row for merged-temp clade
// `tid`.  On commit, only rows in the outside-affected set are recomputed (in
// top-down / decreasing-size order); every other row is reused.
//
// The outside recurrence reads the sibling's INSIDE row, so commit-time
// recomputation requires the matching `inside_chart_cache` (already updated by
// `apply_commit_to_inside_cache` for the same chain tip).  The cold build is
// self-contained (it builds a fresh inside chart per pattern for the base
// grammar); the incremental path reuses the inside cache.
struct outside_chart_cache {
  clade_grammar const* base = nullptr;
  chart_options chart_opts;
  std::vector<site_pattern> patterns;  // active/topology-informative only
  std::uint64_t invariant_constant_offset = 0;

  // Per-pattern reference state, required when chart_opts.score_ua_edge (the
  // root outside row is transition_cost(reference_state, child_state)).  For a
  // compressed pattern spanning multiple reference states the caller picks the
  // convention; UA-free (the default and the search-state hot convention) does
  // not consult this vector.
  std::vector<std::uint8_t> reference_state_by_pattern;

  std::vector<std::vector<std::array<chart_cost, nuc_state_count>>> base_rows;
  std::vector<std::vector<std::array<chart_cost, nuc_state_count>>> temp_rows;
  std::size_t temp_clade_count = 0;

  // Pairing epoch: the number of outside commits applied to this cache (0 at
  // cold build); equals `chain.size()` after a correctly paired commit.  The
  // outside-commit primitive requires this to be exactly one behind the tip and
  // requires the inside cache's epoch to be at the tip, so a skipped or
  // out-of-order inside/outside commit throws instead of reading stale rows.
  // See `inside_chart_cache::commit_epoch` and the pairing contract on
  // `apply_commit_to_outside_cache`.
  std::size_t commit_epoch = 0;

  // Counter: (pattern, clade) outside-row recomputations across all commits,
  // mirroring inside_chart_cache::inside_rows_recomputed_on_commit so a
  // regression to "recompute everything" is visible in both directions.
  std::size_t outside_rows_recomputed_on_commit = 0;

  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      std::size_t pattern, overlay_clade_ref ref) const {
    if (pattern >= base_rows.size()) {
      throw std::runtime_error(
          "outside cache: pattern index out of range");
    }
    if (ref.space == overlay_id_space::base) {
      if (ref.id == no_clade || ref.id >= base_rows[pattern].size()) {
        throw std::runtime_error(
            "outside cache: base clade row out of range");
      }
      return base_rows[pattern][ref.id];
    }
    if (ref.id == no_clade || ref.id >= temp_rows[pattern].size()) {
      throw std::runtime_error(
          "outside cache: temp clade row out of range");
    }
    return temp_rows[pattern][ref.id];
  }

  // The root outside row for a pattern (the tip root is always the frozen-base
  // root clade: SPR preserves the full taxon set).
  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& root_row(
      std::size_t pattern) const {
    if (base == nullptr || base->root_clade == no_clade) {
      throw std::runtime_error("outside cache: no root clade");
    }
    return row(pattern, base_clade_ref(base->root_clade));
  }
};

namespace outside_chart_cache_detail {

inline std::array<chart_cost, nuc_state_count> recompute_tip_outside_row(
    outside_chart_cache const& ocache, inside_chart_cache const& icache,
    chain_tip_index const& idx, std::size_t pattern, overlay_clade_ref ref) {
  auto const& base = *ocache.base;
  auto root_ref = base_clade_ref(base.root_clade);
  using namespace parsimony_chart_detail;

  auto row = make_inf_row();

  // Root base case.  The root is never a child of any production in a
  // well-formed grammar, so the gather below would leave its row at +inf if
  // this case were skipped; the dense builder sets it the same way before the
  // scatter loop.
  if (ref == root_ref) {
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      row[state] =
          ocache.chart_opts.score_ua_edge
              ? transition_cost(ocache.reference_state_by_pattern[pattern], state)
              : chart_cost{0};
    }
    return row;
  }

  // Gather over every production where `ref` is a child. For each occurrence,
  // the contribution to outside[ref][child_state] is the min over parent_state of
  //   outside[parent][parent_state]
  // + sum over siblings of min_s (inside[sibling][s] + c(parent_state, s))
  // + transition_cost(parent_state, child_state).
  // This is `build_single_site_outside_chart`'s inner loop, gathered.
  auto accumulate = [&](overlay_clade_ref parent_ref,
                        std::vector<overlay_clade_ref> const& children) {
    struct production_view {
      std::vector<overlay_clade_ref> const& children;
    } prod{children};
    auto const& parent_outside = ocache.row(pattern, parent_ref);
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      auto row_provider = [&](overlay_clade_ref child) -> auto const& {
        return icache.row(pattern, child);
      };
      auto child_rows = chart_trim_detail::combine_production_outside_rows(
          prod, parent_state, parent_outside[parent_state], row_provider);
      for (std::size_t ci = 0; ci < children.size(); ++ci) {
        if (!(children[ci] == ref)) continue;
        for (std::uint8_t child_state = 0; child_state < nuc_state_count;
             ++child_state) {
          row[child_state] = std::min(row[child_state],
                                      child_rows[ci][child_state]);
        }
      }
    }
  };

  for_each_tip_production_with_child(idx, ref, accumulate);
  return row;
}

}  // namespace outside_chart_cache_detail

// Build a cold outside cache from the frozen base grammar: every active
// pattern's base outside rows are populated via the dense
// `build_single_site_outside_chart` (the same dense path the trim gate uses),
// and no temp rows exist yet.  The cache stores the active patterns, the
// invariant-site offset, and (for score_ua_edge) the per-pattern reference
// states so commit-time recomputation has everything it needs.
//
// For score_ua_edge=false (the default and the search-state hot convention)
// `reference_state_by_pattern` is ignored and may be empty.  For
// score_ua_edge=true it must be sized to the active pattern count.
inline outside_chart_cache build_outside_chart_cache(
    clade_grammar const& base, active_site_pattern_set const& active,
    chart_options options,
    std::vector<std::uint8_t> reference_state_by_pattern = {}) {
  active.assert_no_skipped_invariant_metadata();
  chart_multisite_detail::validate_multisite_inputs(base, active.patterns,
                                                    options);

  if (options.score_ua_edge) {
    if (reference_state_by_pattern.size() != active.patterns.patterns.size()) {
      throw std::runtime_error(
          "build_outside_chart_cache: score_ua_edge=true requires a reference "
          "state per active pattern");
    }
    for (auto rs : reference_state_by_pattern) {
      parsimony_chart_detail::validate_state(rs, "outside cache reference");
    }
  }

  outside_chart_cache cache;
  cache.base = &base;
  cache.chart_opts = options;
  cache.patterns = active.patterns.patterns;
  cache.reference_state_by_pattern = std::move(reference_state_by_pattern);
  cache.temp_clade_count = 0;

  chart_options build_opts = options;
  build_opts.keep_trace = false;
  build_opts.max_trace_choices = 0;

  cache.base_rows.resize(cache.patterns.size());
  cache.temp_rows.resize(cache.patterns.size());

  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    leaf_site_states states;
    states.state_by_taxon = cache.patterns[p].state_by_taxon;
    auto inside = build_single_site_chart(base, states, build_opts);
    single_site_outside_chart outside;
    if (options.score_ua_edge) {
      outside = build_single_site_outside_chart(base, inside, build_opts,
                                                cache.reference_state_by_pattern[p]);
    } else {
      outside = build_single_site_outside_chart(base, inside, build_opts);
    }
    if (outside.outside.size() != base.clades.size()) {
      throw std::runtime_error(
          "outside cache: base outside chart clade count mismatch");
    }
    cache.base_rows[p].assign(outside.outside.begin(), outside.outside.end());
  }
  return cache;
}

namespace outside_chart_cache_detail {

// Helper to append a reachable-clade superset bitset pair (every reachable
// clade).  Used by the conservative-superset policy.
inline descendant_closure_result reachable_superset(chain_tip_index const& idx) {
  descendant_closure_result out;
  out.vis_base = idx.reachable_base;
  out.vis_temp = idx.reachable_temp;
  return out;
}

// Compute the three-term tight outside-affected set (bitsets) for the chain's
// LAST-appended delta, validated by the two-chart oracle at the call site
// before it is relied upon.  Seeds:
//   * case b -- children of touched productions (an added/removed production
//     gives each of its children a new/lost outside contribution), plus
//   * case a -- siblings of inside-affected clades (inside[sibling] changed,
//     so outside[clade] changes).
// The closure is the descendant closure of the seeds (outside propagation is
// strictly downward: if outside[parent] changes, every child's outside
// changes).  This equals the plan's three-term rule (descendants of
// production-touched parents union siblings of inside-affected clades union
// descendants of those siblings).
inline descendant_closure_result compute_three_term_seeds_and_closure(
    overlay_chain const& chain, chain_tip_index const& idx) {
  auto const& base = *idx.tip_overlay.base;
  if (chain.empty()) {
    throw std::runtime_error(
        "compute_chain_outside_affected_set: empty chain has no delta");
  }
  auto const& last = chain.at(chain.size() - 1);

  // Inside-affected set (Phase 2) drives case (a).
  auto inside_affected_vec =
      inside_chart_cache_detail::compute_inside_affected_set(chain, idx);
  std::vector<bool> inside_aff_base(base.clades.size(), false);
  std::vector<bool> inside_aff_temp(idx.temp_clade_count, false);
  for (auto ref : inside_affected_vec) {
    if (ref.space == overlay_id_space::base) {
      inside_aff_base[ref.id] = true;
    } else {
      inside_aff_temp[ref.id] = true;
    }
  }

  std::vector<bool> seed_base(base.clades.size(), false);
  std::vector<bool> seed_temp(idx.temp_clade_count, false);

  auto mark = [&](overlay_clade_ref ref) {
    if (ref.id == no_clade) return;
    if (ref.space == overlay_id_space::base) {
      if (ref.id >= seed_base.size()) {
        throw std::runtime_error(
            "outside cache: three-term seed base ref out of range");
      }
      seed_base[ref.id] = true;
    } else {
      if (ref.id >= seed_temp.size()) {
        throw std::runtime_error(
            "outside cache: three-term seed temp ref out of range");
      }
      seed_temp[ref.id] = true;
    }
  };

  // Case b: children of touched productions.
  // Tombstoned base productions in the last delta.
  for (auto pid : last.removed_base_productions) {
    if (pid == no_production || pid >= base.productions.size()) {
      throw std::runtime_error(
          "outside cache: last delta tombstone out of range");
    }
    for (auto child : base.productions[pid].children) {
      mark(base_clade_ref(child));
    }
  }
  // Added temp productions in the last delta (merged-absolute child refs).
  for (auto const& prod : last.temp_productions) {
    for (auto child : prod.children) {
      mark(child);
    }
  }

  // Case a: siblings of inside-affected clades.  For each inside-affected
  // clade S, for each production P where S is a child, every other child of P
  // has outside depending on inside[S], which changed.
  auto mark_siblings_of = [&](overlay_clade_ref s) {
    for_each_tip_production_with_child(
        idx, s,
        [&](overlay_clade_ref /*parent*/,
            std::vector<overlay_clade_ref> const& children) {
          bool contains_s = false;
          for (auto child : children) {
            contains_s = contains_s || child == s;
          }
          if (!contains_s) return;
          for (auto child : children) {
            if (!(child == s)) mark(child);
          }
        });
  };
  for (auto ref : inside_affected_vec) {
    mark_siblings_of(ref);
  }

  return compute_descendant_closure(idx, std::move(seed_base),
                                    std::move(seed_temp));
}

// Term-1-alone (descendants of production-touched parents ONLY).  This is the
// KNOWN-UNSOUND shortcut: inside propagation reaches non-production-touched
// ancestors, whose siblings have outside rows that flip while sitting under an
// unchanged parent, so term 1 alone misses them.  Exposed for the Phase 3 test
// that demonstrates the under-inclusion; never the default.
inline descendant_closure_result compute_descendant_closure_only(
    overlay_chain const& chain, chain_tip_index const& idx) {
  auto const& base = *idx.tip_overlay.base;
  if (chain.empty()) {
    throw std::runtime_error(
        "compute_chain_outside_descendant_closure_only: empty chain");
  }
  auto const& last = chain.at(chain.size() - 1);

  std::vector<bool> seed_base(base.clades.size(), false);
  std::vector<bool> seed_temp(idx.temp_clade_count, false);
  auto mark = [&](overlay_clade_ref ref) {
    if (ref.id == no_clade) return;
    if (ref.space == overlay_id_space::base) {
      seed_base[ref.id] = true;
    } else {
      seed_temp[ref.id] = true;
    }
  };
  // Production-touched parents: parents of tombstoned base productions and of
  // added temp productions in the last delta.  These are exactly the
  // inside-affected seed parents (Phase 2 seeds 2 and 3).
  for (auto pid : last.removed_base_productions) {
    if (pid >= base.productions.size()) {
      throw std::runtime_error(
          "outside cache: term1 tombstone out of range");
    }
    mark(base_clade_ref(base.productions[pid].parent));
  }
  for (auto const& prod : last.temp_productions) {
    mark(prod.parent);
  }
  // The parents themselves are inside-affected (their productions changed); to
  // mirror "descendants of production-touched parents" faithfully -- and to
  // give the shortcut its best chance -- include the parents AND their
  // descendants.
  return compute_descendant_closure(idx, std::move(seed_base),
                                    std::move(seed_temp));
}

}  // namespace outside_chart_cache_detail

// The outside-affected set for the chain's last-appended delta, in top-down
// (decreasing-size) order.  Under the default `conservative_superset` policy
// this is every reachable clade (trivially correct); under `three_term_tight`
// it is the target tight set, which a caller adopts only after the two-chart
// oracle confirms containment on the relevant move class.
inline std::vector<overlay_clade_ref> compute_chain_outside_affected_set(
    overlay_chain const& chain,
    outside_affected_policy policy = outside_affected_policy::conservative_superset) {
  if (chain.empty()) {
    throw std::runtime_error(
        "compute_chain_outside_affected_set: empty chain has no delta");
  }
  auto idx = outside_chart_cache_detail::build_chain_tip_index(chain);
  outside_chart_cache_detail::descendant_closure_result bits;
  switch (policy) {
    case outside_affected_policy::conservative_superset:
      bits = outside_chart_cache_detail::reachable_superset(idx);
      break;
    case outside_affected_policy::three_term_tight:
      bits = outside_chart_cache_detail::compute_three_term_seeds_and_closure(
          chain, idx);
      break;
  }
  return outside_chart_cache_detail::collect_refs_decreasing_size(
      idx, bits.vis_base, bits.vis_temp);
}

// Term-1-alone outside-affected set (descendants of production-touched parents
// only).  KNOWN UNSOUND in general; exposed for the Phase 3 under-inclusion
// test, never the default.  See the header comment on the adoption policy.
inline std::vector<overlay_clade_ref>
compute_chain_outside_descendant_closure_only(overlay_chain const& chain) {
  if (chain.empty()) {
    throw std::runtime_error(
        "compute_chain_outside_descendant_closure_only: empty chain");
  }
  auto idx = outside_chart_cache_detail::build_chain_tip_index(chain);
  auto bits = outside_chart_cache_detail::compute_descendant_closure_only(chain,
                                                                          idx);
  return outside_chart_cache_detail::collect_refs_decreasing_size(
      idx, bits.vis_base, bits.vis_temp);
}

// Apply the chain's last-appended delta to the persistent outside cache:
// recompute exactly the outside-affected rows (top-down, per active pattern),
// growing the temp-row store to the new merged temp clade count and bumping
// `outside_rows_recomputed_on_commit`.  Rows outside the affected set are left
// untouched (reused).  `icache` must already reflect the same chain tip
// (`apply_commit_to_inside_cache` must have been called for this delta): the
// outside recurrence reads sibling inside rows from it.
//
// Pairing contract.  This primitive and `apply_commit_to_inside_cache` are a
// paired inside-then-outside step per appended delta, enforced by the caches'
// `commit_epoch` fields.  The outside commit requires the INSIDE cache to be at
// the chain tip (`icache.commit_epoch == chain.size()`) and the OUTSIDE cache
// to be exactly one behind (`commit_epoch == chain.size() - 1`).  This catches
// the previously-unguarded case of a tombstone-only delta (which adds no temp
// clades and so passed the old `temp_clade_count` guard): a caller that forgot
// `apply_commit_to_inside_cache` leaves the inside epoch one behind the tip,
// and this guard throws before any stale inside row is read.
//
// The exact-trim invalidation hook is owned by the inside commit primitive
// (Phase 2); the outside commit does not reset it a second time.  The two
// commits are paired: inside-then-outside per accepted delta.
inline void apply_commit_to_outside_cache(
    overlay_chain const& chain, outside_chart_cache& cache,
    inside_chart_cache const& icache,
    outside_affected_policy policy = outside_affected_policy::conservative_superset) {
  if (chain.empty()) {
    throw std::runtime_error(
        "apply_commit_to_outside_cache: empty chain has no delta");
  }
  if (cache.base == nullptr) {
    throw std::runtime_error(
        "apply_commit_to_outside_cache: cache has no base grammar");
  }
  if (cache.base != &chain.base()) {
    throw std::runtime_error(
        "apply_commit_to_outside_cache: cache base does not match chain base");
  }
  if (icache.base != &chain.base()) {
    throw std::runtime_error(
        "apply_commit_to_outside_cache: inside cache base does not match "
        "chain base");
  }

  // Pairing guard (Issue: weakly-guarded commit pairing).  chain.size() >= 1
  // here (empty-chain check above), so `chain.size() - 1` is well-defined.
  // (a) The inside cache must already reflect this chain tip -- the outside
  //     recurrence reads sibling inside rows, so a stale inside cache (e.g. a
  //     caller that appended a delta and skipped apply_commit_to_inside_cache)
  //     would silently read wrong rows.  The epoch catches this for EVERY
  //     delta, including tombstone-only deltas that add no temp clades.
  // (b) This outside cache must be exactly one commit behind the tip -- a
  //     doubled or out-of-order outside commit throws here.
  if (icache.commit_epoch != chain.size()) {
    throw std::runtime_error(
        "apply_commit_to_outside_cache: inside cache commit_epoch (" +
        std::to_string(icache.commit_epoch) +
        ") does not reflect the chain tip (" + std::to_string(chain.size()) +
        "); apply_commit_to_inside_cache must precede apply_commit_to_outside_"
        "cache for every appended delta");
  }
  if (cache.commit_epoch + 1 != chain.size()) {
    throw std::runtime_error(
        "apply_commit_to_outside_cache: outside cache commit_epoch (" +
        std::to_string(cache.commit_epoch) +
        ") is not exactly one behind the chain tip (" +
        std::to_string(chain.size()) +
        "); inside/outside commits must be paired one-per-appended delta");
  }

  auto idx = outside_chart_cache_detail::build_chain_tip_index(chain);

  // Defense-in-depth: when the inside epoch reflects the tip, the inside temp
  // store must be sized to the tip's merged temp space too.  This is implied by
  // the epoch guard above (a current inside cache has a current temp store); it
  // is kept as a cheap structural assertion so a logic error in the inside
  // primitive surfaces here rather than as an out-of-bounds read below.
  if (icache.temp_clade_count != idx.temp_clade_count) {
    throw std::runtime_error(
        "apply_commit_to_outside_cache: inside cache temp_clade_count does not "
        "match the tip (epoch guard passed; temp store is inconsistent)");
  }

  auto affected = compute_chain_outside_affected_set(chain, policy);

  // Grow the per-pattern temp-row store to the new merged temp clade count.
  // New temp slots are filled during the recompute loop (every new temp clade
  // is in the conservative superset, and the three-term set's descendant
  // closure of children-of-touched-productions covers new temp children too);
  // existing temp rows outside the affected set are reused.
  if (idx.temp_clade_count != cache.temp_clade_count) {
    for (std::size_t p = 0; p < cache.temp_rows.size(); ++p) {
      cache.temp_rows[p].resize(
          idx.temp_clade_count, parsimony_chart_detail::make_inf_row());
    }
    cache.temp_clade_count = idx.temp_clade_count;
  }

  // Recompute affected rows top-down (decreasing size).  Every parent a
  // production reads is larger than the clade being recomputed, so it is
  // either earlier in this pass (if affected) or still correct from before
  // (if unaffected, because the affected set is a superset of changed rows).
  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    for (auto ref : affected) {
      auto fresh = outside_chart_cache_detail::recompute_tip_outside_row(
          cache, icache, idx, p, ref);
      if (ref.space == overlay_id_space::base) {
        cache.base_rows[p][ref.id] = fresh;
      } else {
        cache.temp_rows[p][ref.id] = fresh;
      }
    }
  }

  cache.outside_rows_recomputed_on_commit +=
      affected.size() * cache.patterns.size();

  // Advance the pairing epoch to the tip (the post-state the next paired
  // outside-commit guard requires).  Bumped last so the epoch reflects a
  // completed commit.
  cache.commit_epoch = chain.size();
}

// Single-site global optimum from the cached root rows:
//   global_min = min_state ( inside[root][state] + outside[root][state] ).
// Equals the from-scratch `single_site_outside_chart::global_min` after every
// commit (asserted in the Phase 3 test).  For score_ua_edge=false this is
// min_s inside[root][s]; for score_ua_edge=true it is the UA-edge-weighted root
// optimum for the pattern's reference state.
inline chart_cost outside_cache_global_min(outside_chart_cache const& ocache,
                                           inside_chart_cache const& icache,
                                           std::size_t pattern) {
  auto const& inside_root = icache.root_row(pattern);
  auto const& outside_root = ocache.root_row(pattern);
  chart_cost best = chart_inf;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    best = std::min(best, parsimony_chart_detail::saturated_add(
                              inside_root[state], outside_root[state]));
  }
  return best;
}

}  // namespace larch

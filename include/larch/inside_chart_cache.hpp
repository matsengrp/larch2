#pragma once

// WRIC DAG-native SPR & rank-3 rewrite: persistent inside-chart cache on the
// overlay chain (Work item 3, inside half -- Phase 2).
//
// This header persists inside chart rows keyed by (active pattern,
// overlay-clade-ref) across accepted commits, with incremental recomputation
// scoped to the inside-affected set (touched clades plus their ancestor
// closure in the committed grammar, bottom-up).  The outside chart still
// full-rebuilds per commit in this phase -- that is the documented
// conservative superset for the outside direction, and its incremental form
// is Phase 3.
//
// The inside recurrence is UNCHANGED from `build_single_site_chart`: the row
// for a clade X is the minimum, over the productions available at X in the
// committed grammar, of the binary combine of the children's rows plus the
// parent/child transition costs.  `recompute_tip_inside_row` below is a
// faithful inlining of that recurrence (and of
// `chart_spr_search_detail::recompute_overlay_delta_row`); it owns no new
// math.  The only new logic is (a) the cache store and (b) the affected-set
// scoping that decides which rows to recompute on commit.
//
// Correctness oracle.  Every commit must leave the persisted inside chart
// equal to the inside half of Phase 0's
// `recompute_both_charts_from_scratch` on the materialized chain.  The
// outside half is rebuilt from scratch here (correct, slow); its incremental
// form lands in Phase 3.  The Phase 2 test asserts the inside half after
// every commit, for commit sequences of length 1, 2, and long enough to
// exercise multi-level ancestor recomputation.
//
// Exact-trim invalidation.  `apply_commit_to_inside_cache` resets the lazy
// exact-trim cache (`exact_trim_active_only`) on every commit, matching WI3's
// lazy-invalidation rule: the exact-trim cache is recomputed by the next
// exact gate, never eagerly.  The hook lives on the commit primitive so
// Phase 3's `apply_commit_to_outside_cache` inherits the same rule without a
// second reset.

#include <larch/chart_spr.hpp>          // overlay vocabulary, materialize_overlay_grammar
#include <larch/chart_spr_search.hpp>   // spr_overlay_delta, active_site_pattern_set,
                                        // chart_spr_weighted_root_score_from_row
#include <larch/chart_trim.hpp>         // multisite_trim_result, chart_multisite_detail
#include <larch/overlay_chain.hpp>      // overlay_chain
#include <larch/parsimony_chart.hpp>    // single_site_chart, inside recurrence helpers

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {

namespace inside_chart_cache_detail {

// Index of the committed ("tip") grammar in overlay space.  Built once per
// commit from `overlay_chain::tip()` (cheap: concatenation + dedup, no dense
// materialization) and shared by the affected-set computation and the row
// recompute.  It owns the same kind of temp-production indices as
// `spr_overlay_delta` (`temp_productions_by_base_parent` etc.), but over the
// MERGED temp space rather than a single delta's local temp space.
struct chain_tip_index {
  overlay_clade_grammar tip_overlay;
  std::size_t temp_clade_count = 0;

  std::set<production_id> tombstones;  // removed base production ids

  std::vector<std::vector<std::size_t>> temp_prod_by_base_parent;
  std::vector<std::vector<std::size_t>> temp_prod_by_temp_parent;
  std::vector<std::vector<std::size_t>> temp_prod_by_base_child;
  std::vector<std::vector<std::size_t>> temp_prod_by_temp_child;

  std::vector<bool> reachable_base;
  std::vector<bool> reachable_temp;
};

inline clade_key const& chain_tip_clade_key(chain_tip_index const& idx,
                                            overlay_clade_ref ref) {
  auto const& base = *idx.tip_overlay.base;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= base.clades.size()) {
      throw std::runtime_error(
          "inside cache: tip base clade ref out of range");
    }
    return base.clades[ref.id];
  }
  if (ref.id == no_clade || ref.id >= idx.tip_overlay.temp_clades.size()) {
    throw std::runtime_error(
        "inside cache: tip temp clade ref out of range");
  }
  return idx.tip_overlay.temp_clades[ref.id];
}

inline std::size_t chain_tip_clade_size(chain_tip_index const& idx,
                                        overlay_clade_ref ref) {
  return chain_tip_clade_key(idx, ref).taxa.size();
}

inline std::vector<std::size_t> const& temp_prods_for_parent(
    chain_tip_index const& idx, overlay_clade_ref parent) {
  if (parent.space == overlay_id_space::base) {
    if (parent.id >= idx.temp_prod_by_base_parent.size()) {
      throw std::runtime_error(
          "inside cache: tip base parent out of temp index range");
    }
    return idx.temp_prod_by_base_parent[parent.id];
  }
  if (parent.id >= idx.temp_prod_by_temp_parent.size()) {
    throw std::runtime_error(
        "inside cache: tip temp parent out of temp index range");
  }
  return idx.temp_prod_by_temp_parent[parent.id];
}

inline bool chain_tip_base_production_removed(chain_tip_index const& idx,
                                              production_id pid) {
  return idx.tombstones.count(pid) != 0;
}

inline chain_tip_index build_chain_tip_index(overlay_chain const& chain) {
  chain_tip_index idx;
  idx.tip_overlay = chain.tip();
  idx.temp_clade_count = idx.tip_overlay.temp_clades.size();
  auto const& base = chain.base();

  for (auto pid : idx.tip_overlay.removed_base_productions) {
    if (pid == no_production || pid >= base.productions.size()) {
      throw std::runtime_error(
          "inside cache: tip tombstone production out of range");
    }
    idx.tombstones.insert(pid);
  }

  idx.temp_prod_by_base_parent.assign(base.clades.size(), {});
  idx.temp_prod_by_temp_parent.assign(idx.temp_clade_count, {});
  idx.temp_prod_by_base_child.assign(base.clades.size(), {});
  idx.temp_prod_by_temp_child.assign(idx.temp_clade_count, {});

  auto append_parent = [&](overlay_clade_ref ref, std::size_t tpid) {
    if (ref.space == overlay_id_space::base) {
      if (ref.id >= idx.temp_prod_by_base_parent.size()) {
        throw std::runtime_error(
            "inside cache: tip temp production base parent out of range");
      }
      idx.temp_prod_by_base_parent[ref.id].push_back(tpid);
    } else {
      if (ref.id >= idx.temp_prod_by_temp_parent.size()) {
        throw std::runtime_error(
            "inside cache: tip temp production temp parent out of range");
      }
      idx.temp_prod_by_temp_parent[ref.id].push_back(tpid);
    }
  };
  auto append_child = [&](overlay_clade_ref ref, std::size_t tpid) {
    if (ref.space == overlay_id_space::base) {
      if (ref.id >= idx.temp_prod_by_base_child.size()) {
        throw std::runtime_error(
            "inside cache: tip temp production base child out of range");
      }
      idx.temp_prod_by_base_child[ref.id].push_back(tpid);
    } else {
      if (ref.id >= idx.temp_prod_by_temp_child.size()) {
        throw std::runtime_error(
            "inside cache: tip temp production temp child out of range");
      }
      idx.temp_prod_by_temp_child[ref.id].push_back(tpid);
    }
  };

  for (std::size_t i = 0; i < idx.tip_overlay.temp_productions.size(); ++i) {
    auto tpid = i;
    auto const& prod = idx.tip_overlay.temp_productions[i];
    if (prod.children.size() != 2) {
      throw std::runtime_error(
          "inside cache: tip temp production " + std::to_string(tpid) +
          " has arity " + std::to_string(prod.children.size()) +
          "; Phase 2 supports binary productions only");
    }
    append_parent(prod.parent, tpid);
    // Dedup children before indexing (a production may list the same clade
    // twice, e.g. a symmetric split); matches the single-delta substrate.
    auto children = prod.children;
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()),
                   children.end());
    for (auto child : children) {
      append_child(child, tpid);
    }
  }

  // Reachability from the tip root (== frozen base root: SPR preserves the
  // full taxon set).  Mirrors `compute_overlay_delta_reachability` over the
  // merged space.
  idx.reachable_base.assign(base.clades.size(), false);
  idx.reachable_temp.assign(idx.temp_clade_count, false);
  std::vector<overlay_clade_ref> stack{base_clade_ref(base.root_clade)};
  while (!stack.empty()) {
    auto ref = stack.back();
    stack.pop_back();
    bool newly = false;
    if (ref.space == overlay_id_space::base) {
      if (ref.id >= base.clades.size()) {
        throw std::runtime_error("inside cache: reachability base ref oor");
      }
      newly = !idx.reachable_base[ref.id];
      idx.reachable_base[ref.id] = true;
    } else {
      if (ref.id >= idx.temp_clade_count) {
        throw std::runtime_error("inside cache: reachability temp ref oor");
      }
      newly = !idx.reachable_temp[ref.id];
      idx.reachable_temp[ref.id] = true;
    }
    if (!newly) continue;

    if (ref.space == overlay_id_space::base) {
      for (auto pid : base.productions_by_parent[ref.id]) {
        if (chain_tip_base_production_removed(idx, pid)) continue;
        if (pid >= base.productions.size()) {
          throw std::runtime_error(
              "inside cache: reachability base production oor");
        }
        for (auto child : base.productions[pid].children) {
          stack.push_back(base_clade_ref(child));
        }
      }
    }
    for (auto tpid : temp_prods_for_parent(idx, ref)) {
      for (auto child : idx.tip_overlay.temp_productions[tpid].children) {
        stack.push_back(child);
      }
    }
  }

  return idx;
}

// Compute the inside-affected set for the chain's LAST-appended delta against
// the committed (tip) grammar: the touched clades (this delta's new temp
// clades, parents of its tombstoned base productions, parents of its added
// temp productions) plus their ancestor closure via the tip grammar's
// child->parent links, filtered to reachable clades, sorted bottom-up.
//
// This is the same algorithm as `compute_overlay_delta_affected_order`, but
// seeded from the last delta's rebased (merged-absolute) contributions and
// propagated through the MERGED tip grammar instead of a single delta's local
// grammar.  For a single-delta chain the two grammars coincide, so this set
// equals `spr_overlay_delta::affected_order` modulo the candidate-only
// conservative seeds (`candidate->old_parent`, which is always already covered
// by the tombstone-parent seed, and `candidate->new_sibling_or_target`, which
// is a clade whose own inside row does not change -- see
// `assert_affected_set_relation` in the Phase 2 test).  The stored chain delta
// carries no candidate pointer, so those terms are naturally absent here.
inline std::vector<overlay_clade_ref> compute_inside_affected_set(
    overlay_chain const& chain, chain_tip_index const& idx) {
  if (chain.empty()) {
    throw std::runtime_error(
        "compute_chain_inside_affected_set: empty chain has no delta");
  }
  auto const& base = chain.base();
  auto const& last = chain.at(chain.size() - 1);

  std::vector<bool> aff_base(base.clades.size(), false);
  std::vector<bool> aff_temp(idx.temp_clade_count, false);
  std::vector<overlay_clade_ref> queue;

  auto mark = [&](overlay_clade_ref ref) {
    if (ref.id == no_clade) return;
    if (ref.space == overlay_id_space::base) {
      if (ref.id >= base.clades.size()) {
        throw std::runtime_error(
            "inside cache: affected base ref out of range");
      }
      if (!aff_base[ref.id]) {
        aff_base[ref.id] = true;
        queue.push_back(ref);
      }
    } else {
      if (ref.id >= idx.temp_clade_count) {
        throw std::runtime_error(
            "inside cache: affected temp ref out of range");
      }
      if (!aff_temp[ref.id]) {
        aff_temp[ref.id] = true;
        queue.push_back(ref);
      }
    }
  };

  // Seed 1: this delta's genuinely-new temp clades.  Append assigns merged ids
  // contiguously, so the last delta's new clades occupy the highest merged ids
  // [temp_before, temp_clade_count).  (See overlay_chain::append /
  // build_merged_clade_index: each stored delta only records clades that got
  // fresh merged ids.)
  std::size_t last_temp_count = last.temp_clades.size();
  if (last_temp_count > idx.temp_clade_count) {
    throw std::runtime_error(
        "inside cache: last delta temp clade count exceeds merged total");
  }
  std::size_t temp_before = idx.temp_clade_count - last_temp_count;
  for (std::size_t tid = temp_before; tid < idx.temp_clade_count; ++tid) {
    mark(temp_clade_ref(static_cast<clade_id>(tid)));
  }

  // Seed 2: parents of base productions tombstoned by this delta.
  for (auto pid : last.removed_base_productions) {
    if (pid == no_production || pid >= base.productions.size()) {
      throw std::runtime_error(
          "inside cache: last delta tombstone out of range");
    }
    mark(base_clade_ref(base.productions[pid].parent));
  }

  // Seed 3: parents of temp productions added by this delta (merged-absolute
  // refs, as stored by overlay_chain::append).
  for (auto const& prod : last.temp_productions) {
    mark(prod.parent);
  }

  // Propagate upward: a clade whose row changed forces every production that
  // uses it as a child to be re-scored, so those productions' parents are
  // affected too.
  for (std::size_t head = 0; head < queue.size(); ++head) {
    auto child = queue[head];
    if (child.space == overlay_id_space::base) {
      for (auto pid : base.productions_by_child[child.id]) {
        if (chain_tip_base_production_removed(idx, pid)) continue;
        if (pid >= base.productions.size()) {
          throw std::runtime_error(
              "inside cache: propagation base production oor");
        }
        mark(base_clade_ref(base.productions[pid].parent));
      }
      for (auto tpid : idx.temp_prod_by_base_child[child.id]) {
        mark(idx.tip_overlay.temp_productions[tpid].parent);
      }
    } else {
      for (auto tpid : idx.temp_prod_by_temp_child[child.id]) {
        mark(idx.tip_overlay.temp_productions[tpid].parent);
      }
    }
  }

  std::vector<overlay_clade_ref> result;
  for (clade_id cid = 0; cid < base.clades.size(); ++cid) {
    if (aff_base[cid] && idx.reachable_base[cid]) {
      result.push_back(base_clade_ref(cid));
    }
  }
  for (clade_id tid = 0; tid < idx.temp_clade_count; ++tid) {
    if (aff_temp[tid] && idx.reachable_temp[tid]) {
      result.push_back(temp_clade_ref(tid));
    }
  }

  std::stable_sort(result.begin(), result.end(),
                   [&](overlay_clade_ref lhs, overlay_clade_ref rhs) {
                     auto lsize = chain_tip_clade_size(idx, lhs);
                     auto rsize = chain_tip_clade_size(idx, rhs);
                     if (lsize != rsize) return lsize < rsize;
                     return lhs < rhs;
                   });
  return result;
}

}  // namespace inside_chart_cache_detail

// Persistent inside-chart cache keyed by (active pattern, overlay-clade-ref).
// `base_rows[p][cid]` holds the inside row for frozen-base clade `cid` under
// active pattern `p`; `temp_rows[p][tid]` holds the row for merged-temp clade
// `tid`.  On commit, only rows in the inside-affected set are recomputed (in
// bottom-up order); every other row is reused.
//
// The cache is built cold from the frozen base (all base rows populated, no
// temp rows) and updated incrementally by `apply_commit_to_inside_cache` as
// deltas are appended to the chain.
struct inside_chart_cache {
  clade_grammar const* base = nullptr;
  chart_options chart_opts;
  std::vector<site_pattern> patterns;  // active/topology-informative only
  std::uint64_t invariant_constant_offset = 0;

  std::vector<std::vector<std::array<chart_cost, nuc_state_count>>> base_rows;
  std::vector<std::vector<std::array<chart_cost, nuc_state_count>>> temp_rows;
  std::size_t temp_clade_count = 0;

  // Lazy exact-trim cache over the current tip grammar (active-only, like
  // chart_spr_search_state::exact_trim_active_only).  Reset to absent on every
  // commit by `apply_commit_to_inside_cache`; recomputed lazily by the next
  // exact gate (Phase 4).  Phase 3's outside-commit inherits the reset without
  // a second invalidation.
  std::optional<multisite_trim_result> exact_trim_active_only;

  // Counters.  `inside_rows_recomputed_on_commit` counts (pattern, clade) row
  // recomputations across all commits; the Phase 3 counterpart
  // `outside_rows_recomputed_on_commit` lands there.
  std::size_t inside_rows_recomputed_on_commit = 0;

  // Read a cached inside row.  Throws on out-of-range or absent rows; callers
  // (the recompute loop, the root scorer) only read rows that are present.
  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& row(
      std::size_t pattern, overlay_clade_ref ref) const {
    if (pattern >= base_rows.size()) {
      throw std::runtime_error("inside cache: pattern index out of range");
    }
    if (ref.space == overlay_id_space::base) {
      if (ref.id == no_clade || ref.id >= base_rows[pattern].size()) {
        throw std::runtime_error(
            "inside cache: base clade row out of range");
      }
      return base_rows[pattern][ref.id];
    }
    if (ref.id == no_clade || ref.id >= temp_rows[pattern].size()) {
      throw std::runtime_error(
            "inside cache: temp clade row out of range");
    }
    return temp_rows[pattern][ref.id];
  }

  // The root inside row for a pattern (the tip root is always the frozen-base
  // root clade: SPR preserves the full taxon set).
  [[nodiscard]] std::array<chart_cost, nuc_state_count> const& root_row(
      std::size_t pattern) const {
    if (base == nullptr || base->root_clade == no_clade) {
      throw std::runtime_error("inside cache: no root clade");
    }
    return row(pattern, base_clade_ref(base->root_clade));
  }
};

// Build a cold inside cache from the frozen base grammar: every active
// pattern's base rows are populated via `build_single_site_chart` (the same
// dense path the search state uses), and no temp rows exist yet.  The cache
// stores the active patterns and the invariant-site offset so commit-time
// recomputation and the composite-lower-bound helper have everything they
// need.
inline inside_chart_cache build_inside_chart_cache(
    clade_grammar const& base, active_site_pattern_set const& active,
    chart_options options, std::uint64_t invariant_constant_offset) {
  active.assert_no_skipped_invariant_metadata();
  chart_spr_search_detail::validate_binary_chart_compatible_grammar(base);
  chart_multisite_detail::validate_multisite_inputs(
      base, active.patterns, options);

  inside_chart_cache cache;
  cache.base = &base;
  cache.chart_opts = options;
  cache.patterns = active.patterns.patterns;
  cache.invariant_constant_offset = invariant_constant_offset;
  cache.temp_clade_count = 0;

  chart_options build_opts = options;
  build_opts.keep_trace = false;
  build_opts.max_trace_choices = 0;

  cache.base_rows.resize(cache.patterns.size());
  cache.temp_rows.resize(cache.patterns.size());

  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    leaf_site_states states;
    states.state_by_taxon = cache.patterns[p].state_by_taxon;
    auto chart = build_single_site_chart(base, states, build_opts);
    if (chart.inside.size() != base.clades.size()) {
      throw std::runtime_error(
          "inside cache: base chart clade count mismatch");
    }
    // Each inner vector starts empty (resize above), so assign allocates it
    // exactly once rather than zero-initializing then reallocating.
    cache.base_rows[p].assign(chart.inside.begin(), chart.inside.end());
  }
  return cache;
}

namespace inside_chart_cache_detail {

// Recompute the inside row for a single clade of the committed (tip) grammar,
// reading children rows from `cache`.  This is the standard inside recurrence
// (identical to `build_single_site_chart`'s loop body and to
// `chart_spr_search_detail::recompute_overlay_delta_row`); it owns no new
// math.  Available productions at `ref` are the frozen-base productions not
// tombstoned by any chain delta, plus the merged temp productions with
// `ref` as parent.
inline std::array<chart_cost, nuc_state_count> recompute_tip_inside_row(
    inside_chart_cache const& cache, chain_tip_index const& idx,
    std::size_t pattern, overlay_clade_ref ref) {
  auto const& base = *cache.base;
  auto const& key = chain_tip_clade_key(idx, ref);

  if (key.taxa.size() == 1) {
    auto taxon = key.taxa.front();
    if (taxon >= cache.patterns[pattern].state_by_taxon.size()) {
      throw std::runtime_error(
          "inside cache: leaf taxon out of state range");
    }
    auto observed = cache.patterns[pattern].state_by_taxon[taxon];
    parsimony_chart_detail::validate_state(observed,
                                           "inside cache leaf state");
    auto row = parsimony_chart_detail::make_inf_row();
    row[observed] = 0;
    return row;
  }

  auto row = parsimony_chart_detail::make_inf_row();

  // Accumulate one binary production's contribution into `row`, reading the
  // two children rows from the cache.  Matches
  // `accumulate_overlay_production_row` / build_single_site_chart exactly.
  auto accumulate_binary = [&](overlay_clade_ref c0, overlay_clade_ref c1) {
    if (c0.id == no_clade || c1.id == no_clade) {
      throw std::runtime_error(
          "inside cache: production child ref is no_clade");
    }
    auto const& left = cache.row(pattern, c0);
    auto const& right = cache.row(pattern, c1);
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      chart_cost best[2] = {chart_inf, chart_inf};
      std::array<chart_cost, nuc_state_count> const* children[2] = {&left,
                                                                    &right};
      for (std::size_t ci = 0; ci < 2; ++ci) {
        for (std::uint8_t child_state = 0; child_state < nuc_state_count;
             ++child_state) {
          best[ci] = std::min(
              best[ci],
              parsimony_chart_detail::saturated_add(
                  (*children[ci])[child_state],
                  parsimony_chart_detail::transition_cost(parent_state,
                                                          child_state)));
        }
      }
      auto total =
          parsimony_chart_detail::saturated_add(best[0], best[1]);
      if (total < row[parent_state]) row[parent_state] = total;
    }
  };

  bool saw_production = false;
  if (ref.space == overlay_id_space::base) {
    for (auto pid : base.productions_by_parent[ref.id]) {
      if (chain_tip_base_production_removed(idx, pid)) continue;
      if (pid >= base.productions.size()) {
        throw std::runtime_error(
            "inside cache: base production out of range");
      }
      auto const& prod = base.productions[pid];
      if (prod.children.size() != 2) {
        throw std::runtime_error(
            "inside cache: base production arity != 2; binary chart required");
      }
      accumulate_binary(base_clade_ref(prod.children[0]),
                        base_clade_ref(prod.children[1]));
      saw_production = true;
    }
  }
  for (auto tpid : temp_prods_for_parent(idx, ref)) {
    auto const& prod = idx.tip_overlay.temp_productions[tpid];
    // build_chain_tip_index already asserted arity 2.
    accumulate_binary(prod.children[0], prod.children[1]);
    saw_production = true;
  }
  if (!saw_production) {
    throw std::runtime_error(
        "inside cache: non-singleton clade has no available productions");
  }
  return row;
}

}  // namespace inside_chart_cache_detail

// The inside-affected set for the chain's last-appended delta (touched clades
// plus ancestor closure in the committed grammar, bottom-up).  For a
// single-delta chain this is `spr_overlay_delta::affected_order` minus the
// candidate-only conservative seeds (see the detail above).
inline std::vector<overlay_clade_ref> compute_chain_inside_affected_set(
    overlay_chain const& chain) {
  auto idx = inside_chart_cache_detail::build_chain_tip_index(chain);
  return inside_chart_cache_detail::compute_inside_affected_set(chain, idx);
}

// Apply the chain's last-appended delta to the persistent inside cache:
// recompute exactly the inside-affected rows (bottom-up, per active pattern),
// grow the temp-row store to the new merged temp clade count, reset the lazy
// exact-trim cache, and bump `inside_rows_recomputed_on_commit` by the number
// of (pattern, clade) rows recomputed.  Rows outside the affected set are left
// untouched (reused).
inline void apply_commit_to_inside_cache(overlay_chain const& chain,
                                         inside_chart_cache& cache) {
  if (chain.empty()) {
    throw std::runtime_error(
        "apply_commit_to_inside_cache: empty chain has no delta");
  }
  if (cache.base == nullptr) {
    throw std::runtime_error(
        "apply_commit_to_inside_cache: cache has no base grammar");
  }
  if (cache.base != &chain.base()) {
    throw std::runtime_error(
        "apply_commit_to_inside_cache: cache base does not match chain base");
  }

  auto idx = inside_chart_cache_detail::build_chain_tip_index(chain);
  auto affected =
      inside_chart_cache_detail::compute_inside_affected_set(chain, idx);

  // Grow the per-pattern temp-row store to the new merged temp clade count.
  // New temp slots are filled during the recompute loop below (every new temp
  // clade is in the affected set); existing temp rows outside the affected set
  // are reused verbatim.
  if (idx.temp_clade_count != cache.temp_clade_count) {
    for (std::size_t p = 0; p < cache.temp_rows.size(); ++p) {
      cache.temp_rows[p].resize(
          idx.temp_clade_count,
          parsimony_chart_detail::make_inf_row());
    }
    cache.temp_clade_count = idx.temp_clade_count;
  }

  // Recompute affected rows bottom-up.  `affected` is sorted by clade size, so
  // every child row a production reads is either (a) a smaller affected clade
  // already recomputed earlier in this loop, or (b) an unaffected clade whose
  // cached row is still valid for the new tip.
  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    for (auto ref : affected) {
      auto fresh = inside_chart_cache_detail::recompute_tip_inside_row(
          cache, idx, p, ref);
      if (ref.space == overlay_id_space::base) {
        cache.base_rows[p][ref.id] = fresh;
      } else {
        cache.temp_rows[p][ref.id] = fresh;
      }
    }
  }

  cache.inside_rows_recomputed_on_commit +=
      affected.size() * cache.patterns.size();

  // Lazy-invalidation hook (WI3): the exact-trim cache is recomputed by the
  // next exact gate, never eagerly.  Phase 3's outside commit inherits this
  // reset (it calls this primitive or shares the rule).
  cache.exact_trim_active_only.reset();
}

// Composite lower bound (active + invariant offset) from the cached root rows,
// scored through `chart_spr_weighted_root_score_from_row` so
// `score_ua_edge = true` compressed patterns are handled identically to the
// current search-state cache.  Equals the from-scratch
// `build_composite_chart_score(...).weighted_lower_bound` plus the invariant
// offset after every commit (asserted in the Phase 2 test).
inline std::uint64_t inside_cache_composite_lower_bound_with_invariants(
    inside_chart_cache const& cache) {
  if (cache.base == nullptr) {
    throw std::runtime_error(
        "inside cache composite lower bound: no base grammar");
  }
  std::uint64_t total = 0;
  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    auto contribution = chart_spr_weighted_root_score_from_row(
        cache.root_row(p), cache.patterns[p], cache.chart_opts);
    total = chart_multisite_detail::checked_add_u64(
        total, contribution, "inside cache composite lower bound");
  }
  total = chart_multisite_detail::checked_add_u64(
      total, cache.invariant_constant_offset,
      "inside cache composite lower bound + invariant offset");
  return total;
}

}  // namespace larch

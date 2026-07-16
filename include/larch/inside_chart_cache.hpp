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
// committed grammar, of the k-ary combine of the children's rows plus the
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
#include <larch/chart_scheduler.hpp>    // persistent pattern-axis scheduling
#include <larch/chart_spr_search.hpp>   // spr_overlay_delta, active_site_pattern_set,
                                        // chart_spr_weighted_root_score_from_row
#include <larch/chart_trim.hpp>         // multisite_trim_result, chart_multisite_detail
#include <larch/overlay_chain.hpp>      // overlay_chain
#include <larch/parsimony_chart.hpp>    // single_site_chart, inside recurrence helpers

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
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

inline void validate_tip_overlay_production_partition(
    chain_tip_index const& idx, overlay_grammar_production const& prod,
    std::size_t tpid, std::string const& context) {
  if (prod.children.size() < 2) {
    throw std::runtime_error(context + ": tip temp production " +
                             std::to_string(tpid) + " has arity " +
                             std::to_string(prod.children.size()) +
                             "; expected at least 2 children");
  }
  auto const& parent_taxa = chain_tip_clade_key(idx, prod.parent).taxa;
  std::vector<taxon_id> covered;
  for (auto child : prod.children) {
    auto const& child_taxa = chain_tip_clade_key(idx, child).taxa;
    if (child_taxa.size() >= parent_taxa.size()) {
      throw std::runtime_error(context +
                               ": tip temp production child is not smaller "
                               "than parent");
    }
    if (!std::includes(parent_taxa.begin(), parent_taxa.end(),
                       child_taxa.begin(), child_taxa.end())) {
      throw std::runtime_error(context +
                               ": tip temp production child is not a subset "
                               "of parent");
    }

    std::vector<taxon_id> overlap;
    std::set_intersection(covered.begin(), covered.end(),
                          child_taxa.begin(), child_taxa.end(),
                          std::back_inserter(overlap));
    if (!overlap.empty()) {
      throw std::runtime_error(context +
                               ": tip temp production children overlap");
    }

    std::vector<taxon_id> next;
    std::set_union(covered.begin(), covered.end(), child_taxa.begin(),
                   child_taxa.end(), std::back_inserter(next));
    covered = std::move(next);
  }
  if (covered != parent_taxa) {
    throw std::runtime_error(context +
                             ": tip temp production children do not union to "
                             "the parent clade");
  }
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
    validate_tip_overlay_production_partition(idx, prod, tpid, "inside cache");
    append_parent(prod.parent, tpid);
    // Dedup defensively before indexing; valid productions are already
    // pairwise-disjoint by taxon set.
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

// Deterministic cold-construction accounting. A cache built from immutable
// resident charts must report no inside recurrence builds and exactly one
// resident consume per active pattern.
struct inside_chart_cache_build_stats {
  std::size_t inside_charts_built = 0;
  std::size_t resident_inside_charts_consumed = 0;

  bool operator==(inside_chart_cache_build_stats const&) const = default;
};

struct inside_chart_cache_active_pattern_fingerprint {
  static constexpr std::uint32_t current_schema_version = 1;

  std::array<std::uint64_t, 2> structure{};
  std::uint32_t schema_version = current_schema_version;

  bool operator==(inside_chart_cache_active_pattern_fingerprint const&) const =
      default;
};

namespace inside_chart_cache_detail {

// Fingerprint the complete semantic payload of an active pattern set. The
// chart rows depend on ordered leaf states, while scoring and reporting also
// depend on positions, weights, reference-state counts, normalization maps,
// and aggregate site metadata. Include every field so same-address in-place
// mutation cannot make stale resident rows appear compatible.
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

// Provenance for an immutable resident set of single-site charts. The token is
// minted only from a checked grammar/plan publication boundary and records
// generation, full grammar fingerprint, and full active-pattern fingerprint.
// Keeping the token separate from the row provider lets callers adapt resident
// layouts without copying pointer tables.
//
// The token is consulted only during construction. The resulting cache copies
// every row and pattern and retains no token, provider, chart, or pattern
// borrow.
class inside_chart_cache_resident_source_identity {
 public:
  inside_chart_cache_resident_source_identity(
      inside_chart_cache_resident_source_identity const&) = default;
  inside_chart_cache_resident_source_identity& operator=(
      inside_chart_cache_resident_source_identity const&) = default;

  [[nodiscard]] std::uint64_t grammar_generation() const noexcept {
    return grammar_generation_;
  }
  [[nodiscard]] chart_plan_fingerprint const& fingerprint() const noexcept {
    return fingerprint_;
  }
  [[nodiscard]] std::size_t pattern_count() const noexcept {
    return pattern_count_;
  }
  [[nodiscard]] inside_chart_cache_active_pattern_fingerprint const&
  active_pattern_fingerprint() const noexcept {
    return active_pattern_fingerprint_;
  }

  void assert_same(clade_grammar const& grammar,
                   chart_execution_plan const& plan,
                   active_site_pattern_set const& active) const {
    if (grammar_ != &grammar) {
      throw std::runtime_error(
          "inside cache resident source: grammar identity mismatch");
    }
    if (grammar_generation_ != plan.grammar_generation()) {
      throw std::runtime_error(
          "inside cache resident source: grammar generation mismatch");
    }
    if (fingerprint_ != plan.fingerprint()) {
      throw std::runtime_error(
          "inside cache resident source: grammar fingerprint mismatch");
    }
    if (pattern_count_ != active.patterns.patterns.size()) {
      throw std::runtime_error(
          "inside cache resident source: active pattern count mismatch");
    }
    if (active_pattern_fingerprint_ !=
        inside_chart_cache_detail::fingerprint_active_pattern_set(active)) {
      throw std::runtime_error(
          "inside cache resident source: active pattern fingerprint "
          "mismatch");
    }
  }

  // A frozen destination may be a distinct grammar object, but it must carry
  // the exact immutable execution identity and shape that produced the
  // resident rows. Check generation and full fingerprint before the explicit
  // shape guard so each stale-snapshot failure is labelled deterministically.
  void assert_compatible_destination(chart_execution_plan const& plan) const {
    if (grammar_generation_ != plan.grammar_generation()) {
      throw std::runtime_error(
          "inside cache resident destination: grammar generation mismatch");
    }
    if (fingerprint_ != plan.fingerprint()) {
      throw std::runtime_error(
          "inside cache resident destination: grammar fingerprint mismatch");
    }
    if (taxon_count_ != plan.taxon_count() ||
        clade_count_ != plan.clades().size() ||
        production_count_ != plan.productions().size() ||
        root_clade_ != plan.root_clade() || all_binary_ != plan.all_binary() ||
        max_arity_ != plan.max_arity()) {
      throw std::runtime_error(
          "inside cache resident destination: execution shape mismatch");
    }
  }

 private:
  friend inside_chart_cache_resident_source_identity
  make_inside_chart_cache_resident_source_identity(
      clade_grammar const&, checked_chart_execution_plan_ref const&,
      active_site_pattern_set const&);

  inside_chart_cache_resident_source_identity(
      clade_grammar const& grammar, chart_execution_plan const& plan,
      active_site_pattern_set const& active)
      : grammar_(&grammar),
        grammar_generation_(plan.grammar_generation()),
        fingerprint_(plan.fingerprint()),
        pattern_count_(active.patterns.patterns.size()),
        taxon_count_(plan.taxon_count()),
        clade_count_(plan.clades().size()),
        production_count_(plan.productions().size()),
        root_clade_(plan.root_clade()),
        all_binary_(plan.all_binary()),
        max_arity_(plan.max_arity()),
        active_pattern_fingerprint_(
            inside_chart_cache_detail::fingerprint_active_pattern_set(active)) {
  }

  clade_grammar const* grammar_ = nullptr;
  std::uint64_t grammar_generation_ = 0;
  chart_plan_fingerprint fingerprint_{};
  std::size_t pattern_count_ = 0;
  std::size_t taxon_count_ = 0;
  std::size_t clade_count_ = 0;
  std::size_t production_count_ = 0;
  clade_id root_clade_ = no_clade;
  bool all_binary_ = false;
  std::size_t max_arity_ = 0;
  inside_chart_cache_active_pattern_fingerprint active_pattern_fingerprint_{};
};

inline inside_chart_cache_resident_source_identity
make_inside_chart_cache_resident_source_identity(
    clade_grammar const& base, checked_chart_execution_plan_ref const& checked,
    active_site_pattern_set const& active) {
  checked.assert_same(base, checked.plan());
  active.assert_no_skipped_invariant_metadata();
  chart_multisite_detail::validate_multisite_inputs(
      checked.plan(), active.patterns, chart_options{});
  return {base, checked.plan(), active};
}

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

  // Immutable identity of the frozen grammar snapshot whose rows populate
  // `base_rows`.  A base pointer and row dimensions are insufficient: the same
  // grammar object can publish a new generation (or a new same-generation
  // fingerprint) without changing either.  Consumers with a checked plan
  // compare this stamp in O(1) before reading resident rows.
  std::uint64_t base_execution_generation = 0;
  chart_plan_fingerprint base_execution_fingerprint;

  chart_options chart_opts;
  std::vector<site_pattern> patterns;  // active/topology-informative only
  std::size_t taxon_count = 0;
  inside_chart_cache_active_pattern_fingerprint active_pattern_fingerprint;
  std::uint64_t invariant_constant_offset = 0;

  std::vector<std::vector<std::array<chart_cost, nuc_state_count>>> base_rows;
  std::vector<std::vector<std::array<chart_cost, nuc_state_count>>> temp_rows;
  std::size_t temp_clade_count = 0;

  // Pairing epoch: the number of inside commits applied to this cache (0 at
  // cold build).  After a correctly paired commit it equals `chain.size()`.
  // `apply_commit_to_outside_cache` requires this to match the chain tip so a
  // caller that forgot the inside commit (even a tombstone-only delta, which
  // adds no temp clades and so slipped the old temp_clade_count guard) cannot
  // silently read stale inside rows.  This is the Phase 3 pairing guard; it is
  // the natural substrate for Phase 4's reader-snapshot epoch, which will
  // number whole chain+cache snapshots the same way.
  std::size_t commit_epoch = 0;

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
  std::size_t multifurcation_productions_scored = 0;
  inside_chart_cache_build_stats build_stats;

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
  chart_multisite_detail::validate_multisite_inputs(
      base, active.patterns, options);

  inside_chart_cache cache;
  cache.base = &base;
  cache.base_execution_generation = base.execution_generation;
  chart_execution_plan_detail::record_full_grammar_fingerprint_scan();
  cache.base_execution_fingerprint =
      chart_execution_plan_detail::fingerprint_chart_grammar(base);
  cache.chart_opts = options;
  cache.patterns = active.patterns.patterns;
  cache.taxon_count = active.patterns.taxon_count;
  cache.active_pattern_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(active);
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
    cache.multifurcation_productions_scored +=
        chart.multifurcation_productions_scored;
    ++cache.build_stats.inside_charts_built;
    // Each inner vector starts empty (resize above), so assign allocates it
    // exactly once rather than zero-initializing then reallocating.
    cache.base_rows[p].assign(chart.inside.begin(), chart.inside.end());
  }
  return cache;
}

// Trusted cold-cache construction for a grammar that has already published a
// checked immutable execution plan.  Compatibility is checked once at this
// boundary; every pattern then reuses the plan's validated order and
// production descriptors.
inline inside_chart_cache build_inside_chart_cache(
    clade_grammar const& base,
    checked_chart_execution_plan_ref const& checked,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset) {
  active.assert_no_skipped_invariant_metadata();
  checked.assert_same(base, checked.plan());
  auto const& plan = checked.plan();
  chart_multisite_detail::validate_multisite_inputs(plan, active.patterns,
                                                    options);

  inside_chart_cache cache;
  cache.base = &base;
  cache.base_execution_generation = plan.grammar_generation();
  cache.base_execution_fingerprint = plan.fingerprint();
  cache.chart_opts = options;
  cache.patterns = active.patterns.patterns;
  cache.taxon_count = active.patterns.taxon_count;
  cache.active_pattern_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(active);
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
    auto chart = build_single_site_chart(plan, states, build_opts);
    if (chart.inside.size() != plan.clades().size()) {
      throw std::runtime_error(
          "inside cache: base chart clade count mismatch");
    }
    cache.multifurcation_productions_scored +=
        chart.multifurcation_productions_scored;
    ++cache.build_stats.inside_charts_built;
    cache.base_rows[p].assign(chart.inside.begin(), chart.inside.end());
  }
  return cache;
}

// Scheduler-aware trusted cold construction.  All validation and cache-shape
// publication remains on the caller.  Workers own disjoint pattern rows and
// publish only pattern-indexed recurrence counters; the caller folds those
// counters in increasing pattern order after every task has joined.  If a
// worker throws, the scheduler joins the operation and this function publishes
// no cache or partial accounting to its caller.
//
// `run_summary`, when non-null, receives this operation's summary (not a
// lifetime scheduler snapshot) after a successful join.
inline inside_chart_cache build_inside_chart_cache(
    clade_grammar const& base, checked_chart_execution_plan_ref const& checked,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset, chart_scheduler& scheduler,
    chart_indexed_range_options range_options = {},
    chart_scheduler_run_summary* run_summary = nullptr) {
  active.assert_no_skipped_invariant_metadata();
  checked.assert_same(base, checked.plan());
  auto const& plan = checked.plan();
  chart_multisite_detail::validate_multisite_inputs(plan, active.patterns,
                                                    options);

  inside_chart_cache cache;
  cache.base = &base;
  cache.base_execution_generation = plan.grammar_generation();
  cache.base_execution_fingerprint = plan.fingerprint();
  cache.chart_opts = options;
  cache.patterns = active.patterns.patterns;
  cache.taxon_count = active.patterns.taxon_count;
  cache.active_pattern_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(active);
  cache.invariant_constant_offset = invariant_constant_offset;
  cache.temp_clade_count = 0;

  chart_options build_opts = options;
  build_opts.keep_trace = false;
  build_opts.max_trace_choices = 0;

  cache.base_rows.resize(cache.patterns.size());
  cache.temp_rows.resize(cache.patterns.size());
  std::vector<std::size_t> multifurcation_by_pattern(cache.patterns.size(), 0);

  auto summary = scheduler.for_each_indexed_range(
      cache.patterns.size(), range_options,
      [&](chart_indexed_range const& range, std::size_t,
          chart_scheduler_cancellation_token const&) {
        for (std::size_t p = range.begin; p < range.end; ++p) {
          leaf_site_states states;
          states.state_by_taxon = cache.patterns[p].state_by_taxon;
          auto chart = build_single_site_chart(plan, states, build_opts);
          if (chart.inside.size() != plan.clades().size()) {
            throw std::runtime_error(
                "inside cache: base chart clade count mismatch");
          }
          multifurcation_by_pattern[p] =
              chart.multifurcation_productions_scored;
          cache.base_rows[p].assign(chart.inside.begin(), chart.inside.end());
        }
      });

  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    cache.multifurcation_productions_scored += multifurcation_by_pattern[p];
    ++cache.build_stats.inside_charts_built;
  }
  if (run_summary != nullptr) *run_summary = summary;
  return cache;
}

inline inside_chart_cache build_inside_chart_cache(
    clade_grammar const& base, chart_execution_plan const& plan,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset) {
  auto checked = check_chart_execution_plan(base, plan);
  return build_inside_chart_cache(base, checked, active, options,
                                  invariant_constant_offset);
}

inline inside_chart_cache build_inside_chart_cache(
    clade_grammar const& base, chart_execution_plan const& plan,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset, chart_scheduler& scheduler,
    chart_indexed_range_options range_options = {},
    chart_scheduler_run_summary* run_summary = nullptr) {
  auto checked = check_chart_execution_plan(base, plan);
  return build_inside_chart_cache(base, checked, active, options,
                                  invariant_constant_offset, scheduler,
                                  range_options, run_summary);
}

// Build a cold cache by copying compatible, immutable resident inside charts.
// No inside recurrence runs on this path. `source_identity` must have been
// minted for the exact grammar snapshot and active-pattern payload that
// produced the charts; all provenance checks happen before the provider is
// invoked.
//
// The provider must return `single_site_chart const&`. It is invoked once per
// pattern as `provider(pattern_index, active_pattern)`.
template <class ResidentChartProvider>
  requires requires(ResidentChartProvider& provider, std::size_t index,
                    site_pattern const& pattern) {
    {
      std::invoke(provider, index, pattern)
    } -> std::same_as<single_site_chart const&>;
  }
inline inside_chart_cache build_inside_chart_cache_from_resident_inside(
    clade_grammar const& destination_base,
    checked_chart_execution_plan_ref const& destination_checked,
    clade_grammar const& source_base,
    checked_chart_execution_plan_ref const& source_checked,
    inside_chart_cache_resident_source_identity const& source_identity,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset,
    ResidentChartProvider&& resident_chart_provider) {
  source_checked.assert_same(source_base, source_checked.plan());
  source_identity.assert_same(source_base, source_checked.plan(), active);
  destination_checked.assert_same(destination_base, destination_checked.plan());
  auto const& destination_plan = destination_checked.plan();
  source_identity.assert_compatible_destination(destination_plan);

  active.assert_no_skipped_invariant_metadata();
  chart_multisite_detail::validate_multisite_inputs(destination_plan,
                                                    active.patterns, options);

  inside_chart_cache cache;
  cache.base = &destination_base;
  cache.base_execution_generation = destination_plan.grammar_generation();
  cache.base_execution_fingerprint = destination_plan.fingerprint();
  cache.chart_opts = options;
  cache.patterns = active.patterns.patterns;
  cache.taxon_count = active.patterns.taxon_count;
  cache.active_pattern_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(active);
  cache.invariant_constant_offset = invariant_constant_offset;
  cache.temp_clade_count = 0;
  cache.base_rows.resize(cache.patterns.size());
  cache.temp_rows.resize(cache.patterns.size());

  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    single_site_chart const& chart =
        std::invoke(resident_chart_provider, p, active.patterns.patterns[p]);
    if (chart.inside.size() != destination_plan.clades().size()) {
      throw std::runtime_error(
          "inside cache resident source: chart clade count mismatch");
    }
    cache.base_rows[p].assign(chart.inside.begin(), chart.inside.end());
    ++cache.build_stats.resident_inside_charts_consumed;
  }
  return cache;
}

// Scheduler-aware resident construction.  The provider is invoked as
// `provider(pattern_index, active_pattern, stable_slot_id)`.  Calls for
// different slots may overlap; calls carrying the same stable slot never do.
// A provider that needs mutable scratch must therefore bind one scratch object
// to each scheduler slot.  The returned chart is copied before the provider is
// called again on that slot.
template <class ResidentChartProvider>
  requires requires(ResidentChartProvider& provider, std::size_t index,
                    site_pattern const& pattern, std::size_t stable_slot) {
    {
      std::invoke(provider, index, pattern, stable_slot)
    } -> std::same_as<single_site_chart const&>;
  }
inline inside_chart_cache build_inside_chart_cache_from_resident_inside(
    clade_grammar const& destination_base,
    checked_chart_execution_plan_ref const& destination_checked,
    clade_grammar const& source_base,
    checked_chart_execution_plan_ref const& source_checked,
    inside_chart_cache_resident_source_identity const& source_identity,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset,
    ResidentChartProvider&& resident_chart_provider, chart_scheduler& scheduler,
    chart_indexed_range_options range_options = {},
    chart_scheduler_run_summary* run_summary = nullptr) {
  source_checked.assert_same(source_base, source_checked.plan());
  source_identity.assert_same(source_base, source_checked.plan(), active);
  destination_checked.assert_same(destination_base, destination_checked.plan());
  auto const& destination_plan = destination_checked.plan();
  source_identity.assert_compatible_destination(destination_plan);

  active.assert_no_skipped_invariant_metadata();
  chart_multisite_detail::validate_multisite_inputs(destination_plan,
                                                    active.patterns, options);

  inside_chart_cache cache;
  cache.base = &destination_base;
  cache.base_execution_generation = destination_plan.grammar_generation();
  cache.base_execution_fingerprint = destination_plan.fingerprint();
  cache.chart_opts = options;
  cache.patterns = active.patterns.patterns;
  cache.taxon_count = active.patterns.taxon_count;
  cache.active_pattern_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(active);
  cache.invariant_constant_offset = invariant_constant_offset;
  cache.temp_clade_count = 0;
  cache.base_rows.resize(cache.patterns.size());
  cache.temp_rows.resize(cache.patterns.size());

  auto summary = scheduler.for_each_indexed_range(
      cache.patterns.size(), range_options,
      [&](chart_indexed_range const& range, std::size_t stable_slot,
          chart_scheduler_cancellation_token const&) {
        for (std::size_t p = range.begin; p < range.end; ++p) {
          single_site_chart const& chart =
              std::invoke(resident_chart_provider, p,
                          active.patterns.patterns[p], stable_slot);
          if (chart.inside.size() != destination_plan.clades().size()) {
            throw std::runtime_error(
                "inside cache resident source: chart clade count mismatch");
          }
          cache.base_rows[p].assign(chart.inside.begin(), chart.inside.end());
        }
      });

  cache.build_stats.resident_inside_charts_consumed = cache.patterns.size();
  if (run_summary != nullptr) *run_summary = summary;
  return cache;
}

// Convenience overload for the historical/same-source case. It deliberately
// routes through the stronger source-to-destination boundary so both paths
// share exactly the same provenance and shape checks.
template <class ResidentChartProvider>
  requires requires(ResidentChartProvider& provider, std::size_t index,
                    site_pattern const& pattern) {
    {
      std::invoke(provider, index, pattern)
    } -> std::same_as<single_site_chart const&>;
  }
inline inside_chart_cache build_inside_chart_cache_from_resident_inside(
    clade_grammar const& base, checked_chart_execution_plan_ref const& checked,
    inside_chart_cache_resident_source_identity const& source_identity,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset,
    ResidentChartProvider&& resident_chart_provider) {
  return build_inside_chart_cache_from_resident_inside(
      base, checked, base, checked, source_identity, active, options,
      invariant_constant_offset,
      std::forward<ResidentChartProvider>(resident_chart_provider));
}

template <class ResidentChartProvider>
  requires requires(ResidentChartProvider& provider, std::size_t index,
                    site_pattern const& pattern, std::size_t stable_slot) {
    {
      std::invoke(provider, index, pattern, stable_slot)
    } -> std::same_as<single_site_chart const&>;
  }
inline inside_chart_cache build_inside_chart_cache_from_resident_inside(
    clade_grammar const& base, checked_chart_execution_plan_ref const& checked,
    inside_chart_cache_resident_source_identity const& source_identity,
    active_site_pattern_set const& active, chart_options options,
    std::uint64_t invariant_constant_offset,
    ResidentChartProvider&& resident_chart_provider, chart_scheduler& scheduler,
    chart_indexed_range_options range_options = {},
    chart_scheduler_run_summary* run_summary = nullptr) {
  return build_inside_chart_cache_from_resident_inside(
      base, checked, base, checked, source_identity, active, options,
      invariant_constant_offset,
      std::forward<ResidentChartProvider>(resident_chart_provider), scheduler,
      range_options, run_summary);
}

namespace inside_chart_cache_detail {

// Recompute the inside row for a single clade of the committed (tip) grammar,
// reading children rows from `cache`.  This is the standard inside recurrence
// (identical to `build_single_site_chart`'s loop body and to
// `chart_spr_search_detail::recompute_overlay_delta_row`); it owns no new
// math.  Available productions at `ref` are the frozen-base productions not
// tombstoned by any chain delta, plus the merged temp productions with
// `ref` as parent.
template <class RowProvider>
inline std::array<chart_cost, nuc_state_count>
recompute_tip_inside_row_from_rows(
    inside_chart_cache const& cache, chain_tip_index const& idx,
    std::size_t pattern, overlay_clade_ref ref,
    RowProvider&& row_provider) {
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

  // Accumulate one production's contribution into `row`, reading the child rows
  // from the cache. Matches
  // `accumulate_overlay_production_row` / build_single_site_chart exactly.
  auto accumulate_children =
      [&](std::vector<overlay_clade_ref> const& children) {
    struct production_view {
      std::vector<overlay_clade_ref> const& children;
    } prod{children};
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      auto checked_row_provider = [&](overlay_clade_ref child) -> auto const& {
        if (child.id == no_clade) {
          throw std::runtime_error(
              "inside cache: production child ref is no_clade");
        }
        return std::invoke(row_provider, child);
      };
      auto total = parsimony_chart_detail::combine_production_inside_row(
          prod, parent_state, checked_row_provider);
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
      parsimony_chart_detail::validate_production_inside_row_inputs(
          base, prod, pid, "inside cache");
      std::vector<overlay_clade_ref> children;
      children.reserve(prod.children.size());
      for (auto child : prod.children) children.push_back(base_clade_ref(child));
      accumulate_children(children);
      saw_production = true;
    }
  }
  for (auto tpid : temp_prods_for_parent(idx, ref)) {
    auto const& prod = idx.tip_overlay.temp_productions[tpid];
    accumulate_children(prod.children);
    saw_production = true;
  }
  if (!saw_production) {
    throw std::runtime_error(
        "inside cache: non-singleton clade has no available productions");
  }
  return row;
}

inline std::array<chart_cost, nuc_state_count> recompute_tip_inside_row(
    inside_chart_cache const& cache, chain_tip_index const& idx,
    std::size_t pattern, overlay_clade_ref ref) {
  return recompute_tip_inside_row_from_rows(
      cache, idx, pattern, ref,
      [&](overlay_clade_ref child) -> auto const& {
        return cache.row(pattern, child);
      });
}

inline std::size_t count_multifurcating_inside_productions(
    chain_tip_index const& idx, overlay_clade_ref ref) {
  auto const& base = *idx.tip_overlay.base;
  std::size_t count = 0;
  if (ref.space == overlay_id_space::base) {
    for (auto pid : base.productions_by_parent[ref.id]) {
      if (chain_tip_base_production_removed(idx, pid)) continue;
      if (pid >= base.productions.size()) {
        throw std::runtime_error(
            "inside cache: base production out of range while counting");
      }
      if (base.productions[pid].children.size() != 2) ++count;
    }
  }
  for (auto tpid : temp_prods_for_parent(idx, ref)) {
    if (tpid >= idx.tip_overlay.temp_productions.size()) {
      throw std::runtime_error(
          "inside cache: temp production out of range while counting");
    }
    if (idx.tip_overlay.temp_productions[tpid].children.size() != 2) ++count;
  }
  return count;
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
//
// Pairing contract.  `apply_commit_to_inside_cache` and
// `apply_commit_to_outside_cache` (Phase 3) must be invoked as a paired
// inside-then-outside step for every appended delta.  This is enforced by the
// `commit_epoch` field: the inside primitive requires the cache to be exactly
// one commit behind the chain tip (`commit_epoch == chain.size() - 1`) and
// advances it to the tip (`commit_epoch = chain.size()`); the outside
// primitive then requires the inside cache to be at the tip.  A skipped,
// doubled, or out-of-order commit throws a labelled error rather than reading
// stale rows.
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
  if (cache.base_execution_generation != chain.base().execution_generation) {
    throw std::runtime_error(
        "apply_commit_to_inside_cache: cache base execution generation does "
        "not match chain base");
  }

  // Pairing guard: the cache must be exactly one commit behind the tip.  This
  // catches a doubled inside commit (ahead of the chain) and an inside commit
  // issued without a fresh append (stale); the outside-commit guard below
  // catches a skipped inside commit.  chain.size() >= 1 here (empty-chain check
  // above), so `chain.size() - 1` is well-defined.
  if (cache.commit_epoch + 1 != chain.size()) {
    throw std::runtime_error(
        "apply_commit_to_inside_cache: inside cache commit_epoch (" +
        std::to_string(cache.commit_epoch) +
        ") is not exactly one behind the chain tip (" +
        std::to_string(chain.size()) +
        "); inside/outside commits must be paired one-per-appended delta");
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
      cache.multifurcation_productions_scored +=
          inside_chart_cache_detail::count_multifurcating_inside_productions(
              idx, ref);
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

  // Advance the pairing epoch to the tip (the post-state the outside-commit
  // guard requires).  Bumped last so the epoch reflects a completed commit.
  cache.commit_epoch = chain.size();

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

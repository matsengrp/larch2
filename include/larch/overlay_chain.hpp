#pragma once

// WRIC DAG-native SPR & rank-3 rewrite: the overlay chain (Work item 1
// substrate, Phase 1).
//
// This header introduces the append-only overlay chain as a *pure data
// structure*: an ordered sequence of accepted SPR deltas folded onto a frozen
// base grammar, with chain position as ordinal, taxon-set key as identity, and
// the tombstone rule enforced.  It owns no live mutation of any search state
// and no chart cache -- those land in later phases (Phase 2/3 for the cache,
// Phase 4 for wiring into the search loop).  The whole-chain materialization
// exposed here is the correctness oracle every later phase must agree with.
//
// Identity conventions (per the plan's Work item 1, stated here so they are not
// left implicit):
//
//   * Chain position is an ORDINAL.  It orders the deltas for bottom-up /
//     top-down passes and for report readability.  It is *not* an identity.
//   * Taxon-set key is the IDENTITY that survives materialization, rebuild, and
//     report round trips.  Clade identity inside the chain is the sorted set of
//     descendant taxa; production identity is the (parent-taxa, children-taxa)
//     key.  This matches `rank3_detail::production_key_from_id`.
//   * The TOMBSTONE RULE governs re-acceptance: tombstoning a base production
//     already tombstoned by an earlier delta is a hard error, not a key-dedup
//     silent skip.
//
// Temp-clade space model (Phase 1):
//
//   * The chain maintains a single merged temp-clade space, deduplicated by
//     taxon set, across all deltas.  Each appended delta is REBASED onto the
//     frozen base grammar plus this merged temp space: every clade reference in
//     the delta's added productions is resolved by taxon set to either a base
//     clade (`base_clade_ref`) or a merged temp clade (`temp_clade_ref`), and
//     genuinely new clades are assigned fresh merged ids.  Added clades that no
//     production references are still recorded in the merged space (assigned a
//     fresh id), so the stored delta remains faithful to the input; well-formed
//     SPR deltas reference every added clade, so this only matters for
//     hand-built or malformed deltas.
//   * A stored delta's added productions therefore use MERGED-ABSOLUTE temp
//     references that may point at clades introduced by *earlier* deltas.  This
//     means a stored `spr_overlay_delta` is NOT individually materializable via
//     the single-candidate substrate (its temp references can exceed its own
//     `temp_clades.size()`); only the combined overlay returned by `tip()` /
//     `materialize_overlay_chain` is materializable.  Later phases that need
//     per-delta derived indices (affected_order, reachability) recompute them
//     against the merged space, not the stored delta in isolation.
//   * Tombstones (removed productions) are of BASE productions only, matching
//     the stable overlay vocabulary (`overlay_clade_grammar` has no "removed
//     temp productions" field, and changing that vocabulary is a plan non-goal).
//     A delta that tombstones a production introduced by an earlier delta is
//     rejected with a labelled error rather than silently mishandled; the
//     general overlay-production-removal case is out of Phase 1 scope.

#include <larch/chart_spr.hpp>        // overlay vocabulary + materialize_overlay_grammar
#include <larch/chart_spr_search.hpp> // spr_overlay_delta

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {

namespace overlay_chain_detail {

// Find the frozen-base clade id whose taxon set equals `taxa`, or `no_clade`.
// `base_lookup` is `chart_spr_detail::build_clade_lookup(base)`.
inline clade_id find_base_clade_by_taxa(
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::vector<taxon_id> const& taxa) {
  auto it = base_lookup.find(taxa);
  return it == base_lookup.end() ? no_clade : it->second;
}

// Resolve a production of the *tip* grammar `tip` (the grammar a delta was
// built against) to its corresponding frozen-base production id, by taxon-set
// key.  Returns `no_production` if no base production carries that key (i.e. the
// tip production was introduced by an earlier overlay delta and is not a frozen
// base production).
inline production_id find_base_production_for_tip_production(
    clade_grammar const& base,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    clade_grammar const& tip, production_id tip_pid) {
  if (tip_pid == no_production || tip_pid >= tip.productions.size()) {
    throw std::runtime_error(
        "overlay_chain: tip production id out of range during tombstone rebase");
  }
  auto const& tip_prod = tip.productions[tip_pid];
  if (tip_prod.parent == no_clade || tip_prod.parent >= tip.clades.size()) {
    throw std::runtime_error(
        "overlay_chain: tip production parent out of range during rebase");
  }
  auto parent_it = base_lookup.find(tip.clades[tip_prod.parent].taxa);
  if (parent_it == base_lookup.end()) return no_production;
  std::vector<clade_id> child_ids;
  child_ids.reserve(tip_prod.children.size());
  for (auto tip_child : tip_prod.children) {
    if (tip_child == no_clade || tip_child >= tip.clades.size()) {
      throw std::runtime_error(
          "overlay_chain: tip production child out of range during rebase");
    }
    auto child_it = base_lookup.find(tip.clades[tip_child].taxa);
    if (child_it == base_lookup.end()) return no_production;
    child_ids.push_back(child_it->second);
  }
  std::sort(child_ids.begin(), child_ids.end());
  for (auto pid : base.productions_by_parent[parent_it->second]) {
    auto prod_children = base.productions[pid].children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == child_ids) return pid;
  }
  return no_production;
}

inline std::vector<taxon_id> normalize_taxa(std::vector<taxon_id> taxa) {
  std::sort(taxa.begin(), taxa.end());
  taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
  return taxa;
}

}  // namespace overlay_chain_detail

// An append-only chain of accepted SPR deltas folded onto a frozen base
// grammar.  See the header comment for the identity / tombstone / temp-space
// model.
//
// Lifetime: the chain stores a pointer to the frozen base grammar; the caller
// must keep that grammar alive for the lifetime of the chain (mirrors
// `overlay_clade_grammar::base`).
struct overlay_chain {
  overlay_chain() = default;

  explicit overlay_chain(clade_grammar const& base)
      : base_{&base},
        base_lookup_{chart_spr_detail::build_clade_lookup(base)} {}

  // The frozen base grammar.  Never mutated by accepts.
  [[nodiscard]] clade_grammar const& base() const {
    if (base_ == nullptr) {
      throw std::runtime_error("overlay_chain: uninitialized chain has no base");
    }
    return *base_;
  }

  // Number of deltas in the chain == number of chain positions (ordinals).
  [[nodiscard]] std::size_t size() const { return deltas_.size(); }
  [[nodiscard]] bool empty() const { return deltas_.empty(); }

  // Read-only access to the delta at ordinal `pos`.
  [[nodiscard]] spr_overlay_delta const& at(std::size_t pos) const {
    if (pos >= deltas_.size()) {
      throw std::runtime_error("overlay_chain: position " +
                               std::to_string(pos) +
                               " out of range (size " +
                               std::to_string(deltas_.size()) + ")");
    }
    return deltas_[pos];
  }

  // The ordinal (chain position) of a delta stored in this chain, located by
  // address.  Throws if the delta is not a member of this chain.  Chain
  // position is an ordinal for ordering only; taxon-set key is the identity.
  [[nodiscard]] std::size_t position_of(spr_overlay_delta const& delta) const {
    for (std::size_t i = 0; i < deltas_.size(); ++i) {
      if (&deltas_[i] == &delta) return i;
    }
    throw std::runtime_error(
        "overlay_chain: position_of called with a delta not in this chain");
  }

  // The combined overlay at the current chain tip: a single
  // `overlay_clade_grammar` against the frozen base, equivalent to applying
  // every accepted delta.  This is the only materializable view of the chain
  // (see the header comment on the temp-space model).
  [[nodiscard]] overlay_clade_grammar tip() const {
    if (base_ == nullptr) {
      throw std::runtime_error("overlay_chain: uninitialized chain has no tip");
    }
    overlay_clade_grammar overlay;
    overlay.base = base_;

    // Merged temp clades: concatenation of every delta's new clades, in
    // merged-id order.  Append-time dedup guarantees this is globally unique by
    // taxon set, so the concatenation *is* the merged temp space.
    for (auto const& delta : deltas_) {
      for (auto const& clade : delta.temp_clades) {
        overlay.temp_clades.push_back(clade);
      }
    }

    // Merged temp productions: every delta's rebased productions, deduplicated
    // by (parent, sorted children) merged-absolute references.
    std::set<std::pair<overlay_clade_ref, std::vector<overlay_clade_ref>>>
        seen_productions;
    for (auto const& delta : deltas_) {
      for (auto const& prod : delta.temp_productions) {
        auto children_key = prod.children;
        std::sort(children_key.begin(), children_key.end());
        if (seen_productions
                .insert(std::make_pair(prod.parent, children_key))
                .second) {
          overlay.temp_productions.push_back(prod);
        }
      }
    }

    // Merged tombstones: union of every delta's rebased base-production
    // tombstones, as frozen-base production ids.
    std::set<production_id> tombstones;
    for (auto const& delta : deltas_) {
      for (auto pid : delta.removed_base_productions) {
        tombstones.insert(pid);
      }
    }
    overlay.removed_base_productions.assign(tombstones.begin(),
                                            tombstones.end());

    return overlay;
  }

  // Append an accepted SPR delta to the chain.  `delta` must have been built
  // (via `build_spr_overlay_delta`) against the grammar that is the CURRENT
  // chain tip -- for the first delta that is the frozen base, for later deltas
  // it is the materialized previous tip (`materialize_overlay_chain` of the
  // chain before this append, or `materialize_overlay_grammar(tip())`).  The
  // delta is rebased onto the frozen base plus the merged temp space and stored
  // with merged-absolute references.
  //
  // Throws a labelled `std::runtime_error` (and leaves the chain unchanged,
  // strong exception guarantee) when:
  //   * the chain is uninitialized, or `delta.base` is null;
  //   * `delta` carries an out-of-range reference (malformed delta);
  //   * a delta's added production references a clade whose taxon set is absent
  //     from both the frozen base and the earlier-added clades (dangling clade
  //     reference);
  //   * a delta tombstones a production that is not a frozen-base production
  //     (tombstoning an earlier delta's added production is out of Phase 1
  //     scope and is rejected rather than silently mishandled);
  //   * a delta tombstones a base production already tombstoned by an earlier
  //     delta (double tombstone -- hard error, not a key-dedup).
  void append(spr_overlay_delta const& delta) {
    if (base_ == nullptr) {
      throw std::runtime_error(
          "overlay_chain: cannot append to an uninitialized chain");
    }
    if (delta.base == nullptr) {
      throw std::runtime_error(
          "overlay_chain: appended delta has no base grammar pointer");
    }
    clade_grammar const& tip = *delta.base;

    // Accumulated merged-clade index (taxa -> merged id) rebuilt from the chain
    // so far, plus this delta's new assignments.  Rebuilt from `deltas_` so the
    // chain has a single source of truth; `deltas_` is only mutated at the very
    // end to preserve the strong exception guarantee.
    std::map<std::vector<taxon_id>, clade_id> merged_index =
        build_merged_clade_index();
    std::set<production_id> accumulated_tombstones = accumulated_base_tombstones();

    std::map<overlay_clade_ref, overlay_clade_ref> ref_map;
    std::vector<clade_key> new_temp_clades;
    std::size_t next_merged_id = merged_temp_clade_count();
    std::vector<overlay_grammar_production> rebased_productions;
    std::vector<production_id> rebased_tombstones;

    // Resolve an incoming delta clade reference to a merged-absolute reference.
    // `space == base` refs read taxa from the tip grammar; `space == temp` refs
    // read taxa from the delta's own added clades.
    auto resolve_ref = [&](overlay_clade_ref ref) -> overlay_clade_ref {
      if (auto memo = ref_map.find(ref); memo != ref_map.end()) {
        return memo->second;
      }
      std::vector<taxon_id> taxa;
      if (ref.space == overlay_id_space::base) {
        if (ref.id == no_clade || ref.id >= tip.clades.size()) {
          throw std::runtime_error(
              "overlay_chain: delta base clade reference out of range");
        }
        taxa = overlay_chain_detail::normalize_taxa(tip.clades[ref.id].taxa);
      } else {
        if (ref.id == no_clade || ref.id >= delta.temp_clades.size()) {
          throw std::runtime_error(
              "overlay_chain: delta temp clade reference out of range");
        }
        taxa =
            overlay_chain_detail::normalize_taxa(delta.temp_clades[ref.id].taxa);
      }

      // Frozen base?
      if (auto base_id = overlay_chain_detail::find_base_clade_by_taxa(
              base_lookup_, taxa);
          base_id != no_clade) {
        overlay_clade_ref resolved = base_clade_ref(base_id);
        ref_map[ref] = resolved;
        return resolved;
      }
      // Earlier-added merged temp clade?
      if (auto merged_it = merged_index.find(taxa); merged_it != merged_index.end()) {
        overlay_clade_ref resolved = temp_clade_ref(merged_it->second);
        ref_map[ref] = resolved;
        return resolved;
      }
      // A base-space reference that resolves to neither base nor earlier-added
      // is a dangling clade reference: the delta names a clade that does not
      // exist in the chain.  For a well-formed sequential delta (built against
      // the materialized previous tip) this never fires; it catches malformed
      // deltas and chain/tip mismatches.
      if (ref.space == overlay_id_space::base) {
        throw std::runtime_error(
            "overlay_chain: delta references a clade absent from the frozen "
            "base and from earlier-added clades (dangling clade reference)");
      }
      // A temp-space reference is the delta adding a genuinely new clade:
      // assign it a fresh merged id.
      if (next_merged_id >= static_cast<std::size_t>(no_clade)) {
        throw std::runtime_error("overlay_chain: merged temp clade space exhausted");
      }
      clade_id assigned = static_cast<clade_id>(next_merged_id);
      merged_index[taxa] = assigned;
      new_temp_clades.push_back(clade_key{taxa});
      ++next_merged_id;
      overlay_clade_ref resolved = temp_clade_ref(assigned);
      ref_map[ref] = resolved;
      return resolved;
    };

    // Rebase added productions.
    for (auto const& prod : delta.temp_productions) {
      overlay_grammar_production rebased;
      rebased.parent = resolve_ref(prod.parent);
      rebased.children.reserve(prod.children.size());
      for (auto child : prod.children) {
        rebased.children.push_back(resolve_ref(child));
      }
      rebased.witnesses = prod.witnesses;
      rebased.multiplicity = prod.multiplicity;
      rebased_productions.push_back(std::move(rebased));
    }

    // Carry over any of this delta's added temp clades that no production
    // references.  `resolve_ref` only records a clade when a production names
    // it, so an unreferenced added clade would otherwise be silently dropped
    // from the stored delta.  Well-formed SPR deltas reference every added
    // clade, and `materialize_overlay_grammar` drops unreachable clades
    // anyway, so the Phase-1 oracle is insensitive -- but recording them keeps
    // the stored delta faithful to the input and removes the latent "every
    // added clade is referenced" assumption.  Referenced clades (and clades
    // introduced by earlier deltas) are already in `merged_index` and are
    // skipped here so they are not double-counted.
    for (std::size_t i = 0; i < delta.temp_clades.size(); ++i) {
      auto taxa = overlay_chain_detail::normalize_taxa(delta.temp_clades[i].taxa);
      // Skip clades that resolve to the frozen base or to an already-known
      // merged temp clade, mirroring `resolve_ref`'s resolution order.
      // Referenced clades were resolved (and recorded) above; an unreferenced
      // clade that matches a base clade is not new and must not be duplicated
      // in the temp space.
      if (overlay_chain_detail::find_base_clade_by_taxa(base_lookup_, taxa) !=
          no_clade) {
        continue;
      }
      if (merged_index.contains(taxa)) continue;
      if (next_merged_id >= static_cast<std::size_t>(no_clade)) {
        throw std::runtime_error(
            "overlay_chain: merged temp clade space exhausted");
      }
      clade_id assigned = static_cast<clade_id>(next_merged_id);
      merged_index[taxa] = assigned;
      new_temp_clades.push_back(clade_key{taxa});
      ++next_merged_id;
    }

    // Rebase tombstones to frozen-base production ids and enforce the tombstone
    // rule.
    for (auto tip_pid : delta.removed_base_productions) {
      if (tip_pid == no_production || tip_pid >= tip.productions.size()) {
        throw std::runtime_error(
            "overlay_chain: delta removed production id out of range");
      }
      production_id base_pid =
          overlay_chain_detail::find_base_production_for_tip_production(
              *base_, base_lookup_, tip, tip_pid);
      if (base_pid == no_production) {
        throw std::runtime_error(
            "overlay_chain: delta tombstones a production that is not a "
            "frozen-base production; tombstoning an earlier delta's added "
            "production is not supported (stable overlay vocabulary)");
      }
      if (accumulated_tombstones.count(base_pid) != 0) {
        throw std::runtime_error(
            "overlay_chain: delta tombstones a base production already "
            "tombstoned by an earlier delta (double tombstone)");
      }
      accumulated_tombstones.insert(base_pid);
      rebased_tombstones.push_back(base_pid);
    }

    // Commit: build the stored rebased delta and push.  Only `deltas_` is
    // mutated, and only after all validation has passed.
    spr_overlay_delta stored;
    stored.base = base_;
    stored.temp_clades = std::move(new_temp_clades);
    stored.temp_productions = std::move(rebased_productions);
    stored.removed_base_productions = std::move(rebased_tombstones);
    // Phase 10 identity surface: carry the commit-source label through the
    // rebase so the JSON identity report can distinguish Option-C entries
    // from SPR overlay-delta entries.  The label carries no behavioral
    // contract and is not used by any chart/cache logic.
    stored.commit_source = delta.commit_source;
    // Derived index fields (affected_order, reachability, temp indices) are
    // intentionally left empty in Phase 1: they are not needed for the
    // materialization oracle and are recomputed against the merged space by
    // later phases that consume them.
    deltas_.push_back(std::move(stored));
  }

 private:
  // Total number of merged temp clades across all stored deltas (== sum of each
  // delta's new-clade count, since append dedups globally).
  [[nodiscard]] std::size_t merged_temp_clade_count() const {
    std::size_t n = 0;
    for (auto const& delta : deltas_) n += delta.temp_clades.size();
    return n;
  }

  // Taxon-set -> merged-id map for the accumulated temp space, rebuilt from
  // `deltas_`.
  [[nodiscard]] std::map<std::vector<taxon_id>, clade_id>
  build_merged_clade_index() const {
    std::map<std::vector<taxon_id>, clade_id> index;
    clade_id next = 0;
    for (auto const& delta : deltas_) {
      for (auto const& clade : delta.temp_clades) {
        auto taxa = overlay_chain_detail::normalize_taxa(clade.taxa);
        // `append` dedups temp clades globally by taxon set, so every stored
        // clade carries a unique taxon set and the emplace must always insert.
        // Asserting the result documents the load-bearing invariant the
        // merged-id assignment (and therefore production ref stability) relies
        // on.  The emplace is kept OUTSIDE the assert so its side effect runs
        // even when NDEBUG is defined (Release builds), where `assert` is a
        // no-op that does not evaluate its argument.
        bool inserted = index.emplace(taxa, next).second;
        assert(inserted);
        (void)inserted;
        ++next;
      }
    }
    return index;
  }

  // Union of every delta's rebased base-production tombstones.
  [[nodiscard]] std::set<production_id> accumulated_base_tombstones() const {
    std::set<production_id> tombstones;
    for (auto const& delta : deltas_) {
      for (auto pid : delta.removed_base_productions) {
        tombstones.insert(pid);
      }
    }
    return tombstones;
  }

  clade_grammar const* base_ = nullptr;
  std::map<std::vector<taxon_id>, clade_id> base_lookup_;
  std::vector<spr_overlay_delta> deltas_;
};

// Materialize the whole overlay chain into one dense grammar by folding every
// accepted delta onto the frozen base via the existing
// `materialize_overlay_grammar` substrate.  This is the Phase-1 correctness
// oracle: it must produce a grammar whose clade taxon-set set and production
// key set equal the result of applying the deltas sequentially (delta by
// delta) to a live grammar.
inline overlay_materialization_result materialize_overlay_chain(
    overlay_chain const& chain) {
  return materialize_overlay_grammar(chain.tip());
}

// Search-internal checked form.  The token is for the chain's frozen base;
// append/rebase/tombstone validation remains owned by overlay_chain, while the
// combined materializer validates the complete dynamic tip payload and builds
// exactly one plan for the fresh dense generation.
inline planned_overlay_materialization_result
materialize_overlay_chain_with_plan(
    overlay_chain const& chain,
    checked_chart_execution_plan_ref const& checked_base,
    bool* dense_materialization_completed = nullptr,
    overlay_payload_validation_stats* completed_payload_validation_stats =
        nullptr) {
  if (dense_materialization_completed != nullptr) {
    *dense_materialization_completed = false;
  }
  if (completed_payload_validation_stats != nullptr) {
    *completed_payload_validation_stats = {};
  }
  checked_base.assert_same(chain.base(), checked_base.plan());
  auto overlay = chain.tip();
  if (overlay.base != &chain.base()) {
    throw chart_execution_plan_mismatch(
        "overlay_chain: checked materialization base identity mismatch");
  }
  return materialize_overlay_grammar_with_plan(
      overlay, checked_base, dense_materialization_completed,
      completed_payload_validation_stats);
}


template <typename DenseMaterializationFinished>
  requires std::invocable<DenseMaterializationFinished&>
inline planned_overlay_materialization_result
materialize_overlay_chain_with_plan(
    overlay_chain const& chain,
    checked_chart_execution_plan_ref const& checked_base,
    bool* dense_materialization_completed,
    DenseMaterializationFinished&& dense_materialization_finished,
    overlay_payload_validation_stats* completed_payload_validation_stats =
        nullptr) {
  if (dense_materialization_completed != nullptr) {
    *dense_materialization_completed = false;
  }
  if (completed_payload_validation_stats != nullptr) {
    *completed_payload_validation_stats = {};
  }
  checked_base.assert_same(chain.base(), checked_base.plan());
  auto overlay = chain.tip();
  if (overlay.base != &chain.base()) {
    throw chart_execution_plan_mismatch(
        "overlay_chain: checked materialization base identity mismatch");
  }
  return materialize_overlay_grammar_with_plan(
      overlay, checked_base, dense_materialization_completed,
      std::forward<DenseMaterializationFinished>(
          dense_materialization_finished),
      completed_payload_validation_stats);
}

}  // namespace larch

#pragma once

#include <larch/chart_trim.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {

struct grammar_topology_fitch_score_stats {
  std::size_t reachable_internal_clades = 0;
  std::size_t recomputed_internal_clades = 0;
  std::size_t active_patterns = 0;

  bool operator==(grammar_topology_fitch_score_stats const&) const = default;
};

// Stateful exact scorer for a deterministic stream of direct-grammar
// topologies.
//
// For unordered unit-cost parsimony, a subtree contributes to its parent only
// its minimum score and the set of states attaining that minimum.  At an
// arbitrary-arity node, a parent state is optimal exactly when it occurs in the
// largest number of child optimal-state sets; the local score increment is the
// child count minus that maximum.  This is the generalized Fitch recurrence,
// and is algebraically equivalent to score_selected_topology's four-state
// Sankoff recurrence.
//
// The scorer caches one optimal-state byte per active pattern and clade.
// Consecutive topologies recompute only clades whose selected production or
// child state changed, while still traversing the concrete topology to sum its
// exact score.  No binary refinement or topology approximation is introduced.
class grammar_topology_fitch_scorer {
 public:
  grammar_topology_fitch_scorer(clade_grammar const& grammar,
                                site_pattern_set const& patterns,
                                chart_options options = {})
      : grammar_(grammar),
        patterns_(patterns),
        options_(options),
        cached_production_by_clade_(grammar.clades.size(), no_production),
        local_weighted_score_by_clade_(grammar.clades.size(), 0),
        optimal_state_by_clade_and_pattern_(
            checked_state_table_size(grammar.clades.size(), patterns), 0),
        grammar_generation_(grammar.execution_generation),
        grammar_root_clade_(grammar.root_clade),
        grammar_clade_count_(grammar.clades.size()),
        grammar_production_count_(grammar.productions.size()) {
    chart_multisite_detail::validate_multisite_inputs(grammar_, patterns_,
                                                      options_);
    initialize_active_patterns();
    initialize_leaf_states();
    invariant_constant_offset_ =
        chart_multisite_detail::invariant_constant_offset(patterns_, options_);
  }

  [[nodiscard]] std::size_t active_pattern_count() const noexcept {
    return active_pattern_indices_.size();
  }

  // Validates the supplied concrete topology before scoring it.
  [[nodiscard]] std::uint64_t score(
      grammar_topology const& topology,
      grammar_topology_fitch_score_stats* stats = nullptr) {
    (void)validate_grammar_topology(grammar_, topology);
    return score_enumerator_topology(topology, stats);
  }

  // Fast path for grammar_topology_enumerator output, whose construction is
  // already exact and validated.  Callers must not pass an arbitrary or
  // partially initialized topology here.
  [[nodiscard]] std::uint64_t score_enumerator_topology(
      grammar_topology const& topology,
      grammar_topology_fitch_score_stats* stats = nullptr) {
    ensure_grammar_shape_unchanged();
    if (topology.selected_production_by_clade.size() !=
            grammar_.clades.size() ||
        topology.used_production.size() != grammar_.productions.size()) {
      throw std::runtime_error(
          "grammar topology Fitch score: topology vector size mismatch");
    }

    grammar_topology_fitch_score_stats current;
    current.active_patterns = active_pattern_indices_.size();
    std::vector<std::uint8_t> visit_state(grammar_.clades.size(), 0);
    auto root =
        update_subtree(topology, grammar_.root_clade, visit_state, current);
    auto total = chart_multisite_detail::checked_add_u64(
        invariant_constant_offset_, root.weighted_subtree_score,
        "grammar topology Fitch score");

    if (options_.score_ua_edge) {
      auto const root_offset = state_offset(grammar_.root_clade);
      std::uint64_t ua_score = 0;
      for (std::size_t active = 0; active < active_pattern_indices_.size();
           ++active) {
        auto const mask =
            optimal_state_by_clade_and_pattern_[root_offset + active];
        auto const& pattern =
            patterns_.patterns[active_pattern_indices_[active]];
        for (std::uint8_t reference_state = 0;
             reference_state < nuc_state_count; ++reference_state) {
          auto const count = pattern.reference_state_counts[reference_state];
          if (count == 0 ||
              (mask & static_cast<std::uint8_t>(1U << reference_state)) != 0) {
            continue;
          }
          ua_score = chart_multisite_detail::checked_add_u64(
              ua_score, count, "grammar topology Fitch UA-edge score");
        }
      }
      total = chart_multisite_detail::checked_add_u64(
          total, ua_score, "grammar topology Fitch total score");
    }

    if (stats != nullptr) *stats = current;
    return total;
  }

 private:
  struct update_result {
    bool state_changed = false;
    std::uint64_t weighted_subtree_score = 0;
  };

  static std::size_t checked_state_table_size(
      std::size_t clade_count, site_pattern_set const& patterns) {
    std::size_t active_count = 0;
    for (auto const& pattern : patterns.patterns) {
      if (chart_multisite_detail::is_active_pattern(pattern)) ++active_count;
    }
    if (active_count != 0 &&
        clade_count > std::numeric_limits<std::size_t>::max() / active_count) {
      throw std::length_error(
          "grammar topology Fitch score: state table size overflow");
    }
    return clade_count * active_count;
  }

  void initialize_active_patterns() {
    active_pattern_indices_.reserve(patterns_.patterns.size());
    active_pattern_weights_.reserve(patterns_.patterns.size());
    for (std::size_t pattern_index = 0;
         pattern_index < patterns_.patterns.size(); ++pattern_index) {
      auto const& pattern = patterns_.patterns[pattern_index];
      if (options_.score_ua_edge) {
        chart_multisite_detail::validate_pattern_reference_counts(
            pattern, pattern_index);
      }
      if (!chart_multisite_detail::is_active_pattern(pattern)) continue;
      if (pattern.weight == 0) {
        throw std::runtime_error(
            "grammar topology Fitch score: active pattern has zero weight");
      }
      active_pattern_indices_.push_back(pattern_index);
      active_pattern_weights_.push_back(pattern.weight);
    }
  }

  [[nodiscard]] std::size_t state_offset(clade_id clade) const {
    return static_cast<std::size_t>(clade) * active_pattern_indices_.size();
  }

  void initialize_leaf_states() {
    for (std::size_t cid = 0; cid < grammar_.clades.size(); ++cid) {
      auto const& clade = grammar_.clades[cid];
      if (clade.taxa.size() != 1) continue;
      auto const taxon = clade.taxa.front();
      auto const offset = state_offset(static_cast<clade_id>(cid));
      for (std::size_t active = 0; active < active_pattern_indices_.size();
           ++active) {
        auto const& pattern =
            patterns_.patterns[active_pattern_indices_[active]];
        if (taxon >= pattern.state_by_taxon.size()) {
          throw std::runtime_error(
              "grammar topology Fitch score: leaf taxon out of pattern range");
        }
        auto const state = pattern.state_by_taxon[taxon];
        parsimony_chart_detail::validate_state(
            state, "grammar topology Fitch leaf state");
        optimal_state_by_clade_and_pattern_[offset + active] =
            static_cast<std::uint8_t>(1U << state);
      }
    }
  }

  void ensure_grammar_shape_unchanged() const {
    if (grammar_.execution_generation != grammar_generation_ ||
        grammar_.root_clade != grammar_root_clade_ ||
        grammar_.clades.size() != grammar_clade_count_ ||
        grammar_.productions.size() != grammar_production_count_) {
      throw std::runtime_error(
          "grammar topology Fitch score: grammar changed after scorer "
          "construction");
    }
  }

  update_result update_subtree(grammar_topology const& topology, clade_id clade,
                               std::vector<std::uint8_t>& visit_state,
                               grammar_topology_fitch_score_stats& stats) {
    if (clade == no_clade || clade >= grammar_.clades.size()) {
      throw std::runtime_error(
          "grammar topology Fitch score: reachable clade out of range");
    }
    if (visit_state[clade] == 1) {
      throw std::runtime_error(
          "grammar topology Fitch score: cycle in selected topology");
    }
    if (visit_state[clade] == 2) {
      throw std::runtime_error(
          "grammar topology Fitch score: selected topology reuses a clade");
    }
    visit_state[clade] = 1;

    if (grammar_.clades[clade].taxa.size() == 1) {
      visit_state[clade] = 2;
      return {};
    }

    auto const pid = topology.selected_production_by_clade[clade];
    if (pid == no_production || pid >= grammar_.productions.size()) {
      throw std::runtime_error(
          "grammar topology Fitch score: missing selected production");
    }
    auto const& production = grammar_.productions[pid];
    if (production.parent != clade || production.children.size() < 2) {
      throw std::runtime_error(
          "grammar topology Fitch score: invalid selected production");
    }

    ++stats.reachable_internal_clades;
    bool dirty = cached_production_by_clade_[clade] != pid;
    std::uint64_t subtree_score = 0;
    for (auto child : production.children) {
      auto child_result = update_subtree(topology, child, visit_state, stats);
      dirty = dirty || child_result.state_changed;
      subtree_score = chart_multisite_detail::checked_add_u64(
          subtree_score, child_result.weighted_subtree_score,
          "grammar topology Fitch subtree score");
    }

    if (dirty) {
      ++stats.recomputed_internal_clades;
      recompute_clade(production, clade);
      cached_production_by_clade_[clade] = pid;
    }
    subtree_score = chart_multisite_detail::checked_add_u64(
        subtree_score, local_weighted_score_by_clade_[clade],
        "grammar topology Fitch local score");
    visit_state[clade] = 2;
    return {dirty, subtree_score};
  }

  void recompute_clade(grammar_production const& production, clade_id clade) {
    auto const parent_offset = state_offset(clade);
    std::uint64_t local_weighted_score = 0;
    for (std::size_t active = 0; active < active_pattern_indices_.size();
         ++active) {
      std::array<std::uint32_t, nuc_state_count> state_counts{};
      for (auto child : production.children) {
        auto const child_mask =
            optimal_state_by_clade_and_pattern_[state_offset(child) + active];
        if (child_mask == 0) {
          throw std::runtime_error(
              "grammar topology Fitch score: empty child optimal-state set");
        }
        for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
          if ((child_mask & static_cast<std::uint8_t>(1U << state)) != 0) {
            ++state_counts[state];
          }
        }
      }

      auto const maximum =
          *std::max_element(state_counts.begin(), state_counts.end());
      if (maximum == 0 || maximum > production.children.size()) {
        throw std::runtime_error(
            "grammar topology Fitch score: invalid child state counts");
      }
      std::uint8_t parent_mask = 0;
      for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
        if (state_counts[state] == maximum) {
          parent_mask |= static_cast<std::uint8_t>(1U << state);
        }
      }
      optimal_state_by_clade_and_pattern_[parent_offset + active] = parent_mask;

      auto const local_cost =
          static_cast<std::uint64_t>(production.children.size() - maximum);
      if (local_cost != 0 &&
          active_pattern_weights_[active] >
              std::numeric_limits<std::uint64_t>::max() / local_cost) {
        throw std::runtime_error(
            "grammar topology Fitch score: weighted local score overflow");
      }
      local_weighted_score = chart_multisite_detail::checked_add_u64(
          local_weighted_score, local_cost * active_pattern_weights_[active],
          "grammar topology Fitch weighted local score");
    }
    local_weighted_score_by_clade_[clade] = local_weighted_score;
  }

  clade_grammar const& grammar_;
  site_pattern_set const& patterns_;
  chart_options options_;
  std::vector<std::size_t> active_pattern_indices_;
  std::vector<std::uint64_t> active_pattern_weights_;
  std::vector<production_id> cached_production_by_clade_;
  std::vector<std::uint64_t> local_weighted_score_by_clade_;
  std::vector<std::uint8_t> optimal_state_by_clade_and_pattern_;
  std::uint64_t invariant_constant_offset_ = 0;
  std::uint64_t grammar_generation_ = 0;
  clade_id grammar_root_clade_ = no_clade;
  std::size_t grammar_clade_count_ = 0;
  std::size_t grammar_production_count_ = 0;
};

}  // namespace larch

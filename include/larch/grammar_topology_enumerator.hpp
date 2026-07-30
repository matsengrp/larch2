#pragma once

#include <larch/grammar_topology.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace larch {

struct grammar_topology_range {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;

  [[nodiscard]] std::uint64_t size() const noexcept { return end - begin; }
  [[nodiscard]] bool empty() const noexcept { return begin == end; }
};

// Exact direct-grammar topology enumeration.
//
// Each grammar production is kept at its original arity.  The enumerator
// computes the sum/product parse count once, then un-ranks one ordinal at a
// time into a single reusable grammar_topology.  A streamed topology reference
// is therefore valid only for the duration of its callback.
//
// The referenced grammar must outlive this object and must not be mutated.
class grammar_topology_enumerator {
 public:
  explicit grammar_topology_enumerator(clade_grammar const& grammar)
      : grammar_(grammar),
        topology_count_by_clade_(grammar.clades.size(), 0),
        topology_count_by_production_(grammar.productions.size(), 0),
        ordered_productions_by_clade_(grammar.clades.size()),
        count_state_(grammar.clades.size(), 0),
        grammar_generation_(grammar.execution_generation),
        grammar_root_clade_(grammar.root_clade),
        grammar_clade_count_(grammar.clades.size()),
        grammar_production_count_(grammar.productions.size()) {
    validate_and_index_productions();
    for (std::size_t cid = 0; cid < grammar_.clades.size(); ++cid) {
      (void)count_clade(static_cast<clade_id>(cid));
    }
    topology_count_ = topology_count_by_clade_[grammar_.root_clade];
  }

  [[nodiscard]] std::uint64_t topology_count() const noexcept {
    return topology_count_;
  }

  [[nodiscard]] std::uint64_t topology_count(clade_id clade) const {
    ensure_grammar_shape_unchanged();
    if (clade == no_clade || clade >= topology_count_by_clade_.size()) {
      throw std::runtime_error(
          "grammar topology enumeration: clade out of range");
    }
    return topology_count_by_clade_[clade];
  }

  [[nodiscard]] std::uint64_t topology_count_for_production(
      production_id production) const {
    ensure_grammar_shape_unchanged();
    if (production == no_production ||
        production >= topology_count_by_production_.size()) {
      throw std::runtime_error(
          "grammar topology enumeration: production out of range");
    }
    return topology_count_by_production_[production];
  }

  [[nodiscard]] grammar_topology_range full_range() const noexcept {
    return {0, topology_count_};
  }

  // Contiguous quotient/remainder partitioning covers [0, topology_count())
  // exactly once.  Empty ranges are valid when worker_count > topology_count.
  [[nodiscard]] grammar_topology_range worker_partition(
      std::size_t worker_index, std::size_t worker_count) const {
    ensure_grammar_shape_unchanged();
    if (worker_count == 0) {
      throw std::runtime_error(
          "grammar topology enumeration: worker count must be positive");
    }
    if (worker_index >= worker_count) {
      throw std::runtime_error(
          "grammar topology enumeration: worker index out of range");
    }

    auto const workers = static_cast<std::uint64_t>(worker_count);
    auto const worker = static_cast<std::uint64_t>(worker_index);
    auto const quotient = topology_count_ / workers;
    auto const remainder = topology_count_ % workers;
    auto const begin =
        worker * quotient + std::min<std::uint64_t>(worker, remainder);
    auto const count = quotient + (worker < remainder ? 1 : 0);
    return {begin, begin + count};
  }

  [[nodiscard]] grammar_topology topology_at(std::uint64_t ordinal) const {
    ensure_grammar_shape_unchanged();
    if (ordinal >= topology_count_) {
      throw std::runtime_error(
          "grammar topology enumeration: topology ordinal out of range");
    }
    auto topology = make_empty_grammar_topology(grammar_);
    select_clade(grammar_.root_clade, ordinal, topology);
    return topology;
  }

  template <typename Callback>
  std::uint64_t stream(Callback&& callback) const {
    return stream(full_range(), std::forward<Callback>(callback));
  }

  // Callback is invoked as callback(global_ordinal, topology).  A void
  // callback consumes the whole range.  A bool callback may return false to
  // stop after consuming the current topology.
  template <typename Callback>
  std::uint64_t stream(grammar_topology_range range,
                       Callback&& callback) const {
    using result_type =
        std::invoke_result_t<Callback&, std::uint64_t, grammar_topology const&>;
    static_assert(
        std::same_as<result_type, void> || std::same_as<result_type, bool>,
        "grammar topology callback must return void or bool");

    ensure_grammar_shape_unchanged();
    if (range.begin > range.end || range.end > topology_count_) {
      throw std::runtime_error(
          "grammar topology enumeration: invalid topology range");
    }

    auto topology = make_empty_grammar_topology(grammar_);
    std::uint64_t emitted = 0;
    for (auto ordinal = range.begin; ordinal < range.end; ++ordinal) {
      std::fill(topology.selected_production_by_clade.begin(),
                topology.selected_production_by_clade.end(), no_production);
      std::fill(topology.used_production.begin(),
                topology.used_production.end(), false);
      select_clade(grammar_.root_clade, ordinal, topology);
      ++emitted;
      if constexpr (std::same_as<result_type, bool>) {
        if (!std::invoke(callback, ordinal,
                         static_cast<grammar_topology const&>(topology))) {
          break;
        }
      } else {
        std::invoke(callback, ordinal,
                    static_cast<grammar_topology const&>(topology));
      }
    }
    return emitted;
  }

 private:
  static std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs,
                                   std::string const& context) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
      throw std::runtime_error(
          "grammar topology enumeration: uint64 count overflow at " + context);
    }
    return lhs + rhs;
  }

  static std::uint64_t checked_multiply(std::uint64_t lhs, std::uint64_t rhs,
                                        std::string const& context) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
      throw std::runtime_error(
          "grammar topology enumeration: uint64 count overflow at " + context);
    }
    return lhs * rhs;
  }

  void ensure_grammar_shape_unchanged() const {
    if (grammar_.execution_generation != grammar_generation_ ||
        grammar_.root_clade != grammar_root_clade_ ||
        grammar_.clades.size() != grammar_clade_count_ ||
        grammar_.productions.size() != grammar_production_count_ ||
        grammar_.productions_by_parent.size() != grammar_clade_count_) {
      throw std::runtime_error(
          "grammar topology enumeration: grammar changed after enumerator "
          "construction");
    }
  }

  void validate_and_index_productions() {
    grammar_topology_detail::validate_basic_grammar_indices(grammar_);
    std::vector<std::size_t> occurrences(grammar_.productions.size(), 0);

    for (std::size_t cid = 0; cid < grammar_.clades.size(); ++cid) {
      auto const& taxa = grammar_.clades[cid].taxa;
      if (taxa.empty() || !std::is_sorted(taxa.begin(), taxa.end()) ||
          std::adjacent_find(taxa.begin(), taxa.end()) != taxa.end()) {
        throw std::runtime_error(
            "grammar topology enumeration: clade taxa must be nonempty, "
            "sorted, and unique");
      }

      auto& ordered = ordered_productions_by_clade_[cid];
      ordered = grammar_.productions_by_parent[cid];
      std::sort(ordered.begin(), ordered.end());
      if (std::adjacent_find(ordered.begin(), ordered.end()) != ordered.end()) {
        throw std::runtime_error(
            "grammar topology enumeration: duplicate production in "
            "productions_by_parent");
      }
      if (taxa.size() == 1 && !ordered.empty()) {
        throw std::runtime_error(
            "grammar topology enumeration: singleton clade has productions");
      }

      for (auto pid : ordered) {
        if (pid == no_production || pid >= grammar_.productions.size()) {
          throw std::runtime_error(
              "grammar topology enumeration: production id out of range");
        }
        auto const& production = grammar_.productions[pid];
        if (production.parent != cid) {
          throw std::runtime_error(
              "grammar topology enumeration: production parent mismatch");
        }
        if (production.children.size() < 2) {
          throw std::runtime_error(
              "grammar topology enumeration: direct production arity must be "
              "at least two");
        }
        detail::validate_production_partition(
            grammar_, static_cast<clade_id>(cid), production.children,
            "grammar topology enumeration: production " + std::to_string(pid));
        ++occurrences[pid];
      }
    }

    for (std::size_t pid = 0; pid < occurrences.size(); ++pid) {
      if (occurrences[pid] != 1) {
        throw std::runtime_error(
            "grammar topology enumeration: production must occur exactly "
            "once in productions_by_parent");
      }
    }
  }

  std::uint64_t count_clade(clade_id clade) {
    if (clade == no_clade || clade >= grammar_.clades.size()) {
      throw std::runtime_error(
          "grammar topology enumeration: clade out of range while counting");
    }
    if (count_state_[clade] == 1) {
      throw std::runtime_error(
          "grammar topology enumeration: cycle in grammar");
    }
    if (count_state_[clade] == 2) return topology_count_by_clade_[clade];
    count_state_[clade] = 1;

    auto const& productions = ordered_productions_by_clade_[clade];
    if (grammar_.clades[clade].taxa.size() == 1) {
      topology_count_by_clade_[clade] = 1;
      count_state_[clade] = 2;
      return 1;
    }
    if (productions.empty()) {
      throw std::runtime_error(
          "grammar topology enumeration: non-singleton clade has no "
          "productions");
    }

    std::uint64_t total = 0;
    for (auto pid : productions) {
      std::uint64_t production_count = 1;
      for (auto child : grammar_.productions[pid].children) {
        production_count =
            checked_multiply(production_count, count_clade(child),
                             "production " + std::to_string(pid));
      }
      topology_count_by_production_[pid] = production_count;
      total = checked_add(total, production_count,
                          "clade " + std::to_string(clade));
    }
    topology_count_by_clade_[clade] = total;
    count_state_[clade] = 2;
    return total;
  }

  void select_clade(clade_id clade, std::uint64_t ordinal,
                    grammar_topology& topology) const {
    if (grammar_.clades[clade].taxa.size() == 1) {
      if (ordinal != 0) {
        throw std::runtime_error(
            "grammar topology enumeration: leaf ordinal out of range");
      }
      return;
    }

    production_id selected = no_production;
    for (auto pid : ordered_productions_by_clade_[clade]) {
      auto const count = topology_count_by_production_[pid];
      if (ordinal < count) {
        selected = pid;
        break;
      }
      ordinal -= count;
    }
    if (selected == no_production) {
      throw std::runtime_error(
          "grammar topology enumeration: clade ordinal out of range");
    }

    topology.selected_production_by_clade[clade] = selected;
    topology.used_production[selected] = true;
    auto const& children = grammar_.productions[selected].children;
    for (std::size_t i = children.size(); i-- > 0;) {
      auto const child_count = topology_count_by_clade_[children[i]];
      auto const child_ordinal = ordinal % child_count;
      ordinal /= child_count;
      select_clade(children[i], child_ordinal, topology);
    }
    if (ordinal != 0) {
      throw std::runtime_error(
          "grammar topology enumeration: production ordinal out of range");
    }
  }

  clade_grammar const& grammar_;
  std::vector<std::uint64_t> topology_count_by_clade_;
  std::vector<std::uint64_t> topology_count_by_production_;
  std::vector<std::vector<production_id>> ordered_productions_by_clade_;
  std::vector<std::uint8_t> count_state_;
  std::uint64_t topology_count_ = 0;
  std::uint64_t grammar_generation_ = 0;
  clade_id grammar_root_clade_ = no_clade;
  std::size_t grammar_clade_count_ = 0;
  std::size_t grammar_production_count_ = 0;
};

}  // namespace larch

#pragma once

#include <larch/chart_scheduler.hpp>
#include <larch/grammar_topology.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/site_patterns.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace larch {

inline constexpr std::uint8_t no_chart_state =
    std::numeric_limits<std::uint8_t>::max();

struct single_site_outside_chart {
  // outside[clade][state] is the minimum cost of completing from clade to the
  // grammar root when clade presents state to its parent, excluding
  // inside[clade][state].
  std::vector<std::array<chart_cost, nuc_state_count>> outside;
  chart_cost global_min = chart_inf;

  // Number of non-binary production rows evaluated by this outside chart build.
  std::size_t multifurcation_productions_scored = 0;

  // The binary recurrence uses only fixed-size stack rows.  The checked
  // generic-arity recurrence owns reusable vector scratch for one production
  // dispatch.  These counters are incremented by the dispatch itself (not by
  // its callers), so tests can distinguish the two paths without allocator-
  // implementation-dependent instrumentation.
  struct recurrence_work_stats {
    std::size_t binary_stack_productions_scored = 0;
    std::size_t generic_reusable_productions_scored = 0;

    recurrence_work_stats& operator+=(recurrence_work_stats const& other) {
      binary_stack_productions_scored +=
          other.binary_stack_productions_scored;
      generic_reusable_productions_scored +=
          other.generic_reusable_productions_scored;
      return *this;
    }

    bool operator==(recurrence_work_stats const&) const = default;
  } recurrence_work;
};

using outside_recurrence_work_stats =
    single_site_outside_chart::recurrence_work_stats;

struct chart_production_choice {
  production_id production = no_production;
  std::uint8_t parent_state = no_chart_state;
  // Phase 4 is binary-only and aligned with grammar_production::children.
  std::array<std::uint8_t, 2> child_states{no_chart_state, no_chart_state};
  chart_cost cost = chart_inf;  // complete-tree cost for this local choice
};

struct chart_trim_mask {
  std::vector<std::array<bool, nuc_state_count>> keep_clade_state;
  std::vector<bool> keep_production;

  // Optional exact local choices that witness keep_production. Indexed by
  // production_id when chart_trim_options::store_optimal_choices is true;
  // empty on boolean-mask-only builds. Each stored entry realizes a globally
  // optimal complete tree.
  std::vector<std::vector<chart_production_choice>>
      optimal_choices_by_production;

  // Number of globally optimal local choices discovered. When choices are
  // stored this is also the number of stored chart_production_choice records.
  std::size_t kept_production_choice_count = 0;
  chart_cost global_min = chart_inf;
};

struct chart_trim_options {
  bool store_optimal_choices = true;
  // 0 means unlimited. If non-zero and store_optimal_choices=true,
  // build_single_site_trim_mask throws before storing more than this many
  // chart_production_choice records.
  std::size_t max_stored_optimal_choices = 0;
};

struct chart_traceback_result {
  std::vector<production_id> productions;

  // State presented by each clade root in the traced optimal topology. Entries
  // for clades not used by the traceback are no_chart_state.
  std::vector<std::uint8_t> root_state_by_clade;

  chart_cost score = chart_inf;
};

namespace chart_trim_detail {

inline std::array<bool, nuc_state_count> false_state_mask() {
  std::array<bool, nuc_state_count> row{};
  row.fill(false);
  return row;
}

inline std::vector<clade_id> clades_by_decreasing_size(
    clade_grammar const& grammar) {
  std::vector<clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    auto const& ltaxa = grammar.clades[lhs].taxa;
    auto const& rtaxa = grammar.clades[rhs].taxa;
    if (ltaxa.size() != rtaxa.size()) return ltaxa.size() > rtaxa.size();
    return lhs < rhs;
  });
  return order;
}

inline void validate_production_indices(clade_grammar const& grammar) {
  for (clade_id parent = 0; parent < grammar.productions_by_parent.size();
       ++parent) {
    for (auto pid : grammar.productions_by_parent[parent]) {
      if (pid == no_production || pid >= grammar.productions.size()) {
        throw std::runtime_error(
            "chart trim: productions_by_parent contains invalid production id");
      }
      if (grammar.productions[pid].parent != parent) {
        throw std::runtime_error(
            "chart trim: productions_by_parent contains mismatched parent");
      }
    }
  }

  for (clade_id child = 0; child < grammar.productions_by_child.size();
       ++child) {
    for (auto pid : grammar.productions_by_child[child]) {
      if (pid == no_production || pid >= grammar.productions.size()) {
        throw std::runtime_error(
            "chart trim: productions_by_child contains invalid production id");
      }
      auto const& children = grammar.productions[pid].children;
      if (std::find(children.begin(), children.end(), child) ==
          children.end()) {
        throw std::runtime_error(
            "chart trim: productions_by_child contains mismatched child");
      }
    }
  }

  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& prod = grammar.productions[pid];
    if (prod.parent == no_clade || prod.parent >= grammar.clades.size()) {
      throw std::runtime_error("chart trim: production parent out of range");
    }
    auto const& by_parent = grammar.productions_by_parent[prod.parent];
    if (std::find(by_parent.begin(), by_parent.end(), pid) == by_parent.end()) {
      throw std::runtime_error(
          "chart trim: production missing from productions_by_parent");
    }

    std::vector<clade_id> unique_children = prod.children;
    std::sort(unique_children.begin(), unique_children.end());
    unique_children.erase(
        std::unique(unique_children.begin(), unique_children.end()),
        unique_children.end());
    for (auto child : unique_children) {
      if (child == no_clade || child >= grammar.clades.size()) {
        throw std::runtime_error("chart trim: production child out of range");
      }
      auto const& by_child = grammar.productions_by_child[child];
      if (std::find(by_child.begin(), by_child.end(), pid) == by_child.end()) {
        throw std::runtime_error(
            "chart trim: production missing from productions_by_child");
      }
    }
  }
}

inline void validate_chart_shapes(clade_grammar const& grammar,
                                  single_site_chart const& chart) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  validate_production_indices(grammar);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error("chart trim: root clade out of range");
  }
  if (chart.inside.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "chart trim: inside chart size does not match grammar clade count");
  }
  if (!chart.optimal_choices.empty() &&
      chart.optimal_choices.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "chart trim: trace choice size does not match grammar clade count");
  }
}

inline void validate_outside_shapes(clade_grammar const& grammar,
                                    single_site_chart const& chart,
                                    single_site_outside_chart const& outside) {
  validate_chart_shapes(grammar, chart);
  if (outside.outside.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "chart trim: outside chart size does not match grammar clade count");
  }
}

// Plan-backed chart consumers are trusted structural recurrences.  The plan
// was checked when it was built; these boundary checks deliberately validate
// only the dynamic sidecars supplied by the caller.
inline void validate_chart_shapes(chart_execution_plan const& plan,
                                  single_site_chart const& chart) {
  plan.assert_valid();
  if (chart.inside.size() != plan.clades().size()) {
    throw std::runtime_error(
        "chart trim: inside chart size does not match grammar clade count");
  }
  if (!chart.optimal_choices.empty() &&
      chart.optimal_choices.size() != plan.clades().size()) {
    throw std::runtime_error(
        "chart trim: trace choice size does not match grammar clade count");
  }
}

inline void validate_outside_shapes(chart_execution_plan const& plan,
                                    single_site_chart const& chart,
                                    single_site_outside_chart const& outside) {
  validate_chart_shapes(plan, chart);
  if (outside.outside.size() != plan.clades().size()) {
    throw std::runtime_error(
        "chart trim: outside chart size does not match grammar clade count");
  }
}

inline void validate_binary_production_for_trim(clade_grammar const& grammar,
                                                grammar_production const& prod,
                                                production_id pid) {
  if (prod.children.size() != 2) {
    throw std::runtime_error("chart trim: production " + std::to_string(pid) +
                             " has arity " +
                             std::to_string(prod.children.size()) +
                             "; Phase 4 supports binary productions only");
  }
  parsimony_chart_detail::validate_binary_production_partition(grammar, prod,
                                                               pid);
}

inline void validate_binary_production_for_trim(
    chart_execution_plan const& plan,
    chart_plan_production_descriptor const& prod, production_id pid) {
  if (!prod.is_binary()) {
    throw std::runtime_error("chart trim: production " + std::to_string(pid) +
                             " has arity " +
                             std::to_string(prod.child_count) +
                             "; Phase 4 supports binary productions only");
  }
  (void)plan;
}

inline chart_cost add3(chart_cost a, chart_cost b, chart_cost c) {
  return parsimony_chart_detail::saturated_add(
      parsimony_chart_detail::saturated_add(a, b), c);
}

using outside_chart_row = std::array<chart_cost, nuc_state_count>;
using binary_outside_rows = std::array<outside_chart_row, 2>;

struct generic_outside_recurrence_scratch {
  std::vector<outside_chart_row> result;
  std::vector<chart_cost> child_best;
  std::vector<chart_cost> prefix;
  std::vector<chart_cost> suffix;
};

template <class Children, class RowProvider, class TransitionCost>
inline binary_outside_rows combine_binary_production_outside_rows_impl(
    Children const& children, std::uint8_t parent_state,
    chart_cost parent_outside, RowProvider&& inside_provider,
    TransitionCost&& transition_cost) {
  parsimony_chart_detail::validate_state(parent_state, "outside parent state");
  if (children.size() != 2) {
    throw std::runtime_error(
        "outside recurrence: binary path requires exactly two children");
  }

  binary_outside_rows result{
      parsimony_chart_detail::make_inf_row(),
      parsimony_chart_detail::make_inf_row()};
  if (parent_outside >= chart_inf) return result;

  std::array<chart_cost, 2> child_best{chart_inf, chart_inf};
  for (std::size_t child_i = 0; child_i < 2; ++child_i) {
    auto const& child_row = inside_provider(children[child_i]);
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      child_best[child_i] = std::min(
          child_best[child_i],
          parsimony_chart_detail::saturated_add(
              child_row[child_state], transition_cost(parent_state,
                                                       child_state)));
    }
  }

  for (std::size_t child_i = 0; child_i < 2; ++child_i) {
    // Compute the sibling term directly.  Never derive it by subtracting this
    // child's contribution from a possibly saturated production total.
    auto const sibling_i = std::size_t{1} - child_i;
    auto sibling_context = parsimony_chart_detail::saturated_add(
        parent_outside, child_best[sibling_i]);
    if (sibling_context >= chart_inf) continue;
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      result[child_i][child_state] = parsimony_chart_detail::saturated_add(
          sibling_context, transition_cost(parent_state, child_state));
    }
  }
  return result;
}

template <class Children, class RowProvider, class TransitionCost>
inline std::span<outside_chart_row const>
combine_generic_production_outside_rows_into(
    Children const& children, std::uint8_t parent_state,
    chart_cost parent_outside, RowProvider&& inside_provider,
    TransitionCost&& transition_cost,
    generic_outside_recurrence_scratch& scratch) {
  parsimony_chart_detail::validate_state(parent_state, "outside parent state");
  scratch.result.assign(children.size(),
                        parsimony_chart_detail::make_inf_row());
  if (parent_outside >= chart_inf) {
    return std::span<outside_chart_row const>{scratch.result};
  }

  scratch.child_best.assign(children.size(), chart_inf);
  for (std::size_t child_i = 0; child_i < children.size(); ++child_i) {
    auto const& child_row = inside_provider(children[child_i]);
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      scratch.child_best[child_i] = std::min(
          scratch.child_best[child_i],
          parsimony_chart_detail::saturated_add(
              child_row[child_state], transition_cost(parent_state,
                                                       child_state)));
    }
  }

  scratch.prefix.assign(children.size() + 1, chart_cost{0});
  scratch.suffix.assign(children.size() + 1, chart_cost{0});
  for (std::size_t child_i = 0; child_i < children.size(); ++child_i) {
    scratch.prefix[child_i + 1] =
        parsimony_chart_detail::saturated_add(
            scratch.prefix[child_i], scratch.child_best[child_i]);
  }
  for (std::size_t child_i = children.size(); child_i-- > 0;) {
    scratch.suffix[child_i] =
        parsimony_chart_detail::saturated_add(
            scratch.child_best[child_i], scratch.suffix[child_i + 1]);
  }

  for (std::size_t child_i = 0; child_i < children.size(); ++child_i) {
    auto sibling_context = parsimony_chart_detail::saturated_add(
        parent_outside,
        parsimony_chart_detail::saturated_add(scratch.prefix[child_i],
                                              scratch.suffix[child_i + 1]));
    if (sibling_context >= chart_inf) continue;
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      scratch.result[child_i][child_state] =
          parsimony_chart_detail::saturated_add(
              sibling_context, transition_cost(parent_state, child_state));
    }
  }
  return std::span<outside_chart_row const>{scratch.result};
}

template <class Production, class RowProvider>
inline binary_outside_rows combine_binary_production_outside_rows(
    Production const& prod, std::uint8_t parent_state,
    chart_cost parent_outside, RowProvider&& inside_provider) {
  auto transition_cost = [](std::uint8_t from, std::uint8_t to) {
    return parsimony_chart_detail::transition_cost(from, to);
  };
  return combine_binary_production_outside_rows_impl(
      prod.children, parent_state, parent_outside,
      std::forward<RowProvider>(inside_provider), transition_cost);
}

template <class RowProvider>
inline binary_outside_rows combine_binary_production_outside_rows(
    chart_execution_plan const& plan, std::span<clade_id const> children,
    std::uint8_t parent_state, chart_cost parent_outside,
    RowProvider&& inside_provider) {
  auto transition_cost = [&](std::uint8_t from, std::uint8_t to) {
    return static_cast<chart_cost>(plan.transition_cost(from, to));
  };
  return combine_binary_production_outside_rows_impl(
      children, parent_state, parent_outside,
      std::forward<RowProvider>(inside_provider), transition_cost);
}

template <class Production, class RowProvider>
inline std::vector<outside_chart_row> combine_production_outside_rows(
    Production const& prod, std::uint8_t parent_state,
    chart_cost parent_outside, RowProvider&& inside_provider) {
  auto transition_cost = [](std::uint8_t from, std::uint8_t to) {
    return parsimony_chart_detail::transition_cost(from, to);
  };
  generic_outside_recurrence_scratch scratch;
  (void)combine_generic_production_outside_rows_into(
      prod.children, parent_state, parent_outside,
      std::forward<RowProvider>(inside_provider), transition_cost, scratch);
  return std::move(scratch.result);
}

template <class RowProvider>
inline std::vector<outside_chart_row> combine_production_outside_rows(
    chart_execution_plan const& plan, std::span<clade_id const> children,
    std::uint8_t parent_state, chart_cost parent_outside,
    RowProvider&& inside_provider) {
  auto transition_cost = [&](std::uint8_t from, std::uint8_t to) {
    return static_cast<chart_cost>(plan.transition_cost(from, to));
  };
  generic_outside_recurrence_scratch scratch;
  (void)combine_generic_production_outside_rows_into(
      children, parent_state, parent_outside,
      std::forward<RowProvider>(inside_provider), transition_cost, scratch);
  return std::move(scratch.result);
}

template <class Children, class RowProvider, class TransitionCost,
          class RowConsumer>
inline void scatter_production_outside_rows_impl(
    Children const& children, outside_chart_row const& parent_outside,
    RowProvider&& inside_provider, TransitionCost&& transition_cost,
    RowConsumer&& consume_row, outside_recurrence_work_stats& work,
    generic_outside_recurrence_scratch* reusable_scratch = nullptr) {
  // consume_row must consume each row synchronously. Binary rows live on this
  // stack frame and generic rows live in local or caller-owned reusable
  // scratch; no result or cache stores a view into either lifetime.
  if (children.size() == 2) {
    ++work.binary_stack_productions_scored;
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      auto const base = parent_outside[parent_state];
      if (base >= chart_inf) continue;
      auto const rows = combine_binary_production_outside_rows_impl(
          children, parent_state, base, inside_provider, transition_cost);
      consume_row(std::size_t{0}, rows[0]);
      consume_row(std::size_t{1}, rows[1]);
    }
    return;
  }

  ++work.generic_reusable_productions_scored;
  generic_outside_recurrence_scratch local_scratch;
  auto& scratch =
      reusable_scratch != nullptr ? *reusable_scratch : local_scratch;
  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    auto const base = parent_outside[parent_state];
    if (base >= chart_inf) continue;
    auto const rows = combine_generic_production_outside_rows_into(
        children, parent_state, base, inside_provider, transition_cost,
        scratch);
    for (std::size_t child_i = 0; child_i < rows.size(); ++child_i) {
      consume_row(child_i, rows[child_i]);
    }
  }
}

template <class Production, class RowProvider, class RowConsumer>
inline void scatter_production_outside_rows(
    Production const& prod, outside_chart_row const& parent_outside,
    RowProvider&& inside_provider, RowConsumer&& consume_row,
    outside_recurrence_work_stats& work) {
  auto transition_cost = [](std::uint8_t from, std::uint8_t to) {
    return parsimony_chart_detail::transition_cost(from, to);
  };
  scatter_production_outside_rows_impl(
      prod.children, parent_outside, std::forward<RowProvider>(inside_provider),
      transition_cost, std::forward<RowConsumer>(consume_row), work);
}

template <class RowProvider, class RowConsumer>
inline void scatter_production_outside_rows(
    chart_execution_plan const& plan, std::span<clade_id const> children,
    outside_chart_row const& parent_outside, RowProvider&& inside_provider,
    RowConsumer&& consume_row, outside_recurrence_work_stats& work) {
  auto transition_cost = [&](std::uint8_t from, std::uint8_t to) {
    return static_cast<chart_cost>(plan.transition_cost(from, to));
  };
  scatter_production_outside_rows_impl(
      children, parent_outside, std::forward<RowProvider>(inside_provider),
      transition_cost, std::forward<RowConsumer>(consume_row), work);
}

template <class RowProvider, class RowConsumer>
inline void scatter_production_outside_rows(
    chart_execution_plan const& plan, std::span<clade_id const> children,
    outside_chart_row const& parent_outside, RowProvider&& inside_provider,
    RowConsumer&& consume_row, outside_recurrence_work_stats& work,
    generic_outside_recurrence_scratch& reusable_scratch) {
  auto transition_cost = [&](std::uint8_t from, std::uint8_t to) {
    return static_cast<chart_cost>(plan.transition_cost(from, to));
  };
  scatter_production_outside_rows_impl(
      children, parent_outside, std::forward<RowProvider>(inside_provider),
      transition_cost, std::forward<RowConsumer>(consume_row), work,
      &reusable_scratch);
}

inline chart_cost production_choice_inside_cost(
    clade_grammar const& grammar, single_site_chart const& chart,
    grammar_production const& prod, std::uint8_t parent_state,
    std::array<std::uint8_t, 2> child_states) {
  parsimony_chart_detail::validate_state(parent_state,
                                         "production parent state");
  chart_cost total = 0;
  for (std::size_t child_i = 0; child_i < 2; ++child_i) {
    auto child = prod.children[child_i];
    if (child == no_clade || child >= chart.inside.size()) {
      throw std::runtime_error(
          "chart trim: production child clade out of range");
    }
    auto child_state = child_states[child_i];
    parsimony_chart_detail::validate_state(child_state,
                                           "production child state");
    auto term = parsimony_chart_detail::saturated_add(
        chart.inside[child][child_state],
        parsimony_chart_detail::transition_cost(parent_state, child_state));
    total = parsimony_chart_detail::saturated_add(total, term);
  }
  (void)grammar;
  return total;
}

inline chart_cost compute_global_min(clade_grammar const& grammar,
                                     single_site_chart const& chart,
                                     single_site_outside_chart const& outside) {
  auto root = grammar.root_clade;
  chart_cost best = chart_inf;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    auto total = parsimony_chart_detail::saturated_add(
        chart.inside[root][state], outside.outside[root][state]);
    best = std::min(best, total);
  }
  return best;
}

inline bool is_globally_optimal_state(clade_grammar const& grammar,
                                      single_site_chart const& chart,
                                      single_site_outside_chart const& outside,
                                      clade_id clade, std::uint8_t state) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("chart trim: clade id out of range");
  }
  parsimony_chart_detail::validate_state(state, "clade state");
  if (outside.global_min >= chart_inf) return false;
  auto total = parsimony_chart_detail::saturated_add(
      chart.inside[clade][state], outside.outside[clade][state]);
  return total < chart_inf && total == outside.global_min;
}

inline void append_unique_choice(std::vector<chart_production_choice>& choices,
                                 chart_production_choice choice) {
  auto same = [&](chart_production_choice const& existing) {
    return existing.production == choice.production &&
           existing.parent_state == choice.parent_state &&
           existing.child_states == choice.child_states &&
           existing.cost == choice.cost;
  };
  if (std::none_of(choices.begin(), choices.end(), same))
    choices.push_back(choice);
}

inline std::vector<chart_production_choice> globally_optimal_choices_for_state(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside, clade_id clade,
    std::uint8_t parent_state) {
  parsimony_chart_detail::validate_state(parent_state, "parent state");

  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("chart trim: clade id out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1) return {};
  if (!is_globally_optimal_state(grammar, chart, outside, clade,
                                 parent_state)) {
    return {};
  }

  std::vector<chart_production_choice> choices;

  auto consider = [&](production_id pid,
                      std::array<std::uint8_t, 2> child_states) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("chart trim: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "chart trim: production parent does not match requested clade");
    }
    validate_binary_production_for_trim(grammar, prod, pid);

    auto local = production_choice_inside_cost(grammar, chart, prod,
                                               parent_state, child_states);
    if (local >= chart_inf || local != chart.inside[clade][parent_state])
      return;

    auto complete = parsimony_chart_detail::saturated_add(
        outside.outside[clade][parent_state], local);
    if (complete >= chart_inf || complete != outside.global_min) return;

    for (std::size_t child_i = 0; child_i < 2; ++child_i) {
      if (!is_globally_optimal_state(grammar, chart, outside,
                                     prod.children[child_i],
                                     child_states[child_i])) {
        return;
      }
    }

    append_unique_choice(
        choices,
        chart_production_choice{pid, parent_state, child_states, complete});
  };

  if (chart.has_trace()) {
    for (auto const& choice : chart.optimal_choices[clade][parent_state]) {
      consider(choice.production, choice.child_states);
    }
  } else {
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      validate_binary_production_for_trim(grammar, prod, pid);
      for (std::uint8_t left_state = 0; left_state < nuc_state_count;
           ++left_state) {
        for (std::uint8_t right_state = 0; right_state < nuc_state_count;
             ++right_state) {
          consider(pid, {left_state, right_state});
        }
      }
    }
  }

  std::sort(choices.begin(), choices.end(),
            [](chart_production_choice const& lhs,
               chart_production_choice const& rhs) {
              if (lhs.production != rhs.production)
                return lhs.production < rhs.production;
              if (lhs.child_states[0] != rhs.child_states[0])
                return lhs.child_states[0] < rhs.child_states[0];
              if (lhs.child_states[1] != rhs.child_states[1])
                return lhs.child_states[1] < rhs.child_states[1];
              return lhs.parent_state < rhs.parent_state;
            });
  return choices;
}

inline std::vector<std::uint8_t> globally_optimal_root_states(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  std::vector<std::uint8_t> states;
  if (outside.global_min >= chart_inf) return states;
  auto root = grammar.root_clade;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    auto total = parsimony_chart_detail::saturated_add(
        chart.inside[root][state], outside.outside[root][state]);
    if (total < chart_inf && total == outside.global_min)
      states.push_back(state);
  }
  return states;
}

template <typename ChoiceFn>
chart_traceback_result optimal_single_site_traceback_impl(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside, ChoiceFn&& choose_index) {
  validate_outside_shapes(grammar, chart, outside);
  if (compute_global_min(grammar, chart, outside) != outside.global_min) {
    throw std::runtime_error("chart trim: outside global optimum is stale");
  }

  auto root_states = globally_optimal_root_states(grammar, chart, outside);
  if (root_states.empty()) {
    throw std::runtime_error("chart trim: no finite optimal root state");
  }

  chart_traceback_result result;
  result.root_state_by_clade.assign(grammar.clades.size(), no_chart_state);
  result.score = outside.global_min;

  std::vector<bool> expanded(grammar.clades.size(), false);

  auto trace_clade = [&](auto&& self, clade_id clade,
                         std::uint8_t state) -> void {
    if (clade == no_clade || clade >= grammar.clades.size()) {
      throw std::runtime_error("chart trim: traceback clade out of range");
    }
    parsimony_chart_detail::validate_state(state, "traceback state");

    if (result.root_state_by_clade[clade] != no_chart_state) {
      if (result.root_state_by_clade[clade] != state) {
        throw std::runtime_error(
            "chart trim: traceback encountered conflicting states for clade");
      }
      return;
    }
    result.root_state_by_clade[clade] = state;

    if (grammar.clades[clade].taxa.size() == 1) return;
    if (expanded[clade]) return;
    expanded[clade] = true;

    auto choices = globally_optimal_choices_for_state(grammar, chart, outside,
                                                      clade, state);
    if (choices.empty()) {
      throw std::runtime_error(
          "chart trim: non-leaf optimal state has no optimal production "
          "choice");
    }
    auto choice_index = choose_index(choices.size());
    if (choice_index >= choices.size()) {
      throw std::runtime_error(
          "chart trim: traceback choice index out of range");
    }
    auto const& choice = choices[choice_index];
    result.productions.push_back(choice.production);

    auto const& prod = grammar.productions[choice.production];
    validate_binary_production_for_trim(grammar, prod, choice.production);
    for (std::size_t child_i = 0; child_i < 2; ++child_i) {
      self(self, prod.children[child_i], choice.child_states[child_i]);
    }
  };

  auto root_choice_index = choose_index(root_states.size());
  if (root_choice_index >= root_states.size()) {
    throw std::runtime_error(
        "chart trim: traceback root choice index out of range");
  }
  trace_clade(trace_clade, grammar.root_clade, root_states[root_choice_index]);
  return result;
}

inline chart_cost production_choice_inside_cost(
    chart_execution_plan const& plan, single_site_chart const& chart,
    chart_plan_production_descriptor const& prod,
    std::uint8_t parent_state,
    std::array<std::uint8_t, 2> child_states) {
  parsimony_chart_detail::validate_state(parent_state,
                                         "production parent state");
  chart_cost total = 0;
  for (std::size_t child_i = 0; child_i < 2; ++child_i) {
    auto child = prod.binary_children[child_i];
    if (child == no_clade || child >= chart.inside.size()) {
      throw std::runtime_error(
          "chart trim: production child clade out of range");
    }
    auto child_state = child_states[child_i];
    parsimony_chart_detail::validate_state(child_state,
                                           "production child state");
    auto term = parsimony_chart_detail::saturated_add(
        chart.inside[child][child_state],
        static_cast<chart_cost>(
            plan.transition_cost(parent_state, child_state)));
    total = parsimony_chart_detail::saturated_add(total, term);
  }
  return total;
}

inline chart_cost compute_global_min(chart_execution_plan const& plan,
                                     single_site_chart const& chart,
                                     single_site_outside_chart const& outside) {
  auto root = plan.root_clade();
  chart_cost best = chart_inf;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    auto total = parsimony_chart_detail::saturated_add(
        chart.inside[root][state], outside.outside[root][state]);
    best = std::min(best, total);
  }
  return best;
}

inline bool is_globally_optimal_state(
    chart_execution_plan const& plan, single_site_chart const& chart,
    single_site_outside_chart const& outside, clade_id clade,
    std::uint8_t state) {
  if (clade == no_clade || clade >= plan.clades().size()) {
    throw std::runtime_error("chart trim: clade id out of range");
  }
  parsimony_chart_detail::validate_state(state, "clade state");
  if (outside.global_min >= chart_inf) return false;
  auto total = parsimony_chart_detail::saturated_add(
      chart.inside[clade][state], outside.outside[clade][state]);
  return total < chart_inf && total == outside.global_min;
}

inline std::vector<chart_production_choice> globally_optimal_choices_for_state(
    chart_execution_plan const& plan, single_site_chart const& chart,
    single_site_outside_chart const& outside, clade_id clade,
    std::uint8_t parent_state) {
  parsimony_chart_detail::validate_state(parent_state, "parent state");
  if (clade == no_clade || clade >= plan.clades().size()) {
    throw std::runtime_error("chart trim: clade id out of range");
  }
  if (plan.clade(clade).is_leaf()) return {};
  if (!is_globally_optimal_state(plan, chart, outside, clade,
                                 parent_state)) {
    return {};
  }

  std::vector<chart_production_choice> choices;
  auto consider = [&](production_id pid,
                      std::array<std::uint8_t, 2> child_states) {
    auto const& prod = plan.production(pid);
    if (prod.parent != clade) {
      throw std::runtime_error(
          "chart trim: production parent does not match requested clade");
    }
    validate_binary_production_for_trim(plan, prod, pid);

    auto local = production_choice_inside_cost(plan, chart, prod, parent_state,
                                               child_states);
    if (local >= chart_inf || local != chart.inside[clade][parent_state]) {
      return;
    }
    auto complete = parsimony_chart_detail::saturated_add(
        outside.outside[clade][parent_state], local);
    if (complete >= chart_inf || complete != outside.global_min) return;

    for (std::size_t child_i = 0; child_i < 2; ++child_i) {
      if (!is_globally_optimal_state(plan, chart, outside,
                                     prod.binary_children[child_i],
                                     child_states[child_i])) {
        return;
      }
    }
    append_unique_choice(
        choices,
        chart_production_choice{pid, parent_state, child_states, complete});
  };

  if (chart.has_trace()) {
    for (auto const& choice : chart.optimal_choices[clade][parent_state]) {
      consider(choice.production, choice.child_states);
    }
  } else {
    for (auto pid : plan.productions_for_parent(clade)) {
      auto const& prod = plan.production(pid);
      validate_binary_production_for_trim(plan, prod, pid);
      for (std::uint8_t left_state = 0; left_state < nuc_state_count;
           ++left_state) {
        for (std::uint8_t right_state = 0; right_state < nuc_state_count;
             ++right_state) {
          consider(pid, {left_state, right_state});
        }
      }
    }
  }

  std::sort(choices.begin(), choices.end(),
            [](chart_production_choice const& lhs,
               chart_production_choice const& rhs) {
              if (lhs.production != rhs.production)
                return lhs.production < rhs.production;
              if (lhs.child_states[0] != rhs.child_states[0])
                return lhs.child_states[0] < rhs.child_states[0];
              if (lhs.child_states[1] != rhs.child_states[1])
                return lhs.child_states[1] < rhs.child_states[1];
              return lhs.parent_state < rhs.parent_state;
            });
  return choices;
}

// The deterministic traceback consumes only the lexicographically first
// globally optimal choice.  Building, deduplicating, and sorting the complete
// choice vector at every visited clade is needlessly expensive for that
// contract (and creates substantial allocator contention when exact setups
// trace many patterns concurrently).  Keep the exhaustive routine above for
// callers that need every choice and select the same first element here with a
// constant-space min reduction.
inline std::optional<chart_production_choice>
first_globally_optimal_choice_for_state(
    chart_execution_plan const& plan, single_site_chart const& chart,
    single_site_outside_chart const& outside, clade_id clade,
    std::uint8_t parent_state) {
  parsimony_chart_detail::validate_state(parent_state, "parent state");
  if (clade == no_clade || clade >= plan.clades().size()) {
    throw std::runtime_error("chart trim: clade id out of range");
  }
  if (plan.clade(clade).is_leaf() ||
      !is_globally_optimal_state(plan, chart, outside, clade, parent_state)) {
    return std::nullopt;
  }

  auto choice_less = [](chart_production_choice const& lhs,
                        chart_production_choice const& rhs) {
    if (lhs.production != rhs.production)
      return lhs.production < rhs.production;
    if (lhs.child_states[0] != rhs.child_states[0])
      return lhs.child_states[0] < rhs.child_states[0];
    if (lhs.child_states[1] != rhs.child_states[1])
      return lhs.child_states[1] < rhs.child_states[1];
    return lhs.parent_state < rhs.parent_state;
  };

  std::optional<chart_production_choice> first;
  auto consider = [&](production_id pid,
                      std::array<std::uint8_t, 2> child_states) {
    auto const& prod = plan.production(pid);
    if (prod.parent != clade) {
      throw std::runtime_error(
          "chart trim: production parent does not match requested clade");
    }
    validate_binary_production_for_trim(plan, prod, pid);

    auto const local = production_choice_inside_cost(
        plan, chart, prod, parent_state, child_states);
    if (local >= chart_inf || local != chart.inside[clade][parent_state]) {
      return;
    }
    auto const complete = parsimony_chart_detail::saturated_add(
        outside.outside[clade][parent_state], local);
    if (complete >= chart_inf || complete != outside.global_min) return;
    for (std::size_t child_i = 0; child_i < 2; ++child_i) {
      if (!is_globally_optimal_state(plan, chart, outside,
                                     prod.binary_children[child_i],
                                     child_states[child_i])) {
        return;
      }
    }

    chart_production_choice candidate{pid, parent_state, child_states,
                                      complete};
    if (!first || choice_less(candidate, *first)) first = candidate;
  };

  if (chart.has_trace()) {
    for (auto const& choice : chart.optimal_choices[clade][parent_state]) {
      consider(choice.production, choice.child_states);
    }
  } else {
    for (auto pid : plan.productions_for_parent(clade)) {
      auto const& prod = plan.production(pid);
      validate_binary_production_for_trim(plan, prod, pid);
      for (std::uint8_t left_state = 0; left_state < nuc_state_count;
           ++left_state) {
        for (std::uint8_t right_state = 0; right_state < nuc_state_count;
             ++right_state) {
          consider(pid, {left_state, right_state});
        }
      }
    }
  }
  return first;
}

inline std::vector<std::uint8_t> globally_optimal_root_states(
    chart_execution_plan const& plan, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  std::vector<std::uint8_t> states;
  if (outside.global_min >= chart_inf) return states;
  auto root = plan.root_clade();
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    auto total = parsimony_chart_detail::saturated_add(
        chart.inside[root][state], outside.outside[root][state]);
    if (total < chart_inf && total == outside.global_min) {
      states.push_back(state);
    }
  }
  return states;
}

template <typename ChoiceFn>
chart_traceback_result optimal_single_site_traceback_impl(
    chart_execution_plan const& plan, single_site_chart const& chart,
    single_site_outside_chart const& outside, ChoiceFn&& choose_index) {
  validate_outside_shapes(plan, chart, outside);
  if (compute_global_min(plan, chart, outside) != outside.global_min) {
    throw std::runtime_error("chart trim: outside global optimum is stale");
  }

  auto root_states = globally_optimal_root_states(plan, chart, outside);
  if (root_states.empty()) {
    throw std::runtime_error("chart trim: no finite optimal root state");
  }

  chart_traceback_result result;
  result.root_state_by_clade.assign(plan.clades().size(), no_chart_state);
  result.score = outside.global_min;
  std::vector<bool> expanded(plan.clades().size(), false);

  auto trace_clade = [&](auto&& self, clade_id clade,
                         std::uint8_t state) -> void {
    if (clade == no_clade || clade >= plan.clades().size()) {
      throw std::runtime_error("chart trim: traceback clade out of range");
    }
    parsimony_chart_detail::validate_state(state, "traceback state");
    if (result.root_state_by_clade[clade] != no_chart_state) {
      if (result.root_state_by_clade[clade] != state) {
        throw std::runtime_error(
            "chart trim: traceback encountered conflicting states for clade");
      }
      return;
    }
    result.root_state_by_clade[clade] = state;

    if (plan.clade(clade).is_leaf()) return;
    if (expanded[clade]) return;
    expanded[clade] = true;

    auto choices = globally_optimal_choices_for_state(plan, chart, outside,
                                                      clade, state);
    if (choices.empty()) {
      throw std::runtime_error(
          "chart trim: non-leaf optimal state has no optimal production "
          "choice");
    }
    auto choice_index = choose_index(choices.size());
    if (choice_index >= choices.size()) {
      throw std::runtime_error(
          "chart trim: traceback choice index out of range");
    }
    auto const& choice = choices[choice_index];
    result.productions.push_back(choice.production);

    auto const& prod = plan.production(choice.production);
    validate_binary_production_for_trim(plan, prod, choice.production);
    for (std::size_t child_i = 0; child_i < 2; ++child_i) {
      self(self, prod.binary_children[child_i],
           choice.child_states[child_i]);
    }
  };

  auto root_choice_index = choose_index(root_states.size());
  if (root_choice_index >= root_states.size()) {
    throw std::runtime_error(
        "chart trim: traceback root choice index out of range");
  }
  trace_clade(trace_clade, plan.root_clade(), root_states[root_choice_index]);
  return result;
}

inline chart_traceback_result deterministic_optimal_single_site_traceback_impl(
    chart_execution_plan const& plan, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  validate_outside_shapes(plan, chart, outside);
  if (compute_global_min(plan, chart, outside) != outside.global_min) {
    throw std::runtime_error("chart trim: outside global optimum is stale");
  }

  std::optional<std::uint8_t> first_root_state;
  if (outside.global_min < chart_inf) {
    auto const root = plan.root_clade();
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      auto const total = parsimony_chart_detail::saturated_add(
          chart.inside[root][state], outside.outside[root][state]);
      if (total < chart_inf && total == outside.global_min) {
        first_root_state = state;
        break;
      }
    }
  }
  if (!first_root_state) {
    throw std::runtime_error("chart trim: no finite optimal root state");
  }

  chart_traceback_result result;
  result.root_state_by_clade.assign(plan.clades().size(), no_chart_state);
  result.score = outside.global_min;
  std::vector<bool> expanded(plan.clades().size(), false);

  auto trace_clade = [&](auto&& self, clade_id clade,
                         std::uint8_t state) -> void {
    if (clade == no_clade || clade >= plan.clades().size()) {
      throw std::runtime_error("chart trim: traceback clade out of range");
    }
    parsimony_chart_detail::validate_state(state, "traceback state");
    if (result.root_state_by_clade[clade] != no_chart_state) {
      if (result.root_state_by_clade[clade] != state) {
        throw std::runtime_error(
            "chart trim: traceback encountered conflicting states for clade");
      }
      return;
    }
    result.root_state_by_clade[clade] = state;
    if (plan.clade(clade).is_leaf() || expanded[clade]) return;
    expanded[clade] = true;

    auto const choice = first_globally_optimal_choice_for_state(
        plan, chart, outside, clade, state);
    if (!choice) {
      throw std::runtime_error(
          "chart trim: non-leaf optimal state has no optimal production "
          "choice");
    }
    result.productions.push_back(choice->production);
    auto const& prod = plan.production(choice->production);
    validate_binary_production_for_trim(plan, prod, choice->production);
    for (std::size_t child_i = 0; child_i < 2; ++child_i) {
      self(self, prod.binary_children[child_i], choice->child_states[child_i]);
    }
  };

  trace_clade(trace_clade, plan.root_clade(), *first_root_state);
  return result;
}

}  // namespace chart_trim_detail

inline single_site_outside_chart build_single_site_outside_chart(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, std::uint8_t reference_state) {
  using namespace parsimony_chart_detail;
  chart_trim_detail::validate_chart_shapes(grammar, chart);
  if (options.score_ua_edge) validate_state(reference_state, "reference");

  single_site_outside_chart result;
  result.outside.assign(grammar.clades.size(), make_inf_row());

  auto root = grammar.root_clade;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    result.outside[root][state] = options.score_ua_edge
                                      ? transition_cost(reference_state, state)
                                      : chart_cost{0};
  }

  result.global_min =
      chart_trim_detail::compute_global_min(grammar, chart, result);

  // This dense outside pass scans productions_by_parent in topological order.
  // productions_by_child is not needed for the all-clade pass, but is validated
  // above so stale/corrupt coboundary data is caught before downstream sidecars
  // rely on it.
  auto order = chart_trim_detail::clades_by_decreasing_size(grammar);
  for (auto parent : order) {
    for (auto pid : grammar.productions_by_parent[parent]) {
      if (pid == no_production || pid >= grammar.productions.size()) {
        throw std::runtime_error("chart trim: production id out of range");
      }
      auto const& prod = grammar.productions[pid];
      if (prod.parent != parent) {
        throw std::runtime_error(
            "chart trim: productions_by_parent contains mismatched parent");
      }
      validate_production_inside_row_inputs(grammar, prod, pid, "chart trim");
      if (prod.children.size() != 2) {
        ++result.multifurcation_productions_scored;
      }

      auto inside_provider = [&](clade_id child) -> auto const& {
        if (child == no_clade || child >= chart.inside.size()) {
          throw std::runtime_error(
              "chart trim: production child clade out of range");
        }
        return chart.inside[child];
      };
      auto consume_row = [&](std::size_t child_i,
                             chart_trim_detail::outside_chart_row const& row) {
        auto const child = prod.children[child_i];
        for (std::uint8_t child_state = 0; child_state < nuc_state_count;
             ++child_state) {
          auto& cell = result.outside[child][child_state];
          cell = std::min(cell, row[child_state]);
        }
      };
      chart_trim_detail::scatter_production_outside_rows(
          prod, result.outside[parent], inside_provider, consume_row,
          result.recurrence_work);
    }
  }

  return result;
}

inline single_site_outside_chart build_single_site_outside_chart(
    chart_execution_plan const& plan, single_site_chart const& chart,
    chart_options const& options, std::uint8_t reference_state) {
  using namespace parsimony_chart_detail;
  chart_trim_detail::validate_chart_shapes(plan, chart);
  if (options.score_ua_edge) validate_state(reference_state, "reference");

  single_site_outside_chart result;
  result.outside.assign(plan.clades().size(), make_inf_row());
  auto root = plan.root_clade();
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    result.outside[root][state] =
        options.score_ua_edge
            ? static_cast<chart_cost>(plan.transition_cost(reference_state,
                                                           state))
            : chart_cost{0};
  }
  result.global_min =
      chart_trim_detail::compute_global_min(plan, chart, result);

  for (auto parent : plan.top_down_order()) {
    for (auto pid : plan.productions_for_parent(parent)) {
      auto const& prod = plan.production(pid);
      auto children = plan.children(pid);
      if (!prod.is_binary()) ++result.multifurcation_productions_scored;

      auto inside_provider = [&](clade_id child) -> auto const& {
        if (child == no_clade || child >= chart.inside.size()) {
          throw std::runtime_error(
              "chart trim: production child clade out of range");
        }
        return chart.inside[child];
      };
      auto consume_row = [&](std::size_t child_i,
                             chart_trim_detail::outside_chart_row const& row) {
        auto const child = children[child_i];
        for (std::uint8_t child_state = 0; child_state < nuc_state_count;
             ++child_state) {
          auto& cell = result.outside[child][child_state];
          cell = std::min(cell, row[child_state]);
        }
      };
      chart_trim_detail::scatter_production_outside_rows(
          plan, children, result.outside[parent], inside_provider, consume_row,
          result.recurrence_work);
    }
  }
  return result;
}

inline single_site_outside_chart build_single_site_outside_chart(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options = {}) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "chart trim: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  return build_single_site_outside_chart(grammar, chart, options,
                                         std::uint8_t{0});
}

inline single_site_outside_chart build_single_site_outside_chart(
    chart_execution_plan const& plan, single_site_chart const& chart,
    chart_options const& options = {}) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "chart trim: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  return build_single_site_outside_chart(plan, chart, options,
                                         std::uint8_t{0});
}

inline single_site_outside_chart build_single_site_outside_chart(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, phylo_dag& dag, mutation_position pos) {
  if (!options.score_ua_edge) {
    return build_single_site_outside_chart(grammar, chart, options);
  }
  return build_single_site_outside_chart(
      grammar, chart, options, extract_reference_site_state(dag, pos));
}

inline chart_trim_mask build_single_site_trim_mask(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside,
    chart_trim_options const& trim_options = {}) {
  chart_trim_detail::validate_outside_shapes(grammar, chart, outside);
  parsimony_chart_detail::require_no_multifurcating_productions_for_consumer(
      grammar, arity_gate_consumer::single_site_trim_mask,
      "single-site trim mask", "choice layer",
      "use dense inside/outside chart rows or selected-topology/SPR paths on "
      "the multifurcating grammar, or expand polytomies before building a "
      "trim mask");
  if (chart_trim_detail::compute_global_min(grammar, chart, outside) !=
      outside.global_min) {
    throw std::runtime_error("chart trim: outside global optimum is stale");
  }

  chart_trim_mask mask;
  mask.keep_clade_state.assign(grammar.clades.size(),
                               chart_trim_detail::false_state_mask());
  mask.keep_production.assign(grammar.productions.size(), false);
  if (trim_options.store_optimal_choices)
    mask.optimal_choices_by_production.resize(grammar.productions.size());
  mask.global_min = outside.global_min;

  for (clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      mask.keep_clade_state[clade][state] =
          chart_trim_detail::is_globally_optimal_state(grammar, chart, outside,
                                                       clade, state);
    }
  }

  for (clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    if (grammar.clades[clade].taxa.size() == 1) continue;
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      auto choices = chart_trim_detail::globally_optimal_choices_for_state(
          grammar, chart, outside, clade, state);
      for (auto choice : choices) {
        mask.keep_production[choice.production] = true;
        if (trim_options.store_optimal_choices) {
          if (trim_options.max_stored_optimal_choices != 0 &&
              mask.kept_production_choice_count >=
                  trim_options.max_stored_optimal_choices) {
            throw std::runtime_error(
                "chart trim: optimal-choice storage cap exceeded (cap=" +
                std::to_string(trim_options.max_stored_optimal_choices) + ")");
          }
          mask.optimal_choices_by_production[choice.production].push_back(
              choice);
        }
        ++mask.kept_production_choice_count;
      }
    }
  }

  return mask;
}

inline chart_trim_mask build_single_site_trim_mask(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options = {}) {
  auto outside = build_single_site_outside_chart(grammar, chart, options);
  return build_single_site_trim_mask(grammar, chart, outside);
}

inline chart_trim_mask build_single_site_trim_mask(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, chart_trim_options const& trim_options) {
  auto outside = build_single_site_outside_chart(grammar, chart, options);
  return build_single_site_trim_mask(grammar, chart, outside, trim_options);
}

inline chart_trim_mask build_single_site_trim_mask(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, std::uint8_t reference_state,
    chart_trim_options const& trim_options = {}) {
  auto outside =
      build_single_site_outside_chart(grammar, chart, options, reference_state);
  return build_single_site_trim_mask(grammar, chart, outside, trim_options);
}

inline chart_traceback_result deterministic_optimal_single_site_traceback(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  auto choose_first = [](std::size_t size) -> std::size_t {
    if (size == 0) throw std::runtime_error("chart trim: empty choice set");
    return 0;
  };
  return chart_trim_detail::optimal_single_site_traceback_impl(
      grammar, chart, outside, choose_first);
}

inline chart_traceback_result deterministic_optimal_single_site_traceback(
    chart_execution_plan const& plan, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  return chart_trim_detail::deterministic_optimal_single_site_traceback_impl(
      plan, chart, outside);
}

inline chart_traceback_result deterministic_optimal_single_site_traceback(
    chart_execution_plan const& plan, single_site_chart const& chart,
    chart_options const& options = {}) {
  auto outside = build_single_site_outside_chart(plan, chart, options);
  return deterministic_optimal_single_site_traceback(plan, chart, outside);
}

inline chart_traceback_result deterministic_optimal_single_site_traceback(
    chart_execution_plan const& plan, single_site_chart const& chart,
    chart_options const& options, std::uint8_t reference_state) {
  auto outside =
      build_single_site_outside_chart(plan, chart, options, reference_state);
  return deterministic_optimal_single_site_traceback(plan, chart, outside);
}

inline chart_traceback_result deterministic_optimal_single_site_traceback(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options = {}) {
  auto outside = build_single_site_outside_chart(grammar, chart, options);
  return deterministic_optimal_single_site_traceback(grammar, chart, outside);
}

inline chart_traceback_result deterministic_optimal_single_site_traceback(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, std::uint8_t reference_state) {
  auto outside =
      build_single_site_outside_chart(grammar, chart, options, reference_state);
  return deterministic_optimal_single_site_traceback(grammar, chart, outside);
}

inline chart_traceback_result random_optimal_single_site_traceback(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside, std::uint32_t seed) {
  std::mt19937 rng{seed};
  auto choose_random = [&](std::size_t size) -> std::size_t {
    if (size == 0) throw std::runtime_error("chart trim: empty choice set");
    std::uniform_int_distribution<std::size_t> dist(0, size - 1);
    return dist(rng);
  };
  return chart_trim_detail::optimal_single_site_traceback_impl(
      grammar, chart, outside, choose_random);
}

inline chart_traceback_result random_optimal_single_site_traceback(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, std::uint32_t seed) {
  auto outside = build_single_site_outside_chart(grammar, chart, options);
  return random_optimal_single_site_traceback(grammar, chart, outside, seed);
}

inline chart_traceback_result random_optimal_single_site_traceback(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, std::uint8_t reference_state,
    std::uint32_t seed) {
  auto outside =
      build_single_site_outside_chart(grammar, chart, options, reference_state);
  return random_optimal_single_site_traceback(grammar, chart, outside, seed);
}

inline constexpr std::uint64_t multisite_score_inf =
    std::numeric_limits<std::uint64_t>::max() / 4;

struct composite_chart_score {
  // Sum of independent single-site/pattern optima.  This is a lower bound for
  // multi-site topology optimization, not an exact score unless all patterns
  // can share one optimal topology.
  std::uint64_t weighted_lower_bound = 0;

  // Coarse diagnostic per exact pattern. With score_ua_edge=false this is the
  // UA-free root optimum. With score_ua_edge=true a compressed pattern can span
  // sites with different UA/reference states; in that case this is only the
  // minimum over observed reference-state-specific optima. Use
  // per_pattern_root_min_by_reference_state for lossless UA-edge diagnostics.
  std::vector<chart_cost> per_pattern_root_min;

  // Indexed [pattern][reference_state]. With score_ua_edge=true, entries for
  // reference states that occur in the compressed pattern contain the exact
  // root optimum including that reference edge and absent states are INF. With
  // score_ua_edge=false all four entries are filled with the UA-free root
  // optimum for convenience.
  std::vector<std::array<chart_cost, nuc_state_count>>
      per_pattern_root_min_by_reference_state;

  std::size_t multifurcation_productions_scored = 0;
};

struct multisite_cost_function {
  // Flattened [active_pattern][state], unweighted.  Pattern weights are applied
  // only in objective, lower-bound, and upper-bound calculations.
  std::vector<chart_cost> cost;
  // Debug/acceleration only.  Equal hashes are never treated as identity.
  std::uint64_t topology_hash = 0;
};

struct frontier_provenance_choice {
  production_id production = no_production;
  std::size_t left_entry = 0;
  std::size_t right_entry = 0;
};

struct frontier_entry {
  multisite_cost_function f;
  // Provenance for the partial topology below this clade.  Equal cost vectors
  // merge provenance so trimming does not lose equally optimal productions.
  std::vector<bool> used_production;

  // Opt-in topology provenance.  For an internal clade entry, each choice fixes
  // one parent production and the exact child frontier entries that were
  // combined to produce this cost vector.  Leaf entries have no choices.
  std::vector<frontier_provenance_choice> provenance;
};

enum class multisite_keep_mask_kind {
  none,
  exact_optimal_production_union,
  score_only_not_exact,
};

inline char const* multisite_keep_mask_kind_name(
    multisite_keep_mask_kind kind) {
  switch (kind) {
    case multisite_keep_mask_kind::none:
      return "none";
    case multisite_keep_mask_kind::exact_optimal_production_union:
      return "exact_optimal_production_union";
    case multisite_keep_mask_kind::score_only_not_exact:
      return "score_only_not_exact";
  }
  return "unknown";
}

enum class multisite_dominance_mode {
  off,
  score_only,
  strict_mask_safe,
  two_pass_exact_mask,
  provenance_preserving,
};

inline char const* multisite_dominance_mode_name(
    multisite_dominance_mode mode) {
  switch (mode) {
    case multisite_dominance_mode::off:
      return "off";
    case multisite_dominance_mode::score_only:
      return "score-only";
    case multisite_dominance_mode::strict_mask_safe:
      return "strict-mask-safe";
    case multisite_dominance_mode::two_pass_exact_mask:
      return "two-pass-exact-mask";
    case multisite_dominance_mode::provenance_preserving:
      return "provenance-preserving";
  }
  return "unknown";
}

struct multisite_trim_options {
  bool use_bound_pruning = true;
  multisite_dominance_mode dominance_mode = multisite_dominance_mode::off;
  bool require_exact_keep_mask = true;
  std::optional<std::uint64_t> upper_bound_override;
  std::optional<std::uint64_t> known_exact_optimum;

  // 0 means unlimited.
  std::size_t max_frontier_entries_per_clade = 0;

  // Opt-in semantic-oracle evidence.  When enabled, retain the equality-
  // deduplicated optimal root cost vectors together with the union of
  // production provenance represented by each vector.  This is deliberately
  // disabled by default: ordinary trimming needs only the aggregate keep mask
  // and must not pay to copy root-frontier vectors or masks.
  bool capture_optimal_root_provenance = false;

  // Test-only hard-error seam.  This fires inside the primary trim's
  // report-only root-provenance capture, after the exact frontier and optimum
  // have been established.  Search tests use it to prove that moving capture
  // into the primary B&B cannot turn a report failure into candidate
  // invalidation or an accepted-rebuild rejection.
  bool force_optimal_root_provenance_capture_failure_for_tests = false;
};

class multisite_optimal_root_provenance_capture_error
    : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct multisite_optimal_root_provenance_class {
  std::vector<chart_cost> cost;
  std::vector<bool> used_production;

  bool operator==(multisite_optimal_root_provenance_class const&) const =
      default;
};

// Deterministic, implementation-level work counters for the finalized exact
// setup path.  These count logical chart/setup operations rather than timing or
// allocations so they remain stable enough for regression tests.
struct multisite_exact_setup_work_stats {
  std::size_t setup_builds = 0;
  std::size_t inside_charts_built = 0;
  std::size_t resident_inside_charts_consumed = 0;
  std::size_t active_leaf_state_vectors_copied = 0;
  std::size_t active_leaf_states_copied = 0;
  std::size_t outside_boundary_charts_built = 0;
  outside_recurrence_work_stats outside_recurrence_work;
  std::size_t upper_bound_topologies_generated = 0;
  std::size_t upper_bound_topologies_unique = 0;
  std::size_t frontier_passes = 0;

  bool operator==(multisite_exact_setup_work_stats const&) const = default;
};

enum class multisite_frontier_pass_kind {
  exact,
  score_only,
  exact_mask_recovery,
};

inline char const* multisite_frontier_pass_kind_name(
    multisite_frontier_pass_kind kind) noexcept {
  switch (kind) {
    case multisite_frontier_pass_kind::exact:
      return "exact";
    case multisite_frontier_pass_kind::score_only:
      return "score_only";
    case multisite_frontier_pass_kind::exact_mask_recovery:
      return "exact_mask_recovery";
  }
  return "unknown";
}

// Non-semantic diagnostics for one dependency-level frontier wave.  Timing,
// scheduling choices, and scratch estimates are deliberately excluded from
// canonical trim/search serialization; the deterministic logical-work fields
// make serial/parallel accounting independently auditable.
struct multisite_frontier_level_diagnostic {
  multisite_frontier_pass_kind pass_kind = multisite_frontier_pass_kind::exact;
  std::size_t pass_index = 0;
  std::size_t dependency_level = 0;
  std::size_t clades_processed = 0;
  std::size_t internal_clades_processed = 0;
  std::size_t product_combinations = 0;
  std::size_t output_frontier_entries = 0;
  std::size_t maximum_clade_frontier_entries = 0;
  std::size_t equality_deduplicated = 0;
  std::size_t bound_pruned = 0;
  std::size_t dominance_candidates_considered = 0;
  std::size_t dominance_pruned = 0;
  double wave_ms = 0.0;
};

struct multisite_trim_result {
  std::uint64_t optimum = multisite_score_inf;
  std::uint64_t composite_lower_bound = multisite_score_inf;
  std::uint64_t initial_upper_bound = multisite_score_inf;
  std::vector<bool> keep_production;
  std::vector<std::size_t> frontier_sizes_by_clade;
  multisite_dominance_mode dominance_mode = multisite_dominance_mode::off;
  multisite_keep_mask_kind keep_mask_kind =
      multisite_keep_mask_kind::exact_optimal_production_union;
  bool keep_production_exact = true;
  std::size_t dominance_candidates_considered = 0;
  std::size_t dominance_pruned_score_pass = 0;
  std::size_t dominance_pruned_mask_pass = 0;
  // Legacy/total alias: score-pass plus mask-pass dominance pruning.
  std::size_t dominance_pruned = 0;
  std::size_t exact_mask_recovery_passes = 0;
  std::size_t bound_pruned = 0;
  std::size_t equality_deduplicated = 0;
  std::size_t active_pattern_count = 0;
  std::uint64_t invariant_constant_offset = 0;
  bool lazy_chart_used = false;
  std::size_t lazy_inside_rows_computed = 0;
  std::size_t lazy_outside_rows_computed = 0;
  std::size_t lazy_patterns_merged_max = 0;
  std::size_t lazy_remerge_collisions = 0;
  std::size_t lazy_structural_class_count_max = 0;
  std::vector<std::size_t> lazy_structural_class_count_by_clade;
  std::vector<multisite_optimal_root_provenance_class>
      optimal_root_provenance_classes;
  multisite_exact_setup_work_stats exact_setup_work;
  std::vector<multisite_frontier_level_diagnostic> frontier_level_diagnostics;
  std::size_t exact_bnb_levels = 0;
  std::size_t exact_bnb_clades = 0;
  std::size_t exact_bnb_product_combinations = 0;
  std::size_t exact_bnb_frontier_entries = 0;
  double exact_bnb_ms = 0.0;
};

struct multisite_topology_trace_options {
  bool keep_provenance = true;
  // 0 means unlimited.
  std::size_t max_optimal_topologies = 1;
  // 0 means unlimited.
  std::size_t max_provenance_choices_per_entry = 0;
  std::vector<production_id> required_productions;
  bool require_required_production_coverage = true;
  multisite_trim_options trim_options = {};
};

struct multisite_topology_trace_result {
  std::uint64_t optimum = multisite_score_inf;
  std::uint64_t composite_lower_bound = multisite_score_inf;
  std::uint64_t initial_upper_bound = multisite_score_inf;
  std::vector<grammar_topology> topologies;
  std::vector<bool> keep_production;
  std::vector<std::size_t> frontier_sizes_by_clade;
  std::size_t equality_deduplicated = 0;
  std::size_t dominance_pruned = 0;
  std::size_t bound_pruned = 0;
  std::size_t optimal_frontier_entry_count = 0;
  bool topology_cap_truncated = false;
  std::vector<production_id> uncovered_required_productions;
  std::size_t active_pattern_count = 0;
  std::uint64_t invariant_constant_offset = 0;
};

struct coupled_frontier_choice {
  production_id production = no_production;
  clade_id left_child = no_clade;
  clade_id right_child = no_clade;
  // Child entry indices are compact indices into
  // multisite_coupled_frontier_trim_result::entries_by_clade[child].
  std::size_t left_entry = 0;
  std::size_t right_entry = 0;
};

struct coupled_frontier_entry {
  // Cost vector for this exact coupled partial topology class.  All retained
  // provenance choices for this entry have this same vector, so replacing one
  // retained choice with another cannot change the multi-site score seen by an
  // optimal outside context.
  multisite_cost_function f;
  std::vector<coupled_frontier_choice> choices;
};

struct multisite_coupled_frontier_trim_options {
  multisite_trim_options trim_options = {};
  // 0 means unlimited.  This bounds per-entry packed provenance, not the
  // number of represented optimal topologies.
  std::size_t max_provenance_choices_per_entry = 0;
};

struct multisite_coupled_frontier_trim_result {
  std::uint64_t optimum = multisite_score_inf;
  std::uint64_t composite_lower_bound = multisite_score_inf;
  std::uint64_t initial_upper_bound = multisite_score_inf;

  // Compact packed forest of exactly optimal coupled frontier combinations.
  // For each clade, only entries reachable from an optimal root entry are
  // retained; child indices in coupled_frontier_choice are remapped to this
  // compact space.  An ordinary production union may recombine these choices,
  // but this annotated forest preserves the correlations.
  std::vector<std::vector<coupled_frontier_entry>> entries_by_clade;
  std::vector<bool> keep_production;

  // Full bottom-up frontier sizes before compacting to the optimal annotation,
  // plus compact sizes after top-down optimal-context recovery.
  std::vector<std::size_t> frontier_sizes_by_clade;
  std::vector<std::size_t> coupled_frontier_entries_by_clade;

  multisite_dominance_mode dominance_mode = multisite_dominance_mode::off;
  std::size_t equality_deduplicated = 0;
  std::size_t dominance_candidates_considered = 0;
  std::size_t dominance_pruned = 0;
  std::size_t bound_pruned = 0;
  std::size_t optimal_root_frontier_entry_count = 0;
  std::size_t coupled_frontier_entry_count = 0;
  std::size_t coupled_provenance_choice_count = 0;
  bool coupled_frontier_exact = false;
  bool annotated_optimal_trim = false;
  std::size_t active_pattern_count = 0;
  std::uint64_t invariant_constant_offset = 0;
};

struct multisite_coupled_frontier_enumeration_result {
  std::vector<grammar_topology> topologies;
  bool topology_cap_truncated = false;
};

struct multisite_bruteforce_result {
  std::uint64_t optimum = multisite_score_inf;
  std::vector<bool> keep_production;
  std::size_t topology_count = 0;
  std::size_t optimal_topology_count = 0;
};

namespace chart_multisite_detail {

using chart_row = std::array<chart_cost, nuc_state_count>;

struct chart_cost_vector_hash {
  std::size_t operator()(std::vector<chart_cost> const& values) const noexcept {
    std::size_t h = values.size();
    for (auto value : values) {
      h ^= std::hash<chart_cost>{}(value) + 0x9e3779b97f4a7c15ULL + (h << 6) +
           (h >> 2);
    }
    return h;
  }
};

inline std::uint64_t mix_hash(std::uint64_t seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  return seed;
}

inline std::size_t cost_index(std::size_t active_pattern, std::uint8_t state) {
  parsimony_chart_detail::validate_state(state, "multi-site state");
  return active_pattern * nuc_state_count + state;
}

inline std::uint64_t checked_add_u64(std::uint64_t lhs, std::uint64_t rhs,
                                     std::string_view label) {
  if (lhs >= multisite_score_inf || rhs >= multisite_score_inf)
    return multisite_score_inf;
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    auto message =
        std::string{"multi-site trim: uint64 overflow while adding "};
    message.append(label);
    throw std::runtime_error(message);
  }
  auto result = lhs + rhs;
  return result >= multisite_score_inf ? multisite_score_inf : result;
}

inline std::uint64_t checked_mul_cost(std::uint64_t weight, chart_cost cost,
                                      std::string_view label) {
  if (weight == 0) return 0;
  if (cost >= chart_inf) return multisite_score_inf;
  if (cost > std::numeric_limits<std::uint64_t>::max() / weight) {
    auto message =
        std::string{"multi-site trim: uint64 overflow while multiplying "};
    message.append(label);
    throw std::runtime_error(message);
  }
  auto result = weight * static_cast<std::uint64_t>(cost);
  return result >= multisite_score_inf ? multisite_score_inf : result;
}

inline chart_cost row_min(chart_row const& row) {
  chart_cost best = chart_inf;
  for (auto cost : row) best = std::min(best, cost);
  return best;
}

inline chart_row make_inf_row() {
  chart_row row{};
  row.fill(chart_inf);
  return row;
}

inline chart_row combine_rows(std::span<chart_row const> children) {
  if (children.empty()) {
    throw std::runtime_error(
        "multi-site trim: selected topology row combine has no children");
  }
  chart_row row = make_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    chart_cost total = 0;
    for (auto const& child : children) {
      chart_cost best_child = chart_inf;
      for (std::uint8_t child_state = 0; child_state < nuc_state_count;
           ++child_state) {
        best_child = std::min(
            best_child,
            parsimony_chart_detail::saturated_add(
                child[child_state], parsimony_chart_detail::transition_cost(
                                        parent_state, child_state)));
      }
      total = parsimony_chart_detail::saturated_add(total, best_child);
    }
    row[parent_state] = total;
  }
  return row;
}

inline chart_row combine_rows(chart_execution_plan const& plan,
                              std::span<chart_row const> children) {
  if (children.empty()) {
    throw std::runtime_error(
        "multi-site trim: selected topology row combine has no children");
  }
  chart_row row = make_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    chart_cost total = 0;
    for (auto const& child : children) {
      chart_cost best_child = chart_inf;
      for (std::uint8_t child_state = 0; child_state < nuc_state_count;
           ++child_state) {
        best_child = std::min(
            best_child,
            parsimony_chart_detail::saturated_add(
                child[child_state],
                static_cast<chart_cost>(
                    plan.transition_cost(parent_state, child_state))));
      }
      total = parsimony_chart_detail::saturated_add(total, best_child);
    }
    row[parent_state] = total;
  }
  return row;
}

inline chart_row combine_binary_rows(chart_row const& left,
                                     chart_row const& right) {
  std::array<chart_row, 2> children{left, right};
  return combine_rows(
      std::span<chart_row const>{children.data(), children.size()});
}

inline void merge_used_productions(std::vector<bool>& dst,
                                   std::vector<bool> const& src) {
  if (dst.size() != src.size()) {
    throw std::runtime_error(
        "multi-site trim: provenance vector size mismatch");
  }
  for (std::size_t i = 0; i < dst.size(); ++i) dst[i] = dst[i] || src[i];
}

inline bool same_provenance_choice(frontier_provenance_choice const& lhs,
                                   frontier_provenance_choice const& rhs) {
  return lhs.production == rhs.production && lhs.left_entry == rhs.left_entry &&
         lhs.right_entry == rhs.right_entry;
}

inline void merge_provenance_choices(
    std::vector<frontier_provenance_choice>& dst,
    std::vector<frontier_provenance_choice> const& src,
    std::size_t max_choices_per_entry = 0) {
  for (auto choice : src) {
    auto found =
        std::find_if(dst.begin(), dst.end(), [&](auto const& existing) {
          return same_provenance_choice(existing, choice);
        });
    if (found != dst.end()) continue;
    if (max_choices_per_entry != 0 && dst.size() >= max_choices_per_entry) {
      throw std::runtime_error(
          "multi-site topology trace: provenance-choice cap exceeded");
    }
    dst.push_back(choice);
  }
  std::sort(dst.begin(), dst.end(), [](auto const& lhs, auto const& rhs) {
    if (lhs.production != rhs.production)
      return lhs.production < rhs.production;
    if (lhs.left_entry != rhs.left_entry)
      return lhs.left_entry < rhs.left_entry;
    return lhs.right_entry < rhs.right_entry;
  });
}

inline bool is_active_pattern(site_pattern const& pattern) {
  return !is_invariant_site_pattern(pattern);
}

inline void validate_pattern_reference_counts(site_pattern const& pattern,
                                              std::size_t pattern_index) {
  std::uint64_t sum = 0;
  for (auto count : pattern.reference_state_counts) sum += count;
  if (sum != pattern.weight) {
    throw std::runtime_error(
        "multi-site trim: reference-state counts do not sum to pattern "
        "weight for pattern " +
        std::to_string(pattern_index));
  }
}

inline std::uint64_t invariant_constant_offset(site_pattern_set const& patterns,
                                               chart_options const& options) {
  if (!options.score_ua_edge) return 0;

  std::uint64_t total = checked_add_u64(
      0, patterns.skipped_invariant_constant_score_with_reference_edge,
      "skipped invariant UA-edge offset");

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    validate_pattern_reference_counts(pattern, pattern_index);
    if (!is_invariant_site_pattern(pattern)) continue;
    if (pattern.state_by_taxon.empty()) {
      throw std::runtime_error(
          "multi-site trim: invariant pattern has no taxa");
    }
    auto invariant_state = pattern.state_by_taxon.front();
    parsimony_chart_detail::validate_state(invariant_state,
                                           "invariant pattern state");
    for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
         ++reference_state) {
      auto count = pattern.reference_state_counts[reference_state];
      if (count == 0) continue;
      auto cost = parsimony_chart_detail::transition_cost(reference_state,
                                                          invariant_state);
      total = checked_add_u64(
          total, checked_mul_cost(count, cost, "invariant UA-edge offset"),
          "invariant UA-edge offset total");
    }
  }
  return total;
}

inline std::uint64_t invariant_constant_offset(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options) {
  if (!options.score_ua_edge) return 0;

  std::uint64_t total = checked_add_u64(
      0, patterns.skipped_invariant_constant_score_with_reference_edge,
      "skipped invariant UA-edge offset");
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    validate_pattern_reference_counts(pattern, pattern_index);
    if (!is_invariant_site_pattern(pattern)) continue;
    if (pattern.state_by_taxon.empty()) {
      throw std::runtime_error(
          "multi-site trim: invariant pattern has no taxa");
    }
    auto invariant_state = pattern.state_by_taxon.front();
    parsimony_chart_detail::validate_state(invariant_state,
                                           "invariant pattern state");
    for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
         ++reference_state) {
      auto count = pattern.reference_state_counts[reference_state];
      if (count == 0) continue;
      auto cost = static_cast<chart_cost>(
          plan.transition_cost(reference_state, invariant_state));
      total = checked_add_u64(
          total, checked_mul_cost(count, cost, "invariant UA-edge offset"),
          "invariant UA-edge offset total");
    }
  }
  return total;
}

// Shared checked root-row scorer for compressed site patterns.  In
// score_ua_edge=false mode this is weight * min(root_row).  In
// score_ua_edge=true mode one compressed pattern can contain positions with
// different reference states, so this sums each reference-state count times
// min_root_state(root_row[root] + c(reference, root)).  Chart-SPR search wraps
// this helper and local scorers must use that wrapper instead of open-coding
// UA/reference-edge arithmetic.
inline std::uint64_t weighted_root_score_from_row(
    chart_row const& row, site_pattern const& pattern,
    chart_options const& options) {
  if (!options.score_ua_edge) {
    return checked_mul_cost(pattern.weight, row_min(row),
                            "weighted topology root cost");
  }

  validate_pattern_reference_counts(pattern, 0);
  std::uint64_t total = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    auto count = pattern.reference_state_counts[reference_state];
    if (count == 0) continue;
    chart_cost best = chart_inf;
    for (std::uint8_t root_state = 0; root_state < nuc_state_count;
         ++root_state) {
      best = std::min(
          best, parsimony_chart_detail::saturated_add(
                    row[root_state], parsimony_chart_detail::transition_cost(
                                         reference_state, root_state)));
    }
    total = checked_add_u64(
        total,
        checked_mul_cost(count, best, "weighted topology root-edge cost"),
        "weighted topology root-edge total");
  }
  return total;
}

inline std::uint64_t weighted_root_score_from_row(
    chart_execution_plan const& plan, chart_row const& row,
    site_pattern const& pattern, chart_options const& options) {
  if (!options.score_ua_edge) {
    return checked_mul_cost(pattern.weight, row_min(row),
                            "weighted topology root cost");
  }
  validate_pattern_reference_counts(pattern, 0);
  std::uint64_t total = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    auto count = pattern.reference_state_counts[reference_state];
    if (count == 0) continue;
    chart_cost best = chart_inf;
    for (std::uint8_t root_state = 0; root_state < nuc_state_count;
         ++root_state) {
      best = std::min(
          best, parsimony_chart_detail::saturated_add(
                    row[root_state],
                    static_cast<chart_cost>(
                        plan.transition_cost(reference_state, root_state))));
    }
    total = checked_add_u64(
        total,
        checked_mul_cost(count, best, "weighted topology root-edge cost"),
        "weighted topology root-edge total");
  }
  return total;
}

struct active_pattern_info {
  std::size_t pattern_index = no_site_pattern;
  std::uint32_t weight = 0;
  std::array<std::uint32_t, nuc_state_count> reference_state_counts{};
  std::vector<std::uint8_t> state_by_taxon;
  single_site_chart chart;
  single_site_outside_chart outside_ua_free;
  std::array<single_site_outside_chart, nuc_state_count> outside_by_reference;
};

inline std::vector<active_pattern_info> build_active_pattern_info(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options) {
  std::vector<active_pattern_info> active;
  chart_options chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (options.score_ua_edge)
      validate_pattern_reference_counts(pattern, pattern_index);
    if (!is_active_pattern(pattern)) continue;

    leaf_site_states states;
    states.state_by_taxon = pattern.state_by_taxon;

    active_pattern_info info;
    info.pattern_index = pattern_index;
    info.weight = pattern.weight;
    info.reference_state_counts = pattern.reference_state_counts;
    info.state_by_taxon = pattern.state_by_taxon;
    info.chart = build_single_site_chart(grammar, states, chart_build_options);
    if (options.score_ua_edge) {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        if (info.reference_state_counts[reference_state] == 0) continue;
        info.outside_by_reference[reference_state] =
            build_single_site_outside_chart(grammar, info.chart, options,
                                            reference_state);
      }
    } else {
      info.outside_ua_free =
          build_single_site_outside_chart(grammar, info.chart, options);
    }
    active.push_back(std::move(info));
  }
  return active;
}

inline std::vector<active_pattern_info> build_active_pattern_info(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options) {
  plan.assert_valid();
  std::vector<active_pattern_info> active;
  chart_options chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (options.score_ua_edge) {
      validate_pattern_reference_counts(pattern, pattern_index);
    }
    if (!is_active_pattern(pattern)) continue;

    leaf_site_states states;
    states.state_by_taxon = pattern.state_by_taxon;
    active_pattern_info info;
    info.pattern_index = pattern_index;
    info.weight = pattern.weight;
    info.reference_state_counts = pattern.reference_state_counts;
    info.state_by_taxon = pattern.state_by_taxon;
    info.chart = build_single_site_chart(plan, states, chart_build_options);
    if (options.score_ua_edge) {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        if (info.reference_state_counts[reference_state] == 0) continue;
        info.outside_by_reference[reference_state] =
            build_single_site_outside_chart(plan, info.chart, options,
                                            reference_state);
      }
    } else {
      info.outside_ua_free =
          build_single_site_outside_chart(plan, info.chart, options);
    }
    active.push_back(std::move(info));
  }
  return active;
}

inline std::uint64_t lower_bound_for_entry(
    frontier_entry const& entry, clade_id clade,
    std::vector<active_pattern_info> const& active,
    std::uint64_t invariant_offset, chart_options const& options) {
  std::uint64_t total = invariant_offset;
  if (entry.f.cost.size() != active.size() * nuc_state_count) {
    throw std::runtime_error(
        "multi-site trim: frontier cost vector has wrong size");
  }

  for (std::size_t active_index = 0; active_index < active.size();
       ++active_index) {
    auto const& info = active[active_index];
    if (!options.score_ua_edge) {
      if (clade >= info.outside_ua_free.outside.size()) {
        throw std::runtime_error("multi-site trim: clade out of outside range");
      }
      chart_cost best = chart_inf;
      for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
        best = std::min(best, parsimony_chart_detail::saturated_add(
                                  entry.f.cost[cost_index(active_index, state)],
                                  info.outside_ua_free.outside[clade][state]));
      }
      total = checked_add_u64(
          total,
          checked_mul_cost(info.weight, best,
                           "weighted active-pattern lower bound"),
          "active-pattern lower-bound total");
    } else {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        auto count = info.reference_state_counts[reference_state];
        if (count == 0) continue;
        auto const& outside = info.outside_by_reference[reference_state];
        if (clade >= outside.outside.size()) {
          throw std::runtime_error(
              "multi-site trim: clade out of reference outside range");
        }
        chart_cost best = chart_inf;
        for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
          best =
              std::min(best, parsimony_chart_detail::saturated_add(
                                 entry.f.cost[cost_index(active_index, state)],
                                 outside.outside[clade][state]));
        }
        total = checked_add_u64(
            total,
            checked_mul_cost(count, best,
                             "weighted active-pattern root-edge lower "
                             "bound"),
            "active-pattern root-edge lower-bound total");
      }
    }
  }
  return total;
}

inline frontier_entry make_leaf_frontier_entry(
    clade_grammar const& grammar, clade_id clade,
    std::vector<active_pattern_info> const& active,
    bool keep_used_production = true) {
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() != 1) {
    throw std::runtime_error(
        "multi-site trim: leaf frontier requested for non-singleton clade");
  }
  auto taxon = key.taxa.front();
  frontier_entry entry;
  entry.f.cost.assign(active.size() * nuc_state_count, chart_inf);
  entry.f.topology_hash = mix_hash(0x6c656166ULL, taxon);
  if (keep_used_production) {
    entry.used_production.assign(grammar.productions.size(), false);
  }

  for (std::size_t active_index = 0; active_index < active.size();
       ++active_index) {
    auto const& states = active[active_index].state_by_taxon;
    if (taxon >= states.size()) {
      throw std::runtime_error(
          "multi-site trim: taxon out of active pattern range");
    }
    auto observed = states[taxon];
    parsimony_chart_detail::validate_state(observed, "leaf observed state");
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      entry.f.cost[cost_index(active_index, state)] =
          state == observed ? chart_cost{0} : chart_inf;
    }
  }
  return entry;
}

inline frontier_entry make_leaf_frontier_entry(
    chart_execution_plan const& plan, clade_id clade,
    std::vector<active_pattern_info> const& active,
    bool keep_used_production = true) {
  auto const& key = plan.clade(clade);
  if (!key.is_leaf()) {
    throw std::runtime_error(
        "multi-site trim: leaf frontier requested for non-singleton clade");
  }
  auto taxon = key.leaf_taxon;
  frontier_entry entry;
  entry.f.cost.assign(active.size() * nuc_state_count, chart_inf);
  entry.f.topology_hash = mix_hash(0x6c656166ULL, taxon);
  if (keep_used_production) {
    entry.used_production.assign(plan.productions().size(), false);
  }

  for (std::size_t active_index = 0; active_index < active.size();
       ++active_index) {
    auto const& states = active[active_index].state_by_taxon;
    if (taxon >= states.size()) {
      throw std::runtime_error(
          "multi-site trim: taxon out of active pattern range");
    }
    auto observed = states[taxon];
    parsimony_chart_detail::validate_state(observed, "leaf observed state");
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      entry.f.cost[cost_index(active_index, state)] =
          state == observed ? chart_cost{0} : chart_inf;
    }
  }
  return entry;
}

inline frontier_entry combine_frontier_entries(
    clade_grammar const& grammar, grammar_production const& prod,
    production_id pid, frontier_entry const& left, frontier_entry const& right,
    std::size_t active_pattern_count, bool keep_used_production = true) {
  if (prod.children.size() != 2) {
    throw std::runtime_error(
        "multi-site trim: parent combine supports binary productions only");
  }
  auto expected_size = active_pattern_count * nuc_state_count;
  if (left.f.cost.size() != expected_size ||
      right.f.cost.size() != expected_size) {
    throw std::runtime_error(
        "multi-site trim: child frontier cost vector has wrong size");
  }
  if (keep_used_production) {
    if (left.used_production.size() != grammar.productions.size() ||
        right.used_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          "multi-site trim: child provenance vector has wrong size");
    }
  } else if (!left.used_production.empty() || !right.used_production.empty()) {
    throw std::runtime_error(
        "multi-site trim: score-only frontier unexpectedly carries provenance "
        "bitsets");
  }

  frontier_entry candidate;
  candidate.f.cost.assign(expected_size, chart_inf);
  candidate.f.topology_hash =
      mix_hash(mix_hash(mix_hash(0x70726f64ULL, pid), left.f.topology_hash),
               right.f.topology_hash);
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error("multi-site trim: production id out of range");
  }
  if (keep_used_production) {
    candidate.used_production = left.used_production;
    merge_used_productions(candidate.used_production, right.used_production);
    candidate.used_production[pid] = true;
  }

  for (std::size_t active_index = 0; active_index < active_pattern_count;
       ++active_index) {
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      chart_cost best_left = chart_inf;
      chart_cost best_right = chart_inf;
      for (std::uint8_t child_state = 0; child_state < nuc_state_count;
           ++child_state) {
        best_left = std::min(
            best_left, parsimony_chart_detail::saturated_add(
                           left.f.cost[cost_index(active_index, child_state)],
                           parsimony_chart_detail::transition_cost(
                               parent_state, child_state)));
        best_right = std::min(
            best_right, parsimony_chart_detail::saturated_add(
                            right.f.cost[cost_index(active_index, child_state)],
                            parsimony_chart_detail::transition_cost(
                                parent_state, child_state)));
      }
      candidate.f.cost[cost_index(active_index, parent_state)] =
          parsimony_chart_detail::saturated_add(best_left, best_right);
    }
  }
  return candidate;
}

inline void combine_frontier_costs_into(
    chart_execution_plan const& plan,
    chart_plan_production_descriptor const& prod, production_id pid,
    frontier_entry const& left, frontier_entry const& right,
    std::size_t active_pattern_count, multisite_cost_function& out) {
  if (!prod.is_binary()) {
    throw std::runtime_error(
        "multi-site trim: parent combine supports binary productions only");
  }
  auto expected_size = active_pattern_count * nuc_state_count;
  if (left.f.cost.size() != expected_size ||
      right.f.cost.size() != expected_size) {
    throw std::runtime_error(
        "multi-site trim: child frontier cost vector has wrong size");
  }
  out.cost.resize(expected_size);
  out.topology_hash =
      mix_hash(mix_hash(mix_hash(0x70726f64ULL, pid), left.f.topology_hash),
               right.f.topology_hash);
  if (prod.source_id != pid) {
    throw std::runtime_error("multi-site trim: production id out of range");
  }

  for (std::size_t active_index = 0; active_index < active_pattern_count;
       ++active_index) {
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      chart_cost best_left = chart_inf;
      chart_cost best_right = chart_inf;
      for (std::uint8_t child_state = 0; child_state < nuc_state_count;
           ++child_state) {
        auto transition = static_cast<chart_cost>(
            plan.transition_cost(parent_state, child_state));
        best_left = std::min(
            best_left, parsimony_chart_detail::saturated_add(
                           left.f.cost[cost_index(active_index, child_state)],
                           transition));
        best_right = std::min(
            best_right, parsimony_chart_detail::saturated_add(
                            right.f.cost[cost_index(active_index, child_state)],
                            transition));
      }
      out.cost[cost_index(active_index, parent_state)] =
          parsimony_chart_detail::saturated_add(best_left, best_right);
    }
  }
}

inline frontier_entry combine_frontier_entries(
    chart_execution_plan const& plan,
    chart_plan_production_descriptor const& prod, production_id pid,
    frontier_entry const& left, frontier_entry const& right,
    std::size_t active_pattern_count, bool keep_used_production = true) {
  frontier_entry candidate;
  combine_frontier_costs_into(plan, prod, pid, left, right,
                              active_pattern_count, candidate.f);
  if (keep_used_production) {
    if (left.used_production.size() != plan.productions().size() ||
        right.used_production.size() != plan.productions().size()) {
      throw std::runtime_error(
          "multi-site trim: child provenance vector has wrong size");
    }
    candidate.used_production = left.used_production;
    merge_used_productions(candidate.used_production, right.used_production);
    candidate.used_production[pid] = true;
  } else if (!left.used_production.empty() || !right.used_production.empty()) {
    throw std::runtime_error(
        "multi-site trim: score-only frontier unexpectedly carries provenance "
        "bitsets");
  }
  return candidate;
}

// Component-wise dominance is a score-ordering relation only.  It proves that
// any scalar completion available to rhs can be matched no-worse by lhs, so it
// is safe for computing the optimum.  It does not prove that rhs' productions
// are absent from every optimal topology: rhs can tie lhs under a particular
// outside state/context.  Exact keep_production recovery therefore needs either
// no dominance, strict mask-safe dominance, a second exact-mask pass, or a
// provenance-preserving representation.
inline bool dominates(multisite_cost_function const& lhs,
                      multisite_cost_function const& rhs) {
  if (lhs.cost.size() != rhs.cost.size()) {
    throw std::runtime_error(
        "multi-site trim: dominance cost vector size mismatch");
  }
  for (std::size_t i = 0; i < lhs.cost.size(); ++i) {
    if (lhs.cost[i] > rhs.cost[i]) return false;
  }
  return true;
}

// Legacy provenance-merging dominance helper.  Merging a dominated entry's
// used_production/provenance into the dominator is not a valid exact-mask
// recovery strategy: it can over-keep productions that are never globally
// optimal, while simply discarding the dominated entry can under-keep tied
// outside-context optima.  Public B&B trim modes use the score-only,
// strict-mask-safe, or two-pass helpers below instead.
inline void apply_dominance_pruning(std::vector<frontier_entry>& entries,
                                    std::size_t& dominance_pruned) {
  std::vector<bool> remove(entries.size(), false);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (remove[i]) continue;
    for (std::size_t j = 0; j < entries.size(); ++j) {
      if (i == j || remove[j]) continue;
      if (entries[i].f.cost == entries[j].f.cost) continue;
      if (dominates(entries[i].f, entries[j].f)) {
        merge_used_productions(entries[i].used_production,
                               entries[j].used_production);
        merge_provenance_choices(entries[i].provenance, entries[j].provenance);
        remove[j] = true;
        ++dominance_pruned;
      }
    }
  }

  std::vector<frontier_entry> kept;
  kept.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!remove[i]) kept.push_back(std::move(entries[i]));
  }
  entries = std::move(kept);
}

inline void apply_score_only_dominance_pruning(
    std::vector<frontier_entry>& entries,
    std::size_t& dominance_candidates_considered,
    std::size_t& dominance_pruned) {
  // Score-only dominance is safe for the scalar optimum, but it must not merge
  // dominated provenance into the dominator.  A dominated entry can tie the
  // dominator in an optimal outside context, so merging would over-keep and
  // discarding would under-keep for exact optimal-production masks.  Public
  // callers using this helper must label keep_production as non-exact.
  std::vector<bool> remove(entries.size(), false);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (remove[i]) continue;
    for (std::size_t j = 0; j < entries.size(); ++j) {
      if (i == j || remove[j]) continue;
      if (entries[i].f.cost == entries[j].f.cost) continue;
      ++dominance_candidates_considered;
      if (dominates(entries[i].f, entries[j].f)) {
        remove[j] = true;
        ++dominance_pruned;
      }
    }
  }

  std::vector<frontier_entry> kept;
  kept.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!remove[i]) kept.push_back(std::move(entries[i]));
  }
  entries = std::move(kept);
}

inline bool component_has_finite_outside_completion(
    clade_id clade, active_pattern_info const& info, std::uint8_t state,
    chart_options const& options) {
  parsimony_chart_detail::validate_state(state,
                                         "strict dominance outside state");
  if (!options.score_ua_edge) {
    if (info.weight == 0) return false;
    if (clade >= info.outside_ua_free.outside.size()) {
      throw std::runtime_error(
          "multi-site trim: clade out of outside range during strict "
          "dominance");
    }
    return info.outside_ua_free.outside[clade][state] < chart_inf;
  }

  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    auto count = info.reference_state_counts[reference_state];
    if (count == 0) continue;
    auto const& outside = info.outside_by_reference[reference_state];
    if (clade >= outside.outside.size()) {
      throw std::runtime_error(
          "multi-site trim: clade out of reference outside range during "
          "strict dominance");
    }
    if (outside.outside[clade][state] < chart_inf) return true;
  }
  return false;
}

inline bool strictly_mask_safely_dominates(
    multisite_cost_function const& lhs, multisite_cost_function const& rhs,
    clade_id clade, std::vector<active_pattern_info> const& active,
    chart_options const& options) {
  if (lhs.cost.size() != rhs.cost.size()) {
    throw std::runtime_error(
        "multi-site trim: strict dominance cost vector size mismatch");
  }
  if (lhs.cost.size() != active.size() * nuc_state_count) {
    throw std::runtime_error(
        "multi-site trim: strict dominance cost vector has wrong size");
  }
  if (!dominates(lhs, rhs)) return false;

  bool saw_finite_outside_component = false;
  for (std::size_t active_index = 0; active_index < active.size();
       ++active_index) {
    auto const& info = active[active_index];
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      if (!component_has_finite_outside_completion(clade, info, state,
                                                   options)) {
        continue;
      }
      saw_finite_outside_component = true;
      auto index = cost_index(active_index, state);
      if (!(lhs.cost[index] < rhs.cost[index])) return false;
    }
  }
  return saw_finite_outside_component;
}

inline void apply_strict_mask_safe_dominance_pruning(
    std::vector<frontier_entry>& entries, clade_id clade,
    std::vector<active_pattern_info> const& active,
    chart_options const& options, std::size_t& dominance_candidates_considered,
    std::size_t& dominance_pruned) {
  // Strict mask-safe dominance is conservative: a dominated entry is discarded
  // only when the dominator is strictly better on every component that can be
  // used by a finite outside completion for this clade.  Such an entry cannot
  // participate in a globally optimal topology, so its provenance must not be
  // merged into the dominator.
  std::vector<bool> remove(entries.size(), false);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (remove[i]) continue;
    for (std::size_t j = 0; j < entries.size(); ++j) {
      if (i == j || remove[j]) continue;
      if (entries[i].f.cost == entries[j].f.cost) continue;
      ++dominance_candidates_considered;
      if (strictly_mask_safely_dominates(entries[i].f, entries[j].f, clade,
                                         active, options)) {
        remove[j] = true;
        ++dominance_pruned;
      }
    }
  }

  std::vector<frontier_entry> kept;
  kept.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!remove[i]) kept.push_back(std::move(entries[i]));
  }
  entries = std::move(kept);
}

inline void insert_or_merge_frontier_entry(
    std::vector<frontier_entry>& entries,
    std::unordered_map<std::vector<chart_cost>, std::size_t,
                       chart_cost_vector_hash>& index_by_cost,
    frontier_entry candidate, std::size_t& equality_deduplicated,
    std::size_t max_provenance_choices_per_entry = 0) {
  auto found = index_by_cost.find(candidate.f.cost);
  if (found != index_by_cost.end()) {
    if (!entries[found->second].used_production.empty() ||
        !candidate.used_production.empty()) {
      merge_used_productions(entries[found->second].used_production,
                             candidate.used_production);
    }
    merge_provenance_choices(entries[found->second].provenance,
                             candidate.provenance,
                             max_provenance_choices_per_entry);
    // Diagnostic-only, but use an associative representative so equality-class
    // identity does not depend on merge grouping.
    entries[found->second].f.topology_hash = std::min(
        entries[found->second].f.topology_hash, candidate.f.topology_hash);
    ++equality_deduplicated;
    return;
  }
  if (max_provenance_choices_per_entry != 0 &&
      candidate.provenance.size() > max_provenance_choices_per_entry) {
    throw std::runtime_error(
        "multi-site topology trace: provenance-choice cap exceeded");
  }
  auto entry_index = entries.size();
  auto [_, inserted] = index_by_cost.emplace(candidate.f.cost, entry_index);
  (void)_;
  (void)inserted;
  entries.push_back(std::move(candidate));
}

inline void insert_frontier_provenance_choice(
    std::vector<frontier_provenance_choice>& choices,
    frontier_provenance_choice choice,
    std::size_t max_provenance_choices_per_entry) {
  auto less = [](frontier_provenance_choice const& lhs,
                 frontier_provenance_choice const& rhs) {
    if (lhs.production != rhs.production)
      return lhs.production < rhs.production;
    if (lhs.left_entry != rhs.left_entry)
      return lhs.left_entry < rhs.left_entry;
    return lhs.right_entry < rhs.right_entry;
  };
  auto where = std::lower_bound(choices.begin(), choices.end(), choice, less);
  if (where != choices.end() && same_provenance_choice(*where, choice)) return;
  if (max_provenance_choices_per_entry != 0 &&
      choices.size() >= max_provenance_choices_per_entry) {
    throw std::runtime_error(
        "multi-site topology trace: provenance-choice cap exceeded");
  }
  choices.insert(where, choice);
}

// Insert one Cartesian-product result while retaining a reusable cost-vector
// scratch allocation.  Full production masks and provenance are materialized
// only after bound pruning and only for a surviving equality class; duplicates
// merge their child masks directly into the resident entry.
inline void insert_or_merge_combined_frontier_entry(
    chart_execution_plan const& plan, production_id pid,
    frontier_entry const& left, frontier_entry const& right,
    std::size_t left_index, std::size_t right_index,
    multisite_cost_function const& combined, bool keep_used_production,
    bool keep_provenance, std::vector<frontier_entry>& entries,
    std::unordered_map<std::vector<chart_cost>, std::size_t,
                       chart_cost_vector_hash>& index_by_cost,
    std::size_t& equality_deduplicated,
    std::size_t max_provenance_choices_per_entry = 0) {
  if (pid == no_production || pid >= plan.productions().size()) {
    throw std::runtime_error("multi-site trim: production id out of range");
  }
  if (keep_used_production) {
    if (left.used_production.size() != plan.productions().size() ||
        right.used_production.size() != plan.productions().size()) {
      throw std::runtime_error(
          "multi-site trim: child provenance vector has wrong size");
    }
  } else if (!left.used_production.empty() || !right.used_production.empty()) {
    throw std::runtime_error(
        "multi-site trim: score-only frontier unexpectedly carries provenance "
        "bitsets");
  }

  auto found = index_by_cost.find(combined.cost);
  if (found != index_by_cost.end()) {
    auto& resident = entries[found->second];
    if (keep_used_production) {
      merge_used_productions(resident.used_production, left.used_production);
      merge_used_productions(resident.used_production, right.used_production);
      resident.used_production[pid] = true;
    }
    if (keep_provenance) {
      insert_frontier_provenance_choice(
          resident.provenance,
          frontier_provenance_choice{pid, left_index, right_index},
          max_provenance_choices_per_entry);
    }
    resident.f.topology_hash =
        std::min(resident.f.topology_hash, combined.topology_hash);
    ++equality_deduplicated;
    return;
  }

  frontier_entry candidate;
  candidate.f = combined;
  if (keep_used_production) {
    candidate.used_production = left.used_production;
    merge_used_productions(candidate.used_production, right.used_production);
    candidate.used_production[pid] = true;
  }
  if (keep_provenance) {
    candidate.provenance.push_back(
        frontier_provenance_choice{pid, left_index, right_index});
  }
  auto entry_index = entries.size();
  auto [_, inserted] = index_by_cost.emplace(candidate.f.cost, entry_index);
  (void)_;
  (void)inserted;
  entries.push_back(std::move(candidate));
}

using selected_topology = grammar_topology;

inline selected_topology empty_selected_topology(clade_grammar const& grammar) {
  return make_empty_grammar_topology(grammar);
}

inline void fill_first_topology(clade_grammar const& grammar, clade_id clade,
                                selected_topology& topo) {
  if (grammar.clades[clade].taxa.size() == 1) return;
  auto const& productions = grammar.productions_by_parent[clade];
  if (productions.empty()) {
    throw std::runtime_error(
        "multi-site trim: non-singleton clade has no productions");
  }
  auto pid = productions.front();
  auto const& prod = grammar.productions[pid];
  chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
  topo.selected_production_by_clade[clade] = pid;
  topo.used_production[pid] = true;
  for (auto child : prod.children) fill_first_topology(grammar, child, topo);
}

inline selected_topology first_topology(clade_grammar const& grammar) {
  auto topo = empty_selected_topology(grammar);
  fill_first_topology(grammar, grammar.root_clade, topo);
  return topo;
}

inline selected_topology topology_from_traceback(
    clade_grammar const& grammar, chart_traceback_result const& trace) {
  auto topo = empty_selected_topology(grammar);
  for (auto pid : trace.productions) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "multi-site trim: traceback production id out of range");
    }
    auto parent = grammar.productions[pid].parent;
    if (parent == no_clade || parent >= grammar.clades.size()) {
      throw std::runtime_error(
          "multi-site trim: traceback production parent out of range");
    }
    if (topo.selected_production_by_clade[parent] != no_production &&
        topo.selected_production_by_clade[parent] != pid) {
      throw std::runtime_error(
          "multi-site trim: traceback has conflicting production choices");
    }
    topo.selected_production_by_clade[parent] = pid;
    topo.used_production[pid] = true;
  }
  return topo;
}

inline chart_row restricted_topology_row_impl(
    clade_grammar const& grammar, site_pattern const& pattern,
    selected_topology const& topo, clade_id clade,
    std::vector<std::optional<chart_row>>& memo) {
  if (memo[clade].has_value()) return *memo[clade];

  chart_row row = make_inf_row();
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    auto taxon = key.taxa.front();
    if (taxon >= pattern.state_by_taxon.size()) {
      throw std::runtime_error(
          "multi-site trim: taxon out of site-pattern range");
    }
    auto observed = pattern.state_by_taxon[taxon];
    parsimony_chart_detail::validate_state(observed,
                                           "restricted topology leaf state");
    row[observed] = 0;
  } else {
    auto pid = topo.selected_production_by_clade[clade];
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "multi-site trim: selected topology missing production for clade");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "multi-site trim: selected production parent mismatch");
    }
    parsimony_chart_detail::validate_production_inside_row_inputs(
        grammar, prod, pid, "multi-site trim selected topology");
    std::vector<chart_row> child_rows;
    child_rows.reserve(prod.children.size());
    for (auto child : prod.children) {
      child_rows.push_back(
          restricted_topology_row_impl(grammar, pattern, topo, child, memo));
    }
    row = combine_rows(
        std::span<chart_row const>{child_rows.data(), child_rows.size()});
  }

  memo[clade] = row;
  return row;
}

inline chart_row restricted_topology_row(clade_grammar const& grammar,
                                         site_pattern const& pattern,
                                         selected_topology const& topo) {
  std::vector<std::optional<chart_row>> memo(grammar.clades.size());
  return restricted_topology_row_impl(grammar, pattern, topo,
                                      grammar.root_clade, memo);
}

inline std::uint64_t score_selected_topology(clade_grammar const& grammar,
                                             site_pattern_set const& patterns,
                                             selected_topology const& topo,
                                             chart_options const& options) {
  std::uint64_t total = invariant_constant_offset(patterns, options);
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (options.score_ua_edge)
      validate_pattern_reference_counts(pattern, pattern_index);
    if (!is_active_pattern(pattern)) continue;
    auto row = restricted_topology_row(grammar, pattern, topo);
    total = checked_add_u64(total,
                            weighted_root_score_from_row(row, pattern, options),
                            "selected topology score");
  }
  return total;
}

inline std::uint64_t initial_upper_bound(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    std::vector<active_pattern_info> const& active,
    chart_options const& options) {
  std::uint64_t best = multisite_score_inf;
  auto first = first_topology(grammar);
  best = std::min(best,
                  score_selected_topology(grammar, patterns, first, options));

  for (auto const& info : active) {
    single_site_outside_chart const* outside = nullptr;
    if (options.score_ua_edge) {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        if (info.reference_state_counts[reference_state] != 0) {
          outside = &info.outside_by_reference[reference_state];
          break;
        }
      }
    } else {
      outside = &info.outside_ua_free;
    }
    if (outside == nullptr) continue;
    auto trace = deterministic_optimal_single_site_traceback(
        grammar, info.chart, *outside);
    auto topo = topology_from_traceback(grammar, trace);
    best = std::min(best,
                    score_selected_topology(grammar, patterns, topo, options));
  }
  return best;
}

inline selected_topology empty_selected_topology(
    chart_execution_plan const& plan) {
  selected_topology topology;
  topology.selected_production_by_clade.assign(plan.clades().size(),
                                               no_production);
  topology.used_production.assign(plan.productions().size(), false);
  return topology;
}

inline void fill_first_topology(chart_execution_plan const& plan,
                                clade_id clade, selected_topology& topo) {
  if (plan.clade(clade).is_leaf()) return;
  auto productions = plan.productions_for_parent(clade);
  if (productions.empty()) {
    throw std::runtime_error(
        "multi-site trim: non-singleton clade has no productions");
  }
  auto pid = productions.front();
  auto const& prod = plan.production(pid);
  chart_trim_detail::validate_binary_production_for_trim(plan, prod, pid);
  topo.selected_production_by_clade[clade] = pid;
  topo.used_production[pid] = true;
  for (auto child : plan.children(pid)) {
    fill_first_topology(plan, child, topo);
  }
}

inline selected_topology first_topology(chart_execution_plan const& plan) {
  auto topo = empty_selected_topology(plan);
  fill_first_topology(plan, plan.root_clade(), topo);
  return topo;
}

inline selected_topology topology_from_traceback(
    chart_execution_plan const& plan, chart_traceback_result const& trace) {
  auto topo = empty_selected_topology(plan);
  for (auto pid : trace.productions) {
    auto const& prod = plan.production(pid);
    auto parent = prod.parent;
    if (parent == no_clade || parent >= plan.clades().size()) {
      throw std::runtime_error(
          "multi-site trim: traceback production parent out of range");
    }
    if (topo.selected_production_by_clade[parent] != no_production &&
        topo.selected_production_by_clade[parent] != pid) {
      throw std::runtime_error(
          "multi-site trim: traceback has conflicting production choices");
    }
    topo.selected_production_by_clade[parent] = pid;
    topo.used_production[pid] = true;
  }
  return topo;
}

inline chart_row restricted_topology_row_impl(
    chart_execution_plan const& plan, site_pattern const& pattern,
    selected_topology const& topo, clade_id clade,
    std::vector<std::optional<chart_row>>& memo) {
  if (memo[clade].has_value()) return *memo[clade];

  chart_row row = make_inf_row();
  auto const& key = plan.clade(clade);
  if (key.is_leaf()) {
    auto taxon = key.leaf_taxon;
    if (taxon >= pattern.state_by_taxon.size()) {
      throw std::runtime_error(
          "multi-site trim: taxon out of site-pattern range");
    }
    auto observed = pattern.state_by_taxon[taxon];
    parsimony_chart_detail::validate_state(observed,
                                           "restricted topology leaf state");
    row[observed] = 0;
  } else {
    auto pid = topo.selected_production_by_clade[clade];
    auto const& prod = plan.production(pid);
    if (prod.parent != clade) {
      throw std::runtime_error(
          "multi-site trim: selected production parent mismatch");
    }
    std::vector<chart_row> child_rows;
    auto children = plan.children(pid);
    child_rows.reserve(children.size());
    for (auto child : children) {
      child_rows.push_back(
          restricted_topology_row_impl(plan, pattern, topo, child, memo));
    }
    row = combine_rows(
        plan,
        std::span<chart_row const>{child_rows.data(), child_rows.size()});
  }

  memo[clade] = row;
  return row;
}

inline chart_row restricted_topology_row(chart_execution_plan const& plan,
                                         site_pattern const& pattern,
                                         selected_topology const& topo) {
  if (topo.selected_production_by_clade.size() != plan.clades().size()) {
    throw std::runtime_error(
        "multi-site trim: selected topology clade vector has wrong size");
  }
  if (topo.used_production.size() != plan.productions().size()) {
    throw std::runtime_error(
        "multi-site trim: selected topology production vector has wrong size");
  }
  std::vector<std::optional<chart_row>> memo(plan.clades().size());
  return restricted_topology_row_impl(plan, pattern, topo, plan.root_clade(),
                                      memo);
}

inline std::uint64_t score_selected_topology(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    selected_topology const& topo, chart_options const& options) {
  std::uint64_t total = invariant_constant_offset(plan, patterns, options);
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (options.score_ua_edge) {
      validate_pattern_reference_counts(pattern, pattern_index);
    }
    if (!is_active_pattern(pattern)) continue;
    auto row = restricted_topology_row(plan, pattern, topo);
    total = checked_add_u64(
        total, weighted_root_score_from_row(plan, row, pattern, options),
        "selected topology score");
  }
  return total;
}

inline std::uint64_t initial_upper_bound(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    std::vector<active_pattern_info> const& active,
    chart_options const& options) {
  std::uint64_t best = multisite_score_inf;
  auto first = first_topology(plan);
  best =
      std::min(best, score_selected_topology(plan, patterns, first, options));

  for (auto const& info : active) {
    single_site_outside_chart const* outside = nullptr;
    if (options.score_ua_edge) {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        if (info.reference_state_counts[reference_state] != 0) {
          outside = &info.outside_by_reference[reference_state];
          break;
        }
      }
    } else {
      outside = &info.outside_ua_free;
    }
    if (outside == nullptr) continue;
    auto trace =
        deterministic_optimal_single_site_traceback(plan, info.chart, *outside);
    auto topo = topology_from_traceback(plan, trace);
    best = std::min(
        best, score_selected_topology(plan, patterns, topo, options));
  }
  return best;
}

inline std::vector<selected_topology> enumerate_topologies(
    clade_grammar const& grammar, clade_id clade, std::size_t max_topologies) {
  if (grammar.clades[clade].taxa.size() == 1) {
    return {empty_selected_topology(grammar)};
  }

  std::vector<selected_topology> result;
  for (auto pid : grammar.productions_by_parent[clade]) {
    auto const& prod = grammar.productions[pid];
    chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
    auto left_topologies =
        enumerate_topologies(grammar, prod.children[0], max_topologies);
    auto right_topologies =
        enumerate_topologies(grammar, prod.children[1], max_topologies);
    for (auto const& left : left_topologies) {
      for (auto const& right : right_topologies) {
        if (max_topologies != 0 && result.size() >= max_topologies) {
          throw std::runtime_error(
              "multi-site trim: brute-force topology cap exceeded");
        }
        auto topo = empty_selected_topology(grammar);
        for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
          auto lpid = left.selected_production_by_clade[cid];
          auto rpid = right.selected_production_by_clade[cid];
          if (lpid != no_production)
            topo.selected_production_by_clade[cid] = lpid;
          if (rpid != no_production) {
            if (topo.selected_production_by_clade[cid] != no_production &&
                topo.selected_production_by_clade[cid] != rpid) {
              throw std::runtime_error(
                  "multi-site trim: incompatible child topology choices");
            }
            topo.selected_production_by_clade[cid] = rpid;
          }
        }
        topo.used_production = left.used_production;
        merge_used_productions(topo.used_production, right.used_production);
        topo.selected_production_by_clade[clade] = pid;
        topo.used_production[pid] = true;
        result.push_back(std::move(topo));
      }
    }
  }
  return result;
}

inline void merge_topology_into(selected_topology& dst,
                                selected_topology const& src) {
  if (dst.selected_production_by_clade.size() !=
          src.selected_production_by_clade.size() ||
      dst.used_production.size() != src.used_production.size()) {
    throw std::runtime_error(
        "multi-site topology trace: topology shape mismatch");
  }
  for (std::size_t cid = 0; cid < src.selected_production_by_clade.size();
       ++cid) {
    auto pid = src.selected_production_by_clade[cid];
    if (pid == no_production) continue;
    auto& selected = dst.selected_production_by_clade[cid];
    if (selected != no_production && selected != pid) {
      throw std::runtime_error(
          "multi-site topology trace: incompatible child topology choices");
    }
    selected = pid;
  }
  merge_used_productions(dst.used_production, src.used_production);
}

inline selected_topology combine_selected_child_topologies(
    clade_grammar const& grammar, clade_id parent, production_id pid,
    selected_topology const& left, selected_topology const& right) {
  auto topology = empty_selected_topology(grammar);
  merge_topology_into(topology, left);
  merge_topology_into(topology, right);
  if (parent == no_clade ||
      parent >= topology.selected_production_by_clade.size()) {
    throw std::runtime_error(
        "multi-site topology trace: parent clade out of range");
  }
  auto& selected = topology.selected_production_by_clade[parent];
  if (selected != no_production && selected != pid) {
    throw std::runtime_error(
        "multi-site topology trace: incompatible parent topology choice");
  }
  if (pid == no_production || pid >= topology.used_production.size()) {
    throw std::runtime_error(
        "multi-site topology trace: selected production out of range");
  }
  selected = pid;
  topology.used_production[pid] = true;
  return topology;
}

inline bool topology_lexicographic_less(selected_topology const& lhs,
                                        selected_topology const& rhs) {
  return grammar_topology_less(lhs, rhs);
}

inline void sort_and_unique_topologies(
    std::vector<selected_topology>& topologies) {
  std::sort(topologies.begin(), topologies.end(), topology_lexicographic_less);
  topologies.erase(
      std::unique(topologies.begin(), topologies.end(), grammar_topology_equal),
      topologies.end());
}

inline std::vector<selected_topology> enumerate_topologies_from_provenance(
    clade_grammar const& grammar,
    std::vector<std::vector<frontier_entry>> const& frontiers, clade_id clade,
    std::size_t entry_index, std::size_t max_topologies, bool& truncated) {
  if (clade == no_clade || clade >= grammar.clades.size() ||
      clade >= frontiers.size()) {
    throw std::runtime_error(
        "multi-site topology trace: clade out of provenance range");
  }
  if (entry_index >= frontiers[clade].size()) {
    throw std::runtime_error(
        "multi-site topology trace: frontier entry out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1) {
    return {empty_selected_topology(grammar)};
  }

  auto const& entry = frontiers[clade][entry_index];
  if (entry.provenance.empty()) {
    throw std::runtime_error(
        "multi-site topology trace: internal frontier entry has no provenance");
  }

  std::vector<selected_topology> result;
  for (auto const& choice : entry.provenance) {
    if (max_topologies != 0 && result.size() >= max_topologies) {
      truncated = true;
      break;
    }
    if (choice.production == no_production ||
        choice.production >= grammar.productions.size()) {
      throw std::runtime_error(
          "multi-site topology trace: provenance production out of range");
    }
    auto const& prod = grammar.productions[choice.production];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "multi-site topology trace: provenance parent mismatch");
    }
    chart_trim_detail::validate_binary_production_for_trim(grammar, prod,
                                                           choice.production);

    auto remaining = [&](std::size_t used) -> std::size_t {
      if (max_topologies == 0) return std::size_t{0};
      return used >= max_topologies ? std::size_t{1} : max_topologies - used;
    };

    auto left_topologies = enumerate_topologies_from_provenance(
        grammar, frontiers, prod.children[0], choice.left_entry,
        remaining(result.size()), truncated);
    auto right_topologies = enumerate_topologies_from_provenance(
        grammar, frontiers, prod.children[1], choice.right_entry,
        remaining(result.size()), truncated);

    for (auto const& left : left_topologies) {
      for (auto const& right : right_topologies) {
        if (max_topologies != 0 && result.size() >= max_topologies) {
          truncated = true;
          break;
        }
        result.push_back(combine_selected_child_topologies(
            grammar, clade, choice.production, left, right));
      }
      if (max_topologies != 0 && result.size() >= max_topologies) break;
    }
  }
  sort_and_unique_topologies(result);
  return result;
}

inline std::size_t count_new_required_coverage(
    selected_topology const& topology,
    std::vector<production_id> const& required,
    std::vector<bool> const& covered) {
  if (topology.used_production.empty()) return 0;
  std::size_t count = 0;
  for (auto pid : required) {
    if (pid == no_production || pid >= topology.used_production.size()) {
      throw std::runtime_error(
          "multi-site topology trace: required production out of range");
    }
    if (!covered[pid] && topology.used_production[pid]) ++count;
  }
  return count;
}

inline std::vector<selected_topology> choose_topologies_for_required_coverage(
    clade_grammar const& grammar, std::vector<selected_topology> topologies,
    std::vector<production_id> const& required, std::size_t max_topologies,
    std::vector<production_id>& uncovered) {
  sort_and_unique_topologies(topologies);
  std::vector<selected_topology> chosen;
  std::vector<bool> selected(topologies.size(), false);
  std::vector<bool> covered(grammar.productions.size(), false);

  auto mark_covered = [&](selected_topology const& topology) {
    auto reachable = validate_grammar_topology(grammar, topology);
    for (auto pid : required) {
      if (pid == no_production || pid >= reachable.size()) {
        throw std::runtime_error(
            "multi-site topology trace: required production out of range");
      }
      if (reachable[pid]) covered[pid] = true;
    }
  };

  if (required.empty()) {
    for (auto const& topology : topologies) {
      if (max_topologies != 0 && chosen.size() >= max_topologies) break;
      chosen.push_back(topology);
      mark_covered(chosen.back());
    }
  } else {
    while (max_topologies == 0 || chosen.size() < max_topologies) {
      std::size_t best = topologies.size();
      std::size_t best_count = 0;
      for (std::size_t i = 0; i < topologies.size(); ++i) {
        if (selected[i]) continue;
        auto count =
            count_new_required_coverage(topologies[i], required, covered);
        if (count > best_count ||
            (count == best_count && count != 0 && best != topologies.size() &&
             topology_lexicographic_less(topologies[i], topologies[best]))) {
          best = i;
          best_count = count;
        }
      }
      if (best == topologies.size() || best_count == 0) break;
      selected[best] = true;
      chosen.push_back(topologies[best]);
      mark_covered(chosen.back());

      bool all_covered = true;
      for (auto pid : required) all_covered = all_covered && covered[pid];
      if (all_covered) break;
    }

    if (chosen.empty() && !topologies.empty() &&
        (max_topologies == 0 || chosen.size() < max_topologies)) {
      selected[0] = true;
      chosen.push_back(topologies.front());
      mark_covered(chosen.back());
    }

    bool all_covered = true;
    for (auto pid : required) all_covered = all_covered && covered[pid];
    if (!all_covered) {
      for (std::size_t i = 0; i < topologies.size(); ++i) {
        if (selected[i]) continue;
        if (max_topologies != 0 && chosen.size() >= max_topologies) break;
        selected[i] = true;
        chosen.push_back(topologies[i]);
        mark_covered(chosen.back());
      }
    }
  }

  uncovered.clear();
  for (auto pid : required) {
    if (pid == no_production || pid >= covered.size()) {
      throw std::runtime_error(
          "multi-site topology trace: required production out of range");
    }
    if (!covered[pid]) uncovered.push_back(pid);
  }
  return chosen;
}

inline void validate_required_productions(
    clade_grammar const& grammar,
    std::vector<production_id> const& required_productions) {
  std::vector<production_id> sorted = required_productions;
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    throw std::runtime_error(
        "multi-site topology trace: duplicate required production id");
  }
  for (auto pid : sorted) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "multi-site topology trace: required production out of range");
    }
  }
}

inline void validate_multisite_trim_options_supported(
    multisite_trim_options const& trim_options, std::string const& context,
    bool allow_score_only_dominance) {
  if (trim_options.dominance_mode == multisite_dominance_mode::off) return;

  if (trim_options.dominance_mode ==
      multisite_dominance_mode::strict_mask_safe) {
    // Strict mask-safe dominance discards only entries that are strictly worse
    // on every finite outside-completable component, so it is safe for exact
    // keep-production masks and exact topology witnesses.
    return;
  }

  if (trim_options.dominance_mode == multisite_dominance_mode::score_only) {
    if (!allow_score_only_dominance) {
      throw std::runtime_error(
          context +
          ": score-only dominance cannot emit exact topology witnesses; rerun "
          "with dominance off or strict-mask-safe dominance (or a future "
          "provenance-preserving mode) and a validated known_exact_optimum");
    }
    if (trim_options.require_exact_keep_mask) {
      throw std::runtime_error(
          context +
          ": score-only dominance cannot return an exact keep-production "
          "mask; set require_exact_keep_mask=false (CLI: "
          "--chart-bnb-score-only)");
    }
    return;
  }

  if (trim_options.dominance_mode ==
      multisite_dominance_mode::two_pass_exact_mask) {
    if (!allow_score_only_dominance) {
      throw std::runtime_error(
          context +
          ": two-pass exact-mask dominance cannot emit exact topology "
          "witnesses; rerun with dominance off or strict-mask-safe dominance "
          "(or a future provenance-preserving mode) and a validated "
          "known_exact_optimum");
    }
    if (!trim_options.require_exact_keep_mask) {
      throw std::runtime_error(
          context +
          ": two-pass-exact-mask dominance is an exact-mask recovery mode; "
          "use dominance score-only with require_exact_keep_mask=false (CLI: "
          "--chart-bnb-dominance score-only --chart-bnb-score-only) for a "
          "score-only run");
    }
    return;
  }

  throw std::runtime_error(
      context + ": dominance mode '" +
      multisite_dominance_mode_name(trim_options.dominance_mode) +
      "' is not implemented in this public trim path yet; implemented modes "
      "are off, score-only, strict-mask-safe, and two-pass-exact-mask for "
      "exact masks");
}

inline multisite_keep_mask_kind keep_mask_kind_for_options(
    multisite_trim_options const& trim_options) {
  if (!trim_options.require_exact_keep_mask ||
      trim_options.dominance_mode == multisite_dominance_mode::score_only) {
    return multisite_keep_mask_kind::score_only_not_exact;
  }
  return multisite_keep_mask_kind::exact_optimal_production_union;
}

inline std::uint64_t effective_pruning_upper_bound(
    std::uint64_t initial_upper_bound,
    multisite_trim_options const& trim_options) {
  if (trim_options.upper_bound_override) {
    return *trim_options.upper_bound_override;
  }
  return initial_upper_bound;
}

inline void validate_known_exact_optimum(
    std::uint64_t computed_root_optimum,
    multisite_trim_options const& trim_options, std::string const& context) {
  if (!trim_options.known_exact_optimum) return;
  if (computed_root_optimum == *trim_options.known_exact_optimum) return;
  throw std::runtime_error(
      context +
      ": known_exact_optimum validation failed: computed root "
      "optimum " +
      std::to_string(computed_root_optimum) +
      " differs from known_exact_optimum " +
      std::to_string(*trim_options.known_exact_optimum) +
      "; upper_bound_override is pruning-only and is not evidence of "
      "exactness");
}

inline void validate_multisite_inputs(clade_grammar const& grammar,
                                      site_pattern_set const& patterns,
                                      chart_options const& options) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error("multi-site trim: root clade out of range");
  }
  if (patterns.taxon_count != grammar.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(
        "multi-site trim: site-pattern set taxon count mismatch");
  }
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (pattern.state_by_taxon.size() != grammar.taxa.id_to_sample_id.size()) {
      throw std::runtime_error(
          "multi-site trim: site-pattern taxon count mismatch");
    }
    for (auto state : pattern.state_by_taxon) {
      parsimony_chart_detail::validate_state(state, "site-pattern state");
    }
    if (options.score_ua_edge)
      validate_pattern_reference_counts(pattern, pattern_index);
  }
}

inline void validate_multisite_inputs(chart_execution_plan const& plan,
                                      site_pattern_set const& patterns,
                                      chart_options const& options) {
  plan.assert_valid();
  if (patterns.taxon_count != plan.taxon_count()) {
    throw std::runtime_error(
        "multi-site trim: site-pattern set taxon count mismatch");
  }
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (pattern.state_by_taxon.size() != plan.taxon_count()) {
      throw std::runtime_error(
          "multi-site trim: site-pattern taxon count mismatch");
    }
    for (auto state : pattern.state_by_taxon) {
      parsimony_chart_detail::validate_state(state, "site-pattern state");
    }
    if (options.score_ua_edge) {
      validate_pattern_reference_counts(pattern, pattern_index);
    }
  }
}

inline void require_no_multifurcating_productions_for_consumer(
    chart_execution_plan const& plan, arity_gate_consumer consumer,
    std::string_view context, std::string_view layer,
    std::string_view resolution) {
  if (plan.all_binary()) return;
  auto productions = plan.productions();
  auto first = std::find_if(
      productions.begin(), productions.end(),
      [](chart_plan_production_descriptor const& production) {
        return !production.is_binary();
      });
  parsimony_chart_detail::record_arity_gate_throw(consumer);
  auto const pid = first == productions.end() ? no_production
                                               : first->source_id;
  auto const arity =
      first == productions.end() ? plan.max_arity() : first->child_count;
  throw std::runtime_error(
      std::string{context} +
      ": WI6 arity gate: the chart supports multifurcations; this consumer's " +
      std::string{layer} + " does not (production " + std::to_string(pid) +
      " has arity " + std::to_string(arity) + ", grammar max arity " +
      std::to_string(plan.max_arity()) + "); " + std::string{resolution});
}

// A finalized exact setup owns every dynamic input needed by frontier
// construction.  In particular, active_patterns contains copied leaf states
// and owned outside charts, but its transient inside-chart members are empty.
// Pattern sets and resident/cold inside charts are needed only while this setup
// is constructed.
struct multisite_exact_setup {
  std::vector<active_pattern_info> active_patterns;
  std::uint64_t composite_lower_bound = multisite_score_inf;
  std::uint64_t initial_upper_bound = multisite_score_inf;
  std::uint64_t invariant_constant_offset = 0;
  std::size_t clade_count = 0;
  std::size_t production_count = 0;
  std::size_t taxon_count = 0;
  std::uint64_t structural_generation = 0;
  chart_plan_fingerprint structural_fingerprint;
  bool score_ua_edge = false;
  multisite_exact_setup_work_stats work;
};

inline std::size_t exact_setup_clade_count(clade_grammar const& grammar) {
  return grammar.clades.size();
}

inline std::size_t exact_setup_clade_count(chart_execution_plan const& plan) {
  return plan.clades().size();
}

inline std::size_t exact_setup_production_count(
    clade_grammar const& grammar) {
  return grammar.productions.size();
}

inline std::size_t exact_setup_production_count(
    chart_execution_plan const& plan) {
  return plan.productions().size();
}

inline std::uint64_t exact_setup_structural_generation(
    clade_grammar const& grammar) {
  return grammar.execution_generation;
}

inline std::uint64_t exact_setup_structural_generation(
    chart_execution_plan const& plan) {
  return plan.grammar_generation();
}

inline chart_plan_fingerprint exact_setup_structural_fingerprint(
    clade_grammar const& grammar) {
  return chart_execution_plan_detail::fingerprint_chart_grammar(grammar);
}

inline chart_plan_fingerprint exact_setup_structural_fingerprint(
    chart_execution_plan const& plan) {
  return plan.fingerprint();
}

inline std::uint64_t exact_setup_weighted_root_score(
    clade_grammar const& grammar, single_site_chart const& chart,
    site_pattern const& pattern, chart_options const& options) {
  return weighted_root_score_from_row(chart.inside[grammar.root_clade], pattern,
                                      options);
}

inline std::uint64_t exact_setup_weighted_root_score(
    chart_execution_plan const& plan, single_site_chart const& chart,
    site_pattern const& pattern, chart_options const& options) {
  return weighted_root_score_from_row(
      plan, chart.inside[plan.root_clade()], pattern, options);
}

inline std::uint64_t exact_setup_weighted_topology_root_score(
    clade_grammar const&, chart_row const& row, site_pattern const& pattern,
    chart_options const& options) {
  return weighted_root_score_from_row(row, pattern, options);
}

inline std::uint64_t exact_setup_weighted_topology_root_score(
    chart_execution_plan const& plan, chart_row const& row,
    site_pattern const& pattern, chart_options const& options) {
  return weighted_root_score_from_row(plan, row, pattern, options);
}

inline std::uint64_t exact_setup_invariant_constant_offset(
    clade_grammar const&, site_pattern_set const& patterns,
    chart_options const& options) {
  return invariant_constant_offset(patterns, options);
}

inline std::uint64_t exact_setup_invariant_constant_offset(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options) {
  return invariant_constant_offset(plan, patterns, options);
}

inline void require_exact_setup_binary(clade_grammar const& grammar) {
  parsimony_chart_detail::require_no_multifurcating_productions_for_consumer(
      grammar, arity_gate_consumer::multisite_trim, "multi-site exact setup",
      "B&B frontier",
      "use --wric-polytomy-mode expand-exact or expand-bounded before B&B "
      "trim, or use an arity-agnostic SPR/fixed-topology path");
}

inline void require_exact_setup_binary(chart_execution_plan const& plan) {
  require_no_multifurcating_productions_for_consumer(
      plan, arity_gate_consumer::multisite_trim, "multi-site exact setup",
      "B&B frontier",
      "use --wric-polytomy-mode expand-exact or expand-bounded before B&B "
      "trim, or use an arity-agnostic SPR/fixed-topology path");
}

inline bool exact_setup_has_unique_reachable_topology(
    clade_grammar const& grammar) {
  std::vector<bool> visited(grammar.clades.size(), false);
  std::vector<clade_id> pending{grammar.root_clade};
  while (!pending.empty()) {
    auto const clade = pending.back();
    pending.pop_back();
    if (clade == no_clade || clade >= grammar.clades.size()) {
      throw std::runtime_error(
          "multi-site exact setup: reachable clade out of range");
    }
    if (visited[clade]) continue;
    visited[clade] = true;
    auto const& key = grammar.clades[clade];
    auto const& productions = grammar.productions_by_parent[clade];
    if (key.taxa.size() == 1) {
      if (!productions.empty()) {
        throw std::runtime_error(
            "multi-site exact setup: reachable leaf has productions");
      }
      continue;
    }
    if (productions.size() != 1) return false;
    auto const pid = productions.front();
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "multi-site exact setup: reachable production out of range");
    }
    auto const& production = grammar.productions[pid];
    if (production.parent != clade) {
      throw std::runtime_error(
          "multi-site exact setup: reachable production parent mismatch");
    }
    chart_trim_detail::validate_binary_production_for_trim(grammar, production,
                                                           pid);
    pending.insert(pending.end(), production.children.begin(),
                   production.children.end());
  }
  return true;
}

inline bool exact_setup_has_unique_reachable_topology(
    chart_execution_plan const& plan) {
  std::vector<bool> visited(plan.clades().size(), false);
  std::vector<clade_id> pending{plan.root_clade()};
  while (!pending.empty()) {
    auto const clade = pending.back();
    pending.pop_back();
    if (clade == no_clade || clade >= plan.clades().size()) {
      throw std::runtime_error(
          "multi-site exact setup: reachable clade out of range");
    }
    if (visited[clade]) continue;
    visited[clade] = true;
    auto const productions = plan.productions_for_parent(clade);
    if (plan.clade(clade).is_leaf()) {
      if (!productions.empty()) {
        throw std::runtime_error(
            "multi-site exact setup: reachable leaf has productions");
      }
      continue;
    }
    if (productions.size() != 1) return false;
    auto const pid = productions.front();
    auto const& production = plan.production(pid);
    if (production.parent != clade) {
      throw std::runtime_error(
          "multi-site exact setup: reachable production parent mismatch");
    }
    chart_trim_detail::validate_binary_production_for_trim(plan, production,
                                                           pid);
    auto const children = plan.children(pid);
    pending.insert(pending.end(), children.begin(), children.end());
  }
  return true;
}

template <class Structural, class InsideProvider>
inline multisite_exact_setup build_multisite_exact_setup_from_inside(
    Structural const& structural, site_pattern_set const& patterns,
    chart_options const& options, InsideProvider&& inside_provider,
    bool resident_inside) {
  validate_multisite_inputs(structural, patterns, options);
  require_exact_setup_binary(structural);

  multisite_exact_setup setup;
  setup.clade_count = exact_setup_clade_count(structural);
  setup.production_count = exact_setup_production_count(structural);
  setup.taxon_count = patterns.taxon_count;
  setup.structural_generation =
      exact_setup_structural_generation(structural);
  setup.structural_fingerprint =
      exact_setup_structural_fingerprint(structural);
  setup.score_ua_edge = options.score_ua_edge;
  setup.invariant_constant_offset =
      exact_setup_invariant_constant_offset(structural, patterns, options);
  setup.composite_lower_bound = setup.invariant_constant_offset;
  setup.work.setup_builds = 1;
  setup.active_patterns.reserve(patterns.patterns.size());

  // With exactly one production at every reachable internal clade there is
  // exactly one feasible topology.  The sum of per-pattern chart optima is
  // therefore also a feasible coupled score, so no per-pattern traceback,
  // topology materialization/deduplication, or full topology rescore is
  // needed to establish the initial upper bound.
  auto const unique_reachable_topology =
      exact_setup_has_unique_reachable_topology(structural);

  std::vector<selected_topology> upper_bound_topologies;
  if (!unique_reachable_topology) {
    upper_bound_topologies.reserve(patterns.patterns.size() + 1);
    upper_bound_topologies.push_back(first_topology(structural));
  }

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (!is_active_pattern(pattern)) continue;

    // A provider may return either a resident reference or a cold value.  The
    // result is consumed completely in this iteration and is never retained.
    decltype(auto) provided_chart =
        std::invoke(inside_provider, pattern_index, pattern);
    single_site_chart const& chart = provided_chart;
    chart_trim_detail::validate_chart_shapes(structural, chart);
    if (resident_inside) {
      ++setup.work.resident_inside_charts_consumed;
    } else {
      ++setup.work.inside_charts_built;
    }

    setup.composite_lower_bound = checked_add_u64(
        setup.composite_lower_bound,
        exact_setup_weighted_root_score(structural, chart, pattern, options),
        "exact setup composite lower bound");

    active_pattern_info info;
    info.pattern_index = pattern_index;
    info.weight = pattern.weight;
    info.reference_state_counts = pattern.reference_state_counts;
    info.state_by_taxon = pattern.state_by_taxon;
    ++setup.work.active_leaf_state_vectors_copied;
    setup.work.active_leaf_states_copied += info.state_by_taxon.size();

    single_site_outside_chart const* traceback_outside = nullptr;
    if (options.score_ua_edge) {
      for (std::uint8_t reference_state = 0;
           reference_state < nuc_state_count; ++reference_state) {
        if (info.reference_state_counts[reference_state] == 0) continue;
        info.outside_by_reference[reference_state] =
            build_single_site_outside_chart(structural, chart, options,
                                            reference_state);
        setup.work.outside_recurrence_work +=
            info.outside_by_reference[reference_state].recurrence_work;
        ++setup.work.outside_boundary_charts_built;
        if (traceback_outside == nullptr) {
          traceback_outside = &info.outside_by_reference[reference_state];
        }
      }
    } else {
      info.outside_ua_free =
          build_single_site_outside_chart(structural, chart, options);
      setup.work.outside_recurrence_work +=
          info.outside_ua_free.recurrence_work;
      ++setup.work.outside_boundary_charts_built;
      traceback_outside = &info.outside_ua_free;
    }

    // A zero-weight active pattern can have no used reference state.  It still
    // participates in frontier vector shape, but (as in the legacy path) does
    // not contribute a traceback-derived feasible topology.
    if (!unique_reachable_topology && traceback_outside != nullptr) {
      auto trace = deterministic_optimal_single_site_traceback(
          structural, chart, *traceback_outside);
      upper_bound_topologies.push_back(
          topology_from_traceback(structural, trace));
    }

    // Deliberately do not copy chart into info: after the root score,
    // outside boundaries, and traceback topology are finalized, no frontier
    // operation needs an inside chart.
    setup.active_patterns.push_back(std::move(info));
  }

  if (unique_reachable_topology) {
    // Count the one topology established by the structural proof even though
    // the fast path deliberately does not materialize a selected_topology
    // object.  These counters describe feasible topology candidates
    // established for the upper bound, not allocation events.
    setup.work.upper_bound_topologies_generated = 1;
    setup.work.upper_bound_topologies_unique = 1;
    setup.initial_upper_bound = setup.composite_lower_bound;
    return setup;
  }

  setup.work.upper_bound_topologies_generated =
      upper_bound_topologies.size();
  sort_and_unique_topologies(upper_bound_topologies);
  setup.work.upper_bound_topologies_unique = upper_bound_topologies.size();
  for (auto const& topology : upper_bound_topologies) {
    setup.initial_upper_bound =
        std::min(setup.initial_upper_bound,
                 ::larch::chart_multisite_detail::score_selected_topology(
                     structural, patterns, topology, options));
  }
  return setup;
}

// Exact setup has enough work per active pattern that even a small pattern set
// can profitably use the search-lifetime scheduler.  Keeping this policy local
// also avoids inheriting the candidate-oriented scheduler's coarser default
// grain.  The task fan-out remains bounded by the scheduler's adaptive range
// planner.
inline chart_indexed_range_options multisite_exact_setup_range_options() {
  return chart_indexed_range_options{
      .minimum_grain = 1,
      .target_ranges_per_worker = 4,
  };
}

struct scheduled_multisite_exact_pattern_slot {
  active_pattern_info info;
  std::optional<selected_topology> traceback_topology;
  std::uint64_t weighted_root_score = 0;
  multisite_exact_setup_work_stats work;
};

template <class Structural, class InsideProvider>
inline void build_scheduled_multisite_exact_pattern_slot(
    Structural const& structural, site_pattern_set const& patterns,
    std::size_t pattern_index, chart_options const& options,
    InsideProvider& inside_provider, bool resident_inside,
    bool collect_traceback_topology, std::size_t stable_slot_id,
    scheduled_multisite_exact_pattern_slot& result) {
  auto const& pattern = patterns.patterns[pattern_index];

  // A scheduled provider receives the scheduler's stable slot ID.  Providers
  // that materialize a transient chart (for example from a persistent row
  // cache) must own one scratch chart per resolved scheduler slot.  The
  // returned chart is consumed completely before this function returns.
  decltype(auto) provided_chart =
      std::invoke(inside_provider, pattern_index, pattern, stable_slot_id);
  single_site_chart const& chart = provided_chart;
  chart_trim_detail::validate_chart_shapes(structural, chart);
  if (resident_inside) {
    ++result.work.resident_inside_charts_consumed;
  } else {
    ++result.work.inside_charts_built;
  }

  result.weighted_root_score =
      exact_setup_weighted_root_score(structural, chart, pattern, options);

  active_pattern_info info;
  info.pattern_index = pattern_index;
  info.weight = pattern.weight;
  info.reference_state_counts = pattern.reference_state_counts;
  info.state_by_taxon = pattern.state_by_taxon;
  ++result.work.active_leaf_state_vectors_copied;
  result.work.active_leaf_states_copied += info.state_by_taxon.size();

  single_site_outside_chart const* traceback_outside = nullptr;
  if (options.score_ua_edge) {
    for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
         ++reference_state) {
      if (info.reference_state_counts[reference_state] == 0) continue;
      info.outside_by_reference[reference_state] =
          build_single_site_outside_chart(structural, chart, options,
                                          reference_state);
      result.work.outside_recurrence_work +=
          info.outside_by_reference[reference_state].recurrence_work;
      ++result.work.outside_boundary_charts_built;
      if (traceback_outside == nullptr) {
        traceback_outside = &info.outside_by_reference[reference_state];
      }
    }
  } else {
    info.outside_ua_free =
        build_single_site_outside_chart(structural, chart, options);
    result.work.outside_recurrence_work += info.outside_ua_free.recurrence_work;
    ++result.work.outside_boundary_charts_built;
    traceback_outside = &info.outside_ua_free;
  }

  if (collect_traceback_topology && traceback_outside != nullptr) {
    auto trace = deterministic_optimal_single_site_traceback(
        structural, chart, *traceback_outside);
    result.traceback_topology.emplace(
        topology_from_traceback(structural, trace));
  }
  // Publish into this stable active-pattern slot only after every computation
  // that borrows the provider's chart has completed.
  result.info = std::move(info);
}

inline void add_scheduled_multisite_exact_pattern_work(
    multisite_exact_setup_work_stats& total,
    multisite_exact_setup_work_stats const& pattern) {
  total.inside_charts_built += pattern.inside_charts_built;
  total.resident_inside_charts_consumed +=
      pattern.resident_inside_charts_consumed;
  total.active_leaf_state_vectors_copied +=
      pattern.active_leaf_state_vectors_copied;
  total.active_leaf_states_copied += pattern.active_leaf_states_copied;
  total.outside_boundary_charts_built += pattern.outside_boundary_charts_built;
  total.outside_recurrence_work += pattern.outside_recurrence_work;
}

template <class Structural, class InsideProvider>
inline multisite_exact_setup build_multisite_exact_setup_from_inside_scheduled(
    Structural const& structural, site_pattern_set const& patterns,
    chart_options const& options, chart_scheduler& scheduler,
    InsideProvider&& inside_provider, bool resident_inside,
    std::vector<chart_scheduler_run_summary>* run_summaries = nullptr) {
  validate_multisite_inputs(structural, patterns, options);
  require_exact_setup_binary(structural);
  if (run_summaries != nullptr) {
    if (run_summaries->size() > run_summaries->max_size() - 2) {
      throw std::length_error("exact setup scheduler summary overflow");
    }
    // Reserve before either scheduler operation. Once an operation completes,
    // publishing its summary cannot then fail and lose axis accounting.
    run_summaries->reserve(run_summaries->size() + 2);
  }

  multisite_exact_setup setup;
  setup.clade_count = exact_setup_clade_count(structural);
  setup.production_count = exact_setup_production_count(structural);
  setup.taxon_count = patterns.taxon_count;
  setup.structural_generation = exact_setup_structural_generation(structural);
  setup.structural_fingerprint = exact_setup_structural_fingerprint(structural);
  setup.score_ua_edge = options.score_ua_edge;
  setup.invariant_constant_offset =
      exact_setup_invariant_constant_offset(structural, patterns, options);
  setup.composite_lower_bound = setup.invariant_constant_offset;
  setup.work.setup_builds = 1;
  auto const unique_reachable_topology =
      exact_setup_has_unique_reachable_topology(structural);

  // The coordinator fixes the active-pattern order before launching work.
  // Workers write only their pre-sized slot; publication and all reductions
  // happen below in this stable order.
  std::vector<std::size_t> active_pattern_indices;
  active_pattern_indices.reserve(patterns.patterns.size());
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    if (is_active_pattern(patterns.patterns[pattern_index])) {
      active_pattern_indices.push_back(pattern_index);
    }
  }
  std::vector<scheduled_multisite_exact_pattern_slot> pattern_slots(
      active_pattern_indices.size());
  std::vector<std::exception_ptr> pattern_errors(active_pattern_indices.size());

  chart_scheduler_run_summary failed_pattern_run;
  try {
    auto pattern_run = scheduler.for_each_indexed_range(
        active_pattern_indices.size(), multisite_exact_setup_range_options(),
        [&](chart_indexed_range const& range, std::size_t stable_slot_id,
            chart_scheduler_cancellation_token const&) {
          for (std::size_t active_index = range.begin; active_index < range.end;
               ++active_index) {
            auto const pattern_index = active_pattern_indices[active_index];
            try {
              build_scheduled_multisite_exact_pattern_slot(
                  structural, patterns, pattern_index, options, inside_provider,
                  resident_inside, !unique_reachable_topology, stable_slot_id,
                  pattern_slots[active_index]);
            } catch (...) {
              pattern_errors[active_index] = std::current_exception();
              break;
            }
          }
        },
        &failed_pattern_run);
    if (run_summaries != nullptr) run_summaries->push_back(pattern_run);
  } catch (...) {
    if (run_summaries != nullptr && failed_pattern_run.failed) {
      run_summaries->push_back(failed_pattern_run);
    }
    throw;
  }
  for (auto const& error : pattern_errors) {
    if (error) std::rethrow_exception(error);
  }

  setup.active_patterns.reserve(pattern_slots.size());
  std::vector<selected_topology> upper_bound_topologies;
  if (!unique_reachable_topology) {
    upper_bound_topologies.reserve(pattern_slots.size() + 1);
    upper_bound_topologies.push_back(first_topology(structural));
  }
  for (auto& slot : pattern_slots) {
    setup.composite_lower_bound =
        checked_add_u64(setup.composite_lower_bound, slot.weighted_root_score,
                        "exact setup composite lower bound");
    add_scheduled_multisite_exact_pattern_work(setup.work, slot.work);
    setup.active_patterns.push_back(std::move(slot.info));
    if (slot.traceback_topology.has_value()) {
      upper_bound_topologies.push_back(std::move(*slot.traceback_topology));
    }
  }

  if (unique_reachable_topology) {
    // The sole feasible topology is established structurally rather than
    // materialized. See the serial builder's counter contract above.
    setup.work.upper_bound_topologies_generated = 1;
    setup.work.upper_bound_topologies_unique = 1;
    setup.initial_upper_bound = setup.composite_lower_bound;
    return setup;
  }

  // Topology identity and deduplication remain coordinator-only and therefore
  // byte-for-byte independent of completion order.
  setup.work.upper_bound_topologies_generated = upper_bound_topologies.size();
  sort_and_unique_topologies(upper_bound_topologies);
  setup.work.upper_bound_topologies_unique = upper_bound_topologies.size();

  // Prefer one topology per dynamic range when that axis is wide. If it is
  // narrower than the worker set, flatten topology x pattern instead; this
  // keeps a one-topology exact setup parallel without nesting scheduler work.
  // Both variants publish pattern/topology-indexed slots and fold in the same
  // topology-major, increasing-pattern order.
  std::vector<std::uint64_t> topology_scores(upper_bound_topologies.size(),
                                             multisite_score_inf);
  auto const topology_count = upper_bound_topologies.size();
  auto const pattern_count = patterns.patterns.size();
  auto const workers = scheduler.worker_resolution().resolved_workers;
  bool const flatten_topology_patterns =
      topology_count < workers && pattern_count > 1;
  chart_scheduler_run_summary topology_run;
  if (flatten_topology_patterns) {
    if (topology_count >
        (std::numeric_limits<std::size_t>::max)() / pattern_count) {
      throw std::overflow_error("exact setup topology-pattern size overflow");
    }
    auto const item_count = topology_count * pattern_count;
    std::vector<std::uint64_t> contributions(item_count, 0);
    std::vector<std::exception_ptr> errors(item_count);
    chart_scheduler_run_summary failed_topology_run;
    try {
      topology_run = scheduler.for_each_indexed_range(
          item_count, multisite_exact_setup_range_options(),
          [&](chart_indexed_range const& range, std::size_t,
              chart_scheduler_cancellation_token const&) {
            for (std::size_t flat = range.begin; flat < range.end; ++flat) {
              auto const topology_index = flat / pattern_count;
              auto const pattern_index = flat % pattern_count;
              try {
                auto const& pattern = patterns.patterns[pattern_index];
                if (!is_active_pattern(pattern)) continue;
                auto row = restricted_topology_row(
                    structural, pattern,
                    upper_bound_topologies[topology_index]);
                contributions[flat] = exact_setup_weighted_topology_root_score(
                    structural, row, pattern, options);
              } catch (...) {
                errors[flat] = std::current_exception();
                break;
              }
            }
          },
          &failed_topology_run);
      if (run_summaries != nullptr) run_summaries->push_back(topology_run);
    } catch (...) {
      if (run_summaries != nullptr && failed_topology_run.failed) {
        run_summaries->push_back(failed_topology_run);
      }
      throw;
    }
    for (auto const& error : errors) {
      if (error) std::rethrow_exception(error);
    }
    for (std::size_t topology_index = 0; topology_index < topology_count;
         ++topology_index) {
      auto total =
          exact_setup_invariant_constant_offset(structural, patterns, options);
      for (std::size_t pattern_index = 0; pattern_index < pattern_count;
           ++pattern_index) {
        total = checked_add_u64(
            total,
            contributions[topology_index * pattern_count + pattern_index],
            "selected topology score");
      }
      topology_scores[topology_index] = total;
    }
  } else {
    std::vector<std::exception_ptr> errors(topology_count);
    chart_scheduler_run_summary failed_topology_run;
    try {
      topology_run = scheduler.for_each_indexed_range(
          topology_count, multisite_exact_setup_range_options(),
          [&](chart_indexed_range const& range, std::size_t,
              chart_scheduler_cancellation_token const&) {
            for (std::size_t topology_index = range.begin;
                 topology_index < range.end; ++topology_index) {
              try {
                topology_scores[topology_index] =
                    ::larch::chart_multisite_detail::score_selected_topology(
                        structural, patterns,
                        upper_bound_topologies[topology_index], options);
              } catch (...) {
                errors[topology_index] = std::current_exception();
                break;
              }
            }
          },
          &failed_topology_run);
      if (run_summaries != nullptr) run_summaries->push_back(topology_run);
    } catch (...) {
      if (run_summaries != nullptr && failed_topology_run.failed) {
        run_summaries->push_back(failed_topology_run);
      }
      throw;
    }
    for (auto const& error : errors) {
      if (error) std::rethrow_exception(error);
    }
  }
  for (auto score : topology_scores) {
    setup.initial_upper_bound = std::min(setup.initial_upper_bound, score);
  }
  return setup;
}

template <class Structural>
inline std::uint64_t score_selected_topology_scheduled(
    Structural const& structural, site_pattern_set const& patterns,
    selected_topology const& topology, chart_options const& options,
    chart_scheduler& scheduler,
    chart_scheduler_run_summary* run_summary = nullptr) {
  std::vector<std::uint64_t> pattern_scores(patterns.patterns.size(), 0);
  std::vector<std::exception_ptr> errors(patterns.patterns.size());
  auto run = scheduler.for_each_indexed_range(
      patterns.patterns.size(), multisite_exact_setup_range_options(),
      [&](chart_indexed_range const& range, std::size_t,
          chart_scheduler_cancellation_token const&) {
        for (std::size_t pattern_index = range.begin; pattern_index < range.end;
             ++pattern_index) {
          try {
            auto const& pattern = patterns.patterns[pattern_index];
            if (!is_active_pattern(pattern)) continue;
            auto row = restricted_topology_row(structural, pattern, topology);
            pattern_scores[pattern_index] =
                exact_setup_weighted_topology_root_score(structural, row,
                                                         pattern, options);
          } catch (...) {
            errors[pattern_index] = std::current_exception();
            break;
          }
        }
      });
  if (run_summary != nullptr) *run_summary = run;
  for (auto const& error : errors) {
    if (error) std::rethrow_exception(error);
  }

  auto total =
      exact_setup_invariant_constant_offset(structural, patterns, options);
  for (auto score : pattern_scores) {
    total = checked_add_u64(total, score, "selected topology score");
  }
  return total;
}

inline multisite_exact_setup build_multisite_exact_setup_cold(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options) {
  chart_options build_options = options;
  build_options.keep_trace = false;
  build_options.max_trace_choices = 0;
  return build_multisite_exact_setup_from_inside(
      grammar, patterns, options,
      [&](std::size_t, site_pattern const& pattern) {
        return build_single_site_chart(
            grammar, view_leaf_site_states(pattern.state_by_taxon),
            build_options);
      },
      false);
}

inline multisite_exact_setup build_multisite_exact_setup_cold(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_scheduler& scheduler, chart_options const& options,
    std::vector<chart_scheduler_run_summary>* run_summaries = nullptr) {
  chart_options build_options = options;
  build_options.keep_trace = false;
  build_options.max_trace_choices = 0;
  return build_multisite_exact_setup_from_inside_scheduled(
      grammar, patterns, options, scheduler,
      [&](std::size_t, site_pattern const& pattern, std::size_t) {
        return build_single_site_chart(
            grammar, view_leaf_site_states(pattern.state_by_taxon),
            build_options);
      },
      false, run_summaries);
}

inline multisite_exact_setup build_multisite_exact_setup_cold(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options) {
  chart_options build_options = options;
  build_options.keep_trace = false;
  build_options.max_trace_choices = 0;
  return build_multisite_exact_setup_from_inside(
      plan, patterns, options,
      [&](std::size_t, site_pattern const& pattern) {
        return build_single_site_chart(
            plan, view_leaf_site_states(pattern.state_by_taxon), build_options);
      },
      false);
}

inline multisite_exact_setup build_multisite_exact_setup_cold(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_scheduler& scheduler, chart_options const& options,
    std::vector<chart_scheduler_run_summary>* run_summaries = nullptr) {
  chart_options build_options = options;
  build_options.keep_trace = false;
  build_options.max_trace_choices = 0;
  return build_multisite_exact_setup_from_inside_scheduled(
      plan, patterns, options, scheduler,
      [&](std::size_t, site_pattern const& pattern, std::size_t) {
        return build_single_site_chart(
            plan, view_leaf_site_states(pattern.state_by_taxon), build_options);
      },
      false, run_summaries);
}

}  // namespace chart_multisite_detail

using multisite_exact_setup = chart_multisite_detail::multisite_exact_setup;

inline multisite_exact_setup build_multisite_exact_setup(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {}) {
  return chart_multisite_detail::build_multisite_exact_setup_cold(
      grammar, patterns, options);
}

inline multisite_exact_setup build_multisite_exact_setup(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options = {}) {
  return chart_multisite_detail::build_multisite_exact_setup_cold(
      plan, patterns, options);
}

// Scheduler-taking exact-setup boundaries use the scheduler's stable slot ID
// throughout active-pattern work and retain the caller's one pool lifetime.
inline multisite_exact_setup build_multisite_exact_setup(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_scheduler& scheduler, chart_options const& options = {},
    std::vector<chart_scheduler_run_summary>* run_summaries = nullptr) {
  return chart_multisite_detail::build_multisite_exact_setup_cold(
      grammar, patterns, scheduler, options, run_summaries);
}

inline multisite_exact_setup build_multisite_exact_setup(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_scheduler& scheduler, chart_options const& options = {},
    std::vector<chart_scheduler_run_summary>* run_summaries = nullptr) {
  return chart_multisite_detail::build_multisite_exact_setup_cold(
      plan, patterns, scheduler, options, run_summaries);
}

// A resident provider is a trusted recurrence source: for each requested
// index it must return the inside chart computed from that exact pattern and
// structural object. Shape validation catches stale structure, but deliberately
// does not recompute every inside row merely to verify provider contents.
template <class InsideProvider>
inline multisite_exact_setup
build_multisite_exact_setup_from_resident_inside(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    InsideProvider&& inside_provider, chart_options const& options = {}) {
  return chart_multisite_detail::build_multisite_exact_setup_from_inside(
      grammar, patterns, options,
      std::forward<InsideProvider>(inside_provider), true);
}

template <class InsideProvider>
inline multisite_exact_setup
build_multisite_exact_setup_from_resident_inside(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    InsideProvider&& inside_provider, chart_options const& options = {}) {
  return chart_multisite_detail::build_multisite_exact_setup_from_inside(
      plan, patterns, options, std::forward<InsideProvider>(inside_provider),
      true);
}

// Scheduled resident providers must be safe for concurrent invocation and have
// signature (pattern_index, pattern, stable_slot_id).  A provider that needs a
// mutable materialization buffer owns one buffer per resolved scheduler slot.
// The legacy two-argument provider overloads above remain strictly serial.
template <class InsideProvider>
inline multisite_exact_setup build_multisite_exact_setup_from_resident_inside(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_scheduler& scheduler, InsideProvider&& inside_provider,
    chart_options const& options = {},
    std::vector<chart_scheduler_run_summary>* run_summaries = nullptr) {
  return chart_multisite_detail::
      build_multisite_exact_setup_from_inside_scheduled(
          grammar, patterns, options, scheduler,
          std::forward<InsideProvider>(inside_provider), true, run_summaries);
}

template <class InsideProvider>
inline multisite_exact_setup build_multisite_exact_setup_from_resident_inside(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_scheduler& scheduler, InsideProvider&& inside_provider,
    chart_options const& options = {},
    std::vector<chart_scheduler_run_summary>* run_summaries = nullptr) {
  return chart_multisite_detail::
      build_multisite_exact_setup_from_inside_scheduled(
          plan, patterns, options, scheduler,
          std::forward<InsideProvider>(inside_provider), true, run_summaries);
}

inline composite_chart_score build_composite_chart_score(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {}) {
  chart_multisite_detail::validate_multisite_inputs(grammar, patterns, options);

  composite_chart_score result;
  result.per_pattern_root_min.reserve(patterns.patterns.size());
  result.per_pattern_root_min_by_reference_state.reserve(
      patterns.patterns.size());
  chart_options chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  std::uint64_t total = 0;
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    leaf_site_states states;
    states.state_by_taxon = pattern.state_by_taxon;
    auto chart = build_single_site_chart(grammar, states, chart_build_options);
    result.multifurcation_productions_scored +=
        chart.multifurcation_productions_scored;

    std::array<chart_cost, nuc_state_count> by_reference{};
    by_reference.fill(chart_inf);
    chart_cost diagnostic_min = chart_inf;
    if (!options.score_ua_edge) {
      diagnostic_min = chart.root_min_excluding_ua(grammar.root_clade);
      by_reference.fill(diagnostic_min);
      total = chart_multisite_detail::checked_add_u64(
          total,
          chart_multisite_detail::checked_mul_cost(
              pattern.weight, diagnostic_min, "composite weighted root cost"),
          "composite lower bound");
    } else {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        auto count = pattern.reference_state_counts[reference_state];
        if (count == 0) continue;
        auto cost = chart.root_min_with_reference_edge(grammar.root_clade,
                                                       reference_state);
        by_reference[reference_state] = cost;
        diagnostic_min = std::min(diagnostic_min, cost);
        total = chart_multisite_detail::checked_add_u64(
            total,
            chart_multisite_detail::checked_mul_cost(
                count, cost, "composite weighted root-edge cost"),
            "composite root-edge lower bound");
      }
    }
    result.per_pattern_root_min.push_back(diagnostic_min);
    result.per_pattern_root_min_by_reference_state.push_back(by_reference);
  }

  if (options.score_ua_edge) {
    total = chart_multisite_detail::checked_add_u64(
        total, patterns.skipped_invariant_constant_score_with_reference_edge,
        "composite skipped invariant UA-edge offset");
  }
  result.weighted_lower_bound = total;
  return result;
}

inline composite_chart_score build_composite_chart_score(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options = {}) {
  chart_multisite_detail::validate_multisite_inputs(plan, patterns, options);

  composite_chart_score result;
  result.per_pattern_root_min.reserve(patterns.patterns.size());
  result.per_pattern_root_min_by_reference_state.reserve(
      patterns.patterns.size());
  chart_options chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  std::uint64_t total = 0;
  for (auto const& pattern : patterns.patterns) {
    leaf_site_states states;
    states.state_by_taxon = pattern.state_by_taxon;
    auto chart = build_single_site_chart(plan, states, chart_build_options);
    result.multifurcation_productions_scored +=
        chart.multifurcation_productions_scored;

    std::array<chart_cost, nuc_state_count> by_reference{};
    by_reference.fill(chart_inf);
    chart_cost diagnostic_min = chart_inf;
    auto const& root_row = chart.inside[plan.root_clade()];
    if (!options.score_ua_edge) {
      diagnostic_min = chart_multisite_detail::row_min(root_row);
      by_reference.fill(diagnostic_min);
      total = chart_multisite_detail::checked_add_u64(
          total,
          chart_multisite_detail::checked_mul_cost(
              pattern.weight, diagnostic_min, "composite weighted root cost"),
          "composite lower bound");
    } else {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        auto count = pattern.reference_state_counts[reference_state];
        if (count == 0) continue;
        chart_cost cost = chart_inf;
        for (std::uint8_t root_state = 0; root_state < nuc_state_count;
             ++root_state) {
          cost = std::min(
              cost, parsimony_chart_detail::saturated_add(
                        root_row[root_state],
                        static_cast<chart_cost>(plan.transition_cost(
                            reference_state, root_state))));
        }
        by_reference[reference_state] = cost;
        diagnostic_min = std::min(diagnostic_min, cost);
        total = chart_multisite_detail::checked_add_u64(
            total,
            chart_multisite_detail::checked_mul_cost(
                count, cost, "composite weighted root-edge cost"),
            "composite root-edge lower bound");
      }
    }
    result.per_pattern_root_min.push_back(diagnostic_min);
    result.per_pattern_root_min_by_reference_state.push_back(by_reference);
  }

  if (options.score_ua_edge) {
    total = chart_multisite_detail::checked_add_u64(
        total, patterns.skipped_invariant_constant_score_with_reference_edge,
        "composite skipped invariant UA-edge offset");
  }
  result.weighted_lower_bound = total;
  return result;
}

namespace chart_multisite_detail {

struct multisite_frontier_build_options {
  bool keep_provenance = false;
  bool keep_used_production = true;
  bool use_bound_pruning = true;
  multisite_dominance_mode dominance_mode = multisite_dominance_mode::off;
  std::optional<std::uint64_t> upper_bound_override;
  std::size_t max_frontier_entries_per_clade = 0;
  std::size_t max_provenance_choices_per_entry = 0;
};

struct multisite_frontier_build_result {
  std::vector<std::vector<frontier_entry>> frontiers;
  std::vector<active_pattern_info> active_patterns;
  // Exact setup builds borrow their active metadata synchronously from the
  // caller-owned setup; legacy/lazy builds leave this null and own the vector
  // above.  A frontier build result never escapes the trim operation that owns
  // the setup.
  std::vector<active_pattern_info> const* setup_active_patterns = nullptr;
  std::uint64_t composite_lower_bound = 0;
  std::uint64_t initial_upper_bound = 0;
  std::uint64_t invariant_constant_offset = 0;
  std::vector<std::size_t> frontier_sizes_by_clade;
  std::size_t equality_deduplicated = 0;
  std::size_t bound_pruned = 0;
  std::size_t dominance_candidates_considered = 0;
  std::size_t dominance_pruned = 0;
  std::size_t active_pattern_count = 0;
  std::vector<multisite_frontier_level_diagnostic> level_diagnostics;

  [[nodiscard]] std::vector<active_pattern_info> const&
  active_pattern_view() const noexcept {
    return setup_active_patterns == nullptr ? active_patterns
                                            : *setup_active_patterns;
  }
};

// Scheduler operations used by one exact trim remain separated by semantic
// stage, so callers never infer a semantic axis from completion order.
struct multisite_trim_scheduler_run_summaries {
  std::vector<chart_scheduler_run_summary> exact_setup;
  std::vector<chart_scheduler_run_summary> frontier_clades;
};

struct multisite_frontier_clade_work_stats {
  std::size_t product_combinations = 0;
  std::size_t equality_deduplicated = 0;
  std::size_t bound_pruned = 0;
  std::size_t dominance_candidates_considered = 0;
  std::size_t dominance_pruned = 0;

  bool operator==(multisite_frontier_clade_work_stats const&) const = default;
};

// Narrow test seam for proving dependency publication and real overlap.  The
// hooks are invoked by scheduler workers immediately before and after one
// clade kernel.  Production callers use the null default and pay only the two
// predictable null checks.  The object is borrowed synchronously for the
// scheduler operation and therefore needs no shared ownership.
struct multisite_frontier_scheduler_test_hooks {
  std::function<void(clade_id, std::size_t, std::size_t)> before_clade;
  std::function<void(clade_id, std::size_t, std::size_t)> after_clade;
};

inline void validate_multisite_frontier_build_options(
    multisite_frontier_build_options const& build_options,
    std::string const& context) {
  if (build_options.keep_provenance &&
      build_options.dominance_mode == multisite_dominance_mode::score_only) {
    throw std::runtime_error(
        context +
        ": score-only dominance cannot build exact topology provenance");
  }
  if (build_options.dominance_mode != multisite_dominance_mode::off &&
      build_options.dominance_mode != multisite_dominance_mode::score_only &&
      build_options.dominance_mode !=
          multisite_dominance_mode::strict_mask_safe) {
    throw std::runtime_error(
        context + ": dominance mode '" +
        multisite_dominance_mode_name(build_options.dominance_mode) +
        "' is not implemented in frontier construction");
  }
}

inline multisite_frontier_build_result
build_multisite_frontiers_from_prepared_active(
    clade_grammar const& grammar, chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context,
    std::vector<active_pattern_info> const* setup_active_patterns,
    std::vector<active_pattern_info> owned_active_patterns,
    std::uint64_t composite_lower_bound,
    std::uint64_t invariant_constant_offset_value,
    std::uint64_t initial_upper_bound_value) {
  validate_multisite_frontier_build_options(build_options, context);
  multisite_frontier_build_result result;
  result.composite_lower_bound = composite_lower_bound;
  result.frontier_sizes_by_clade.assign(grammar.clades.size(), 0);
  result.invariant_constant_offset = invariant_constant_offset_value;

  result.active_patterns = std::move(owned_active_patterns);
  result.setup_active_patterns = setup_active_patterns;
  auto const& active_patterns = result.active_pattern_view();
  result.active_pattern_count = active_patterns.size();
  result.initial_upper_bound = initial_upper_bound_value;
  auto pruning_upper_bound = build_options.upper_bound_override
                                 ? *build_options.upper_bound_override
                                 : result.initial_upper_bound;

  std::vector<clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize < rsize;
    return lhs < rhs;
  });

  result.frontiers.assign(grammar.clades.size(), {});
  for (auto clade : order) {
    auto const& key = grammar.clades[clade];
    if (key.taxa.size() == 1) {
      result.frontiers[clade].push_back(
          make_leaf_frontier_entry(grammar, clade, active_patterns,
                                   build_options.keep_used_production));
      result.frontier_sizes_by_clade[clade] = result.frontiers[clade].size();
      continue;
    }

    std::unordered_map<std::vector<chart_cost>, std::size_t,
                       chart_cost_vector_hash>
        index_by_cost;
    auto& entries = result.frontiers[clade];
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      if (prod.parent != clade) {
        throw std::runtime_error(
            context +
            ": production parent mismatch during frontier construction");
      }
      chart_trim_detail::validate_binary_production_for_trim(grammar, prod,
                                                             pid);
      auto left_child = prod.children[0];
      auto right_child = prod.children[1];
      if (left_child >= result.frontiers.size() ||
          right_child >= result.frontiers.size()) {
        throw std::runtime_error(context +
                                 ": production child out of frontier range");
      }
      for (std::size_t left_index = 0;
           left_index < result.frontiers[left_child].size(); ++left_index) {
        auto const& left = result.frontiers[left_child][left_index];
        for (std::size_t right_index = 0;
             right_index < result.frontiers[right_child].size();
             ++right_index) {
          auto const& right = result.frontiers[right_child][right_index];
          auto candidate = combine_frontier_entries(
              grammar, prod, pid, left, right, active_patterns.size(),
              build_options.keep_used_production);
          if (build_options.keep_provenance) {
            candidate.provenance.push_back(
                frontier_provenance_choice{pid, left_index, right_index});
          }
          if (build_options.use_bound_pruning &&
              pruning_upper_bound < multisite_score_inf) {
            auto lb = lower_bound_for_entry(
                candidate, clade, active_patterns,
                result.invariant_constant_offset, options);
            if (lb > pruning_upper_bound) {
              ++result.bound_pruned;
              continue;
            }
          }
          insert_or_merge_frontier_entry(
              entries, index_by_cost, std::move(candidate),
              result.equality_deduplicated,
              build_options.max_provenance_choices_per_entry);
        }
      }
    }

    if (build_options.dominance_mode == multisite_dominance_mode::score_only) {
      apply_score_only_dominance_pruning(entries,
                                         result.dominance_candidates_considered,
                                         result.dominance_pruned);
    } else if (build_options.dominance_mode ==
               multisite_dominance_mode::strict_mask_safe) {
      apply_strict_mask_safe_dominance_pruning(
          entries, clade, active_patterns, options,
          result.dominance_candidates_considered, result.dominance_pruned);
    }

    if (build_options.max_frontier_entries_per_clade != 0 &&
        entries.size() > build_options.max_frontier_entries_per_clade) {
      std::string message = context +
                            ": frontier entry cap exceeded for clade " +
                            std::to_string(clade);
      if (context.find("exact mask recovery pass") != std::string::npos) {
        message +=
            "; exact mask recovery pass exceeded the frontier cap; rerun "
            "with a larger cap or use score-only mode if an exact mask is "
            "not required";
      }
      throw std::runtime_error(message);
    }
    // Bound pruning can empty a non-root clade frontier when every topology
    // using that clade is already provably worse than the current feasible
    // upper bound.  Parent combinations that depend on it simply generate no
    // candidates.  The root frontier is checked by the caller after the pass.
    result.frontier_sizes_by_clade[clade] = entries.size();
  }

  return result;
}

inline void validate_multisite_exact_setup(
    clade_grammar const& grammar, multisite_exact_setup const& setup,
    chart_options const& options, std::string const& context) {
  if (setup.clade_count != grammar.clades.size() ||
      setup.production_count != grammar.productions.size() ||
      setup.taxon_count != grammar.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(context + ": exact setup shape mismatch");
  }
  if (setup.score_ua_edge != options.score_ua_edge) {
    throw std::runtime_error(context + ": exact setup scoring-mode mismatch");
  }
  if (setup.structural_generation != grammar.execution_generation) {
    throw std::runtime_error(context +
                             ": exact setup structural-generation mismatch");
  }
  if (setup.structural_fingerprint !=
      chart_execution_plan_detail::fingerprint_chart_grammar(grammar)) {
    throw std::runtime_error(context +
                             ": exact setup structural-fingerprint mismatch");
  }
  for (auto const& info : setup.active_patterns) {
    if (!info.chart.inside.empty() || !info.chart.optimal_choices.empty()) {
      throw std::runtime_error(
          context + ": finalized exact setup retained an inside chart");
    }
    if (info.state_by_taxon.size() != setup.taxon_count) {
      throw std::runtime_error(context +
                               ": exact setup leaf-state shape mismatch");
    }
  }
}

inline multisite_frontier_build_result build_multisite_frontiers_from_active(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context,
    std::vector<active_pattern_info> active_patterns,
    std::uint64_t composite_lower_bound) {
  validate_multisite_inputs(grammar, patterns, options);
  auto const invariant_offset = invariant_constant_offset(patterns, options);
  auto const upper_bound =
      initial_upper_bound(grammar, patterns, active_patterns, options);
  return build_multisite_frontiers_from_prepared_active(
      grammar, options, build_options, context, nullptr,
      std::move(active_patterns), composite_lower_bound, invariant_offset,
      upper_bound);
}

inline multisite_frontier_build_result build_multisite_frontiers_from_setup(
    clade_grammar const& grammar, multisite_exact_setup const& setup,
    chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context) {
  validate_multisite_exact_setup(grammar, setup, options, context);
  return build_multisite_frontiers_from_prepared_active(
      grammar, options, build_options, context, &setup.active_patterns, {},
      setup.composite_lower_bound, setup.invariant_constant_offset,
      setup.initial_upper_bound);
}

inline multisite_frontier_build_result initialize_multisite_frontier_build(
    chart_execution_plan const& plan,
    multisite_frontier_build_options const& build_options,
    std::string const& context,
    std::vector<active_pattern_info> const* setup_active_patterns,
    std::vector<active_pattern_info> owned_active_patterns,
    std::uint64_t composite_lower_bound,
    std::uint64_t invariant_constant_offset_value,
    std::uint64_t initial_upper_bound_value) {
  validate_multisite_frontier_build_options(build_options, context);
  multisite_frontier_build_result result;
  result.composite_lower_bound = composite_lower_bound;
  result.frontier_sizes_by_clade.assign(plan.clades().size(), 0);
  result.invariant_constant_offset = invariant_constant_offset_value;

  result.active_patterns = std::move(owned_active_patterns);
  result.setup_active_patterns = setup_active_patterns;
  auto const& active_patterns = result.active_pattern_view();
  result.active_pattern_count = active_patterns.size();
  result.initial_upper_bound = initial_upper_bound_value;
  result.frontiers.assign(plan.clades().size(), {});
  return result;
}

inline multisite_frontier_clade_work_stats build_multisite_frontier_clade(
    chart_execution_plan const& plan, chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context,
    std::vector<active_pattern_info> const& active_patterns,
    std::uint64_t invariant_constant_offset_value,
    std::uint64_t pruning_upper_bound, clade_id clade,
    std::vector<std::vector<frontier_entry>>& frontiers) {
  multisite_frontier_clade_work_stats work;
  auto const& key = plan.clade(clade);
  auto& entries = frontiers[clade];
  if (key.is_leaf()) {
    entries.push_back(make_leaf_frontier_entry(
        plan, clade, active_patterns, build_options.keep_used_production));
    return work;
  }

  std::unordered_map<std::vector<chart_cost>, std::size_t,
                     chart_cost_vector_hash>
      index_by_cost;
  frontier_entry combined_scratch;
  for (auto pid : plan.productions_for_parent(clade)) {
    auto const& prod = plan.production(pid);
    if (prod.parent != clade) {
      throw std::runtime_error(
          context +
          ": production parent mismatch during frontier construction");
    }
    chart_trim_detail::validate_binary_production_for_trim(plan, prod, pid);
    auto left_child = prod.binary_children[0];
    auto right_child = prod.binary_children[1];
    if (left_child >= frontiers.size() || right_child >= frontiers.size()) {
      throw std::runtime_error(context +
                               ": production child out of frontier range");
    }
    for (std::size_t left_index = 0; left_index < frontiers[left_child].size();
         ++left_index) {
      auto const& left = frontiers[left_child][left_index];
      for (std::size_t right_index = 0;
           right_index < frontiers[right_child].size(); ++right_index) {
        auto const& right = frontiers[right_child][right_index];
        ++work.product_combinations;
        combine_frontier_costs_into(plan, prod, pid, left, right,
                                    active_patterns.size(), combined_scratch.f);
        if (build_options.use_bound_pruning &&
            pruning_upper_bound < multisite_score_inf) {
          auto lb =
              lower_bound_for_entry(combined_scratch, clade, active_patterns,
                                    invariant_constant_offset_value, options);
          if (lb > pruning_upper_bound) {
            ++work.bound_pruned;
            continue;
          }
        }
        insert_or_merge_combined_frontier_entry(
            plan, pid, left, right, left_index, right_index, combined_scratch.f,
            build_options.keep_used_production, build_options.keep_provenance,
            entries, index_by_cost, work.equality_deduplicated,
            build_options.max_provenance_choices_per_entry);
      }
    }
  }

  if (build_options.dominance_mode == multisite_dominance_mode::score_only) {
    apply_score_only_dominance_pruning(
        entries, work.dominance_candidates_considered, work.dominance_pruned);
  } else if (build_options.dominance_mode ==
             multisite_dominance_mode::strict_mask_safe) {
    apply_strict_mask_safe_dominance_pruning(
        entries, clade, active_patterns, options,
        work.dominance_candidates_considered, work.dominance_pruned);
  }

  if (build_options.max_frontier_entries_per_clade != 0 &&
      entries.size() > build_options.max_frontier_entries_per_clade) {
    std::string message = context + ": frontier entry cap exceeded for clade " +
                          std::to_string(clade);
    if (context.find("exact mask recovery pass") != std::string::npos) {
      message +=
          "; exact mask recovery pass exceeded the frontier cap; rerun "
          "with a larger cap or use score-only mode if an exact mask is "
          "not required";
    }
    throw std::runtime_error(message);
  }
  return work;
}

inline void add_multisite_frontier_clade_work(
    multisite_frontier_build_result& result,
    multisite_frontier_clade_work_stats const& work) {
  result.equality_deduplicated += work.equality_deduplicated;
  result.bound_pruned += work.bound_pruned;
  result.dominance_candidates_considered +=
      work.dominance_candidates_considered;
  result.dominance_pruned += work.dominance_pruned;
}

inline multisite_frontier_build_result
build_multisite_frontiers_from_prepared_active(
    chart_execution_plan const& plan, chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context,
    std::vector<active_pattern_info> const* setup_active_patterns,
    std::vector<active_pattern_info> owned_active_patterns,
    std::uint64_t composite_lower_bound,
    std::uint64_t invariant_constant_offset_value,
    std::uint64_t initial_upper_bound_value) {
  auto result = initialize_multisite_frontier_build(
      plan, build_options, context, setup_active_patterns,
      std::move(owned_active_patterns), composite_lower_bound,
      invariant_constant_offset_value, initial_upper_bound_value);
  auto const& active_patterns = result.active_pattern_view();
  auto const pruning_upper_bound = build_options.upper_bound_override
                                       ? *build_options.upper_bound_override
                                       : result.initial_upper_bound;
  auto const level_order = plan.bottom_up_level_order();
  auto const level_offsets = plan.bottom_up_level_offsets();
  for (std::size_t level = 0; level + 1 < level_offsets.size(); ++level) {
    auto const started = std::chrono::steady_clock::now();
    multisite_frontier_level_diagnostic diagnostic;
    diagnostic.dependency_level = level;
    for (std::size_t item = level_offsets[level];
         item < level_offsets[level + 1]; ++item) {
      auto const clade = level_order[item];
      auto work = build_multisite_frontier_clade(
          plan, options, build_options, context, active_patterns,
          result.invariant_constant_offset, pruning_upper_bound, clade,
          result.frontiers);
      add_multisite_frontier_clade_work(result, work);
      auto const frontier_size = result.frontiers[clade].size();
      result.frontier_sizes_by_clade[clade] = frontier_size;
      ++diagnostic.clades_processed;
      if (!plan.clade(clade).is_leaf()) {
        ++diagnostic.internal_clades_processed;
      }
      diagnostic.product_combinations += work.product_combinations;
      diagnostic.output_frontier_entries += frontier_size;
      diagnostic.maximum_clade_frontier_entries =
          std::max(diagnostic.maximum_clade_frontier_entries, frontier_size);
      diagnostic.equality_deduplicated += work.equality_deduplicated;
      diagnostic.bound_pruned += work.bound_pruned;
      diagnostic.dominance_candidates_considered +=
          work.dominance_candidates_considered;
      diagnostic.dominance_pruned += work.dominance_pruned;
    }
    diagnostic.wave_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    result.level_diagnostics.push_back(std::move(diagnostic));
  }
  return result;
}

inline chart_indexed_range_options multisite_frontier_clade_range_options() {
  return chart_indexed_range_options{
      .minimum_grain = 1,
      .target_ranges_per_worker = 4,
  };
}

inline multisite_frontier_build_result
build_multisite_frontiers_from_prepared_active_scheduled(
    chart_execution_plan const& plan, chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context,
    std::vector<active_pattern_info> const* setup_active_patterns,
    std::vector<active_pattern_info> owned_active_patterns,
    std::uint64_t composite_lower_bound,
    std::uint64_t invariant_constant_offset_value,
    std::uint64_t initial_upper_bound_value, chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary>* clade_run_summaries = nullptr,
    multisite_frontier_scheduler_test_hooks const* test_hooks = nullptr) {
  auto result = initialize_multisite_frontier_build(
      plan, build_options, context, setup_active_patterns,
      std::move(owned_active_patterns), composite_lower_bound,
      invariant_constant_offset_value, initial_upper_bound_value);
  auto const& active_patterns = result.active_pattern_view();
  auto const pruning_upper_bound = build_options.upper_bound_override
                                       ? *build_options.upper_bound_override
                                       : result.initial_upper_bound;
  auto const level_order = plan.bottom_up_level_order();
  auto const level_offsets = plan.bottom_up_level_offsets();
  if (level_offsets.empty() || level_offsets.front() != 0 ||
      level_offsets.back() != level_order.size()) {
    throw std::runtime_error(context +
                             ": invalid dependency-level execution plan");
  }
  auto const level_count = level_offsets.size() - 1;
  if (clade_run_summaries != nullptr) {
    if (level_count >
        clade_run_summaries->max_size() - clade_run_summaries->size()) {
      throw std::length_error("exact frontier scheduler summary overflow");
    }
    // Reserve before the first level.  Once a scheduler operation completes,
    // publishing its summary cannot fail before a stable semantic exception is
    // rethrown to the caller.
    clade_run_summaries->reserve(clade_run_summaries->size() + level_count);
  }
  result.level_diagnostics.reserve(level_count);

  std::vector<multisite_frontier_clade_work_stats> work_by_clade(
      plan.clades().size());
  std::vector<std::exception_ptr> errors_by_clade(plan.clades().size());
  for (std::size_t level = 0; level < level_count; ++level) {
    auto const wave_started = std::chrono::steady_clock::now();
    auto const begin = level_offsets[level];
    auto const end = level_offsets[level + 1];
    auto const item_count = end - begin;
    multisite_frontier_level_diagnostic diagnostic;
    diagnostic.dependency_level = level;
    chart_scheduler_run_summary failed_run;
    chart_scheduler_run_summary run;
    try {
      run = scheduler.for_each_indexed_range(
          item_count, multisite_frontier_clade_range_options(),
          [&](chart_indexed_range const& range, std::size_t stable_slot,
              chart_scheduler_cancellation_token const&) {
            for (std::size_t item = range.begin; item < range.end; ++item) {
              auto const clade = level_order[begin + item];
              try {
                if (test_hooks != nullptr && test_hooks->before_clade) {
                  test_hooks->before_clade(clade, level, stable_slot);
                }
                work_by_clade[clade] = build_multisite_frontier_clade(
                    plan, options, build_options, context, active_patterns,
                    result.invariant_constant_offset, pruning_upper_bound,
                    clade, result.frontiers);
                if (test_hooks != nullptr && test_hooks->after_clade) {
                  test_hooks->after_clade(clade, level, stable_slot);
                }
              } catch (...) {
                errors_by_clade[clade] = std::current_exception();
                break;
              }
            }
          },
          &failed_run);
      if (clade_run_summaries != nullptr) {
        clade_run_summaries->push_back(run);
      }
    } catch (...) {
      if (clade_run_summaries != nullptr && failed_run.failed) {
        clade_run_summaries->push_back(failed_run);
      }
      throw;
    }

    // The join above forms the complete dependency-level publication barrier.
    // Select semantic failures exactly as W1 does: original bottom-up level
    // order. Completion order can never select the reported exception.
    for (std::size_t item = 0; item < item_count; ++item) {
      auto const clade = level_order[begin + item];
      if (errors_by_clade[clade]) {
        std::rethrow_exception(errors_by_clade[clade]);
      }
    }

    // Publish/fold the complete level only after the clade wave has joined.
    for (std::size_t item = 0; item < item_count; ++item) {
      auto const clade = level_order[begin + item];
      auto const& work = work_by_clade[clade];
      add_multisite_frontier_clade_work(result, work);
      auto const frontier_size = result.frontiers[clade].size();
      result.frontier_sizes_by_clade[clade] = frontier_size;
      ++diagnostic.clades_processed;
      if (!plan.clade(clade).is_leaf()) {
        ++diagnostic.internal_clades_processed;
      }
      diagnostic.product_combinations += work.product_combinations;
      diagnostic.output_frontier_entries += frontier_size;
      diagnostic.maximum_clade_frontier_entries =
          std::max(diagnostic.maximum_clade_frontier_entries, frontier_size);
      diagnostic.equality_deduplicated += work.equality_deduplicated;
      diagnostic.bound_pruned += work.bound_pruned;
      diagnostic.dominance_candidates_considered +=
          work.dominance_candidates_considered;
      diagnostic.dominance_pruned += work.dominance_pruned;
    }
    diagnostic.wave_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - wave_started)
                             .count();
    result.level_diagnostics.push_back(std::move(diagnostic));
  }
  return result;
}

inline void validate_multisite_exact_setup(
    chart_execution_plan const& plan, multisite_exact_setup const& setup,
    chart_options const& options, std::string const& context) {
  plan.assert_valid();
  if (setup.clade_count != plan.clades().size() ||
      setup.production_count != plan.productions().size() ||
      setup.taxon_count != plan.taxon_count()) {
    throw std::runtime_error(context + ": exact setup shape mismatch");
  }
  if (setup.score_ua_edge != options.score_ua_edge) {
    throw std::runtime_error(context + ": exact setup scoring-mode mismatch");
  }
  if (setup.structural_generation != plan.grammar_generation()) {
    throw std::runtime_error(context +
                             ": exact setup structural-generation mismatch");
  }
  if (setup.structural_fingerprint != plan.fingerprint()) {
    throw std::runtime_error(context +
                             ": exact setup structural-fingerprint mismatch");
  }
  for (auto const& info : setup.active_patterns) {
    if (!info.chart.inside.empty() || !info.chart.optimal_choices.empty()) {
      throw std::runtime_error(
          context + ": finalized exact setup retained an inside chart");
    }
    if (info.state_by_taxon.size() != setup.taxon_count) {
      throw std::runtime_error(context +
                               ": exact setup leaf-state shape mismatch");
    }
  }
}

inline multisite_frontier_build_result build_multisite_frontiers_from_active(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context,
    std::vector<active_pattern_info> active_patterns,
    std::uint64_t composite_lower_bound) {
  validate_multisite_inputs(plan, patterns, options);
  auto const invariant_offset =
      invariant_constant_offset(plan, patterns, options);
  auto const upper_bound =
      initial_upper_bound(plan, patterns, active_patterns, options);
  return build_multisite_frontiers_from_prepared_active(
      plan, options, build_options, context, nullptr,
      std::move(active_patterns), composite_lower_bound, invariant_offset,
      upper_bound);
}

inline multisite_frontier_build_result build_multisite_frontiers_from_setup(
    chart_execution_plan const& plan, multisite_exact_setup const& setup,
    chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context) {
  validate_multisite_exact_setup(plan, setup, options, context);
  return build_multisite_frontiers_from_prepared_active(
      plan, options, build_options, context, &setup.active_patterns, {},
      setup.composite_lower_bound, setup.invariant_constant_offset,
      setup.initial_upper_bound);
}

inline multisite_frontier_build_result build_multisite_frontiers_from_setup(
    chart_execution_plan const& plan, multisite_exact_setup const& setup,
    chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context, chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary>* clade_run_summaries = nullptr,
    multisite_frontier_scheduler_test_hooks const* test_hooks = nullptr) {
  validate_multisite_exact_setup(plan, setup, options, context);
  return build_multisite_frontiers_from_prepared_active_scheduled(
      plan, options, build_options, context, &setup.active_patterns, {},
      setup.composite_lower_bound, setup.invariant_constant_offset,
      setup.initial_upper_bound, scheduler, clade_run_summaries, test_hooks);
}

inline multisite_frontier_build_result build_multisite_frontiers(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context) {
  auto composite = build_composite_chart_score(grammar, patterns, options);
  auto active = build_active_pattern_info(grammar, patterns, options);
  return build_multisite_frontiers_from_active(
      grammar, patterns, options, build_options, context, std::move(active),
      composite.weighted_lower_bound);
}

inline multisite_frontier_build_result build_multisite_frontiers(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options,
    multisite_frontier_build_options const& build_options,
    std::string const& context) {
  auto composite = build_composite_chart_score(plan, patterns, options);
  auto active = build_active_pattern_info(plan, patterns, options);
  return build_multisite_frontiers_from_active(
      plan, patterns, options, build_options, context, std::move(active),
      composite.weighted_lower_bound);
}

inline std::uint64_t compute_root_frontier_optimum_and_update_mask(
    clade_grammar const& grammar, chart_options const& options,
    multisite_frontier_build_result const& build, bool merge_exact_keep_mask,
    std::vector<bool>& keep_production, std::string const& context) {
  auto const& root_frontier = build.frontiers[grammar.root_clade];
  if (root_frontier.empty()) {
    throw std::runtime_error(context + ": empty root frontier");
  }
  if (merge_exact_keep_mask &&
      keep_production.size() != grammar.productions.size()) {
    throw std::runtime_error(context + ": keep-production mask size mismatch");
  }

  std::uint64_t optimum = multisite_score_inf;
  for (auto const& entry : root_frontier) {
    auto score =
        lower_bound_for_entry(entry, grammar.root_clade,
                              build.active_pattern_view(),
                              build.invariant_constant_offset, options);
    if (score < optimum) {
      optimum = score;
      if (merge_exact_keep_mask) {
        std::fill(keep_production.begin(), keep_production.end(), false);
      }
    }
    if (score == optimum && merge_exact_keep_mask) {
      merge_used_productions(keep_production, entry.used_production);
    }
  }
  return optimum;
}

inline std::uint64_t compute_root_frontier_optimum_and_update_mask(
    chart_execution_plan const& plan, chart_options const& options,
    multisite_frontier_build_result const& build, bool merge_exact_keep_mask,
    std::vector<bool>& keep_production, std::string const& context) {
  auto const& root_frontier = build.frontiers[plan.root_clade()];
  if (root_frontier.empty()) {
    throw std::runtime_error(context + ": empty root frontier");
  }
  if (merge_exact_keep_mask &&
      keep_production.size() != plan.productions().size()) {
    throw std::runtime_error(context + ": keep-production mask size mismatch");
  }

  std::uint64_t optimum = multisite_score_inf;
  for (auto const& entry : root_frontier) {
    auto score = lower_bound_for_entry(entry, plan.root_clade(),
                                       build.active_pattern_view(),
                                       build.invariant_constant_offset, options);
    if (score < optimum) {
      optimum = score;
      if (merge_exact_keep_mask) {
        std::fill(keep_production.begin(), keep_production.end(), false);
      }
    }
    if (score == optimum && merge_exact_keep_mask) {
      merge_used_productions(keep_production, entry.used_production);
    }
  }
  return optimum;
}

inline void capture_optimal_root_provenance_classes(
    clade_grammar const& grammar, chart_options const& options,
    multisite_frontier_build_result const& build, std::uint64_t optimum,
    std::vector<multisite_optimal_root_provenance_class>& out,
    std::string const& context) {
  auto const& root_frontier = build.frontiers[grammar.root_clade];
  out.clear();
  for (auto const& entry : root_frontier) {
    auto score =
        lower_bound_for_entry(entry, grammar.root_clade,
                              build.active_pattern_view(),
                              build.invariant_constant_offset, options);
    if (score != optimum) continue;
    if (entry.used_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          context +
          ": optimal-root provenance requested without an exact production "
          "mask");
    }
    out.push_back({entry.f.cost, entry.used_production});
  }
  std::sort(out.begin(), out.end(), [](auto const& lhs, auto const& rhs) {
    if (lhs.cost != rhs.cost) return lhs.cost < rhs.cost;
    return lhs.used_production < rhs.used_production;
  });
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

inline void capture_optimal_root_provenance_classes(
    chart_execution_plan const& plan, chart_options const& options,
    multisite_frontier_build_result const& build, std::uint64_t optimum,
    std::vector<multisite_optimal_root_provenance_class>& out,
    std::string const& context) {
  auto const& root_frontier = build.frontiers[plan.root_clade()];
  out.clear();
  for (auto const& entry : root_frontier) {
    auto score = lower_bound_for_entry(entry, plan.root_clade(),
                                       build.active_pattern_view(),
                                       build.invariant_constant_offset, options);
    if (score != optimum) continue;
    if (entry.used_production.size() != plan.productions().size()) {
      throw std::runtime_error(
          context +
          ": optimal-root provenance requested without an exact production "
          "mask");
    }
    out.push_back({entry.f.cost, entry.used_production});
  }
  std::sort(out.begin(), out.end(), [](auto const& lhs, auto const& rhs) {
    if (lhs.cost != rhs.cost) return lhs.cost < rhs.cost;
    return lhs.used_production < rhs.used_production;
  });
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

template <class Capture>
inline void capture_optimal_root_provenance_for_trim(
    multisite_trim_options const& trim_options, std::string const& context,
    Capture&& capture) {
  if (trim_options.force_optimal_root_provenance_capture_failure_for_tests) {
    throw multisite_optimal_root_provenance_capture_error(
        context + ": forced optimal-root provenance capture failure for test");
  }
  try {
    std::invoke(std::forward<Capture>(capture));
  } catch (std::bad_alloc const&) {
    // Allocation failure is process/infrastructure failure, not a biological
    // property of one candidate.  Preserve its standard type for the search's
    // hard-error boundary.
    throw;
  } catch (multisite_optimal_root_provenance_capture_error const&) {
    throw;
  } catch (std::exception const& error) {
    throw multisite_optimal_root_provenance_capture_error(
        context + ": optimal-root provenance capture failed: " + error.what());
  }
}

}  // namespace chart_multisite_detail

inline multisite_bruteforce_result brute_force_multisite_topologies(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {}, std::size_t max_topologies = 100000) {
  using namespace chart_multisite_detail;
  validate_multisite_inputs(grammar, patterns, options);

  multisite_bruteforce_result result;
  result.keep_production.assign(grammar.productions.size(), false);

  auto topologies =
      enumerate_topologies(grammar, grammar.root_clade, max_topologies);
  result.topology_count = topologies.size();
  for (auto const& topo : topologies) {
    auto score = score_selected_topology(grammar, patterns, topo, options);
    if (score < result.optimum) {
      result.optimum = score;
      result.optimal_topology_count = 0;
      std::fill(result.keep_production.begin(), result.keep_production.end(),
                false);
    }
    if (score == result.optimum) {
      ++result.optimal_topology_count;
      merge_used_productions(result.keep_production, topo.used_production);
    }
  }
  return result;
}

namespace chart_multisite_detail {

inline void append_multisite_frontier_diagnostics(
    multisite_trim_result& result, multisite_frontier_build_result const& build,
    multisite_frontier_pass_kind pass_kind, std::size_t pass_index) {
  if (build.level_diagnostics.size() >
      result.frontier_level_diagnostics.max_size() -
          result.frontier_level_diagnostics.size()) {
    throw std::length_error("multi-site frontier diagnostic overflow");
  }
  result.frontier_level_diagnostics.reserve(
      result.frontier_level_diagnostics.size() +
      build.level_diagnostics.size());
  for (auto diagnostic : build.level_diagnostics) {
    diagnostic.pass_kind = pass_kind;
    diagnostic.pass_index = pass_index;
    ++result.exact_bnb_levels;
    result.exact_bnb_clades += diagnostic.clades_processed;
    result.exact_bnb_product_combinations += diagnostic.product_combinations;
    result.exact_bnb_frontier_entries += diagnostic.output_frontier_entries;
    result.exact_bnb_ms += diagnostic.wave_ms;
    result.frontier_level_diagnostics.push_back(std::move(diagnostic));
  }
}

template <class BuildFrontiers>
inline multisite_trim_result build_multisite_trim_impl(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options,
    multisite_trim_options const& trim_options,
    BuildFrontiers&& build_frontiers) {
  validate_multisite_inputs(grammar, patterns, options);
  validate_multisite_trim_options_supported(trim_options, "multi-site trim",
                                            true);
  parsimony_chart_detail::require_no_multifurcating_productions_for_consumer(
      grammar, arity_gate_consumer::multisite_trim, "multi-site trim",
      "B&B frontier",
      "use --wric-polytomy-mode expand-exact or expand-bounded before B&B "
      "trim, or use an arity-agnostic SPR/fixed-topology path");

  auto keep_mask_kind = keep_mask_kind_for_options(trim_options);
  auto keep_production_exact =
      keep_mask_kind ==
      multisite_keep_mask_kind::exact_optimal_production_union;
  if (trim_options.capture_optimal_root_provenance &&
      !keep_production_exact) {
    throw std::runtime_error(
        "multi-site trim: optimal-root provenance requires an exact "
        "keep-production mask");
  }

  multisite_trim_result result;
  result.dominance_mode = trim_options.dominance_mode;
  result.keep_mask_kind = keep_mask_kind;
  result.keep_production_exact = keep_production_exact;
  result.keep_production.assign(grammar.productions.size(), false);

  if (trim_options.dominance_mode ==
      multisite_dominance_mode::two_pass_exact_mask) {
    // Phase 3 two-pass exact-mask recovery: use score-only dominance only for
    // the scalar objective pass, then rebuild without dominance under the
    // validated optimum to recover the exact optimal-production union mask.
    multisite_frontier_build_options score_build_options;
    score_build_options.keep_provenance = false;
    score_build_options.keep_used_production = false;
    score_build_options.use_bound_pruning = trim_options.use_bound_pruning;
    score_build_options.dominance_mode = multisite_dominance_mode::score_only;
    score_build_options.upper_bound_override =
        trim_options.upper_bound_override;
    score_build_options.max_frontier_entries_per_clade =
        trim_options.max_frontier_entries_per_clade;
    {
      auto score_build =
          build_frontiers(score_build_options, "multi-site trim score pass");

      result.optimum = compute_root_frontier_optimum_and_update_mask(
          grammar, options, score_build, false, result.keep_production,
          "multi-site trim score pass");
      validate_known_exact_optimum(result.optimum, trim_options,
                                   "multi-site trim score pass");
      append_multisite_frontier_diagnostics(
          result, score_build, multisite_frontier_pass_kind::score_only, 0);

      // Retain every score-pass scalar and diagnostic needed by the result,
      // then release its full frontier vectors before allocating the exact
      // mask-recovery pass.
      result.composite_lower_bound = score_build.composite_lower_bound;
      result.initial_upper_bound = score_build.initial_upper_bound;
      result.dominance_candidates_considered =
          score_build.dominance_candidates_considered;
      result.dominance_pruned_score_pass = score_build.dominance_pruned;
      result.bound_pruned = score_build.bound_pruned;
      result.equality_deduplicated = score_build.equality_deduplicated;
      result.active_pattern_count = score_build.active_pattern_count;
      result.invariant_constant_offset = score_build.invariant_constant_offset;
    }

    multisite_frontier_build_options mask_build_options;
    mask_build_options.keep_provenance = false;
    mask_build_options.keep_used_production = true;
    mask_build_options.use_bound_pruning = trim_options.use_bound_pruning;
    mask_build_options.dominance_mode = multisite_dominance_mode::off;
    // The score-pass optimum is a pruning threshold only until the recovered
    // root frontier independently validates it below.
    mask_build_options.upper_bound_override = result.optimum;
    mask_build_options.max_frontier_entries_per_clade =
        trim_options.max_frontier_entries_per_clade;
    auto mask_build = build_frontiers(
        mask_build_options, "multi-site trim exact mask recovery pass");
    append_multisite_frontier_diagnostics(
        result, mask_build, multisite_frontier_pass_kind::exact_mask_recovery,
        1);

    auto recovered_optimum = compute_root_frontier_optimum_and_update_mask(
        grammar, options, mask_build, true, result.keep_production,
        "multi-site trim exact mask recovery pass");
    multisite_trim_options recovery_validation_options;
    recovery_validation_options.known_exact_optimum = result.optimum;
    validate_known_exact_optimum(recovered_optimum, recovery_validation_options,
                                 "multi-site trim exact mask recovery pass");
    if (trim_options.capture_optimal_root_provenance) {
      capture_optimal_root_provenance_for_trim(
          trim_options, "multi-site trim exact mask recovery pass", [&] {
            capture_optimal_root_provenance_classes(
                grammar, options, mask_build, recovered_optimum,
                result.optimal_root_provenance_classes,
                "multi-site trim exact mask recovery pass");
          });
    }

    result.frontier_sizes_by_clade = mask_build.frontier_sizes_by_clade;
    result.dominance_pruned_mask_pass = mask_build.dominance_pruned;
    result.bound_pruned += mask_build.bound_pruned;
    result.equality_deduplicated += mask_build.equality_deduplicated;
    result.exact_mask_recovery_passes = 1;
    result.dominance_pruned =
        result.dominance_pruned_score_pass + result.dominance_pruned_mask_pass;
    return result;
  }

  multisite_frontier_build_options build_options;
  build_options.keep_provenance = false;
  build_options.keep_used_production = keep_production_exact;
  build_options.use_bound_pruning = trim_options.use_bound_pruning;
  build_options.dominance_mode = trim_options.dominance_mode;
  build_options.upper_bound_override = trim_options.upper_bound_override;
  build_options.max_frontier_entries_per_clade =
      trim_options.max_frontier_entries_per_clade;
  auto build = build_frontiers(build_options, "multi-site trim");
  append_multisite_frontier_diagnostics(result, build,
                                        multisite_frontier_pass_kind::exact, 0);

  result.composite_lower_bound = build.composite_lower_bound;
  result.initial_upper_bound = build.initial_upper_bound;
  result.frontier_sizes_by_clade = build.frontier_sizes_by_clade;
  result.dominance_candidates_considered =
      build.dominance_candidates_considered;
  if (trim_options.dominance_mode ==
          multisite_dominance_mode::strict_mask_safe &&
      result.keep_production_exact) {
    result.dominance_pruned_score_pass = 0;
    result.dominance_pruned_mask_pass = build.dominance_pruned;
  } else {
    result.dominance_pruned_score_pass = build.dominance_pruned;
    result.dominance_pruned_mask_pass = 0;
  }
  result.bound_pruned = build.bound_pruned;
  result.equality_deduplicated = build.equality_deduplicated;
  result.active_pattern_count = build.active_pattern_count;
  result.invariant_constant_offset = build.invariant_constant_offset;

  result.optimum = compute_root_frontier_optimum_and_update_mask(
      grammar, options, build, result.keep_production_exact,
      result.keep_production, "multi-site trim");
  validate_known_exact_optimum(result.optimum, trim_options, "multi-site trim");
  if (trim_options.capture_optimal_root_provenance) {
    capture_optimal_root_provenance_for_trim(
        trim_options, "multi-site trim", [&] {
          capture_optimal_root_provenance_classes(
              grammar, options, build, result.optimum,
              result.optimal_root_provenance_classes, "multi-site trim");
        });
  }
  result.dominance_pruned =
      result.dominance_pruned_score_pass + result.dominance_pruned_mask_pass;
  return result;
}

template <class BuildFrontiers>
inline multisite_trim_result build_multisite_trim_impl(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options,
    multisite_trim_options const& trim_options,
    BuildFrontiers&& build_frontiers) {
  validate_multisite_inputs(plan, patterns, options);
  validate_multisite_trim_options_supported(trim_options, "multi-site trim",
                                            true);
  require_no_multifurcating_productions_for_consumer(
      plan, arity_gate_consumer::multisite_trim, "multi-site trim",
      "B&B frontier",
      "use --wric-polytomy-mode expand-exact or expand-bounded before B&B "
      "trim, or use an arity-agnostic SPR/fixed-topology path");

  auto keep_mask_kind = keep_mask_kind_for_options(trim_options);
  auto keep_production_exact =
      keep_mask_kind ==
      multisite_keep_mask_kind::exact_optimal_production_union;
  if (trim_options.capture_optimal_root_provenance &&
      !keep_production_exact) {
    throw std::runtime_error(
        "multi-site trim: optimal-root provenance requires an exact "
        "keep-production mask");
  }

  multisite_trim_result result;
  result.dominance_mode = trim_options.dominance_mode;
  result.keep_mask_kind = keep_mask_kind;
  result.keep_production_exact = keep_production_exact;
  result.keep_production.assign(plan.productions().size(), false);

  // The result retains one diagnostic per dependency level/pass while each
  // frontier build owns its current pass diagnostics.  Reserve the complete
  // result surface before the first pass: in two-pass mode this avoids the
  // L -> 2L vector reallocation overlap during the second append and leaves a
  // stable, directly estimable peak of source L plus retained result 2L.
  auto const& level_offsets = plan.bottom_up_level_offsets();
  auto const level_count = level_offsets.empty() ? 0 : level_offsets.size() - 1;
  auto const frontier_pass_count =
      trim_options.dominance_mode ==
              multisite_dominance_mode::two_pass_exact_mask
          ? std::size_t{2}
          : std::size_t{1};
  if (level_count != 0) {
    if (frontier_pass_count >
        result.frontier_level_diagnostics.max_size() / level_count) {
      throw std::length_error("multi-site frontier diagnostic overflow");
    }
    result.frontier_level_diagnostics.reserve(frontier_pass_count *
                                              level_count);
  }

  if (trim_options.dominance_mode ==
      multisite_dominance_mode::two_pass_exact_mask) {
    multisite_frontier_build_options score_build_options;
    score_build_options.keep_provenance = false;
    score_build_options.keep_used_production = false;
    score_build_options.use_bound_pruning = trim_options.use_bound_pruning;
    score_build_options.dominance_mode = multisite_dominance_mode::score_only;
    score_build_options.upper_bound_override =
        trim_options.upper_bound_override;
    score_build_options.max_frontier_entries_per_clade =
        trim_options.max_frontier_entries_per_clade;
    {
      auto score_build =
          build_frontiers(score_build_options, "multi-site trim score pass");

      result.optimum = compute_root_frontier_optimum_and_update_mask(
          plan, options, score_build, false, result.keep_production,
          "multi-site trim score pass");
      validate_known_exact_optimum(result.optimum, trim_options,
                                   "multi-site trim score pass");
      append_multisite_frontier_diagnostics(
          result, score_build, multisite_frontier_pass_kind::score_only, 0);

      // Retain every score-pass scalar and diagnostic needed by the result,
      // then release its full frontier vectors before allocating the exact
      // mask-recovery pass.
      result.composite_lower_bound = score_build.composite_lower_bound;
      result.initial_upper_bound = score_build.initial_upper_bound;
      result.dominance_candidates_considered =
          score_build.dominance_candidates_considered;
      result.dominance_pruned_score_pass = score_build.dominance_pruned;
      result.bound_pruned = score_build.bound_pruned;
      result.equality_deduplicated = score_build.equality_deduplicated;
      result.active_pattern_count = score_build.active_pattern_count;
      result.invariant_constant_offset = score_build.invariant_constant_offset;
    }

    multisite_frontier_build_options mask_build_options;
    mask_build_options.keep_provenance = false;
    mask_build_options.keep_used_production = true;
    mask_build_options.use_bound_pruning = trim_options.use_bound_pruning;
    mask_build_options.dominance_mode = multisite_dominance_mode::off;
    mask_build_options.upper_bound_override = result.optimum;
    mask_build_options.max_frontier_entries_per_clade =
        trim_options.max_frontier_entries_per_clade;
    auto mask_build = build_frontiers(
        mask_build_options, "multi-site trim exact mask recovery pass");
    append_multisite_frontier_diagnostics(
        result, mask_build, multisite_frontier_pass_kind::exact_mask_recovery,
        1);

    auto recovered_optimum = compute_root_frontier_optimum_and_update_mask(
        plan, options, mask_build, true, result.keep_production,
        "multi-site trim exact mask recovery pass");
    multisite_trim_options recovery_validation_options;
    recovery_validation_options.known_exact_optimum = result.optimum;
    validate_known_exact_optimum(recovered_optimum, recovery_validation_options,
                                 "multi-site trim exact mask recovery pass");
    if (trim_options.capture_optimal_root_provenance) {
      capture_optimal_root_provenance_for_trim(
          trim_options, "multi-site trim exact mask recovery pass", [&] {
            capture_optimal_root_provenance_classes(
                plan, options, mask_build, recovered_optimum,
                result.optimal_root_provenance_classes,
                "multi-site trim exact mask recovery pass");
          });
    }

    result.frontier_sizes_by_clade = mask_build.frontier_sizes_by_clade;
    result.dominance_pruned_mask_pass = mask_build.dominance_pruned;
    result.bound_pruned += mask_build.bound_pruned;
    result.equality_deduplicated += mask_build.equality_deduplicated;
    result.exact_mask_recovery_passes = 1;
    result.dominance_pruned =
        result.dominance_pruned_score_pass + result.dominance_pruned_mask_pass;
    return result;
  }

  multisite_frontier_build_options build_options;
  build_options.keep_provenance = false;
  build_options.keep_used_production = keep_production_exact;
  build_options.use_bound_pruning = trim_options.use_bound_pruning;
  build_options.dominance_mode = trim_options.dominance_mode;
  build_options.upper_bound_override = trim_options.upper_bound_override;
  build_options.max_frontier_entries_per_clade =
      trim_options.max_frontier_entries_per_clade;
  auto build = build_frontiers(build_options, "multi-site trim");
  append_multisite_frontier_diagnostics(result, build,
                                        multisite_frontier_pass_kind::exact, 0);

  result.composite_lower_bound = build.composite_lower_bound;
  result.initial_upper_bound = build.initial_upper_bound;
  result.frontier_sizes_by_clade = build.frontier_sizes_by_clade;
  result.dominance_candidates_considered =
      build.dominance_candidates_considered;
  if (trim_options.dominance_mode ==
          multisite_dominance_mode::strict_mask_safe &&
      result.keep_production_exact) {
    result.dominance_pruned_score_pass = 0;
    result.dominance_pruned_mask_pass = build.dominance_pruned;
  } else {
    result.dominance_pruned_score_pass = build.dominance_pruned;
    result.dominance_pruned_mask_pass = 0;
  }
  result.bound_pruned = build.bound_pruned;
  result.equality_deduplicated = build.equality_deduplicated;
  result.active_pattern_count = build.active_pattern_count;
  result.invariant_constant_offset = build.invariant_constant_offset;

  result.optimum = compute_root_frontier_optimum_and_update_mask(
      plan, options, build, result.keep_production_exact,
      result.keep_production, "multi-site trim");
  validate_known_exact_optimum(result.optimum, trim_options, "multi-site trim");
  if (trim_options.capture_optimal_root_provenance) {
    capture_optimal_root_provenance_for_trim(
        trim_options, "multi-site trim", [&] {
          capture_optimal_root_provenance_classes(
              plan, options, build, result.optimum,
              result.optimal_root_provenance_classes, "multi-site trim");
        });
  }
  result.dominance_pruned =
      result.dominance_pruned_score_pass + result.dominance_pruned_mask_pass;
  return result;
}

}  // namespace chart_multisite_detail

using multisite_trim_scheduler_run_summaries =
    chart_multisite_detail::multisite_trim_scheduler_run_summaries;

inline multisite_trim_result build_multisite_trim_from_exact_setup(
    clade_grammar const& grammar, multisite_exact_setup const& setup,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  chart_multisite_detail::validate_multisite_exact_setup(
      grammar, setup, options, "multi-site trim");
  site_pattern_set validation_shell;
  validation_shell.taxon_count = setup.taxon_count;
  std::size_t frontier_passes = 0;
  auto result = chart_multisite_detail::build_multisite_trim_impl(
      grammar, validation_shell, options, trim_options,
      [&](chart_multisite_detail::multisite_frontier_build_options const&
              build_options,
          std::string const& context) {
        ++frontier_passes;
        return chart_multisite_detail::build_multisite_frontiers_from_setup(
            grammar, setup, options, build_options, context);
      });
  result.exact_setup_work = setup.work;
  result.exact_setup_work.frontier_passes = frontier_passes;
  return result;
}

inline multisite_trim_result build_multisite_trim_from_exact_setup(
    chart_execution_plan const& plan, multisite_exact_setup const& setup,
    chart_scheduler& scheduler, chart_options const& options = {},
    multisite_trim_options const& trim_options = {},
    multisite_trim_scheduler_run_summaries* run_summaries = nullptr) {
  chart_multisite_detail::validate_multisite_exact_setup(plan, setup, options,
                                                         "multi-site trim");
  site_pattern_set validation_shell;
  validation_shell.taxon_count = setup.taxon_count;
  if (run_summaries != nullptr) {
    auto const& level_offsets = plan.bottom_up_level_offsets();
    auto const level_count =
        level_offsets.empty() ? 0 : level_offsets.size() - 1;
    auto const frontier_pass_count =
        trim_options.dominance_mode ==
                multisite_dominance_mode::two_pass_exact_mask
            ? std::size_t{2}
            : std::size_t{1};
    if (level_count != 0) {
      if (frontier_pass_count > (run_summaries->frontier_clades.max_size() -
                                 run_summaries->frontier_clades.size()) /
                                    level_count) {
        throw std::length_error("exact frontier scheduler summary overflow");
      }
      // Reserve every pass before the first frontier operation.  The
      // per-pass builder can then publish summaries without an L -> 2L
      // reallocation whose old and new buffers would overlap.
      run_summaries->frontier_clades.reserve(
          run_summaries->frontier_clades.size() +
          frontier_pass_count * level_count);
    }
  }
  std::size_t frontier_passes = 0;
  auto result = chart_multisite_detail::build_multisite_trim_impl(
      plan, validation_shell, options, trim_options,
      [&](chart_multisite_detail::multisite_frontier_build_options const&
              build_options,
          std::string const& context) {
        ++frontier_passes;
        return chart_multisite_detail::build_multisite_frontiers_from_setup(
            plan, setup, options, build_options, context, scheduler,
            run_summaries == nullptr ? nullptr
                                     : &run_summaries->frontier_clades);
      });
  result.exact_setup_work = setup.work;
  result.exact_setup_work.frontier_passes = frontier_passes;
  return result;
}

inline multisite_trim_result build_multisite_trim_from_exact_setup(
    chart_execution_plan const& plan, multisite_exact_setup const& setup,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  chart_multisite_detail::validate_multisite_exact_setup(
      plan, setup, options, "multi-site trim");
  site_pattern_set validation_shell;
  validation_shell.taxon_count = setup.taxon_count;
  std::size_t frontier_passes = 0;
  auto result = chart_multisite_detail::build_multisite_trim_impl(
      plan, validation_shell, options, trim_options,
      [&](chart_multisite_detail::multisite_frontier_build_options const&
              build_options,
          std::string const& context) {
        ++frontier_passes;
        return chart_multisite_detail::build_multisite_frontiers_from_setup(
            plan, setup, options, build_options, context);
      });
  result.exact_setup_work = setup.work;
  result.exact_setup_work.frontier_passes = frontier_passes;
  return result;
}

inline multisite_trim_result build_multisite_trim(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  chart_multisite_detail::validate_multisite_inputs(grammar, patterns, options);
  chart_multisite_detail::validate_multisite_trim_options_supported(
      trim_options, "multi-site trim", true);
  auto setup = build_multisite_exact_setup(grammar, patterns, options);
  return build_multisite_trim_from_exact_setup(grammar, setup, options,
                                               trim_options);
}

inline multisite_trim_result build_multisite_trim(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  chart_multisite_detail::validate_multisite_inputs(plan, patterns, options);
  chart_multisite_detail::validate_multisite_trim_options_supported(
      trim_options, "multi-site trim", true);
  auto setup = build_multisite_exact_setup(plan, patterns, options);
  return build_multisite_trim_from_exact_setup(plan, setup, options,
                                               trim_options);
}

inline multisite_trim_result build_multisite_trim(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    chart_scheduler& scheduler, chart_options const& options = {},
    multisite_trim_options const& trim_options = {},
    multisite_trim_scheduler_run_summaries* run_summaries = nullptr) {
  chart_multisite_detail::validate_multisite_inputs(plan, patterns, options);
  chart_multisite_detail::validate_multisite_trim_options_supported(
      trim_options, "multi-site trim", true);
  auto setup = build_multisite_exact_setup(
      plan, patterns, scheduler, options,
      run_summaries == nullptr ? nullptr : &run_summaries->exact_setup);
  return build_multisite_trim_from_exact_setup(plan, setup, scheduler, options,
                                               trim_options, run_summaries);
}

inline std::uint64_t score_selected_topology(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    grammar_topology const& topology, chart_options const& options = {}) {
  chart_multisite_detail::validate_multisite_inputs(grammar, patterns, options);
  (void)validate_grammar_topology(grammar, topology);
  return chart_multisite_detail::score_selected_topology(grammar, patterns,
                                                         topology, options);
}

inline std::uint64_t score_selected_topology(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    grammar_topology const& topology, chart_scheduler& scheduler,
    chart_options const& options = {},
    chart_scheduler_run_summary* run_summary = nullptr) {
  chart_multisite_detail::validate_multisite_inputs(grammar, patterns, options);
  (void)validate_grammar_topology(grammar, topology);
  return chart_multisite_detail::score_selected_topology_scheduled(
      grammar, patterns, topology, options, scheduler, run_summary);
}

inline multisite_topology_trace_result build_multisite_optimal_topologies(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_topology_trace_options const& trace_opts = {}) {
  using namespace chart_multisite_detail;
  validate_multisite_inputs(grammar, patterns, options);
  validate_required_productions(grammar, trace_opts.required_productions);
  validate_multisite_trim_options_supported(trace_opts.trim_options,
                                            "multi-site topology trace", false);
  if (!trace_opts.keep_provenance) {
    throw std::runtime_error(
        "multi-site topology trace: keep_provenance=false cannot emit "
        "concrete topologies");
  }

  multisite_frontier_build_options build_options;
  build_options.keep_provenance = true;
  build_options.keep_used_production = false;
  build_options.use_bound_pruning = trace_opts.trim_options.use_bound_pruning;
  build_options.dominance_mode = trace_opts.trim_options.dominance_mode;
  build_options.upper_bound_override =
      trace_opts.trim_options.upper_bound_override;
  build_options.max_frontier_entries_per_clade =
      trace_opts.trim_options.max_frontier_entries_per_clade;
  build_options.max_provenance_choices_per_entry =
      trace_opts.max_provenance_choices_per_entry;
  auto build = build_multisite_frontiers(
      grammar, patterns, options, build_options, "multi-site topology trace");

  multisite_topology_trace_result result;
  result.composite_lower_bound = build.composite_lower_bound;
  result.initial_upper_bound = build.initial_upper_bound;
  result.keep_production.assign(grammar.productions.size(), false);
  result.frontier_sizes_by_clade = build.frontier_sizes_by_clade;
  result.equality_deduplicated = build.equality_deduplicated;
  result.dominance_pruned = build.dominance_pruned;
  result.bound_pruned = build.bound_pruned;
  result.active_pattern_count = build.active_pattern_count;
  result.invariant_constant_offset = build.invariant_constant_offset;

  auto const& frontiers = build.frontiers;
  auto const& root_frontier = frontiers[grammar.root_clade];
  if (root_frontier.empty()) {
    throw std::runtime_error("multi-site topology trace: empty root frontier");
  }

  std::vector<std::size_t> optimal_root_entries;
  for (std::size_t entry_index = 0; entry_index < root_frontier.size();
       ++entry_index) {
    auto score = lower_bound_for_entry(
        root_frontier[entry_index], grammar.root_clade,
        build.active_pattern_view(), result.invariant_constant_offset, options);
    if (score < result.optimum) {
      result.optimum = score;
      optimal_root_entries.clear();
    }
    if (score == result.optimum) optimal_root_entries.push_back(entry_index);
  }
  result.optimal_frontier_entry_count = optimal_root_entries.size();
  if (result.optimum >= multisite_score_inf || optimal_root_entries.empty()) {
    throw std::runtime_error(
        "multi-site topology trace: no finite optimal topology");
  }
  validate_known_exact_optimum(result.optimum, trace_opts.trim_options,
                               "multi-site topology trace");

  std::sort(optimal_root_entries.begin(), optimal_root_entries.end(),
            [&](std::size_t lhs, std::size_t rhs) {
              auto const& lcost = root_frontier[lhs].f.cost;
              auto const& rcost = root_frontier[rhs].f.cost;
              if (lcost != rcost) return lcost < rcost;
              return lhs < rhs;
            });

  auto enumeration_limit = trace_opts.max_optimal_topologies;
  if (enumeration_limit != 0 &&
      enumeration_limit < std::numeric_limits<std::size_t>::max()) {
    ++enumeration_limit;
  }

  bool provenance_truncated = false;
  std::vector<grammar_topology> all_emitted;
  for (auto entry_index : optimal_root_entries) {
    if (enumeration_limit != 0 && all_emitted.size() >= enumeration_limit) {
      provenance_truncated = true;
      break;
    }
    auto remaining = enumeration_limit == 0
                         ? std::size_t{0}
                         : enumeration_limit - all_emitted.size();
    auto topologies = enumerate_topologies_from_provenance(
        grammar, frontiers, grammar.root_clade, entry_index, remaining,
        provenance_truncated);
    all_emitted.insert(all_emitted.end(), topologies.begin(), topologies.end());
  }
  sort_and_unique_topologies(all_emitted);

  if (trace_opts.max_optimal_topologies != 0 &&
      all_emitted.size() > trace_opts.max_optimal_topologies) {
    result.topology_cap_truncated = true;
  }
  result.topology_cap_truncated =
      result.topology_cap_truncated || provenance_truncated;

  std::vector<production_id> uncovered;
  result.topologies = choose_topologies_for_required_coverage(
      grammar, all_emitted, trace_opts.required_productions,
      trace_opts.max_optimal_topologies, uncovered);
  result.uncovered_required_productions = uncovered;

  if (result.topologies.empty()) {
    throw std::runtime_error(
        "multi-site topology trace: no optimal topologies were emitted");
  }

  for (auto const& topology : result.topologies) {
    auto reachable = validate_grammar_topology(grammar, topology);
    auto score = chart_multisite_detail::score_selected_topology(
        grammar, patterns, topology, options);
    if (score != result.optimum) {
      throw std::runtime_error(
          "multi-site topology trace: emitted topology failed exact re-score");
    }
    merge_used_productions(result.keep_production, reachable);
  }

  if (!result.uncovered_required_productions.empty() &&
      trace_opts.require_required_production_coverage) {
    std::string message =
        "multi-site topology trace: required productions are not covered by "
        "emitted optimal topologies (optimum=" +
        std::to_string(result.optimum) +
        ", emitted=" + std::to_string(result.topologies.size()) +
        ", uncovered=[";
    for (std::size_t i = 0; i < result.uncovered_required_productions.size();
         ++i) {
      if (i != 0) message += ",";
      message += std::to_string(result.uncovered_required_productions[i]);
    }
    message += result.topology_cap_truncated ? "], cap_truncated=true)"
                                             : "], cap_truncated=false)";
    throw std::runtime_error(message);
  }

  return result;
}

inline multisite_coupled_frontier_trim_result
build_multisite_coupled_frontier_trim(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_coupled_frontier_trim_options const& coupled_opts = {}) {
  using namespace chart_multisite_detail;
  validate_multisite_inputs(grammar, patterns, options);
  if (!coupled_opts.trim_options.require_exact_keep_mask) {
    throw std::runtime_error(
        "multi-site coupled frontier trim: annotated optimal trim requires "
        "exact keep-mask/provenance semantics; score-only mode is not a "
        "valid coupled exact output");
  }
  validate_multisite_trim_options_supported(
      coupled_opts.trim_options, "multi-site coupled frontier trim", false);

  multisite_frontier_build_options build_options;
  build_options.keep_provenance = true;
  build_options.keep_used_production = false;
  build_options.use_bound_pruning = coupled_opts.trim_options.use_bound_pruning;
  build_options.dominance_mode = coupled_opts.trim_options.dominance_mode;
  build_options.upper_bound_override =
      coupled_opts.trim_options.upper_bound_override;
  build_options.max_frontier_entries_per_clade =
      coupled_opts.trim_options.max_frontier_entries_per_clade;
  build_options.max_provenance_choices_per_entry =
      coupled_opts.max_provenance_choices_per_entry;
  auto build =
      build_multisite_frontiers(grammar, patterns, options, build_options,
                                "multi-site coupled frontier trim");

  multisite_coupled_frontier_trim_result result;
  result.coupled_frontier_exact = true;
  result.annotated_optimal_trim = true;
  result.composite_lower_bound = build.composite_lower_bound;
  result.initial_upper_bound = build.initial_upper_bound;
  result.frontier_sizes_by_clade = build.frontier_sizes_by_clade;
  result.coupled_frontier_entries_by_clade.assign(grammar.clades.size(), 0);
  result.entries_by_clade.assign(grammar.clades.size(), {});
  result.keep_production.assign(grammar.productions.size(), false);
  result.dominance_mode = coupled_opts.trim_options.dominance_mode;
  result.equality_deduplicated = build.equality_deduplicated;
  result.dominance_candidates_considered =
      build.dominance_candidates_considered;
  result.dominance_pruned = build.dominance_pruned;
  result.bound_pruned = build.bound_pruned;
  result.active_pattern_count = build.active_pattern_count;
  result.invariant_constant_offset = build.invariant_constant_offset;

  auto const& frontiers = build.frontiers;
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= frontiers.size()) {
    throw std::runtime_error(
        "multi-site coupled frontier trim: root clade out of range");
  }
  auto const& root_frontier = frontiers[grammar.root_clade];
  if (root_frontier.empty()) {
    throw std::runtime_error(
        "multi-site coupled frontier trim: empty root frontier");
  }

  std::vector<std::size_t> optimal_root_entries;
  for (std::size_t entry_index = 0; entry_index < root_frontier.size();
       ++entry_index) {
    auto score = lower_bound_for_entry(
        root_frontier[entry_index], grammar.root_clade,
        build.active_pattern_view(), result.invariant_constant_offset, options);
    if (score < result.optimum) {
      result.optimum = score;
      optimal_root_entries.clear();
    }
    if (score == result.optimum) optimal_root_entries.push_back(entry_index);
  }
  result.optimal_root_frontier_entry_count = optimal_root_entries.size();
  if (result.optimum >= multisite_score_inf || optimal_root_entries.empty()) {
    throw std::runtime_error(
        "multi-site coupled frontier trim: no finite optimal topology");
  }
  validate_known_exact_optimum(result.optimum, coupled_opts.trim_options,
                               "multi-site coupled frontier trim");

  std::vector<std::vector<bool>> marked(grammar.clades.size());
  for (std::size_t clade = 0; clade < grammar.clades.size(); ++clade) {
    marked[clade].assign(frontiers[clade].size(), false);
  }

  std::function<void(clade_id, std::size_t)> mark_entry =
      [&](clade_id clade, std::size_t entry_index) {
        if (clade == no_clade || clade >= frontiers.size()) {
          throw std::runtime_error(
              "multi-site coupled frontier trim: clade out of frontier range");
        }
        if (entry_index >= frontiers[clade].size()) {
          throw std::runtime_error(
              "multi-site coupled frontier trim: entry out of frontier range");
        }
        if (marked[clade][entry_index]) return;
        marked[clade][entry_index] = true;

        if (grammar.clades[clade].taxa.size() == 1) return;
        auto const& entry = frontiers[clade][entry_index];
        if (entry.provenance.empty()) {
          throw std::runtime_error(
              "multi-site coupled frontier trim: marked internal entry has "
              "no packed provenance");
        }
        for (auto const& choice : entry.provenance) {
          if (choice.production == no_production ||
              choice.production >= grammar.productions.size()) {
            throw std::runtime_error(
                "multi-site coupled frontier trim: provenance production out "
                "of range");
          }
          auto const& prod = grammar.productions[choice.production];
          if (prod.parent != clade) {
            throw std::runtime_error(
                "multi-site coupled frontier trim: provenance parent "
                "mismatch");
          }
          chart_trim_detail::validate_binary_production_for_trim(
              grammar, prod, choice.production);
          mark_entry(prod.children[0], choice.left_entry);
          mark_entry(prod.children[1], choice.right_entry);
        }
      };

  for (auto entry_index : optimal_root_entries) {
    mark_entry(grammar.root_clade, entry_index);
  }

  std::vector<std::vector<std::size_t>> old_to_compact(grammar.clades.size());
  for (std::size_t clade = 0; clade < grammar.clades.size(); ++clade) {
    old_to_compact[clade].assign(frontiers[clade].size(), no_idx);
    for (std::size_t entry_index = 0; entry_index < frontiers[clade].size();
         ++entry_index) {
      if (!marked[clade][entry_index]) continue;
      old_to_compact[clade][entry_index] =
          result.entries_by_clade[clade].size();
      coupled_frontier_entry compact;
      compact.f = frontiers[clade][entry_index].f;
      result.entries_by_clade[clade].push_back(std::move(compact));
      ++result.coupled_frontier_entry_count;
    }
    result.coupled_frontier_entries_by_clade[clade] =
        result.entries_by_clade[clade].size();
  }

  for (std::size_t clade = 0; clade < grammar.clades.size(); ++clade) {
    for (std::size_t entry_index = 0; entry_index < frontiers[clade].size();
         ++entry_index) {
      if (!marked[clade][entry_index]) continue;
      auto compact_index = old_to_compact[clade][entry_index];
      if (compact_index == no_idx ||
          compact_index >= result.entries_by_clade[clade].size()) {
        throw std::runtime_error(
            "multi-site coupled frontier trim: compact entry remap failed");
      }
      if (grammar.clades[clade].taxa.size() == 1) continue;

      auto& compact_entry = result.entries_by_clade[clade][compact_index];
      for (auto const& choice : frontiers[clade][entry_index].provenance) {
        auto const& prod = grammar.productions[choice.production];
        auto left_child = prod.children[0];
        auto right_child = prod.children[1];
        auto left_compact = old_to_compact[left_child].at(choice.left_entry);
        auto right_compact = old_to_compact[right_child].at(choice.right_entry);
        if (left_compact == no_idx || right_compact == no_idx) {
          throw std::runtime_error(
              "multi-site coupled frontier trim: retained provenance points "
              "to an unmarked child entry");
        }
        compact_entry.choices.push_back(
            coupled_frontier_choice{choice.production, left_child, right_child,
                                    left_compact, right_compact});
        result.keep_production[choice.production] = true;
        ++result.coupled_provenance_choice_count;
      }
      if (compact_entry.choices.empty()) {
        throw std::runtime_error(
            "multi-site coupled frontier trim: compact internal entry has no "
            "choices");
      }
    }
  }

  return result;
}

namespace chart_multisite_detail {

inline std::vector<selected_topology> enumerate_coupled_frontier_entry(
    clade_grammar const& grammar,
    multisite_coupled_frontier_trim_result const& coupled, clade_id clade,
    std::size_t entry_index, std::size_t max_topologies, bool& truncated) {
  if (clade == no_clade || clade >= grammar.clades.size() ||
      clade >= coupled.entries_by_clade.size()) {
    throw std::runtime_error(
        "multi-site coupled frontier enumeration: clade out of range");
  }
  if (entry_index >= coupled.entries_by_clade[clade].size()) {
    throw std::runtime_error(
        "multi-site coupled frontier enumeration: entry out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1) {
    return {empty_selected_topology(grammar)};
  }

  auto const& entry = coupled.entries_by_clade[clade][entry_index];
  if (entry.choices.empty()) {
    throw std::runtime_error(
        "multi-site coupled frontier enumeration: internal entry has no "
        "choices");
  }

  std::vector<selected_topology> result;
  for (auto const& choice : entry.choices) {
    if (max_topologies != 0 && result.size() >= max_topologies) {
      truncated = true;
      break;
    }
    if (choice.production == no_production ||
        choice.production >= grammar.productions.size()) {
      throw std::runtime_error(
          "multi-site coupled frontier enumeration: production out of range");
    }
    auto const& prod = grammar.productions[choice.production];
    if (prod.parent != clade || prod.children.size() != 2 ||
        prod.children[0] != choice.left_child ||
        prod.children[1] != choice.right_child) {
      throw std::runtime_error(
          "multi-site coupled frontier enumeration: production/child "
          "annotation mismatch");
    }

    auto remaining = [&](std::size_t used) -> std::size_t {
      if (max_topologies == 0) return std::size_t{0};
      return used >= max_topologies ? std::size_t{1} : max_topologies - used;
    };
    auto left_topologies = enumerate_coupled_frontier_entry(
        grammar, coupled, choice.left_child, choice.left_entry,
        remaining(result.size()), truncated);
    auto right_topologies = enumerate_coupled_frontier_entry(
        grammar, coupled, choice.right_child, choice.right_entry,
        remaining(result.size()), truncated);

    for (auto const& left : left_topologies) {
      for (auto const& right : right_topologies) {
        if (max_topologies != 0 && result.size() >= max_topologies) {
          truncated = true;
          break;
        }
        result.push_back(combine_selected_child_topologies(
            grammar, clade, choice.production, left, right));
      }
      if (max_topologies != 0 && result.size() >= max_topologies) break;
    }
  }
  sort_and_unique_topologies(result);
  return result;
}

}  // namespace chart_multisite_detail

inline multisite_coupled_frontier_enumeration_result
enumerate_multisite_coupled_frontier_topologies(
    clade_grammar const& grammar,
    multisite_coupled_frontier_trim_result const& coupled,
    std::size_t max_topologies = 0) {
  using namespace chart_multisite_detail;
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= coupled.entries_by_clade.size()) {
    throw std::runtime_error(
        "multi-site coupled frontier enumeration: root clade out of range");
  }
  if (coupled.entries_by_clade[grammar.root_clade].empty()) {
    throw std::runtime_error(
        "multi-site coupled frontier enumeration: no optimal root entries");
  }

  auto enumeration_limit = max_topologies;
  if (enumeration_limit != 0 &&
      enumeration_limit < std::numeric_limits<std::size_t>::max()) {
    ++enumeration_limit;
  }

  multisite_coupled_frontier_enumeration_result result;
  for (std::size_t entry_index = 0;
       entry_index < coupled.entries_by_clade[grammar.root_clade].size();
       ++entry_index) {
    if (enumeration_limit != 0 &&
        result.topologies.size() >= enumeration_limit) {
      result.topology_cap_truncated = true;
      break;
    }
    bool truncated = false;
    auto remaining = enumeration_limit == 0
                         ? std::size_t{0}
                         : enumeration_limit - result.topologies.size();
    auto topologies =
        enumerate_coupled_frontier_entry(grammar, coupled, grammar.root_clade,
                                         entry_index, remaining, truncated);
    result.topologies.insert(result.topologies.end(), topologies.begin(),
                             topologies.end());
    result.topology_cap_truncated = result.topology_cap_truncated || truncated;
  }
  sort_and_unique_topologies(result.topologies);
  if (max_topologies != 0 && result.topologies.size() > max_topologies) {
    result.topology_cap_truncated = true;
    result.topologies.resize(max_topologies);
  }
  return result;
}

}  // namespace larch

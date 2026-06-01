#pragma once

#include <larch/grammar_topology.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/site_patterns.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
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
};

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

inline chart_cost add3(chart_cost a, chart_cost b, chart_cost c) {
  return parsimony_chart_detail::saturated_add(
      parsimony_chart_detail::saturated_add(a, b), c);
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
      chart_trim_detail::validate_binary_production_for_trim(grammar, prod,
                                                             pid);

      std::array<clade_id, 2> children{prod.children[0], prod.children[1]};
      for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
           ++parent_state) {
        auto base = result.outside[parent][parent_state];
        if (base >= chart_inf) continue;

        std::array<chart_cost, 2> sibling_best{chart_inf, chart_inf};
        for (std::size_t child_i = 0; child_i < 2; ++child_i) {
          auto sibling = children[1 - child_i];
          for (std::uint8_t sibling_state = 0; sibling_state < nuc_state_count;
               ++sibling_state) {
            auto candidate =
                saturated_add(chart.inside[sibling][sibling_state],
                              transition_cost(parent_state, sibling_state));
            sibling_best[child_i] = std::min(sibling_best[child_i], candidate);
          }
        }

        for (std::size_t child_i = 0; child_i < 2; ++child_i) {
          auto child = children[child_i];
          if (sibling_best[child_i] >= chart_inf) continue;
          for (std::uint8_t child_state = 0; child_state < nuc_state_count;
               ++child_state) {
            auto candidate = chart_trim_detail::add3(
                base, sibling_best[child_i],
                transition_cost(parent_state, child_state));
            auto& cell = result.outside[child][child_state];
            cell = std::min(cell, candidate);
          }
        }
      }
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

struct multisite_bruteforce_result {
  std::uint64_t optimum = multisite_score_inf;
  std::vector<bool> keep_production;
  std::size_t topology_count = 0;
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
                                     std::string const& label) {
  if (lhs >= multisite_score_inf || rhs >= multisite_score_inf)
    return multisite_score_inf;
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    throw std::runtime_error("multi-site trim: uint64 overflow while adding " +
                             label);
  }
  auto result = lhs + rhs;
  return result >= multisite_score_inf ? multisite_score_inf : result;
}

inline std::uint64_t checked_mul_cost(std::uint64_t weight, chart_cost cost,
                                      std::string const& label) {
  if (weight == 0) return 0;
  if (cost >= chart_inf) return multisite_score_inf;
  if (cost > std::numeric_limits<std::uint64_t>::max() / weight) {
    throw std::runtime_error(
        "multi-site trim: uint64 overflow while multiplying " + label);
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

inline chart_row combine_binary_rows(chart_row const& left,
                                     chart_row const& right) {
  chart_row row = make_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    chart_cost best_left = chart_inf;
    chart_cost best_right = chart_inf;
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      best_left = std::min(
          best_left,
          parsimony_chart_detail::saturated_add(
              left[child_state], parsimony_chart_detail::transition_cost(
                                     parent_state, child_state)));
      best_right = std::min(
          best_right,
          parsimony_chart_detail::saturated_add(
              right[child_state], parsimony_chart_detail::transition_cost(
                                      parent_state, child_state)));
    }
    row[parent_state] =
        parsimony_chart_detail::saturated_add(best_left, best_right);
  }
  return row;
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
    auto found = std::find_if(dst.begin(), dst.end(), [&](auto const& existing) {
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
    if (lhs.production != rhs.production) return lhs.production < rhs.production;
    if (lhs.left_entry != rhs.left_entry) return lhs.left_entry < rhs.left_entry;
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
    std::vector<active_pattern_info> const& active) {
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() != 1) {
    throw std::runtime_error(
        "multi-site trim: leaf frontier requested for non-singleton clade");
  }
  auto taxon = key.taxa.front();
  frontier_entry entry;
  entry.f.cost.assign(active.size() * nuc_state_count, chart_inf);
  entry.f.topology_hash = mix_hash(0x6c656166ULL, taxon);
  entry.used_production.assign(grammar.productions.size(), false);

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
    std::size_t active_pattern_count) {
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
  if (left.used_production.size() != grammar.productions.size() ||
      right.used_production.size() != grammar.productions.size()) {
    throw std::runtime_error(
        "multi-site trim: child provenance vector has wrong size");
  }

  frontier_entry candidate;
  candidate.f.cost.assign(expected_size, chart_inf);
  candidate.f.topology_hash =
      mix_hash(mix_hash(mix_hash(0x70726f64ULL, pid), left.f.topology_hash),
               right.f.topology_hash);
  candidate.used_production = left.used_production;
  merge_used_productions(candidate.used_production, right.used_production);
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error("multi-site trim: production id out of range");
  }
  candidate.used_production[pid] = true;

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
        merge_provenance_choices(entries[i].provenance,
                                 entries[j].provenance);
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
    merge_used_productions(entries[found->second].used_production,
                           candidate.used_production);
    merge_provenance_choices(entries[found->second].provenance,
                             candidate.provenance,
                             max_provenance_choices_per_entry);
    entries[found->second].f.topology_hash = mix_hash(
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
    chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
    auto left = restricted_topology_row_impl(grammar, pattern, topo,
                                             prod.children[0], memo);
    auto right = restricted_topology_row_impl(grammar, pattern, topo,
                                              prod.children[1], memo);
    row = combine_binary_rows(left, right);
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

inline void sort_and_unique_topologies(std::vector<selected_topology>& topologies) {
  std::sort(topologies.begin(), topologies.end(), topology_lexicographic_less);
  topologies.erase(std::unique(topologies.begin(), topologies.end(),
                               grammar_topology_equal),
                   topologies.end());
}

inline std::vector<selected_topology> enumerate_topologies_from_provenance(
    clade_grammar const& grammar,
    std::vector<std::vector<frontier_entry>> const& frontiers,
    clade_id clade, std::size_t entry_index, std::size_t max_topologies,
    bool& truncated) {
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
    chart_trim_detail::validate_binary_production_for_trim(
        grammar, prod, choice.production);

    auto remaining = [&](std::size_t used) -> std::size_t {
      if (max_topologies == 0) return std::size_t{0};
      return used >= max_topologies ? std::size_t{1}
                                    : max_topologies - used;
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
    selected_topology const& topology, std::vector<production_id> const& required,
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
        auto count = count_new_required_coverage(topologies[i], required,
                                                 covered);
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
    multisite_trim_options const& trim_options, std::string const& context) {
  if (trim_options.dominance_mode != multisite_dominance_mode::off) {
    throw std::runtime_error(
        context + ": dominance mode '" +
        multisite_dominance_mode_name(trim_options.dominance_mode) +
        "' is not implemented in this public trim path yet; dominance is "
        "currently disabled, use off");
  }
}

inline multisite_keep_mask_kind keep_mask_kind_for_options(
    multisite_trim_options const& trim_options) {
  if (!trim_options.require_exact_keep_mask) {
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
      context + ": known_exact_optimum validation failed: computed root "
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

}  // namespace chart_multisite_detail

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
      std::fill(result.keep_production.begin(), result.keep_production.end(),
                false);
    }
    if (score == result.optimum) {
      merge_used_productions(result.keep_production, topo.used_production);
    }
  }
  return result;
}

inline multisite_trim_result build_multisite_trim(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  using namespace chart_multisite_detail;
  validate_multisite_inputs(grammar, patterns, options);
  validate_multisite_trim_options_supported(trim_options, "multi-site trim");

  multisite_trim_result result;
  result.dominance_mode = trim_options.dominance_mode;
  result.keep_mask_kind = keep_mask_kind_for_options(trim_options);
  result.keep_production_exact =
      result.keep_mask_kind ==
      multisite_keep_mask_kind::exact_optimal_production_union;
  auto composite = build_composite_chart_score(grammar, patterns, options);
  result.composite_lower_bound = composite.weighted_lower_bound;
  result.keep_production.assign(grammar.productions.size(), false);
  result.frontier_sizes_by_clade.assign(grammar.clades.size(), 0);
  result.invariant_constant_offset =
      invariant_constant_offset(patterns, options);

  auto active = build_active_pattern_info(grammar, patterns, options);
  result.active_pattern_count = active.size();
  result.initial_upper_bound =
      initial_upper_bound(grammar, patterns, active, options);
  auto pruning_upper_bound =
      effective_pruning_upper_bound(result.initial_upper_bound, trim_options);

  std::vector<clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize < rsize;
    return lhs < rhs;
  });

  std::vector<std::vector<frontier_entry>> frontiers(grammar.clades.size());
  for (auto clade : order) {
    auto const& key = grammar.clades[clade];
    if (key.taxa.size() == 1) {
      frontiers[clade].push_back(
          make_leaf_frontier_entry(grammar, clade, active));
      result.frontier_sizes_by_clade[clade] = frontiers[clade].size();
      continue;
    }

    std::unordered_map<std::vector<chart_cost>, std::size_t,
                       chart_cost_vector_hash>
        index_by_cost;
    auto& entries = frontiers[clade];
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      if (prod.parent != clade) {
        throw std::runtime_error(
            "multi-site trim: production parent mismatch during frontier "
            "construction");
      }
      chart_trim_detail::validate_binary_production_for_trim(grammar, prod,
                                                             pid);
      auto left_child = prod.children[0];
      auto right_child = prod.children[1];
      if (left_child >= frontiers.size() || right_child >= frontiers.size()) {
        throw std::runtime_error(
            "multi-site trim: production child out of frontier range");
      }
      for (auto const& left : frontiers[left_child]) {
        for (auto const& right : frontiers[right_child]) {
          auto candidate = combine_frontier_entries(grammar, prod, pid, left,
                                                    right, active.size());
          if (trim_options.use_bound_pruning &&
              pruning_upper_bound < multisite_score_inf) {
            auto lb = lower_bound_for_entry(candidate, clade, active,
                                            result.invariant_constant_offset,
                                            options);
            if (lb > pruning_upper_bound) {
              ++result.bound_pruned;
              continue;
            }
          }
          insert_or_merge_frontier_entry(entries, index_by_cost,
                                         std::move(candidate),
                                         result.equality_deduplicated);
        }
      }
    }

    if (trim_options.max_frontier_entries_per_clade != 0 &&
        entries.size() > trim_options.max_frontier_entries_per_clade) {
      throw std::runtime_error(
          "multi-site trim: frontier entry cap exceeded for clade " +
          std::to_string(clade));
    }
    // Bound pruning can empty a non-root clade frontier when every topology
    // using that clade is already provably worse than the current feasible
    // upper bound.  Parent combinations that depend on it simply generate no
    // candidates.  The root frontier is checked after the pass.
    result.frontier_sizes_by_clade[clade] = entries.size();
  }

  auto const& root_frontier = frontiers[grammar.root_clade];
  if (root_frontier.empty()) {
    throw std::runtime_error("multi-site trim: empty root frontier");
  }

  for (auto const& entry : root_frontier) {
    auto score =
        lower_bound_for_entry(entry, grammar.root_clade, active,
                              result.invariant_constant_offset, options);
    if (score < result.optimum) {
      result.optimum = score;
      std::fill(result.keep_production.begin(), result.keep_production.end(),
                false);
    }
    if (score == result.optimum) {
      merge_used_productions(result.keep_production, entry.used_production);
    }
  }
  validate_known_exact_optimum(result.optimum, trim_options,
                               "multi-site trim");
  result.dominance_pruned = result.dominance_pruned_score_pass +
                            result.dominance_pruned_mask_pass;
  return result;
}

inline std::uint64_t score_selected_topology(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    grammar_topology const& topology, chart_options const& options = {}) {
  chart_multisite_detail::validate_multisite_inputs(grammar, patterns, options);
  (void)validate_grammar_topology(grammar, topology);
  return chart_multisite_detail::score_selected_topology(grammar, patterns,
                                                         topology, options);
}

inline multisite_topology_trace_result build_multisite_optimal_topologies(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_topology_trace_options const& trace_opts = {}) {
  using namespace chart_multisite_detail;
  validate_multisite_inputs(grammar, patterns, options);
  validate_required_productions(grammar, trace_opts.required_productions);
  validate_multisite_trim_options_supported(trace_opts.trim_options,
                                            "multi-site topology trace");
  if (!trace_opts.keep_provenance) {
    throw std::runtime_error(
        "multi-site topology trace: keep_provenance=false cannot emit "
        "concrete topologies");
  }

  multisite_topology_trace_result result;
  auto composite = build_composite_chart_score(grammar, patterns, options);
  result.composite_lower_bound = composite.weighted_lower_bound;
  result.keep_production.assign(grammar.productions.size(), false);
  result.frontier_sizes_by_clade.assign(grammar.clades.size(), 0);
  result.invariant_constant_offset = invariant_constant_offset(patterns, options);

  auto active = build_active_pattern_info(grammar, patterns, options);
  result.active_pattern_count = active.size();
  result.initial_upper_bound = initial_upper_bound(grammar, patterns, active,
                                                   options);
  auto pruning_upper_bound = effective_pruning_upper_bound(
      result.initial_upper_bound, trace_opts.trim_options);

  std::vector<clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize < rsize;
    return lhs < rhs;
  });

  std::vector<std::vector<frontier_entry>> frontiers(grammar.clades.size());
  for (auto clade : order) {
    auto const& key = grammar.clades[clade];
    if (key.taxa.size() == 1) {
      frontiers[clade].push_back(
          make_leaf_frontier_entry(grammar, clade, active));
      result.frontier_sizes_by_clade[clade] = frontiers[clade].size();
      continue;
    }

    std::unordered_map<std::vector<chart_cost>, std::size_t,
                       chart_cost_vector_hash>
        index_by_cost;
    auto& entries = frontiers[clade];
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      if (prod.parent != clade) {
        throw std::runtime_error(
            "multi-site topology trace: production parent mismatch during "
            "frontier construction");
      }
      chart_trim_detail::validate_binary_production_for_trim(grammar, prod,
                                                             pid);
      auto left_child = prod.children[0];
      auto right_child = prod.children[1];
      if (left_child >= frontiers.size() || right_child >= frontiers.size()) {
        throw std::runtime_error(
            "multi-site topology trace: production child out of frontier "
            "range");
      }
      for (std::size_t left_index = 0;
           left_index < frontiers[left_child].size(); ++left_index) {
        auto const& left = frontiers[left_child][left_index];
        for (std::size_t right_index = 0;
             right_index < frontiers[right_child].size(); ++right_index) {
          auto const& right = frontiers[right_child][right_index];
          auto candidate = combine_frontier_entries(grammar, prod, pid, left,
                                                    right, active.size());
          candidate.provenance.push_back(frontier_provenance_choice{
              pid, left_index, right_index});
          if (trace_opts.trim_options.use_bound_pruning &&
              pruning_upper_bound < multisite_score_inf) {
            auto lb = lower_bound_for_entry(candidate, clade, active,
                                            result.invariant_constant_offset,
                                            options);
            if (lb > pruning_upper_bound) {
              ++result.bound_pruned;
              continue;
            }
          }
          insert_or_merge_frontier_entry(
              entries, index_by_cost, std::move(candidate),
              result.equality_deduplicated,
              trace_opts.max_provenance_choices_per_entry);
        }
      }
    }

    if (trace_opts.trim_options.max_frontier_entries_per_clade != 0 &&
        entries.size() > trace_opts.trim_options.max_frontier_entries_per_clade) {
      throw std::runtime_error(
          "multi-site topology trace: frontier entry cap exceeded for clade " +
          std::to_string(clade));
    }
    result.frontier_sizes_by_clade[clade] = entries.size();
  }

  auto const& root_frontier = frontiers[grammar.root_clade];
  if (root_frontier.empty()) {
    throw std::runtime_error("multi-site topology trace: empty root frontier");
  }

  std::vector<std::size_t> optimal_root_entries;
  for (std::size_t entry_index = 0; entry_index < root_frontier.size();
       ++entry_index) {
    auto score = lower_bound_for_entry(root_frontier[entry_index],
                                       grammar.root_clade, active,
                                       result.invariant_constant_offset,
                                       options);
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
  result.topology_cap_truncated = result.topology_cap_truncated ||
                                  provenance_truncated;

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
        std::to_string(result.optimum) + ", emitted=" +
        std::to_string(result.topologies.size()) + ", uncovered=[";
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

}  // namespace larch

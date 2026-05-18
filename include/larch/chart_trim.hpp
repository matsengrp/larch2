#pragma once

#include <larch/parsimony_chart.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
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
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs,
                                                   clade_id rhs) {
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
      if (std::find(children.begin(), children.end(), child) == children.end()) {
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
      throw std::runtime_error("chart trim: production child clade out of range");
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

inline bool is_globally_optimal_state(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside, clade_id clade,
    std::uint8_t state) {
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
    if (local >= chart_inf || local != chart.inside[clade][parent_state]) return;

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
        choices, chart_production_choice{pid, parent_state, child_states,
                                         complete});
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
          "chart trim: non-leaf optimal state has no optimal production choice");
    }
    auto choice_index = choose_index(choices.size());
    if (choice_index >= choices.size()) {
      throw std::runtime_error("chart trim: traceback choice index out of range");
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
    throw std::runtime_error("chart trim: traceback root choice index out of range");
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
    result.outside[root][state] =
        options.score_ua_edge ? transition_cost(reference_state, state)
                              : chart_cost{0};
  }

  result.global_min = chart_trim_detail::compute_global_min(grammar, chart,
                                                            result);

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
          for (std::uint8_t sibling_state = 0;
               sibling_state < nuc_state_count; ++sibling_state) {
            auto candidate = saturated_add(
                chart.inside[sibling][sibling_state],
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

}  // namespace larch

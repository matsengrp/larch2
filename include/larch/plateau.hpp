#pragma once

#include <larch/chart_trim.hpp>
#include <larch/lazy_chart.hpp>
#include <larch/site_patterns.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larch {

inline constexpr std::uint8_t no_child_slot =
    std::numeric_limits<std::uint8_t>::max();

struct plateau_row_resolution_choice {
  production_id production = no_production;
  std::uint8_t parent_state = no_chart_state;
  std::vector<std::uint8_t> child_states;
  chart_cost cost = chart_inf;

  bool operator==(plateau_row_resolution_choice const&) const = default;
};

struct plateau_parent_compatibility {
  // Root-boundary compatibility is represented with parent == no_clade and
  // production == no_production.  For non-root clades, parent/production name a
  // globally optimal parent production in which this clade can present
  // child_state to the parent.
  clade_id parent = no_clade;
  production_id production = no_production;
  std::uint8_t parent_state = no_chart_state;
  std::uint8_t child_state = no_chart_state;
  std::uint8_t child_slot = no_child_slot;
  std::uint8_t sibling_state = no_chart_state;
  // Filled only for non-binary parent productions.  The vector is aligned with
  // grammar_production::children and records the complete parent-resolution
  // child-state context.
  std::vector<std::uint8_t> production_child_states;
  bool root_boundary = false;

  bool operator==(plateau_parent_compatibility const&) const = default;
};

struct fluidity_group {
  clade_id clade = no_clade;
  // State presented by this group to its parent/root boundary.
  std::uint8_t state = no_chart_state;
  std::vector<production_id> productions;
  std::vector<chart_production_choice> choices;
  std::vector<plateau_row_resolution_choice> row_choices;
  std::vector<plateau_parent_compatibility> parent_compatibilities;
  std::size_t choice_count = 0;
  bool fluid = false;
  bool globally_optimal = false;
  bool globally_fluid = false;
  bool externally_fluid = false;
};

struct fluidity_report {
  // Phase-7 fluidity: true when multiple distinct optimal
  // production/child-state choices for (clade,state) participate in at least
  // one globally optimal complete traceback.
  std::vector<std::array<bool, nuc_state_count>> fluid_clade_state;

  // Local inside-chart fluidity, regardless of whether the state can appear in
  // a globally optimal complete traceback.  This is a diagnostic for local
  // trace-set ties and paper-style subchart examples.
  std::vector<std::array<bool, nuc_state_count>> locally_fluid_clade_state;

  // Globally optimal states according to inside + outside.
  std::vector<std::array<bool, nuc_state_count>> globally_optimal_clade_state;

  // Global fluidity: local choices that also participate in a globally optimal
  // complete traceback under the supplied outside chart.
  std::vector<std::array<bool, nuc_state_count>> globally_fluid_clade_state;

  // A clade is externally fluid when its globally optimal traces can expose
  // more than one state/context to the parent.  Purely internal ties with the
  // same parent-visible state remain externally rigid.
  std::vector<bool> externally_fluid_clade;

  // Coarse groups requested by the phase plan: one entry per globally optimal
  // output-state / parent-compatibility group, containing the unique production
  // IDs in that group.  See optimal_group_descriptors for labels/counts.
  std::vector<std::vector<production_id>> optimal_groups;
  std::vector<fluidity_group> optimal_group_descriptors;

  std::vector<std::array<std::size_t, nuc_state_count>> optimal_choice_count;
  std::vector<std::array<std::size_t, nuc_state_count>>
      globally_optimal_choice_count;

  std::vector<std::array<std::vector<chart_production_choice>,
                         nuc_state_count>>
      optimal_choices_by_clade_state;
  std::vector<std::array<std::vector<chart_production_choice>,
                         nuc_state_count>>
      globally_optimal_choices_by_clade_state;
  std::vector<std::array<std::vector<plateau_row_resolution_choice>,
                         nuc_state_count>>
      row_optimal_choices_by_clade_state;
  std::vector<std::array<std::vector<plateau_row_resolution_choice>,
                         nuc_state_count>>
      row_globally_optimal_choices_by_clade_state;
  std::vector<std::array<std::vector<plateau_parent_compatibility>,
                         nuc_state_count>>
      parent_compatibilities;

  chart_cost global_min = chart_inf;
  std::size_t fluid_clade_state_count = 0;
  std::size_t locally_fluid_clade_state_count = 0;
  std::size_t globally_fluid_clade_state_count = 0;
  std::size_t externally_fluid_clade_count = 0;
  std::size_t externally_fluid_group_count = 0;
  std::size_t chart_row_fluidity_runs = 0;
};

struct plateau_graph_node {
  // Coarsened graph nodes are externally-fluid groups, not whole clades.
  std::size_t group_index = std::numeric_limits<std::size_t>::max();
  clade_id clade = no_clade;
  std::vector<std::uint8_t> globally_optimal_states;
  std::vector<std::size_t> optimal_group_indices;
};

struct plateau_graph_edge {
  std::size_t from = 0;
  std::size_t to = 0;
  std::uint64_t compatibility_count = 0;
};

struct plateau_graph {
  // Coarsened nodes are externally-fluid output-state / parent-compatibility
  // groups. Edges connect compatible externally-fluid parent/child groups in
  // at least one globally optimal outside trace. Connected components are
  // undirected components of this coarsened graph.
  std::vector<plateau_graph_node> nodes;
  std::vector<plateau_graph_edge> edges;
  std::vector<std::vector<std::size_t>> connected_components;
};

struct multisite_plateau_report {
  // Placeholder until exact multi-site plateau extraction is wired to the
  // Phase-5 coupled topology frontiers/masks.  Independent per-site trace sets
  // are not an exact multi-site plateau description.
};

namespace plateau_detail {

inline std::array<bool, nuc_state_count> false_state_mask() {
  std::array<bool, nuc_state_count> row{};
  row.fill(false);
  return row;
}

inline std::array<std::size_t, nuc_state_count> zero_state_counts() {
  std::array<std::size_t, nuc_state_count> row{};
  row.fill(0);
  return row;
}

inline char state_char(std::uint8_t state) {
  switch (state) {
    case nuc_base::A:
      return 'A';
    case nuc_base::C:
      return 'C';
    case nuc_base::G:
      return 'G';
    case nuc_base::T:
      return 'T';
    default:
      return '?';
  }
}

inline bool compatibility_less(plateau_parent_compatibility const& lhs,
                               plateau_parent_compatibility const& rhs) {
  if (lhs.root_boundary != rhs.root_boundary)
    return lhs.root_boundary < rhs.root_boundary;
  if (lhs.parent != rhs.parent) return lhs.parent < rhs.parent;
  if (lhs.production != rhs.production) return lhs.production < rhs.production;
  if (lhs.parent_state != rhs.parent_state)
    return lhs.parent_state < rhs.parent_state;
  if (lhs.child_state != rhs.child_state)
    return lhs.child_state < rhs.child_state;
  if (lhs.child_slot != rhs.child_slot) return lhs.child_slot < rhs.child_slot;
  if (lhs.sibling_state != rhs.sibling_state)
    return lhs.sibling_state < rhs.sibling_state;
  return lhs.production_child_states < rhs.production_child_states;
}

inline void append_unique_compatibility(
    std::vector<plateau_parent_compatibility>& compatibilities,
    plateau_parent_compatibility compatibility) {
  if (std::none_of(compatibilities.begin(), compatibilities.end(),
                   [&](auto const& existing) {
                     return existing == compatibility;
                   })) {
    compatibilities.push_back(compatibility);
  }
}

inline std::vector<chart_production_choice> local_optimal_choices_for_state(
    clade_grammar const& grammar, single_site_chart const& chart,
    clade_id clade, std::uint8_t state) {
  parsimony_chart_detail::validate_state(state, "plateau clade state");
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("plateau: clade id out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1 ||
      chart.inside[clade][state] >= chart_inf) {
    return {};
  }

  std::vector<chart_production_choice> choices;

  auto consider = [&](production_id pid,
                      std::array<std::uint8_t, 2> child_states) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("plateau: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "plateau: production parent does not match requested clade");
    }
    chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
    auto local = chart_trim_detail::production_choice_inside_cost(
        grammar, chart, prod, state, child_states);
    if (local < chart_inf && local == chart.inside[clade][state]) {
      chart_trim_detail::append_unique_choice(
          choices, chart_production_choice{pid, state, child_states, local});
    }
  };

  if (chart.has_trace()) {
    for (auto const& choice : chart.optimal_choices[clade][state]) {
      consider(choice.production, choice.child_states);
    }
  } else {
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      chart_trim_detail::validate_binary_production_for_trim(grammar, prod,
                                                             pid);
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

inline std::vector<production_id> unique_productions(
    std::vector<chart_production_choice> const& choices) {
  std::vector<production_id> productions;
  productions.reserve(choices.size());
  for (auto const& choice : choices) productions.push_back(choice.production);
  std::sort(productions.begin(), productions.end());
  productions.erase(std::unique(productions.begin(), productions.end()),
                    productions.end());
  return productions;
}

inline std::vector<production_id> unique_row_productions(
    std::vector<plateau_row_resolution_choice> const& choices) {
  std::vector<production_id> productions;
  productions.reserve(choices.size());
  for (auto const& choice : choices) productions.push_back(choice.production);
  std::sort(productions.begin(), productions.end());
  productions.erase(std::unique(productions.begin(), productions.end()),
                    productions.end());
  return productions;
}

struct resolution_signature {
  production_id production = no_production;
  std::array<std::uint8_t, 2> child_states{no_chart_state, no_chart_state};

  bool operator<(resolution_signature const& other) const {
    if (production != other.production) return production < other.production;
    return child_states < other.child_states;
  }

  bool operator==(resolution_signature const&) const = default;
};

struct choice_signature {
  production_id production = no_production;
  std::array<std::uint8_t, 2> child_states{no_chart_state, no_chart_state};

  bool operator<(choice_signature const& other) const {
    if (production != other.production) return production < other.production;
    return child_states < other.child_states;
  }

  bool operator==(choice_signature const&) const = default;
};

struct row_resolution_signature {
  production_id production = no_production;
  std::vector<std::uint8_t> child_states;

  bool operator<(row_resolution_signature const& other) const {
    if (production != other.production) return production < other.production;
    return child_states < other.child_states;
  }

  bool operator==(row_resolution_signature const&) const = default;
};

inline resolution_signature resolution_from_choice(
    chart_production_choice const& choice) {
  return resolution_signature{.production = choice.production,
                              .child_states = choice.child_states};
}

inline row_resolution_signature row_resolution_from_choice(
    plateau_row_resolution_choice const& choice) {
  return row_resolution_signature{.production = choice.production,
                                  .child_states = choice.child_states};
}

inline choice_signature signature_from_choice(
    chart_production_choice const& choice) {
  return choice_signature{.production = choice.production,
                          .child_states = choice.child_states};
}

inline std::vector<choice_signature> choice_signatures(
    std::vector<chart_production_choice> const& choices) {
  std::vector<choice_signature> signatures;
  signatures.reserve(choices.size());
  for (auto const& choice : choices)
    signatures.push_back(signature_from_choice(choice));
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(std::unique(signatures.begin(), signatures.end()),
                   signatures.end());
  return signatures;
}

inline std::vector<row_resolution_signature> row_resolution_signatures(
    std::vector<plateau_row_resolution_choice> const& choices) {
  std::vector<row_resolution_signature> signatures;
  signatures.reserve(choices.size());
  for (auto const& choice : choices)
    signatures.push_back(row_resolution_from_choice(choice));
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(std::unique(signatures.begin(), signatures.end()),
                   signatures.end());
  return signatures;
}

inline void sort_choices(std::vector<chart_production_choice>& choices) {
  std::sort(choices.begin(), choices.end(),
            [](chart_production_choice const& lhs,
               chart_production_choice const& rhs) {
              if (lhs.production != rhs.production)
                return lhs.production < rhs.production;
              if (lhs.parent_state != rhs.parent_state)
                return lhs.parent_state < rhs.parent_state;
              if (lhs.child_states[0] != rhs.child_states[0])
                return lhs.child_states[0] < rhs.child_states[0];
              if (lhs.child_states[1] != rhs.child_states[1])
                return lhs.child_states[1] < rhs.child_states[1];
              return lhs.cost < rhs.cost;
            });
}

inline void sort_row_choices(
    std::vector<plateau_row_resolution_choice>& choices) {
  std::sort(choices.begin(), choices.end(),
            [](plateau_row_resolution_choice const& lhs,
               plateau_row_resolution_choice const& rhs) {
              if (lhs.production != rhs.production)
                return lhs.production < rhs.production;
              if (lhs.parent_state != rhs.parent_state)
                return lhs.parent_state < rhs.parent_state;
              if (lhs.child_states != rhs.child_states)
                return lhs.child_states < rhs.child_states;
              return lhs.cost < rhs.cost;
            });
}

inline void append_unique_row_choice(
    std::vector<plateau_row_resolution_choice>& choices,
    plateau_row_resolution_choice choice) {
  if (std::none_of(choices.begin(), choices.end(), [&](auto const& existing) {
        return existing == choice;
      })) {
    choices.push_back(std::move(choice));
  }
}

inline std::optional<chart_production_choice> binary_choice_from_row_choice(
    clade_grammar const& grammar, plateau_row_resolution_choice const& choice) {
  if (choice.production == no_production ||
      choice.production >= grammar.productions.size()) {
    throw std::runtime_error("plateau: row choice production id out of range");
  }
  auto const& prod = grammar.productions[choice.production];
  if (prod.children.size() != 2) return std::nullopt;
  if (choice.child_states.size() != 2) {
    throw std::runtime_error(
        "plateau: binary row choice child-state count mismatch");
  }
  return chart_production_choice{
      .production = choice.production,
      .parent_state = choice.parent_state,
      .child_states = {choice.child_states[0], choice.child_states[1]},
      .cost = choice.cost};
}

inline std::vector<chart_production_choice> binary_choices_from_row_choices(
    clade_grammar const& grammar,
    std::vector<plateau_row_resolution_choice> const& row_choices) {
  std::vector<chart_production_choice> choices;
  choices.reserve(row_choices.size());
  for (auto const& row_choice : row_choices) {
    auto binary = binary_choice_from_row_choice(grammar, row_choice);
    if (binary) chart_trim_detail::append_unique_choice(choices, *binary);
  }
  sort_choices(choices);
  return choices;
}

inline std::vector<std::size_t> child_slots(grammar_production const& prod,
                                            clade_id child) {
  std::vector<std::size_t> slots;
  for (std::size_t i = 0; i < prod.children.size(); ++i) {
    if (prod.children[i] == child) slots.push_back(i);
  }
  return slots;
}

inline std::vector<plateau_parent_compatibility>
parent_compatibilities_for_state(clade_grammar const& grammar,
                                 single_site_chart const& chart,
                                 single_site_outside_chart const& outside,
                                 clade_id clade, std::uint8_t state) {
  parsimony_chart_detail::validate_state(state, "plateau child state");
  if (!chart_trim_detail::is_globally_optimal_state(grammar, chart, outside,
                                                    clade, state)) {
    return {};
  }

  std::vector<plateau_parent_compatibility> result;
  if (clade == grammar.root_clade) {
    append_unique_compatibility(
        result, plateau_parent_compatibility{.parent = no_clade,
                                             .production = no_production,
                                             .parent_state = state,
                                             .child_state = state,
                                             .child_slot = no_child_slot,
                                             .sibling_state = no_chart_state,
                                             .root_boundary = true});
    return result;
  }

  for (auto pid : grammar.productions_by_child[clade]) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "plateau: productions_by_child contains invalid production id");
    }
    auto const& prod = grammar.productions[pid];
    chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
    auto parent = prod.parent;
    if (parent == no_clade || parent >= grammar.clades.size()) {
      throw std::runtime_error("plateau: production parent out of range");
    }

    for (auto slot : child_slots(prod, clade)) {
      if (slot >= 2) {
        throw std::runtime_error("plateau: binary child slot out of range");
      }
      auto sibling_slot = 1 - slot;
      auto sibling = prod.children[sibling_slot];
      for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
           ++parent_state) {
        auto outside_parent = outside.outside[parent][parent_state];
        if (outside_parent >= chart_inf) continue;

        for (std::uint8_t sibling_state = 0; sibling_state < nuc_state_count;
             ++sibling_state) {
          auto child_term = parsimony_chart_detail::saturated_add(
              chart.inside[clade][state],
              parsimony_chart_detail::transition_cost(parent_state, state));
          auto sibling_term = parsimony_chart_detail::saturated_add(
              chart.inside[sibling][sibling_state],
              parsimony_chart_detail::transition_cost(parent_state,
                                                      sibling_state));
          auto local = parsimony_chart_detail::saturated_add(child_term,
                                                            sibling_term);
          if (local >= chart_inf || local != chart.inside[parent][parent_state])
            continue;

          auto complete = parsimony_chart_detail::saturated_add(
              outside_parent, local);
          if (complete >= chart_inf || complete != outside.global_min) continue;

          append_unique_compatibility(
              result,
              plateau_parent_compatibility{
                  .parent = parent,
                  .production = pid,
                  .parent_state = parent_state,
                  .child_state = state,
                  .child_slot = static_cast<std::uint8_t>(slot),
                  .sibling_state = sibling_state,
                  .root_boundary = false});
        }
      }
    }
  }

  std::sort(result.begin(), result.end(), compatibility_less);
  return result;
}

inline chart_cost child_term(single_site_chart const& chart, clade_id child,
                             std::uint8_t parent_state,
                             std::uint8_t child_state) {
  if (child == no_clade || child >= chart.inside.size()) {
    throw std::runtime_error("plateau: production child clade out of range");
  }
  parsimony_chart_detail::validate_state(child_state,
                                         "plateau row child state");
  return parsimony_chart_detail::saturated_add(
      chart.inside[child][child_state],
      parsimony_chart_detail::transition_cost(parent_state, child_state));
}

inline std::pair<chart_cost, std::vector<std::uint8_t>>
optimal_child_states_for_parent_state(single_site_chart const& chart,
                                      clade_id child,
                                      std::uint8_t parent_state) {
  chart_cost best = chart_inf;
  std::vector<std::uint8_t> states;
  for (std::uint8_t child_state = 0; child_state < nuc_state_count;
       ++child_state) {
    auto term = child_term(chart, child, parent_state, child_state);
    if (term < best) {
      best = term;
      states.clear();
      states.push_back(child_state);
    } else if (term == best) {
      states.push_back(child_state);
    }
  }
  if (best >= chart_inf) states.clear();
  return {best, states};
}

template <typename Callback>
inline void enumerate_state_product(
    std::vector<std::vector<std::uint8_t>> const& states_by_child,
    Callback&& callback) {
  std::vector<std::uint8_t> selected(states_by_child.size(), no_chart_state);
  auto recur = [&](auto&& self, std::size_t child_i) -> void {
    if (child_i == states_by_child.size()) {
      callback(selected);
      return;
    }
    for (auto state : states_by_child[child_i]) {
      selected[child_i] = state;
      self(self, child_i + 1);
    }
  };
  recur(recur, 0);
}

inline chart_cost production_inside_cost_from_child_states(
    single_site_chart const& chart, grammar_production const& prod,
    std::uint8_t parent_state,
    std::vector<std::uint8_t> const& child_states) {
  if (child_states.size() != prod.children.size()) {
    throw std::runtime_error(
        "plateau: row resolution child-state count mismatch");
  }
  chart_cost total = 0;
  for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
    total = parsimony_chart_detail::saturated_add(
        total, child_term(chart, prod.children[child_i], parent_state,
                          child_states[child_i]));
  }
  return total;
}

inline std::vector<plateau_row_resolution_choice>
row_local_optimal_choices_for_state(clade_grammar const& grammar,
                                    single_site_chart const& chart,
                                    clade_id clade, std::uint8_t state) {
  parsimony_chart_detail::validate_state(state, "plateau clade state");
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("plateau: clade id out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1 ||
      chart.inside[clade][state] >= chart_inf) {
    return {};
  }

  std::vector<plateau_row_resolution_choice> choices;
  for (auto pid : grammar.productions_by_parent[clade]) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("plateau: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "plateau: production parent does not match requested clade");
    }

    chart_cost local = 0;
    std::vector<std::vector<std::uint8_t>> states_by_child;
    states_by_child.reserve(prod.children.size());
    bool finite = true;
    for (auto child : prod.children) {
      auto [best, states] =
          optimal_child_states_for_parent_state(chart, child, state);
      if (best >= chart_inf || states.empty()) {
        finite = false;
        break;
      }
      local = parsimony_chart_detail::saturated_add(local, best);
      states_by_child.push_back(std::move(states));
    }
    if (!finite || local >= chart_inf || local != chart.inside[clade][state])
      continue;

    enumerate_state_product(states_by_child, [&](auto const& child_states) {
      append_unique_row_choice(
          choices, plateau_row_resolution_choice{.production = pid,
                                                 .parent_state = state,
                                                 .child_states = child_states,
                                                 .cost = local});
    });
  }

  sort_row_choices(choices);
  choices.erase(std::unique(choices.begin(), choices.end()), choices.end());
  return choices;
}

inline std::vector<plateau_row_resolution_choice>
row_globally_optimal_choices_for_state(clade_grammar const& grammar,
                                       single_site_chart const& chart,
                                       single_site_outside_chart const& outside,
                                       clade_id clade,
                                       std::uint8_t state) {
  parsimony_chart_detail::validate_state(state, "plateau clade state");
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("plateau: clade id out of range");
  }
  if (grammar.clades[clade].taxa.size() == 1) return {};
  if (!chart_trim_detail::is_globally_optimal_state(grammar, chart, outside,
                                                    clade, state)) {
    return {};
  }

  std::vector<plateau_row_resolution_choice> choices;
  for (auto pid : grammar.productions_by_parent[clade]) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("plateau: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "plateau: production parent does not match requested clade");
    }

    chart_cost local = 0;
    std::vector<std::vector<std::uint8_t>> states_by_child;
    states_by_child.reserve(prod.children.size());
    bool finite = true;
    for (auto child : prod.children) {
      auto [best, states] =
          optimal_child_states_for_parent_state(chart, child, state);
      if (best >= chart_inf || states.empty()) {
        finite = false;
        break;
      }
      local = parsimony_chart_detail::saturated_add(local, best);
      states.erase(std::remove_if(states.begin(), states.end(),
                                  [&](std::uint8_t child_state) {
                                    return !chart_trim_detail::
                                        is_globally_optimal_state(
                                            grammar, chart, outside, child,
                                            child_state);
                                  }),
                   states.end());
      if (states.empty()) {
        finite = false;
        break;
      }
      states_by_child.push_back(std::move(states));
    }
    if (!finite || local >= chart_inf || local != chart.inside[clade][state])
      continue;

    auto complete = parsimony_chart_detail::saturated_add(
        outside.outside[clade][state], local);
    if (complete >= chart_inf || complete != outside.global_min) continue;

    enumerate_state_product(states_by_child, [&](auto const& child_states) {
      append_unique_row_choice(
          choices, plateau_row_resolution_choice{.production = pid,
                                                 .parent_state = state,
                                                 .child_states = child_states,
                                                 .cost = complete});
    });
  }

  sort_row_choices(choices);
  choices.erase(std::unique(choices.begin(), choices.end()), choices.end());
  return choices;
}

inline std::vector<plateau_parent_compatibility>
row_parent_compatibilities_for_state(clade_grammar const& grammar,
                                     single_site_chart const& chart,
                                     single_site_outside_chart const& outside,
                                     clade_id clade, std::uint8_t state) {
  parsimony_chart_detail::validate_state(state, "plateau child state");
  if (!chart_trim_detail::is_globally_optimal_state(grammar, chart, outside,
                                                    clade, state)) {
    return {};
  }

  std::vector<plateau_parent_compatibility> result;
  if (clade == grammar.root_clade) {
    append_unique_compatibility(
        result, plateau_parent_compatibility{.parent = no_clade,
                                             .production = no_production,
                                             .parent_state = state,
                                             .child_state = state,
                                             .child_slot = no_child_slot,
                                             .sibling_state = no_chart_state,
                                             .production_child_states = {},
                                             .root_boundary = true});
    return result;
  }

  for (auto pid : grammar.productions_by_child[clade]) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "plateau: productions_by_child contains invalid production id");
    }
    auto const& prod = grammar.productions[pid];
    auto parent = prod.parent;
    if (parent == no_clade || parent >= grammar.clades.size()) {
      throw std::runtime_error("plateau: production parent out of range");
    }

    for (auto slot : child_slots(prod, clade)) {
      for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
           ++parent_state) {
        if (!chart_trim_detail::is_globally_optimal_state(
                grammar, chart, outside, parent, parent_state)) {
          continue;
        }

        std::vector<std::vector<std::uint8_t>> states_by_child(
            prod.children.size());
        for (std::size_t child_i = 0; child_i < prod.children.size();
             ++child_i) {
          if (child_i == slot) {
            states_by_child[child_i] = {state};
          } else {
            states_by_child[child_i] = {0, 1, 2, 3};
          }
        }

        enumerate_state_product(states_by_child, [&](auto const& child_states) {
          auto local = production_inside_cost_from_child_states(
              chart, prod, parent_state, child_states);
          if (local >= chart_inf ||
              local != chart.inside[parent][parent_state]) {
            return;
          }
          auto complete = parsimony_chart_detail::saturated_add(
              outside.outside[parent][parent_state], local);
          if (complete >= chart_inf || complete != outside.global_min) return;

          std::uint8_t sibling_state = no_chart_state;
          std::vector<std::uint8_t> production_child_states;
          if (prod.children.size() == 2) {
            auto sibling_slot = slot == 0 ? 1 : 0;
            sibling_state = child_states[sibling_slot];
          } else {
            production_child_states = child_states;
          }

          append_unique_compatibility(
              result,
              plateau_parent_compatibility{
                  .parent = parent,
                  .production = pid,
                  .parent_state = parent_state,
                  .child_state = state,
                  .child_slot = static_cast<std::uint8_t>(slot),
                  .sibling_state = sibling_state,
                  .production_child_states = std::move(production_child_states),
                  .root_boundary = false});
        });
      }
    }
  }

  std::sort(result.begin(), result.end(), compatibility_less);
  return result;
}

struct external_signature {
  bool root_boundary = false;
  clade_id parent = no_clade;
  production_id production = no_production;
  std::uint8_t parent_state = no_chart_state;
  std::uint8_t child_state = no_chart_state;
  std::uint8_t child_slot = no_child_slot;
  std::uint8_t sibling_state = no_chart_state;
  std::vector<std::uint8_t> production_child_states;

  bool operator<(external_signature const& other) const {
    if (root_boundary != other.root_boundary)
      return root_boundary < other.root_boundary;
    if (parent != other.parent) return parent < other.parent;
    if (production != other.production) return production < other.production;
    if (parent_state != other.parent_state)
      return parent_state < other.parent_state;
    if (child_state != other.child_state) return child_state < other.child_state;
    if (child_slot != other.child_slot) return child_slot < other.child_slot;
    if (sibling_state != other.sibling_state)
      return sibling_state < other.sibling_state;
    return production_child_states < other.production_child_states;
  }
};

inline external_signature signature_from_compatibility(
    plateau_parent_compatibility const& compatibility) {
  return external_signature{.root_boundary = compatibility.root_boundary,
                            .parent = compatibility.parent,
                            .production = compatibility.production,
                            .parent_state = compatibility.parent_state,
                            .child_state = compatibility.child_state,
                            .child_slot = compatibility.child_slot,
                            .sibling_state = compatibility.sibling_state,
                            .production_child_states =
                                compatibility.production_child_states};
}

inline std::vector<std::size_t> connected_component_from(
    std::size_t start, std::vector<std::vector<std::size_t>> const& adjacency,
    std::vector<bool>& seen) {
  std::vector<std::size_t> stack{start};
  std::vector<std::size_t> component;
  seen[start] = true;
  while (!stack.empty()) {
    auto node = stack.back();
    stack.pop_back();
    component.push_back(node);
    for (auto next : adjacency[node]) {
      if (!seen[next]) {
        seen[next] = true;
        stack.push_back(next);
      }
    }
  }
  std::sort(component.begin(), component.end());
  return component;
}

inline std::string clade_taxa_label(clade_grammar const& grammar,
                                    clade_id clade) {
  if (clade == no_clade || clade >= grammar.clades.size()) return "<none>";
  std::string label;
  label += "{";
  bool first = true;
  for (auto taxon : grammar.clades[clade].taxa) {
    if (!first) label += ",";
    first = false;
    if (taxon < grammar.taxa.id_to_sample_id.size()) {
      label += grammar.taxa.id_to_sample_id[taxon];
    } else {
      label += std::to_string(taxon);
    }
  }
  label += "}";
  return label;
}

inline std::string state_list(
    std::array<bool, nuc_state_count> const& state_mask) {
  std::string result;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    if (!state_mask[state]) continue;
    if (!result.empty()) result += ",";
    result += state_char(state);
  }
  return result.empty() ? std::string{"-"} : result;
}

inline fluidity_report make_empty_fluidity_report(
    clade_grammar const& grammar, chart_cost global_min) {
  fluidity_report report;
  report.global_min = global_min;
  report.fluid_clade_state.assign(grammar.clades.size(), false_state_mask());
  report.locally_fluid_clade_state.assign(grammar.clades.size(),
                                          false_state_mask());
  report.globally_optimal_clade_state.assign(grammar.clades.size(),
                                             false_state_mask());
  report.globally_fluid_clade_state.assign(grammar.clades.size(),
                                           false_state_mask());
  report.externally_fluid_clade.assign(grammar.clades.size(), false);
  report.optimal_choice_count.assign(grammar.clades.size(),
                                     zero_state_counts());
  report.globally_optimal_choice_count.assign(grammar.clades.size(),
                                             zero_state_counts());
  report.optimal_choices_by_clade_state.resize(grammar.clades.size());
  report.globally_optimal_choices_by_clade_state.resize(grammar.clades.size());
  report.row_optimal_choices_by_clade_state.resize(grammar.clades.size());
  report.row_globally_optimal_choices_by_clade_state.resize(
      grammar.clades.size());
  report.parent_compatibilities.resize(grammar.clades.size());
  return report;
}

}  // namespace plateau_detail

inline fluidity_report build_single_site_fluidity_report_from_choice_layer(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  chart_trim_detail::validate_outside_shapes(grammar, chart, outside);
  parsimony_chart_detail::require_no_multifurcating_productions_for_consumer(
      grammar, arity_gate_consumer::single_site_fluidity_report,
      "single-site fluidity report", "choice layer",
      "use dense chart diagnostics or selected-topology/SPR paths on the "
      "multifurcating grammar, or expand polytomies before fluidity analysis");
  if (chart_trim_detail::compute_global_min(grammar, chart, outside) !=
      outside.global_min) {
    throw std::runtime_error("plateau: outside global optimum is stale");
  }

  auto report =
      plateau_detail::make_empty_fluidity_report(grammar, outside.global_min);

  std::vector<std::vector<std::size_t>> group_indices_by_clade(
      grammar.clades.size());

  for (clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    std::map<plateau_detail::external_signature, std::size_t>
        group_by_signature;
    std::set<plateau_detail::resolution_signature> clade_resolution_signatures;

    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      auto local_choices = plateau_detail::local_optimal_choices_for_state(
          grammar, chart, clade, state);
      auto global_choices =
          chart_trim_detail::globally_optimal_choices_for_state(
              grammar, chart, outside, clade, state);
      auto compatibilities = plateau_detail::parent_compatibilities_for_state(
          grammar, chart, outside, clade, state);
      auto globally_optimal_state =
          chart_trim_detail::is_globally_optimal_state(grammar, chart, outside,
                                                       clade, state);

      report.optimal_choice_count[clade][state] = local_choices.size();
      report.globally_optimal_choice_count[clade][state] =
          global_choices.size();
      report.locally_fluid_clade_state[clade][state] =
          local_choices.size() > 1;
      report.fluid_clade_state[clade][state] = global_choices.size() > 1;
      report.globally_optimal_clade_state[clade][state] =
          globally_optimal_state;
      report.globally_fluid_clade_state[clade][state] =
          report.fluid_clade_state[clade][state];

      if (report.locally_fluid_clade_state[clade][state])
        ++report.locally_fluid_clade_state_count;
      if (report.fluid_clade_state[clade][state])
        ++report.fluid_clade_state_count;
      if (report.globally_fluid_clade_state[clade][state])
        ++report.globally_fluid_clade_state_count;

      report.optimal_choices_by_clade_state[clade][state] =
          std::move(local_choices);
      report.globally_optimal_choices_by_clade_state[clade][state] =
          std::move(global_choices);
      report.parent_compatibilities[clade][state] =
          std::move(compatibilities);

      auto const& stored_global_choices =
          report.globally_optimal_choices_by_clade_state[clade][state];
      if (stored_global_choices.empty()) continue;

      for (auto const& choice : stored_global_choices) {
        clade_resolution_signatures.insert(
            plateau_detail::resolution_from_choice(choice));
      }

      auto group_compatibilities = report.parent_compatibilities[clade][state];
      if (group_compatibilities.empty()) {
        // A globally optimal non-leaf state should normally have an explicit
        // root or parent compatibility.  Keep a sentinel group rather than
        // dropping trace choices if an unusual caller supplies a degenerate
        // outside chart.
        group_compatibilities.push_back(
            plateau_parent_compatibility{.parent = no_clade,
                                         .production = no_production,
                                         .parent_state = state,
                                         .child_state = state,
                                         .child_slot = no_child_slot,
                                         .sibling_state = no_chart_state,
                                         .root_boundary =
                                             clade == grammar.root_clade});
      }

      for (auto const& compatibility : group_compatibilities) {
        auto signature =
            plateau_detail::signature_from_compatibility(compatibility);
        auto [it, inserted] = group_by_signature.emplace(
            signature, report.optimal_group_descriptors.size());
        if (inserted) {
          fluidity_group group;
          group.clade = clade;
          group.state = signature.child_state;
          group.globally_optimal = globally_optimal_state;
          group.parent_compatibilities.push_back(compatibility);
          report.optimal_groups.emplace_back();
          report.optimal_group_descriptors.push_back(std::move(group));
          group_indices_by_clade[clade].push_back(it->second);
        }

        auto& group = report.optimal_group_descriptors[it->second];
        for (auto const& choice : stored_global_choices) {
          chart_trim_detail::append_unique_choice(group.choices, choice);
        }
      }
    }

    std::set<std::vector<plateau_detail::choice_signature>>
        distinct_group_choice_sets;
    for (auto group_index : group_indices_by_clade[clade]) {
      auto& group = report.optimal_group_descriptors[group_index];
      plateau_detail::sort_choices(group.choices);
      group.productions = plateau_detail::unique_productions(group.choices);
      group.choice_count = group.choices.size();
      group.fluid = group.choice_count > 1;
      group.globally_fluid = group.fluid;
      report.optimal_groups[group_index] = group.productions;
      distinct_group_choice_sets.insert(
          plateau_detail::choice_signatures(group.choices));
    }

    // External fluidity requires actual globally optimal local resolution
    // multiplicity and a visible split by output state or parent compatibility
    // that separates those resolutions. Multiple parent/sibling contexts for
    // the same output state do not make an internally-fluid clade externally
    // fluid when every context can use the same local resolutions.
    auto externally_fluid = grammar.clades[clade].taxa.size() > 1 &&
                            clade_resolution_signatures.size() > 1 &&
                            distinct_group_choice_sets.size() > 1;
    report.externally_fluid_clade[clade] = externally_fluid;
    if (externally_fluid) {
      ++report.externally_fluid_clade_count;
      report.externally_fluid_group_count +=
          group_indices_by_clade[clade].size();
    }
    for (auto group_index : group_indices_by_clade[clade]) {
      report.optimal_group_descriptors[group_index].externally_fluid =
          externally_fluid;
    }
  }

  return report;
}

inline fluidity_report build_single_site_fluidity_report_from_rows(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  chart_trim_detail::validate_outside_shapes(grammar, chart, outside);
  if (chart_trim_detail::compute_global_min(grammar, chart, outside) !=
      outside.global_min) {
    throw std::runtime_error("plateau: outside global optimum is stale");
  }

  auto report =
      plateau_detail::make_empty_fluidity_report(grammar, outside.global_min);
  report.chart_row_fluidity_runs = 1;

  std::vector<std::vector<std::size_t>> group_indices_by_clade(
      grammar.clades.size());

  for (clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    std::map<plateau_detail::external_signature, std::size_t>
        group_by_signature;
    std::set<plateau_detail::row_resolution_signature>
        clade_resolution_signatures;

    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      auto row_local_choices = plateau_detail::row_local_optimal_choices_for_state(
          grammar, chart, clade, state);
      auto row_global_choices =
          plateau_detail::row_globally_optimal_choices_for_state(
              grammar, chart, outside, clade, state);
      auto local_choices = plateau_detail::binary_choices_from_row_choices(
          grammar, row_local_choices);
      auto global_choices = plateau_detail::binary_choices_from_row_choices(
          grammar, row_global_choices);
      auto compatibilities =
          plateau_detail::row_parent_compatibilities_for_state(
              grammar, chart, outside, clade, state);
      auto globally_optimal_state =
          chart_trim_detail::is_globally_optimal_state(grammar, chart, outside,
                                                       clade, state);

      report.optimal_choice_count[clade][state] = row_local_choices.size();
      report.globally_optimal_choice_count[clade][state] =
          row_global_choices.size();
      report.locally_fluid_clade_state[clade][state] =
          row_local_choices.size() > 1;
      report.fluid_clade_state[clade][state] = row_global_choices.size() > 1;
      report.globally_optimal_clade_state[clade][state] =
          globally_optimal_state;
      report.globally_fluid_clade_state[clade][state] =
          report.fluid_clade_state[clade][state];

      if (report.locally_fluid_clade_state[clade][state])
        ++report.locally_fluid_clade_state_count;
      if (report.fluid_clade_state[clade][state])
        ++report.fluid_clade_state_count;
      if (report.globally_fluid_clade_state[clade][state])
        ++report.globally_fluid_clade_state_count;

      report.optimal_choices_by_clade_state[clade][state] =
          std::move(local_choices);
      report.globally_optimal_choices_by_clade_state[clade][state] =
          std::move(global_choices);
      report.row_optimal_choices_by_clade_state[clade][state] =
          std::move(row_local_choices);
      report.row_globally_optimal_choices_by_clade_state[clade][state] =
          std::move(row_global_choices);
      report.parent_compatibilities[clade][state] =
          std::move(compatibilities);

      auto const& stored_global_row_choices =
          report.row_globally_optimal_choices_by_clade_state[clade][state];
      if (stored_global_row_choices.empty()) continue;

      for (auto const& choice : stored_global_row_choices) {
        clade_resolution_signatures.insert(
            plateau_detail::row_resolution_from_choice(choice));
      }

      auto group_compatibilities = report.parent_compatibilities[clade][state];
      if (group_compatibilities.empty()) {
        group_compatibilities.push_back(
            plateau_parent_compatibility{.parent = no_clade,
                                         .production = no_production,
                                         .parent_state = state,
                                         .child_state = state,
                                         .child_slot = no_child_slot,
                                         .sibling_state = no_chart_state,
                                         .production_child_states = {},
                                         .root_boundary =
                                             clade == grammar.root_clade});
      }

      auto const& stored_global_choices =
          report.globally_optimal_choices_by_clade_state[clade][state];
      for (auto const& compatibility : group_compatibilities) {
        auto signature =
            plateau_detail::signature_from_compatibility(compatibility);
        auto [it, inserted] = group_by_signature.emplace(
            signature, report.optimal_group_descriptors.size());
        if (inserted) {
          fluidity_group group;
          group.clade = clade;
          group.state = signature.child_state;
          group.globally_optimal = globally_optimal_state;
          group.parent_compatibilities.push_back(compatibility);
          report.optimal_groups.emplace_back();
          report.optimal_group_descriptors.push_back(std::move(group));
          group_indices_by_clade[clade].push_back(it->second);
        }

        auto& group = report.optimal_group_descriptors[it->second];
        for (auto const& choice : stored_global_row_choices) {
          plateau_detail::append_unique_row_choice(group.row_choices, choice);
        }
        for (auto const& choice : stored_global_choices) {
          chart_trim_detail::append_unique_choice(group.choices, choice);
        }
      }
    }

    std::set<std::vector<plateau_detail::row_resolution_signature>>
        distinct_group_choice_sets;
    for (auto group_index : group_indices_by_clade[clade]) {
      auto& group = report.optimal_group_descriptors[group_index];
      plateau_detail::sort_row_choices(group.row_choices);
      plateau_detail::sort_choices(group.choices);
      group.productions =
          plateau_detail::unique_row_productions(group.row_choices);
      group.choice_count = group.row_choices.size();
      group.fluid = group.choice_count > 1;
      group.globally_fluid = group.fluid;
      report.optimal_groups[group_index] = group.productions;
      distinct_group_choice_sets.insert(
          plateau_detail::row_resolution_signatures(group.row_choices));
    }

    auto externally_fluid = grammar.clades[clade].taxa.size() > 1 &&
                            clade_resolution_signatures.size() > 1 &&
                            distinct_group_choice_sets.size() > 1;
    report.externally_fluid_clade[clade] = externally_fluid;
    if (externally_fluid) {
      ++report.externally_fluid_clade_count;
      report.externally_fluid_group_count +=
          group_indices_by_clade[clade].size();
    }
    for (auto group_index : group_indices_by_clade[clade]) {
      report.optimal_group_descriptors[group_index].externally_fluid =
          externally_fluid;
    }
  }

  return report;
}

inline fluidity_report build_single_site_fluidity_report_from_rows(
    clade_grammar const& grammar, lazy_multisite_chart const& lazy_chart,
    std::size_t pattern_index) {
  auto chart = lazy_chart_detail::single_site_chart_from_lazy(
      grammar, lazy_chart, pattern_index);
  auto outside =
      lazy_chart_detail::single_site_outside_chart_from_lazy(grammar,
                                                             lazy_chart,
                                                             pattern_index);
  return build_single_site_fluidity_report_from_rows(grammar, chart, outside);
}

inline fluidity_report build_single_site_fluidity_report(
    clade_grammar const& grammar, single_site_chart const& chart,
    single_site_outside_chart const& outside) {
  if (first_multifurcating_production(grammar)) {
    return build_single_site_fluidity_report_from_rows(grammar, chart, outside);
  }
  return build_single_site_fluidity_report_from_choice_layer(grammar, chart,
                                                            outside);
}

inline fluidity_report build_single_site_fluidity_report(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options = {}) {
  auto outside = build_single_site_outside_chart(grammar, chart, options);
  return build_single_site_fluidity_report(grammar, chart, outside);
}

inline fluidity_report build_single_site_fluidity_report(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, std::uint8_t reference_state) {
  auto outside =
      build_single_site_outside_chart(grammar, chart, options, reference_state);
  return build_single_site_fluidity_report(grammar, chart, outside);
}

inline fluidity_report build_single_site_fluidity_report(
    clade_grammar const& grammar, single_site_chart const& chart,
    chart_options const& options, phylo_dag& dag, mutation_position pos) {
  auto outside = build_single_site_outside_chart(grammar, chart, options, dag,
                                                pos);
  return build_single_site_fluidity_report(grammar, chart, outside);
}

inline fluidity_report build_single_site_fluidity_report(
    clade_grammar const& grammar, leaf_site_states const& states,
    chart_options options = {}) {
  options.keep_trace = !first_multifurcating_production(grammar).has_value();
  auto chart = build_single_site_chart(grammar, states, options);
  auto outside = build_single_site_outside_chart(grammar, chart, options);
  return build_single_site_fluidity_report(grammar, chart, outside);
}

inline fluidity_report build_single_site_fluidity_report(
    clade_grammar const& grammar, leaf_site_states const& states,
    chart_options options, std::uint8_t reference_state) {
  options.keep_trace = !first_multifurcating_production(grammar).has_value();
  auto chart = build_single_site_chart(grammar, states, options);
  auto outside =
      build_single_site_outside_chart(grammar, chart, options, reference_state);
  return build_single_site_fluidity_report(grammar, chart, outside);
}

inline plateau_graph build_plateau_graph(clade_grammar const& grammar,
                                         fluidity_report const& report) {
  if (report.externally_fluid_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "plateau: fluidity report clade count does not match grammar");
  }
  if (report.globally_optimal_clade_state.size() != grammar.clades.size() ||
      report.parent_compatibilities.size() != grammar.clades.size()) {
    throw std::runtime_error("plateau: incomplete fluidity report");
  }
  if (report.optimal_groups.size() !=
      report.optimal_group_descriptors.size()) {
    throw std::runtime_error(
        "plateau: optimal group vector/descriptor size mismatch");
  }

  plateau_graph graph;
  auto no_node = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> node_by_group(
      report.optimal_group_descriptors.size(), no_node);
  std::vector<std::array<std::vector<std::size_t>, nuc_state_count>>
      group_nodes_by_clade_state(grammar.clades.size());

  for (std::size_t group_index = 0;
       group_index < report.optimal_group_descriptors.size(); ++group_index) {
    auto const& group = report.optimal_group_descriptors[group_index];
    if (!group.externally_fluid) continue;
    if (group.clade == no_clade || group.clade >= grammar.clades.size()) {
      throw std::runtime_error("plateau: optimal group clade out of range");
    }
    parsimony_chart_detail::validate_state(group.state,
                                           "plateau graph group state");

    node_by_group[group_index] = graph.nodes.size();
    plateau_graph_node node;
    node.group_index = group_index;
    node.clade = group.clade;
    node.globally_optimal_states.push_back(group.state);
    node.optimal_group_indices.push_back(group_index);
    graph.nodes.push_back(std::move(node));
    group_nodes_by_clade_state[group.clade][group.state].push_back(
        node_by_group[group_index]);
  }

  std::map<std::pair<std::size_t, std::size_t>, std::uint64_t> edge_counts;
  for (std::size_t group_index = 0;
       group_index < report.optimal_group_descriptors.size(); ++group_index) {
    auto child_node = node_by_group[group_index];
    if (child_node == no_node) continue;
    auto const& group = report.optimal_group_descriptors[group_index];

    for (auto const& compatibility : group.parent_compatibilities) {
      if (compatibility.root_boundary || compatibility.parent == no_clade)
        continue;
      if (compatibility.parent >= group_nodes_by_clade_state.size()) {
        throw std::runtime_error(
            "plateau: parent compatibility clade out of range");
      }
      parsimony_chart_detail::validate_state(
          compatibility.parent_state, "plateau graph parent state");
      for (auto parent_node :
           group_nodes_by_clade_state[compatibility.parent]
                                     [compatibility.parent_state]) {
        ++edge_counts[{parent_node, child_node}];
      }
    }
  }

  for (auto const& [endpoints, count] : edge_counts) {
    graph.edges.push_back(plateau_graph_edge{.from = endpoints.first,
                                             .to = endpoints.second,
                                             .compatibility_count = count});
  }

  std::vector<std::vector<std::size_t>> adjacency(graph.nodes.size());
  for (auto const& edge : graph.edges) {
    if (edge.from >= graph.nodes.size() || edge.to >= graph.nodes.size()) {
      throw std::runtime_error("plateau: graph edge endpoint out of range");
    }
    adjacency[edge.from].push_back(edge.to);
    adjacency[edge.to].push_back(edge.from);
  }
  for (auto& row : adjacency) {
    std::sort(row.begin(), row.end());
    row.erase(std::unique(row.begin(), row.end()), row.end());
  }

  std::vector<bool> seen(graph.nodes.size(), false);
  for (std::size_t node = 0; node < graph.nodes.size(); ++node) {
    if (seen[node]) continue;
    graph.connected_components.push_back(
        plateau_detail::connected_component_from(node, adjacency, seen));
  }
  return graph;
}

inline multisite_plateau_report build_multisite_plateau_report(
    clade_grammar const& grammar, site_pattern_set const&,
    chart_options const& = {}) {
  parsimony_chart_detail::require_no_multifurcating_productions_for_consumer(
      grammar, arity_gate_consumer::multisite_plateau_report,
      "multi-site plateau report", "choice layer",
      "expand polytomies before using the binary plateau consumer");
  throw std::runtime_error(
      "plateau: exact multi-site plateau detection requires coupled Phase-5 "
      "optimal topology frontiers/masks; independent per-site fluidity is only "
      "a diagnostic");
}

inline std::ostream& print_fluidity_report(std::ostream& out,
                                           clade_grammar const& grammar,
                                           fluidity_report const& report,
                                           std::size_t max_clades = 25) {
  if (report.fluid_clade_state.size() != grammar.clades.size() ||
      report.locally_fluid_clade_state.size() != grammar.clades.size() ||
      report.globally_optimal_clade_state.size() != grammar.clades.size() ||
      report.globally_fluid_clade_state.size() != grammar.clades.size() ||
      report.externally_fluid_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "plateau: fluidity report clade count does not match grammar");
  }

  out << "fluidity:\n";
  out << "  global_min: ";
  if (report.global_min >= chart_inf)
    out << "INF\n";
  else
    out << report.global_min << "\n";
  out << "  clades: " << grammar.clades.size() << "\n";
  out << "  local_fluid_clade_states: "
      << report.locally_fluid_clade_state_count << "\n";
  out << "  fluid_clade_states: " << report.fluid_clade_state_count << "\n";
  out << "  globally_fluid_clade_states: "
      << report.globally_fluid_clade_state_count << "\n";
  out << "  externally_fluid_clades: "
      << report.externally_fluid_clade_count << "\n";
  out << "  externally_fluid_groups: "
      << report.externally_fluid_group_count << "\n";
  out << "  optimal_groups: " << report.optimal_groups.size() << "\n";
  out << "  chart_row_fluidity_runs: "
      << report.chart_row_fluidity_runs << "\n";

  std::size_t printed = 0;
  for (clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    bool interesting = report.externally_fluid_clade[clade];
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      interesting = interesting ||
                    report.locally_fluid_clade_state[clade][state] ||
                    report.fluid_clade_state[clade][state] ||
                    report.globally_fluid_clade_state[clade][state];
    }
    if (!interesting) continue;
    if (printed == 0) out << "  interesting_clades:\n";
    if (printed >= max_clades) {
      out << "    ...\n";
      break;
    }
    ++printed;
    out << "    - clade: " << clade << "\n";
    out << "      taxa: " << plateau_detail::clade_taxa_label(grammar, clade)
        << "\n";
    out << "      local_fluid_states: "
        << plateau_detail::state_list(
               report.locally_fluid_clade_state[clade])
        << "\n";
    out << "      fluid_states: "
        << plateau_detail::state_list(report.fluid_clade_state[clade])
        << "\n";
    out << "      globally_optimal_states: "
        << plateau_detail::state_list(
               report.globally_optimal_clade_state[clade])
        << "\n";
    out << "      globally_fluid_states: "
        << plateau_detail::state_list(report.globally_fluid_clade_state[clade])
        << "\n";
    out << "      externally_fluid: "
        << (report.externally_fluid_clade[clade] ? "true" : "false")
        << "\n";
  }
  return out;
}

}  // namespace larch

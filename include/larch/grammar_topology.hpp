#pragma once

#include <larch/clade_grammar.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace larch {

// Neutral concrete topology over a clade_grammar.  This intentionally lives
// outside rank3_rewrite.hpp so chart/trim code can emit compatible topology
// witnesses without depending on rewrite/materialization APIs.
struct grammar_topology {
  // selected_production_by_clade[c] is the production chosen for non-leaf clade
  // c in the concrete topology. Leaf clades and unreachable clades contain
  // no_production.
  std::vector<production_id> selected_production_by_clade;
  std::vector<bool> used_production;
};

inline grammar_topology make_empty_grammar_topology(
    clade_grammar const& grammar) {
  grammar_topology topology;
  topology.selected_production_by_clade.assign(grammar.clades.size(),
                                               no_production);
  topology.used_production.assign(grammar.productions.size(), false);
  return topology;
}

namespace grammar_topology_detail {

inline void validate_basic_grammar_indices(clade_grammar const& grammar) {
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error("grammar topology: root clade out of range");
  }
  if (grammar.productions_by_parent.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "grammar topology: productions_by_parent size mismatch");
  }
  if (grammar.productions_by_child.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "grammar topology: productions_by_child size mismatch");
  }
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& prod = grammar.productions[pid];
    if (prod.parent == no_clade || prod.parent >= grammar.clades.size()) {
      throw std::runtime_error("grammar topology: production parent out of range");
    }
    auto const& by_parent = grammar.productions_by_parent[prod.parent];
    if (std::find(by_parent.begin(), by_parent.end(),
                  static_cast<production_id>(pid)) == by_parent.end()) {
      throw std::runtime_error(
          "grammar topology: production missing from productions_by_parent");
    }
    for (auto child : prod.children) {
      if (child == no_clade || child >= grammar.clades.size()) {
        throw std::runtime_error("grammar topology: production child out of range");
      }
    }
  }
}

inline void collect_reachable_productions(
    clade_grammar const& grammar, grammar_topology const& topology,
    clade_id clade, std::vector<std::uint8_t>& clade_state,
    std::vector<bool>& reachable_production) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("grammar topology: clade out of range");
  }
  if (clade_state[clade] == 1) {
    throw std::runtime_error("grammar topology: cycle in selected topology");
  }
  if (clade_state[clade] == 2) {
    throw std::runtime_error(
        "grammar topology: selected topology reuses a clade in two places; "
        "expected a concrete tree topology");
  }
  clade_state[clade] = 1;

  if (grammar.clades[clade].taxa.size() == 1) {
    if (topology.selected_production_by_clade[clade] != no_production) {
      throw std::runtime_error(
          "grammar topology: singleton clade has a selected production");
    }
    clade_state[clade] = 2;
    return;
  }

  auto pid = topology.selected_production_by_clade[clade];
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error(
        "grammar topology: selected topology is incomplete at clade " +
        std::to_string(clade));
  }
  auto const& prod = grammar.productions[pid];
  if (prod.parent != clade) {
    throw std::runtime_error(
        "grammar topology: selected production parent mismatch at clade " +
        std::to_string(clade));
  }
  reachable_production[pid] = true;
  for (auto child : prod.children) {
    collect_reachable_productions(grammar, topology, child, clade_state,
                                  reachable_production);
  }
  clade_state[clade] = 2;
}

}  // namespace grammar_topology_detail

inline std::vector<bool> validate_grammar_topology(
    clade_grammar const& grammar, grammar_topology const& topology) {
  grammar_topology_detail::validate_basic_grammar_indices(grammar);
  if (topology.selected_production_by_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "grammar topology: clade-choice vector has wrong size");
  }
  if (topology.used_production.size() != grammar.productions.size()) {
    throw std::runtime_error(
        "grammar topology: production-use vector has wrong size");
  }

  std::vector<std::uint8_t> clade_state(grammar.clades.size(), 0);
  std::vector<bool> reachable_production(grammar.productions.size(), false);
  grammar_topology_detail::collect_reachable_productions(
      grammar, topology, grammar.root_clade, clade_state, reachable_production);

  for (std::size_t clade = 0; clade < clade_state.size(); ++clade) {
    if (clade_state[clade] == 0 &&
        topology.selected_production_by_clade[clade] != no_production) {
      throw std::runtime_error(
          "grammar topology: unreachable clade " + std::to_string(clade) +
          " has a selected production");
    }
  }

  for (std::size_t pid = 0; pid < reachable_production.size(); ++pid) {
    if (topology.used_production[pid] != reachable_production[pid]) {
      throw std::runtime_error(
          "grammar topology: used_production does not match reachable "
          "selected productions at production " +
          std::to_string(pid));
    }
  }
  return reachable_production;
}

inline grammar_topology grammar_topology_from_productions(
    clade_grammar const& grammar,
    std::vector<production_id> const& productions) {
  grammar_topology_detail::validate_basic_grammar_indices(grammar);
  auto topology = make_empty_grammar_topology(grammar);
  for (auto pid : productions) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "grammar topology: production id out of range");
    }
    auto parent = grammar.productions[pid].parent;
    if (parent == no_clade || parent >= grammar.clades.size()) {
      throw std::runtime_error(
          "grammar topology: production parent out of range");
    }
    auto& selected = topology.selected_production_by_clade[parent];
    if (selected != no_production && selected != pid) {
      throw std::runtime_error(
          "grammar topology: conflicting production choices for clade " +
          std::to_string(parent));
    }
    selected = pid;
    topology.used_production[pid] = true;
  }
  return topology;
}

inline bool grammar_topology_less(grammar_topology const& lhs,
                                  grammar_topology const& rhs) {
  if (lhs.selected_production_by_clade != rhs.selected_production_by_clade) {
    return lhs.selected_production_by_clade < rhs.selected_production_by_clade;
  }
  return lhs.used_production < rhs.used_production;
}

inline bool grammar_topology_equal(grammar_topology const& lhs,
                                   grammar_topology const& rhs) {
  return lhs.selected_production_by_clade == rhs.selected_production_by_clade &&
         lhs.used_production == rhs.used_production;
}

}  // namespace larch

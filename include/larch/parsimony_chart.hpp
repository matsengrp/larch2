#pragma once

#include <larch/clade_grammar.hpp>
#include <larch/compute.hpp>
#include <larch/compact_genome.hpp>
#include <larch/phylo_dag.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larch {

using chart_cost = std::uint32_t;
inline constexpr chart_cost chart_inf =
    std::numeric_limits<chart_cost>::max() / 4;
inline constexpr std::size_t nuc_state_count = 4;

struct chart_choice {
  production_id production = no_production;
  // Phase 2 supports binary productions only. The entry is aligned with
  // grammar_production::children for the chosen production.
  std::array<std::uint8_t, 2> child_states{0, 0};
  chart_cost cost = chart_inf;
};

struct single_site_chart {
  std::vector<std::array<chart_cost, nuc_state_count>> inside;

  // Empty when build_single_site_chart(..., keep_trace=false). When present,
  // optimal_choices[clade][state] stores every binary production / child-state
  // pair that ties for inside[clade][state].
  std::vector<std::array<std::vector<chart_choice>, nuc_state_count>>
      optimal_choices;

  // Number of stored chart_choice records. Zero on the no-trace fast path.
  std::size_t trace_choice_count = 0;

  [[nodiscard]] bool has_trace() const { return !optimal_choices.empty(); }

  [[nodiscard]] chart_cost root_min_excluding_ua(clade_id root) const {
    if (root == no_clade || root >= inside.size())
      throw std::runtime_error("single-site chart: root clade out of range");

    chart_cost best = chart_inf;
    for (auto cost : inside[root]) best = std::min(best, cost);
    return best;
  }

  [[nodiscard]] chart_cost root_min_with_reference_edge(
      clade_id root, std::uint8_t reference_state) const;
};

struct leaf_site_states {
  // taxon_id -> 0..3 (A,C,G,T)
  std::vector<std::uint8_t> state_by_taxon;
};

struct chart_options {
  bool keep_trace = false;
  // Interpreted by callers when selecting the root convention; chart
  // construction itself always computes the UA-free inside recurrence.
  bool score_ua_edge = false;
  // 0 means unlimited. If non-zero and keep_trace=true, construction throws
  // before exceeding this many stored chart_choice records.
  std::size_t max_trace_choices = 0;
};

namespace parsimony_chart_detail {

inline std::uint8_t strict_decode_acgt_state(char c) {
  switch (c) {
    case 'A':
    case 'a':
      return nuc_base::A;
    case 'C':
    case 'c':
      return nuc_base::C;
    case 'G':
    case 'g':
      return nuc_base::G;
    case 'T':
    case 't':
      return nuc_base::T;
    default:
      throw std::runtime_error(std::string{"single-site chart: non-ACGT "
                                           "reference nucleotide '"} +
                               c + "'");
  }
}

inline std::uint8_t strict_decode_acgt_state(nuc_base base) {
  if (base.raw() > nuc_base::T)
    throw std::runtime_error("single-site chart: invalid nuc_base raw value " +
                             std::to_string(base.raw()));
  return base.raw();
}

inline void validate_state(std::uint8_t state, std::string_view label) {
  if (state >= nuc_state_count)
    throw std::runtime_error("single-site chart: invalid nucleotide state " +
                             std::to_string(state) + " for " +
                             std::string{label});
}

inline chart_cost transition_cost(std::uint8_t parent_state,
                                  std::uint8_t child_state) {
  validate_state(parent_state, "parent");
  validate_state(child_state, "child");
  return parent_state == child_state ? chart_cost{0} : chart_cost{1};
}

inline chart_cost saturated_add(chart_cost lhs, chart_cost rhs) {
  if (lhs >= chart_inf || rhs >= chart_inf) return chart_inf;
  if (lhs > chart_inf - rhs) return chart_inf;
  return lhs + rhs;
}

inline std::uint8_t strict_reference_state_at(std::string_view reference,
                                              mutation_position pos) {
  if (pos == 0 || pos > reference.size()) {
    throw std::runtime_error(
        "single-site chart: mutation position " + std::to_string(pos) +
        " outside reference length " + std::to_string(reference.size()));
  }

  std::uint8_t result = nuc_base::A;
  for (std::size_t i = 0; i < reference.size(); ++i) {
    auto state = strict_decode_acgt_state(reference[i]);
    if (i + 1 == pos) result = state;
  }
  return result;
}

inline std::uint8_t strict_compact_genome_state_at(compact_genome const& cg,
                                                   std::string_view reference,
                                                   mutation_position pos,
                                                   std::uint8_t reference_state,
                                                   std::string_view sample_id) {
  auto observed = reference_state;
  for (auto const& [mut_pos, base] : cg) {
    if (mut_pos == 0 || mut_pos > reference.size()) {
      throw std::runtime_error(
          "single-site chart: compact-genome mutation position " +
          std::to_string(mut_pos) + " for sample '" + std::string{sample_id} +
          "' outside reference length " + std::to_string(reference.size()));
    }
    auto state = strict_decode_acgt_state(base);
    if (mut_pos == pos) observed = state;
  }
  return observed;
}

inline std::array<chart_cost, nuc_state_count> make_inf_row() {
  std::array<chart_cost, nuc_state_count> row{};
  row.fill(chart_inf);
  return row;
}

inline void validate_binary_production_partition(clade_grammar const& grammar,
                                                 grammar_production const& prod,
                                                 production_id pid) {
  if (prod.parent == no_clade || prod.parent >= grammar.clades.size())
    throw std::runtime_error(
        "single-site chart: production parent clade out of range");

  auto const& parent_taxa = grammar.clades[prod.parent].taxa;
  std::vector<taxon_id> covered;
  for (auto child : prod.children) {
    if (child == no_clade || child >= grammar.clades.size())
      throw std::runtime_error(
          "single-site chart: production child clade out of range");

    auto const& child_taxa = grammar.clades[child].taxa;
    if (!std::includes(parent_taxa.begin(), parent_taxa.end(),
                       child_taxa.begin(), child_taxa.end())) {
      throw std::runtime_error(
          "single-site chart: production " + std::to_string(pid) +
          " child clade is not a subset of its parent clade");
    }

    std::vector<taxon_id> overlap;
    std::set_intersection(covered.begin(), covered.end(), child_taxa.begin(),
                          child_taxa.end(), std::back_inserter(overlap));
    if (!overlap.empty()) {
      throw std::runtime_error("single-site chart: production " +
                               std::to_string(pid) +
                               " children are not pairwise disjoint");
    }

    std::vector<taxon_id> next;
    std::set_union(covered.begin(), covered.end(), child_taxa.begin(),
                   child_taxa.end(), std::back_inserter(next));
    covered = std::move(next);
  }

  if (covered != parent_taxa) {
    throw std::runtime_error("single-site chart: production " +
                             std::to_string(pid) +
                             " children do not union to the parent clade");
  }
}

inline void validate_chart_grammar(clade_grammar const& grammar) {
  if (grammar.clades.size() >= static_cast<std::size_t>(no_clade))
    throw std::runtime_error("single-site chart: too many clades");
  if (grammar.productions.size() >= static_cast<std::size_t>(no_production))
    throw std::runtime_error("single-site chart: too many productions");
  if (grammar.productions_by_parent.size() != grammar.clades.size())
    throw std::runtime_error(
        "single-site chart: productions_by_parent size mismatch");
  if (grammar.productions_by_child.size() != grammar.clades.size())
    throw std::runtime_error(
        "single-site chart: productions_by_child size mismatch");

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto const& taxa = grammar.clades[cid].taxa;
    if (taxa.empty())
      throw std::runtime_error("single-site chart: empty clade is invalid");
    if (!std::is_sorted(taxa.begin(), taxa.end()) ||
        std::adjacent_find(taxa.begin(), taxa.end()) != taxa.end()) {
      throw std::runtime_error(
          "single-site chart: clade taxa must be sorted and unique");
    }
  }
}

}  // namespace parsimony_chart_detail

inline chart_cost single_site_chart::root_min_with_reference_edge(
    clade_id root, std::uint8_t reference_state) const {
  parsimony_chart_detail::validate_state(reference_state, "reference");
  if (root == no_clade || root >= inside.size())
    throw std::runtime_error("single-site chart: root clade out of range");

  chart_cost best = chart_inf;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    auto candidate = parsimony_chart_detail::saturated_add(
        inside[root][state],
        parsimony_chart_detail::transition_cost(reference_state, state));
    best = std::min(best, candidate);
  }
  return best;
}

inline std::uint8_t extract_reference_site_state(phylo_dag& dag,
                                                 mutation_position pos) {
  return parsimony_chart_detail::strict_reference_state_at(
      get_reference_sequence(dag), pos);
}

inline chart_cost root_min(single_site_chart const& chart, clade_id root,
                           chart_options const& options) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "single-site chart: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  return chart.root_min_excluding_ua(root);
}

inline chart_cost root_min(single_site_chart const& chart, clade_id root,
                           chart_options const& options,
                           std::uint8_t reference_state) {
  if (options.score_ua_edge)
    return chart.root_min_with_reference_edge(root, reference_state);
  return chart.root_min_excluding_ua(root);
}

inline chart_cost root_min(single_site_chart const& chart, clade_id root,
                           chart_options const& options, phylo_dag& dag,
                           mutation_position pos) {
  if (options.score_ua_edge) {
    return chart.root_min_with_reference_edge(
        root, extract_reference_site_state(dag, pos));
  }
  return chart.root_min_excluding_ua(root);
}

inline leaf_site_states extract_leaf_site_states(phylo_dag& dag,
                                                 clade_grammar const& grammar,
                                                 mutation_position pos) {
  auto const& reference = get_reference_sequence(dag);
  auto reference_state =
      parsimony_chart_detail::strict_reference_state_at(reference, pos);

  leaf_site_states result;
  result.state_by_taxon.assign(grammar.taxa.id_to_sample_id.size(),
                               static_cast<std::uint8_t>(nuc_state_count));
  std::vector<bool> seen(grammar.taxa.id_to_sample_id.size(), false);

  auto reachable = detail::collect_reachable(dag);
  for (auto node_idx : reachable.nodes) {
    auto nv = dag.get_node(node_idx);
    if (!detail::is_leaf_node(nv)) continue;

    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            std::string sample_id{node.sample_id()};
            auto it = grammar.taxa.sample_id_to_id.find(sample_id);
            if (it == grammar.taxa.sample_id_to_id.end()) {
              throw std::runtime_error(
                  "single-site chart: reachable leaf sample_id '" + sample_id +
                  "' is not present in the supplied clade grammar");
            }
            auto tid = it->second;
            if (tid >= result.state_by_taxon.size())
              throw std::runtime_error(
                  "single-site chart: taxon id out of range for sample '" +
                  sample_id + "'");

            auto observed =
                parsimony_chart_detail::strict_compact_genome_state_at(
                    node.cg(), reference, pos, reference_state, sample_id);
            if (seen[tid] && result.state_by_taxon[tid] != observed) {
              throw std::runtime_error(
                  "single-site chart: duplicate sample_id '" + sample_id +
                  "' has conflicting observed states at position " +
                  std::to_string(pos));
            }
            result.state_by_taxon[tid] = observed;
            seen[tid] = true;
          } else {
            throw std::runtime_error(
                "single-site chart: expected leaf node annotation");
          }
        },
        nv);
  }

  for (std::size_t tid = 0; tid < seen.size(); ++tid) {
    if (!seen[tid]) {
      throw std::runtime_error(
          "single-site chart: missing observed leaf state "
          "for taxon '" +
          grammar.taxa.id_to_sample_id[tid] + "'");
    }
  }
  return result;
}

inline single_site_chart build_single_site_chart(
    clade_grammar const& grammar, leaf_site_states const& leaf_states,
    chart_options const& options) {
  using namespace parsimony_chart_detail;

  validate_chart_grammar(grammar);
  if (leaf_states.state_by_taxon.size() !=
      grammar.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(
        "single-site chart: leaf state count does not match taxon count");
  }
  for (std::size_t tid = 0; tid < leaf_states.state_by_taxon.size(); ++tid)
    validate_state(leaf_states.state_by_taxon[tid],
                   "taxon " + std::to_string(tid));

  single_site_chart chart;
  chart.inside.assign(grammar.clades.size(), make_inf_row());
  if (options.keep_trace) chart.optimal_choices.resize(grammar.clades.size());

  std::vector<clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    auto const& ltaxa = grammar.clades[lhs].taxa;
    auto const& rtaxa = grammar.clades[rhs].taxa;
    if (ltaxa.size() != rtaxa.size()) return ltaxa.size() < rtaxa.size();
    return lhs < rhs;
  });

  auto append_choice = [&](clade_id cid, std::uint8_t state,
                           chart_choice choice) {
    if (!options.keep_trace) return;
    if (options.max_trace_choices != 0 &&
        chart.trace_choice_count >= options.max_trace_choices) {
      throw std::runtime_error(
          "single-site chart: trace choice cap exceeded (cap=" +
          std::to_string(options.max_trace_choices) + ")");
    }
    chart.optimal_choices[cid][state].push_back(choice);
    ++chart.trace_choice_count;
  };

  auto clear_choices = [&](clade_id cid, std::uint8_t state) {
    if (!options.keep_trace) return;
    auto& choices = chart.optimal_choices[cid][state];
    chart.trace_choice_count -= choices.size();
    choices.clear();
  };

  for (auto cid : order) {
    if (cid >= grammar.clades.size())
      throw std::runtime_error("single-site chart: clade id out of range");

    auto const& clade = grammar.clades[cid];
    if (clade.taxa.empty())
      throw std::runtime_error("single-site chart: empty clade is invalid");

    if (clade.taxa.size() == 1) {
      if (!grammar.productions_by_parent[cid].empty()) {
        throw std::runtime_error(
            "single-site chart: singleton leaf clade has productions");
      }
      auto taxon = clade.taxa.front();
      if (taxon >= leaf_states.state_by_taxon.size())
        throw std::runtime_error(
            "single-site chart: singleton taxon id out of range");
      auto observed = leaf_states.state_by_taxon[taxon];
      for (std::uint8_t state = 0; state < nuc_state_count; ++state)
        chart.inside[cid][state] =
            (state == observed) ? chart_cost{0} : chart_inf;
      continue;
    }

    auto const& parent_productions = grammar.productions_by_parent[cid];
    if (parent_productions.empty()) {
      throw std::runtime_error(
          "single-site chart: non-singleton clade has no productions");
    }

    for (auto pid : parent_productions) {
      if (pid == no_production || pid >= grammar.productions.size())
        throw std::runtime_error(
            "single-site chart: production id out of range");
      auto const& prod = grammar.productions[pid];
      if (prod.parent != cid)
        throw std::runtime_error(
            "single-site chart: productions_by_parent contains production "
            "with mismatched parent");
      if (prod.children.size() != 2) {
        throw std::runtime_error("single-site chart: production " +
                                 std::to_string(pid) + " has arity " +
                                 std::to_string(prod.children.size()) +
                                 "; Phase 2 supports binary productions only");
      }

      std::array<clade_id, 2> children{prod.children[0], prod.children[1]};
      for (auto child : children) {
        if (child == no_clade || child >= grammar.clades.size())
          throw std::runtime_error(
              "single-site chart: production child clade out of range");
        if (grammar.clades[child].taxa.size() >= clade.taxa.size())
          throw std::runtime_error(
              "single-site chart: production child is not smaller than parent");
      }
      validate_binary_production_partition(grammar, prod, pid);

      for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
           ++parent_state) {
        std::array<chart_cost, 2> best_child_cost{chart_inf, chart_inf};
        std::array<std::vector<std::uint8_t>, 2> best_child_states;

        for (std::size_t child_i = 0; child_i < children.size(); ++child_i) {
          auto child = children[child_i];
          for (std::uint8_t child_state = 0; child_state < nuc_state_count;
               ++child_state) {
            auto candidate =
                saturated_add(chart.inside[child][child_state],
                              transition_cost(parent_state, child_state));
            if (candidate < best_child_cost[child_i]) {
              best_child_cost[child_i] = candidate;
              if (options.keep_trace) {
                best_child_states[child_i].clear();
                if (candidate < chart_inf)
                  best_child_states[child_i].push_back(child_state);
              }
            } else if (options.keep_trace &&
                       candidate == best_child_cost[child_i] &&
                       candidate < chart_inf) {
              best_child_states[child_i].push_back(child_state);
            }
          }
        }

        auto candidate = saturated_add(best_child_cost[0], best_child_cost[1]);
        if (candidate >= chart_inf) continue;

        auto& cell = chart.inside[cid][parent_state];
        if (candidate < cell) {
          cell = candidate;
          clear_choices(cid, parent_state);
        }
        if (options.keep_trace && candidate == cell) {
          for (auto left_state : best_child_states[0]) {
            for (auto right_state : best_child_states[1]) {
              append_choice(
                  cid, parent_state,
                  chart_choice{pid, {left_state, right_state}, candidate});
            }
          }
        }
      }
    }
  }

  return chart;
}

inline single_site_chart build_single_site_chart(
    clade_grammar const& grammar, leaf_site_states const& leaf_states,
    bool keep_trace = false) {
  chart_options options;
  options.keep_trace = keep_trace;
  return build_single_site_chart(grammar, leaf_states, options);
}

}  // namespace larch

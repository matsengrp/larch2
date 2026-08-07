#include <larch/chart_spr.hpp>
#include <larch/chart_trim.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/grammar_topology_census.hpp>
#include <larch/grammar_topology_enumerator.hpp>
#include <larch/lazy_chart.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/plateau.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <print>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr)                                    \
  do {                                                 \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

using brute_row = std::array<larch::chart_cost, larch::nuc_state_count>;

static std::string runtime_error_message(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const& e) {
    return e.what();
  }
  return {};
}

static void check_arity_gate_message(std::string const& message,
                                     std::string const& layer) {
  CHECK(message.find("WI6 arity gate") != std::string::npos);
  CHECK(message.find("the chart supports multifurcations") !=
        std::string::npos);
  CHECK(message.find(layer) != std::string::npos);
  CHECK(message.find("grammar max arity 3") != std::string::npos);
}

static larch::chart_cost brute_add(larch::chart_cost lhs,
                                   larch::chart_cost rhs) {
  if (lhs >= larch::chart_inf || rhs >= larch::chart_inf)
    return larch::chart_inf;
  if (lhs > larch::chart_inf - rhs) return larch::chart_inf;
  return lhs + rhs;
}

static brute_row brute_inf_row() {
  brute_row row{};
  row.fill(larch::chart_inf);
  return row;
}

static brute_row brute_leaf_row(std::uint8_t observed) {
  auto row = brute_inf_row();
  row[observed] = 0;
  return row;
}

static brute_row brute_combine_multifurcation(
    std::vector<brute_row> const& children) {
  CHECK(!children.empty());
  auto row = brute_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
       ++parent_state) {
    larch::chart_cost total = 0;
    for (auto const& child : children) {
      larch::chart_cost best_child = larch::chart_inf;
      for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
           ++child_state) {
        auto edge_cost = parent_state == child_state ? larch::chart_cost{0}
                                                     : larch::chart_cost{1};
        best_child = std::min(best_child,
                              brute_add(child[child_state], edge_cost));
      }
      total = brute_add(total, best_child);
    }
    row[parent_state] = total;
  }
  return row;
}

static std::vector<brute_row> brute_enumerate_multifurcating_topologies(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states,
    larch::clade_id clade,
    std::vector<std::optional<std::vector<brute_row>>>& memo) {
  if (memo[clade].has_value()) return *memo[clade];

  std::vector<brute_row> rows;
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    rows.push_back(brute_leaf_row(states.state_by_taxon[key.taxa.front()]));
    memo[clade] = rows;
    return rows;
  }

  for (auto pid : grammar.productions_by_parent[clade]) {
    auto const& prod = grammar.productions[pid];
    CHECK(prod.children.size() >= 2);

    std::vector<std::vector<brute_row>> child_rows_by_child;
    child_rows_by_child.reserve(prod.children.size());
    for (auto child : prod.children) {
      child_rows_by_child.push_back(
          brute_enumerate_multifurcating_topologies(grammar, states, child,
                                                    memo));
      CHECK(!child_rows_by_child.back().empty());
    }

    std::vector<brute_row> selected(prod.children.size(), brute_inf_row());
    auto enumerate_product = [&](auto&& self, std::size_t child_i) -> void {
      if (child_i == child_rows_by_child.size()) {
        rows.push_back(brute_combine_multifurcation(selected));
        CHECK(rows.size() < 10000);
        return;
      }
      for (auto const& child_row : child_rows_by_child[child_i]) {
        selected[child_i] = child_row;
        self(self, child_i + 1);
      }
    };
    enumerate_product(enumerate_product, 0);
  }

  memo[clade] = rows;
  return rows;
}

static std::vector<brute_row> brute_root_rows(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states) {
  std::vector<std::optional<std::vector<brute_row>>> memo(grammar.clades.size());
  return brute_enumerate_multifurcating_topologies(grammar, states,
                                                   grammar.root_clade, memo);
}

static larch::chart_cost row_min(brute_row const& row) {
  larch::chart_cost best = larch::chart_inf;
  for (auto cost : row) best = std::min(best, cost);
  return best;
}

static brute_row brute_min_row(std::vector<brute_row> const& rows) {
  CHECK(!rows.empty());
  auto result = brute_inf_row();
  for (auto const& row : rows) {
    for (std::uint8_t state = 0; state < larch::nuc_state_count; ++state)
      result[state] = std::min(result[state], row[state]);
  }
  return result;
}

static std::size_t max_production_arity(larch::clade_grammar const& grammar) {
  std::size_t max_arity = 0;
  for (auto const& prod : grammar.productions)
    max_arity = std::max(max_arity, prod.children.size());
  return max_arity;
}

static std::vector<larch::clade_id> clades_by_increasing_size(
    larch::clade_grammar const& grammar) {
  std::vector<larch::clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), larch::clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](auto lhs, auto rhs) {
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize < rsize;
    return lhs < rhs;
  });
  return order;
}

static std::vector<larch::clade_id> clades_by_decreasing_size(
    larch::clade_grammar const& grammar) {
  auto order = clades_by_increasing_size(grammar);
  std::reverse(order.begin(), order.end());
  return order;
}

static std::vector<brute_row> brute_dense_outside(
    larch::clade_grammar const& grammar, larch::single_site_chart const& chart,
    larch::chart_options const& options, std::uint8_t reference_state) {
  std::vector<brute_row> outside(grammar.clades.size(), brute_inf_row());
  auto root = grammar.root_clade;
  for (std::uint8_t state = 0; state < larch::nuc_state_count; ++state) {
    outside[root][state] =
        options.score_ua_edge
            ? larch::parsimony_chart_detail::transition_cost(reference_state,
                                                             state)
            : larch::chart_cost{0};
  }

  for (auto parent : clades_by_decreasing_size(grammar)) {
    for (auto pid : grammar.productions_by_parent[parent]) {
      auto const& prod = grammar.productions[pid];
      CHECK(prod.children.size() >= 2);
      for (std::uint8_t parent_state = 0;
           parent_state < larch::nuc_state_count; ++parent_state) {
        auto base = outside[parent][parent_state];
        if (base >= larch::chart_inf) continue;

        for (std::size_t child_i = 0; child_i < prod.children.size();
             ++child_i) {
          auto child = prod.children[child_i];
          for (std::uint8_t child_state = 0;
               child_state < larch::nuc_state_count; ++child_state) {
            larch::chart_cost total = base;
            for (std::size_t sibling_i = 0;
                 sibling_i < prod.children.size(); ++sibling_i) {
              if (sibling_i == child_i) continue;
              auto sibling = prod.children[sibling_i];
              larch::chart_cost sibling_best = larch::chart_inf;
              for (std::uint8_t sibling_state = 0;
                   sibling_state < larch::nuc_state_count; ++sibling_state) {
                sibling_best = std::min(
                    sibling_best,
                    brute_add(chart.inside[sibling][sibling_state],
                              parent_state == sibling_state ? 0 : 1));
              }
              total = brute_add(total, sibling_best);
            }
            total = brute_add(
                total, parent_state == child_state ? larch::chart_cost{0}
                                                   : larch::chart_cost{1});
            outside[child][child_state] =
                std::min(outside[child][child_state], total);
          }
        }
      }
    }
  }
  return outside;
}

static larch::single_site_chart build_and_check_dense_inside(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states) {
  auto chart = larch::build_single_site_chart(grammar, states);
  CHECK(chart.multifurcation_productions_scored > 0);
  std::vector<std::optional<std::vector<brute_row>>> memo(grammar.clades.size());
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto rows = brute_enumerate_multifurcating_topologies(
        grammar, states, static_cast<larch::clade_id>(cid), memo);
    CHECK(chart.inside[cid] == brute_min_row(rows));
  }

  larch::single_site_chart recomputed;
  recomputed.inside.assign(grammar.clades.size(),
                           larch::parsimony_chart_detail::make_inf_row());
  for (auto clade : clades_by_increasing_size(grammar)) {
    larch::chart_spr_detail::recompute_single_inside_row(
        grammar, states, recomputed, clade);
  }
  CHECK(recomputed.inside == chart.inside);
  CHECK(recomputed.multifurcation_productions_scored ==
        chart.multifurcation_productions_scored);

  larch::chart_options ua_free;
  auto outside = larch::build_single_site_outside_chart(grammar, chart, ua_free);
  CHECK(outside.multifurcation_productions_scored > 0);
  CHECK(outside.outside ==
        brute_dense_outside(grammar, chart, ua_free, larch::nuc_base::A));
  CHECK(outside.global_min == chart.root_min_excluding_ua(grammar.root_clade));

  larch::chart_options ua_edge;
  ua_edge.score_ua_edge = true;
  auto outside_ua = larch::build_single_site_outside_chart(
      grammar, chart, ua_edge, larch::nuc_base::A);
  CHECK(outside_ua.multifurcation_productions_scored > 0);
  CHECK(outside_ua.outside ==
        brute_dense_outside(grammar, chart, ua_edge, larch::nuc_base::A));
  CHECK(outside_ua.global_min ==
        chart.root_min_with_reference_edge(grammar.root_clade,
                                           larch::nuc_base::A));
  return chart;
}

static larch::clade_grammar_build_result build_allowing_polytomies(
    larch::phylo_dag& dag) {
  larch::clade_grammar_options opts;
  opts.allow_polytomies = true;
  return larch::build_clade_grammar_with_audit(dag, opts);
}

static larch::test::tiny_tree_node leaf(std::string name, char state) {
  return larch::test::tiny_leaf(std::move(name), std::string(1, state));
}

static std::uint8_t test_state(char c) {
  switch (c) {
    case 'A':
      return larch::nuc_base::A;
    case 'C':
      return larch::nuc_base::C;
    case 'G':
      return larch::nuc_base::G;
    case 'T':
      return larch::nuc_base::T;
    default:
      throw std::runtime_error("invalid test nucleotide");
  }
}

static larch::site_pattern_set make_pattern_set(
    std::vector<std::vector<std::uint8_t>> states_by_pattern) {
  larch::site_pattern_set patterns;
  if (!states_by_pattern.empty()) {
    patterns.taxon_count = states_by_pattern.front().size();
  }

  for (std::size_t i = 0; i < states_by_pattern.size(); ++i) {
    auto const& states = states_by_pattern[i];
    CHECK(states.size() == patterns.taxon_count);
    larch::site_pattern pattern;
    pattern.state_by_taxon = states;
    pattern.weight = static_cast<std::uint32_t>((i % 5) + 1);
    pattern.reference_state_counts[larch::nuc_base::A] = pattern.weight;
    for (std::uint32_t copy = 0; copy < pattern.weight; ++copy) {
      auto pos =
          static_cast<larch::mutation_position>(patterns.total_site_count + 1);
      pattern.positions.push_back(pos);
      patterns.original_site_to_pattern.push_back(i);
      ++patterns.total_site_count;
      ++patterns.variable_site_count;
    }
    patterns.patterns.push_back(std::move(pattern));
  }
  patterns.exact_pattern_to_normalized_binary_pattern.assign(
      patterns.patterns.size(), larch::no_site_pattern);
  patterns.exact_pattern_to_normalized_binary_state_map.assign(
      patterns.patterns.size(), larch::normalized_binary_state_map{});
  return patterns;
}

static larch::site_pattern_set make_pattern_set_from_strings(
    std::vector<std::string> const& rows) {
  std::vector<std::vector<std::uint8_t>> states_by_pattern;
  states_by_pattern.reserve(rows.size());
  for (auto const& row : rows) {
    std::vector<std::uint8_t> states;
    states.reserve(row.size());
    for (auto c : row) states.push_back(test_state(c));
    states_by_pattern.push_back(std::move(states));
  }
  return make_pattern_set(std::move(states_by_pattern));
}

static larch::site_pattern_set with_cycling_reference_counts(
    larch::site_pattern_set patterns) {
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto& pattern = patterns.patterns[pattern_index];
    pattern.reference_state_counts.fill(0);
    for (std::uint32_t copy = 0; copy < pattern.weight; ++copy) {
      auto state = static_cast<std::uint8_t>(
          (pattern_index + copy) % larch::nuc_state_count);
      ++pattern.reference_state_counts[state];
    }
  }
  return patterns;
}

static larch::site_pattern_set lazy_oracle_patterns(std::size_t taxon_count) {
  std::vector<std::vector<std::uint8_t>> states_by_pattern;
  auto all_a = std::vector<std::uint8_t>(taxon_count, larch::nuc_base::A);
  states_by_pattern.push_back(all_a);

  for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
    auto states = all_a;
    states[taxon] = static_cast<std::uint8_t>((taxon % 3) + 1);
    states_by_pattern.push_back(std::move(states));
  }

  std::vector<std::uint8_t> cycling(taxon_count, larch::nuc_base::A);
  for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
    cycling[taxon] = static_cast<std::uint8_t>(taxon % larch::nuc_state_count);
  }
  states_by_pattern.push_back(cycling);

  std::reverse(cycling.begin(), cycling.end());
  states_by_pattern.push_back(cycling);
  states_by_pattern.push_back(
      std::vector<std::uint8_t>(taxon_count, larch::nuc_base::C));
  states_by_pattern.push_back(
      std::vector<std::uint8_t>(taxon_count, larch::nuc_base::T));
  return make_pattern_set(std::move(states_by_pattern));
}

static larch::site_pattern_set random_lazy_oracle_patterns(
    std::size_t taxon_count, std::size_t pattern_count, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> state_dist(
      0, static_cast<int>(larch::nuc_state_count - 1));
  std::vector<std::vector<std::uint8_t>> states_by_pattern;
  states_by_pattern.reserve(pattern_count);
  for (std::size_t pattern = 0; pattern < pattern_count; ++pattern) {
    std::vector<std::uint8_t> states(taxon_count, larch::nuc_base::A);
    for (auto& state : states) {
      state = static_cast<std::uint8_t>(state_dist(rng));
    }
    states_by_pattern.push_back(std::move(states));
  }
  return make_pattern_set(std::move(states_by_pattern));
}

static larch::site_pattern_set exhaustive_ac_mask_patterns(
    std::size_t taxon_count) {
  CHECK(taxon_count < 20);
  std::vector<std::vector<std::uint8_t>> states_by_pattern;
  auto mask_count = std::size_t{1} << taxon_count;
  states_by_pattern.reserve(mask_count);
  for (std::size_t mask = 0; mask < mask_count; ++mask) {
    std::vector<std::uint8_t> states(taxon_count, larch::nuc_base::A);
    for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
      if (((mask >> taxon) & 1U) != 0) states[taxon] = larch::nuc_base::C;
    }
    states_by_pattern.push_back(std::move(states));
  }
  return make_pattern_set(std::move(states_by_pattern));
}

static std::uint64_t total_pattern_weight(
    larch::site_pattern_set const& patterns) {
  std::uint64_t total = 0;
  for (auto const& pattern : patterns.patterns) total += pattern.weight;
  return total;
}

static void check_lazy_inside_matches_dense(
    larch::clade_grammar const& grammar,
    larch::site_pattern_set const& patterns) {
  larch::lazy_chart_options options;
  options.retain_all_inside_class_maps = true;
  auto lazy = larch::build_lazy_inside_chart(grammar, patterns, options);
  CHECK(lazy.pattern_count == patterns.patterns.size());
  CHECK(lazy.total_pattern_weight == total_pattern_weight(patterns));
  if (max_production_arity(grammar) > 2) {
    CHECK(lazy.multifurcation_productions_scored > 0);
  } else {
    CHECK(lazy.multifurcation_productions_scored == 0);
  }

  std::size_t row_sum = 0;
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto clade = static_cast<larch::clade_id>(cid);
    row_sum += lazy.inside_class_count(clade);
    CHECK(lazy.class_index_by_pattern_by_clade[cid].has_value());
    CHECK(lazy.structural_class_index_by_pattern_by_clade[cid].has_value());
    CHECK(lazy.structural_class_count(clade) <= patterns.patterns.size());
    CHECK(lazy.inside_class_count(clade) <= lazy.structural_class_count(clade));

    std::uint64_t weight_sum = 0;
    for (auto weight : lazy.class_weight_by_clade[cid]) weight_sum += weight;
    CHECK(weight_sum == total_pattern_weight(patterns));

    auto const& row_map = *lazy.class_index_by_pattern_by_clade[cid];
    auto const& structural_map =
        *lazy.structural_class_index_by_pattern_by_clade[cid];
    for (std::size_t lhs = 0; lhs < patterns.patterns.size(); ++lhs) {
      for (std::size_t rhs = 0; rhs < patterns.patterns.size(); ++rhs) {
        if (structural_map[lhs] == structural_map[rhs]) {
          CHECK(row_map[lhs] == row_map[rhs]);
        }
      }
    }
  }
  CHECK(lazy.lazy_inside_rows_computed == row_sum);

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    larch::leaf_site_states states;
    states.state_by_taxon = patterns.patterns[pattern_index].state_by_taxon;
    auto dense = larch::build_single_site_chart(grammar, states);
    for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
      CHECK(lazy.inside_row(static_cast<larch::clade_id>(cid), pattern_index) ==
            dense.inside[cid]);
    }
  }
}

static void check_lazy_inside_default_keeps_root_maps_only(
    larch::clade_grammar const& grammar,
    larch::site_pattern_set const& patterns) {
  auto lazy = larch::build_lazy_inside_chart(grammar, patterns);
  CHECK(lazy.class_index_by_pattern_by_clade[grammar.root_clade].has_value());
  CHECK(lazy.structural_class_index_by_pattern_by_clade[grammar.root_clade]
            .has_value());
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (cid == grammar.root_clade) continue;
    CHECK(!lazy.class_index_by_pattern_by_clade[cid]);
    CHECK(!lazy.structural_class_index_by_pattern_by_clade[cid]);
  }
}

static void check_lazy_composite_matches_dense(
    larch::clade_grammar const& grammar,
    larch::site_pattern_set const& patterns,
    larch::chart_options options = {}) {
  auto lazy = larch::build_lazy_inside_chart(grammar, patterns);
  auto dense = larch::build_composite_chart_score(grammar, patterns, options);
  auto lazy_score =
      larch::lazy_composite_chart_score(grammar, patterns, lazy, options);

  CHECK(larch::lazy_composite_lower_bound(grammar, patterns, lazy, options) ==
        dense.weighted_lower_bound);
  CHECK(lazy_score.weighted_lower_bound == dense.weighted_lower_bound);
  CHECK(lazy_score.per_pattern_root_min == dense.per_pattern_root_min);
  CHECK(lazy_score.per_pattern_root_min_by_reference_state ==
        dense.per_pattern_root_min_by_reference_state);
  CHECK(lazy_score.multifurcation_productions_scored ==
        lazy.multifurcation_productions_scored);

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    larch::leaf_site_states states;
    states.state_by_taxon = patterns.patterns[pattern_index].state_by_taxon;
    auto dense_chart = larch::build_single_site_chart(grammar, states);
    auto dense_score = larch::chart_multisite_detail::weighted_root_score_from_row(
        dense_chart.inside[grammar.root_clade], patterns.patterns[pattern_index],
        options);
    CHECK(larch::lazy_weighted_root_score_from_row(
              grammar, patterns, lazy, pattern_index, options) == dense_score);
  }
}

static void check_row_fluidity_reports_equal(
    larch::fluidity_report const& lhs, larch::fluidity_report const& rhs) {
  CHECK(lhs.global_min == rhs.global_min);
  CHECK(lhs.fluid_clade_state == rhs.fluid_clade_state);
  CHECK(lhs.locally_fluid_clade_state == rhs.locally_fluid_clade_state);
  CHECK(lhs.globally_optimal_clade_state == rhs.globally_optimal_clade_state);
  CHECK(lhs.globally_fluid_clade_state == rhs.globally_fluid_clade_state);
  CHECK(lhs.externally_fluid_clade == rhs.externally_fluid_clade);
  CHECK(lhs.optimal_choice_count == rhs.optimal_choice_count);
  CHECK(lhs.globally_optimal_choice_count == rhs.globally_optimal_choice_count);
  CHECK(lhs.fluid_clade_state_count == rhs.fluid_clade_state_count);
  CHECK(lhs.locally_fluid_clade_state_count ==
        rhs.locally_fluid_clade_state_count);
  CHECK(lhs.globally_fluid_clade_state_count ==
        rhs.globally_fluid_clade_state_count);
  CHECK(lhs.externally_fluid_clade_count == rhs.externally_fluid_clade_count);
  CHECK(lhs.externally_fluid_group_count == rhs.externally_fluid_group_count);
  CHECK(lhs.chart_row_fluidity_runs == 1);
  CHECK(rhs.chart_row_fluidity_runs == 1);
}

static void check_lazy_outside_matches_dense(
    larch::clade_grammar const& grammar,
    larch::site_pattern_set const& patterns,
    larch::chart_options options = {},
    std::uint8_t reference_state = larch::nuc_base::A) {
  auto lazy = larch::build_lazy_inside_chart(grammar, patterns);
  lazy = larch::build_lazy_outside_chart(grammar, patterns, std::move(lazy),
                                         options, reference_state);

  std::size_t outside_row_sum = 0;
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto clade = static_cast<larch::clade_id>(cid);
    outside_row_sum += lazy.outside_class_count(clade);
    CHECK(lazy.outside_class_index_by_pattern_by_clade[cid].has_value());

    std::uint64_t weight_sum = 0;
    for (auto weight : lazy.outside_class_weight_by_clade[cid])
      weight_sum += weight;
    CHECK(weight_sum == total_pattern_weight(patterns));
  }
  CHECK(lazy.lazy_outside_rows_computed == outside_row_sum);

  std::size_t expected_multifurcation_contexts = 0;
  for (std::size_t child = 0; child < grammar.productions_by_child.size();
       ++child) {
    for (auto pid : grammar.productions_by_child[child]) {
      auto const& prod = grammar.productions[pid];
      if (prod.children.size() == 2) continue;
      std::set<std::vector<std::size_t>> contexts;
      for (std::size_t pattern = 0; pattern < patterns.patterns.size();
           ++pattern) {
        std::vector<std::size_t> key;
        key.push_back(
            (*lazy.outside_class_index_by_pattern_by_clade[prod.parent])
                [pattern]);
        for (auto prod_child : prod.children) {
          key.push_back((*lazy.class_index_by_pattern_by_clade[prod_child])
                            [pattern]);
        }
        contexts.insert(std::move(key));
      }
      expected_multifurcation_contexts += contexts.size();
    }
  }
  CHECK(lazy.outside_multifurcation_productions_scored ==
        expected_multifurcation_contexts);

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    larch::leaf_site_states states;
    states.state_by_taxon = patterns.patterns[pattern_index].state_by_taxon;
    auto dense_inside = larch::build_single_site_chart(grammar, states);
    auto dense_outside = larch::build_single_site_outside_chart(
        grammar, dense_inside, options, reference_state);
    CHECK(lazy.outside_global_min(pattern_index) == dense_outside.global_min);
    for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
      CHECK(lazy.outside_row(static_cast<larch::clade_id>(cid),
                             pattern_index) == dense_outside.outside[cid]);
    }
    auto dense_fluidity = larch::build_single_site_fluidity_report_from_rows(
        grammar, dense_inside, dense_outside);
    auto lazy_fluidity = larch::build_single_site_fluidity_report_from_rows(
        grammar, lazy, pattern_index);
    check_row_fluidity_reports_equal(dense_fluidity, lazy_fluidity);
  }
}

static larch::clade_grammar validator_test_grammar() {
  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};
  grammar.taxa.sample_id_to_id = {{"A", 0}, {"B", 1}, {"C", 2}, {"D", 3}};
  grammar.clades = {
      {{0}},        // 0: A
      {{1}},        // 1: B
      {{2}},        // 2: C
      {{3}},        // 3: D
      {{0, 1}},     // 4: AB
      {{1, 2}},     // 5: BC
      {{0, 1, 2}},  // 6: ABC
      {{0, 1, 2, 3}}};
  grammar.root_clade = 7;
  grammar.productions_by_parent.resize(grammar.clades.size());
  grammar.productions_by_child.resize(grammar.clades.size());
  return grammar;
}

static void test_invalid_shared_partition_validator() {
  std::println("test_invalid_shared_partition_validator");

  auto grammar = validator_test_grammar();
  CHECK(runtime_error_message([&] {
          larch::detail::validate_production_partition(
              grammar, 6, std::vector<larch::clade_id>{0, 1, 2},
              "valid ABC partition");
        })
            .empty());

  auto overlap_message = runtime_error_message([&] {
    larch::detail::validate_production_partition(
        grammar, 6, std::vector<larch::clade_id>{0, 4, 2},
        "overlapping ABC partition");
  });
  CHECK(overlap_message.find("not pairwise disjoint") != std::string::npos);

  auto missing_cover_message = runtime_error_message([&] {
    larch::detail::validate_production_partition(
        grammar, 7, std::vector<larch::clade_id>{0, 1, 2},
        "missing-cover ABCD partition");
  });
  CHECK(missing_cover_message.find("do not union to parent clade") !=
        std::string::npos);

  auto non_subset_message = runtime_error_message([&] {
    larch::detail::validate_production_partition(
        grammar, 6, std::vector<larch::clade_id>{0, 1, 3},
        "non-subset ABC partition");
  });
  CHECK(non_subset_message.find("not a subset of the parent") !=
        std::string::npos);

  std::println("  PASS");
}

static void test_lazy_inside_validates_before_recursing() {
  std::println("test_lazy_inside_validates_before_recursing");

  auto grammar = validator_test_grammar();
  larch::grammar_production mismatched_parent;
  mismatched_parent.parent = 6;
  mismatched_parent.children = {4, 2};
  mismatched_parent.multiplicity = 1;
  grammar.productions.push_back(std::move(mismatched_parent));
  grammar.productions_by_parent[4].push_back(0);
  grammar.productions_by_child[4].push_back(0);
  grammar.productions_by_child[2].push_back(0);
  grammar.root_clade = 4;

  auto patterns = make_pattern_set_from_strings({"AAAA", "ACGT"});
  auto message = runtime_error_message([&] {
    (void)larch::build_lazy_inside_chart(grammar, patterns);
  });
  CHECK(message.find("mismatched parent") != std::string::npos);

  larch::lazy_chart_options retain_all;
  retain_all.retain_all_inside_class_maps = true;
  auto retain_message = runtime_error_message([&] {
    (void)larch::build_lazy_inside_chart(grammar, patterns, retain_all);
  });
  CHECK(retain_message.find("mismatched parent") != std::string::npos);

  std::println("  PASS");
}

static void test_trinary_fixture_and_allow_gate() {
  std::println("test_trinary_fixture_and_allow_gate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_inner("root", "A",
                                   {leaf("A", 'A'), leaf("B", 'C'),
                                    leaf("C", 'G')}));
  auto reject_message =
      runtime_error_message([&] { (void)larch::build_clade_grammar(dag); });
  CHECK(reject_message.find("non-binary production of arity 3") !=
        std::string::npos);

  auto built = build_allowing_polytomies(dag);
  auto const& grammar = built.grammar;
  CHECK(built.audit.non_binary_production_count == 1);
  CHECK(grammar.productions.size() == 1);
  CHECK(max_production_arity(grammar) == 3);
  larch::detail::validate_production_partition(
      grammar, grammar.productions.front().parent,
      grammar.productions.front().children, "test trinary production");

  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto rows = brute_root_rows(grammar, states);
  CHECK(rows.size() == 1);
  CHECK(rows.front()[larch::nuc_base::A] == 2);
  CHECK(rows.front()[larch::nuc_base::C] == 2);
  CHECK(rows.front()[larch::nuc_base::G] == 2);
  CHECK(rows.front()[larch::nuc_base::T] == 3);
  CHECK(row_min(rows.front()) == 2);
  auto chart = build_and_check_dense_inside(grammar, states);
  CHECK(chart.inside[grammar.root_clade] == rows.front());
  auto outside = larch::build_single_site_outside_chart(grammar, chart);
  larch::parsimony_chart_detail::reset_arity_gate_throw_counters_for_tests();
  auto trim_message = runtime_error_message([&] {
    (void)larch::build_single_site_trim_mask(grammar, chart, outside);
  });
  check_arity_gate_message(trim_message, "choice layer");

  auto patterns = larch::build_site_patterns(dag, grammar);
  auto multisite_trim_message = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns);
  });
  check_arity_gate_message(multisite_trim_message, "B&B frontier");

  auto fluidity =
      larch::build_single_site_fluidity_report(grammar, chart, outside);
  CHECK(fluidity.chart_row_fluidity_runs == 1);
  CHECK(fluidity.global_min == outside.global_min);
  CHECK(fluidity.globally_optimal_clade_state[grammar.root_clade]
                                              [larch::nuc_base::A]);
  CHECK(fluidity.globally_optimal_choice_count[grammar.root_clade]
                                               [larch::nuc_base::A] == 1);
  CHECK(fluidity.row_globally_optimal_choices_by_clade_state[grammar.root_clade]
                                                          [larch::nuc_base::A]
                                                              .size() == 1);

  auto fluidity_message = runtime_error_message([&] {
    (void)larch::build_single_site_fluidity_report_from_choice_layer(
        grammar, chart, outside);
  });
  check_arity_gate_message(fluidity_message, "choice layer");

  auto plateau_message = runtime_error_message([&] {
    (void)larch::build_multisite_plateau_report(grammar, patterns);
  });
  check_arity_gate_message(plateau_message, "choice layer");

  auto traceback_message = runtime_error_message([&] {
    (void)larch::deterministic_optimal_single_site_traceback(grammar, chart,
                                                             outside);
  });
  CHECK(traceback_message.find("Phase 4 supports binary productions only") !=
        std::string::npos);

  larch::chart_options trace_opts;
  trace_opts.keep_trace = true;
  auto trace_message = runtime_error_message([&] {
    (void)larch::build_single_site_chart(grammar, states, trace_opts);
  });
  check_arity_gate_message(trace_message, "trace");

  auto arity_gate_throws =
      larch::parsimony_chart_detail::arity_gate_throws_snapshot();
  CHECK(arity_gate_throws.total == 5);
  CHECK(arity_gate_throws.count(
            larch::arity_gate_consumer::single_site_trim_mask) == 1);
  CHECK(arity_gate_throws.count(larch::arity_gate_consumer::multisite_trim) ==
        1);
  CHECK(arity_gate_throws.count(
            larch::arity_gate_consumer::single_site_fluidity_report) == 1);
  CHECK(arity_gate_throws.count(
            larch::arity_gate_consumer::multisite_plateau_report) == 1);
  CHECK(arity_gate_throws.count(
            larch::arity_gate_consumer::single_site_chart_trace) == 1);
  CHECK(larch::parsimony_chart_detail::arity_gate_consumer_name(
            larch::arity_gate_consumer::single_site_trim_mask) ==
        std::string_view{"single_site_trim_mask"});
  CHECK(larch::parsimony_chart_detail::arity_gate_consumer_reason(
            larch::arity_gate_consumer::multisite_trim)
            .find("B&B frontier") != std::string_view::npos);

  std::println("  PASS");
}

static void test_alternative_multifurcating_productions() {
  std::println("test_alternative_multifurcating_productions");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_inner("root", "A",
                                   {leaf("A", 'A'), leaf("B", 'C'),
                                    leaf("C", 'G')})));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_inner(
               "root", "A",
               {leaf("A", 'A'),
                larch::test::tiny_inner("BC", "A",
                                        {leaf("B", 'C'), leaf("C", 'G')})})));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto built = build_allowing_polytomies(dag);
  auto const& grammar = built.grammar;
  CHECK(max_production_arity(grammar) == 3);
  CHECK(built.audit.non_binary_production_count == 1);

  bool saw_binary_root = false;
  bool saw_trinary_root = false;
  for (auto pid : grammar.productions_by_parent[grammar.root_clade]) {
    auto arity = grammar.productions[pid].children.size();
    saw_binary_root = saw_binary_root || arity == 2;
    saw_trinary_root = saw_trinary_root || arity == 3;
  }
  CHECK(saw_binary_root);
  CHECK(saw_trinary_root);

  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto rows = brute_root_rows(grammar, states);
  CHECK(rows.size() >= 2);
  auto chart = build_and_check_dense_inside(grammar, states);
  CHECK(chart.inside[grammar.root_clade] == brute_min_row(rows));
  check_lazy_inside_matches_dense(
      grammar, exhaustive_ac_mask_patterns(grammar.taxa.id_to_sample_id.size()));
  check_lazy_composite_matches_dense(
      grammar, exhaustive_ac_mask_patterns(grammar.taxa.id_to_sample_id.size()));
  check_lazy_outside_matches_dense(
      grammar, exhaustive_ac_mask_patterns(grammar.taxa.id_to_sample_id.size()));
  check_lazy_inside_matches_dense(
      grammar, lazy_oracle_patterns(grammar.taxa.id_to_sample_id.size()));
  check_lazy_inside_matches_dense(
      grammar,
      random_lazy_oracle_patterns(grammar.taxa.id_to_sample_id.size(), 32, 41));

  std::println("  PASS");
}

static void test_four_ary_fixture() {
  std::println("test_four_ary_fixture");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_inner("root", "A",
                                   {leaf("A", 'A'), leaf("B", 'A'),
                                    leaf("C", 'C'), leaf("D", 'C')}));
  auto built = build_allowing_polytomies(dag);
  CHECK(max_production_arity(built.grammar) == 4);
  CHECK(built.audit.non_binary_production_count == 1);

  auto states = larch::extract_leaf_site_states(dag, built.grammar, 1);
  auto rows = brute_root_rows(built.grammar, states);
  CHECK(rows.size() == 1);
  CHECK(rows.front()[larch::nuc_base::A] == 2);
  CHECK(rows.front()[larch::nuc_base::C] == 2);
  CHECK(rows.front()[larch::nuc_base::G] == 4);
  CHECK(rows.front()[larch::nuc_base::T] == 4);
  CHECK(row_min(rows.front()) == 2);
  auto chart = build_and_check_dense_inside(built.grammar, states);
  CHECK(chart.inside[built.grammar.root_clade] == rows.front());

  std::println("  PASS");
}

static void test_five_ary_fixture() {
  std::println("test_five_ary_fixture");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_inner("root", "A",
                                   {leaf("A", 'A'), leaf("B", 'C'),
                                    leaf("C", 'G'), leaf("D", 'T'),
                                    leaf("E", 'A')}));
  auto built = build_allowing_polytomies(dag);
  CHECK(max_production_arity(built.grammar) == 5);
  CHECK(built.audit.non_binary_production_count == 1);

  auto states = larch::extract_leaf_site_states(dag, built.grammar, 1);
  auto rows = brute_root_rows(built.grammar, states);
  CHECK(rows.size() == 1);
  CHECK(rows.front()[larch::nuc_base::A] == 3);
  CHECK(rows.front()[larch::nuc_base::C] == 4);
  CHECK(rows.front()[larch::nuc_base::G] == 4);
  CHECK(rows.front()[larch::nuc_base::T] == 4);
  CHECK(row_min(rows.front()) == 3);
  auto chart = build_and_check_dense_inside(built.grammar, states);
  CHECK(chart.inside[built.grammar.root_clade] == rows.front());

  std::println("  PASS");
}

static void test_mixed_arity_fixture() {
  std::println("test_mixed_arity_fixture");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A",
      larch::test::tiny_inner(
          "root", "A",
          {larch::test::tiny_inner("AB", "A",
                                   {leaf("A", 'A'), leaf("B", 'A')}),
           larch::test::tiny_inner("CDE", "A",
                                   {leaf("C", 'C'), leaf("D", 'C'),
                                    leaf("E", 'C')}),
           leaf("F", 'C')}));
  auto built = build_allowing_polytomies(dag);
  CHECK(max_production_arity(built.grammar) == 3);
  CHECK(built.audit.non_binary_production_count == 2);

  auto states = larch::extract_leaf_site_states(dag, built.grammar, 1);
  auto rows = brute_root_rows(built.grammar, states);
  CHECK(rows.size() == 1);
  CHECK(rows.front()[larch::nuc_base::A] == 2);
  CHECK(rows.front()[larch::nuc_base::C] == 1);
  CHECK(rows.front()[larch::nuc_base::G] == 3);
  CHECK(rows.front()[larch::nuc_base::T] == 3);
  CHECK(row_min(rows.front()) == 1);
  auto chart = build_and_check_dense_inside(built.grammar, states);
  CHECK(chart.inside[built.grammar.root_clade] == rows.front());

  std::println("  PASS");
}

static larch::clade_grammar direct_mixed_enumeration_grammar() {
  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D", "E", "F"};
  for (std::size_t i = 0; i < grammar.taxa.id_to_sample_id.size(); ++i) {
    grammar.taxa.sample_id_to_id.emplace(grammar.taxa.id_to_sample_id[i],
                                         static_cast<larch::taxon_id>(i));
  }
  grammar.clades = {
      {{0}},                 // A
      {{1}},                 // B
      {{2}},                 // C
      {{3}},                 // D
      {{4}},                 // E
      {{5}},                 // F
      {{1, 2}},              // BC
      {{0, 1, 2}},           // ABC
      {{3, 4}},              // DE
      {{3, 4, 5}},           // DEF
      {{0, 1, 2, 3, 4, 5}},  // root
  };
  grammar.root_clade = 10;
  grammar.productions_by_parent.resize(grammar.clades.size());
  grammar.productions_by_child.resize(grammar.clades.size());

  auto add_production = [&](larch::clade_id parent,
                            std::vector<larch::clade_id> children) {
    auto pid = static_cast<larch::production_id>(grammar.productions.size());
    grammar.productions.push_back(
        {.parent = parent, .children = std::move(children)});
    grammar.productions_by_parent[parent].push_back(pid);
    for (auto child : grammar.productions.back().children) {
      grammar.productions_by_child[child].push_back(pid);
    }
    return pid;
  };

  CHECK(add_production(6, {1, 2}) == 0);     // BC
  CHECK(add_production(7, {0, 1, 2}) == 1);  // direct ternary ABC
  CHECK(add_production(7, {0, 6}) == 2);     // A + BC
  CHECK(add_production(8, {3, 4}) == 3);     // DE
  CHECK(add_production(9, {3, 4, 5}) == 4);  // direct ternary DEF
  CHECK(add_production(9, {8, 5}) == 5);     // DE + F
  CHECK(add_production(10, {7, 9}) == 6);    // ABC + DEF
  return grammar;
}

static std::vector<larch::production_id> selected_productions(
    larch::grammar_topology const& topology) {
  std::vector<larch::production_id> selected;
  for (std::size_t pid = 0; pid < topology.used_production.size(); ++pid) {
    if (topology.used_production[pid])
      selected.push_back(static_cast<larch::production_id>(pid));
  }
  return selected;
}

static void test_direct_kary_streaming_topology_enumerator() {
  std::println("test_direct_kary_streaming_topology_enumerator");

  auto grammar = direct_mixed_enumeration_grammar();
  larch::grammar_topology_enumerator enumerator(grammar);
  CHECK(enumerator.topology_count() == 4);
  CHECK(enumerator.topology_count(7) == 2);
  CHECK(enumerator.topology_count(9) == 2);
  CHECK(enumerator.topology_count(grammar.root_clade) == 4);
  CHECK(enumerator.topology_count_for_production(1) == 1);
  CHECK(enumerator.topology_count_for_production(2) == 1);
  CHECK(enumerator.topology_count_for_production(6) == 4);

  auto patterns = make_pattern_set_from_strings({"ACCAAA", "AAACCA"});
  std::vector<std::vector<larch::production_id>> expected_productions{
      {1, 4, 6}, {1, 3, 5, 6}, {0, 2, 4, 6}, {0, 2, 3, 5, 6}};
  std::vector<std::uint64_t> expected_scores{6, 4, 5, 3};
  std::map<std::uint64_t, std::size_t> expected_histogram{
      {3, 1}, {4, 1}, {5, 1}, {6, 1}};

  std::vector<std::vector<larch::production_id>> serial_productions;
  std::vector<std::uint64_t> serial_scores;
  std::map<std::uint64_t, std::size_t> serial_histogram;
  auto emitted = enumerator.stream(
      [&](std::uint64_t ordinal, larch::grammar_topology const& topology) {
        CHECK(ordinal == serial_scores.size());
        (void)larch::validate_grammar_topology(grammar, topology);
        serial_productions.push_back(selected_productions(topology));
        auto score =
            larch::score_selected_topology(grammar, patterns, topology);
        serial_scores.push_back(score);
        ++serial_histogram[score];
      });
  CHECK(emitted == 4);
  CHECK(serial_productions == expected_productions);
  CHECK(serial_scores == expected_scores);
  CHECK(serial_histogram == expected_histogram);

  for (bool score_ua_edge : {false, true}) {
    larch::chart_options chart_options;
    chart_options.score_ua_edge = score_ua_edge;
    larch::grammar_topology_fitch_scorer incremental_scorer(
        grammar, patterns, chart_options);
    std::map<std::uint64_t, std::uint64_t> parity_histogram;
    enumerator.stream(
        [&](std::uint64_t, larch::grammar_topology const& topology) {
          auto const fitch =
              incremental_scorer.score_enumerator_topology(topology);
          auto const sankoff = larch::score_selected_topology(
              grammar, patterns, topology, chart_options);
          CHECK(fitch == sankoff);
          ++parity_histogram[fitch];
        });

    larch::grammar_topology_census_options serial_options;
    serial_options.worker_count = 1;
    serial_options.sankoff_verification_stride = 1;
    serial_options.retain_ordinal_score_ledger = true;
    auto serial_census = larch::census_grammar_topologies(
        grammar, patterns, chart_options, serial_options);
    CHECK(serial_census.expected_topology_count == 4);
    CHECK(serial_census.scored_topology_count == 4);
    CHECK(serial_census.sankoff_verified_topology_count == 4);
    CHECK(serial_census.score_histogram == parity_histogram);
    CHECK(serial_census.ordinal_scores == expected_scores);
    CHECK(serial_census.recomputed_internal_clade_visits > 0);
    CHECK(serial_census.selected_score_histogram_by_production.size() ==
          grammar.productions.size());
    CHECK(serial_census.selected_score_histogram_by_production[6] ==
          parity_histogram);
    std::uint64_t abc_direct_count = 0;
    for (auto const& [score, count] :
         serial_census.selected_score_histogram_by_production[1]) {
      (void)score;
      abc_direct_count += count;
    }
    CHECK(abc_direct_count == 2);

    auto parallel_options = serial_options;
    parallel_options.worker_count = 3;
    auto parallel_census = larch::census_grammar_topologies(
        grammar, patterns, chart_options, parallel_options);
    CHECK(parallel_census.expected_topology_count ==
          serial_census.expected_topology_count);
    CHECK(parallel_census.scored_topology_count ==
          serial_census.scored_topology_count);
    CHECK(parallel_census.score_histogram ==
          serial_census.score_histogram);
    CHECK(parallel_census.optimum == serial_census.optimum);
    CHECK(parallel_census.optimal_ordinals ==
          serial_census.optimal_ordinals);
    CHECK(parallel_census.ordinal_scores == serial_census.ordinal_scores);
    CHECK(parallel_census.selected_score_histogram_by_production ==
          serial_census.selected_score_histogram_by_production);
  }

  for (std::uint64_t ordinal = 0; ordinal < enumerator.topology_count();
       ++ordinal) {
    auto topology = enumerator.topology_at(ordinal);
    CHECK(selected_productions(topology) == expected_productions[ordinal]);
    CHECK(larch::score_selected_topology(grammar, patterns, topology) ==
          expected_scores[ordinal]);
  }

  struct worker_row {
    std::uint64_t ordinal = 0;
    std::vector<larch::production_id> productions;
    std::uint64_t score = 0;
  };
  for (std::size_t worker_count :
       {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}}) {
    std::vector<std::vector<worker_row>> rows(worker_count);
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back([&, worker] {
        auto range = enumerator.worker_partition(worker, worker_count);
        auto worker_emitted = enumerator.stream(
            range, [&](std::uint64_t ordinal,
                       larch::grammar_topology const& topology) {
              rows[worker].push_back(
                  {.ordinal = ordinal,
                   .productions = selected_productions(topology),
                   .score = larch::score_selected_topology(grammar, patterns,
                                                           topology)});
            });
        CHECK(worker_emitted == range.size());
      });
    }
    for (auto& worker : workers) worker.join();

    std::vector<worker_row> combined;
    for (auto& worker_rows : rows) {
      combined.insert(combined.end(),
                      std::make_move_iterator(worker_rows.begin()),
                      std::make_move_iterator(worker_rows.end()));
    }
    CHECK(combined.size() == enumerator.topology_count());
    for (std::size_t ordinal = 0; ordinal < combined.size(); ++ordinal) {
      CHECK(combined[ordinal].ordinal == ordinal);
      CHECK(combined[ordinal].productions == expected_productions[ordinal]);
      CHECK(combined[ordinal].score == expected_scores[ordinal]);
    }
  }

  std::vector<std::uint64_t> stopped_ordinals;
  CHECK(enumerator.stream(
            [&](std::uint64_t ordinal, larch::grammar_topology const&) -> bool {
              stopped_ordinals.push_back(ordinal);
              return stopped_ordinals.size() < 2;
            }) == 2);
  CHECK(stopped_ordinals == std::vector<std::uint64_t>({0, 1}));
  CHECK(enumerator.stream(
            {2, 2}, [](std::uint64_t, larch::grammar_topology const&) {}) == 0);
  CHECK(runtime_error_message([&] {
          (void)enumerator.worker_partition(0, 0);
        }).find("worker count must be positive") != std::string::npos);
  CHECK(runtime_error_message([&] {
          (void)enumerator.topology_at(4);
        }).find("topology ordinal out of range") != std::string::npos);

  std::println("  PASS");
}

static void test_lazy_inside_binary_fixture() {
  std::println("test_lazy_inside_binary_fixture");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A",
      larch::test::tiny_inner(
          "root", "A",
          {larch::test::tiny_inner("AB", "A",
                                   {leaf("A", 'A'), leaf("B", 'A')}),
           larch::test::tiny_inner("CD", "A",
                                   {leaf("C", 'A'), leaf("D", 'A')})}));
  auto built = larch::build_clade_grammar(dag);
  CHECK(max_production_arity(built) == 2);

  check_lazy_inside_matches_dense(
      built, make_pattern_set_from_strings({"AAAA", "AACC", "ACGT", "CCAA",
                                            "TTTT", "AGAG"}));
  check_lazy_inside_matches_dense(built, exhaustive_ac_mask_patterns(4));
  check_lazy_composite_matches_dense(built, exhaustive_ac_mask_patterns(4));
  check_lazy_outside_matches_dense(built, exhaustive_ac_mask_patterns(4));
  larch::chart_options ua_edge;
  ua_edge.score_ua_edge = true;
  check_lazy_composite_matches_dense(
      built, with_cycling_reference_counts(exhaustive_ac_mask_patterns(4)),
      ua_edge);
  check_lazy_outside_matches_dense(built, exhaustive_ac_mask_patterns(4),
                                   ua_edge, larch::nuc_base::G);
  check_lazy_inside_matches_dense(built, random_lazy_oracle_patterns(4, 24, 17));
  check_lazy_inside_default_keeps_root_maps_only(
      built, random_lazy_oracle_patterns(4, 12, 19));

  std::println("  PASS");
}

static void test_lazy_inside_multifurcation_fixtures() {
  std::println("test_lazy_inside_multifurcation_fixtures");

  {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_inner("root", "A",
                                     {leaf("A", 'A'), leaf("B", 'A'),
                                      leaf("C", 'A')}));
    auto built = build_allowing_polytomies(dag);
    CHECK(max_production_arity(built.grammar) == 3);
    check_lazy_inside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_composite_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_outside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    larch::chart_options ua_edge;
    ua_edge.score_ua_edge = true;
    check_lazy_composite_matches_dense(
        built.grammar,
        with_cycling_reference_counts(
            exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size())),
        ua_edge);
    check_lazy_inside_matches_dense(
        built.grammar, lazy_oracle_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_inside_matches_dense(
        built.grammar,
        random_lazy_oracle_patterns(built.grammar.taxa.id_to_sample_id.size(),
                                    32, 31));
  }

  {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_inner("root", "A",
                                     {leaf("A", 'A'), leaf("B", 'A'),
                                      leaf("C", 'A'), leaf("D", 'A')}));
    auto built = build_allowing_polytomies(dag);
    CHECK(max_production_arity(built.grammar) == 4);
    check_lazy_inside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_composite_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_outside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_inside_matches_dense(
        built.grammar, lazy_oracle_patterns(built.grammar.taxa.id_to_sample_id.size()));
  }

  {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_inner("root", "A",
                                     {leaf("A", 'A'), leaf("B", 'A'),
                                      leaf("C", 'A'), leaf("D", 'A'),
                                      leaf("E", 'A')}));
    auto built = build_allowing_polytomies(dag);
    CHECK(max_production_arity(built.grammar) == 5);
    check_lazy_inside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_composite_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_outside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_inside_matches_dense(
        built.grammar, lazy_oracle_patterns(built.grammar.taxa.id_to_sample_id.size()));
  }

  {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A",
        larch::test::tiny_inner(
            "root", "A",
            {larch::test::tiny_inner("AB", "A",
                                     {leaf("A", 'A'), leaf("B", 'A')}),
             larch::test::tiny_inner("CDE", "A",
                                     {leaf("C", 'A'), leaf("D", 'A'),
                                      leaf("E", 'A')}),
             leaf("F", 'A')}));
    auto built = build_allowing_polytomies(dag);
    CHECK(max_production_arity(built.grammar) == 3);
    check_lazy_inside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_composite_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_outside_matches_dense(
        built.grammar,
        exhaustive_ac_mask_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_inside_matches_dense(
        built.grammar, lazy_oracle_patterns(built.grammar.taxa.id_to_sample_id.size()));
    check_lazy_inside_matches_dense(
        built.grammar,
        random_lazy_oracle_patterns(built.grammar.taxa.id_to_sample_id.size(),
                                    40, 47));
  }

  std::println("  PASS");
}

static void test_lazy_inside_pandemic_shape_counter() {
  std::println("test_lazy_inside_pandemic_shape_counter");

  constexpr std::size_t taxon_count = 80;
  std::vector<larch::test::tiny_tree_node> children;
  children.reserve(taxon_count);
  for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
    children.push_back(
        leaf("T" + std::to_string(taxon), 'A'));
  }
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_inner("root", "A", std::move(children)));
  auto built = build_allowing_polytomies(dag);
  CHECK(max_production_arity(built.grammar) == taxon_count);

  std::vector<std::vector<std::uint8_t>> states_by_pattern;
  states_by_pattern.push_back(
      std::vector<std::uint8_t>(taxon_count, larch::nuc_base::A));
  for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
    auto states = states_by_pattern.front();
    states[taxon] = larch::nuc_base::C;
    states_by_pattern.push_back(std::move(states));
  }
  auto patterns = make_pattern_set(std::move(states_by_pattern));
  larch::lazy_chart_options options;
  options.retain_all_inside_class_maps = true;
  auto lazy = larch::build_lazy_inside_chart(built.grammar, patterns, options);
  check_lazy_inside_matches_dense(built.grammar, patterns);
  check_lazy_inside_default_keeps_root_maps_only(built.grammar, patterns);

  auto denominator = patterns.patterns.size() * built.grammar.clades.size();
  CHECK(lazy.lazy_inside_rows_computed * 10 <= denominator);
  CHECK(lazy.lazy_patterns_merged_max > 0);
  CHECK(lazy.lazy_remerge_collisions > 0);

  std::println("  PASS");
}

int main() {
  test_invalid_shared_partition_validator();
  test_lazy_inside_validates_before_recursing();
  test_trinary_fixture_and_allow_gate();
  test_alternative_multifurcating_productions();
  test_four_ary_fixture();
  test_five_ary_fixture();
  test_mixed_arity_fixture();
  test_direct_kary_streaming_topology_enumerator();
  test_lazy_inside_binary_fixture();
  test_lazy_inside_multifurcation_fixtures();
  test_lazy_inside_pandemic_shape_counter();

  std::println("All multifurcation chart oracle tests passed!");
  return 0;
}

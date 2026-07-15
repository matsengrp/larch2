#include <larch/chart_execution_plan.hpp>
#include <larch/chart_trim.hpp>
#include <larch/lazy_chart.hpp>
#include <larch/parsimony_chart.hpp>

#include "chart_spr_allocation_observer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
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

template <class Fn>
static std::string runtime_error_message(Fn&& fn) {
  try {
    fn();
  } catch (std::runtime_error const& error) {
    return error.what();
  }
  CHECK(false && "expected std::runtime_error");
  return {};
}

static larch::clade_grammar make_binary_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};
  grammar.taxa.sample_id_to_id = {{"A", taxon_id{0}},
                                  {"B", taxon_id{1}},
                                  {"C", taxon_id{2}},
                                  {"D", taxon_id{3}}};
  grammar.clades = {{{taxon_id{0}}},
                    {{taxon_id{1}}},
                    {{taxon_id{2}}},
                    {{taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}}},
                    {{taxon_id{2}, taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}, taxon_id{3}}}};
  grammar.productions = {
      larch::grammar_production{clade_id{4}, {clade_id{0}, clade_id{1}}, {}, 1},
      larch::grammar_production{clade_id{5}, {clade_id{2}, clade_id{3}}, {}, 1},
      larch::grammar_production{
          clade_id{6}, {clade_id{4}, clade_id{5}}, {}, 1}};
  grammar.productions_by_parent = {{},
                                   {},
                                   {},
                                   {},
                                   {production_id{0}},
                                   {production_id{1}},
                                   {production_id{2}}};
  grammar.productions_by_child = {{production_id{0}},
                                  {production_id{0}},
                                  {production_id{1}},
                                  {production_id{1}},
                                  {production_id{2}},
                                  {production_id{2}},
                                  {}};
  grammar.root_clade = clade_id{6};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

static larch::clade_grammar make_trinary_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C"};
  grammar.taxa.sample_id_to_id = {
      {"A", taxon_id{0}}, {"B", taxon_id{1}}, {"C", taxon_id{2}}};
  grammar.clades = {{{taxon_id{0}}},
                    {{taxon_id{1}}},
                    {{taxon_id{2}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}}}};
  grammar.productions = {larch::grammar_production{
      clade_id{3}, {clade_id{0}, clade_id{1}, clade_id{2}}, {}, 1}};
  grammar.productions_by_parent = {{}, {}, {}, {production_id{0}}};
  grammar.productions_by_child = {
      {production_id{0}}, {production_id{0}}, {production_id{0}}, {}};
  grammar.root_clade = clade_id{3};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

static larch::clade_grammar make_wide_star_grammar(
    std::size_t leaf_count = 64) {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  CHECK(leaf_count >= 2);
  CHECK(leaf_count <= std::numeric_limits<taxon_id>::max());
  CHECK(leaf_count <= std::numeric_limits<clade_id>::max());
  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id.reserve(leaf_count);
  grammar.clades.reserve(leaf_count + 1);
  std::vector<taxon_id> root_taxa;
  std::vector<clade_id> root_children;
  root_taxa.reserve(leaf_count);
  root_children.reserve(leaf_count);
  for (std::size_t index = 0; index < leaf_count; ++index) {
    auto const taxon = static_cast<taxon_id>(index);
    auto const clade = static_cast<clade_id>(index);
    auto name = std::string{"T"} + std::to_string(index);
    grammar.taxa.id_to_sample_id.push_back(name);
    grammar.taxa.sample_id_to_id.emplace(std::move(name), taxon);
    grammar.clades.push_back(larch::clade_key{{taxon}});
    root_taxa.push_back(taxon);
    root_children.push_back(clade);
  }
  auto const root = static_cast<clade_id>(leaf_count);
  grammar.clades.push_back(larch::clade_key{std::move(root_taxa)});
  grammar.productions.push_back(
      larch::grammar_production{root, std::move(root_children), {}, 1});
  grammar.productions_by_parent.resize(leaf_count + 1);
  grammar.productions_by_parent[root].push_back(production_id{0});
  grammar.productions_by_child.resize(leaf_count + 1);
  for (std::size_t index = 0; index < leaf_count; ++index) {
    grammar.productions_by_child[index].push_back(production_id{0});
  }
  grammar.root_clade = root;
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

static larch::clade_grammar make_wide_balanced_binary_grammar(
    std::size_t leaf_count = 64) {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  CHECK(leaf_count >= 2);
  CHECK((leaf_count & (leaf_count - 1)) == 0);
  CHECK(leaf_count <= std::numeric_limits<taxon_id>::max());
  CHECK(leaf_count <=
        (static_cast<std::size_t>(std::numeric_limits<clade_id>::max()) + 1) /
            2);
  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id.reserve(leaf_count);
  grammar.clades.reserve(2 * leaf_count - 1);
  std::vector<clade_id> level;
  level.reserve(leaf_count);
  for (std::size_t index = 0; index < leaf_count; ++index) {
    auto const taxon = static_cast<taxon_id>(index);
    auto const clade = static_cast<clade_id>(index);
    auto name = std::string{"T"} + std::to_string(index);
    grammar.taxa.id_to_sample_id.push_back(name);
    grammar.taxa.sample_id_to_id.emplace(std::move(name), taxon);
    grammar.clades.push_back(larch::clade_key{{taxon}});
    level.push_back(clade);
  }
  while (level.size() > 1) {
    std::vector<clade_id> parents;
    parents.reserve(level.size() / 2);
    for (std::size_t index = 0; index < level.size(); index += 2) {
      auto const left = level[index];
      auto const right = level[index + 1];
      auto taxa = grammar.clades[left].taxa;
      taxa.insert(taxa.end(), grammar.clades[right].taxa.begin(),
                  grammar.clades[right].taxa.end());
      auto const parent = static_cast<clade_id>(grammar.clades.size());
      grammar.clades.push_back(larch::clade_key{std::move(taxa)});
      grammar.productions.push_back(
          larch::grammar_production{parent, {left, right}, {}, 1});
      parents.push_back(parent);
    }
    level = std::move(parents);
  }
  grammar.productions_by_parent.resize(grammar.clades.size());
  grammar.productions_by_child.resize(grammar.clades.size());
  for (std::size_t index = 0; index < grammar.productions.size(); ++index) {
    auto const production = static_cast<production_id>(index);
    auto const& entry = grammar.productions[index];
    grammar.productions_by_parent[entry.parent].push_back(production);
    for (auto child : entry.children) {
      grammar.productions_by_child[child].push_back(production);
    }
  }
  grammar.root_clade = level.front();
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

// Two alternative binary root productions make the complete row key wider
// than the structural key selected by the canonical first production.
static larch::clade_grammar make_nonlex_binary_dag_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};
  grammar.taxa.sample_id_to_id = {{"A", taxon_id{0}},
                                  {"B", taxon_id{1}},
                                  {"C", taxon_id{2}},
                                  {"D", taxon_id{3}}};
  grammar.clades = {{{taxon_id{0}}},
                    {{taxon_id{1}}},
                    {{taxon_id{2}}},
                    {{taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}}},
                    {{taxon_id{2}, taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{2}}},
                    {{taxon_id{1}, taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}, taxon_id{3}}}};
  grammar.productions = {
      larch::grammar_production{clade_id{4}, {clade_id{0}, clade_id{1}}, {}, 1},
      larch::grammar_production{clade_id{5}, {clade_id{2}, clade_id{3}}, {}, 1},
      larch::grammar_production{clade_id{6}, {clade_id{0}, clade_id{2}}, {}, 1},
      larch::grammar_production{clade_id{7}, {clade_id{1}, clade_id{3}}, {}, 1},
      larch::grammar_production{clade_id{8}, {clade_id{4}, clade_id{5}}, {}, 1},
      larch::grammar_production{clade_id{8}, {clade_id{6}, clade_id{7}}, {}, 1},
  };
  grammar.productions_by_parent = {
      {},
      {},
      {},
      {},
      {production_id{0}},
      {production_id{1}},
      {production_id{2}},
      {production_id{3}},
      {production_id{4}, production_id{5}},
  };
  grammar.productions_by_child = {
      {production_id{0}, production_id{2}},
      {production_id{0}, production_id{3}},
      {production_id{1}, production_id{2}},
      {production_id{1}, production_id{3}},
      {production_id{4}},
      {production_id{4}},
      {production_id{5}},
      {production_id{5}},
      {},
  };
  grammar.root_clade = clade_id{8};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

static larch::clade_grammar make_nonlex_multifurcating_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};
  grammar.taxa.sample_id_to_id = {{"A", taxon_id{0}},
                                  {"B", taxon_id{1}},
                                  {"C", taxon_id{2}},
                                  {"D", taxon_id{3}}};
  grammar.clades = {{{taxon_id{0}}},
                    {{taxon_id{1}}},
                    {{taxon_id{2}}},
                    {{taxon_id{3}}},
                    {{taxon_id{2}, taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}, taxon_id{3}}}};
  grammar.productions = {
      larch::grammar_production{clade_id{4}, {clade_id{2}, clade_id{3}}, {}, 1},
      larch::grammar_production{
          clade_id{5}, {clade_id{0}, clade_id{1}, clade_id{4}}, {}, 1},
  };
  grammar.productions_by_parent = {
      {}, {}, {}, {}, {production_id{0}}, {production_id{1}},
  };
  grammar.productions_by_child = {
      {production_id{1}}, {production_id{1}}, {production_id{0}},
      {production_id{0}}, {production_id{1}}, {},
  };
  grammar.root_clade = clade_id{5};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

// The first top-down nonroot occurrences are binary, while the later A/B/C
// leaves occur under a trinary parent. This makes a first-item outside
// preflight strictly smaller than the plan-wide singleton requirement.
static larch::clade_grammar make_asymmetric_outside_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};
  grammar.taxa.sample_id_to_id = {{"A", taxon_id{0}},
                                  {"B", taxon_id{1}},
                                  {"C", taxon_id{2}},
                                  {"D", taxon_id{3}}};
  grammar.clades = {{{taxon_id{0}}},
                    {{taxon_id{1}}},
                    {{taxon_id{2}}},
                    {{taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}, taxon_id{3}}}};
  grammar.productions = {
      larch::grammar_production{
          clade_id{4}, {clade_id{0}, clade_id{1}, clade_id{2}}, {}, 1},
      larch::grammar_production{clade_id{5}, {clade_id{4}, clade_id{3}}, {}, 1},
  };
  grammar.productions_by_parent = {
      {}, {}, {}, {}, {production_id{0}}, {production_id{1}},
  };
  grammar.productions_by_child = {
      {production_id{0}}, {production_id{0}}, {production_id{0}},
      {production_id{1}}, {production_id{1}}, {},
  };
  grammar.root_clade = clade_id{5};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

// AB feeds two level-2 parents. A sparse scheduled build must keep AB's maps
// readable until both ABC and ABD have completed their level, then reclaim the
// shared dependency on the coordinator.
static larch::clade_grammar make_shared_internal_child_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D", "E"};
  grammar.taxa.sample_id_to_id = {{"A", taxon_id{0}},
                                  {"B", taxon_id{1}},
                                  {"C", taxon_id{2}},
                                  {"D", taxon_id{3}},
                                  {"E", taxon_id{4}}};
  grammar.clades = {
      {{taxon_id{0}}},
      {{taxon_id{1}}},
      {{taxon_id{2}}},
      {{taxon_id{3}}},
      {{taxon_id{4}}},
      {{taxon_id{0}, taxon_id{1}}},
      {{taxon_id{3}, taxon_id{4}}},
      {{taxon_id{2}, taxon_id{4}}},
      {{taxon_id{0}, taxon_id{1}, taxon_id{2}}},
      {{taxon_id{0}, taxon_id{1}, taxon_id{3}}},
      {{taxon_id{0}, taxon_id{1}, taxon_id{2}, taxon_id{3}, taxon_id{4}}}};
  grammar.productions = {
      larch::grammar_production{clade_id{5}, {clade_id{0}, clade_id{1}}, {}, 1},
      larch::grammar_production{clade_id{6}, {clade_id{3}, clade_id{4}}, {}, 1},
      larch::grammar_production{clade_id{7}, {clade_id{2}, clade_id{4}}, {}, 1},
      larch::grammar_production{clade_id{8}, {clade_id{5}, clade_id{2}}, {}, 1},
      larch::grammar_production{clade_id{9}, {clade_id{5}, clade_id{3}}, {}, 1},
      larch::grammar_production{
          clade_id{10}, {clade_id{8}, clade_id{6}}, {}, 1},
      larch::grammar_production{
          clade_id{10}, {clade_id{9}, clade_id{7}}, {}, 1},
  };
  grammar.productions_by_parent = {
      {},
      {},
      {},
      {},
      {},
      {production_id{0}},
      {production_id{1}},
      {production_id{2}},
      {production_id{3}},
      {production_id{4}},
      {production_id{5}, production_id{6}},
  };
  grammar.productions_by_child = {
      {production_id{0}},
      {production_id{0}},
      {production_id{2}, production_id{3}},
      {production_id{1}, production_id{4}},
      {production_id{1}, production_id{2}},
      {production_id{3}, production_id{4}},
      {production_id{5}},
      {production_id{6}},
      {production_id{5}},
      {production_id{6}},
      {},
  };
  grammar.root_clade = clade_id{10};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

static larch::site_pattern_set make_nonlex_four_taxon_patterns() {
  larch::site_pattern_set patterns;
  patterns.taxon_count = 4;
  patterns.patterns = {
      larch::site_pattern{.state_by_taxon = {3, 3, 3, 3},
                          .positions = {},
                          .weight = 1,
                          .reference_state_counts = {}},
      larch::site_pattern{.state_by_taxon = {0, 0, 3, 3},
                          .positions = {},
                          .weight = 2,
                          .reference_state_counts = {}},
      larch::site_pattern{.state_by_taxon = {3, 3, 0, 0},
                          .positions = {},
                          .weight = 3,
                          .reference_state_counts = {}},
      larch::site_pattern{.state_by_taxon = {0, 3, 0, 3},
                          .positions = {},
                          .weight = 4,
                          .reference_state_counts = {}},
      larch::site_pattern{.state_by_taxon = {3, 0, 3, 0},
                          .positions = {},
                          .weight = 5,
                          .reference_state_counts = {}},
      larch::site_pattern{.state_by_taxon = {1, 2, 3, 0},
                          .positions = {},
                          .weight = 6,
                          .reference_state_counts = {}},
  };
  return patterns;
}

static larch::site_pattern_set make_wide_star_patterns(
    std::size_t taxon_count = 64, std::size_t pattern_count = 32) {
  larch::site_pattern_set patterns;
  patterns.taxon_count = taxon_count;
  patterns.patterns.reserve(pattern_count);
  for (std::size_t pattern = 0; pattern < pattern_count; ++pattern) {
    std::vector<std::uint8_t> states;
    states.reserve(taxon_count);
    for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
      states.push_back(static_cast<std::uint8_t>(
          (taxon * 3 + pattern * 5 + taxon / 7) % larch::nuc_state_count));
    }
    patterns.patterns.push_back(larch::site_pattern{
        .state_by_taxon = std::move(states),
        .weight = static_cast<std::uint32_t>(pattern % 5 + 1),
    });
  }
  return patterns;
}

static larch::site_pattern_set make_weighted_patterns(
    std::size_t taxon_count) {
  larch::site_pattern_set patterns;
  patterns.taxon_count = taxon_count;
  if (taxon_count == 5) {
    patterns.patterns = {
        larch::site_pattern{.state_by_taxon = {0, 0, 1, 2, 3},
                            .weight = 2,
                            .reference_state_counts = {1, 1, 0, 0}},
        larch::site_pattern{.state_by_taxon = {0, 1, 0, 1, 2},
                            .weight = 1,
                            .reference_state_counts = {0, 0, 1, 0}},
        larch::site_pattern{.state_by_taxon = {3, 2, 1, 0, 3},
                            .weight = 3,
                            .reference_state_counts = {0, 1, 0, 2}},
        larch::site_pattern{.state_by_taxon = {1, 1, 1, 3, 3},
                            .weight = 4,
                            .reference_state_counts = {0, 4, 0, 0}}};
  } else if (taxon_count == 4) {
    patterns.patterns = {
        larch::site_pattern{.state_by_taxon = {0, 0, 1, 1},
                            .weight = 2,
                            .reference_state_counts = {1, 1, 0, 0}},
        larch::site_pattern{.state_by_taxon = {0, 1, 0, 1},
                            .weight = 1,
                            .reference_state_counts = {0, 0, 1, 0}},
        larch::site_pattern{.state_by_taxon = {3, 2, 1, 0},
                            .weight = 3,
                            .reference_state_counts = {0, 1, 0, 2}}};
  } else if (taxon_count == 3) {
    patterns.patterns = {
        larch::site_pattern{.state_by_taxon = {0, 1, 2},
                            .weight = 2,
                            .reference_state_counts = {1, 0, 1, 0}},
        larch::site_pattern{.state_by_taxon = {3, 3, 0},
                            .weight = 1,
                            .reference_state_counts = {0, 0, 0, 1}}};
  } else if (taxon_count == 1) {
    patterns.patterns = {
        larch::site_pattern{.state_by_taxon = {0},
                            .weight = 2,
                            .reference_state_counts = {2, 0, 0, 0}},
        larch::site_pattern{.state_by_taxon = {3},
                            .weight = 1,
                            .reference_state_counts = {0, 0, 0, 1}}};
  } else {
    CHECK(false && "unsupported test pattern taxon count");
  }
  return patterns;
}

static void check_charts_equal(larch::single_site_chart const& expected,
                               larch::single_site_chart const& actual) {
  CHECK(expected.inside == actual.inside);
  CHECK(expected.trace_choice_count == actual.trace_choice_count);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
  CHECK(expected.optimal_choices.size() == actual.optimal_choices.size());
  for (std::size_t clade = 0; clade < expected.optimal_choices.size();
       ++clade) {
    for (std::size_t state = 0; state < larch::nuc_state_count; ++state) {
      auto const& lhs = expected.optimal_choices[clade][state];
      auto const& rhs = actual.optimal_choices[clade][state];
      CHECK(lhs.size() == rhs.size());
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        CHECK(lhs[i].production == rhs[i].production);
        CHECK(lhs[i].child_states == rhs[i].child_states);
        CHECK(lhs[i].cost == rhs[i].cost);
      }
    }
  }
}

static void check_outside_charts_equal(
    larch::single_site_outside_chart const& expected,
    larch::single_site_outside_chart const& actual) {
  CHECK(expected.outside == actual.outside);
  CHECK(expected.global_min == actual.global_min);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
}

static void check_composite_scores_equal(
    larch::composite_chart_score const& expected,
    larch::composite_chart_score const& actual) {
  CHECK(expected.weighted_lower_bound == actual.weighted_lower_bound);
  CHECK(expected.per_pattern_root_min == actual.per_pattern_root_min);
  CHECK(expected.per_pattern_root_min_by_reference_state ==
        actual.per_pattern_root_min_by_reference_state);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
}

static void check_lazy_charts_equal(
    larch::lazy_multisite_chart const& expected,
    larch::lazy_multisite_chart const& actual) {
  CHECK(expected.inside_rows_by_clade == actual.inside_rows_by_clade);
  CHECK(expected.outside_rows_by_clade == actual.outside_rows_by_clade);
  CHECK(expected.class_index_by_pattern_by_clade ==
        actual.class_index_by_pattern_by_clade);
  CHECK(expected.structural_class_index_by_pattern_by_clade ==
        actual.structural_class_index_by_pattern_by_clade);
  CHECK(expected.outside_class_index_by_pattern_by_clade ==
        actual.outside_class_index_by_pattern_by_clade);
  CHECK(expected.structural_class_count_by_clade ==
        actual.structural_class_count_by_clade);
  CHECK(expected.class_weight_by_clade == actual.class_weight_by_clade);
  CHECK(expected.outside_class_weight_by_clade ==
        actual.outside_class_weight_by_clade);
  CHECK(expected.outside_global_min_by_pattern ==
        actual.outside_global_min_by_pattern);
  CHECK(expected.pattern_count == actual.pattern_count);
  CHECK(expected.total_pattern_weight == actual.total_pattern_weight);
  CHECK(expected.lazy_inside_rows_computed ==
        actual.lazy_inside_rows_computed);
  CHECK(expected.lazy_outside_rows_computed ==
        actual.lazy_outside_rows_computed);
  CHECK(expected.lazy_patterns_merged_max ==
        actual.lazy_patterns_merged_max);
  CHECK(expected.lazy_remerge_collisions == actual.lazy_remerge_collisions);
  CHECK(expected.lazy_structural_class_count_max ==
        actual.lazy_structural_class_count_max);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
  CHECK(expected.outside_multifurcation_productions_scored ==
        actual.outside_multifurcation_productions_scored);
  CHECK(expected.outside_recurrence_work == actual.outside_recurrence_work);
}

static void check_multisite_trim_equal(
    larch::multisite_trim_result const& expected,
    larch::multisite_trim_result const& actual) {
  CHECK(expected.optimum == actual.optimum);
  CHECK(expected.composite_lower_bound == actual.composite_lower_bound);
  CHECK(expected.initial_upper_bound == actual.initial_upper_bound);
  CHECK(expected.keep_production == actual.keep_production);
  CHECK(expected.frontier_sizes_by_clade == actual.frontier_sizes_by_clade);
  CHECK(expected.dominance_mode == actual.dominance_mode);
  CHECK(expected.keep_mask_kind == actual.keep_mask_kind);
  CHECK(expected.keep_production_exact == actual.keep_production_exact);
  CHECK(expected.dominance_candidates_considered ==
        actual.dominance_candidates_considered);
  CHECK(expected.dominance_pruned_score_pass ==
        actual.dominance_pruned_score_pass);
  CHECK(expected.dominance_pruned_mask_pass ==
        actual.dominance_pruned_mask_pass);
  CHECK(expected.dominance_pruned == actual.dominance_pruned);
  CHECK(expected.exact_mask_recovery_passes ==
        actual.exact_mask_recovery_passes);
  CHECK(expected.bound_pruned == actual.bound_pruned);
  CHECK(expected.equality_deduplicated == actual.equality_deduplicated);
  CHECK(expected.active_pattern_count == actual.active_pattern_count);
  CHECK(expected.invariant_constant_offset ==
        actual.invariant_constant_offset);
  CHECK(expected.lazy_chart_used == actual.lazy_chart_used);
  CHECK(expected.lazy_inside_rows_computed ==
        actual.lazy_inside_rows_computed);
  CHECK(expected.lazy_outside_rows_computed ==
        actual.lazy_outside_rows_computed);
  CHECK(expected.lazy_patterns_merged_max ==
        actual.lazy_patterns_merged_max);
  CHECK(expected.lazy_remerge_collisions == actual.lazy_remerge_collisions);
  CHECK(expected.lazy_structural_class_count_max ==
        actual.lazy_structural_class_count_max);
  CHECK(expected.lazy_structural_class_count_by_clade ==
        actual.lazy_structural_class_count_by_clade);
  CHECK(expected.optimal_root_provenance_classes ==
        actual.optimal_root_provenance_classes);
}

static void test_execution_plan_binary_and_generic_invariants() {
  std::println("test_execution_plan_binary_and_generic_invariants");
  auto binary = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(binary);

  CHECK(binary.execution_generation != 0);
  CHECK(plan.grammar_generation() == binary.execution_generation);
  CHECK(plan.root_clade() == binary.root_clade);
  CHECK(plan.taxon_count() == 4);
  CHECK(plan.all_binary());
  CHECK(plan.max_arity() == 2);
  CHECK(plan.build_stats().plan_builds == 1);
  CHECK(plan.build_stats().full_grammar_validations == 1);
  CHECK(plan.build_stats().production_partition_validations ==
        binary.productions.size());
  CHECK(plan.build_stats().clade_order_sorts == 2);
  CHECK(plan.build_stats().production_descriptors_compiled ==
        binary.productions.size());
  CHECK(std::vector<larch::clade_id>(plan.bottom_up_order().begin(),
                                     plan.bottom_up_order().end()) ==
        std::vector<larch::clade_id>({0, 1, 2, 3, 4, 5, 6}));
  CHECK(std::vector<larch::clade_id>(plan.top_down_order().begin(),
                                     plan.top_down_order().end()) ==
        std::vector<larch::clade_id>({6, 4, 5, 0, 1, 2, 3}));

  std::vector<bool> seen(plan.clades().size(), false);
  for (auto cid : plan.bottom_up_level_order()) {
    CHECK(cid < seen.size());
    CHECK(!seen[cid]);
    seen[cid] = true;
  }
  for (bool value : seen) CHECK(value);
  for (auto const& production : plan.productions()) {
    CHECK(production.source_id < binary.productions.size());
    CHECK(production.parent == binary.productions[production.source_id].parent);
    auto children = plan.children(production.source_id);
    CHECK(std::vector<larch::clade_id>(children.begin(), children.end()) ==
          binary.productions[production.source_id].children);
    for (auto child : children) {
      CHECK(plan.clade(child).dependency_level <
            plan.clade(production.parent).dependency_level);
    }
  }
  for (larch::clade_id child = 0; child < plan.clades().size(); ++child) {
    auto occurrences = plan.child_occurrences_for_clade(child);
    CHECK(occurrences.size() == binary.productions_by_child[child].size());
    for (std::size_t i = 0; i < occurrences.size(); ++i) {
      auto const& occurrence = occurrences[i];
      CHECK(occurrence.production == binary.productions_by_child[child][i]);
      auto children = plan.children(occurrence.production);
      CHECK(occurrence.child_slot < children.size());
      CHECK(children[occurrence.child_slot] == child);
    }
  }
  for (std::uint8_t parent = 0; parent < 4; ++parent) {
    for (std::uint8_t child = 0; child < 4; ++child) {
      CHECK(plan.transition_cost(parent, child) == (parent == child ? 0 : 1));
    }
  }

  auto trinary = make_trinary_grammar();
  auto generic = larch::build_chart_execution_plan(trinary);
  CHECK(!generic.all_binary());
  CHECK(generic.max_arity() == 3);
  CHECK(generic.production(0).child_count == 3);
  CHECK(std::vector<larch::clade_id>(generic.children(0).begin(),
                                     generic.children(0).end()) ==
        std::vector<larch::clade_id>({0, 1, 2}));
  for (larch::clade_id child = 0; child < 3; ++child) {
    auto occurrences = generic.child_occurrences_for_clade(child);
    CHECK(occurrences.size() == 1);
    CHECK(occurrences.front().production == 0);
    CHECK(occurrences.front().child_slot == child);
  }
}

static void test_checked_and_plan_dense_equivalence() {
  std::println("test_checked_and_plan_dense_equivalence");
  auto binary = make_binary_grammar();
  auto binary_plan = larch::build_chart_execution_plan(binary);
  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};
  for (std::uint32_t mask = 0; mask < 256; ++mask) {
    larch::leaf_site_states states;
    states.state_by_taxon.resize(4);
    auto value = mask;
    for (auto& state : states.state_by_taxon) {
      state = static_cast<std::uint8_t>(value & 3U);
      value >>= 2U;
    }
    auto checked = larch::build_single_site_chart(binary, states, false);
    larch::single_site_chart planned;
    larch::single_site_chart planned_from_view;
    auto const* state_storage = states.state_by_taxon.data();
    auto state_view = larch::view_leaf_site_states(states);
    CHECK(state_view.state_by_taxon.data() == state_storage);
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned = larch::build_single_site_chart(binary_plan, states, false);
      planned_from_view =
          larch::build_single_site_chart(binary_plan, state_view, false);
    }
    check_charts_equal(checked, planned);
    check_charts_equal(checked, planned_from_view);
    CHECK(states.state_by_taxon.data() == state_storage);

    auto checked_outside =
        larch::build_single_site_outside_chart(binary, checked);
    larch::single_site_outside_chart planned_outside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_outside =
          larch::build_single_site_outside_chart(binary_plan, planned);
    }
    check_outside_charts_equal(checked_outside, planned_outside);
    for (std::uint8_t reference = 0; reference < 4; ++reference) {
      larch::chart_options root_edge_options;
      root_edge_options.score_ua_edge = true;
      auto checked_with_reference = larch::build_single_site_outside_chart(
          binary, checked, root_edge_options, reference);
      larch::single_site_outside_chart planned_with_reference;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned_with_reference = larch::build_single_site_outside_chart(
            binary_plan, planned, root_edge_options, reference);
      }
      check_outside_charts_equal(checked_with_reference,
                                 planned_with_reference);
    }

    larch::chart_options trace_options;
    trace_options.keep_trace = true;
    auto checked_trace =
        larch::build_single_site_chart(binary, states, trace_options);
    larch::single_site_chart planned_trace;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_trace =
          larch::build_single_site_chart(binary_plan, states, trace_options);
    }
    check_charts_equal(checked_trace, planned_trace);
  }

  auto trinary = make_trinary_grammar();
  auto trinary_plan = larch::build_chart_execution_plan(trinary);
  for (std::uint32_t mask = 0; mask < 64; ++mask) {
    larch::leaf_site_states states;
    states.state_by_taxon.resize(3);
    auto value = mask;
    for (auto& state : states.state_by_taxon) {
      state = static_cast<std::uint8_t>(value & 3U);
      value >>= 2U;
    }
    auto checked = larch::build_single_site_chart(trinary, states, false);
    larch::single_site_chart planned;
    larch::single_site_chart planned_from_view;
    auto const* state_storage = states.state_by_taxon.data();
    auto state_view = larch::view_leaf_site_states(states);
    CHECK(state_view.state_by_taxon.data() == state_storage);
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned = larch::build_single_site_chart(trinary_plan, states, false);
      planned_from_view =
          larch::build_single_site_chart(trinary_plan, state_view, false);
    }
    check_charts_equal(checked, planned);
    check_charts_equal(checked, planned_from_view);
    CHECK(states.state_by_taxon.data() == state_storage);
    auto checked_outside =
        larch::build_single_site_outside_chart(trinary, checked);
    larch::single_site_outside_chart planned_outside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_outside =
          larch::build_single_site_outside_chart(trinary_plan, planned);
    }
    check_outside_charts_equal(checked_outside, planned_outside);
  }
  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void test_stale_and_mismatched_plan_rejected() {
  std::println("test_stale_and_mismatched_plan_rejected");
  auto first = make_binary_grammar();
  auto second = make_binary_grammar();
  auto first_plan = larch::build_chart_execution_plan(first);
  auto second_plan = larch::build_chart_execution_plan(second);
  CHECK(first_plan.grammar_generation() != second_plan.grammar_generation());

  auto mismatch =
      runtime_error_message([&] { first_plan.assert_compatible(second); });
  CHECK(mismatch.find("chart execution plan") != std::string::npos);
  CHECK(mismatch.find("generation mismatch") != std::string::npos);

  std::swap(first.productions[0].children[0], first.productions[0].children[1]);
  auto stale =
      runtime_error_message([&] { first_plan.assert_compatible(first); });
  CHECK(stale.find("chart execution plan") != std::string::npos);
  CHECK(stale.find("fingerprint mismatch") != std::string::npos);

  auto reverse_registry = make_binary_grammar();
  auto reverse_registry_plan =
      larch::build_chart_execution_plan(reverse_registry);
  reverse_registry.taxa.sample_id_to_id["A"] = 1;
  auto reverse_stale = runtime_error_message(
      [&] { reverse_registry_plan.assert_compatible(reverse_registry); });
  CHECK(reverse_stale.find("fingerprint mismatch") != std::string::npos);
}

static void test_checked_and_plan_composite_equivalence() {
  std::println("test_checked_and_plan_composite_equivalence");
  for (bool trinary : {false, true}) {
    auto grammar = trinary ? make_trinary_grammar() : make_binary_grammar();
    auto plan = larch::build_chart_execution_plan(grammar);
    auto patterns = make_weighted_patterns(plan.taxon_count());
    for (bool score_ua_edge : {false, true}) {
      larch::chart_options options;
      options.score_ua_edge = score_ua_edge;
      auto checked =
          larch::build_composite_chart_score(grammar, patterns, options);
      std::size_t full_validations = 0;
      std::size_t partition_validations = 0;
      std::size_t clade_sorts = 0;
      larch::parsimony_chart_detail::structural_work_observer observer{
          &full_validations, &partition_validations, &clade_sorts};
      larch::composite_chart_score planned;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned =
            larch::build_composite_chart_score(plan, patterns, options);
      }
      check_composite_scores_equal(checked, planned);
      CHECK(full_validations == 0);
      CHECK(partition_validations == 0);
      CHECK(clade_sorts == 0);
    }
  }
}

static void test_checked_and_plan_lazy_inside_outside_equivalence() {
  std::println("test_checked_and_plan_lazy_inside_outside_equivalence");
  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};

  for (bool retain_all : {false, true}) {
    larch::lazy_chart_options lazy_options;
    lazy_options.retain_all_inside_class_maps = retain_all;
    auto checked_inside =
        larch::build_lazy_inside_chart(grammar, patterns, lazy_options);
    larch::lazy_multisite_chart planned_inside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_inside =
          larch::build_lazy_inside_chart(plan, patterns, lazy_options);
    }
    check_lazy_charts_equal(checked_inside, planned_inside);

    for (larch::clade_id clade = 0; clade < plan.clades().size(); ++clade) {
      auto const should_retain = retain_all || clade == plan.root_clade();
      CHECK(planned_inside.class_index_by_pattern_by_clade[clade].has_value() ==
            should_retain);
      CHECK(planned_inside
                .structural_class_index_by_pattern_by_clade[clade]
                .has_value() == should_retain);
    }

    auto checked_outside = larch::build_lazy_outside_chart(
        grammar, patterns, checked_inside, larch::chart_options{});
    larch::lazy_multisite_chart planned_outside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_outside = larch::build_lazy_outside_chart(
          plan, patterns, planned_inside, larch::chart_options{});
    }
    check_lazy_charts_equal(checked_outside, planned_outside);
    for (auto const& class_map :
         planned_outside.outside_class_index_by_pattern_by_clade) {
      CHECK(class_map.has_value());
    }

    larch::chart_options reference_options;
    reference_options.score_ua_edge = true;
    for (std::uint8_t reference_state = 0;
         reference_state < larch::nuc_state_count; ++reference_state) {
      auto checked_with_reference = larch::build_lazy_outside_chart(
          grammar, patterns, checked_inside, reference_options,
          reference_state);
      larch::lazy_multisite_chart planned_with_reference;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned_with_reference = larch::build_lazy_outside_chart(
            plan, patterns, planned_inside, reference_options,
            reference_state);
      }
      check_lazy_charts_equal(checked_with_reference,
                              planned_with_reference);
    }
  }

  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void test_checked_and_plan_lazy_score_and_exact_trim_equivalence() {
  std::println(
      "test_checked_and_plan_lazy_score_and_exact_trim_equivalence");
  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};

  for (bool retain_all : {false, true}) {
    larch::lazy_chart_options lazy_options;
    lazy_options.retain_all_inside_class_maps = retain_all;
    auto checked_lazy =
        larch::build_lazy_inside_chart(grammar, patterns, lazy_options);
    larch::lazy_multisite_chart planned_lazy;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_lazy = larch::build_lazy_inside_chart(plan, patterns,
                                                    lazy_options);
    }

    for (bool score_ua_edge : {false, true}) {
      larch::chart_options options;
      options.score_ua_edge = score_ua_edge;
      auto checked_score = larch::lazy_composite_chart_score(
          grammar, patterns, checked_lazy, options);
      auto const checked_lower_bound = larch::lazy_composite_lower_bound(
          grammar, patterns, checked_lazy, options);
      larch::composite_chart_score planned_score;
      std::uint64_t planned_lower_bound = 0;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned_score = larch::lazy_composite_chart_score(
            plan, patterns, planned_lazy, options);
        planned_lower_bound = larch::lazy_composite_lower_bound(
            plan, patterns, planned_lazy, options);
      }
      check_composite_scores_equal(checked_score, planned_score);
      CHECK(checked_lower_bound == planned_lower_bound);
      CHECK(planned_lower_bound == planned_score.weighted_lower_bound);
    }
  }

  auto checked_lazy = larch::build_lazy_inside_chart(grammar, patterns);
  larch::lazy_multisite_chart planned_lazy;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_lazy = larch::build_lazy_inside_chart(plan, patterns);
  }
  larch::multisite_trim_options trim_options;
  trim_options.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  trim_options.use_bound_pruning = true;
  trim_options.capture_optimal_root_provenance = true;
  for (bool score_ua_edge : {false, true}) {
    larch::chart_options options;
    options.score_ua_edge = score_ua_edge;
    auto checked_trim = larch::build_multisite_trim(
        grammar, patterns, checked_lazy, options, trim_options);
    larch::multisite_trim_result planned_trim;
    larch::multisite_trim_result bridged_trim;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_trim = larch::build_multisite_trim(
          plan, patterns, planned_lazy, options, trim_options);
      bridged_trim = larch::build_multisite_trim(
          grammar, plan, patterns, planned_lazy, options, trim_options);
    }
    check_multisite_trim_equal(checked_trim, planned_trim);
    check_multisite_trim_equal(checked_trim, bridged_trim);
    CHECK(planned_trim.lazy_chart_used);
    CHECK(!planned_trim.optimal_root_provenance_classes.empty());
  }

  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void test_checked_and_plan_lazy_multifurcation_equivalence() {
  std::println("test_checked_and_plan_lazy_multifurcation_equivalence");
  auto grammar = make_trinary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};

  auto checked_inside = larch::build_lazy_inside_chart(grammar, patterns);
  larch::lazy_multisite_chart planned_inside;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_inside = larch::build_lazy_inside_chart(plan, patterns);
  }
  check_lazy_charts_equal(checked_inside, planned_inside);
  CHECK(planned_inside.multifurcation_productions_scored > 0);

  auto checked_outside = larch::build_lazy_outside_chart(
      grammar, patterns, checked_inside, larch::chart_options{});
  larch::lazy_multisite_chart planned_outside;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_outside = larch::build_lazy_outside_chart(
        plan, patterns, planned_inside, larch::chart_options{});
  }
  check_lazy_charts_equal(checked_outside, planned_outside);
  CHECK(planned_outside.outside_multifurcation_productions_scored > 0);

  auto const checked_error = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns, checked_inside);
  });
  std::string planned_error;
  std::string bridged_error;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_error = runtime_error_message([&] {
      (void)larch::build_multisite_trim(plan, patterns, planned_inside);
    });
    bridged_error = runtime_error_message([&] {
      (void)larch::build_multisite_trim(grammar, plan, patterns,
                                        planned_inside);
    });
  }
  CHECK(checked_error.find("WI6 arity gate") != std::string::npos);
  CHECK(planned_error == checked_error);
  CHECK(bridged_error == checked_error);

  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void check_deliberately_nonlex_root_classes(
    larch::chart_execution_plan const& plan,
    larch::lazy_multisite_chart const& chart) {
  auto const root = plan.root_clade();
  auto const production_ids = plan.productions_for_parent(root);
  CHECK(!production_ids.empty());
  auto const structural_children = plan.children(production_ids.front());
  CHECK(!structural_children.empty());

  std::vector<std::size_t> first_seen_key;
  std::vector<std::size_t> lexicographically_earlier_key;
  for (auto child : structural_children) {
    auto const& child_map =
        chart.structural_class_index_by_pattern_by_clade[child];
    CHECK(child_map.has_value());
    first_seen_key.push_back((*child_map)[1]);
    lexicographically_earlier_key.push_back((*child_map)[2]);
  }
  CHECK(std::lexicographical_compare(lexicographically_earlier_key.begin(),
                                     lexicographically_earlier_key.end(),
                                     first_seen_key.begin(),
                                     first_seen_key.end()));

  auto const& root_map = chart.structural_class_index_by_pattern_by_clade[root];
  CHECK(root_map.has_value());
  CHECK((*root_map)[1] == 1);
  CHECK((*root_map)[2] == 2);
  CHECK((*root_map)[1] < (*root_map)[2]);
}

static void check_nonlex_packed_plan_against_grammar_oracle(
    larch::clade_grammar grammar, bool expect_multifurcation) {
  auto const plan = larch::build_chart_execution_plan(grammar);
  auto const patterns = make_nonlex_four_taxon_patterns();

  for (bool retain_all : {false, true}) {
    larch::lazy_chart_options options;
    options.retain_all_inside_class_maps = retain_all;
    auto const oracle_inside =
        larch::build_lazy_inside_chart(grammar, patterns, options);
    auto const packed_inside =
        larch::build_lazy_inside_chart(plan, patterns, options);
    check_lazy_charts_equal(oracle_inside, packed_inside);
    CHECK((packed_inside.multifurcation_productions_scored > 0) ==
          expect_multifurcation);
    if (retain_all) {
      check_deliberately_nonlex_root_classes(plan, oracle_inside);
      check_deliberately_nonlex_root_classes(plan, packed_inside);
    }

    auto const oracle_outside = larch::build_lazy_outside_chart(
        grammar, patterns, oracle_inside, larch::chart_options{});
    auto const packed_outside = larch::build_lazy_outside_chart(
        plan, patterns, packed_inside, larch::chart_options{});
    check_lazy_charts_equal(oracle_outside, packed_outside);
    check_deliberately_nonlex_root_classes(plan, oracle_outside);
    check_deliberately_nonlex_root_classes(plan, packed_outside);
    CHECK((packed_outside.outside_multifurcation_productions_scored > 0) ==
          expect_multifurcation);
  }
}

static void test_nonlex_packed_plan_lazy_grouping_equivalence() {
  std::println("test_nonlex_packed_plan_lazy_grouping_equivalence");
  check_nonlex_packed_plan_against_grammar_oracle(
      make_nonlex_binary_dag_grammar(), false);
  check_nonlex_packed_plan_against_grammar_oracle(
      make_nonlex_multifurcating_grammar(), true);
}

static void test_scheduled_plan_lazy_dependency_wavefronts() {
  std::println("test_scheduled_plan_lazy_dependency_wavefronts");

  auto const grammar = make_shared_internal_child_grammar();
  auto const plan = larch::build_chart_execution_plan(grammar);
  auto const patterns = make_weighted_patterns(plan.taxon_count());
  larch::lazy_chart_options options;
  options.retain_all_inside_class_maps = false;

  auto serial = larch::build_lazy_inside_chart(plan, patterns, options);
  larch::build_lazy_outside_chart_in_place(plan, patterns, serial,
                                           larch::chart_options{});

  larch::chart_scheduler w1{larch::chart_scheduler_options{
      .requested_workers = 1,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace w1_workspace;
  std::vector<larch::chart_scheduler_run_summary> w1_inside_runs;
  std::vector<larch::chart_scheduler_run_summary> w1_outside_runs;
  auto w1_chart = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, w1, &w1_inside_runs, nullptr, &w1_workspace);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, w1_chart, larch::chart_options{}, w1, &w1_outside_runs,
      nullptr, &w1_workspace);
  check_lazy_charts_equal(serial, w1_chart);
  CHECK(w1_inside_runs.size() == plan.bottom_up_level_offsets().size() - 1);
  CHECK(w1_outside_runs.size() + 1 == plan.top_down_level_offsets().size() - 1);
  CHECK(std::ranges::none_of(w1_inside_runs, [](auto const& run) {
    return run.used_parallel_workers();
  }));
  CHECK(std::ranges::none_of(w1_outside_runs, [](auto const& run) {
    return run.used_parallel_workers();
  }));

  larch::chart_scheduler w4{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace w4_workspace;
  std::atomic<std::size_t> active_shared_parents = 0;
  std::atomic<std::size_t> active_shared_parents_high_water = 0;
  std::atomic<bool> abd_finished = false;
  std::atomic<bool> abc_observed_abd_finished = false;
  std::atomic<std::size_t> shared_reader_maps_seen = 0;
  bool shared_map_present_at_level_join = false;
  bool shared_map_reclaimed_after_level = false;
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks overlap_hooks;
  overlap_hooks.before_inside_clade =
      [&](larch::clade_id clade, std::size_t level, std::size_t stable_slot) {
        CHECK(stable_slot < w4.worker_resolution().resolved_workers);
        if (level != 2) return;
        auto const active = active_shared_parents.fetch_add(1) + 1;
        auto high = active_shared_parents_high_water.load();
        while (active > high &&
               !active_shared_parents_high_water.compare_exchange_weak(
                   high, active)) {
        }
        if (clade != larch::clade_id{8}) return;
        for (std::size_t attempt = 0;
             attempt < 1'000'000 && !abd_finished.load(); ++attempt) {
          std::this_thread::yield();
        }
        abc_observed_abd_finished = abd_finished.load();
      };
  overlap_hooks.after_inside_clade =
      [&](larch::clade_id clade, std::size_t level, std::size_t stable_slot) {
        CHECK(stable_slot < w4.worker_resolution().resolved_workers);
        if (level != 2) return;
        if (clade == larch::clade_id{9}) abd_finished = true;
        active_shared_parents.fetch_sub(1);
      };
  overlap_hooks.observe_completed_inside_clade =
      [&](larch::lazy_multisite_chart const& chart, larch::clade_id clade,
          std::size_t level, std::size_t stable_slot) {
        CHECK(stable_slot < w4.worker_resolution().resolved_workers);
        if (level != 2 ||
            (clade != larch::clade_id{8} && clade != larch::clade_id{9})) {
          return;
        }
        CHECK(chart.class_index_by_pattern_by_clade[5].has_value());
        CHECK(chart.structural_class_index_by_pattern_by_clade[5].has_value());
        ++shared_reader_maps_seen;
      };
  overlap_hooks.observe_inside_level_join =
      [&](larch::lazy_multisite_chart const& chart, std::size_t level) {
        if (level != 2) return;
        shared_map_present_at_level_join =
            chart.class_index_by_pattern_by_clade[5].has_value() &&
            chart.structural_class_index_by_pattern_by_clade[5].has_value();
      };
  overlap_hooks.observe_inside_level_reclamation =
      [&](larch::lazy_multisite_chart const& chart, std::size_t level) {
        if (level != 2) return;
        shared_map_reclaimed_after_level =
            !chart.class_index_by_pattern_by_clade[5].has_value() &&
            !chart.structural_class_index_by_pattern_by_clade[5].has_value();
      };
  std::vector<larch::chart_scheduler_run_summary> w4_inside_runs;
  std::vector<larch::chart_scheduler_run_summary> w4_outside_runs;
  auto w4_chart = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, w4, &w4_inside_runs, &overlap_hooks,
      &w4_workspace);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, w4_chart, larch::chart_options{}, w4, &w4_outside_runs,
      &overlap_hooks, &w4_workspace);
  check_lazy_charts_equal(serial, w4_chart);
  CHECK(abc_observed_abd_finished.load());
  CHECK(active_shared_parents_high_water.load() >= 2);
  CHECK(shared_reader_maps_seen.load() == 2);
  CHECK(shared_map_present_at_level_join);
  CHECK(shared_map_reclaimed_after_level);
  CHECK(std::ranges::any_of(w4_inside_runs, [](auto const& run) {
    return run.used_parallel_workers() && run.worker_tasks_submitted > 1;
  }));
  CHECK(std::ranges::any_of(w4_outside_runs, [](auto const& run) {
    return run.used_parallel_workers() && run.worker_tasks_submitted > 1;
  }));

  larch::chart_scheduler w2{larch::chart_scheduler_options{
      .requested_workers = 2,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace w2_workspace;
  auto w2_chart = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, w2, nullptr, nullptr, &w2_workspace);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, w2_chart, larch::chart_options{}, w2, nullptr, nullptr,
      &w2_workspace);
  check_lazy_charts_equal(serial, w2_chart);

  larch::chart_scheduler w8{larch::chart_scheduler_options{
      .requested_workers = 8,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace w8_workspace;
  std::atomic<std::size_t> wide_active = 0;
  std::atomic<std::size_t> wide_active_high_water = 0;
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks wide_hooks;
  wide_hooks.before_inside_clade = [&](larch::clade_id, std::size_t level,
                                       std::size_t stable_slot) {
    CHECK(stable_slot < w8.worker_resolution().resolved_workers);
    if (level != 0) return;
    auto const active = wide_active.fetch_add(1) + 1;
    auto high = wide_active_high_water.load();
    while (active > high &&
           !wide_active_high_water.compare_exchange_weak(high, active)) {
    }
    for (std::size_t attempt = 0;
         attempt < 1'000'000 && wide_active_high_water.load() < 4; ++attempt) {
      std::this_thread::yield();
    }
  };
  wide_hooks.after_inside_clade = [&](larch::clade_id, std::size_t level,
                                      std::size_t stable_slot) {
    CHECK(stable_slot < w8.worker_resolution().resolved_workers);
    if (level == 0) wide_active.fetch_sub(1);
  };
  std::vector<larch::chart_scheduler_run_summary> first_w8_inside_runs;
  std::vector<larch::chart_scheduler_run_summary> first_w8_outside_runs;
  auto first_w8_chart = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, w8, &first_w8_inside_runs, &wide_hooks,
      &w8_workspace);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, first_w8_chart, larch::chart_options{}, w8,
      &first_w8_outside_runs, nullptr, &w8_workspace);
  check_lazy_charts_equal(serial, first_w8_chart);
  CHECK(wide_active_high_water.load() >= 4);

  std::vector<larch::chart_scheduler_run_summary> second_w8_inside_runs;
  std::vector<larch::chart_scheduler_run_summary> second_w8_outside_runs;
  auto second_w8_chart = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, w8, &second_w8_inside_runs, nullptr,
      &w8_workspace);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, second_w8_chart, larch::chart_options{}, w8,
      &second_w8_outside_runs, nullptr, &w8_workspace);
  check_lazy_charts_equal(first_w8_chart, second_w8_chart);
  CHECK(first_w8_inside_runs.size() == second_w8_inside_runs.size());
  CHECK(first_w8_outside_runs.size() == second_w8_outside_runs.size());
  auto check_repeated_run_shape = [](auto const& lhs, auto const& rhs) {
    CHECK(lhs.item_count == rhs.item_count);
    CHECK(lhs.range_count == rhs.range_count);
    CHECK(lhs.effective_grain == rhs.effective_grain);
    CHECK(lhs.worker_tasks_submitted == rhs.worker_tasks_submitted);
    CHECK(lhs.serial_reason == rhs.serial_reason);
    CHECK(lhs.cancelled == rhs.cancelled);
    CHECK(lhs.failed == rhs.failed);
  };
  for (std::size_t i = 0; i < first_w8_inside_runs.size(); ++i) {
    check_repeated_run_shape(first_w8_inside_runs[i], second_w8_inside_runs[i]);
  }
  for (std::size_t i = 0; i < first_w8_outside_runs.size(); ++i) {
    check_repeated_run_shape(first_w8_outside_runs[i],
                             second_w8_outside_runs[i]);
  }

  larch::chart_options reference_options;
  reference_options.score_ua_edge = true;
  auto reference_lazy_options = options;
  reference_lazy_options.chart = reference_options;
  auto reference_serial =
      larch::build_lazy_inside_chart(plan, patterns, reference_lazy_options);
  larch::build_lazy_outside_chart_in_place(
      plan, patterns, reference_serial, reference_options, larch::nuc_base::G);
  auto reference_scheduled = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, reference_lazy_options, w4);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, reference_scheduled, reference_options,
      larch::nuc_base::G, w4);
  check_lazy_charts_equal(reference_serial, reference_scheduled);

  // Semantic exceptions are selected in level order, independent of which
  // worker reports first.
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks w1_error_hooks;
  w1_error_hooks.before_inside_clade = [](larch::clade_id clade,
                                          std::size_t level, std::size_t) {
    if (level == 0 && clade == larch::clade_id{0}) {
      throw std::runtime_error("stable first lazy-wave error");
    }
  };
  auto const w1_error = runtime_error_message([&] {
    (void)larch::build_lazy_inside_chart_scheduled(plan, patterns, options, w1,
                                                   nullptr, &w1_error_hooks);
  });
  std::atomic<bool> later_error_started = false;
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks w4_error_hooks;
  w4_error_hooks.before_inside_clade = [&](larch::clade_id clade,
                                           std::size_t level, std::size_t) {
    if (level != 0) return;
    if (clade == larch::clade_id{1}) {
      later_error_started = true;
      throw std::runtime_error("later lazy-wave error");
    }
    if (clade != larch::clade_id{0}) return;
    for (std::size_t attempt = 0;
         attempt < 1'000'000 && !later_error_started.load(); ++attempt) {
      std::this_thread::yield();
    }
    throw std::runtime_error("stable first lazy-wave error");
  };
  auto const w4_error = runtime_error_message([&] {
    (void)larch::build_lazy_inside_chart_scheduled(plan, patterns, options, w4,
                                                   nullptr, &w4_error_hooks);
  });
  CHECK(w4_error == w1_error);

  auto outside_w1_error_chart =
      larch::build_lazy_inside_chart(plan, patterns, options);
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks
      outside_w1_error_hooks;
  outside_w1_error_hooks.before_outside_clade =
      [](larch::clade_id clade, std::size_t level, std::size_t) {
        if (level == 1 && clade == larch::clade_id{8}) {
          throw std::runtime_error("stable first lazy-outside error");
        }
      };
  auto const outside_w1_error = runtime_error_message([&] {
    larch::build_lazy_outside_chart_in_place_scheduled(
        plan, patterns, outside_w1_error_chart, larch::chart_options{}, w1,
        nullptr, &outside_w1_error_hooks);
  });
  CHECK(outside_w1_error_chart.outside_rows_by_clade[8].empty());
  CHECK(outside_w1_error_chart.outside_rows_by_clade[9].empty());
  CHECK(!outside_w1_error_chart.outside_class_index_by_pattern_by_clade[8]);
  CHECK(!outside_w1_error_chart.outside_class_index_by_pattern_by_clade[9]);

  auto outside_w4_error_chart =
      larch::build_lazy_inside_chart(plan, patterns, options);
  std::atomic<bool> later_outside_error_started = false;
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks
      outside_w4_error_hooks;
  outside_w4_error_hooks.before_outside_clade =
      [&](larch::clade_id clade, std::size_t level, std::size_t) {
        if (level != 1) return;
        if (clade == larch::clade_id{9}) {
          later_outside_error_started = true;
          throw std::runtime_error("later lazy-outside error");
        }
        if (clade != larch::clade_id{8}) return;
        for (std::size_t attempt = 0;
             attempt < 1'000'000 && !later_outside_error_started.load();
             ++attempt) {
          std::this_thread::yield();
        }
        throw std::runtime_error("stable first lazy-outside error");
      };
  auto const outside_w4_error = runtime_error_message([&] {
    larch::build_lazy_outside_chart_in_place_scheduled(
        plan, patterns, outside_w4_error_chart, larch::chart_options{}, w4,
        nullptr, &outside_w4_error_hooks);
  });
  CHECK(outside_w4_error == outside_w1_error);
  CHECK(outside_w4_error_chart.outside_rows_by_clade[8].empty());
  CHECK(outside_w4_error_chart.outside_rows_by_clade[9].empty());
  CHECK(!outside_w4_error_chart.outside_class_index_by_pattern_by_clade[8]);
  CHECK(!outside_w4_error_chart.outside_class_index_by_pattern_by_clade[9]);

  // A partially submitted level joins its accepted runner, publishes the
  // failed run, and leaves the same scheduler/workspace reusable.
  larch::chart_scheduler retry_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace retry_workspace;
  larch::chart_scheduler_test_detail::access::fail_submission_after(
      retry_scheduler, 1);
  std::vector<larch::chart_scheduler_run_summary> failed_runs;
  bool submit_error_escaped = false;
  try {
    (void)larch::build_lazy_inside_chart_scheduled(
        plan, patterns, options, retry_scheduler, &failed_runs, nullptr,
        &retry_workspace);
  } catch (larch::chart_scheduler_submit_error const&) {
    submit_error_escaped = true;
  }
  CHECK(submit_error_escaped);
  CHECK(failed_runs.size() == 1);
  CHECK(failed_runs.front().failed);
  CHECK(failed_runs.front().worker_tasks_submitted == 1);
  CHECK(retry_scheduler.metrics().pending_tasks == 0);
  auto recovered = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, retry_scheduler, nullptr, nullptr,
      &retry_workspace);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, recovered, larch::chart_options{}, retry_scheduler,
      nullptr, nullptr, &retry_workspace);
  check_lazy_charts_equal(serial, recovered);

  larch::chart_scheduler outside_retry_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  auto outside_retry_chart =
      larch::build_lazy_inside_chart(plan, patterns, options);
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace
      outside_retry_workspace;
  larch::chart_scheduler_test_detail::access::fail_submission_after(
      outside_retry_scheduler, 1);
  std::vector<larch::chart_scheduler_run_summary> outside_failed_runs;
  bool outside_submit_error_escaped = false;
  try {
    larch::build_lazy_outside_chart_in_place_scheduled(
        plan, patterns, outside_retry_chart, larch::chart_options{},
        outside_retry_scheduler, &outside_failed_runs, nullptr,
        &outside_retry_workspace);
  } catch (larch::chart_scheduler_submit_error const&) {
    outside_submit_error_escaped = true;
  }
  CHECK(outside_submit_error_escaped);
  CHECK(outside_failed_runs.size() == 1);
  CHECK(outside_failed_runs.front().failed);
  CHECK(outside_failed_runs.front().worker_tasks_submitted == 1);
  CHECK(outside_retry_scheduler.metrics().pending_tasks == 0);
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, outside_retry_chart, larch::chart_options{},
      outside_retry_scheduler, nullptr, nullptr, &outside_retry_workspace);
  check_lazy_charts_equal(serial, outside_retry_chart);

  // Reusing one W1 scheduled workspace for a multifurcating build keeps both
  // the per-pattern rows and generic recurrence buffers at their first-build
  // capacities.
  auto const multifurcating_grammar = make_nonlex_multifurcating_grammar();
  auto const multifurcating_plan =
      larch::build_chart_execution_plan(multifurcating_grammar);
  auto const multifurcating_patterns = make_nonlex_four_taxon_patterns();
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace reuse_workspace;
  auto build_multifurcating = [&] {
    auto chart = larch::build_lazy_inside_chart_scheduled(
        multifurcating_plan, multifurcating_patterns, options, w1, nullptr,
        nullptr, &reuse_workspace);
    larch::build_lazy_outside_chart_in_place_scheduled(
        multifurcating_plan, multifurcating_patterns, chart,
        larch::chart_options{}, w1, nullptr, nullptr, &reuse_workspace);
    return chart;
  };
  auto const multifurcating_oracle = larch::build_lazy_outside_chart(
      multifurcating_plan, multifurcating_patterns,
      larch::build_lazy_inside_chart(multifurcating_plan,
                                     multifurcating_patterns, options));
  auto first_multifurcating = build_multifurcating();
  check_lazy_charts_equal(multifurcating_oracle, first_multifurcating);
  auto const& reuse_slot = reuse_workspace.outside_by_slot.front();
  auto const pattern_capacity = reuse_slot.outside_by_pattern.capacity();
  auto const recurrence_result_capacity =
      reuse_slot.recurrence_scratch.result.capacity();
  auto const recurrence_child_capacity =
      reuse_slot.recurrence_scratch.child_best.capacity();
  auto const capacity_growths = reuse_slot.outside_pattern_capacity_growths;
  CHECK(pattern_capacity >= multifurcating_patterns.patterns.size());
  CHECK(recurrence_result_capacity >= 3);
  CHECK(recurrence_child_capacity >= 3);
  auto second_multifurcating = build_multifurcating();
  check_lazy_charts_equal(multifurcating_oracle, second_multifurcating);
  CHECK(reuse_slot.outside_by_pattern.capacity() == pattern_capacity);
  CHECK(reuse_slot.recurrence_scratch.result.capacity() ==
        recurrence_result_capacity);
  CHECK(reuse_slot.recurrence_scratch.child_best.capacity() ==
        recurrence_child_capacity);
  CHECK(reuse_slot.outside_pattern_capacity_growths == capacity_growths);

  // A leaf root has no scheduled outside levels. Root initialization remains
  // serial and produces exactly the ordinary chart.
  larch::clade_grammar root_only;
  root_only.taxa.id_to_sample_id = {"A"};
  root_only.taxa.sample_id_to_id = {{"A", larch::taxon_id{0}}};
  root_only.clades = {{{larch::taxon_id{0}}}};
  root_only.productions_by_parent = {{}};
  root_only.productions_by_child = {{}};
  root_only.root_clade = larch::clade_id{0};
  root_only.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  auto const root_plan = larch::build_chart_execution_plan(root_only);
  auto const root_patterns = make_weighted_patterns(1);
  std::vector<larch::chart_scheduler_run_summary> root_inside_runs;
  std::vector<larch::chart_scheduler_run_summary> root_outside_runs;
  auto root_scheduled = larch::build_lazy_inside_chart_scheduled(
      root_plan, root_patterns, options, w4, &root_inside_runs);
  larch::build_lazy_outside_chart_in_place_scheduled(
      root_plan, root_patterns, root_scheduled, larch::chart_options{}, w4,
      &root_outside_runs);
  auto root_serial =
      larch::build_lazy_inside_chart(root_plan, root_patterns, options);
  larch::build_lazy_outside_chart_in_place(root_plan, root_patterns,
                                           root_serial, larch::chart_options{});
  check_lazy_charts_equal(root_serial, root_scheduled);
  CHECK(root_inside_runs.size() == 1);
  CHECK(root_outside_runs.empty());
}

static void test_finite_scheduled_lazy_state_admission() {
  std::println("test_finite_scheduled_lazy_state_admission");
  using larch::lazy_chart_detail::plan_lazy_chart_memory_budget_error;
  using larch::lazy_chart_detail::plan_lazy_chart_memory_options;
  using larch::lazy_chart_detail::plan_lazy_chart_memory_report;
  using larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks;
  using larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace;
  using larch::test::chart_spr_allocation::allocation_observer;
  namespace allocation_detail = larch::test::chart_spr_allocation::detail;

  auto const grammar = make_shared_internal_child_grammar();
  auto const plan = larch::build_chart_execution_plan(grammar);
  auto const patterns = make_weighted_patterns(plan.taxon_count());
  larch::lazy_chart_options options;
  options.retain_all_inside_class_maps = false;
  auto const oracle = larch::build_lazy_inside_chart(plan, patterns, options);

  constexpr auto huge_budget = std::size_t{1} << 40;
  auto make_scheduler = [](std::size_t workers) {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = workers,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 4,
        });
  };

  // Preparation errors distinguish a projected pre-allocation rejection from
  // a measured transient staging peak. The distinction is what keeps the
  // public actual-peak counter truthful when rejected staging is destroyed.
  std::vector<std::uint8_t> calibrated_vector;
  auto const calibrated_vector_report =
      larch::lazy_chart_detail::prepare_plan_lazy_vector_storage(
          1, calibrated_vector);
  auto const projected_vector_peak =
      2 * sizeof(calibrated_vector) + sizeof(std::uint8_t);
  auto const measured_vector_peak =
      calibrated_vector_report
          .observed_prepublication_peak_capacity_resident_bytes;
  auto const selected_vector_bytes = larch::lazy_key_grouping_detail::
      frozen_libstdcxx_allocate_at_least_capacity_bytes<std::uint8_t>(
          1, "test prepared vector selected capacity");
  CHECK(measured_vector_peak ==
        2 * sizeof(calibrated_vector) + selected_vector_bytes);
  CHECK(measured_vector_peak > projected_vector_peak);
  auto check_vector_rejection = [](std::size_t limit,
                                   std::size_t expected_required,
                                   std::size_t expected_observed) {
    std::vector<std::uint8_t> values;
    bool rejected = false;
    try {
      (void)larch::lazy_chart_detail::prepare_plan_lazy_vector_storage(
          1, values, limit);
    } catch (
        larch::lazy_key_grouping_detail::packed_key_grouping_budget_error const&
            error) {
      rejected = true;
      CHECK(error.required_bytes() == expected_required);
      CHECK(error.budget_bytes() == limit);
      CHECK(error.observed_peak_bytes() == expected_observed);
    }
    CHECK(rejected);
    CHECK(values.empty());
    CHECK(values.capacity() == 0);
  };
  check_vector_rejection(projected_vector_peak - 1, projected_vector_peak, 0);
  check_vector_rejection(measured_vector_peak - 1, measured_vector_peak,
                         measured_vector_peak);

  // Both phase tags use the same already-observed rejection contract: the
  // complete live requirement is present in both report axes exactly once.
  for (auto phase :
       {larch::lazy_chart_detail::plan_lazy_chart_memory_phase::inside,
        larch::lazy_chart_detail::plan_lazy_chart_memory_phase::outside}) {
    constexpr auto observed_required = std::size_t{101};
    plan_lazy_chart_memory_report observed_report;
    bool observed_rejected = false;
    try {
      larch::lazy_chart_detail::reject_plan_lazy_chart_already_observed(
          plan_lazy_chart_memory_options{.memory_budget_bytes =
                                             observed_required - 1},
          &observed_report, phase, observed_required);
    } catch (plan_lazy_chart_memory_budget_error const& error) {
      observed_rejected = true;
      CHECK(error.phase() == phase);
      CHECK(error.required_bytes() == observed_required);
      CHECK(error.budget_bytes() == observed_required - 1);
    }
    CHECK(observed_rejected);
    CHECK(observed_report.preflight_peak_capacity_resident_bytes ==
          observed_required);
    CHECK(observed_report.actual_peak_capacity_resident_bytes ==
          observed_required);
    CHECK(observed_report.pre_submit_rejections == 1);
  }

  // W1 finite admission is the same coordinator-prepared route.  Its measured
  // high-water is a reproducible exact-fit budget on this frozen toolchain.
  auto calibration_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report calibration_report;
  plan_lazy_chart_scheduler_workspace calibration_workspace;
  auto calibration = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, *calibration_scheduler, nullptr, nullptr,
      &calibration_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &calibration_report);
  check_lazy_charts_equal(oracle, calibration);
  CHECK(calibration_report.inside_max_admitted_slots == 1);
  CHECK(calibration_report.inside_admission_waves > 0);
  CHECK(calibration_report.inside_reused_slot_waves > 0);
  CHECK(calibration_report.inside_coordinator_capacity_resident_bytes > 0);
  CHECK(calibration_report.actual_peak_capacity_resident_bytes <= huge_budget);
  CHECK(calibration_report.preflight_peak_capacity_resident_bytes <=
        huge_budget);
  auto const exact_w1_budget =
      std::max(calibration_report.actual_peak_capacity_resident_bytes,
               calibration_report.preflight_peak_capacity_resident_bytes);
  CHECK(exact_w1_budget > 1);
  auto const w4_singleton_budget =
      exact_w1_budget +
      larch::estimate_chart_scheduler_pool_owning_heap_bytes(4);

  auto exact_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report exact_report;
  auto exact = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, *exact_scheduler, nullptr, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes = exact_w1_budget},
      &exact_report);
  check_lazy_charts_equal(oracle, exact);
  CHECK(exact_report.actual_peak_capacity_resident_bytes <= exact_w1_budget);
  CHECK(exact_report.preflight_peak_capacity_resident_bytes <= exact_w1_budget);

  auto one_under_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report one_under_report;
  bool one_under_rejected = false;
  try {
    (void)larch::build_lazy_inside_chart_scheduled(
        plan, patterns, options, *one_under_scheduler, nullptr, nullptr,
        nullptr,
        plan_lazy_chart_memory_options{.memory_budget_bytes =
                                           exact_w1_budget - 1},
        &one_under_report);
  } catch (plan_lazy_chart_memory_budget_error const& error) {
    one_under_rejected = true;
    CHECK(error.required_bytes() > error.budget_bytes());
  }
  CHECK(one_under_rejected);
  CHECK(one_under_report.pre_submit_rejections == 1);

  // A budget below the coordinator envelope rejects before even a serial
  // scheduler operation is registered.
  auto presubmit_scheduler = make_scheduler(4);
  auto const metrics_before_rejection = presubmit_scheduler->metrics();
  plan_lazy_chart_memory_report presubmit_report;
  plan_lazy_chart_scheduler_workspace presubmit_workspace;
  presubmit_workspace.outside_by_slot.resize(2);
  for (auto& workspace : presubmit_workspace.outside_by_slot) {
    (void)larch::lazy_chart_detail::prepare_plan_outside_slot(
        workspace, patterns.patterns.size(), 64, 64);
  }
  allocation_observer inside_initialization_observer;
  bool presubmit_rejected = false;
  CHECK(allocation_detail::active_observer == nullptr);
  allocation_detail::active_observer = &inside_initialization_observer;
  try {
    (void)larch::build_lazy_inside_chart_scheduled(
        plan, patterns, options, *presubmit_scheduler, nullptr, nullptr,
        &presubmit_workspace,
        plan_lazy_chart_memory_options{.memory_budget_bytes = 1},
        &presubmit_report);
  } catch (plan_lazy_chart_memory_budget_error const&) {
    presubmit_rejected = true;
  }
  allocation_detail::active_observer = nullptr;
  CHECK(presubmit_rejected);
  CHECK(inside_initialization_observer.statistics.calls == 0);
  CHECK(presubmit_report.pre_submit_rejections == 1);
  CHECK(presubmit_report.outside_workspace_evictions == 1);
  CHECK(presubmit_workspace.outside_by_slot.capacity() == 0);
  CHECK(presubmit_scheduler->metrics().operations ==
        metrics_before_rejection.operations);

  // A first-leaf singleton envelope is insufficient for this asymmetric plan:
  // later internal row keys are wider. A budget strictly between the old
  // first-item and plan-wide bounds must reject before the first submission.
  auto const asymmetric_inside_grammar = make_wide_star_grammar();
  auto const asymmetric_inside_plan =
      larch::build_chart_execution_plan(asymmetric_inside_grammar);
  auto const asymmetric_inside_patterns = make_wide_star_patterns();
  auto first_inside_width = std::size_t{0};
  auto maximum_inside_width = std::size_t{0};
  for (auto clade : asymmetric_inside_plan.bottom_up_level_order()) {
    auto const width =
        asymmetric_inside_plan.clade(clade).is_leaf()
            ? std::size_t{0}
            : larch::lazy_chart_detail::plan_inside_clade_row_key_width(
                  asymmetric_inside_plan, clade);
    if (clade == asymmetric_inside_plan.bottom_up_level_order().front()) {
      first_inside_width = width;
    }
    maximum_inside_width = std::max(maximum_inside_width, width);
  }
  auto const first_inside_slot =
      larch::lazy_chart_detail::logical_plan_inside_slot_resident_bytes(
          asymmetric_inside_patterns.patterns.size(), first_inside_width);
  auto const maximum_inside_slot =
      larch::lazy_chart_detail::logical_plan_inside_slot_resident_bytes(
          asymmetric_inside_patterns.patterns.size(), maximum_inside_width);
  CHECK(maximum_inside_slot > first_inside_slot);
  auto global_inside_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report global_inside_preflight;
  larch::lazy_chart_detail::preflight_plan_lazy_inside_direct_finite(
      asymmetric_inside_plan, asymmetric_inside_patterns, options,
      *global_inside_scheduler, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &global_inside_preflight);
  auto const global_inside_bound =
      global_inside_preflight.preflight_peak_capacity_resident_bytes;
  CHECK(global_inside_bound >= maximum_inside_slot - first_inside_slot);
  auto const first_only_inside_bound =
      global_inside_bound - maximum_inside_slot + first_inside_slot;
  auto const between_inside_bound =
      first_only_inside_bound +
      (global_inside_bound - first_only_inside_bound) / 2;
  CHECK(between_inside_bound >= first_only_inside_bound);
  CHECK(between_inside_bound < global_inside_bound);
  auto const global_inside_metrics = global_inside_scheduler->metrics();
  plan_lazy_chart_memory_report global_inside_rejection;
  bool global_inside_rejected = false;
  try {
    (void)larch::build_lazy_inside_chart_scheduled(
        asymmetric_inside_plan, asymmetric_inside_patterns, options,
        *global_inside_scheduler, nullptr, nullptr, nullptr,
        plan_lazy_chart_memory_options{.memory_budget_bytes =
                                           between_inside_bound},
        &global_inside_rejection);
  } catch (plan_lazy_chart_memory_budget_error const&) {
    global_inside_rejected = true;
  }
  CHECK(global_inside_rejected);
  CHECK(global_inside_rejection.inside_admission_waves == 0);
  CHECK(global_inside_rejection.pre_submit_rejections == 1);
  CHECK(global_inside_scheduler->metrics().operations ==
        global_inside_metrics.operations);

  // The W1 exact envelope forces the shared-parent level into singleton
  // subwaves at W4. Both readers observe the sparse child maps alive; only the
  // full dependency-level join may reclaim them.
  auto split_scheduler = make_scheduler(4);
  plan_lazy_chart_memory_report split_report;
  plan_lazy_chart_scheduler_workspace split_workspace;
  std::size_t level_two_starts = 0;
  std::size_t shared_maps_seen = 0;
  bool shared_alive_at_join = false;
  bool shared_reclaimed_after_join = false;
  plan_lazy_chart_scheduler_test_hooks split_hooks;
  split_hooks.before_inside_clade = [&](larch::clade_id, std::size_t level,
                                        std::size_t) {
    if (level == 2) ++level_two_starts;
  };
  split_hooks.observe_completed_inside_clade =
      [&](larch::lazy_multisite_chart const& chart, larch::clade_id clade,
          std::size_t level, std::size_t) {
        if (level != 2 ||
            (clade != larch::clade_id{8} && clade != larch::clade_id{9})) {
          return;
        }
        CHECK(chart.class_index_by_pattern_by_clade[5].has_value());
        CHECK(chart.structural_class_index_by_pattern_by_clade[5].has_value());
        ++shared_maps_seen;
      };
  split_hooks.observe_inside_level_join =
      [&](larch::lazy_multisite_chart const& chart, std::size_t level) {
        if (level == 2) {
          shared_alive_at_join =
              chart.class_index_by_pattern_by_clade[5].has_value() &&
              chart.structural_class_index_by_pattern_by_clade[5].has_value();
        }
      };
  split_hooks.observe_inside_level_reclamation =
      [&](larch::lazy_multisite_chart const& chart, std::size_t level) {
        if (level == 2) {
          shared_reclaimed_after_join =
              !chart.class_index_by_pattern_by_clade[5].has_value() &&
              !chart.structural_class_index_by_pattern_by_clade[5].has_value();
        }
      };
  auto split = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, *split_scheduler, nullptr, &split_hooks,
      &split_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes =
                                         w4_singleton_budget},
      &split_report);
  check_lazy_charts_equal(oracle, split);
  CHECK(level_two_starts == 2);
  CHECK(shared_maps_seen == 2);
  CHECK(shared_alive_at_join);
  CHECK(shared_reclaimed_after_join);
  CHECK(split_report.inside_memory_limited_levels > 0);
  CHECK(split_report.inside_admission_waves >
        plan.bottom_up_level_offsets().size() - 1);
  CHECK(split_report.actual_peak_capacity_resident_bytes <=
        w4_singleton_budget);

  // A semantic error in the first singleton of the W4-split parent level stops
  // the later stable-prefix subwave and clears every output in that level. W1
  // admits the complete K=2 prefix with S=1 and may enter its later range, but
  // still publishes the same lowest-stable-clade error after the join.
  auto const level_two_begin = plan.bottom_up_level_offsets()[2];
  CHECK(plan.bottom_up_level_offsets()[3] - level_two_begin == 2);
  auto const first_level_two_clade =
      plan.bottom_up_level_order()[level_two_begin];
  auto const later_level_two_clade =
      plan.bottom_up_level_order()[level_two_begin + 1];
  auto run_split_error = [&](std::size_t workers, bool& later_started,
                             bool& cleanup_seen,
                             plan_lazy_chart_memory_report& report) {
    auto scheduler = make_scheduler(workers);
    plan_lazy_chart_scheduler_test_hooks hooks;
    hooks.before_inside_clade = [&](larch::clade_id clade, std::size_t level,
                                    std::size_t) {
      if (level == 2 && clade == later_level_two_clade) later_started = true;
      if (level == 2 && clade == first_level_two_clade) {
        throw std::runtime_error("finite stable first parent error");
      }
    };
    hooks.observe_inside_level_failure_cleanup =
        [&](larch::lazy_multisite_chart const& chart, std::size_t level) {
          if (level != 2) return;
          cleanup_seen = true;
          for (auto clade : {larch::clade_id{8}, larch::clade_id{9}}) {
            CHECK(chart.inside_rows_by_clade[clade].empty());
            CHECK(chart.class_weight_by_clade[clade].empty());
            CHECK(!chart.class_index_by_pattern_by_clade[clade]);
            CHECK(!chart.structural_class_index_by_pattern_by_clade[clade]);
          }
        };
    return runtime_error_message([&] {
      (void)larch::build_lazy_inside_chart_scheduled(
          plan, patterns, options, *scheduler, nullptr, &hooks, nullptr,
          plan_lazy_chart_memory_options{.memory_budget_bytes =
                                             w4_singleton_budget},
          &report);
    });
  };
  bool w1_later_started = false;
  bool w1_cleanup_seen = false;
  plan_lazy_chart_memory_report w1_error_report;
  auto const w1_error =
      run_split_error(1, w1_later_started, w1_cleanup_seen, w1_error_report);
  bool w4_later_started = false;
  bool w4_cleanup_seen = false;
  plan_lazy_chart_memory_report w4_error_report;
  auto const w4_error =
      run_split_error(4, w4_later_started, w4_cleanup_seen, w4_error_report);
  CHECK(w4_error == w1_error);
  CHECK(w1_later_started);
  CHECK(!w4_later_started);
  CHECK(w1_cleanup_seen);
  CHECK(w4_cleanup_seen);
  CHECK(w1_error_report.inside_admission_waves > 0);
  CHECK(w4_error_report.inside_admission_waves > 0);
  CHECK(w1_error_report.scheduler_submissions ==
        w1_error_report.inside_admission_waves);
  CHECK(w4_error_report.scheduler_submissions ==
        w4_error_report.inside_admission_waves);

  // Admission reporting includes an infrastructure-failed wave as soon as its
  // prepared slots are handed to the scheduler. A partial submission therefore
  // remains visible and reconciles with the published failed run.
  auto partial_inside_scheduler = make_scheduler(4);
  larch::chart_scheduler_test_detail::access::fail_submission_after(
      *partial_inside_scheduler, 1);
  std::vector<larch::chart_scheduler_run_summary> partial_inside_runs;
  plan_lazy_chart_memory_report partial_inside_report;
  bool partial_inside_failed = false;
  try {
    (void)larch::build_lazy_inside_chart_scheduled(
        plan, patterns, options, *partial_inside_scheduler,
        &partial_inside_runs, nullptr, nullptr,
        plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
        &partial_inside_report);
  } catch (larch::chart_scheduler_submit_error const&) {
    partial_inside_failed = true;
  }
  CHECK(partial_inside_failed);
  CHECK(partial_inside_runs.size() == 1);
  CHECK(partial_inside_runs.front().failed);
  CHECK(partial_inside_runs.front().worker_tasks_submitted == 1);
  CHECK(partial_inside_report.inside_admission_waves == 1);
  CHECK(partial_inside_report.inside_max_admitted_slots > 1);
  CHECK(partial_inside_report.scheduler_submissions == 1);
  CHECK(partial_inside_scheduler->metrics().operations ==
        partial_inside_report.scheduler_submissions);
  CHECK(partial_inside_scheduler->metrics().pending_tasks == 0);

  // Finite preparation hard-bounds stale W8/deep scratch before its preflight.
  plan_lazy_chart_scheduler_workspace stale_workspace;
  stale_workspace.inside_by_slot.resize(8);
  for (auto& workspace : stale_workspace.inside_by_slot) {
    (void)larch::lazy_chart_detail::prepare_plan_inside_slot(
        workspace, patterns.patterns.size(), 128);
  }
  auto const stale_bytes = stale_workspace.inside_capacity_resident_bytes();
  auto stale_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report stale_report;
  auto stale_result = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, *stale_scheduler, nullptr, nullptr,
      &stale_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = exact_w1_budget},
      &stale_report);
  check_lazy_charts_equal(oracle, stale_result);
  CHECK(stale_workspace.inside_by_slot.size() == 1);
  CHECK(stale_workspace.inside_by_slot.capacity() == 1);
  CHECK(stale_workspace.inside_capacity_resident_bytes() < stale_bytes);
  CHECK(stale_report.actual_peak_capacity_resident_bytes <= exact_w1_budget);

  // An empty vector can still retain its old slot-array allocation.  Cold
  // replacement must release that capacity before allocating the admitted
  // array, even though there are no live workspace elements to inspect.
  plan_lazy_chart_scheduler_workspace empty_inside_workspace;
  empty_inside_workspace.inside_by_slot.reserve(8);
  auto empty_inside_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report empty_inside_report;
  auto empty_inside = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, *empty_inside_scheduler, nullptr, nullptr,
      &empty_inside_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = exact_w1_budget},
      &empty_inside_report);
  check_lazy_charts_equal(oracle, empty_inside);
  CHECK(empty_inside_workspace.inside_by_slot.size() == 1);
  CHECK(empty_inside_workspace.inside_by_slot.capacity() == 1);
  CHECK(empty_inside_report.inside_workspace_evictions > 0);

  // With a loose budget, W4 admits four clades. Allocation observation begins
  // only after coordinator preparation and spans the complete worker core.
  auto allocation_scheduler = make_scheduler(4);
  plan_lazy_chart_scheduler_workspace allocation_workspace;
  plan_lazy_chart_memory_report allocation_report;
  std::array<allocation_observer, 4> observers;
  plan_lazy_chart_scheduler_test_hooks allocation_hooks;
  allocation_hooks.before_inside_clade = [&](larch::clade_id, std::size_t,
                                             std::size_t slot) {
    CHECK(slot < observers.size());
    CHECK(allocation_detail::active_observer == nullptr);
    allocation_detail::active_observer = &observers[slot];
  };
  allocation_hooks.after_inside_clade = [&](larch::clade_id, std::size_t,
                                            std::size_t slot) {
    CHECK(allocation_detail::active_observer == &observers[slot]);
    allocation_detail::active_observer = nullptr;
  };
  auto allocation_chart = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, options, *allocation_scheduler, nullptr,
      &allocation_hooks, &allocation_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &allocation_report);
  check_lazy_charts_equal(oracle, allocation_chart);
  CHECK(allocation_report.inside_max_admitted_slots >= 4);
  for (auto const& observer : observers) {
    CHECK(observer.statistics.calls == 0);
  }

  // Outside uses the same finite admission route. Keep all inside maps in the
  // input so this test isolates scheduled outside preparation rather than the
  // compatibility materialization boundary.
  auto outside_inside_options = options;
  outside_inside_options.retain_all_inside_class_maps = true;
  auto make_outside_input = [&] {
    return larch::build_lazy_inside_chart(plan, patterns,
                                          outside_inside_options);
  };
  auto outside_oracle = make_outside_input();
  larch::build_lazy_outside_chart_in_place(plan, patterns, outside_oracle,
                                           larch::chart_options{});

  auto outside_calibration_scheduler = make_scheduler(1);
  auto outside_calibration = make_outside_input();
  plan_lazy_chart_memory_report outside_calibration_report;
  plan_lazy_chart_scheduler_workspace outside_calibration_workspace;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, outside_calibration, larch::chart_options{},
      *outside_calibration_scheduler, nullptr, nullptr,
      &outside_calibration_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &outside_calibration_report);
  check_lazy_charts_equal(outside_oracle, outside_calibration);
  CHECK(outside_calibration_report.outside_max_admitted_slots == 1);
  CHECK(outside_calibration_report.outside_admission_waves > 0);
  CHECK(outside_calibration_report.outside_reused_slot_waves > 0);
  CHECK(outside_calibration_report.actual_peak_capacity_resident_bytes <=
        huge_budget);
  auto const exact_outside_w1_budget = std::max(
      outside_calibration_report.actual_peak_capacity_resident_bytes,
      outside_calibration_report.preflight_peak_capacity_resident_bytes);
  CHECK(exact_outside_w1_budget > 1);

  auto outside_exact_scheduler = make_scheduler(1);
  auto outside_exact = make_outside_input();
  plan_lazy_chart_memory_report outside_exact_report;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, outside_exact, larch::chart_options{},
      *outside_exact_scheduler, nullptr, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes =
                                         exact_outside_w1_budget},
      &outside_exact_report);
  check_lazy_charts_equal(outside_oracle, outside_exact);
  CHECK(outside_exact_report.actual_peak_capacity_resident_bytes <=
        exact_outside_w1_budget);
  CHECK(outside_exact_report.preflight_peak_capacity_resident_bytes <=
        exact_outside_w1_budget);

  auto outside_under_scheduler = make_scheduler(1);
  auto outside_under = make_outside_input();
  plan_lazy_chart_memory_report outside_under_report;
  bool outside_under_rejected = false;
  try {
    larch::build_lazy_outside_chart_in_place_scheduled(
        plan, patterns, outside_under, larch::chart_options{},
        *outside_under_scheduler, nullptr, nullptr, nullptr,
        plan_lazy_chart_memory_options{.memory_budget_bytes =
                                           exact_outside_w1_budget - 1},
        &outside_under_report);
  } catch (plan_lazy_chart_memory_budget_error const& error) {
    outside_under_rejected = true;
    CHECK(error.required_bytes() > error.budget_bytes());
  }
  CHECK(outside_under_rejected);
  CHECK(outside_under_report.pre_submit_rejections == 1);

  // The public finite outside API must also cover sparse inside charts. Their
  // compatibility materializer owns an additional structural-class -> row
  // vector at the same time as the two published maps, packed-key workspace,
  // and ordered row lookup. Lock that overlap into the preflight formula, then
  // exercise an odd-pattern exact and one-under no-submit boundary: GCC 17
  // rounds three size_t elements from 24 allocation bytes to 32.
  auto sparse_patterns = patterns;
  sparse_patterns.patterns.resize(3);
  CHECK(sparse_patterns.patterns.size() == 3);
  auto sparse_outside_input =
      larch::build_lazy_inside_chart(plan, sparse_patterns, options);
  std::size_t sparse_missing_map_count = 0;
  std::size_t sparse_maximum_row_width = 0;
  bool sparse_missing_internal_map = false;
  for (auto clade : plan.bottom_up_order()) {
    if (sparse_outside_input.class_index_by_pattern_by_clade[clade] &&
        sparse_outside_input
            .structural_class_index_by_pattern_by_clade[clade]) {
      continue;
    }
    ++sparse_missing_map_count;
    if (!plan.clade(clade).is_leaf()) {
      sparse_missing_internal_map = true;
      sparse_maximum_row_width =
          std::max(sparse_maximum_row_width,
                   larch::lazy_chart_detail::plan_inside_clade_row_key_width(
                       plan, clade));
    }
  }
  CHECK(sparse_missing_internal_map);
  auto const sparse_pattern_count = sparse_patterns.patterns.size();
  auto const sparse_selected_index_bytes = larch::lazy_key_grouping_detail::
      frozen_libstdcxx_allocate_at_least_capacity_bytes<std::size_t>(
          sparse_pattern_count, "test sparse compatibility maps");
  CHECK(sparse_selected_index_bytes == 32);
  auto sparse_compatibility_bound =
      sparse_missing_map_count * 2 * sparse_selected_index_bytes;
  sparse_compatibility_bound +=
      larch::lazy_chart_detail::logical_plan_inside_slot_resident_bytes(
          sparse_pattern_count, sparse_maximum_row_width);
  sparse_compatibility_bound += larch::lazy_chart_detail::
      logical_plan_inside_slot_preparation_extra_bytes(sparse_pattern_count);
  sparse_compatibility_bound +=
      sparse_pattern_count *
      (sizeof(std::pair<larch::lazy_multisite_chart::row_type const,
                        std::size_t>) +
       4 * sizeof(void*));
  auto const sparse_structural_row_lookup_bytes =
      sizeof(std::vector<std::size_t>) + sparse_selected_index_bytes;
  CHECK(larch::lazy_chart_detail::
            logical_plan_lazy_missing_inside_map_preparation_bytes(
                sparse_outside_input, plan) ==
        sparse_compatibility_bound + sparse_structural_row_lookup_bytes);

  auto sparse_outside_oracle = sparse_outside_input;
  larch::build_lazy_outside_chart_in_place(
      plan, sparse_patterns, sparse_outside_oracle, larch::chart_options{});
  // A chart copy retains values but may tighten vector capacity. Calibrate on
  // the same copied-capacity shape used by both boundary executions.
  auto const sparse_preflight_input = sparse_outside_input;
  auto sparse_calibration_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report sparse_calibration_report;
  larch::lazy_chart_detail::preflight_plan_lazy_outside_direct_finite(
      plan, sparse_patterns, sparse_preflight_input, larch::chart_options{}, 0,
      *sparse_calibration_scheduler, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &sparse_calibration_report);
  auto const sparse_exact_budget =
      sparse_calibration_report.preflight_peak_capacity_resident_bytes;
  CHECK(sparse_exact_budget > 1);

  auto sparse_exact = sparse_outside_input;
  auto sparse_exact_scheduler = make_scheduler(1);
  plan_lazy_chart_memory_report sparse_exact_report;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, sparse_patterns, sparse_exact, larch::chart_options{},
      *sparse_exact_scheduler, nullptr, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes =
                                         sparse_exact_budget},
      &sparse_exact_report);
  check_lazy_charts_equal(sparse_outside_oracle, sparse_exact);
  CHECK(sparse_exact_report.preflight_peak_capacity_resident_bytes <=
        sparse_exact_budget);
  CHECK(sparse_exact_report.actual_peak_capacity_resident_bytes <=
        sparse_exact_budget);

  auto sparse_one_under = sparse_outside_input;
  auto const sparse_one_under_before = sparse_one_under;
  auto sparse_under_scheduler = make_scheduler(1);
  auto const sparse_under_operations =
      sparse_under_scheduler->metrics().operations;
  plan_lazy_chart_memory_report sparse_under_report;
  allocation_observer sparse_under_observer;
  bool sparse_under_rejected = false;
  CHECK(allocation_detail::active_observer == nullptr);
  allocation_detail::active_observer = &sparse_under_observer;
  try {
    larch::build_lazy_outside_chart_in_place_scheduled(
        plan, sparse_patterns, sparse_one_under, larch::chart_options{},
        *sparse_under_scheduler, nullptr, nullptr, nullptr,
        plan_lazy_chart_memory_options{.memory_budget_bytes =
                                           sparse_exact_budget - 1},
        &sparse_under_report);
  } catch (plan_lazy_chart_memory_budget_error const& error) {
    sparse_under_rejected = true;
    CHECK(error.required_bytes() == sparse_exact_budget);
    CHECK(error.budget_bytes() == sparse_exact_budget - 1);
  }
  allocation_detail::active_observer = nullptr;
  CHECK(sparse_under_rejected);
  CHECK(sparse_under_observer.statistics.calls == 0);
  CHECK(sparse_under_report.pre_submit_rejections == 1);
  CHECK(sparse_under_report.outside_admission_waves == 0);
  CHECK(sparse_under_report.actual_peak_capacity_resident_bytes <=
        sparse_exact_budget - 1);
  CHECK(sparse_under_scheduler->metrics().operations ==
        sparse_under_operations);
  check_lazy_charts_equal(sparse_one_under_before, sparse_one_under);

  auto outside_presubmit_scheduler = make_scheduler(4);
  auto outside_presubmit = make_outside_input();
  auto const outside_metrics_before = outside_presubmit_scheduler->metrics();
  plan_lazy_chart_memory_report outside_presubmit_report;
  plan_lazy_chart_scheduler_workspace outside_presubmit_workspace;
  outside_presubmit_workspace.inside_by_slot.resize(2);
  for (auto& workspace : outside_presubmit_workspace.inside_by_slot) {
    (void)larch::lazy_chart_detail::prepare_plan_inside_slot(
        workspace, patterns.patterns.size(), 64);
  }
  allocation_observer outside_initialization_observer;
  bool outside_presubmit_rejected = false;
  CHECK(allocation_detail::active_observer == nullptr);
  allocation_detail::active_observer = &outside_initialization_observer;
  try {
    larch::build_lazy_outside_chart_in_place_scheduled(
        plan, patterns, outside_presubmit, larch::chart_options{},
        *outside_presubmit_scheduler, nullptr, nullptr,
        &outside_presubmit_workspace,
        plan_lazy_chart_memory_options{.memory_budget_bytes = 1},
        &outside_presubmit_report);
  } catch (plan_lazy_chart_memory_budget_error const&) {
    outside_presubmit_rejected = true;
  }
  allocation_detail::active_observer = nullptr;
  CHECK(outside_presubmit_rejected);
  CHECK(outside_initialization_observer.statistics.calls == 0);
  CHECK(outside_presubmit_report.pre_submit_rejections == 1);
  CHECK(outside_presubmit_report.inside_workspace_evictions == 1);
  CHECK(outside_presubmit_workspace.inside_by_slot.capacity() == 0);
  CHECK(outside_presubmit_scheduler->metrics().operations ==
        outside_metrics_before.operations);

  auto const asymmetric_grammar = make_asymmetric_outside_grammar();
  auto const asymmetric_plan =
      larch::build_chart_execution_plan(asymmetric_grammar);
  auto const asymmetric_patterns = make_weighted_patterns(4);
  larch::lazy_chart_options asymmetric_inside_options;
  asymmetric_inside_options.retain_all_inside_class_maps = true;
  auto const asymmetric_inside = larch::build_lazy_inside_chart(
      asymmetric_plan, asymmetric_patterns, asymmetric_inside_options);
  auto first_outside_shape = std::pair<std::size_t, std::size_t>{};
  auto maximum_outside_shape =
      std::pair<std::size_t, std::size_t>{larch::nuc_state_count, 0};
  bool saw_first_nonroot = false;
  for (auto clade : asymmetric_plan.top_down_level_order()) {
    if (clade == asymmetric_plan.root_clade()) continue;
    auto const shape = larch::lazy_chart_detail::plan_outside_clade_shape(
        asymmetric_plan, clade);
    if (!saw_first_nonroot) {
      first_outside_shape = shape;
      saw_first_nonroot = true;
    }
    maximum_outside_shape.first =
        std::max(maximum_outside_shape.first, shape.first);
    maximum_outside_shape.second =
        std::max(maximum_outside_shape.second, shape.second);
  }
  CHECK(saw_first_nonroot);
  auto const first_outside_slot =
      larch::lazy_chart_detail::logical_plan_outside_slot_resident_bytes(
          asymmetric_patterns.patterns.size(), first_outside_shape.first,
          first_outside_shape.second);
  auto const maximum_outside_slot =
      larch::lazy_chart_detail::logical_plan_outside_slot_resident_bytes(
          asymmetric_patterns.patterns.size(), maximum_outside_shape.first,
          maximum_outside_shape.second);
  CHECK(maximum_outside_slot > first_outside_slot);
  auto global_outside_scheduler = make_scheduler(1);
  auto rejected_outside = asymmetric_inside;
  plan_lazy_chart_memory_report global_outside_preflight;
  larch::lazy_chart_detail::preflight_plan_lazy_outside_direct_finite(
      asymmetric_plan, asymmetric_patterns, rejected_outside,
      larch::chart_options{}, 0, *global_outside_scheduler, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &global_outside_preflight);
  auto const global_outside_bound =
      global_outside_preflight.preflight_peak_capacity_resident_bytes;
  CHECK(global_outside_bound >= maximum_outside_slot - first_outside_slot);
  auto const first_only_outside_bound =
      global_outside_bound - maximum_outside_slot + first_outside_slot;
  auto const between_outside_bound =
      first_only_outside_bound +
      (global_outside_bound - first_only_outside_bound) / 2;
  CHECK(between_outside_bound >= first_only_outside_bound);
  CHECK(between_outside_bound < global_outside_bound);
  auto const rejected_outside_before = rejected_outside;
  auto const global_outside_metrics = global_outside_scheduler->metrics();
  plan_lazy_chart_memory_report global_outside_rejection;
  bool global_outside_rejected = false;
  try {
    larch::build_lazy_outside_chart_in_place_scheduled(
        asymmetric_plan, asymmetric_patterns, rejected_outside,
        larch::chart_options{}, *global_outside_scheduler, nullptr, nullptr,
        nullptr,
        plan_lazy_chart_memory_options{.memory_budget_bytes =
                                           between_outside_bound},
        &global_outside_rejection);
  } catch (plan_lazy_chart_memory_budget_error const&) {
    global_outside_rejected = true;
  }
  CHECK(global_outside_rejected);
  CHECK(global_outside_rejection.outside_admission_waves == 0);
  CHECK(global_outside_rejection.pre_submit_rejections == 1);
  CHECK(global_outside_scheduler->metrics().operations ==
        global_outside_metrics.operations);
  check_lazy_charts_equal(rejected_outside_before, rejected_outside);

  auto const outside_w4_singleton_budget =
      exact_outside_w1_budget +
      larch::estimate_chart_scheduler_pool_owning_heap_bytes(4);
  auto outside_split_scheduler = make_scheduler(4);
  auto outside_split = make_outside_input();
  plan_lazy_chart_memory_report outside_split_report;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, outside_split, larch::chart_options{},
      *outside_split_scheduler, nullptr, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes =
                                         outside_w4_singleton_budget},
      &outside_split_report);
  check_lazy_charts_equal(outside_oracle, outside_split);
  CHECK(outside_split_report.outside_max_admitted_slots == 1);
  CHECK(outside_split_report.outside_memory_limited_levels > 0);
  CHECK(outside_split_report.actual_peak_capacity_resident_bytes <=
        outside_w4_singleton_budget);

  // The lowest clade error in a W4-split outside level prevents every later
  // stable-prefix subwave and clears the complete level. W1 admits the complete
  // K=2 prefix with S=1, yet selects the same stable error after its join.
  auto const outside_level_one_begin = plan.top_down_level_offsets()[1];
  CHECK(plan.top_down_level_offsets()[2] - outside_level_one_begin == 2);
  auto const first_outside_level_one_clade =
      plan.top_down_level_order()[outside_level_one_begin];
  auto const later_outside_level_one_clade =
      plan.top_down_level_order()[outside_level_one_begin + 1];
  auto run_outside_split_error = [&](std::size_t workers, std::size_t budget,
                                     bool& later_started, bool& cleanup_seen,
                                     plan_lazy_chart_memory_report& report) {
    auto scheduler = make_scheduler(workers);
    auto chart = make_outside_input();
    plan_lazy_chart_scheduler_test_hooks hooks;
    hooks.before_outside_clade = [&](larch::clade_id clade, std::size_t level,
                                     std::size_t) {
      if (level == 1 && clade == later_outside_level_one_clade) {
        later_started = true;
      }
      if (level == 1 && clade == first_outside_level_one_clade) {
        throw std::runtime_error("finite stable first outside error");
      }
    };
    hooks.observe_outside_level_failure_cleanup =
        [&](larch::lazy_multisite_chart const& failed, std::size_t level) {
          if (level != 1) return;
          cleanup_seen = true;
          for (auto clade : {larch::clade_id{8}, larch::clade_id{9}}) {
            CHECK(failed.outside_rows_by_clade[clade].empty());
            CHECK(failed.outside_class_weight_by_clade[clade].empty());
            CHECK(!failed.outside_class_index_by_pattern_by_clade[clade]);
          }
        };
    return runtime_error_message([&] {
      larch::build_lazy_outside_chart_in_place_scheduled(
          plan, patterns, chart, larch::chart_options{}, *scheduler, nullptr,
          &hooks, nullptr,
          plan_lazy_chart_memory_options{.memory_budget_bytes = budget},
          &report);
    });
  };
  bool outside_w1_later = false;
  bool outside_w1_cleanup = false;
  plan_lazy_chart_memory_report outside_w1_error_report;
  auto const outside_w1_error =
      run_outside_split_error(1, exact_outside_w1_budget, outside_w1_later,
                              outside_w1_cleanup, outside_w1_error_report);
  bool outside_w4_later = false;
  bool outside_w4_cleanup = false;
  plan_lazy_chart_memory_report outside_w4_error_report;
  auto const outside_w4_error =
      run_outside_split_error(4, outside_w4_singleton_budget, outside_w4_later,
                              outside_w4_cleanup, outside_w4_error_report);
  CHECK(outside_w4_error == outside_w1_error);
  CHECK(outside_w1_later);
  CHECK(!outside_w4_later);
  CHECK(outside_w1_cleanup);
  CHECK(outside_w4_cleanup);
  CHECK(outside_w1_error_report.outside_admission_waves > 0);
  CHECK(outside_w4_error_report.outside_admission_waves > 0);
  CHECK(outside_w1_error_report.scheduler_submissions ==
        outside_w1_error_report.outside_admission_waves);
  CHECK(outside_w4_error_report.scheduler_submissions ==
        outside_w4_error_report.outside_admission_waves);

  auto partial_outside_scheduler = make_scheduler(4);
  larch::chart_scheduler_test_detail::access::fail_submission_after(
      *partial_outside_scheduler, 1);
  auto partial_outside = make_outside_input();
  std::vector<larch::chart_scheduler_run_summary> partial_outside_runs;
  plan_lazy_chart_memory_report partial_outside_report;
  bool partial_outside_failed = false;
  try {
    larch::build_lazy_outside_chart_in_place_scheduled(
        plan, patterns, partial_outside, larch::chart_options{},
        *partial_outside_scheduler, &partial_outside_runs, nullptr, nullptr,
        plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
        &partial_outside_report);
  } catch (larch::chart_scheduler_submit_error const&) {
    partial_outside_failed = true;
  }
  CHECK(partial_outside_failed);
  CHECK(partial_outside_runs.size() == 1);
  CHECK(partial_outside_runs.front().failed);
  CHECK(partial_outside_runs.front().worker_tasks_submitted == 1);
  CHECK(partial_outside_report.outside_admission_waves == 1);
  CHECK(partial_outside_report.outside_max_admitted_slots > 1);
  CHECK(partial_outside_report.scheduler_submissions == 1);
  CHECK(partial_outside_scheduler->metrics().operations ==
        partial_outside_report.scheduler_submissions);
  CHECK(partial_outside_scheduler->metrics().pending_tasks == 0);

  // Oversized but shape-compatible W1 scratch is retained under a loose
  // envelope, while the same capacity is evicted before a tight exact build.
  plan_lazy_chart_scheduler_workspace loose_outside_workspace;
  loose_outside_workspace.outside_by_slot.resize(1);
  (void)larch::lazy_chart_detail::prepare_plan_outside_slot(
      loose_outside_workspace.outside_by_slot.front(), patterns.patterns.size(),
      128, 128);
  auto const loose_outside_bytes =
      loose_outside_workspace.outside_capacity_resident_bytes();
  auto loose_outside_scheduler = make_scheduler(1);
  auto loose_outside = make_outside_input();
  plan_lazy_chart_memory_report loose_outside_report;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, loose_outside, larch::chart_options{},
      *loose_outside_scheduler, nullptr, nullptr, &loose_outside_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &loose_outside_report);
  check_lazy_charts_equal(outside_oracle, loose_outside);
  CHECK(loose_outside_report.outside_workspace_evictions == 0);
  CHECK(loose_outside_report.outside_reused_slot_waves > 0);
  CHECK(loose_outside_workspace.outside_capacity_resident_bytes() ==
        loose_outside_bytes);

  plan_lazy_chart_scheduler_workspace tight_outside_workspace;
  tight_outside_workspace.outside_by_slot.resize(1);
  (void)larch::lazy_chart_detail::prepare_plan_outside_slot(
      tight_outside_workspace.outside_by_slot.front(), patterns.patterns.size(),
      128, 128);
  auto tight_outside_scheduler = make_scheduler(1);
  auto tight_outside = make_outside_input();
  plan_lazy_chart_memory_report tight_outside_report;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, tight_outside, larch::chart_options{},
      *tight_outside_scheduler, nullptr, nullptr, &tight_outside_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes =
                                         exact_outside_w1_budget},
      &tight_outside_report);
  check_lazy_charts_equal(outside_oracle, tight_outside);
  CHECK(tight_outside_report.outside_workspace_evictions > 0);
  CHECK(tight_outside_workspace.outside_capacity_resident_bytes() <
        loose_outside_bytes);

  plan_lazy_chart_scheduler_workspace empty_outside_workspace;
  empty_outside_workspace.outside_by_slot.reserve(8);
  auto empty_outside_scheduler = make_scheduler(1);
  auto empty_outside = make_outside_input();
  plan_lazy_chart_memory_report empty_outside_report;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, empty_outside, larch::chart_options{},
      *empty_outside_scheduler, nullptr, nullptr, &empty_outside_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes =
                                         exact_outside_w1_budget},
      &empty_outside_report);
  check_lazy_charts_equal(outside_oracle, empty_outside);
  CHECK(empty_outside_workspace.outside_by_slot.size() == 1);
  CHECK(empty_outside_workspace.outside_by_slot.capacity() == 1);
  CHECK(empty_outside_report.outside_workspace_evictions > 0);

  auto outside_allocation_scheduler = make_scheduler(4);
  auto outside_allocation = make_outside_input();
  plan_lazy_chart_memory_report outside_allocation_report;
  plan_lazy_chart_scheduler_workspace outside_allocation_workspace;
  std::array<allocation_observer, 4> outside_observers;
  plan_lazy_chart_scheduler_test_hooks outside_allocation_hooks;
  outside_allocation_hooks.before_outside_clade =
      [&](larch::clade_id, std::size_t, std::size_t slot) {
        CHECK(slot < outside_observers.size());
        CHECK(allocation_detail::active_observer == nullptr);
        allocation_detail::active_observer = &outside_observers[slot];
      };
  outside_allocation_hooks.after_outside_clade =
      [&](larch::clade_id, std::size_t, std::size_t slot) {
        CHECK(allocation_detail::active_observer == &outside_observers[slot]);
        allocation_detail::active_observer = nullptr;
      };
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, outside_allocation, larch::chart_options{},
      *outside_allocation_scheduler, nullptr, &outside_allocation_hooks,
      &outside_allocation_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &outside_allocation_report);
  check_lazy_charts_equal(outside_oracle, outside_allocation);
  CHECK(outside_allocation_report.outside_max_admitted_slots >= 4);
  for (auto const& observer : outside_observers) {
    CHECK(observer.statistics.calls == 0);
  }

  // Reference-edge root initialization and generic multifurcating recurrence
  // share the finite outside route and remain semantically identical.
  auto const generic_grammar = make_nonlex_multifurcating_grammar();
  auto const generic_plan = larch::build_chart_execution_plan(generic_grammar);
  auto const generic_patterns = make_nonlex_four_taxon_patterns();
  larch::chart_options reference_options;
  reference_options.score_ua_edge = true;
  larch::lazy_chart_options generic_inside_options;
  generic_inside_options.chart = reference_options;
  generic_inside_options.retain_all_inside_class_maps = true;
  auto generic_oracle = larch::build_lazy_inside_chart(
      generic_plan, generic_patterns, generic_inside_options);
  larch::build_lazy_outside_chart_in_place(generic_plan, generic_patterns,
                                           generic_oracle, reference_options,
                                           larch::nuc_base::G);
  auto generic_finite = larch::build_lazy_inside_chart(
      generic_plan, generic_patterns, generic_inside_options);
  auto generic_scheduler = make_scheduler(4);
  plan_lazy_chart_memory_report generic_report;
  larch::build_lazy_outside_chart_in_place_scheduled(
      generic_plan, generic_patterns, generic_finite, reference_options,
      larch::nuc_base::G, *generic_scheduler, nullptr, nullptr, nullptr,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &generic_report);
  check_lazy_charts_equal(generic_oracle, generic_finite);
  CHECK(generic_finite.outside_recurrence_work
            .generic_reusable_productions_scored > 0);
  CHECK(generic_report.actual_peak_capacity_resident_bytes <= huge_budget);
}

static void test_finite_wide_lazy_prefix_admission() {
  std::println("test_finite_wide_lazy_prefix_admission");
  using larch::lazy_chart_detail::plan_lazy_chart_memory_budget_error;
  using larch::lazy_chart_detail::plan_lazy_chart_memory_options;
  using larch::lazy_chart_detail::plan_lazy_chart_memory_report;
  using larch::lazy_chart_detail::plan_lazy_chart_scheduler_workspace;

  constexpr auto workers = std::size_t{8};
  constexpr auto huge_budget = std::size_t{1} << 40;
  auto const grammar = make_wide_star_grammar();
  auto const plan = larch::build_chart_execution_plan(grammar);
  auto const patterns = make_wide_star_patterns();
  larch::lazy_chart_options inside_options;
  inside_options.retain_all_inside_class_maps = true;
  auto const inside_oracle =
      larch::build_lazy_inside_chart(plan, patterns, inside_options);
  auto make_scheduler = [=] {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = workers,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 4,
        });
  };
  auto prepare_inside_workspace = [&] {
    plan_lazy_chart_scheduler_workspace workspace;
    workspace.hard_bound_inside_slots(workers);
    for (auto& slot : workspace.inside_by_slot) {
      (void)larch::lazy_chart_detail::prepare_plan_inside_slot(
          slot, patterns.patterns.size(), 128);
    }
    return workspace;
  };
  auto prepare_outside_workspace = [&] {
    plan_lazy_chart_scheduler_workspace workspace;
    workspace.hard_bound_outside_slots(workers);
    for (auto& slot : workspace.outside_by_slot) {
      (void)larch::lazy_chart_detail::prepare_plan_outside_slot(
          slot, patterns.patterns.size(), 128, 128);
    }
    return workspace;
  };

  // K is the complete dependency-level prefix while only S=min(W,K) scratch
  // slots are retained. The 64-leaf level therefore needs one scheduler join,
  // and its W8 scratch remains reusable for the following singleton root.
  auto inside_scheduler = make_scheduler();
  auto inside_workspace = prepare_inside_workspace();
  plan_lazy_chart_memory_report inside_report;
  std::vector<larch::chart_scheduler_run_summary> inside_runs;
  auto inside = larch::build_lazy_inside_chart_scheduled(
      plan, patterns, inside_options, *inside_scheduler, &inside_runs, nullptr,
      &inside_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &inside_report);
  check_lazy_charts_equal(inside_oracle, inside);
  CHECK(inside_runs.size() == 2);
  CHECK(inside_runs[0].item_count == 64);
  CHECK(inside_runs[1].item_count == 1);
  CHECK(inside_report.inside_admission_waves == 2);
  CHECK(inside_report.inside_max_admitted_slots == workers);
  CHECK(inside_report.inside_memory_limited_levels == 0);
  CHECK(inside_report.inside_reused_slot_waves == 2);
  CHECK(inside_report.inside_workspace_evictions == 0);
  CHECK(inside_workspace.inside_by_slot.size() == workers);
  CHECK(inside_workspace.inside_by_slot.capacity() == workers);

  auto outside_oracle = inside_oracle;
  larch::build_lazy_outside_chart_in_place(plan, patterns, outside_oracle,
                                           larch::chart_options{});
  auto outside_scheduler = make_scheduler();
  auto outside_workspace = prepare_outside_workspace();
  auto outside = inside_oracle;
  plan_lazy_chart_memory_report outside_report;
  std::vector<larch::chart_scheduler_run_summary> outside_runs;
  larch::build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, outside, larch::chart_options{}, *outside_scheduler,
      &outside_runs, nullptr, &outside_workspace,
      plan_lazy_chart_memory_options{.memory_budget_bytes = huge_budget},
      &outside_report);
  check_lazy_charts_equal(outside_oracle, outside);
  CHECK(outside_runs.size() == 1);
  CHECK(outside_runs.front().item_count == 64);
  CHECK(outside_report.outside_admission_waves == 1);
  CHECK(outside_report.outside_max_admitted_slots == workers);
  CHECK(outside_report.outside_memory_limited_levels == 0);
  CHECK(outside_report.outside_reused_slot_waves == 1);
  CHECK(outside_report.outside_workspace_evictions == 0);
  CHECK(outside_workspace.outside_by_slot.size() == workers);
  CHECK(outside_workspace.outside_by_slot.capacity() == workers);

  auto make_tight_workspace = [] {
    plan_lazy_chart_scheduler_workspace workspace;
    return workspace;
  };

  // Freeze a full first outside wave, then reject only its scheduler-operation
  // envelope. The prepared chart/workspaces are live and within budget; the
  // operation estimate belongs to preflight until scheduler entry.
  auto const projection_patterns = make_wide_star_patterns(64, 1);
  larch::lazy_chart_options projection_inside_options;
  projection_inside_options.retain_all_inside_class_maps = true;
  auto const projection_inside = larch::build_lazy_inside_chart(
      plan, projection_patterns, projection_inside_options);
  auto projection_oracle = projection_inside;
  larch::build_lazy_outside_chart_in_place(
      plan, projection_patterns, projection_oracle, larch::chart_options{});

  auto build_projection_inside =
      [&](larch::chart_scheduler& scheduler,
          plan_lazy_chart_scheduler_workspace& workspace,
          std::vector<larch::chart_scheduler_run_summary>* runs,
          std::size_t budget, plan_lazy_chart_memory_report* report,
          larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks const*
              hooks = nullptr) {
        auto chart =
            larch::lazy_chart_detail::initialize_plan_lazy_inside_chart(
                plan, projection_patterns, projection_inside_options);
        return larch::lazy_chart_detail::
            build_plan_lazy_inside_chart_scheduled_finite(
                std::move(chart), plan, projection_patterns,
                projection_inside_options, scheduler,
                plan.bottom_up_level_order(), plan.bottom_up_level_offsets(),
                runs, hooks, workspace,
                plan_lazy_chart_memory_options{.memory_budget_bytes = budget},
                report);
      };
  std::size_t projection_first_inside_level_chart_bytes = 0;
  struct projection_inside_first_level_complete {};
  larch::lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks
      projection_inside_hooks;
  projection_inside_hooks.observe_inside_level_join =
      [&](larch::lazy_multisite_chart const& chart, std::size_t level) {
        if (level == 0) {
          projection_first_inside_level_chart_bytes = larch::lazy_chart_detail::
              lazy_multisite_chart_capacity_resident_bytes(chart);
          throw projection_inside_first_level_complete{};
        }
      };
  auto projection_inside_calibration_scheduler = make_scheduler();
  auto projection_inside_calibration_workspace = make_tight_workspace();
  std::vector<larch::chart_scheduler_run_summary>
      projection_inside_calibration_runs;
  plan_lazy_chart_memory_report projection_inside_calibration_report;
  bool projection_inside_first_level_observed = false;
  try {
    (void)build_projection_inside(
        *projection_inside_calibration_scheduler,
        projection_inside_calibration_workspace,
        &projection_inside_calibration_runs, huge_budget,
        &projection_inside_calibration_report, &projection_inside_hooks);
  } catch (projection_inside_first_level_complete const&) {
    projection_inside_first_level_observed = true;
  }
  CHECK(projection_inside_first_level_observed);
  CHECK(projection_inside_calibration_runs.size() == 1);
  CHECK(projection_inside_calibration_report.inside_admission_waves == 1);
  CHECK(projection_inside_calibration_report.scheduler_submissions == 1);
  CHECK(projection_first_inside_level_chart_bytes > 0);
  auto inside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_scheduler_resident_bytes(
          *projection_inside_calibration_scheduler);
  inside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          inside_scheduler_projection,
          projection_first_inside_level_chart_bytes,
          "test inside scheduler projection");
  inside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          inside_scheduler_projection,
          projection_inside_calibration_report
              .inside_coordinator_capacity_resident_bytes,
          "test inside scheduler projection");
  inside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          inside_scheduler_projection,
          projection_inside_calibration_workspace
              .inside_capacity_resident_bytes(),
          "test inside scheduler projection");
  inside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          inside_scheduler_projection,
          larch::estimate_chart_scheduler_operation_peak_bytes(
              projection_inside_calibration_scheduler->plan_indexed_ranges(
                  64, larch::lazy_chart_detail::
                          plan_lazy_chart_clade_range_options())),
          "test inside scheduler projection");
  CHECK(inside_scheduler_projection > 1);
  CHECK(
      inside_scheduler_projection ==
      projection_inside_calibration_report.actual_peak_capacity_resident_bytes);

  auto projection_inside_rejected_scheduler = make_scheduler();
  auto const projection_inside_rejected_operations =
      projection_inside_rejected_scheduler->metrics().operations;
  auto projection_inside_rejected_workspace = make_tight_workspace();
  std::vector<larch::chart_scheduler_run_summary>
      projection_inside_rejected_runs;
  plan_lazy_chart_memory_report projection_inside_rejected_report;
  bool inside_projection_rejected = false;
  try {
    (void)build_projection_inside(
        *projection_inside_rejected_scheduler,
        projection_inside_rejected_workspace, &projection_inside_rejected_runs,
        inside_scheduler_projection - 1, &projection_inside_rejected_report);
  } catch (plan_lazy_chart_memory_budget_error const& error) {
    inside_projection_rejected = true;
    CHECK(error.phase() ==
          larch::lazy_chart_detail::plan_lazy_chart_memory_phase::inside);
    CHECK(error.required_bytes() == inside_scheduler_projection);
    CHECK(error.budget_bytes() == inside_scheduler_projection - 1);
  }
  CHECK(inside_projection_rejected);
  CHECK(projection_inside_rejected_report
            .preflight_peak_capacity_resident_bytes >=
        inside_scheduler_projection);
  CHECK(projection_inside_rejected_report.actual_peak_capacity_resident_bytes <=
        inside_scheduler_projection - 1);
  CHECK(projection_inside_rejected_report.pre_submit_rejections == 1);
  CHECK(projection_inside_rejected_report.inside_admission_waves == 0);
  CHECK(projection_inside_rejected_report.scheduler_submissions == 0);
  CHECK(projection_inside_rejected_runs.empty());
  CHECK(projection_inside_rejected_scheduler->metrics().operations ==
        projection_inside_rejected_operations);

  auto build_projection_outside =
      [&](larch::chart_scheduler& scheduler,
          plan_lazy_chart_scheduler_workspace& workspace,
          std::vector<larch::chart_scheduler_run_summary>* runs,
          std::size_t budget, plan_lazy_chart_memory_report* report) {
        auto chart = projection_inside;
        larch::lazy_chart_detail::initialize_plan_lazy_outside_chart(
            plan, projection_patterns, chart, larch::chart_options{}, 0);
        larch::lazy_chart_detail::
            build_plan_lazy_outside_chart_scheduled_finite(
                chart, plan, projection_patterns, scheduler,
                plan.top_down_level_order(), plan.top_down_level_offsets(),
                runs, nullptr, workspace,
                plan_lazy_chart_memory_options{.memory_budget_bytes = budget},
                report);
        return chart;
      };
  auto projection_calibration_scheduler = make_scheduler();
  auto projection_calibration_workspace = make_tight_workspace();
  std::vector<larch::chart_scheduler_run_summary> projection_calibration_runs;
  plan_lazy_chart_memory_report projection_calibration_report;
  auto projection_calibration = build_projection_outside(
      *projection_calibration_scheduler, projection_calibration_workspace,
      &projection_calibration_runs, huge_budget,
      &projection_calibration_report);
  check_lazy_charts_equal(projection_oracle, projection_calibration);
  CHECK(projection_calibration_runs.size() == 1);
  auto outside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_scheduler_resident_bytes(
          *projection_calibration_scheduler);
  outside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          outside_scheduler_projection,
          larch::lazy_chart_detail::
              lazy_multisite_chart_capacity_resident_bytes(
                  projection_calibration),
          "test outside scheduler projection");
  outside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          outside_scheduler_projection,
          projection_calibration_report
              .outside_coordinator_capacity_resident_bytes,
          "test outside scheduler projection");
  outside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          outside_scheduler_projection,
          projection_calibration_workspace.outside_capacity_resident_bytes(),
          "test outside scheduler projection");
  outside_scheduler_projection =
      larch::lazy_chart_detail::plan_lazy_chart_capacity_add(
          outside_scheduler_projection,
          larch::estimate_chart_scheduler_operation_peak_bytes(
              projection_calibration_scheduler->plan_indexed_ranges(
                  64, larch::lazy_chart_detail::
                          plan_lazy_chart_clade_range_options())),
          "test outside scheduler projection");
  CHECK(outside_scheduler_projection > 1);

  auto projection_rejected_scheduler = make_scheduler();
  auto const projection_rejected_operations =
      projection_rejected_scheduler->metrics().operations;
  auto projection_rejected_workspace = make_tight_workspace();
  std::vector<larch::chart_scheduler_run_summary> projection_rejected_runs;
  plan_lazy_chart_memory_report projection_rejected_report;
  bool outside_projection_rejected = false;
  try {
    (void)build_projection_outside(
        *projection_rejected_scheduler, projection_rejected_workspace,
        &projection_rejected_runs, outside_scheduler_projection - 1,
        &projection_rejected_report);
  } catch (plan_lazy_chart_memory_budget_error const& error) {
    outside_projection_rejected = true;
    CHECK(error.phase() ==
          larch::lazy_chart_detail::plan_lazy_chart_memory_phase::outside);
    CHECK(error.required_bytes() == outside_scheduler_projection);
    CHECK(error.budget_bytes() == outside_scheduler_projection - 1);
  }
  CHECK(outside_projection_rejected);
  CHECK(projection_rejected_report.preflight_peak_capacity_resident_bytes >=
        outside_scheduler_projection);
  CHECK(projection_rejected_report.actual_peak_capacity_resident_bytes <=
        outside_scheduler_projection - 1);
  CHECK(projection_rejected_report.pre_submit_rejections == 1);
  CHECK(projection_rejected_report.outside_admission_waves == 0);
  CHECK(projection_rejected_report.scheduler_submissions == 0);
  CHECK(projection_rejected_runs.empty());
  CHECK(projection_rejected_scheduler->metrics().operations ==
        projection_rejected_operations);

  // With one pattern and binary row shapes, an W8 scheduler can fit the final
  // singleton at a budget that cannot admit S=8 scratch for every preceding
  // parallel level. Admission must split maximal stable prefixes and retain the
  // smaller cold allocation across every following narrower level.
  auto const tight_grammar = make_wide_balanced_binary_grammar(16);
  auto const tight_plan = larch::build_chart_execution_plan(tight_grammar);
  auto const tight_patterns = make_wide_star_patterns(16, 1);
  larch::lazy_chart_options tight_inside_options;
  tight_inside_options.retain_all_inside_class_maps = false;
  auto const tight_oracle = larch::build_lazy_inside_chart(
      tight_plan, tight_patterns, tight_inside_options);
  auto build_tight_finite =
      [&](larch::chart_scheduler& scheduler,
          plan_lazy_chart_scheduler_workspace& workspace,
          std::vector<larch::chart_scheduler_run_summary>* runs,
          std::size_t budget, plan_lazy_chart_memory_report* report = nullptr) {
        auto chart =
            larch::lazy_chart_detail::initialize_plan_lazy_inside_chart(
                tight_plan, tight_patterns, tight_inside_options);
        return larch::lazy_chart_detail::
            build_plan_lazy_inside_chart_scheduled_finite(
                std::move(chart), tight_plan, tight_patterns,
                tight_inside_options, scheduler,
                tight_plan.bottom_up_level_order(),
                tight_plan.bottom_up_level_offsets(), runs, nullptr, workspace,
                plan_lazy_chart_memory_options{.memory_budget_bytes = budget},
                report);
      };
  auto calibration_scheduler = make_scheduler();
  auto calibration_workspace = make_tight_workspace();
  plan_lazy_chart_memory_report tight_calibration_report;
  std::vector<larch::chart_scheduler_run_summary> tight_calibration_runs;
  auto tight_calibration = build_tight_finite(
      *calibration_scheduler, calibration_workspace, &tight_calibration_runs,
      huge_budget, &tight_calibration_report);
  check_lazy_charts_equal(tight_oracle, tight_calibration);
  auto minimum_successful_budget =
      std::max(tight_calibration_report.preflight_peak_capacity_resident_bytes,
               tight_calibration_report.actual_peak_capacity_resident_bytes);
  CHECK(minimum_successful_budget > 1);
  auto probe_scheduler = make_scheduler();
  auto can_build_at = [&](std::size_t budget) {
    auto workspace = make_tight_workspace();
    std::vector<larch::chart_scheduler_run_summary> runs;
    try {
      (void)build_tight_finite(*probe_scheduler, workspace, &runs, budget);
      return true;
    } catch (plan_lazy_chart_memory_budget_error const&) {
      return false;
    }
  };
  CHECK(!can_build_at(1));
  CHECK(can_build_at(minimum_successful_budget));
  auto rejected_budget = std::size_t{1};
  while (rejected_budget + 1 < minimum_successful_budget) {
    auto const midpoint =
        rejected_budget + (minimum_successful_budget - rejected_budget) / 2;
    if (can_build_at(midpoint)) {
      minimum_successful_budget = midpoint;
    } else {
      rejected_budget = midpoint;
    }
  }
  CHECK(minimum_successful_budget == rejected_budget + 1);

  // The final one-under rejection is the post-preparation scheduler-operation
  // projection. It remains preflight-only until scheduler entry, while the
  // stable prepared live set remains within budget. The rejected final wave is
  // not published, and every earlier successful wave reconciles with one
  // scheduler submission and operation.
  auto rejected_scheduler = make_scheduler();
  auto const rejected_operations = rejected_scheduler->metrics().operations;
  auto rejected_workspace = make_tight_workspace();
  std::vector<larch::chart_scheduler_run_summary> rejected_runs;
  plan_lazy_chart_memory_report rejected_report;
  bool projection_rejected = false;
  std::size_t rejected_required = 0;
  try {
    (void)build_tight_finite(*rejected_scheduler, rejected_workspace,
                             &rejected_runs, rejected_budget, &rejected_report);
  } catch (plan_lazy_chart_memory_budget_error const& error) {
    projection_rejected = true;
    rejected_required = error.required_bytes();
    CHECK(error.required_bytes() == minimum_successful_budget);
    CHECK(error.budget_bytes() == rejected_budget);
  }
  CHECK(projection_rejected);
  CHECK(rejected_report.preflight_peak_capacity_resident_bytes >=
        rejected_required);
  CHECK(rejected_report.actual_peak_capacity_resident_bytes <= rejected_budget);
  CHECK(rejected_report.pre_submit_rejections == 1);
  CHECK(rejected_report.inside_admission_waves ==
        rejected_report.scheduler_submissions);
  CHECK(rejected_runs.size() == rejected_report.scheduler_submissions);
  CHECK(rejected_scheduler->metrics().operations - rejected_operations ==
        rejected_report.scheduler_submissions);

  auto tight_scheduler = make_scheduler();
  auto tight_workspace = make_tight_workspace();
  plan_lazy_chart_memory_report tight_report;
  std::vector<larch::chart_scheduler_run_summary> tight_runs;
  auto tight =
      build_tight_finite(*tight_scheduler, tight_workspace, &tight_runs,
                         minimum_successful_budget, &tight_report);
  check_lazy_charts_equal(tight_oracle, tight);
  auto const tight_level_count =
      tight_plan.bottom_up_level_offsets().size() - 1;
  CHECK(tight_runs.size() > tight_level_count);
  CHECK(std::accumulate(tight_runs.begin(), tight_runs.end(), std::size_t{0},
                        [](std::size_t total, auto const& run) {
                          return total + run.item_count;
                        }) == tight_plan.clades().size());
  CHECK(tight_report.inside_memory_limited_levels > 0);
  CHECK(tight_report.inside_max_admitted_slots < workers);
  CHECK(tight_report.inside_workspace_evictions == 0);
  CHECK(tight_report.inside_reused_slot_waves + 1 == tight_runs.size());
  CHECK(tight_workspace.inside_by_slot.size() ==
        tight_report.inside_max_admitted_slots);
  CHECK(tight_workspace.inside_by_slot.capacity() ==
        tight_report.inside_max_admitted_slots);
}

static void test_scheduler_memory_estimate_allocation_bounds() {
  std::println("test_scheduler_memory_estimate_allocation_bounds");
  using larch::test::chart_spr_allocation::allocation_observer;
  using larch::test::chart_spr_allocation::scoped_allocation_observation;

  auto const scheduler_options = larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 1,
  };
  std::optional<larch::chart_scheduler> scheduler;
  allocation_observer implementation_observer;
  {
    scoped_allocation_observation observation{implementation_observer};
    scheduler.emplace(scheduler_options);
  }
  CHECK(scheduler->worker_resolution().resolved_workers == 4);
  CHECK(implementation_observer.statistics.calls > 0);
  CHECK(implementation_observer.statistics.requested_bytes <=
        larch::estimate_chart_scheduler_implementation_resident_bytes());

  auto const range_options = larch::chart_indexed_range_options{
      .minimum_grain = 1,
      .target_ranges_per_worker = 1,
  };
  auto const operation_plan = scheduler->plan_indexed_ranges(4, range_options);
  CHECK(operation_plan.range_count == 4);
  CHECK(operation_plan.worker_task_limit == 4);
  auto const operation_bound =
      larch::estimate_chart_scheduler_operation_peak_bytes(operation_plan);
  auto const cold_bound =
      larch::estimate_chart_scheduler_pool_owning_heap_bytes(4) +
      operation_bound;

  // Observation is deliberately limited to replaceable operator-new calls on
  // the coordinator. std::thread's native pthread/TLS/stack allocations use
  // implementation-owned malloc paths, are not visible here, and are the same
  // explicitly excluded native ownership documented by the estimator API.
  std::array<allocation_observer, 4> cold_callback_observers;
  std::atomic<std::size_t> cold_items = 0;
  auto cold_callback = [&](larch::chart_indexed_range const& range,
                           std::size_t stable_slot,
                           larch::chart_scheduler_cancellation_token const&) {
    scoped_allocation_observation observation{
        cold_callback_observers[stable_slot]};
    cold_items.fetch_add(range.end - range.begin, std::memory_order_relaxed);
  };
  allocation_observer cold_operation_observer;
  larch::chart_scheduler_run_summary cold_run;
  {
    scoped_allocation_observation observation{cold_operation_observer};
    cold_run =
        scheduler->for_each_indexed_range(4, range_options, cold_callback);
  }
  CHECK(cold_run.used_parallel_workers());
  CHECK(cold_items.load(std::memory_order_relaxed) == 4);
  CHECK(cold_operation_observer.statistics.calls > 0);
  CHECK(cold_operation_observer.statistics.requested_bytes <= cold_bound);
  for (auto const& observer : cold_callback_observers) {
    CHECK(observer.statistics.calls == 0);
  }

  std::array<allocation_observer, 4> warm_callback_observers;
  std::atomic<std::size_t> warm_items = 0;
  auto warm_callback = [&](larch::chart_indexed_range const& range,
                           std::size_t stable_slot,
                           larch::chart_scheduler_cancellation_token const&) {
    scoped_allocation_observation observation{
        warm_callback_observers[stable_slot]};
    warm_items.fetch_add(range.end - range.begin, std::memory_order_relaxed);
  };
  allocation_observer warm_operation_observer;
  larch::chart_scheduler_run_summary warm_run;
  {
    scoped_allocation_observation observation{warm_operation_observer};
    warm_run =
        scheduler->for_each_indexed_range(4, range_options, warm_callback);
  }
  CHECK(warm_run.used_parallel_workers());
  CHECK(warm_items.load(std::memory_order_relaxed) == 4);
  CHECK(warm_operation_observer.statistics.calls > 0);
  CHECK(warm_operation_observer.statistics.requested_bytes <= operation_bound);
  for (auto const& observer : warm_callback_observers) {
    CHECK(observer.statistics.calls == 0);
  }
}

static void test_plan_packed_key_narrowing_and_accounting() {
  std::println("test_plan_packed_key_narrowing_and_accounting");
  using larch::lazy_key_grouping_detail::checked_packed_key_word;
  using larch::lazy_key_grouping_detail::packed_key_word;
  auto const word_max =
      static_cast<std::size_t>((std::numeric_limits<packed_key_word>::max)());
  CHECK(checked_packed_key_word(word_max, "test plan class index") ==
        (std::numeric_limits<packed_key_word>::max)());
  if ((std::numeric_limits<std::size_t>::max)() > word_max) {
    auto const message = runtime_error_message([&] {
      (void)checked_packed_key_word(word_max + 1, "test plan class index");
    });
    CHECK(message.find("test plan class index") != std::string::npos);
    CHECK(message.find("does not fit") != std::string::npos);
  }

  auto const logical = larch::lazy_chart_detail::
      estimate_plan_parent_key_grouping_logical_resident_bytes(6, 2, 5, 4, 4);
  CHECK(logical > sizeof(larch::lazy_chart_detail::plan_parent_key_workspace));
  auto const overflow = runtime_error_message([&] {
    (void)larch::lazy_chart_detail::
        estimate_plan_parent_key_grouping_logical_resident_bytes(
            (std::numeric_limits<std::size_t>::max)(), 2, 1, 1, 1);
  });
  CHECK(overflow.find("overflow") != std::string::npos);

  auto grammar = make_nonlex_binary_dag_grammar();
  auto const plan = larch::build_chart_execution_plan(grammar);
  auto const patterns = make_nonlex_four_taxon_patterns();
  larch::lazy_chart_options options;
  options.retain_all_inside_class_maps = true;
  auto chart = larch::build_lazy_inside_chart(plan, patterns, options);
  larch::lazy_chart_detail::plan_parent_key_workspace workspace;
  {
    auto keys = larch::lazy_chart_detail::collect_plan_parent_keys(
        chart, plan, patterns, larch::clade_id{4}, workspace);
    CHECK(keys.memory.structural_key_count == patterns.patterns.size());
    CHECK(keys.memory.structural_key_width == 2);
    CHECK(keys.memory.row_key_count == keys.structural_classes().class_count());
    CHECK(keys.memory.row_key_count < keys.memory.structural_key_count);
    CHECK(keys.memory.row_key_width == 2);
    CHECK(keys.row_key_words().size() ==
          keys.memory.row_key_count * keys.memory.row_key_width);
    CHECK(keys.memory.actual_capacity_resident_bytes >=
          keys.memory.logical_resident_bytes);
    CHECK(keys.memory.observed_prepublication_peak_capacity_resident_bytes >=
          keys.memory.actual_capacity_resident_bytes);
  }
  auto reused = larch::lazy_chart_detail::collect_plan_parent_keys(
      chart, plan, patterns, larch::clade_id{4}, workspace);
  CHECK(reused.memory.structural_word_preparation.reused_existing_capacity);
  CHECK(reused.memory.structural_grouping_preparation.reused_existing_capacity);
  CHECK(reused.memory.row_word_preparation.reused_existing_capacity);
  CHECK(reused.memory.row_grouping_preparation.reused_existing_capacity);

  auto owned = larch::lazy_chart_detail::collect_plan_parent_keys(
      chart, plan, patterns, larch::clade_id{4});
  CHECK(owned.owned_storage != nullptr);
  CHECK(owned.storage == owned.owned_storage.get());
  std::size_t multifurcation_counter = 0;
  auto const owned_row =
      larch::lazy_chart_detail::compute_plan_internal_inside_row_from_keys(
          chart, plan, larch::clade_id{4}, owned, 0, multifurcation_counter);
  CHECK(owned_row == chart.inside_row(larch::clade_id{4}, 0));
  CHECK(multifurcation_counter == 0);
}

static void test_grammar_packed_key_first_occurrence_reuse_and_accounting() {
  std::println("test_grammar_packed_key_first_occurrence_reuse_and_accounting");
  using larch::lazy_key_grouping_detail::checked_packed_key_word;
  using larch::lazy_key_grouping_detail::packed_key_word;
  auto const word_max =
      static_cast<std::size_t>((std::numeric_limits<packed_key_word>::max)());
  CHECK(checked_packed_key_word(word_max, "test grammar class index") ==
        (std::numeric_limits<packed_key_word>::max)());
  if ((std::numeric_limits<std::size_t>::max)() > word_max) {
    auto const message = runtime_error_message([&] {
      (void)checked_packed_key_word(word_max + 1, "test grammar class index");
    });
    CHECK(message.find("test grammar class index") != std::string::npos);
    CHECK(message.find("does not fit") != std::string::npos);
  }

  auto const logical = larch::lazy_chart_detail::
      estimate_grammar_parent_key_grouping_logical_resident_bytes(6, 2, 5, 2,
                                                                  5);
  CHECK(logical >
        sizeof(larch::lazy_chart_detail::grammar_parent_key_workspace));
  auto const overflow = runtime_error_message([&] {
    (void)larch::lazy_chart_detail::
        estimate_grammar_parent_key_grouping_logical_resident_bytes(
            (std::numeric_limits<std::size_t>::max)(), 2, 1, 1, 1);
  });
  CHECK(overflow.find("overflow") != std::string::npos);

  auto const grammar = make_nonlex_binary_dag_grammar();
  auto const patterns = make_nonlex_four_taxon_patterns();
  larch::lazy_chart_options options;
  options.retain_all_inside_class_maps = true;
  auto chart = larch::build_lazy_inside_chart(grammar, patterns, options);
  larch::lazy_chart_detail::grammar_parent_key_workspace workspace;
  {
    auto const keys =
        larch::lazy_chart_detail::collect_ready_grammar_parent_keys(
            chart, grammar, patterns, larch::clade_id{4}, nullptr, workspace);
    CHECK(keys.memory.structural_key_count == 6);
    CHECK(keys.memory.structural_key_width == 2);
    CHECK(keys.memory.row_key_count == 5);
    CHECK(keys.memory.row_key_width == 2);
    CHECK(keys.memory.row_key_class_count == 5);
    CHECK((keys.structural_classes().class_by_input ==
           std::vector<std::size_t>{0, 1, 0, 2, 3, 4}));
    CHECK((keys.structural_classes().representative_by_class ==
           std::vector<std::size_t>{0, 1, 3, 4, 5}));
    CHECK((keys.structural_classes().lexicographic_class_order ==
           std::vector<std::size_t>{0, 3, 2, 1, 4}));
    CHECK(keys.row_key_words().size() ==
          keys.memory.row_key_count * keys.memory.row_key_width);
    CHECK(keys.memory.actual_capacity_resident_bytes >=
          keys.memory.logical_resident_bytes);
    CHECK(keys.memory.observed_prepublication_peak_capacity_resident_bytes >=
          keys.memory.actual_capacity_resident_bytes);

    auto const& representatives =
        keys.structural_classes().representative_by_class;
    for (std::size_t structural_class = 0;
         structural_class < representatives.size(); ++structural_class) {
      std::size_t multifurcation_counter = 0;
      auto const row = larch::lazy_chart_detail::
          compute_grammar_internal_inside_row_from_keys(
              chart, grammar, larch::clade_id{4}, keys, structural_class,
              multifurcation_counter);
      CHECK(row == chart.inside_row(larch::clade_id{4},
                                    representatives[structural_class]));
      CHECK(multifurcation_counter == 0);
    }
  }

  auto const reused =
      larch::lazy_chart_detail::collect_ready_grammar_parent_keys(
          chart, grammar, patterns, larch::clade_id{4}, nullptr, workspace);
  CHECK(reused.memory.structural_word_preparation.reused_existing_capacity);
  CHECK(reused.memory.structural_grouping_preparation.reused_existing_capacity);
  CHECK(reused.memory.row_word_preparation.reused_existing_capacity);
  CHECK(reused.memory.row_grouping_preparation.reused_existing_capacity);
}

static std::vector<std::size_t> outside_context_key_from_chart(
    larch::lazy_multisite_chart const& chart,
    larch::grammar_production const& production, std::size_t pattern) {
  auto const& outside_map =
      chart.outside_class_index_by_pattern_by_clade[production.parent];
  CHECK(outside_map.has_value());
  std::vector<std::size_t> key;
  key.reserve(production.children.size() + 1);
  key.push_back((*outside_map)[pattern]);
  for (auto child : production.children) {
    auto const& inside_map = chart.class_index_by_pattern_by_clade[child];
    CHECK(inside_map.has_value());
    key.push_back((*inside_map)[pattern]);
  }
  return key;
}

static void check_packed_outside_context_against_ordered_map(
    larch::clade_grammar const& grammar,
    larch::chart_execution_plan const& plan,
    larch::site_pattern_set const& patterns,
    larch::lazy_multisite_chart const& chart, larch::production_id pid,
    std::size_t expected_arity,
    std::vector<std::size_t> const& expected_class_by_input,
    std::vector<std::size_t> const& expected_representatives,
    std::vector<std::size_t> const& expected_lexicographic_order,
    bool require_collision) {
  auto const& production = grammar.productions[pid];
  CHECK(production.children.size() == expected_arity);

  std::map<std::vector<std::size_t>, std::vector<std::size_t>> ordered_oracle;
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    ordered_oracle[outside_context_key_from_chart(chart, production, pattern)]
        .push_back(pattern);
  }

  larch::lazy_chart_detail::outside_context_key_workspace grammar_workspace;
  larch::lazy_chart_detail::outside_context_key_workspace plan_workspace;
  {
    auto const grammar_keys =
        larch::lazy_chart_detail::collect_outside_context_keys(
            chart, grammar, patterns, production, grammar_workspace);
    auto const plan_keys =
        larch::lazy_chart_detail::collect_outside_context_keys(
            chart, plan, patterns, pid, plan_workspace);
    auto const& classes = grammar_keys.classes();

    CHECK(grammar_keys.key_width == expected_arity + 1);
    CHECK(grammar_keys.memory.key_count == chart.pattern_count);
    CHECK(grammar_keys.memory.key_width == grammar_keys.key_width);
    CHECK(grammar_keys.memory.class_count == ordered_oracle.size());
    CHECK(grammar_keys.key_words().size() ==
          chart.pattern_count * grammar_keys.key_width);
    CHECK(classes.class_by_input == expected_class_by_input);
    CHECK(classes.representative_by_class == expected_representatives);
    CHECK(classes.lexicographic_class_order ==
          expected_lexicographic_order);
    CHECK(grammar_keys.memory.actual_capacity_resident_bytes >=
          grammar_keys.memory.logical_resident_bytes);
    CHECK(
        grammar_keys.memory
            .observed_prepublication_peak_capacity_resident_bytes >=
        grammar_keys.memory.actual_capacity_resident_bytes);
    CHECK(plan_keys.memory.actual_capacity_resident_bytes >=
          plan_keys.memory.logical_resident_bytes);
    CHECK(plan_keys.memory.observed_prepublication_peak_capacity_resident_bytes >=
          plan_keys.memory.actual_capacity_resident_bytes);
    CHECK(std::ranges::equal(grammar_keys.key_words(), plan_keys.key_words()));
    CHECK(grammar_keys.classes() == plan_keys.classes());

    for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
      auto const expected_key =
          outside_context_key_from_chart(chart, production, pattern);
      for (std::size_t word = 0; word < expected_key.size(); ++word) {
        CHECK(grammar_keys.key_words()[pattern * grammar_keys.key_width + word] ==
              larch::lazy_key_grouping_detail::checked_packed_key_word(
                  expected_key[word], "test outside context key"));
      }
    }

    std::vector<std::size_t> expected_lexicographic_class_order;
    for (auto const& [key, members] : ordered_oracle) {
      (void)key;
      auto const context_class = classes.class_by_input[members.front()];
      expected_lexicographic_class_order.push_back(context_class);
      CHECK(classes.representative_by_class[context_class] == members.front());
      CHECK(std::vector<std::size_t>(
                classes.members_for_class(context_class).begin(),
                classes.members_for_class(context_class).end()) == members);
      for (auto member : members) {
        CHECK(classes.class_by_input[member] == context_class);
      }
    }
    CHECK(classes.lexicographic_class_order ==
          expected_lexicographic_class_order);

    {
      bool has_nonlex_position = false;
      for (std::size_t position = 0;
           position < classes.lexicographic_class_order.size(); ++position) {
        has_nonlex_position |=
            classes.lexicographic_class_order[position] != position;
      }
      CHECK(has_nonlex_position);
    }
    if (require_collision) {
      CHECK(classes.class_count() < chart.pattern_count);
    }
  }

  auto const reused_grammar =
      larch::lazy_chart_detail::collect_outside_context_keys(
          chart, grammar, patterns, production, grammar_workspace);
  CHECK(reused_grammar.memory.word_preparation.reused_existing_capacity);
  CHECK(reused_grammar.memory.grouping_preparation.reused_existing_capacity);
  auto const reused_plan =
      larch::lazy_chart_detail::collect_outside_context_keys(
          chart, plan, patterns, pid, plan_workspace);
  CHECK(reused_plan.memory.word_preparation.reused_existing_capacity);
  CHECK(reused_plan.memory.grouping_preparation.reused_existing_capacity);
}

static void test_packed_outside_context_order_reuse_and_accounting() {
  std::println("test_packed_outside_context_order_reuse_and_accounting");
  using larch::lazy_key_grouping_detail::checked_packed_key_word;
  using larch::lazy_key_grouping_detail::packed_key_word;
  auto const word_max =
      static_cast<std::size_t>((std::numeric_limits<packed_key_word>::max)());
  CHECK(checked_packed_key_word(word_max, "test outside context class index") ==
        (std::numeric_limits<packed_key_word>::max)());
  if ((std::numeric_limits<std::size_t>::max)() > word_max) {
    auto const message = runtime_error_message([&] {
      (void)checked_packed_key_word(word_max + 1,
                                    "test outside context class index");
    });
    CHECK(message.find("test outside context class index") !=
          std::string::npos);
    CHECK(message.find("does not fit") != std::string::npos);
  }

  auto const logical = larch::lazy_chart_detail::
      estimate_outside_context_key_grouping_logical_resident_bytes(6, 3, 5);
  CHECK(logical >
        sizeof(larch::lazy_chart_detail::outside_context_key_workspace));
  auto const overflow = runtime_error_message([&] {
    (void)larch::lazy_chart_detail::
        estimate_outside_context_key_grouping_logical_resident_bytes(
            (std::numeric_limits<std::size_t>::max)(), 2, 1);
  });
  CHECK(overflow.find("overflow") != std::string::npos);

  {
    larch::lazy_chart_detail::outside_context_key_workspace empty_workspace;
    auto const empty =
        larch::lazy_chart_detail::collect_packed_outside_context_keys(
            0, 3, empty_workspace,
            [&](std::span<packed_key_word> words) { CHECK(words.empty()); });
    CHECK(empty.classes().class_count() == 0);
    CHECK(empty.classes().class_by_input.empty());
    CHECK(empty.classes().lexicographic_class_order.empty());
    CHECK(empty.memory.actual_capacity_resident_bytes >=
          empty.memory.logical_resident_bytes);
  }

  {
    auto const grammar = make_nonlex_binary_dag_grammar();
    auto const plan = larch::build_chart_execution_plan(grammar);
    auto const patterns = make_nonlex_four_taxon_patterns();
    auto chart = larch::build_lazy_inside_chart(grammar, patterns);
    chart = larch::build_lazy_outside_chart(grammar, patterns,
                                            std::move(chart));
    CHECK(chart.lazy_outside_rows_computed == 28);
    CHECK(chart.outside_multifurcation_productions_scored == 0);
    CHECK(chart.outside_recurrence_work.binary_stack_productions_scored == 68);
    CHECK(chart.outside_recurrence_work.generic_reusable_productions_scored ==
          0);
    auto const pid = grammar.productions_by_parent[grammar.root_clade].front();
    check_packed_outside_context_against_ordered_map(
        grammar, plan, patterns, chart, pid, 2,
        std::vector<std::size_t>{0, 1, 2, 3, 3, 4},
        std::vector<std::size_t>{0, 1, 2, 3, 5},
        std::vector<std::size_t>{0, 2, 1, 3, 4}, true);
  }

  {
    auto const grammar = make_nonlex_multifurcating_grammar();
    auto const plan = larch::build_chart_execution_plan(grammar);
    auto const patterns = make_nonlex_four_taxon_patterns();
    auto chart = larch::build_lazy_inside_chart(grammar, patterns);
    chart = larch::build_lazy_outside_chart(grammar, patterns,
                                            std::move(chart));
    CHECK(chart.lazy_outside_rows_computed == 25);
    CHECK(chart.outside_multifurcation_productions_scored == 18);
    CHECK(chart.outside_recurrence_work.binary_stack_productions_scored == 12);
    CHECK(chart.outside_recurrence_work.generic_reusable_productions_scored ==
          18);
    auto const pid = grammar.productions_by_parent[grammar.root_clade].front();
    check_packed_outside_context_against_ordered_map(
        grammar, plan, patterns, chart, pid, 3,
        std::vector<std::size_t>{0, 1, 2, 3, 4, 5},
        std::vector<std::size_t>{0, 1, 2, 3, 4, 5},
        std::vector<std::size_t>{0, 2, 4, 3, 1, 5}, false);
  }
}

static void test_packed_outside_context_preserves_exception_order() {
  std::println("test_packed_outside_context_preserves_exception_order");
  auto const grammar = make_trinary_grammar();
  auto const plan = larch::build_chart_execution_plan(grammar);
  larch::site_pattern_set patterns;
  patterns.taxon_count = 3;
  patterns.patterns = {
      larch::site_pattern{.state_by_taxon = {0, 0, 0}, .weight = 1},
      larch::site_pattern{.state_by_taxon = {0, 0, 0}, .weight = 2},
  };

  auto corrupt_and_reset = [&](larch::lazy_multisite_chart& chart) {
    auto& corrupted_map =
        *chart.class_index_by_pattern_by_clade[larch::clade_id{1}];
    CHECK(chart.inside_rows_by_clade[larch::clade_id{1}].size() == 1);
    CHECK((corrupted_map == std::vector<std::size_t>{0, 0}));
    corrupted_map[0] = 1;
    chart.outside_rows_by_clade[larch::clade_id{0}].clear();
    chart.outside_class_weight_by_clade[larch::clade_id{0}].clear();
    chart.outside_class_index_by_pattern_by_clade[larch::clade_id{0}] =
        std::nullopt;
    chart.outside_multifurcation_productions_scored = 0;
    chart.outside_recurrence_work = {};
  };

  auto grammar_chart = larch::build_lazy_inside_chart(grammar, patterns);
  grammar_chart = larch::build_lazy_outside_chart(
      grammar, patterns, std::move(grammar_chart));
  corrupt_and_reset(grammar_chart);
  larch::lazy_chart_detail::outside_context_key_workspace grammar_workspace;
  {
    auto const keys = larch::lazy_chart_detail::collect_outside_context_keys(
        grammar_chart, grammar, patterns, grammar.productions.front(),
        grammar_workspace);
    CHECK((keys.classes().class_by_input ==
           std::vector<std::size_t>{0, 1}));
    CHECK((keys.classes().lexicographic_class_order ==
           std::vector<std::size_t>{1, 0}));
  }
  auto const grammar_error = runtime_error_message([&] {
    larch::lazy_chart_detail::assign_outside_classes_for_clade(
        grammar_chart, grammar, patterns, larch::clade_id{0},
        grammar_workspace);
  });
  CHECK(grammar_error.find("child inside class index out of range") !=
        std::string::npos);
  CHECK(grammar_chart.outside_multifurcation_productions_scored == 2);
  CHECK(grammar_chart.outside_recurrence_work
            .generic_reusable_productions_scored == 2);
  CHECK(grammar_chart.outside_recurrence_work.binary_stack_productions_scored ==
        0);

  auto plan_chart = larch::build_lazy_inside_chart(plan, patterns);
  plan_chart =
      larch::build_lazy_outside_chart(plan, patterns, std::move(plan_chart));
  corrupt_and_reset(plan_chart);
  larch::lazy_chart_detail::outside_context_key_workspace plan_workspace;
  auto const plan_error = runtime_error_message([&] {
    larch::lazy_chart_detail::assign_outside_classes_for_clade(
        plan_chart, plan, patterns, larch::clade_id{0}, plan_workspace);
  });
  CHECK(plan_error == grammar_error);
  CHECK(plan_chart.outside_multifurcation_productions_scored == 2);
  CHECK(plan_chart.outside_recurrence_work
            .generic_reusable_productions_scored == 2);
  CHECK(plan_chart.outside_recurrence_work.binary_stack_productions_scored ==
        0);
}

static void test_checked_and_plan_exact_frontier_equivalence() {
  std::println("test_checked_and_plan_exact_frontier_equivalence");
  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  for (auto mode : {larch::multisite_dominance_mode::off,
                    larch::multisite_dominance_mode::score_only,
                    larch::multisite_dominance_mode::strict_mask_safe,
                    larch::multisite_dominance_mode::two_pass_exact_mask}) {
    for (bool bound_pruning : {false, true}) {
      larch::multisite_trim_options trim_options;
      trim_options.dominance_mode = mode;
      trim_options.use_bound_pruning = bound_pruning;
      trim_options.require_exact_keep_mask =
          mode != larch::multisite_dominance_mode::score_only;
      auto checked = larch::build_multisite_trim(
          grammar, patterns, larch::chart_options{}, trim_options);

      std::size_t full_validations = 0;
      std::size_t partition_validations = 0;
      std::size_t clade_sorts = 0;
      larch::parsimony_chart_detail::structural_work_observer observer{
          &full_validations, &partition_validations, &clade_sorts};
      larch::multisite_trim_result planned;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned = larch::build_multisite_trim(
            plan, patterns, larch::chart_options{}, trim_options);
      }
      check_multisite_trim_equal(checked, planned);
      CHECK(full_validations == 0);
      CHECK(partition_validations == 0);
      CHECK(clade_sorts == 0);
    }
  }

  larch::multisite_trim_options provenance_options;
  provenance_options.capture_optimal_root_provenance = true;
  auto checked = larch::build_multisite_trim(
      grammar, patterns, larch::chart_options{}, provenance_options);
  auto planned = larch::build_multisite_trim(
      plan, patterns, larch::chart_options{}, provenance_options);
  check_multisite_trim_equal(checked, planned);
  CHECK(!planned.optimal_root_provenance_classes.empty());

  auto trinary = make_trinary_grammar();
  auto trinary_plan = larch::build_chart_execution_plan(trinary);
  auto trinary_patterns = make_weighted_patterns(3);
  auto checked_error = runtime_error_message([&] {
    (void)larch::build_multisite_trim(trinary, trinary_patterns);
  });
  auto planned_error = runtime_error_message([&] {
    (void)larch::build_multisite_trim(trinary_plan, trinary_patterns);
  });
  CHECK(checked_error.find("WI6 arity gate") != std::string::npos);
  CHECK(planned_error.find("WI6 arity gate") != std::string::npos);
}

static void test_plan_lifetime_and_uninitialized_guards() {
  std::println("test_plan_lifetime_and_uninitialized_guards");
  larch::chart_execution_plan empty;
  CHECK(!empty.valid());
  auto uninitialized = runtime_error_message([&] {
    (void)larch::build_single_site_chart(empty, larch::leaf_site_states{},
                                         larch::chart_options{});
  });
  CHECK(uninitialized.find("uninitialized plan") != std::string::npos);

  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto grammar_copy = grammar;
  plan.assert_compatible(grammar_copy);
  auto copied_plan = plan;
  auto moved_plan = std::move(copied_plan);
  CHECK(moved_plan.valid());
  CHECK(!copied_plan.valid());
  moved_plan.assert_compatible(grammar_copy);

  grammar_copy.productions[0].children = {1, 0};
  auto diverged = runtime_error_message(
      [&] { moved_plan.assert_compatible(grammar_copy); });
  CHECK(diverged.find("fingerprint mismatch") != std::string::npos);

  auto unpublished = make_binary_grammar();
  unpublished.execution_generation = 0;
  auto unpublished_error = runtime_error_message(
      [&] { (void)larch::build_chart_execution_plan(unpublished); });
  CHECK(unpublished_error.find("no published execution generation") !=
        std::string::npos);
  CHECK(unpublished.execution_generation == 0);
}

static void test_invalid_input_fails_at_plan_construction() {
  std::println("test_invalid_input_fails_at_plan_construction");
  {
    auto grammar = make_trinary_grammar();
    grammar.productions[0].children = {0, 0, 2};
    grammar.productions_by_child = {{0}, {}, {0}, {}};
    auto message = runtime_error_message(
        [&] { (void)larch::build_chart_execution_plan(grammar); });
    CHECK(message.find("chart execution plan: production 0") !=
          std::string::npos);
    CHECK(message.find("not pairwise disjoint") != std::string::npos);
  }
  {
    auto grammar = make_trinary_grammar();
    grammar.productions[0].children = {0, 1};
    grammar.productions_by_child = {{0}, {0}, {}, {}};
    auto message = runtime_error_message(
        [&] { (void)larch::build_chart_execution_plan(grammar); });
    CHECK(message.find("chart execution plan: production 0") !=
          std::string::npos);
    CHECK(message.find("do not union to parent clade") != std::string::npos);
  }
  {
    auto grammar = make_binary_grammar();
    grammar.productions[0].parent = 5;
    auto message = runtime_error_message(
        [&] { (void)larch::build_chart_execution_plan(grammar); });
    CHECK(message.find("chart execution plan") != std::string::npos);
    CHECK(message.find("mismatched parent") != std::string::npos);
  }
}

int main() {
  test_execution_plan_binary_and_generic_invariants();
  test_checked_and_plan_dense_equivalence();
  test_checked_and_plan_composite_equivalence();
  test_checked_and_plan_lazy_inside_outside_equivalence();
  test_checked_and_plan_lazy_score_and_exact_trim_equivalence();
  test_checked_and_plan_lazy_multifurcation_equivalence();
  test_nonlex_packed_plan_lazy_grouping_equivalence();
  test_scheduled_plan_lazy_dependency_wavefronts();
  test_scheduler_memory_estimate_allocation_bounds();
  test_finite_scheduled_lazy_state_admission();
  test_finite_wide_lazy_prefix_admission();
  test_plan_packed_key_narrowing_and_accounting();
  test_grammar_packed_key_first_occurrence_reuse_and_accounting();
  test_packed_outside_context_order_reuse_and_accounting();
  test_packed_outside_context_preserves_exception_order();
  test_checked_and_plan_exact_frontier_equivalence();
  test_stale_and_mismatched_plan_rejected();
  test_plan_lifetime_and_uninitialized_guards();
  test_invalid_input_fails_at_plan_construction();
  std::println("All chart parallel tests passed!");
}

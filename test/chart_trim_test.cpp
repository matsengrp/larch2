#include <larch/chart_trim.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
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

static larch::test::tiny_tree_node paper_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("C", "AC"),
                   tiny_inner("DE", "AA",
                              {tiny_leaf("D", "CA"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node paper_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("D", "CA"),
                   tiny_inner("CE", "AA",
                              {tiny_leaf("C", "AC"), tiny_leaf("E", "CC")})})});
}

static larch::taxon_id taxon_for(larch::clade_grammar const& grammar,
                                 std::string const& sample_id) {
  auto it = grammar.taxa.sample_id_to_id.find(sample_id);
  CHECK(it != grammar.taxa.sample_id_to_id.end());
  return it->second;
}

static larch::clade_id clade_for(larch::clade_grammar const& grammar,
                                 std::vector<std::string> sample_ids) {
  std::vector<larch::taxon_id> ids;
  ids.reserve(sample_ids.size());
  for (auto const& sample_id : sample_ids)
    ids.push_back(taxon_for(grammar, sample_id));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa == ids)
      return static_cast<larch::clade_id>(cid);
  }
  CHECK(false && "missing clade");
  return larch::no_clade;
}

static larch::production_id production_id_for(
    larch::clade_grammar const& grammar, larch::clade_id parent,
    std::vector<larch::clade_id> children) {
  std::sort(children.begin(), children.end());
  for (auto pid : grammar.productions_by_parent[parent]) {
    auto prod_children = grammar.productions[pid].children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == children) return pid;
  }
  CHECK(false && "missing production");
  return larch::no_production;
}

static bool contains(std::vector<larch::production_id> const& ids,
                     larch::production_id needle) {
  return std::find(ids.begin(), ids.end(), needle) != ids.end();
}

static bool throws_runtime_error(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const&) {
    return true;
  }
  return false;
}

using row_t = std::array<larch::chart_cost, larch::nuc_state_count>;

static larch::chart_cost brute_add(larch::chart_cost lhs,
                                   larch::chart_cost rhs) {
  if (lhs >= larch::chart_inf || rhs >= larch::chart_inf)
    return larch::chart_inf;
  if (lhs > larch::chart_inf - rhs) return larch::chart_inf;
  return lhs + rhs;
}

static row_t brute_inf_row() {
  row_t row{};
  row.fill(larch::chart_inf);
  return row;
}

static row_t brute_leaf_row(std::uint8_t observed) {
  auto row = brute_inf_row();
  row[observed] = 0;
  return row;
}

static row_t brute_combine_binary(row_t const& left, row_t const& right) {
  auto row = brute_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
       ++parent_state) {
    larch::chart_cost best_left = larch::chart_inf;
    larch::chart_cost best_right = larch::chart_inf;
    for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
         ++child_state) {
      best_left = std::min(
          best_left,
          brute_add(left[child_state], parent_state == child_state ? 0 : 1));
      best_right = std::min(
          best_right,
          brute_add(right[child_state], parent_state == child_state ? 0 : 1));
    }
    row[parent_state] = brute_add(best_left, best_right);
  }
  return row;
}

struct brute_topology {
  std::vector<row_t> inside_by_clade;
  std::vector<larch::production_id> selected_prod_by_clade;
  std::vector<bool> used_production;
};

static std::vector<brute_topology> brute_enumerate_topologies(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states,
    larch::clade_id clade) {
  std::vector<brute_topology> result;
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    brute_topology topo;
    topo.inside_by_clade.assign(grammar.clades.size(), brute_inf_row());
    topo.selected_prod_by_clade.assign(grammar.clades.size(),
                                       larch::no_production);
    topo.used_production.assign(grammar.productions.size(), false);
    topo.inside_by_clade[clade] =
        brute_leaf_row(states.state_by_taxon[key.taxa.front()]);
    result.push_back(std::move(topo));
    return result;
  }

  for (auto pid : grammar.productions_by_parent[clade]) {
    auto const& prod = grammar.productions[pid];
    CHECK(prod.children.size() == 2);
    auto lefts = brute_enumerate_topologies(grammar, states, prod.children[0]);
    auto rights = brute_enumerate_topologies(grammar, states, prod.children[1]);
    for (auto const& left : lefts) {
      for (auto const& right : rights) {
        brute_topology topo;
        topo.inside_by_clade.assign(grammar.clades.size(), brute_inf_row());
        topo.selected_prod_by_clade.assign(grammar.clades.size(),
                                           larch::no_production);
        topo.used_production.assign(grammar.productions.size(), false);

        for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
          if (left.inside_by_clade[cid] != brute_inf_row())
            topo.inside_by_clade[cid] = left.inside_by_clade[cid];
          if (right.inside_by_clade[cid] != brute_inf_row())
            topo.inside_by_clade[cid] = right.inside_by_clade[cid];
          if (left.selected_prod_by_clade[cid] != larch::no_production)
            topo.selected_prod_by_clade[cid] = left.selected_prod_by_clade[cid];
          if (right.selected_prod_by_clade[cid] != larch::no_production)
            topo.selected_prod_by_clade[cid] = right.selected_prod_by_clade[cid];
        }
        for (std::size_t i = 0; i < grammar.productions.size(); ++i) {
          topo.used_production[i] = left.used_production[i] ||
                                    right.used_production[i];
        }

        topo.selected_prod_by_clade[clade] = pid;
        topo.used_production[pid] = true;
        topo.inside_by_clade[clade] = brute_combine_binary(
            topo.inside_by_clade[prod.children[0]],
            topo.inside_by_clade[prod.children[1]]);
        result.push_back(std::move(topo));
        CHECK(result.size() < 10000);
      }
    }
  }
  return result;
}

static larch::chart_cost row_min(row_t const& row) {
  larch::chart_cost best = larch::chart_inf;
  for (auto cost : row) best = std::min(best, cost);
  return best;
}

static std::vector<larch::clade_id> clades_by_decreasing_size(
    larch::clade_grammar const& grammar) {
  std::vector<larch::clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), larch::clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](auto lhs, auto rhs) {
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize > rsize;
    return lhs < rhs;
  });
  return order;
}

static std::vector<row_t> brute_topology_outside(
    larch::clade_grammar const& grammar, brute_topology const& topo) {
  std::vector<row_t> outside(grammar.clades.size(), brute_inf_row());
  outside[grammar.root_clade].fill(0);

  for (auto parent : clades_by_decreasing_size(grammar)) {
    auto pid = topo.selected_prod_by_clade[parent];
    if (pid == larch::no_production) continue;
    auto const& prod = grammar.productions[pid];
    CHECK(prod.children.size() == 2);
    std::array<larch::clade_id, 2> children{prod.children[0], prod.children[1]};

    for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
         ++parent_state) {
      auto base = outside[parent][parent_state];
      if (base >= larch::chart_inf) continue;
      for (std::size_t child_i = 0; child_i < 2; ++child_i) {
        auto child = children[child_i];
        auto sibling = children[1 - child_i];
        larch::chart_cost sibling_best = larch::chart_inf;
        for (std::uint8_t sibling_state = 0;
             sibling_state < larch::nuc_state_count; ++sibling_state) {
          sibling_best = std::min(
              sibling_best,
              brute_add(topo.inside_by_clade[sibling][sibling_state],
                        parent_state == sibling_state ? 0 : 1));
        }
        for (std::uint8_t child_state = 0;
             child_state < larch::nuc_state_count; ++child_state) {
          auto candidate = brute_add(
              brute_add(base, sibling_best),
              parent_state == child_state ? larch::chart_cost{0}
                                          : larch::chart_cost{1});
          outside[child][child_state] =
              std::min(outside[child][child_state], candidate);
        }
      }
    }
  }

  return outside;
}

static void compare_outside_and_mask_to_bruteforce(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states,
    larch::single_site_chart const& chart,
    larch::single_site_outside_chart const& outside,
    larch::chart_trim_mask const& mask) {
  auto topologies = brute_enumerate_topologies(grammar, states,
                                               grammar.root_clade);
  CHECK(!topologies.empty());

  larch::chart_cost optimum = larch::chart_inf;
  for (auto const& topo : topologies)
    optimum = std::min(optimum, row_min(topo.inside_by_clade[grammar.root_clade]));
  CHECK(optimum == outside.global_min);
  CHECK(optimum == chart.root_min_excluding_ua(grammar.root_clade));

  std::vector<row_t> brute_outside(grammar.clades.size(), brute_inf_row());
  std::vector<bool> brute_keep_production(grammar.productions.size(), false);

  for (auto const& topo : topologies) {
    auto root_cost = row_min(topo.inside_by_clade[grammar.root_clade]);
    if (root_cost == optimum) {
      for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid)
        brute_keep_production[pid] =
            brute_keep_production[pid] || topo.used_production[pid];
    }

    auto topo_outside = brute_topology_outside(grammar, topo);
    for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
      if (topo.inside_by_clade[cid] == brute_inf_row()) continue;
      for (std::uint8_t state = 0; state < larch::nuc_state_count; ++state) {
        brute_outside[cid][state] =
            std::min(brute_outside[cid][state], topo_outside[cid][state]);
      }
    }
  }

  CHECK(outside.outside == brute_outside);
  CHECK(mask.keep_production == brute_keep_production);
}

static void test_paper_counterexample_outside_trim_and_traceback() {
  std::println("test_paper_counterexample_outside_trim_and_traceback");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);

  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto de = clade_for(grammar, {"D", "E"});
  auto ce = clade_for(grammar, {"C", "E"});
  auto cde = clade_for(grammar, {"C", "D", "E"});
  auto prod_c_de = production_id_for(grammar, cde, {c, de});
  auto prod_d_ce = production_id_for(grammar, cde, {d, ce});

  auto states1 = larch::extract_leaf_site_states(merged, grammar, 1);
  larch::chart_options trace_options;
  trace_options.keep_trace = true;
  auto chart1 = larch::build_single_site_chart(grammar, states1, trace_options);
  auto outside1 = larch::build_single_site_outside_chart(grammar, chart1);
  auto mask1 = larch::build_single_site_trim_mask(grammar, chart1, outside1);

  CHECK(outside1.global_min == 1);
  CHECK(mask1.global_min == 1);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::A].size() == 1);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::A].front().production ==
        prod_c_de);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::C].size() == 2);
  CHECK(mask1.keep_clade_state[cde][larch::nuc_base::A]);
  CHECK(!mask1.keep_clade_state[cde][larch::nuc_base::C]);
  CHECK(mask1.keep_production[prod_c_de]);
  CHECK(!mask1.keep_production[prod_d_ce]);
  CHECK(!mask1.optimal_choices_by_production[prod_c_de].empty());

  larch::chart_trim_options bool_only_trim;
  bool_only_trim.store_optimal_choices = false;
  auto bool_only_mask1 =
      larch::build_single_site_trim_mask(grammar, chart1, outside1,
                                         bool_only_trim);
  CHECK(bool_only_mask1.keep_production == mask1.keep_production);
  CHECK(bool_only_mask1.optimal_choices_by_production.empty());
  CHECK(bool_only_mask1.kept_production_choice_count ==
        mask1.kept_production_choice_count);

  CHECK(mask1.kept_production_choice_count > 1);
  larch::chart_trim_options capped_trim;
  capped_trim.max_stored_optimal_choices = 1;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_single_site_trim_mask(grammar, chart1, outside1,
                                             capped_trim);
  }));

  auto traceback1 = larch::deterministic_optimal_single_site_traceback(
      grammar, chart1, outside1);
  CHECK(traceback1.score == 1);
  CHECK(contains(traceback1.productions, prod_c_de));
  CHECK(!contains(traceback1.productions, prod_d_ce));
  CHECK(traceback1.root_state_by_clade[cde] == larch::nuc_base::A);

  compare_outside_and_mask_to_bruteforce(grammar, states1, chart1, outside1,
                                         mask1);

  auto states2 = larch::extract_leaf_site_states(merged, grammar, 2);
  auto chart2 = larch::build_single_site_chart(grammar, states2, trace_options);
  auto outside2 = larch::build_single_site_outside_chart(grammar, chart2);
  auto mask2 = larch::build_single_site_trim_mask(grammar, chart2, outside2);

  CHECK(outside2.global_min == 1);
  CHECK(mask2.keep_production[prod_d_ce]);
  CHECK(!mask2.keep_production[prod_c_de]);
  auto traceback2 = larch::deterministic_optimal_single_site_traceback(
      grammar, chart2, outside2);
  CHECK(contains(traceback2.productions, prod_d_ce));
  CHECK(!contains(traceback2.productions, prod_c_de));

  compare_outside_and_mask_to_bruteforce(grammar, states2, chart2, outside2,
                                         mask2);

  std::println("  PASS");
}

static void test_single_tree_keeps_all_productions() {
  std::println("test_single_tree_keeps_all_productions");

  auto spec = larch::test::tiny_inner(
      "root", "A",
      {larch::test::tiny_inner("AB", "A",
                                {larch::test::tiny_leaf("A", "A"),
                                 larch::test::tiny_leaf("B", "C")}),
       larch::test::tiny_inner("CD", "A",
                                {larch::test::tiny_leaf("C", "G"),
                                 larch::test::tiny_leaf("D", "T")})});
  auto tree = larch::test::make_tiny_labelled_tree("A", spec);
  auto grammar = larch::build_clade_grammar(tree);
  auto states = larch::extract_leaf_site_states(tree, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);
  auto outside = larch::build_single_site_outside_chart(grammar, chart);
  auto mask = larch::build_single_site_trim_mask(grammar, chart, outside);

  CHECK(grammar.productions.size() == 3);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid)
    CHECK(mask.keep_production[pid]);

  auto traceback = larch::deterministic_optimal_single_site_traceback(
      grammar, chart, outside);
  CHECK(traceback.score == chart.root_min_excluding_ua(grammar.root_clade));
  CHECK(traceback.productions.size() == grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid)
    CHECK(contains(traceback.productions, static_cast<larch::production_id>(pid)));

  compare_outside_and_mask_to_bruteforce(grammar, states, chart, outside, mask);

  std::println("  PASS");
}

static void test_reference_edge_outside_boundary() {
  std::println("test_reference_edge_outside_boundary");

  auto spec = larch::test::tiny_inner(
      "root", "A", {larch::test::tiny_leaf("A", "C"),
                     larch::test::tiny_leaf("B", "C")});
  auto tree = larch::test::make_tiny_labelled_tree("A", spec);
  auto grammar = larch::build_clade_grammar(tree);
  auto states = larch::extract_leaf_site_states(tree, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);

  larch::chart_options opts;
  opts.score_ua_edge = true;
  auto outside = larch::build_single_site_outside_chart(
      grammar, chart, opts, larch::extract_reference_site_state(tree, 1));
  auto mask = larch::build_single_site_trim_mask(grammar, chart, outside);

  CHECK(outside.outside[grammar.root_clade][larch::nuc_base::A] == 0);
  CHECK(outside.outside[grammar.root_clade][larch::nuc_base::C] == 1);
  CHECK(outside.global_min == 1);
  CHECK(mask.keep_clade_state[grammar.root_clade][larch::nuc_base::C]);
  CHECK(!mask.keep_clade_state[grammar.root_clade][larch::nuc_base::A]);

  auto traceback = larch::deterministic_optimal_single_site_traceback(
      grammar, chart, outside);
  CHECK(traceback.score == 1);
  CHECK(traceback.root_state_by_clade[grammar.root_clade] == larch::nuc_base::C);

  larch::chart_options ua_free_opts;
  CHECK(!throws_runtime_error([&] {
    (void)larch::build_single_site_outside_chart(grammar, chart, ua_free_opts,
                                                 std::uint8_t{99});
  }));
  CHECK(!throws_runtime_error([&] {
    (void)larch::build_single_site_outside_chart(grammar, chart, ua_free_opts,
                                                 tree, 2);
  }));

  auto impossible_chart = chart;
  impossible_chart.inside[grammar.root_clade].fill(larch::chart_inf);
  auto impossible_outside =
      larch::build_single_site_outside_chart(grammar, impossible_chart);
  CHECK(impossible_outside.global_min == larch::chart_inf);
  auto impossible_mask = larch::build_single_site_trim_mask(
      grammar, impossible_chart, impossible_outside);
  for (auto const& state_mask : impossible_mask.keep_clade_state)
    for (bool keep : state_mask) CHECK(!keep);
  for (bool keep : impossible_mask.keep_production) CHECK(!keep);
  CHECK(throws_runtime_error([&] {
    (void)larch::deterministic_optimal_single_site_traceback(
        grammar, impossible_chart, impossible_outside);
  }));

  std::println("  PASS");
}

static void test_exhaustive_binary_assignments() {
  std::println("test_exhaustive_binary_assignments");

  for (std::uint32_t mask_bits = 0; mask_bits < 32; ++mask_bits) {
    std::array<char, 5> states{};
    for (std::size_t i = 0; i < states.size(); ++i)
      states[i] = (mask_bits & (1u << i)) ? 'C' : 'A';

    auto s = [](char c) { return std::string(1, c); };
    auto tree1 = larch::test::tiny_inner(
        "root", "A",
        {larch::test::tiny_inner(
             "AB", "A",
             {larch::test::tiny_leaf("A", s(states[0])),
              larch::test::tiny_leaf("B", s(states[1]))}),
         larch::test::tiny_inner(
             "CDE", "A",
             {larch::test::tiny_leaf("C", s(states[2])),
              larch::test::tiny_inner(
                  "DE", "A", {larch::test::tiny_leaf("D", s(states[3])),
                                larch::test::tiny_leaf("E", s(states[4]))})})});
    auto tree2 = larch::test::tiny_inner(
        "root", "A",
        {larch::test::tiny_inner(
             "AB", "A",
             {larch::test::tiny_leaf("A", s(states[0])),
              larch::test::tiny_leaf("B", s(states[1]))}),
         larch::test::tiny_inner(
             "CDE", "A",
             {larch::test::tiny_leaf("D", s(states[3])),
              larch::test::tiny_inner(
                  "CE", "A", {larch::test::tiny_leaf("C", s(states[2])),
                                larch::test::tiny_leaf("E", s(states[4]))})})});

    std::vector<larch::phylo_dag> trees;
    trees.push_back(larch::test::make_tiny_labelled_tree("A", tree1));
    trees.push_back(larch::test::make_tiny_labelled_tree("A", tree2));
    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(merged);
    auto leaf_states = larch::extract_leaf_site_states(merged, grammar, 1);
    auto chart = larch::build_single_site_chart(grammar, leaf_states);
    auto outside = larch::build_single_site_outside_chart(grammar, chart);
    auto trim = larch::build_single_site_trim_mask(grammar, chart, outside);
    compare_outside_and_mask_to_bruteforce(grammar, leaf_states, chart, outside,
                                           trim);
  }

  std::println("  PASS");
}

int main() {
  test_paper_counterexample_outside_trim_and_traceback();
  test_single_tree_keeps_all_productions();
  test_reference_edge_outside_boundary();
  test_exhaustive_binary_assignments();

  std::println("All chart trim tests passed!");
  return 0;
}

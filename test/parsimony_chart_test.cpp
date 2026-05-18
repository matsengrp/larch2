#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
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

static larch::test::tiny_tree_node cde_tree1_one_site(
    std::array<char, 5> const& states) {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto s = [](char c) { return std::string(1, c); };
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A",
                  {tiny_leaf("A", s(states[0])), tiny_leaf("B", s(states[1]))}),
       tiny_inner("CDE", "A",
                  {tiny_leaf("C", s(states[2])),
                   tiny_inner("DE", "A",
                              {tiny_leaf("D", s(states[3])),
                               tiny_leaf("E", s(states[4]))})})});
}

static larch::test::tiny_tree_node cde_tree2_one_site(
    std::array<char, 5> const& states) {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto s = [](char c) { return std::string(1, c); };
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A",
                  {tiny_leaf("A", s(states[0])), tiny_leaf("B", s(states[1]))}),
       tiny_inner("CDE", "A",
                  {tiny_leaf("D", s(states[3])),
                   tiny_inner("CE", "A",
                              {tiny_leaf("C", s(states[2])),
                               tiny_leaf("E", s(states[4]))})})});
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

static bool throws_runtime_error(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const&) {
    return true;
  }
  return false;
}

static larch::test::tiny_tree_node random_binary_tree(
    std::vector<std::pair<std::string, char>> leaves, std::mt19937& rng,
    int& inner_id) {
  if (leaves.size() == 1)
    return larch::test::tiny_leaf(leaves.front().first,
                                  std::string(1, leaves.front().second));

  std::shuffle(leaves.begin(), leaves.end(), rng);
  std::uniform_int_distribution<int> split_dist(
      1, static_cast<int>(leaves.size()) - 1);
  auto split = static_cast<std::size_t>(split_dist(rng));
  std::vector<std::pair<std::string, char>> left(leaves.begin(),
                                                 leaves.begin() + split);
  std::vector<std::pair<std::string, char>> right(leaves.begin() + split,
                                                  leaves.end());

  auto name = "N" + std::to_string(inner_id++);
  return larch::test::tiny_inner(
      name, "A",
      {random_binary_tree(std::move(left), rng, inner_id),
       random_binary_tree(std::move(right), rng, inner_id)});
}

using brute_chart_row = std::array<larch::chart_cost, larch::nuc_state_count>;

static larch::chart_cost brute_add(larch::chart_cost lhs,
                                   larch::chart_cost rhs) {
  if (lhs >= larch::chart_inf || rhs >= larch::chart_inf)
    return larch::chart_inf;
  if (lhs > larch::chart_inf - rhs) return larch::chart_inf;
  return lhs + rhs;
}

static brute_chart_row brute_leaf_row(std::uint8_t observed) {
  brute_chart_row row{};
  row.fill(larch::chart_inf);
  row[observed] = 0;
  return row;
}

static brute_chart_row brute_combine_binary(brute_chart_row const& left,
                                            brute_chart_row const& right) {
  brute_chart_row row{};
  row.fill(larch::chart_inf);
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

static std::vector<brute_chart_row> brute_enumerate_rows(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states,
    larch::clade_id clade,
    std::vector<std::optional<std::vector<brute_chart_row>>>& memo) {
  if (memo[clade].has_value()) return *memo[clade];

  std::vector<brute_chart_row> rows;
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    rows.push_back(brute_leaf_row(states.state_by_taxon[key.taxa.front()]));
  } else {
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      CHECK(prod.children.size() == 2);
      auto left_rows =
          brute_enumerate_rows(grammar, states, prod.children[0], memo);
      auto right_rows =
          brute_enumerate_rows(grammar, states, prod.children[1], memo);
      for (auto const& left : left_rows) {
        for (auto const& right : right_rows) {
          rows.push_back(brute_combine_binary(left, right));
          CHECK(rows.size() < 10000);
        }
      }
    }
  }

  memo[clade] = rows;
  return rows;
}

static larch::chart_cost brute_force_grammar_root_min(
    larch::clade_grammar const& grammar,
    larch::leaf_site_states const& states) {
  std::vector<std::optional<std::vector<brute_chart_row>>> memo(
      grammar.clades.size());
  auto rows = brute_enumerate_rows(grammar, states, grammar.root_clade, memo);
  larch::chart_cost best = larch::chart_inf;
  for (auto const& row : rows)
    for (auto cost : row) best = std::min(best, cost);
  return best;
}

static void test_paper_counterexample_single_sites() {
  std::println("test_paper_counterexample_single_sites");

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
  auto chart1 = larch::build_single_site_chart(grammar, states1, true);
  CHECK(chart1.has_trace());
  CHECK(chart1.inside[cde][larch::nuc_base::A] == 1);
  CHECK(chart1.inside[cde][larch::nuc_base::C] == 1);
  CHECK(chart1.root_min_excluding_ua(grammar.root_clade) == 1);
  CHECK(chart1.root_min_with_reference_edge(
            grammar.root_clade,
            larch::extract_reference_site_state(merged, 1)) == 1);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::A].size() == 1);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::A].front().production ==
        prod_c_de);

  auto states2 = larch::extract_leaf_site_states(merged, grammar, 2);
  auto chart2 = larch::build_single_site_chart(grammar, states2, true);
  CHECK(chart2.inside[cde][larch::nuc_base::A] == 1);
  CHECK(chart2.inside[cde][larch::nuc_base::C] == 1);
  CHECK(chart2.root_min_excluding_ua(grammar.root_clade) == 1);
  CHECK(chart2.optimal_choices[cde][larch::nuc_base::A].size() == 1);
  CHECK(chart2.optimal_choices[cde][larch::nuc_base::A].front().production ==
        prod_d_ce);

  std::println("  PASS");
}

static void test_leaf_clade_and_reference_edge_conventions() {
  std::println("test_leaf_clade_and_reference_edge_conventions");

  auto leaf_tree = larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_leaf("solo", "C"));
  auto leaf_grammar = larch::build_clade_grammar(leaf_tree);
  auto leaf_states =
      larch::extract_leaf_site_states(leaf_tree, leaf_grammar, 1);
  auto leaf_chart = larch::build_single_site_chart(leaf_grammar, leaf_states);

  CHECK(leaf_chart.optimal_choices.empty());
  CHECK(leaf_grammar.productions.empty());
  CHECK(leaf_chart.inside[leaf_grammar.root_clade][larch::nuc_base::A] ==
        larch::chart_inf);
  CHECK(leaf_chart.inside[leaf_grammar.root_clade][larch::nuc_base::C] == 0);
  CHECK(leaf_chart.root_min_excluding_ua(leaf_grammar.root_clade) == 0);
  CHECK(leaf_chart.root_min_with_reference_edge(
            leaf_grammar.root_clade,
            larch::extract_reference_site_state(leaf_tree, 1)) == 1);

  auto all_c_spec = larch::test::tiny_inner(
      "root", "A",
      {larch::test::tiny_leaf("L1", "C"), larch::test::tiny_leaf("L2", "C")});
  auto chart_tree = larch::test::make_tiny_labelled_tree("A", all_c_spec);
  auto chart_grammar = larch::build_clade_grammar(chart_tree);
  auto states = larch::extract_leaf_site_states(chart_tree, chart_grammar, 1);
  auto chart = larch::build_single_site_chart(chart_grammar, states);
  CHECK(chart.root_min_excluding_ua(chart_grammar.root_clade) == 0);
  CHECK(chart.root_min_with_reference_edge(
            chart_grammar.root_clade,
            larch::extract_reference_site_state(chart_tree, 1)) == 1);

  larch::chart_options ua_free_opts;
  CHECK(larch::root_min(chart, chart_grammar.root_clade, ua_free_opts) == 0);
  larch::chart_options score_ua_opts;
  score_ua_opts.score_ua_edge = true;
  CHECK(throws_runtime_error([&] {
    (void)larch::root_min(chart, chart_grammar.root_clade, score_ua_opts);
  }));
  CHECK(larch::root_min(chart, chart_grammar.root_clade, score_ua_opts,
                        larch::extract_reference_site_state(chart_tree, 1)) ==
        1);
  CHECK(larch::root_min(chart, chart_grammar.root_clade, score_ua_opts,
                        chart_tree, 1) == 1);

  auto fitch_tree = larch::test::make_tiny_labelled_tree("A", all_c_spec);
  CHECK(larch::test::score_tree_fitch_parsimony(fitch_tree, false) == 0);
  auto fitch_tree_with_ua =
      larch::test::make_tiny_labelled_tree("A", all_c_spec);
  CHECK(larch::test::score_tree_fitch_parsimony(fitch_tree_with_ua, true) == 1);

  std::println("  PASS");
}

static void test_exhaustive_two_tree_single_site_equivalence() {
  std::println("test_exhaustive_two_tree_single_site_equivalence");

  for (std::uint32_t mask = 0; mask < 32; ++mask) {
    std::array<char, 5> states{};
    for (std::size_t i = 0; i < states.size(); ++i)
      states[i] = (mask & (1u << i)) ? 'C' : 'A';

    auto tree1_spec = cde_tree1_one_site(states);
    auto tree2_spec = cde_tree2_one_site(states);

    std::vector<larch::phylo_dag> trees;
    trees.push_back(larch::test::make_tiny_labelled_tree("A", tree1_spec));
    trees.push_back(larch::test::make_tiny_labelled_tree("A", tree2_spec));
    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(merged);
    auto leaf_states = larch::extract_leaf_site_states(merged, grammar, 1);
    auto chart = larch::build_single_site_chart(grammar, leaf_states);

    auto tree1 = larch::test::make_tiny_labelled_tree("A", tree1_spec);
    auto tree2 = larch::test::make_tiny_labelled_tree("A", tree2_spec);
    auto best_ua_free =
        std::min(larch::test::score_tree_fitch_parsimony(tree1, false),
                 larch::test::score_tree_fitch_parsimony(tree2, false));
    CHECK(chart.root_min_excluding_ua(grammar.root_clade) == best_ua_free);

    auto tree1_with_ua = larch::test::make_tiny_labelled_tree("A", tree1_spec);
    auto tree2_with_ua = larch::test::make_tiny_labelled_tree("A", tree2_spec);
    auto best_with_ua =
        std::min(larch::test::score_tree_fitch_parsimony(tree1_with_ua, true),
                 larch::test::score_tree_fitch_parsimony(tree2_with_ua, true));
    CHECK(chart.root_min_with_reference_edge(
              grammar.root_clade,
              larch::extract_reference_site_state(merged, 1)) == best_with_ua);
  }

  std::println("  PASS");
}

static void test_randomized_tiny_dag_regression() {
  std::println("test_randomized_tiny_dag_regression");

  std::mt19937 rng{0x5eed1234u};
  constexpr std::array<char, 4> alphabet{'A', 'C', 'G', 'T'};

  for (int iter = 0; iter < 40; ++iter) {
    int taxon_count = 3 + (iter % 3);
    std::vector<std::pair<std::string, char>> leaves;
    leaves.reserve(static_cast<std::size_t>(taxon_count));
    for (int i = 0; i < taxon_count; ++i) {
      std::uniform_int_distribution<int> base_dist(0, 3);
      leaves.emplace_back("T" + std::to_string(i), alphabet[base_dist(rng)]);
    }

    int input_tree_count = 2 + (iter % 3);
    std::vector<larch::phylo_dag> trees;
    trees.reserve(static_cast<std::size_t>(input_tree_count));
    for (int t = 0; t < input_tree_count; ++t) {
      int inner_id = 0;
      auto spec = random_binary_tree(leaves, rng, inner_id);
      trees.push_back(larch::test::make_tiny_labelled_tree("A", spec));
    }

    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(merged);
    auto states = larch::extract_leaf_site_states(merged, grammar, 1);
    auto chart = larch::build_single_site_chart(grammar, states);
    auto brute = brute_force_grammar_root_min(grammar, states);
    CHECK(chart.root_min_excluding_ua(grammar.root_clade) == brute);
  }

  std::println("  PASS");
}

static void test_strict_validation_errors() {
  std::println("test_strict_validation_errors");

  CHECK(throws_runtime_error([] {
    auto bad = larch::test::make_tiny_labelled_tree(
        "N", larch::test::tiny_leaf("bad", "N"));
    auto grammar = larch::build_clade_grammar(bad);
    (void)larch::extract_leaf_site_states(bad, grammar, 1);
  }));

  CHECK(throws_runtime_error([] {
    auto tree = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_leaf("solo", "A"));
    auto grammar = larch::build_clade_grammar(tree);
    larch::leaf_site_states states;
    states.state_by_taxon = {4};
    (void)larch::build_single_site_chart(grammar, states);
  }));

  CHECK(throws_runtime_error([] {
    auto tree = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_leaf("solo", "A"));
    auto grammar = larch::build_clade_grammar(tree);
    (void)larch::extract_leaf_site_states(tree, grammar, 2);
  }));

  CHECK(throws_runtime_error([] {
    auto bad = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_leaf("bad", "A"));
    for (auto nv : bad.get_all_nodes()) {
      std::visit(
          [](auto node) {
            if constexpr (requires {
                            node.sample_id();
                            node.cg();
                          }) {
              if (node.sample_id() == "bad") {
                node.cg() = larch::compact_genome{
                    std::map<larch::mutation_position, larch::nuc_base>{
                        {1, larch::nuc_base{7}}}};
              }
            }
          },
          nv);
    }
    auto grammar = larch::build_clade_grammar(bad);
    (void)larch::extract_leaf_site_states(bad, grammar, 1);
  }));

  CHECK(throws_runtime_error([] {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_inner("root", "A",
                                     {larch::test::tiny_leaf("A", "A"),
                                      larch::test::tiny_leaf("B", "C"),
                                      larch::test::tiny_leaf("C", "A")}));
    larch::clade_grammar_options opts;
    opts.allow_polytomies = true;
    auto grammar = larch::build_clade_grammar(dag, opts);
    auto states = larch::extract_leaf_site_states(dag, grammar, 1);
    (void)larch::build_single_site_chart(grammar, states);
  }));

  CHECK(throws_runtime_error([] {
    larch::clade_grammar grammar;
    grammar.taxa.id_to_sample_id = {"A", "B"};
    grammar.taxa.sample_id_to_id = {{"A", larch::taxon_id{0}},
                                    {"B", larch::taxon_id{1}}};
    grammar.clades = {{{larch::taxon_id{0}}},
                      {{larch::taxon_id{1}}},
                      {{larch::taxon_id{0}, larch::taxon_id{1}}}};
    grammar.root_clade = larch::clade_id{2};
    grammar.productions.push_back(larch::grammar_production{
        larch::clade_id{2}, {larch::clade_id{0}, larch::clade_id{0}}, {}, 1});
    grammar.productions_by_parent = {{}, {}, {larch::production_id{0}}};
    grammar.productions_by_child = {{larch::production_id{0}}, {}, {}};
    larch::leaf_site_states states{{larch::nuc_base::A, larch::nuc_base::C}};
    (void)larch::build_single_site_chart(grammar, states);
  }));

  std::println("  PASS");
}

int main() {
  test_paper_counterexample_single_sites();
  test_leaf_clade_and_reference_edge_conventions();
  test_exhaustive_two_tree_single_site_equivalence();
  test_randomized_tiny_dag_regression();
  test_strict_validation_errors();

  std::println("All parsimony chart tests passed!");
  return 0;
}

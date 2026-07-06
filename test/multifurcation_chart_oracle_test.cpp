#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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

using brute_row = std::array<larch::chart_cost, larch::nuc_state_count>;

static std::string runtime_error_message(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const& e) {
    return e.what();
  }
  return {};
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

static std::size_t max_production_arity(larch::clade_grammar const& grammar) {
  std::size_t max_arity = 0;
  for (auto const& prod : grammar.productions)
    max_arity = std::max(max_arity, prod.children.size());
  return max_arity;
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

  std::println("  PASS");
}

int main() {
  test_invalid_shared_partition_validator();
  test_trinary_fixture_and_allow_gate();
  test_four_ary_fixture();
  test_five_ary_fixture();
  test_mixed_arity_fixture();

  std::println("All multifurcation chart oracle tests passed!");
  return 0;
}

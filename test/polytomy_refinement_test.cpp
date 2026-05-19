#include <larch/parsimony_chart.hpp>
#include <larch/polytomy_refinement.hpp>

#include "test_util.hpp"

#include <print>
#include <stdexcept>
#include <string>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file, int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr) \
  do { \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

static bool throws_runtime_error(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const&) {
    return true;
  }
  return false;
}

static larch::phylo_dag make_three_taxon_star() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA", tiny_inner("root", "AAAA",
                          {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA"),
                           tiny_leaf("C", "AAGA")}));
}

static larch::phylo_dag make_three_taxon_binary_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA", tiny_inner("root", "AAAA",
                          {tiny_leaf("A", "AAAA"),
                           tiny_inner("BC", "AAAA",
                                      {tiny_leaf("B", "CAAA"),
                                       tiny_leaf("C", "AAGA")})}));
}

static void test_default_reject_mode_rejects_kary() {
  std::println("test_default_reject_mode_rejects_kary");

  auto dag = make_three_taxon_star();
  CHECK(throws_runtime_error([&] {
    (void)larch::build_polytomy_refined_clade_grammar(dag);
  }));

  std::println("  PASS");
}

static void test_reject_mode_accepts_binary_grammar() {
  std::println("test_reject_mode_accepts_binary_grammar");

  auto dag = make_three_taxon_binary_tree();
  auto result = larch::build_polytomy_refined_clade_grammar(dag);
  auto const& grammar = result.grammar;

  CHECK(!larch::grammar_has_kary_productions(grammar));
  CHECK(larch::kary_productions(grammar).empty());
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.source_kary_production_count == 0);
  CHECK(result.source_grammar_audit.non_binary_production_count == 0);
  CHECK(!result.audit.contains_kary_productions);
  CHECK(result.audit.binary_chart_compatible);
  CHECK(result.audit.exact_for_soft_polytomies);
  CHECK(result.audit.events.empty());
  CHECK(result.clade_info.size() == grammar.clades.size());
  CHECK(result.production_info.size() == grammar.productions.size());
  for (std::size_t cid = 0; cid < result.source_clade_to_refined.size(); ++cid)
    CHECK(result.source_clade_to_refined[cid] == cid);
  for (std::size_t pid = 0; pid < result.source_production_to_refined.size();
       ++pid) {
    CHECK(result.source_production_to_refined[pid] == pid);
    CHECK(result.production_info[pid].origin ==
          larch::refined_production_origin::observed_binary);
    CHECK(result.production_info[pid].source_productions.size() == 1);
    CHECK(result.production_info[pid].source_productions.front() == pid);
  }

  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);
  CHECK(chart.inside.size() == grammar.clades.size());

  std::println("  PASS");
}

static void test_audit_kary_returns_diagnostic_grammar() {
  std::println("test_audit_kary_returns_diagnostic_grammar");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::audit_kary;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(grammar.productions.size() == 1);
  CHECK(grammar.productions.front().children.size() == 3);
  CHECK(larch::grammar_has_kary_productions(grammar));
  auto kary = larch::kary_productions(grammar);
  CHECK(kary.size() == 1);
  CHECK(kary.front() == 0);
  CHECK(!larch::grammar_is_binary_chart_compatible(grammar));

  CHECK(result.audit.source_clade_count == grammar.clades.size());
  CHECK(result.source_grammar_audit.non_binary_production_count == 1);
  CHECK(result.audit.source_production_count == grammar.productions.size());
  CHECK(result.audit.source_kary_production_count == 1);
  CHECK(result.audit.refined_clade_count == grammar.clades.size());
  CHECK(result.audit.refined_production_count == grammar.productions.size());
  CHECK(result.audit.synthetic_clade_count == 0);
  CHECK(result.audit.synthetic_production_count == 0);
  CHECK(result.audit.contains_kary_productions);
  CHECK(!result.audit.binary_chart_compatible);
  CHECK(!result.audit.exact_for_soft_polytomies);
  CHECK(!result.audit.any_truncated);
  CHECK(!result.audit.any_refused);

  CHECK(result.audit.events.size() == 1);
  auto const& event = result.audit.events.front();
  CHECK(event.source_production == 0);
  CHECK(event.parent == grammar.root_clade);
  CHECK(event.arity == 3);
  CHECK(event.source_multiplicity == grammar.productions.front().multiplicity);
  CHECK(!event.expanded);
  CHECK(!event.exact);

  CHECK(result.production_info.size() == 1);
  auto const& info = result.production_info.front();
  CHECK(info.origin == larch::refined_production_origin::observed_kary_unexpanded);
  CHECK(info.source_productions.size() == 1);
  CHECK(info.source_productions.front() == 0);
  CHECK(!info.source_parent_nodes.empty());
  CHECK(!info.exact_refinement_component);

  std::println("  PASS");
}

static void test_binary_chart_rejects_audit_kary_grammar() {
  std::println("test_binary_chart_rejects_audit_kary_grammar");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::audit_kary;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto states = larch::extract_leaf_site_states(dag, result.grammar, 1);

  CHECK(throws_runtime_error([&] {
    (void)larch::build_single_site_chart(result.grammar, states);
  }));

  std::println("  PASS");
}

static void test_binary_compatibility_rejects_out_of_range_taxa() {
  std::println("test_binary_compatibility_rejects_out_of_range_taxa");

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A"};
  grammar.taxa.sample_id_to_id.emplace("A", 0);
  grammar.clades.push_back(larch::clade_key{{1}});
  grammar.productions_by_parent.push_back({});
  grammar.productions_by_child.push_back({});
  grammar.root_clade = 0;

  CHECK(!larch::grammar_is_binary_chart_compatible(grammar));

  std::println("  PASS");
}

int main() {
  test_default_reject_mode_rejects_kary();
  test_reject_mode_accepts_binary_grammar();
  test_audit_kary_returns_diagnostic_grammar();
  test_binary_chart_rejects_audit_kary_grammar();
  test_binary_compatibility_rejects_out_of_range_taxa();

  std::println("All polytomy refinement Phase 0 tests passed!");
  return 0;
}

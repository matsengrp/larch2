#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/plateau.hpp>
#include <larch/site_patterns.hpp>

#include "test_util.hpp"

#include <algorithm>
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

static larch::test::tiny_tree_node root_state_ambiguous_single_topology() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner("root", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "C")});
}

static larch::test::tiny_tree_node internally_fluid_same_visibility_tree1() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("CDE", "A",
                  {tiny_leaf("C", "A"),
                   tiny_inner("DE", "A",
                              {tiny_leaf("D", "A"), tiny_leaf("E", "A")})}),
       tiny_inner("FGH", "A",
                  {tiny_leaf("F", "C"),
                   tiny_inner("GH", "A",
                              {tiny_leaf("G", "A"), tiny_leaf("H", "C")})})});
}

static larch::test::tiny_tree_node internally_fluid_same_visibility_tree2() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("CDE", "A",
                  {tiny_leaf("D", "A"),
                   tiny_inner("CE", "A",
                              {tiny_leaf("C", "A"), tiny_leaf("E", "A")})}),
       tiny_inner("FGH", "A",
                  {tiny_leaf("F", "C"),
                   tiny_inner("GH", "A",
                              {tiny_leaf("G", "A"), tiny_leaf("H", "C")})})});
}

static larch::test::tiny_tree_node externally_rigid_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CDE", "A",
                  {tiny_leaf("C", "A"),
                   tiny_inner("DE", "A",
                              {tiny_leaf("D", "A"), tiny_leaf("E", "C")})})});
}

static larch::test::tiny_tree_node externally_rigid_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CDE", "A",
                  {tiny_leaf("D", "A"),
                   tiny_inner("CE", "A",
                              {tiny_leaf("C", "A"), tiny_leaf("E", "C")})})});
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

static larch::phylo_dag merged_paper_dag() {
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  return larch::test::merge_tiny_trees(std::move(trees));
}

static void test_paper_local_trace_fluidity() {
  std::println("test_paper_local_trace_fluidity");

  auto merged = merged_paper_dag();
  auto grammar = larch::build_clade_grammar(merged);

  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto de = clade_for(grammar, {"D", "E"});
  auto ce = clade_for(grammar, {"C", "E"});
  auto cde = clade_for(grammar, {"C", "D", "E"});
  auto prod_c_de = production_id_for(grammar, cde, {c, de});
  auto prod_d_ce = production_id_for(grammar, cde, {d, ce});

  auto states = larch::extract_leaf_site_states(merged, grammar, 1);
  larch::chart_options opts;
  opts.keep_trace = true;
  auto chart = larch::build_single_site_chart(grammar, states, opts);
  auto outside = larch::build_single_site_outside_chart(grammar, chart);
  auto report = larch::build_single_site_fluidity_report(grammar, chart, outside);

  CHECK(report.global_min == 1);
  CHECK(report.locally_fluid_clade_state[cde][larch::nuc_base::C]);
  CHECK(!report.locally_fluid_clade_state[cde][larch::nuc_base::A]);
  CHECK(report.optimal_choice_count[cde][larch::nuc_base::C] == 2);
  CHECK(report.optimal_choice_count[cde][larch::nuc_base::A] == 1);

  // Site 1's locally-fluid C state is not parent-compatible in a globally
  // optimal complete tree; the globally optimal CDE state is A and keeps only
  // the C|DE resolution.
  CHECK(report.globally_optimal_clade_state[cde][larch::nuc_base::A]);
  CHECK(!report.globally_optimal_clade_state[cde][larch::nuc_base::C]);
  CHECK(!report.fluid_clade_state[cde][larch::nuc_base::C]);
  CHECK(!report.globally_fluid_clade_state[cde][larch::nuc_base::C]);
  CHECK(report.globally_optimal_choice_count[cde][larch::nuc_base::A] >= 1);
  CHECK(report.globally_optimal_choices_by_clade_state[cde][larch::nuc_base::A]
            .front()
            .production == prod_c_de);

  auto c_state_productions = larch::plateau_detail::unique_productions(
      report.optimal_choices_by_clade_state[cde][larch::nuc_base::C]);
  CHECK(c_state_productions.size() == 2);
  CHECK(std::find(c_state_productions.begin(), c_state_productions.end(),
                  prod_c_de) != c_state_productions.end());
  CHECK(std::find(c_state_productions.begin(), c_state_productions.end(),
                  prod_d_ce) != c_state_productions.end());

  std::println("  PASS");
}

static void test_root_state_ambiguity_is_not_external_fluidity() {
  std::println("test_root_state_ambiguity_is_not_external_fluidity");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", root_state_ambiguous_single_topology());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  larch::chart_options opts;
  opts.keep_trace = true;
  auto chart = larch::build_single_site_chart(grammar, states, opts);
  auto outside = larch::build_single_site_outside_chart(grammar, chart);
  auto report = larch::build_single_site_fluidity_report(grammar, chart, outside);

  CHECK(report.global_min == 1);
  CHECK(report.externally_fluid_clade_count == 0);
  CHECK(report.externally_fluid_group_count == 0);
  for (auto external : report.externally_fluid_clade) CHECK(!external);

  auto graph = larch::build_plateau_graph(grammar, report);
  CHECK(graph.nodes.empty());
  CHECK(graph.edges.empty());

  std::println("  PASS");
}

static void test_internal_fluidity_under_sibling_state_ambiguity_is_not_external() {
  std::println(
      "test_internal_fluidity_under_sibling_state_ambiguity_is_not_external");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", internally_fluid_same_visibility_tree1()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", internally_fluid_same_visibility_tree2()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto cde = clade_for(grammar, {"C", "D", "E"});

  auto states = larch::extract_leaf_site_states(merged, grammar, 1);
  larch::chart_options opts;
  opts.keep_trace = true;
  opts.score_ua_edge = true;
  auto chart = larch::build_single_site_chart(grammar, states, opts);
  auto outside = larch::build_single_site_outside_chart(
      grammar, chart, opts, static_cast<std::uint8_t>(larch::nuc_base::A));
  auto report = larch::build_single_site_fluidity_report(grammar, chart, outside);

  CHECK(report.global_min == 2);
  CHECK(report.fluid_clade_state[cde][larch::nuc_base::A]);
  CHECK(report.globally_optimal_choice_count[cde][larch::nuc_base::A] == 2);
  CHECK(report.parent_compatibilities[cde][larch::nuc_base::A].size() == 2);

  // The two CDE resolutions are internally fluid, but both present state A to
  // the parent and both are compatible with the same sibling/root ambiguity.
  // Parent-context ambiguity alone should not turn an invisible internal
  // plateau into an externally-fluid group.
  CHECK(!report.externally_fluid_clade[cde]);
  auto graph = larch::build_plateau_graph(grammar, report);
  for (auto const& node : graph.nodes) CHECK(node.clade != cde);

  std::println("  PASS");
}

static void test_externally_rigid_internal_plateau() {
  std::println("test_externally_rigid_internal_plateau");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", externally_rigid_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", externally_rigid_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto cde = clade_for(grammar, {"C", "D", "E"});

  auto states = larch::extract_leaf_site_states(merged, grammar, 1);
  larch::chart_options opts;
  opts.keep_trace = true;
  auto chart = larch::build_single_site_chart(grammar, states, opts);
  auto outside = larch::build_single_site_outside_chart(grammar, chart);
  auto report = larch::build_single_site_fluidity_report(grammar, chart, outside);

  CHECK(report.global_min == 1);
  CHECK(report.locally_fluid_clade_state[cde][larch::nuc_base::A]);
  CHECK(report.globally_optimal_clade_state[cde][larch::nuc_base::A]);
  CHECK(report.fluid_clade_state[cde][larch::nuc_base::A]);
  CHECK(report.globally_fluid_clade_state[cde][larch::nuc_base::A]);
  CHECK(report.globally_optimal_choice_count[cde][larch::nuc_base::A] == 2);
  CHECK(!report.externally_fluid_clade[cde]);

  auto graph = larch::build_plateau_graph(grammar, report);
  bool cde_in_graph = false;
  for (auto const& node : graph.nodes)
    cde_in_graph = cde_in_graph || node.clade == cde;
  CHECK(!cde_in_graph);

  std::println("  PASS");
}

static void test_multisite_guard() {
  std::println("test_multisite_guard");

  auto merged = merged_paper_dag();
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);
  CHECK(patterns.patterns.size() == 2);
  CHECK(throws_runtime_error(
      [&] { (void)larch::build_multisite_plateau_report(grammar, patterns); }));

  std::println("  PASS");
}

int main() try {
  test_paper_local_trace_fluidity();
  test_root_state_ambiguity_is_not_external_fluidity();
  test_internal_fluidity_under_sibling_state_ambiguity_is_not_external();
  test_externally_rigid_internal_plateau();
  test_multisite_guard();
  std::println("All plateau tests passed!");
  return 0;
} catch (std::exception const& e) {
  std::println(stderr, "plateau_test failed: {}", e.what());
  return 1;
}

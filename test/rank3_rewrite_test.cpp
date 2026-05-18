#include <larch/rank3_rewrite.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <print>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file, int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr) \
  do { \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

static larch::test::tiny_tree_node four_taxon_base_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

static larch::taxon_id taxon_for(larch::clade_grammar const& grammar,
                                 std::string const& sample_id) {
  auto it = grammar.taxa.sample_id_to_id.find(sample_id);
  CHECK(it != grammar.taxa.sample_id_to_id.end());
  return it->second;
}

static std::vector<larch::taxon_id> taxa_for(
    larch::clade_grammar const& grammar, std::vector<std::string> sample_ids) {
  std::vector<larch::taxon_id> ids;
  ids.reserve(sample_ids.size());
  for (auto const& sample_id : sample_ids)
    ids.push_back(taxon_for(grammar, sample_id));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

static larch::clade_id clade_for(larch::clade_grammar const& grammar,
                                 std::vector<std::string> sample_ids) {
  auto ids = taxa_for(grammar, std::move(sample_ids));
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

static std::set<std::string> production_shape(larch::clade_grammar const& grammar) {
  auto taxa_key = [](std::vector<larch::taxon_id> taxa) {
    std::sort(taxa.begin(), taxa.end());
    std::ostringstream out;
    for (auto taxon : taxa) out << taxon << ',';
    return out.str();
  };

  std::set<std::string> result;
  for (auto const& prod : grammar.productions) {
    std::vector<std::string> children;
    for (auto child : prod.children)
      children.push_back(taxa_key(grammar.clades[child].taxa));
    std::sort(children.begin(), children.end());
    std::ostringstream out;
    out << taxa_key(grammar.clades[prod.parent].taxa) << "->";
    for (auto const& child : children) out << child << '|';
    result.insert(out.str());
  }
  return result;
}

static larch::overlay_grammar_production temp_prod(
    larch::overlay_clade_ref parent,
    std::vector<larch::overlay_clade_ref> children) {
  std::sort(children.begin(), children.end());
  larch::overlay_grammar_production prod;
  prod.parent = parent;
  prod.children = std::move(children);
  prod.multiplicity = 1;
  return prod;
}

static larch::grammar_spr_candidate cross_candidate(
    larch::clade_grammar const& grammar) {
  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto root = grammar.root_clade;
  auto root_pid = production_id_for(grammar, root, {ab, cd});

  larch::grammar_spr_candidate candidate;
  candidate.moved_clade = larch::base_clade_ref(a);
  candidate.old_parent = larch::base_clade_ref(ab);
  candidate.old_sibling = larch::base_clade_ref(b);
  candidate.new_sibling_or_target = larch::base_clade_ref(c);
  candidate.removed_productions.push_back(larch::base_production_ref(root_pid));
  candidate.added_clades.push_back(larch::clade_key{taxa_for(grammar, {"A", "C"})});
  candidate.added_clades.push_back(larch::clade_key{taxa_for(grammar, {"B", "D"})});
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(0), {larch::base_clade_ref(a), larch::base_clade_ref(c)}));
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(1), {larch::base_clade_ref(b), larch::base_clade_ref(d)}));
  candidate.added_productions.push_back(
      temp_prod(larch::base_clade_ref(root), {larch::temp_clade_ref(0), larch::temp_clade_ref(1)}));
  return candidate;
}

static larch::grammar_spr_candidate cross_candidate_without_tombstone(
    larch::clade_grammar const& grammar) {
  auto candidate = cross_candidate(grammar);
  candidate.removed_productions.clear();
  return candidate;
}

static bool has_clade(larch::clade_grammar const& grammar,
                      std::vector<std::string> sample_ids) {
  auto ids = taxa_for(grammar, std::move(sample_ids));
  return std::any_of(grammar.clades.begin(), grammar.clades.end(),
                     [&](auto const& clade) { return clade.taxa == ids; });
}

static void set_all_edge_weights(larch::phylo_dag& dag, float weight) {
  for (auto ev : dag.get_all_edges()) {
    std::visit([&](auto edge) { edge.edge_weight() = weight; }, ev);
  }
}

static double min_edge_weight(larch::phylo_dag& dag) {
  double best = std::numeric_limits<double>::infinity();
  for (auto ev : dag.get_all_edges()) {
    std::visit([&](auto edge) {
      best = std::min(best, static_cast<double>(edge.edge_weight()));
    }, ev);
  }
  return best;
}

static void test_materialize_tree_from_overlay_topology() {
  std::println("test_materialize_tree_from_overlay_topology");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto overlay = larch::overlay_from_candidate(base_grammar,
                                               cross_candidate(base_grammar));
  auto materialized = larch::materialize_overlay_grammar(overlay);
  auto topology = larch::first_rank3_topology(materialized.grammar);

  auto tree = larch::materialize_rank3_tree_from_topology(
      dag, materialized.grammar, topology);
  CHECK(larch::is_tree(tree));
  auto tree_grammar = larch::build_clade_grammar(tree);
  CHECK(production_shape(tree_grammar) == production_shape(materialized.grammar));
  CHECK(larch::test::score_tree_parsimony(tree, false) == 2);

  std::println("  PASS");
}

static void test_option_a_merges_and_rebuilds_grammar() {
  std::println("test_option_a_merges_and_rebuilds_grammar");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);

  auto result = larch::materialize_rank3_option_a(dag, base_grammar, candidate);
  CHECK(result.materialized_tree_count == 1);
  CHECK(result.rebuilt.grammar.root_clade != larch::no_clade);
  CHECK(result.all_intended_productions_present());
  CHECK(result.intended_productions.size() >= 3);

  auto ac = clade_for(result.rebuilt.grammar, {"A", "C"});
  auto bd = clade_for(result.rebuilt.grammar, {"B", "D"});
  CHECK(ac != larch::no_clade);
  CHECK(bd != larch::no_clade);
  CHECK(production_id_for(result.rebuilt.grammar, ac,
                          {clade_for(result.rebuilt.grammar, {"A"}),
                           clade_for(result.rebuilt.grammar, {"C"})}) !=
        larch::no_production);
  CHECK(production_id_for(result.rebuilt.grammar, bd,
                          {clade_for(result.rebuilt.grammar, {"B"}),
                           clade_for(result.rebuilt.grammar, {"D"})}) !=
        larch::no_production);

  // Option A adds/merges concrete trees and then rebuilds the grammar; it does
  // not destructively remove the old topology from the input DAG.
  CHECK(result.rebuilt.grammar.productions.size() > base_grammar.productions.size());

  std::println("  PASS");
}

static void test_single_site_traceback_option_a() {
  std::println("test_single_site_traceback_option_a");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, base_grammar, 1);
  auto candidate = cross_candidate(base_grammar);

  auto result = larch::materialize_rank3_option_a_single_site(
      dag, base_grammar, candidate, states);
  CHECK(result.materialized_tree_count == 1);
  CHECK(result.all_intended_productions_present());

  std::println("  PASS");
}

static void test_default_candidate_prefers_added_productions() {
  std::println("test_default_candidate_prefers_added_productions");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate_without_tombstone(base_grammar);

  auto result = larch::materialize_rank3_option_a(dag, base_grammar, candidate);
  CHECK(result.all_intended_productions_present());
  CHECK(has_clade(result.rebuilt.grammar, {"A", "C"}));
  CHECK(has_clade(result.rebuilt.grammar, {"B", "D"}));

  std::println("  PASS");
}

static void test_explicit_old_topology_missing_candidate_productions_fails() {
  std::println("test_explicit_old_topology_missing_candidate_productions_fails");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate_without_tombstone(base_grammar);
  auto overlay = larch::overlay_from_candidate(base_grammar, candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  auto old_topology = larch::first_rank3_topology(materialized.grammar);

  bool threw = false;
  try {
    (void)larch::materialize_rank3_option_a(
        dag, base_grammar, candidate,
        std::vector<larch::rank3_topology>{old_topology});
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  larch::rank3_option_a_options permissive;
  permissive.require_intended_productions_present = false;
  auto result = larch::materialize_rank3_option_a(
      dag, base_grammar, candidate,
      std::vector<larch::rank3_topology>{old_topology}, permissive);
  CHECK(!result.all_intended_productions_present());

  std::println("  PASS");
}

static void test_generated_edge_weights_do_not_lower_existing_weights() {
  std::println("test_generated_edge_weights_do_not_lower_existing_weights");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  set_all_edge_weights(dag, 7.0f);
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);

  auto result = larch::materialize_rank3_option_a(dag, base_grammar, candidate);
  CHECK(min_edge_weight(result.dag) == 7.0);

  std::println("  PASS");
}

static void test_include_original_false_replaces_with_generated_tree() {
  std::println("test_include_original_false_replaces_with_generated_tree");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  larch::rank3_option_a_options options;
  options.include_original_dag = false;

  auto result = larch::materialize_rank3_option_a(dag, base_grammar, candidate,
                                                  options);
  CHECK(result.all_intended_productions_present());
  CHECK(has_clade(result.rebuilt.grammar, {"A", "C"}));
  CHECK(has_clade(result.rebuilt.grammar, {"B", "D"}));
  CHECK(!has_clade(result.rebuilt.grammar, {"A", "B"}));
  CHECK(!has_clade(result.rebuilt.grammar, {"C", "D"}));

  std::println("  PASS");
}

static void test_score_ua_edge_true_single_site_overload() {
  std::println("test_score_ua_edge_true_single_site_overload");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, base_grammar, 1);
  auto candidate = cross_candidate(base_grammar);
  larch::chart_options chart_opts;
  chart_opts.score_ua_edge = true;

  auto result = larch::materialize_rank3_option_a_single_site(
      dag, base_grammar, candidate, states, chart_opts,
      larch::extract_reference_site_state(dag, 1));
  CHECK(result.all_intended_productions_present());

  std::println("  PASS");
}

static void test_invalid_unreachable_used_production_rejected() {
  std::println("test_invalid_unreachable_used_production_rejected");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate_without_tombstone(base_grammar);
  auto overlay = larch::overlay_from_candidate(base_grammar, candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  auto topology = larch::first_rank3_topology(materialized.grammar);

  for (auto dense_pid : materialized.temp_production_to_dense) {
    if (dense_pid != larch::no_production) topology.used_production[dense_pid] = true;
  }

  bool threw = false;
  try {
    (void)larch::materialize_rank3_tree_from_topology(
        dag, materialized.grammar, topology);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

int main() {
  test_materialize_tree_from_overlay_topology();
  test_option_a_merges_and_rebuilds_grammar();
  test_single_site_traceback_option_a();
  test_default_candidate_prefers_added_productions();
  test_explicit_old_topology_missing_candidate_productions_fails();
  test_generated_edge_weights_do_not_lower_existing_weights();
  test_include_original_false_replaces_with_generated_tree();
  test_score_ua_edge_true_single_site_overload();
  test_invalid_unreachable_used_production_rejected();
  std::println("rank3_rewrite_test PASS");
  return 0;
}

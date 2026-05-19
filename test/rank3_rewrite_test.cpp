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

static larch::test::tiny_tree_node four_taxon_alt_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node three_taxon_missing_d_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_leaf("C", "C")});
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

static std::size_t leaf_node_for(larch::phylo_dag& dag,
                                 std::string const& sample_id) {
  for (auto nv : dag.get_all_nodes()) {
    std::optional<std::size_t> found;
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            if (node.sample_id() == sample_id) found = node.index();
          }
        },
        nv);
    if (found) return *found;
  }
  CHECK(false && "missing leaf node");
  return 0;
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

static larch::grammar_spr_candidate mutually_exclusive_cross_candidate(
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
  candidate.added_clades.push_back(larch::clade_key{taxa_for(grammar, {"A", "D"})});
  candidate.added_clades.push_back(larch::clade_key{taxa_for(grammar, {"B", "C"})});
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(0), {larch::base_clade_ref(a), larch::base_clade_ref(c)}));
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(1), {larch::base_clade_ref(b), larch::base_clade_ref(d)}));
  candidate.added_productions.push_back(
      temp_prod(larch::base_clade_ref(root), {larch::temp_clade_ref(0), larch::temp_clade_ref(1)}));
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(2), {larch::base_clade_ref(a), larch::base_clade_ref(d)}));
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(3), {larch::base_clade_ref(b), larch::base_clade_ref(c)}));
  candidate.added_productions.push_back(
      temp_prod(larch::base_clade_ref(root), {larch::temp_clade_ref(2), larch::temp_clade_ref(3)}));
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

static void test_option_b_stages_overlay_without_mutating_source() {
  std::println("test_option_b_stages_overlay_without_mutating_source");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto before_grammar = larch::build_clade_grammar(dag);
  auto before_shape = production_shape(before_grammar);
  auto before_nodes = larch::node_count(dag);
  auto before_edges = larch::edge_count(dag);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");
  larch::spr_move move{.src = src,
                       .dst = dst,
                       .lca = larch::compute_lca(dag, src, dst),
                       .score_change = std::nullopt};

  auto stage = larch::stage_rank3_option_b_overlay(dag, move);
  CHECK(stage.fragment_root != larch::no_idx);
  CHECK(larch::node_count(dag) == before_nodes);
  CHECK(larch::edge_count(dag) == before_edges);
  auto still_before = larch::build_clade_grammar(dag);
  CHECK(production_shape(still_before) == before_shape);

  auto overlay_tree = larch::materialize_rank3_option_b_overlay_tree(stage);
  auto overlay_grammar = larch::build_clade_grammar(overlay_tree);
  auto after_tree = larch::apply_spr_move(dag, src, dst);
  auto after_grammar = larch::build_clade_grammar(after_tree);
  CHECK(production_shape(overlay_grammar) == production_shape(after_grammar));

  std::println("  PASS");
}

static void test_option_b_projected_candidate_materializes_replacement() {
  std::println("test_option_b_projected_candidate_materializes_replacement");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");
  larch::spr_move move{.src = src,
                       .dst = dst,
                       .lca = larch::compute_lca(dag, src, dst),
                       .score_change = std::nullopt};
  auto candidate = larch::project_tree_spr_move_to_candidate(base_grammar, dag,
                                                             move);
  CHECK(candidate.has_value());

  larch::rank3_option_b_options options;
  options.include_original_dag = false;
  options.use_source_tree_overlay_staging = true;
  auto result = larch::materialize_rank3_option_b(dag, base_grammar,
                                                  *candidate, options);
  CHECK(result.staged_in_overlay);
  CHECK(result.used_source_tree_move);
  CHECK(result.materialized_tree_count == 1);
  CHECK(result.all_intended_productions_present());
  CHECK(has_clade(result.rebuilt.grammar, {"A", "D"}));
  CHECK(has_clade(result.rebuilt.grammar, {"A", "C", "D"}));
  CHECK(!has_clade(result.rebuilt.grammar, {"A", "B"}));
  CHECK(!has_clade(result.rebuilt.grammar, {"C", "D"}));

  std::println("  PASS");
}

static void test_option_b_grammar_native_without_source_move() {
  std::println("test_option_b_grammar_native_without_source_move");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);

  larch::rank3_option_b_options options;
  options.include_original_dag = false;
  auto result = larch::materialize_rank3_option_b(dag, base_grammar,
                                                  candidate, options);
  CHECK(!result.used_source_tree_move);
  CHECK(!result.staged_in_overlay);
  CHECK(result.selection_diagnostics.has_value());
  CHECK(result.selection_diagnostics->reachable_temp_production_count == 3);
  CHECK(result.materialized_tree_count == 1);
  CHECK(result.all_intended_productions_present());
  CHECK(has_clade(result.rebuilt.grammar, {"A", "C"}));
  CHECK(has_clade(result.rebuilt.grammar, {"B", "D"}));
  CHECK(!has_clade(result.rebuilt.grammar, {"A", "B"}));
  CHECK(!has_clade(result.rebuilt.grammar, {"C", "D"}));

  std::println("  PASS");
}

static void test_grammar_native_explicit_topology_path() {
  std::println("test_grammar_native_explicit_topology_path");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate_without_tombstone(base_grammar);
  auto dense = larch::validate_and_dense_materialize_candidate(
      base_grammar, candidate);

  std::vector<larch::production_id> temp_productions;
  for (auto pid : dense.temp_production_to_dense) {
    if (pid != larch::no_production) temp_productions.push_back(pid);
  }
  auto topology = larch::rank3_topology_preferring_productions(
      dense.grammar, temp_productions);

  larch::grammar_native_materialization_options options;
  options.include_original_dag = false;
  options.topology_policy = larch::grammar_native_topology_policy::explicit_topologies;
  options.explicit_topologies.push_back(topology);
  auto result = larch::materialize_grammar_native_overlay(
      dag, base_grammar, candidate, options);
  CHECK(result.selected_topologies.size() == 1);
  CHECK(result.merged.materialized_tree_count == 1);
  CHECK(result.merged.all_intended_productions_present());
  CHECK(has_clade(result.merged.rebuilt.grammar, {"A", "C"}));
  CHECK(has_clade(result.merged.rebuilt.grammar, {"B", "D"}));

  std::println("  PASS");
}

static void test_grammar_native_single_site_policy_path() {
  std::println("test_grammar_native_single_site_policy_path");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  auto states = larch::extract_leaf_site_states(dag, base_grammar, 1);

  larch::grammar_native_materialization_options options;
  options.include_original_dag = false;
  options.topology_policy = larch::grammar_native_topology_policy::single_site_optimal;
  options.single_site_leaf_states = states;
  auto result = larch::materialize_grammar_native_overlay(
      dag, base_grammar, candidate, options);
  CHECK(result.selected_topologies.size() == 1);
  CHECK(result.selection_diagnostics.selection_kind ==
        larch::grammar_native_selection_kind::exact);
  CHECK(result.merged.all_intended_productions_present());
  CHECK(has_clade(result.merged.rebuilt.grammar, {"A", "C"}));
  CHECK(has_clade(result.merged.rebuilt.grammar, {"B", "D"}));

  std::println("  PASS");
}

static void test_grammar_native_multisite_policy_uses_selector() {
  std::println("test_grammar_native_multisite_policy_uses_selector");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate_without_tombstone(base_grammar);
  auto dense = larch::validate_and_dense_materialize_candidate(
      base_grammar, candidate);

  std::vector<larch::production_id> temp_productions;
  for (auto pid : dense.temp_production_to_dense) {
    if (pid != larch::no_production) temp_productions.push_back(pid);
  }
  auto topology = larch::rank3_topology_preferring_productions(
      dense.grammar, temp_productions);

  larch::grammar_native_materialization_options options;
  options.include_original_dag = false;
  options.topology_policy = larch::grammar_native_topology_policy::multisite_bnb_optimal;
  options.topology_selector = [topology](auto const&) {
    return std::vector<larch::rank3_topology>{topology};
  };
  auto result = larch::materialize_grammar_native_overlay(
      dag, base_grammar, candidate, options);
  CHECK(result.selection_diagnostics.selection_kind ==
        larch::grammar_native_selection_kind::caller_supplied);
  CHECK(result.merged.all_intended_productions_present());

  std::println("  PASS");
}

static void test_grammar_native_mutually_exclusive_temp_productions() {
  std::println("test_grammar_native_mutually_exclusive_temp_productions");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = mutually_exclusive_cross_candidate(base_grammar);

  bool threw = false;
  try {
    (void)larch::materialize_grammar_native_overlay(dag, base_grammar,
                                                    candidate);
  } catch (larch::grammar_native_selection_error const& e) {
    threw = true;
    CHECK(e.diagnostics.reachable_temp_production_count == 6);
    CHECK(e.diagnostics.selected_temp_production_count_by_topology.size() == 1);
    CHECK(e.diagnostics.selected_temp_production_count_by_topology[0] == 3);
    CHECK(e.diagnostics.uncovered_temp_productions.size() == 3);
    CHECK(e.diagnostics.topology_cap_truncated);
  }
  CHECK(threw);

  larch::grammar_native_materialization_options options;
  options.include_original_dag = false;
  options.topology_policy = larch::grammar_native_topology_policy::bounded_enumeration;
  options.max_materialized_topologies = 2;
  auto result = larch::materialize_grammar_native_overlay(
      dag, base_grammar, candidate, options);
  CHECK(result.selected_topologies.size() == 2);
  CHECK(result.selection_diagnostics.reachable_temp_production_count == 6);
  CHECK(result.selection_diagnostics.selected_temp_production_count_by_topology.size() == 2);
  CHECK(result.selection_diagnostics.selected_temp_production_count_by_topology[0] == 3);
  CHECK(result.selection_diagnostics.selected_temp_production_count_by_topology[1] == 3);
  CHECK(result.selection_diagnostics.uncovered_temp_productions.empty());
  CHECK(!result.selection_diagnostics.topology_cap_truncated);
  CHECK(result.selection_diagnostics.selection_kind ==
        larch::grammar_native_selection_kind::heuristic);
  CHECK(result.merged.materialized_tree_count == 2);
  CHECK(result.merged.all_intended_productions_present());
  CHECK(has_clade(result.merged.rebuilt.grammar, {"A", "C"}));
  CHECK(has_clade(result.merged.rebuilt.grammar, {"B", "D"}));
  CHECK(has_clade(result.merged.rebuilt.grammar, {"A", "D"}));
  CHECK(has_clade(result.merged.rebuilt.grammar, {"B", "C"}));

  std::println("  PASS");
}

static void test_grammar_native_rejects_invalid_temp_partition() {
  std::println("test_grammar_native_rejects_invalid_temp_partition");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  candidate.added_productions[0].children = {
      larch::base_clade_ref(clade_for(base_grammar, {"A"})),
      larch::base_clade_ref(clade_for(base_grammar, {"B"}))};

  bool threw = false;
  try {
    (void)larch::validate_and_dense_materialize_candidate(base_grammar,
                                                          candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_grammar_native_rejects_temp_clade_taxon_out_of_range() {
  std::println("test_grammar_native_rejects_temp_clade_taxon_out_of_range");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  candidate.added_clades[0].taxa = {
      static_cast<larch::taxon_id>(base_grammar.taxa.id_to_sample_id.size())};

  bool threw = false;
  try {
    (void)larch::validate_and_dense_materialize_candidate(base_grammar,
                                                          candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_grammar_native_rejects_temp_clade_ref_out_of_range() {
  std::println("test_grammar_native_rejects_temp_clade_ref_out_of_range");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  candidate.added_productions[0].children[0] = larch::temp_clade_ref(99);

  bool threw = false;
  try {
    (void)larch::validate_and_dense_materialize_candidate(base_grammar,
                                                          candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_grammar_native_rejects_non_binary_temp_production() {
  std::println("test_grammar_native_rejects_non_binary_temp_production");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  candidate.added_productions[0].children.push_back(
      larch::base_clade_ref(clade_for(base_grammar, {"D"})));

  bool threw = false;
  try {
    (void)larch::validate_and_dense_materialize_candidate(base_grammar,
                                                          candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_grammar_native_rejects_overlapping_temp_children() {
  std::println("test_grammar_native_rejects_overlapping_temp_children");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  candidate.added_clades.push_back(
      larch::clade_key{taxa_for(base_grammar, {"A", "B", "C"})});
  auto abc_temp = static_cast<larch::clade_id>(candidate.added_clades.size() - 1);
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(abc_temp),
                {larch::base_clade_ref(clade_for(base_grammar, {"A", "B"})),
                 larch::temp_clade_ref(0)}));

  bool threw = false;
  try {
    (void)larch::validate_and_dense_materialize_candidate(base_grammar,
                                                          candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_grammar_native_rejects_unreachable_temp_production() {
  std::println("test_grammar_native_rejects_unreachable_temp_production");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto candidate = cross_candidate(base_grammar);
  candidate.added_clades.push_back(
      larch::clade_key{taxa_for(base_grammar, {"A", "D"})});
  auto ad_temp = static_cast<larch::clade_id>(candidate.added_clades.size() - 1);
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(ad_temp),
                {larch::base_clade_ref(clade_for(base_grammar, {"A"})),
                 larch::base_clade_ref(clade_for(base_grammar, {"D"}))}));

  bool threw = false;
  try {
    (void)larch::validate_and_dense_materialize_candidate(base_grammar,
                                                          candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_grammar_native_rejects_missing_source_leaf_compact_genome() {
  std::println("test_grammar_native_rejects_missing_source_leaf_compact_genome");

  auto base_dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto source_without_d = larch::test::make_tiny_labelled_tree(
      "A", three_taxon_missing_d_tree());
  auto base_grammar = larch::build_clade_grammar(base_dag);
  auto candidate = cross_candidate(base_grammar);

  bool threw = false;
  try {
    (void)larch::materialize_grammar_native_overlay(source_without_d,
                                                    base_grammar, candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_option_b_added_edge_weights_do_not_lower_existing_weights() {
  std::println("test_option_b_added_edge_weights_do_not_lower_existing_weights");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  set_all_edge_weights(dag, 7.0f);
  auto base_grammar = larch::build_clade_grammar(dag);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");
  larch::spr_move move{.src = src,
                       .dst = dst,
                       .lca = larch::compute_lca(dag, src, dst),
                       .score_change = std::nullopt};
  auto candidate = larch::project_tree_spr_move_to_candidate(base_grammar, dag,
                                                             move);
  CHECK(candidate.has_value());

  larch::rank3_option_b_options options;
  options.use_source_tree_overlay_staging = true;
  auto result = larch::materialize_rank3_option_b(dag, base_grammar,
                                                  *candidate, options);
  CHECK(result.all_intended_productions_present());
  CHECK(min_edge_weight(result.dag) == 7.0);

  std::println("  PASS");
}

static void test_option_b_grammar_native_accepts_collapsed_dag_base_grammar() {
  std::println("test_option_b_grammar_native_accepts_collapsed_dag_base_grammar");

  auto source_tree = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree());
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", four_taxon_alt_tree()));
  auto base_dag = larch::test::merge_tiny_trees(trees);
  auto base_grammar = larch::build_clade_grammar(base_dag);
  CHECK(base_grammar.productions.size() >
        larch::build_clade_grammar(source_tree).productions.size());

  auto src = leaf_node_for(source_tree, "A");
  auto dst = leaf_node_for(source_tree, "D");
  larch::spr_move move{.src = src,
                       .dst = dst,
                       .lca = larch::compute_lca(source_tree, src, dst),
                       .score_change = std::nullopt};
  auto candidate = larch::project_tree_spr_move_to_candidate(
      base_grammar, source_tree, move);
  CHECK(candidate.has_value());

  larch::rank3_option_b_options options;
  options.include_original_dag = false;
  auto result = larch::materialize_rank3_option_b(
      source_tree, base_grammar, *candidate, options);
  CHECK(!result.staged_in_overlay);
  CHECK(!result.used_source_tree_move);
  CHECK(result.selection_diagnostics.has_value());
  CHECK(result.all_intended_productions_present());
  CHECK(has_clade(result.rebuilt.grammar, {"A", "D"}));
  CHECK(has_clade(result.rebuilt.grammar, {"A", "C", "D"}));

  std::println("  PASS");
}

static void test_option_b_rejects_stale_projected_candidate() {
  std::println("test_option_b_rejects_stale_projected_candidate");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto base_grammar = larch::build_clade_grammar(dag);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");
  larch::spr_move move{.src = src,
                       .dst = dst,
                       .lca = larch::compute_lca(dag, src, dst),
                       .score_change = std::nullopt};
  auto candidate = larch::project_tree_spr_move_to_candidate(base_grammar, dag,
                                                             move);
  CHECK(candidate.has_value());
  candidate->new_sibling_or_target =
      larch::base_clade_ref(clade_for(base_grammar, {"C"}));

  bool threw = false;
  try {
    (void)larch::stage_rank3_option_b_overlay(dag, base_grammar, *candidate);
  } catch (std::runtime_error const&) {
    threw = true;
  }
  CHECK(threw);

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
  test_option_b_stages_overlay_without_mutating_source();
  test_option_b_projected_candidate_materializes_replacement();
  test_option_b_grammar_native_without_source_move();
  test_grammar_native_explicit_topology_path();
  test_grammar_native_single_site_policy_path();
  test_grammar_native_multisite_policy_uses_selector();
  test_grammar_native_mutually_exclusive_temp_productions();
  test_grammar_native_rejects_invalid_temp_partition();
  test_grammar_native_rejects_temp_clade_taxon_out_of_range();
  test_grammar_native_rejects_temp_clade_ref_out_of_range();
  test_grammar_native_rejects_non_binary_temp_production();
  test_grammar_native_rejects_overlapping_temp_children();
  test_grammar_native_rejects_unreachable_temp_production();
  test_grammar_native_rejects_missing_source_leaf_compact_genome();
  test_option_b_added_edge_weights_do_not_lower_existing_weights();
  test_option_b_grammar_native_accepts_collapsed_dag_base_grammar();
  test_option_b_rejects_stale_projected_candidate();
  test_invalid_unreachable_used_production_rejected();
  std::println("rank3_rewrite_test PASS");
  return 0;
}

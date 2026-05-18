#include <larch/chart_spr.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <print>
#include <stdexcept>
#include <set>
#include <sstream>
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
  for (auto const& sample_id : sample_ids) ids.push_back(taxon_for(grammar, sample_id));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

static larch::clade_id clade_for(larch::clade_grammar const& grammar,
                                 std::vector<std::string> sample_ids) {
  auto ids = taxa_for(grammar, std::move(sample_ids));
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa == ids) return static_cast<larch::clade_id>(cid);
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

static std::string taxa_key(std::vector<larch::taxon_id> taxa) {
  std::sort(taxa.begin(), taxa.end());
  std::ostringstream out;
  for (auto taxon : taxa) out << taxon << ',';
  return out.str();
}

static std::set<std::string> clade_shape(larch::clade_grammar const& grammar) {
  std::set<std::string> result;
  for (auto const& clade : grammar.clades) result.insert(taxa_key(clade.taxa));
  return result;
}

static std::set<std::string> production_shape(larch::clade_grammar const& grammar) {
  std::set<std::string> result;
  for (auto const& prod : grammar.productions) {
    std::vector<std::string> children;
    for (auto child : prod.children) children.push_back(taxa_key(grammar.clades[child].taxa));
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

static void test_overlay_temp_clades_and_single_site_score() {
  std::println("test_overlay_temp_clades_and_single_site_score");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto candidate = cross_candidate(grammar);

  auto overlay = larch::overlay_from_candidate(grammar, candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  CHECK(materialized.temp_clade_to_dense.size() == 2);
  CHECK(materialized.temp_clade_to_dense[0] != larch::no_clade);
  CHECK(materialized.temp_clade_to_dense[1] != larch::no_clade);
  CHECK(materialized.base_clade_to_dense[ab] == larch::no_clade);
  CHECK(materialized.base_clade_to_dense[cd] == larch::no_clade);
  CHECK(materialized.grammar.root_clade != larch::no_clade);
  CHECK(materialized.grammar.productions_by_parent[materialized.grammar.root_clade].size() == 1);

  auto score = larch::score_single_site_spr_candidate(grammar, states, candidate);
  CHECK(score.old_score == 1);
  CHECK(score.new_score == 2);
  CHECK(score.delta == 1);
  CHECK(!score.improves());
  CHECK(!score.exact_multisite);

  std::println("  PASS");
}

static void test_local_recompute_matches_full_rebuild() {
  std::println("test_local_recompute_matches_full_rebuild");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto base_chart = larch::build_single_site_chart(grammar, states);
  auto overlay = larch::overlay_from_candidate(grammar, cross_candidate(grammar));

  auto local = larch::build_single_site_overlay_chart_locally(overlay, base_chart, states);
  auto full = larch::build_single_site_overlay_chart(overlay, states);
  CHECK(local.chart.inside == full.inside);
  CHECK(local.affected_clade_count >= 3);
  CHECK(!local.used_full_rebuild);
  CHECK(larch::overlay_local_recompute_matches_full(overlay, base_chart, states));

  std::println("  PASS");
}

// TODO: add an exhaustive tiny-tree oracle here once grammar-native SPR
// enumeration is broad enough to compare complete before/after SPR sets.
static void test_grammar_native_candidate_enumeration() {
  std::println("test_grammar_native_candidate_enumeration");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto ad_taxa = taxa_for(grammar, {"A", "D"});
  auto acd_taxa = taxa_for(grammar, {"A", "C", "D"});

  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(candidates.size() >= 4);

  bool found_leaf_target_spr = false;
  for (auto const& candidate : candidates) {
    auto overlay = larch::overlay_from_candidate(grammar, candidate);
    auto materialized = larch::materialize_overlay_grammar(overlay);
    auto chart = larch::build_single_site_overlay_chart(overlay, states);
    CHECK(chart.root_min_excluding_ua(materialized.grammar.root_clade) < larch::chart_inf);

    bool has_ad = false;
    bool has_acd = false;
    for (auto const& key : candidate.added_clades) {
      has_ad = has_ad || key.taxa == ad_taxa;
      has_acd = has_acd || key.taxa == acd_taxa;
    }
    if (has_ad && has_acd) {
      found_leaf_target_spr = true;
      auto score = larch::score_single_site_spr_candidate(grammar, states, candidate);
      CHECK(score.old_score == 1);
      CHECK(score.new_score == 2);
    }
  }
  CHECK(found_leaf_target_spr);

  std::println("  PASS");
}

static void test_projected_tree_spr_matches_apply_spr_move() {
  std::println("test_projected_tree_spr_matches_apply_spr_move");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");
  larch::spr_move move{.src = src,
                       .dst = dst,
                       .lca = larch::compute_lca(dag, src, dst),
                       .score_change = std::nullopt};

  auto projected = larch::project_tree_spr_move_to_candidate(grammar, dag, move);
  CHECK(projected.has_value());
  auto overlay = larch::overlay_from_candidate(grammar, *projected);
  auto materialized = larch::materialize_overlay_grammar(overlay);

  auto after_tree = larch::apply_spr_move(dag, src, dst);
  larch::build_clade_offsets(after_tree);
  auto after_grammar = larch::build_clade_grammar(after_tree);

  CHECK(clade_shape(materialized.grammar) == clade_shape(after_grammar));
  CHECK(production_shape(materialized.grammar) == production_shape(after_grammar));

  auto projected_score = larch::score_single_site_spr_candidate(grammar, states, *projected);
  auto after_chart = larch::build_single_site_chart(after_grammar, states);
  auto after_score = after_chart.root_min_excluding_ua(after_grammar.root_clade);
  CHECK(projected_score.old_score == 1);
  CHECK(projected_score.new_score == after_score);

  larch::tree_index after_index{after_tree};
  CHECK(after_index.compute_parsimony_score() == static_cast<int>(after_score));

  std::println("  PASS");
}

static void test_bootstrap_projection_from_tree_validates_against_apply() {
  std::println("test_bootstrap_projection_from_tree_validates_against_apply");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");

  larch::tree_spr_bootstrap_options options;
  options.radius = 4;
  options.score_threshold = std::numeric_limits<int>::max();
  auto candidates = larch::bootstrap_spr_candidates_from_tree(grammar, dag, options);
  CHECK(!candidates.empty());

  bool found_a_to_d = false;
  std::size_t checked = 0;
  for (auto const& candidate : candidates) {
    CHECK(candidate.source_tree_move.has_value());
    auto const& move = *candidate.source_tree_move;
    if (move.src == src && move.dst == dst) found_a_to_d = true;

    auto after_tree = larch::apply_spr_move(dag, move.src, move.dst);
    larch::build_clade_offsets(after_tree);
    auto after_grammar = larch::build_clade_grammar(after_tree);
    auto after_chart = larch::build_single_site_chart(after_grammar, states);
    auto after_score = after_chart.root_min_excluding_ua(after_grammar.root_clade);
    auto projected_score = larch::score_single_site_spr_candidate(grammar, states, candidate);
    CHECK(projected_score.new_score == after_score);
    if (++checked == 8) break;
  }
  CHECK(found_a_to_d);

  std::println("  PASS");
}

static void test_multisite_exact_vs_lower_bound_labels() {
  std::println("test_multisite_exact_vs_lower_bound_labels");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto candidate = cross_candidate(grammar);

  auto lower = larch::score_multisite_spr_candidate_lower_bound(grammar, patterns, candidate);
  CHECK(lower.old_score == 1);
  CHECK(lower.new_score == 2);
  CHECK(!lower.exact_multisite);

  auto exact = larch::score_multisite_spr_candidate_exact(grammar, patterns, candidate);
  CHECK(exact.old_score == 1);
  CHECK(exact.new_score == 2);
  CHECK(exact.exact_multisite);

  std::println("  PASS");
}

int main() {
  test_overlay_temp_clades_and_single_site_score();
  test_local_recompute_matches_full_rebuild();
  test_grammar_native_candidate_enumeration();
  test_projected_tree_spr_matches_apply_spr_move();
  test_bootstrap_projection_from_tree_validates_against_apply();
  test_multisite_exact_vs_lower_bound_labels();
  std::println("chart_spr_test PASS");
  return 0;
}

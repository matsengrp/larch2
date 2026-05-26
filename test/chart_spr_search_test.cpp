#include <larch/chart_spr_search.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <limits>
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

static larch::test::tiny_tree_node four_taxon_base_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node four_taxon_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node four_taxon_two_site_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AC", "AA", {tiny_leaf("A", "AC"), tiny_leaf("C", "CG")}),
       tiny_inner("BD", "AA", {tiny_leaf("B", "AC"), tiny_leaf("D", "CG")})});
}

static larch::test::tiny_tree_node six_taxon_paired_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AD", "A", {tiny_leaf("A", "A"), tiny_leaf("D", "C")}),
       tiny_inner("rest", "A",
                  {tiny_inner("BE", "A", {tiny_leaf("B", "A"),
                                             tiny_leaf("E", "C")}),
                   tiny_inner("CF", "A", {tiny_leaf("C", "A"),
                                             tiny_leaf("F", "C")})})});
}

struct tiny_chart_spr_fixture {
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
  larch::site_pattern_set patterns;
  std::vector<larch::grammar_spr_candidate> candidates;
};

static tiny_chart_spr_fixture make_fixture() {
  tiny_chart_spr_fixture fixture;
  fixture.dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  fixture.grammar = larch::build_clade_grammar(fixture.dag);
  fixture.patterns = larch::build_site_patterns(fixture.dag, fixture.grammar);
  fixture.candidates = larch::enumerate_grammar_spr_candidates(fixture.grammar);
  CHECK(!fixture.candidates.empty());
  return fixture;
}

static larch::test::tiny_tree_node four_taxon_offset_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "CCAA",
      {tiny_inner("AB", "CCAA", {tiny_leaf("A", "CCAA"),
                                    tiny_leaf("B", "CCAA")}),
       tiny_inner("CD", "CCGA", {tiny_leaf("C", "CCGA"),
                                    tiny_leaf("D", "CCGA")})});
}

static larch::test::tiny_tree_node four_taxon_repeated_reference_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"),
                                  tiny_leaf("B", "AA")}),
       tiny_inner("CD", "AC", {tiny_leaf("C", "AA"),
                                  tiny_leaf("D", "CC")})});
}

static larch::test::tiny_tree_node four_taxon_two_pattern_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAA",
      {tiny_inner("AB", "AAA", {tiny_leaf("A", "AAA"),
                                   tiny_leaf("B", "AAC")}),
       tiny_inner("CD", "CCC", {tiny_leaf("C", "CCA"),
                                   tiny_leaf("D", "CCC")})});
}

static larch::test::tiny_tree_node five_taxon_multiparent_tree_one() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABC", "A",
                  {tiny_inner("AB", "A", {tiny_leaf("A", "A"),
                                             tiny_leaf("B", "C")}),
                   tiny_leaf("C", "A")}),
       tiny_inner("DE", "A", {tiny_leaf("D", "C"),
                                tiny_leaf("E", "A")})});
}

static larch::test::tiny_tree_node five_taxon_multiparent_tree_two() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABD", "A",
                  {tiny_inner("AB", "A", {tiny_leaf("A", "A"),
                                             tiny_leaf("B", "C")}),
                   tiny_leaf("D", "C")}),
       tiny_inner("CE", "A", {tiny_leaf("C", "A"),
                                tiny_leaf("E", "A")})});
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

static larch::chart_spr_production_signature production_signature_for_id(
    larch::clade_grammar const& grammar, larch::production_id pid) {
  auto const& prod = grammar.productions[pid];
  larch::chart_spr_production_signature signature;
  signature.parent_taxa = grammar.clades[prod.parent].taxa;
  std::sort(signature.parent_taxa.begin(), signature.parent_taxa.end());
  signature.child_taxa.reserve(prod.children.size());
  for (auto child : prod.children) {
    auto child_taxa = grammar.clades[child].taxa;
    std::sort(child_taxa.begin(), child_taxa.end());
    signature.child_taxa.push_back(std::move(child_taxa));
  }
  std::sort(signature.child_taxa.begin(), signature.child_taxa.end());
  return signature;
}

static bool grammar_contains_production_signature(
    larch::clade_grammar const& grammar,
    larch::chart_spr_production_signature const& signature) {
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (production_signature_for_id(
            grammar, static_cast<larch::production_id>(pid)) == signature) {
      return true;
    }
  }
  return false;
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

static void clear_optional_candidate_metadata(
    larch::grammar_spr_candidate& candidate) {
  candidate.moved_clade = {};
  candidate.old_parent = {};
  candidate.old_sibling = {};
  candidate.new_sibling_or_target = {};
  candidate.source_tree_move.reset();
  candidate.source_before_topology_productions.reset();
  candidate.source_after_topology_productions.reset();
}

static void test_lower_bound_oracle_counters_show_full_rebuild_cost() {
  std::println("test_lower_bound_oracle_counters_show_full_rebuild_cost");

  auto fixture = make_fixture();
  larch::chart_spr_search_counters counters;
  auto score = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front(), {}, &counters);

  CHECK(score.old_score > 0);
  CHECK(counters.full_overlay_materializations == 1);
  CHECK(counters.overlay_materializations_for_oracle == 1);
  CHECK(counters.overlay_materializations_for_exact_verification == 0);
  CHECK(counters.overlay_materializations_for_accept_materialization == 0);
  CHECK(counters.full_composite_rebuilds == 2);
  CHECK(counters.local_candidate_scores == 0);

  std::println("  PASS");
}

static void test_local_rejected_candidate_counter_guardrail() {
  std::println("test_local_rejected_candidate_counter_guardrail");

  auto fixture = make_fixture();
  auto states = larch::leaf_site_states{
      .state_by_taxon = fixture.patterns.patterns.front().state_by_taxon};
  auto base_chart = larch::build_single_site_chart(fixture.grammar, states);
  auto overlay = larch::overlay_from_candidate(fixture.grammar,
                                               fixture.candidates.front());

  larch::chart_spr_search_counters counters;
  auto local = larch::score_rejected_candidate_with_local_recompute_oracle(
      overlay, base_chart, states, {}, &counters);
  larch::record_chart_spr_rejected_candidate(counters);

  CHECK(local.affected_clade_count > 0);
  CHECK(counters.local_candidate_scores == 1);
  CHECK(counters.rejected_moves == 1);
  CHECK(counters.full_composite_rebuilds == 0);
  CHECK(counters.full_overlay_materializations == 1);
  CHECK(counters.overlay_materializations_for_oracle == 1);

  std::println("  PASS");
}

static std::vector<std::string> candidate_taxon_signatures(
    larch::clade_grammar const& grammar,
    std::vector<larch::grammar_spr_candidate> const& candidates) {
  std::vector<std::string> signatures;
  signatures.reserve(candidates.size());
  for (auto const& candidate : candidates) {
    signatures.push_back(
        larch::chart_spr_candidate_taxon_signature(grammar, candidate));
  }
  return signatures;
}

static void test_streaming_and_eager_candidate_apis_match() {
  std::println("test_streaming_and_eager_candidate_apis_match");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.include_root_moves = true;

  std::vector<larch::grammar_spr_candidate> streamed;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const& candidate) {
        streamed.push_back(candidate);
        return true;
      });
  auto eager = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options);

  CHECK(stats.stop_reason == larch::chart_spr_candidate_stop_reason::exhausted);
  CHECK(stats.candidates_generated_after_dedup == streamed.size());
  CHECK(candidate_taxon_signatures(fixture.grammar, streamed) ==
        candidate_taxon_signatures(fixture.grammar, eager.candidates));

  std::println("  PASS");
}

static void test_search_state_local_score_entry_point_matches_oracle() {
  std::println("test_search_state_local_score_entry_point_matches_oracle");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto scored = larch::score_candidate_locally(state, fixture.candidates.front());
  CHECK(scored.valid);

  larch::chart_spr_search_counters oracle_counters;
  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front(), {},
      &oracle_counters);

  CHECK(scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(scored.lower_bound.value.delta == oracle.delta);
  CHECK(scored.affected_clade_count > 0);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);
  CHECK(state.counters.local_candidate_scores == 1);
  CHECK(state.counters.full_composite_rebuilds == 0);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.overlay_materializations_for_local_scoring_bridge == 0);
  CHECK(state.counters.overlay_reachability_validations == 1);
  CHECK(state.counters.reachable_clades_traversed > 0);

  std::println("  PASS");
}

static void test_local_score_with_ua_edge_and_invariant_offset_matches_oracle() {
  std::println("test_local_score_with_ua_edge_and_invariant_offset_matches_oracle");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(!candidates.empty());
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto state = larch::build_chart_spr_search_state(
      dag, grammar, patterns, opts);
  auto scored = larch::score_candidate_locally(state, candidates.front());
  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      grammar, patterns, candidates.front(), opts);

  CHECK(scored.valid);
  CHECK(scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(scored.lower_bound.value.delta == oracle.delta);
  CHECK(scored.lower_bound.invariant_offset_applied ==
        state.invariant_constant_offset);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);

  std::println("  PASS");
}

static void test_production_delta_only_candidate_metadata_absent_matches_oracle() {
  std::println("test_production_delta_only_candidate_metadata_absent_matches_oracle");

  auto fixture = make_fixture();
  auto candidate = fixture.candidates.front();
  clear_optional_candidate_metadata(candidate);

  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto scored = larch::score_candidate_locally(state, candidate);
  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, candidate);
  auto signature = larch::chart_spr_candidate_taxon_signature(
      fixture.grammar, candidate);

  CHECK(scored.valid);
  CHECK(signature.find("m={};op={};os={};nt={}") !=
        std::string::npos);
  CHECK(scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(scored.lower_bound.value.delta == oracle.delta);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_multiparent_dag_affected_closure_and_slot_maps() {
  std::println("test_multiparent_dag_affected_closure_and_slot_maps");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_one()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_two()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto abc = clade_for(grammar, {"A", "B", "C"});
  auto abd = clade_for(grammar, {"A", "B", "D"});
  auto ab_prod = production_id_for(grammar, ab, {a, b});
  CHECK(grammar.productions_by_child[ab].size() >= 2);

  larch::grammar_spr_candidate candidate;
  candidate.removed_productions.push_back(larch::base_production_ref(ab_prod));
  candidate.added_productions.push_back(temp_prod(
      larch::base_clade_ref(ab), {larch::base_clade_ref(a),
                                  larch::base_clade_ref(b)}));

  auto delta = larch::build_spr_overlay_delta(grammar, candidate);
  CHECK(delta.reachability_stats.full_grammar_like);
  CHECK(delta.affected_base_clade[ab]);
  CHECK(delta.affected_base_clade[abc]);
  CHECK(delta.affected_base_clade[abd]);
  CHECK(delta.affected_base_clade[grammar.root_clade]);
  CHECK(delta.affected_base_row_slot[ab] !=
        larch::local_overlay_chart_rows::npos);
  CHECK(delta.affected_order.size() >= 4);

  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto scored = larch::score_candidate_locally(state, candidate);
  CHECK(scored.valid);
  CHECK(scored.lower_bound.value.delta == 0);
  CHECK(state.counters.reachability_full_grammar_like_passes == 1);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_unreachable_dead_clades_ignored_reachable_dead_clades_fail() {
  std::println("test_unreachable_dead_clades_ignored_reachable_dead_clades_fail");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto ab = clade_for(fixture.grammar, {"A", "B"});
  auto cd = clade_for(fixture.grammar, {"C", "D"});

  larch::grammar_spr_candidate unreachable_dead;
  unreachable_dead.added_clades.push_back(fixture.grammar.clades[ab]);
  auto delta = larch::build_spr_overlay_delta(
      fixture.grammar, unreachable_dead);
  CHECK(delta.reachability_stats.reachable_temp_clades == 0);
  CHECK(delta.affected_order.empty());
  auto scored = larch::score_candidate_locally(state, unreachable_dead);
  CHECK(scored.valid);
  CHECK(scored.lower_bound.value.delta == 0);

  larch::grammar_spr_candidate reachable_dead;
  reachable_dead.added_clades.push_back(fixture.grammar.clades[ab]);
  reachable_dead.added_productions.push_back(temp_prod(
      larch::base_clade_ref(fixture.grammar.root_clade),
      {larch::temp_clade_ref(0), larch::base_clade_ref(cd)}));
  auto bad_scored = larch::score_candidate_locally(state, reachable_dead);
  CHECK(!bad_scored.valid);
  CHECK(bad_scored.invalid_reason.find("no available productions") !=
        std::string::npos);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_nonbinary_overlay_production_rejected() {
  std::println("test_nonbinary_overlay_production_rejected");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto a = clade_for(fixture.grammar, {"A"});
  auto b = clade_for(fixture.grammar, {"B"});
  auto cd = clade_for(fixture.grammar, {"C", "D"});

  larch::grammar_spr_candidate candidate;
  candidate.added_productions.push_back(temp_prod(
      larch::base_clade_ref(fixture.grammar.root_clade),
      {larch::base_clade_ref(a), larch::base_clade_ref(b),
       larch::base_clade_ref(cd)}));

  auto scored = larch::score_candidate_locally(state, candidate);
  CHECK(!scored.valid);
  CHECK(scored.invalid_reason.find("arity 3") != std::string::npos);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_overlay_delta_rows_match_full_overlay_for_tiny_candidates() {
  std::println("test_overlay_delta_rows_match_full_overlay_for_tiny_candidates");

  auto fixture = make_fixture();
  auto const& pattern = fixture.patterns.patterns.front();
  larch::leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
  auto base_chart = larch::build_single_site_chart(fixture.grammar, states);

  bool saw_strictly_local_candidate = false;
  for (auto const& candidate : fixture.candidates) {
    auto delta = larch::build_spr_overlay_delta(fixture.grammar, candidate);
    auto overlay = larch::overlay_from_candidate(fixture.grammar, candidate);
    auto materialized = larch::materialize_overlay_grammar(overlay);
    auto full = larch::build_single_site_chart(materialized.grammar, states);
    auto local_rows = larch::build_local_overlay_chart_rows(
        delta, base_chart, states);

    CHECK(delta.reachability_stats.reachable_clades ==
          materialized.grammar.clades.size());
    CHECK(delta.affected_order.size() > 0);
    if (delta.affected_order.size() < materialized.grammar.clades.size()) {
      saw_strictly_local_candidate = true;
    }
    for (std::size_t dense = 0;
         dense < materialized.dense_clade_to_ref.size(); ++dense) {
      auto ref = materialized.dense_clade_to_ref[dense];
      CHECK(larch::local_overlay_chart_row(local_rows, base_chart, ref) ==
            full.inside[dense]);
    }
  }
  CHECK(saw_strictly_local_candidate);

  std::println("  PASS");
}

static void test_local_scoring_verify_option_counts_oracle_materialization() {
  std::println("test_local_scoring_verify_option_counts_oracle_materialization");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  larch::local_spr_score_options opts;
  opts.verify_against_full_overlay = true;
  auto scored = larch::score_candidate_locally(
      state, fixture.candidates.front(), opts);

  CHECK(scored.valid);
  CHECK(state.counters.local_candidate_scores == 1);
  CHECK(state.counters.full_overlay_materializations == 1);
  CHECK(state.counters.overlay_materializations_for_oracle == 1);
  CHECK(state.counters.overlay_materializations_for_local_scoring_bridge == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);

  std::println("  PASS");
}

static void test_invalid_disconnected_overlay_returns_invalid_score() {
  std::println("test_invalid_disconnected_overlay_returns_invalid_score");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  CHECK(!fixture.grammar.productions_by_parent[fixture.grammar.root_clade]
             .empty());
  larch::grammar_spr_candidate bad;
  bad.removed_productions.push_back(larch::base_production_ref(
      fixture.grammar.productions_by_parent[fixture.grammar.root_clade]
          .front()));

  auto scored = larch::score_candidate_locally(state, bad);
  CHECK(!scored.valid);
  CHECK(!scored.invalid_reason.empty());
  CHECK(!scored.lower_bound.value.improves());
  CHECK(scored.lower_bound.value.old_score ==
        state.composite_lower_bound_with_invariants);
  CHECK(scored.lower_bound.value.new_score ==
        state.composite_lower_bound_with_invariants);
  CHECK(state.counters.local_candidate_scores == 1);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);

  std::println("  PASS");
}

static void test_persistent_active_pattern_cache_matches_full_composite() {
  std::println("test_persistent_active_pattern_cache_matches_full_composite");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto full_patterns = larch::build_site_patterns(dag, grammar);
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto active_build = larch::make_active_search_patterns(full_patterns, opts);
  CHECK(active_build.skipped_invariant_site_count == 3);
  CHECK(active_build.invariant_constant_offset == 2);
  active_build.active_patterns.assert_no_skipped_invariant_metadata();

  auto active_composite = larch::build_composite_chart_score_active(
      grammar, active_build.active_patterns, opts);
  auto full_composite = larch::build_composite_chart_score(
      grammar, full_patterns, opts);

  auto state = larch::build_chart_spr_search_state(
      dag, grammar, full_patterns, opts);
  CHECK(state.pattern_charts.size() ==
        active_build.active_patterns.patterns.patterns.size());
  CHECK(state.skipped_invariant_site_count == 3);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(state.composite_lower_bound_without_invariants ==
        active_composite.weighted_lower_bound);
  CHECK(state.composite_lower_bound_with_invariants ==
        full_composite.weighted_lower_bound);
  CHECK(state.composite_lower_bound_with_invariants ==
        state.composite_lower_bound_without_invariants +
            state.invariant_constant_offset);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);

  std::println("  PASS");
}

static void test_root_reference_counts_preserved_in_cache() {
  std::println("test_root_reference_counts_preserved_in_cache");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AC", four_taxon_repeated_reference_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  CHECK(patterns.patterns.size() == 1);
  CHECK(patterns.patterns.front().weight == 2);
  CHECK(patterns.patterns.front()
            .reference_state_counts[larch::nuc_base::A] == 1);
  CHECK(patterns.patterns.front()
            .reference_state_counts[larch::nuc_base::C] == 1);

  larch::chart_options opts;
  opts.score_ua_edge = true;
  auto state = larch::build_chart_spr_search_state(
      dag, grammar, patterns, opts);
  CHECK(state.pattern_charts.size() == 1);
  auto const& entry = state.pattern_charts.front();
  CHECK(entry.reference_state_counts[larch::nuc_base::A] == 1);
  CHECK(entry.reference_state_counts[larch::nuc_base::C] == 1);
  CHECK(entry.root_min_excluding_ua == 1);
  CHECK(entry.weighted_root_score == 3);
  CHECK(entry.weighted_root_score >
        patterns.patterns.front().weight * entry.root_min_excluding_ua);
  CHECK(state.composite_lower_bound_without_invariants == 3);
  CHECK(state.composite_lower_bound_with_invariants == 3);

  std::println("  PASS");
}

static void test_active_pattern_assertions_reject_skipped_metadata() {
  std::println("test_active_pattern_assertions_reject_skipped_metadata");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::site_pattern_options pattern_opts;
  pattern_opts.skip_invariant_sites = true;
  auto skipped = larch::build_site_patterns(dag, grammar, pattern_opts);
  CHECK(skipped.skipped_invariant_site_count == 3);

  larch::active_site_pattern_set bad{skipped};
  bool threw = false;
  try {
    bad.assert_no_skipped_invariant_metadata();
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_state_builder_from_dag_rebuilds_patterns_once() {
  std::println("test_state_builder_from_dag_rebuilds_patterns_once");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto state = larch::build_chart_spr_search_state(dag, grammar, opts);
  CHECK(state.counters.pattern_rebuilds == 1);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);
  CHECK(state.skipped_invariant_site_count == 3);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(larch::chart_spr_pattern_source_fingerprint_matches(
      dag, grammar, state.pattern_source_fingerprint));
  state.active_patterns.assert_no_skipped_invariant_metadata();

  std::println("  PASS");
}

static void test_pattern_batch_cache_options_match_all_cache() {
  std::println("test_pattern_batch_cache_options_match_all_cache");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(candidates.size() >= 2);

  auto all_state = larch::build_chart_spr_search_state(dag, grammar);
  CHECK(all_state.active_patterns.patterns.patterns.size() >= 2);
  CHECK(all_state.cache_strategy ==
        larch::chart_spr_cache_strategy::all_active_patterns);

  larch::chart_spr_search_options options;
  options.cache.max_cached_patterns = 1;
  options.cache.candidate_batch_size = 2;
  auto batched_state = larch::build_chart_spr_search_state(
      dag, grammar, options);
  CHECK(batched_state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);
  CHECK(batched_state.pattern_charts.empty());
  CHECK(batched_state.effective_pattern_batch_size == 1);
  CHECK(batched_state.composite_lower_bound_with_invariants ==
        all_state.composite_lower_bound_with_invariants);

  std::vector<larch::grammar_spr_candidate> subset{
      candidates[0], candidates[1]};
  auto all_scores = larch::score_candidates_locally(all_state, subset, {}, 1);
  auto batched_scores = larch::score_candidates_locally(
      batched_state, subset, {}, 1);
  CHECK(all_scores.size() == batched_scores.size());
  for (std::size_t i = 0; i < all_scores.size(); ++i) {
    CHECK(all_scores[i].valid == batched_scores[i].valid);
    CHECK(all_scores[i].lower_bound.value.old_score ==
          batched_scores[i].lower_bound.value.old_score);
    CHECK(all_scores[i].lower_bound.value.new_score ==
          batched_scores[i].lower_bound.value.new_score);
    CHECK(all_scores[i].lower_bound.value.delta ==
          batched_scores[i].lower_bound.value.delta);
    CHECK(all_scores[i].affected_clade_count ==
          batched_scores[i].affected_clade_count);
  }
  CHECK(batched_state.counters.local_candidate_scores == subset.size());
  CHECK(batched_state.counters.pattern_batch_cache_builds >=
        batched_state.active_patterns.patterns.patterns.size());
  CHECK(batched_scores.front().local_score_ms > 0.0);

  bool single_threw = false;
  try {
    (void)larch::score_candidate_locally(batched_state, subset.front());
  } catch (std::exception const& e) {
    single_threw = true;
    CHECK(std::string{e.what()}.find("score_candidates_locally") !=
          std::string::npos);
  }
  CHECK(single_threw);

  larch::chart_spr_search_options memory_options;
  memory_options.cache.memory_budget_bytes = 1;
  memory_options.cache.candidate_batch_size = 2;
  auto memory_state = larch::build_chart_spr_search_state(
      dag, grammar, memory_options);
  CHECK(memory_state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);
  auto memory_scores = larch::score_candidates_locally(
      memory_state, subset, {}, 1);
  CHECK(memory_scores.size() == all_scores.size());
  for (std::size_t i = 0; i < all_scores.size(); ++i) {
    CHECK(memory_scores[i].lower_bound.value.new_score ==
          all_scores[i].lower_bound.value.new_score);
    CHECK(memory_scores[i].lower_bound.value.delta ==
          all_scores[i].lower_bound.value.delta);
  }

  std::println("  PASS");
}

static void test_parallel_local_scores_match_serial() {
  std::println("test_parallel_local_scores_match_serial");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(candidates.size() >= 2);
  if (candidates.size() > 6) candidates.resize(6);

  auto serial_state = larch::build_chart_spr_search_state(dag, grammar);
  auto parallel_state = larch::build_chart_spr_search_state(dag, grammar);
  auto serial = larch::score_candidates_locally(serial_state, candidates, {}, 1);
  auto parallel = larch::score_candidates_locally(
      parallel_state, candidates, {}, 2);

  CHECK(serial.size() == parallel.size());
  for (std::size_t i = 0; i < serial.size(); ++i) {
    CHECK(serial[i].valid == parallel[i].valid);
    CHECK(serial[i].lower_bound.value.old_score ==
          parallel[i].lower_bound.value.old_score);
    CHECK(serial[i].lower_bound.value.new_score ==
          parallel[i].lower_bound.value.new_score);
    CHECK(serial[i].lower_bound.value.delta ==
          parallel[i].lower_bound.value.delta);
    CHECK(serial[i].affected_clade_count == parallel[i].affected_clade_count);
  }
  CHECK(parallel_state.counters.local_candidate_scores == candidates.size());
  CHECK(parallel_state.counters.local_score_parallel_batches == 1);
  CHECK(parallel_state.counters.local_score_worker_tasks == 2);
  CHECK(parallel_state.counters.local_rows_recomputed ==
        serial_state.counters.local_rows_recomputed);

  larch::chart_spr_search_options batched_options;
  batched_options.cache.max_cached_patterns = 1;
  batched_options.cache.candidate_batch_size = candidates.size();
  auto batched_serial_state = larch::build_chart_spr_search_state(
      dag, grammar, batched_options);
  auto batched_parallel_state = larch::build_chart_spr_search_state(
      dag, grammar, batched_options);
  auto batched_serial = larch::score_candidates_locally(
      batched_serial_state, candidates, {}, 1);
  auto batched_parallel = larch::score_candidates_locally(
      batched_parallel_state, candidates, {}, 2);
  CHECK(batched_serial.size() == batched_parallel.size());
  for (std::size_t i = 0; i < batched_serial.size(); ++i) {
    CHECK(batched_serial[i].valid == batched_parallel[i].valid);
    CHECK(batched_serial[i].lower_bound.value.old_score ==
          batched_parallel[i].lower_bound.value.old_score);
    CHECK(batched_serial[i].lower_bound.value.new_score ==
          batched_parallel[i].lower_bound.value.new_score);
    CHECK(batched_serial[i].lower_bound.value.delta ==
          batched_parallel[i].lower_bound.value.delta);
  }
  CHECK(batched_parallel_state.counters.local_score_parallel_batches == 1);
  CHECK(batched_parallel_state.counters.pattern_batch_cache_builds >=
        batched_parallel_state.active_patterns.patterns.patterns.size());
  CHECK(batched_parallel_state.counters.local_rows_recomputed ==
        batched_serial_state.counters.local_rows_recomputed);

  std::println("  PASS");
}

static void test_pattern_batch_nonreplayable_uses_automatic_candidate_batch() {
  std::println(
      "test_pattern_batch_nonreplayable_uses_automatic_candidate_batch");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.cache.max_cached_patterns = 1;
  auto state = larch::build_chart_spr_search_state(dag, grammar, options);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);

  auto enumeration = options.enumeration;
  enumeration.source = larch::chart_spr_candidate_source::sampled_tree;
  auto automatic_batch_size = larch::chart_spr_effective_candidate_batch_size(
      state, options);
  CHECK(automatic_batch_size != 0);
  larch::validate_chart_spr_pattern_batch_replay_strategy(
      state, options, enumeration, automatic_batch_size);

  options.cache.candidate_batch_size = 2;
  larch::validate_chart_spr_pattern_batch_replay_strategy(
      state, options, enumeration,
      larch::chart_spr_effective_candidate_batch_size(state, options));

  std::println("  PASS");
}

static void test_unchartable_grammar_rejected_with_empty_active_patterns() {
  std::println("test_unchartable_grammar_rejected_with_empty_active_patterns");

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", tiny_inner("root", "A", {tiny_leaf("A", "A"),
                                      tiny_leaf("B", "A")}));
  auto grammar = larch::build_clade_grammar(dag);
  grammar.productions.clear();
  grammar.productions_by_parent.assign(grammar.clades.size(), {});
  grammar.productions_by_child.assign(grammar.clades.size(), {});
  auto patterns = larch::build_site_patterns(dag, grammar);

  bool threw = false;
  try {
    (void)larch::build_chart_spr_search_state(dag, grammar, patterns);
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_unsupported_enumeration_options_fail_explicitly() {
  std::println("test_unsupported_enumeration_options_fail_explicitly");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.include_neutral_or_reversal_candidates = true;

  bool threw = false;
  try {
    (void)larch::for_each_grammar_spr_candidate(
        fixture.grammar, options,
        [](larch::grammar_spr_candidate const&) { return true; });
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_max_affected_estimate_prunes_before_construction() {
  std::println("test_max_affected_estimate_prunes_before_construction");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_estimated_affected_clades = 1;

  std::size_t callbacks = 0;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const&) {
        ++callbacks;
        return true;
      });

  CHECK(callbacks == 0);
  CHECK(stats.candidates_constructed == 0);
  CHECK(stats.candidates_pruned_before_construction > 0);

  std::println("  PASS");
}

static void test_streaming_candidate_cap_stops_before_eager_path_precompute() {
  std::println("test_streaming_candidate_cap_stops_before_eager_path_precompute");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_candidates = 1;

  std::vector<larch::grammar_spr_candidate> streamed;
  auto streaming_stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const& candidate) {
        streamed.push_back(candidate);
        return true;
      });
  auto eager = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options);

  CHECK(streamed.size() == 1);
  CHECK(streaming_stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(streaming_stats.candidates_generated_after_dedup == 1);
  CHECK(streaming_stats.candidates_constructed == 1);
  CHECK(streaming_stats.upward_paths_completed <
        eager.stats.upward_paths_completed);

  std::println("  PASS");
}

static void test_streaming_path_pair_budget_stops_early() {
  std::println("test_streaming_path_pair_budget_stops_early");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_path_pairs_considered = 1;

  std::size_t callbacks = 0;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const&) {
        ++callbacks;
        return true;
      });

  CHECK(stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::path_budget);
  CHECK(stats.path_pairs_considered == 1);
  CHECK(stats.candidates_constructed <= 1);
  CHECK(callbacks <= 1);

  std::println("  PASS");
}

static void test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute() {
  std::println("test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_candidates = 1;
  larch::chart_spr_search_counters counters;
  auto result = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options, &counters);

  CHECK(result.candidates.size() == 1);
  CHECK(result.stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(result.stats.upward_paths_completed > result.candidates.size());
  CHECK(result.stats.upward_path_iterator_steps > 0);
  CHECK(result.stats.candidates_constructed >= result.candidates.size());
  CHECK(counters.candidate_cap_cutoffs == 1);
  CHECK(counters.candidates_generated_after_dedup == result.candidates.size());
  CHECK(counters.upward_paths_completed == result.stats.upward_paths_completed);
  CHECK(counters.path_pairs_considered == result.stats.path_pairs_considered);

  std::println("  PASS");
}

static void test_exact_verification_reuses_state_old_score() {
  std::println("test_exact_verification_reuses_state_old_score");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);
  CHECK(state.exact_trim_active_only.has_value());

  auto local = larch::score_candidate_locally(state, fixture.candidates.front());
  auto exact = larch::verify_candidate_exact_against_state(
      state, local, options.exact_trim);
  auto oracle = larch::score_multisite_spr_candidate_exact_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front());

  CHECK(exact.valid);
  CHECK(exact.exact.has_value());
  CHECK(exact.exact->kind == larch::chart_spr_score_kind::grammar_exact);
  CHECK(exact.exact->convention ==
        larch::chart_spr_score_convention::full_with_invariants);
  CHECK(exact.exact->value.old_score == oracle.old_score);
  CHECK(exact.exact->value.new_score == oracle.new_score);
  CHECK(exact.exact->value.delta == oracle.delta);
  CHECK(state.counters.exact_verifications == 1);
  CHECK(state.counters.full_overlay_materializations == 1);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 1);
  CHECK(state.counters.overlay_materializations_for_oracle == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);

  std::println("  PASS");
}

static void test_top_k_exact_verification_count_is_bounded() {
  std::println("test_top_k_exact_verification_count_is_bounded");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 1;
  options.enumeration.max_candidates = 6;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);

  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidates_scored > 1);
  CHECK(iteration.candidates_exact_verified <= 1);
  CHECK(state.counters.exact_verifications <= 1);
  CHECK(state.counters.overlay_materializations_for_exact_verification ==
        state.counters.exact_verifications);
  CHECK(state.counters.local_candidate_scores == iteration.candidates_scored);
  CHECK(state.counters.full_composite_rebuilds == 0);
  CHECK(iteration.unverified_candidates_may_contain_improvements);
  CHECK(std::string{larch::chart_spr_candidate_selection_mode_name(
            iteration.candidate_selection)} == "lower_bound_top_k");

  std::println("  PASS");
}

static void test_lower_bound_heuristic_acceptance_is_explicit() {
  std::println("test_lower_bound_heuristic_acceptance_is_explicit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.enumeration.max_candidates = 0;
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.local_improving_candidates > 0);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->lower_bound.value.improves());
  CHECK(!iteration.accepted->exact.has_value());
  CHECK(state.counters.exact_verifications == 0);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.accepted_moves == 0);
  CHECK(state.counters.candidate_accepts_attempted == 1);
  CHECK(std::string{larch::chart_spr_acceptance_mode_name(
            iteration.acceptance_mode)} == "lower_bound_heuristic");

  std::println("  PASS");
}

static void test_fixed_topology_exact_rejects_bare_candidate() {
  std::println("test_fixed_topology_exact_rejects_bare_candidate");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto local = larch::score_candidate_locally(state, fixture.candidates.front());
  auto rejected = larch::verify_candidate_fixed_topology_exact(state, local);

  CHECK(!rejected.valid);
  CHECK(rejected.invalid_reason.find("requires") != std::string::npos);
  CHECK(!rejected.exact.has_value());
  CHECK(state.counters.exact_verifications == 0);

  std::println("  PASS");
}

static void test_fixed_topology_iteration_uses_default_selector() {
  std::println("test_fixed_topology_iteration_uses_default_selector");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 4;
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidates_exact_verified > 0);
  CHECK(state.counters.exact_verifications ==
        iteration.candidates_exact_verified);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->value.improves());
  CHECK(iteration.accepted->topology_selection.kind ==
        larch::chart_spr_topology_selection_kind::deterministic_selector);
  CHECK(iteration.accepted->topology_selection.selector_name ==
        "first_reachable_overlay_topology");
  CHECK(iteration.accepted->topology_selection.certificate.has_value());
  CHECK(state.counters.accepted_moves == 0);
  CHECK(state.counters.candidate_accepts_attempted == 1);

  std::println("  PASS");
}

static void test_sampled_tree_fixed_topology_uses_source_certificate() {
  std::println("test_sampled_tree_fixed_topology_uses_source_certificate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.enumeration.source = larch::chart_spr_candidate_source::sampled_tree;
  options.enumeration.max_candidates = 8;
  options.top_k_exact_verify = 8;

  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidates_scored > 0);
  CHECK(iteration.candidates_exact_verified > 0);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->candidate.source_tree_move.has_value());
  CHECK(iteration.accepted->candidate.source_before_topology_productions
            .has_value());
  CHECK(iteration.accepted->candidate.source_after_topology_productions
            .has_value());
  CHECK(iteration.accepted->topology_selection.kind ==
        larch::chart_spr_topology_selection_kind::explicit_certificate);
  CHECK(iteration.accepted->topology_selection.selector_name ==
        "source_tree_move_certificate");
  CHECK(iteration.accepted->topology_selection.certificate.has_value());
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->value.improves());

  std::println("  PASS");
}

static void test_fixed_topology_exact_certificate_scores_selected_topology() {
  std::println("test_fixed_topology_exact_certificate_scores_selected_topology");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);

  std::optional<larch::chart_spr_candidate_score> chosen;
  for (auto const& candidate : candidates) {
    auto local = larch::score_candidate_locally(state, candidate);
    if (local.valid && local.lower_bound.value.improves()) {
      chosen = std::move(local);
      break;
    }
  }
  CHECK(chosen.has_value());

  std::vector<larch::overlay_production_ref> before_refs;
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    before_refs.push_back(larch::base_production_ref(
        static_cast<larch::production_id>(pid)));
  }
  auto overlay = larch::overlay_from_candidate(grammar, chosen->candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  auto after_refs = materialized.dense_production_to_ref;

  chosen->topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  chosen->topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(
          grammar, chosen->candidate, before_refs, after_refs);

  auto fixed = larch::verify_candidate_fixed_topology_exact(state, *chosen);
  CHECK(fixed.valid);
  CHECK(fixed.exact.has_value());
  CHECK(fixed.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(fixed.exact->value.improves());
  CHECK(state.counters.exact_verifications == 1);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 1);

  std::vector<larch::production_id> before_ids;
  for (auto ref : before_refs) before_ids.push_back(ref.id);
  auto before_topology = larch::grammar_topology_from_productions(
      grammar, before_ids);
  std::vector<larch::production_id> after_ids;
  for (std::size_t pid = 0; pid < materialized.grammar.productions.size(); ++pid) {
    after_ids.push_back(static_cast<larch::production_id>(pid));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);
  auto old_score = larch::score_selected_topology(
      grammar, patterns, before_topology);
  auto new_score = larch::score_selected_topology(
      materialized.grammar, patterns, after_topology);
  CHECK(fixed.exact->value.old_score == old_score);
  CHECK(fixed.exact->value.new_score == new_score);

  std::println("  PASS");
}

static void test_enumeration_truncation_sets_unverified_flag_even_exhaustive() {
  std::println("test_enumeration_truncation_sets_unverified_flag_even_exhaustive");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.enumeration.max_candidates = 1;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidate_generation.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(iteration.candidates_scored == 1);
  CHECK(iteration.candidates_exact_verified == 1);
  CHECK(iteration.unverified_candidates_may_contain_improvements);

  std::println("  PASS");
}

static void test_phase5_no_improvement_search_stops_without_commit() {
  std::println("test_phase5_no_improvement_search_stops_without_commit");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 3;

  auto initial_nodes = larch::node_count(fixture.dag);
  auto initial_edges = larch::edge_count(fixture.dag);
  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  CHECK(search.iterations.size() == 1);
  CHECK(!search.iterations.front().accepted_move_committed);
  CHECK(search.counters.accepted_moves == 0);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.initial_search_state_rebuilds == 1);
  CHECK(search.summary.full_search_state_rebuilds == 1);
  CHECK(search.summary.final_compaction_rebuilds == 0);
  CHECK(search.counters.local_candidate_scores ==
        search.iterations.front().candidates_scored);
  CHECK(larch::node_count(search.dag) == initial_nodes);
  CHECK(larch::edge_count(search.dag) == initial_edges);

  std::println("  PASS");
}

static void test_phase5_known_improving_search_commits_once() {
  std::println("test_phase5_known_improving_search_commits_once");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;

  auto taxon_count = grammar.taxa.id_to_sample_id.size();
  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().post_materialization_rejected);
  CHECK(search.iterations.front().reused_patterns_after_accept);
  CHECK(search.counters.candidate_accepts_attempted == 1);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.overlay_materializations_for_accept_materialization == 1);
  CHECK(search.summary.initial_search_state_rebuilds == 1);
  CHECK(search.summary.full_search_state_rebuilds == 2);
  CHECK(search.summary.final_compaction_rebuilds == 0);
  CHECK(search.summary.final_score <= search.summary.initial_score);
  CHECK(search.iterations.front().state_score_after <=
        search.iterations.front().state_score_before);
  CHECK(search.summary.local_scoring_ms >=
        search.iterations.front().local_scoring_ms);
  CHECK(search.summary.exact_verification_ms >=
        search.iterations.front().exact_verification_ms);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  CHECK(rebuilt.taxa.id_to_sample_id.size() == taxon_count);
  auto const& accepted = *search.iterations.front().accepted;
  CHECK(!accepted.candidate.added_productions.empty());
  for (std::size_t pid = 0; pid < accepted.candidate.added_productions.size();
       ++pid) {
    auto signature = larch::chart_spr_production_signature_for_ref(
        grammar, accepted.candidate,
        larch::temp_production_ref(static_cast<larch::production_id>(pid)));
    CHECK(grammar_contains_production_signature(rebuilt, signature));
  }

  std::println("  PASS");
}

static void test_phase9_known_improving_search_uses_local_accept_update() {
  std::println("test_phase9_known_improving_search_uses_local_accept_update");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_local_against_full_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().post_materialization_rejected);
  CHECK(search.iterations.front().reused_patterns_after_accept);
  CHECK(search.counters.candidate_accepts_attempted == 1);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.initial_search_state_rebuilds == 1);
  CHECK(search.summary.full_search_state_rebuilds == 1);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_ms > 0.0);
  CHECK(search.summary.final_score <= search.summary.initial_score);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  CHECK(rebuilt.clades.size() > 0);
  CHECK(search.summary.final_grammar_clade_count == rebuilt.clades.size());
  CHECK(search.summary.final_grammar_production_count ==
        rebuilt.productions.size());
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase9_final_compaction_mismatch_fails_clearly() {
  std::println("test_phase9_final_compaction_mismatch_fails_clearly");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.override_final_compaction_rebuilt_score_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = std::string{e.what()}.find("final compaction") !=
            std::string::npos;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_phase9_pattern_batch_local_update_matches_output_dag() {
  std::println("test_phase9_pattern_batch_local_update_matches_output_dag");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AA", four_taxon_two_site_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_local_against_full_for_tests = true;
  options.cache.pattern_batch_size = 1;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.effective_pattern_batch_size == 1);
  CHECK(search.counters.pattern_batch_cache_builds > 0);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase9_fixed_topology_compaction_uses_certificate() {
  std::println("test_phase9_fixed_topology_compaction_uses_certificate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.iterations.front().accepted->exact.has_value());
  CHECK(search.summary.final_score ==
        search.iterations.front().accepted->exact->value.new_score);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(larch::build_clade_grammar(search.dag).clades.size() > 0);

  std::println("  PASS");
}

static void test_phase9_lower_bound_compaction_matches_output_dag() {
  std::println("test_phase9_lower_bound_compaction_matches_output_dag");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(rebuilt_state.composite_lower_bound_with_invariants ==
        search.summary.final_score);

  std::println("  PASS");
}

static void test_phase9_multi_iteration_local_updates_match_output_dag() {
  std::println("test_phase9_multi_iteration_local_updates_match_output_dag");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", six_taxon_paired_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 32;
  options.max_iterations = 3;
  options.rebuild_after_accept = false;
  options.verify_local_against_full_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.counters.accepted_moves >= 2);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase5_rejected_candidates_do_not_rebuild_sidecar() {
  std::println("test_phase5_rejected_candidates_do_not_rebuild_sidecar");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.max_iterations = 1;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  CHECK(search.counters.local_candidate_scores ==
        search.iterations.front().candidates_scored);
  CHECK(search.counters.full_composite_rebuilds == 0);
  CHECK(search.counters.sidecar_rebuilds_after_accept ==
        search.counters.accepted_moves);
  if (!search.iterations.front().accepted_move_committed) {
    CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  }

  std::println("  PASS");
}

static void test_phase5_post_materialization_worsening_rejects_commit() {
  std::println("test_phase5_post_materialization_worsening_rejects_commit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto initial_nodes = larch::node_count(dag);
  auto initial_edges = larch::edge_count(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.override_post_materialization_rebuilt_score_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(!iteration.accepted_move_committed);
  CHECK(iteration.post_materialization_rejected);
  CHECK(iteration.post_materialization_rebuilt_score ==
        std::numeric_limits<std::uint64_t>::max());
  CHECK(search.counters.candidate_accepts_attempted == 1);
  CHECK(search.counters.accepted_moves == 0);
  CHECK(search.counters.post_materialization_rejections == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.overlay_materializations_for_accept_materialization == 1);
  CHECK(search.summary.full_search_state_rebuilds == 2);
  CHECK(search.summary.final_score == search.summary.initial_score);
  CHECK(larch::node_count(search.dag) == initial_nodes);
  CHECK(larch::edge_count(search.dag) == initial_edges);

  std::println("  PASS");
}

static void
    test_phase5_fixed_topology_mode_commits_with_rebuilt_certificate_gate() {
  std::println(
      "test_phase5_fixed_topology_mode_commits_with_rebuilt_certificate_gate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().post_materialization_rejected);
  CHECK(search.iterations.front().accepted->exact.has_value());
  CHECK(search.iterations.front().accepted->exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.iterations.front().state_score_after <=
        search.iterations.front().state_score_before);
  CHECK(search.summary.final_grammar_clade_count > 0);
  CHECK(search.summary.final_grammar_production_count > 0);

  std::println("  PASS");
}

static void test_phase5_pattern_fingerprint_mismatch_rebuilds_patterns() {
  std::println("test_phase5_pattern_fingerprint_mismatch_rebuilds_patterns");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.force_pattern_fingerprint_mismatch_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().reused_patterns_after_accept);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.pattern_rebuilds == 2);

  std::println("  PASS");
}

static void test_phase5_seeded_multi_iteration_is_deterministic() {
  std::println("test_phase5_seeded_multi_iteration_is_deterministic");

  auto run_once = [] {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", four_taxon_misplaced_tree());
    auto grammar = larch::build_clade_grammar(dag);

    larch::chart_spr_search_options options;
    options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.top_k_exact_verify = 8;
    options.max_iterations = 2;
    options.seed = 12345;
    return larch::run_chart_spr_search(std::move(dag), grammar, options);
  };

  auto first = run_once();
  auto second = run_once();

  CHECK(first.iterations.size() == 2);
  CHECK(first.iterations.size() == second.iterations.size());
  CHECK(first.summary.accepted_moves == second.summary.accepted_moves);
  CHECK(first.summary.initial_score == second.summary.initial_score);
  CHECK(first.summary.final_score == second.summary.final_score);
  CHECK(larch::node_count(first.dag) == larch::node_count(second.dag));
  CHECK(larch::edge_count(first.dag) == larch::edge_count(second.dag));

  std::size_t max_iter_affected = 0;
  std::vector<std::size_t> aggregate_affected_counts;
  for (std::size_t i = 0; i < first.iterations.size(); ++i) {
    auto const& a = first.iterations[i];
    auto const& b = second.iterations[i];
    CHECK(a.candidates_generated == b.candidates_generated);
    CHECK(a.candidates_scored == b.candidates_scored);
    CHECK(a.candidates_exact_verified == b.candidates_exact_verified);
    CHECK(a.accepted.has_value() == b.accepted.has_value());
    CHECK(a.accepted_move_committed == b.accepted_move_committed);
    CHECK(a.state_score_before == b.state_score_before);
    CHECK(a.state_score_after == b.state_score_after);
    if (a.accepted) {
      CHECK(a.accepted->lower_bound.value.delta ==
            b.accepted->lower_bound.value.delta);
      CHECK(a.accepted->exact.has_value() == b.accepted->exact.has_value());
      if (a.accepted->exact) {
        CHECK(a.accepted->exact->value.delta ==
              b.accepted->exact->value.delta);
      }
    }
    max_iter_affected = std::max(max_iter_affected,
                                 a.affected_distribution.max);
    aggregate_affected_counts.insert(aggregate_affected_counts.end(),
                                     a.affected_clade_counts.begin(),
                                     a.affected_clade_counts.end());
  }
  auto aggregate_affected = larch::summarize_affected_clade_counts(
      aggregate_affected_counts);
  CHECK(first.summary.affected_distribution.mean == aggregate_affected.mean);
  CHECK(first.summary.affected_distribution.p50 == aggregate_affected.p50);
  CHECK(first.summary.affected_distribution.p95 == aggregate_affected.p95);
  CHECK(first.summary.affected_distribution.max == max_iter_affected);
  CHECK(first.summary.affected_distribution.max == aggregate_affected.max);
  CHECK(first.summary.local_scoring_ms >= 0.0);
  CHECK(first.summary.exact_verification_ms >= 0.0);

  std::println("  PASS");
}

static void test_exhaustive_exact_acceptance_matches_oracle() {
  std::println("test_exhaustive_exact_acceptance_matches_oracle");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.enumeration.max_candidates = 0;
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  auto candidates = larch::enumerate_grammar_spr_candidates(
      grammar, options.enumeration);
  std::optional<larch::spr_score_result> best_oracle;
  for (auto const& candidate : candidates) {
    auto oracle = larch::score_multisite_spr_candidate_exact_oracle(
        grammar, patterns, candidate);
    if (!oracle.improves()) continue;
    if (!best_oracle || oracle.new_score < best_oracle->new_score ||
        (oracle.new_score == best_oracle->new_score &&
         oracle.delta < best_oracle->delta)) {
      best_oracle = oracle;
    }
  }

  CHECK(iteration.candidates_exact_verified == iteration.candidates_scored);
  CHECK(state.counters.exact_verifications == iteration.candidates_scored);
  CHECK(iteration.unverified_candidates_may_contain_improvements == false);
  if (best_oracle) {
    CHECK(iteration.accepted.has_value());
    CHECK(iteration.accepted->exact.has_value());
    CHECK(iteration.accepted->exact->value.new_score ==
          best_oracle->new_score);
    CHECK(iteration.accepted->exact->value.delta == best_oracle->delta);
  } else {
    CHECK(!iteration.accepted.has_value());
  }

  std::println("  PASS");
}

int main() {
  test_lower_bound_oracle_counters_show_full_rebuild_cost();
  test_local_rejected_candidate_counter_guardrail();
  test_streaming_and_eager_candidate_apis_match();
  test_search_state_local_score_entry_point_matches_oracle();
  test_local_score_with_ua_edge_and_invariant_offset_matches_oracle();
  test_production_delta_only_candidate_metadata_absent_matches_oracle();
  test_multiparent_dag_affected_closure_and_slot_maps();
  test_unreachable_dead_clades_ignored_reachable_dead_clades_fail();
  test_nonbinary_overlay_production_rejected();
  test_overlay_delta_rows_match_full_overlay_for_tiny_candidates();
  test_local_scoring_verify_option_counts_oracle_materialization();
  test_invalid_disconnected_overlay_returns_invalid_score();
  test_persistent_active_pattern_cache_matches_full_composite();
  test_root_reference_counts_preserved_in_cache();
  test_active_pattern_assertions_reject_skipped_metadata();
  test_state_builder_from_dag_rebuilds_patterns_once();
  test_pattern_batch_cache_options_match_all_cache();
  test_parallel_local_scores_match_serial();
  test_pattern_batch_nonreplayable_uses_automatic_candidate_batch();
  test_unchartable_grammar_rejected_with_empty_active_patterns();
  test_unsupported_enumeration_options_fail_explicitly();
  test_max_affected_estimate_prunes_before_construction();
  test_streaming_candidate_cap_stops_before_eager_path_precompute();
  test_streaming_path_pair_budget_stops_early();
  test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute();
  test_exact_verification_reuses_state_old_score();
  test_top_k_exact_verification_count_is_bounded();
  test_lower_bound_heuristic_acceptance_is_explicit();
  test_fixed_topology_exact_rejects_bare_candidate();
  test_fixed_topology_iteration_uses_default_selector();
  test_sampled_tree_fixed_topology_uses_source_certificate();
  test_fixed_topology_exact_certificate_scores_selected_topology();
  test_enumeration_truncation_sets_unverified_flag_even_exhaustive();
  test_phase5_no_improvement_search_stops_without_commit();
  test_phase5_known_improving_search_commits_once();
  test_phase9_known_improving_search_uses_local_accept_update();
  test_phase9_final_compaction_mismatch_fails_clearly();
  test_phase9_pattern_batch_local_update_matches_output_dag();
  test_phase9_fixed_topology_compaction_uses_certificate();
  test_phase9_lower_bound_compaction_matches_output_dag();
  test_phase9_multi_iteration_local_updates_match_output_dag();
  test_phase5_rejected_candidates_do_not_rebuild_sidecar();
  test_phase5_post_materialization_worsening_rejects_commit();
  test_phase5_fixed_topology_mode_commits_with_rebuilt_certificate_gate();
  test_phase5_pattern_fingerprint_mismatch_rebuilds_patterns();
  test_phase5_seeded_multi_iteration_is_deterministic();
  test_exhaustive_exact_acceptance_matches_oracle();
  std::println("chart_spr_search_test PASS");
  return 0;
}

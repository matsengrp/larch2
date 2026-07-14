#include <larch/build_fasta_newick.hpp>
#include <larch/chart_spr_search.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/overlay_chain_compaction.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

static larch::test::tiny_tree_node phase5_arity3_selected_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_leaf("A", "AA"), tiny_leaf("B", "AC"),
       tiny_inner("CD", "CA", {tiny_leaf("C", "CA"),
                                  tiny_leaf("D", "CC")})});
}

static larch::test::tiny_tree_node phase5_arity4_selected_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner("root", "AA",
                    {tiny_leaf("A", "AA"), tiny_leaf("B", "AC"),
                     tiny_leaf("C", "CA"), tiny_leaf("D", "CC")});
}

static larch::test::tiny_tree_node phase6_arity3_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_leaf("B", "A"), tiny_leaf("D", "C")});
}

static larch::test::tiny_tree_node phase6_internal_arity3_source_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABC", "A",
                  {tiny_leaf("A", "A"), tiny_leaf("B", "A"),
                   tiny_leaf("C", "C")}),
       tiny_leaf("D", "C")});
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

static std::vector<larch::overlay_production_ref> all_base_production_refs(
    larch::clade_grammar const& grammar) {
  std::vector<larch::overlay_production_ref> refs;
  refs.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    refs.push_back(larch::base_production_ref(
        static_cast<larch::production_id>(pid)));
  }
  return refs;
}

static std::size_t max_production_arity(
    larch::clade_grammar const& grammar) {
  std::size_t max_arity = 0;
  for (auto const& prod : grammar.productions) {
    max_arity = std::max(max_arity, prod.children.size());
  }
  return max_arity;
}

static larch::chart_cost phase5_brute_add(larch::chart_cost lhs,
                                           larch::chart_cost rhs) {
  if (lhs >= larch::chart_inf || rhs >= larch::chart_inf) {
    return larch::chart_inf;
  }
  if (lhs > larch::chart_inf - rhs) return larch::chart_inf;
  return lhs + rhs;
}

static larch::chart_multisite_detail::chart_row phase5_brute_inf_row() {
  auto row = larch::chart_multisite_detail::chart_row{};
  row.fill(larch::chart_inf);
  return row;
}

static larch::chart_multisite_detail::chart_row
phase5_brute_combine_rows(
    std::vector<larch::chart_multisite_detail::chart_row> const& children) {
  CHECK(!children.empty());
  auto row = phase5_brute_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
       ++parent_state) {
    larch::chart_cost total = 0;
    for (auto const& child : children) {
      larch::chart_cost best_child = larch::chart_inf;
      for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
           ++child_state) {
        auto term = phase5_brute_add(
            child[child_state],
            larch::parsimony_chart_detail::transition_cost(parent_state,
                                                           child_state));
        best_child = std::min(best_child, term);
      }
      total = phase5_brute_add(total, best_child);
    }
    row[parent_state] = total;
  }
  return row;
}

static larch::chart_multisite_detail::chart_row
phase5_brute_selected_topology_row_impl(
    larch::clade_grammar const& grammar, larch::site_pattern const& pattern,
    larch::grammar_topology const& topology, larch::clade_id clade,
    std::vector<std::optional<larch::chart_multisite_detail::chart_row>>&
        memo) {
  if (memo[clade].has_value()) return *memo[clade];

  auto row = phase5_brute_inf_row();
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    auto taxon = key.taxa.front();
    CHECK(taxon < pattern.state_by_taxon.size());
    auto observed = pattern.state_by_taxon[taxon];
    larch::parsimony_chart_detail::validate_state(
        observed, "phase5 brute selected topology leaf");
    row[observed] = 0;
  } else {
    auto pid = topology.selected_production_by_clade[clade];
    CHECK(pid != larch::no_production);
    CHECK(pid < grammar.productions.size());
    auto const& prod = grammar.productions[pid];
    CHECK(prod.parent == clade);
    CHECK(prod.children.size() >= 2);
    std::vector<larch::chart_multisite_detail::chart_row> child_rows;
    child_rows.reserve(prod.children.size());
    for (auto child : prod.children) {
      child_rows.push_back(phase5_brute_selected_topology_row_impl(
          grammar, pattern, topology, child, memo));
    }
    row = phase5_brute_combine_rows(child_rows);
  }

  memo[clade] = row;
  return row;
}

static larch::chart_multisite_detail::chart_row
phase5_brute_selected_topology_row(
    larch::clade_grammar const& grammar, larch::site_pattern const& pattern,
    larch::grammar_topology const& topology) {
  std::vector<std::optional<larch::chart_multisite_detail::chart_row>> memo(
      grammar.clades.size());
  return phase5_brute_selected_topology_row_impl(
      grammar, pattern, topology, grammar.root_clade, memo);
}

static larch::chart_spr_search_state make_phase5_direct_state(
    larch::phylo_dag& dag, larch::clade_grammar grammar) {
  auto active_build = larch::make_active_search_patterns(dag, grammar);
  CHECK(!active_build.active_patterns.patterns.patterns.empty());
  larch::chart_spr_search_state state;
  state.dag = &dag;
  state.grammar = std::move(grammar);
  state.active_patterns = std::move(active_build.active_patterns);
  state.pattern_source_fingerprint =
      std::move(active_build.pattern_source_fingerprint);
  state.invariant_constant_offset = active_build.invariant_constant_offset;
  state.skipped_invariant_site_count =
      active_build.skipped_invariant_site_count;
  return state;
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

  larch::chart_spr_search_options lazy_options;
  lazy_options.chart = opts;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      dag, grammar, lazy_options);
  auto lazy_scored =
      larch::score_candidate_locally(lazy_state, candidates.front());
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.counters.lazy_inside_rows_computed > 0);
  CHECK(lazy_scored.valid);
  CHECK(lazy_scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(lazy_scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(lazy_scored.lower_bound.value.delta == oracle.delta);
  CHECK(lazy_scored.lower_bound.invariant_offset_applied ==
        lazy_state.invariant_constant_offset);
  CHECK(lazy_state.invariant_constant_offset == 2);
  CHECK(lazy_state.counters.local_rows_recomputed > 0);

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

static void test_nonbinary_overlay_production_scored_locally() {
  std::println("test_nonbinary_overlay_production_scored_locally");

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

  auto const counters_before = state.counters;
  auto scored = larch::score_candidate_locally(state, candidate);
  CHECK(scored.valid);
  CHECK(scored.invalid_reason.empty());
  CHECK(scored.affected_clade_count > 0);
  auto const active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  CHECK(active_pattern_count > 0);
  CHECK(state.counters.local_leaf_state_view_uses ==
        counters_before.local_leaf_state_view_uses + active_pattern_count);
  CHECK(state.counters.local_leaf_state_owned_copies ==
        counters_before.local_leaf_state_owned_copies);
  CHECK(state.counters.local_row_scratch_capacity_growths ==
        counters_before.local_row_scratch_capacity_growths + 1);
  CHECK(state.counters.multifurcation_productions_scored >
        counters_before.multifurcation_productions_scored);
  CHECK(state.counters.full_overlay_materializations == 0);

  auto delta = larch::build_spr_overlay_delta(fixture.grammar, candidate);
  auto overlay = larch::overlay_from_candidate(fixture.grammar, candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  for (auto const& pattern : fixture.patterns.patterns) {
    larch::leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
    auto base_chart = larch::build_single_site_chart(fixture.grammar, states);
    auto local_rows = larch::build_local_overlay_chart_rows(
        delta, base_chart, states);
    larch::verify_local_overlay_rows_against_full(
        delta, local_rows, base_chart, materialized, states,
        larch::chart_options{});
  }

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
  auto patterns = larch::build_site_patterns(dag, grammar);
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
  auto expected_candidate_partitions = std::size_t{0};
  auto expected_candidate_descriptors = std::size_t{0};
  for (auto const& candidate : subset) {
    auto delta = larch::build_spr_overlay_delta(
        all_state.grammar, all_state.execution_plan, candidate);
    CHECK(delta.base == &all_state.grammar);
    CHECK(delta.base_plan_generation ==
          all_state.execution_plan.grammar_generation());
    CHECK(delta.base_plan_fingerprint == all_state.execution_plan.fingerprint());
    CHECK(delta.compiled_rows.size() == delta.affected_order.size());
    CHECK(delta.candidate_plan_build_stats.candidate_partition_validations ==
          candidate.added_productions.size());
    CHECK(delta.candidate_plan_build_stats.clade_order_sorts == 1);
    CHECK(delta.candidate_plan_build_stats.production_descriptors_compiled ==
          delta.compiled_productions.size());
    expected_candidate_partitions += candidate.added_productions.size();
    expected_candidate_descriptors += delta.compiled_productions.size();
  }

  auto all_counters_before = all_state.counters;
  auto all_scores = larch::score_candidates_locally(all_state, subset, {}, 1);
  auto batched_counters_before = batched_state.counters;
  auto batched_scores = larch::score_candidates_locally(
      batched_state, subset, {}, 1);
  larch::chart_spr_search_options lazy_options;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      dag, grammar, lazy_options);
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.pattern_charts.empty());
  CHECK(lazy_state.lazy_chart.has_value());
  CHECK(lazy_state.composite_lower_bound_with_invariants ==
        all_state.composite_lower_bound_with_invariants);
  auto lazy_counters_before = lazy_state.counters;
  auto lazy_scores = larch::score_candidates_locally(lazy_state, subset, {}, 1);
  CHECK(all_scores.size() == batched_scores.size());
  CHECK(all_scores.size() == lazy_scores.size());
  for (std::size_t i = 0; i < all_scores.size(); ++i) {
    auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
        grammar, patterns, subset[i]);
    CHECK(all_scores[i].valid);
    CHECK(batched_scores[i].valid);
    CHECK(lazy_scores[i].valid);
    CHECK(all_scores[i].valid == batched_scores[i].valid);
    CHECK(all_scores[i].lower_bound.value.old_score ==
          batched_scores[i].lower_bound.value.old_score);
    CHECK(all_scores[i].lower_bound.value.new_score ==
          batched_scores[i].lower_bound.value.new_score);
    CHECK(all_scores[i].lower_bound.value.delta ==
          batched_scores[i].lower_bound.value.delta);
    CHECK(all_scores[i].affected_clade_count ==
          batched_scores[i].affected_clade_count);
    CHECK(all_scores[i].valid == lazy_scores[i].valid);
    CHECK(all_scores[i].lower_bound.value.old_score ==
          lazy_scores[i].lower_bound.value.old_score);
    CHECK(all_scores[i].lower_bound.value.new_score ==
          lazy_scores[i].lower_bound.value.new_score);
    CHECK(all_scores[i].lower_bound.value.delta ==
          lazy_scores[i].lower_bound.value.delta);
    CHECK(all_scores[i].affected_clade_count ==
          lazy_scores[i].affected_clade_count);
    CHECK(all_scores[i].lower_bound.value.old_score == oracle.old_score);
    CHECK(all_scores[i].lower_bound.value.new_score == oracle.new_score);
    CHECK(all_scores[i].lower_bound.value.delta == oracle.delta);
  }

  auto const active_pattern_count =
      all_state.active_patterns.patterns.patterns.size();
  for (auto const& score : all_scores) {
    CHECK(score.valid);
    CHECK(score.affected_clade_count > 0);
  }
  auto const observed_row_scratch_growths =
      all_state.counters.local_row_scratch_capacity_growths -
      all_counters_before.local_row_scratch_capacity_growths;
  // reserve(n) may allocate more than n, so the number of capacity changes is
  // deliberately observed rather than inferred from record-high row counts.
  CHECK(observed_row_scratch_growths > 0);
  CHECK(observed_row_scratch_growths <= subset.size());
  auto const expected_leaf_state_views =
      subset.size() * active_pattern_count;
  CHECK(expected_leaf_state_views > 0);
  auto check_candidate_plan_counters =
      [&](auto const& state, auto const& before,
          std::size_t pattern_chart_plan_hits_during_scoring,
          std::size_t expected_leaf_views,
          std::size_t expected_scratch_growths) {
    CHECK(state.active_patterns.patterns.patterns.size() ==
          active_pattern_count);
    CHECK(state.counters.chart_execution_plan_builds ==
          before.chart_execution_plan_builds);
    CHECK(state.counters.chart_execution_plan_cache_hits ==
          before.chart_execution_plan_cache_hits + subset.size() +
              pattern_chart_plan_hits_during_scoring);
    CHECK(state.counters.candidate_execution_plan_builds ==
          before.candidate_execution_plan_builds + subset.size());
    CHECK(state.counters.candidate_execution_plan_cache_hits ==
          before.candidate_execution_plan_cache_hits +
              subset.size() * active_pattern_count);
    CHECK(state.counters.candidate_partition_validations ==
          before.candidate_partition_validations +
              expected_candidate_partitions);
    CHECK(state.counters.clade_order_sorts ==
          before.clade_order_sorts + subset.size());
    CHECK(state.counters.production_descriptors_compiled ==
          before.production_descriptors_compiled +
              expected_candidate_descriptors);
    CHECK(state.counters.plan_mismatch_rejections ==
          before.plan_mismatch_rejections);
    CHECK(state.counters.candidate_pattern_full_grammar_validations ==
          before.candidate_pattern_full_grammar_validations);
    CHECK(state.counters.candidate_pattern_partition_validations ==
          before.candidate_pattern_partition_validations);
    CHECK(state.counters.candidate_pattern_clade_order_sorts ==
          before.candidate_pattern_clade_order_sorts);
    CHECK(state.counters.full_grammar_validations ==
          before.full_grammar_validations);
    CHECK(state.counters.production_partition_validations ==
          before.production_partition_validations);
    CHECK(state.counters.dynamic_overlay_payload_partition_validations ==
          before.dynamic_overlay_payload_partition_validations);
    CHECK(state.counters.local_candidate_scores ==
          before.local_candidate_scores + subset.size());
    CHECK(active_pattern_count >= 2);
    CHECK(state.counters.local_rows_recomputed >
          before.local_rows_recomputed);
    CHECK(state.counters.local_leaf_state_view_uses ==
          before.local_leaf_state_view_uses + expected_leaf_views);
    CHECK(state.counters.local_leaf_state_owned_copies ==
          before.local_leaf_state_owned_copies);
    CHECK(state.counters.local_row_scratch_capacity_growths ==
          before.local_row_scratch_capacity_growths +
              expected_scratch_growths);
  };
  check_candidate_plan_counters(
      all_state, all_counters_before, 0, expected_leaf_state_views,
      observed_row_scratch_growths);
  check_candidate_plan_counters(batched_state, batched_counters_before,
                                active_pattern_count,
                                expected_leaf_state_views,
                                observed_row_scratch_growths);
  check_candidate_plan_counters(lazy_state, lazy_counters_before, 0, 0, 0);
  CHECK(batched_state.counters.local_candidate_scores == subset.size());
  CHECK(batched_state.counters.pattern_batch_cache_builds >=
        batched_state.active_patterns.patterns.patterns.size());
  CHECK(batched_scores.front().local_score_ms > 0.0);
  CHECK(lazy_state.counters.local_candidate_scores == subset.size());
  CHECK(lazy_state.counters.local_rows_recomputed > 0);
  CHECK(lazy_scores.front().local_score_ms > 0.0);

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

static void test_candidate_execution_plan_lifetime_and_mismatch_guards() {
  std::println(
      "test_candidate_execution_plan_lifetime_and_mismatch_guards");

  {
    auto fixture = make_fixture();
    auto state = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    auto source = std::make_unique<larch::grammar_spr_candidate>(
        fixture.candidates.front());
    auto signature = larch::chart_spr_candidate_taxon_signature(
        state.grammar, *source);
    larch::chart_spr_search_counters counters;
    auto prepared =
        larch::chart_spr_search_detail::prepare_local_candidate_score(
            state, *source, {}, &counters);
    CHECK(prepared.valid_for_accumulation);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);
    CHECK(counters.plan_mismatch_rejections == 0);
    source.reset();

    auto broken_entries = state.pattern_charts;
    CHECK(!broken_entries.empty());
    broken_entries.front().chart.inside.clear();
    auto builds_before = counters.candidate_execution_plan_builds;
    auto plan_hits_before = counters.chart_execution_plan_cache_hits;
    larch::chart_spr_local_score_scratch scratch;
    larch::chart_spr_search_detail::accumulate_prepared_local_candidate_patterns(
        state, prepared, 0, broken_entries, {}, &counters, scratch);
    CHECK(!prepared.valid_for_accumulation);
    CHECK(larch::chart_spr_candidate_taxon_signature(
              state.grammar, prepared.scored.candidate) == signature);
    CHECK(counters.candidate_execution_plan_builds == builds_before);
    CHECK(counters.chart_execution_plan_cache_hits == plan_hits_before);
  }

  {
    auto fixture = make_fixture();
    auto resident = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    std::optional<
        larch::chart_spr_search_detail::prepared_local_candidate_score>
        prepared;
    larch::chart_spr_search_counters counters;
    {
      auto source = larch::build_chart_spr_search_state(
          fixture.dag, fixture.grammar, fixture.patterns);
      CHECK(source.grammar.execution_generation ==
            resident.grammar.execution_generation);
      CHECK(source.execution_plan.fingerprint() ==
            resident.execution_plan.fingerprint());
      prepared.emplace(
          larch::chart_spr_search_detail::prepare_local_candidate_score(
              source, fixture.candidates.front(), {}, &counters));
      CHECK(prepared->valid_for_accumulation);
      CHECK(prepared->delta().base == nullptr);
    }

    // The source state is now dead.  The published descriptor owns its raw and
    // compiled arrays, retains no grammar pointer, and consumes the separately
    // checked equivalent resident snapshot without rebinding shared state.
    auto rows_before = counters.local_rows_recomputed;
    larch::chart_spr_local_score_scratch scratch;
    larch::chart_spr_search_detail::accumulate_prepared_local_candidate_patterns(
        resident, *prepared, 0, resident.pattern_charts, {}, &counters,
        scratch);
    CHECK(prepared->valid_for_accumulation);
    CHECK(prepared->delta().base == nullptr);
    CHECK(counters.plan_mismatch_rejections == 0);
    CHECK(counters.local_rows_recomputed > rows_before);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);
    CHECK(counters.candidate_execution_plan_cache_hits ==
          resident.active_patterns.patterns.patterns.size());
    auto scored =
        larch::chart_spr_search_detail::finish_prepared_local_candidate_score(
            resident, *prepared);
    CHECK(scored.valid);

    // A published descriptor is a shared, const recurrence input.  Exercise
    // the intended future pattern-axis use directly: several workers consume
    // the same descriptor and resident base while owning all row scratch.
    CHECK(!resident.pattern_charts.empty());
    auto const& shared_descriptor = prepared->delta();
    auto descriptor_snapshot = shared_descriptor;
    larch::leaf_site_states leaf_states{
        .state_by_taxon = resident.active_patterns.patterns.patterns.front()
                              .state_by_taxon};
    auto serial_rows = larch::build_local_overlay_chart_rows(
        resident.grammar, shared_descriptor,
        resident.pattern_charts.front().chart, leaf_states);
    std::vector<std::future<larch::local_overlay_chart_rows>> futures;
    for (std::size_t worker = 0; worker < 4; ++worker) {
      futures.push_back(std::async(std::launch::async, [&] {
        return larch::build_local_overlay_chart_rows(
            resident.grammar, shared_descriptor,
            resident.pattern_charts.front().chart, leaf_states);
      }));
    }
    for (auto& future : futures) {
      auto rows = future.get();
      CHECK(rows.rows == serial_rows.rows);
      CHECK(rows.base_row_slot.size() == serial_rows.base_row_slot.size());
      CHECK(rows.temp_row_slot.size() == serial_rows.temp_row_slot.size());
    }
    CHECK(shared_descriptor.base == nullptr);
    CHECK(shared_descriptor.temp_clades == descriptor_snapshot.temp_clades);
    CHECK(shared_descriptor.temp_productions.size() ==
          descriptor_snapshot.temp_productions.size());
    for (std::size_t i = 0; i < shared_descriptor.temp_productions.size();
         ++i) {
      CHECK(shared_descriptor.temp_productions[i].parent ==
            descriptor_snapshot.temp_productions[i].parent);
      CHECK(shared_descriptor.temp_productions[i].children ==
            descriptor_snapshot.temp_productions[i].children);
      CHECK(shared_descriptor.temp_productions[i].multiplicity ==
            descriptor_snapshot.temp_productions[i].multiplicity);
    }
    CHECK(shared_descriptor.affected_order ==
          descriptor_snapshot.affected_order);
    CHECK(shared_descriptor.affected_base_row_slot ==
          descriptor_snapshot.affected_base_row_slot);
    CHECK(shared_descriptor.affected_temp_row_slot ==
          descriptor_snapshot.affected_temp_row_slot);
    CHECK(shared_descriptor.reachability_stats ==
          descriptor_snapshot.reachability_stats);
    CHECK(shared_descriptor.base_plan_generation ==
          descriptor_snapshot.base_plan_generation);
    CHECK(shared_descriptor.base_plan_fingerprint ==
          descriptor_snapshot.base_plan_fingerprint);
    CHECK(shared_descriptor.candidate_plan_build_stats ==
          descriptor_snapshot.candidate_plan_build_stats);
    CHECK(shared_descriptor.compiled_rows ==
          descriptor_snapshot.compiled_rows);
    CHECK(shared_descriptor.compiled_productions ==
          descriptor_snapshot.compiled_productions);
    CHECK(shared_descriptor.compiled_children ==
          descriptor_snapshot.compiled_children);
    CHECK(shared_descriptor.removed_base_production ==
          descriptor_snapshot.removed_base_production);
    CHECK(shared_descriptor.reachable_base_clade ==
          descriptor_snapshot.reachable_base_clade);
    CHECK(shared_descriptor.reachable_temp_clade ==
          descriptor_snapshot.reachable_temp_clade);
    CHECK(shared_descriptor.temp_productions_by_base_parent ==
          descriptor_snapshot.temp_productions_by_base_parent);
    CHECK(shared_descriptor.temp_productions_by_temp_parent ==
          descriptor_snapshot.temp_productions_by_temp_parent);
    CHECK(shared_descriptor.temp_productions_by_base_child ==
          descriptor_snapshot.temp_productions_by_base_child);
    CHECK(shared_descriptor.temp_productions_by_temp_child ==
          descriptor_snapshot.temp_productions_by_temp_child);
  }

  {
    auto fixture = make_fixture();
    auto state = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    auto const generation_before = state.grammar.execution_generation;
    CHECK(!state.grammar.productions.empty());
    CHECK(state.grammar.productions.front().children.size() >= 2);
    std::swap(state.grammar.productions.front().children[0],
              state.grammar.productions.front().children[1]);
    CHECK(state.grammar.execution_generation == generation_before);

    auto const counters_before = state.counters;
    bool threw = false;
    std::string message;
    try {
      (void)larch::score_candidates_locally(
          state, {fixture.candidates.front()}, {}, 1);
    } catch (larch::chart_execution_plan_mismatch const& e) {
      threw = true;
      message = e.what();
    }
    CHECK(threw);
    CHECK(message.find("stale grammar fingerprint mismatch") !=
          std::string::npos);
    CHECK(state.counters.plan_mismatch_rejections ==
          counters_before.plan_mismatch_rejections + 1);
    CHECK(state.counters.local_rows_recomputed ==
          counters_before.local_rows_recomputed);
    CHECK(state.counters.local_candidate_scores ==
          counters_before.local_candidate_scores);
    CHECK(state.counters.candidate_execution_plan_builds ==
          counters_before.candidate_execution_plan_builds);
    CHECK(state.counters.chart_execution_plan_cache_hits ==
          counters_before.chart_execution_plan_cache_hits);
    CHECK(state.counters.candidate_execution_plan_cache_hits ==
          counters_before.candidate_execution_plan_cache_hits);
  }

  {
    auto fixture = make_fixture();
    auto state = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    larch::chart_spr_search_counters counters;
    auto prepared =
        larch::chart_spr_search_detail::prepare_local_candidate_score(
            state, fixture.candidates.front(), {}, &counters);
    CHECK(prepared.valid_for_accumulation);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);

    auto overlay = larch::overlay_from_candidate(
        state.grammar, fixture.candidates.front());
    auto materialized = larch::materialize_overlay_grammar(overlay);
    CHECK(materialized.grammar.execution_generation != 0);
    CHECK(materialized.grammar.execution_generation !=
          state.grammar.execution_generation);
    auto accepted_state = larch::build_chart_spr_search_state(
        fixture.dag, materialized.grammar, fixture.patterns);
    CHECK(accepted_state.grammar.execution_generation ==
          materialized.grammar.execution_generation);
    CHECK(accepted_state.execution_plan.grammar_generation() ==
          accepted_state.grammar.execution_generation);
    accepted_state.execution_plan.assert_compatible(accepted_state.grammar);

    auto rows_before = counters.local_rows_recomputed;
    auto candidate_hits_before = counters.candidate_execution_plan_cache_hits;
    larch::chart_spr_local_score_scratch scratch;
    larch::chart_spr_search_detail::accumulate_prepared_local_candidate_patterns(
        accepted_state, prepared, 0, accepted_state.pattern_charts, {},
        &counters, scratch);
    CHECK(!prepared.valid_for_accumulation);
    CHECK(counters.plan_mismatch_rejections == 1);
    CHECK(counters.local_rows_recomputed == rows_before);
    CHECK(counters.candidate_execution_plan_cache_hits ==
          candidate_hits_before);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);
    CHECK(counters.candidate_pattern_full_grammar_validations == 0);
    CHECK(counters.candidate_pattern_partition_validations == 0);
    CHECK(counters.candidate_pattern_clade_order_sorts == 0);
  }

  std::println("  PASS");
}

static void test_checked_candidate_sources_and_planned_materialization() {
  std::println(
      "test_checked_candidate_sources_and_planned_materialization");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto checked = larch::check_chart_execution_plan(state.grammar,
                                                    state.execution_plan);

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer structural_observer{
      &full_validations, &partition_validations, &clade_sorts};
  std::size_t compatibility_checks = 0;
  std::size_t fingerprint_scans = 0;
  std::size_t legacy_index_validations = 0;
  std::size_t dynamic_overlay_partition_validations = 0;
  larch::chart_execution_plan_detail::compatibility_work_observer
      compatibility_observer{&compatibility_checks, &fingerprint_scans,
                             &legacy_index_validations,
                             &dynamic_overlay_partition_validations};

  auto enumerate = [&](larch::chart_spr_candidate_source source,
                       bool trusted) {
    larch::grammar_spr_enumeration_options options;
    options.source = source;
    options.max_candidates = 2;
    options.max_candidates_is_post_dedup = true;
    if (source != larch::chart_spr_candidate_source::grammar) {
      options.sampled_tree_source_dag = &fixture.dag;
      options.sampled_tree_count = 1;
    }
    std::size_t emitted = 0;
    if (trusted) {
      (void)larch::for_each_grammar_spr_candidate(
          state.grammar, checked, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    } else if (source == larch::chart_spr_candidate_source::sampled_tree) {
      (void)larch::for_each_sampled_tree_spr_candidate(
          state.grammar, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    } else if (source == larch::chart_spr_candidate_source::hybrid) {
      (void)larch::for_each_hybrid_spr_candidate(
          state.grammar, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    } else {
      (void)larch::for_each_grammar_spr_candidate(
          state.grammar, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    }
    return emitted;
  };

  {
    larch::parsimony_chart_detail::structural_work_observer_scope structural{
        &structural_observer};
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    (void)enumerate(larch::chart_spr_candidate_source::grammar, true);
    (void)enumerate(larch::chart_spr_candidate_source::sampled_tree, true);
    (void)enumerate(larch::chart_spr_candidate_source::hybrid, true);
  }
  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
  CHECK(compatibility_checks == 0);
  CHECK(fingerprint_scans == 0);
  CHECK(legacy_index_validations == 0);
  CHECK(dynamic_overlay_partition_validations == 0);

  {
    larch::parsimony_chart_detail::structural_work_observer_scope structural{
        &structural_observer};
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    (void)enumerate(larch::chart_spr_candidate_source::grammar, false);
    (void)enumerate(larch::chart_spr_candidate_source::sampled_tree, false);
    (void)enumerate(larch::chart_spr_candidate_source::hybrid, false);
  }
  CHECK(full_validations == 3);
  CHECK(legacy_index_validations == 3);
  // The recurrence observer is intentionally independent of the legacy
  // whole-grammar/index boundary observer used by candidate enumeration.
  CHECK(partition_validations == 0);
  CHECK(compatibility_checks == 0);
  CHECK(fingerprint_scans == 0);
  CHECK(dynamic_overlay_partition_validations == 0);

  full_validations = 0;
  partition_validations = 0;
  clade_sorts = 0;
  legacy_index_validations = 0;
  dynamic_overlay_partition_validations = 0;
  larch::planned_overlay_materialization_result planned;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope structural{
        &structural_observer};
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    planned = larch::materialize_candidate_overlay_grammar_with_plan(
        state.grammar, checked, fixture.candidates.front());
  }
  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
  CHECK(legacy_index_validations == 0);
  CHECK(compatibility_checks == 0);
  CHECK(fingerprint_scans == 0);
  CHECK(dynamic_overlay_partition_validations ==
        fixture.candidates.front().added_productions.size());
  CHECK(planned.payload_validation_stats.production_partition_validations ==
        fixture.candidates.front().added_productions.size());
  CHECK(planned.execution_plan.build_stats().plan_builds == 1);
  CHECK(planned.execution_plan.build_stats().full_grammar_validations == 1);
  CHECK(planned.execution_plan.build_stats().clade_order_sorts == 2);
  CHECK(planned.execution_plan.grammar_generation() ==
        planned.materialized.grammar.execution_generation);
  CHECK(planned.materialized.grammar.execution_generation !=
        state.grammar.execution_generation);

  // A valid but unreachable dynamic production is deliberately absent from
  // the dense grammar/plan while remaining visible in the planned-build and
  // search-level boundary counters.
  auto unreachable = larch::overlay_from_candidate(
      state.grammar, checked, fixture.candidates.front());
  unreachable.temp_clades.push_back(larch::clade_key{{0, 1}});
  larch::clade_id leaf_zero = larch::no_clade;
  larch::clade_id leaf_one = larch::no_clade;
  for (larch::clade_id cid = 0; cid < state.grammar.clades.size(); ++cid) {
    if (state.grammar.clades[cid].taxa == std::vector<larch::taxon_id>{0}) {
      leaf_zero = cid;
    } else if (state.grammar.clades[cid].taxa ==
               std::vector<larch::taxon_id>{1}) {
      leaf_one = cid;
    }
  }
  CHECK(leaf_zero != larch::no_clade);
  CHECK(leaf_one != larch::no_clade);
  larch::overlay_grammar_production unreachable_production;
  unreachable_production.parent = larch::temp_clade_ref(
      static_cast<larch::clade_id>(unreachable.temp_clades.size() - 1));
  unreachable_production.children = {larch::base_clade_ref(leaf_zero),
                                     larch::base_clade_ref(leaf_one)};
  unreachable.temp_productions.push_back(
      std::move(unreachable_production));
  dynamic_overlay_partition_validations = 0;
  larch::planned_overlay_materialization_result unreachable_planned;
  {
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    unreachable_planned = larch::materialize_overlay_grammar_with_plan(
        unreachable, checked);
  }
  CHECK(unreachable_planned.materialized.temp_clade_to_dense.back() ==
        larch::no_clade);
  CHECK(unreachable_planned.materialized.temp_production_to_dense.back() ==
        larch::no_production);
  CHECK(dynamic_overlay_partition_validations ==
        unreachable.temp_productions.size());
  CHECK(unreachable_planned.payload_validation_stats
            .production_partition_validations ==
        unreachable.temp_productions.size());
  larch::chart_spr_search_counters boundary_counters;
  larch::record_planned_overlay_materialization_stats(
      boundary_counters, unreachable_planned);
  CHECK(boundary_counters.dynamic_overlay_payload_partition_validations ==
        unreachable.temp_productions.size());
  CHECK(boundary_counters.candidate_pattern_partition_validations == 0);

  // The callback overload is constrained: a literal null selects the
  // no-callback stats-pointer shape for overlay, candidate, and chain APIs.
  // A typed stats pointer remains available on both success and failure.
  bool typed_dense_completed = false;
  larch::overlay_payload_validation_stats typed_payload_stats;
  auto typed_stats_planned = larch::materialize_overlay_grammar_with_plan(
      unreachable, checked, &typed_dense_completed, &typed_payload_stats);
  CHECK(typed_dense_completed);
  CHECK(typed_payload_stats.production_partition_validations ==
        unreachable.temp_productions.size());
  CHECK(typed_stats_planned.payload_validation_stats
            .production_partition_validations ==
        typed_payload_stats.production_partition_validations);

  auto literal_null_overlay = larch::materialize_overlay_grammar_with_plan(
      unreachable, checked, nullptr, nullptr);
  CHECK(literal_null_overlay.payload_validation_stats
            .production_partition_validations ==
        unreachable.temp_productions.size());
  auto literal_null_candidate =
      larch::materialize_candidate_overlay_grammar_with_plan(
          state.grammar, checked, fixture.candidates.front(), nullptr,
          nullptr);
  CHECK(literal_null_candidate.payload_validation_stats
            .production_partition_validations ==
        fixture.candidates.front().added_productions.size());
  larch::overlay_chain empty_chain{state.grammar};
  auto literal_null_chain = larch::materialize_overlay_chain_with_plan(
      empty_chain, checked, nullptr, nullptr);
  CHECK(literal_null_chain.payload_validation_stats
            .production_partition_validations == 0);
  CHECK(literal_null_chain.materialized.grammar.productions.size() ==
        state.grammar.productions.size());

  // An invalid dynamic production must likewise be rejected even though it is
  // unreachable and therefore absent from the dense output plan.
  auto malformed = unreachable;
  malformed.temp_productions.back().children = {
      larch::base_clade_ref(leaf_zero), larch::base_clade_ref(leaf_zero)};
  bool dense_completed = false;
  bool threw = false;
  larch::overlay_payload_validation_stats failed_payload_stats;
  dynamic_overlay_partition_validations = 0;
  {
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    try {
      (void)larch::materialize_overlay_grammar_with_plan(
          malformed, checked, &dense_completed, &failed_payload_stats);
    } catch (std::runtime_error const& e) {
      threw = true;
      CHECK(std::string{e.what()}.find("children overlap") !=
            std::string::npos);
    }
  }
  CHECK(threw);
  CHECK(!dense_completed);
  CHECK(dynamic_overlay_partition_validations ==
        malformed.temp_productions.size());
  CHECK(failed_payload_stats.production_partition_validations ==
        malformed.temp_productions.size());

  std::println("  PASS");
}

static void test_acceptance_iteration_checks_resident_plan_once() {
  std::println("test_acceptance_iteration_checks_resident_plan_once");

  for (std::size_t candidate_batch_size : {std::size_t{1}, std::size_t{4}}) {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", four_taxon_misplaced_tree());
    auto grammar = larch::build_clade_grammar(dag);
    auto state = larch::build_chart_spr_search_state(dag, grammar);

    larch::chart_spr_search_options options;
    options.acceptance_mode =
        larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.top_k_exact_verify = 3;
    options.enumeration.max_candidates = 6;
    options.enumeration.max_candidates_is_post_dedup = true;
    options.cache.candidate_batch_size = candidate_batch_size;
    options.semantic_capture =
        larch::chart_spr_semantic_capture_mode::digest;

    std::size_t compatibility_checks = 0;
    std::size_t fingerprint_scans = 0;
    std::size_t legacy_index_validations = 0;
    std::size_t dynamic_overlay_partition_validations = 0;
    larch::chart_execution_plan_detail::compatibility_work_observer observer{
        &compatibility_checks, &fingerprint_scans,
        &legacy_index_validations,
        &dynamic_overlay_partition_validations};
    auto dynamic_counter_before =
        state.counters.dynamic_overlay_payload_partition_validations;
    larch::chart_spr_iteration_result iteration;
    {
      larch::chart_execution_plan_detail::compatibility_work_observer_scope
          scope{&observer};
      iteration =
          larch::run_chart_spr_acceptance_iteration(state, options);
    }
    CHECK(iteration.candidates_scored > 0);
    CHECK(iteration.candidates_exact_verified > 0);
    CHECK(compatibility_checks == 1);
    CHECK(fingerprint_scans == 1);
    CHECK(legacy_index_validations == 0);
    CHECK(dynamic_overlay_partition_validations > 0);
    CHECK(state.counters.dynamic_overlay_payload_partition_validations -
              dynamic_counter_before ==
          dynamic_overlay_partition_validations);
    CHECK(state.counters.candidate_pattern_partition_validations == 0);
  }

  std::println("  PASS");
}

static void test_lazy_cache_fixed_topology_conservative_search() {
  std::println("test_lazy_cache_fixed_topology_conservative_search");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);
  auto dense_state = larch::build_chart_spr_search_state(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;
  options.cache.use_lazy_multisite_chart = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  auto dense_scores = larch::fixed_topology_direct_selected_pattern_scores(
      dense_state, *iteration.accepted);
  auto dense_old_full = larch::chart_spr_add_invariant_offset(
      dense_scores.old_active_total, dense_state,
      "lazy fixed-topology dense oracle old score");
  auto dense_new_full = larch::chart_spr_add_invariant_offset(
      dense_scores.new_active_total, dense_state,
      "lazy fixed-topology dense oracle new score");
  CHECK(iteration.accepted->exact->value.old_score == dense_old_full);
  CHECK(iteration.accepted->exact->value.new_score == dense_new_full);
  CHECK(search.counters.local_candidate_scores > 0);
  CHECK(search.counters.local_rows_recomputed > 0);
  CHECK(search.counters.fixed_topology_selected_rows_computed > 0);
  CHECK(search.counters.selected_topology_class_rows_computed ==
        search.counters.fixed_topology_selected_rows_computed);
  CHECK(search.counters.selected_topology_multifurcation_rows > 0);
  CHECK(search.counters.spr_multifurcation_moves_generated > 0);
  CHECK(search.counters.multifurcation_productions_scored > 0);
  CHECK(search.summary.lazy_inside_rows_computed > 0);
  CHECK(search.summary.lazy_merge_ratio > 0.0);
  CHECK(search.summary.selected_topology_class_rows_computed ==
        search.counters.selected_topology_class_rows_computed);
  CHECK(search.summary.final_score < search.summary.initial_score);
  auto rebuilt = larch::build_clade_grammar(search.dag, gopts);
  CHECK(max_production_arity(rebuilt) == 3);

  std::println("  PASS");
}

static void test_lazy_cache_local_commit_updates_lazy_chart() {
  std::println("test_lazy_cache_local_commit_updates_lazy_chart");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.cache.use_lazy_multisite_chart = true;
  options.verify_local_commit_two_chart_oracle_for_tests = true;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(search.counters.local_commit_accepted_moves == 1);
  CHECK(search.counters.local_commit_two_chart_oracle_runs == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_incremental_rows_recomputed ==
        search.counters.lazy_inside_rows_recomputed_on_commit +
            search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.counters.lazy_inside_rows_recomputed_on_commit <=
        search.counters.inside_rows_recomputed_on_commit);
  CHECK(search.counters.lazy_outside_rows_recomputed_on_commit <=
        search.counters.outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_inside_rows_recomputed_on_commit ==
        search.counters.lazy_inside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_outside_rows_recomputed_on_commit ==
        search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_incremental_rows_recomputed ==
        search.counters.lazy_incremental_rows_recomputed);
  CHECK(search.summary.lazy_inside_rows_computed > 0);
  CHECK(search.summary.lazy_merge_ratio > 0.0);
  auto lazy_denominator =
      static_cast<double>(search.summary.active_pattern_count) *
      static_cast<double>(search.summary.final_grammar_clade_count);
  CHECK(lazy_denominator > 0.0);
  CHECK(search.summary.lazy_merge_ratio ==
        static_cast<double>(search.summary.lazy_inside_rows_computed) /
            lazy_denominator);
  CHECK(search.canonical_digest.has_value());

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

  auto lazy_options = options;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, lazy_options);
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.exact_trim_active_only.has_value());
  CHECK(lazy_state.exact_trim_active_only->lazy_chart_used);
  CHECK(lazy_state.exact_trim_active_only->optimum ==
        state.exact_trim_active_only->optimum);

  auto local = larch::score_candidate_locally(state, fixture.candidates.front());
  auto exact = larch::verify_candidate_exact_against_state(
      state, local, options.exact_trim);
  auto lazy_local = larch::score_candidate_locally(
      lazy_state, fixture.candidates.front());
  auto lazy_exact = larch::verify_candidate_exact_against_state(
      lazy_state, lazy_local, lazy_options.exact_trim);
  auto oracle = larch::score_multisite_spr_candidate_exact_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front());

  CHECK(exact.valid);
  CHECK(exact.exact.has_value());
  CHECK(exact.exact->kind == larch::chart_spr_score_kind::grammar_exact);
  CHECK(exact.exact->convention ==
        larch::chart_spr_score_convention::full_with_invariants);
  CHECK(lazy_exact.valid);
  CHECK(lazy_exact.exact.has_value());
  CHECK(lazy_exact.exact->kind ==
        larch::chart_spr_score_kind::grammar_exact);
  CHECK(lazy_exact.exact->convention ==
        larch::chart_spr_score_convention::full_with_invariants);
  CHECK(lazy_exact.exact->value.old_score == exact.exact->value.old_score);
  CHECK(lazy_exact.exact->value.new_score == exact.exact->value.new_score);
  CHECK(lazy_exact.exact->value.delta == exact.exact->value.delta);
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

static void test_failed_exact_materialization_is_timed() {
  std::println("test_failed_exact_materialization_is_timed");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);

  // The verifier converts exact-materializer exceptions into invalid
  // candidates.  Force that call boundary to throw so the failed span must be
  // charged even though the success-only materialization counters stay zero.
  auto candidate =
      larch::score_candidate_locally(state, fixture.candidates.front());
  CHECK(candidate.valid);
  candidate.force_exact_materializer_failure_for_tests = true;

  auto const before =
      state.counters.materialization_exact_verification_ms;
  auto verified = larch::verify_candidate_exact_against_state(
      state, std::move(candidate), options.exact_trim);

  CHECK(!verified.valid);
  CHECK(verified.invalid_reason ==
        "forced exact materializer failure for tests");
  CHECK(state.counters.exact_verifications == 1);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.materialization_exact_verification_ms > before);

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
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);
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
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);

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
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);

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

struct phase8_fixed_fixture {
  std::string name;
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
};

static phase8_fixed_fixture load_phase8_binary_four_fixture() {
  phase8_fixed_fixture f;
  f.name = "wric_binary_four";
  f.dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/wric_binary_four.fa"),
      larch::test::source_path_string("test/wric_binary_four.nwk"),
      larch::test::source_path_string("test/wric_binary_four.ref"));
  f.grammar = larch::build_clade_grammar(f.dag);
  return f;
}

static phase8_fixed_fixture load_phase8_two_polytomy_fixture() {
  phase8_fixed_fixture f;
  f.name = "wric_two_polytomy";
  f.dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/wric_two_polytomy.fa"),
      larch::test::source_path_string("test/wric_two_polytomy.nwk"),
      larch::test::source_path_string("test/wric_two_polytomy.ref"));
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      f.dag, larch::clade_grammar_options{}, opts);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "phase8 fixed-topology two-polytomy");
  f.grammar = std::move(refinement.grammar);
  return f;
}

static phase8_fixed_fixture load_phase8_test_5_trees_fixture() {
  phase8_fixed_fixture f;
  f.name = "data/test_5_trees";
  constexpr std::array<char const*, 5> paths = {
      "data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
      "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
      "data/test_5_trees/tree_4.pb.gz",
  };
  std::vector<larch::phylo_dag> trees;
  for (auto* path : paths) {
    trees.emplace_back(larch::load_proto_dag(path));
    larch::recompute_compact_genomes(trees.back());
    larch::set_sample_ids_from_cg(trees.back());
  }
  larch::merge merger{larch::get_reference_sequence(trees.front())};
  for (auto& tree : trees) merger.add_dag(tree);
  f.dag = larch::phylo_dag{std::move(merger.get_result())};

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      f.dag, larch::clade_grammar_options{}, opts);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "phase8 fixed-topology test_5_trees");
  f.grammar = std::move(refinement.grammar);
  return f;
}

static bool phase8_candidate_matches_class(
    larch::grammar_spr_candidate const& candidate, std::string const& klass) {
  if (klass == "binary_spr") {
    return !candidate.removed_productions.empty() &&
           candidate.removed_productions.size() <= 2;
  }
  if (klass == "spr_with_collapse") {
    return candidate.removed_productions.size() > 1;
  }
  if (klass == "child_set_change") {
    return !candidate.removed_productions.empty() &&
           !candidate.added_productions.empty();
  }
  return true;
}

static larch::chart_spr_candidate_score phase8_choose_fixed_candidate(
    larch::chart_spr_search_state const& state, std::string const& klass,
    std::size_t max_candidates = 0) {
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;

  larch::grammar_spr_enumeration_options enumeration;
  enumeration.max_candidates = max_candidates;
  enumeration.max_candidates_is_post_dedup = true;

  std::optional<larch::chart_spr_candidate_score> chosen;
  (void)larch::for_each_grammar_spr_candidate(
      state.grammar, enumeration,
      [&](larch::grammar_spr_candidate const& candidate) {
        if (!phase8_candidate_matches_class(candidate, klass)) return true;
        auto local = larch::score_candidate_locally(state, candidate);
        if (!local.valid) return true;
        larch::attach_fixed_topology_selection_for_acceptance(state, local,
                                                              options);
        if (!local.valid || !local.topology_selection.certificate) return true;
        chosen = std::move(local);
        return false;
      });
  CHECK(chosen.has_value());
  return *chosen;
}

static void phase8_assert_fixed_candidate_matches_materialized_per_pattern(
    phase8_fixed_fixture& fixture, std::string const& klass) {
  auto state = larch::build_chart_spr_search_state(fixture.dag,
                                                    fixture.grammar);
  auto chosen = phase8_choose_fixed_candidate(state, klass);

  auto fixed = larch::verify_candidate_fixed_topology_exact(state, chosen);
  CHECK(fixed.valid);
  CHECK(fixed.exact.has_value());
  CHECK(fixed.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);

  auto const& certificate = *chosen.topology_selection.certificate;
  auto overlay = larch::overlay_from_candidate(fixture.grammar,
                                               chosen.candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);

  std::vector<larch::production_id> after_ids;
  after_ids.reserve(certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    after_ids.push_back(
        larch::chart_spr_dense_production_id_for_ref(materialized, ref));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);

  auto selected = larch::chart_spr_overlay_selected_production_by_parent(
      fixture.grammar, chosen.candidate,
      certificate.after_overlay_productions);
  auto direct_scores = larch::fixed_topology_direct_selected_pattern_scores(
      state, chosen);
  auto const& active = state.active_patterns.patterns.patterns;
  CHECK(!active.empty());
  CHECK(direct_scores.old_pattern_scores.size() == active.size());
  CHECK(direct_scores.new_pattern_scores.size() == active.size());

  std::uint64_t materialized_new_active = 0;
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto direct_row = larch::chart_spr_restricted_overlay_topology_row(
        fixture.grammar, chosen.candidate, active[p], selected);
    auto materialized_row =
        larch::chart_multisite_detail::restricted_topology_row(
            materialized.grammar, active[p], after_topology);
    CHECK(direct_row == materialized_row);
    auto materialized_score = larch::chart_spr_weighted_root_score_from_row(
        materialized_row, active[p], state.chart_opts);
    CHECK(direct_scores.new_pattern_scores[p] == materialized_score);
    materialized_new_active = larch::chart_multisite_detail::checked_add_u64(
        materialized_new_active, materialized_score,
        "phase8 materialized per-pattern total");
  }

  auto new_full = larch::chart_spr_add_invariant_offset(
      materialized_new_active, state,
      "phase8 fixed-topology per-pattern new oracle");
  CHECK(fixed.exact->value.new_score == new_full);

  std::println("    {} / {} PASS", fixture.name, klass);
}

static void
    test_phase8_fixed_topology_cache_score_matches_materialized_per_pattern() {
  std::println(
      "test_phase8_fixed_topology_cache_score_matches_materialized_per_pattern");

  auto binary = load_phase8_binary_four_fixture();
  phase8_assert_fixed_candidate_matches_materialized_per_pattern(
      binary, "binary_spr");

  auto polytomy = load_phase8_two_polytomy_fixture();
  phase8_assert_fixed_candidate_matches_materialized_per_pattern(
      polytomy, "spr_with_collapse");

  auto five = load_phase8_test_5_trees_fixture();
  phase8_assert_fixed_candidate_matches_materialized_per_pattern(
      five, "child_set_change");

  std::println("  PASS");
}

static void test_phase8_fixed_topology_rejects_bad_selected_partition() {
  std::println("test_phase8_fixed_topology_rejects_bad_selected_partition");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto state = larch::build_chart_spr_search_state(dag, grammar);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto root = grammar.root_clade;
  auto root_prod = production_id_for(grammar, root, {ab, cd});
  auto ab_prod = production_id_for(grammar, ab, {a, b});
  auto cd_prod = production_id_for(grammar, cd, {c, d});

  larch::grammar_spr_candidate candidate;
  candidate.added_clades.push_back(larch::clade_key{{0, 2}});  // AC
  larch::overlay_grammar_production bad_root;
  bad_root.parent = larch::base_clade_ref(root);
  bad_root.children = {larch::base_clade_ref(ab), larch::temp_clade_ref(0)};
  bad_root.multiplicity = 1;
  candidate.added_productions.push_back(bad_root);
  larch::overlay_grammar_production ac_prod;
  ac_prod.parent = larch::temp_clade_ref(0);
  ac_prod.children = {larch::base_clade_ref(a), larch::base_clade_ref(c)};
  ac_prod.multiplicity = 1;
  candidate.added_productions.push_back(ac_prod);

  larch::chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  scored.topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(
          grammar, candidate,
          {larch::base_production_ref(root_prod),
           larch::base_production_ref(ab_prod),
           larch::base_production_ref(cd_prod)},
          {larch::temp_production_ref(0),
           larch::base_production_ref(ab_prod),
           larch::temp_production_ref(1)});

  auto verified = larch::verify_candidate_fixed_topology_exact(state, scored);
  CHECK(!verified.valid);
  CHECK(verified.invalid_reason.find("selected production children overlap") !=
            std::string::npos ||
        verified.invalid_reason.find("selected production children do not union") !=
            std::string::npos);

  std::println("  PASS");
}

static void test_phase8_fixed_topology_rejects_unreachable_selected_after_ref() {
  std::println(
      "test_phase8_fixed_topology_rejects_unreachable_selected_after_ref");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto state = larch::build_chart_spr_search_state(dag, grammar);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto root = grammar.root_clade;
  auto root_prod = production_id_for(grammar, root, {ab, cd});
  auto ab_prod = production_id_for(grammar, ab, {a, b});
  auto cd_prod = production_id_for(grammar, cd, {c, d});

  larch::grammar_spr_candidate candidate;
  candidate.added_clades.push_back(larch::clade_key{{0, 2}});  // AC
  candidate.added_productions.push_back(temp_prod(
      larch::temp_clade_ref(0), {larch::base_clade_ref(a),
                                 larch::base_clade_ref(c)}));

  larch::chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  scored.topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(
          grammar, candidate,
          {larch::base_production_ref(root_prod),
           larch::base_production_ref(ab_prod),
           larch::base_production_ref(cd_prod)},
          {larch::base_production_ref(root_prod),
           larch::base_production_ref(ab_prod),
           larch::base_production_ref(cd_prod),
           larch::temp_production_ref(0)});

  auto verified = larch::verify_candidate_fixed_topology_exact(state, scored);
  CHECK(!verified.valid);
  CHECK(verified.invalid_reason.find("unreachable selected production") !=
        std::string::npos);

  std::println("  PASS");
}

static void test_phase8_persistent_cache_local_commit_matches_materialized() {
  std::println(
      "test_phase8_persistent_cache_local_commit_matches_materialized");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto oracle_state = larch::build_chart_spr_search_state(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_fixed_topology_materialized_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);
  CHECK(!search.iterations.empty());
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  CHECK(search.summary.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.overlay_materializations_for_oracle ==
        search.counters.fixed_topology_persistent_cache_verifications);

  auto const& chosen = *iteration.accepted;
  auto const& certificate = *chosen.topology_selection.certificate;
  auto overlay = larch::overlay_from_candidate(oracle_state.grammar,
                                               chosen.candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  std::vector<larch::production_id> after_ids;
  after_ids.reserve(certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    after_ids.push_back(
        larch::chart_spr_dense_production_id_for_ref(materialized, ref));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);
  (void)larch::validate_grammar_topology(materialized.grammar,
                                         after_topology);

  auto direct_scores = larch::fixed_topology_direct_selected_pattern_scores(
      oracle_state, chosen);
  auto const& active = oracle_state.active_patterns.patterns.patterns;
  std::uint64_t materialized_new_active = 0;
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto materialized_row =
        larch::chart_multisite_detail::restricted_topology_row(
            materialized.grammar, active[p], after_topology);
    auto materialized_score = larch::chart_spr_weighted_root_score_from_row(
        materialized_row, active[p], oracle_state.chart_opts);
    CHECK(direct_scores.new_pattern_scores[p] == materialized_score);
    materialized_new_active = larch::chart_multisite_detail::checked_add_u64(
        materialized_new_active, materialized_score,
        "phase8 persistent local-commit materialized total");
  }
  auto materialized_new_full = larch::chart_spr_add_invariant_offset(
      materialized_new_active, oracle_state,
      "phase8 persistent local-commit materialized full");
  CHECK(chosen.exact->value.new_score == materialized_new_full);

  std::println("  PASS");
}

static void test_phase8_persistent_cache_dag_verification_has_no_fallback() {
  std::println(
      "test_phase8_persistent_cache_dag_verification_has_no_fallback");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_one()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_two()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_fixed_topology_materialized_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.overlay_materializations_for_oracle ==
        search.counters.fixed_topology_persistent_cache_verifications);

  std::println("  PASS");
}

static void test_phase8_per_pattern_oracle_catches_independent_moved_state() {
  std::println(
      "test_phase8_per_pattern_oracle_catches_independent_moved_state");

  auto inf = larch::chart_inf;
  std::array<larch::chart_cost, larch::nuc_state_count> moved{0, 0, inf, inf};
  std::array<larch::chart_cost, larch::nuc_state_count> detach{0, 10, inf, inf};
  std::array<larch::chart_cost, larch::nuc_state_count> reattach{10, 0, inf, inf};

  auto shared = inf;
  auto detach_only = inf;
  auto reattach_only = inf;
  for (std::uint8_t s = 0; s < larch::nuc_state_count; ++s) {
    shared = std::min(
        shared, larch::parsimony_chart_detail::saturated_add(
                    moved[s], larch::parsimony_chart_detail::saturated_add(
                                  detach[s], reattach[s])));
    detach_only = std::min(
        detach_only, larch::parsimony_chart_detail::saturated_add(moved[s],
                                                                  detach[s]));
    reattach_only = std::min(
        reattach_only, larch::parsimony_chart_detail::saturated_add(moved[s],
                                                                    reattach[s]));
  }
  auto independent = larch::parsimony_chart_detail::saturated_add(detach_only,
                                                                  reattach_only);
  CHECK(independent < shared);

  // The Phase-8 oracle is per pattern, not aggregate-only: a scorer that lets
  // the detach and reattach terms choose independent moved-subtree states would
  // under-count this pattern and fail equality immediately.
  std::vector<std::uint64_t> oracle_new{static_cast<std::uint64_t>(shared)};
  std::vector<std::uint64_t> bad_new{static_cast<std::uint64_t>(independent)};
  CHECK(bad_new != oracle_new);

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto same_state_pair = [](char const* name, char const* left,
                            char const* right, char const* state) {
    return tiny_inner(name, state,
                      {tiny_leaf(left, state), tiny_leaf(right, state)});
  };
  auto four_a_context = [&] {
    return tiny_inner("S", "A",
                      {same_state_pair("S1", "B", "C", "A"),
                       same_state_pair("S2", "D", "E", "A")});
  };
  auto four_c_context = [&] {
    return tiny_inner("T", "C",
                      {same_state_pair("T1", "F", "G", "C"),
                       same_state_pair("T2", "H", "I", "C")});
  };
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", tiny_inner("root", "A",
                       {tiny_inner("P", "A",
                                   {tiny_inner("M", "A",
                                               {tiny_leaf("A", "A"),
                                                tiny_leaf("J", "C")}),
                                    four_a_context()}),
                        four_c_context()}));
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.force_fixed_topology_independent_sm_bug_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests >
        0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks ==
        search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches ==
        search.counters.fixed_topology_persistent_cache_fallbacks);
  auto perturbations =
      search.counters.fixed_topology_independent_sm_bug_perturbations_for_tests;
  CHECK(perturbations == 0);
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);

  std::println("  PASS");
}

static void test_phase5_combine_rows_binary_regression() {
  std::println("test_phase5_combine_rows_binary_regression");

  larch::chart_multisite_detail::chart_row left{
      larch::chart_cost{0}, larch::chart_cost{2}, larch::chart_cost{5},
      larch::chart_inf};
  larch::chart_multisite_detail::chart_row right{
      larch::chart_cost{3}, larch::chart_cost{0}, larch::chart_cost{4},
      larch::chart_cost{6}};
  std::array<larch::chart_multisite_detail::chart_row, 2> children{left,
                                                                   right};
  auto variadic = larch::chart_multisite_detail::combine_rows(
      std::span<larch::chart_multisite_detail::chart_row const>{
          children.data(), children.size()});
  auto binary = larch::chart_multisite_detail::combine_binary_rows(left,
                                                                   right);
  CHECK(variadic == binary);

  std::println("  PASS");
}

static void phase5_check_selected_topology_fixture(
    larch::test::tiny_tree_node const& tree, std::size_t expected_max_arity) {
  auto dag = larch::test::make_tiny_labelled_tree("AA", tree);
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == expected_max_arity);

  auto refs = all_base_production_refs(grammar);
  larch::grammar_spr_candidate candidate;
  auto topology_ids = std::vector<larch::production_id>{};
  topology_ids.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    topology_ids.push_back(static_cast<larch::production_id>(pid));
  }
  auto topology = larch::grammar_topology_from_productions(grammar,
                                                           topology_ids);

  auto state = make_phase5_direct_state(dag, grammar);
  larch::chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  scored.topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(grammar, candidate, refs,
                                                 refs);

  auto selected = larch::chart_spr_overlay_selected_production_by_parent(
      grammar, candidate, refs);
  auto const& active = state.active_patterns.patterns.patterns;
  std::vector<std::uint64_t> brute_scores;
  brute_scores.reserve(active.size());
  std::uint64_t brute_active_total = 0;
  for (auto const& pattern : active) {
    auto brute_row = phase5_brute_selected_topology_row(grammar, pattern,
                                                        topology);
    auto direct_row = larch::chart_spr_restricted_overlay_topology_row(
        grammar, candidate, pattern, selected);
    auto selected_row = larch::chart_multisite_detail::restricted_topology_row(
        grammar, pattern, topology);
    CHECK(direct_row == brute_row);
    CHECK(selected_row == brute_row);
    auto brute_score = larch::chart_spr_weighted_root_score_from_row(
        brute_row, pattern, state.chart_opts);
    brute_scores.push_back(brute_score);
    brute_active_total = larch::chart_multisite_detail::checked_add_u64(
        brute_active_total, brute_score, "phase5 brute active total");
  }

  auto cache_scores =
      larch::fixed_topology_selected_cache_pattern_scores_for_tests(state,
                                                                    scored);
  CHECK(cache_scores.old_pattern_scores == brute_scores);
  CHECK(cache_scores.new_pattern_scores == brute_scores);
  CHECK(cache_scores.old_active_total == brute_active_total);
  CHECK(cache_scores.new_active_total == brute_active_total);

  larch::chart_spr_search_options lazy_options;
  lazy_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      dag, grammar, lazy_options);
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.active_patterns.patterns.patterns.size() == active.size());
  auto lazy_scores =
      larch::fixed_topology_direct_selected_pattern_scores(lazy_state, scored);
  CHECK(lazy_scores.old_pattern_scores == brute_scores);
  CHECK(lazy_scores.new_pattern_scores == brute_scores);
  CHECK(lazy_scores.old_active_total == brute_active_total);
  CHECK(lazy_scores.new_active_total == brute_active_total);
  CHECK(lazy_state.counters.fixed_topology_selected_rows_computed > 0);
  CHECK(lazy_state.counters.selected_topology_multifurcation_rows > 0);

  auto verified = larch::verify_candidate_fixed_topology_exact(state, scored);
  CHECK(verified.valid);
  CHECK(verified.exact.has_value());
  CHECK(verified.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);

  auto selected_full = larch::chart_multisite_detail::checked_add_u64(
      brute_active_total, state.invariant_constant_offset,
      "phase5 selected topology invariant offset");
  CHECK(verified.exact->value.old_score == selected_full);
  CHECK(verified.exact->value.new_score == selected_full);
  CHECK(state.counters.selected_topology_multifurcation_rows >=
        active.size());
}

static void test_phase5_multifurcation_selected_topology_rows() {
  std::println("test_phase5_multifurcation_selected_topology_rows");

  phase5_check_selected_topology_fixture(phase5_arity3_selected_tree(), 3);
  phase5_check_selected_topology_fixture(phase5_arity4_selected_tree(), 4);

  std::println("  PASS");
}

static void
test_phase5_multifurcation_outside_rows_exercise_shared_state_guard() {
  std::println(
      "test_phase5_multifurcation_outside_rows_exercise_shared_state_guard");

  auto inf = larch::chart_inf;
  larch::chart_multisite_detail::chart_row moved_context{1, 1, inf, inf};
  larch::chart_multisite_detail::chart_row a_context{0, inf, inf, inf};
  larch::chart_multisite_detail::chart_row c_context{inf, 0, inf, inf};
  larch::chart_multisite_detail::chart_row parent_outside{};
  parent_outside.fill(0);

  auto detach_rows = larch::chart_spr_selected_overlay_child_outside_rows(
      parent_outside, {moved_context, a_context, a_context});
  auto reattach_rows = larch::chart_spr_selected_overlay_child_outside_rows(
      parent_outside, {moved_context, c_context, c_context});
  CHECK(detach_rows.size() == 3);
  CHECK(reattach_rows.size() == 3);
  CHECK(detach_rows[0][larch::nuc_base::A] == 0);
  CHECK(detach_rows[0][larch::nuc_base::C] == 1);
  CHECK(reattach_rows[0][larch::nuc_base::A] == 1);
  CHECK(reattach_rows[0][larch::nuc_base::C] == 0);

  // The rows above prove the selected-outside context builder accepts k-ary
  // productions.  The adversarial context below is the shared-s_M guard: a
  // scorer that minimizes detach and reattach states independently would
  // under-count the same moved-subtree root state.
  larch::chart_multisite_detail::chart_row moved{0, 0, inf, inf};
  larch::chart_multisite_detail::chart_row detach{0, 10, inf, inf};
  larch::chart_multisite_detail::chart_row reattach{10, 0, inf, inf};
  auto shared = inf;
  auto detach_only = inf;
  auto reattach_only = inf;
  for (std::uint8_t state = 0; state < larch::nuc_state_count; ++state) {
    shared = std::min(
        shared, larch::chart_trim_detail::add3(
                    moved[state], detach[state], reattach[state]));
    detach_only = std::min(
        detach_only, larch::parsimony_chart_detail::saturated_add(
                         moved[state], detach[state]));
    reattach_only = std::min(
        reattach_only, larch::parsimony_chart_detail::saturated_add(
                           moved[state], reattach[state]));
  }
  auto independent = larch::parsimony_chart_detail::saturated_add(
      detach_only, reattach_only);
  CHECK(independent < shared);

  std::println("  PASS");
}

static larch::rank3_production_taxa_key phase6_key_from_signature(
    larch::chart_spr_production_signature signature) {
  larch::rank3_production_taxa_key key;
  key.parent = std::move(signature.parent_taxa);
  key.children = std::move(signature.child_taxa);
  larch::rank3_detail::normalize_production_key(key);
  return key;
}

static larch::production_id phase6_production_id_for_key(
    larch::clade_grammar const& grammar,
    larch::rank3_production_taxa_key key) {
  larch::rank3_detail::normalize_production_key(key);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto id = static_cast<larch::production_id>(pid);
    if (larch::rank3_detail::production_key_from_id(grammar, id) == key) {
      return id;
    }
  }
  return larch::no_production;
}

static std::uint64_t phase6_score_certificate_after_topology(
    larch::phylo_dag& dag, larch::clade_grammar const& grammar,
    larch::chart_spr_topology_certificate const& certificate) {
  std::vector<larch::production_id> pids;
  pids.reserve(certificate.after_signatures.size());
  for (auto const& signature : certificate.after_signatures) {
    auto pid = phase6_production_id_for_key(
        grammar, phase6_key_from_signature(signature));
    CHECK(pid != larch::no_production);
    pids.push_back(pid);
  }
  auto topology = larch::rank3_topology_from_productions(grammar, pids);
  auto patterns = larch::build_site_patterns(dag, grammar);
  std::uint64_t total = 0;
  for (auto const& pattern : patterns.patterns) {
    auto row = larch::chart_multisite_detail::restricted_topology_row(
        grammar, pattern, topology);
    total = larch::chart_multisite_detail::checked_add_u64(
        total,
        larch::chart_spr_weighted_root_score_from_row(row, pattern, {}),
        "phase6 rebuilt certificate score");
  }
  return total;
}

static void
test_phase6_source_multifurcation_candidate_has_no_unreachable_helper() {
  std::println(
      "test_phase6_source_multifurcation_candidate_has_no_unreachable_helper");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_internal_arity3_source_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  auto a_taxa = taxa_for(grammar, {"A"});
  auto b_taxa = taxa_for(grammar, {"B"});
  auto abc = clade_for(grammar, {"A", "B", "C"});
  CHECK(max_production_arity(grammar) == 3);

  bool found = false;
  for (auto const& candidate :
       larch::enumerate_grammar_spr_candidates(grammar)) {
    auto moved_taxa = larch::chart_spr_clade_taxa_for_ref(
        grammar, candidate, candidate.moved_clade);
    auto target_taxa = larch::chart_spr_clade_taxa_for_ref(
        grammar, candidate, candidate.new_sibling_or_target);
    if (candidate.old_parent != larch::base_clade_ref(abc) ||
        moved_taxa != a_taxa || target_taxa != b_taxa) {
      continue;
    }
    auto materialized =
        larch::materialize_overlay_grammar(larch::overlay_from_candidate(
            grammar, candidate));
    for (auto dense_pid : materialized.temp_production_to_dense) {
      CHECK(dense_pid != larch::no_production);
    }
    found = true;
  }
  CHECK(found);

  std::println("  PASS");
}

static void test_phase6_multifurcation_fixed_topology_local_commit() {
  std::println("test_phase6_multifurcation_fixed_topology_local_commit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_local_commit_two_chart_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(iteration.state_score_after < iteration.state_score_before);
  CHECK(search.summary.final_score < search.summary.initial_score);
  CHECK(search.counters.local_commit_accepted_moves == 1);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.local_commit_two_chart_oracle_runs == 1);
  CHECK(search.counters.spr_multifurcation_moves_generated > 0);
  CHECK(search.summary.spr_multifurcation_moves_generated ==
        search.counters.spr_multifurcation_moves_generated);
  CHECK(search.counters.multifurcation_productions_scored > 0);
  CHECK(search.summary.multifurcation_productions_scored ==
        search.counters.multifurcation_productions_scored);
  CHECK(search.summary.local_leaf_state_view_uses ==
        search.counters.local_leaf_state_view_uses);
  CHECK(search.summary.local_leaf_state_view_uses > 0);
  CHECK(search.summary.local_leaf_state_owned_copies == 0);
  CHECK(search.summary.local_row_scratch_capacity_growths ==
        search.counters.local_row_scratch_capacity_growths);
  CHECK(search.summary.local_row_scratch_capacity_growths > 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag, gopts);
  CHECK(max_production_arity(rebuilt) == 3);
  CHECK(iteration.accepted->topology_selection.certificate.has_value());
  CHECK(phase6_score_certificate_after_topology(
            search.dag, rebuilt,
            *iteration.accepted->topology_selection.certificate) ==
        search.summary.final_score);

  std::println("  PASS");
}

static void test_phase6_multifurcation_lower_bound_conservative_commit() {
  std::println(
      "test_phase6_multifurcation_lower_bound_conservative_commit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(iteration.state_score_after < iteration.state_score_before);
  CHECK(search.summary.final_score < search.summary.initial_score);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.local_commit_accepted_moves == 0);
  CHECK(search.counters.spr_multifurcation_moves_generated > 0);
  CHECK(search.summary.spr_multifurcation_moves_generated ==
        search.counters.spr_multifurcation_moves_generated);
  CHECK(search.counters.multifurcation_productions_scored > 0);
  CHECK(search.summary.multifurcation_productions_scored ==
        search.counters.multifurcation_productions_scored);

  std::println("  PASS");
}

static void test_phase6_exact_multisite_multifurcation_gate() {
  std::println("test_phase6_exact_multisite_multifurcation_gate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;

  larch::parsimony_chart_detail::reset_arity_gate_throw_counters_for_tests();
  bool threw = false;
  std::string message;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("WI6") != std::string::npos);
  CHECK(message.find("exact_multisite") != std::string::npos);
  CHECK(message.find("multifurcating") != std::string::npos);
  auto arity_gate_throws =
      larch::parsimony_chart_detail::arity_gate_throws_snapshot();
  CHECK(arity_gate_throws.total == 1);
  CHECK(arity_gate_throws.count(
            larch::arity_gate_consumer::chart_spr_exact_multisite) == 1);
  CHECK(larch::parsimony_chart_detail::arity_gate_consumer_reason(
            larch::arity_gate_consumer::chart_spr_exact_multisite)
            .find("exact_multisite") != std::string_view::npos);

  std::println("  PASS");
}

static void test_phase8_persistent_cache_invariant_failure_is_hard_error() {
  std::println(
      "test_phase8_persistent_cache_invariant_failure_is_hard_error");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.force_fixed_topology_cache_epoch_mismatch_for_tests = true;

  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    CHECK(std::string(e.what()).find("persistent cache epochs") !=
          std::string::npos);
  }
  CHECK(threw);

  std::println("  PASS");
}

// Phase 8 issue 2 (production gate + persistent-verifier/materialized-oracle
// coverage for every required move class).  Runs the persistent local-commit
// verifier through run_chart_spr_search on each Phase-8 fixture/move-class
// pair, with the materialized from-scratch oracle enabled, and asserts the
// per-pattern oracle contract: no cache/oracle mismatch, no fallback, the
// production direct-overlay gate stayed clean, and the persistent inside cache
// was actually consulted (issue 1: icache participation, not just epoch/shape).
static void phase8_run_persistent_verifier_on_fixture(
    phase8_fixed_fixture& fixture) {
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_fixed_topology_materialized_oracle_for_tests = true;
  // The materialized from-scratch oracle materializes once per verified
  // candidate, so bound the candidate stream to keep the fixture matrix
  // affordable while still exercising the persistent verifier + materialized
  // oracle on every required move class.
  options.enumeration.max_candidates = 12;
  options.enumeration.max_candidates_is_post_dedup = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  // The persistent-cache value agreed with BOTH the production direct-overlay
  // per-pattern gate (issue 2a) and the materialized from-scratch oracle
  // (issue 2b), so neither fallback path fired.
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  CHECK(search.counters
            .fixed_topology_persistent_cache_direct_oracle_mismatches == 0);
  // Issue 1: the persistent inside cache participated in the delta (rows were
  // cross-checked against icache), not just consulted for epoch/shape.  At
  // least one base-clade selected row must have been reused from icache on a
  // real fixture, proving the delta is "from persistent inside+outside cache,
  // restricted to affected rows".
  CHECK(search.counters.fixed_topology_icache_rows_reused > 0);
  // No dense materialization happened for exact verification (the persistent
  // path and the direct gate are both non-materializing); the only
  // materialization charged is the diagnostic materialized oracle, counted
  // separately under overlay_materializations_for_oracle.
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.overlay_materializations_for_oracle ==
        search.counters.fixed_topology_persistent_cache_verifications);
  // Every accepted move (if any) is labelled fixed_topology_exact.
  for (auto const& iter : search.iterations) {
    if (iter.accepted) {
      CHECK(iter.accepted->exact.has_value());
      CHECK(iter.accepted->exact->kind ==
            larch::chart_spr_score_kind::fixed_topology_exact);
    }
  }
  std::println("    {} PASS", fixture.name);
}

static void
    test_phase8_persistent_verifier_materialized_oracle_all_move_classes() {
  std::println(
      "test_phase8_persistent_verifier_materialized_oracle_all_move_classes");

  auto binary = load_phase8_binary_four_fixture();
  phase8_run_persistent_verifier_on_fixture(binary);

  auto polytomy = load_phase8_two_polytomy_fixture();
  phase8_run_persistent_verifier_on_fixture(polytomy);

  auto five = load_phase8_test_5_trees_fixture();
  phase8_run_persistent_verifier_on_fixture(five);

  std::println("  PASS");
}

// Phase 8 issue 4: when the independent materialized from-scratch oracle finds
// a per-pattern mismatch (forced here by the independent-s_M corruption hook),
// the verifier must use the materialized oracle's own scores as the authority
// rather than re-running the direct overlay scorer (which shares overlay-space
// machinery with the cache path).  We assert this indirectly: the fallback
// counter equals the witness count, the materialized-oracle mismatch counter
// equals the fallback count, and -- critically -- the resulting accepted
// candidate's exact score is still labelled fixed_topology_exact and matches the
// materialized oracle's per-pattern new-score total (the authority value).
static void test_phase8_oracle_mismatch_uses_materialized_oracle_result() {
  std::println(
      "test_phase8_oracle_mismatch_uses_materialized_oracle_result");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto oracle_state = larch::build_chart_spr_search_state(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.force_fixed_topology_independent_sm_bug_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(!search.iterations.empty());
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->topology_selection.certificate);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests >
        0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks ==
        search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches ==
        search.counters.fixed_topology_persistent_cache_fallbacks);

  // The accepted exact score is the materialized oracle's authority value:
  // recompute the materialized selected-after score per pattern and confirm the
  // accepted new_score equals it (not the cache path's corrupted value and not
  // a re-run direct-overlay value).
  auto const& chosen = *iteration.accepted;
  auto const& certificate = *chosen.topology_selection.certificate;
  auto overlay = larch::overlay_from_candidate(oracle_state.grammar,
                                               chosen.candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  std::vector<larch::production_id> after_ids;
  after_ids.reserve(certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    after_ids.push_back(
        larch::chart_spr_dense_production_id_for_ref(materialized, ref));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);
  auto const& active = oracle_state.active_patterns.patterns.patterns;
  std::uint64_t materialized_new_active = 0;
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto row = larch::chart_multisite_detail::restricted_topology_row(
        materialized.grammar, active[p], after_topology);
    materialized_new_active = larch::chart_multisite_detail::checked_add_u64(
        materialized_new_active,
        larch::chart_spr_weighted_root_score_from_row(
            row, active[p], oracle_state.chart_opts),
        "phase8 oracle-mismatch materialized authority total");
  }
  auto materialized_new_full = larch::chart_spr_add_invariant_offset(
      materialized_new_active, oracle_state,
      "phase8 oracle-mismatch materialized authority full");
  CHECK(chosen.exact.has_value());
  CHECK(chosen.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(chosen.exact->value.new_score == materialized_new_full);

  std::println("  PASS");
}

static void test_phase8_chain_objective_gate_is_monotone_across_commits() {
  std::println(
      "test_phase8_chain_objective_gate_is_monotone_across_commits");

  // Issue 3: sequential fixed_topology_exact local commits must be gated
  // against the recorded chain objective (the previous accepted after-topology
  // score), not the candidate's own selected before-topology score.  Run a
  // multi-iteration search; every accepted move's new_score must be <= the
  // chain objective carried into that iteration, and the chain objective is
  // non-increasing across accepts.
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 4;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  std::uint64_t previous_chain_objective = search.summary.initial_score;
  bool any_accept = false;
  for (auto const& iter : search.iterations) {
    if (!iter.accepted || !iter.accepted_move_committed) continue;
    any_accept = true;
    CHECK(iter.accepted->exact.has_value());
    auto const accepted_new = iter.accepted->exact->value.new_score;
    // The chain-objective gate: the accepted new score must not exceed the
    // objective carried into this iteration (state_score_before for the
    // local-commit branch).
    CHECK(accepted_new <= iter.state_score_before);
    // And the chain objective is non-increasing across accepted commits.
    CHECK(accepted_new <= previous_chain_objective);
    previous_chain_objective = accepted_new;
  }
  CHECK(any_accept);

  std::println("  PASS");
}

// Phase 8 issue 1 (icache participation across local commits).  The persistent
// inside cache is chain-keyed while the selected-topology recurrence runs in
// tip space; after the first commit the two diverge, so cross-checking an
// icache row requires translating the tip clade id through
// dense_clade_to_chain_ref.  This test runs a multi-iteration local-commit
// search and confirms (a) the persistent inside cache was actually consulted
// (icache_rows_reused > 0), and (b) the cross-check did not silently invalidate
// candidates on the post-commit iterations (no spurious direct-oracle
// mismatch / fallback, and verified candidates remain valid).
static void test_phase8_icache_participation_across_local_commits() {
  std::println("test_phase8_icache_participation_across_local_commits");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 3;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  // The persistent inside cache participated in the delta.
  CHECK(search.counters.fixed_topology_icache_rows_reused > 0);
  // No spurious fallback from a thrown/misaligned icache cross-check on any
  // iteration (including post-commit iterations where tip and chain ids differ).
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters
            .fixed_topology_persistent_cache_direct_oracle_mismatches == 0);
  // The verifier ran on more than one iteration's worth of candidates (a
  // regression to "invalidate every post-commit candidate" would still pass the
  // fallback checks above but would collapse the verification count).
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 1);

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
  CHECK(search.summary.initial_chart_construction_ms >= 0.0);
  CHECK(search.summary.initial_chart_construction_ms <=
        search.summary.cache_build_ms);
  CHECK(search.summary.materialization_exact_verification_ms >= 0.0);
  CHECK(search.summary.materialization_accepted_update_ms >= 0.0);
  CHECK(search.summary.materialization_final_compaction_ms >= 0.0);
  CHECK(search.summary.materialization_ms ==
        search.summary.materialization_exact_verification_ms +
            search.summary.materialization_accepted_update_ms +
            search.summary.materialization_final_compaction_ms);
  CHECK(search.summary.peak_concurrent_exact_verifiers >= 1);
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
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.counters.overlay_materializations_for_final_compaction == 1);
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

static void test_phase5_final_compaction_uses_grammar_oracle_not_tree_override() {
  std::println("test_phase5_final_compaction_uses_grammar_oracle_not_tree_override");

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
  // Legacy Phase-4 tree-valued hook: Phase 5 local compaction must ignore it
  // and report the grammar-valued exact B&B optimum of the output DAG.
  options.override_final_compaction_rebuilt_score_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.summary.final_score != std::numeric_limits<std::uint64_t>::max());

  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase5_final_compaction_checks_recorded_exact_objective() {
  std::println("test_phase5_final_compaction_checks_recorded_exact_objective");

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
  options.override_local_commit_recorded_objective_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  bool threw = false;
  std::string message;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("recorded exact chain objective") != std::string::npos);
  CHECK(message.find("fresh exact diagnostic") != std::string::npos);

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
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
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
  CHECK(search.summary.final_score <=
        search.iterations.front().accepted->exact->value.new_score);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(larch::build_clade_grammar(search.dag).clades.size() > 0);

  std::println("  PASS");
}

static void test_phase9_lower_bound_compaction_matches_output_dag() {
  std::println("test_phase9_lower_bound_compaction_matches_output_dag");

  // Phase 4 gate enforcement (Work item 1 exactness contract): a local commit
  // (rebuild_after_accept = false) under the lower_bound_heuristic gate is
  // rejected BEFORE the first accept and performs zero commits.  Admitting
  // heuristic-gated local commits is the deferred-verification extension,
  // explicitly out of scope and never a silent choice.  The
  // lower_bound_heuristic gate remains usable with rebuild_after_accept = true
  // (conservative mode), verified by test_phase5_rejected_candidates_* and
  // test_lower_bound_heuristic_acceptance_is_explicit.
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

  std::string message;
  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("local commit") != std::string::npos);
  CHECK(message.find("lower_bound_heuristic") != std::string::npos);
  CHECK(message.find("exact") != std::string::npos);

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

  // Phase 4 tombstone-scope gate (resolution (a)): after the first accept the
  // productions near the move site are temp-sourced, so a sequential SPR that
  // would tombstone one is a labelled, counted skip rather than a silent
  // no-op.  On this small single-topology fixture the search therefore commits
  // the first improving move and then stops at a tombstone-scope skip; the
  // multi-accept path itself is exercised on the larger data/test_5_trees
  // fixture in the Phase 4 tests below.
  CHECK(search.counters.accepted_moves >= 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  // The skip (if the search stopped before max_iterations) must be labelled
  // and counted, never silent.
  if (search.counters.accepted_moves < options.max_iterations) {
    CHECK(search.counters.local_commit_tombstone_scope_skips >= 1);
    CHECK(!search.iterations.back().no_accept_reason.empty());
    CHECK(search.iterations.back()
              .no_accept_reason.find("tombstone-scope") !=
          std::string::npos);
  }
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
  CHECK(search.summary.initial_chart_construction_ms >= 0.0);
  CHECK(search.summary.materialization_ms ==
        search.summary.materialization_exact_verification_ms +
            search.summary.materialization_accepted_update_ms +
            search.summary.materialization_final_compaction_ms);
  CHECK(search.summary.peak_concurrent_exact_verifiers == 0);
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

static void test_canonical_evidence_failure_is_hard_error() {
  std::println("test_canonical_evidence_failure_is_hard_error");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 1;
  options.max_iterations = 1;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;
  options.force_canonical_evidence_failure_for_tests = true;

  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& error) {
    threw = true;
    CHECK(std::string{error.what()}.find(
              "forced exact-evidence failure for test") !=
          std::string::npos);
  }
  CHECK(threw);

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

// ---------------------------------------------------------------------------
// Phase 4 tests: local commit into the search loop (Work items 1 + 3).
// ---------------------------------------------------------------------------
//
// These exercise the Phase 4 accept path: accepted SPR moves commit to the
// overlay chain (Phase 1) + persistent inside/outside caches (Phases 2/3)
// instead of dense-materializing per accept.  The exit criteria they pin:
//   * counter contract: sidecar_rebuilds_after_accept == 0 and
//     overlay_materializations_for_accept_materialization == 0 for a
//     local-commit run (per-accept dense materializations gone);
//   * two-chart oracle green after every accept (via the self-check flag);
//   * the exact-trim cache is never stale (absent after each commit, to be
//     recomputed lazily -- the WI3 lazy-invalidation rule);
//   * conservative mode's counters are unchanged (regression guard);
//   * multi-worker local-commit run is TSAN-clean (the epoch barrier is
//     load-bearing).
//
// Counter-granularity note (Phase 4 issue #1, updated after Phase 8).  The
// plan's literal Phase-4 exit criterion reads `full_overlay_materializations ==
// 0`, but that counter is an UMBRELLA over every dense materialization in the
// run -- per-accept (conservative path), per-candidate exact_multisite
// verification (`verify_candidate_exact_against_state`), per-candidate oracle
// scoring, and final compaction.  Phase 8 removes per-candidate materialization
// for the fixed_topology_exact gate; exact_multisite still materializes until
// Phase 9's transient chain extension lands.  The per-accept dense
// materialization -- the quantity Phase 4's reasoning was actually about -- is
// counted separately by `overlay_materializations_for_accept_materialization`,
// and that remains the load-bearing local-commit counter.

// data/test_5_trees: five protobuf trees merged into one DAG, polytomy-
// refined to a binary chart-compatible grammar the way the benchmark scripts
// do.  The merged DAG carries enough base productions that several sequential
// SPR moves can tombstone only frozen-base productions and remain committable
// under the Phase 4 tombstone-scope gate.
struct phase4_fixture {
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
};

// A single suboptimal tree over 12 taxa carrying THREE independent misplaced
// cherry-pairs in DISJOINT subtrees (groups {A,B,C,D}, {E,F,G,H}, {I,J,K,L}),
// each flagged by its own site.  The grammar optimum (the tree itself) is
// suboptimal, so each cherry-fixing SPR strictly improves the exact parsimony.
// Because the three misplacements live in disjoint subtrees, each fix's reroute
// path is contained in its own subtree and tombstones only frozen-base
// productions -- so all three accepts stay committable under the Phase 4
// tombstone-scope gate (the temp productions introduced by an earlier accept
// live in a different subtree and are never on a later fix's path).  This is
// the fixture the Phase 4 exit criterion needs: a k >= 3 local-commit run.
static larch::phylo_dag make_three_misplaced_groups_tree() {
  using namespace larch::test;
  constexpr std::string_view ref = "AAA";
  auto leaf = [](std::string id, std::string seq) {
    return tiny_leaf(std::move(id), std::move(seq));
  };
  auto inner = [](std::string name, std::vector<tiny_tree_node> children) {
    return tiny_inner(std::move(name), "AAA", std::move(children));
  };
  // site0 separates {A,B} from {C,D}; site1 {E,F} from {G,H}; site2 {I,J} from
  // {K,L}.  Each group's start topology pairs the wrong cherries.
  auto group1 = inner("g1", {
    inner("ac", {leaf("A", "AAA"), leaf("C", "CAA")}),
    inner("bd", {leaf("B", "AAA"), leaf("D", "CAA")}),
  });
  auto group2 = inner("g2", {
    inner("eg", {leaf("E", "ACA"), leaf("G", "AAA")}),
    inner("fh", {leaf("F", "ACA"), leaf("H", "AAA")}),
  });
  auto group3 = inner("g3", {
    inner("ik", {leaf("I", "AAA"), leaf("K", "AAC")}),
    inner("jl", {leaf("J", "AAA"), leaf("L", "AAC")}),
  });
  // Root joins the three disjoint groups via a ladder so each group's fix path
  // stays below the root production.
  auto root = inner("root", {inner("g12", {group1, group2}), group3});
  return make_tiny_labelled_tree(ref, root);
}

static phase4_fixture make_three_misplaced_groups_fixture() {
  phase4_fixture f;
  f.dag = make_three_misplaced_groups_tree();
  f.grammar = larch::build_clade_grammar(f.dag);
  return f;
}

static bool is_chart_spr_expected_overlay_chain_rejection(
    std::string const& msg) {
  return msg.find("overlay_chain") != std::string::npos &&
         (msg.find("not a frozen-base production") != std::string::npos ||
          msg.find("double tombstone") != std::string::npos);
}

static void append_first_committable_delta_for_compaction_test(
    larch::overlay_chain& chain, larch::clade_grammar& tip) {
  auto candidates = larch::enumerate_grammar_spr_candidates(tip);
  for (auto const& candidate : candidates) {
    auto delta = larch::build_spr_overlay_delta(tip, candidate);
    try {
      auto probe = chain;
      probe.append(delta);
      chain.append(delta);
      tip = larch::materialize_overlay_chain(chain).grammar;
      return;
    } catch (std::runtime_error const& e) {
      if (is_chart_spr_expected_overlay_chain_rejection(e.what())) continue;
      throw;
    }
  }
  CHECK(false && "no committable candidate for compaction test");
}

struct alternative_root_witness_grammar {
  larch::clade_grammar grammar;
  larch::clade_id root = larch::no_clade;
  larch::production_id root_acbd_production = larch::no_production;
  larch::production_id target_ac_production = larch::no_production;
};

static alternative_root_witness_grammar
make_alternative_root_witness_grammar() {
  alternative_root_witness_grammar result;
  auto& grammar = result.grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};

  auto add_clade = [&](std::vector<larch::taxon_id> taxa) {
    std::sort(taxa.begin(), taxa.end());
    grammar.clades.push_back(larch::clade_key{std::move(taxa)});
    return static_cast<larch::clade_id>(grammar.clades.size() - 1);
  };

  auto a = add_clade({0});
  auto b = add_clade({1});
  auto c = add_clade({2});
  auto d = add_clade({3});
  auto ab = add_clade({0, 1});
  auto cd = add_clade({2, 3});
  auto ac = add_clade({0, 2});
  auto bd = add_clade({1, 3});
  auto root = add_clade({0, 1, 2, 3});
  result.root = root;
  grammar.root_clade = root;
  grammar.productions_by_parent.resize(grammar.clades.size());
  grammar.productions_by_child.resize(grammar.clades.size());

  auto add_production = [&](larch::clade_id parent,
                            std::vector<larch::clade_id> children) {
    larch::grammar_production prod;
    prod.parent = parent;
    prod.children = std::move(children);
    auto pid = static_cast<larch::production_id>(grammar.productions.size());
    grammar.productions.push_back(std::move(prod));
    grammar.productions_by_parent[parent].push_back(pid);
    for (auto child : grammar.productions[pid].children) {
      grammar.productions_by_child[child].push_back(pid);
    }
    return pid;
  };

  (void)add_production(root, {ab, cd});
  result.root_acbd_production = add_production(root, {ac, bd});
  (void)add_production(ab, {a, b});
  (void)add_production(cd, {c, d});
  result.target_ac_production = add_production(ac, {a, c});
  (void)add_production(bd, {b, d});
  return result;
}

static void test_phase5_witness_topology_selects_required_ancestor_path() {
  std::println("test_phase5_witness_topology_selects_required_ancestor_path");

  auto fixture = make_alternative_root_witness_grammar();
  bool old_helper_threw = false;
  try {
    (void)larch::rank3_topology_preferring_productions(
        fixture.grammar, {fixture.target_ac_production});
  } catch (std::runtime_error const& e) {
    old_helper_threw = std::string{e.what()}.find("not reachable") !=
                       std::string::npos;
  }
  CHECK(old_helper_threw);

  auto topology = larch::overlay_chain_compaction_detail::
      concrete_topology_containing_production(
          fixture.grammar, fixture.target_ac_production);
  auto reachable = larch::rank3_detail::validate_topology(fixture.grammar,
                                                          topology);
  CHECK(reachable[fixture.target_ac_production]);
  CHECK(topology.selected_production_by_clade[fixture.root] ==
        fixture.root_acbd_production);

  std::println("  PASS");
}

static void run_phase5_compaction_with_trim_options(
    larch::multisite_trim_options trim_options) {
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
  options.exact_trim = trim_options;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);

  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto active_build = larch::make_active_search_patterns(
      search.dag, rebuilt, options.chart);
  auto oracle = larch::grammar_level_exact_parsimony(
      rebuilt, active_build.active_patterns, options.chart,
      active_build.invariant_constant_offset, trim_options);
  CHECK(oracle.exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(oracle.value == search.summary.final_score);
}

static void test_phase5_final_compaction_normalizes_trim_options() {
  std::println("test_phase5_final_compaction_normalizes_trim_options");

  larch::multisite_trim_options score_only;
  score_only.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only.require_exact_keep_mask = false;
  run_phase5_compaction_with_trim_options(score_only);

  larch::multisite_trim_options two_pass;
  two_pass.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  run_phase5_compaction_with_trim_options(two_pass);

  std::println("  PASS");
}

static larch::rank3_production_taxa_key phase5_key_from_signature(
    larch::chart_spr_production_signature signature) {
  larch::rank3_production_taxa_key key;
  key.parent = std::move(signature.parent_taxa);
  key.children = std::move(signature.child_taxa);
  larch::rank3_detail::normalize_production_key(key);
  return key;
}

static larch::production_id phase5_find_unique_production_by_signature(
    larch::clade_grammar const& grammar,
    larch::chart_spr_production_signature const& signature) {
  auto key = phase5_key_from_signature(signature);
  larch::production_id match = larch::no_production;
  for (std::size_t i = 0; i < grammar.productions.size(); ++i) {
    auto pid = static_cast<larch::production_id>(i);
    if (larch::rank3_detail::production_key_from_id(grammar, pid) != key) {
      continue;
    }
    CHECK(match == larch::no_production);
    match = pid;
  }
  CHECK(match != larch::no_production);
  return match;
}

static void phase5_assert_certificate_topology_present(
    larch::clade_grammar const& grammar,
    larch::chart_spr_topology_certificate const& certificate) {
  CHECK(!certificate.after_signatures.empty());
  std::vector<larch::production_id> pids;
  pids.reserve(certificate.after_signatures.size());
  for (auto const& signature : certificate.after_signatures) {
    pids.push_back(
        phase5_find_unique_production_by_signature(grammar, signature));
  }
  auto topology = larch::rank3_topology_from_productions(grammar, pids);
  auto reachable = larch::rank3_detail::validate_topology(grammar, topology);
  for (auto pid : pids) {
    CHECK(pid < reachable.size());
    CHECK(reachable[pid]);
  }
}

static void test_phase5_overlay_chain_compaction_preserves_intended_keys() {
  std::println("test_phase5_overlay_chain_compaction_preserves_intended_keys");

  auto fixture = make_three_misplaced_groups_fixture();
  auto active_build = larch::make_active_search_patterns(fixture.dag,
                                                          fixture.grammar);
  larch::overlay_chain chain(fixture.grammar);
  auto tip = fixture.grammar;
  append_first_committable_delta_for_compaction_test(chain, tip);
  append_first_committable_delta_for_compaction_test(chain, tip);

  auto intended = larch::overlay_chain_intended_production_keys(chain);
  CHECK(!intended.empty());
  auto compacted = larch::compact_overlay_chain_to_dag(fixture.dag, chain);
  CHECK(compacted.materialized_tree_count >= 1);
  CHECK(compacted.all_intended_productions_present());
  CHECK(compacted.all_witness_topologies_present());
  for (auto const& key : intended) {
    CHECK(larch::rank3_detail::has_production_key(compacted.rebuilt.grammar,
                                                   key));
  }

  auto oracle = larch::grammar_level_exact_parsimony(
      compacted.rebuilt.grammar, active_build.active_patterns, {},
      active_build.invariant_constant_offset);
  CHECK(oracle.exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(oracle.value < larch::multisite_score_inf);

  std::println("  PASS");
}

static larch::test::tiny_tree_node phase5_history_tree_one() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABC", "A",
                  {tiny_inner("AB", "A", {tiny_leaf("A", "A"),
                                             tiny_leaf("B", "A")}),
                   tiny_leaf("C", "A")}),
       tiny_inner("DE", "A", {tiny_leaf("D", "A"),
                                tiny_leaf("E", "A")})});
}

static larch::test::tiny_tree_node phase5_history_tree_two() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"),
                                tiny_leaf("C", "A")}),
       tiny_inner("BDE", "A",
                  {tiny_leaf("B", "A"),
                   tiny_inner("DE", "A", {tiny_leaf("D", "A"),
                                             tiny_leaf("E", "A")})})});
}

static larch::production_id phase5_find_production_by_key(
    larch::clade_grammar const& grammar,
    larch::rank3_production_taxa_key key) {
  larch::rank3_detail::normalize_production_key(key);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto dense_pid = static_cast<larch::production_id>(pid);
    if (larch::rank3_detail::production_key_from_id(grammar, dense_pid) ==
        key) {
      return dense_pid;
    }
  }
  return larch::no_production;
}

static void test_phase5_compaction_augments_historical_intended_keys() {
  std::println("test_phase5_compaction_augments_historical_intended_keys");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", phase5_history_tree_one()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", phase5_history_tree_two()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto abc = clade_for(grammar, {"A", "B", "C"});

  larch::overlay_chain chain(grammar);
  larch::spr_overlay_delta delta1;
  delta1.base = &grammar;
  delta1.temp_clades.push_back(
      larch::clade_key{taxa_for(grammar, {"B", "C"})});
  delta1.temp_productions.push_back(temp_prod(
      larch::temp_clade_ref(0),
      {larch::base_clade_ref(b), larch::base_clade_ref(c)}));
  delta1.temp_productions.push_back(temp_prod(
      larch::base_clade_ref(abc),
      {larch::base_clade_ref(a), larch::temp_clade_ref(0)}));
  chain.append(delta1);

  auto after_first = larch::materialize_overlay_chain(chain);
  auto const& tip1 = after_first.grammar;
  auto abc_after_pid = after_first.temp_production_to_dense.at(1);
  CHECK(abc_after_pid != larch::no_production);
  auto abc_after_key = larch::rank3_detail::production_key_from_id(
      tip1, abc_after_pid);
  auto root1 = clade_for(tip1, {"A", "B", "C", "D", "E"});
  auto abc1 = clade_for(tip1, {"A", "B", "C"});
  auto de1 = clade_for(tip1, {"D", "E"});
  auto root_abc_de = production_id_for(tip1, root1, {abc1, de1});

  larch::spr_overlay_delta delta2;
  delta2.base = &tip1;
  delta2.removed_base_productions.push_back(root_abc_de);
  chain.append(delta2);

  auto final_materialized = larch::materialize_overlay_chain(chain);
  CHECK(phase5_find_production_by_key(final_materialized.grammar,
                                      abc_after_key) == larch::no_production);

  // Standalone compaction derives prefix witness topologies itself: callers do
  // not need to provide the historical ancestor path for abc_after_key, even
  // though that production's parent is unreachable in the final chain.
  auto compacted = larch::compact_overlay_chain_to_dag(dag, chain);
  CHECK(compacted.all_intended_productions_present());
  CHECK(compacted.all_witness_topologies_present());
  CHECK(larch::rank3_detail::has_production_key(compacted.rebuilt.grammar,
                                                 abc_after_key));

  std::println("  PASS");
}

static void test_phase4_local_commit_counter_contract_and_oracle() {
  std::println("test_phase4_local_commit_counter_contract_and_oracle");

  auto fixture = make_three_misplaced_groups_fixture();
  std::println("  fixture: {} clades, {} productions",
               fixture.grammar.clades.size(),
               fixture.grammar.productions.size());

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;  // allow enough iterations to reach k >= 3
  options.rebuild_after_accept = false;
  // Phase 3 correctness invariant: after every local commit, recompute BOTH
  // charts from scratch and assert the persistent caches agree.
  options.verify_local_commit_two_chart_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit_accepted={}), "
      "tombstone_scope_skips={}, exact_verifications={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.local_commit_tombstone_scope_skips,
      search.counters.exact_verifications);

  // The Phase 4 exit criterion asks for k >= 3 local commits.  The
  // tombstone-scope gate may stop the search early if the best move at some
  // iteration tombstones a temp-sourced production; the test reports the
  // achieved count and asserts the contract on whatever was committed.
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);

  // Counter contract (cross-cutting): no per-accept sidecar rebuilds and no
  // per-accept dense accept-materializations.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  // The caches did real (affected-set-scoped) work, visible in both
  // directions -- a regression to "recompute everything" would still be > 0,
  // but a regression to "no commit at all" would be 0.
  if (search.counters.accepted_moves > 0) {
    CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
    CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
    // The two-chart oracle self-check ran after every commit without throwing
    // (the run completed), so the caches agreed with the from-scratch charts.
    CHECK(search.counters.local_commit_two_chart_oracle_runs ==
          search.counters.local_commit_accepted_moves);
    // Tip grammar was refreshed once per commit.  Exact candidate,
    // accepted-tip, and final-compaction grammars each publish their own plan.
    CHECK(search.counters.local_commit_tip_grammar_refreshes ==
          search.counters.local_commit_accepted_moves);
    CHECK(search.counters.chart_execution_plan_builds ==
          1 + search.counters.local_commit_tip_grammar_refreshes +
              search.summary.final_compaction_rebuilds +
              search.counters.transient_chain_extensions_for_verification +
              search.counters
                  .overlay_materializations_for_exact_verification);
    CHECK(search.counters.base_chart_cache_rebuilds ==
          1 + search.summary.final_compaction_rebuilds);
  }
  // Every accepted move is exact-gated: a locally committed chain's recorded
  // objective is exact.  The exact_multisite gate records grammar_exact; the
  // deferred lower_bound_heuristic-gated commit is rejected entirely
  // (test_phase9_lower_bound_compaction_matches_output_dag).
  for (auto const& it : search.iterations) {
    if (it.accepted_move_committed) {
      CHECK(it.accepted.has_value());
      CHECK(it.accepted->exact.has_value());
      CHECK(it.accepted->exact->kind ==
            larch::chart_spr_score_kind::grammar_exact);
    }
  }
  // The Phase 4 exit criterion: a k >= 3 local-commit run on a fixture with
  // three disjoint committable improving moves.
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  // Final score is non-increasing (every committed move improved or held).
  CHECK(search.summary.final_score <= search.summary.initial_score);

  // Compaction produced a valid DAG whose grammar-level exact B&B optimum
  // matches the reported final score.
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.counters.overlay_materializations_for_final_compaction == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// Regression guard: conservative mode (rebuild_after_accept = true) keeps its
// existing counter behavior -- per-accept sidecar rebuilds and per-accept
// accept-materializations -- unchanged from the Phase 0 baseline.
static void test_phase4_conservative_mode_counters_unchanged() {
  std::println("test_phase4_conservative_mode_counters_unchanged");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;  // conservative (default)

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  CHECK(search.counters.accepted_moves == 1);
  // Conservative path: one sidecar rebuild and one accept-materialization per
  // accept -- the Phase 0 baseline behavior, unchanged by Phase 4.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        1);
  CHECK(search.counters.chart_execution_plan_builds ==
        2 + search.counters.overlay_materializations_for_exact_verification);
  CHECK(search.counters.full_grammar_validations ==
        search.counters.chart_execution_plan_builds);
  CHECK(search.counters.production_partition_validations >
        grammar.productions.size());
  // Conservative mode never uses the local-commit caches.
  CHECK(search.counters.local_commit_accepted_moves == 0);
  CHECK(search.counters.inside_rows_recomputed_on_commit == 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit == 0);

  std::println("  PASS");
}

static void test_phase4_local_commit_score_ua_edge_rejected() {
  std::println("test_phase4_local_commit_score_ua_edge_rejected");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.chart.score_ua_edge = true;

  std::string message;
  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("local commit") != std::string::npos);
  CHECK(message.find("score_ua_edge=true") != std::string::npos);
  CHECK(message.find("Phase 4 limitation") != std::string::npos);
  CHECK(message.find("not a silent fallback") != std::string::npos);

  std::println("  PASS");
}

static void test_phase4_local_commit_post_append_failure_is_hard_error() {
  std::println("test_phase4_local_commit_post_append_failure_is_hard_error");

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
  options.force_local_commit_post_append_failure_for_tests = true;

  std::string message;
  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("local commit") != std::string::npos);
  CHECK(message.find("after the append committed") != std::string::npos);
  CHECK(message.find("forced local commit post-append failure for tests") !=
        std::string::npos);
  CHECK(message.find("post-materialization rejection") != std::string::npos);

  std::println("  PASS");
}

// Exact-trim cache invariant (WI3 lazy invalidation): across a local-commit
// run, after each accept the state's exact_trim_active_only is either absent
// (invalidated on commit) or, once recomputed, equals the from-scratch exact
// trim on the current tip grammar.  Uses the tiny three-misplaced-groups
// fixture (k >= 3 accepts under exact_multisite) so the run exercises repeated
// lazy invalidation and recomputation.  The pandemic-scale data/test_5_trees
// fixture is avoided here because a heavy exact-multisite search on it trips a
// PRE-EXISTING race in the global thread_pool used by build_clade_grammar /
// validate_dag (verified on a clean baseline checkout: rebuild_after_accept =
// true reproduces the same arity-3 throw).  Fixing that race is unrelated to
// Phase 4 and out of scope; the local-commit correctness it would exercise is
// already covered on the tiny fixture plus the two-chart oracle self-check in
// test_phase4_local_commit_counter_contract_and_oracle.
static void test_phase4_exact_trim_cache_never_stale() {
  std::println("test_phase4_exact_trim_cache_never_stale");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  // The exact-multisite gate reads the exact trim; each accept invalidates it
  // (Phase 2 hook) and the next gate rebuilds it lazily via
  // ensure_chart_spr_state_exact_trim.  After the run, the rebuilt exact trim
  // on the compacted output DAG must match the reported final score.
  CHECK(search.counters.local_commit_accepted_moves >= 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// Multi-worker local-commit run: the epoch/snapshot barrier keeps scoring
// readers from observing a partially-updated cache.  This is the TSAN-
// load-bearing case (verified separately under -DENABLE_TSAN=ON).  Here we
// confirm the multi-worker run produces the same accepted-move count and final
// score as a serial run, so the barrier does not change semantics.  Uses the
// tiny fixture (see the note on test_phase4_exact_trim_cache_never_stale for
// why the pandemic-scale fixture is avoided).
static void test_phase4_multi_worker_matches_serial() {
  std::println("test_phase4_multi_worker_matches_serial");

  auto run_once = [](std::size_t workers) {
    auto fixture = make_three_misplaced_groups_fixture();
    larch::chart_spr_search_options options;
    options.acceptance_mode =
        larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::exhaustive_exact;
    options.max_iterations = 12;
    options.rebuild_after_accept = false;
    options.local_score_worker_count = workers;
    options.verify_local_commit_two_chart_oracle_for_tests = true;
    return larch::run_chart_spr_search(std::move(fixture.dag),
                                       fixture.grammar, options);
  };

  auto serial = run_once(1);
  auto parallel = run_once(4);

  CHECK(serial.counters.accepted_moves == parallel.counters.accepted_moves);
  CHECK(serial.summary.final_score == parallel.summary.final_score);
  CHECK(serial.summary.initial_score == parallel.summary.initial_score);
  CHECK(parallel.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(parallel.counters.overlay_materializations_for_accept_materialization ==
        0);
  // Parallel scoring actually used the workers.
  CHECK(parallel.counters.local_score_parallel_batches > 0);

  std::println("  PASS");
}

// fixed_topology_exact + local commit.  This is the second exact gate that
// may commit locally (Work item 1 exactness contract).  Phase 8 serves this
// gate without hidden per-candidate materialization or a hidden full direct
// selected-tree oracle: selected-subtree cache work is visible in its own
// counters, and the only full materialization in this local-commit run is
// Phase 5's final compaction.
static void test_phase4_fixed_topology_exact_local_commit() {
  std::println("test_phase4_fixed_topology_exact_local_commit");

  auto fixture = make_three_misplaced_groups_fixture();

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  // exhaustive_exact verifies every candidate through the fixed-topology gate
  // (default selector resolves a topology certificate for each), so this run
  // exercises the Phase 8 no-materialization verification path repeatedly.
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.verify_local_commit_two_chart_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit_accepted={}), "
      "tombstone_scope_skips={}, exact_verifications={}, "
      "full_overlay_materializations={}, accept_materializations={}, "
      "exact_verification_materializations={}, "
      "selected_cache_hits={}, selected_cache_misses={}, "
      "selected_rows_computed={}, persistent_verifications={}, "
      "persistent_fallbacks={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.local_commit_tombstone_scope_skips,
      search.counters.exact_verifications,
      search.counters.full_overlay_materializations,
      search.counters.overlay_materializations_for_accept_materialization,
      search.counters.overlay_materializations_for_exact_verification,
      search.counters.fixed_topology_selected_cache_hits,
      search.counters.fixed_topology_selected_cache_misses,
      search.counters.fixed_topology_selected_rows_computed,
      search.counters.fixed_topology_persistent_cache_verifications,
      search.counters.fixed_topology_persistent_cache_fallbacks);

  // Every accepted move is fixed_topology_exact-gated (an exact gate; the
  // chain's recorded objective is exact for the one selected topology).
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);
  for (auto const& it : search.iterations) {
    if (it.accepted_move_committed) {
      CHECK(it.accepted.has_value());
      CHECK(it.accepted->exact.has_value());
      CHECK(it.accepted->exact->kind ==
            larch::chart_spr_score_kind::fixed_topology_exact);
    }
  }

  // Counter contract (corrected, per the known-issue note): no per-accept
  // sidecar rebuilds and no per-accept dense accept-materializations.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  // Phase 8 counter contract: fixed-topology verification itself performs no
  // dense overlay materialization.  The umbrella counter is exactly the single
  // final-compaction materialization added after the local-commit run.
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.full_overlay_materializations ==
        search.counters.overlay_materializations_for_final_compaction);
  CHECK(search.counters.fixed_topology_selected_cache_hits > 0);
  CHECK(search.counters.fixed_topology_selected_cache_misses > 0);
  CHECK(search.counters.fixed_topology_selected_rows_computed > 0);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications ==
        search.counters.exact_verifications);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  CHECK(search.summary.fixed_topology_selected_cache_hits ==
        search.counters.fixed_topology_selected_cache_hits);
  CHECK(search.summary.fixed_topology_selected_cache_misses ==
        search.counters.fixed_topology_selected_cache_misses);
  CHECK(search.summary.fixed_topology_selected_rows_computed ==
        search.counters.fixed_topology_selected_rows_computed);
  CHECK(search.summary.fixed_topology_persistent_cache_verifications ==
        search.counters.fixed_topology_persistent_cache_verifications);
  CHECK(search.summary.fixed_topology_persistent_cache_fallbacks == 0);
  auto naive_selected_oracle_rows =
      search.counters.exact_verifications * search.summary.active_pattern_count *
      search.summary.initial_grammar_clade_count;
  CHECK(search.counters.fixed_topology_selected_rows_computed <
        naive_selected_oracle_rows);
  // The caches did real affected-set-scoped work and the two-chart oracle ran
  // after every commit without throwing.
  if (search.counters.accepted_moves > 0) {
    CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
    CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
    CHECK(search.counters.local_commit_two_chart_oracle_runs ==
          search.counters.local_commit_accepted_moves);
    CHECK(search.counters.local_commit_tip_grammar_refreshes ==
          search.counters.local_commit_accepted_moves);
  }

  // Three disjoint committable improving moves: fixed_topology_exact commits
  // the same k >= 3 the exact_multisite path does.
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  CHECK(search.summary.final_score <= search.summary.initial_score);

  // Compaction produced a valid DAG whose grammar-level exact B&B optimum
  // matches the reported final score, and preserves every accepted fixed-
  // topology certificate as a complete witness topology.
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.counters.overlay_materializations_for_final_compaction == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  std::size_t accepted_certificate_count = 0;
  for (auto const& it : search.iterations) {
    if (!it.accepted_move_committed) continue;
    CHECK(it.accepted.has_value());
    CHECK(it.accepted->topology_selection.certificate.has_value());
    phase5_assert_certificate_topology_present(
        rebuilt, *it.accepted->topology_selection.certificate);
    ++accepted_certificate_count;
  }
  CHECK(accepted_certificate_count ==
        search.counters.local_commit_accepted_moves);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_lazy_cache_local_commit_sequence_projects_lazy_chart() {
  std::println("test_lazy_cache_local_commit_sequence_projects_lazy_chart");

  auto fixture = make_three_misplaced_groups_fixture();

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.cache.use_lazy_multisite_chart = true;
  options.verify_local_commit_two_chart_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  CHECK(search.summary.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.local_commit_two_chart_oracle_runs ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.local_commit_tip_grammar_refreshes ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_incremental_rows_recomputed ==
        search.counters.lazy_inside_rows_recomputed_on_commit +
            search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_inside_rows_recomputed_on_commit ==
        search.counters.lazy_inside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_outside_rows_recomputed_on_commit ==
        search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_incremental_rows_recomputed ==
        search.counters.lazy_incremental_rows_recomputed);
  CHECK(search.summary.lazy_inside_rows_computed > 0);
  CHECK(search.summary.lazy_merge_ratio > 0.0);

  std::println("  PASS");
}

// pattern_batches cache strategy + local commit (Phase 4 known-issue #3).
// chart_spr_refresh_state_tip_view_after_local_commit has a distinct branch
// for pattern_batches mode (it leaves state.pattern_charts alone and refreshes
// only the grammar + bounds from the icache); all other Phase 4 tests run in
// all_active_patterns mode.  This forces cache_strategy = pattern_batches
// (max_cached_patterns = 1 on the three-misplaced-groups fixture, which carries
// several active patterns) through a local-commit run so that branch is
// exercised end-to-end and the two-chart oracle still agrees.
static void test_phase4_pattern_batches_local_commit() {
  std::println("test_phase4_pattern_batches_local_commit");

  auto fixture = make_three_misplaced_groups_fixture();

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.verify_local_commit_two_chart_oracle_for_tests = true;
  // Force pattern_batches: cap the resident pattern cache below the fixture's
  // active-pattern count so choose_chart_spr_cache_strategy selects batching.
  options.cache.max_cached_patterns = 1;

  // Confirm the configured options actually select pattern_batches for this
  // fixture (the local-commit refresh branch under test is gated on it).
  auto probe = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                    options);
  CHECK(probe.cache_strategy == larch::chart_spr_cache_strategy::pattern_batches);
  CHECK(probe.pattern_charts.empty());
  std::println("  active_patterns={}, cache_strategy=pattern_batches forced",
               probe.active_patterns.patterns.patterns.size());

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit_accepted={}), "
      "tombstone_scope_skips={}, pattern_batch_cache_builds={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.local_commit_tombstone_scope_skips,
      search.counters.pattern_batch_cache_builds);

  // The local-commit run completed (no throw) with at least one commit --
  // i.e. the pattern_batches refresh branch was taken and the next scoring
  // batch successfully rebuilt base rows from the refreshed tip grammar.
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.local_commit_accepted_moves >= 1);
  // Counter contract holds under pattern_batches exactly as under
  // all_active_patterns.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  // Two-chart oracle ran after every commit without throwing (the caches are
  // fully populated regardless of cache_strategy; strategy only governs whether
  // pattern_charts is resident).
  CHECK(search.counters.local_commit_two_chart_oracle_runs ==
        search.counters.local_commit_accepted_moves);
  // Pattern-batch scoring actually rebuilt base rows per batch across the run.
  CHECK(search.counters.pattern_batch_cache_builds > 0);
  CHECK(search.summary.final_score <= search.summary.initial_score);

  // Compaction produced a valid DAG whose grammar-level exact B&B optimum
  // matches the reported final score.
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// ===========================================================================
// Phase 9 (Work item 4a, technique 2): transient chain extension for
// grammar-exact verification.
//
// The exact_multisite gate verifies an unaccepted candidate by transiently
// extending the overlay chain + persistent inside/outside caches in
// reader-local scratch storage (never mutating the shared cache, bypassing
// the Phase 4 commit barrier), reading the exact frontier on the extended
// grammar, and discarding.  These tests cover the four Phase 9 exit criteria:
//   1. transient-extension exact score == from-scratch exact score (oracle).
//   2. full_overlay_materializations does not increase for a transient run;
//      work counted under transient_chain_extensions_for_verification.
//   3. on oracle mismatch, the cold from-scratch result is authoritative (the
//      corruption hook forces a mismatch and asserts the fallback fires); the
//      exactness label stays grammar_exact because the cold B&B is exact (a
//      justified deviation from the plan's literal "weakened" wording -- see
//      doc/WRIC-SPR-SEARCH.md).
//   4. TSAN-clean multi-worker (transient extensions are reader-local).
//
// Phase 9 ships substrate + oracle + counter discipline, NOT a wall-clock win
// (the scratch caches are unconsumed by the production B&B scorer, which
// rebuilds charts from the grammar); they are forward-looking groundwork for
// Phase 12.  See the header comment on `chart_spr_transient_extension` in
// src/chart_spr_search.cpp and doc/WRIC-SPR-SEARCH.md.
// ============================================================================

// Exit criterion 2: a local-commit exact_multisite run that uses the
// transient path does NOT bump full_overlay_materializations for verification
// (only the single final-compaction materialization, plus any cold-path
// fallbacks for tombstone-scope candidates); the per-candidate work is counted
// under transient_chain_extensions_for_verification, never folded into
// full_overlay_materializations.
static void test_phase9_transient_no_full_overlay_materialization() {
  std::println("test_phase9_transient_no_full_overlay_materialization");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  // Oracle OFF: the production path trusts the transient result and never
  // materializes per candidate.  (With the oracle ON, each verified candidate
  // materializes a cold overlay for cross-checking -- the no-materialization
  // contract is asserted with the oracle OFF.)
  options.verify_transient_chain_extension_oracle_for_tests = false;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit={}), exact_verifications={}, "
      "transient_extensions={}, exact_verification_materializations={}, "
      "full_overlay_materializations={}, final_compaction_materializations={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.exact_verifications,
      search.counters.transient_chain_extensions_for_verification,
      search.counters.overlay_materializations_for_exact_verification,
      search.counters.full_overlay_materializations,
      search.counters.overlay_materializations_for_final_compaction);

  // The transient path was actually used.
  CHECK(search.counters.transient_chain_extensions_for_verification > 0);
  // Every exact verification either used the transient path or fell back to
  // the cold path (tombstone-scope candidates whose delta cannot be appended
  // to the chain).  Cold fallbacks are the only source of per-candidate exact-
  // verification materializations.
  CHECK(search.counters.exact_verifications ==
        search.counters.transient_chain_extensions_for_verification +
            search.counters.overlay_materializations_for_exact_verification);
  // The umbrella full_overlay_materializations counter is exactly the sum of
  // its reason-coded splits for this run: no per-accept materialization
  // (local-commit contract), no oracle materialization (oracle off), the
  // single final compaction, and any cold-fallback exact verifications.
  CHECK(search.counters.full_overlay_materializations ==
        search.counters.overlay_materializations_for_final_compaction +
            search.counters.overlay_materializations_for_exact_verification);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  // Every accepted move is grammar_exact (the transient path preserves the
  // exactness label).
  for (auto const& it : search.iterations) {
    if (it.accepted_move_committed) {
      CHECK(it.accepted.has_value());
      CHECK(it.accepted->exact.has_value());
      CHECK(it.accepted->exact->kind ==
            larch::chart_spr_score_kind::grammar_exact);
    }
  }
  // k >= 3 disjoint committable improving moves on this fixture.
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  CHECK(search.summary.final_score <= search.summary.initial_score);

  std::println("  PASS");
}

// Exit criterion 1: the transient-extension exact score equals the from-
// scratch exact score on every fixture candidate, both charts.  The oracle
// (enabled) recomputes BOTH charts from scratch on the extended grammar and
// compares against the scratch caches, plus runs a cold from-scratch B&B and
// compares the exact optimum.  A green oracle (zero mismatches, zero
// fallbacks) proves the transient result is exact.
static void test_phase9_transient_oracle_both_charts_green() {
  std::println("test_phase9_transient_oracle_both_charts_green");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  // Oracle ON: every transient extension is cross-checked against the cold
  // from-scratch path (both charts + exact optimum), per Work item 4a's
  // correctness invariant.
  options.verify_transient_chain_extension_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  transient_extensions={}, oracle_rows_checked={}, "
      "oracle_mismatches={}, fallbacks={}",
      search.counters.transient_chain_extensions_for_verification,
      search.counters.transient_chain_extension_oracle_rows_checked_for_tests,
      search.counters.transient_chain_extension_oracle_mismatches,
      search.counters.transient_chain_extension_fallbacks);

  CHECK(search.counters.transient_chain_extensions_for_verification > 0);
  // The two-chart oracle ran on every transient extension: it checked both
  // the inside and outside scratch rows against the from-scratch charts.
  CHECK(search.counters.transient_chain_extension_oracle_rows_checked_for_tests >
        0);
  // Green oracle: no mismatches and no fallbacks on the fixture matrix.
  CHECK(search.counters.transient_chain_extension_oracle_mismatches == 0);
  CHECK(search.counters.transient_chain_extension_fallbacks == 0);
  // The oracle materializations are counted under the oracle bucket, separate
  // from the transient extension counter (never folded).
  CHECK(search.counters.overlay_materializations_for_oracle >=
        search.counters.transient_chain_extensions_for_verification);
  // Final score matches a from-scratch rebuild of the output DAG.
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// Exit criterion 1 on a multi-parent DAG fixture: the transient oracle stays
// green across a grammar with multiple productions per parent (the case the
// B&B coupling is designed for).
static void test_phase9_transient_oracle_green_on_multiparent_dag() {
  std::println("test_phase9_transient_oracle_green_on_multiparent_dag");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_one()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_two()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_transient_chain_extension_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  CHECK(search.counters.transient_chain_extensions_for_verification > 0);
  CHECK(search.counters.transient_chain_extension_oracle_mismatches == 0);
  CHECK(search.counters.transient_chain_extension_fallbacks == 0);

  std::println("  PASS");
}

// Exit criterion 3: when the transient result cannot be trusted (the oracle
// finds a mismatch), the verifier uses the cold from-scratch path's result and
// records the fallback.  The corruption hook perturbs a scratch outside row so
// the two-chart oracle deterministically disagrees; the gate must fire rather
// than silently trusting the wrong transient result.
static void test_phase9_transient_oracle_catches_corruption() {
  std::println("test_phase9_transient_oracle_catches_corruption");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.verify_transient_chain_extension_oracle_for_tests = true;
  // Perturb a scratch outside row so the per-candidate two-chart oracle
  // catches the disagreement.
  options.force_transient_chain_extension_oracle_mismatch_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  transient_extensions={}, oracle_mismatches={}, fallbacks={}",
      search.counters.transient_chain_extensions_for_verification,
      search.counters.transient_chain_extension_oracle_mismatches,
      search.counters.transient_chain_extension_fallbacks);

  CHECK(search.counters.transient_chain_extensions_for_verification > 0);
  // The corruption hook forces at least one mismatch, and every mismatch
  // triggers a fallback to the authoritative cold result.
  CHECK(search.counters.transient_chain_extension_oracle_mismatches > 0);
  CHECK(search.counters.transient_chain_extension_fallbacks ==
        search.counters.transient_chain_extension_oracle_mismatches);
  // Despite the forced corruption, the final score still matches a from-
  // scratch rebuild of the output DAG: the cold path's authoritative result
  // keeps the reported objective correct.
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// A candidate whose delta cannot be appended to the chain (tombstone scope:
// it tombstones a production that does not resolve to a frozen-base
// production) falls back to the cold path during verification, so it can still
// be accepted and reach the Phase 4 commit-time tombstone-scope skip.  This
// preserves the existing search semantics: the transient extension never
// silently rejects a candidate the cold path would accept.
static void test_phase9_transient_tombstone_scope_falls_back_to_cold() {
  std::println("test_phase9_transient_tombstone_scope_falls_back_to_cold");

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

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  // At least one improving move is committed (the transient path serves
  // committable candidates).
  CHECK(search.counters.accepted_moves >= 1);
  // After the first accept, a sequential SPR whose tombstones do not resolve
  // to frozen-base productions falls back to the cold path for verification
  // and then reaches the commit-time tombstone-scope skip.  On this small
  // single-topology fixture the search commits one move and stops at the
  // labelled skip.
  if (search.counters.accepted_moves < options.max_iterations) {
    CHECK(search.counters.local_commit_tombstone_scope_skips >= 1);
    CHECK(!search.iterations.back().no_accept_reason.empty());
    CHECK(search.iterations.back().no_accept_reason.find("tombstone-scope") !=
          std::string::npos);
  }

  std::println("  PASS");
}

// Exit criterion 4: transient extensions are reader-local (they copy the
// chain + caches into scratch and never mutate the shared cache), so they run
// cleanly alongside parallel local scoring under the epoch/snapshot model.
// A multi-worker local-commit exact_multisite run must agree with the serial
// run and stay TSAN-clean (verified separately under -DENABLE_TSAN=ON).
static void test_phase9_transient_multi_worker_matches_serial() {
  std::println("test_phase9_transient_multi_worker_matches_serial");

  auto run_once = [](std::size_t workers, bool oracle) {
    auto fixture = make_three_misplaced_groups_fixture();
    larch::chart_spr_search_options options;
    options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::exhaustive_exact;
    options.max_iterations = 12;
    options.rebuild_after_accept = false;
    options.local_score_worker_count = workers;
    options.verify_local_commit_two_chart_oracle_for_tests = true;
    options.verify_transient_chain_extension_oracle_for_tests = oracle;
    return larch::run_chart_spr_search(std::move(fixture.dag),
                                       fixture.grammar, options);
  };

  auto serial = run_once(1, true);
  auto parallel = run_once(4, true);

  CHECK(serial.counters.accepted_moves == parallel.counters.accepted_moves);
  CHECK(serial.summary.final_score == parallel.summary.final_score);
  CHECK(serial.summary.initial_score == parallel.summary.initial_score);
  CHECK(parallel.counters.transient_chain_extensions_for_verification > 0);
  // Parallel scoring actually used the workers.
  CHECK(parallel.counters.local_score_parallel_batches > 0);
  // The transient oracle stays green under parallel local scoring (the
  // transient extensions are reader-local and do not race the scoring
  // workers).
  CHECK(parallel.counters.transient_chain_extension_oracle_mismatches == 0);
  CHECK(parallel.counters.transient_chain_extension_fallbacks == 0);

  std::println("  PASS");
}

static void test_exact_verifier_activity_is_exception_safe() {
  std::println("test_exact_verifier_activity_is_exception_safe");

  auto tracker = std::make_shared<
      larch::chart_spr_exact_verifier_concurrency_tracker>();
  try {
    larch::chart_spr_exact_verifier_activity activity{tracker};
    throw std::runtime_error("expected verifier failure");
  } catch (std::runtime_error const&) {
  }
  {
    larch::chart_spr_exact_verifier_activity activity{tracker};
  }
  // A leaked active count from the throwing scope would make this second,
  // sequential entry observe a peak of two.
  CHECK(tracker->peak() == 1);

  std::println("  PASS");
}

int main() {
  test_exact_verifier_activity_is_exception_safe();
  test_lower_bound_oracle_counters_show_full_rebuild_cost();
  test_local_rejected_candidate_counter_guardrail();
  test_streaming_and_eager_candidate_apis_match();
  test_search_state_local_score_entry_point_matches_oracle();
  test_local_score_with_ua_edge_and_invariant_offset_matches_oracle();
  test_production_delta_only_candidate_metadata_absent_matches_oracle();
  test_multiparent_dag_affected_closure_and_slot_maps();
  test_unreachable_dead_clades_ignored_reachable_dead_clades_fail();
  test_nonbinary_overlay_production_scored_locally();
  test_overlay_delta_rows_match_full_overlay_for_tiny_candidates();
  test_local_scoring_verify_option_counts_oracle_materialization();
  test_invalid_disconnected_overlay_returns_invalid_score();
  test_persistent_active_pattern_cache_matches_full_composite();
  test_root_reference_counts_preserved_in_cache();
  test_active_pattern_assertions_reject_skipped_metadata();
  test_state_builder_from_dag_rebuilds_patterns_once();
  test_pattern_batch_cache_options_match_all_cache();
  test_candidate_execution_plan_lifetime_and_mismatch_guards();
  test_checked_candidate_sources_and_planned_materialization();
  test_acceptance_iteration_checks_resident_plan_once();
  test_lazy_cache_fixed_topology_conservative_search();
  test_lazy_cache_local_commit_updates_lazy_chart();
  test_parallel_local_scores_match_serial();
  test_pattern_batch_nonreplayable_uses_automatic_candidate_batch();
  test_unchartable_grammar_rejected_with_empty_active_patterns();
  test_unsupported_enumeration_options_fail_explicitly();
  test_max_affected_estimate_prunes_before_construction();
  test_streaming_candidate_cap_stops_before_eager_path_precompute();
  test_streaming_path_pair_budget_stops_early();
  test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute();
  test_exact_verification_reuses_state_old_score();
  test_failed_exact_materialization_is_timed();
  test_top_k_exact_verification_count_is_bounded();
  test_lower_bound_heuristic_acceptance_is_explicit();
  test_fixed_topology_exact_rejects_bare_candidate();
  test_fixed_topology_iteration_uses_default_selector();
  test_sampled_tree_fixed_topology_uses_source_certificate();
  test_fixed_topology_exact_certificate_scores_selected_topology();
  test_phase8_fixed_topology_cache_score_matches_materialized_per_pattern();
  test_phase8_fixed_topology_rejects_bad_selected_partition();
  test_phase8_fixed_topology_rejects_unreachable_selected_after_ref();
  test_phase8_persistent_cache_local_commit_matches_materialized();
  test_phase8_persistent_cache_dag_verification_has_no_fallback();
  test_phase8_per_pattern_oracle_catches_independent_moved_state();
  test_phase5_combine_rows_binary_regression();
  test_phase5_multifurcation_selected_topology_rows();
  test_phase5_multifurcation_outside_rows_exercise_shared_state_guard();
  test_phase6_source_multifurcation_candidate_has_no_unreachable_helper();
  test_phase6_multifurcation_fixed_topology_local_commit();
  test_phase6_multifurcation_lower_bound_conservative_commit();
  test_phase6_exact_multisite_multifurcation_gate();
  test_phase8_persistent_cache_invariant_failure_is_hard_error();
  test_phase8_persistent_verifier_materialized_oracle_all_move_classes();
  test_phase8_oracle_mismatch_uses_materialized_oracle_result();
  test_phase8_chain_objective_gate_is_monotone_across_commits();
  test_phase8_icache_participation_across_local_commits();
  test_enumeration_truncation_sets_unverified_flag_even_exhaustive();
  test_phase5_no_improvement_search_stops_without_commit();
  test_phase5_known_improving_search_commits_once();
  test_phase9_known_improving_search_uses_local_accept_update();
  test_phase5_final_compaction_uses_grammar_oracle_not_tree_override();
  test_phase5_final_compaction_checks_recorded_exact_objective();
  test_phase9_pattern_batch_local_update_matches_output_dag();
  test_phase9_fixed_topology_compaction_uses_certificate();
  test_phase9_lower_bound_compaction_matches_output_dag();
  test_phase9_multi_iteration_local_updates_match_output_dag();
  test_phase5_rejected_candidates_do_not_rebuild_sidecar();
  test_phase5_post_materialization_worsening_rejects_commit();
  test_phase5_fixed_topology_mode_commits_with_rebuilt_certificate_gate();
  test_phase5_pattern_fingerprint_mismatch_rebuilds_patterns();
  test_phase5_seeded_multi_iteration_is_deterministic();
  test_canonical_evidence_failure_is_hard_error();
  test_exhaustive_exact_acceptance_matches_oracle();
  test_phase5_witness_topology_selects_required_ancestor_path();
  test_phase5_final_compaction_normalizes_trim_options();
  test_phase5_overlay_chain_compaction_preserves_intended_keys();
  test_phase5_compaction_augments_historical_intended_keys();
  test_phase4_local_commit_counter_contract_and_oracle();
  test_phase4_conservative_mode_counters_unchanged();
  test_phase4_local_commit_score_ua_edge_rejected();
  test_phase4_local_commit_post_append_failure_is_hard_error();
  test_phase4_exact_trim_cache_never_stale();
  test_phase4_multi_worker_matches_serial();
  test_phase4_fixed_topology_exact_local_commit();
  test_lazy_cache_local_commit_sequence_projects_lazy_chart();
  test_phase4_pattern_batches_local_commit();
  test_phase9_transient_no_full_overlay_materialization();
  test_phase9_transient_oracle_both_charts_green();
  test_phase9_transient_oracle_green_on_multiparent_dag();
  test_phase9_transient_oracle_catches_corruption();
  test_phase9_transient_tombstone_scope_falls_back_to_cold();
  test_phase9_transient_multi_worker_matches_serial();
  std::println("chart_spr_search_test PASS");
  return 0;
}

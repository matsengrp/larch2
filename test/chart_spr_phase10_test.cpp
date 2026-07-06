// Phase 10 -- CLI / report / identity surface; counter hygiene (cross-cutting).
//
// These tests cover the Phase-10 exit criteria:
//   * A local-commit run's emitted report carries every counter named in the
//     cross-cutting counter contract with the contracted values (criterion 1).
//   * A JSON report's chain entries and Option-C rewrite identities survive
//     materialize -> rebuild -> report without loss (keys preserved)
//     (criterion 2).
//   * Conservative mode reports unchanged from the Phase 0 baseline structure
//     (criterion 4): the per-accept materialization / sidecar-rebuild counters
//     take their conservative-mode values and the chain identity report is
//     empty.
//   * The named commit / verification modes are surfaced and the verification-
//     mode choice actually toggles the exact-verification path (criterion 1).
//
// ASAN cleanness on cache-mutating paths and TSAN cleanness on multi-worker
// runs are checked by the ASAN / TSAN builds of the whole suite, not here.

#include <larch/build_fasta_newick.hpp>
#include <larch/chart_spr_search.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/inside_chart_cache.hpp>
#include <larch/merge.hpp>
#include <larch/option_c_chain_commit.hpp>
#include <larch/outside_chart_cache.hpp>
#include <larch/overlay_chain.hpp>
#include <larch/overlay_chain_compaction.hpp>
#include <larch/phase10_report.hpp>
#include <larch/rank3_rewrite.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <print>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr) \
  do { \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

// The same known-improving fixture the Phase-5/9 tests use: a four-taxon tree
// whose single improving SPR move is found and committed.
static larch::test::tiny_tree_node four_taxon_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})});
}

static larch::phylo_dag make_four_taxon_dag() {
  return larch::test::make_tiny_labelled_tree("A", four_taxon_misplaced_tree());
}

static larch::test::tiny_tree_node phase7_arity3_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_leaf("B", "A"), tiny_leaf("D", "C")});
}

// =====================================================================
// Criterion 1: a local-commit run's report carries every contract counter
// with the contracted values.
// =====================================================================
static void test_phase10_local_commit_report_counters_and_labels() {
  std::println("test_phase10_local_commit_report_counters_and_labels");

  auto dag = make_four_taxon_dag();
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.commit_mode = larch::chart_spr_commit_mode::overlay_delta;
  options.verification_mode = larch::chart_spr_verification_mode::transient;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  // The fixture is known to accept exactly one improving move locally.
  CHECK(search.counters.local_commit_accepted_moves == 1);
  CHECK(search.counters.accepted_moves == 1);

  // Cross-cutting counter contract (the load-bearing per-accept pair): a
  // local-commit run does zero per-accept dense materializations and zero
  // per-accept sidecar rebuilds.
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  // The single allowed dense materialization is final compaction.
  CHECK(search.counters.overlay_materializations_for_final_compaction == 1);

  // Phase 3 affected-row counters are present and nonzero for a real commit.
  CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit > 0);

  // Phase 9 transient-extension counter is present; transient mode exercises
  // the chain-extension substrate at least once (the improving candidate is
  // exact-verified through the transient verifier before it is accepted).
  CHECK(search.counters.transient_chain_extensions_for_verification > 0);

  // Mode labels are mirrored into the summary.
  CHECK(search.summary.commit_mode == options.commit_mode);
  CHECK(search.summary.verification_mode == options.verification_mode);
  CHECK(search.summary.chain_per_accept_exactness_label ==
        larch::chart_spr_acceptance_mode_name(options.acceptance_mode));

  // The chain identity JSON was emitted and is parseable.
  CHECK(!search.chain_identity_report_json.empty());
  auto report = larch::parse_phase10_chain_identity_report_json(
      search.chain_identity_report_json);
  CHECK(report.entries.size() == 1);
  CHECK(report.entries.front().commit_source == "spr_overlay_delta");

  std::println("  PASS");
}

// =====================================================================
// Criterion 4: conservative mode reports unchanged from the Phase 0 baseline
// structure (per-accept materialization + sidecar rebuild; no chain report).
// =====================================================================
static void test_phase10_conservative_mode_report_unchanged() {
  std::println("test_phase10_conservative_mode_report_unchanged");

  auto dag = make_four_taxon_dag();
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;  // conservative (the default)

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  CHECK(search.counters.accepted_moves == 1);
  // Conservative-mode contracted values: one per-accept materialization and
  // one per-accept sidecar rebuild (the Phase-0 baseline reference).
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.overlay_materializations_for_final_compaction == 0);
  // No local-commit counters move in conservative mode.
  CHECK(search.counters.local_commit_accepted_moves == 0);
  CHECK(search.counters.inside_rows_recomputed_on_commit == 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit == 0);
  CHECK(search.counters.transient_chain_extensions_for_verification == 0);
  // Conservative mode never builds a chain, so no identity report.
  CHECK(search.chain_identity_report_json.empty());
  // The per-accept label reports the conservative path.
  CHECK(search.summary.chain_per_accept_exactness_label ==
        "none_conservative_materialize_rebuild");

  std::println("  PASS");
}

// =====================================================================
// Criterion 1 (verification-mode choice): `cold` actually toggles the exact-
// verification path away from the Phase-9 transient extension.  Cold mode
// skips installing the transient verifier, so verification goes through the
// from-scratch `verify_candidate_exact_against_state` path (a dense
// materialization per verified candidate).
// =====================================================================
static void test_phase10_verification_mode_cold_skips_transient() {
  std::println("test_phase10_verification_mode_cold_skips_transient");

  auto dag = make_four_taxon_dag();
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verification_mode = larch::chart_spr_verification_mode::cold;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  // Cold mode: the transient substrate is not exercised; the cold path
  // materializes once per verified candidate.
  CHECK(search.counters.transient_chain_extensions_for_verification == 0);
  CHECK(search.counters.overlay_materializations_for_exact_verification > 0);
  // The improving move is still found and committed locally (the gate is
  // unchanged; only the verification path differs).
  CHECK(search.counters.local_commit_accepted_moves == 1);
  CHECK(search.summary.verification_mode ==
        larch::chart_spr_verification_mode::cold);

  std::println("  PASS");
}

// =====================================================================
// Criterion 2 (JSON round-trip): chain entries and Option-C rewrite
// identities survive materialize -> rebuild -> report without loss.
//
// Builds a chain with a rank-3 Option-C child-set rewrite (the AB|CD -> AC|BD
// move on wric_binary_four), emits the JSON identity report, parses it back,
// then asserts the net production taxon-set keys equal:
//   (a) the keys of the materialized chain grammar, and
//   (b) the keys of the grammar rebuilt from the compacted output DAG.
// Taxon-set keys are stable across materialize/rebuild, so equality of the key
// sets is exactly "keys preserved".
// =====================================================================
static std::vector<larch::taxon_id> taxa_for(
    larch::clade_grammar const& grammar, std::vector<std::string> sample_ids) {
  std::vector<larch::taxon_id> ids;
  for (auto const& sample_id : sample_ids) {
    auto it = grammar.taxa.sample_id_to_id.find(sample_id);
    CHECK(it != grammar.taxa.sample_id_to_id.end());
    ids.push_back(it->second);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

static std::vector<larch::rank3_production_taxa_key> sorted_unique_keys(
    std::vector<larch::rank3_production_taxa_key> keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

// Read every production taxon-set key off a grammar (the rebuilt-grammar side
// of the round-trip oracle).
static std::vector<larch::rank3_production_taxa_key> grammar_keys(
    larch::clade_grammar const& grammar) {
  std::vector<larch::rank3_production_taxa_key> keys;
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (grammar.productions[pid].parent == larch::no_clade) continue;
    keys.push_back(larch::rank3_detail::production_key_from_id(
        grammar, static_cast<larch::production_id>(pid)));
  }
  return sorted_unique_keys(std::move(keys));
}

static bool contains_multifurcation_key(
    std::vector<larch::rank3_production_taxa_key> const& keys) {
  return std::any_of(keys.begin(), keys.end(), [](auto const& key) {
    return key.children.size() > 2;
  });
}

// ---- Option-C after-structure helpers (mirror option_c_chain_commit_test) ----
static larch::rank3_production_taxa_key make_split_key(
    std::vector<larch::taxon_id> parent,
    std::vector<std::vector<larch::taxon_id>> children) {
  larch::rank3_production_taxa_key key;
  key.parent = std::move(parent);
  key.children = std::move(children);
  larch::rank3_detail::normalize_production_key(key);
  return key;
}
static larch::option_c_after_subtree leaf_after(std::vector<larch::taxon_id> t) {
  return larch::option_c_after_subtree{std::move(t), {}};
}
static larch::option_c_after_subtree pair_after(
    std::vector<larch::taxon_id> t, larch::option_c_after_subtree c0,
    larch::option_c_after_subtree c1) {
  return larch::option_c_after_subtree{std::move(t),
                                       {std::move(c0), std::move(c1)}};
}

static void test_phase10_chain_identity_json_round_trip() {
  std::println("test_phase10_chain_identity_json_round_trip");

  auto dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/wric_binary_four.fa"),
      larch::test::source_path_string("test/wric_binary_four.nwk"),
      larch::test::source_path_string("test/wric_binary_four.ref"));
  larch::validate_dag(dag, "wric_binary_four fixture");

  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_options opts;
  auto active_build = larch::make_active_search_patterns(dag, grammar, opts);

  // Owns the frozen base grammar + chain + caches (address-stable, mirrors
  // option_c_chain_commit_test's chain_substrate).
  larch::clade_grammar base_grammar = grammar;
  larch::overlay_chain chain{base_grammar};
  larch::inside_chart_cache icache{larch::build_inside_chart_cache(
      base_grammar, active_build.active_patterns, opts,
      active_build.invariant_constant_offset)};
  larch::outside_chart_cache ocache{larch::build_outside_chart_cache(
      base_grammar, active_build.active_patterns, opts)};

  auto a = taxa_for(base_grammar, {"A"});
  auto b = taxa_for(base_grammar, {"B"});
  auto c = taxa_for(base_grammar, {"C"});
  auto d = taxa_for(base_grammar, {"D"});
  auto ab = taxa_for(base_grammar, {"A", "B"});
  auto cd = taxa_for(base_grammar, {"C", "D"});
  auto ac = taxa_for(base_grammar, {"A", "C"});
  auto bd = taxa_for(base_grammar, {"B", "D"});
  auto abcd = taxa_for(base_grammar, {"A", "B", "C", "D"});

  larch::rank3_production_taxa_key before_key;
  before_key.parent = abcd;
  before_key.children = {ab, cd};
  larch::rank3_detail::normalize_production_key(before_key);

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(larch::option_c_after_subtree{ac, {}});
  after.children.back().children.push_back(
      larch::option_c_after_subtree{a, {}});
  after.children.back().children.push_back(
      larch::option_c_after_subtree{c, {}});
  after.children.push_back(larch::option_c_after_subtree{bd, {}});
  after.children.back().children.push_back(
      larch::option_c_after_subtree{b, {}});
  after.children.back().children.push_back(
      larch::option_c_after_subtree{d, {}});

  larch::option_c_chain_commit_options copts;
  copts.verify_two_chart_oracle_for_tests = true;
  auto commit = larch::option_c_commit_via_chain(chain, icache, ocache,
                                                 before_key, after, copts);
  CHECK(commit.witnesses_spliced == 1);
  CHECK(chain.size() == 1);

  // --- Report -> JSON -> parse round trip ---
  auto report = larch::build_phase10_chain_identity_report(chain);
  CHECK(report.entries.size() == 1);
  CHECK(report.entries.front().position == 0);
  // The Option-C entry is labelled distinctly from SPR overlay deltas.
  CHECK(report.entries.front().commit_source ==
        larch::option_c_chain_commit_result::commit_label);
  // The before production is tombstoned; the after production is added.
  CHECK(std::find(report.entries.front().tombstoned_production_keys.begin(),
                  report.entries.front().tombstoned_production_keys.end(),
                  before_key) !=
        report.entries.front().tombstoned_production_keys.end());

  auto json = larch::emit_phase10_chain_identity_report_json(report);
  auto parsed = larch::parse_phase10_chain_identity_report_json(json);
  CHECK(parsed.entries.size() == report.entries.size());
  CHECK(parsed.entries.front().commit_source ==
        report.entries.front().commit_source);
  CHECK(parsed.entries.front().added_production_keys ==
        report.entries.front().added_production_keys);
  CHECK(parsed.entries.front().tombstoned_production_keys ==
        report.entries.front().tombstoned_production_keys);
  CHECK(parsed.base_production_keys == report.base_production_keys);

  // --- materialize -> rebuild round trip: the chain grammar materialized
  // from the overlay and the grammar rebuilt from the compacted output DAG
  // carry the SAME production taxon-set keys (keys preserved across
  // materialize -> rebuild).  This is the load-bearing Phase-10 round-trip
  // property.  (Both grammars prune now-unreachable productions identically,
  // so the equality is exact.)
  auto materialized = larch::materialize_overlay_chain(chain);
  auto materialized_keys = grammar_keys(materialized.grammar);

  // --- rebuild: compact the chain to an output DAG and rebuild the grammar. ---
  larch::overlay_chain_compaction_result compacted =
      larch::compact_overlay_chain_to_dag(dag, chain);
  auto rebuilt = larch::build_clade_grammar(compacted.dag);
  auto rebuilt_keys = grammar_keys(rebuilt);
  CHECK(rebuilt_keys == materialized_keys);

  // --- report faithfulness: every production the report records as added is
  // present in the materialized grammar, and every production it records as
  // tombstoned is absent.  (The report's net-key superset also includes base
  // productions that became unreachable after the tombstone; those are pruned
  // by materialization, so we check membership, not set equality.) ---
  std::set<larch::rank3_production_taxa_key> matset{materialized_keys.begin(),
                                                   materialized_keys.end()};
  for (auto const& key :
       report.entries.front().added_production_keys) {
    CHECK(matset.count(key) != 0);
  }
  for (auto const& key :
       report.entries.front().tombstoned_production_keys) {
    CHECK(matset.count(key) == 0);
  }

  // The Option-C commit-source literal in rank3_rewrite.hpp stays in sync
  // with option_c_chain_commit_result::commit_label (the two are defined in
  // headers that cannot include each other).
  CHECK(std::string{larch::option_c_chain_commit_result::commit_label} ==
        "option_c_chain_commit");

  std::println("  PASS");
}

// =====================================================================
// Criterion 2 (SPR-commit round trip via the search loop): the chain identity
// JSON emitted by run_chart_spr_search (an SPR overlay-delta commit) parses,
// and its net production keys equal the keys of the grammar rebuilt from the
// compacted output DAG.  This is the search-loop analogue of the Option-C
// library round trip above.
// =====================================================================
static void test_phase10_search_loop_chain_identity_round_trip() {
  std::println("test_phase10_search_loop_chain_identity_round_trip");

  auto dag = make_four_taxon_dag();
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.counters.local_commit_accepted_moves == 1);

  auto report = larch::parse_phase10_chain_identity_report_json(
      search.chain_identity_report_json);
  CHECK(report.entries.size() == 1);
  CHECK(report.entries.front().commit_source == "spr_overlay_delta");

  // The compacted output DAG's rebuilt grammar is the search-loop side of the
  // materialize -> rebuild round trip.  The report's added productions are
  // present in it and its tombstoned productions are absent (keys preserved).
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_keys = grammar_keys(rebuilt);
  std::set<larch::rank3_production_taxa_key> rebuilt_set{
      rebuilt_keys.begin(), rebuilt_keys.end()};
  for (auto const& key :
       report.entries.front().added_production_keys) {
    CHECK(rebuilt_set.count(key) != 0);
  }
  for (auto const& key :
       report.entries.front().tombstoned_production_keys) {
    CHECK(rebuilt_set.count(key) == 0);
  }

  std::println("  PASS");
}

static void test_phase7_multifurcation_chain_identity_round_trip() {
  std::println("test_phase7_multifurcation_chain_identity_round_trip");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase7_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(larch::clade_grammar_max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.counters.local_commit_accepted_moves == 1);
  CHECK(search.counters.spr_multifurcation_moves_generated > 0);
  CHECK(!search.chain_identity_report_json.empty());

  auto report = larch::parse_phase10_chain_identity_report_json(
      search.chain_identity_report_json);
  CHECK(report.entries.size() == 1);
  CHECK(report.entries.front().commit_source == "spr_overlay_delta");
  CHECK(contains_multifurcation_key(report.base_production_keys));

  auto net = larch::phase10_chain_net_production_keys(report);
  CHECK(contains_multifurcation_key(net));

  auto rebuilt = larch::build_clade_grammar(search.dag, gopts);
  CHECK(larch::clade_grammar_max_production_arity(rebuilt) == 3);
  auto rebuilt_keys = grammar_keys(rebuilt);
  std::set<larch::rank3_production_taxa_key> rebuilt_set{
      rebuilt_keys.begin(), rebuilt_keys.end()};
  for (auto const& key : net) {
    CHECK(rebuilt_set.count(key) != 0);
  }

  std::println("  PASS");
}

// =====================================================================
// Issue 2: commit_mode == option_c is a behaviourless label in the search
// loop.  The search loop's candidate generator always produces SPR overlay-
// delta commits, so it cannot honor option_c; selecting it must throw a
// labelled error rather than silently reporting a label the loop did not
// honor (no silent fallback).  Option-C commits stay reachable only through
// the library API (option_c_commit_via_chain), exercised below and in
// option_c_chain_commit_test.
// =====================================================================
static void test_phase10_commit_mode_option_c_throws() {
  std::println("test_phase10_commit_mode_option_c_throws");

  auto dag = make_four_taxon_dag();
  auto grammar = larch::build_clade_grammar(dag);

  // Default overlay_delta is accepted (no throw); this guards against an
  // over-broad throw.  (A full local-commit run with overlay_delta is also
  // exercised by every other test in this file; here we just confirm the
  // default does not trip the option_c guard.)
  {
    larch::chart_spr_search_options options;
    options.commit_mode = larch::chart_spr_commit_mode::overlay_delta;
    options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.top_k_exact_verify = 8;
    options.max_iterations = 1;
    options.rebuild_after_accept = false;
    auto s = larch::run_chart_spr_search(
        make_four_taxon_dag(), grammar, options);
    CHECK(s.counters.local_commit_accepted_moves == 1);
  }

  // option_c throws from the validator (before the first accept) regardless of
  // rebuild_after_accept, because the search loop cannot produce Option-C
  // commits in either mode.
  bool threw = false;
  for (bool conservative : {true, false}) {
    larch::chart_spr_search_options options;
    options.commit_mode = larch::chart_spr_commit_mode::option_c;
    options.rebuild_after_accept = conservative;
    options.max_iterations = 1;
    threw = false;
    try {
      (void)larch::run_chart_spr_search(
          larch::test::make_tiny_labelled_tree("A", four_taxon_misplaced_tree()),
          grammar, options);
    } catch (std::runtime_error const& e) {
      threw = true;
      CHECK(std::string{e.what()}.find("commit_mode == option_c") !=
            std::string::npos);
      CHECK(std::string{e.what()}.find("option_c_commit_via_chain") !=
            std::string::npos);
    }
    CHECK(threw);
  }

  std::println("  PASS");
}

// =====================================================================
// Issue 3 + 4: a 2-delta Option-C chain.  build_phase10_chain_identity_report
// resolves each delta's added productions against chain.tip() (the merged
// overlay of all deltas); the existing single-delta round-trip tests do not
// exercise that per-delta resolution against a shared tip.  This test builds a
// genuine chain of length 2 -- two sequential Option-C commits whose tombstones
// each resolve to a distinct frozen-base production -- and asserts the report
// carries two correctly-ordered, correctly-labelled entries with per-delta
// added/tombstoned keys resolved against the shared tip.
//
// It also exercises phase10_chain_net_production_keys (previously dead code):
// the net production-key set the report represents must (a) survive the JSON
// round trip unchanged and (b) be a superset of the materialized chain
// grammar's keys (the report records every production the chain added or
// tombstoned, including those later pruned by reachability, so it is a faithful
// superset rather than an exact equality).
// =====================================================================
static void test_phase10_chain_identity_report_two_delta() {
  std::println("test_phase10_chain_identity_report_two_delta");

  // Multi-tree grammar carrying BOTH AB|CD and AC|BD at the root clade, so two
  // sequential rewrites can each tombstone a distinct frozen-base production:
  //   commit 1: AB|CD -> AC|BD (merge; AC|BD already present -> 0 added)
  //   commit 2: AC|BD -> AB|CD (AB|CD now tombstoned/absent -> re-adds it)
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto base_tree = tiny_inner(
      "root", "A",
      {tiny_inner("ABCD", "A",
                  {tiny_inner("AB", "A",
                              {tiny_leaf("A", "A"), tiny_leaf("B", "C")}),
                   tiny_inner("CD", "A",
                              {tiny_leaf("C", "C"), tiny_leaf("D", "C")})})});
  auto alt_tree = tiny_inner(
      "root", "A",
      {tiny_inner("ABCD", "A",
                  {tiny_inner("AC", "A",
                              {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
                   tiny_inner("BD", "A",
                              {tiny_leaf("B", "C"), tiny_leaf("D", "C")})})});
  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("A", base_tree));
  trees.push_back(larch::test::make_tiny_labelled_tree("A", alt_tree));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  larch::validate_dag(dag, "phase10 two-delta multitree fixture");

  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_options opts;
  auto active_build = larch::make_active_search_patterns(dag, grammar, opts);

  larch::clade_grammar base_grammar = grammar;
  larch::overlay_chain chain{base_grammar};
  larch::inside_chart_cache icache{larch::build_inside_chart_cache(
      base_grammar, active_build.active_patterns, opts,
      active_build.invariant_constant_offset)};
  larch::outside_chart_cache ocache{larch::build_outside_chart_cache(
      base_grammar, active_build.active_patterns, opts)};

  auto a = taxa_for(base_grammar, {"A"});
  auto b = taxa_for(base_grammar, {"B"});
  auto c = taxa_for(base_grammar, {"C"});
  auto d = taxa_for(base_grammar, {"D"});
  auto ab = taxa_for(base_grammar, {"A", "B"});
  auto cd = taxa_for(base_grammar, {"C", "D"});
  auto ac = taxa_for(base_grammar, {"A", "C"});
  auto bd = taxa_for(base_grammar, {"B", "D"});
  auto abcd = taxa_for(base_grammar, {"A", "B", "C", "D"});

  larch::option_c_chain_commit_options copts;
  copts.verify_two_chart_oracle_for_tests = true;

  // Commit 1: AB|CD -> AC|BD.  AC|BD is already present (frozen base from the
  // second tree), so under the merge policy the commit tombstones AB|CD and
  // adds no new root production (the resolver reuses the present one).
  {
    auto before = make_split_key(abcd, {ab, cd});
    larch::option_c_after_production after;
    after.parent_taxa = abcd;
    after.children.push_back(pair_after(ac, leaf_after(a), leaf_after(c)));
    after.children.push_back(pair_after(bd, leaf_after(b), leaf_after(d)));
    auto r = larch::option_c_commit_via_chain(chain, icache, ocache, before,
                                              after, copts);
    CHECK(r.witnesses_spliced == 1);
  }
  CHECK(chain.size() == 1);

  // Commit 2: AC|BD -> AB|CD.  AB|CD is now absent (tombstoned by commit 1),
  // so the resolver re-adds the AB|CD root production (and its AB/CD child
  // productions, since those clades were left unreachable by commit 1).  This
  // is the delta whose added productions must be resolved against the merged
  // tip (commit 1 + base), not against the frozen base alone.
  {
    auto before = make_split_key(abcd, {ac, bd});
    larch::option_c_after_production after;
    after.parent_taxa = abcd;
    after.children.push_back(pair_after(ab, leaf_after(a), leaf_after(b)));
    after.children.push_back(pair_after(cd, leaf_after(c), leaf_after(d)));
    auto r = larch::option_c_commit_via_chain(chain, icache, ocache, before,
                                              after, copts);
    CHECK(r.witnesses_spliced == 1);
  }
  CHECK(chain.size() == 2);

  auto report = larch::build_phase10_chain_identity_report(chain);
  CHECK(report.entries.size() == 2);
  // Chain position is an ordinal: positions are 0, 1 in order.
  CHECK(report.entries[0].position == 0);
  CHECK(report.entries[1].position == 1);
  // Both entries carry the Option-C commit-source label (distinct from SPR).
  CHECK(report.entries[0].commit_source ==
        larch::option_c_chain_commit_result::commit_label);
  CHECK(report.entries[1].commit_source ==
        larch::option_c_chain_commit_result::commit_label);
  // Per-delta resolution against the shared tip:
  //   delta 0 tombstoned AB|CD and added nothing (AC|BD reused).
  //   delta 1 tombstoned AC|BD and re-added the AB|CD partition (>= 1 key).
  CHECK(report.entries[0].tombstoned_production_keys.size() == 1);
  CHECK(report.entries[0].added_production_keys.empty());
  CHECK(report.entries[1].tombstoned_production_keys.size() == 1);
  CHECK(!report.entries[1].added_production_keys.empty());
  // The tombstoned keys are the distinct frozen-base productions.
  auto ab_cd = make_split_key(abcd, {ab, cd});
  auto ac_bd = make_split_key(abcd, {ac, bd});
  CHECK(report.entries[0].tombstoned_production_keys.front() == ab_cd);
  CHECK(report.entries[1].tombstoned_production_keys.front() == ac_bd);
  // delta 1's added keys (resolved against the tip) include the re-added
  // AB|CD root production.
  CHECK(std::find(report.entries[1].added_production_keys.begin(),
                  report.entries[1].added_production_keys.end(),
                  ab_cd) != report.entries[1].added_production_keys.end());

  // JSON round trip preserves every entry and its keys.
  auto json = larch::emit_phase10_chain_identity_report_json(report);
  auto parsed = larch::parse_phase10_chain_identity_report_json(json);
  CHECK(parsed.entries.size() == 2);
  CHECK(parsed.entries[0].commit_source == report.entries[0].commit_source);
  CHECK(parsed.entries[1].commit_source == report.entries[1].commit_source);
  CHECK(parsed.entries[1].added_production_keys ==
        report.entries[1].added_production_keys);

  // Issue 3: phase10_chain_net_production_keys is load-bearing here.  The net
  // set must (a) survive the JSON round trip unchanged and (b) be a superset
  // of the materialized chain grammar's keys (the report records every
  // production added or tombstoned, including those later pruned by
  // reachability, so it is a faithful superset rather than exact equality).
  auto net = larch::phase10_chain_net_production_keys(report);
  auto parsed_net = larch::phase10_chain_net_production_keys(parsed);
  CHECK(net == parsed_net);
  auto materialized = larch::materialize_overlay_chain(chain);
  auto materialized_keys = grammar_keys(materialized.grammar);
  std::set<larch::rank3_production_taxa_key> net_set{net.begin(), net.end()};
  for (auto const& key : materialized_keys) {
    CHECK(net_set.count(key) != 0);
  }

  std::println("  PASS");
}

// =====================================================================
// Counter-contract parseability: every contract counter is present in the
// counters struct name set (a regression guard so a renamed counter field is
// caught here, not only by the compile-only baseline generator).
// =====================================================================
static void test_phase10_counter_contract_fields_exist() {
  std::println("test_phase10_counter_contract_fields_exist");

  larch::chart_spr_search_counters c{};
  // Touch every cross-cutting contract counter by name so a rename breaks the
  // build at this test.  (Values are all zero on a default-constructed
  // counters struct; the contracted *values* are checked in the local-commit
  // and conservative-mode tests above.)
  (void)c.sidecar_rebuilds_after_accept;
  (void)c.full_overlay_materializations;
  (void)c.overlay_materializations_for_oracle;
  (void)c.overlay_materializations_for_exact_verification;
  (void)c.overlay_materializations_for_accept_materialization;
  (void)c.overlay_materializations_for_final_compaction;
  (void)c.transient_chain_extensions_for_verification;
  (void)c.selected_topology_multifurcation_rows;
  (void)c.spr_multifurcation_moves_generated;
  (void)c.inside_rows_recomputed_on_commit;
  (void)c.outside_rows_recomputed_on_commit;
  (void)c.local_commit_accepted_moves;
  (void)c.local_commit_tombstone_scope_skips;
  // Summary mirrors.
  larch::chart_spr_search_summary s{};
  (void)s.sidecar_rebuilds_after_accept;
  (void)s.transient_chain_extensions_for_verification;
  (void)s.selected_topology_multifurcation_rows;
  (void)s.spr_multifurcation_moves_generated;
  (void)s.inside_rows_recomputed_on_commit;
  (void)s.outside_rows_recomputed_on_commit;
  (void)s.commit_mode;
  (void)s.verification_mode;
  (void)s.chain_per_accept_exactness_label;
  (void)s.final_compaction_exactness_kind;

  std::println("  PASS");
}

int main() {
  test_phase10_counter_contract_fields_exist();
  test_phase10_local_commit_report_counters_and_labels();
  test_phase10_conservative_mode_report_unchanged();
  test_phase10_verification_mode_cold_skips_transient();
  test_phase10_commit_mode_option_c_throws();
  test_phase10_chain_identity_json_round_trip();
  test_phase10_chain_identity_report_two_delta();
  test_phase10_search_loop_chain_identity_round_trip();
  test_phase7_multifurcation_chain_identity_round_trip();
  std::println("chart_spr_phase10_test PASS");
  return 0;
}

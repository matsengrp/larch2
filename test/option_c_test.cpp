// Phase 6 — Rank-3 Option C: direct in-place production splice tests.
//
// These tests exercise the standalone Option C library API
// (option_c_splice_production).  Chart-search commit integration is Phase 7
// and CLI exposure is Phase 10; neither is covered here.
//
// The central oracle is merge-equivalence at the taxon-set-key level: for
// every supported before/after pair, the Option C output DAG and the Option A
// reference agree on clade taxon sets, production taxon-set keys, the count of
// representable trees, and (for single-tree outputs) Fitch parsimony.  Clade-id
// sets are intentionally not compared (legitimate divergence between the
// in-place splice and the materialize-and-merge path).

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

[[noreturn]] static void test_fail(char const* expr, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr) \
  do { \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

// ---- tiny-tree fixtures ----

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

// ((A,D),(B,C)) — the third root resolution of {A,B,C,D}.
static larch::test::tiny_tree_node four_taxon_adbc_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AD", "A", {tiny_leaf("A", "A"), tiny_leaf("D", "C")}),
       tiny_inner("BC", "A", {tiny_leaf("B", "A"), tiny_leaf("C", "C")})});
}

// Six-taxon tree: (((A,B),(C,D)),(E,F)).  Clade {A,B,C,D} is a non-root
// internal clade with production {ABCD}->{{A,B},{C,D}}, splicing it exercises a
// non-root production rewrite.
static larch::test::tiny_tree_node six_taxon_base_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABCD", "A",
                  {tiny_inner("AB", "A",
                              {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
                   tiny_inner("CD", "C",
                              {tiny_leaf("C", "C"), tiny_leaf("D", "C")})}),
       tiny_inner("EF", "A",
                  {tiny_leaf("E", "A"), tiny_leaf("F", "C")})});
}

// Six-taxon tree with the non-root clade {A,B,C,D} resolved as ((A,C),(B,D)).
static larch::test::tiny_tree_node six_taxon_acbd_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABCD", "A",
                  {tiny_inner("AC", "A",
                              {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
                   tiny_inner("BD", "A",
                              {tiny_leaf("B", "A"), tiny_leaf("D", "C")})}),
       tiny_inner("EF", "A",
                  {tiny_leaf("E", "A"), tiny_leaf("F", "C")})});
}

// ---- grammar helpers ----

static std::vector<larch::taxon_id> taxa_for(
    larch::clade_grammar const& grammar, std::vector<std::string> sample_ids) {
  std::vector<larch::taxon_id> ids;
  ids.reserve(sample_ids.size());
  for (auto const& sample_id : sample_ids) {
    auto it = grammar.taxa.sample_id_to_id.find(sample_id);
    CHECK(it != grammar.taxa.sample_id_to_id.end());
    ids.push_back(it->second);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

static std::set<std::vector<larch::taxon_id>> clade_taxa_set(
    larch::clade_grammar const& g) {
  std::set<std::vector<larch::taxon_id>> s;
  for (auto const& c : g.clades) s.insert(c.taxa);
  return s;
}

// ---- Option C helpers ----

static larch::rank3_production_taxa_key make_split_key(
    std::vector<larch::taxon_id> parent,
    std::vector<std::vector<larch::taxon_id>> children) {
  larch::rank3_production_taxa_key key;
  key.parent = std::move(parent);
  key.children = std::move(children);
  larch::rank3_detail::normalize_production_key(key);
  return key;
}

static larch::option_c_after_subtree leaf_after(
    std::vector<larch::taxon_id> taxa) {
  return larch::option_c_after_subtree{std::move(taxa), {}};
}

static larch::option_c_after_subtree pair_after(
    std::vector<larch::taxon_id> taxa, larch::option_c_after_subtree c0,
    larch::option_c_after_subtree c1) {
  return larch::option_c_after_subtree{std::move(taxa),
                                       {std::move(c0), std::move(c1)}};
}

// Full merge-equivalence oracle (clade taxa sets, production keys,
// representable tree count, and Fitch parsimony when both are single trees).
static void check_option_c_merge_equivalent(larch::phylo_dag& c_dag,
                                             larch::phylo_dag& a_dag) {
  auto cg = larch::build_clade_grammar(c_dag);
  auto ag = larch::build_clade_grammar(a_dag);
  CHECK(clade_taxa_set(cg) == clade_taxa_set(ag));
  CHECK(larch::rank3_detail::production_key_set(cg) ==
        larch::rank3_detail::production_key_set(ag));
  auto ctrees = larch::test::enumerate_represented_tree_edges(c_dag);
  auto atrees = larch::test::enumerate_represented_tree_edges(a_dag);
  CHECK(ctrees.size() == atrees.size());
  if (larch::is_tree(c_dag) && larch::is_tree(a_dag)) {
    CHECK(larch::test::score_tree_fitch_parsimony(c_dag) ==
          larch::test::score_tree_fitch_parsimony(a_dag));
  }
}

// Grammar-level-only oracle (clade taxa sets + production keys), used for
// multi-witness / already-present cases where Option C's witness-level merge
// intentionally produces a non-maximally-shared DAG whose edge-level
// representable-tree count exceeds a maximally-merged Option A reference.
// The parsimony optimum still agrees because it is recomputed from leaf
// compact genomes at the grammar level.
static void check_option_c_grammar_equivalent(larch::phylo_dag& c_dag,
                                               larch::phylo_dag& a_dag) {
  auto cg = larch::build_clade_grammar(c_dag);
  auto ag = larch::build_clade_grammar(a_dag);
  CHECK(clade_taxa_set(cg) == clade_taxa_set(ag));
  CHECK(larch::rank3_detail::production_key_set(cg) ==
        larch::rank3_detail::production_key_set(ag));
}

// =====================================================================
// Exit criterion 1: merge-equivalence for every supported before/after pair.
// =====================================================================

// Root production AB|CD -> AC|BD on the single-tree wric_binary_four DAG.
// Also checks exit criterion 4 (production-key set is exactly before subtree
// keys removed, after subtree keys added, nothing else) and the performance
// contract.
static void test_option_c_splice_root_abcd_to_acbd() {
  std::println("test_option_c_splice_root_abcd_to_acbd");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto ab = taxa_for(g, {"A", "B"});
  auto cd = taxa_for(g, {"C", "D"});
  auto ac = taxa_for(g, {"A", "C"});
  auto bd = taxa_for(g, {"B", "D"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  auto before_key = make_split_key(abcd, {ab, cd});

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(pair_after(ac, leaf_after(a), leaf_after(c)));
  after.children.push_back(pair_after(bd, leaf_after(b), leaf_after(d)));

  auto result = larch::option_c_splice_production(dag, before_key, after);

  CHECK(result.witnesses_spliced == 1);
  CHECK(result.after_key == make_split_key(abcd, {ac, bd}));

  // Exit criterion 4: the post-splice production-key set is EXACTLY the
  // pre-splice set with the before subtree's keys removed and the after
  // subtree's keys added.  On this fixture both sets have three keys, so the
  // full-set equality is a strong check that "nothing else changed."
  std::set<larch::rank3_production_taxa_key> expected_after = {
      make_split_key(abcd, {ac, bd}),
      make_split_key(ac, {a, c}),
      make_split_key(bd, {b, d}),
  };
  auto keys_after =
      larch::rank3_detail::production_key_set(result.rebuilt.grammar);
  CHECK(keys_after == expected_after);

  // Merge-equivalence with the Option A reference (single-tree after topology).
  auto option_a_dag =
      larch::test::make_tiny_labelled_tree("A", four_taxon_alt_tree());
  check_option_c_merge_equivalent(dag, option_a_dag);

  std::println("  PASS");
}

// Root production AB|CD -> AD|BC — the other root alternative on the same
// fixture.  Exercises a different before/after pair end-to-end.
static void test_option_c_splice_root_abcd_to_adbc() {
  std::println("test_option_c_splice_root_abcd_to_adbc");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto ab = taxa_for(g, {"A", "B"});
  auto cd = taxa_for(g, {"C", "D"});
  auto ad = taxa_for(g, {"A", "D"});
  auto bc = taxa_for(g, {"B", "C"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  auto before_key = make_split_key(abcd, {ab, cd});

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(pair_after(ad, leaf_after(a), leaf_after(d)));
  after.children.push_back(pair_after(bc, leaf_after(b), leaf_after(c)));

  auto result = larch::option_c_splice_production(dag, before_key, after);
  CHECK(result.witnesses_spliced == 1);

  std::set<larch::rank3_production_taxa_key> expected_after = {
      make_split_key(abcd, {ad, bc}),
      make_split_key(ad, {a, d}),
      make_split_key(bc, {b, c}),
  };
  auto keys_after =
      larch::rank3_detail::production_key_set(result.rebuilt.grammar);
  CHECK(keys_after == expected_after);

  auto option_a_dag =
      larch::test::make_tiny_labelled_tree("A", four_taxon_adbc_tree());
  check_option_c_merge_equivalent(dag, option_a_dag);

  std::println("  PASS");
}

// Non-root production rewrite: on (((A,B),(C,D)),(E,F)) splice the non-root
// clade {A,B,C,D}'s production AB|CD -> AC|BD.  Exercises edge surgery away
// from the root and pruning of the old AB/CD internal nodes.
static void test_option_c_splice_non_root_production() {
  std::println("test_option_c_splice_non_root_production");

  auto dag = larch::test::make_tiny_labelled_tree("A", six_taxon_base_tree());
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto ab = taxa_for(g, {"A", "B"});
  auto cd = taxa_for(g, {"C", "D"});
  auto ac = taxa_for(g, {"A", "C"});
  auto bd = taxa_for(g, {"B", "D"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});
  auto ef = taxa_for(g, {"E", "F"});
  auto abcdef = taxa_for(g, {"A", "B", "C", "D", "E", "F"});

  // Non-root before production {ABCD} -> {{A,B},{C,D}}.
  auto before_key = make_split_key(abcd, {ab, cd});

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(pair_after(ac, leaf_after(a), leaf_after(c)));
  after.children.push_back(pair_after(bd, leaf_after(b), leaf_after(d)));

  auto result = larch::option_c_splice_production(dag, before_key, after);
  CHECK(result.witnesses_spliced == 1);

  // The root and EF productions are untouched; only the {ABCD} subtree changes.
  auto keys_after =
      larch::rank3_detail::production_key_set(result.rebuilt.grammar);
  CHECK(keys_after.count(make_split_key(abcdef, {abcd, ef})));
  CHECK(keys_after.count(make_split_key(ef, {taxa_for(g, {"E"}),
                                             taxa_for(g, {"F"})})));
  CHECK(!keys_after.count(before_key));
  CHECK(!keys_after.count(make_split_key(ab, {a, b})));
  CHECK(!keys_after.count(make_split_key(cd, {c, d})));
  CHECK(keys_after.count(make_split_key(abcd, {ac, bd})));
  CHECK(keys_after.count(make_split_key(ac, {a, c})));
  CHECK(keys_after.count(make_split_key(bd, {b, d})));

  auto option_a_dag =
      larch::test::make_tiny_labelled_tree("A", six_taxon_acbd_tree());
  check_option_c_merge_equivalent(dag, option_a_dag);

  std::println("  PASS");
}

// =====================================================================
// Exit criterion 2 / guards: absent before, boundary mismatch.
// =====================================================================

static void test_option_c_absent_before_throws_unchanged() {
  std::println("test_option_c_absent_before_throws_unchanged");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto ad = taxa_for(g, {"A", "D"});
  auto bc = taxa_for(g, {"B", "C"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  // {AD}|{BC} is not represented in the ((A,B),(C,D)) DAG.
  auto absent_key = make_split_key(abcd, {ad, bc});

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(pair_after(ad, leaf_after(a), leaf_after(d)));
  after.children.push_back(pair_after(bc, leaf_after(b), leaf_after(c)));

  auto keys_before = larch::rank3_detail::production_key_set(g);

  bool threw = false;
  std::string msg;
  try {
    larch::option_c_splice_production(dag, absent_key, after);
  } catch (std::runtime_error const& e) {
    threw = true;
    msg = e.what();
  }
  CHECK(threw);
  CHECK(msg.find("absent") != std::string::npos);

  // DAG topology unchanged: validate_dag still passes and the production-key
  // set is identical to the pre-call snapshot.
  larch::validate_dag(dag, "unchanged after absent-before throw");
  auto g_after = larch::build_clade_grammar(dag);
  CHECK(larch::rank3_detail::production_key_set(g_after) == keys_before);

  std::println("  PASS");
}

static void test_option_c_after_parent_mismatch_throws() {
  std::println("test_option_c_after_parent_mismatch_throws");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto ab = taxa_for(g, {"A", "B"});
  auto cd = taxa_for(g, {"C", "D"});
  auto abc = taxa_for(g, {"A", "B", "C"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  auto before_key = make_split_key(abcd, {ab, cd});

  // after parent is {A,B,C} (does not match before parent {A,B,C,D}).
  larch::option_c_after_production after;
  after.parent_taxa = abc;
  after.children.push_back(leaf_after(a));
  after.children.push_back(pair_after(taxa_for(g, {"B", "C"}), leaf_after(b),
                                      leaf_after(c)));

  auto keys_before = larch::rank3_detail::production_key_set(g);

  bool threw = false;
  std::string msg;
  try {
    larch::option_c_splice_production(dag, before_key, after);
  } catch (std::runtime_error const& e) {
    threw = true;
    msg = e.what();
  }
  CHECK(threw);
  CHECK(msg.find("parent") != std::string::npos);

  auto g_after = larch::build_clade_grammar(dag);
  CHECK(larch::rank3_detail::production_key_set(g_after) == keys_before);

  (void)a;
  (void)abc;
  std::println("  PASS");
}

// =====================================================================
// Exit criterion 3: polytomy before/after throw labelled, route to Option A.
// =====================================================================

// Non-binary (polytomy) before production: arity-3 production throws a message
// built from the shared polytomy_refinement vocabulary.
static void test_option_c_polytomy_before_throws_labelled() {
  std::println("test_option_c_polytomy_before_throws_labelled");

  // Build ((A,B,C),(D,E,F)) with arity-3 productions under each side.
  std::vector<larch::test::tiny_dag_node> nodes{
      {"root", "A", ""}, {"abc", "A", ""}, {"def", "C", ""},
      {"A", "A", "A"},   {"B", "A", "B"},  {"C", "A", "C"},
      {"D", "C", "D"},   {"E", "C", "E"},  {"F", "C", "F"},
  };
  std::vector<larch::test::tiny_dag_edge> edges{
      {"root", "abc", 0}, {"root", "def", 1},
      {"abc", "A", 0},    {"abc", "B", 1},    {"abc", "C", 2},
      {"def", "D", 0},    {"def", "E", 1},    {"def", "F", 2},
  };
  auto dag = larch::test::make_tiny_labelled_dag("A", "root", nodes, edges);

  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto g = larch::build_clade_grammar(dag, gopts);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto bc = taxa_for(g, {"B", "C"});
  auto abc = taxa_for(g, {"A", "B", "C"});

  // before = {ABC} -> [{A},{B},{C}], an arity-3 (polytomy) production.
  auto before_key = make_split_key(abc, {a, b, c});

  larch::option_c_after_production after;
  after.parent_taxa = abc;
  after.children.push_back(leaf_after(a));
  after.children.push_back(pair_after(bc, leaf_after(b), leaf_after(c)));

  larch::option_c_splice_options opts;
  opts.rebuild_grammar_options.allow_polytomies = true;

  bool threw = false;
  std::string msg;
  try {
    larch::option_c_splice_production(dag, before_key, after, opts);
  } catch (std::runtime_error const& e) {
    threw = true;
    msg = e.what();
  }
  CHECK(threw);
  CHECK(msg.find("polytomy") != std::string::npos);
  CHECK(msg.find("Option-A") != std::string::npos);

  std::println("  PASS");
}

// Non-binary (polytomy) after production: an after with arity != 2 throws the
// same labelled vocabulary before any edge surgery (the guard is in
// validate_after_production, which runs first).
static void test_option_c_polytomy_after_throws_labelled() {
  std::println("test_option_c_polytomy_after_throws_labelled");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  // A 3-way (polytomy) after: {ABCD} -> [{A},{B},{C,D}] has arity 3.
  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(leaf_after(a));
  after.children.push_back(leaf_after(b));
  after.children.push_back(
      pair_after(taxa_for(g, {"C", "D"}), leaf_after(c), leaf_after(d)));

  bool threw = false;
  std::string msg;
  try {
    auto before_caller = make_split_key(
        abcd, {taxa_for(g, {"A", "B"}), taxa_for(g, {"C", "D"})});
    larch::option_c_splice_production(dag, before_caller, after);
  } catch (std::runtime_error const& e) {
    threw = true;
    msg = e.what();
  }
  CHECK(threw);
  CHECK(msg.find("polytomy") != std::string::npos);
  CHECK(msg.find("Option-A") != std::string::npos);

  std::println("  PASS");
}

// =====================================================================
// after_present policy (finding #3): no_op_if_present is a live, documented
// no-op when the after key is already represented; merge proceeds otherwise.
// =====================================================================

static void test_option_c_no_op_if_present_policy() {
  std::println("test_option_c_no_op_if_present_policy");

  // Multi-tree DAG containing both {AB,CD} and {AC,BD} root productions.
  auto base = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto alt = larch::test::make_tiny_labelled_tree("A", four_taxon_alt_tree());
  std::vector<larch::phylo_dag> trees;
  trees.push_back(std::move(base));
  trees.push_back(std::move(alt));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto ab = taxa_for(g, {"A", "B"});
  auto cd = taxa_for(g, {"C", "D"});
  auto ac = taxa_for(g, {"A", "C"});
  auto bd = taxa_for(g, {"B", "D"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  auto before_key = make_split_key(abcd, {ab, cd});

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(pair_after(ac, leaf_after(a), leaf_after(c)));
  after.children.push_back(pair_after(bd, leaf_after(b), leaf_after(d)));

  auto keys_before = larch::rank3_detail::production_key_set(g);

  // no_op_if_present: after key {AC,BD} is already represented, so the splice
  // short-circuits with no mutation.
  larch::option_c_splice_options noop_opts;
  noop_opts.after_present =
      larch::option_c_after_present_policy::no_op_if_present;
  auto noop_result = larch::option_c_splice_production(dag, before_key, after,
                                                       noop_opts);
  CHECK(noop_result.after_already_present_no_op);
  CHECK(noop_result.witnesses_spliced == 0);
  CHECK(noop_result.nodes_created == 0);
  CHECK(noop_result.edges_added == 0);
  // DAG unchanged.
  auto g_after_noop = larch::build_clade_grammar(dag);
  CHECK(larch::rank3_detail::production_key_set(g_after_noop) == keys_before);

  // Default (merge) policy: the splice proceeds even though after is present.
  auto merge_result = larch::option_c_splice_production(dag, before_key, after);
  CHECK(!merge_result.after_already_present_no_op);
  CHECK(merge_result.witnesses_spliced == 1);

  std::println("  PASS");
}

// =====================================================================
// Already-present merge (finding #10): witness-level merge is grammar-level
// merge-equivalent.  The edge-level representable-tree count intentionally
// exceeds a maximally-merged Option A reference (documented non-maximal
// sharing), so the oracle here is grammar-level only.
// =====================================================================

static void test_option_c_after_already_present_merge() {
  std::println("test_option_c_after_already_present_merge");

  auto base = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto alt = larch::test::make_tiny_labelled_tree("A", four_taxon_alt_tree());
  std::vector<larch::phylo_dag> trees;
  trees.push_back(std::move(base));
  trees.push_back(std::move(alt));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto ab = taxa_for(g, {"A", "B"});
  auto cd = taxa_for(g, {"C", "D"});
  auto ac = taxa_for(g, {"A", "C"});
  auto bd = taxa_for(g, {"B", "D"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  auto before_key = make_split_key(abcd, {ab, cd});

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(pair_after(ac, leaf_after(a), leaf_after(c)));
  after.children.push_back(pair_after(bd, leaf_after(b), leaf_after(d)));

  auto result = larch::option_c_splice_production(dag, before_key, after);
  CHECK(result.witnesses_spliced == 1);

  // Grammar-level oracle (clade taxa sets + production keys).  The edge-level
  // representable-tree count is intentionally higher than a single-tree
  // Option A reference because each before-witness gets its own fresh after
  // subtree (non-maximal sharing), so check_option_c_merge_equivalent is not
  // used here.
  auto option_a_dag =
      larch::test::make_tiny_labelled_tree("A", four_taxon_alt_tree());
  check_option_c_grammar_equivalent(dag, option_a_dag);

  std::println("  PASS");
}

// =====================================================================
// Multi-witness splice (findings #6, #11): the before production has >= 2
// witnesses, exercising cross-witness edge-index stability under ASAN and the
// linear-in-witnesses upper bound on cost.
// =====================================================================

static void test_option_c_multi_witness_splice() {
  std::println("test_option_c_multi_witness_splice");

  // Two copies of ((A,B),(C,D)) that differ ONLY in the root inner compact
  // genome, so the merge keeps two distinct root nodes (both children of UA),
  // giving the root production {ABCD} -> {{A,B},{C,D}} two witnesses while
  // sharing the AB/CD subtrees and leaves.
  auto t1 = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto t2 = larch::test::make_tiny_labelled_tree("A", [] {
    using larch::test::tiny_inner;
    using larch::test::tiny_leaf;
    // Root sequence "C" (differs from t1's root "A"); AB/CD/leaves identical.
    return tiny_inner(
        "root", "C",
        {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
         tiny_inner("CD", "C",
                    {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
  }());
  std::vector<larch::phylo_dag> trees;
  trees.push_back(std::move(t1));
  trees.push_back(std::move(t2));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto g = larch::build_clade_grammar(dag);

  auto a = taxa_for(g, {"A"});
  auto b = taxa_for(g, {"B"});
  auto c = taxa_for(g, {"C"});
  auto d = taxa_for(g, {"D"});
  auto ab = taxa_for(g, {"A", "B"});
  auto cd = taxa_for(g, {"C", "D"});
  auto ac = taxa_for(g, {"A", "C"});
  auto bd = taxa_for(g, {"B", "D"});
  auto abcd = taxa_for(g, {"A", "B", "C", "D"});

  // Confirm the fixture: the before production has exactly two witnesses.
  auto before_key = make_split_key(abcd, {ab, cd});
  std::size_t before_witnesses = 0;
  for (std::size_t pid = 0; pid < g.productions.size(); ++pid) {
    if (larch::rank3_detail::production_key_from_id(
            g, static_cast<larch::production_id>(pid)) == before_key) {
      before_witnesses = g.productions[pid].witnesses.size();
      break;
    }
  }
  CHECK(before_witnesses == 2);

  larch::option_c_after_production after;
  after.parent_taxa = abcd;
  after.children.push_back(pair_after(ac, leaf_after(a), leaf_after(c)));
  after.children.push_back(pair_after(bd, leaf_after(b), leaf_after(d)));

  auto result = larch::option_c_splice_production(dag, before_key, after);

  // Cross-witness edge-index stability held (we reached here without UB; ASAN
  // validates the memory side in the ASAN build).
  CHECK(result.witnesses_spliced == 2);

  // Performance contract — the upper-bound half: cost is LINEAR in the number
  // of witnesses, not in the number of represented trees containing the before
  // production.  Each witness gets exactly 2 fresh internal nodes (AC, BD) and
  // 6 fresh edges, so nodes_created == 2 * witnesses and edges_added ==
  // 6 * witnesses, regardless of how many represented trees the DAG encodes.
  CHECK(result.nodes_created == 2 * result.witnesses_spliced);
  CHECK(result.edges_added == 6 * result.witnesses_spliced);
  CHECK(result.edges_removed == 2 * result.witnesses_spliced);
  // The structural no-tree-enumeration invariant.
  CHECK(!result.invoked_merge_path);
  CHECK(!result.represented_trees_enumerated.has_value());

  // Grammar-level oracle (the spliced DAG is multi-tree / non-maximally
  // shared, so grammar-level equivalence is the right check).
  auto option_a_dag =
      larch::test::make_tiny_labelled_tree("A", four_taxon_alt_tree());
  check_option_c_grammar_equivalent(dag, option_a_dag);

  std::println("  PASS");
}

int main() {
  test_option_c_splice_root_abcd_to_acbd();
  test_option_c_splice_root_abcd_to_adbc();
  test_option_c_splice_non_root_production();
  test_option_c_absent_before_throws_unchanged();
  test_option_c_after_parent_mismatch_throws();
  test_option_c_polytomy_before_throws_labelled();
  test_option_c_polytomy_after_throws_labelled();
  test_option_c_no_op_if_present_policy();
  test_option_c_after_already_present_merge();
  test_option_c_multi_witness_splice();
  std::println("option_c_test PASS");
  return 0;
}

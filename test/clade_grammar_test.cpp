#include <larch/clade_grammar.hpp>
#include <larch/load_proto_dag.hpp>

#include "test_util.hpp"

#include <print>
#include <cstdlib>
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

static larch::test::tiny_tree_node five_taxon_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAAA",
      {tiny_inner("AB", "AAAA",
                  {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA")}),
       tiny_inner("CDE", "AAAA",
                  {tiny_leaf("C", "ACAA"),
                   tiny_inner("DE", "AAAA",
                              {tiny_leaf("D", "AAGA"),
                               tiny_leaf("E", "AAAT")})})});
}

static larch::test::tiny_tree_node alternate_cde_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAAA",
      {tiny_inner("AB", "AAAA",
                  {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA")}),
       tiny_inner("CDE", "CAAA",
                  {tiny_leaf("D", "AAGA"),
                   tiny_inner("CE", "AAAA",
                              {tiny_leaf("C", "ACAA"),
                               tiny_leaf("E", "AAAT")})})});
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
  for (auto const& sample_id : sample_ids) ids.push_back(taxon_for(grammar, sample_id));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa == ids) return static_cast<larch::clade_id>(cid);
  }
  CHECK(false && "missing clade");
  return larch::no_clade;
}

static larch::grammar_production const& production_for(
    larch::clade_grammar const& grammar, larch::clade_id parent,
    std::vector<larch::clade_id> children) {
  std::sort(children.begin(), children.end());
  for (auto pid : grammar.productions_by_parent[parent]) {
    auto const& prod = grammar.productions[pid];
    auto prod_children = prod.children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == children) return prod;
  }
  CHECK(false && "missing production");
  return grammar.productions.front();
}

static bool throws_grammar_error(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const&) {
    return true;
  }
  return false;
}

static void test_single_tree_clades_and_productions() {
  std::println("test_single_tree_clades_and_productions");

  auto tree = larch::test::make_tiny_labelled_tree("AAAA", five_taxon_tree_spec());
  auto built = larch::build_clade_grammar_with_audit(tree);
  auto const& grammar = built.grammar;
  auto const& audit = built.audit;

  CHECK(grammar.taxa.id_to_sample_id.size() == 5);
  CHECK(grammar.clades.size() == 9);  // 5 leaves + AB + DE + CDE + root
  CHECK(grammar.productions.size() == 4);
  CHECK(grammar.root_clade == clade_for(grammar, {"A", "B", "C", "D", "E"}));
  CHECK(grammar.productions_by_parent[grammar.root_clade].size() == 1);
  CHECK(audit.grammar_tree_count_estimate == 1);

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa.size() == 1) {
      CHECK(grammar.productions_by_parent[cid].empty());
    }
  }
  for (auto const& prod : grammar.productions) {
    CHECK(prod.children.size() == 2);
    for (auto child : prod.children) CHECK(child != prod.parent);
  }

  std::println("  PASS");
}

static void test_two_trees_deduplicate_shared_clades() {
  std::println("test_two_trees_deduplicate_shared_clades");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("AAAA", five_taxon_tree_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree("AAAA", alternate_cde_tree_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto built = larch::build_clade_grammar_with_audit(merged);
  auto const& grammar = built.grammar;

  auto ab = clade_for(grammar, {"A", "B"});
  auto cde = clade_for(grammar, {"C", "D", "E"});
  auto root = clade_for(grammar, {"A", "B", "C", "D", "E"});

  CHECK(grammar.productions_by_parent[ab].size() == 1);
  CHECK(grammar.productions_by_parent[cde].size() == 2);
  CHECK(grammar.productions_by_parent[root].size() == 1);
  CHECK(built.audit.grammar_tree_count_estimate == 2);

  std::println("  PASS");
}

static void test_compact_genome_redundancy_collapses_by_leaf_set() {
  std::println("test_compact_genome_redundancy_collapses_by_leaf_set");

  auto dag = larch::test::make_tiny_labelled_dag(
      "AAAA", "root",
      {{"root", "AAAA", ""},
       {"ab1", "AAAA", ""},
       {"ab2", "CAAA", ""},
       {"A", "AAAA", "A"},
       {"B", "CAAA", "B"},
       {"C", "AAGA", "C"}},
      {{"root", "ab1", 0},
       {"root", "ab2", 0},
       {"root", "C", 1},
       {"ab1", "A", 0},
       {"ab1", "B", 1},
       {"ab2", "A", 0},
       {"ab2", "B", 1}});

  auto built = larch::build_clade_grammar_with_audit(dag);
  auto const& grammar = built.grammar;
  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto root = clade_for(grammar, {"A", "B", "C"});

  CHECK(built.audit.collapsed_clade_node_count[ab] == 2);
  auto const& ab_prod = production_for(grammar, ab, {a, b});
  CHECK(ab_prod.multiplicity == 2);

  auto const& root_prod = production_for(grammar, root, {ab, c});
  bool saw_ab_alternatives = false;
  CHECK(root_prod.witnesses.size() == 1);
  for (auto const& child_witness : root_prod.witnesses.front().children) {
    if (child_witness.child == ab) {
      CHECK(child_witness.edge_alternatives.size() == 2);
      saw_ab_alternatives = true;
    }
  }
  CHECK(saw_ab_alternatives);

  std::println("  PASS");
}

static void test_taxon_identity_policy() {
  std::println("test_taxon_identity_policy");

  CHECK(throws_grammar_error([] {
    auto dag = larch::test::make_tiny_labelled_tree(
        "AAAA", larch::test::tiny_leaf("", "AAAA"));
    (void)larch::build_clade_grammar(dag);
  }));

  auto duplicate_ok = larch::test::make_tiny_labelled_dag(
      "AAAA", "root",
      {{"root", "AAAA", ""}, {"a1", "AAAA", "A"}, {"a2", "AAAA", "A"}},
      {{"root", "a1", 0}, {"root", "a2", 0}});
  auto built = larch::build_clade_grammar_with_audit(duplicate_ok);
  CHECK(built.grammar.taxa.id_to_sample_id.size() == 1);
  CHECK(built.audit.duplicate_sample_id_occurrences == 1);
  CHECK(built.grammar.productions.empty());

  CHECK(throws_grammar_error([] {
    auto duplicate_bad = larch::test::make_tiny_labelled_dag(
        "AAAA", "root",
        {{"root", "AAAA", ""}, {"a1", "AAAA", "A"}, {"a2", "CAAA", "A"}},
        {{"root", "a1", 0}, {"root", "a2", 0}});
    (void)larch::build_clade_grammar(duplicate_bad);
  }));

  std::println("  PASS");
}

static void test_invalid_clade_group_alternatives_fail() {
  std::println("test_invalid_clade_group_alternatives_fail");

  CHECK(throws_grammar_error([] {
    auto dag = larch::test::make_tiny_labelled_dag(
        "AAAA", "root",
        {{"root", "AAAA", ""}, {"A", "AAAA", "A"}, {"B", "CAAA", "B"}},
        {{"root", "A", 0}, {"root", "B", 0}});
    (void)larch::build_clade_grammar(dag);
  }));

  std::println("  PASS");
}

static void test_invalid_child_partition_fails() {
  std::println("test_invalid_child_partition_fails");

  CHECK(throws_grammar_error([] {
    auto dag = larch::test::make_tiny_labelled_dag(
        "AAAA", "root",
        {{"root", "AAAA", ""},
         {"AB", "AAAA", ""},
         {"AC", "AAAA", ""},
         {"A", "AAAA", "A"},
         {"B", "CAAA", "B"},
         {"C", "AAGA", "C"}},
        {{"root", "AB", 0},
         {"root", "AC", 1},
         {"AB", "A", 0},
         {"AB", "B", 1},
         {"AC", "A", 0},
         {"AC", "C", 1}});
    (void)larch::build_clade_grammar(dag);
  }));

  std::println("  PASS");
}

static void test_polytomy_policy() {
  std::println("test_polytomy_policy");

  CHECK(throws_grammar_error([] {
    auto dag = larch::test::make_tiny_labelled_tree(
        "AAAA", larch::test::tiny_inner(
                    "root", "AAAA",
                    {larch::test::tiny_leaf("A", "AAAA"),
                     larch::test::tiny_leaf("B", "CAAA"),
                     larch::test::tiny_leaf("C", "AAGA")}));
    (void)larch::build_clade_grammar(dag);
  }));

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAAA", larch::test::tiny_inner(
                  "root", "AAAA",
                  {larch::test::tiny_leaf("A", "AAAA"),
                   larch::test::tiny_leaf("B", "CAAA"),
                   larch::test::tiny_leaf("C", "AAGA")}));
  larch::clade_grammar_options opts;
  opts.allow_polytomies = true;
  auto built = larch::build_clade_grammar_with_audit(dag, opts);
  CHECK(built.audit.non_binary_production_count == 1);
  CHECK(built.grammar.productions.size() == 1);
  CHECK(built.grammar.productions.front().children.size() == 3);

  std::println("  PASS");
}

static void test_existing_fixture_builds() {
  std::println("test_existing_fixture_builds");

  std::vector<larch::phylo_dag> dags;
  for (int i = 0; i < 5; ++i) {
    auto path = larch::test::source_path_string(
        "data/test_5_trees/tree_" + std::to_string(i) + ".pb.gz");
    dags.push_back(larch::load_proto_dag(path));
  }

  auto const reference = larch::get_reference_sequence(dags.front());
  larch::merge merger{reference};
  for (auto& dag : dags) merger.add_dag(dag);
  auto merged = std::move(merger.get_result());
  larch::clade_grammar_options opts;
  opts.allow_polytomies = true;
  auto built = larch::build_clade_grammar_with_audit(merged, opts);

  CHECK(built.audit.taxon_count > 0);
  CHECK(built.audit.collapsed_clade_count >= built.audit.taxon_count);
  CHECK(built.audit.grammar_production_count > 0);
  CHECK(built.audit.invalid_clade_index_group_count == 0);
  CHECK(built.audit.non_binary_production_count > 0);

  std::println("  fixture taxa={}, clades={}, productions={}, polytomies={}",
               built.audit.taxon_count, built.audit.collapsed_clade_count,
               built.audit.grammar_production_count,
               built.audit.non_binary_production_count);
  std::println("  PASS");
}

int main() {
  test_single_tree_clades_and_productions();
  test_two_trees_deduplicate_shared_clades();
  test_compact_genome_redundancy_collapses_by_leaf_set();
  test_taxon_identity_policy();
  test_invalid_clade_group_alternatives_fail();
  test_invalid_child_partition_fails();
  test_polytomy_policy();
  test_existing_fixture_builds();

  std::println("All clade grammar tests passed!");
  return 0;
}

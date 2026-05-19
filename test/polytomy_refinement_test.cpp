#include <larch/parsimony_chart.hpp>
#include <larch/polytomy_refinement.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <print>
#include <stdexcept>
#include <string>
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
      "AAAA",
      tiny_inner(
          "root", "AAAA",
          {tiny_leaf("A", "AAAA"),
           tiny_inner("BC", "AAAA",
                      {tiny_leaf("B", "CAAA"), tiny_leaf("C", "AAGA")})}));
}

static larch::phylo_dag make_four_taxon_star() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA", tiny_inner("root", "AAAA",
                         {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA"),
                          tiny_leaf("C", "AAGA"), tiny_leaf("D", "AAAT")}));
}

static larch::phylo_dag make_four_taxon_abcd_polytomy_with_ab() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA",
      tiny_inner("root", "AAAA",
                 {tiny_inner("AB", "AAAA",
                             {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA")}),
                  tiny_leaf("C", "AAGA"), tiny_leaf("D", "AAAT")}));
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

static larch::production_id production_for(
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

static std::uint64_t count_derivations(larch::clade_grammar const& grammar) {
  std::vector<std::uint64_t> counts(grammar.clades.size(), 0);
  std::vector<larch::clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), larch::clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](auto lhs, auto rhs) {
    auto const& ltaxa = grammar.clades[lhs].taxa;
    auto const& rtaxa = grammar.clades[rhs].taxa;
    if (ltaxa.size() != rtaxa.size()) return ltaxa.size() < rtaxa.size();
    return ltaxa < rtaxa;
  });

  for (auto cid : order) {
    if (grammar.clades[cid].taxa.size() == 1) {
      counts[cid] = 1;
      continue;
    }
    for (auto pid : grammar.productions_by_parent[cid]) {
      std::uint64_t prod_count = 1;
      for (auto child : grammar.productions[pid].children)
        prod_count *= counts[child];
      counts[cid] += prod_count;
    }
  }
  return counts[grammar.root_clade];
}

static larch::chart_cost brute_force_star_score(
    std::size_t leaf_count, std::vector<std::uint8_t> const& states) {
  using row_t = std::array<larch::chart_cost, larch::nuc_state_count>;
  constexpr auto inf = larch::chart_inf;

  std::map<std::uint64_t, std::vector<row_t>> memo;
  auto combine = [inf](row_t const& left, row_t const& right) {
    row_t out{};
    out.fill(inf);
    for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
         ++parent_state) {
      larch::chart_cost best_left = inf;
      larch::chart_cost best_right = inf;
      for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
           ++child_state) {
        auto left_cost =
            left[child_state] >= inf
                ? inf
                : left[child_state] + (parent_state == child_state ? 0 : 1);
        auto right_cost =
            right[child_state] >= inf
                ? inf
                : right[child_state] + (parent_state == child_state ? 0 : 1);
        best_left = std::min(best_left, left_cost);
        best_right = std::min(best_right, right_cost);
      }
      out[parent_state] = best_left + best_right;
    }
    return out;
  };

  auto rows_for = [&](auto& self,
                      std::uint64_t mask) -> std::vector<row_t> const& {
    auto found = memo.find(mask);
    if (found != memo.end()) return found->second;

    std::vector<row_t> rows;
    if (std::popcount(mask) == 1) {
      auto leaf_index = static_cast<std::size_t>(std::countr_zero(mask));
      row_t row{};
      row.fill(inf);
      row[states[leaf_index]] = 0;
      rows.push_back(row);
    } else {
      auto lowest = mask & (~mask + 1);
      for (auto sub = (mask - 1) & mask; sub != 0; sub = (sub - 1) & mask) {
        if ((sub & lowest) == 0) continue;
        auto other = mask ^ sub;
        if (other == 0) continue;
        for (auto const& left : self(self, sub)) {
          for (auto const& right : self(self, other))
            rows.push_back(combine(left, right));
        }
      }
    }

    auto [it, _] = memo.emplace(mask, std::move(rows));
    return it->second;
  };

  auto full = (std::uint64_t{1} << leaf_count) - 1;
  larch::chart_cost best = inf;
  for (auto const& row : rows_for(rows_for, full))
    for (auto cost : row) best = std::min(best, cost);
  return best;
}

static void test_default_reject_mode_rejects_kary() {
  std::println("test_default_reject_mode_rejects_kary");

  auto dag = make_three_taxon_star();
  CHECK(throws_runtime_error(
      [&] { (void)larch::build_polytomy_refined_clade_grammar(dag); }));

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
  CHECK(info.origin ==
        larch::refined_production_origin::observed_kary_unexpanded);
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

  CHECK(throws_runtime_error(
      [&] { (void)larch::build_single_site_chart(result.grammar, states); }));

  std::println("  PASS");
}

static void test_exact_expansion_three_taxon_star() {
  std::println("test_exact_expansion_three_taxon_star");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(!larch::grammar_has_kary_productions(grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.binary_chart_compatible);
  CHECK(result.audit.exact_for_soft_polytomies);
  CHECK(!result.audit.contains_kary_productions);
  CHECK(result.audit.source_kary_production_count == 1);
  CHECK(result.audit.refined_clade_count == 7);
  CHECK(result.audit.refined_production_count == 6);
  CHECK(result.audit.synthetic_clade_count == 3);
  CHECK(result.audit.synthetic_production_count == 6);
  CHECK(result.audit.events.size() == 1);
  CHECK(result.audit.events.front().expanded);
  CHECK(result.audit.events.front().exact);
  CHECK(result.audit.events.front().arity == 3);
  CHECK(result.audit.events.front().new_clades_added == 3);
  CHECK(result.audit.events.front().new_productions_added == 6);
  CHECK(result.audit.events.front().represented_refinement_count == 3);
  CHECK(count_derivations(grammar) == 3);

  (void)clade_for(grammar, {"A", "B"});
  (void)clade_for(grammar, {"A", "C"});
  (void)clade_for(grammar, {"B", "C"});

  for (auto pid : result.source_production_to_refined)
    CHECK(pid == larch::no_production);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    CHECK(grammar.productions[pid].children.size() == 2);
    CHECK(grammar.productions[pid].multiplicity == 0);
    CHECK(result.production_info[pid].origin ==
          larch::refined_production_origin::synthetic_polytomy_refinement);
    CHECK(result.production_info[pid].source_productions.size() == 1);
  }

  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);
  CHECK(chart.root_min_excluding_ua(grammar.root_clade) == 1);

  std::println("  PASS");
}

static void test_exact_expansion_four_taxon_star_matches_bruteforce() {
  std::println("test_exact_expansion_four_taxon_star_matches_bruteforce");

  auto dag = make_four_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.refined_clade_count == 15);
  CHECK(result.audit.refined_production_count == 25);
  CHECK(result.audit.events.size() == 1);
  CHECK(result.audit.events.front().represented_refinement_count == 15);
  CHECK(count_derivations(grammar) == 15);

  for (std::uint32_t packed = 0; packed < 256; ++packed) {
    larch::leaf_site_states states;
    states.state_by_taxon.assign(grammar.taxa.id_to_sample_id.size(), 0);
    std::vector<std::uint8_t> ordered_states(4, 0);
    for (std::size_t i = 0; i < 4; ++i) {
      auto state = static_cast<std::uint8_t>((packed >> (2 * i)) & 3U);
      ordered_states[i] = state;
      std::string sample(1, static_cast<char>('A' + i));
      states.state_by_taxon[taxon_for(grammar, sample)] = state;
    }

    auto chart = larch::build_single_site_chart(grammar, states);
    auto chart_best = chart.root_min_excluding_ua(grammar.root_clade);
    CHECK(chart_best == brute_force_star_score(4, ordered_states));
  }

  std::println("  PASS");
}

static void test_exact_expansion_reuses_existing_observed_clade() {
  std::println("test_exact_expansion_reuses_existing_observed_clade");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(make_three_taxon_star());
  trees.push_back(make_three_taxon_binary_tree());
  auto dag = larch::test::merge_tiny_trees(std::move(trees));

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.source_kary_production_count == 1);
  CHECK(result.audit.synthetic_clade_count ==
        2);  // AB and AC; BC was observed.
  CHECK(result.audit.refined_clade_count == 7);
  CHECK(count_derivations(grammar) == 3);

  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto bc = clade_for(grammar, {"B", "C"});
  CHECK(result.clade_info[bc].origin ==
        larch::refined_clade_origin::observed_and_synthetic);

  auto bc_prod = production_for(grammar, bc, {b, c});
  CHECK(grammar.productions[bc_prod].multiplicity > 0);
  CHECK(result.production_info[bc_prod].origin ==
        larch::refined_production_origin::observed_and_synthetic);
  CHECK(result.production_info[bc_prod].source_productions.size() == 2);

  std::println("  PASS");
}

static void test_exact_expansion_merges_reversed_observed_witnesses() {
  std::println("test_exact_expansion_merges_reversed_observed_witnesses");

  larch::clade_grammar_build_result built;
  auto& source = built.grammar;
  source.taxa.id_to_sample_id = {"A", "B", "C"};
  source.taxa.sample_id_to_id = {{"A", 0}, {"B", 1}, {"C", 2}};
  source.clades = {larch::clade_key{{0}}, larch::clade_key{{1}},
                   larch::clade_key{{2}}, larch::clade_key{{0, 1}},
                   larch::clade_key{{0, 1, 2}}};
  source.root_clade = 4;

  auto witness = [](std::size_t parent_node,
                    std::vector<larch::clade_id> children) {
    larch::production_witness w;
    w.parent_node = parent_node;
    for (auto child : children) {
      larch::production_child_witness child_witness;
      child_witness.child = child;
      child_witness.edge_alternatives = {parent_node * 10 + child};
      w.children.push_back(std::move(child_witness));
    }
    return w;
  };

  auto make_prod = [&](larch::clade_id parent,
                       std::vector<larch::clade_id> children,
                       std::size_t parent_node) {
    larch::grammar_production prod;
    prod.parent = parent;
    prod.children = children;
    prod.witnesses = {witness(parent_node, children)};
    prod.multiplicity = 1;
    return prod;
  };

  source.productions.push_back(make_prod(3, {0, 1}, 10));
  source.productions.push_back(make_prod(3, {1, 0}, 11));
  source.productions.push_back(make_prod(4, {0, 1, 2}, 12));

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  opts.canonicalize_binary_children = true;
  auto result = larch::polytomy_refinement_detail::make_exact_expansion_result(
      std::move(built), opts);
  auto const& grammar = result.grammar;

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto ab_prod = production_for(grammar, ab, {a, b});

  CHECK(grammar.productions[ab_prod].multiplicity == 2);
  CHECK(grammar.productions[ab_prod].witnesses.size() == 2);
  CHECK(result.production_info[ab_prod].origin ==
        larch::refined_production_origin::observed_and_synthetic);
  CHECK(result.production_info[ab_prod].source_productions.size() == 3);

  std::println("  PASS");
}

static void test_exact_expansion_merges_synthetic_provenance() {
  std::println("test_exact_expansion_merges_synthetic_provenance");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(make_four_taxon_star());
  trees.push_back(make_four_taxon_abcd_polytomy_with_ab());
  auto dag = larch::test::merge_tiny_trees(std::move(trees));

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(result.audit.source_kary_production_count == 2);
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto cd_prod = production_for(grammar, cd, {c, d});
  CHECK(result.production_info[cd_prod].origin ==
        larch::refined_production_origin::synthetic_polytomy_refinement);
  CHECK(result.production_info[cd_prod].source_productions.size() == 2);
  CHECK(grammar.productions[cd_prod].multiplicity == 0);

  std::println("  PASS");
}

static void test_exact_expansion_caps_throw() {
  std::println("test_exact_expansion_caps_throw");

  auto dag = make_four_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  opts.max_exact_arity = 3;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_polytomy_refined_clade_grammar(
        dag, larch::clade_grammar_options{}, opts);
  }));

  opts.max_exact_arity = 6;
  opts.max_new_productions_per_polytomy = 5;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_polytomy_refined_clade_grammar(
        dag, larch::clade_grammar_options{}, opts);
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
  test_exact_expansion_three_taxon_star();
  test_exact_expansion_four_taxon_star_matches_bruteforce();
  test_exact_expansion_reuses_existing_observed_clade();
  test_exact_expansion_merges_reversed_observed_witnesses();
  test_exact_expansion_merges_synthetic_provenance();
  test_exact_expansion_caps_throw();
  test_binary_compatibility_rejects_out_of_range_taxa();

  std::println("All polytomy refinement tests passed!");
  return 0;
}

#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/site_patterns.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <variant>
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

static std::uint64_t explicit_uncompressed_total(
    larch::phylo_dag& dag, larch::clade_grammar const& grammar,
    bool score_ua_edge) {
  std::uint64_t total = 0;
  auto const& reference = larch::get_reference_sequence(dag);
  for (larch::mutation_position pos = 1; pos <= reference.size(); ++pos) {
    auto states = larch::extract_leaf_site_states(dag, grammar, pos);
    auto chart = larch::build_single_site_chart(grammar, states);
    if (score_ua_edge) {
      total += chart.root_min_with_reference_edge(
          grammar.root_clade, larch::extract_reference_site_state(dag, pos));
    } else {
      total += chart.root_min_excluding_ua(grammar.root_clade);
    }
  }
  return total;
}

static void test_identical_sites_collapse_and_weight() {
  std::println("test_identical_sites_collapse_and_weight");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AA", larch::test::tiny_inner(
                "root", "AA", {larch::test::tiny_leaf("A", "AA"),
                                  larch::test::tiny_leaf("B", "CC")}));
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  CHECK(patterns.total_site_count == 2);
  CHECK(patterns.patterns.size() == 1);
  CHECK(patterns.patterns[0].weight == 2);
  CHECK((patterns.patterns[0].positions ==
         std::vector<larch::mutation_position>{1, 2}));
  CHECK(patterns.original_site_to_pattern.size() == 2);
  CHECK(patterns.original_site_to_pattern[0] == 0);
  CHECK(patterns.original_site_to_pattern[1] == 0);
  CHECK(patterns.invariant_site_count == 0);
  CHECK(patterns.variable_site_count == 2);
  CHECK(patterns.binary_variable_site_count == 2);

  std::size_t one_arg_callback_count = 0;
  larch::for_each_site_pattern(
      patterns, [&](larch::site_pattern const&) { ++one_arg_callback_count; });
  CHECK(one_arg_callback_count == 1);

  std::size_t two_arg_index_sum = 0;
  larch::for_each_site_pattern(
      patterns, [&](std::size_t idx, larch::site_pattern const&) {
        two_arg_index_sum += idx;
      });
  CHECK(two_arg_index_sum == 0);

  auto charts = larch::build_pattern_charts(grammar, patterns);
  CHECK(charts.size() == 1);
  CHECK(larch::weighted_pattern_chart_total(charts, patterns,
                                            grammar.root_clade) == 2);
  CHECK(explicit_uncompressed_total(dag, grammar, false) == 2);

  std::println("  PASS");
}

static void test_invariant_sites_can_be_skipped_with_constants() {
  std::println("test_invariant_sites_can_be_skipped_with_constants");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAAA", larch::test::tiny_inner(
                  "root", "AAAA",
                  {larch::test::tiny_leaf("L1", "ACAA"),
                   larch::test::tiny_leaf("L2", "ACCA")}));
  auto grammar = larch::build_clade_grammar(dag);

  larch::site_pattern_options keep_opts;
  auto keep = larch::build_site_patterns(dag, grammar, keep_opts);
  CHECK(keep.total_site_count == 4);
  CHECK(keep.patterns.size() == 3);
  CHECK(keep.invariant_site_count == 3);
  CHECK(keep.variable_site_count == 1);
  CHECK(keep.invariant_constant_score_excluding_ua == 0);
  CHECK(keep.invariant_constant_score_with_reference_edge == 1);

  larch::site_pattern_options skip_opts;
  skip_opts.skip_invariant_sites = true;
  auto skipped = larch::build_site_patterns(dag, grammar, skip_opts);
  CHECK(skipped.patterns.size() == 1);
  CHECK(skipped.skipped_invariant_site_count == 3);
  CHECK(skipped.skipped_invariant_constant_score_with_reference_edge == 1);
  CHECK(skipped.original_site_to_pattern[0] == larch::no_site_pattern);
  CHECK(skipped.original_site_to_pattern[1] == larch::no_site_pattern);
  CHECK(skipped.original_site_to_pattern[2] == 0);
  CHECK(skipped.original_site_to_pattern[3] == larch::no_site_pattern);

  auto keep_charts = larch::build_pattern_charts(grammar, keep);
  auto skipped_charts = larch::build_pattern_charts(grammar, skipped);
  larch::chart_options ua_free;
  CHECK(larch::weighted_pattern_chart_total(keep_charts, keep,
                                            grammar.root_clade, ua_free) ==
        larch::weighted_pattern_chart_total(skipped_charts, skipped,
                                            grammar.root_clade, ua_free));

  larch::chart_options with_ua;
  with_ua.score_ua_edge = true;
  CHECK(larch::weighted_pattern_chart_total(keep_charts, keep,
                                            grammar.root_clade, with_ua) == 2);
  CHECK(larch::weighted_pattern_chart_total(skipped_charts, skipped,
                                            grammar.root_clade, with_ua) == 2);

  std::println("  PASS");
}

static void test_binary_normalization_groups_complements() {
  std::println("test_binary_normalization_groups_complements");

  auto dag = larch::test::make_tiny_labelled_tree(
      "GG", larch::test::tiny_inner(
                "root", "GG",
                {larch::test::tiny_inner(
                     "T01", "GG",
                     {larch::test::tiny_leaf("T0", "CT"),
                      larch::test::tiny_leaf("T1", "CT")}),
                 larch::test::tiny_inner(
                     "T23", "GG",
                     {larch::test::tiny_leaf("T2", "TC"),
                      larch::test::tiny_leaf("T3", "TC")})}));
  auto grammar = larch::build_clade_grammar(dag);

  larch::site_pattern_options opts;
  opts.build_normalized_binary_patterns = true;
  auto patterns = larch::build_site_patterns(dag, grammar, opts);

  CHECK(patterns.patterns.size() == 2);
  CHECK(patterns.binary_variable_site_count == 2);
  CHECK(patterns.normalized_binary_patterns.size() == 1);
  auto const& normalized = patterns.normalized_binary_patterns.front();
  CHECK(normalized.weight == 2);
  CHECK((normalized.positions == std::vector<larch::mutation_position>{1, 2}));
  CHECK((normalized.state_by_taxon == std::vector<std::uint8_t>{0, 0, 1, 1}));
  CHECK(patterns.exact_pattern_to_normalized_binary_pattern.size() == 2);
  CHECK(patterns.exact_pattern_to_normalized_binary_pattern[0] == 0);
  CHECK(patterns.exact_pattern_to_normalized_binary_pattern[1] == 0);

  auto exact_charts = larch::build_pattern_charts(grammar, patterns);
  auto first_pattern = patterns.original_site_to_pattern[0];
  auto complement_pattern = patterns.original_site_to_pattern[1];

  CHECK(patterns.exact_pattern_to_normalized_binary_state_map.size() == 2);
  auto const& first_map =
      patterns.exact_pattern_to_normalized_binary_state_map[first_pattern];
  auto const& complement_map =
      patterns.exact_pattern_to_normalized_binary_state_map[complement_pattern];
  CHECK(first_map.normalized_to_original[0] == larch::nuc_base::C);
  CHECK(first_map.normalized_to_original[1] == larch::nuc_base::T);
  CHECK(complement_map.normalized_to_original[0] == larch::nuc_base::T);
  CHECK(complement_map.normalized_to_original[1] == larch::nuc_base::C);

  larch::leaf_site_states normalized_states;
  normalized_states.state_by_taxon = normalized.state_by_taxon;
  auto normalized_chart =
      larch::build_single_site_chart(grammar, normalized_states);

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    CHECK(exact_charts[first_pattern].inside[cid] ==
          larch::remap_normalized_binary_chart_row(
              normalized_chart.inside[cid], first_map));
    CHECK(exact_charts[complement_pattern].inside[cid] ==
          larch::remap_normalized_binary_chart_row(
              normalized_chart.inside[cid], complement_map));
  }

  larch::chart_options with_ua;
  with_ua.score_ua_edge = true;
  CHECK(larch::weighted_root_min_from_normalized_binary_chart(
            normalized_chart, patterns.patterns[first_pattern], first_map,
            grammar.root_clade, with_ua) ==
        larch::weighted_root_min(exact_charts[first_pattern],
                                 patterns.patterns[first_pattern],
                                 grammar.root_clade, with_ua));
  CHECK(larch::weighted_root_min_from_normalized_binary_chart(
            normalized_chart, patterns.patterns[complement_pattern],
            complement_map, grammar.root_clade, with_ua) ==
        larch::weighted_root_min(exact_charts[complement_pattern],
                                 patterns.patterns[complement_pattern],
                                 grammar.root_clade, with_ua));

  std::println("  PASS");
}

static void test_weighted_chart_score_matches_uncompressed_sites() {
  std::println("test_weighted_chart_score_matches_uncompressed_sites");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AG", larch::test::tiny_inner(
                "root", "AG", {larch::test::tiny_leaf("L1", "AA"),
                                  larch::test::tiny_leaf("L2", "CC")}));
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  CHECK(patterns.patterns.size() == 1);
  CHECK(patterns.patterns.front().weight == 2);
  CHECK(patterns.patterns.front().reference_state_counts[larch::nuc_base::A] ==
        1);
  CHECK(patterns.patterns.front().reference_state_counts[larch::nuc_base::G] ==
        1);

  auto charts = larch::build_pattern_charts(grammar, patterns);
  larch::chart_options ua_free;
  CHECK(larch::weighted_pattern_chart_total(charts, patterns,
                                            grammar.root_clade, ua_free) ==
        explicit_uncompressed_total(dag, grammar, false));

  larch::chart_options with_ua;
  with_ua.score_ua_edge = true;
  CHECK(larch::weighted_pattern_chart_total(charts, patterns,
                                            grammar.root_clade, with_ua) ==
        explicit_uncompressed_total(dag, grammar, true));
  CHECK(larch::weighted_pattern_chart_total(charts, patterns,
                                            grammar.root_clade, with_ua) == 3);

  std::println("  PASS");
}

static void test_strict_validation_errors() {
  std::println("test_strict_validation_errors");

  CHECK(throws_runtime_error([] {
    auto bad = larch::test::make_tiny_labelled_tree(
        "N", larch::test::tiny_leaf("bad", "N"));
    auto grammar = larch::build_clade_grammar(bad);
    (void)larch::build_site_patterns(bad, grammar);
  }));

  CHECK(throws_runtime_error([] {
    auto bad = larch::test::make_tiny_labelled_tree(
        "A", larch::test::tiny_leaf("bad", "A"));
    for (auto nv : bad.get_all_nodes()) {
      std::visit(
          [](auto node) {
            if constexpr (requires {
                            node.sample_id();
                            node.cg();
                          }) {
              node.cg() = larch::compact_genome{
                  std::map<larch::mutation_position, larch::nuc_base>{
                      {1, larch::nuc_base{7}}}};
            }
          },
          nv);
    }
    auto grammar = larch::build_clade_grammar(bad);
    (void)larch::build_site_patterns(bad, grammar);
  }));

  std::println("  PASS");
}

int main() {
  test_identical_sites_collapse_and_weight();
  test_invariant_sites_can_be_skipped_with_constants();
  test_binary_normalization_groups_complements();
  test_weighted_chart_score_matches_uncompressed_sites();
  test_strict_validation_errors();

  std::println("All site pattern tests passed!");
  return 0;
}

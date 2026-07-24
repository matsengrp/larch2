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
#include <utility>
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

static void check_normalized_binary_state_map_equal(
    larch::normalized_binary_state_map const& lhs,
    larch::normalized_binary_state_map const& rhs) {
  CHECK(lhs.exact_pattern == rhs.exact_pattern);
  CHECK(lhs.normalized_binary_pattern == rhs.normalized_binary_pattern);
  CHECK(lhs.normalized_to_original == rhs.normalized_to_original);
  CHECK(lhs.original_to_normalized == rhs.original_to_normalized);
}

static void check_site_pattern_sets_equal(larch::site_pattern_set const& lhs,
                                          larch::site_pattern_set const& rhs) {
  CHECK(lhs.patterns.size() == rhs.patterns.size());
  for (std::size_t index = 0; index < lhs.patterns.size(); ++index) {
    auto const& left = lhs.patterns[index];
    auto const& right = rhs.patterns[index];
    CHECK(left.state_by_taxon == right.state_by_taxon);
    CHECK(left.positions == right.positions);
    CHECK(left.weight == right.weight);
    CHECK(left.reference_state_counts == right.reference_state_counts);
  }

  CHECK(lhs.original_site_to_pattern == rhs.original_site_to_pattern);
  CHECK(lhs.normalized_binary_patterns.size() ==
        rhs.normalized_binary_patterns.size());
  for (std::size_t index = 0; index < lhs.normalized_binary_patterns.size();
       ++index) {
    auto const& left = lhs.normalized_binary_patterns[index];
    auto const& right = rhs.normalized_binary_patterns[index];
    CHECK(left.state_by_taxon == right.state_by_taxon);
    CHECK(left.positions == right.positions);
    CHECK(left.weight == right.weight);
    CHECK(left.exact_pattern_indices == right.exact_pattern_indices);
    CHECK(left.exact_state_maps.size() == right.exact_state_maps.size());
    for (std::size_t map_index = 0;
         map_index < left.exact_state_maps.size(); ++map_index) {
      check_normalized_binary_state_map_equal(left.exact_state_maps[map_index],
                                               right.exact_state_maps[map_index]);
    }
  }

  CHECK(lhs.exact_pattern_to_normalized_binary_pattern ==
        rhs.exact_pattern_to_normalized_binary_pattern);
  CHECK(lhs.exact_pattern_to_normalized_binary_state_map.size() ==
        rhs.exact_pattern_to_normalized_binary_state_map.size());
  for (std::size_t index = 0;
       index < lhs.exact_pattern_to_normalized_binary_state_map.size();
       ++index) {
    check_normalized_binary_state_map_equal(
        lhs.exact_pattern_to_normalized_binary_state_map[index],
        rhs.exact_pattern_to_normalized_binary_state_map[index]);
  }

  CHECK(lhs.taxon_count == rhs.taxon_count);
  CHECK(lhs.total_site_count == rhs.total_site_count);
  CHECK(lhs.invariant_site_count == rhs.invariant_site_count);
  CHECK(lhs.variable_site_count == rhs.variable_site_count);
  CHECK(lhs.binary_variable_site_count == rhs.binary_variable_site_count);
  CHECK(lhs.nonbinary_variable_site_count ==
        rhs.nonbinary_variable_site_count);
  CHECK(lhs.skipped_invariant_site_count ==
        rhs.skipped_invariant_site_count);
  CHECK(lhs.invariant_constant_score_excluding_ua ==
        rhs.invariant_constant_score_excluding_ua);
  CHECK(lhs.invariant_constant_score_with_reference_edge ==
        rhs.invariant_constant_score_with_reference_edge);
  CHECK(lhs.skipped_invariant_constant_score_with_reference_edge ==
        rhs.skipped_invariant_constant_score_with_reference_edge);
}

static larch::phylo_dag make_duplicate_identical_leaf_pattern_dag() {
  using larch::test::tiny_dag_edge;
  using larch::test::tiny_dag_node;
  return larch::test::make_tiny_labelled_dag(
      "AAA", "root",
      std::vector<tiny_dag_node>{
          {"root", "AAA", ""},
          {"a1", "CAC", "A"},
          {"a2", "CAC", "A"},
          {"b", "ACC", "B"},
      },
      std::vector<tiny_dag_edge>{
          {"root", "a1", 0},
          {"root", "a2", 0},
          {"root", "b", 1},
      });
}

static void test_optional_source_metadata_is_exact_and_transactional() {
  std::println("test_optional_source_metadata_is_exact_and_transactional");

  auto dag = make_duplicate_identical_leaf_pattern_dag();
  auto built = larch::build_clade_grammar_with_audit(dag);
  CHECK(built.audit.duplicate_sample_id_occurrences == 1);
  auto const& grammar = built.grammar;

  larch::site_pattern_options options;
  options.build_normalized_binary_patterns = true;
  auto expected = larch::build_site_patterns(dag, grammar, options);
  auto const a_taxon = grammar.taxa.sample_id_to_id.at("A");
  auto const b_taxon = grammar.taxa.sample_id_to_id.at("B");

  larch::site_pattern_source_metadata metadata;
  metadata.compact_genome_leaf_hashes_by_taxon = {{b_taxon, 7}};
  auto actual =
      larch::site_patterns_detail::
          build_site_patterns_with_optional_source_metadata(
              dag, grammar, options, &metadata);
  check_site_pattern_sets_equal(actual, expected);

  auto hashes = metadata.compact_genome_leaf_hashes_by_taxon;
  std::sort(hashes.begin(), hashes.end());
  CHECK(hashes.size() == 3);
  CHECK(hashes[0].first == a_taxon);
  CHECK(hashes[1].first == a_taxon);
  CHECK(hashes[2].first == b_taxon);
  CHECK(hashes[0].second == hashes[1].second);

  // Fail after the complete reachable-leaf scan. Keeping both registry maps
  // internally consistent avoids an earlier validation error and proves that
  // the caller-owned metadata is published only after the whole build commits.
  auto missing_taxon_grammar = grammar;
  auto const missing_id = static_cast<larch::taxon_id>(
      missing_taxon_grammar.taxa.id_to_sample_id.size());
  missing_taxon_grammar.taxa.id_to_sample_id.push_back("missing");
  missing_taxon_grammar.taxa.sample_id_to_id.emplace("missing", missing_id);

  larch::site_pattern_source_metadata unchanged;
  unchanged.compact_genome_leaf_hashes_by_taxon = {{a_taxon, 19}};
  auto const sentinel = unchanged.compact_genome_leaf_hashes_by_taxon;
  std::string failure;
  try {
    (void)larch::site_patterns_detail::
        build_site_patterns_with_optional_source_metadata(
            dag, missing_taxon_grammar, options, &unchanged);
  } catch (std::runtime_error const& error) {
    failure = error.what();
  }
  CHECK(failure.find("missing reachable leaf for taxon 'missing'") !=
        std::string::npos);
  CHECK(unchanged.compact_genome_leaf_hashes_by_taxon == sentinel);

  std::println("  PASS");
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
  test_optional_source_metadata_is_exact_and_transactional();
  test_identical_sites_collapse_and_weight();
  test_invariant_sites_can_be_skipped_with_constants();
  test_binary_normalization_groups_complements();
  test_weighted_chart_score_matches_uncompressed_sites();
  test_strict_validation_errors();

  std::println("All site pattern tests passed!");
  return 0;
}

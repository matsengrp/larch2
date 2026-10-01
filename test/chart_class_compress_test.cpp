#include <larch/chart_trim.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <ranges>
#include <cstdint>
#include <numeric>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
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

namespace {

using larch::test::tiny_inner;
using larch::test::tiny_leaf;
using larch::test::tiny_tree_node;

// Two merged trees over four taxa with genuinely competing productions and
// enough pattern repetition to exercise equality deduplication.
tiny_tree_node class_compress_tree1_spec() {
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A",
                  {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C",
                  {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

tiny_tree_node class_compress_tree2_spec() {
  return tiny_inner(
      "root", "A",
      {tiny_leaf("A", "A"),
       tiny_inner("BCD", "A",
                  {tiny_inner("BC", "A",
                              {tiny_leaf("B", "A"), tiny_leaf("C", "C")}),
                   tiny_leaf("D", "C")})});
}

struct class_compress_fixture {
  larch::clade_grammar grammar;
  larch::site_pattern_set patterns;

  class_compress_fixture() {
    std::vector<larch::phylo_dag> trees;
    trees.push_back(
        larch::test::make_tiny_labelled_tree("AA", class_compress_tree1_spec()));
    trees.push_back(
        larch::test::make_tiny_labelled_tree("AA", class_compress_tree2_spec()));
    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    grammar = larch::build_clade_grammar(merged);
    patterns = larch::build_site_patterns(merged, grammar);
    // Repeat the site patterns so that restriction classes are strictly
    // coarser than per-pattern columns on interior clades.
    auto const originals = patterns.patterns;
    for (std::size_t repetition = 0; repetition < 3; ++repetition) {
      for (auto const& pattern : originals) {
        patterns.patterns.push_back(pattern);
      }
    }
  }
};

std::uint64_t fixture_optimum(class_compress_fixture const& fixture,
                              larch::multisite_trim_options options = {}) {
  options.dominance_mode = larch::multisite_dominance_mode::score_only;
  options.require_exact_keep_mask = false;
  return larch::build_multisite_trim(fixture.grammar, fixture.patterns, {},
                                     options)
      .optimum;
}

void check_trim_scalars_match(larch::multisite_trim_result const& expected,
                              larch::multisite_trim_result const& actual) {
  CHECK(actual.optimum == expected.optimum);
  CHECK(actual.composite_lower_bound == expected.composite_lower_bound);
  CHECK(actual.initial_upper_bound == expected.initial_upper_bound);
  CHECK(actual.dominance_mode == expected.dominance_mode);
  CHECK(actual.keep_mask_kind == expected.keep_mask_kind);
  CHECK(actual.dominance_candidates_considered ==
        expected.dominance_candidates_considered);
  CHECK(actual.dominance_pruned == expected.dominance_pruned);
  CHECK(actual.dominance_pruned_score_pass ==
        expected.dominance_pruned_score_pass);
  CHECK(actual.dominance_pruned_mask_pass ==
        expected.dominance_pruned_mask_pass);
  CHECK(actual.bound_pruned == expected.bound_pruned);
  CHECK(actual.equality_deduplicated == expected.equality_deduplicated);
  CHECK(actual.active_pattern_count == expected.active_pattern_count);
  CHECK(actual.invariant_constant_offset ==
        expected.invariant_constant_offset);
  CHECK(actual.frontier_sizes_by_clade == expected.frontier_sizes_by_clade);
  CHECK(actual.exact_bnb_levels == expected.exact_bnb_levels);
  CHECK(actual.exact_bnb_clades == expected.exact_bnb_clades);
  CHECK(actual.exact_bnb_product_combinations ==
        expected.exact_bnb_product_combinations);
  CHECK(actual.exact_bnb_frontier_entries ==
        expected.exact_bnb_frontier_entries);
}

void test_score_pass_equivalence() {
  std::println("test_score_pass_equivalence");
  class_compress_fixture fixture;

  larch::multisite_trim_options score_only;
  score_only.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only.require_exact_keep_mask = false;

  auto expected =
      larch::build_multisite_trim(fixture.grammar, fixture.patterns, {},
                                  score_only);

  auto compressed_options = score_only;
  compressed_options.class_compressed_score_pass = true;
  auto actual = larch::build_multisite_trim(fixture.grammar, fixture.patterns,
                                            {}, compressed_options);

  CHECK(!expected.class_compressed_score_pass);
  CHECK(actual.class_compressed_score_pass);
  CHECK(actual.restriction_class_total > 0);
  CHECK(actual.restriction_class_max <= fixture.patterns.patterns.size());
  // Constant-class factorization must fire on this fixture: leaves and
  // cherries are fully factored (single entry / single production), so the
  // factored total is strictly positive and the widest varying set is
  // strictly smaller than the widest class table.
  CHECK(actual.factored_class_total ==
        actual.factored_predictive_total + actual.factored_empirical_total);
  CHECK(actual.factored_class_total > 0);
  CHECK(actual.factored_predictive_total > 0);
  CHECK(actual.varying_class_max < actual.restriction_class_max);
  check_trim_scalars_match(expected, actual);

  std::println("  PASS");
}

void test_off_dominance_equivalence() {
  std::println("test_off_dominance_equivalence");
  class_compress_fixture fixture;

  larch::multisite_trim_options plain;
  plain.require_exact_keep_mask = false;

  auto expected =
      larch::build_multisite_trim(fixture.grammar, fixture.patterns, {}, plain);

  auto compressed_options = plain;
  compressed_options.class_compressed_score_pass = true;
  auto actual = larch::build_multisite_trim(fixture.grammar, fixture.patterns,
                                            {}, compressed_options);
  check_trim_scalars_match(expected, actual);

  std::println("  PASS");
}

void test_two_pass_equivalence() {
  std::println("test_two_pass_equivalence");
  class_compress_fixture fixture;

  larch::multisite_trim_options two_pass;
  two_pass.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;

  auto expected =
      larch::build_multisite_trim(fixture.grammar, fixture.patterns, {},
                                  two_pass);

  auto compressed_options = two_pass;
  compressed_options.class_compressed_score_pass = true;
  auto actual = larch::build_multisite_trim(fixture.grammar, fixture.patterns,
                                            {}, compressed_options);

  CHECK(actual.class_compressed_score_pass);
  check_trim_scalars_match(expected, actual);
  CHECK(actual.keep_production == expected.keep_production);

  std::println("  PASS");
}

void test_upper_bound_override_and_cap_equivalence() {
  std::println("test_upper_bound_override_and_cap_equivalence");
  class_compress_fixture fixture;

  larch::multisite_trim_options score_only;
  score_only.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only.require_exact_keep_mask = false;
  score_only.upper_bound_override = fixture_optimum(fixture);

  auto expected =
      larch::build_multisite_trim(fixture.grammar, fixture.patterns, {},
                                  score_only);

  auto compressed_options = score_only;
  compressed_options.class_compressed_score_pass = true;
  auto actual = larch::build_multisite_trim(fixture.grammar, fixture.patterns,
                                            {}, compressed_options);
  check_trim_scalars_match(expected, actual);

  // The frontier cap fires identically in both representations.  With
  // dominance off some clade frontier must exceed one entry, so the cap of
  // one is guaranteed to fire and to fire at the same clade.
  larch::multisite_trim_options uncapped;
  uncapped.require_exact_keep_mask = false;
  // Dominance off plus a deliberately loose bound keeps both competing root
  // derivations, so a cap of one is guaranteed to fire at the root.
  uncapped.upper_bound_override = fixture_optimum(fixture) + 5;
  auto uncapped_result =
      larch::build_multisite_trim(fixture.grammar, fixture.patterns, {},
                                  uncapped);
  CHECK(*std::ranges::max_element(uncapped_result.frontier_sizes_by_clade) >
        1);

  auto cap_fires = [&](bool compress) {
    auto options = uncapped;
    options.max_frontier_entries_per_clade = 1;
    options.class_compressed_score_pass = compress;
    return throws_runtime_error([&] {
      (void)larch::build_multisite_trim(fixture.grammar, fixture.patterns,
                                        {}, options);
    });
  };
  CHECK(cap_fires(false));
  CHECK(cap_fires(true));

  std::println("  PASS");
}

void test_exact_mask_request_rejected() {
  std::println("test_exact_mask_request_rejected");
  class_compress_fixture fixture;

  // An exact single-pass keep mask needs production bitsets, which
  // class-compressed frontiers do not carry.
  larch::multisite_trim_options exact;
  exact.class_compressed_score_pass = true;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_multisite_trim(fixture.grammar, fixture.patterns, {},
                                      exact);
  }));

  std::println("  PASS");
}

tiny_tree_node random_tree_spec(
    std::vector<std::pair<std::string, std::string>> leaves,
    std::string const& reference, std::mt19937& rng, int& inner_id) {
  if (leaves.size() == 1) {
    return tiny_leaf(leaves.front().first, leaves.front().second);
  }

  std::shuffle(leaves.begin(), leaves.end(), rng);
  std::uniform_int_distribution<int> split_dist(
      1, static_cast<int>(leaves.size()) - 1);
  auto split = static_cast<std::size_t>(split_dist(rng));
  std::vector<std::pair<std::string, std::string>> left(leaves.begin(),
                                                        leaves.begin() + split);
  std::vector<std::pair<std::string, std::string>> right(leaves.begin() + split,
                                                         leaves.end());

  return tiny_inner(
      "R" + std::to_string(inner_id++), reference,
      {random_tree_spec(std::move(left), reference, rng, inner_id),
       random_tree_spec(std::move(right), reference, rng, inner_id)});
}

// Randomized cross-check against the per-pattern builder on merged random
// trees with repeated patterns and several dominance modes.
void test_random_grammar_equivalence() {
  std::println("test_random_grammar_equivalence");

  std::mt19937 rng(0x5eed4a11);
  for (std::size_t iteration = 0; iteration < 40; ++iteration) {
    std::vector<std::pair<std::string, std::string>> leaves;
    std::size_t const taxon_count = 3 + (iteration % 6);
    for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
      auto state = static_cast<char>('A' + (rng() % 4));
      leaves.emplace_back("L" + std::to_string(taxon),
                          std::string(1, state));
    }
    int inner_id = 0;
    std::vector<larch::phylo_dag> trees;
    for (std::size_t tree_index = 0; tree_index < 2; ++tree_index) {
      auto spec = random_tree_spec(leaves, "A", rng, inner_id);
      trees.push_back(larch::test::make_tiny_labelled_tree("AA", spec));
    }
    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(merged);
    auto patterns = larch::build_site_patterns(merged, grammar);
    // Duplicate a few patterns so restriction classes are coarser than
    // per-pattern columns on interior clades.
    auto const original_count = patterns.patterns.size();
    for (std::size_t copy = 0; copy < original_count; ++copy) {
      if (rng() % 2 == 0) {
        patterns.patterns.push_back(patterns.patterns[copy]);
      }
    }

    for (auto dominance :
         {larch::multisite_dominance_mode::off,
          larch::multisite_dominance_mode::score_only,
          larch::multisite_dominance_mode::two_pass_exact_mask}) {
      larch::multisite_trim_options options;
      options.dominance_mode = dominance;
      if (dominance != larch::multisite_dominance_mode::two_pass_exact_mask) {
        options.require_exact_keep_mask = false;
      }
      auto expected = larch::build_multisite_trim(grammar, patterns, {},
                                                  options);
      auto compressed = options;
      compressed.class_compressed_score_pass = true;
      auto actual =
          larch::build_multisite_trim(grammar, patterns, {}, compressed);
      CHECK(actual.class_compressed_score_pass);
      check_trim_scalars_match(expected, actual);
      if (dominance ==
          larch::multisite_dominance_mode::two_pass_exact_mask) {
        CHECK(actual.keep_production == expected.keep_production);
      }
    }
  }

  std::println("  PASS");
}

void test_witness_equivalence() {
  std::println("test_witness_equivalence");
  class_compress_fixture fixture;

  // Exact reference trim: gives the tight, validated bound both witness
  // builds prune with (mirrors the larch2 apply layer).
  larch::multisite_trim_options exact_opts;
  exact_opts.dominance_mode = larch::multisite_dominance_mode::off;
  exact_opts.require_exact_keep_mask = false;
  auto exact = larch::build_multisite_trim(fixture.grammar, fixture.patterns,
                                           {}, exact_opts);

  auto run_witness = [&](larch::multisite_trim_options trim) {
    larch::multisite_topology_trace_options trace;
    trace.max_optimal_topologies = 1;
    trim.dominance_mode = larch::multisite_dominance_mode::score_only;
    trim.require_exact_keep_mask = false;
    trim.use_bound_pruning = true;
    trim.upper_bound_override = exact.optimum;
    trim.known_exact_optimum = exact.optimum;
    trace.trim_options = trim;
    return larch::build_multisite_optimal_topology_witnesses(
        fixture.grammar, fixture.patterns, {}, trace);
  };

  larch::multisite_trim_options legacy_trim;
  auto legacy = run_witness(legacy_trim);
  CHECK(legacy.optimum == exact.optimum);
  CHECK(legacy.topologies.size() == 1);

  larch::multisite_trim_options compressed_trim;
  compressed_trim.class_compressed_score_pass = true;
  auto compressed = run_witness(compressed_trim);
  CHECK(compressed.optimum == legacy.optimum);
  CHECK(compressed.topologies.size() == legacy.topologies.size());
  CHECK(compressed.frontier_sizes_by_clade ==
        legacy.frontier_sizes_by_clade);
  CHECK(compressed.equality_deduplicated == legacy.equality_deduplicated);
  CHECK(compressed.beam_truncated == false);

  // Beam-on: witness semantics tolerate incompleteness, never wrongness.
  // With a beam wider than any fixture frontier this must succeed and
  // validate against the exact optimum (every emitted topology is
  // exactly re-scored inside the trace).
  larch::multisite_trim_options beam_trim;
  beam_trim.class_compressed_score_pass = true;
  beam_trim.beam_after_taxa = 2;
  beam_trim.beam_width = 16;
  auto beamed = run_witness(beam_trim);
  CHECK(beamed.optimum == exact.optimum);
  CHECK(beamed.topologies.size() >= 1);

  std::println("  PASS");
}

void test_random_witness_equivalence() {
  std::println("test_random_witness_equivalence");

  std::mt19937 rng(0xbea12d5e);
  std::size_t beamed_successes = 0;
  for (std::size_t iteration = 0; iteration < 20; ++iteration) {
    std::vector<std::pair<std::string, std::string>> leaves;
    std::size_t const taxon_count = 3 + (iteration % 6);
    for (std::size_t taxon = 0; taxon < taxon_count; ++taxon) {
      auto state = static_cast<char>('A' + (rng() % 4));
      leaves.emplace_back("L" + std::to_string(taxon),
                          std::string(1, state));
    }
    int inner_id = 0;
    std::vector<larch::phylo_dag> trees;
    for (std::size_t tree_index = 0; tree_index < 2; ++tree_index) {
      auto spec = random_tree_spec(leaves, "A", rng, inner_id);
      trees.push_back(larch::test::make_tiny_labelled_tree("AA", spec));
    }
    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(merged);
    auto patterns = larch::build_site_patterns(merged, grammar);
    auto const original_count = patterns.patterns.size();
    for (std::size_t copy = 0; copy < original_count; ++copy) {
      if (rng() % 2 == 0) {
        patterns.patterns.push_back(patterns.patterns[copy]);
      }
    }

    larch::multisite_trim_options exact_opts;
    exact_opts.dominance_mode = larch::multisite_dominance_mode::off;
    exact_opts.require_exact_keep_mask = false;
    auto exact = larch::build_multisite_trim(grammar, patterns, {},
                                             exact_opts);

    auto run_witness = [&](larch::multisite_trim_options trim) {
      larch::multisite_topology_trace_options trace;
      trace.max_optimal_topologies = 1;
      trim.dominance_mode = larch::multisite_dominance_mode::score_only;
      trim.require_exact_keep_mask = false;
      trim.use_bound_pruning = true;
      trim.upper_bound_override = exact.optimum;
      trim.known_exact_optimum = exact.optimum;
      trace.trim_options = trim;
      return larch::build_multisite_optimal_topology_witnesses(
          grammar, patterns, {}, trace);
    };

    auto legacy = run_witness(larch::multisite_trim_options{});
    CHECK(legacy.optimum == exact.optimum);

    larch::multisite_trim_options compressed_trim;
    compressed_trim.class_compressed_score_pass = true;
    auto compressed = run_witness(compressed_trim);
    CHECK(compressed.optimum == legacy.optimum);
    CHECK(compressed.topologies.size() == legacy.topologies.size());
    CHECK(compressed.frontier_sizes_by_clade ==
          legacy.frontier_sizes_by_clade);

    // Narrow beam: either validates against the exact optimum (counted)
    // or fails loudly at optimum validation.  Wrong trees can never be
    // emitted: every topology is exactly re-scored inside the trace.
    larch::multisite_trim_options beam_trim;
    beam_trim.class_compressed_score_pass = true;
    beam_trim.beam_after_taxa = 2;
    beam_trim.beam_width = 2;
    auto beamed_ok = true;
    try {
      auto beamed = run_witness(beam_trim);
      CHECK(beamed.optimum == exact.optimum);
      CHECK(beamed.topologies.size() >= 1);
    } catch (std::runtime_error const&) {
      beamed_ok = false;
    }
    if (beamed_ok) ++beamed_successes;
  }
  CHECK(beamed_successes >= 10);

  std::println("  PASS (beamed successes: {})", beamed_successes);
}

}  // namespace

int main() {
  test_score_pass_equivalence();
  test_off_dominance_equivalence();
  test_two_pass_equivalence();
  test_upper_bound_override_and_cap_equivalence();
  test_exact_mask_request_rejected();
  test_random_grammar_equivalence();
  test_witness_equivalence();
  test_random_witness_equivalence();
  std::println("All chart class-compress tests passed!");
  return 0;
}

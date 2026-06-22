// Phase 0 deliverable: two-chart from-scratch oracle.
//
// This test pins `recompute_both_charts_from_scratch` to the current dense
// `build_single_site_chart` + `build_single_site_outside_chart` paths on the
// small committed fixtures (`wric_binary_four.*` and `wric_two_polytomy.*`),
// so every later phase can treat the helper as a trustworthy oracle.  The
// oracle owns no incremental logic; these tests confirm it is a faithful
// wrapper (option propagation, reference-state handling, trace retention) and
// that the dense paths it wraps are themselves consistent with an independent
// brute-force inside-chart enumeration and with the documented
// outside/global_min identity.

#include <larch/build_fasta_newick.hpp>
#include <larch/chart_two_chart_oracle.hpp>
#include <larch/chart_trim.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/polytomy_refinement.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <print>
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

// ---------------------------------------------------------------------------
// Independent inside-chart oracle: brute-force enumeration of every binary
// derivation in the grammar.  Ported from parsimony_chart_test.cpp so the
// oracle test has real teeth rather than comparing the wrapper to itself.
// ---------------------------------------------------------------------------

using brute_chart_row = std::array<larch::chart_cost, larch::nuc_state_count>;

static larch::chart_cost brute_add(larch::chart_cost lhs,
                                   larch::chart_cost rhs) {
  if (lhs >= larch::chart_inf || rhs >= larch::chart_inf)
    return larch::chart_inf;
  if (lhs > larch::chart_inf - rhs) return larch::chart_inf;
  return lhs + rhs;
}

static brute_chart_row brute_leaf_row(std::uint8_t observed) {
  brute_chart_row row{};
  row.fill(larch::chart_inf);
  row[observed] = 0;
  return row;
}

static brute_chart_row brute_combine_binary(brute_chart_row const& left,
                                            brute_chart_row const& right) {
  brute_chart_row row{};
  row.fill(larch::chart_inf);
  for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
       ++parent_state) {
    larch::chart_cost best_left = larch::chart_inf;
    larch::chart_cost best_right = larch::chart_inf;
    for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
         ++child_state) {
      best_left = std::min(
          best_left,
          brute_add(left[child_state], parent_state == child_state ? 0 : 1));
      best_right = std::min(
          best_right,
          brute_add(right[child_state], parent_state == child_state ? 0 : 1));
    }
    row[parent_state] = brute_add(best_left, best_right);
  }
  return row;
}

static std::vector<brute_chart_row> brute_enumerate_rows(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states,
    larch::clade_id clade,
    std::vector<std::optional<std::vector<brute_chart_row>>>& memo) {
  if (memo[clade].has_value()) return *memo[clade];

  std::vector<brute_chart_row> rows;
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    rows.push_back(brute_leaf_row(states.state_by_taxon[key.taxa.front()]));
  } else {
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      CHECK(prod.children.size() == 2);
      auto left_rows =
          brute_enumerate_rows(grammar, states, prod.children[0], memo);
      auto right_rows =
          brute_enumerate_rows(grammar, states, prod.children[1], memo);
      for (auto const& left : left_rows) {
        for (auto const& right : right_rows) {
          rows.push_back(brute_combine_binary(left, right));
          CHECK(rows.size() < 100000);
        }
      }
    }
  }

  memo[clade] = rows;
  return rows;
}

static larch::chart_cost brute_force_grammar_root_min(
    larch::clade_grammar const& grammar,
    larch::leaf_site_states const& states) {
  std::vector<std::optional<std::vector<brute_chart_row>>> memo(
      grammar.clades.size());
  auto rows = brute_enumerate_rows(grammar, states, grammar.root_clade, memo);
  larch::chart_cost best = larch::chart_inf;
  for (auto const& row : rows)
    for (auto cost : row) best = std::min(best, cost);
  return best;
}

// ---------------------------------------------------------------------------
// Fixture loading.
// ---------------------------------------------------------------------------

struct oracle_fixture {
  std::string name;
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
  std::size_t reference_length = 0;
};

static oracle_fixture load_binary_fixture(std::string const& stem) {
  oracle_fixture f;
  f.name = stem;
  f.dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/" + stem + ".fa"),
      larch::test::source_path_string("test/" + stem + ".nwk"),
      larch::test::source_path_string("test/" + stem + ".ref"));
  f.grammar = larch::build_clade_grammar(f.dag);
  f.reference_length = larch::get_reference_sequence(f.dag).size();
  CHECK(!larch::grammar_has_kary_productions(f.grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(f.grammar));
  return f;
}

static oracle_fixture load_polytomy_fixture(std::string const& stem) {
  oracle_fixture f;
  f.name = stem + " (polytomy-refined)";
  f.dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/" + stem + ".fa"),
      larch::test::source_path_string("test/" + stem + ".nwk"),
      larch::test::source_path_string("test/" + stem + ".ref"));
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      f.dag, larch::clade_grammar_options{}, opts);
  larch::require_polytomy_refinement_binary_charting(refinement.audit,
                                                     "chart_two_chart_oracle");
  CHECK(refinement.audit.exact_for_soft_polytomies);
  f.grammar = std::move(refinement.grammar);
  f.reference_length = larch::get_reference_sequence(f.dag).size();
  CHECK(!larch::grammar_has_kary_productions(f.grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(f.grammar));
  return f;
}

// ---------------------------------------------------------------------------
// Core assertions.
// ---------------------------------------------------------------------------

// A normalized, order-independent view of a chart_choice used for set
// comparison.  An incremental cache (Phase 2/3) may populate the tied-choice
// vectors in a different order than the from-scratch dense path, so the oracle
// compares per-cell *sets* (sorted multisets of these keys), not just per-cell
// counts -- equal counts with different witnesses is exactly the bug this is
// here to catch once the oracle is wired into commit verification.
struct normalized_choice {
  larch::production_id production;
  std::uint8_t child_state_left;
  std::uint8_t child_state_right;
  larch::chart_cost cost;
  bool operator==(normalized_choice const&) const = default;
  bool operator<(normalized_choice const& o) const {
    if (production != o.production) return production < o.production;
    if (child_state_left != o.child_state_left)
      return child_state_left < o.child_state_left;
    if (child_state_right != o.child_state_right)
      return child_state_right < o.child_state_right;
    return cost < o.cost;
  }
};

static std::vector<normalized_choice> normalize_cell(
    std::vector<larch::chart_choice> const& choices) {
  std::vector<normalized_choice> out;
  out.reserve(choices.size());
  for (auto const& ch : choices) {
    out.push_back({ch.production, ch.child_states[0], ch.child_states[1],
                   ch.cost});
  }
  std::sort(out.begin(), out.end());
  return out;
}

static void assert_charts_equal(larch::single_site_chart const& a_inside,
                                larch::single_site_outside_chart const& a_out,
                                larch::single_site_chart const& b_inside,
                                larch::single_site_outside_chart const& b_out,
                                std::string const& context) {
  CHECK(a_inside.inside.size() == b_inside.inside.size());
  for (std::size_t cid = 0; cid < a_inside.inside.size(); ++cid) {
    for (std::uint8_t s = 0; s < larch::nuc_state_count; ++s) {
      if (a_inside.inside[cid][s] != b_inside.inside[cid][s]) {
        std::println(stderr, "  inside mismatch at clade {} state {}: {} != {} "
                             "[{}]",
                     cid, static_cast<unsigned>(s), a_inside.inside[cid][s],
                     b_inside.inside[cid][s], context);
        CHECK(false);
      }
    }
  }
  CHECK(a_inside.has_trace() == b_inside.has_trace());
  if (a_inside.has_trace()) {
    CHECK(a_inside.optimal_choices.size() == b_inside.optimal_choices.size());
    CHECK(a_inside.trace_choice_count == b_inside.trace_choice_count);
    for (std::size_t cid = 0; cid < a_inside.optimal_choices.size(); ++cid) {
      for (std::uint8_t s = 0; s < larch::nuc_state_count; ++s) {
        // Full witness-set comparison, not just per-cell counts: an
        // incremental path could produce equal counts with different actual
        // choices, which the old size-only check would miss.
        auto na = normalize_cell(a_inside.optimal_choices[cid][s]);
        auto nb = normalize_cell(b_inside.optimal_choices[cid][s]);
        if (na != nb) {
          std::println(stderr, "  inside-trace choice-set mismatch at clade {} "
                               "state {}: {} witnesses vs {} [{}]",
                       cid, static_cast<unsigned>(s), na.size(), nb.size(),
                       context);
          for (std::size_t i = 0; i < na.size() && i < nb.size(); ++i) {
            if (!(na[i] == nb[i])) {
              std::println(stderr,
                           "    first diff: prod {} children {}/{} cost {}"
                           "  vs  prod {} children {}/{} cost {}",
                           na[i].production, na[i].child_state_left,
                           na[i].child_state_right, na[i].cost,
                           nb[i].production, nb[i].child_state_left,
                           nb[i].child_state_right, nb[i].cost);
              break;
            }
          }
          CHECK(false);
        }
      }
    }
  }

  CHECK(a_out.outside.size() == b_out.outside.size());
  for (std::size_t cid = 0; cid < a_out.outside.size(); ++cid) {
    for (std::uint8_t s = 0; s < larch::nuc_state_count; ++s) {
      if (a_out.outside[cid][s] != b_out.outside[cid][s]) {
        std::println(stderr, "  outside mismatch at clade {} state {}: {} != {} "
                             "[{}]",
                     cid, static_cast<unsigned>(s), a_out.outside[cid][s],
                     b_out.outside[cid][s], context);
        CHECK(false);
      }
    }
  }
  CHECK(a_out.global_min == b_out.global_min);
}

static void exercise_fixture(oracle_fixture& f) {
  std::println("exercise_fixture: {}", f.name);

  for (std::size_t pos = 1; pos <= f.reference_length; ++pos) {
    auto leaf_states =
        larch::extract_leaf_site_states(f.dag, f.grammar, pos);
    auto reference_state =
        larch::extract_reference_site_state(f.dag, static_cast<larch::mutation_position>(pos));

    for (bool keep_trace : {false, true}) {
      // UA-free convention.
      {
        larch::chart_options options;
        options.keep_trace = keep_trace;

        auto oracle = larch::recompute_both_charts_from_scratch(
            f.grammar, leaf_states, options);

        // Headline check: the oracle reproduces the dense paths called
        // directly (not through the wrapper).
        auto direct_inside =
            larch::build_single_site_chart(f.grammar, leaf_states, options);
        auto direct_outside = larch::build_single_site_outside_chart(
            f.grammar, direct_inside, options);
        assert_charts_equal(oracle.first, oracle.second, direct_inside,
                            direct_outside,
                            "ua_free pos=" + std::to_string(pos) +
                                " keep_trace=" + (keep_trace ? "1" : "0"));

        // Outside/global_min identity for the UA-free convention:
        //   outside[root][s] == 0 for all s
        //   => global_min == min_s inside[root][s] == root_min_excluding_ua
        CHECK(oracle.second.global_min ==
              oracle.first.root_min_excluding_ua(f.grammar.root_clade));

        // Independent brute-force cross-check of the inside root minimum.
        CHECK(oracle.first.root_min_excluding_ua(f.grammar.root_clade) ==
              brute_force_grammar_root_min(f.grammar, leaf_states));
      }

      // score_ua_edge=true convention with the site's reference state.
      {
        larch::chart_options options;
        options.keep_trace = keep_trace;
        options.score_ua_edge = true;

        auto oracle = larch::recompute_both_charts_from_scratch(
            f.grammar, leaf_states, options, reference_state);

        auto direct_inside =
            larch::build_single_site_chart(f.grammar, leaf_states, options);
        auto direct_outside = larch::build_single_site_outside_chart(
            f.grammar, direct_inside, options, reference_state);
        assert_charts_equal(oracle.first, oracle.second, direct_inside,
                            direct_outside,
                            "ua_edge pos=" + std::to_string(pos) +
                                " keep_trace=" + (keep_trace ? "1" : "0"));

        // Outside/global_min identity for the UA-edge convention:
        //   outside[root][s] == c(reference_state, s)
        //   => global_min == root_min_with_reference_edge
        CHECK(oracle.second.global_min ==
              oracle.first.root_min_with_reference_edge(f.grammar.root_clade,
                                                        reference_state));
      }
    }

    // DAG-and-pos convenience overload must agree with the explicit form for
    // both conventions.
    {
      larch::chart_options ua_free;
      auto from_dag = larch::recompute_both_charts_from_scratch(
          f.grammar, f.dag, static_cast<larch::mutation_position>(pos), ua_free);
      auto explicit_ = larch::recompute_both_charts_from_scratch(
          f.grammar, leaf_states, ua_free);
      assert_charts_equal(from_dag.first, from_dag.second, explicit_.first,
                          explicit_.second,
                          "dag-overload ua_free pos=" + std::to_string(pos));

      larch::chart_options ua_edge;
      ua_edge.score_ua_edge = true;
      auto from_dag_ua = larch::recompute_both_charts_from_scratch(
          f.grammar, f.dag, static_cast<larch::mutation_position>(pos), ua_edge);
      auto explicit_ua = larch::recompute_both_charts_from_scratch(
          f.grammar, leaf_states, ua_edge, reference_state);
      assert_charts_equal(from_dag_ua.first, from_dag_ua.second,
                          explicit_ua.first, explicit_ua.second,
                          "dag-overload ua_edge pos=" + std::to_string(pos));
    }
  }

  std::println("  PASS ({} sites)", f.reference_length);
}

static void test_score_ua_edge_requires_reference_state() {
  std::println("test_score_ua_edge_requires_reference_state");

  auto f = load_binary_fixture("wric_binary_four");
  auto leaf_states =
      larch::extract_leaf_site_states(f.dag, f.grammar, 1);

  larch::chart_options ua_edge;
  ua_edge.score_ua_edge = true;

  // The bare oracle overload must refuse score_ua_edge=true without a
  // reference state, exactly as build_single_site_outside_chart does.  The
  // oracle is never a silent UA-edge-free fallback.
  CHECK(throws_runtime_error([&] {
    (void)larch::recompute_both_charts_from_scratch(f.grammar, leaf_states,
                                                    ua_edge);
  }));

  // The reference-state overload succeeds and stays finite.
  auto ok = larch::recompute_both_charts_from_scratch(
      f.grammar, leaf_states, ua_edge,
      larch::extract_reference_site_state(f.dag, 1));
  CHECK(ok.second.global_min < larch::chart_inf);

  std::println("  PASS");
}

int main() {
  auto binary = load_binary_fixture("wric_binary_four");
  exercise_fixture(binary);
  auto polytomy = load_polytomy_fixture("wric_two_polytomy");
  exercise_fixture(polytomy);
  test_score_ua_edge_requires_reference_state();

  std::println("All chart two-chart oracle tests passed!");
  return 0;
}

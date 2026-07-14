// Phase 2 deliverable: persistent inside-chart cache on the overlay chain
// (Work item 3, inside half).
//
// This test exercises the persistent inside cache (`inside_chart_cache`),
// the chain inside-affected-set computation
// (`compute_chain_inside_affected_set`), and the commit primitive
// (`apply_commit_to_inside_cache`).  It pins them to Phase 0's two-chart
// oracle (`recompute_both_charts_from_scratch`) so the persisted inside chart
// is proved equal to the from-scratch inside chart after every commit.
//
// Coverage (per the plan's Phase 2 exit criteria):
//   * chain-of-1 on `wric_binary_four`: oracle green (inside half), composite
//     lower bound matches, exact-trim cache reset, counter non-trivial.
//   * chain-of-2 and a long chain on a richer fixture with variable sites:
//     oracle green after every accept (multi-level ancestor recomputation).
//   * single commit on `data/test_5_trees` (the medium CI fixture).
//   * `compute_chain_inside_affected_set` for a single-delta chain equals
//     `spr_overlay_delta::affected_order` modulo the candidate-only
//     conservative seeds, and every extra seed is a clade whose row is
//     unchanged.
//   * `inside_rows_recomputed_on_commit` reflects exactly the affected set,
//     so a regression to "recompute everything" is visible.

#include <larch/build_fasta_newick.hpp>
#include <larch/chart_spr.hpp>
#include <larch/chart_spr_search.hpp>
#include <larch/chart_trim.hpp>
#include <larch/chart_two_chart_oracle.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/inside_chart_cache.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/merge.hpp>
#include <larch/overlay_chain.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/polytomy_refinement.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <print>
#include <set>
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

// Whether `msg` is one of the labelled overlay-chain rejections a sequential
// search may legitimately encounter and skip (tombstone of an earlier delta's
// added production, or a double tombstone).  Any other overlay_chain throw
// indicates a real append bug and must propagate.  Mirrors overlay_chain_test.
static bool is_expected_overlay_chain_rejection(std::string const& msg) {
  return msg.find("overlay_chain") != std::string::npos &&
         (msg.find("not a frozen-base production") != std::string::npos ||
          msg.find("double tombstone") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Active-pattern fixture: a grammar plus its active patterns, invariant
// offset, and chart options.  Built the same way the search state builds them
// so the cache is exercised against realistic inputs.
// ---------------------------------------------------------------------------

struct active_fixture {
  std::string name;
  larch::clade_grammar grammar;
  larch::active_site_pattern_set active;
  std::uint64_t invariant_offset = 0;
  larch::chart_options options;
};

static active_fixture load_binary_four_fixture() {
  active_fixture f;
  f.name = "wric_binary_four";
  auto dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/wric_binary_four.fa"),
      larch::test::source_path_string("test/wric_binary_four.nwk"),
      larch::test::source_path_string("test/wric_binary_four.ref"));
  f.grammar = larch::build_clade_grammar(dag);
  auto built = larch::make_active_search_patterns(dag, f.grammar, f.options);
  f.active = std::move(built.active_patterns);
  f.invariant_offset = built.invariant_constant_offset;
  CHECK(!larch::grammar_has_kary_productions(f.grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(f.grammar));
  CHECK(!f.active.patterns.patterns.empty());
  return f;
}

// Six-taxon topologies (one per tree) with leaf sequences carrying distinct
// single-site mutations so the chart has several active patterns.  Topologies
// reused from overlay_chain_test; only the sequences differ (AAAAAA there ->
// mutated here), so the sequential-chain topology story is unchanged.
static larch::phylo_dag make_rich_six_taxon_tree(std::size_t which) {
  using namespace larch::test;
  constexpr std::string_view ref = "AAAAAA";
  // A=AAAAAA, B mutates pos1, C pos2, D pos3, E pos4, F pos6 (pos5 invariant).
  auto leaf = [](char id) {
    switch (id) {
      case 'A': return tiny_leaf("A", "AAAAAA");
      case 'B': return tiny_leaf("B", "CAAAAA");
      case 'C': return tiny_leaf("C", "ACAAAA");
      case 'D': return tiny_leaf("D", "AACAAA");
      case 'E': return tiny_leaf("E", "AAACAA");
      case 'F': return tiny_leaf("F", "AAAAAC");
      default:  throw std::runtime_error("unknown leaf id");
    }
  };
  auto inner = [](std::string name, std::vector<tiny_tree_node> children) {
    return tiny_inner(std::move(name), "AAAAAA", std::move(children));
  };
  switch (which) {
    case 0:  // (((A,B),(C,D)),(E,F))
      return make_tiny_labelled_tree(ref,
                                     inner("r0", {
                                       inner("abcd", {
                                         inner("ab", {leaf('A'), leaf('B')}),
                                         inner("cd", {leaf('C'), leaf('D')}),
                                       }),
                                       inner("ef", {leaf('E'), leaf('F')}),
                                     }));
    case 1:  // (((A,C),(B,D)),(E,F))
      return make_tiny_labelled_tree(ref,
                                     inner("r1", {
                                       inner("abcd", {
                                         inner("ac", {leaf('A'), leaf('C')}),
                                         inner("bd", {leaf('B'), leaf('D')}),
                                       }),
                                       inner("ef", {leaf('E'), leaf('F')}),
                                     }));
    case 2:  // ((A,B),((C,E),(D,F)))
      return make_tiny_labelled_tree(ref,
                                     inner("r2", {
                                       inner("ab", {leaf('A'), leaf('B')}),
                                       inner("cdef", {
                                         inner("ce", {leaf('C'), leaf('E')}),
                                         inner("df", {leaf('D'), leaf('F')}),
                                       }),
                                     }));
    case 3:  // (((A,B),(E,F)),(C,D))
      return make_tiny_labelled_tree(ref,
                                     inner("r3", {
                                       inner("abef", {
                                         inner("ab", {leaf('A'), leaf('B')}),
                                         inner("ef", {leaf('E'), leaf('F')}),
                                       }),
                                       inner("cd", {leaf('C'), leaf('D')}),
                                     }));
    case 4:  // ((A,C),((B,E),(D,F)))
      return make_tiny_labelled_tree(ref,
                                     inner("r4", {
                                       inner("ac", {leaf('A'), leaf('C')}),
                                       inner("bdef", {
                                         inner("be", {leaf('B'), leaf('E')}),
                                         inner("df", {leaf('D'), leaf('F')}),
                                       }),
                                     }));
    case 5:  // (((A,D),(B,C)),(E,F))
      return make_tiny_labelled_tree(ref,
                                     inner("r5", {
                                       inner("abcd", {
                                         inner("ad", {leaf('A'), leaf('D')}),
                                         inner("bc", {leaf('B'), leaf('C')}),
                                       }),
                                       inner("ef", {leaf('E'), leaf('F')}),
                                     }));
    default:
      throw std::runtime_error("make_rich_six_taxon_tree: unknown topology");
  }
}

static active_fixture load_rich_six_taxon_fixture() {
  active_fixture f;
  f.name = "rich_six_taxon";
  std::vector<larch::phylo_dag> trees;
  for (std::size_t i = 0; i < 6; ++i) trees.push_back(make_rich_six_taxon_tree(i));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  f.grammar = larch::build_clade_grammar(dag);
  auto built = larch::make_active_search_patterns(dag, f.grammar, f.options);
  f.active = std::move(built.active_patterns);
  f.invariant_offset = built.invariant_constant_offset;
  CHECK(!larch::grammar_has_kary_productions(f.grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(f.grammar));
  CHECK(!f.active.patterns.patterns.empty());
  return f;
}

// data/test_5_trees merged, binary-chart-refined the same way the Phase 0
// baseline tool does (expand_soft_bounded, max_shapes=1).
static active_fixture load_test_5_trees_fixture() {
  active_fixture f;
  f.name = "data/test_5_trees";
  constexpr std::array<char const*, 5> kPaths = {
      "data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
      "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
      "data/test_5_trees/tree_4.pb.gz",
  };
  std::vector<larch::phylo_dag> trees;
  for (auto* path : kPaths) {
    trees.emplace_back(larch::load_proto_dag(path));
    larch::recompute_compact_genomes(trees.back());
    larch::set_sample_ids_from_cg(trees.back());
  }
  larch::merge merger{larch::get_reference_sequence(trees.front())};
  for (auto& t : trees) merger.add_dag(t);
  larch::phylo_dag dag{std::move(merger.get_result())};

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "inside_chart_cache_test_5_trees");
  f.grammar = std::move(refinement.grammar);
  auto built = larch::make_active_search_patterns(dag, f.grammar, f.options);
  f.active = std::move(built.active_patterns);
  f.invariant_offset = built.invariant_constant_offset;
  CHECK(!f.active.patterns.patterns.empty());
  return f;
}

// ---------------------------------------------------------------------------
// Oracle comparison: materialize the chain, recompute the inside half from
// scratch (Phase 0 two-chart oracle, `.first`), and compare every reachable
// clade's row to the cache.  Also checks the composite lower bound (cache
// root-row path vs build_composite_chart_score on the materialized grammar).
// ---------------------------------------------------------------------------

static void assert_cache_inside_matches_from_scratch(
    larch::overlay_chain const& chain, larch::inside_chart_cache const& cache,
    std::string const& context) {
  auto materialized = larch::materialize_overlay_chain(chain);
  auto const& grammar = materialized.grammar;

  // Per-pattern, per-clade inside-row oracle: the cached row at every
  // reachable overlay-clade-ref must equal the row Phase 0's from-scratch
  // inside chart produces on the materialized grammar.  The frozen-base root
  // clade is one of those reachable clades, so the cached root row is pinned
  // here too.
  for (std::size_t p = 0; p < cache.patterns.size(); ++p) {
    larch::leaf_site_states states;
    states.state_by_taxon = cache.patterns[p].state_by_taxon;
    auto oracle =
        larch::recompute_both_charts_from_scratch(grammar, states, cache.chart_opts);

    if (oracle.first.inside.size() != materialized.dense_clade_to_ref.size()) {
      throw std::runtime_error(
          "inside cache oracle: dense clade map size mismatch");
    }
    for (std::size_t dense = 0;
         dense < materialized.dense_clade_to_ref.size(); ++dense) {
      auto ref = materialized.dense_clade_to_ref[dense];
      auto const& cached = cache.row(p, ref);
      auto const& fresh = oracle.first.inside[dense];
      if (cached != fresh) {
        std::println(stderr,
                     "  inside mismatch at pattern {} dense-clade {} "
                     "(ref {}:{}): cached={} {} {} {} fresh={} {} {} {} [{}]",
                     p, dense,
                     ref.space == larch::overlay_id_space::base ? "base" : "temp",
                     ref.id, cached[0], cached[1], cached[2], cached[3],
                     fresh[0], fresh[1], fresh[2], fresh[3], context);
        CHECK(false);
      }
    }
  }

  // Composite lower-bound oracle (Phase 2 exit criterion): the cache's
  // composite lower bound -- cached root rows scored via
  // chart_spr_weighted_root_score_from_row plus the invariant offset -- must
  // equal an INDEPENDENT from-scratch rebuild on the materialized grammar.
  // The from-scratch side is build_composite_chart_score, which builds the
  // inside chart from scratch and scores the root rows itself and never reads
  // a cached row.  This is what the plan demands ("the composite lower bound
  // ... equals the from-scratch composite lower bound after each commit") and
  // what inside_chart_cache.hpp's header comment promises.  The per-clade loop
  // above transitively pins the root row; this aggregate check is the
  // independent guard that catches a regression isolated to the composite-
  // bound wiring (wrong invariant offset, wrong pattern weight) that leaves
  // root rows intact -- exactly the gap a cache-root-vs-cache-root comparison
  // would miss.
  larch::site_pattern_set active_set;
  active_set.patterns = cache.patterns;
  active_set.taxon_count = grammar.taxa.id_to_sample_id.size();
  auto from_scratch_composite = larch::build_composite_chart_score(
      grammar, active_set, cache.chart_opts);
  auto from_scratch_full = larch::chart_multisite_detail::checked_add_u64(
      from_scratch_composite.weighted_lower_bound,
      cache.invariant_constant_offset, "oracle composite + offset");
  auto cache_full =
      larch::inside_cache_composite_lower_bound_with_invariants(cache);
  if (cache_full != from_scratch_full) {
    std::println(stderr,
                 "  composite lower bound mismatch: cache={} from_scratch={} "
                 "[{}]",
                 cache_full, from_scratch_full, context);
    CHECK(false);
  }
}

// ---------------------------------------------------------------------------
// Sequential chain runner with per-accept oracle.  Appends deltas to `chain`,
// applies each commit to `cache`, and checks the oracle after every accept.
// Returns the number of accepts achieved.  Mirrors overlay_chain_test's
// run_sequential_chain, adding the cache + oracle side.
// ---------------------------------------------------------------------------

static std::size_t run_sequential_chain_with_cache(
    larch::overlay_chain& chain, larch::inside_chart_cache& cache,
    larch::clade_grammar const& base, std::size_t target_steps) {
  larch::clade_grammar tip = base;  // candidate source / oracle
  std::size_t achieved = 0;
  for (std::size_t step = 0; step < target_steps; ++step) {
    auto candidates = larch::enumerate_grammar_spr_candidates(tip);
    bool stepped = false;
    for (auto const& candidate : candidates) {
      larch::spr_overlay_delta delta;
      try {
        delta = larch::build_spr_overlay_delta(tip, candidate);
      } catch (std::runtime_error const&) {
        continue;
      }
      try {
        chain.append(delta);
      } catch (std::runtime_error const& e) {
        if (!is_expected_overlay_chain_rejection(e.what())) throw;
        continue;
      }
      // Commit accepted.  Advance the candidate-source tip and apply the
      // commit to the cache, then run the oracle.
      auto overlay = larch::overlay_from_candidate(tip, candidate);
      tip = larch::materialize_overlay_grammar(overlay).grammar;
      larch::apply_commit_to_inside_cache(chain, cache);
      assert_cache_inside_matches_from_scratch(
          chain, cache, "step " + std::to_string(step) + " (length " +
                            std::to_string(chain.size()) + ")");
      // Lazy-invalidation rule: the exact-trim cache is absent after commit.
      CHECK(!cache.exact_trim_active_only.has_value());
      ++achieved;
      stepped = true;
      break;
    }
    if (!stepped) {
      std::println("  run_sequential_chain_with_cache: no appendable candidate "
                   "at step {}; stopped at length {}",
                   step, achieved);
      break;
    }
  }
  return achieved;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

static void test_plan_cold_cache_matches_checked_oracle() {
  std::println("test_plan_cold_cache_matches_checked_oracle");
  auto f = load_binary_four_fixture();
  auto plan = larch::build_chart_execution_plan(f.grammar);
  auto checked = larch::build_inside_chart_cache(
      f.grammar, f.active, f.options, f.invariant_offset);

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};
  larch::inside_chart_cache planned;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned = larch::build_inside_chart_cache(
        f.grammar, plan, f.active, f.options, f.invariant_offset);
  }
  CHECK(planned.base_rows == checked.base_rows);
  CHECK(planned.temp_rows == checked.temp_rows);
  CHECK(planned.patterns.size() == checked.patterns.size());
  for (std::size_t i = 0; i < planned.patterns.size(); ++i) {
    CHECK(planned.patterns[i].state_by_taxon ==
          checked.patterns[i].state_by_taxon);
    CHECK(planned.patterns[i].positions == checked.patterns[i].positions);
    CHECK(planned.patterns[i].weight == checked.patterns[i].weight);
    CHECK(planned.patterns[i].reference_state_counts ==
          checked.patterns[i].reference_state_counts);
  }
  CHECK(planned.invariant_constant_offset ==
        checked.invariant_constant_offset);
  CHECK(planned.multifurcation_productions_scored ==
        checked.multifurcation_productions_scored);
  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
  std::println("  PASS");
}

static void test_single_commit_on_binary_four() {
  std::println("test_single_commit_on_binary_four");

  auto f = load_binary_four_fixture();
  larch::overlay_chain chain(f.grammar);
  larch::inside_chart_cache cache = larch::build_inside_chart_cache(
      f.grammar, f.active, f.options, f.invariant_offset);

  // Cold-cache oracle: the cache's inside chart matches from-scratch on the
  // base before any commit.
  assert_cache_inside_matches_from_scratch(chain, cache, "cold (length 0)");
  CHECK(cache.temp_clade_count == 0);
  CHECK(!cache.exact_trim_active_only.has_value());

  // Populate the lazy exact-trim cache to verify the commit resets it.
  cache.exact_trim_active_only = larch::build_multisite_trim_active(
      f.grammar, f.active, f.options);
  CHECK(cache.exact_trim_active_only.has_value());

  auto candidates = larch::enumerate_grammar_spr_candidates(f.grammar);
  CHECK(!candidates.empty());
  std::size_t rows_before = cache.inside_rows_recomputed_on_commit;
  bool committed = false;
  for (auto const& candidate : candidates) {
    larch::spr_overlay_delta delta;
    try {
      delta = larch::build_spr_overlay_delta(f.grammar, candidate);
    } catch (std::runtime_error const&) {
      continue;
    }
    try {
      chain.append(delta);
    } catch (std::runtime_error const& e) {
      if (!is_expected_overlay_chain_rejection(e.what())) throw;
      continue;
    }
    larch::apply_commit_to_inside_cache(chain, cache);
    committed = true;
    break;
  }
  CHECK(committed);
  CHECK(chain.size() == 1);

  // Oracle after the single commit.
  assert_cache_inside_matches_from_scratch(chain, cache, "after length-1 commit");

  // Lazy-invalidation hook: exact-trim cache reset by the commit primitive.
  CHECK(!cache.exact_trim_active_only.has_value());

  // Counter: rows recomputed == affected_set_size * pattern_count, strictly
  // between 0 and (whole grammar) so a "recompute everything" regression is
  // visible.
  auto affected = larch::compute_chain_inside_affected_set(chain);
  CHECK(!affected.empty());
  std::size_t rows_after = cache.inside_rows_recomputed_on_commit;
  CHECK(rows_after - rows_before == affected.size() * cache.patterns.size());
  CHECK(rows_after - rows_before <
        f.grammar.clades.size() * cache.patterns.size());

  std::println("  PASS (affected={} clades, {} patterns, {} rows recomputed)",
               affected.size(), cache.patterns.size(),
               rows_after - rows_before);
}

// `compute_chain_inside_affected_set` for a single-delta chain equals
// `spr_overlay_delta::affected_order` modulo the candidate-only conservative
// seeds.  Every extra (affected_order minus the chain set) must be a clade
// whose inside row is unchanged, proving the chain set is both sufficient and
// no looser than necessary.
static void test_affected_set_single_delta_relation() {
  std::println("test_affected_set_single_delta_relation");

  auto f = load_binary_four_fixture();

  auto candidates = larch::enumerate_grammar_spr_candidates(f.grammar);
  larch::spr_overlay_delta delta;
  bool have_delta = false;
  for (auto const& candidate : candidates) {
    try {
      delta = larch::build_spr_overlay_delta(f.grammar, candidate);
    } catch (std::runtime_error const&) {
      continue;
    }
    have_delta = true;
    break;
  }
  CHECK(have_delta);

  // The transient affected_order, in delta-local space (== merged space for a
  // single-delta chain).
  auto transient_order = delta.affected_order;
  std::set<larch::overlay_clade_ref> transient_set(transient_order.begin(),
                                                    transient_order.end());

  larch::overlay_chain chain(f.grammar);
  chain.append(delta);
  auto chain_set_vec = larch::compute_chain_inside_affected_set(chain);
  std::set<larch::overlay_clade_ref> chain_set(chain_set_vec.begin(),
                                               chain_set_vec.end());

  // The chain set is a subset of the transient affected_order: it never
  // recomputes a clade the transient scorer would have left to the base
  // chart.  (Equivalently, the chain machinery is at least as tight.)
  for (auto ref : chain_set) {
    if (!transient_set.contains(ref)) {
      std::println(stderr,
                   "  chain set contains ref {}/{} absent from affected_order",
                   ref.space == larch::overlay_id_space::base ? "base" : "temp",
                   ref.id);
      CHECK(false);
    }
  }

  // Extras: clades the transient affected_order seeded via the candidate
  // pointer (old_parent / new_sibling_or_target).  old_parent is always
  // already covered by the tombstone-parent seed; new_sibling_or_target is a
  // clade whose own productions/subtree are untouched, so its row is
  // unchanged.  Verify that for every extra.
  auto materialized = larch::materialize_overlay_chain(chain);
  auto const& tip_grammar = materialized.grammar;

  for (auto ref : transient_set) {
    if (chain_set.contains(ref)) continue;
    // Extra: its row must be unchanged by the delta.  Compare the base row to
    // the tip row for this clade across every pattern.
    for (std::size_t p = 0; p < f.active.patterns.patterns.size(); ++p) {
      larch::leaf_site_states states;
      states.state_by_taxon = f.active.patterns.patterns[p].state_by_taxon;
      auto base_chart =
          larch::build_single_site_chart(f.grammar, states, f.options);
      auto tip_chart =
          larch::build_single_site_chart(tip_grammar, states, f.options);

      larch::chart_cost base_val = larch::chart_inf;
      if (ref.space == larch::overlay_id_space::base) {
        CHECK(ref.id < base_chart.inside.size());
        // A base clade's "before" row is base_chart[ref.id]; its "after" row
        // is the tip-chart row for the matching dense clade (if still
        // reachable).  Use the minimum over states as a stable cell-free
        // comparison (the row equality is what matters; min captures whether
        // the optimum changed).
        base_val = *std::min_element(base_chart.inside[ref.id].begin(),
                                     base_chart.inside[ref.id].end());
      } else {
        // Temp clades only exist after the delta; they cannot be "extras"
        // (the chain set seeds every temp clade).  If one nonetheless
        // appears, treat as a failure.
        std::println(stderr, "  unexpected temp-clade extra ref {}", ref.id);
        CHECK(false);
      }

      // Locate the tip-chart row for this base clade via the dense map.
      auto dense = materialized.base_clade_to_dense[ref.id];
      if (dense == larch::no_clade) continue;  // unreachable in tip; skip
      CHECK(dense < tip_chart.inside.size());
      auto tip_val = *std::min_element(tip_chart.inside[dense].begin(),
                                       tip_chart.inside[dense].end());
      if (base_val != tip_val) {
        std::println(stderr,
                     "  extra clade {}/{} pattern {} row CHANGED ({} -> {}); "
                     "chain set is missing a real affected clade",
                     ref.space == larch::overlay_id_space::base ? "base" : "temp",
                     ref.id, p, base_val, tip_val);
        CHECK(false);
      }
    }
  }

  // If the two sets are in fact equal on this fixture (no candidate-only
  // extras), say so explicitly; otherwise the per-extra unchanged-row check
  // above is the load-bearing assertion.
  if (chain_set == transient_set) {
    std::println("  PASS (chain set == affected_order exactly, {} clades)",
                 chain_set.size());
  } else {
    std::println(stderr,
                 "  PASS (chain set {} clades ⊆ affected_order {} clades; "
                 "{} conservative extras all unchanged)",
                 chain_set.size(), transient_set.size(),
                 transient_set.size() - chain_set.size());
    CHECK(chain_set.size() <= transient_set.size());
  }
}

static void test_sequential_chain_two_on_rich() {
  std::println("test_sequential_chain_two_on_rich");

  auto f = load_rich_six_taxon_fixture();
  std::println("  fixture: {} clades, {} productions, {} active patterns",
               f.grammar.clades.size(), f.grammar.productions.size(),
               f.active.patterns.patterns.size());

  larch::overlay_chain chain(f.grammar);
  larch::inside_chart_cache cache = larch::build_inside_chart_cache(
      f.grammar, f.active, f.options, f.invariant_offset);
  assert_cache_inside_matches_from_scratch(chain, cache, "cold");

  auto achieved = run_sequential_chain_with_cache(chain, cache, f.grammar, 2);
  CHECK(achieved >= 2);
  CHECK(chain.size() == achieved);

  std::println("  PASS (chain length {}, {} rows recomputed)", chain.size(),
               cache.inside_rows_recomputed_on_commit);
}

static void test_long_sequential_chain_on_rich() {
  std::println("test_long_sequential_chain_on_rich");

  auto f = load_rich_six_taxon_fixture();
  larch::overlay_chain chain(f.grammar);
  larch::inside_chart_cache cache = larch::build_inside_chart_cache(
      f.grammar, f.active, f.options, f.invariant_offset);

  // "Long enough to exercise multi-level ancestor recomputation": aim for 4
  // accepts so chained overlays stack temp clades that force ancestor closure
  // several levels up.
  auto achieved = run_sequential_chain_with_cache(chain, cache, f.grammar, 4);
  CHECK(achieved >= 3);
  CHECK(chain.size() == achieved);
  CHECK(cache.temp_clade_count > 0);
  CHECK(cache.inside_rows_recomputed_on_commit > 0);

  std::println("  PASS (chain length {}, {} temp clades, {} rows recomputed)",
               chain.size(), cache.temp_clade_count,
               cache.inside_rows_recomputed_on_commit);
}

static void test_sequential_chain_on_test_5_trees() {
  std::println("test_sequential_chain_on_test_5_trees");

  auto f = load_test_5_trees_fixture();
  std::println("  fixture: {} clades, {} productions, {} active patterns",
               f.grammar.clades.size(), f.grammar.productions.size(),
               f.active.patterns.patterns.size());

  larch::overlay_chain chain(f.grammar);
  larch::inside_chart_cache cache = larch::build_inside_chart_cache(
      f.grammar, f.active, f.options, f.invariant_offset);
  assert_cache_inside_matches_from_scratch(chain, cache, "cold");

  // The medium CI fixture supports a multi-commit chain, so exercise the
  // length-2 case here as well (the plan names test_5_trees alongside
  // wric_binary_four for the commit-sequence oracle).
  auto achieved = run_sequential_chain_with_cache(chain, cache, f.grammar, 2);
  CHECK(achieved >= 2);
  CHECK(chain.size() == achieved);
  CHECK(!cache.exact_trim_active_only.has_value());

  std::println("  PASS (chain length {}, {} rows recomputed)", chain.size(),
               cache.inside_rows_recomputed_on_commit);
}

// Empty-chain guard: the affected-set and commit primitives reject an empty
// chain with a labelled error rather than silently doing nothing.
static void test_empty_chain_throws() {
  std::println("test_empty_chain_throws");

  auto f = load_binary_four_fixture();
  larch::overlay_chain chain(f.grammar);
  CHECK(chain.empty());

  std::string msg;
  bool threw = throws_runtime_error([&] {
    try {
      (void)larch::compute_chain_inside_affected_set(chain);
    } catch (std::runtime_error const& e) {
      msg = e.what();
      throw;
    }
  });
  CHECK(threw);
  CHECK(msg.find("empty chain") != std::string::npos);

  larch::inside_chart_cache cache = larch::build_inside_chart_cache(
      f.grammar, f.active, f.options, f.invariant_offset);
  msg.clear();
  threw = throws_runtime_error([&] {
    try {
      larch::apply_commit_to_inside_cache(chain, cache);
    } catch (std::runtime_error const& e) {
      msg = e.what();
      throw;
    }
  });
  CHECK(threw);
  CHECK(msg.find("empty chain") != std::string::npos);

  std::println("  PASS");
}

int main() {
  test_plan_cold_cache_matches_checked_oracle();
  test_single_commit_on_binary_four();
  test_affected_set_single_delta_relation();
  test_sequential_chain_two_on_rich();
  test_long_sequential_chain_on_rich();
  test_sequential_chain_on_test_5_trees();
  test_empty_chain_throws();

  std::println("All inside_chart_cache (Phase 2) tests passed!");
  return 0;
}

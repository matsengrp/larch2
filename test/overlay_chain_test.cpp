// Phase 1 deliverable: overlay-chain container with materialization oracle.
//
// This test exercises the append-only overlay chain (`overlay_chain`) as a pure
// data structure -- chain position as ordinal, taxon-set key as identity,
// tombstone rule enforced -- and pins its whole-chain materialization
// (`materialize_overlay_chain`) to the from-scratch sequential application of
// the same deltas.  It touches no search-loop code; the chain is driven only
// from this test.
//
// Coverage (per the plan's Phase 1 exit criteria):
//   * chain-of-1 on `wric_binary_four`: materialize_overlay_chain equals the
//     single-delta sequential materialization.
//   * chain-of-2 and chain-of-5 on a richer merged fixture: per-step two-sided
//     oracle (clade taxon-set set + production-key set equality) after every
//     accept.
//   * double-tombstone throws a labelled error and leaves the chain unchanged.
//   * dangling-clade reference throws a labelled error and leaves the chain
//     unchanged.

#include <larch/build_fasta_newick.hpp>
#include <larch/chart_spr.hpp>
#include <larch/chart_spr_search.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/overlay_chain.hpp>
#include <larch/rank3_rewrite.hpp>

#include "test_util.hpp"

#include <algorithm>
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

// Whether `msg` is one of the labelled Phase-1 overlay-chain rejections that a
// sequential search may legitimately encounter and skip -- a tombstone of an
// earlier delta's added production (out of Phase-1 scope) or a double
// tombstone.  Any other `overlay_chain` throw indicates a real append bug (a
// dangling clade reference, an out-of-range index, ...) and must propagate so
// a wrong-rejection regression fails loudly instead of being masked as a
// silent skip.  Shared by `run_sequential_chain` and
// `test_chain_of_one_on_binary_four` so the classification lives in one place.
static bool is_expected_overlay_chain_rejection(std::string const& msg) {
  return msg.find("overlay_chain") != std::string::npos &&
         (msg.find("not a frozen-base production") != std::string::npos ||
          msg.find("double tombstone") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Grammar-shape comparison helpers (taxon-set-key level, per the plan's exit
// criteria: clade taxon-set set and production-key set, never dense clade ids).
// ---------------------------------------------------------------------------

static std::set<std::vector<larch::taxon_id>> clade_taxa_set(
    larch::clade_grammar const& grammar) {
  std::set<std::vector<larch::taxon_id>> set;
  for (auto const& clade : grammar.clades) set.insert(clade.taxa);
  return set;
}

static void assert_grammars_match_at_key_level(
    larch::clade_grammar const& chain_grammar,
    larch::clade_grammar const& oracle_grammar, std::string const& context) {
  auto chain_clades = clade_taxa_set(chain_grammar);
  auto oracle_clades = clade_taxa_set(oracle_grammar);
  if (chain_clades != oracle_clades) {
    std::println(stderr, "  clade taxon-set mismatch [{}]", context);
    std::println(stderr, "    chain clades:  {}", chain_clades.size());
    std::println(stderr, "    oracle clades: {}", oracle_clades.size());
    for (auto const& c : chain_clades) {
      if (!oracle_clades.contains(c)) {
        std::print(stderr, "    chain-only clade: {{");
        for (std::size_t i = 0; i < c.size(); ++i) {
          if (i) std::print(stderr, ",");
          std::print(stderr, "{}", c[i]);
        }
        std::println(stderr, "}}");
      }
    }
    for (auto const& c : oracle_clades) {
      if (!chain_clades.contains(c)) {
        std::print(stderr, "    oracle-only clade: {{");
        for (std::size_t i = 0; i < c.size(); ++i) {
          if (i) std::print(stderr, ",");
          std::print(stderr, "{}", c[i]);
        }
        std::println(stderr, "}}");
      }
    }
    CHECK(false);
  }

  auto chain_keys = larch::rank3_detail::production_key_set(chain_grammar);
  auto oracle_keys = larch::rank3_detail::production_key_set(oracle_grammar);
  if (chain_keys != oracle_keys) {
    std::println(stderr, "  production-key mismatch [{}]", context);
    std::println(stderr, "    chain keys:  {}", chain_keys.size());
    std::println(stderr, "    oracle keys: {}", oracle_keys.size());
    for (auto const& k : chain_keys) {
      if (!oracle_keys.contains(k)) {
        std::println(stderr, "    chain-only production: {}",
                     larch::rank3_detail::production_key_to_string(k));
      }
    }
    for (auto const& k : oracle_keys) {
      if (!chain_keys.contains(k)) {
        std::println(stderr, "    oracle-only production: {}",
                     larch::rank3_detail::production_key_to_string(k));
      }
    }
    CHECK(false);
  }
}

// ---------------------------------------------------------------------------
// Fixtures.
// ---------------------------------------------------------------------------

static larch::clade_grammar load_wric_binary_four_grammar() {
  auto dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/wric_binary_four.fa"),
      larch::test::source_path_string("test/wric_binary_four.nwk"),
      larch::test::source_path_string("test/wric_binary_four.ref"));
  auto grammar = larch::build_clade_grammar(dag);
  CHECK(!larch::grammar_has_kary_productions(grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  return grammar;
}

// A richer grammar (six taxa, several distinct merged topologies) so that long
// sequential SPR chains exist whose every tombstone resolves to a frozen-base
// production -- the precondition the Phase-1 chain enforces.
static larch::phylo_dag make_six_taxon_tree(std::size_t which) {
  using namespace larch::test;
  constexpr std::string_view ref = "AAAAAA";
  auto leaf = [](char id) {
    return tiny_leaf(std::string{&id, 1}, "AAAAAA");
  };
  auto inner = [](std::string name, std::vector<tiny_tree_node> children) {
    return tiny_inner(std::move(name), "", std::move(children));
  };

  switch (which) {
    case 0:
      // (((A,B),(C,D)),(E,F))
      return make_tiny_labelled_tree(ref,
                                     inner("r0", {
                                       inner("abcd", {
                                         inner("ab", {leaf('A'), leaf('B')}),
                                         inner("cd", {leaf('C'), leaf('D')}),
                                       }),
                                       inner("ef", {leaf('E'), leaf('F')}),
                                     }));
    case 1:
      // (((A,C),(B,D)),(E,F))
      return make_tiny_labelled_tree(ref,
                                     inner("r1", {
                                       inner("abcd", {
                                         inner("ac", {leaf('A'), leaf('C')}),
                                         inner("bd", {leaf('B'), leaf('D')}),
                                       }),
                                       inner("ef", {leaf('E'), leaf('F')}),
                                     }));
    case 2:
      // ((A,B),((C,E),(D,F)))
      return make_tiny_labelled_tree(ref,
                                     inner("r2", {
                                       inner("ab", {leaf('A'), leaf('B')}),
                                       inner("cdef", {
                                         inner("ce", {leaf('C'), leaf('E')}),
                                         inner("df", {leaf('D'), leaf('F')}),
                                       }),
                                     }));
    case 3:
      // (((A,B),(E,F)),(C,D))
      return make_tiny_labelled_tree(ref,
                                     inner("r3", {
                                       inner("abef", {
                                         inner("ab", {leaf('A'), leaf('B')}),
                                         inner("ef", {leaf('E'), leaf('F')}),
                                       }),
                                       inner("cd", {leaf('C'), leaf('D')}),
                                     }));
    case 4:
      // ((A,C),((B,E),(D,F)))
      return make_tiny_labelled_tree(ref,
                                     inner("r4", {
                                       inner("ac", {leaf('A'), leaf('C')}),
                                       inner("bdef", {
                                         inner("be", {leaf('B'), leaf('E')}),
                                         inner("df", {leaf('D'), leaf('F')}),
                                       }),
                                     }));
    case 5:
      // (((A,D),(B,C)),(E,F))
      return make_tiny_labelled_tree(ref,
                                     inner("r5", {
                                       inner("abcd", {
                                         inner("ad", {leaf('A'), leaf('D')}),
                                         inner("bc", {leaf('B'), leaf('C')}),
                                       }),
                                       inner("ef", {leaf('E'), leaf('F')}),
                                     }));
    default:
      throw std::runtime_error("make_six_taxon_tree: unknown topology index");
  }
}

static larch::clade_grammar load_rich_six_taxon_grammar() {
  std::vector<larch::phylo_dag> trees;
  for (std::size_t i = 0; i < 6; ++i) trees.push_back(make_six_taxon_tree(i));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);
  CHECK(!larch::grammar_has_kary_productions(grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  return grammar;
}

// ---------------------------------------------------------------------------
// Sequential chain runner with a per-step two-sided oracle.
//
// `tip` is both the candidate source (each move is generated against it) and
// the oracle (it is advanced by applying the accepted move via the existing
// dense materialization path).  After every accept, the chain's whole-chain
// materialization must equal `tip` at the taxon-set-key level.
//
// A candidate whose append throws (because it would tombstone an earlier
// delta's added production, which the Phase-1 chain rejects as out of scope)
// is skipped; the runner tries the next candidate.  This is the documented
// Phase-1 scope, not a silent fallback: the rejection is a labelled throw
// inside `overlay_chain::append`.
// ---------------------------------------------------------------------------

static std::size_t run_sequential_chain(larch::overlay_chain& chain,
                                        larch::clade_grammar const& base,
                                        std::size_t target_steps) {
  larch::clade_grammar tip = base;  // copy: candidate source + oracle
  std::size_t achieved = 0;
  for (std::size_t step = 0; step < target_steps; ++step) {
    auto candidates = larch::enumerate_grammar_spr_candidates(tip);
    bool stepped = false;
    for (auto const& candidate : candidates) {
      larch::spr_overlay_delta delta;
      try {
        delta = larch::build_spr_overlay_delta(tip, candidate);
      } catch (std::runtime_error const&) {
        continue;  // candidate not buildable against this tip
      }

      // Append offers the strong exception guarantee; on throw the chain is
      // unchanged and we try the next candidate.  We swallow ONLY the labelled
      // rejections the Phase-1 chain is documented to raise for legitimate
      // candidates built against the materialized tip (see
      // `is_expected_overlay_chain_rejection`); anything else is a real append
      // bug and is re-thrown so a wrong-rejection regression fails loudly
      // instead of being masked as a silent skip.
      try {
        chain.append(delta);
      } catch (std::runtime_error const& e) {
        if (!is_expected_overlay_chain_rejection(e.what())) throw;
        continue;
      }

      // Advance the oracle by applying the accepted move densely, exactly as
      // the rebuild_after_accept search path does.
      auto overlay = larch::overlay_from_candidate(tip, candidate);
      tip = larch::materialize_overlay_grammar(overlay).grammar;

      auto chain_materialized = larch::materialize_overlay_chain(chain);
      assert_grammars_match_at_key_level(
          chain_materialized.grammar, tip,
          "step " + std::to_string(step) + " (chain length " +
              std::to_string(chain.size()) + ")");
      ++achieved;
      stepped = true;
      break;
    }
    if (!stepped) {
      std::println("  run_sequential_chain: no appendable candidate at step {}; "
                   "stopped at length {}",
                   step, achieved);
      break;
    }
  }
  return achieved;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

static void test_chain_of_one_on_binary_four() {
  std::println("test_chain_of_one_on_binary_four");

  auto base = load_wric_binary_four_grammar();
  larch::overlay_chain chain(base);

  // Take the first buildable candidate and append it; the chain's
  // materialization must equal the dense single-delta materialization.
  auto candidates = larch::enumerate_grammar_spr_candidates(base);
  CHECK(!candidates.empty());
  bool appended = false;
  for (auto const& candidate : candidates) {
    larch::spr_overlay_delta delta;
    try {
      delta = larch::build_spr_overlay_delta(base, candidate);
    } catch (std::runtime_error const&) {
      continue;
    }
    // On a first append against the frozen base, neither legitimate Phase-1
    // rejection can fire (no earlier delta -> no double tombstone; delta built
    // against base -> tombstones are base productions), so any throw from
    // `append` would be a real bug.  Use the same filtered-rethrow pattern as
    // `run_sequential_chain` for consistency: only the labelled skip-worthy
    // rejections are swallowed, everything else propagates.  The oracle check
    // runs inside the same try, so a materialization bug or key-level mismatch
    // (thrown as a std::runtime_error by `test_fail`) likewise propagates
    // instead of being masked as a skip.
    try {
      chain.append(delta);
      appended = true;

      auto overlay = larch::overlay_from_candidate(base, candidate);
      auto oracle = larch::materialize_overlay_grammar(overlay).grammar;
      auto chain_materialized = larch::materialize_overlay_chain(chain);
      assert_grammars_match_at_key_level(chain_materialized.grammar, oracle,
                                         "chain-of-1");
      break;
    } catch (std::runtime_error const& e) {
      if (!is_expected_overlay_chain_rejection(e.what())) throw;
    }
  }
  CHECK(appended);
  CHECK(chain.size() == 1);

  std::println("  PASS (chain length {})", chain.size());
}

static void test_sequential_chain_length_two() {
  std::println("test_sequential_chain_length_two");

  auto base = load_rich_six_taxon_grammar();
  std::println("  rich fixture: {} clades, {} productions",
               base.clades.size(), base.productions.size());

  larch::overlay_chain chain(base);
  auto achieved = run_sequential_chain(chain, base, /*target_steps=*/2);
  CHECK(achieved >= 2);
  CHECK(chain.size() == achieved);

  std::println("  PASS (chain length {})", chain.size());
}

static void test_sequential_chain_length_five() {
  std::println("test_sequential_chain_length_five");

  auto base = load_rich_six_taxon_grammar();
  larch::overlay_chain chain(base);
  auto achieved = run_sequential_chain(chain, base, /*target_steps=*/5);
  CHECK(achieved >= 5);
  CHECK(chain.size() == achieved);

  std::println("  PASS (chain length {})", chain.size());
}

static void test_double_tombstone_throws_and_leaves_chain_unchanged() {
  std::println("test_double_tombstone_throws_and_leaves_chain_unchanged");

  auto base = load_wric_binary_four_grammar();
  larch::overlay_chain chain(base);

  // Find two candidates against the same base that tombstone at least one
  // common base production (on this fixture essentially every SPR tombstones
  // the root production), so the second append hits the double-tombstone rule.
  auto candidates = larch::enumerate_grammar_spr_candidates(base);
  CHECK(candidates.size() >= 2);

  larch::spr_overlay_delta first_delta;
  std::vector<larch::production_id> first_tombstones;
  bool have_first = false;
  for (auto const& candidate : candidates) {
    larch::spr_overlay_delta delta;
    try {
      delta = larch::build_spr_overlay_delta(base, candidate);
    } catch (std::runtime_error const&) {
      continue;
    }
    if (delta.removed_base_productions.empty()) continue;
    try {
      chain.append(delta);
    } catch (std::runtime_error const&) {
      continue;
    }
    first_delta = delta;
    first_tombstones = delta.removed_base_productions;
    have_first = true;
    break;
  }
  CHECK(have_first);
  CHECK(chain.size() == 1);
  CHECK(!first_tombstones.empty());

  // Append a second delta (built against the SAME base) that tombstones a
  // production the first delta already tombstoned.  Because both are against
  // the frozen base, their tombstone id spaces coincide, so any second
  // candidate overlapping the first's tombstones triggers the rule.
  bool threw = false;
  for (auto const& candidate : candidates) {
    larch::spr_overlay_delta delta;
    try {
      delta = larch::build_spr_overlay_delta(base, candidate);
    } catch (std::runtime_error const&) {
      continue;
    }
    bool overlaps = false;
    for (auto pid : delta.removed_base_productions) {
      if (std::find(first_tombstones.begin(), first_tombstones.end(), pid) !=
          first_tombstones.end()) {
        overlaps = true;
        break;
      }
    }
    if (!overlaps) continue;

    std::string message;
    threw = throws_runtime_error([&] {
      try {
        chain.append(delta);
      } catch (std::runtime_error const& e) {
        message = e.what();
        throw;
      }
    });
    CHECK(message.find("overlay_chain") != std::string::npos);
    CHECK(message.find("double tombstone") != std::string::npos);
    break;
  }
  CHECK(threw);
  // Strong exception guarantee: the chain is unchanged by the failed append.
  // `size()` is the chain-position count; `position_of` confirms the surviving
  // delta still occupies ordinal 0.
  CHECK(chain.size() == 1);
  CHECK(chain.position_of(chain.at(0)) == 0);
  CHECK(chain.at(0).removed_base_productions == first_tombstones);
  (void)first_delta;

  std::println("  PASS (chain length still {})", chain.size());
}

static void test_dangling_clade_reference_throws_and_leaves_chain_unchanged() {
  std::println("test_dangling_clade_reference_throws_and_leaves_chain_unchanged");

  // A chain over the four-taxon base, fed a delta whose base grammar is a
  // DIFFERENT (six-taxon) grammar.  The delta's added production references
  // the six-taxon root clade via base space; that taxon set is absent from the
  // chain's frozen base and from every earlier-added clade, so `append` must
  // throw the labelled dangling-clade error.  The delta deliberately carries
  // no tombstones, so the dangling check (which runs during production
  // rebasing, before the tombstone rule) is the first guard to fire.
  auto base = load_wric_binary_four_grammar();
  auto other = load_rich_six_taxon_grammar();
  larch::overlay_chain chain(base);
  CHECK(chain.empty());

  larch::spr_overlay_delta delta;
  delta.base = &other;
  larch::overlay_grammar_production prod;
  prod.parent = larch::base_clade_ref(other.root_clade);
  // The child reference is intentionally to the same six-taxon root: the only
  // thing that matters for this guard is that the referenced clade's taxon set
  // is absent from the chain base, which forces the dangling path.
  prod.children = {larch::base_clade_ref(other.root_clade)};
  prod.multiplicity = 1;
  delta.temp_productions.push_back(std::move(prod));

  std::string message;
  bool threw = throws_runtime_error([&] {
    try {
      chain.append(delta);
    } catch (std::runtime_error const& e) {
      message = e.what();
      throw;
    }
  });
  CHECK(threw);
  CHECK(message.find("overlay_chain") != std::string::npos);
  CHECK(message.find("dangling clade reference") != std::string::npos);

  // Strong exception guarantee: the chain is still empty.
  CHECK(chain.empty());

  std::println("  PASS (chain length still {})", chain.size());
}

// A delta may carry an added temp clade that no production references.  The
// Phase-1 oracle (materialize_overlay_chain) is insensitive to such clades --
// materialize_overlay_grammar drops unreachable clades -- so this test inspects
// the stored delta directly to confirm the chain records every added clade
// rather than silently dropping unreferenced ones (the latent assumption noted
// in review).
static void test_unreferenced_temp_clades_are_carried() {
  std::println("test_unreferenced_temp_clades_are_carried");

  auto base = load_wric_binary_four_grammar();

  // Pick two taxon sets that are NOT frozen-base clades, so both are genuinely
  // new and would be assigned fresh merged ids (not resolved to base).
  std::set<std::vector<larch::taxon_id>> base_taxa;
  for (auto const& c : base.clades) base_taxa.insert(c.taxa);
  std::vector<std::vector<larch::taxon_id>> non_base_pairs;
  for (larch::taxon_id a = 0; a < 4; ++a) {
    for (larch::taxon_id b = static_cast<larch::taxon_id>(a + 1); b < 4; ++b) {
      std::vector<larch::taxon_id> pair{a, b};
      if (!base_taxa.contains(pair)) non_base_pairs.push_back(std::move(pair));
    }
  }
  CHECK(non_base_pairs.size() >= 2);
  auto referenced_taxa = non_base_pairs[0];
  auto unreferenced_taxa = non_base_pairs[1];

  larch::overlay_chain chain(base);

  larch::spr_overlay_delta delta;
  delta.base = &base;
  // temp_clades[0] is referenced by the production below; temp_clades[1] is
  // deliberately unreferenced.
  delta.temp_clades.push_back(larch::clade_key{referenced_taxa});
  delta.temp_clades.push_back(larch::clade_key{unreferenced_taxa});
  larch::overlay_grammar_production prod;
  prod.parent = larch::base_clade_ref(base.root_clade);
  prod.children = {larch::temp_clade_ref(0)};
  prod.multiplicity = 1;
  delta.temp_productions.push_back(std::move(prod));

  chain.append(delta);
  CHECK(chain.size() == 1);

  // Both clades must be recorded: resolve_ref only records clades named by
  // productions, so without the explicit carry-over the unreferenced clade
  // would be silently dropped.
  auto const& stored = chain.at(0);
  std::set<std::vector<larch::taxon_id>> stored_taxa;
  for (auto const& c : stored.temp_clades) stored_taxa.insert(c.taxa);
  CHECK(stored_taxa.contains(referenced_taxa));
  CHECK(stored_taxa.contains(unreferenced_taxa));

  std::println("  PASS (stored {} temp clades)", stored.temp_clades.size());
}

int main() {
  test_chain_of_one_on_binary_four();
  test_sequential_chain_length_two();
  test_sequential_chain_length_five();
  test_double_tombstone_throws_and_leaves_chain_unchanged();
  test_dangling_clade_reference_throws_and_leaves_chain_unchanged();
  test_unreferenced_temp_clades_are_carried();

  std::println("All overlay_chain tests passed!");
  return 0;
}

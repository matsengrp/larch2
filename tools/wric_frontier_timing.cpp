// wric_frontier_timing: time build_multisite_frontiers under the exact option
// combos used by the primary score-only trim vs the witness trace, to isolate
// what makes witness provenance retention expensive.
#include <larch/load_proto_dag.hpp>
#include <larch/chart_trim.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/site_patterns.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <dag.pb[.gz]> <optimum> <combo>\n", argv[0]);
    std::fprintf(stderr, "  combo: a=no-prov/no-ub  b=no-prov/ub  c=prov/ub  d=prov/ub/witness-cap\n");
    return 2;
  }
  auto dag = larch::load_proto_dag(argv[1]);
  auto ub = static_cast<std::uint64_t>(std::strtoull(argv[2], nullptr, 10));
  std::string combo = argv[3];

  larch::clade_grammar_options grammar_opts;
  larch::polytomy_refinement_options polytomy_opts;
  polytomy_opts.mode = larch::polytomy_mode::expand_soft_bounded;
  polytomy_opts.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, grammar_opts, polytomy_opts);

  larch::site_pattern_options pattern_opts;
  auto patterns = larch::build_site_patterns(dag, refinement.grammar, pattern_opts);

  larch::chart_options chart_opts;
  chart_opts.score_ua_edge = true;

  larch::chart_multisite_detail::multisite_frontier_build_options build_options;
  build_options.keep_provenance = (combo == "c" || combo == "d");
  build_options.keep_used_production = false;
  build_options.use_bound_pruning = true;
  build_options.dominance_mode = larch::multisite_dominance_mode::score_only;
  build_options.provenance_for_witness_only = (combo == "d" || combo == "c");
  if (combo == "b" || combo == "c" || combo == "d") {
    build_options.upper_bound_override = ub;
  }
  if (combo == "d") {
    build_options.max_provenance_choices_per_entry = 2;
  }

  auto start = std::chrono::steady_clock::now();
  auto build = larch::chart_multisite_detail::build_multisite_frontiers(
      refinement.grammar, patterns, chart_opts, build_options, "timing");
  auto ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start)
                .count();

  std::uint64_t root_best = ~0ull;
  auto const& root = build.frontiers[refinement.grammar.root_clade];
  for (auto const& e : root) {
    auto lb = larch::chart_multisite_detail::lower_bound_for_entry(
        e, refinement.grammar.root_clade, build.active_pattern_view(),
        build.invariant_constant_offset, chart_opts);
    if (lb < root_best) root_best = lb;
  }
  std::size_t prov_total = 0, prov_max = 0, entries = 0;
  for (auto const& fr : build.frontiers) {
    entries += fr.size();
    for (auto const& e : fr) {
      prov_total += e.provenance.size();
      if (e.provenance.size() > prov_max) prov_max = e.provenance.size();
    }
  }
  std::fprintf(stderr, "combo=%s initial_ub=%llu build_ms=%.1f root_best=%llu entries=%zu prov_total=%zu prov_max=%zu dom_pruned=%zu eq_dedup=%zu bound_pruned=%zu\n",
               combo.c_str(), ms, (unsigned long long)build.initial_upper_bound, (unsigned long long)root_best, entries,
               prov_total, prov_max, build.dominance_pruned,
               build.equality_deduplicated, build.bound_pruned);
  return 0;
}

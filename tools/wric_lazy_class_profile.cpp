// wric_lazy_class_profile: load a DAG pb, build the chart-BNB refined grammar
// (same options as larch2 --trim-mode chart-bnb), build the lazy inside chart,
// and print the per-clade class-count profile vs the dense per-pattern chart.
#include <larch/load_proto_dag.hpp>
#include <larch/lazy_chart.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/site_patterns.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dag.pb[.gz]> [--dense-cells-cap N]\n", argv[0]);
    return 2;
  }
  auto dag = larch::load_proto_dag(argv[1]);

  larch::clade_grammar_options grammar_opts;
  larch::polytomy_refinement_options polytomy_opts;
  polytomy_opts.mode = larch::polytomy_mode::expand_soft_bounded;
  polytomy_opts.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, grammar_opts, polytomy_opts);
  auto const& grammar = refinement.grammar;

  larch::site_pattern_options pattern_opts;
  auto patterns = larch::build_site_patterns(dag, grammar, pattern_opts);

  auto chart = larch::build_lazy_inside_chart(grammar, patterns);

  std::size_t C = grammar.clades.size();
  std::size_t K = chart.pattern_count;
  std::size_t active_K = 0;
  for (auto const& p : patterns.patterns) {
    if (larch::chart_multisite_detail::is_active_pattern(p)) ++active_K;
  }
  std::size_t total_weight = chart.total_pattern_weight;

  std::uint64_t dense_cells = static_cast<std::uint64_t>(C) * K;
  std::uint64_t lazy_cells = 0;
  std::uint64_t lazy_structural = 0;
  std::size_t max_classes = 0, max_at = 0;
  std::vector<std::size_t> classes_by_clade(C);
  for (std::size_t c = 0; c < C; ++c) {
    auto n = chart.inside_class_count(larch::clade_id{static_cast<std::uint32_t>(c)});
    classes_by_clade[c] = n;
    lazy_cells += n;
    lazy_structural += chart.structural_class_count(larch::clade_id{static_cast<std::uint32_t>(c)});
    if (n > max_classes) { max_classes = n; max_at = c; }
  }

  std::fprintf(stderr, "input: %s\n", argv[1]);
  std::fprintf(stderr, "clades C: %zu\n", C);
  std::fprintf(stderr, "patterns K: %zu (active %zu), total site weight %zu\n",
               K, active_K, total_weight);
  std::fprintf(stderr, "dense chart cells (C*K): %llu\n",
               (unsigned long long)dense_cells);
  std::fprintf(stderr, "lazy inside-class cells (sum_X classes(X)): %llu\n",
               (unsigned long long)lazy_cells);
  std::fprintf(stderr, "lazy structural-class cells: %llu\n",
               (unsigned long long)lazy_structural);
  std::fprintf(stderr, "compression vs dense: %.2fx\n",
               dense_cells ? (double)dense_cells / (double)lazy_cells : 0.0);
  std::fprintf(stderr, "max classes at one clade: %zu (of K=%zu)\n", max_classes, K);

  // Bucket by clade taxon-set size to show the descent of the class count.
  std::fprintf(stderr, "classes by clade size bucket (taxa: clades, sum classes, max classes):\n");
  auto bucket = [](std::size_t taxa) -> const char* {
    if (taxa == 1) return "1";
    if (taxa <= 4) return "2-4";
    if (taxa <= 16) return "5-16";
    if (taxa <= 64) return "17-64";
    if (taxa <= 256) return "65-256";
    return ">256";
  };
  struct Agg { std::size_t n = 0, sum = 0, max = 0; };
  std::vector<std::pair<const char*, Agg>> order = {
      {"1", {}}, {"2-4", {}}, {"5-16", {}}, {"17-64", {}}, {"65-256", {}}, {">256", {}}};
  for (std::size_t c = 0; c < C; ++c) {
    auto taxa = grammar.clades[larch::clade_id{static_cast<std::uint32_t>(c)}].taxa.size();
    for (auto& [name, agg] : order) {
      if (bucket(taxa) == name) {
        ++agg.n; agg.sum += classes_by_clade[c];
        agg.max = std::max(agg.max, classes_by_clade[c]);
      }
    }
  }
  for (auto const& [name, agg] : order) {
    if (agg.n) {
      std::fprintf(stderr, "  %7s: %6zu clades, %10zu sum, %8zu max\n",
                   name, agg.n, agg.sum, agg.max);
    }
  }
  // Weight distribution at the root: how many root classes and their weights.
  auto root = grammar.root_clade;
  std::fprintf(stderr, "root classes: %zu (== distinct full-column patterns)\n",
               chart.inside_class_count(root));
  return 0;
}

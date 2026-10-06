// wric_hull_lb (step 1 of the compression design discussion):
// quantifies the LB slack of mask-hull ("wavelet-style") pattern groupings.
//
// A grouping merges active patterns into groups; a group's row at every
// clade is the POINTWISE MIN over its members' per-pattern chart rows
// (equivalently: union the leaf-state masks and run one chart DP).  Min-plus
// monotonicity makes every grouped LB admissible; the slack
//   exact_lb - grouped_lb
// is the price of the compression.  This tool measures slack vs group count
// for (a) locality-sorted groupings (lexicographic over state vectors =
// wavelet-style neighbor merging) and (b) random-hash groupings (control),
// with the singleton level validating against the exact composite LB.
//
// Usage: wric_hull_lb levels <grammar_dag.pb.gz> [expected_exact_lb]

#include <larch/load_proto_dag.hpp>
#include <larch/phylo_dag.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/site_patterns.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/chart_trim.hpp>
#include <larch/compute.hpp>
#include <larch/polytomy_refinement.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace larch;
using chart_multisite_detail::active_pattern_info;
using chart_multisite_detail::combine_class_row;
using chart_multisite_detail::multisite_restriction_classes;
using chart_multisite_detail::multisite_production_class_map;
using chart_multisite_detail::build_restriction_classes;
using chart_multisite_detail::build_production_class_map;

namespace {

struct grammar_ctx {
  clade_grammar grammar;
  std::vector<multisite_restriction_classes> classes;
  std::vector<multisite_production_class_map> production_maps;
  std::vector<active_pattern_info> active;
  site_pattern_set patterns;
  chart_options chart_opts;
  std::uint64_t invariant_offset = 0;
};

grammar_ctx prepare(std::string const& dag_path) {
  grammar_ctx ctx;
  auto dag = load_proto_dag(dag_path);
  clade_grammar_options grammar_opts;
  polytomy_refinement_options polytomy_opts;
  polytomy_opts.mode = polytomy_mode::expand_soft_bounded;
  polytomy_opts.max_shapes_per_polytomy = 1;
  auto refinement =
      build_polytomy_refined_clade_grammar(dag, grammar_opts, polytomy_opts);
  ctx.grammar = std::move(refinement.grammar);
  site_pattern_options pattern_opts;
  ctx.patterns = build_site_patterns(dag, ctx.grammar, pattern_opts);
  ctx.chart_opts.score_ua_edge = true;
  ctx.invariant_offset = chart_multisite_detail::invariant_constant_offset(
      ctx.patterns, ctx.chart_opts);
  for (std::size_t i = 0; i < ctx.patterns.patterns.size(); ++i) {
    auto const& pattern = ctx.patterns.patterns[i];
    if (!chart_multisite_detail::is_active_pattern(pattern)) continue;
    active_pattern_info info;
    info.pattern_index = i;
    info.weight = pattern.weight;
    info.reference_state_counts = pattern.reference_state_counts;
    info.state_by_taxon = pattern.state_by_taxon;
    ctx.active.push_back(std::move(info));
  }
  // (classes/production maps are not needed for the grouped DP; the DP uses
  // leaf states + productions directly.)
  return ctx;
}

// One chart DP for one group of patterns (pointwise-min rows by
// construction: union leaf masks, min over productions).
std::uint64_t group_value(grammar_ctx const& ctx,
                          std::vector<std::size_t> const& members,
                          std::vector<chart_cost>& scratch_a,
                          std::vector<chart_cost>& scratch_b) {
  auto const& G = ctx.grammar;
  auto ncl = G.clades.size();
  static constexpr std::size_t S = nuc_state_count;
  std::vector<chart_cost> rows(ncl * S, chart_inf);
  // postorder: ascending clade size (children strictly smaller)
  std::vector<clade_id> order(ncl);
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id a, clade_id b) {
    auto l = G.clades[a].taxa.size(), r = G.clades[b].taxa.size();
    if (l != r) return l < r;
    return a < b;
  });
  // union leaf masks per taxon
  auto ntax = G.taxa.id_to_sample_id.size();
  std::vector<std::uint8_t> mask(ntax, 0);
  for (auto p : members)
    for (std::size_t t = 0; t < ntax; ++t)
      mask[t] |= static_cast<std::uint8_t>(1u << ctx.active[p].state_by_taxon[t]);
  for (auto c : order) {
    auto* row = &rows[static_cast<std::size_t>(c) * S];
    if (G.clades[c].taxa.size() == 1) {
      auto t = G.clades[c].taxa[0];
      for (std::uint8_t s = 0; s < S; ++s)
        row[s] = ((mask[t] >> s) & 1) ? 0 : chart_inf;
      continue;
    }
    for (auto pid : G.productions_by_parent[c]) {
      auto l = G.productions[pid].children[0];
      auto r = G.productions[pid].children[1];
      combine_class_row(&rows[static_cast<std::size_t>(l) * S],
                        &rows[static_cast<std::size_t>(r) * S],
                        scratch_a.data());
      for (std::uint8_t s = 0; s < S; ++s)
        row[s] = std::min(row[s], scratch_a[s]);
    }
  }
  std::array<std::uint32_t, S> counts{};
  for (auto p : members)
    for (std::uint8_t s = 0; s < S; ++s)
      counts[s] += ctx.active[p].reference_state_counts[s];
  // eval_row equivalent (kept local to avoid pulling extra deps)
  std::uint64_t total = 0;
  auto* root_row = &rows[static_cast<std::size_t>(G.root_clade) * S];
  for (std::uint8_t ref = 0; ref < S; ++ref) {
    auto count = counts[ref];
    if (count == 0) continue;
    chart_cost best = chart_inf;
    for (std::uint8_t s = 0; s < S; ++s)
      best = std::min(
          best,
          parsimony_chart_detail::saturated_add(
              root_row[s],
              parsimony_chart_detail::transition_cost(ref, s)));
    total += static_cast<std::uint64_t>(count) * best;
  }
  (void)scratch_b;
  return total;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 3 || std::string(argv[1]) != "levels")
      throw std::runtime_error("usage: wric_hull_lb levels <dag.pb.gz> "
                               "[expected_exact_lb]");
    auto ctx = prepare(argv[2]);
    std::uint64_t expected = 0;
    if (argc >= 4) expected = std::strtoull(argv[3], nullptr, 10);
    auto K = ctx.active.size();
    std::fprintf(stderr, "hull_lb: clades=%zu productions=%zu active=%zu "
                         "invariant_offset=%llu\n",
                 ctx.grammar.clades.size(), ctx.grammar.productions.size(), K,
                 (unsigned long long)ctx.invariant_offset);

    std::vector<chart_cost> sa(nuc_state_count), sb(nuc_state_count);

    // exact (singleton) LB - validation anchor
    std::uint64_t exact_lb = ctx.invariant_offset;
    {
      std::vector<std::vector<std::size_t>> groups(K);
      for (std::size_t p = 0; p < K; ++p) groups[p] = {p};
      for (auto const& g : groups)
        exact_lb += group_value(ctx, g, sa, sb);
    }
    std::fprintf(stderr, "hull_lb: exact (singleton) LB = %llu (expected %llu)"
                         " %s\n",
                 (unsigned long long)exact_lb, (unsigned long long)expected,
                 !expected || exact_lb == expected ? "OK" : "MISMATCH");
    if (expected && exact_lb != expected) return 1;

    // grouping orders
    std::vector<std::size_t> locality(K), rnd(K);
    std::iota(locality.begin(), locality.end(), 0);
    std::sort(locality.begin(), locality.end(), [&](std::size_t a, std::size_t b) {
      return ctx.active[a].state_by_taxon < ctx.active[b].state_by_taxon;
    });
    std::iota(rnd.begin(), rnd.end(), 0);
    std::uint64_t h = 0x9e3779b97f4a7c15ULL;
    std::sort(rnd.begin(), rnd.end(), [&](std::size_t a, std::size_t b) {
      auto ha = a * 0x9e3779b97f4a7c15ULL + h;
      ha ^= ha >> 31;
      auto hb = b * 0x9e3779b97f4a7c15ULL + h;
      hb ^= hb >> 31;
      return ha < hb;
    });
    (void)h;

    std::printf("groups\tlocality_lb\tlocality_slack\trandom_lb\trandom_slack\n");
    std::size_t levels[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    for (auto ng : levels) {
      if (ng > K) break;
      for (int scheme = 0; scheme < 2; ++scheme) {
        auto const& ord = scheme == 0 ? locality : rnd;
        // weight-balanced contiguous blocks over ord
        std::uint64_t wtotal = 0;
        for (std::size_t p = 0; p < K; ++p) wtotal += ctx.active[p].weight;
        std::vector<std::vector<std::size_t>> groups(ng);
        std::uint64_t acc = 0;
        std::size_t gi = 0;
        for (std::size_t i = 0; i < K; ++i) {
          auto p = ord[i];
          groups[std::min(gi, ng - 1)].push_back(p);
          acc += ctx.active[p].weight;
          if (gi < ng - 1 && acc * static_cast<std::uint64_t>(ng) >=
                                 wtotal * static_cast<std::uint64_t>(gi + 1)) {
            ++gi;
          }
        }
        std::uint64_t lb = ctx.invariant_offset;
        std::size_t ne = 0, mx = 0, tot = 0;
        for (auto const& g : groups) {
          if (g.empty()) continue;
          ++ne;
          mx = std::max(mx, g.size());
          tot += g.size();
          lb += group_value(ctx, g, sa, sb);
        }
        if (std::getenv("WRIC_HULL_DEBUG"))
          std::fprintf(stderr, "dbg ng=%zu scheme=%d nonempty=%zu max=%zu "
                              "total=%zu lb=%llu\n",
                       ng, scheme, ne, mx, tot, (unsigned long long)lb);
        if (scheme == 0)
          std::printf("%zu\t%llu\t%lld\t", ng, (unsigned long long)lb,
                      (long long)(exact_lb - lb));
        else
          std::printf("%llu\t%lld\n", (unsigned long long)lb,
                      (long long)(exact_lb - lb));
      }
    }
    return 0;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

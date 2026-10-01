// wric_ambiguity_diagnostic: measure whether a wide clade's surviving
// score-only frontier entries are distinguished by a concentrated or a
// diffuse set of site classes.
//
// Runs the class-compressed score pass (identical math to
// build_multisite_frontiers_class_compressed) with early exit at a target
// clade, then analyzes the surviving entries:
//   * per-class distinct column count across survivors (histogram),
//   * light-first prefix curve: distinct projected entries after the t
//     least-varying classes,
//   * heavy-first prefix curve for contrast.
// If the light-first curve collapses to a small number of projected
// meta-classes early, ambiguity is concentrated and a cut-and-condition
// scaffold is worth pursuing; if it tracks the full entry count until most
// classes are included, ambiguity is diffuse.
#include <larch/load_proto_dag.hpp>
#include <larch/chart_trim.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/site_patterns.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using larch::chart_multisite_detail::compressed_frontier_entry;
using larch::chart_multisite_detail::multisite_restriction_classes;

std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s <dag.pb[.gz]> <ub> <target-clade-id>\n", argv[0]);
    return 2;
  }
  auto dag = larch::load_proto_dag(argv[1]);
  auto ub = static_cast<std::uint64_t>(std::strtoull(argv[2], nullptr, 10));
  auto target = static_cast<larch::clade_id>(
      std::strtoul(argv[3], nullptr, 10));

  larch::clade_grammar_options grammar_opts;
  larch::polytomy_refinement_options polytomy_opts;
  polytomy_opts.mode = larch::polytomy_mode::expand_soft_bounded;
  polytomy_opts.max_shapes_per_polytomy = 1;
  auto refinement =
      larch::build_polytomy_refined_clade_grammar(dag, grammar_opts,
                                                  polytomy_opts);
  auto const& grammar = refinement.grammar;

  larch::site_pattern_options pattern_opts;
  auto patterns = larch::build_site_patterns(dag, grammar, pattern_opts);

  larch::chart_options options;
  options.score_ua_edge = true;

  std::fprintf(stderr, "building exact setup...\n");
  auto setup = larch::build_multisite_exact_setup(grammar, patterns, options);
  auto const& active = setup.active_patterns;
  std::fprintf(stderr, "setup: active=%zu clades=%zu lb=%llu ub0=%llu\n",
               active.size(), grammar.clades.size(),
               static_cast<unsigned long long>(setup.composite_lower_bound),
               static_cast<unsigned long long>(setup.initial_upper_bound));

  // Class tables, bottom-up.
  std::vector<multisite_restriction_classes> classes(grammar.clades.size());
  {
    std::vector<larch::clade_id> table_order(grammar.clades.size());
    std::iota(table_order.begin(), table_order.end(), larch::clade_id{0});
    std::stable_sort(table_order.begin(), table_order.end(),
                     [&](larch::clade_id lhs, larch::clade_id rhs) {
                       auto l = grammar.clades[lhs].taxa.size();
                       auto r = grammar.clades[rhs].taxa.size();
                       if (l != r) return l < r;
                       return lhs < rhs;
                     });
    for (auto clade : table_order) {
      classes[clade] = larch::chart_multisite_detail::build_restriction_classes(
          grammar, clade, active, classes);
    }
  }
  std::vector<larch::chart_multisite_detail::multisite_production_class_map>
      production_maps(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    production_maps[pid] =
        larch::chart_multisite_detail::build_production_class_map(
            grammar, grammar.productions[pid], classes);
  }

  std::vector<std::size_t> remaining_consumers(grammar.clades.size(), 0);
  for (auto const& prod : grammar.productions) {
    for (auto child : prod.children) ++remaining_consumers[child];
  }

  std::vector<larch::clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), larch::clade_id{0});
  std::stable_sort(order.begin(), order.end(),
                   [&](larch::clade_id lhs, larch::clade_id rhs) {
                     auto l = grammar.clades[lhs].taxa.size();
                     auto r = grammar.clades[rhs].taxa.size();
                     if (l != r) return l < r;
                     return lhs < rhs;
                   });

  std::vector<std::vector<compressed_frontier_entry>> frontiers(
      grammar.clades.size());
  std::size_t bound_pruned = 0, equality_deduplicated = 0;
  std::size_t dominance_candidates = 0, dominance_pruned = 0;
  std::size_t ordinal = 0;
  bool reached_target = false;

  for (auto clade : order) {
    ++ordinal;
    auto t0 = std::chrono::steady_clock::now();
    auto const& key = grammar.clades[clade];
    if (key.taxa.size() == 1) {
      compressed_frontier_entry leaf;
      auto const& leaf_classes = classes[clade];
      leaf.rows.assign(leaf_classes.class_count * larch::nuc_state_count,
                       larch::chart_inf);
      for (std::size_t c = 0; c < leaf_classes.class_count; ++c) {
        leaf.rows[c * larch::nuc_state_count +
                  leaf_classes.leaf_state_by_class[c]] = larch::chart_cost{0};
      }
      leaf.topology_hash = larch::chart_multisite_detail::mix_hash(0x6c656166ULL, key.taxa.front());
      frontiers[clade].push_back(std::move(leaf));
      continue;
    }

    std::unordered_map<std::size_t, std::vector<std::size_t>> entry_index;
    auto& entries = frontiers[clade];
    compressed_frontier_entry candidate;
    for (auto pid : grammar.productions_by_parent[clade]) {
      auto const& prod = grammar.productions[pid];
      larch::chart_trim_detail::validate_binary_production_for_trim(grammar,
                                                                    prod, pid);
      auto left_child = prod.children[0];
      auto right_child = prod.children[1];
      for (std::size_t li = 0; li < frontiers[left_child].size(); ++li) {
        auto const& left = frontiers[left_child][li];
        for (std::size_t ri = 0; ri < frontiers[right_child].size(); ++ri) {
          auto const& right = frontiers[right_child][ri];
          larch::chart_multisite_detail::combine_compressed_rows(
              left.rows, right.rows, production_maps[pid], pid,
              left.topology_hash, right.topology_hash,
              classes[clade].class_count, candidate);
          auto lb = larch::chart_multisite_detail::
              lower_bound_for_compressed_rows(
                  candidate.rows, classes[clade], clade, active,
                  setup.invariant_constant_offset, options);
          if (lb > ub) {
            ++bound_pruned;
            continue;
          }
          auto hash =
              larch::chart_multisite_detail::compressed_row_hash(
                  candidate.rows);
          std::size_t found = std::numeric_limits<std::size_t>::max();
          auto bucket = entry_index.find(hash);
          if (bucket != entry_index.end()) {
            for (auto entry : bucket->second) {
              if (entries[entry].rows == candidate.rows) {
                found = entry;
                break;
              }
            }
          }
          if (found != std::numeric_limits<std::size_t>::max()) {
            entries[found].topology_hash = std::min(
                entries[found].topology_hash, candidate.topology_hash);
            ++equality_deduplicated;
          } else {
            entry_index[hash].push_back(entries.size());
            entries.push_back(
                compressed_frontier_entry{candidate.rows,
                                          candidate.topology_hash});
          }
        }
      }
    }
    larch::chart_multisite_detail::
        apply_score_only_dominance_pruning_compressed(
            entries, dominance_candidates, dominance_pruned);

    auto ms = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (ms > 2000.0 || ordinal % 256 == 0) {
      std::fprintf(stderr,
                   "  clade=%u taxa=%zu entries=%zu ms=%.0f\n",
                   static_cast<unsigned>(clade), key.taxa.size(),
                   entries.size(), ms);
      std::fflush(stderr);
    }

    if (clade == target) {
      reached_target = true;
      break;
    }
    for (auto pid : grammar.productions_by_parent[clade]) {
      for (auto child : grammar.productions[pid].children) {
        if (--remaining_consumers[child] == 0) {
          std::vector<compressed_frontier_entry>().swap(frontiers[child]);
        }
      }
    }
  }
  if (!reached_target) {
    std::fprintf(stderr, "target clade %u never reached\n",
                 static_cast<unsigned>(target));
    return 1;
  }

  auto const& entries = frontiers[target];
  auto n = entries.size();
  auto class_count = classes[target].class_count;
  std::fprintf(stderr,
               "\ntarget clade %u: taxa=%zu entries=%zu classes=%zu "
               "(bound_pruned=%zu dedup=%zu dom_pruned=%zu)\n",
               static_cast<unsigned>(target),
               grammar.clades[target].taxa.size(), n, class_count,
               bound_pruned, equality_deduplicated, dominance_pruned);

  // Flat array of per-entry per-class column hashes.
  std::vector<std::uint64_t> col_hash(n * class_count);
  for (std::size_t e = 0; e < n; ++e) {
    auto const& rows = entries[e].rows;
    for (std::size_t c = 0; c < class_count; ++c) {
      std::uint64_t h = splitmix64(c + 1);
      for (std::uint8_t s = 0; s < larch::nuc_state_count; ++s) {
        h = splitmix64(h ^ (rows[c * larch::nuc_state_count + s] + 0x100));
      }
      col_hash[e * class_count + c] = h;
    }
  }

  // Per-class distinct column counts.
  std::vector<std::uint32_t> distinct_by_class(class_count);
  {
    std::vector<std::uint64_t> column(n);
    for (std::size_t c = 0; c < class_count; ++c) {
      for (std::size_t e = 0; e < n; ++e) {
        column[e] = col_hash[e * class_count + c];
      }
      std::sort(column.begin(), column.end());
      auto last = std::unique(column.begin(), column.end());
      distinct_by_class[c] =
          static_cast<std::uint32_t>(last - column.begin());
    }
  }
  std::size_t hist_const = 0, hist_2_5 = 0, hist_6_50 = 0, hist_51_500 = 0,
              hist_big = 0;
  for (auto d : distinct_by_class) {
    if (d == 1) ++hist_const;
    else if (d <= 5) ++hist_2_5;
    else if (d <= 50) ++hist_6_50;
    else if (d <= 500) ++hist_51_500;
    else ++hist_big;
  }
  std::printf("entries=%zu classes=%zu\n", n, class_count);
  std::printf("class distinct-value histogram: const=%zu 2-5=%zu 6-50=%zu "
              "51-500=%zu >500=%zu\n",
              hist_const, hist_2_5, hist_6_50, hist_51_500, hist_big);

  std::vector<std::uint32_t> light_order(class_count);
  std::iota(light_order.begin(), light_order.end(), 0u);
  std::stable_sort(light_order.begin(), light_order.end(),
                   [&](std::uint32_t a, std::uint32_t b) {
                     return distinct_by_class[a] < distinct_by_class[b];
                   });
  auto heavy_order = light_order;
  std::reverse(heavy_order.begin(), heavy_order.end());

  auto prefix_distinct = [&](std::vector<std::uint32_t> const& order,
                             std::size_t t) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> keys(n);
    for (std::size_t e = 0; e < n; ++e) {
      std::uint64_t k1 = 0x243f6a8885a308d3ULL;
      std::uint64_t k2 = 0x13198a2e03707344ULL;
      for (std::size_t i = 0; i < t; ++i) {
        auto h = col_hash[e * class_count + order[i]];
        k1 = k1 * 0x9e3779b97f4a7c15ULL + h;
        k2 = k2 * 0xc2b2ae3d27d4eb4fULL + h;
      }
      keys[e] = {k1, k2};
    }
    std::sort(keys.begin(), keys.end());
    return static_cast<std::size_t>(
        std::unique(keys.begin(), keys.end()) - keys.begin());
  };

  std::printf("light-first prefix curve (t classes -> distinct projected "
              "entries):\n");
  for (auto t : {1u, 2u, 5u, 10u, 25u, 50u, 100u, 250u, 500u, 1000u,
                 static_cast<std::uint32_t>(class_count)}) {
    if (t > class_count) continue;
    std::printf("  t=%-5u -> %zu\n", t, prefix_distinct(light_order, t));
  }
  std::printf("heavy-first prefix curve:\n");
  for (auto t : {1u, 2u, 5u, 10u, 25u, 50u, 100u, 250u, 500u, 1000u,
                 static_cast<std::uint32_t>(class_count)}) {
    if (t > class_count) continue;
    std::printf("  t=%-5u -> %zu\n", t, prefix_distinct(heavy_order, t));
  }
  return 0;
}

#pragma once

#include <larch/grammar_topology_enumerator.hpp>
#include <larch/grammar_topology_fitch_scorer.hpp>
#include <larch/thread_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {

struct grammar_topology_census_options {
  std::size_t worker_count = 1;

  // Cross-check every Nth global ordinal with the independent, stateless
  // four-state Sankoff implementation in score_selected_topology.  Zero
  // disables periodic checks.  Final minimum witnesses should additionally be
  // checked by the caller after the census.
  std::uint64_t sankoff_verification_stride = 0;

  // Retain one compact score per global grammar ordinal while performing the
  // existing exact pass.  This is opt-in so historical census callers keep
  // their prior memory envelope; score-band export enables it to avoid a
  // second full enumeration.
  bool retain_ordinal_score_ledger = false;
};

struct grammar_topology_census_result {
  std::uint64_t expected_topology_count = 0;
  std::uint64_t scored_topology_count = 0;
  std::map<std::uint64_t, std::uint64_t> score_histogram;
  // Indexed by production ID. Each map counts scores among concrete
  // topologies that select that production. Together with score_histogram,
  // this yields exact force-production and remove-production optima without
  // re-enumerating the grammar.
  std::vector<std::map<std::uint64_t, std::uint64_t>>
      selected_score_histogram_by_production;
  std::uint64_t optimum = multisite_score_inf;
  std::vector<std::uint64_t> optimal_ordinals;
  std::uint64_t sankoff_verified_topology_count = 0;
  std::uint64_t reachable_internal_clade_visits = 0;
  std::uint64_t recomputed_internal_clade_visits = 0;
  // Present exactly when retain_ordinal_score_ledger was requested.  Index is
  // the global grammar ordinal; worker-local contiguous ranges are joined in
  // canonical ordinal order and checked against the score histogram.
  std::vector<std::uint64_t> ordinal_scores;
};

namespace grammar_topology_census_detail {

inline void checked_accumulate(std::uint64_t& target, std::uint64_t value,
                               char const* label) {
  if (value > std::numeric_limits<std::uint64_t>::max() - target) {
    throw std::runtime_error(std::string{"grammar topology census: "} + label +
                             " overflow");
  }
  target += value;
}

struct worker_result {
  std::uint64_t ordinal_begin = 0;
  std::uint64_t scored_topology_count = 0;
  std::map<std::uint64_t, std::uint64_t> score_histogram;
  std::vector<std::map<std::uint64_t, std::uint64_t>>
      selected_score_histogram_by_production;
  std::uint64_t optimum = multisite_score_inf;
  std::vector<std::uint64_t> optimal_ordinals;
  std::uint64_t sankoff_verified_topology_count = 0;
  std::uint64_t reachable_internal_clade_visits = 0;
  std::uint64_t recomputed_internal_clade_visits = 0;
  std::vector<std::uint64_t> ordinal_scores;
};

inline worker_result census_worker(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& chart_options,
    grammar_topology_enumerator const& enumerator, grammar_topology_range range,
    std::uint64_t sankoff_verification_stride,
    bool retain_ordinal_score_ledger) {
  worker_result result;
  result.ordinal_begin = range.begin;
  result.selected_score_histogram_by_production.resize(
      grammar.productions.size());
  if (retain_ordinal_score_ledger) {
    if (range.size() > std::numeric_limits<std::size_t>::max()) {
      throw std::length_error(
          "grammar topology census: ordinal-score ledger exceeds size_t");
    }
    result.ordinal_scores.reserve(static_cast<std::size_t>(range.size()));
  }
  grammar_topology_fitch_scorer scorer(grammar, patterns, chart_options);
  auto emitted = enumerator.stream(
      range, [&](std::uint64_t ordinal, grammar_topology const& topology) {
        grammar_topology_fitch_score_stats stats;
        auto const score = scorer.score_enumerator_topology(topology, &stats);
        if (score >= multisite_score_inf) {
          throw std::runtime_error(
              "grammar topology census: non-finite or saturated score at "
              "ordinal " +
              std::to_string(ordinal));
        }
        if (retain_ordinal_score_ledger) result.ordinal_scores.push_back(score);
        checked_accumulate(result.reachable_internal_clade_visits,
                           stats.reachable_internal_clades,
                           "reachable-clade counter");
        checked_accumulate(result.recomputed_internal_clade_visits,
                           stats.recomputed_internal_clades,
                           "recomputed-clade counter");

        if (sankoff_verification_stride != 0 &&
            ordinal % sankoff_verification_stride == 0) {
          auto const sankoff = score_selected_topology(grammar, patterns,
                                                       topology, chart_options);
          if (sankoff != score) {
            throw std::runtime_error(
                "grammar topology census: generalized-Fitch/Sankoff score "
                "mismatch at ordinal " +
                std::to_string(ordinal) + ": " + std::to_string(score) +
                " != " + std::to_string(sankoff));
          }
          ++result.sankoff_verified_topology_count;
        }

        auto& histogram_count = result.score_histogram[score];
        if (histogram_count == std::numeric_limits<std::uint64_t>::max()) {
          throw std::runtime_error(
              "grammar topology census: histogram count overflow");
        }
        ++histogram_count;
        for (std::size_t pid = 0; pid < topology.used_production.size();
             ++pid) {
          if (!topology.used_production[pid]) continue;
          auto& selected_count =
              result.selected_score_histogram_by_production[pid][score];
          if (selected_count == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error(
                "grammar topology census: selected-production histogram "
                "count overflow");
          }
          ++selected_count;
        }
        if (score < result.optimum) {
          result.optimum = score;
          result.optimal_ordinals.clear();
        }
        if (score == result.optimum) {
          result.optimal_ordinals.push_back(ordinal);
        }
      });
  result.scored_topology_count = emitted;
  return result;
}

}  // namespace grammar_topology_census_detail

inline grammar_topology_census_result census_grammar_topologies(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& chart_options = {},
    grammar_topology_census_options const& census_options = {}) {
  if (census_options.worker_count == 0) {
    throw std::runtime_error(
        "grammar topology census: worker count must be positive");
  }

  grammar_topology_enumerator enumerator(grammar);
  grammar_topology_census_result result;
  result.expected_topology_count = enumerator.topology_count();
  result.selected_score_histogram_by_production.resize(
      grammar.productions.size());
  if (result.expected_topology_count == 0) {
    throw std::runtime_error(
        "grammar topology census: grammar has no concrete topologies");
  }
  if (census_options.retain_ordinal_score_ledger &&
      result.expected_topology_count >
          std::numeric_limits<std::size_t>::max()) {
    throw std::length_error(
        "grammar topology census: ordinal-score ledger exceeds size_t");
  }
  if (census_options.retain_ordinal_score_ledger) {
    result.ordinal_scores.reserve(
        static_cast<std::size_t>(result.expected_topology_count));
  }

  thread_pool pool(census_options.worker_count);
  std::vector<std::future<grammar_topology_census_detail::worker_result>>
      futures;
  futures.reserve(census_options.worker_count);
  for (std::size_t worker = 0; worker < census_options.worker_count; ++worker) {
    auto const range =
        enumerator.worker_partition(worker, census_options.worker_count);
    futures.push_back(pool.submit([&, range] {
      return grammar_topology_census_detail::census_worker(
          grammar, patterns, chart_options, enumerator, range,
          census_options.sankoff_verification_stride,
          census_options.retain_ordinal_score_ledger);
    }));
  }

  for (auto& future : futures) {
    auto worker = future.get();
    if (census_options.retain_ordinal_score_ledger) {
      if (worker.ordinal_begin != result.ordinal_scores.size() ||
          worker.ordinal_scores.size() != worker.scored_topology_count) {
        throw std::runtime_error(
            "grammar topology census: worker ordinal-score ledger is not "
            "gap-free");
      }
      result.ordinal_scores.insert(result.ordinal_scores.end(),
                                   worker.ordinal_scores.begin(),
                                   worker.ordinal_scores.end());
    } else if (!worker.ordinal_scores.empty()) {
      throw std::logic_error(
          "grammar topology census: unrequested ordinal-score ledger retained");
    }
    grammar_topology_census_detail::checked_accumulate(
        result.scored_topology_count, worker.scored_topology_count,
        "scored-topology count");
    grammar_topology_census_detail::checked_accumulate(
        result.sankoff_verified_topology_count,
        worker.sankoff_verified_topology_count, "Sankoff-verification count");
    grammar_topology_census_detail::checked_accumulate(
        result.reachable_internal_clade_visits,
        worker.reachable_internal_clade_visits, "reachable-clade counter");
    grammar_topology_census_detail::checked_accumulate(
        result.recomputed_internal_clade_visits,
        worker.recomputed_internal_clade_visits, "recomputed-clade counter");
    for (auto const& [score, count] : worker.score_histogram) {
      grammar_topology_census_detail::checked_accumulate(
          result.score_histogram[score], count, "histogram count");
    }
    if (worker.selected_score_histogram_by_production.size() !=
        result.selected_score_histogram_by_production.size()) {
      throw std::logic_error(
          "grammar topology census: production histogram size mismatch");
    }
    for (std::size_t pid = 0;
         pid < worker.selected_score_histogram_by_production.size(); ++pid) {
      for (auto const& [score, count] :
           worker.selected_score_histogram_by_production[pid]) {
        grammar_topology_census_detail::checked_accumulate(
            result.selected_score_histogram_by_production[pid][score], count,
            "selected-production histogram count");
      }
    }
    if (worker.optimum < result.optimum) {
      result.optimum = worker.optimum;
      result.optimal_ordinals.clear();
    }
    if (worker.optimum == result.optimum) {
      result.optimal_ordinals.insert(result.optimal_ordinals.end(),
                                     worker.optimal_ordinals.begin(),
                                     worker.optimal_ordinals.end());
    }
  }

  std::sort(result.optimal_ordinals.begin(), result.optimal_ordinals.end());
  if (result.scored_topology_count != result.expected_topology_count) {
    throw std::runtime_error(
        "grammar topology census: worker ranges did not cover exact count");
  }
  std::uint64_t histogram_total = 0;
  for (auto const& [score, count] : result.score_histogram) {
    (void)score;
    grammar_topology_census_detail::checked_accumulate(histogram_total, count,
                                                       "histogram total");
  }
  if (histogram_total != result.expected_topology_count) {
    throw std::runtime_error(
        "grammar topology census: histogram does not reconcile");
  }
  if (census_options.retain_ordinal_score_ledger) {
    if (result.ordinal_scores.size() != result.expected_topology_count) {
      throw std::runtime_error(
          "grammar topology census: ordinal-score ledger does not reconcile");
    }
    std::map<std::uint64_t, std::uint64_t> ledger_histogram;
    for (auto score : result.ordinal_scores) {
      grammar_topology_census_detail::checked_accumulate(
          ledger_histogram[score], 1, "ordinal-score ledger histogram");
    }
    if (ledger_histogram != result.score_histogram) {
      throw std::runtime_error(
          "grammar topology census: ordinal-score ledger histogram mismatch");
    }
  } else if (!result.ordinal_scores.empty()) {
    throw std::logic_error(
        "grammar topology census: unrequested ordinal-score ledger published");
  }
  if (result.optimum >= multisite_score_inf ||
      result.optimal_ordinals.empty()) {
    throw std::runtime_error(
        "grammar topology census: no finite minimum topology");
  }
  return result;
}

}  // namespace larch

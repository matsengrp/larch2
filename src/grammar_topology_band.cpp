#include <larch/grammar_topology_band.hpp>

#include <larch/chart_spr_semantic_report.hpp>
#include <larch/sha256.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

namespace larch {

std::string direct_kary_selection_sha256(
    std::vector<std::string> production_keys) {
  std::sort(production_keys.begin(), production_keys.end());
  if (std::adjacent_find(production_keys.begin(), production_keys.end()) !=
      production_keys.end()) {
    throw std::runtime_error(
        "grammar topology band: duplicate stable production key");
  }
  sha256 digest;
  digest.update("larch.direct-kary-topology.v1\n");
  for (auto const& key : production_keys) {
    digest.update(std::to_string(key.size()));
    digest.update(":");
    digest.update(key);
    digest.update("\n");
  }
  return digest.hex_digest();
}

grammar_topology_band_result select_grammar_topology_band(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& chart_options,
    grammar_topology_band_options const& options) {
  if (options.worker_count == 0) {
    throw std::runtime_error(
        "grammar topology band: worker count must be positive");
  }
  if (options.minimum_score > options.maximum_score ||
      options.maximum_score > static_cast<std::uint64_t>(
                                  std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error(
        "grammar topology band: invalid inclusive score interval");
  }

  grammar_topology_census_options census_options;
  census_options.worker_count = options.worker_count;
  census_options.sankoff_verification_stride =
      options.sankoff_verification_stride;
  census_options.retain_ordinal_score_ledger = true;

  auto census = census_grammar_topologies(grammar, patterns, chart_options,
                                          census_options);
  return project_grammar_topology_band(
      grammar, patterns, chart_options, std::move(census),
      options.minimum_score, options.maximum_score, options.worker_count);
}

grammar_topology_band_result project_grammar_topology_band(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& chart_options,
    grammar_topology_census_result census, std::uint64_t minimum_score,
    std::uint64_t maximum_score, std::size_t worker_count) {
  if (worker_count == 0) {
    throw std::runtime_error(
        "grammar topology band: worker count must be positive");
  }
  if (minimum_score > maximum_score ||
      maximum_score > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max())) {
    throw std::runtime_error(
        "grammar topology band: invalid inclusive score interval");
  }

  grammar_topology_band_result result;
  result.minimum_score = minimum_score;
  result.maximum_score = maximum_score;
  result.worker_count = worker_count;
  result.census = std::move(census);
  grammar_topology_enumerator enumerator(grammar);
  if (result.census.expected_topology_count != enumerator.topology_count() ||
      result.census.scored_topology_count !=
          result.census.expected_topology_count ||
      result.census.ordinal_scores.size() !=
          result.census.expected_topology_count) {
    throw std::logic_error(
        "grammar topology band: census count/ledger mismatch");
  }
  std::map<std::uint64_t, std::uint64_t> reconstructed_histogram;
  std::uint64_t reconstructed_optimum = multisite_score_inf;
  std::vector<std::uint64_t> reconstructed_optimal_ordinals;
  for (std::size_t ordinal = 0;
       ordinal < result.census.ordinal_scores.size(); ++ordinal) {
    auto score = result.census.ordinal_scores[ordinal];
    if (score >= multisite_score_inf) {
      throw std::logic_error(
          "grammar topology band: census contains non-finite score");
    }
    ++reconstructed_histogram[score];
    if (score < reconstructed_optimum) {
      reconstructed_optimum = score;
      reconstructed_optimal_ordinals.clear();
    }
    if (score == reconstructed_optimum) {
      reconstructed_optimal_ordinals.push_back(ordinal);
    }
  }
  if (result.census.score_histogram != reconstructed_histogram ||
      result.census.optimum != reconstructed_optimum ||
      result.census.optimal_ordinals != reconstructed_optimal_ordinals) {
    throw std::logic_error(
        "grammar topology band: census ledger summary mismatch");
  }

  std::uint64_t expected_selected_count = 0;
  for (auto const& [score, count] : result.census.score_histogram) {
    if (score < minimum_score || score > maximum_score) {
      continue;
    }
    grammar_topology_census_detail::checked_accumulate(
        expected_selected_count, count, "score-band selected count");
  }
  if (expected_selected_count == 0) {
    throw std::runtime_error(
        "grammar topology band: score interval selects no topology");
  }
  if (expected_selected_count > std::numeric_limits<std::size_t>::max()) {
    throw std::length_error(
        "grammar topology band: selected rows exceed size_t");
  }
  result.selected.reserve(static_cast<std::size_t>(expected_selected_count));

  for (std::uint64_t ordinal = 0;
       ordinal < result.census.expected_topology_count; ++ordinal) {
    auto const score = result.census.ordinal_scores[ordinal];
    if (score < minimum_score || score > maximum_score) {
      continue;
    }
    auto topology = enumerator.topology_at(ordinal);
    auto const sankoff =
        score_selected_topology(grammar, patterns, topology, chart_options);
    if (sankoff != score) {
      throw std::runtime_error(
          "grammar topology band: selected generalized-Fitch/Sankoff "
          "mismatch at ordinal " +
          std::to_string(ordinal));
    }

    grammar_topology_band_row row;
    row.grammar_ordinal = ordinal;
    row.absolute_score = score;
    row.selected_grammar_sankoff_score = sankoff;
    for (std::size_t pid = 0; pid < topology.used_production.size(); ++pid) {
      if (!topology.used_production[pid]) continue;
      row.selected_productions.push_back(static_cast<production_id>(pid));
      row.selected_production_keys.push_back(
          chart_spr_canonical_production_sample_key(
              grammar, static_cast<production_id>(pid)));
    }
    std::sort(row.selected_production_keys.begin(),
              row.selected_production_keys.end());
    if (row.selected_productions.empty() ||
        row.selected_production_keys.size() !=
            row.selected_productions.size() ||
        std::adjacent_find(row.selected_production_keys.begin(),
                           row.selected_production_keys.end()) !=
            row.selected_production_keys.end()) {
      throw std::runtime_error(
          "grammar topology band: invalid selected production coordinates");
    }
    result.selected.push_back(std::move(row));
  }

  if (result.selected.size() != expected_selected_count) {
    throw std::runtime_error(
        "grammar topology band: selected rows do not reconcile with census "
        "buckets");
  }
  return result;
}

}  // namespace larch

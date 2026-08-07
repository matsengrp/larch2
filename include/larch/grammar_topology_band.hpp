#pragma once

#include <larch/grammar_topology_census.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace larch {

struct grammar_topology_band_options {
  std::uint64_t minimum_score = 0;
  std::uint64_t maximum_score = 0;
  std::size_t worker_count = 1;
  std::uint64_t sankoff_verification_stride = 0;
};

struct grammar_topology_band_row {
  std::uint64_t grammar_ordinal = 0;
  std::uint64_t absolute_score = 0;
  std::uint64_t selected_grammar_sankoff_score = 0;
  std::vector<production_id> selected_productions;
  std::vector<std::string> selected_production_keys;

  bool operator==(grammar_topology_band_row const&) const = default;
};

struct grammar_topology_band_result {
  std::uint64_t minimum_score = 0;
  std::uint64_t maximum_score = 0;
  std::size_t worker_count = 1;
  grammar_topology_census_result census;
  std::vector<grammar_topology_band_row> selected;
};

// Run one exact census while retaining its compact ordinal-score ledger, then
// unrank only the requested inclusive score interval. Selected rows remain in
// global ordinal order and include both immediate production IDs and stable
// sample-key coordinates.
[[nodiscard]] grammar_topology_band_result select_grammar_topology_band(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& chart_options,
    grammar_topology_band_options const& options);

// Project a score interval from an already completed census.  This is the
// single-pass path used by the production CLI after it has reconciled the full
// ledger with the replay manifest.  The census must contain its complete
// ordinal-score ledger.
[[nodiscard]] grammar_topology_band_result project_grammar_topology_band(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options const& chart_options,
    grammar_topology_census_result census, std::uint64_t minimum_score,
    std::uint64_t maximum_score, std::size_t worker_count);

// Historical larch.direct-kary-topology.v1 witness over sorted stable
// production keys. This is deliberately not neutral canonical-tree identity.
[[nodiscard]] std::string direct_kary_selection_sha256(
    std::vector<std::string> production_keys);

}  // namespace larch

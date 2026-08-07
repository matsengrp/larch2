#pragma once

#include <larch/parsimony_chart.hpp>
#include <larch/rooted_topology.hpp>
#include <larch/site_patterns.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace larch {

// One compressed DNA pattern for the independent canonical-tree oracle.
// Each leaf observation is a nonempty four-bit A/C/G/T mask, so ambiguity is
// represented directly rather than silently coerced to one state.
struct tree_sankoff_pattern {
  std::vector<std::uint8_t> state_mask_by_taxon;
  std::uint64_t weight = 0;
  std::array<std::uint64_t, nuc_state_count> reference_state_counts{};
};

// Stateless unit-cost four-state Sankoff over child adjacency in TREE.
// TAXON_LABELS defines pattern-column order. The optional constant accounts
// for invariant sites omitted from PATTERNS; it is used only when the fixed
// reference/root edge is scored.
[[nodiscard]] std::uint64_t score_rooted_tree_sankoff(
    topology::rooted_tree const& tree,
    std::span<std::string const> taxon_labels,
    std::span<tree_sankoff_pattern const> patterns,
    bool score_reference_edge,
    std::uint64_t skipped_invariant_reference_edge_offset = 0);

// Adapter from larch's exact compressed site-pattern representation. It
// converts exact states to singleton masks and carries the separately tracked
// skipped-invariant reference-edge constant.
[[nodiscard]] std::uint64_t score_rooted_tree_sankoff(
    topology::rooted_tree const& tree,
    std::span<std::string const> taxon_labels,
    site_pattern_set const& patterns,
    chart_options const& options = {});

}  // namespace larch

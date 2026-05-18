#pragma once

#include <cstdint>
#include <limits>

namespace larch {

// WRIC / collapsed-clade grammar sidecar invariants
// --------------------------------------------------
// Phase 0 intentionally does not implement the grammar builder yet; this
// header records the invariants that the Phase 1 sidecar must preserve.
//
// * build_clade_offsets(d) should be called before grammar construction for
//   fast get_clades(d, node_idx) access.  A builder may repair missing offsets,
//   but code paths that construct/modify DAGs should treat offsets as stale
//   until rebuilt.
// * Do not use larch::no_idx (std::size_t(-1)) as a sentinel for grammar IDs.
//   Grammar IDs are intentionally narrower than DAG node/edge indices; use the
//   dedicated no_clade and no_production values below.
// * Leaf identity is sample_id, not the compact-genome string, unless the
//   caller has intentionally coerced sample IDs.
// * Empty sample IDs and conflicting duplicate sample IDs are hard errors by
//   default.
// * Grammar clade identity is the sorted set of descendant taxa.
// * Productions are keyed only by parent clade and child clades, not by
//   compact-genome labels.
// * Leaf clades have no productions; their chart entries are initialized
//   directly from observed states.
// * UA/root passthrough nodes must be suppressed in the collapsed grammar to
//   avoid self-productions on the full-taxon clade.  Root-edge scoring is
//   handled as an optional chart boundary condition instead.
// * For binary phylogenetic trees, internal productions should normally have
//   arity 2; only UA/root self-passthroughs are suppressed.  Non-UA unary and
//   other non-binary structure is a hard error until deliberately supported.

using taxon_id = std::uint32_t;
using clade_id = std::uint32_t;
using production_id = std::uint32_t;

inline constexpr clade_id no_clade = std::numeric_limits<clade_id>::max();
inline constexpr production_id no_production =
    std::numeric_limits<production_id>::max();

}  // namespace larch

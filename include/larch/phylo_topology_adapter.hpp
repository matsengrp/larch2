#pragma once

#include <larch/phylo_dag.hpp>
#include <larch/rooted_topology.hpp>

namespace larch::topology {

// Convert one planted larch phylo tree to the TI-1 topology-only value type.
// The synthetic UA and its incoming stem convention are omitted. The input
// must be a tree: UA has exactly one child and every reachable biological node
// has exactly one parent except that child.
[[nodiscard]] rooted_tree rooted_topology_from_phylo_tree(phylo_dag& tree);

// Build the inverse planted representation: one synthetic UA with exactly one
// biological-root child. No sequence or ancestral-state meaning is assigned.
[[nodiscard]] phylo_dag phylo_tree_from_rooted_topology(rooted_tree const& tree);

}  // namespace larch::topology

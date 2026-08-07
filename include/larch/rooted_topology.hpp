#pragma once

#include <span>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace larch::topology {

// A topology-only rooted phylogenetic tree. Child order is never semantic;
// canonical_tree_bytes() sorts children recursively before encoding them.
struct rooted_tree {
  std::string label;
  std::vector<rooted_tree> children;

  [[nodiscard]] bool is_leaf() const noexcept { return children.empty(); }
  bool operator==(rooted_tree const&) const = default;
};

[[nodiscard]] rooted_tree leaf(std::string label);
[[nodiscard]] rooted_tree internal(std::vector<rooted_tree> children);

// Throws std::invalid_argument for invalid-UTF-8/duplicate labels, empty or
// unary internals, or a node that has both a label and children. The frozen v1
// grammar permits an empty UTF-8 label (L0:), though production phylo adapters
// still require nonempty sample IDs.
void validate(rooted_tree const& tree);

[[nodiscard]] std::vector<std::string> taxa(rooted_tree const& tree);
[[nodiscard]] bool is_binary(rooted_tree const& tree) noexcept;

// TI-0 rooted-labelled-length-grammar-v1, without branch lengths or UA data.
[[nodiscard]] std::string canonical_tree_bytes(rooted_tree const& tree);
[[nodiscard]] rooted_tree parse_canonical_tree(std::string_view bytes);
[[nodiscard]] std::string topology_sha256(rooted_tree const& tree);

// Canonical, sorted support bytes for witness fields. This is deliberately not
// topology identity: it records only the leaf set below a structural site.
[[nodiscard]] std::string taxon_support_bytes(rooted_tree const& tree);

// Complete unordered rooted tree universes. The binary implementation uses
// canonical taxon-set bipartitions. The hard implementation contracts every
// subset of non-root internal edges of the binary universe, then deduplicates.
[[nodiscard]] std::vector<rooted_tree> enumerate_rooted_binary(
    std::span<std::string const> labels);
[[nodiscard]] std::vector<rooted_tree> enumerate_rooted_hard(
    std::span<std::string const> labels);

// Exact bounded Sankoff scorer used by TI-1 calibration. Leaf observations
// must contain exactly the tree's labels and one symbol from ALPHABET. With
// score_root_edge=true, the chosen root state also pays unit cost when it
// differs from reference_state.
[[nodiscard]] std::uint64_t unit_cost_sankoff_score(
    rooted_tree const& tree,
    std::map<std::string, char> const& leaf_observations,
    std::string_view alphabet,
    char reference_state,
    bool score_root_edge = true);

}  // namespace larch::topology

#pragma once

#include <compare>
#include <span>
#include <string>
#include <vector>

#include <larch/rooted_topology.hpp>

namespace larch::topology {

enum class rspr_policy {
  binary_edge_subdivision,
  hard_symmetric_edge_or_vertex,
};

enum class source_parent_fate {
  retained,
  suppressed,
  root_suppressed,
};

enum class attachment_kind {
  edge,
  root_stem,
  internal_vertex,
};

[[nodiscard]] std::string_view name(source_parent_fate value) noexcept;
[[nodiscard]] std::string_view name(attachment_kind value) noexcept;

struct rspr_witness {
  std::string source;
  std::string destination;
  std::string moved_support;
  std::string source_parent_support;
  source_parent_fate source_fate{};
  attachment_kind attachment{};
  // "root-stem" for root-stem attachment; taxon-support bytes otherwise.
  std::string destination_support;

  bool operator==(rspr_witness const&) const = default;
  auto operator<=>(rspr_witness const&) const = default;
};

struct rspr_edge {
  std::string low;
  std::string high;
  std::vector<rspr_witness> directed_witnesses;

  bool operator==(rspr_edge const&) const = default;
};

[[nodiscard]] std::string witness_semantic_bytes(rspr_witness const& witness);
[[nodiscard]] std::string witness_sha256(rspr_witness const& witness);

// Exhaustive structural moves from one source. Canonical self outcomes are
// discarded; distinct semantic witnesses are retained and sorted.
[[nodiscard]] std::vector<rspr_witness> enumerate_rooted_rspr(
    rooted_tree const& source, rspr_policy policy);

// Project all in-universe directed witnesses to canonical simple edges. Throws
// if a generated endpoint is outside the supplied complete universe.
[[nodiscard]] std::vector<rspr_edge> build_rooted_rspr_graph(
    std::span<rooted_tree const> universe, rspr_policy policy);

// Exact gate used by callers before interpreting an undirected graph.
[[nodiscard]] bool has_reverse_witnesses(std::span<rspr_edge const> edges);

}  // namespace larch::topology

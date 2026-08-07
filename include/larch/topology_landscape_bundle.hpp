#pragma once

#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace larch::topology_landscape {

struct problem {
  std::string code;
  std::string file;
  std::size_t line{};
  std::string detail;
};

struct completeness_summary {
  std::string name;
  std::string state;
  std::string scope_ref;
  std::optional<std::uint64_t> denominator;
  std::uint64_t observed_numerator{};
};

struct validation {
  std::vector<problem> problems;
  std::string landscape_semantics_hash;
  std::string analysis_policy_hash;
  std::string search_run_id;
  std::string artifact_provenance_id;
  std::size_t topology_count{};
  std::size_t simple_edge_count{};
  std::size_t move_witness_count{};
  std::vector<completeness_summary> completeness;

  [[nodiscard]] explicit operator bool() const noexcept {
    return problems.empty();
  }
};

[[nodiscard]] validation validate_bundle(std::filesystem::path const& directory);

// Validate the larch-owned, grammar-relative score-band handoff consumed by
// the neutral-v2 renderer.  For this result, landscape_semantics_hash and
// artifact_provenance_id carry the raw semantics/provenance identities;
// topology_count is the selected-topology row count.
[[nodiscard]] validation validate_score_band_raw(
    std::filesystem::path const& directory);

// Validate INPUT, copy its already-canonical bytes through a staging directory,
// validate the staged copy, and publish OUTPUT without ever replacing an
// existing path. Publication is atomic where the filesystem supports
// no-replace directory rename; otherwise OUTPUT is exclusively reserved and
// readers reject its transient incomplete member set. This is the TI-0
// byte-preserving writer/replay primitive.
[[nodiscard]] validation rewrite_canonical_bundle(
    std::filesystem::path const& input,
    std::filesystem::path const& output);

[[nodiscard]] std::string topology_digest(std::string_view canonical_bytes);

}

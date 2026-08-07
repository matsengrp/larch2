#pragma once

#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <larch/exhaustive_rooted_rspr.hpp>

namespace larch::topology {

// The exact common-prune signature for one non-root cut. cut_path is expressed
// in canonical child order, so it is stable under input child permutations.
struct rooted_rspr_cut_signature {
  std::string cut_path;
  std::string moved;
  std::string reduced_remainder;
  source_parent_fate source_fate{};

  bool operator==(rooted_rspr_cut_signature const&) const = default;
};

// Fingerprints select candidate buckets only. Equality of cut signatures is
// always decided by moved and reduced_remainder byte strings.
struct rooted_rspr_signature_fingerprint {
  std::string moved;
  std::string reduced_remainder;

  bool operator==(rooted_rspr_signature_fingerprint const&) const = default;
  auto operator<=>(rooted_rspr_signature_fingerprint const&) const = default;
};

using rooted_rspr_candidate_fingerprint_function =
    rooted_rspr_signature_fingerprint (*)(std::string_view moved,
                                          std::string_view reduced_remainder);

[[nodiscard]] rooted_rspr_signature_fingerprint
rooted_rspr_sha256_candidate_fingerprint(
    std::string_view moved, std::string_view reduced_remainder);

struct rooted_rspr_pair_index_options {
  rooted_rspr_candidate_fingerprint_function candidate_fingerprint =
      &rooted_rspr_sha256_candidate_fingerprint;
};

struct rooted_rspr_endpoint_pair {
  // TI-0 topology_sha256() identities in ascending byte order.
  std::string low;
  std::string high;

  bool operator==(rooted_rspr_endpoint_pair const&) const = default;
  auto operator<=>(rooted_rspr_endpoint_pair const&) const = default;
};

struct rooted_rspr_pair_index_statistics {
  std::size_t tree_count = 0;
  std::size_t cut_record_count = 0;
  std::size_t candidate_bucket_count = 0;
  std::size_t exact_signature_class_count = 0;
  std::size_t fingerprint_collision_bucket_count = 0;
  std::size_t largest_exact_signature_class_size = 0;

  bool operator==(rooted_rspr_pair_index_statistics const&) const = default;
};

struct rooted_rspr_pair_index_result {
  std::vector<rooted_rspr_endpoint_pair> endpoint_pairs;
  rooted_rspr_pair_index_statistics statistics;

  bool operator==(rooted_rspr_pair_index_result const&) const = default;
};

// Enumerates every non-root cut after canonicalizing child order. This exact,
// byte-retaining view is intended for bounded tests and proof replay.
[[nodiscard]] std::vector<rooted_rspr_cut_signature>
enumerate_rooted_rspr_cut_signatures(rooted_tree const& source);

// Complete hard-rSPR common-prune index over a finite set of rooted labelled
// trees. Inputs must have one common taxon set and unique canonical topology
// bytes. Output contains sorted unique topology-hash endpoint pairs.
[[nodiscard]] rooted_rspr_pair_index_result build_rooted_rspr_pair_index(
    std::span<rooted_tree const> universe,
    rooted_rspr_pair_index_options options = {});

}  // namespace larch::topology

#include <larch/exhaustive_rooted_rspr.hpp>
#include <larch/rooted_rspr_pair_index.hpp>
#include <larch/rooted_topology.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace larch::topology;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> labels(std::size_t count) {
  std::vector<std::string> result;
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(std::string(1, static_cast<char>('A' + index)));
  }
  return result;
}

std::vector<rooted_rspr_endpoint_pair> endpoint_pairs_from_exhaustive(
    std::vector<rspr_edge> const& graph) {
  std::vector<rooted_rspr_endpoint_pair> result;
  result.reserve(graph.size());
  for (auto const& edge : graph) {
    auto first = topology_sha256(parse_canonical_tree(edge.low));
    auto second = topology_sha256(parse_canonical_tree(edge.high));
    if (second < first) std::swap(first, second);
    result.push_back({std::move(first), std::move(second)});
  }
  std::ranges::sort(result);
  result.erase(std::ranges::unique(result).begin(), result.end());
  return result;
}

void require_sorted_unique(std::vector<rooted_rspr_endpoint_pair> const& pairs) {
  require(std::ranges::is_sorted(pairs),
          "pair index endpoint pairs are not sorted");
  require(std::ranges::adjacent_find(pairs) == pairs.end(),
          "pair index endpoint pairs are not unique");
  require(std::ranges::all_of(pairs, [](auto const& pair) {
            return pair.low < pair.high;
          }),
          "pair index endpoint hashes are not strictly ordered");
}

void parity_test() {
  auto binary = enumerate_rooted_binary(labels(6));
  auto binary_expected = endpoint_pairs_from_exhaustive(build_rooted_rspr_graph(
      binary, rspr_policy::binary_edge_subdivision));
  auto binary_actual = build_rooted_rspr_pair_index(binary);
  require(binary_actual.endpoint_pairs == binary_expected,
          "binary common-prune index differs from the exhaustive rSPR graph");
  require(binary_actual.endpoint_pairs.size() == 22320,
          "binary common-prune index edge count is not 22320");
  require(binary_actual.statistics.cut_record_count == 9450,
          "binary common-prune index did not enumerate every non-root cut");
  require(binary_actual.statistics.fingerprint_collision_bucket_count == 0,
          "default SHA-256 index reported a fingerprint collision");
  require_sorted_unique(binary_actual.endpoint_pairs);

  auto hard = enumerate_rooted_hard(labels(4));
  auto hard_expected = endpoint_pairs_from_exhaustive(build_rooted_rspr_graph(
      hard, rspr_policy::hard_symmetric_edge_or_vertex));
  auto hard_actual = build_rooted_rspr_pair_index(hard);
  require(hard_actual.endpoint_pairs == hard_expected,
          "hard common-prune index differs from the exhaustive rSPR graph");
  require(hard_actual.endpoint_pairs.size() == 202,
          "hard common-prune index edge count is not 202");
  require_sorted_unique(hard_actual.endpoint_pairs);
}

rooted_rspr_signature_fingerprint colliding_fingerprint(
    std::string_view, std::string_view) {
  return {"forced", "collision"};
}

void collision_test() {
  auto universe = enumerate_rooted_hard(labels(4));
  auto expected = build_rooted_rspr_pair_index(universe);
  auto actual = build_rooted_rspr_pair_index(
      universe, {.candidate_fingerprint = &colliding_fingerprint});
  require(actual.endpoint_pairs == expected.endpoint_pairs,
          "forced fingerprint collision changed the exact endpoint relation");
  require(actual.statistics.candidate_bucket_count == 1,
          "forced fingerprint collision did not form one candidate bucket");
  require(actual.statistics.fingerprint_collision_bucket_count == 1,
          "forced fingerprint collision was not resolved into exact classes");
  require(actual.statistics.exact_signature_class_count > 1,
          "forced fingerprint collision skipped full-byte resolution");
}

void reverse_children(rooted_tree& tree) {
  std::ranges::reverse(tree.children);
  for (auto& child : tree.children) reverse_children(child);
}

void determinism_test() {
  auto universe = enumerate_rooted_hard(labels(4));
  auto expected = build_rooted_rspr_pair_index(universe);

  auto permuted = universe;
  std::ranges::reverse(permuted);
  for (auto& tree : permuted) reverse_children(tree);
  auto actual = build_rooted_rspr_pair_index(permuted);
  require(actual == expected,
          "tree/child input permutation changed the common-prune index");

  auto signature_source =
      internal({internal({leaf("A"), leaf("C"), leaf("B")}),
                internal({leaf("E"), leaf("D")})});
  auto reordered_source = signature_source;
  reverse_children(reordered_source);
  require(enumerate_rooted_rspr_cut_signatures(signature_source) ==
              enumerate_rooted_rspr_cut_signatures(reordered_source),
          "child order changed exact canonical cut signatures");
}

void parent_fates_test() {
  auto tree = internal({internal({leaf("A"), leaf("B"), leaf("C")}),
                        internal({leaf("D"), leaf("E")})});
  auto signatures = enumerate_rooted_rspr_cut_signatures(tree);
  require(signatures.size() == 7,
          "parent-fate fixture did not enumerate all seven non-root cuts");

  std::map<source_parent_fate, std::size_t> fates;
  for (auto const& signature : signatures) {
    ++fates[signature.source_fate];
    require(!signature.cut_path.empty(), "cut signature contains the root path");
    auto moved = parse_canonical_tree(signature.moved);
    auto remainder = parse_canonical_tree(signature.reduced_remainder);
    auto moved_taxa = taxa(moved);
    auto remainder_taxa = taxa(remainder);
    std::vector<std::string> combined;
    std::ranges::set_union(moved_taxa, remainder_taxa,
                           std::back_inserter(combined));
    require(combined == labels(5),
            "detachment changed the complete labelled taxon set");
    std::vector<std::string> overlap;
    std::ranges::set_intersection(moved_taxa, remainder_taxa,
                                  std::back_inserter(overlap));
    require(overlap.empty(), "moved and remainder taxa overlap");
  }
  require(fates[source_parent_fate::retained] == 3,
          "multifurcating-parent retained branch count differs");
  require(fates[source_parent_fate::suppressed] == 2,
          "non-root binary-parent suppressed branch count differs");
  require(fates[source_parent_fate::root_suppressed] == 2,
          "binary-root suppressed branch count differs");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: rooted_rspr_pair_index_test MODE\n";
    return 2;
  }
  auto const mode = std::string_view(argv[1]);
  try {
    if (mode == "parity") parity_test();
    else if (mode == "collision") collision_test();
    else if (mode == "determinism") determinism_test();
    else if (mode == "parent_fates") parent_fates_test();
    else throw std::invalid_argument("unknown mode");
  } catch (std::exception const& error) {
    std::cerr << mode << ": " << error.what() << '\n';
    return 1;
  }
}

#include <larch/topology_landscape_bundle.hpp>

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  using namespace larch::topology_landscape;
  if (argc != 3 && argc != 4) {
    std::cerr << "usage: topology-landscape-bundle validate BUNDLE\n"
                 "       topology-landscape-bundle validate-score-band-raw RAW_BUNDLE\n"
                 "       topology-landscape-bundle rewrite-canonical INPUT OUTPUT\n";
    return 2;
  }
  validation result;
  if (std::string_view(argv[1]) == "validate" && argc == 3) {
    result = validate_bundle(argv[2]);
  } else if (std::string_view(argv[1]) == "validate-score-band-raw" &&
             argc == 3) {
    result = validate_score_band_raw(argv[2]);
  } else if (std::string_view(argv[1]) == "rewrite-canonical" && argc == 4) {
    result = rewrite_canonical_bundle(argv[2], argv[3]);
  } else {
    std::cerr << "invalid command shape\n";
    return 2;
  }
  for (auto const& item : result.problems) {
    std::cerr << item.code << '\t' << item.file << '\t' << item.line << '\t'
              << item.detail << '\n';
  }
  if (!result) return 1;
  std::cout << "landscape_semantics_hash=" << result.landscape_semantics_hash << '\n'
            << "analysis_policy_hash=" << result.analysis_policy_hash << '\n'
            << "search_run_id=" << result.search_run_id << '\n'
            << "artifact_provenance_id=" << result.artifact_provenance_id << '\n'
            << "topologies=" << result.topology_count << '\n'
            << "simple_edges=" << result.simple_edge_count << '\n'
            << "move_witnesses=" << result.move_witness_count << '\n';
  for (auto const& claim : result.completeness) {
    std::cout << "completeness." << claim.name << '=' << claim.state << ':'
              << claim.observed_numerator;
    if (claim.denominator) std::cout << '/' << *claim.denominator;
    std::cout << " scope=" << claim.scope_ref << '\n';
  }
  return 0;
}

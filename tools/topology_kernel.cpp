#include <larch/exhaustive_rooted_rspr.hpp>
#include <larch/rooted_topology.hpp>
#include <larch/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

using namespace larch::topology;

std::string hash(std::string_view value) {
  larch::sha256 digest;
  digest.update(value);
  return digest.hex_digest();
}

void write(std::filesystem::path const& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot create " + path.native());
  output << bytes;
  if (!output) throw std::runtime_error("cannot write " + path.native());
}

void publish_no_replace(std::filesystem::path const& staging,
                        std::filesystem::path const& output) {
#if defined(__linux__)
  if (::syscall(SYS_renameat2, static_cast<long>(AT_FDCWD), staging.c_str(),
                static_cast<long>(AT_FDCWD), output.c_str(),
                static_cast<unsigned int>(RENAME_NOREPLACE)) == 0) {
    return;
  }
  if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP &&
      errno != EXDEV) {
    throw std::filesystem::filesystem_error(
        "cannot publish topology kernel artifact", output,
        std::error_code(errno, std::generic_category()));
  }
#endif
  // Portable no-replace fallback: reserve OUTPUT exclusively, copy payloads,
  // and publish SUMMARY last. Readers require the exact member set and all
  // SUMMARY hashes, so a transient reservation is never a valid artifact.
  std::error_code error;
  if (!std::filesystem::create_directory(output, error)) {
    throw std::filesystem::filesystem_error(
        "topology kernel output already exists or cannot be reserved", output,
        error ? error : std::make_error_code(std::errc::file_exists));
  }
  try {
    for (auto const* name : {"trees.raw.tsv", "edges.raw.tsv",
                             "witnesses.raw.tsv", "SUMMARY.tsv"}) {
      std::filesystem::copy_file(staging / name, output / name,
                                 std::filesystem::copy_options::none);
    }
    std::filesystem::remove_all(staging);
  } catch (...) {
    std::filesystem::remove_all(output, error);
    throw;
  }
}

std::vector<std::string> labels(std::size_t count) {
  std::vector<std::string> result;
  for (std::size_t i = 0; i < count; ++i) {
    result.emplace_back(1, static_cast<char>('A' + i));
  }
  return result;
}

void generate(std::string_view scope, std::filesystem::path const& output) {
  std::vector<rooted_tree> universe;
  rspr_policy policy{};
  std::map<std::string, char> observations;
  std::string move_relation;
  if (scope == "s0_binary_6") {
    universe = enumerate_rooted_binary(labels(6));
    policy = rspr_policy::binary_edge_subdivision;
    observations = {{"A", 'A'}, {"B", 'A'}, {"C", 'A'},
                    {"D", 'C'}, {"E", 'C'}, {"F", 'C'}};
    move_relation = "rooted_common_prune_reduction_binary_rspr_v1";
  } else if (scope == "s0m_hard_4") {
    universe = enumerate_rooted_hard(labels(4));
    policy = rspr_policy::hard_symmetric_edge_or_vertex;
    observations = {{"A", 'A'}, {"B", 'A'}, {"C", 'C'}, {"D", 'C'}};
    move_relation = "rooted_common_prune_reduction_hard_rspr_v1";
  } else {
    throw std::invalid_argument("scope must be s0_binary_6 or s0m_hard_4");
  }
  auto graph = build_rooted_rspr_graph(universe, policy);
  if (!has_reverse_witnesses(graph)) {
    throw std::runtime_error("exact graph has an edge without reverse witness");
  }

  std::vector<std::string> tree_rows;
  for (auto const& tree : universe) {
    auto bytes = canonical_tree_bytes(tree);
    tree_rows.push_back(bytes + "\t" + topology_sha256(tree) + "\t" +
                        std::to_string(unit_cost_sankoff_score(
                            tree, observations, "ACGT", 'A', true)));
  }
  std::ranges::sort(tree_rows);
  std::string tree_file = "canonical_bytes\ttopology_sha256\tabsolute_score\n";
  for (auto const& row : tree_rows) tree_file += row + "\n";

  std::string edge_file = "topology_low\ttopology_high\n";
  std::vector<std::string> witness_rows;
  for (auto const& edge : graph) {
    edge_file += edge.low + "\t" + edge.high + "\n";
    for (auto const& witness : edge.directed_witnesses) {
      witness_rows.push_back(
          witness.source + "\t" + witness.destination + "\t" +
          witness.moved_support + "\t" + witness.source_parent_support +
          "\t" + std::string(name(witness.source_fate)) + "\t" +
          std::string(name(witness.attachment)) + "\t" +
          witness.destination_support + "\t" + witness_sha256(witness));
    }
  }
  std::ranges::sort(witness_rows);
  std::string witness_file =
      "source\tdestination\tmoved_support\tsource_parent_support\t"
      "source_fate\tattachment\tdestination_support\twitness_sha256\n";
  for (auto const& row : witness_rows) witness_file += row + "\n";

  if (!std::filesystem::create_directory(output)) {
    throw std::runtime_error("cannot create topology kernel staging directory");
  }
  write(output / "trees.raw.tsv", tree_file);
  write(output / "edges.raw.tsv", edge_file);
  write(output / "witnesses.raw.tsv", witness_file);
  std::string summary =
      "key\tvalue\n"
      "edge_count\t" + std::to_string(graph.size()) + "\n" +
      "edges_sha256\t" + hash(edge_file) + "\n" +
      "move_relation\t" + move_relation + "\n" +
      "move_symmetry\tintrinsic_bidirectional\n" +
      "root_policy\trooted_no_synthetic_ua\n" +
      "scope\t" + std::string(scope) + "\n" +
      "tree_count\t" + std::to_string(universe.size()) + "\n" +
      "trees_sha256\t" + hash(tree_file) + "\n" +
      "witness_count\t" + std::to_string(witness_rows.size()) + "\n" +
      "witnesses_sha256\t" + hash(witness_file) + "\n" +
      "ua_scoring\tfixed_reference_root_boundary_unit_cost\n";
  write(output / "SUMMARY.tsv", summary);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 4 || std::string_view(argv[1]) != "generate") {
      std::cerr << "usage: topology-kernel generate SCOPE OUTPUT_DIRECTORY\n";
      return 2;
    }
    auto output = std::filesystem::path(argv[3]);
    static std::atomic<std::uint64_t> serial{0};
    auto staging = output;
    staging += ".staging-" + std::to_string(++serial);
#if defined(__linux__)
    staging += "-" + std::to_string(static_cast<long long>(::getpid()));
#endif
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
    try {
      generate(argv[2], staging);
      publish_no_replace(staging, output);
    } catch (...) {
      std::filesystem::remove_all(staging, ignored);
      throw;
    }
  } catch (std::exception const& error) {
    std::cerr << "topology-kernel: " << error.what() << '\n';
    return 1;
  }
}

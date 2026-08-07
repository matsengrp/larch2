#include <larch/exhaustive_rooted_rspr.hpp>
#include <larch/rooted_topology.hpp>
#include <larch/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef LARCH_TOPOLOGY_KERNEL_CERTIFICATE
#error "LARCH_TOPOLOGY_KERNEL_CERTIFICATE must name the TI-1 certificate"
#endif

namespace {

using namespace larch::topology;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> labels(std::size_t count) {
  std::vector<std::string> result;
  for (std::size_t i = 0; i < count; ++i) {
    result.push_back(std::string(1, static_cast<char>('A' + i)));
  }
  return result;
}

std::map<std::size_t, std::size_t> degree_histogram(
    std::vector<rooted_tree> const& trees, std::vector<rspr_edge> const& edges) {
  std::map<std::string, std::size_t> degree;
  for (auto const& tree : trees) degree.emplace(canonical_tree_bytes(tree), 0);
  for (auto const& edge : edges) {
    ++degree.at(edge.low);
    ++degree.at(edge.high);
  }
  std::map<std::size_t, std::size_t> histogram;
  for (auto const& [vertex, value] : degree) {
    (void)vertex;
    ++histogram[value];
  }
  return histogram;
}

std::size_t witness_count(std::vector<rspr_edge> const& edges) {
  std::size_t count = 0;
  for (auto const& edge : edges) count += edge.directed_witnesses.size();
  return count;
}

std::map<std::size_t, std::size_t> per_direction_multiplicity(
    std::vector<rspr_edge> const& edges) {
  std::map<std::size_t, std::size_t> result;
  for (auto const& edge : edges) {
    std::size_t forward = 0;
    std::size_t reverse = 0;
    for (auto const& witness : edge.directed_witnesses) {
      if (witness.source == edge.low) ++forward;
      else ++reverse;
    }
    require(forward == reverse, "edge has unequal directional multiplicity");
    ++result[forward];
  }
  return result;
}

void require_constructive_inverses(std::vector<rspr_edge> const& edges) {
  for (auto const& edge : edges) {
    for (auto const& witness : edge.directed_witnesses) {
      auto expected_attachment =
          witness.source_fate == source_parent_fate::retained
              ? attachment_kind::internal_vertex
          : witness.source_fate == source_parent_fate::root_suppressed
              ? attachment_kind::root_stem
              : attachment_kind::edge;
      auto expected_fate =
          witness.attachment == attachment_kind::internal_vertex
              ? source_parent_fate::retained
          : witness.attachment == attachment_kind::root_stem
              ? source_parent_fate::root_suppressed
              : source_parent_fate::suppressed;
      auto inverse = std::ranges::find_if(
          edge.directed_witnesses, [&](rspr_witness const& candidate) {
            return candidate.source == witness.destination &&
                   candidate.destination == witness.source &&
                   candidate.moved_support == witness.moved_support &&
                   candidate.attachment == expected_attachment &&
                   candidate.source_fate == expected_fate;
          });
      require(inverse != edge.directed_witnesses.end(),
              "semantic witness lacks its constructed inverse");
    }
  }
}

void canonical_test() {
  auto lhs = internal({leaf("B"), internal({leaf("C"), leaf("A")})});
  auto rhs = internal({internal({leaf("A"), leaf("C")}), leaf("B")});
  auto bytes = canonical_tree_bytes(lhs);
  require(bytes == canonical_tree_bytes(rhs), "child order changed identity");
  require(parse_canonical_tree(bytes) == rhs, "canonical round trip failed");
  require(canonical_tree_bytes(parse_canonical_tree("I2[3:L0:4:L1:A]")) ==
              "I2[3:L0:4:L1:A]",
          "frozen L0 empty label stopped round-tripping");
  require(topology_sha256(lhs) == topology_sha256(rhs),
          "child order changed digest");
  bool rejected = false;
  try {
    (void)parse_canonical_tree("I2[4:L1:B4:L1:A]");
  } catch (std::invalid_argument const&) {
    rejected = true;
  }
  require(rejected, "noncanonical child order was accepted");
  std::vector<std::string> invalid_encodings{
      "I2[4:L1:A4:L1:A]", "I01[4:L1:A]",
      std::string("I2[5:L2:") + static_cast<char>(0xff) +
          static_cast<char>(0xff) + "4:L1:A]"};
  for (auto const& invalid : invalid_encodings) {
    rejected = false;
    try {
      (void)parse_canonical_tree(invalid);
    } catch (std::invalid_argument const&) {
      rejected = true;
    }
    require(rejected, "malformed canonical tree was accepted");
  }
  rejected = false;
  try {
    (void)enumerate_rooted_rspr(
        lhs, static_cast<rspr_policy>(999));
  } catch (std::invalid_argument const&) {
    rejected = true;
  }
  require(rejected, "unknown rSPR policy was accepted");
}

void binary_enumeration_test() {
  constexpr std::array expected{std::size_t{1}, std::size_t{3},
                                std::size_t{15}, std::size_t{105},
                                std::size_t{945}};
  for (std::size_t n = 2; n <= 6; ++n) {
    auto universe = enumerate_rooted_binary(labels(n));
    require(universe.size() == expected[n - 2],
            "wrong rooted binary topology count at n=" + std::to_string(n));
    for (auto const& tree : universe) {
      require(is_binary(tree), "binary enumerator emitted a polytomy");
      require(taxa(tree) == labels(n), "binary enumerator changed the taxon set");
    }
  }
}

void hard_enumeration_test() {
  constexpr std::array expected{std::size_t{1}, std::size_t{4},
                                std::size_t{26}, std::size_t{236}};
  for (std::size_t n = 2; n <= 5; ++n) {
    auto universe = enumerate_rooted_hard(labels(n));
    require(universe.size() == expected[n - 2],
            "wrong rooted hard topology count at n=" + std::to_string(n));
    for (auto const& tree : universe) {
      require(taxa(tree) == labels(n), "hard enumerator changed the taxon set");
      validate(tree);
    }
  }
  auto universe = enumerate_rooted_hard(labels(4));
  auto binary_count = static_cast<std::size_t>(
      std::ranges::count_if(universe, is_binary));
  require(binary_count == 15 && universe.size() - binary_count == 11,
          "hard four-taxon binary/nonbinary split is not 15/11");
}

void binary_rspr_test() {
  auto universe = enumerate_rooted_binary(labels(6));
  auto graph =
      build_rooted_rspr_graph(universe, rspr_policy::binary_edge_subdivision);
  require(graph.size() == 22320, "binary rSPR simple-edge count is not 22320");
  require(witness_count(graph) == 59760,
          "binary rSPR directed witness count is not 59760");
  require(has_reverse_witnesses(graph), "binary rSPR edge lacks a reverse witness");
  require_constructive_inverses(graph);
  require(degree_histogram(universe, graph) ==
              std::map<std::size_t, std::size_t>{{44, 360}, {46, 90}, {48, 180},
                                                 {50, 180}, {52, 135}},
          "binary rSPR degree histogram differs from the exact oracle");
  require(per_direction_multiplicity(graph) ==
              std::map<std::size_t, std::size_t>{{1, 18540}, {3, 3780}},
          "binary rSPR witness multiplicity histogram differs");
}

void hard_rspr_test() {
  auto universe = enumerate_rooted_hard(labels(4));
  auto graph = build_rooted_rspr_graph(
      universe, rspr_policy::hard_symmetric_edge_or_vertex);
  require(graph.size() == 202, "hard rSPR simple-edge count is not 202");
  require(witness_count(graph) == 656,
          "hard rSPR directed witness count is not 656");
  require(has_reverse_witnesses(graph), "hard rSPR edge lacks a reverse witness");
  require_constructive_inverses(graph);
  require(degree_histogram(universe, graph) ==
              std::map<std::size_t, std::size_t>{{10, 1}, {15, 12}, {16, 10},
                                                 {18, 3}},
          "hard rSPR degree histogram differs from the exact oracle");
  require(per_direction_multiplicity(graph) ==
              std::map<std::size_t, std::size_t>{{1, 136}, {2, 6}, {3, 60}},
          "hard rSPR witness multiplicity histogram differs");
}

void binary_restriction_test() {
  auto universe = enumerate_rooted_binary(labels(6));
  std::set<std::string> binary_vertices;
  for (auto const& tree : universe) binary_vertices.insert(canonical_tree_bytes(tree));
  for (auto const& tree : universe) {
    auto binary = enumerate_rooted_rspr(tree, rspr_policy::binary_edge_subdivision);
    auto hard = enumerate_rooted_rspr(
        tree, rspr_policy::hard_symmetric_edge_or_vertex);
    std::erase_if(hard, [&](rspr_witness const& witness) {
      return !binary_vertices.contains(witness.destination);
    });
    require(binary == hard,
            "binary endpoint restriction of hard rSPR differs from binary policy");
  }
}

void digest_test() {
  auto emit = [](std::vector<rooted_tree> const& universe,
                 rspr_policy policy,
                 std::array<std::string_view, 3> expected) {
    std::vector<std::string> trees;
    for (auto const& tree : universe) trees.push_back(canonical_tree_bytes(tree));
    std::ranges::sort(trees);
    auto graph = build_rooted_rspr_graph(universe, policy);
    std::string tree_ledger;
    for (auto const& tree : trees) {
      tree_ledger += std::to_string(tree.size()) + ":" + tree + "\n";
    }
    std::string edge_ledger;
    std::vector<std::string> witnesses;
    for (auto const& edge : graph) {
      edge_ledger += std::to_string(edge.low.size()) + ":" + edge.low +
                     std::to_string(edge.high.size()) + ":" + edge.high + "\n";
      for (auto const& witness : edge.directed_witnesses) {
        witnesses.push_back(witness_semantic_bytes(witness));
      }
    }
    std::ranges::sort(witnesses);
    std::string witness_ledger;
    for (auto const& witness : witnesses) {
      witness_ledger += std::to_string(witness.size()) + ":" + witness + "\n";
    }
    auto hash = [](std::string_view value) {
      larch::sha256 digest;
      digest.update(value);
      return digest.hex_digest();
    };
    std::array actual{hash(tree_ledger), hash(edge_ledger), hash(witness_ledger)};
    require(std::ranges::equal(actual, expected),
            "canonical topology/edge/witness certificate differs");
    std::cout << actual[0] << ' ' << actual[1] << ' ' << actual[2] << '\n';
  };
  emit(enumerate_rooted_binary(labels(6)),
       rspr_policy::binary_edge_subdivision,
       {"434d7408599fd7ee2011b4edec9d0cca5c5009cbc35ed28800fdb6726c1eedbb",
        "26824e306fdad49490f59b770316b9cd348db6c76c08be0db554d3b247f73c67",
        "abf62d6057554bf86797eaeb43a19d43141a0e7101867253ea3dec55eca5a61e"});
  emit(enumerate_rooted_hard(labels(4)),
       rspr_policy::hard_symmetric_edge_or_vertex,
       {"88c2bc06862c2ebe1d16371f2f28b1ee5b0d7c2b8a23919bd363b93f62655f9b",
        "ac09576346a0b10c5ee5c5cb55719be91c9f4ca5a093dbcce5ebfcef47908fce",
        "d68d584ef4166f7aab4ead890fbd44b87595a978f3679c22a290143e4d68c93e"});
}

void scoring_test() {
  auto binary = enumerate_rooted_binary(labels(6));
  std::map<std::string, char> all_a{{"A", 'A'}, {"B", 'A'}, {"C", 'A'},
                                    {"D", 'A'}, {"E", 'A'}, {"F", 'A'}};
  std::map<std::string, char> split{{"A", 'A'}, {"B", 'A'}, {"C", 'A'},
                                    {"D", 'C'}, {"E", 'C'}, {"F", 'C'}};
  std::map<std::uint64_t, std::size_t> null_histogram;
  std::map<std::uint64_t, std::size_t> split_histogram;
  for (auto const& tree : binary) {
    ++null_histogram[unit_cost_sankoff_score(tree, all_a, "ACGT", 'A')];
    ++split_histogram[unit_cost_sankoff_score(tree, split, "ACGT", 'A')];
  }
  require(null_histogram == std::map<std::uint64_t, std::size_t>{{0, 945}},
          "all-identical Sankoff histogram differs");
  require(split_histogram ==
              std::map<std::uint64_t, std::size_t>{{1, 45}, {2, 360}, {3, 540}},
          "AAACCC Sankoff histogram differs");

  auto hard = enumerate_rooted_hard(labels(4));
  std::map<std::string, char> hard_split{{"A", 'A'}, {"B", 'A'},
                                         {"C", 'C'}, {"D", 'C'}};
  std::map<std::uint64_t, std::size_t> hard_histogram;
  for (auto const& tree : hard) {
    ++hard_histogram[
        unit_cost_sankoff_score(tree, hard_split, "ACGT", 'A')];
  }
  require(hard_histogram ==
              std::map<std::uint64_t, std::size_t>{{1, 4}, {2, 22}},
          "hard AACC Sankoff histogram differs");
}

void metamorphic_test() {
  auto universe = enumerate_rooted_binary(labels(6));
  auto expected =
      build_rooted_rspr_graph(universe, rspr_policy::binary_edge_subdivision);
  auto permuted = universe;
  auto reverse_children = [&](auto&& self, rooted_tree& tree) -> void {
    std::ranges::reverse(tree.children);
    for (auto& child : tree.children) self(self, child);
  };
  for (auto& tree : permuted) reverse_children(reverse_children, tree);
  std::ranges::reverse(permuted);
  auto actual =
      build_rooted_rspr_graph(permuted, rspr_policy::binary_edge_subdivision);
  require(actual == expected,
          "source partition/traversal/child order changed exact graph");

  auto input = labels(6);
  std::ranges::reverse(input);
  auto inserted = enumerate_rooted_binary(input);
  std::vector<std::string> expected_bytes;
  std::vector<std::string> actual_bytes;
  for (auto const& tree : universe) expected_bytes.push_back(canonical_tree_bytes(tree));
  for (auto const& tree : inserted) actual_bytes.push_back(canonical_tree_bytes(tree));
  require(expected_bytes == actual_bytes,
          "taxon input order changed canonical topology universe");
}

void certificate_test() {
  std::ifstream input(LARCH_TOPOLOGY_KERNEL_CERTIFICATE, std::ios::binary);
  require(static_cast<bool>(input), "cannot open the checked-in TI-1 certificate");
  std::string bytes{std::istreambuf_iterator<char>{input},
                    std::istreambuf_iterator<char>{}};
  larch::sha256 digest;
  digest.update(bytes);
  require(digest.hex_digest() ==
              "1bea87afaf5dcb23e6f959cf799f80df16c4483019d6913edeb9e7c30cfed0f6",
          "checked-in TI-1 certificate digest differs");
  require(bytes.starts_with("scope\tmetric\tvalue\n"),
          "TI-1 certificate header differs");
  std::map<std::pair<std::string, std::string>, std::string> rows;
  std::string previous;
  std::size_t offset = bytes.find('\n') + 1;
  while (offset < bytes.size()) {
    auto end = bytes.find('\n', offset);
    require(end != std::string::npos, "TI-1 certificate lacks final LF");
    auto row = bytes.substr(offset, end - offset);
    require(previous.empty() || previous < row,
            "TI-1 certificate rows are not strictly canonical");
    previous = row;
    auto first = row.find('\t');
    auto second = row.find('\t', first + 1);
    require(first != std::string::npos && second != std::string::npos &&
                row.find('\t', second + 1) == std::string::npos,
            "TI-1 certificate row is not three-column TSV");
    auto inserted = rows.emplace(
        std::pair{row.substr(0, first), row.substr(first + 1, second - first - 1)},
        row.substr(second + 1));
    require(inserted.second, "TI-1 certificate contains a duplicate metric");
    offset = end + 1;
  }
  auto value = [&](std::string const& scope, std::string const& metric) {
    auto found = rows.find({scope, metric});
    require(found != rows.end(), "TI-1 certificate is missing " + scope + "/" +
                                     metric);
    return found->second;
  };
  require(value("s0_binary_6", "tree_count") == "945" &&
              value("s0_binary_6", "edge_count") == "22320" &&
              value("s0_binary_6", "witness_count") == "59760" &&
              value("s0_binary_6", "beta0") == "1" &&
              value("s0_binary_6", "bundle_validator_parity") ==
                  "tric_and_larch_accept",
          "TI-1 binary certificate facts differ");
  require(value("s0m_hard_4", "tree_count") == "26" &&
              value("s0m_hard_4", "edge_count") == "202" &&
              value("s0m_hard_4", "witness_count") == "656" &&
              value("s0m_hard_4", "beta0") == "1" &&
              value("synthetic_sampler_64", "false_disconnected_pairs") ==
                  "125/2016",
          "TI-1 hard/sampler certificate facts differ");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: rooted_topology_kernel_test MODE\n";
    return 2;
  }
  auto const mode = std::string_view(argv[1]);
  try {
    if (mode == "canonical") canonical_test();
    else if (mode == "binary_enumeration") binary_enumeration_test();
    else if (mode == "hard_enumeration") hard_enumeration_test();
    else if (mode == "binary_rspr") binary_rspr_test();
    else if (mode == "hard_rspr") hard_rspr_test();
    else if (mode == "digest") digest_test();
    else if (mode == "scoring") scoring_test();
    else if (mode == "metamorphic") metamorphic_test();
    else if (mode == "binary_restriction") binary_restriction_test();
    else if (mode == "certificate") certificate_test();
    else throw std::invalid_argument("unknown mode");
  } catch (std::exception const& error) {
    std::cerr << mode << ": " << error.what() << '\n';
    return 1;
  }
}

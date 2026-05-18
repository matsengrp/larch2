#include <larch/build_fasta_newick.hpp>
#include <larch/compute.hpp>

#include "test_util.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <variant>

using namespace larch;

namespace {

class temp_dir {
 public:
  explicit temp_dir(std::string_view name)
      : path_{larch::test::unique_temp_path(name, "")} {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~temp_dir() { std::filesystem::remove_all(path_); }

  [[nodiscard]] std::filesystem::path const& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void write_text(std::filesystem::path const& path, std::string_view contents) {
  std::ofstream out{path};
  out << contents;
  assert(out.good());
}

}  // namespace

int main() {
  temp_dir tmp{"build_fasta_newick_test"};
  auto const fasta_path = tmp.path() / "sequences.fa";
  auto const newick_path = tmp.path() / "tree.nwk";
  auto const reference_path = tmp.path() / "reference.txt";

  write_text(reference_path, "AAAA\n");
  write_text(fasta_path, R"(>A
AAAA
>B
AACA
>C
TTAA
)");
  write_text(newick_path, "((A,B),C);\n");

  auto dag = build_from_fasta_newick(fasta_path.string(), newick_path.string(),
                                     reference_path.string());

  validate_dag(dag, "build_fasta_newick");
  assert(is_tree(dag));
  assert(node_count(dag) == 6);  // UA + 2 Newick internal nodes + 3 leaves.
  assert(edge_count(dag) == 5);  // 4 Newick edges + UA edge.
  assert(leaf_count(dag) == 3);
  assert(get_reference_sequence(dag) == "AAAA");

  std::map<std::string, std::string> leaf_sequences;
  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            leaf_sequences.emplace(node.sample_id(),
                                   larch::test::node_sequence(dag,
                                                              node.index()));
          }
        },
        nv);
  }

  std::map<std::string, std::string> const expected_leaf_sequences{
      {"A", "AAAA"}, {"B", "AACA"}, {"C", "TTAA"}};
  assert(leaf_sequences == expected_leaf_sequences);

  auto const real_root_idx = get_non_ua_root_idx(dag);
  assert(get_child_indices(dag, real_root_idx).size() == 2);

  std::println("build_fasta_newick test passed");
}

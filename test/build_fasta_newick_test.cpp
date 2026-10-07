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
using larch::test::count_mutations;
using larch::test::node_sequence;
using larch::test::require;

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

void test_multifurcating_fitch_reconstruction(
    std::filesystem::path const& directory) {
  auto const fasta_path = directory / "polytomy.fa";
  auto const newick_path = directory / "polytomy.nwk";
  auto const reference_path = directory / "polytomy-reference.txt";

  write_text(fasta_path, R"(>reference_sequence
GGTTGG
>s1
GGTAGG
>s2
GGTAGG
>s3
GGTAGG
>s4
GGTTGG
)");
  write_text(reference_path, "GGTTGG\n");

  write_text(newick_path, "(s1,s2,s3,s4);\n");
  auto star = build_from_fasta_newick(fasta_path.string(),
                                      newick_path.string(),
                                      reference_path.string());
  auto const star_root = get_non_ua_root_idx(star);
  require(get_child_indices(star, star_root).size() == 4,
          "Fitch regression fixture must contain a four-way node");
  require(node_sequence(star, star_root) == "GGTAGG",
          "four-way node must be assigned its majority state");
  require(count_mutations(star) == 2,
          "four-way topology must have parsimony score two");

  write_text(newick_path, "((s1,s2),(s3,s4));\n");
  auto binary = build_from_fasta_newick(fasta_path.string(),
                                        newick_path.string(),
                                        reference_path.string());
  require(count_mutations(binary) == 2,
          "binary control topology must have parsimony score two");
}

// Ambiguous sample bases (IUPAC codes, e.g. N or K) must be treated as
// matching the reference — not silently coerced to A by nuc_base::from_char,
// which would fabricate a mutation and inflate the parsimony score.
void test_ambiguous_bases_are_not_mutations(
    std::filesystem::path const& directory) {
  auto const fasta_path = directory / "ambiguous.fa";
  auto const newick_path = directory / "ambiguous.nwk";
  auto const reference_path = directory / "ambiguous-reference.txt";

  // s2 has an ambiguous K (= G/T) at position 2 and an N at position 3;
  // the reference has an N at position 4.
  write_text(fasta_path, R"(>s1
ACGA
>s2
AKNA
>s3
ACGA
)");
  write_text(reference_path, "ACNA\n");
  write_text(newick_path, "(s1,s2,s3);\n");

  auto dag = build_from_fasta_newick(fasta_path.string(),
                                     newick_path.string(),
                                     reference_path.string());
  require(count_mutations(dag) == 0,
          "ambiguous bases must not fabricate mutations");
  // s2's ambiguous bases are skipped, so its compact genome stays empty
  // (= identical to the reference) instead of carrying fabricated mutations.
  auto& s2_cg = std::visit(
      [](auto node) -> compact_genome& {
        if constexpr (requires { node.cg(); }) {
          return node.cg();
        } else {
          assert(false && "not a cg node");
          std::unreachable();
        }
      },
      dag.get_node(larch::test::find_leaf_by_sample_id(dag, "s2")));
  require(s2_cg.to_string() == "<>",
          "ambiguous sample bases record no mutations");
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
                                   node_sequence(dag, node.index()));
          }
        },
        nv);
  }

  std::map<std::string, std::string> const expected_leaf_sequences{
      {"A", "AAAA"}, {"B", "AACA"}, {"C", "TTAA"}};
  assert(leaf_sequences == expected_leaf_sequences);

  auto const real_root_idx = get_non_ua_root_idx(dag);
  assert(get_child_indices(dag, real_root_idx).size() == 2);

  test_multifurcating_fitch_reconstruction(tmp.path());
  test_ambiguous_bases_are_not_mutations(tmp.path());

  std::println("build_fasta_newick test passed");
}

// load_parsimony_test.cpp — parsimony protobuf tree loading tests.
//
// Regression test for condensed-node expansion: UShER collapses identical
// samples into condensed_nodes.  Expanded leaves are siblings of the retained
// leaf and must each get their own clade index under the shared parent.
// Previously every expanded leaf reused the retained leaf's edge clade index,
// which made the DAG machinery treat them as mutually-exclusive alternatives:
// tree_count exploded and trim_inconsistent_clade_edges reported "all N
// alternatives are missing leaves (cannot trim)".

#include <larch/load_parsimony.hpp>
#include <larch/compute.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/protobuf_encode.hpp>
#include <larch/io_util.hpp>

#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
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

void write_bytes(std::filesystem::path const& path,
                 std::vector<uint8_t> const& bytes) {
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<char const*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

pars_data make_condensed_tree_pb() {
  pars_data data;
  // Two leaves: A and a condensed node representing three identical samples.
  data.newick = "(A,node_1);\n";
  // Preorder: root, A, node_1 — each list holds that node's parent-edge
  // mutations.
  data.node_mutations.push_back({});  // root (UA edge)
  pars_mut mut_a;
  mut_a.position = 1;
  mut_a.ref_nuc = 0;   // A
  mut_a.par_nuc = 0;   // A
  mut_a.mut_nuc = {1}; // C
  data.node_mutations.push_back({{mut_a}});
  pars_mut mut_n;
  mut_n.position = 4;
  mut_n.ref_nuc = 3;   // T
  mut_n.par_nuc = 3;   // T
  mut_n.mut_nuc = {2}; // G
  data.node_mutations.push_back({{mut_n}});
  pars_condensed_node cn;
  cn.node_name = "node_1";
  cn.condensed_leaves = {"s1", "s2", "s3"};
  data.condensed_nodes.push_back(std::move(cn));
  return data;
}

void test_condensed_expansion_single_topology() {
  std::println("test_condensed_expansion_single_topology");
  temp_dir tmp{"load_parsimony_test"};

  auto data = make_condensed_tree_pb();
  auto const pb_path = tmp.path() / "condensed.pb";
  write_bytes(pb_path, pb::encode(data));

  std::string const reference = "ACGT";
  auto dag = load_parsimony_tree(pb_path.string(), reference);

  validate_dag(dag, "condensed expansion");
  larch::test::require(leaf_count(dag) == 4,
                       "A plus three condensed samples = 4 leaves");
  larch::test::require(is_tree(dag), "expanded tree has exactly one topology");

  // Expanded leaves must be parallel children, one per clade — never
  // alternatives sharing a clade.
  std::size_t parent_idx = larch::test::find_leaf_by_sample_id(dag, "s1");
  std::visit(
      [&](auto leaf) {
        for (auto pe_var : leaf.get_parents()) {
          std::visit(
              [&](auto pe) {
                auto parent_idx2 =
                    std::visit([](auto p) { return p.index(); },
                               pe.get_parent());
                auto clades = get_clades(dag, parent_idx2);
                larch::test::require(clades.size() == 4,
                                     "four children in four distinct clades");
                for (auto const& clade : clades)
                  larch::test::require(clade.size() == 1,
                                       "no alternatives within a clade");
              },
              pe_var);
        }
      },
      dag.get_node(parent_idx));

  // Tree count must be exactly 1 (previously ∏ alternatives per clade).
  auto root_idx = get_root_idx(dag);
  tree_count_ops tc_ops;
  subtree_weight<tree_count_ops> tc_sw(dag, 1);
  larch::test::require(tc_sw.compute_weight_below(root_idx, tc_ops) ==
                           bigint{1},
                       "tree_count == 1");

  // Trimming must find nothing to fix (previously: unresolvable clades).
  auto trim = trim_inconsistent_clade_edges(dag);
  larch::test::require(trim.edges_removed == 0 && trim.unresolvable_clades == 0,
                       "no inconsistent clade alternatives");

  // Mutation placement: the condensed node's parent-edge mutation applies to
  // every expanded sibling.
  for (auto name : {"s1", "s2", "s3"}) {
    auto idx = larch::test::find_leaf_by_sample_id(dag, name);
    larch::test::require(larch::test::node_sequence(dag, idx) == "ACGG",
                         "condensed siblings share the node's sequence");
  }
  larch::test::require(
      larch::test::node_sequence(dag, larch::test::find_leaf_by_sample_id(dag, "A")) ==
          "CCGT",
      "leaf A keeps its own sequence");
  std::println("  PASS");
}

}  // namespace

int main() {
  std::println("load_parsimony_test");
  test_condensed_expansion_single_topology();
  std::println("load_parsimony_test all passed");
  return 0;
}

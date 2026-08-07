#include <larch/phylo_topology_adapter.hpp>
#include <larch/exhaustive_rooted_rspr.hpp>

#include <larch/compute.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/save_proto_dag.hpp>

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <unistd.h>

namespace {

using namespace larch;
using namespace larch::topology;

void require(bool condition, std::string const& message) {
  if (!condition) throw std::runtime_error(message);
}

void add_edge(phylo_dag& dag, std::size_t parent, std::size_t child,
              std::size_t clade) {
  auto edge = dag.append_edge<edge_kind::clade>();
  edge.clade_index() = clade;
  std::visit([&](auto node) { edge.set_parent(node); }, dag.get_node(parent));
  std::visit([&](auto node) { edge.set_child(node); }, dag.get_node(child));
}

phylo_dag make_tree(bool reverse_nodes, bool reverse_edges,
                    bool equal_compact_genomes) {
  phylo_dag dag;
  auto ua = dag.append_node<node_kind::ua>();
  dag.set_root(ua);
  struct item {
    std::string label;
    std::size_t index{};
  };
  std::vector<item> leaves{{"A"}, {"B"}, {"C"}, {"D"}};
  if (reverse_nodes) std::ranges::reverse(leaves);
  for (auto& item : leaves) {
    auto node = dag.append_node<node_kind::leaf>();
    node.sample_id() = item.label;
    if (!equal_compact_genomes && item.label == "B") {
      node.cg() = compact_genome{
          std::map<mutation_position, nuc_base>{{1, nuc_base::from_char('C')}}};
    }
    item.index = node.index();
  }
  auto root = dag.append_node<node_kind::inner>();
  auto left = dag.append_node<node_kind::inner>();
  auto by_label = [&](std::string const& label) {
    return std::ranges::find(leaves, label, &item::label)->index;
  };
  std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> edges{
      {ua.index(), root.index(), 0}, {root.index(), left.index(), 0},
      {root.index(), by_label("D"), 1}, {left.index(), by_label("A"), 0},
      {left.index(), by_label("B"), 1}, {left.index(), by_label("C"), 2}};
  if (reverse_edges) std::ranges::reverse(edges);
  for (auto [parent, child, clade] : edges) add_edge(dag, parent, child, clade);
  return dag;
}

}  // namespace

int main() {
  try {
    auto first = make_tree(false, false, true);
    auto reordered = make_tree(true, true, true);
    auto perturbed_observation = make_tree(true, false, false);
    auto first_topology = rooted_topology_from_phylo_tree(first);
    require(canonical_tree_bytes(first_topology) ==
                canonical_tree_bytes(rooted_topology_from_phylo_tree(reordered)),
            "node/edge insertion order changed topology identity");
    require(canonical_tree_bytes(first_topology) ==
                canonical_tree_bytes(
                    rooted_topology_from_phylo_tree(perturbed_observation)),
            "compact-genome equality changed labelled topology identity");
    require(taxa(first_topology) ==
                std::vector<std::string>{"A", "B", "C", "D"},
            "UA was not omitted or leaf labels changed");
    require(first_topology.children.size() == 2,
            "biological root did not survive planted-tree conversion");

    auto const temporary = std::filesystem::temp_directory_path() /
                           ("larch-ti1-topology-" +
                            std::to_string(static_cast<long long>(::getpid())));
    auto const first_path = temporary.native() + "-first.pb";
    auto const reordered_path = temporary.native() + "-reordered.pb";
    save_proto_dag(first, first_path);
    save_proto_dag(reordered, reordered_path);
    auto first_reload = load_proto_dag(first_path);
    auto reordered_reload = load_proto_dag(reordered_path);
    std::filesystem::remove(first_path);
    std::filesystem::remove(reordered_path);
    require(canonical_tree_bytes(rooted_topology_from_phylo_tree(first_reload)) ==
                canonical_tree_bytes(
                    rooted_topology_from_phylo_tree(reordered_reload)),
            "protobuf/node insertion order changed topology after reload");

    std::vector<std::string> six_labels{"A", "B", "C", "D", "E", "F"};
    auto complete = enumerate_rooted_binary(six_labels);
    require(complete.size() == 945, "adapter bijection source is not S0");
    for (auto const& topology : complete) {
      auto planted = phylo_tree_from_rooted_topology(topology);
      auto round_trip = rooted_topology_from_phylo_tree(planted);
      require(canonical_tree_bytes(round_trip) == canonical_tree_bytes(topology),
              "S0 abstract/planted/adapter round trip changed topology");
    }

    auto unreachable = make_tree(false, false, true);
    (void)unreachable.append_node<node_kind::inner>();
    bool rejected = false;
    try {
      (void)rooted_topology_from_phylo_tree(unreachable);
    } catch (std::invalid_argument const&) {
      rejected = true;
    }
    require(rejected, "adapter accepted unreachable active node");

    auto wrong_kind = make_tree(false, false, true);
    std::size_t leaf_parent = no_idx;
    for (auto node : wrong_kind.get_all_nodes()) {
      auto const id = std::visit([](auto value) { return value.index(); }, node);
      if (is_leaf(wrong_kind, id)) {
        leaf_parent = id;
        break;
      }
    }
    auto extra_leaf = wrong_kind.append_node<node_kind::leaf>();
    extra_leaf.sample_id() = "E";
    add_edge(wrong_kind, leaf_parent, extra_leaf.index(), 0);
    rejected = false;
    try {
      (void)rooted_topology_from_phylo_tree(wrong_kind);
    } catch (std::invalid_argument const&) {
      rejected = true;
    }
    require(rejected, "adapter accepted leaf-kind node with a child");
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

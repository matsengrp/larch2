#include <larch/phylo_topology_adapter.hpp>

#include <larch/compute.hpp>

#include <set>
#include <stdexcept>
#include <variant>

namespace larch::topology {

rooted_tree rooted_topology_from_phylo_tree(phylo_dag& dag) {
  auto const ua = get_root_idx(dag);
  if (!is_ua(dag, ua)) {
    throw std::invalid_argument("phylo topology adapter root is not UA");
  }
  auto roots = get_child_indices(dag, ua);
  if (roots.size() != 1) {
    throw std::invalid_argument("phylo topology adapter requires one UA child");
  }
  std::set<std::size_t> visited;
  auto convert = [&](auto&& self, std::size_t id, bool biological_root) -> rooted_tree {
    if (!visited.insert(id).second) {
      throw std::invalid_argument("phylo topology adapter found a cycle or reticulation");
    }
    if (is_ua(dag, id)) {
      throw std::invalid_argument("phylo topology adapter found nested UA");
    }
    auto parents = get_parent_edges(dag, id);
    if (parents.size() != 1) {
      throw std::invalid_argument("phylo topology adapter input is not a tree");
    }
    auto children = get_child_indices(dag, id);
    auto const leaf_kind = is_leaf(dag, id);
    if (leaf_kind != children.empty()) {
      throw std::invalid_argument(
          "phylo topology adapter node kind disagrees with topology");
    }
    if (children.empty()) {
      std::string sample;
      std::visit(
          [&](auto node) {
            if constexpr (requires { node.sample_id(); }) sample = node.sample_id();
          },
          dag.get_node(id));
      if (sample.empty()) {
        throw std::invalid_argument("phylo topology adapter leaf lacks sample ID");
      }
      return leaf(std::move(sample));
    }
    if (children.size() < 2) {
      throw std::invalid_argument("phylo topology adapter found unary biological node");
    }
    std::vector<rooted_tree> converted;
    for (auto child : children) converted.push_back(self(self, child, false));
    (void)biological_root;
    return internal(std::move(converted));
  };
  auto result = convert(convert, roots.front(), true);
  std::size_t active_nodes = 0;
  for (auto node : dag.get_all_nodes()) {
    (void)node;
    ++active_nodes;
  }
  if (visited.size() + 1 != active_nodes) {
    throw std::invalid_argument(
        "phylo topology adapter found unreachable active nodes");
  }
  validate(result);
  return result;
}

phylo_dag phylo_tree_from_rooted_topology(rooted_tree const& tree) {
  validate(tree);
  if (tree.is_leaf()) {
    throw std::invalid_argument(
        "planted phylo topology requires an internal biological root");
  }
  phylo_dag result;
  auto ua = result.append_node<node_kind::ua>();
  result.set_root(ua);
  auto add = [&](auto&& self, rooted_tree const& source) -> std::size_t {
    if (source.is_leaf()) {
      auto node = result.append_node<node_kind::leaf>();
      node.sample_id() = source.label;
      return node.index();
    }
    auto node = result.append_node<node_kind::inner>();
    auto const parent = node.index();
    for (std::size_t clade = 0; clade < source.children.size(); ++clade) {
      auto const child = self(self, source.children[clade]);
      auto edge = result.append_edge<edge_kind::clade>();
      edge.clade_index() = clade;
      std::visit([&](auto parent_node) { edge.set_parent(parent_node); },
                 result.get_node(parent));
      std::visit([&](auto child_node) { edge.set_child(child_node); },
                 result.get_node(child));
    }
    return parent;
  };
  auto biological_root = add(add, tree);
  auto stem = result.append_edge<edge_kind::clade>();
  stem.clade_index() = 0;
  std::visit([&](auto parent) { stem.set_parent(parent); },
             result.get_node(ua.index()));
  std::visit([&](auto child) { stem.set_child(child); },
             result.get_node(biological_root));
  return result;
}

}  // namespace larch::topology

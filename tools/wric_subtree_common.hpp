// Shared helpers for the wric subtree/context tools: minimal tree pb I/O on
// the larch phylo_dag API (load a tree DAG into a nested tnode, splice a
// candidate subtree in place of a taxon set, emit/validate/write a tree
// DAG with Fitch-assigned CGs).
#pragma once

#include <larch/load_proto_dag.hpp>
#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/clade_grammar.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <fstream>

namespace wsub {

struct tnode {
  bool leaf = false;
  std::string sid;
  std::vector<tnode> children;
  std::vector<std::string> taxa;  // sorted sample ids
};

inline std::vector<std::string> sorted_union(std::vector<std::string> a,
                                             std::vector<std::string> const& b) {
  a.insert(a.end(), b.begin(), b.end());
  std::sort(a.begin(), a.end());
  a.erase(std::unique(a.begin(), a.end()), a.end());
  return a;
}

// Load a tree pb into a tnode tree (root = UA's single child).
inline tnode load_tree(std::string const& path, std::string* reference_out,
                       std::map<std::string, larch::compact_genome>* leaf_cgs) {
  using namespace larch;
  auto dag = load_proto_dag(path);
  std::size_t ua = no_idx, root = no_idx;
  for (std::size_t i = 0; i < dag.node_count(); ++i)
    if (larch::detail::is_ua_node(dag.get_node(i))) {
      ua = i;
      break;
    }
  if (ua == no_idx) throw std::runtime_error("wsub: no UA node in " + path);
  if (reference_out) {
    std::visit(
        [&](auto n) {
          using N = std::remove_cvref_t<decltype(n)>;
          if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::ua>>)
            *reference_out = n.reference_sequence();
        },
        dag.get_node(ua));
  }
  std::size_t ua_kids = 0;
  std::visit(
      [&](auto n) {
        for (auto ev : n.get_children())
          std::visit(
              [&](auto e) {
                ++ua_kids;
                if (root == no_idx) root = larch::get_child_idx(dag, e.index());
              },
              ev);
      },
      dag.get_node(ua));
  if (ua_kids != 1 || root == no_idx)
    throw std::runtime_error("wsub: UA must have exactly one child in " + path);

  // Iterative DFS building a flat index tree, then nest it.
  struct flat {
    bool leaf = false;
    std::string sid;
    std::vector<std::size_t> kids;  // indices into flat array
  };
  std::vector<flat> out;
  std::vector<std::pair<std::size_t, std::size_t>> stack{{root, no_idx}};
  while (!stack.empty()) {
    auto [idx, parent] = stack.back();
    stack.pop_back();
    auto nv = dag.get_node(idx);
    flat node;
    if (larch::detail::is_leaf_node(nv)) {
      node.leaf = true;
      std::visit(
          [&](auto n) {
            using N = std::remove_cvref_t<decltype(n)>;
            if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::leaf>>) {
              node.sid = n.sample_id();
              if (leaf_cgs) (*leaf_cgs)[node.sid] = n.cg();
            }
          },
          nv);
    } else {
      node.leaf = false;
    }
    auto self = out.size();
    if (parent != no_idx) out[parent].kids.push_back(self);
    out.push_back(std::move(node));
    if (!out[self].leaf) {
      auto dag_kids = std::vector<std::size_t>{};
      std::visit(
          [&](auto n) {
            for (auto ev : n.get_children())
              std::visit(
                  [&](auto e) {
                    dag_kids.push_back(larch::get_child_idx(dag, e.index()));
                  },
                  ev);
          },
          nv);
      if (dag_kids.size() != 2)
        throw std::runtime_error("wsub: non-binary internal node in " + path);
      out[self].kids.clear();  // remove self-link artifact
      stack.push_back({dag_kids[1], self});
      stack.push_back({dag_kids[0], self});
    }
  }
  std::function<tnode(std::size_t)> nest = [&](std::size_t i) -> tnode {
    tnode node;
    node.leaf = out[i].leaf;
    node.sid = out[i].sid;
    if (node.leaf) {
      node.taxa = {node.sid};
      return node;
    }
    for (auto c : out[i].kids) node.children.push_back(nest(c));
    if (node.children.size() != 2)
      throw std::runtime_error("wsub: nested node without two children");
    node.taxa = sorted_union(node.children[0].taxa, node.children[1].taxa);
    return node;
  };
  return nest(0);
}

// Splice: replace the subtree whose taxa == target with cand.
inline tnode splice(tnode node, std::vector<std::string> const& target,
                    tnode const& cand) {
  if (node.leaf) return node;
  if (node.taxa == target) return cand;
  node.children[0] = splice(std::move(node.children[0]), target, cand);
  node.children[1] = splice(std::move(node.children[1]), target, cand);
  node.taxa = sorted_union(node.children[0].taxa, node.children[1].taxa);
  return node;
}

// Emit a tnode tree as a phylo_dag with UA and reference.
inline std::size_t emit_tree(larch::phylo_dag& d, tnode const& node,
                             std::map<std::string, larch::compact_genome> const& leaf_cgs) {
  using namespace larch;
  if (node.leaf) {
    auto it = leaf_cgs.find(node.sid);
    if (it == leaf_cgs.end())
      throw std::runtime_error("wsub: leaf CG missing for " + node.sid);
    auto leaf = d.append_node<node_kind::leaf>();
    leaf.sample_id() = node.sid;
    leaf.cg() = it->second;
    return leaf.index();
  }
  auto inner = d.append_node<node_kind::inner>();
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    auto child_idx = emit_tree(d, node.children[i], leaf_cgs);
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = i;
    edge.edge_weight() = 0.0f;
    std::visit([&](auto p) { edge.set_parent(p); }, d.get_node(inner.index()));
    std::visit([&](auto c) { edge.set_child(c); }, d.get_node(child_idx));
  }
  return inner.index();
}

inline void write_tree(tnode const& root, std::string const& reference,
                       std::map<std::string, larch::compact_genome> const& leaf_cgs,
                       std::string const& out_path) {
  using namespace larch;
  phylo_dag d;
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = reference;
  d.set_root(ua);
  auto root_idx = emit_tree(d, root, leaf_cgs);
  {
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = 0;
    edge.edge_weight() = 0.0f;
    std::visit([&](auto p) { edge.set_parent(p); }, d.get_node(ua.index()));
    std::visit([&](auto c) { edge.set_child(c); }, d.get_node(root_idx));
  }
  build_clade_offsets(d);
  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);
  build_clade_offsets(d);
  if (!is_tree(d)) throw std::runtime_error("wsub: emitted DAG is not a tree");
  validate_dag(d, "wsub emitted tree");
  save_proto_dag(d, out_path);
}

inline std::vector<std::string> read_ids(std::string const& path) {
  std::vector<std::string> ids;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (!line.empty()) ids.push_back(line);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace wsub

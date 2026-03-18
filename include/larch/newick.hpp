#pragma once

#include <larch/compute.hpp>

#include <cassert>
#include <optional>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

template <typename N, typename E>
void parse_newick(std::string_view source, N&& on_node, E&& on_edge) {
  struct newick_node {
    std::size_t id = {};
    std::string label;
  };
  std::stack<std::vector<newick_node>> nodes;
  std::string label;
  std::size_t node_id = 0;

  auto begin_node = [&]() { nodes.push({}); };

  auto end_label = [&]() {
    if (!nodes.empty()) {
      for (auto& i : nodes.top()) {
        on_edge(node_id, i.id);
      }
      nodes.pop();
      if (!nodes.empty()) {
        nodes.top().push_back({node_id, label});
      }
    }
    std::string branch_length;
    bool have_branch_length = false;
    for (auto it = label.rbegin(); it != label.rend(); ++it) {
      if (*it == ':') {
        have_branch_length = true;
        break;
      }
      if ((*it < '0' || *it > '9') && *it != '.') {
        break;
      }
      branch_length.insert(branch_length.begin(), *it);
    }
    if (have_branch_length) {
      label.erase(label.size() - branch_length.size() - 1);
      on_node(node_id++, label, std::stod(branch_length));
    } else {
      on_node(node_id++, label, std::optional<double>{});
    }
    label = "";
  };

  begin_node();
  for (auto it = source.begin(); it != source.end(); ++it) {
    switch (*it) {
      case '(':
        begin_node();
        break;
      case ',':
        end_label();
        begin_node();
        break;
      case ')':
      case ';':
        end_label();
        break;
      default:
        label += *it;
        break;
    }
  }
}

// Write a tree to Newick format.
// Precondition: is_tree(tree).
// Skips the UA node, starts from its single child.
inline std::string to_newick(phylo_dag& tree) {
  assert(is_tree(tree));

  auto ua = tree.get_root_as<node_kind::ua>();
  // Find the single child of the UA node (the real root).
  std::size_t real_root_idx = 0;
  for (auto edge_var : ua.get_children()) {
    std::visit(
        [&](auto edge) {
          auto cv = edge.get_child();
          std::visit([&](auto child) { real_root_idx = child.index(); }, cv);
        },
        edge_var);
    break;
  }

  // Recursive DFS via explicit stack.
  // Each entry: (node_idx, phase). phase=0 means "entering", phase=1 means
  // "done with children".
  std::string result;

  struct frame {
    std::size_t node_idx;
    std::vector<std::size_t> children;  // child node indices
    std::size_t next_child = 0;
  };

  std::vector<frame> stack;

  auto make_frame = [&](std::size_t idx) -> frame {
    frame f;
    f.node_idx = idx;
    auto nv = tree.get_node(idx);
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto cv = edge.get_child();
                  std::visit(
                      [&](auto child) { f.children.push_back(child.index()); },
                      cv);
                },
                ev);
          }
        },
        nv);
    return f;
  };

  auto get_label = [&](std::size_t idx) -> std::string {
    auto nv = tree.get_node(idx);
    return std::visit(
        [](auto node) -> std::string {
          if constexpr (requires { node.sample_id(); }) {
            if (!node.sample_id().empty()) return node.sample_id();
            if constexpr (requires { node.cg(); }) {
              return node.cg().to_string();
            }
          }
          return {};
        },
        nv);
  };

  stack.push_back(make_frame(real_root_idx));

  while (!stack.empty()) {
    auto& top = stack.back();
    if (top.children.empty() || top.next_child == top.children.size()) {
      // Leaf or done with all children: emit label, close paren if internal.
      if (!top.children.empty()) result += ')';
      result += get_label(top.node_idx);
      stack.pop_back();
      // If there's a parent frame, add comma before next sibling.
      if (!stack.empty()) {
        auto& parent = stack.back();
        if (parent.next_child < parent.children.size()) result += ',';
      }
    } else {
      // First time or still have children to process.
      if (top.next_child == 0) result += '(';
      auto child_idx = top.children[top.next_child++];
      stack.push_back(make_frame(child_idx));
    }
  }

  result += ';';
  return result;
}

}  // namespace larch

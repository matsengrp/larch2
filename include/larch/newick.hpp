#pragma once

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

}  // namespace larch

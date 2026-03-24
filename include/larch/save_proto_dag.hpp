#pragma once

#include <larch/load_proto_dag.hpp>
#include <larch/protobuf_encode.hpp>
#include <larch/compute.hpp>

#include <unordered_map>

namespace larch {

inline void save_proto_dag(phylo_dag& dag, std::string_view path,
                           std::vector<float> const& edge_scores) {
  assert(edge_scores.empty() || edge_scores.size() >= dag.edge_high_mark());
  dag_data data;
  data.reference_seq = get_reference_sequence(dag);

  // Build node_idx -> proto_id mapping (UA gets ID 0)
  std::unordered_map<std::size_t, int64_t> node_to_proto;
  auto root_idx = get_root_idx(dag);
  node_to_proto[root_idx] = 0;

  int64_t next_id = 1;
  for (auto nv : dag.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    if (idx == root_idx) continue;
    node_to_proto[idx] = next_id++;
  }

  // Create edges
  int64_t edge_id = 0;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto parent_idx =
              std::visit([](auto n) { return n.index(); }, edge.get_parent());
          auto child_idx =
              std::visit([](auto n) { return n.index(); }, edge.get_child());

          dag_edge de;
          de.edge_id = edge_id++;
          de.parent_node = node_to_proto[parent_idx];
          de.child_node = node_to_proto[child_idx];
          de.parent_clade = static_cast<int64_t>(edge.clade_index());

          auto eidx = edge.index();
          if (eidx < edge_scores.size()) de.edge_weight = edge_scores[eidx];

          for (auto& [pos, nucs] : edge.mutations()) {
            dag_mut dm;
            dm.position = static_cast<int32_t>(pos);
            dm.par_nuc = static_cast<int32_t>(nucs.first.raw());
            dm.mut_nuc = {static_cast<int32_t>(nucs.second.raw())};
            de.edge_mutations.push_back(std::move(dm));
          }

          data.edges.push_back(std::move(de));
        },
        ev);
  }

  // Create node names for all nodes (historydag requires every node in
  // node_names)
  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          dag_node_name nn;
          nn.node_id = node_to_proto[node.index()];
          if constexpr (requires { node.sample_id(); }) {
            nn.condensed_leaves = {node.sample_id()};
          }
          data.node_names.push_back(std::move(nn));
        },
        nv);
  }

  pb::encode_file(path, data);
}

inline void save_proto_dag(phylo_dag& dag, std::string_view path) {
  save_proto_dag(dag, path, {});
}

}  // namespace larch

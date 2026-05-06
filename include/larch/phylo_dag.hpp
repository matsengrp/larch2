#pragma once

#include <larch/dag.hpp>
#include <larch/compact_genome.hpp>
#include <larch/edge_mutations.hpp>

#include <string>

namespace larch {

enum class node_kind { ua, inner, leaf };
enum class edge_kind { clade };

template <>
struct annotation<node_kind::ua> {
  std::string reference_sequence;
};

template <>
struct annotation<node_kind::inner> {
  compact_genome cg;
};

template <>
struct annotation<node_kind::leaf> {
  compact_genome cg;
  std::string sample_id;
};

template <>
struct annotation<edge_kind::clade> {
  edge_mutations mutations;
  std::size_t clade_index = 0;
  float edge_weight = 0.0f;
};

template <>
struct interface<node_kind::ua> {
  auto& reference_sequence(this auto&& self) {
    return self.get_annotation().reference_sequence;
  }
};

template <>
struct interface<node_kind::inner> {
  auto& cg(this auto&& self) { return self.get_annotation().cg; }
};

template <>
struct interface<node_kind::leaf> {
  auto& cg(this auto&& self) { return self.get_annotation().cg; }
  auto& sample_id(this auto&& self) { return self.get_annotation().sample_id; }
};

template <>
struct interface<edge_kind::clade> {
  auto& mutations(this auto&& self) { return self.get_annotation().mutations; }
  auto& clade_index(this auto&& self) {
    return self.get_annotation().clade_index;
  }
  auto& edge_weight(this auto&& self) {
    return self.get_annotation().edge_weight;
  }
};

using phylo_dag = dag<node_kind, edge_kind>;

}  // namespace larch

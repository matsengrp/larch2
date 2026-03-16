#pragma once

#include <larch/dag.hpp>

#include <vector>

namespace larch {

template <typename Base, typename ExtraNodeData = empty_type,
          typename ExtraEdgeData = empty_type>
class extend_dag
    : public dag_interface<typename Base::node_enum, typename Base::edge_enum> {
  using N = typename Base::node_enum;
  using E = typename Base::edge_enum;
  using base_interface = dag_interface<N, E>;

 public:
  using typename base_interface::edge_enumerators;
  using typename base_interface::index_type;
  using typename base_interface::node_enumerators;

  using node_variant_type =
      detail::node_variant_from<extend_dag, node_enumerators>;
  using edge_variant_type =
      detail::edge_variant_from<extend_dag, edge_enumerators>;

  explicit extend_dag(Base& base) : base_(base) {}

  Base& base() { return base_; }
  Base const& base() const { return base_; }

  ExtraNodeData& node_extra(std::size_t idx) {
    if (idx >= node_extra_.size()) node_extra_.resize(idx + 1);
    return node_extra_[idx];
  }

  ExtraEdgeData& edge_extra(std::size_t idx) {
    if (idx >= edge_extra_.size()) edge_extra_.resize(idx + 1);
    return edge_extra_[idx];
  }

 private:
  Base& base_;
  std::vector<ExtraNodeData> node_extra_;
  std::vector<ExtraEdgeData> edge_extra_;

  // --- Storage primitives: delegate to base via dag_interface forwarding ---

  auto& s_node_topo(std::size_t idx) { return base_.node_topo(idx); }
  auto const& s_node_topo(std::size_t idx) const {
    return base_.node_topo(idx);
  }
  auto& s_edge_topo(std::size_t idx) { return base_.edge_topo(idx); }
  auto const& s_edge_topo(std::size_t idx) const {
    return base_.edge_topo(idx);
  }

  template <auto V>
  auto& s_node_ann(std::size_t node_idx) {
    return base_.template node_ann<V>(node_idx);
  }
  template <auto V>
  auto const& s_node_ann(std::size_t node_idx) const {
    return base_.template node_ann<V>(node_idx);
  }
  template <auto V>
  auto& s_edge_ann(std::size_t edge_idx) {
    return base_.template edge_ann<V>(edge_idx);
  }
  template <auto V>
  auto const& s_edge_ann(std::size_t edge_idx) const {
    return base_.template edge_ann<V>(edge_idx);
  }

  auto s_children_section(std::size_t node_idx) {
    return base_.children_section(node_idx);
  }
  auto s_parents_section(std::size_t node_idx) {
    return base_.parents_section(node_idx);
  }

  void s_link_child(std::size_t node_idx, std::size_t edge_idx) {
    base_.link_child(node_idx, edge_idx);
  }
  void s_unlink_child(std::size_t node_idx, std::size_t edge_idx) {
    base_.unlink_child(node_idx, edge_idx);
  }
  void s_link_parent(std::size_t node_idx, std::size_t edge_idx) {
    base_.link_parent(node_idx, edge_idx);
  }
  void s_unlink_parent(std::size_t node_idx, std::size_t edge_idx) {
    base_.unlink_parent(node_idx, edge_idx);
  }

  template <auto V>
  std::size_t s_alloc_node() {
    return base_.template alloc_node<V>();
  }
  template <auto V>
  std::size_t s_alloc_edge() {
    return base_.template alloc_edge<V>();
  }

  void s_dealloc_node(std::size_t idx, N kind) {
    base_.dealloc_node(idx, kind);
  }
  void s_dealloc_edge(std::size_t idx, E kind) {
    base_.dealloc_edge(idx, kind);
  }
  void s_dealloc_neighbors(std::size_t start, std::size_t count) {
    base_.dealloc_neighbors(start, count);
  }

  auto& s_node_topos() { return base_.node_topos(); }
  auto const& s_node_topos() const { return base_.node_topos(); }
  auto& s_edge_topos() { return base_.edge_topos(); }
  auto const& s_edge_topos() const { return base_.edge_topos(); }

  std::size_t s_root_idx() const { return base_.root_idx(); }
  void s_set_root_idx(std::size_t idx) { base_.set_root_idx(idx); }
  std::size_t s_node_high_mark() const { return base_.node_high_mark_val(); }
  std::size_t s_edge_high_mark() const { return base_.edge_high_mark_val(); }

  friend class dag_interface<N, E>;
};

}  // namespace larch

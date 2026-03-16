#pragma once

#include <larch/dag.hpp>

#include <unordered_map>
#include <vector>

namespace larch {

template <typename Base>
class overlay_dag
    : public dag_interface<typename Base::node_enum, typename Base::edge_enum> {
  using N = typename Base::node_enum;
  using E = typename Base::edge_enum;
  using base_interface = dag_interface<N, E>;

 public:
  using typename base_interface::edge_enumerators;
  using typename base_interface::index_type;
  using typename base_interface::node_enumerators;

  using node_variant_type =
      detail::node_variant_from<overlay_dag, node_enumerators>;
  using edge_variant_type =
      detail::edge_variant_from<overlay_dag, edge_enumerators>;

  explicit overlay_dag(Base& base)
      : base_(base),
        added_node_base_(base.node_high_mark()),
        added_edge_base_(base.edge_high_mark()) {}

  Base& base() { return base_; }
  Base const& base() const { return base_; }

  bool is_overlaid_node(std::size_t idx) const {
    return overlaid_nodes_.contains(idx);
  }

  bool is_overlaid_edge(std::size_t idx) const {
    return overlaid_edges_.contains(idx);
  }

  bool is_added_node(std::size_t idx) const { return idx >= added_node_base_; }

  bool is_added_edge(std::size_t idx) const { return idx >= added_edge_base_; }

  void set_overlay_node(std::size_t idx) {
    if (overlaid_nodes_.contains(idx)) return;

    auto& src = base_.node_topo(idx);
    auto& dst = overlaid_nodes_[idx];
    dst = src;

    // Copy children neighbor list into overlay_neighbors_
    if (src.children_count_ > 0) {
      auto csec = base_.children_section(idx);
      std::vector<std::size_t> children;
      for (auto e : csec) children.push_back(e);
      auto new_sec = overlay_neighbors_.move_range(std::move(children));
      dst.children_start_ = new_sec.start();
      dst.children_count_ = new_sec.size();
    }

    // Copy parents neighbor list into overlay_neighbors_
    if (src.parents_count_ > 0) {
      auto psec = base_.parents_section(idx);
      std::vector<std::size_t> parents;
      for (auto e : psec) parents.push_back(e);
      auto new_sec = overlay_neighbors_.move_range(std::move(parents));
      dst.parents_start_ = new_sec.start();
      dst.parents_count_ = new_sec.size();
    }

    // Copy annotation into overlay map
    copy_node_annotation(idx, src.kind_);
  }

  void set_overlay_edge(std::size_t idx) {
    if (overlaid_edges_.contains(idx)) return;
    auto& src = base_.edge_topo(idx);
    overlaid_edges_[idx] = src;
    copy_edge_annotation(idx, src.kind_);
  }

 private:
  Base& base_;
  std::size_t added_node_base_;
  std::size_t added_edge_base_;

  // Overlaid nodes/edges: copied-on-write from base
  std::unordered_map<std::size_t, node_topology<N>> overlaid_nodes_;
  std::unordered_map<std::size_t, edge_topology<E>> overlaid_edges_;
  chain<std::size_t> overlay_neighbors_;

  // Annotation override maps keyed by DAG index (for overlaid + added elements)
  detail::annotation_map_from<node_enumerators> overlaid_node_annotations_;
  detail::annotation_map_from<edge_enumerators> overlaid_edge_annotations_;

  // Added nodes/edges (new allocations in overlay)
  chain<node_topology<N>> added_nodes_;
  chain<edge_topology<E>> added_edges_;

  // Overlaid root
  std::size_t overlaid_root_ = no_idx;

  // --- Annotation copy helpers ---

  void copy_node_annotation(std::size_t idx, N kind) {
    detail::enum_dispatch<node_enumerators>::call(kind, [&](auto v) {
      constexpr auto V = decltype(v)::value;
      constexpr std::size_t I = node_enumerators::template index_of<V>;
      auto& src_ann = base_.template node_ann<V>(idx);
      std::get<I>(overlaid_node_annotations_)[idx] = src_ann;
      return 0;
    });
  }

  void copy_edge_annotation(std::size_t idx, E kind) {
    detail::enum_dispatch<edge_enumerators>::call(kind, [&](auto v) {
      constexpr auto V = decltype(v)::value;
      constexpr std::size_t I = edge_enumerators::template index_of<V>;
      auto& src_ann = base_.template edge_ann<V>(idx);
      std::get<I>(overlaid_edge_annotations_)[idx] = src_ann;
      return 0;
    });
  }

  // --- Storage primitives ---

  auto& s_node_topo(std::size_t idx) {
    if (auto it = overlaid_nodes_.find(idx); it != overlaid_nodes_.end())
      return it->second;
    if (is_added_node(idx)) return added_nodes_[idx - added_node_base_];
    return base_.node_topo(idx);
  }

  auto const& s_node_topo(std::size_t idx) const {
    if (auto it = overlaid_nodes_.find(idx); it != overlaid_nodes_.end())
      return it->second;
    if (is_added_node(idx)) return added_nodes_[idx - added_node_base_];
    return base_.node_topo(idx);
  }

  auto& s_edge_topo(std::size_t idx) {
    if (auto it = overlaid_edges_.find(idx); it != overlaid_edges_.end())
      return it->second;
    if (is_added_edge(idx)) return added_edges_[idx - added_edge_base_];
    return base_.edge_topo(idx);
  }

  auto const& s_edge_topo(std::size_t idx) const {
    if (auto it = overlaid_edges_.find(idx); it != overlaid_edges_.end())
      return it->second;
    if (is_added_edge(idx)) return added_edges_[idx - added_edge_base_];
    return base_.edge_topo(idx);
  }

  template <auto V>
  auto& s_node_ann(std::size_t node_idx) {
    constexpr std::size_t I = node_enumerators::template index_of<V>;
    auto& map = std::get<I>(overlaid_node_annotations_);
    if (auto it = map.find(node_idx); it != map.end()) return it->second;
    return base_.template node_ann<V>(node_idx);
  }

  template <auto V>
  auto const& s_node_ann(std::size_t node_idx) const {
    constexpr std::size_t I = node_enumerators::template index_of<V>;
    auto& map = std::get<I>(overlaid_node_annotations_);
    if (auto it = map.find(node_idx); it != map.end()) return it->second;
    return base_.template node_ann<V>(node_idx);
  }

  template <auto V>
  auto& s_edge_ann(std::size_t edge_idx) {
    constexpr std::size_t I = edge_enumerators::template index_of<V>;
    auto& map = std::get<I>(overlaid_edge_annotations_);
    if (auto it = map.find(edge_idx); it != map.end()) return it->second;
    return base_.template edge_ann<V>(edge_idx);
  }

  template <auto V>
  auto const& s_edge_ann(std::size_t edge_idx) const {
    constexpr std::size_t I = edge_enumerators::template index_of<V>;
    auto& map = std::get<I>(overlaid_edge_annotations_);
    if (auto it = map.find(edge_idx); it != map.end()) return it->second;
    return base_.template edge_ann<V>(edge_idx);
  }

  auto s_children_section(std::size_t node_idx) {
    if (overlaid_nodes_.contains(node_idx) || is_added_node(node_idx)) {
      auto& ntopo = s_node_topo(node_idx);
      return typename chain<std::size_t>::contiguous_section{
          overlay_neighbors_, ntopo.children_start_, ntopo.children_count_};
    }
    return base_.children_section(node_idx);
  }

  auto s_parents_section(std::size_t node_idx) {
    if (overlaid_nodes_.contains(node_idx) || is_added_node(node_idx)) {
      auto& ntopo = s_node_topo(node_idx);
      return typename chain<std::size_t>::contiguous_section{
          overlay_neighbors_, ntopo.parents_start_, ntopo.parents_count_};
    }
    return base_.parents_section(node_idx);
  }

  void ensure_overlaid(std::size_t node_idx) {
    if (!overlaid_nodes_.contains(node_idx) && !is_added_node(node_idx))
      set_overlay_node(node_idx);
  }

  void s_link_child(std::size_t node_idx, std::size_t edge_idx) {
    ensure_overlaid(node_idx);
    auto& ntopo = s_node_topo(node_idx);
    auto sec = typename chain<std::size_t>::contiguous_section{
        overlay_neighbors_, ntopo.children_start_, ntopo.children_count_};
    sec = sec.emplace_back(edge_idx);
    ntopo.children_start_ = sec.start();
    ntopo.children_count_ = sec.size();
  }

  void s_unlink_child(std::size_t node_idx, std::size_t edge_idx) {
    ensure_overlaid(node_idx);
    auto& ntopo = s_node_topo(node_idx);
    if (ntopo.children_count_ == 0) return;
    auto sec = typename chain<std::size_t>::contiguous_section{
        overlay_neighbors_, ntopo.children_start_, ntopo.children_count_};
    for (std::size_t i = 0; i < sec.size(); ++i) {
      if (sec[i] == edge_idx) {
        sec = sec.erase(i);
        break;
      }
    }
    ntopo.children_start_ = sec.empty() ? no_idx : sec.start();
    ntopo.children_count_ = sec.size();
  }

  void s_link_parent(std::size_t node_idx, std::size_t edge_idx) {
    ensure_overlaid(node_idx);
    auto& ntopo = s_node_topo(node_idx);
    auto sec = typename chain<std::size_t>::contiguous_section{
        overlay_neighbors_, ntopo.parents_start_, ntopo.parents_count_};
    sec = sec.emplace_back(edge_idx);
    ntopo.parents_start_ = sec.start();
    ntopo.parents_count_ = sec.size();
  }

  void s_unlink_parent(std::size_t node_idx, std::size_t edge_idx) {
    ensure_overlaid(node_idx);
    auto& ntopo = s_node_topo(node_idx);
    if (ntopo.parents_count_ == 0) return;
    auto sec = typename chain<std::size_t>::contiguous_section{
        overlay_neighbors_, ntopo.parents_start_, ntopo.parents_count_};
    for (std::size_t i = 0; i < sec.size(); ++i) {
      if (sec[i] == edge_idx) {
        sec = sec.erase(i);
        break;
      }
    }
    ntopo.parents_start_ = sec.empty() ? no_idx : sec.start();
    ntopo.parents_count_ = sec.size();
  }

  template <auto V>
  std::size_t s_alloc_node() {
    constexpr std::size_t I = node_enumerators::template index_of<V>;

    node_topology<N> topo{};
    topo.kind_ = V;
    topo.self_index_ = no_idx;
    topo.parents_start_ = no_idx;
    topo.parents_count_ = 0;
    topo.children_start_ = no_idx;
    topo.children_count_ = 0;
    topo.annotation_index_ = 0;  // not used — annotations keyed by DAG index

    auto local_idx = added_nodes_.emplace(topo);
    auto dag_idx = added_node_base_ + local_idx;
    added_nodes_[local_idx].self_index_ = dag_idx;

    // Create annotation in overlay map keyed by dag_idx
    std::get<I>(overlaid_node_annotations_)[dag_idx];  // default-construct

    return dag_idx;
  }

  template <auto V>
  std::size_t s_alloc_edge() {
    constexpr std::size_t I = edge_enumerators::template index_of<V>;

    edge_topology<E> topo{};
    topo.kind_ = V;
    topo.self_index_ = no_idx;
    topo.parent_ = no_idx;
    topo.child_ = no_idx;
    topo.annotation_index_ = 0;  // not used — annotations keyed by DAG index

    auto local_idx = added_edges_.emplace(topo);
    auto dag_idx = added_edge_base_ + local_idx;
    added_edges_[local_idx].self_index_ = dag_idx;

    std::get<I>(overlaid_edge_annotations_)[dag_idx];  // default-construct

    return dag_idx;
  }

  void s_dealloc_node(std::size_t idx, N kind) {
    detail::enum_dispatch<node_enumerators>::call(kind, [&](auto v) {
      constexpr std::size_t I =
          node_enumerators::template index_of<decltype(v)::value>;
      std::get<I>(overlaid_node_annotations_).erase(idx);
      return 0;
    });
    if (is_added_node(idx))
      added_nodes_.remove(idx - added_node_base_);
    else
      overlaid_nodes_.erase(idx);
  }

  void s_dealloc_edge(std::size_t idx, E kind) {
    detail::enum_dispatch<edge_enumerators>::call(kind, [&](auto v) {
      constexpr std::size_t I =
          edge_enumerators::template index_of<decltype(v)::value>;
      std::get<I>(overlaid_edge_annotations_).erase(idx);
      return 0;
    });
    if (is_added_edge(idx))
      added_edges_.remove(idx - added_edge_base_);
    else
      overlaid_edges_.erase(idx);
  }

  void s_dealloc_neighbors(std::size_t start, std::size_t count) {
    overlay_neighbors_.remove(start, count);
  }

  // --- Iteration support (cached vectors rebuilt on each call) ---

  std::vector<node_topology<N>> cached_node_topos_;
  std::vector<edge_topology<E>> cached_edge_topos_;

  auto& s_node_topos() {
    cached_node_topos_.clear();
    for (auto& t : base_.node_topos()) {
      if (!overlaid_nodes_.contains(t.self_index_))
        cached_node_topos_.push_back(t);
    }
    for (auto& [idx, t] : overlaid_nodes_) cached_node_topos_.push_back(t);
    for (auto& t : added_nodes_) cached_node_topos_.push_back(t);
    return cached_node_topos_;
  }

  auto& s_edge_topos() {
    cached_edge_topos_.clear();
    for (auto& t : base_.edge_topos()) {
      if (!overlaid_edges_.contains(t.self_index_))
        cached_edge_topos_.push_back(t);
    }
    for (auto& [idx, t] : overlaid_edges_) cached_edge_topos_.push_back(t);
    for (auto& t : added_edges_) cached_edge_topos_.push_back(t);
    return cached_edge_topos_;
  }

  std::size_t s_root_idx() const {
    return overlaid_root_ != no_idx ? overlaid_root_ : base_.root_idx();
  }
  void s_set_root_idx(std::size_t idx) { overlaid_root_ = idx; }

  std::size_t s_node_high_mark() const {
    return added_node_base_ + added_nodes_.high_mark();
  }
  std::size_t s_edge_high_mark() const {
    return added_edge_base_ + added_edges_.high_mark();
  }

  friend class dag_interface<N, E>;
};

}  // namespace larch

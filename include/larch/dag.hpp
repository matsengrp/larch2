#pragma once

// Section 1: Includes
#include <larch/common.hpp>
#include <larch/chain.hpp>

#include <algorithm>
#include <cassert>
#include <concepts>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

namespace larch {

// Section 2: Forward Declarations

template <auto V>
  requires Enum<decltype(V)>
struct interface;
template <auto V>
  requires Enum<decltype(V)>
struct annotation;
template <Enum N, Enum E>
class dag_interface;
template <Enum N, Enum E>
class dag;
template <typename Base, typename ExtraNodeData, typename ExtraEdgeData>
class extend_dag;
template <typename Base>
class overlay_dag;
template <typename D, auto V>
  requires Enum<decltype(V)>
class node_view;
template <typename D, auto V>
  requires Enum<decltype(V)>
class edge_view;

// Section 3: Metaprogramming Helpers

namespace detail {

// node_variant_from
template <typename D, typename L>
struct node_variant_from_impl;
template <typename D, auto... Vs>
struct node_variant_from_impl<D, nttp_list<Vs...>> {
  using type = std::variant<node_view<D, Vs>...>;
};
template <typename D, typename L>
using node_variant_from = typename node_variant_from_impl<D, L>::type;

// edge_variant_from
template <typename D, typename L>
struct edge_variant_from_impl;
template <typename D, auto... Vs>
struct edge_variant_from_impl<D, nttp_list<Vs...>> {
  using type = std::variant<edge_view<D, Vs>...>;
};
template <typename D, typename L>
using edge_variant_from = typename edge_variant_from_impl<D, L>::type;

// annotation_tuple_from
template <typename L>
struct annotation_tuple_from_impl;
template <auto... Vs>
struct annotation_tuple_from_impl<nttp_list<Vs...>> {
  using type = std::tuple<chain<annotation<Vs>>...>;
};
template <typename L>
using annotation_tuple_from = typename annotation_tuple_from_impl<L>::type;

// annotation_map_from (tuple of unordered_maps, one per enumerator)
template <typename L>
struct annotation_map_from_impl;
template <auto... Vs>
struct annotation_map_from_impl<nttp_list<Vs...>> {
  using type = std::tuple<std::unordered_map<std::size_t, annotation<Vs>>...>;
};
template <typename L>
using annotation_map_from = typename annotation_map_from_impl<L>::type;

// enum_dispatch
template <typename L>
struct enum_dispatch;
template <auto... Vs>
struct enum_dispatch<nttp_list<Vs...>> {
  template <typename E, typename F>
  static auto call(E val, F&& f)
      -> std::common_type_t<decltype(f(cw<Vs>{}))...> {
    using R = std::common_type_t<decltype(f(cw<Vs>{}))...>;
    std::optional<R> result;
    ((static_cast<E>(Vs) == val ? (result.emplace(f(cw<Vs>{})), true)
                                : false) ||
     ...);
    return std::move(*result);
  }
};

}  // namespace detail

// Section 4: Internal Topology Structs

template <typename N>
struct node_topology {
  N kind_;
  std::size_t self_index_;
  std::size_t parents_start_;
  std::size_t parents_count_;
  std::size_t children_start_;
  std::size_t children_count_;
  std::size_t clade_offsets_start_ = no_idx;  // index into clade_offsets_ chain
  std::size_t clade_count_ = 0;               // number of clades (offsets has count+1 entries)
  std::size_t annotation_index_;
};

template <typename E>
struct edge_topology {
  E kind_;
  std::size_t self_index_;
  std::size_t parent_;
  std::size_t child_;
  std::size_t annotation_index_;
};

// Section 5: node_view and edge_view

template <typename D, auto V>
  requires Enum<decltype(V)>
class node_view : public interface<V> {
  D& dag_;
  std::size_t index_;

  node_view(D& d, std::size_t idx) : dag_{d}, index_{idx} {}

  template <auto K = V>
  auto& get_annotation();
  template <auto K = V>
  auto const& get_annotation() const;

 public:
  template <auto EV>
  auto append_child();
  template <auto EV>
  auto append_parent();
  auto get_children();
  auto get_parents();
  auto get_clade(std::size_t clade_i);
  std::size_t clade_count() const;
  void remove();
  std::size_t index() const { return index_; }

  friend struct interface<V>;
  template <Enum, Enum>
  friend class dag_interface;
};

template <typename D, auto V>
  requires Enum<decltype(V)>
class edge_view : public interface<V> {
  D& dag_;
  std::size_t index_;

  edge_view(D& d, std::size_t idx) : dag_{d}, index_{idx} {}

  template <auto K = V>
  auto& get_annotation();
  template <auto K = V>
  auto const& get_annotation() const;

 public:
  template <typename NV>
  void set_child(NV node);
  template <typename NV>
  void set_parent(NV node);
  auto get_child();
  auto get_parent();
  template <auto NV>
  auto get_child_as();
  template <auto NV>
  auto get_parent_as();
  void remove();
  std::size_t index() const { return index_; }

  friend struct interface<V>;
  template <Enum, Enum>
  friend class dag_interface;
};

// Section 6: dag_interface — public API + view-facing internal API
// (deducing-this)

template <Enum N, Enum E>
class dag_interface {
 public:
  using node_enum = N;
  using edge_enum = E;
  using index_type = std::size_t;
  using node_index_type = index_type;
  using edge_index_type = index_type;
  using node_enumerators = enumerators_list<N>;
  using edge_enumerators = enumerators_list<E>;

  // --- Public API (deducing-this dispatches to concrete type) ---

  template <auto V>
  auto append_node(this auto& self) {
    using Self = std::remove_reference_t<decltype(self)>;
    auto topo_idx = self.template s_alloc_node<V>();
    return node_view<Self, V>{self, topo_idx};
  }

  template <auto V>
  auto append_edge(this auto& self) {
    using Self = std::remove_reference_t<decltype(self)>;
    auto topo_idx = self.template s_alloc_edge<V>();
    return edge_view<Self, V>{self, topo_idx};
  }

  auto append_node(this auto& self, N kind) {
    using Self = std::remove_reference_t<decltype(self)>;
    using variant_type = detail::node_variant_from<Self, node_enumerators>;
    return detail::enum_dispatch<node_enumerators>::call(
        kind, [&self](auto v) -> variant_type {
          return self.template append_node<decltype(v)::value>();
        });
  }

  auto append_edge(this auto& self, E kind) {
    using Self = std::remove_reference_t<decltype(self)>;
    using variant_type = detail::edge_variant_from<Self, edge_enumerators>;
    return detail::enum_dispatch<edge_enumerators>::call(
        kind, [&self](auto v) -> variant_type {
          return self.template append_edge<decltype(v)::value>();
        });
  }

  template <typename NV>
  void set_root(this auto& self, NV node) {
    self.s_set_root_idx(node.index());
  }

  auto get_root(this auto& self) {
    return self.make_node_variant(self.s_root_idx());
  }

  template <auto V>
  auto get_root_as(this auto& self) {
    using Self = std::remove_reference_t<decltype(self)>;
    return node_view<Self, V>{self, self.s_root_idx()};
  }

  auto get_all_nodes(this auto& self) {
    return std::views::transform(self.s_node_topos(), [p = &self](auto& topo) {
      return p->make_node_variant(topo.self_index_);
    });
  }

  auto get_all_edges(this auto& self) {
    return std::views::transform(self.s_edge_topos(), [p = &self](auto& topo) {
      return p->make_edge_variant(topo.self_index_);
    });
  }

  auto get_node(this auto& self, std::size_t idx) {
    return self.make_node_variant(idx);
  }
  auto get_edge(this auto& self, std::size_t idx) {
    return self.make_edge_variant(idx);
  }
  template <auto V>
  auto get_edge_as(this auto& self, std::size_t idx) {
    using Self = std::remove_reference_t<decltype(self)>;
    return edge_view<Self, V>{self, idx};
  }

  std::size_t node_count(this auto& self) { return self.s_node_topos().size(); }
  std::size_t edge_count(this auto& self) { return self.s_edge_topos().size(); }
  std::size_t node_high_mark(this auto const& self) {
    return self.s_node_high_mark();
  }
  std::size_t edge_high_mark(this auto const& self) {
    return self.s_edge_high_mark();
  }

  // --- Clade-grouped children (CSR offsets into children section) ---

  // Number of clades for this node (0 if offsets not built).
  std::size_t clade_count(this auto const& self, std::size_t node_idx) {
    return self.s_clade_count(node_idx);
  }

  // Contiguous section of edge indices for one clade.
  // Requires clade offsets to have been built (clade_count > 0).
  auto clade_section(this auto& self, std::size_t node_idx,
                     std::size_t clade_i) {
    return self.s_clade_section(node_idx, clade_i);
  }

  // Build CSR-style clade offsets for all nodes.
  // F is called as get_edge_clade_idx(edge_idx) -> std::size_t.
  template <typename F>
  void build_all_clade_offsets(this auto& self, F&& get_edge_clade_idx) {
    self.s_build_all_clade_offsets(std::forward<F>(get_edge_clade_idx));
  }

 private:
  // --- View-facing internal API (deducing-this, friends only) ---

  auto make_node_variant(this auto& self, std::size_t idx) {
    using Self = std::remove_reference_t<decltype(self)>;
    using variant_type = detail::node_variant_from<Self, node_enumerators>;
    auto& topo = self.s_node_topo(idx);
    return detail::enum_dispatch<node_enumerators>::call(
        topo.kind_, [&self, idx](auto v) -> variant_type {
          return node_view<Self, decltype(v)::value>{self, idx};
        });
  }

  auto make_edge_variant(this auto& self, std::size_t idx) {
    using Self = std::remove_reference_t<decltype(self)>;
    using variant_type = detail::edge_variant_from<Self, edge_enumerators>;
    auto& topo = self.s_edge_topo(idx);
    return detail::enum_dispatch<edge_enumerators>::call(
        topo.kind_, [&self, idx](auto v) -> variant_type {
          return edge_view<Self, decltype(v)::value>{self, idx};
        });
  }

  // Topology access
  auto& node_topo(this auto& self, std::size_t idx) {
    return self.s_node_topo(idx);
  }
  auto& edge_topo(this auto& self, std::size_t idx) {
    return self.s_edge_topo(idx);
  }

  // Annotation access (takes DAG index, not annotation chain index)
  template <auto V>
  auto& node_ann(this auto& self, std::size_t node_idx) {
    return self.template s_node_ann<V>(node_idx);
  }
  template <auto V>
  auto& edge_ann(this auto& self, std::size_t edge_idx) {
    return self.template s_edge_ann<V>(edge_idx);
  }

  // Neighbor sections
  auto children_section(this auto& self, std::size_t node_idx) {
    return self.s_children_section(node_idx);
  }
  auto parents_section(this auto& self, std::size_t node_idx) {
    return self.s_parents_section(node_idx);
  }

  // Link/unlink
  void link_child(this auto& self, std::size_t node_idx, std::size_t edge_idx) {
    self.s_link_child(node_idx, edge_idx);
  }
  void unlink_child(this auto& self, std::size_t node_idx,
                    std::size_t edge_idx) {
    self.s_unlink_child(node_idx, edge_idx);
  }
  void link_parent(this auto& self, std::size_t node_idx,
                   std::size_t edge_idx) {
    self.s_link_parent(node_idx, edge_idx);
  }
  void unlink_parent(this auto& self, std::size_t node_idx,
                     std::size_t edge_idx) {
    self.s_unlink_parent(node_idx, edge_idx);
  }

  // Deallocation
  void dealloc_node(this auto& self, std::size_t idx, N kind) {
    self.s_dealloc_node(idx, kind);
  }
  void dealloc_edge(this auto& self, std::size_t idx, E kind) {
    self.s_dealloc_edge(idx, kind);
  }
  void dealloc_neighbors(this auto& self, std::size_t start,
                         std::size_t count) {
    self.s_dealloc_neighbors(start, count);
  }
  void dealloc_clade_offsets(this auto& self, std::size_t start,
                             std::size_t count) {
    self.s_dealloc_clade_offsets(start, count);
  }
  // Factory for typed views (used by edge_view::get_child_as / get_parent_as)
  template <auto V>
  auto make_typed_node_view(this auto& self, std::size_t idx) {
    using Self = std::remove_reference_t<decltype(self)>;
    return node_view<Self, V>{self, idx};
  }

  // Allocation
  template <auto V>
  std::size_t alloc_node(this auto& self) {
    return self.template s_alloc_node<V>();
  }
  template <auto V>
  std::size_t alloc_edge(this auto& self) {
    return self.template s_alloc_edge<V>();
  }

  // Iterable topology access
  auto& node_topos(this auto& self) { return self.s_node_topos(); }
  auto& edge_topos(this auto& self) { return self.s_edge_topos(); }

  // Root access
  std::size_t root_idx(this auto const& self) { return self.s_root_idx(); }
  void set_root_idx(this auto& self, std::size_t idx) {
    self.s_set_root_idx(idx);
  }

  // High mark access
  std::size_t node_high_mark_val(this auto const& self) {
    return self.s_node_high_mark();
  }
  std::size_t edge_high_mark_val(this auto const& self) {
    return self.s_edge_high_mark();
  }

  template <typename D, auto V>
    requires Enum<decltype(V)>
  friend class node_view;
  template <typename D, auto V>
    requires Enum<decltype(V)>
  friend class edge_view;
  template <typename, typename, typename>
  friend class extend_dag;
  template <typename>
  friend class overlay_dag;
};

// Section 7: dag — owning storage + storage primitives

template <Enum N, Enum E>
class dag : public dag_interface<N, E> {
 public:
  using typename dag_interface<N, E>::node_enumerators;
  using typename dag_interface<N, E>::edge_enumerators;
  using typename dag_interface<N, E>::index_type;

  using node_variant_type = detail::node_variant_from<dag, node_enumerators>;
  using edge_variant_type = detail::edge_variant_from<dag, edge_enumerators>;

 private:
  index_type root_ = no_idx;
  chain<std::size_t> neighbors_;
  chain<std::size_t> clade_offsets_;
  chain<node_topology<N>> node_topologies_;
  chain<edge_topology<E>> edge_topologies_;
  detail::annotation_tuple_from<node_enumerators> node_annotations_;
  detail::annotation_tuple_from<edge_enumerators> edge_annotations_;

  // --- Storage primitives (called by dag_interface via deducing-this) ---

  auto& s_node_topo(std::size_t idx) { return node_topologies_[idx]; }
  auto const& s_node_topo(std::size_t idx) const {
    return node_topologies_[idx];
  }
  auto& s_edge_topo(std::size_t idx) { return edge_topologies_[idx]; }
  auto const& s_edge_topo(std::size_t idx) const {
    return edge_topologies_[idx];
  }

  template <auto V>
  auto& s_node_ann(std::size_t node_idx) {
    constexpr std::size_t I = node_enumerators::template index_of<V>;
    return std::get<I>(
        node_annotations_)[node_topologies_[node_idx].annotation_index_];
  }
  template <auto V>
  auto const& s_node_ann(std::size_t node_idx) const {
    constexpr std::size_t I = node_enumerators::template index_of<V>;
    return std::get<I>(
        node_annotations_)[node_topologies_[node_idx].annotation_index_];
  }
  template <auto V>
  auto& s_edge_ann(std::size_t edge_idx) {
    constexpr std::size_t I = edge_enumerators::template index_of<V>;
    return std::get<I>(
        edge_annotations_)[edge_topologies_[edge_idx].annotation_index_];
  }
  template <auto V>
  auto const& s_edge_ann(std::size_t edge_idx) const {
    constexpr std::size_t I = edge_enumerators::template index_of<V>;
    return std::get<I>(
        edge_annotations_)[edge_topologies_[edge_idx].annotation_index_];
  }

  auto s_children_section(std::size_t node_idx) {
    auto& ntopo = node_topologies_[node_idx];
    return typename chain<std::size_t>::contiguous_section{
        neighbors_, ntopo.children_start_, ntopo.children_count_};
  }
  auto s_parents_section(std::size_t node_idx) {
    auto& ntopo = node_topologies_[node_idx];
    return typename chain<std::size_t>::contiguous_section{
        neighbors_, ntopo.parents_start_, ntopo.parents_count_};
  }

  auto s_clade_section(std::size_t node_idx, std::size_t clade_i) {
    auto& ntopo = node_topologies_[node_idx];
    assert(ntopo.clade_count_ > 0 && clade_i < ntopo.clade_count_);
    auto start_off = clade_offsets_[ntopo.clade_offsets_start_ + clade_i];
    auto end_off = clade_offsets_[ntopo.clade_offsets_start_ + clade_i + 1];
    return typename chain<std::size_t>::contiguous_section{
        neighbors_, ntopo.children_start_ + start_off, end_off - start_off};
  }

  std::size_t s_clade_count(std::size_t node_idx) const {
    return node_topologies_[node_idx].clade_count_;
  }

  void s_dealloc_clade_offsets(std::size_t start, std::size_t count) {
    clade_offsets_.remove(start, count);
  }

  template <typename F>
  void s_build_all_clade_offsets(F&& get_edge_clade_idx) {
    for (auto& ntopo : node_topologies_) {
      // Deallocate old offsets if any
      if (ntopo.clade_count_ > 0) {
        clade_offsets_.remove(ntopo.clade_offsets_start_,
                              ntopo.clade_count_ + 1);
        ntopo.clade_offsets_start_ = no_idx;
        ntopo.clade_count_ = 0;
      }
      if (ntopo.children_count_ == 0) continue;

      // Collect (clade_idx, edge_idx) pairs from children section
      auto sec = typename chain<std::size_t>::contiguous_section{
          neighbors_, ntopo.children_start_, ntopo.children_count_};
      std::vector<std::pair<std::size_t, std::size_t>> pairs;
      pairs.reserve(ntopo.children_count_);
      for (std::size_t i = 0; i < sec.size(); ++i)
        pairs.emplace_back(get_edge_clade_idx(sec[i]), sec[i]);

      // Sort by clade index (stable to preserve order within clades)
      std::stable_sort(pairs.begin(), pairs.end(),
                       [](auto const& a, auto const& b) {
                         return a.first < b.first;
                       });

      // Write back sorted edge indices
      for (std::size_t i = 0; i < pairs.size(); ++i) sec[i] = pairs[i].second;

      // Build CSR offsets
      std::size_t num_clades = pairs.back().first + 1;
      std::vector<std::size_t> offsets(num_clades + 1, 0);
      for (auto const& [clade, _] : pairs) offsets[clade + 1]++;
      for (std::size_t i = 1; i <= num_clades; ++i) offsets[i] += offsets[i - 1];

      auto off_sec = clade_offsets_.move_range(std::move(offsets));
      ntopo.clade_offsets_start_ = off_sec.start();
      ntopo.clade_count_ = num_clades;
    }
  }

  void s_invalidate_clade_offsets(node_topology<N>& ntopo) {
    if (ntopo.clade_count_ > 0) {
      clade_offsets_.remove(ntopo.clade_offsets_start_,
                            ntopo.clade_count_ + 1);
      ntopo.clade_offsets_start_ = no_idx;
      ntopo.clade_count_ = 0;
    }
  }

  void s_link_child(std::size_t node_idx, std::size_t edge_idx) {
    auto& ntopo = node_topologies_[node_idx];
    s_invalidate_clade_offsets(ntopo);
    auto sec = typename chain<std::size_t>::contiguous_section{
        neighbors_, ntopo.children_start_, ntopo.children_count_};
    sec = sec.emplace_back(edge_idx);
    ntopo.children_start_ = sec.start();
    ntopo.children_count_ = sec.size();
  }

  void s_unlink_child(std::size_t node_idx, std::size_t edge_idx) {
    auto& ntopo = node_topologies_[node_idx];
    if (ntopo.children_count_ == 0) return;
    s_invalidate_clade_offsets(ntopo);
    auto sec = typename chain<std::size_t>::contiguous_section{
        neighbors_, ntopo.children_start_, ntopo.children_count_};
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
    auto& ntopo = node_topologies_[node_idx];
    auto sec = typename chain<std::size_t>::contiguous_section{
        neighbors_, ntopo.parents_start_, ntopo.parents_count_};
    sec = sec.emplace_back(edge_idx);
    ntopo.parents_start_ = sec.start();
    ntopo.parents_count_ = sec.size();
  }

  void s_unlink_parent(std::size_t node_idx, std::size_t edge_idx) {
    auto& ntopo = node_topologies_[node_idx];
    if (ntopo.parents_count_ == 0) return;
    auto sec = typename chain<std::size_t>::contiguous_section{
        neighbors_, ntopo.parents_start_, ntopo.parents_count_};
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
    auto& ann_chain = std::get<I>(node_annotations_);
    auto ann_idx = ann_chain.emplace();
    node_topology<N> topo{};
    topo.kind_ = V;
    topo.self_index_ = no_idx;
    topo.parents_start_ = no_idx;
    topo.parents_count_ = 0;
    topo.children_start_ = no_idx;
    topo.children_count_ = 0;
    topo.clade_offsets_start_ = no_idx;
    topo.clade_count_ = 0;
    topo.annotation_index_ = ann_idx;
    auto topo_idx = node_topologies_.emplace(topo);
    node_topologies_[topo_idx].self_index_ = topo_idx;
    return topo_idx;
  }

  template <auto V>
  std::size_t s_alloc_edge() {
    constexpr std::size_t I = edge_enumerators::template index_of<V>;
    auto& ann_chain = std::get<I>(edge_annotations_);
    auto ann_idx = ann_chain.emplace();
    edge_topology<E> topo{};
    topo.kind_ = V;
    topo.self_index_ = no_idx;
    topo.parent_ = no_idx;
    topo.child_ = no_idx;
    topo.annotation_index_ = ann_idx;
    auto topo_idx = edge_topologies_.emplace(topo);
    edge_topologies_[topo_idx].self_index_ = topo_idx;
    return topo_idx;
  }

  void s_dealloc_node(std::size_t idx, N kind) {
    auto& ntopo = node_topologies_[idx];
    if (ntopo.clade_count_ > 0)
      clade_offsets_.remove(ntopo.clade_offsets_start_,
                            ntopo.clade_count_ + 1);
    detail::enum_dispatch<node_enumerators>::call(kind, [&](auto v) {
      constexpr std::size_t I =
          node_enumerators::template index_of<decltype(v)::value>;
      std::get<I>(node_annotations_).remove(ntopo.annotation_index_);
      return 0;
    });
    node_topologies_.remove(idx);
  }

  void s_dealloc_edge(std::size_t idx, E kind) {
    auto& etopo = edge_topologies_[idx];
    detail::enum_dispatch<edge_enumerators>::call(kind, [&](auto v) {
      constexpr std::size_t I =
          edge_enumerators::template index_of<decltype(v)::value>;
      std::get<I>(edge_annotations_).remove(etopo.annotation_index_);
      return 0;
    });
    edge_topologies_.remove(idx);
  }

  void s_dealloc_neighbors(std::size_t start, std::size_t count) {
    neighbors_.remove(start, count);
  }

  auto& s_node_topos() { return node_topologies_; }
  auto const& s_node_topos() const { return node_topologies_; }
  auto& s_edge_topos() { return edge_topologies_; }
  auto const& s_edge_topos() const { return edge_topologies_; }

  std::size_t s_root_idx() const { return root_; }
  void s_set_root_idx(std::size_t idx) { root_ = idx; }
  std::size_t s_node_high_mark() const { return node_topologies_.high_mark(); }
  std::size_t s_edge_high_mark() const { return edge_topologies_.high_mark(); }

  friend class dag_interface<N, E>;
};

// Section 8: Out-of-line view definitions

// --- node_view methods ---

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto EV>
auto node_view<D, V>::append_child() {
  auto edge = dag_.template append_edge<EV>();
  dag_.edge_topo(edge.index()).parent_ = index_;
  dag_.link_child(index_, edge.index());
  return edge;
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto EV>
auto node_view<D, V>::append_parent() {
  auto edge = dag_.template append_edge<EV>();
  dag_.edge_topo(edge.index()).child_ = index_;
  dag_.link_parent(index_, edge.index());
  return edge;
}

template <typename D, auto V>
  requires Enum<decltype(V)>
auto node_view<D, V>::get_children() {
  auto sec = dag_.children_section(index_);
  return std::views::transform(std::move(sec), [this](auto& idx) {
    return dag_.make_edge_variant(idx);
  });
}

template <typename D, auto V>
  requires Enum<decltype(V)>
auto node_view<D, V>::get_parents() {
  auto sec = dag_.parents_section(index_);
  return std::views::transform(std::move(sec), [this](auto& idx) {
    return dag_.make_edge_variant(idx);
  });
}

template <typename D, auto V>
  requires Enum<decltype(V)>
auto node_view<D, V>::get_clade(std::size_t clade_i) {
  auto sec = dag_.clade_section(index_, clade_i);
  return std::views::transform(std::move(sec), [this](auto& idx) {
    return dag_.make_edge_variant(idx);
  });
}

template <typename D, auto V>
  requires Enum<decltype(V)>
std::size_t node_view<D, V>::clade_count() const {
  return dag_.clade_count(index_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
void node_view<D, V>::remove() {
  auto& ntopo = dag_.node_topo(index_);

  std::vector<std::size_t> edge_indices;
  if (ntopo.parents_count_ > 0) {
    auto psec = dag_.parents_section(index_);
    for (auto idx : psec) edge_indices.push_back(idx);
  }
  if (ntopo.children_count_ > 0) {
    auto csec = dag_.children_section(index_);
    for (auto idx : csec) edge_indices.push_back(idx);
  }

  for (auto eidx : edge_indices) {
    auto& etopo = dag_.edge_topo(eidx);
    if (etopo.parent_ != no_idx && etopo.parent_ != index_)
      dag_.unlink_child(etopo.parent_, eidx);
    if (etopo.child_ != no_idx && etopo.child_ != index_)
      dag_.unlink_parent(etopo.child_, eidx);
    dag_.dealloc_edge(eidx, etopo.kind_);
  }

  if (ntopo.parents_count_ > 0)
    dag_.dealloc_neighbors(ntopo.parents_start_, ntopo.parents_count_);
  if (ntopo.children_count_ > 0)
    dag_.dealloc_neighbors(ntopo.children_start_, ntopo.children_count_);
  if (ntopo.clade_count_ > 0) {
    dag_.dealloc_clade_offsets(ntopo.clade_offsets_start_,
                               ntopo.clade_count_ + 1);
    ntopo.clade_offsets_start_ = no_idx;
    ntopo.clade_count_ = 0;
  }

  dag_.dealloc_node(index_, ntopo.kind_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto K>
auto& node_view<D, V>::get_annotation() {
  static_assert(K == V);
  return dag_.template node_ann<V>(index_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto K>
auto const& node_view<D, V>::get_annotation() const {
  static_assert(K == V);
  return dag_.template node_ann<V>(index_);
}

// --- edge_view methods ---

template <typename D, auto V>
  requires Enum<decltype(V)>
template <typename NV>
void edge_view<D, V>::set_child(NV node) {
  auto& etopo = dag_.edge_topo(index_);
  if (etopo.child_ != no_idx) dag_.unlink_parent(etopo.child_, index_);
  etopo.child_ = node.index();
  dag_.link_parent(node.index(), index_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <typename NV>
void edge_view<D, V>::set_parent(NV node) {
  auto& etopo = dag_.edge_topo(index_);
  if (etopo.parent_ != no_idx) dag_.unlink_child(etopo.parent_, index_);
  etopo.parent_ = node.index();
  dag_.link_child(node.index(), index_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
auto edge_view<D, V>::get_child() {
  auto& etopo = dag_.edge_topo(index_);
  return dag_.make_node_variant(etopo.child_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
auto edge_view<D, V>::get_parent() {
  auto& etopo = dag_.edge_topo(index_);
  return dag_.make_node_variant(etopo.parent_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto NV>
auto edge_view<D, V>::get_child_as() {
  auto& etopo = dag_.edge_topo(index_);
  return dag_.template make_typed_node_view<NV>(etopo.child_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto NV>
auto edge_view<D, V>::get_parent_as() {
  auto& etopo = dag_.edge_topo(index_);
  return dag_.template make_typed_node_view<NV>(etopo.parent_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
void edge_view<D, V>::remove() {
  auto& etopo = dag_.edge_topo(index_);

  if (etopo.parent_ != no_idx) dag_.unlink_child(etopo.parent_, index_);

  if (etopo.child_ != no_idx) dag_.unlink_parent(etopo.child_, index_);

  dag_.dealloc_edge(index_, etopo.kind_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto K>
auto& edge_view<D, V>::get_annotation() {
  static_assert(K == V);
  return dag_.template edge_ann<V>(index_);
}

template <typename D, auto V>
  requires Enum<decltype(V)>
template <auto K>
auto const& edge_view<D, V>::get_annotation() const {
  static_assert(K == V);
  return dag_.template edge_ann<V>(index_);
}

}  // namespace larch

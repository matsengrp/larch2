#pragma once

#include <larch/overlay_dag.hpp>
#include <larch/pmr_arena.hpp>
#include <larch/native_optimize.hpp>
#include <larch/spr_move.hpp>

#include <cassert>
#include <functional>
#include <memory_resource>
#include <unordered_map>
#include <variant>

namespace larch {

// ============================================================================
// Template helpers for overlay DAG operations
// ============================================================================

namespace overlay_detail {

template <typename Dag>
std::pmr::vector<std::size_t> get_parent_edges_of(
    Dag& d, std::size_t node_idx, std::pmr::memory_resource* mr) {
  std::pmr::vector<std::size_t> result(mr);
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_parents()) {
          std::visit([&](auto edge) { result.push_back(edge.index()); }, ev);
        }
      },
      nv);
  return result;
}

template <typename Dag>
std::size_t get_parent_idx_of(Dag& d, std::size_t edge_idx) {
  auto ev = d.get_edge(edge_idx);
  return std::visit(
      [](auto edge) {
        auto pv = edge.get_parent();
        return std::visit([](auto parent) { return parent.index(); }, pv);
      },
      ev);
}

template <typename Dag>
std::size_t get_child_idx_of(Dag& d, std::size_t edge_idx) {
  auto ev = d.get_edge(edge_idx);
  return std::visit(
      [](auto edge) {
        auto cv = edge.get_child();
        return std::visit([](auto child) { return child.index(); }, cv);
      },
      ev);
}

template <typename Dag>
std::size_t get_clade_idx_of(Dag& d, std::size_t edge_idx) {
  auto ev = d.get_edge(edge_idx);
  return std::visit([](auto edge) { return edge.clade_index(); }, ev);
}

template <typename Dag>
std::size_t child_count_of(Dag& d, std::size_t node_idx) {
  std::size_t count = 0;
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          (void)ev;
          ++count;
        }
      },
      nv);
  return count;
}

template <typename Dag>
std::pmr::vector<std::size_t> get_child_edges_of(
    Dag& d, std::size_t node_idx, std::pmr::memory_resource* mr) {
  std::pmr::vector<std::size_t> result(mr);
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          std::visit([&](auto edge) { result.push_back(edge.index()); }, ev);
        }
      },
      nv);
  return result;
}

}  // namespace overlay_detail

// ============================================================================
// apply_spr_topology: SPR topology changes on an overlay (no Fitch/CG
// recompute)
// ============================================================================

// Applies SPR topological changes on the overlay.
// Returns the fragment root index (the topmost affected node).
inline std::size_t apply_spr_topology(
    overlay_dag<phylo_dag>& ov, phylo_dag& base, spr_move const& move,
    std::pmr::memory_resource* mr = std::pmr::new_delete_resource()) {
  auto src = move.src;
  auto dst = move.dst;
  auto lca = move.lca;

  // Get src's parent edge and parent node
  auto src_pe = overlay_detail::get_parent_edges_of(ov, src, mr);
  auto src_parent_edge = src_pe[0];
  auto src_parent = overlay_detail::get_parent_idx_of(ov, src_parent_edge);
  auto src_parent_child_count = overlay_detail::child_count_of(ov, src_parent);

  bool src_parent_collapsed = false;
  std::size_t grandparent = 0;
  std::size_t remaining_child_after_collapse = 0;

  // 1. Remove edge from src_parent to src
  auto ev1 = ov.get_edge(src_parent_edge);
  std::visit([](auto edge) { edge.remove(); }, ev1);

  // 2. If src_parent was binary, collapse it
  if (src_parent_child_count == 2) {
    src_parent_collapsed = true;

    // Find remaining child (only one left after removing src)
    std::size_t remaining_child = 0;
    auto nv = ov.get_node(src_parent);
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto cv = edge.get_child();
                  remaining_child =
                      std::visit([](auto c) { return c.index(); }, cv);
                },
                ev);
            break;
          }
        },
        nv);

    remaining_child_after_collapse = remaining_child;

    // Find grandparent
    auto sp_pe = overlay_detail::get_parent_edges_of(ov, src_parent, mr);
    if (!sp_pe.empty()) {
      grandparent = overlay_detail::get_parent_idx_of(ov, sp_pe[0]);
      auto gp_clade = overlay_detail::get_clade_idx_of(ov, sp_pe[0]);

      // Remove src_parent (removes all its remaining edges)
      auto sp_nv = ov.get_node(src_parent);
      std::visit([](auto n) { n.remove(); }, sp_nv);

      // Connect grandparent to remaining child
      auto e = ov.append_edge<edge_kind::clade>();
      e.clade_index() = gp_clade;
      auto gpv = ov.get_node(grandparent);
      std::visit([&](auto p) { e.set_parent(p); }, gpv);
      auto rcv = ov.get_node(remaining_child);
      std::visit([&](auto c) { e.set_child(c); }, rcv);
    }
  }

  // 3. Remove edge from dst_parent to dst
  // Re-fetch since topology may have changed above
  auto dst_pe = overlay_detail::get_parent_edges_of(ov, dst, mr);
  auto dst_parent_edge2 = dst_pe[0];
  auto dst_clade = overlay_detail::get_clade_idx_of(ov, dst_parent_edge2);
  auto dst_parent = overlay_detail::get_parent_idx_of(ov, dst_parent_edge2);

  auto ev3 = ov.get_edge(dst_parent_edge2);
  std::visit([](auto edge) { edge.remove(); }, ev3);

  // 4. Create new inner node
  auto new_inner = ov.append_node<node_kind::inner>();

  // 5. Connect dst_parent -> new_inner
  {
    auto e = ov.append_edge<edge_kind::clade>();
    e.clade_index() = dst_clade;
    auto dpv = ov.get_node(dst_parent);
    std::visit([&](auto p) { e.set_parent(p); }, dpv);
    e.set_child(new_inner);
  }

  // 6. Connect new_inner -> dst
  {
    auto e = ov.append_edge<edge_kind::clade>();
    e.clade_index() = 0;
    e.set_parent(new_inner);
    auto dv = ov.get_node(dst);
    std::visit([&](auto c) { e.set_child(c); }, dv);
  }

  // 7. Connect new_inner -> src
  {
    auto e = ov.append_edge<edge_kind::clade>();
    e.clade_index() = 1;
    e.set_parent(new_inner);
    auto sv = ov.get_node(src);
    std::visit([&](auto c) { e.set_child(c); }, sv);
  }

  // Determine fragment root:
  // If LCA was collapsed (src_parent == lca and was binary), use grandparent.
  // But if grandparent is UA, find UA's actual child in the overlay — the SPR
  // may have created a new inner node that is now the tree root.  Returning
  // remaining_child_after_collapse would miss the new inner node and the
  // moved source, producing an incomplete fragment.
  if (src_parent_collapsed && src_parent == lca) {
    if (is_ua(base, grandparent)) {
      auto ua_children = overlay_detail::get_child_edges_of(ov, grandparent, mr);
      assert(!ua_children.empty());
      return overlay_detail::get_child_idx_of(ov, ua_children[0]);
    }
    return grandparent;
  }
  return lca;
}

// ============================================================================
// copy_subtree_from_overlay: DFS-copy subtree into a new plain phylo_dag
// ============================================================================

inline phylo_dag copy_subtree_from_overlay(
    overlay_dag<phylo_dag>& ov, phylo_dag& base, std::size_t fragment_root,
    std::pmr::memory_resource* mr = std::pmr::new_delete_resource()) {
  phylo_dag fragment;

  // Create UA node with reference sequence
  auto ua = fragment.append_node<node_kind::ua>();
  ua.reference_sequence() =
      base.get_root_as<node_kind::ua>().reference_sequence();
  fragment.set_root(ua);

  // DFS copy from fragment_root
  std::pmr::unordered_map<std::size_t, std::size_t> old_to_new(mr);

  std::function<std::size_t(std::size_t)> copy_node =
      [&](std::size_t ov_idx) -> std::size_t {
    if (auto it = old_to_new.find(ov_idx); it != old_to_new.end())
      return it->second;

    auto nv = ov.get_node(ov_idx);
    std::size_t new_idx = std::visit(
        [&](auto node) -> std::size_t {
          if constexpr (requires { node.sample_id(); }) {
            // Leaf: copy CG and sample_id
            auto n = fragment.append_node<node_kind::leaf>();
            n.cg() = node.cg();
            n.sample_id() = node.sample_id();
            return n.index();
          } else if constexpr (requires { node.reference_sequence(); }) {
            // UA -- shouldn't happen in subtree copy
            return std::size_t(-1);
          } else {
            // Inner: don't copy CG (Fitch will assign it)
            auto n = fragment.append_node<node_kind::inner>();
            return n.index();
          }
        },
        nv);

    old_to_new[ov_idx] = new_idx;

    // Copy children recursively
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto cv = edge.get_child();
                  auto child_ov_idx =
                      std::visit([](auto c) { return c.index(); }, cv);
                  auto new_child_idx = copy_node(child_ov_idx);

                  auto e = fragment.append_edge<edge_kind::clade>();
                  e.clade_index() = edge.clade_index();
                  auto pv = fragment.get_node(new_idx);
                  std::visit([&](auto p) { e.set_parent(p); }, pv);
                  auto ccv = fragment.get_node(new_child_idx);
                  std::visit([&](auto c) { e.set_child(c); }, ccv);
                },
                ev);
          }
        },
        ov.get_node(ov_idx));

    return new_idx;
  };

  auto frag_root_idx = copy_node(fragment_root);

  // Connect UA to fragment root
  auto e = fragment.append_edge<edge_kind::clade>();
  e.clade_index() = 0;
  e.set_parent(ua);
  auto frv = fragment.get_node(frag_root_idx);
  std::visit([&](auto c) { e.set_child(c); }, frv);

  return fragment;
}

// ============================================================================
// apply_spr_as_fragment: combined overlay SPR + fragment extraction
// ============================================================================

inline phylo_dag apply_spr_as_fragment(phylo_dag& tree, spr_move const& move) {
  scoped_arena<4096> arena;
  auto* mr = arena.get();

  overlay_dag<phylo_dag> ov{tree};
  auto fragment_root = apply_spr_topology(ov, tree, move, mr);

  // Get the fragment root's parent CG from the overlay.  The parent's CG
  // is unchanged by the SPR (it's above the affected region), so we can
  // read it from the base tree.  This lets fitch_assign_compact_genomes
  // use the correct parent state instead of the reference.
  compact_genome const* parent_cg = nullptr;
  compact_genome parent_cg_storage;
  auto frag_parents = overlay_detail::get_parent_edges_of(ov, fragment_root, mr);
  if (!frag_parents.empty()) {
    auto parent_idx = overlay_detail::get_parent_idx_of(ov, frag_parents[0]);
    auto pv = ov.get_node(parent_idx);
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.cg(); }) {
            parent_cg_storage = node.cg();
            parent_cg = &parent_cg_storage;
          }
        },
        pv);
  }

  auto fragment = copy_subtree_from_overlay(ov, tree, fragment_root, mr);

  fitch_assign_compact_genomes(fragment, parent_cg);
  recompute_edge_mutations(fragment);
  set_sample_ids_from_cg(fragment);

  return fragment;
}

}  // namespace larch

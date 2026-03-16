#include <larch/overlay_dag.hpp>

#include <cassert>
#include <cstddef>
#include <print>
#include <string>
#include <variant>

enum class my_node { root, branch, leaf };
enum class my_edge { structural, reference };

template <>
struct larch::annotation<my_node::root> {
  std::string name;
};
template <>
struct larch::annotation<my_node::branch> {
  int priority = 0;
};
template <>
struct larch::annotation<my_node::leaf> {
  double value = 0.0;
};
template <>
struct larch::annotation<my_edge::structural> {
  int weight = 1;
};
template <>
struct larch::annotation<my_edge::reference> {
  std::string label;
};

template <>
struct larch::interface<my_node::root> {
  auto& name(this auto& self) { return self.get_annotation().name; }
};
template <>
struct larch::interface<my_node::branch> {
  auto& priority(this auto& self) { return self.get_annotation().priority; }
};
template <>
struct larch::interface<my_node::leaf> {
  auto& value(this auto& self) { return self.get_annotation().value; }
};
template <>
struct larch::interface<my_edge::structural> {
  auto& weight(this auto& self) { return self.get_annotation().weight; }
};
template <>
struct larch::interface<my_edge::reference> {
  auto& label(this auto& self) { return self.get_annotation().label; }
};

using dag_t = larch::dag<my_node, my_edge>;
using overlay_t = larch::overlay_dag<dag_t>;

// Helper: build a small tree in base dag
//   root --[structural]--> branch --[structural]--> leaf
// Returns {root_idx, branch_idx, leaf_idx, e1_idx, e2_idx}
struct tree_indices {
  std::size_t root, branch, leaf, e1, e2;
};

tree_indices build_small_tree(dag_t& d) {
  auto r = d.append_node<my_node::root>();
  r.name() = "base_root";
  auto b = d.append_node<my_node::branch>();
  b.priority() = 5;
  auto l = d.append_node<my_node::leaf>();
  l.value() = 2.718;

  auto e1 = d.append_edge<my_edge::structural>();
  e1.set_parent(r);
  e1.set_child(b);
  e1.weight() = 10;

  auto e2 = d.append_edge<my_edge::structural>();
  e2.set_parent(b);
  e2.set_child(l);
  e2.weight() = 20;

  d.set_root(r);

  return {r.index(), b.index(), l.index(), e1.index(), e2.index()};
}

int main() {
  // 1. test_read_fallthrough — Create dag, wrap in overlay. Read nodes ->
  // values match base
  {
    dag_t d;
    auto ids = build_small_tree(d);
    overlay_t ov{d};

    // Read through overlay — should fall through to base
    auto rv = ov.get_root();
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.name(); }) {
            assert(v.name() == "base_root");
          } else {
            assert(false);
          }
        },
        rv);

    auto nv = ov.get_node(ids.branch);
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.priority(); }) {
            assert(v.priority() == 5);
          }
        },
        nv);

    auto ev = ov.get_edge(ids.e1);
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.weight(); }) {
            assert(v.weight() == 10);
          }
        },
        ev);

    std::println("  1. test_read_fallthrough passed");
  }

  // 2. test_annotation_overlay — Overlay a node, modify annotation. Base
  // unchanged.
  {
    dag_t d;
    auto ids = build_small_tree(d);
    overlay_t ov{d};

    // Overlay the branch node and change its priority
    ov.set_overlay_node(ids.branch);
    assert(ov.is_overlaid_node(ids.branch));

    auto b_ov = ov.get_node(ids.branch);
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.priority(); }) {
            // Should have copied the original value
            assert(v.priority() == 5);
            v.priority() = 99;
          }
        },
        b_ov);

    // Overlay sees the new value
    auto b_ov2 = ov.get_node(ids.branch);
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.priority(); }) {
            assert(v.priority() == 99);
          }
        },
        b_ov2);

    // Base unchanged
    auto b_base = d.get_node(ids.branch);
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.priority(); }) {
            assert(v.priority() == 5);
          }
        },
        b_base);

    std::println("  2. test_annotation_overlay passed");
  }

  // 3. test_topology_overlay — Overlay a node, add/remove child edge. Base
  // topology unchanged.
  {
    dag_t d;
    auto ids = build_small_tree(d);
    overlay_t ov{d};

    // Add a new leaf to the branch via overlay
    auto new_leaf = ov.append_node<my_node::leaf>();
    new_leaf.value() = 42.0;

    auto new_edge = ov.append_edge<my_edge::structural>();
    auto branch_var = ov.get_node(ids.branch);
    std::visit([&](auto& v) { new_edge.set_parent(v); }, branch_var);
    new_edge.set_child(new_leaf);
    new_edge.weight() = 30;

    // Branch should now have 2 children in overlay (original + new)
    auto b_ov = ov.get_node(ids.branch);
    std::size_t ov_child_count = 0;
    std::visit(
        [&](auto& v) {
          for (auto ev : v.get_children()) {
            (void)ev;
            ++ov_child_count;
          }
        },
        b_ov);
    assert(ov_child_count == 2);

    // Base branch should still have 1 child
    auto b_base = d.get_node(ids.branch);
    std::size_t base_child_count = 0;
    std::visit(
        [&](auto& v) {
          for (auto ev : v.get_children()) {
            (void)ev;
            ++base_child_count;
          }
        },
        b_base);
    assert(base_child_count == 1);

    std::println("  3. test_topology_overlay passed");
  }

  // 4. test_append_node — Append new node to overlay. Not in base.
  {
    dag_t d;
    auto ids = build_small_tree(d);
    overlay_t ov{d};

    auto new_node = ov.append_node<my_node::leaf>();
    new_node.value() = 7.77;
    assert(ov.is_added_node(new_node.index()));

    // Read it back
    auto nv = ov.get_node(new_node.index());
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.value(); }) {
            assert(v.value() == 7.77);
          }
        },
        nv);

    // Base node count unchanged (3 original)
    std::size_t base_count = 0;
    for (auto nv : d.get_all_nodes()) {
      (void)nv;
      ++base_count;
    }
    assert(base_count == 3);

    std::println("  4. test_append_node passed");
  }

  // 5. test_append_edge — Append new edge connecting base node to overlay node
  {
    dag_t d;
    auto ids = build_small_tree(d);
    overlay_t ov{d};

    auto new_leaf = ov.append_node<my_node::leaf>();
    new_leaf.value() = 9.99;

    auto new_edge = ov.append_edge<my_edge::reference>();
    // Connect root (base node) to new_leaf (added node)
    auto r = ov.get_root_as<my_node::root>();
    new_edge.set_parent(r);
    new_edge.set_child(new_leaf);
    new_edge.label() = "overlay_edge";

    // Read back
    auto ev = ov.get_edge(new_edge.index());
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.label(); }) {
            assert(v.label() == "overlay_edge");
          }
        },
        ev);

    // Base edge count unchanged (2 original)
    std::size_t base_edge_count = 0;
    for (auto ev : d.get_all_edges()) {
      (void)ev;
      ++base_edge_count;
    }
    assert(base_edge_count == 2);

    std::println("  5. test_append_edge passed");
  }

  // 6. test_iteration — get_all_nodes/get_all_edges sees base + overlay content
  {
    dag_t d;
    auto ids = build_small_tree(d);
    overlay_t ov{d};

    // Add one node and one edge in overlay
    auto new_leaf = ov.append_node<my_node::leaf>();
    new_leaf.value() = 1.23;

    auto new_edge = ov.append_edge<my_edge::structural>();
    auto r = ov.get_root_as<my_node::root>();
    new_edge.set_parent(r);
    new_edge.set_child(new_leaf);

    // Also overlay the branch (copy-on-write, doesn't add new node)
    ov.set_overlay_node(ids.branch);

    // Should see 4 nodes (3 base + 1 added), 3 edges (2 base + 1 added)
    std::size_t node_count = 0;
    for (auto nv : ov.get_all_nodes()) {
      (void)nv;
      ++node_count;
    }
    assert(node_count == 4);

    std::size_t edge_count = 0;
    for (auto ev : ov.get_all_edges()) {
      (void)ev;
      ++edge_count;
    }
    assert(edge_count == 3);

    std::println("  6. test_iteration passed");
  }

  // 7. test_discard — Destroy overlay. Base unchanged.
  {
    dag_t d;
    auto ids = build_small_tree(d);

    {
      overlay_t ov{d};
      ov.set_overlay_node(ids.branch);

      auto b_ov = ov.get_node(ids.branch);
      std::visit(
          [](auto& v) {
            if constexpr (requires { v.priority(); }) {
              v.priority() = 999;
            }
          },
          b_ov);

      auto new_node = ov.append_node<my_node::leaf>();
      new_node.value() = 100.0;
    }
    // Overlay destroyed

    // Base still has original values
    auto b = d.get_node(ids.branch);
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.priority(); }) {
            assert(v.priority() == 5);
          }
        },
        b);

    std::size_t count = 0;
    for (auto nv : d.get_all_nodes()) {
      (void)nv;
      ++count;
    }
    assert(count == 3);

    std::println("  7. test_discard passed");
  }

  // 8. test_get_children_parents — After topology overlay, children/parents
  // correct
  {
    dag_t d;
    auto ids = build_small_tree(d);
    overlay_t ov{d};

    // Non-overlaid node: children/parents should fallthrough to base
    auto r = ov.get_root_as<my_node::root>();
    std::size_t r_children = 0;
    for (auto ev : r.get_children()) {
      (void)ev;
      ++r_children;
    }
    assert(r_children == 1);  // root has 1 child edge

    // Overlay the root, then check children still correct
    ov.set_overlay_node(ids.root);
    auto r2 = ov.get_root_as<my_node::root>();
    std::size_t r2_children = 0;
    for (auto ev : r2.get_children()) {
      (void)ev;
      ++r2_children;
    }
    assert(r2_children == 1);  // same count after overlay copy

    // Check that leaf has 1 parent (non-overlaid, fallthrough)
    auto l = ov.get_node(ids.leaf);
    std::size_t l_parents = 0;
    std::visit(
        [&](auto& v) {
          for (auto ev : v.get_parents()) {
            (void)ev;
            ++l_parents;
          }
        },
        l);
    assert(l_parents == 1);

    // Navigate through edges
    auto branch = ov.get_node(ids.branch);
    std::visit(
        [&](auto& v) {
          std::size_t child_count = 0;
          for (auto ev : v.get_children()) {
            std::visit(
                [](auto& e) {
                  auto child = e.get_child();
                  std::visit(
                      [](auto& c) {
                        if constexpr (requires { c.value(); }) {
                          assert(c.value() == 2.718);
                        }
                      },
                      child);
                },
                ev);
            ++child_count;
          }
          assert(child_count == 1);
        },
        branch);

    std::println("  8. test_get_children_parents passed");
  }

  std::println("All overlay_dag tests passed");
  return 0;
}

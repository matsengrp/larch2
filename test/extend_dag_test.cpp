#include <larch/extend_dag.hpp>

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

struct node_data {
  int color = 0;
};
struct edge_data {
  float length = 0.0f;
};

using ext_t = larch::extend_dag<dag_t, node_data, edge_data>;

int main() {
  // 1. test_passthrough — All standard operations work through extend_dag
  {
    dag_t d;
    ext_t ext{d};

    auto r = ext.append_node<my_node::root>();
    r.name() = "hello";
    assert(r.name() == "hello");

    auto l = ext.append_node<my_node::leaf>();
    l.value() = 3.14;
    assert(l.value() == 3.14);

    auto e = ext.append_edge<my_edge::structural>();
    e.set_parent(r);
    e.set_child(l);
    e.weight() = 10;
    assert(e.weight() == 10);

    std::size_t node_count = 0;
    for (auto nv : ext.get_all_nodes()) {
      (void)nv;
      ++node_count;
    }
    assert(node_count == 2);

    std::size_t edge_count = 0;
    for (auto ev : ext.get_all_edges()) {
      (void)ev;
      ++edge_count;
    }
    assert(edge_count == 1);

    ext.set_root(r);
    auto rv = ext.get_root();
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.name(); }) {
            assert(v.name() == "hello");
          } else {
            assert(false);
          }
        },
        rv);
  }

  // 2. test_extra_data — Write/read node_extra and edge_extra
  {
    dag_t d;
    ext_t ext{d};

    auto r = ext.append_node<my_node::root>();
    auto l = ext.append_node<my_node::leaf>();
    auto e = ext.append_edge<my_edge::structural>();
    e.set_parent(r);
    e.set_child(l);

    ext.node_extra(r.index()).color = 42;
    ext.node_extra(l.index()).color = 99;
    ext.edge_extra(e.index()).length = 1.5f;

    assert(ext.node_extra(r.index()).color == 42);
    assert(ext.node_extra(l.index()).color == 99);
    assert(ext.edge_extra(e.index()).length == 1.5f);
  }

  // 3. test_views_work — node_view<extend_dag<...>, V> works for annotations
  {
    dag_t d;
    ext_t ext{d};

    auto r = ext.append_node<my_node::root>();
    r.name() = "root_via_extend";

    auto r2 = ext.get_root_as<my_node::root>();
    // r2 is not valid until we set root
    ext.set_root(r);
    auto r3 = ext.get_root_as<my_node::root>();
    assert(r3.name() == "root_via_extend");

    // Runtime variant access
    auto nv = ext.append_node(my_node::leaf);
    bool ok =
        std::holds_alternative<larch::node_view<ext_t, my_node::leaf>>(nv);
    assert(ok);
  }

  // 4. test_mutation_through_extend — Append node/edge through extend, visible
  // in both
  {
    dag_t d;
    ext_t ext{d};

    auto r = ext.append_node<my_node::root>();
    r.name() = "shared";

    // Visible through base
    std::size_t base_count = 0;
    for (auto nv : d.get_all_nodes()) {
      (void)nv;
      ++base_count;
    }
    assert(base_count == 1);

    // Check annotation through base
    auto base_r = d.get_node(r.index());
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.name(); }) {
            assert(v.name() == "shared");
          }
        },
        base_r);
  }

  // 5. test_get_children_parents — Navigation works through extend
  {
    dag_t d;
    ext_t ext{d};

    auto r = ext.append_node<my_node::root>();
    auto b1 = ext.append_node<my_node::branch>();
    auto b2 = ext.append_node<my_node::branch>();

    auto e1 = ext.append_edge<my_edge::structural>();
    e1.set_parent(r);
    e1.set_child(b1);

    auto e2 = ext.append_edge<my_edge::structural>();
    e2.set_parent(r);
    e2.set_child(b2);

    std::size_t child_count = 0;
    for (auto ev : r.get_children()) {
      (void)ev;
      ++child_count;
    }
    assert(child_count == 2);

    std::size_t parent_count = 0;
    for (auto ev : b1.get_parents()) {
      (void)ev;
      ++parent_count;
    }
    assert(parent_count == 1);
  }

  // 6. test_remove_through_extend — Remove works through extend
  {
    dag_t d;
    ext_t ext{d};

    auto r = ext.append_node<my_node::root>();
    auto l = ext.append_node<my_node::leaf>();
    auto e = ext.append_edge<my_edge::structural>();
    e.set_parent(r);
    e.set_child(l);

    e.remove();

    std::size_t edge_count = 0;
    for (auto ev : ext.get_all_edges()) {
      (void)ev;
      ++edge_count;
    }
    assert(edge_count == 0);
  }

  // 7. test_high_mark — High mark works through extend
  {
    dag_t d;
    ext_t ext{d};

    assert(ext.node_high_mark() == 0);
    auto r = ext.append_node<my_node::root>();
    assert(ext.node_high_mark() > 0);
    assert(ext.node_high_mark() == d.node_high_mark());
  }

  // 8. test_base_accessor
  {
    dag_t d;
    ext_t ext{d};
    assert(&ext.base() == &d);
  }

  std::println("All extend_dag tests passed");
  return 0;
}

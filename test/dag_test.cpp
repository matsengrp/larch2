#include <larch/dag.hpp>

#include <cassert>
#include <cstddef>
#include <print>
#include <string>
#include <variant>

enum class my_node { root, branch, leaf };
enum class my_edge { structural, reference };

// Annotations
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

// Interfaces
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

int main() {
  // 1. Empty DAG construction
  {
    dag_t d;
    std::size_t count = 0;
    for (auto n : d.get_all_nodes()) {
      (void)n;
      ++count;
    }
    assert(count == 0);
    count = 0;
    for (auto e : d.get_all_edges()) {
      (void)e;
      ++count;
    }
    assert(count == 0);
  }

  // 2. append_node and access annotation via view
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    r.name() = "hello";
    assert(r.name() == "hello");
  }

  // 3. Interface method works (deducing this -> get_annotation)
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    r.name() = "world";
    assert(r.name() == "world");

    auto b = d.append_node<my_node::branch>();
    b.priority() = 42;
    assert(b.priority() == 42);

    auto l = d.append_node<my_node::leaf>();
    l.value() = 3.14;
    assert(l.value() == 3.14);
  }

  // 4. append_edge + set_child/set_parent
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    auto l = d.append_node<my_node::leaf>();
    auto e = d.append_edge<my_edge::structural>();
    e.set_parent(r);
    e.set_child(l);
    e.weight() = 10;
    assert(e.weight() == 10);

    auto parent_var = e.get_parent();
    {
      bool ok = std::holds_alternative<larch::node_view<dag_t, my_node::root>>(
          parent_var);
      assert(ok);
    }
    auto child_var = e.get_child();
    {
      bool ok = std::holds_alternative<larch::node_view<dag_t, my_node::leaf>>(
          child_var);
      assert(ok);
    }
  }

  // 5. append_child / append_parent
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    auto l = d.append_node<my_node::leaf>();
    auto e = r.template append_child<my_edge::structural>();
    e.set_child(l);
    e.weight() = 5;
    assert(e.weight() == 5);
  }

  // 6. get_children / get_parents iteration
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    auto b1 = d.append_node<my_node::branch>();
    auto b2 = d.append_node<my_node::branch>();

    auto e1 = d.append_edge<my_edge::structural>();
    e1.set_parent(r);
    e1.set_child(b1);

    auto e2 = d.append_edge<my_edge::structural>();
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

  // 7. Runtime append_node(kind) / append_edge(kind) returning variants
  {
    dag_t d;
    auto nv = d.append_node(my_node::leaf);
    {
      bool ok =
          std::holds_alternative<larch::node_view<dag_t, my_node::leaf>>(nv);
      assert(ok);
    }
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.value(); }) {
            v.value() = 99.0;
            assert(v.value() == 99.0);
          }
        },
        nv);

    auto ev = d.append_edge(my_edge::reference);
    {
      bool ok =
          std::holds_alternative<larch::edge_view<dag_t, my_edge::reference>>(
              ev);
      assert(ok);
    }
  }

  // 8. set_root / get_root / get_root_as
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    r.name() = "root_node";
    d.set_root(r);

    auto rv = d.get_root();
    std::visit(
        [](auto& v) {
          if constexpr (requires { v.name(); }) {
            assert(v.name() == "root_node");
          } else {
            assert(false);
          }
        },
        rv);

    auto r2 = d.get_root_as<my_node::root>();
    assert(r2.name() == "root_node");
  }

  // 9. get_all_nodes / get_all_edges iteration
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    auto b = d.append_node<my_node::branch>();
    auto l = d.append_node<my_node::leaf>();
    auto e1 = d.append_edge<my_edge::structural>();
    e1.set_parent(r);
    e1.set_child(b);
    auto e2 = d.append_edge<my_edge::reference>();
    e2.set_parent(b);
    e2.set_child(l);

    std::size_t node_count = 0;
    for (auto nv : d.get_all_nodes()) {
      (void)nv;
      ++node_count;
    }
    assert(node_count == 3);

    std::size_t edge_count = 0;
    for (auto ev : d.get_all_edges()) {
      (void)ev;
      ++edge_count;
    }
    assert(edge_count == 2);
  }

  // 10. node_view::remove() cascades edge removal
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    auto b = d.append_node<my_node::branch>();
    auto l = d.append_node<my_node::leaf>();

    auto e1 = d.append_edge<my_edge::structural>();
    e1.set_parent(r);
    e1.set_child(b);

    auto e2 = d.append_edge<my_edge::structural>();
    e2.set_parent(b);
    e2.set_child(l);

    // Remove branch — should remove both edges
    b.remove();

    std::size_t node_count = 0;
    for (auto nv : d.get_all_nodes()) {
      (void)nv;
      ++node_count;
    }
    assert(node_count == 2);

    std::size_t edge_count = 0;
    for (auto ev : d.get_all_edges()) {
      (void)ev;
      ++edge_count;
    }
    assert(edge_count == 0);

    // r and l should have no children/parents now
    std::size_t r_children = 0;
    for (auto ev : r.get_children()) {
      (void)ev;
      ++r_children;
    }
    assert(r_children == 0);

    std::size_t l_parents = 0;
    for (auto ev : l.get_parents()) {
      (void)ev;
      ++l_parents;
    }
    assert(l_parents == 0);
  }

  // 11. edge_view::remove()
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    auto l = d.append_node<my_node::leaf>();
    auto e = d.append_edge<my_edge::structural>();
    e.set_parent(r);
    e.set_child(l);

    e.remove();

    std::size_t edge_count = 0;
    for (auto ev : d.get_all_edges()) {
      (void)ev;
      ++edge_count;
    }
    assert(edge_count == 0);

    std::size_t r_children = 0;
    for (auto ev : r.get_children()) {
      (void)ev;
      ++r_children;
    }
    assert(r_children == 0);

    std::size_t l_parents = 0;
    for (auto ev : l.get_parents()) {
      (void)ev;
      ++l_parents;
    }
    assert(l_parents == 0);
  }

  // 12. Multiple node/edge kinds in same DAG
  {
    dag_t d;
    auto r = d.append_node<my_node::root>();
    auto b = d.append_node<my_node::branch>();
    auto l = d.append_node<my_node::leaf>();

    r.name() = "top";
    b.priority() = 7;
    l.value() = 2.718;

    auto e1 = d.append_edge<my_edge::structural>();
    e1.set_parent(r);
    e1.set_child(b);
    e1.weight() = 100;

    auto e2 = d.append_edge<my_edge::reference>();
    e2.set_parent(r);
    e2.set_child(l);
    e2.label() = "ref";

    assert(r.name() == "top");
    assert(b.priority() == 7);
    assert(l.value() == 2.718);
    assert(e1.weight() == 100);
    assert(e2.label() == "ref");
  }

  std::println("All dag tests passed");
  return 0;
}

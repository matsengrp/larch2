#include <larch/compute.hpp>

#include "test_util.hpp"

#include <cassert>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace larch;
using larch::test::cg_from_sequence;

static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx, std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
}

// Small valid tree: UA -> inner -> {leaf1, leaf2}
static phylo_dag make_valid_dag() {
  constexpr std::string_view ref = "GAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto inner = d.append_node<node_kind::inner>();
  inner.cg() = cg_from_sequence("GAA", ref);

  auto leaf1 = d.append_node<node_kind::leaf>();
  leaf1.cg() = cg_from_sequence("ACC", ref);
  leaf1.sample_id() = "leaf1";

  auto leaf2 = d.append_node<node_kind::leaf>();
  leaf2.cg() = cg_from_sequence("GTT", ref);
  leaf2.sample_id() = "leaf2";

  add_edge(d, ua.index(), inner.index(), 0);
  add_edge(d, inner.index(), leaf1.index(), 0);
  add_edge(d, inner.index(), leaf2.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

int main() {
  // Test 1: valid DAG passes
  {
    auto d = make_valid_dag();
    validate_dag(d, "valid");  // should not throw
    std::println("valid DAG: PASS");
  }

  // Test 2: orphan node (check #5)
  {
    auto d = make_valid_dag();
    auto orphan = d.append_node<node_kind::leaf>();
    orphan.cg() = compact_genome{};
    orphan.sample_id() = "orphan";
    bool caught = false;
    try {
      validate_dag(d, "orphan");
    } catch (std::runtime_error const&) {
      caught = true;
    }
    assert(caught);
    std::println("orphan node: PASS");
  }

  // Test 3: leaf with children (check #4)
  {
    auto d = make_valid_dag();
    // Add an extra leaf, then add an edge from an existing leaf to it
    auto extra_leaf = d.append_node<node_kind::leaf>();
    extra_leaf.cg() = compact_genome{};
    extra_leaf.sample_id() = "extra";
    // leaf1 is at index 2 (ua=0, inner=1, leaf1=2, leaf2=3)
    add_edge(d, 2, extra_leaf.index(), 0);
    bool caught = false;
    try {
      validate_dag(d, "leaf-with-child");
    } catch (std::runtime_error const&) {
      caught = true;
    }
    assert(caught);
    std::println("leaf with children: PASS");
  }

  // Test 4: clade gap (check #7)
  {
    auto d = make_valid_dag();
    // Change clade_index of one edge from 1 to 5, creating a gap
    bool modified = false;
    for (auto ev : d.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            if (!modified && edge.clade_index() == 1) {
              edge.clade_index() = 5;
              modified = true;
            }
          },
          ev);
      if (modified) break;
    }
    bool caught = false;
    try {
      validate_dag(d, "clade-gap");
    } catch (std::runtime_error const&) {
      caught = true;
    }
    assert(caught);
    std::println("clade gap: PASS");
  }

  // Test 5: parallel version passes on valid DAG
  {
    auto d = make_valid_dag();
    auto& pool = thread_pool::get_default();
    validate_dag(d, "parallel-valid", pool);  // should not throw
    std::println("parallel valid DAG: PASS");
  }

  // Test 6: parallel version catches orphan
  {
    auto d = make_valid_dag();
    auto orphan = d.append_node<node_kind::leaf>();
    orphan.cg() = compact_genome{};
    orphan.sample_id() = "orphan";
    auto& pool = thread_pool::get_default();
    bool caught = false;
    try {
      validate_dag(d, "parallel-orphan", pool);
    } catch (std::runtime_error const&) {
      caught = true;
    }
    assert(caught);
    std::println("parallel orphan: PASS");
  }

  std::println("All validate_dag tests passed.");
}

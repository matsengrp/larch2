#pragma once

#include <larch/pmr_arena.hpp>
#include <larch/phylo_dag.hpp>

#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory_resource>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <variant>

namespace larch {

inline std::string const& get_reference_sequence(phylo_dag& d) {
  return d.get_root_as<node_kind::ua>().reference_sequence();
}

inline std::size_t node_count(phylo_dag& d) { return d.node_count(); }

inline std::size_t edge_count(phylo_dag& d) { return d.edge_count(); }

inline void recompute_compact_genomes(phylo_dag& d) {
  auto const& ref = get_reference_sequence(d);
  auto ua = d.get_root_as<node_kind::ua>();

  // BFS from UA's children (with visited set to handle DAGs)
  scoped_arena<4096> arena;
  auto* mr = arena.get();
  std::queue<std::size_t, std::pmr::deque<std::size_t>> q{
      std::pmr::deque<std::size_t>(mr)};
  std::pmr::unordered_set<std::size_t> visited(mr);
  for (auto edge_var : ua.get_children()) {
    std::visit(
        [&](auto edge) {
          std::visit(
              [&](auto child) {
                if (visited.insert(child.index()).second) q.push(child.index());
              },
              edge.get_child());
        },
        edge_var);
  }

  while (!q.empty()) {
    auto node_idx = q.front();
    q.pop();

    auto nv = d.get_node(node_idx);
    std::visit(
        [&](auto node) {
          // Get parent edge mutations and parent CG
          for (auto pe_var : node.get_parents()) {
            std::visit(
                [&](auto pe) {
                  auto& edge_muts = pe.mutations();
                  compact_genome parent_cg;
                  std::visit(
                      [&](auto parent) {
                        if constexpr (requires { parent.cg(); }) {
                          parent_cg = parent.cg();
                        }
                      },
                      pe.get_parent());

                  if constexpr (requires { node.cg(); }) {
                    node.cg() = compact_genome{};
                    node.cg().add_parent_edge(edge_muts, parent_cg, ref);
                  }
                },
                pe_var);
            break;  // Only use first parent (tree case)
          }

          // Enqueue children
          for (auto ce_var : node.get_children()) {
            std::visit(
                [&](auto ce) {
                  std::visit(
                      [&](auto child) {
                        if (visited.insert(child.index()).second)
                          q.push(child.index());
                      },
                      ce.get_child());
                },
                ce_var);
          }
        },
        nv);
  }
}

inline void recompute_edge_mutations(phylo_dag& d) {
  auto const& ref = get_reference_sequence(d);

  for (auto edge_var : d.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          compact_genome parent_cg;
          std::visit(
              [&](auto parent) {
                if constexpr (requires { parent.cg(); }) {
                  parent_cg = parent.cg();
                }
              },
              edge.get_parent());

          compact_genome child_cg;
          std::visit(
              [&](auto child) {
                if constexpr (requires { child.cg(); }) {
                  child_cg = child.cg();
                }
              },
              edge.get_child());

          edge.mutations() =
              compact_genome::to_edge_mutations(ref, parent_cg, child_cg);
        },
        edge_var);
  }
}

inline bool is_leaf(phylo_dag& d, std::size_t node_idx) {
  auto nv = d.get_node(node_idx);
  return std::visit(
      [](auto n) {
        std::size_t count = 0;
        for (auto e : n.get_children()) {
          (void)e;
          ++count;
        }
        return count == 0;
      },
      nv);
}

// Groups a node's child edges by clade_index.
// Returns vector where result[i] = edge indices for clade i.
inline auto get_clades(phylo_dag& d, std::size_t node_idx)
    -> std::vector<std::vector<std::size_t>> {
  std::vector<std::vector<std::size_t>> clades;
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          std::visit(
              [&](auto edge) {
                auto ci = edge.clade_index();
                if (ci >= clades.size()) clades.resize(ci + 1);
                clades[ci].push_back(edge.index());
              },
              ev);
        }
      },
      nv);
  return clades;
}

inline auto get_clades(phylo_dag& d, std::size_t node_idx,
                       std::pmr::memory_resource* mr)
    -> std::pmr::vector<std::pmr::vector<std::size_t>> {
  std::pmr::vector<std::pmr::vector<std::size_t>> clades(mr);
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          std::visit(
              [&](auto edge) {
                auto ci = edge.clade_index();
                if (ci >= clades.size()) clades.resize(ci + 1);
                clades[ci].push_back(edge.index());
              },
              ev);
        }
      },
      nv);
  return clades;
}

inline void set_sample_ids_from_cg(phylo_dag& d, bool coerce = false) {
  // Set leaf sample_ids from compact genome strings.
  //
  // When coerce=false (default): only assigns to leaves with empty sample_ids,
  // disambiguating duplicates with "#N" suffix.
  //
  // When coerce=true: overwrites ALL leaf sample_ids with the CG string.
  // Two leaves with the same CG get the same sample_id (no disambiguation).
  // This matches the original larch's SampleIdsFromCG(true) behavior,
  // which the merge algorithm relies on: leaves with identical compact
  // genomes are treated as the same leaf for deduplication purposes.
  if (coerce) {
    for (auto nv : d.get_all_nodes()) {
      std::visit(
          [](auto node) {
            if constexpr (requires {
                            node.sample_id();
                            node.cg();
                          }) {
              node.sample_id() = node.cg().to_string();
            }
          },
          nv);
    }
  } else {
    std::unordered_map<std::string, std::size_t> cg_str_count;
    for (auto nv : d.get_all_nodes()) {
      std::visit(
          [&](auto node) {
            if constexpr (requires {
                            node.sample_id();
                            node.cg();
                          }) {
              if (node.sample_id().empty()) {
                auto cg_str = node.cg().to_string();
                auto count = cg_str_count[cg_str]++;
                if (count > 0) {
                  node.sample_id() = cg_str + "#" + std::to_string(count);
                } else {
                  node.sample_id() = cg_str;
                }
              }
            }
          },
          nv);
    }
  }
}

inline std::size_t get_child_idx(phylo_dag& d, std::size_t edge_idx) {
  auto ev = d.get_edge(edge_idx);
  return std::visit(
      [](auto edge) {
        auto cv = edge.get_child();
        return std::visit([](auto child) { return child.index(); }, cv);
      },
      ev);
}

inline std::size_t get_parent_idx(phylo_dag& d, std::size_t edge_idx) {
  auto ev = d.get_edge(edge_idx);
  return std::visit(
      [](auto edge) {
        auto pv = edge.get_parent();
        return std::visit([](auto parent) { return parent.index(); }, pv);
      },
      ev);
}

inline std::size_t get_root_idx(phylo_dag& d) {
  auto rv = d.get_root();
  return std::visit([](auto n) { return n.index(); }, rv);
}

inline std::vector<std::size_t> get_parent_edges(phylo_dag& d,
                                                 std::size_t node_idx) {
  std::vector<std::size_t> result;
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

inline std::size_t leaf_count(phylo_dag& d) {
  std::size_t count = 0;
  for (auto nv : d.get_all_nodes()) {
    if (std::visit(
            [](auto n) {
              std::size_t c = 0;
              for (auto e : n.get_children()) {
                (void)e;
                ++c;
              }
              return c == 0;
            },
            nv))
      ++count;
  }
  return count;
}

inline bool is_tree(phylo_dag& d) { return edge_count(d) == node_count(d) - 1; }

inline bool is_ua(phylo_dag& d, std::size_t node_idx) {
  auto nv = d.get_node(node_idx);
  return std::visit(
      [](auto n) {
        return std::is_same_v<std::remove_cvref_t<decltype(n)>,
                              node_view<phylo_dag, node_kind::ua>>;
      },
      nv);
}

inline std::size_t get_clade_idx(phylo_dag& d, std::size_t edge_idx) {
  auto ev = d.get_edge(edge_idx);
  return std::visit([](auto edge) { return edge.clade_index(); }, ev);
}

inline void validate_dag(phylo_dag& d, std::string_view label) {
  auto fail = [&](std::string const& msg) {
    throw std::runtime_error("validate_dag [" + std::string{label} +
                             "]: " + msg);
  };

  // 1. Root is UA
  auto root_idx = get_root_idx(d);
  if (!is_ua(d, root_idx)) fail("root node is not UA");

  // 2. Root has no parents
  if (!get_parent_edges(d, root_idx).empty()) fail("root node has parents");

  // 3. Edge consistency
  for (auto ev : d.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto eidx = edge.index();

          auto pv = edge.get_parent();
          auto parent_idx = std::visit([](auto p) { return p.index(); }, pv);
          auto cv = edge.get_child();
          auto child_idx = std::visit([](auto c) { return c.index(); }, cv);

          if (parent_idx == no_idx)
            fail("edge " + std::to_string(eidx) + " has no parent");
          if (child_idx == no_idx)
            fail("edge " + std::to_string(eidx) + " has no child");

          // Verify parent lists this edge as a child edge
          bool found_in_parent = false;
          auto pnv = d.get_node(parent_idx);
          std::visit(
              [&](auto pnode) {
                for (auto ce : pnode.get_children()) {
                  std::visit(
                      [&](auto e) {
                        if (e.index() == eidx) found_in_parent = true;
                      },
                      ce);
                }
              },
              pnv);
          if (!found_in_parent)
            fail("edge " + std::to_string(eidx) + " not in parent node " +
                 std::to_string(parent_idx) + "'s children");

          // Verify child lists this edge as a parent edge
          bool found_in_child = false;
          auto cnv = d.get_node(child_idx);
          std::visit(
              [&](auto cnode) {
                for (auto pe : cnode.get_parents()) {
                  std::visit(
                      [&](auto e) {
                        if (e.index() == eidx) found_in_child = true;
                      },
                      pe);
                }
              },
              cnv);
          if (!found_in_child)
            fail("edge " + std::to_string(eidx) + " not in child node " +
                 std::to_string(child_idx) + "'s parents");
        },
        ev);
  }

  // 4. Leaf nodes have no children
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            std::size_t count = 0;
            for (auto e : node.get_children()) {
              (void)e;
              ++count;
            }
            if (count > 0)
              fail("leaf node " + std::to_string(node.index()) + " has " +
                   std::to_string(count) + " children");
          }
        },
        nv);
  }

  // 5. No orphan nodes — BFS from root
  scoped_arena<8192> arena;
  auto* mr = arena.get();
  std::queue<std::size_t, std::pmr::deque<std::size_t>> q{
      std::pmr::deque<std::size_t>(mr)};
  std::pmr::unordered_set<std::size_t> reachable(mr);
  reachable.insert(root_idx);
  q.push(root_idx);
  while (!q.empty()) {
    auto idx = q.front();
    q.pop();
    auto nv = d.get_node(idx);
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto cv = edge.get_child();
                  auto cidx = std::visit([](auto c) { return c.index(); }, cv);
                  if (reachable.insert(cidx).second) q.push(cidx);
                },
                ev);
          }
        },
        nv);
  }
  for (auto nv : d.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    if (!reachable.count(idx))
      fail("node " + std::to_string(idx) + " is not reachable from root");
  }

  // 6. Clade disjointness — each child appears in at most one clade
  for (auto nv : d.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    if (is_leaf(d, idx)) continue;

    auto clades = get_clades(d, idx, mr);
    std::pmr::set<std::size_t> seen_children(mr);
    for (std::size_t ci = 0; ci < clades.size(); ++ci) {
      for (auto eidx : clades[ci]) {
        auto child_idx = get_child_idx(d, eidx);
        if (!seen_children.insert(child_idx).second)
          fail("node " + std::to_string(idx) + ": child " +
               std::to_string(child_idx) + " appears in multiple clades");
      }
    }
  }

  // 7. Contiguous clade indices (0..n-1 with no gaps)
  for (auto nv : d.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    if (is_leaf(d, idx)) continue;

    auto clades = get_clades(d, idx, mr);
    for (std::size_t ci = 0; ci < clades.size(); ++ci) {
      if (clades[ci].empty())
        fail("node " + std::to_string(idx) + ": clade index " +
             std::to_string(ci) + " is empty (gap in indices)");
    }
  }

  std::cerr << "validate_dag [" << label << "]: OK (" << node_count(d)
            << " nodes, " << edge_count(d) << " edges)\n";
}

}  // namespace larch

// Parallel overload — include thread_pool after ogi namespace closes to avoid
// circular includes, then reopen namespace.
#include <larch/thread_pool.hpp>

namespace larch {

inline void validate_dag(phylo_dag& d, std::string_view label,
                         thread_pool& pool) {
  auto fail = [&](std::string const& msg) {
    throw std::runtime_error("validate_dag [" + std::string{label} +
                             "]: " + msg);
  };

  // 1. Root is UA
  auto root_idx = get_root_idx(d);
  if (!is_ua(d, root_idx)) fail("root node is not UA");

  // 2. Root has no parents
  if (!get_parent_edges(d, root_idx).empty()) fail("root node has parents");

  // 3. Edge consistency — node-centric (O(nodes+edges) instead of O(sum(k^2)))
  //    For each edge: verify parent and child are set.
  //    For each node: verify every child edge points back to this node as
  //    parent, and every parent edge points back to this node as child.
  for (auto ev : d.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto eidx = edge.index();
          auto pv = edge.get_parent();
          auto parent_idx = std::visit([](auto p) { return p.index(); }, pv);
          auto cv = edge.get_child();
          auto child_idx = std::visit([](auto c) { return c.index(); }, cv);

          if (parent_idx == no_idx)
            fail("edge " + std::to_string(eidx) + " has no parent");
          if (child_idx == no_idx)
            fail("edge " + std::to_string(eidx) + " has no child");
        },
        ev);
  }

  {
    std::vector<std::size_t> node_indices;
    node_indices.reserve(node_count(d));
    for (auto nv : d.get_all_nodes()) {
      node_indices.push_back(std::visit([](auto n) { return n.index(); }, nv));
    }

    parallel_for_each(pool, node_indices, [&](std::size_t nidx) {
      auto nv = d.get_node(nidx);
      std::visit(
          [&](auto node) {
            // Every child edge must have this node as parent
            for (auto ce : node.get_children()) {
              std::visit(
                  [&](auto e) {
                    auto pv = e.get_parent();
                    auto pidx =
                        std::visit([](auto p) { return p.index(); }, pv);
                    if (pidx != nidx)
                      throw std::runtime_error(
                          "validate_dag [" + std::string{label} +
                          "]: child edge " + std::to_string(e.index()) +
                          " of node " + std::to_string(nidx) + " has parent " +
                          std::to_string(pidx));
                  },
                  ce);
            }
            // Every parent edge must have this node as child
            for (auto pe : node.get_parents()) {
              std::visit(
                  [&](auto e) {
                    auto cv = e.get_child();
                    auto cidx =
                        std::visit([](auto c) { return c.index(); }, cv);
                    if (cidx != nidx)
                      throw std::runtime_error(
                          "validate_dag [" + std::string{label} +
                          "]: parent edge " + std::to_string(e.index()) +
                          " of node " + std::to_string(nidx) + " has child " +
                          std::to_string(cidx));
                  },
                  pe);
            }
          },
          nv);
    });
  }

  // 3b. No orphan edges — total children across all nodes must equal edge
  // count.
  {
    std::size_t adj_edges = 0;
    for (auto nv : d.get_all_nodes()) {
      std::visit(
          [&](auto node) {
            for (auto e : node.get_children()) {
              (void)e;
              ++adj_edges;
            }
          },
          nv);
    }
    if (adj_edges != edge_count(d))
      fail("adjacency child-edge count (" + std::to_string(adj_edges) +
           ") != total edge count (" + std::to_string(edge_count(d)) + ")");
  }

  // 4. Leaf nodes have no children
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            std::size_t count = 0;
            for (auto e : node.get_children()) {
              (void)e;
              ++count;
            }
            if (count > 0)
              fail("leaf node " + std::to_string(node.index()) + " has " +
                   std::to_string(count) + " children");
          }
        },
        nv);
  }

  // 5. No orphan nodes — BFS from root
  {
    scoped_arena<8192> bfs_arena;
    auto* bfs_mr = bfs_arena.get();
    std::queue<std::size_t, std::pmr::deque<std::size_t>> q{
        std::pmr::deque<std::size_t>(bfs_mr)};
    std::pmr::unordered_set<std::size_t> reachable(bfs_mr);
    reachable.insert(root_idx);
    q.push(root_idx);
    while (!q.empty()) {
      auto idx = q.front();
      q.pop();
      auto nv = d.get_node(idx);
      std::visit(
          [&](auto node) {
            for (auto ev : node.get_children()) {
              std::visit(
                  [&](auto edge) {
                    auto cv = edge.get_child();
                    auto cidx =
                        std::visit([](auto c) { return c.index(); }, cv);
                    if (reachable.insert(cidx).second) q.push(cidx);
                  },
                  ev);
            }
          },
          nv);
    }
    for (auto nv : d.get_all_nodes()) {
      auto idx = std::visit([](auto n) { return n.index(); }, nv);
      if (!reachable.count(idx))
        fail("node " + std::to_string(idx) + " is not reachable from root");
    }
  }

  // 6+7. Clade disjointness + contiguous indices — parallel
  {
    std::vector<std::size_t> non_leaf_indices;
    for (auto nv : d.get_all_nodes()) {
      auto idx = std::visit([](auto n) { return n.index(); }, nv);
      if (!is_leaf(d, idx)) non_leaf_indices.push_back(idx);
    }

    parallel_for_each(pool, non_leaf_indices, [&](std::size_t idx) {
      scoped_arena<4096> arena;
      auto* mr = arena.get();

      auto clades = get_clades(d, idx, mr);

      // Check #6: clade disjointness
      std::pmr::set<std::size_t> seen_children(mr);
      for (std::size_t ci = 0; ci < clades.size(); ++ci) {
        for (auto eidx : clades[ci]) {
          auto child_idx = get_child_idx(d, eidx);
          if (!seen_children.insert(child_idx).second)
            throw std::runtime_error("validate_dag [" + std::string{label} +
                                     "]: node " + std::to_string(idx) +
                                     ": child " + std::to_string(child_idx) +
                                     " appears in multiple clades");
        }
      }

      // Check #7: contiguous clade indices
      for (std::size_t ci = 0; ci < clades.size(); ++ci) {
        if (clades[ci].empty())
          throw std::runtime_error("validate_dag [" + std::string{label} +
                                   "]: node " + std::to_string(idx) +
                                   ": clade index " + std::to_string(ci) +
                                   " is empty (gap in indices)");
      }
    });
  }

  std::cerr << "validate_dag [" << label << "]: OK (" << node_count(d)
            << " nodes, " << edge_count(d) << " edges)\n";
}

}  // namespace larch

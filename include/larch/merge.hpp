#pragma once

#include <larch/hash_chain.hpp>
#include <larch/pmr_arena.hpp>
#include <larch/phylo_dag.hpp>
#include <larch/compact_genome.hpp>
#include <larch/edge_mutations.hpp>
#include <larch/leaf_set.hpp>
#include <larch/node_label.hpp>
#include <larch/edge_label.hpp>
#include <larch/compute.hpp>
#include <larch/thread_pool.hpp>

#include <algorithm>
#include <cassert>
#include <map>
#include <future>
#include <memory_resource>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace larch {

class merge {
  std::string reference_sequence_;

  // Phase 1: Deduplicated compact genomes (set mode)
  hash_chain<compact_genome> all_cgs_;

  // Phase 1b: Deduplicated leaf identities (sample_id -> unique index)
  hash_chain<std::string> all_leaf_ids_;

  // Phase 2: Deduplicated leaf sets
  hash_chain<leaf_set> all_leaf_sets_;

  // Phase 3: Deduplicated nodes (map: node_label -> result node index)
  hash_chain<node_label, std::size_t> result_nodes_;

  // Phase 4: Deduplicated edges (map: edge_label -> result edge index)
  hash_chain<edge_label, std::size_t> result_edges_;

  // CG index for sample_ids (leaf sample_id -> cg_idx)
  std::unordered_map<std::string, std::size_t> sample_id_to_cg_idx_;

  // The result DAG
  phylo_dag result_dag_;
  bool result_built_ = false;

  // DAG node index -> node_label (populated in build_result)
  std::unordered_map<std::size_t, node_label> dag_idx_to_label_;

  // Per-DAG node labels
  struct dag_info {
    // node_idx in source DAG -> node_label
    std::unordered_map<std::size_t, node_label> labels;
  };

  // Local result from processing a single DAG (no shared state)
  struct dag_local_result {
    hash_chain<compact_genome> cgs;
    hash_chain<std::string> leaf_ids;  // sample_id -> unique leaf index
    hash_chain<leaf_set> leaf_sets;
    hash_chain<node_label, std::size_t> nodes;
    hash_chain<edge_label, std::size_t> edges;
    std::vector<std::pair<std::string, std::size_t>> sample_mappings;
  };

  // Thread pool and pending futures
  thread_pool& pool_;
  std::vector<std::future<dag_local_result>> pending_;

  // Get CG for a node variant
  static compact_genome get_node_cg(phylo_dag::node_variant_type const& nv) {
    compact_genome cg;
    std::visit(
        [&](auto const& n) {
          if constexpr (requires { n.cg(); }) {
            cg = n.cg();
          }
        },
        nv);
    return cg;
  }

  static std::string get_node_sample_id(
      phylo_dag::node_variant_type const& nv) {
    std::string sid;
    std::visit(
        [&](auto const& n) {
          if constexpr (requires { n.sample_id(); }) {
            sid = n.sample_id();
          }
        },
        nv);
    return sid;
  }

  static bool is_ua(phylo_dag::node_variant_type const& nv) {
    return std::visit(
        [](auto const& n) {
          return std::is_same_v<std::remove_cvref_t<decltype(n)>,
                                node_view<phylo_dag, node_kind::ua>>;
        },
        nv);
  }

  static bool is_leaf(phylo_dag::node_variant_type const& nv) {
    return std::visit(
        [](auto const& n) {
          return std::is_same_v<std::remove_cvref_t<decltype(n)>,
                                node_view<phylo_dag, node_kind::leaf>>;
        },
        nv);
  }

  // Collect nodes in postorder (children before parents)
  static std::pmr::vector<std::size_t> postorder(
      phylo_dag& d, std::pmr::memory_resource* mr) {
    std::pmr::vector<std::size_t> result(mr);
    std::size_t root_idx = 0;
    std::visit([&](auto r) { root_idx = r.index(); }, d.get_root());

    // DFS postorder (with visited set to handle DAGs)
    std::pmr::unordered_set<std::size_t> visited(mr);
    std::pmr::vector<std::pair<std::size_t, bool>> stack(mr);
    stack.push_back({root_idx, false});
    visited.insert(root_idx);

    while (!stack.empty()) {
      auto [idx, expanded] = stack.back();
      if (expanded) {
        stack.pop_back();
        result.push_back(idx);
        continue;
      }
      stack.back().second = true;

      // Enqueue children
      std::visit(
          [&](auto n) {
            std::vector<std::size_t> children;
            for (auto ev : n.get_children()) {
              std::visit(
                  [&](auto e) {
                    auto cv = e.get_child();
                    std::visit(
                        [&](auto c) {
                          if (visited.insert(c.index()).second)
                            children.push_back(c.index());
                        },
                        cv);
                  },
                  ev);
            }
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
              stack.push_back({*it, false});
            }
          },
          d.get_node(idx));
    }
    return result;
  }

  // Get children grouped by clade
  struct clade_child {
    std::size_t clade_idx;
    std::size_t child_node_idx;
  };

  static std::pmr::vector<clade_child> get_clade_children(
      phylo_dag& d, std::size_t node_idx, std::pmr::memory_resource* mr) {
    std::pmr::vector<clade_child> result(mr);
    std::visit(
        [&](auto n) {
          for (auto ev : n.get_children()) {
            std::visit(
                [&](auto e) {
                  auto cv = e.get_child();
                  std::visit(
                      [&](auto c) {
                        result.push_back({e.clade_index(), c.index()});
                      },
                      cv);
                },
                ev);
          }
        },
        d.get_node(node_idx));
    return result;
  }

  // Process a DAG with fully local state — no access to shared members.
  static dag_local_result process_dag_locally(phylo_dag& d) {
    scoped_arena<32768> arena;
    auto* mr = arena.get();

    dag_local_result result;
    dag_info info;

    // Phase 1: MergeCompactGenomes
    for (auto nv : d.get_all_nodes()) {
      std::size_t node_idx = std::visit([](auto n) { return n.index(); }, nv);

      if (is_ua(nv)) {
        // UA: empty CG, special label
        info.labels[node_idx] = node_label{};
        continue;
      }

      auto cg = get_node_cg(nv);
      auto sid = get_node_sample_id(nv);

      if (is_leaf(nv)) {
        // Leaf: store sample_id, map sample_id->cg, assign leaf_id
        auto [cg_idx, _] = result.cgs.insert(cg);
        auto [lid_idx, __] = result.leaf_ids.insert(sid);
        info.labels[node_idx].sample_id = sid;
        info.labels[node_idx].cg_idx = cg_idx;
        info.labels[node_idx].leaf_id_idx = lid_idx;
        result.sample_mappings.push_back({sid, cg_idx});
      } else {
        // Inner: store CG
        if (!cg.empty()) {
          auto [cg_idx, _] = result.cgs.insert(cg);
          info.labels[node_idx].cg_idx = cg_idx;
        }
      }
    }

    // Phase 2: ComputeLeafSets (bottom-up)
    auto order = postorder(d, mr);

    // Computed leaf sets per node (node_idx -> leaf_set)
    std::pmr::unordered_map<std::size_t, leaf_set> computed_ls(mr);

    for (auto node_idx : order) {
      auto nv = d.get_node(node_idx);

      if (is_ua(nv)) continue;

      if (is_leaf(nv)) {
        // Leaf: single clade containing this leaf's unique identity
        auto& label = info.labels[node_idx];
        std::vector<std::vector<std::size_t>> clades;
        clades.push_back({label.leaf_id_idx});
        leaf_set ls{std::move(clades)};
        computed_ls[node_idx] = ls;
        auto [ls_idx, _] = result.leaf_sets.insert(std::move(ls));
        label.ls_idx = ls_idx;
      } else {
        // Inner: collect children by clade
        auto clade_children = get_clade_children(d, node_idx, mr);

        // Group by clade_idx
        std::pmr::map<std::size_t, std::pmr::vector<std::size_t>> clade_groups(
            mr);
        for (auto& cc : clade_children) {
          clade_groups[cc.clade_idx].push_back(cc.child_node_idx);
        }

        std::vector<std::vector<std::size_t>> clades;
        for (auto& [clade_idx, children] : clade_groups) {
          std::pmr::vector<std::size_t> clade_leaves(mr);
          for (auto child_idx : children) {
            auto child_nv = d.get_node(child_idx);
            if (is_leaf(child_nv)) {
              clade_leaves.push_back(info.labels[child_idx].leaf_id_idx);
            } else {
              // Add all leaves from child's leaf set
              auto it = computed_ls.find(child_idx);
              if (it != computed_ls.end()) {
                for (auto& child_clade : it->second.clades()) {
                  clade_leaves.insert(clade_leaves.end(), child_clade.begin(),
                                      child_clade.end());
                }
              }
            }
          }
          std::sort(clade_leaves.begin(), clade_leaves.end());
          clade_leaves.erase(
              std::unique(clade_leaves.begin(), clade_leaves.end()),
              clade_leaves.end());
          clades.emplace_back(clade_leaves.begin(), clade_leaves.end());
        }
        std::sort(clades.begin(), clades.end());

        leaf_set ls{std::move(clades)};
        computed_ls[node_idx] = ls;
        auto [ls_idx, _] = result.leaf_sets.insert(std::move(ls));
        info.labels[node_idx].ls_idx = ls_idx;
      }
    }

    // Phase 3: MergeNodes
    for (auto node_idx : order) {
      auto nv = d.get_node(node_idx);
      if (is_ua(nv)) continue;

      auto& label = info.labels[node_idx];
      if (label.empty()) continue;

      result.nodes.insert({label, no_idx});
    }

    // Phase 4: MergeEdges
    for (auto ev : d.get_all_edges()) {
      std::visit(
          [&](auto e) {
            auto pv = e.get_parent();
            auto cv = e.get_child();
            auto parent_idx = std::visit([](auto n) { return n.index(); }, pv);
            auto child_idx = std::visit([](auto n) { return n.index(); }, cv);

            if (is_ua(pv)) return;  // skip UA edges

            auto& parent_label = info.labels[parent_idx];
            auto& child_label = info.labels[child_idx];
            if (parent_label.empty() || child_label.empty()) return;

            edge_label el{parent_label, child_label};
            result.edges.insert({el, no_idx});
          },
          ev);
    }

    return result;
  }

  // Merge a local result into global hash chains with index remapping.
  void merge_local_result(dag_local_result&& lr) {
    scoped_arena<> arena;
    auto* mr = arena.get();

    // Step 1: Build CG remap (local index -> global index)
    std::pmr::unordered_map<std::size_t, std::size_t> cg_remap(mr);

    for (auto it = lr.nodes.begin(); it != lr.nodes.end(); ++it) {
      auto& label = (*it).first;
      if (label.cg_idx != no_idx && !cg_remap.contains(label.cg_idx)) {
        auto [global_idx, _] = all_cgs_.insert(lr.cgs[label.cg_idx]);
        cg_remap[label.cg_idx] = global_idx;
      }
    }
    for (auto& [sid, local_cg_idx] : lr.sample_mappings) {
      if (!cg_remap.contains(local_cg_idx)) {
        auto [global_idx, _] = all_cgs_.insert(lr.cgs[local_cg_idx]);
        cg_remap[local_cg_idx] = global_idx;
      }
    }

    // Step 1b: Build leaf_id remap (local leaf_id index -> global)
    // Remap all leaf_ids since they appear in leaf_sets of ancestor nodes
    std::pmr::unordered_map<std::size_t, std::size_t> lid_remap(mr);

    for (auto it = lr.nodes.begin(); it != lr.nodes.end(); ++it) {
      auto& label = (*it).first;
      if (label.leaf_id_idx != no_idx &&
          !lid_remap.contains(label.leaf_id_idx)) {
        auto& local_sid = lr.leaf_ids[label.leaf_id_idx];
        auto [global_idx, _] = all_leaf_ids_.insert(local_sid);
        lid_remap[label.leaf_id_idx] = global_idx;
      }
    }

    // Step 2: Build leaf set remap (remap leaf_id indices inside clades)
    std::pmr::unordered_map<std::size_t, std::size_t> ls_remap(mr);

    for (auto it = lr.nodes.begin(); it != lr.nodes.end(); ++it) {
      auto& label = (*it).first;
      if (label.ls_idx != no_idx && !ls_remap.contains(label.ls_idx)) {
        auto& local_ls = lr.leaf_sets[label.ls_idx];
        std::vector<std::vector<std::size_t>> remapped_clades;
        for (auto& clade : local_ls.clades()) {
          std::pmr::vector<std::size_t> remapped_clade(mr);
          remapped_clade.reserve(clade.size());
          for (auto local_lid : clade) {
            remapped_clade.push_back(lid_remap.at(local_lid));
          }
          std::sort(remapped_clade.begin(), remapped_clade.end());
          remapped_clades.emplace_back(remapped_clade.begin(),
                                       remapped_clade.end());
        }
        std::sort(remapped_clades.begin(), remapped_clades.end());
        auto [global_idx, _] =
            all_leaf_sets_.insert(leaf_set{std::move(remapped_clades)});
        ls_remap[label.ls_idx] = global_idx;
      }
    }

    // Step 3: Remap and insert nodes
    for (auto it = lr.nodes.begin(); it != lr.nodes.end(); ++it) {
      node_label label = (*it).first;  // copy to modify
      if (label.cg_idx != no_idx) label.cg_idx = cg_remap.at(label.cg_idx);
      if (label.leaf_id_idx != no_idx)
        label.leaf_id_idx = lid_remap.at(label.leaf_id_idx);
      if (label.ls_idx != no_idx) label.ls_idx = ls_remap.at(label.ls_idx);
      result_nodes_.insert({label, no_idx});
    }

    // Step 4: Remap and insert edges
    for (auto it = lr.edges.begin(); it != lr.edges.end(); ++it) {
      edge_label el = (*it).first;  // copy to modify
      if (el.parent.cg_idx != no_idx)
        el.parent.cg_idx = cg_remap.at(el.parent.cg_idx);
      if (el.parent.leaf_id_idx != no_idx)
        el.parent.leaf_id_idx = lid_remap.at(el.parent.leaf_id_idx);
      if (el.parent.ls_idx != no_idx)
        el.parent.ls_idx = ls_remap.at(el.parent.ls_idx);
      if (el.child.cg_idx != no_idx)
        el.child.cg_idx = cg_remap.at(el.child.cg_idx);
      if (el.child.leaf_id_idx != no_idx)
        el.child.leaf_id_idx = lid_remap.at(el.child.leaf_id_idx);
      if (el.child.ls_idx != no_idx)
        el.child.ls_idx = ls_remap.at(el.child.ls_idx);
      result_edges_.insert({el, no_idx});
    }

    // Step 5: Update sample_id_to_cg_idx_
    for (auto& [sid, local_cg_idx] : lr.sample_mappings) {
      sample_id_to_cg_idx_[sid] = cg_remap.at(local_cg_idx);
    }
  }

  void drain_pending() {
    for (auto& fut : pending_) {
      merge_local_result(fut.get());
    }
    pending_.clear();
  }

  void build_result() {
    if (result_built_) return;

    scoped_arena<> arena;
    auto* mr = arena.get();

    auto ua = result_dag_.append_node<node_kind::ua>();
    ua.reference_sequence() = reference_sequence_;
    result_dag_.set_root(ua);

    // Assign node IDs
    std::size_t node_counter = 0;
    std::pmr::unordered_map<std::size_t, std::size_t> node_idx_to_dag_idx(mr);

    // Create a node for each unique node_label
    for (auto it = result_nodes_.begin(); it != result_nodes_.end(); ++it) {
      auto& [label, result_id] = *it;
      result_id = node_counter++;

      bool is_leaf_node = !label.sample_id.empty();
      std::size_t dag_idx;

      if (is_leaf_node) {
        auto leaf = result_dag_.append_node<node_kind::leaf>();
        leaf.sample_id() = label.sample_id;
        // Look up CG from sample_id_to_cg_idx_
        auto cg_it = sample_id_to_cg_idx_.find(label.sample_id);
        if (cg_it != sample_id_to_cg_idx_.end()) {
          leaf.cg() = all_cgs_[cg_it->second];
        }
        dag_idx = leaf.index();
      } else {
        auto inner = result_dag_.append_node<node_kind::inner>();
        if (label.cg_idx != no_idx) {
          inner.cg() = all_cgs_[label.cg_idx];
        }
        dag_idx = inner.index();
      }
      node_idx_to_dag_idx[result_id] = dag_idx;
      dag_idx_to_label_[dag_idx] = label;
    }

    // Connect UA to all nodes that have no parents (roots of input trees)
    // Find nodes that are parents in edges but not children
    std::pmr::unordered_map<std::size_t, bool> is_child_in_edges(mr);
    for (auto it = result_edges_.begin(); it != result_edges_.end(); ++it) {
      auto& [el, _] = *it;
      auto child_it = result_nodes_.find(el.child);
      if (child_it != no_idx) {
        is_child_in_edges[result_nodes_[child_it].second] = true;
      }
    }

    // Build clade lookup: flat clade -> list of (result_id, clade_index)
    // Used to connect orphaned fragment roots to their correct parent.
    // Only include non-orphan nodes (nodes that appear as children in
    // some edge) so that orphan tree-roots don't match against each other.
    std::pmr::map<std::vector<std::size_t>,
                  std::pmr::vector<std::pair<std::size_t, std::size_t>>>
        clade_map(mr);

    for (auto it = result_nodes_.begin(); it != result_nodes_.end(); ++it) {
      auto& [label, result_id] = *it;
      if (label.ls_idx == no_idx) continue;
      if (!is_child_in_edges.contains(result_id)) continue;
      auto& ls = all_leaf_sets_[label.ls_idx];
      for (std::size_t ci = 0; ci < ls.clades().size(); ++ci) {
        clade_map[ls.clades()[ci]].push_back({result_id, ci});
      }
    }

    // Connect orphan nodes: use clade map to find correct parent, or UA
    for (auto it = result_nodes_.begin(); it != result_nodes_.end(); ++it) {
      auto& [label, result_id] = *it;
      if (is_child_in_edges.contains(result_id)) continue;

      bool connected = false;
      if (label.ls_idx != no_idx) {
        auto flat = all_leaf_sets_[label.ls_idx].to_parent_clade();
        auto cm_it = clade_map.find(flat);
        if (cm_it != clade_map.end()) {
          for (auto& [parent_result_id, clade_idx] : cm_it->second) {
            if (parent_result_id == result_id) continue;

            auto parent_dag_idx = node_idx_to_dag_idx[parent_result_id];
            auto child_dag_idx = node_idx_to_dag_idx[result_id];

            auto edge = result_dag_.append_edge<edge_kind::clade>();
            edge.clade_index() = clade_idx;
            std::visit([&](auto n) { edge.set_parent(n); },
                       result_dag_.get_node(parent_dag_idx));
            std::visit([&](auto n) { edge.set_child(n); },
                       result_dag_.get_node(child_dag_idx));

            // Compute edge mutations from parent/child CGs
            auto& parent_label = dag_idx_to_label_[parent_dag_idx];
            compact_genome parent_cg;
            compact_genome child_cg;

            if (parent_label.cg_idx != no_idx) {
              parent_cg = all_cgs_[parent_label.cg_idx];
            }
            if (!label.sample_id.empty()) {
              auto cg_it = sample_id_to_cg_idx_.find(label.sample_id);
              if (cg_it != sample_id_to_cg_idx_.end()) {
                child_cg = all_cgs_[cg_it->second];
              }
            } else if (label.cg_idx != no_idx) {
              child_cg = all_cgs_[label.cg_idx];
            }

            edge.mutations() = compact_genome::to_edge_mutations(
                reference_sequence_, parent_cg, child_cg);
            connected = true;
          }
        }
      }

      if (!connected) {
        // True root: connect to UA
        auto dag_idx = node_idx_to_dag_idx[result_id];
        auto edge = ua.append_child<edge_kind::clade>();
        std::visit([&](auto n) { edge.set_child(n); },
                   result_dag_.get_node(dag_idx));
        edge.clade_index() = 0;

        // Compute edge mutations from reference (UA CG = empty) to child CG
        compact_genome child_cg;
        if (!label.sample_id.empty()) {
          auto cg_it = sample_id_to_cg_idx_.find(label.sample_id);
          if (cg_it != sample_id_to_cg_idx_.end()) {
            child_cg = all_cgs_[cg_it->second];
          }
        } else if (label.cg_idx != no_idx) {
          child_cg = all_cgs_[label.cg_idx];
        }
        edge.mutations() = compact_genome::to_edge_mutations(
            reference_sequence_, compact_genome{}, child_cg);
      }
    }

    // Create edges
    for (auto it = result_edges_.begin(); it != result_edges_.end(); ++it) {
      auto& [el, _] = *it;

      auto parent_it = result_nodes_.find(el.parent);
      auto child_it = result_nodes_.find(el.child);
      if (parent_it == no_idx || child_it == no_idx) continue;

      auto parent_result_id = result_nodes_[parent_it].second;
      auto child_result_id = result_nodes_[child_it].second;

      auto parent_dag_idx = node_idx_to_dag_idx[parent_result_id];
      auto child_dag_idx = node_idx_to_dag_idx[child_result_id];

      // Compute clade index
      auto& parent_label = el.parent;
      auto& child_label = el.child;

      std::size_t clade_idx = 0;
      if (parent_label.ls_idx != no_idx) {
        auto& parent_ls = all_leaf_sets_[parent_label.ls_idx];
        leaf_set child_ls;
        if (child_label.ls_idx != no_idx) {
          child_ls = all_leaf_sets_[child_label.ls_idx];
        }
        clade_idx = el.compute_clade_index(parent_ls.clades(), child_ls);
      }

      auto edge = result_dag_.append_edge<edge_kind::clade>();
      edge.clade_index() = clade_idx;

      // Set parent and child
      std::visit([&](auto n) { edge.set_parent(n); },
                 result_dag_.get_node(parent_dag_idx));
      std::visit([&](auto n) { edge.set_child(n); },
                 result_dag_.get_node(child_dag_idx));

      // Compute edge mutations from CGs
      compact_genome parent_cg;
      compact_genome child_cg;

      if (parent_label.cg_idx != no_idx) {
        parent_cg = all_cgs_[parent_label.cg_idx];
      }

      if (!child_label.sample_id.empty()) {
        auto cg_it = sample_id_to_cg_idx_.find(child_label.sample_id);
        if (cg_it != sample_id_to_cg_idx_.end()) {
          child_cg = all_cgs_[cg_it->second];
        }
      } else if (child_label.cg_idx != no_idx) {
        child_cg = all_cgs_[child_label.cg_idx];
      }

      edge.mutations() = compact_genome::to_edge_mutations(reference_sequence_,
                                                           parent_cg, child_cg);
    }

    result_built_ = true;
  }

 public:
  explicit merge(std::string_view reference_sequence,
                 thread_pool& pool = thread_pool::get_default())
      : reference_sequence_{reference_sequence}, pool_{pool} {}

  ~merge() { drain_pending(); }

  void add_dag(phylo_dag& d) {
    result_built_ = false;
    pending_.push_back(pool_.submit([&d] { return process_dag_locally(d); }));
  }

  void add_dag(phylo_dag&& d) {
    result_built_ = false;
    pending_.push_back(pool_.submit(
        [d = std::move(d)]() mutable { return process_dag_locally(d); }));
  }

  void add_dags(std::span<phylo_dag*> dags) {
    result_built_ = false;
    for (auto* d : dags) {
      pending_.push_back(pool_.submit([d] { return process_dag_locally(*d); }));
    }
  }

  phylo_dag& get_result() {
    drain_pending();
    if (!result_built_) {
      result_dag_ = phylo_dag{};
      build_result();
      // Safety net: trim incomplete-leaf-set clade alternatives that may
      // exist in DAGs loaded from older protobuf files.  For freshly
      // generated fragments this should be a no-op (the fragment-root fix
      // ensures fragments capture the full affected subtree).
      trim_inconsistent_clade_edges(result_dag_);
      build_clade_offsets(result_dag_);
    }
    return result_dag_;
  }

  node_label const& get_node_label(std::size_t dag_idx) {
    drain_pending();
    if (!result_built_) build_result();
    return dag_idx_to_label_.at(dag_idx);
  }

  leaf_set const& get_leaf_set(std::size_t ls_idx) {
    drain_pending();
    return all_leaf_sets_[ls_idx];
  }

  compact_genome const& get_cg(std::size_t cg_idx) {
    drain_pending();
    return all_cgs_[cg_idx];
  }

  std::string const& get_leaf_id(std::size_t leaf_id_idx) {
    drain_pending();
    return all_leaf_ids_[leaf_id_idx];
  }

  std::size_t result_node_count() {
    drain_pending();
    // +1 for UA
    return result_nodes_.size() + 1;
  }

  std::size_t result_edge_count() {
    drain_pending();
    // +N for UA edges (one per root node that has no parent in merged edges)
    std::unordered_set<std::size_t> is_child_idx;
    for (auto it = result_edges_.begin(); it != result_edges_.end(); ++it) {
      auto& [el, _] = *it;
      auto child_it = result_nodes_.find(el.child);
      if (child_it != no_idx) {
        is_child_idx.insert(child_it);
      }
    }
    std::size_t ua_edges = 0;
    for (auto it = result_nodes_.begin(); it != result_nodes_.end(); ++it) {
      // Get this node's index in the hash_chain
      // We need the chain index, not the mapped value
      // Use find() to get the index
      auto& [label, _] = *it;
      auto idx = result_nodes_.find(label);
      if (!is_child_idx.contains(idx)) {
        ua_edges++;
      }
    }
    return result_edges_.size() + ua_edges;
  }

  // Count internal nodes in fragment whose leaf sets don't already exist
  // in the merged DAG. Read-only on merge state (doesn't insert).
  std::size_t count_novel_nodes(phylo_dag& fragment) {
    drain_pending();
    auto lr = process_dag_locally(fragment);

    // Build leaf_id remap (local -> global), tracking unmapped leaves
    scoped_arena<> arena;
    auto* mr = arena.get();
    std::pmr::unordered_map<std::size_t, std::size_t> lid_remap(mr);

    for (auto it = lr.nodes.begin(); it != lr.nodes.end(); ++it) {
      auto& label = (*it).first;
      if (label.leaf_id_idx != no_idx &&
          !lid_remap.contains(label.leaf_id_idx)) {
        auto& local_sid = lr.leaf_ids[label.leaf_id_idx];
        auto idx = all_leaf_ids_.find(local_sid);
        if (idx != no_idx) {
          lid_remap[label.leaf_id_idx] = idx;
        }
      }
    }

    std::size_t novel_count = 0;
    for (auto it = lr.nodes.begin(); it != lr.nodes.end(); ++it) {
      auto& label = (*it).first;
      if (!label.sample_id.empty()) continue;  // skip leaves
      if (label.ls_idx == no_idx) continue;

      auto& local_ls = lr.leaf_sets[label.ls_idx];

      // Remap clades to global leaf_id indices
      bool remap_ok = true;
      std::vector<std::vector<std::size_t>> remapped_clades;
      for (auto& clade : local_ls.clades()) {
        std::vector<std::size_t> rc;
        for (auto local_lid : clade) {
          auto it2 = lid_remap.find(local_lid);
          if (it2 == lid_remap.end()) {
            remap_ok = false;
            break;
          }
          rc.push_back(it2->second);
        }
        if (!remap_ok) break;
        std::sort(rc.begin(), rc.end());
        remapped_clades.push_back(std::move(rc));
      }

      if (!remap_ok) {
        novel_count++;
        continue;
      }

      std::sort(remapped_clades.begin(), remapped_clades.end());
      if (!all_leaf_sets_.contains(leaf_set{std::move(remapped_clades)})) {
        novel_count++;
      }
    }
    return novel_count;
  }
};

}  // namespace larch

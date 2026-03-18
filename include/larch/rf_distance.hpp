#pragma once

#include <larch/bigint.hpp>
#include <larch/compute.hpp>
#include <larch/merge.hpp>
#include <larch/simple_weight_ops.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>

#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace larch {

inline bigint compute_above_tree_count(std::size_t node_idx, phylo_dag& dag,
                                       std::vector<bigint>& above_counts,
                                       subtree_weight<tree_count_ops>& below,
                                       tree_count_ops const& ops) {
  if (above_counts[node_idx] > 0) return above_counts[node_idx];
  if (is_ua(dag, node_idx)) {
    above_counts[node_idx] = bigint{1};
    return bigint{1};
  }

  bigint above{0};
  auto parent_edge_indices = get_parent_edges(dag, node_idx);

  for (auto pe_idx : parent_edge_indices) {
    auto parent_idx = get_parent_idx(dag, pe_idx);
    auto current_clade = get_clade_idx(dag, pe_idx);

    auto parent_clades = get_clades(dag, parent_idx);

    bigint below_parent{1};
    for (std::size_t ci = 0; ci < parent_clades.size(); ++ci) {
      if (ci == current_clade) continue;
      bigint below_clade{0};
      for (auto edge_idx : parent_clades[ci]) {
        auto child_idx = get_child_idx(dag, edge_idx);
        below_clade += below.compute_weight_below(child_idx, ops);
      }
      below_parent *= below_clade;
    }

    above +=
        compute_above_tree_count(parent_idx, dag, above_counts, below, ops) *
        below_parent;
  }

  above_counts[node_idx] = above;
  return above;
}

struct sum_rf_distance_ops {
  using weight_type = bigint;
  static inline weight_type const identity{0};

  bigint num_trees;
  std::map<std::set<std::string>, bigint> leafset_to_full_treecount;
  bigint shift_sum_;
  merge* reference_;
  merge* compute_;

  sum_rf_distance_ops() : reference_{nullptr}, compute_{nullptr} {}

  // Convert parent clade (leaf_id indices) to content-based key using sample_id
  // strings
  static std::set<std::string> clade_key(merge& m, node_label const& label) {
    std::set<std::string> key;
    if (label.ls_idx != no_idx) {
      for (auto leaf_id_idx : m.get_leaf_set(label.ls_idx).to_parent_clade()) {
        key.insert(m.get_leaf_id(leaf_id_idx));
      }
    }
    if (key.empty() && !label.sample_id.empty()) {
      key.insert(label.sample_id);
    }
    if (key.empty() && label.cg_idx != no_idx) {
      key.insert(m.get_cg(label.cg_idx).to_string());
    }
    return key;
  }

  sum_rf_distance_ops(merge& reference, merge& compute)
      : reference_{&reference}, compute_{&compute} {
    auto& ref_dag = reference.get_result();
    subtree_weight<tree_count_ops> below(ref_dag);
    tree_count_ops tc_ops;
    auto root = get_root_idx(ref_dag);
    num_trees = below.compute_weight_below(root, tc_ops);

    std::size_t n = ref_dag.node_high_mark();
    std::vector<bigint> above(n, bigint{0});

    for (auto nv : ref_dag.get_all_nodes()) {
      auto idx = std::visit([](auto nn) { return nn.index(); }, nv);
      compute_above_tree_count(idx, ref_dag, above, below, tc_ops);
    }

    for (auto nv : ref_dag.get_all_nodes()) {
      auto idx = std::visit([](auto nn) { return nn.index(); }, nv);
      if (is_ua(ref_dag, idx)) continue;

      auto& label = reference.get_node_label(idx);
      if (label.ls_idx == no_idx && label.sample_id.empty() &&
          label.cg_idx == no_idx)
        continue;

      auto key = clade_key(reference, label);
      if (key.empty()) continue;

      leafset_to_full_treecount[key] +=
          above[idx] * below.compute_weight_below(idx, tc_ops);
    }

    shift_sum_ = bigint{0};
    for (auto& [_, count] : leafset_to_full_treecount) shift_sum_ += count;
  }

  weight_type compute_leaf(phylo_dag& /*dag*/, std::size_t /*node_idx*/) const {
    return bigint{0};
  }

  weight_type compute_edge(phylo_dag& /*dag*/, std::size_t edge_idx) const {
    auto& comp_dag = compute_->get_result();
    auto child_idx = get_child_idx(comp_dag, edge_idx);
    if (is_ua(comp_dag, child_idx)) return num_trees;

    auto& label = compute_->get_node_label(child_idx);
    auto key = clade_key(*compute_, label);
    if (key.empty()) return num_trees;

    auto it = leafset_to_full_treecount.find(key);
    if (it == leafset_to_full_treecount.end()) return num_trees;
    return num_trees - bigint{2} * it->second;
  }

  bool compare(weight_type const& lhs, weight_type const& rhs) const {
    return lhs < rhs;
  }

  bool compare_equal(weight_type const& lhs, weight_type const& rhs) const {
    return lhs == rhs;
  }

  weight_type combine(weight_type const& lhs, weight_type const& rhs) const {
    return lhs + rhs;
  }

  bigint const& get_shift_sum() const { return shift_sum_; }
};

using sum_rf_distance = simple_weight_ops<sum_rf_distance_ops>;

struct rf_distance : sum_rf_distance {
  rf_distance() = default;
  rf_distance(merge& reference, merge& compute)
      : sum_rf_distance{sum_rf_distance_ops{reference, compute}} {
    assert(is_tree(reference.get_result()));
  }
};

struct max_sum_rf_distance_ops : sum_rf_distance_ops {
  max_sum_rf_distance_ops() = default;
  max_sum_rf_distance_ops(merge& reference, merge& compute)
      : sum_rf_distance_ops{reference, compute} {}

  bool compare(weight_type const& lhs, weight_type const& rhs) const {
    return lhs > rhs;
  }
};

using max_sum_rf_distance = simple_weight_ops<max_sum_rf_distance_ops>;

struct max_rf_distance : max_sum_rf_distance {
  max_rf_distance() = default;
  max_rf_distance(merge& reference, merge& compute)
      : max_sum_rf_distance{max_sum_rf_distance_ops{reference, compute}} {
    assert(is_tree(reference.get_result()));
  }
};

// Bitset clade representation for efficient RF distance computation.
// Each clade is a bitvector over a shared leaf-id mapping, enabling O(n/64)
// comparison via word-level operations.

// Build a mapping from sample_id -> integer index for all leaves in the DAG.
inline std::unordered_map<std::string, uint32_t> build_leaf_id_map(
    phylo_dag& dag) {
  std::unordered_map<std::string, uint32_t> leaf_map;
  uint32_t next_id = 0;
  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            auto const& sid = node.sample_id();
            if (!sid.empty() && !leaf_map.count(sid)) {
              leaf_map[sid] = next_id++;
            }
          }
        },
        nv);
  }
  return leaf_map;
}

// A compact bitvector representing a set of leaf indices.
using clade_bitset = std::vector<uint64_t>;

inline clade_bitset make_clade_bitset(uint32_t num_leaves) {
  return clade_bitset((num_leaves + 63) / 64, 0);
}

inline void bitset_set(clade_bitset& bs, uint32_t idx) {
  bs[idx / 64] |= uint64_t{1} << (idx % 64);
}

inline void bitset_or(clade_bitset& dst, clade_bitset const& src) {
  for (std::size_t i = 0; i < dst.size(); ++i) dst[i] |= src[i];
}

inline std::size_t bitset_popcount(clade_bitset const& bs) {
  std::size_t count = 0;
  for (auto w : bs) count += static_cast<std::size_t>(__builtin_popcountll(w));
  return count;
}

inline std::size_t bitset_and_popcount(clade_bitset const& a,
                                       clade_bitset const& b) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    count += static_cast<std::size_t>(__builtin_popcountll(a[i] & b[i]));
  return count;
}

// Collect clade bitsets for a tree using a pre-built leaf-id mapping.
// Returns a sorted vector of clade bitsets (one per internal non-root node).
inline std::vector<clade_bitset> collect_clade_bitsets(
    phylo_dag& tree,
    std::unordered_map<std::string, uint32_t> const& leaf_map,
    uint32_t num_leaves) {
  assert(is_tree(tree));

  auto root_idx = get_root_idx(tree);
  std::unordered_map<std::size_t, clade_bitset> descendants;
  std::vector<clade_bitset> clades;

  struct frame {
    std::size_t node_idx;
    std::vector<std::size_t> children;
    std::size_t next = 0;
  };

  auto make_frame = [&](std::size_t idx) -> frame {
    frame f;
    f.node_idx = idx;
    auto nv = tree.get_node(idx);
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto cv = edge.get_child();
                  std::visit(
                      [&](auto child) { f.children.push_back(child.index()); },
                      cv);
                },
                ev);
          }
        },
        nv);
    return f;
  };

  // Skip UA: start from its single child.
  std::size_t real_root_idx = root_idx;
  {
    auto nv = tree.get_node(root_idx);
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto cv = edge.get_child();
                  std::visit(
                      [&](auto child) { real_root_idx = child.index(); }, cv);
                },
                ev);
            break;
          }
        },
        nv);
  }

  std::vector<frame> stack;
  stack.push_back(make_frame(real_root_idx));

  while (!stack.empty()) {
    auto& top = stack.back();
    if (top.next < top.children.size()) {
      auto child_idx = top.children[top.next++];
      stack.push_back(make_frame(child_idx));
    } else {
      auto idx = top.node_idx;
      if (top.children.empty()) {
        // Leaf: set the single bit for this leaf's integer id.
        auto bs = make_clade_bitset(num_leaves);
        auto nv = tree.get_node(idx);
        std::visit(
            [&](auto node) {
              if constexpr (requires { node.sample_id(); }) {
                auto it = leaf_map.find(node.sample_id());
                if (it != leaf_map.end()) bitset_set(bs, it->second);
              }
            },
            nv);
        descendants[idx] = std::move(bs);
      } else {
        // Internal node: OR children's bitsets.
        auto bs = make_clade_bitset(num_leaves);
        for (auto child_idx : top.children) {
          bitset_or(bs, descendants[child_idx]);
        }
        descendants[idx] = bs;
        // Record clade for non-root internal nodes.
        if (idx != real_root_idx) {
          clades.push_back(std::move(bs));
        }
      }
      stack.pop_back();
    }
  }

  std::sort(clades.begin(), clades.end());
  return clades;
}

// Compute RF distance between two trees using bitset clade representations.
// Both trees must satisfy is_tree(). The leaf_map must cover all leaves.
inline std::size_t pairwise_rf_distance(
    std::vector<clade_bitset> const& clades1,
    std::vector<clade_bitset> const& clades2) {
  // Merge-style count of shared clades (both vectors are sorted).
  std::size_t shared = 0;
  std::size_t i = 0, j = 0;
  while (i < clades1.size() && j < clades2.size()) {
    if (clades1[i] == clades2[j]) {
      ++shared;
      ++i;
      ++j;
    } else if (clades1[i] < clades2[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return clades1.size() + clades2.size() - 2 * shared;
}

}  // namespace larch

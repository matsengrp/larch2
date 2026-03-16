#pragma once

#include <larch/bigint.hpp>
#include <larch/compute.hpp>
#include <larch/phylo_dag.hpp>
#include <larch/weight_ops.hpp>

#include <cassert>
#include <memory_resource>
#include <optional>
#include <random>
#include <variant>
#include <vector>

namespace larch {

// Helper to get node_kind from a node_view
template <typename N>
node_kind node_kind_of(N const&) {
  if constexpr (requires(N n) { n.reference_sequence(); })
    return node_kind::ua;
  else if constexpr (requires(N n) { n.sample_id(); })
    return node_kind::leaf;
  else
    return node_kind::inner;
}

template <WeightOps Ops>
class subtree_weight {
 public:
  using weight_type = typename Ops::weight_type;

  explicit subtree_weight(
      phylo_dag& dag, std::optional<uint32_t> seed = std::nullopt,
      std::pmr::memory_resource* mr = std::pmr::new_delete_resource())
      : dag_(dag), rng_(seed.value_or(std::random_device{}())), mr_(mr) {}

  // Compute the aggregate weight below a node (memoized)
  weight_type compute_weight_below(std::size_t node_idx, Ops const& ops) {
    ensure_cache_size();
    if (cached_weights_[node_idx].has_value())
      return *cached_weights_[node_idx];

    // Cycle detection: if we're already computing this node,
    // the DAG has a cycle. Return max weight to avoid this path.
    if (in_progress_[node_idx]) {
      if constexpr (requires(weight_type w) { w / 2; }) {
        return std::numeric_limits<weight_type>::max() / 2;
      } else {
        return weight_type{};
      }
    }
    in_progress_[node_idx] = true;

    weight_type result;
    if (is_leaf(dag_, node_idx)) {
      result = ops.compute_leaf(dag_, node_idx);
      if (node_idx >= cached_min_weight_edges_.size())
        cached_min_weight_edges_.resize(node_idx + 1);
    } else {
      auto clades = get_clades(dag_, node_idx, mr_);
      if (node_idx >= cached_min_weight_edges_.size())
        cached_min_weight_edges_.resize(node_idx + 1);
      cached_min_weight_edges_[node_idx].resize(clades.size());

      std::vector<weight_type> clade_weights;
      clade_weights.reserve(clades.size());

      for (std::size_t ci = 0; ci < clades.size(); ++ci) {
        auto const& edges = clades[ci];
        std::vector<weight_type> edge_weights;
        edge_weights.reserve(edges.size());

        for (auto edge_idx : edges) {
          auto child_idx = get_child_idx(edge_idx);
          auto child_w = compute_weight_below(child_idx, ops);
          auto edge_w = ops.compute_edge(dag_, edge_idx);
          edge_weights.push_back(ops.above_node(edge_w, child_w));
        }

        auto [clade_w, best_indices] = ops.within_clade_accum(edge_weights);
        cached_min_weight_edges_[node_idx][ci].clear();
        for (auto i : best_indices)
          cached_min_weight_edges_[node_idx][ci].push_back(edges[i]);
        clade_weights.push_back(clade_w);
      }

      result = ops.between_clades(clade_weights);
    }

    in_progress_[node_idx] = false;
    cached_weights_[node_idx] = result;
    return result;
  }

  // Count min-weight trees below a node
  bigint min_weight_count(std::size_t node_idx, Ops const& ops) {
    compute_weight_below(node_idx, ops);

    ensure_count_cache_size();
    if (cached_subtree_counts_[node_idx].has_value())
      return *cached_subtree_counts_[node_idx];

    bigint result;
    if (is_leaf(dag_, node_idx)) {
      result = bigint{1};
    } else {
      auto const& clade_edges = cached_min_weight_edges_[node_idx];
      bigint product{1};
      for (auto const& edges : clade_edges) {
        bigint clade_sum{0};
        for (auto edge_idx : edges) {
          auto child_idx = get_child_idx(edge_idx);
          clade_sum += min_weight_count(child_idx, ops);
        }
        product *= clade_sum;
      }
      result = product;
    }

    cached_subtree_counts_[node_idx] = result;
    return result;
  }

  // Sample a tree using uniform random edge selection at each clade
  phylo_dag sample_tree(Ops const& ops) {
    auto root_idx = get_root_idx();
    compute_weight_below(root_idx, ops);
    return extract_tree([this](auto const& edges) -> std::size_t {
      std::uniform_int_distribution<std::size_t> dist(0, edges.size() - 1);
      return dist(rng_);
    });
  }

  // Sample a tree proportional to subtree tree-counts
  phylo_dag uniform_sample_tree(Ops const& ops) {
    auto root_idx = get_root_idx();
    compute_weight_below(root_idx, ops);
    return extract_tree([this](auto const& edges) -> std::size_t {
      if (edges.size() == 1) return 0;
      std::vector<double> weights;
      weights.reserve(edges.size());
      for (auto edge_idx : edges) {
        auto child_idx = get_child_idx(edge_idx);
        bigint count = count_trees_below(child_idx);
        weights.push_back(count.to_double());
      }
      double sum = 0;
      for (auto w : weights) sum += w;
      if (sum <= 0) {
        std::uniform_int_distribution<std::size_t> dist(0, edges.size() - 1);
        return dist(rng_);
      }
      std::discrete_distribution<std::size_t> dist(weights.begin(),
                                                   weights.end());
      return dist(rng_);
    });
  }

  // Sample a tree selecting only among min-weight edges at each clade
  phylo_dag min_weight_sample_tree(Ops const& ops) {
    auto root_idx = get_root_idx();
    compute_weight_below(root_idx, ops);
    return extract_tree_min_weight([this](auto const& edges) -> std::size_t {
      std::uniform_int_distribution<std::size_t> dist(0, edges.size() - 1);
      return dist(rng_);
    });
  }

  // Sample min-weight tree proportional to subtree counts
  phylo_dag min_weight_uniform_sample_tree(Ops const& ops) {
    auto root_idx = get_root_idx();
    compute_weight_below(root_idx, ops);
    return extract_tree_min_weight([this,
                                    &ops](auto const& edges) -> std::size_t {
      if (edges.size() == 1) return 0;
      std::vector<double> weights;
      weights.reserve(edges.size());
      for (auto edge_idx : edges) {
        auto child_idx = get_child_idx(edge_idx);
        auto c = min_weight_count(child_idx, ops);
        weights.push_back(c.to_double());
      }
      double sum = 0;
      for (auto w : weights) sum += w;
      if (sum <= 0) {
        std::uniform_int_distribution<std::size_t> dist(0, edges.size() - 1);
        return dist(rng_);
      }
      std::discrete_distribution<std::size_t> dist(weights.begin(),
                                                   weights.end());
      return dist(rng_);
    });
  }

 private:
  phylo_dag& dag_;
  std::vector<std::optional<weight_type>> cached_weights_;
  std::vector<std::optional<bigint>> cached_subtree_counts_;
  std::vector<std::vector<std::vector<std::size_t>>> cached_min_weight_edges_;
  std::vector<bool> in_progress_;  // cycle detection
  std::mt19937 rng_;
  std::pmr::memory_resource* mr_;

  bigint count_trees_below(std::size_t node_idx) {
    if (is_leaf(dag_, node_idx)) return bigint{1};
    auto clades = get_clades(dag_, node_idx, mr_);
    bigint product{1};
    for (auto const& edges : clades) {
      bigint clade_sum{0};
      for (auto edge_idx : edges) {
        auto child_idx = get_child_idx(edge_idx);
        clade_sum += count_trees_below(child_idx);
      }
      product *= clade_sum;
    }
    return product;
  }

  void ensure_cache_size() {
    auto hm = dag_.node_high_mark();
    if (cached_weights_.size() < hm) {
      cached_weights_.resize(hm);
      in_progress_.resize(hm, false);
    }
  }

  void ensure_count_cache_size() {
    auto hm = dag_.node_high_mark();
    if (cached_subtree_counts_.size() < hm) cached_subtree_counts_.resize(hm);
  }

  std::size_t get_root_idx() {
    auto rv = dag_.get_root();
    return std::visit([](auto n) { return n.index(); }, rv);
  }

  std::size_t get_child_idx(std::size_t edge_idx) {
    auto ev = dag_.get_edge(edge_idx);
    return std::visit(
        [](auto edge) {
          auto cv = edge.get_child();
          return std::visit([](auto child) { return child.index(); }, cv);
        },
        ev);
  }

  void copy_node_annotations(phylo_dag::node_variant_type const& src_nv,
                             phylo_dag::node_variant_type& dst_nv) {
    std::visit(
        [](auto src, auto dst) {
          if constexpr (requires {
                          src.cg();
                          dst.cg();
                        }) {
            dst.cg() = src.cg();
          }
          if constexpr (requires {
                          src.sample_id();
                          dst.sample_id();
                        }) {
            dst.sample_id() = src.sample_id();
          }
          if constexpr (requires {
                          src.reference_sequence();
                          dst.reference_sequence();
                        }) {
            dst.reference_sequence() = src.reference_sequence();
          }
        },
        src_nv, dst_nv);
  }

  static node_kind get_node_kind(phylo_dag::node_variant_type const& nv) {
    return std::visit([](auto n) { return node_kind_of(n); }, nv);
  }

  template <typename Chooser>
  phylo_dag extract_tree(Chooser&& chooser) {
    phylo_dag tree;
    auto root_idx = get_root_idx();
    auto src_root = dag_.get_root();
    auto src_kind = get_node_kind(src_root);
    auto dst_root_v = tree.append_node(src_kind);
    copy_node_annotations(src_root, dst_root_v);
    auto dst_root_idx =
        std::visit([](auto n) { return n.index(); }, dst_root_v);
    std::visit([&](auto n) { tree.set_root(n); }, dst_root_v);

    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.emplace_back(root_idx, dst_root_idx);

    while (!stack.empty()) {
      auto [src_nidx, dst_nidx] = stack.back();
      stack.pop_back();

      if (is_leaf(dag_, src_nidx)) continue;

      auto clades = get_clades(dag_, src_nidx, mr_);
      for (auto const& edges : clades) {
        if (edges.empty()) continue;
        auto chosen = chooser(edges);
        auto edge_idx = edges[chosen];
        auto child_idx = get_child_idx(edge_idx);

        auto src_child = dag_.get_node(child_idx);
        auto child_kind = get_node_kind(src_child);
        auto dst_child_v = tree.append_node(child_kind);
        copy_node_annotations(src_child, dst_child_v);
        auto dst_child_idx =
            std::visit([](auto n) { return n.index(); }, dst_child_v);

        auto dst_edge = tree.template append_edge<edge_kind::clade>();
        auto dst_parent_nv = tree.get_node(dst_nidx);
        std::visit([&](auto parent) { dst_edge.set_parent(parent); },
                   dst_parent_nv);
        std::visit([&](auto child) { dst_edge.set_child(child); }, dst_child_v);

        auto src_edge = dag_.get_edge(edge_idx);
        std::visit(
            [&](auto se) {
              dst_edge.mutations() = se.mutations();
              dst_edge.clade_index() = se.clade_index();
            },
            src_edge);

        stack.emplace_back(child_idx, dst_child_idx);
      }
    }

    return tree;
  }

  template <typename Chooser>
  phylo_dag extract_tree_min_weight(Chooser&& chooser) {
    phylo_dag tree;
    auto root_idx = get_root_idx();
    auto src_root = dag_.get_root();
    auto src_kind = get_node_kind(src_root);
    auto dst_root_v = tree.append_node(src_kind);
    copy_node_annotations(src_root, dst_root_v);
    std::visit([&](auto n) { tree.set_root(n); }, dst_root_v);
    auto dst_root_idx =
        std::visit([](auto n) { return n.index(); }, dst_root_v);

    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.emplace_back(root_idx, dst_root_idx);

    while (!stack.empty()) {
      auto [src_nidx, dst_nidx] = stack.back();
      stack.pop_back();

      if (is_leaf(dag_, src_nidx)) continue;

      auto const& clade_edges = cached_min_weight_edges_[src_nidx];
      for (auto const& edges : clade_edges) {
        if (edges.empty()) continue;
        auto chosen = chooser(edges);
        auto edge_idx = edges[chosen];
        auto child_idx = get_child_idx(edge_idx);

        auto src_child = dag_.get_node(child_idx);
        auto child_kind = get_node_kind(src_child);
        auto dst_child_v = tree.append_node(child_kind);
        copy_node_annotations(src_child, dst_child_v);
        auto dst_child_idx =
            std::visit([](auto n) { return n.index(); }, dst_child_v);

        auto dst_edge = tree.template append_edge<edge_kind::clade>();
        auto dst_parent_nv = tree.get_node(dst_nidx);
        std::visit([&](auto parent) { dst_edge.set_parent(parent); },
                   dst_parent_nv);
        std::visit([&](auto child) { dst_edge.set_child(child); }, dst_child_v);

        auto src_edge = dag_.get_edge(edge_idx);
        std::visit(
            [&](auto se) {
              dst_edge.mutations() = se.mutations();
              dst_edge.clade_index() = se.clade_index();
            },
            src_edge);

        stack.emplace_back(child_idx, dst_child_idx);
      }
    }

    return tree;
  }
};

}  // namespace larch

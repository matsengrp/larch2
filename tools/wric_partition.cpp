// wric_partition: partition a tree pb into connected parts by cutting edges.
// Bottom-up: child subtrees/regions whose combined leaf count lands in
// [min,max] form one part; a node whose total exceeds max packs its child
// regions by first-fit-decreasing into bins capped at max (bins >= min are
// cut as parts); regions below min ride upward. Covers ALL taxa.
// usage: wric_partition <tree.pb.gz> <min> <max> <out_dir> [min_part_leaves_to_write]
#include "wric_subtree_common.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>

using namespace wsub;

namespace {

std::size_t total_leaves(tnode const& n) { return n.taxa.size(); }

// Returns the region(s) that ride up from n. Emits cut parts into parts.
std::vector<tnode> walk(tnode& n, std::size_t min_l, std::size_t max_l,
                        std::vector<tnode>& parts) {
  if (n.children.empty()) return {n};
  std::vector<tnode> regs;
  for (auto& c : n.children) {
    auto sub = walk(c, min_l, max_l, parts);
    for (auto& r : sub) regs.push_back(std::move(r));
  }
  // n keeps only the riding regions (cut parts were already extracted).
  {
    std::vector<tnode> keep;
    for (auto& r : regs) keep.push_back(std::move(r));
    n.children = std::move(keep);
    n.taxa.clear();
    for (auto& c : n.children) n.taxa = sorted_union(n.taxa, c.taxa);
  }
  std::size_t total = n.taxa.size();

  if (total >= min_l && total <= max_l) {
    parts.push_back(std::move(n));  // only riding regions attached
    return {};
  }
  if (total < min_l) {
    return {std::move(n)};
  }
  // total > max_l: first-fit-decreasing over the riding regions.
  std::sort(n.children.begin(), n.children.end(),
            [](tnode const& a, tnode const& b) {
              return total_leaves(a) > total_leaves(b);
            });
  std::vector<std::vector<std::size_t>> bins;
  std::vector<std::size_t> bin_sum;
  auto& riders = n.children;
  for (std::size_t i = 0; i < riders.size(); ++i) {
    bool placed = false;
    for (std::size_t b = 0; b < bins.size(); ++b) {
      if (bin_sum[b] + total_leaves(riders[i]) <= max_l) {
        bins[b].push_back(i);
        bin_sum[b] += total_leaves(riders[i]);
        placed = true;
        break;
      }
    }
    if (!placed) {
      bins.push_back({i});
      bin_sum.push_back(total_leaves(riders[i]));
    }
  }
  std::vector<tnode> riding;
  for (std::size_t b = 0; b < bins.size(); ++b) {
    if (bin_sum[b] >= min_l) {
      if (bins[b].size() == 1) {
        parts.push_back(std::move(riders[bins[b][0]]));
      } else {
        tnode bushy;
        for (auto i : bins[b]) bushy.children.push_back(std::move(riders[i]));
        for (auto& c : bushy.children)
          bushy.taxa = sorted_union(bushy.taxa, c.taxa);
        parts.push_back(std::move(bushy));
      }
    } else {
      // Undersized bin rides up merged.
      tnode merged;
      for (auto i : bins[b]) merged.children.push_back(std::move(riders[i]));
      for (auto& c : merged.children)
        merged.taxa = sorted_union(merged.taxa, c.taxa);
      riding.push_back(std::move(merged));
    }
  }
  return riding;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 5 || argc > 6) {
      std::fprintf(stderr,
                   "usage: %s <tree.pb.gz> <min> <max> <out_dir> "
                   "[min_part_leaves_to_write]\n",
                   argv[0]);
      return 2;
    }
    using namespace larch;
    std::string reference;
    std::map<std::string, larch::compact_genome> leaf_cgs;
    auto dag = load_proto_dag(argv[1]);
    std::size_t ua = no_idx;
    for (std::size_t i = 0; i < dag.node_count(); ++i)
      if (larch::detail::is_ua_node(dag.get_node(i))) {
        ua = i;
        break;
      }
    if (ua == no_idx) throw std::runtime_error("no UA node");
    std::visit(
        [&](auto n) {
          using N = std::remove_cvref_t<decltype(n)>;
          if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::ua>>)
            reference = n.reference_sequence();
        },
        dag.get_node(ua));
    std::size_t root = no_idx;
    std::visit(
        [&](auto n) {
          for (auto ev : n.get_children())
            std::visit(
                [&](auto e) {
                  if (root == no_idx) root = get_child_idx(dag, e.index());
                },
                ev);
        },
        dag.get_node(ua));

    std::function<tnode(std::size_t)> nest = [&](std::size_t idx) -> tnode {
      tnode node;
      auto nv = dag.get_node(idx);
      if (larch::detail::is_leaf_node(nv)) {
        std::visit(
            [&](auto n) {
              using N = std::remove_cvref_t<decltype(n)>;
              if constexpr (std::is_same_v<N,
                               node_view<phylo_dag, node_kind::leaf>>) {
                node.leaf = true;
                node.sid = n.sample_id();
                node.taxa = {node.sid};
                leaf_cgs[node.sid] = n.cg();
              }
            },
            nv);
        return node;
      }
      std::visit(
          [&](auto n) {
            for (auto const& ev : n.get_children())
              std::visit(
                  [&](auto e) {
                    node.children.push_back(
                        nest(get_child_idx(dag, e.index())));
                  },
                  ev);
          },
          nv);
      for (auto& c : node.children)
        node.taxa = sorted_union(node.taxa, c.taxa);
      return node;
    };
    auto tree = nest(root);

    auto min_l = static_cast<std::size_t>(std::stoul(argv[2]));
    auto max_l = static_cast<std::size_t>(std::stoul(argv[3]));
    std::filesystem::path outdir = argv[4];
    std::size_t write_min =
        argc == 6 ? static_cast<std::size_t>(std::stoul(argv[5])) : 0;
    std::filesystem::create_directories(outdir);

    std::vector<tnode> parts;
    auto riding = walk(tree, min_l, max_l, parts);
    std::sort(parts.begin(), parts.end(),
              [](tnode const& a, tnode const& b) {
                return a.taxa.size() < b.taxa.size();
              });
    std::FILE* tsv = std::fopen((outdir / "parts.tsv").string().c_str(), "w");
    std::size_t total = 0, written = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      total += parts[i].taxa.size();
      std::fprintf(tsv, "%03zu\t%zu\n", i, parts[i].taxa.size());
      if (parts[i].taxa.size() >= write_min) {
        auto name = outdir / ("part_" + std::to_string(i) + ".pb.gz");
        write_tree(parts[i], reference, leaf_cgs, name.string());
        ++written;
      }
    }
    std::size_t riding_total = 0;
    for (auto const& r : riding) riding_total += r.taxa.size();
    if (!riding.empty())
      std::fprintf(tsv, "remainder\t%zu\n", riding_total);
    std::fclose(tsv);
    std::fprintf(stderr,
                 "parts=%zu in_parts=%zu remainder=%zu (all=%zu) written=%zu "
                 "sizes: min=%zu max=%zu\n",
                 parts.size(), total, riding_total, total + riding_total,
                 written, parts.empty() ? 0 : parts.front().taxa.size(),
                 parts.empty() ? 0 : parts.back().taxa.size());
    return 0;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

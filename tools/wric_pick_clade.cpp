// wric_pick_clade: load a tree pb (any arity), find a clade whose leaf count
// lies in [min,max] (post-order DFS), and write its leaf sample ids.
// usage: wric_pick_clade <tree.pb.gz> <min_leaves> <max_leaves> <out_ids.txt>
#include "wric_subtree_common.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>

using namespace wsub;

namespace {

// Post-order search: report the FIRST (deepest) node whose taxon count is in
// range. Returns nullptr if none.
tnode* find_clade(tnode& n, std::size_t min_l, std::size_t max_l) {
  for (auto& c : n.children)
    if (auto* hit = find_clade(c, min_l, max_l)) return hit;
  if (n.taxa.size() >= min_l && n.taxa.size() <= max_l) return &n;
  return nullptr;
}

// Preorder-index extraction: emit leaf ids (and optionally the subtree pb)
// for the clade at PREORDER index k in the (node, left, right) DFS.
tnode* preorder_pick(tnode& n, std::size_t& k, std::size_t target) {
  std::size_t here = k++;
  if (here == target) return &n;
  for (auto& c : n.children)
    if (auto* hit = preorder_pick(c, k, target)) return hit;
  return nullptr;
}

// Restriction: keep only the spanning subtree of the target leaf set
// (drop empty subtrees, hoist unary chains). Result is any-arity but
// typically has unary-free structure after hoisting.
tnode restrict_to(tnode node, std::set<std::string> const& want) {
  if (node.leaf) {
    if (want.count(node.sid)) return node;
    return {};  // dropped; caller filters empties
  }
  std::vector<tnode> kept;
  for (auto& c : node.children) {
    auto r = restrict_to(std::move(c), want);
    if (r.leaf || !r.children.empty()) kept.push_back(std::move(r));
  }
  if (kept.empty()) return {};
  if (kept.size() == 1) return std::move(kept[0]);  // hoist unary
  node.children = std::move(kept);
  node.taxa.clear();
  for (auto& c : node.children) node.taxa = sorted_union(node.taxa, c.taxa);
  return node;
}

// Fragment enumeration: maximal clades containing NO excluded leaf. Used to
// partition the complement of frozen carrier clades into graftable (clade)
// parts. Emits ids + subtree pb per fragment with >= min_leaves.
void emit_fragments(tnode const& node, std::set<std::string> const& excl,
                    std::size_t min_leaves, std::string const& dir,
                    std::string const& reference,
                    std::map<std::string, larch::compact_genome> const& cgs,
                    std::size_t& counter, std::size_t& tiny_leaves) {
  bool touches = false;
  for (auto const& t : node.taxa)
    if (excl.count(t)) {
      touches = true;
      break;
    }
  if (!touches) {
    if (node.taxa.size() >= min_leaves) {
      char ids_path[512], pb_path[512];
      std::snprintf(ids_path, sizeof ids_path, "%s/frag_%03zu.ids.txt",
                    dir.c_str(), counter);
      std::snprintf(pb_path, sizeof pb_path, "%s/frag_%03zu.pb.gz",
                    dir.c_str(), counter);
      std::FILE* out = std::fopen(ids_path, "w");
      if (!out) throw std::runtime_error("cannot write " + std::string(ids_path));
      for (auto const& id : node.taxa) std::fprintf(out, "%s\n", id.c_str());
      std::fclose(out);
      write_tree(node, reference, cgs, pb_path);
      ++counter;
    } else {
      tiny_leaves += node.taxa.size();
    }
    return;
  }
  if (node.leaf) return;  // excluded leaf
  for (auto const& c : node.children)
    emit_fragments(c, excl, min_leaves, dir, reference, cgs, counter,
                   tiny_leaves);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 3 && std::string(argv[1]) == "fragments") {
      // fragments <tree.pb.gz> <min_leaves> <out_dir> <excl1.ids> [excl2.ids]
      std::string reference;
      std::map<std::string, larch::compact_genome> leaf_cgs;
      auto root = load_tree_any(argv[2], &reference, &leaf_cgs);
      std::size_t min_leaves = std::strtoull(argv[3], nullptr, 10);
      std::set<std::string> excl;
      for (int i = 5; i < argc; ++i)
        for (auto const& id : read_ids(argv[i])) excl.insert(id);
      std::size_t counter = 0, tiny = 0;
      emit_fragments(root, excl, min_leaves, argv[4], reference, leaf_cgs,
                     counter, tiny);
      std::fprintf(stderr, "fragments: %zu emitted (>= %zu leaves), "
                           "%zu leaves in sub-min fragments\n",
                   counter, min_leaves, tiny);
      return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "preorder") {
      if (argc != 5 && argc != 6) {
        std::fprintf(stderr,
                     "usage: %s preorder <tree.pb.gz> <index> <out_ids.txt> "
                     "[out_subtree.pb.gz]\n",
                     argv[0]);
        return 2;
      }
      std::string reference;
      std::map<std::string, larch::compact_genome> leaf_cgs;
      auto root = load_tree_any(argv[2], &reference, &leaf_cgs);
      std::size_t k = 0, target = std::strtoull(argv[3], nullptr, 10);
      auto* hit = preorder_pick(root, k, target);
      if (!hit) {
        std::fprintf(stderr, "error: preorder index %zu out of range (%zu nodes)\n",
                     target, k);
        return 1;
      }
      std::FILE* out = std::fopen(argv[4], "w");
      if (!out) throw std::runtime_error("cannot write " + std::string(argv[4]));
      for (auto const& id : hit->taxa) std::fprintf(out, "%s\n", id.c_str());
      std::fclose(out);
      if (argc == 6) write_tree(*hit, reference, leaf_cgs, argv[5]);
      std::fprintf(stderr, "preorder %zu: %zu leaves\n", target,
                   hit->taxa.size());
      return 0;
    }
    if (argc == 5 && std::string(argv[1]) == "restrict") {
      // restrict <tree.pb.gz> <ids.txt> <out_subtree.pb.gz>
      std::string reference;
      std::map<std::string, larch::compact_genome> leaf_cgs;
      auto root = load_tree_any(argv[2], &reference, &leaf_cgs);
      auto want = read_ids(argv[3]);
      std::set<std::string> want_set(want.begin(), want.end());
      auto sub = restrict_to(std::move(root), want_set);
      if (sub.children.empty() && !sub.leaf)
        throw std::runtime_error("restrict: empty result");
      write_tree(sub, reference, leaf_cgs, argv[4]);
      std::fprintf(stderr, "restrict: %zu target leaves -> %s\n",
                   want_set.size(), argv[4]);
      return 0;
    }
    if (argc != 5 && argc != 6) {
      std::fprintf(stderr,
                   "usage: %s <tree.pb.gz> <min> <max> <out_ids.txt> "
                   "[out_subtree.pb.gz]\n",
                   argv[0]);
      return 2;
    }
    std::string reference;
    std::map<std::string, larch::compact_genome> leaf_cgs;
    auto root = load_tree_any(argv[1], &reference, &leaf_cgs);
    auto min_l = static_cast<std::size_t>(std::stoul(argv[2]));
    auto max_l = static_cast<std::size_t>(std::stoul(argv[3]));
    auto* hit = find_clade(root, min_l, max_l);
    if (!hit) {
      std::fprintf(stderr, "error: no clade with leaf count in [%zu,%zu]\n",
                   min_l, max_l);
      return 1;
    }
    std::FILE* out = std::fopen(argv[4], "w");
    if (!out) throw std::runtime_error("cannot write " + std::string(argv[4]));
    for (auto const& id : hit->taxa) std::fprintf(out, "%s\n", id.c_str());
    std::fclose(out);
    std::fprintf(stderr, "picked clade with %zu leaves -> %s\n",
                 hit->taxa.size(), argv[4]);
    if (argc == 6) write_tree(*hit, reference, leaf_cgs, argv[5]);
    return 0;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

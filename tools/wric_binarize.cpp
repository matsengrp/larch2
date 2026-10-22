// wric_binarize: load a tree pb (any arity) and resolve every polytomy
// deterministically as a LEFT COMB: ((c0,c1),c2),... in input child order.
// Unary nodes (if any) are hoisted. Leaves are untouched. Output is a binary
// tree pb with Fitch CG reassignment (write_tree). Optionally writes the leaf
// sample ids (sorted, one per line) for graft contracts.
// usage: wric_binarize <in.pb.gz> <out.pb.gz> [out_ids.txt]
#include "wric_subtree_common.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>

using namespace wsub;

namespace {

tnode binarize(tnode node) {
  if (node.leaf) return node;
  std::vector<tnode> kids;
  kids.reserve(node.children.size());
  for (auto& c : node.children) kids.push_back(binarize(std::move(c)));
  if (kids.size() == 1) return std::move(kids[0]);  // hoist unary
  if (kids.size() <= 2) {
    node.children = std::move(kids);
    node.taxa.clear();
    for (auto& c : node.children) node.taxa = sorted_union(node.taxa, c.taxa);
    return node;
  }
  // left comb: acc = ((c0,c1),c2)...
  tnode acc;
  acc.children = {std::move(kids[0]), std::move(kids[1])};
  acc.taxa = sorted_union(acc.children[0].taxa, acc.children[1].taxa);
  for (std::size_t i = 2; i < kids.size(); ++i) {
    tnode next;
    next.children = {std::move(acc), std::move(kids[i])};
    next.taxa =
        sorted_union(next.children[0].taxa, next.children[1].taxa);
    acc = std::move(next);
  }
  return acc;
}

void collect_ids(tnode const& node, std::vector<std::string>& out) {
  if (node.leaf) {
    out.push_back(node.sid);
    return;
  }
  for (auto const& c : node.children) collect_ids(c, out);
}

std::size_t max_arity(tnode const& n) {
  if (n.leaf) return 0;
  std::size_t m = n.children.size();
  for (auto const& c : n.children) m = std::max(m, max_arity(c));
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3 && argc != 4) {
      std::fprintf(stderr,
                   "usage: %s <in.pb.gz> <out.pb.gz> [out_ids.txt]\n",
                   argv[0]);
      return 2;
    }
    std::string reference;
    std::map<std::string, larch::compact_genome> leaf_cgs;
    auto root = load_tree_any(argv[1], &reference, &leaf_cgs);
    std::size_t before = max_arity(root);
    auto bin = binarize(std::move(root));
    write_tree(bin, reference, leaf_cgs, argv[2]);
    std::size_t after = max_arity(bin);
    if (argc == 4) {
      std::vector<std::string> ids;
      collect_ids(bin, ids);
      std::sort(ids.begin(), ids.end());
      std::ofstream of(argv[3]);
      for (auto const& id : ids) of << id << '\n';
      std::fprintf(stderr, "ids: %zu -> %s\n", ids.size(), argv[3]);
    }
    std::fprintf(stderr, "binarize: max arity %zu -> %zu\n", before, after);
    return 0;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

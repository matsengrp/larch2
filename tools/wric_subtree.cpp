// wric_subtree: subtree extraction and grafting for the directed-injection
// loop (rounds 2-3 of ~/tmp/diversity-signals.md).  Tree I/O helpers live in
// wric_subtree_common.hpp.
//
// Modes:
//   extract <witness.pb.gz> <ids.txt> <out.pb.gz>
//   graft <witness.pb.gz> <ids.txt> <outdir> <cand1.pb.gz> [cand2...]
//
// The outputs merge into the enriched DAG with dagutil --dag-pb ... (same
// leaf set = the standard hDAG merger path).  NOTE: save_proto_dag writes
// RAW protobuf; the loaders sniff the format, but gzip explicitly when
// content-hashing.

#include "wric_subtree_common.hpp"

#include <cstdio>

using namespace wsub;

int main(int argc, char** argv) {
  try {
    if (argc >= 5 && std::string(argv[1]) == "extract") {
      auto ids = read_ids(argv[3]);
      std::string reference;
      std::map<std::string, larch::compact_genome> leaf_cgs;
      bool any = std::getenv("WRIC_GRAFT_ANY");
      auto root = any ? load_tree_any(argv[2], &reference, &leaf_cgs)
                      : load_tree(argv[2], &reference, &leaf_cgs);
      tnode const* found = nullptr;
      std::vector<tnode const*> stack{&root};
      while (!stack.empty()) {
        auto* n = stack.back();
        stack.pop_back();
        if (n->leaf) continue;
        if (n->taxa == ids) {
          found = n;
          break;
        }
        for (auto const& c : n->children) stack.push_back(&c);
      }
      if (!found) throw std::runtime_error("subtree: target taxon set not found");
      write_tree(*found, reference, leaf_cgs, argv[4]);
      std::fprintf(stderr, "extract: %zu taxa -> %s\n", ids.size(), argv[4]);
      return 0;
    }
    if (argc >= 6 && std::string(argv[1]) == "graft") {
      auto ids = read_ids(argv[3]);
      std::string outdir = argv[4];
      std::string reference;
      std::map<std::string, larch::compact_genome> leaf_cgs;
      bool any = std::getenv("WRIC_GRAFT_ANY");
      auto root = any ? load_tree_any(argv[2], &reference, &leaf_cgs)
                      : load_tree(argv[2], &reference, &leaf_cgs);
      for (int i = 5; i < argc; ++i) {
        std::string cand_ref_ignored;
        auto cand = any ? load_tree_any(argv[i], &cand_ref_ignored, nullptr)
                        : load_tree(argv[i], &cand_ref_ignored, nullptr);
        if (cand.taxa != ids)
          throw std::runtime_error(std::string("graft: candidate ") + argv[i] +
                                   " has wrong taxon set");
        auto grafted = any ? splice_any(root, ids, cand) : splice(root, ids, cand);
        auto out = outdir + "/graft_" + std::to_string(i - 5) + ".pb.gz";
        write_tree(grafted, reference, leaf_cgs, out);
        std::fprintf(stderr, "graft%s: %s -> %s\n", any ? "-any" : "",
                     argv[i], out.c_str());
      }
      return 0;
    }
    std::fprintf(stderr,
                 "usage: %s extract <witness.pb.gz> <ids.txt> <out.pb.gz>\n"
                 "       %s graft <witness.pb.gz> <ids.txt> <outdir> "
                 "<cand1.pb.gz> [cand2...]\n",
                 argv[0], argv[0]);
    return 2;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}

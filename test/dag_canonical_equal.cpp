// dag_canonical_equal: small helper for the checkpoint integration test.
//
// save_proto_dag does not guarantee any output ordering — node proto-IDs are
// allocated in dag.get_all_nodes() iteration order, which depends on the
// internal merge state. Two runs that produce logically equivalent DAGs can
// therefore produce .pb.gz files that differ in node IDs even though the
// underlying topology and mutations match.
//
// This tool loads both files via load_proto_dag (which gives us a phylo_dag
// with computed compact-genome strings), assigns each node a content-based
// canonical key — for leaves the sample_id, for inner nodes the
// compact-genome string — and rewrites every edge as a tuple of canonical
// parent key, parent clade, canonical child key, and a sorted multiset of
// mutations. Two DAGs are canonically equal iff their multisets of these
// canonical edge tuples match exactly.
//
// Usage: dag_canonical_equal <a.pb.gz> <b.pb.gz>
// Exit:  0 if canonical-equal, 1 otherwise.

#include <larch/load_proto_dag.hpp>

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace larch;

namespace {

std::string node_canonical_key(phylo_dag& d, std::size_t idx) {
  auto nv = d.get_node(idx);
  return std::visit(
      [](auto n) -> std::string {
        if constexpr (requires { n.reference_sequence(); }) {
          return "UA";
        } else if constexpr (requires { n.sample_id(); n.cg(); }) {
          // Leaf: sample_id alone is unique inside a merge.
          return "L:" + std::string{n.sample_id()};
        } else if constexpr (requires { n.cg(); }) {
          return "I:" + n.cg().to_string();
        } else {
          return "?";
        }
      },
      nv);
}

// Serialize each edge as a comparable string. Format:
//   <parent_key>|<parent_clade>|<child_key>|<sorted-mutations>
// where mutations are joined as "pos:par_nuc->mut_nuc" tokens in sorted
// order.
std::string edge_signature(phylo_dag& d, std::size_t edge_idx) {
  auto ev = d.get_edge(edge_idx);
  return std::visit(
      [&](auto edge) {
        auto p_idx =
            std::visit([](auto n) { return n.index(); }, edge.get_parent());
        auto c_idx =
            std::visit([](auto n) { return n.index(); }, edge.get_child());
        std::string p_key = node_canonical_key(d, p_idx);
        std::string c_key = node_canonical_key(d, c_idx);
        std::vector<std::string> mut_strs;
        for (auto& [pos, nucs] : edge.mutations()) {
          std::ostringstream s;
          s << pos << ':' << static_cast<int>(nucs.first.raw()) << "->"
            << static_cast<int>(nucs.second.raw());
          mut_strs.push_back(s.str());
        }
        std::sort(mut_strs.begin(), mut_strs.end());
        std::ostringstream out;
        out << p_key << '|' << edge.clade_index() << '|' << c_key << '|';
        for (std::size_t i = 0; i < mut_strs.size(); ++i) {
          if (i) out << ',';
          out << mut_strs[i];
        }
        return out.str();
      },
      ev);
}

std::vector<std::string> all_edge_signatures(phylo_dag& d) {
  std::vector<std::string> sigs;
  for (auto ev : d.get_all_edges()) {
    auto idx = std::visit([](auto e) { return e.index(); }, ev);
    sigs.push_back(edge_signature(d, idx));
  }
  std::sort(sigs.begin(), sigs.end());
  return sigs;
}

std::vector<std::string> all_node_keys(phylo_dag& d) {
  std::vector<std::string> keys;
  for (auto nv : d.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    keys.push_back(node_canonical_key(d, idx));
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: dag_canonical_equal <a.pb.gz> <b.pb.gz>\n";
    return 2;
  }
  auto da = load_proto_dag(argv[1]);
  auto db = load_proto_dag(argv[2]);

  if (get_reference_sequence(da) != get_reference_sequence(db)) {
    std::cerr << "dag_canonical_equal: differ (reference sequence)\n";
    return 1;
  }

  auto a_nodes = all_node_keys(da);
  auto b_nodes = all_node_keys(db);
  if (a_nodes != b_nodes) {
    std::cerr << "dag_canonical_equal: differ (node-key multiset; "
              << a_nodes.size() << " vs " << b_nodes.size() << ")\n";
    return 1;
  }

  auto a_edges = all_edge_signatures(da);
  auto b_edges = all_edge_signatures(db);
  if (a_edges != b_edges) {
    std::cerr << "dag_canonical_equal: differ (edge-signature multiset; "
              << a_edges.size() << " vs " << b_edges.size() << ")\n";
    // Print a few divergent signatures to aid debugging.
    std::set<std::string> as{a_edges.begin(), a_edges.end()};
    std::set<std::string> bs{b_edges.begin(), b_edges.end()};
    std::size_t shown = 0;
    for (auto& s : as) {
      if (!bs.contains(s) && shown++ < 3)
        std::cerr << "  only in a: " << s << "\n";
    }
    shown = 0;
    for (auto& s : bs) {
      if (!as.contains(s) && shown++ < 3)
        std::cerr << "  only in b: " << s << "\n";
    }
    return 1;
  }

  return 0;
}

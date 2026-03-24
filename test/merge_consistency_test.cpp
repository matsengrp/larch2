#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/fasta.hpp>
#include <larch/merge.hpp>
#include <larch/newick.hpp>
#include <larch/io_util.hpp>
#include <larch/thread_pool.hpp>

#include <cassert>
#include <filesystem>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

using namespace larch;

// Build a tree from FASTA+Newick (same logic as dagutil's
// build_from_fasta_newick).
static phylo_dag build_from_fasta_newick(std::string_view fasta_path,
                                         std::string_view newick_path,
                                         std::string_view refseq_path) {
  auto ref_bytes = read_file(refseq_path);
  std::string_view ref_content{ref_bytes.data(), ref_bytes.size()};
  std::string reference;
  if (!ref_content.empty() && ref_content[0] == '>') {
    auto entries = read_fasta(refseq_path);
    if (!entries.empty()) reference = std::move(entries[0].sequence);
  } else {
    for (char c : ref_content) {
      if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
        reference +=
            static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }

  auto entries = read_fasta(fasta_path);
  std::unordered_map<std::string, std::string> fasta_map;
  for (auto& e : entries) fasta_map[e.name] = std::move(e.sequence);

  auto nw_bytes = read_file(newick_path);
  std::string newick_str{nw_bytes.data(), nw_bytes.size()};

  phylo_dag d;
  std::unordered_map<std::size_t, std::size_t> num_children;
  std::map<std::size_t, std::string> seq_ids;
  struct nw_edge_t {
    std::size_t parent, child, clade;
  };
  std::vector<nw_edge_t> nw_edges;

  parse_newick(
      newick_str,
      [&](std::size_t id, std::string_view label, std::optional<double>) {
        seq_ids[id] = std::string{label};
      },
      [&](std::size_t parent, std::size_t child) {
        nw_edges.push_back({parent, child, num_children[parent]++});
      });

  std::unordered_map<std::size_t, bool> has_children;
  std::unordered_map<std::size_t, bool> is_child_map;
  for (auto& e : nw_edges) {
    has_children[e.parent] = true;
    is_child_map[e.child] = true;
  }
  std::size_t newick_root = 0;
  for (auto& [id, _] : seq_ids) {
    if (!is_child_map.contains(id)) {
      newick_root = id;
      break;
    }
  }

  std::unordered_map<std::size_t, std::size_t> nw_to_dag;
  for (auto& [nw_id, label] : seq_ids) {
    bool is_leaf_node = !has_children.contains(nw_id);
    if (is_leaf_node) {
      auto leaf = d.append_node<node_kind::leaf>();
      if (!label.empty()) leaf.sample_id() = label;
      nw_to_dag[nw_id] = leaf.index();
    } else {
      auto inner = d.append_node<node_kind::inner>();
      nw_to_dag[nw_id] = inner.index();
    }
  }

  for (auto& ne : nw_edges) {
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = ne.clade;
    auto pi = nw_to_dag[ne.parent];
    auto ci = nw_to_dag[ne.child];
    std::visit([&](auto n) { edge.set_parent(n); }, d.get_node(pi));
    std::visit([&](auto n) { edge.set_child(n); }, d.get_node(ci));
  }

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = reference;
  d.set_root(ua);
  {
    auto edge = ua.append_child<edge_kind::clade>();
    std::visit([&](auto n) { edge.set_child(n); },
               d.get_node(nw_to_dag[newick_root]));
    edge.clade_index() = 0;
  }

  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            auto it = fasta_map.find(node.sample_id());
            if (it == fasta_map.end()) return;
            auto& seq = it->second;
            std::map<mutation_position, nuc_base> muts;
            for (std::size_t i = 0; i < seq.size() && i < reference.size();
                 ++i) {
              auto ref_base = nuc_base::from_char(reference[i]);
              auto seq_base = nuc_base::from_char(seq[i]);
              if (!(ref_base == seq_base)) muts[i + 1] = seq_base;
            }
            node.cg() = compact_genome{std::move(muts)};
          }
        },
        nv);
  }

  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);
  return d;
}

// Verify that every edge's stored mutations exactly match the Hamming
// distance between adjacent node CGs (full content comparison, not just
// count).  Returns (total_mutations, inconsistent_count).
static std::pair<std::size_t, std::size_t> check_consistency(
    phylo_dag& d, std::string_view label) {
  auto const& ref = get_reference_sequence(d);
  std::size_t total_muts = 0;
  std::size_t inconsistent = 0;

  for (auto ev : d.get_all_edges()) {
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

          auto expected =
              compact_genome::to_edge_mutations(ref, parent_cg, child_cg);
          if (expected != edge.mutations()) inconsistent++;
          total_muts += edge.mutations().size();
        },
        ev);
  }

  std::println("  [{}] {} edges: {} mutations, {} inconsistent", label,
               edge_count(d), total_muts, inconsistent);
  return {total_muts, inconsistent};
}

static void test_merge_roundtrip(std::string const& repro) {
  std::string fasta = repro + "/input.fa";
  std::string refseq = repro + "/root.fa";

  auto tree0 = build_from_fasta_newick(fasta, repro + "/tree0.nwk", refseq);
  auto tree1 = build_from_fasta_newick(fasta, repro + "/tree1.nwk", refseq);

  auto [t0_muts, t0_bad] = check_consistency(tree0, "tree0");
  assert(t0_bad == 0 && "tree0 edge mutations inconsistent with CGs");
  auto [t1_muts, t1_bad] = check_consistency(tree1, "tree1");
  assert(t1_bad == 0 && "tree1 edge mutations inconsistent with CGs");

  // Merge 2 trees and check consistency
  auto const& ref = get_reference_sequence(tree0);
  merge m{ref};
  m.add_dag(tree0);
  m.add_dag(tree1);
  auto& result = m.get_result();
  auto [fresh_muts, fresh_bad] = check_consistency(result, "merged-fresh");
  assert(fresh_bad == 0 && "fresh merged DAG inconsistent");

  // recompute_compact_genomes should preserve consistency
  recompute_compact_genomes(result);
  auto [recomp_muts, recomp_bad] =
      check_consistency(result, "recomputed-fresh");
  assert(recomp_bad == 0 && "recompute_compact_genomes broke CG consistency");
  assert(recomp_muts == fresh_muts);

  // Save/load roundtrip should preserve mutations and CG consistency
  auto tmp = std::filesystem::temp_directory_path() /
             ("iris_merge_consistency_" + std::to_string(::getpid()) + ".pb");
  save_proto_dag(result, tmp.string());
  auto loaded = load_proto_dag(tmp.string());
  std::filesystem::remove(tmp);

  auto [loaded_muts, loaded_bad] = check_consistency(loaded, "loaded");
  assert(loaded_bad == 0 && "save/load broke CG consistency");
  assert(loaded_muts == fresh_muts && "save/load changed total mutations");

  // Re-merge of loaded DAG should give same result
  auto const& ref2 = get_reference_sequence(loaded);
  merge m2{ref2};
  m2.add_dag(loaded);
  auto& result2 = m2.get_result();
  auto [rem_muts, rem_bad] = check_consistency(result2, "re-merged");
  assert(rem_bad == 0 && "re-merge broke CG consistency");
  assert(rem_muts == fresh_muts && "re-merge changed total mutations");

  std::println("  PASS");
}

int main() {
  std::string repro =
      "/home/matsen/re/pz/maple/experiments/"
      "2026-03-18-madag-sampling-factorial/runs/dagmerge_debug_rota";

  if (!std::filesystem::exists(repro + "/input.fa")) {
    std::println(
        "(skipping merge_consistency_test: repro data not found at {})", repro);
    return 0;
  }

  std::println("test_merge_roundtrip");
  test_merge_roundtrip(repro);

  std::println("All merge consistency checks passed!");
  return 0;
}

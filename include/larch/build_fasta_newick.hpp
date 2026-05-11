#pragma once

#include <larch/compute.hpp>
#include <larch/fasta.hpp>
#include <larch/io_util.hpp>
#include <larch/newick.hpp>

#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace larch {

// Build a phylo_dag tree from a pre-loaded FASTA map, a Newick file, and a
// reference sequence string.  Assigns leaf compact genomes by diffing against
// the reference, then runs Fitch assignment for inner nodes.
inline phylo_dag build_from_fasta_newick(
    std::unordered_map<std::string, std::string> const& fasta_map,
    std::string_view newick_path, std::string const& reference) {
  // Read newick string
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

  // Determine leaf vs inner
  std::unordered_map<std::size_t, bool> has_children;
  std::unordered_map<std::size_t, bool> is_child;
  for (auto& e : nw_edges) {
    has_children[e.parent] = true;
    is_child[e.child] = true;
  }

  // Find newick root
  std::size_t newick_root = 0;
  for (auto& [id, unused] : seq_ids) {
    if (!is_child.contains(id)) {
      newick_root = id;
      break;
    }
  }

  // Create nodes
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

  // Create edges
  for (auto& ne : nw_edges) {
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = ne.clade;
    std::visit([&](auto parent) { edge.set_parent(parent); },
               d.get_node(nw_to_dag.at(ne.parent)));
    std::visit([&](auto child) { edge.set_child(child); },
               d.get_node(nw_to_dag.at(ne.child)));
  }

  // Add UA node
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = reference;
  d.set_root(ua);
  {
    auto edge = ua.append_child<edge_kind::clade>();
    std::visit([&](auto child) { edge.set_child(child); },
               d.get_node(nw_to_dag.at(newick_root)));
    edge.clade_index() = 0;
  }

  // Assign leaf CGs from FASTA by diffing against reference
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

// Convenience overload: loads a single FASTA file and a reference sequence
// file (may be FASTA or raw text), then delegates to the map-based overload.
inline phylo_dag build_from_fasta_newick(std::string_view fasta_path,
                                         std::string_view newick_path,
                                         std::string_view refseq_path) {
  // Read reference sequence (may be FASTA or raw text)
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

  // Read FASTA leaf sequences
  auto entries = read_fasta(fasta_path);
  std::unordered_map<std::string, std::string> fasta_map;
  for (auto& e : entries) fasta_map[e.name] = std::move(e.sequence);

  return build_from_fasta_newick(fasta_map, newick_path, reference);
}

}  // namespace larch

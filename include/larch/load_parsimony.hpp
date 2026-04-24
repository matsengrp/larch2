#pragma once

#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>
#include <larch/newick.hpp>
#include <larch/protobuf.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace larch {

// --- Proto message structs matching parsimony.proto ---

struct pars_mut {
  int32_t position;
  int32_t ref_nuc;
  int32_t par_nuc;
  std::vector<int32_t> mut_nuc;
  std::string chromosome;
};

struct pars_iupac_site {
  int32_t position;
  int32_t state_set;
};

struct pars_mutation_list {
  std::vector<pars_mut> mutation;
  std::vector<int32_t> ambiguous_sites;
  std::vector<pars_iupac_site> iupac_sites;
};

struct pars_condensed_node {
  std::string node_name;
  std::vector<std::string> condensed_leaves;
};

struct pars_node_metadata {
  std::vector<std::string> clade_annotations;
};

struct pars_data {
  std::string newick;
  std::vector<pars_mutation_list> node_mutations;
  std::vector<pars_condensed_node> condensed_nodes;
  std::vector<pars_node_metadata> metadata;
};

inline phylo_dag load_parsimony_tree(std::string_view path,
                                     std::string_view reference_sequence) {
  auto msg = pb::decode_file<pars_data>(path);

  phylo_dag d;

  // Parse newick
  std::unordered_map<std::size_t, std::size_t> num_children;
  std::map<std::size_t, std::string> seq_ids;
  struct newick_edge_t {
    std::size_t parent;
    std::size_t child;
    std::size_t clade;
  };
  std::vector<newick_edge_t> newick_edges;

  parse_newick(
      msg.newick,
      [&](std::size_t id, std::string_view label, std::optional<double>) {
        seq_ids[id] = std::string{label};
      },
      [&](std::size_t parent, std::size_t child) {
        newick_edges.push_back({parent, child, num_children[parent]++});
      });

  // Determine leaf vs inner
  std::unordered_map<std::size_t, bool> has_children;
  std::unordered_map<std::size_t, bool> is_child;
  for (auto& e : newick_edges) {
    has_children[e.parent] = true;
    is_child[e.child] = true;
  }

  // Find newick root (not a child)
  std::size_t newick_root = 0;
  for (auto& [id, _] : seq_ids) {
    if (!is_child.contains(id)) {
      newick_root = id;
      break;
    }
  }

  // Create all newick nodes
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

  // Create newick edges
  for (auto& ne : newick_edges) {
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = ne.clade;
    auto pi = nw_to_dag[ne.parent];
    auto ci = nw_to_dag[ne.child];
    std::visit([&](auto n) { edge.set_parent(n); }, d.get_node(pi));
    std::visit([&](auto n) { edge.set_child(n); }, d.get_node(ci));
  }

  // Add UA node above newick root (like larch's AddUA)
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{reference_sequence};
  d.set_root(ua);
  {
    auto edge = ua.append_child<edge_kind::clade>();
    auto root_dag_idx = nw_to_dag[newick_root];
    std::visit([&](auto n) { edge.set_child(n); }, d.get_node(root_dag_idx));
    edge.clade_index() = 0;
  }

  // Apply mutations in preorder.
  // node_mutations are in preorder traversal order of the newick tree.
  std::unordered_map<std::size_t, std::map<mutation_position, uint8_t>>
      ambiguity_sets_by_node;
  std::size_t muts_idx = 0;
  auto apply_muts = [&](auto& self, std::size_t dag_idx) -> void {
    if (muts_idx >= msg.node_mutations.size()) return;
    auto& ml = msg.node_mutations[muts_idx++];
    if (!ml.ambiguous_sites.empty()) {
      auto& ambiguity_sets = ambiguity_sets_by_node[dag_idx];
      for (auto pos : ml.ambiguous_sites)
        ambiguity_sets[static_cast<mutation_position>(pos)] = 0b1111;
    }
    for (auto const& site : ml.iupac_sites) {
      auto state_set = static_cast<uint8_t>(site.state_set) & 0b1111;
      if (state_set != 0) {
        ambiguity_sets_by_node[dag_idx][static_cast<mutation_position>(
            site.position)] = state_set;
      }
    }

    std::visit(
        [&](auto node) {
          // Set this node's parent edge mutations
          for (auto pe_var : node.get_parents()) {
            std::visit(
                [&](auto pe) {
                  edge_mutations muts;
                  for (auto& m : ml.mutation) {
                    muts[static_cast<mutation_position>(m.position)] = {
                        nuc_base::from_proto(m.par_nuc),
                        nuc_base::from_proto(m.mut_nuc.empty() ? 0
                                                               : m.mut_nuc[0])};
                  }
                  pe.mutations() = std::move(muts);
                },
                pe_var);
            break;
          }
          // Recurse into children
          for (auto ce_var : node.get_children()) {
            std::visit(
                [&](auto ce) {
                  std::visit([&](auto c) { self(self, c.index()); },
                             ce.get_child());
                },
                ce_var);
          }
        },
        d.get_node(dag_idx));
  };

  // Start from newick root (which is now UA's child)
  apply_muts(apply_muts, nw_to_dag[newick_root]);

  // Expand condensed nodes
  for (auto& cn : msg.condensed_nodes) {
    std::string condensed_name = cn.node_name;

    std::size_t condensed_dag_idx = larch::no_idx;
    for (auto nv : d.get_all_nodes()) {
      std::visit(
          [&](auto n) {
            if constexpr (requires { n.sample_id(); }) {
              if (n.sample_id() == condensed_name)
                condensed_dag_idx = n.index();
            }
          },
          nv);
    }
    if (condensed_dag_idx == larch::no_idx) continue;

    edge_mutations parent_muts;
    std::size_t parent_dag_idx = larch::no_idx;
    std::size_t parent_clade = 0;
    std::visit(
        [&](auto n) {
          for (auto pe_var : n.get_parents()) {
            std::visit(
                [&](auto pe) {
                  parent_muts = pe.mutations();
                  parent_clade = pe.clade_index();
                  std::visit([&](auto p) { parent_dag_idx = p.index(); },
                             pe.get_parent());
                },
                pe_var);
            break;
          }
        },
        d.get_node(condensed_dag_idx));

    for (std::size_t j = 0; j < cn.condensed_leaves.size(); ++j) {
      std::string sib_name = cn.condensed_leaves[j];
      if (j == 0) {
        std::visit(
            [&](auto n) {
              if constexpr (requires { n.sample_id(); }) {
                n.sample_id() = sib_name;
              }
            },
            d.get_node(condensed_dag_idx));
      } else {
        auto new_leaf = d.append_node<node_kind::leaf>();
        new_leaf.sample_id() = sib_name;

        auto new_edge = d.append_edge<edge_kind::clade>();
        new_edge.mutations() = parent_muts;
        new_edge.clade_index() = parent_clade;
        new_edge.set_child(new_leaf);

        std::visit([&](auto n) { new_edge.set_parent(n); },
                   d.get_node(parent_dag_idx));
      }
    }
  }

  recompute_compact_genomes(d);

  for (auto& [dag_idx, ambiguity_sets] : ambiguity_sets_by_node) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.cg(); }) {
            node.cg().set_ambiguity_sets(std::move(ambiguity_sets));
          }
        },
        d.get_node(dag_idx));
  }

  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [](auto n) {
          if constexpr (requires {
                          n.sample_id();
                          n.cg();
                        }) {
            if (n.sample_id().empty()) n.sample_id() = n.cg().to_string();
          }
        },
        nv);
  }

  build_clade_offsets(d);
  return d;
}

}  // namespace larch

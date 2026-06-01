#include <larch/chart_bnb_trim_apply.hpp>

#include <larch/site_patterns.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <variant>

namespace larch {

char const* chart_bnb_trim_application_mode_name(
    chart_bnb_trim_application_mode mode) {
  switch (mode) {
    case chart_bnb_trim_application_mode::production_mask_superset:
      return "production_mask_superset";
    case chart_bnb_trim_application_mode::optimal_topology_materialize:
      return "optimal_topology_materialize";
  }
  return "unknown";
}

namespace chart_bnb_trim_apply_detail {

static std::size_t count_true(std::vector<bool> const& values) {
  return static_cast<std::size_t>(
      std::count(values.begin(), values.end(), true));
}

static std::string refinement_exactness_label(
    polytomy_refinement_audit const& audit) {
  return audit.exact_for_soft_polytomies ? "EXACT"
                                         : "BOUNDED_REFINED_GRAMMAR";
}

struct production_taxon_signature {
  std::vector<taxon_id> parent;
  std::vector<std::vector<taxon_id>> children;

  bool operator<(production_taxon_signature const& other) const {
    return std::tie(parent, children) < std::tie(other.parent, other.children);
  }
  bool operator==(production_taxon_signature const&) const = default;
};

static production_taxon_signature production_signature(
    clade_grammar const& grammar, production_id pid) {
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error(
        "chart B&B trim apply: production id out of range");
  }
  auto const& prod = grammar.productions[pid];
  if (prod.parent == no_clade || prod.parent >= grammar.clades.size()) {
    throw std::runtime_error(
        "chart B&B trim apply: production parent out of range");
  }
  production_taxon_signature key;
  key.parent = grammar.clades[prod.parent].taxa;
  key.children.reserve(prod.children.size());
  for (auto child : prod.children) {
    if (child == no_clade || child >= grammar.clades.size()) {
      throw std::runtime_error(
          "chart B&B trim apply: production child out of range");
    }
    key.children.push_back(grammar.clades[child].taxa);
  }
  std::sort(key.children.begin(), key.children.end());
  return key;
}

static std::set<production_taxon_signature> production_signature_set(
    clade_grammar const& grammar) {
  std::set<production_taxon_signature> keys;
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    keys.insert(production_signature(grammar, static_cast<production_id>(pid)));
  }
  return keys;
}

static void validate_trim_mask_for_apply(clade_grammar const& grammar,
                                         multisite_trim_result const& trim) {
  if (!trim.keep_production_exact ||
      trim.keep_mask_kind !=
          multisite_keep_mask_kind::exact_optimal_production_union) {
    throw std::runtime_error(
        "chart B&B trim apply: production-mask mode requires an exact "
        "optimal-production union keep mask");
  }
  if (trim.keep_production.size() != grammar.productions.size()) {
    throw std::runtime_error(
        "chart B&B trim apply: keep_production size does not match grammar");
  }
}

static void validate_no_synthetic_provenance_for_direct_mask(
    polytomy_refinement_result const& refinement) {
  if (refinement.production_info.size() != refinement.grammar.productions.size()) {
    throw std::runtime_error(
        "chart B&B trim apply: refinement production-info size mismatch");
  }
  for (std::size_t pid = 0; pid < refinement.production_info.size(); ++pid) {
    if (!refined_production_has_synthetic_polytomy_provenance(
            refinement.production_info[pid])) {
      continue;
    }
    throw std::runtime_error(
        "chart B&B trim apply: direct production-mask source-edge pruning "
        "does not support synthetic polytomy-refinement provenance "
        "(production " +
        std::to_string(pid) +
        "); use optimal-topology materialization for refined-binary output");
  }
}

static void copy_node_annotations(phylo_dag::node_variant_type src,
                                  phylo_dag::node_variant_type dst) {
  std::visit(
      [](auto src_node, auto dst_node) {
        if constexpr (requires {
                        src_node.reference_sequence();
                        dst_node.reference_sequence();
                      }) {
          dst_node.reference_sequence() = src_node.reference_sequence();
        }
        if constexpr (requires {
                        src_node.cg();
                        dst_node.cg();
                      }) {
          dst_node.cg() = src_node.cg();
        }
        if constexpr (requires {
                        src_node.sample_id();
                        dst_node.sample_id();
                      }) {
          dst_node.sample_id() = src_node.sample_id();
        }
      },
      src, dst);
}

struct cloned_phylo_dag {
  phylo_dag dag;
  std::vector<std::size_t> source_node_to_clone;
  std::vector<std::size_t> source_edge_to_clone;
};

static cloned_phylo_dag clone_phylo_dag(phylo_dag& source) {
  cloned_phylo_dag cloned;
  auto& out = cloned.dag;
  cloned.source_node_to_clone.assign(source.node_high_mark(), no_idx);
  cloned.source_edge_to_clone.assign(source.edge_high_mark(), no_idx);

  for (auto src_nv : source.get_all_nodes()) {
    auto src_idx = std::visit([](auto node) { return node.index(); }, src_nv);
    auto kind = std::visit([](auto node) { return node_kind_of(node); }, src_nv);
    auto dst_nv = out.append_node(kind);
    copy_node_annotations(src_nv, dst_nv);
    auto dst_idx = std::visit([](auto node) { return node.index(); }, dst_nv);
    cloned.source_node_to_clone[src_idx] = dst_idx;
  }

  auto source_root = get_root_idx(source);
  if (source_root >= cloned.source_node_to_clone.size() ||
      cloned.source_node_to_clone[source_root] == no_idx) {
    throw std::runtime_error("chart B&B trim apply: source root was not cloned");
  }
  std::visit([&](auto root) { out.set_root(root); },
             out.get_node(cloned.source_node_to_clone[source_root]));

  for (auto src_ev : source.get_all_edges()) {
    std::visit(
        [&](auto src_edge) {
          auto src_edge_idx = src_edge.index();
          auto parent = get_parent_idx(source, src_edge_idx);
          auto child = get_child_idx(source, src_edge_idx);
          if (parent >= cloned.source_node_to_clone.size() ||
              child >= cloned.source_node_to_clone.size() ||
              cloned.source_node_to_clone[parent] == no_idx ||
              cloned.source_node_to_clone[child] == no_idx) {
            throw std::runtime_error(
                "chart B&B trim apply: source edge endpoint was not cloned");
          }
          auto dst_edge = out.append_edge<edge_kind::clade>();
          dst_edge.clade_index() = src_edge.clade_index();
          dst_edge.edge_weight() = src_edge.edge_weight();
          dst_edge.mutations() = src_edge.mutations();
          std::visit([&](auto parent_node) { dst_edge.set_parent(parent_node); },
                     out.get_node(cloned.source_node_to_clone[parent]));
          std::visit([&](auto child_node) { dst_edge.set_child(child_node); },
                     out.get_node(cloned.source_node_to_clone[child]));
          cloned.source_edge_to_clone[src_edge_idx] = dst_edge.index();
        },
        src_ev);
  }
  build_clade_offsets(out);
  return cloned;
}

static std::vector<bool> remap_source_edge_mask_to_clone(
    std::vector<bool> const& source_edge_keep,
    cloned_phylo_dag const& cloned) {
  std::vector<bool> clone_edge_keep(cloned.dag.edge_high_mark(), false);
  for (std::size_t source_edge = 0; source_edge < source_edge_keep.size();
       ++source_edge) {
    if (!source_edge_keep[source_edge]) continue;
    if (source_edge >= cloned.source_edge_to_clone.size() ||
        cloned.source_edge_to_clone[source_edge] == no_idx) {
      throw std::runtime_error(
          "chart B&B trim apply: kept source witness edge was not cloned");
    }
    auto clone_edge = cloned.source_edge_to_clone[source_edge];
    if (clone_edge >= clone_edge_keep.size()) {
      throw std::runtime_error(
          "chart B&B trim apply: cloned witness edge out of range");
    }
    clone_edge_keep[clone_edge] = true;
  }
  return clone_edge_keep;
}

static std::vector<bool> active_source_nodes(phylo_dag& dag) {
  std::vector<bool> active(dag.node_high_mark(), false);
  for (auto nv : dag.get_all_nodes()) {
    auto node_idx = std::visit([](auto node) { return node.index(); }, nv);
    if (node_idx < active.size()) active[node_idx] = true;
  }
  return active;
}

static std::vector<bool> active_source_edges(phylo_dag& dag) {
  std::vector<bool> active(dag.edge_high_mark(), false);
  for (auto ev : dag.get_all_edges()) {
    auto edge_idx = std::visit([](auto edge) { return edge.index(); }, ev);
    if (edge_idx < active.size()) active[edge_idx] = true;
  }
  return active;
}

static void validate_witness_edge_addressable(
    phylo_dag& dag, clade_grammar const& grammar, production_id pid,
    std::vector<bool> const& active_nodes,
    std::vector<bool> const& active_edges) {
  auto const& prod = grammar.productions[pid];
  if (prod.witnesses.empty()) {
    throw std::runtime_error(
        "chart B&B trim apply: production " + std::to_string(pid) +
        " has no source DAG witnesses");
  }
  for (auto const& witness : prod.witnesses) {
    if (witness.parent_node >= dag.node_high_mark()) {
      throw std::runtime_error(
          "chart B&B trim apply: witness parent node out of range");
    }
    if (witness.parent_node >= active_nodes.size() ||
        !active_nodes[witness.parent_node]) {
      throw std::runtime_error(
          "chart B&B trim apply: witness parent node is not active in the "
          "source DAG");
    }
    if (witness.parent_node >= grammar.node_to_clade.size() ||
        grammar.node_to_clade[witness.parent_node] != prod.parent) {
      throw std::runtime_error(
          "chart B&B trim apply: witness parent node/clade mismatch");
    }
    if (witness.children.size() != prod.children.size()) {
      throw std::runtime_error(
          "chart B&B trim apply: witness arity mismatch");
    }
    for (std::size_t child_i = 0; child_i < witness.children.size();
         ++child_i) {
      auto const& child_witness = witness.children[child_i];
      if (child_witness.child != prod.children[child_i]) {
        throw std::runtime_error(
            "chart B&B trim apply: witness child is not aligned with "
            "production child");
      }
      if (child_witness.edge_alternatives.empty()) {
        throw std::runtime_error(
            "chart B&B trim apply: witness child has no edge alternatives");
      }
      for (auto edge_idx : child_witness.edge_alternatives) {
        if (edge_idx >= dag.edge_high_mark()) {
          throw std::runtime_error(
              "chart B&B trim apply: witness edge out of range");
        }
        if (edge_idx >= active_edges.size() || !active_edges[edge_idx]) {
          throw std::runtime_error(
              "chart B&B trim apply: witness edge is not active in the "
              "source DAG");
        }
        if (get_parent_idx(dag, edge_idx) != witness.parent_node) {
          throw std::runtime_error(
              "chart B&B trim apply: witness edge parent mismatch");
        }
        auto child_node = get_child_idx(dag, edge_idx);
        if (child_node >= grammar.node_to_clade.size() ||
            grammar.node_to_clade[child_node] != child_witness.child) {
          throw std::runtime_error(
              "chart B&B trim apply: witness edge child clade mismatch");
        }
      }
    }
  }
}

static void validate_all_witnesses_edge_addressable(
    phylo_dag& dag, clade_grammar const& grammar) {
  auto active_nodes = active_source_nodes(dag);
  auto active_edges = active_source_edges(dag);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    validate_witness_edge_addressable(dag, grammar,
                                      static_cast<production_id>(pid),
                                      active_nodes, active_edges);
  }
}

static std::vector<bool> represented_source_edges(
    phylo_dag& dag, clade_grammar const& grammar) {
  std::vector<bool> represented(dag.edge_high_mark(), false);
  for (auto const& prod : grammar.productions) {
    for (auto const& witness : prod.witnesses) {
      for (auto const& child : witness.children) {
        for (auto edge_idx : child.edge_alternatives) {
          if (edge_idx >= represented.size()) {
            throw std::runtime_error(
                "chart B&B trim apply: witness edge out of represented-edge "
                "range");
          }
          represented[edge_idx] = true;
        }
      }
    }
  }
  return represented;
}

static void mark_kept_production_witness_edges(
    clade_grammar const& grammar, std::vector<bool> const& keep_production,
    std::vector<bool>& edge_keep) {
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (!keep_production[pid]) continue;
    auto const& prod = grammar.productions[pid];
    for (auto const& witness : prod.witnesses) {
      for (auto const& child : witness.children) {
        for (auto edge_idx : child.edge_alternatives) {
          edge_keep.at(edge_idx) = true;
        }
      }
    }
  }
}

static std::vector<std::size_t> kept_root_parent_nodes(
    clade_grammar const& grammar, std::vector<bool> const& keep_production) {
  std::vector<std::size_t> nodes;
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (!keep_production[pid]) continue;
    auto const& prod = grammar.productions[pid];
    if (prod.parent != grammar.root_clade) continue;
    for (auto const& witness : prod.witnesses) nodes.push_back(witness.parent_node);
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

static void mark_unrepresented_root_paths_to_kept_root_productions(
    phylo_dag& dag, clade_grammar const& grammar,
    std::vector<bool> const& keep_production,
    std::vector<bool> const& represented_edge, std::vector<bool>& edge_keep) {
  if (grammar.productions.empty()) {
    for (auto ev : dag.get_all_edges()) {
      std::visit([&](auto edge) { edge_keep[edge.index()] = true; }, ev);
    }
    return;
  }

  auto kept_roots = kept_root_parent_nodes(grammar, keep_production);
  if (kept_roots.empty()) {
    throw std::runtime_error(
        "chart B&B trim apply: exact mask has no kept root production");
  }

  std::vector<bool> node_on_path(dag.node_high_mark(), false);
  std::vector<std::size_t> stack;
  for (auto node_idx : kept_roots) {
    if (node_idx >= node_on_path.size()) {
      throw std::runtime_error(
          "chart B&B trim apply: kept root witness node out of range");
    }
    if (!node_on_path[node_idx]) {
      node_on_path[node_idx] = true;
      stack.push_back(node_idx);
    }
  }

  while (!stack.empty()) {
    auto node_idx = stack.back();
    stack.pop_back();
    for (auto edge_idx : get_parent_edges(dag, node_idx)) {
      if (edge_idx >= represented_edge.size() || represented_edge[edge_idx]) {
        continue;
      }
      auto parent = get_parent_idx(dag, edge_idx);
      if (parent < node_on_path.size() && !node_on_path[parent]) {
        node_on_path[parent] = true;
        stack.push_back(parent);
      }
    }
  }

  auto root = get_root_idx(dag);
  if (root >= node_on_path.size() || !node_on_path[root]) {
    throw std::runtime_error(
        "chart B&B trim apply: could not connect kept root productions to "
        "the DAG UA/root through unrepresented passthrough edges");
  }

  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto edge_idx = edge.index();
          if (edge_idx >= represented_edge.size() || represented_edge[edge_idx])
            return;
          auto parent = get_parent_idx(dag, edge_idx);
          auto child = get_child_idx(dag, edge_idx);
          if (parent < node_on_path.size() && child < node_on_path.size() &&
              node_on_path[parent] && node_on_path[child]) {
            edge_keep[edge_idx] = true;
          }
        },
        ev);
  }
}

static std::vector<bool> reachable_nodes_using_kept_edges(
    phylo_dag& dag, std::vector<bool> const& edge_keep) {
  std::vector<bool> reachable(dag.node_high_mark(), false);
  auto root = get_root_idx(dag);
  std::queue<std::size_t> q;
  reachable[root] = true;
  q.push(root);
  while (!q.empty()) {
    auto node_idx = q.front();
    q.pop();
    auto nv = dag.get_node(node_idx);
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto edge_idx = edge.index();
                  if (edge_idx >= edge_keep.size() || !edge_keep[edge_idx])
                    return;
                  auto child = get_child_idx(dag, edge_idx);
                  if (child < reachable.size() && !reachable[child]) {
                    reachable[child] = true;
                    q.push(child);
                  }
                },
                ev);
          }
        },
        nv);
  }
  return reachable;
}

static bool close_edge_keep_for_reachable_dag_validity(
    phylo_dag& dag, std::vector<bool>& edge_keep) {
  bool changed = false;
  auto reachable = reachable_nodes_using_kept_edges(dag, edge_keep);
  for (auto nv : dag.get_all_nodes()) {
    auto node_idx = std::visit([](auto node) { return node.index(); }, nv);
    if (node_idx >= reachable.size() || !reachable[node_idx]) continue;

    std::map<std::size_t, std::vector<std::size_t>> groups;
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  groups[edge.clade_index()].push_back(edge.index());
                },
                ev);
          }
        },
        nv);

    for (auto const& [_, edges] : groups) {
      bool group_has_kept_edge = false;
      for (auto edge_idx : edges) {
        if (edge_idx < edge_keep.size() && edge_keep[edge_idx]) {
          group_has_kept_edge = true;
          break;
        }
      }
      if (group_has_kept_edge) continue;
      for (auto edge_idx : edges) {
        if (!edge_keep.at(edge_idx)) {
          edge_keep[edge_idx] = true;
          changed = true;
        }
      }
    }
  }
  return changed;
}

static void compact_clade_indices(phylo_dag& dag) {
  for (auto nv : dag.get_all_nodes()) {
    std::map<std::size_t, std::vector<std::size_t>> groups;
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  groups[edge.clade_index()].push_back(edge.index());
                },
                ev);
          }
        },
        nv);
    std::size_t next = 0;
    for (auto const& [_, edges] : groups) {
      for (auto edge_idx : edges) {
        dag.get_edge_as<edge_kind::clade>(edge_idx).clade_index() = next;
      }
      ++next;
    }
  }
  build_clade_offsets(dag);
}

static void remove_unreachable_nodes(phylo_dag& dag) {
  auto root = get_root_idx(dag);
  std::vector<bool> reachable(dag.node_high_mark(), false);
  std::queue<std::size_t> q;
  reachable[root] = true;
  q.push(root);
  while (!q.empty()) {
    auto node_idx = q.front();
    q.pop();
    auto nv = dag.get_node(node_idx);
    std::visit(
        [&](auto node) {
          for (auto ev : node.get_children()) {
            std::visit(
                [&](auto edge) {
                  auto child = get_child_idx(dag, edge.index());
                  if (child < reachable.size() && !reachable[child]) {
                    reachable[child] = true;
                    q.push(child);
                  }
                },
                ev);
          }
        },
        nv);
  }

  std::vector<std::size_t> orphan_nodes;
  for (auto nv : dag.get_all_nodes()) {
    auto node_idx = std::visit([](auto node) { return node.index(); }, nv);
    if (node_idx != root &&
        (node_idx >= reachable.size() || !reachable[node_idx])) {
      orphan_nodes.push_back(node_idx);
    }
  }
  for (auto node_idx : orphan_nodes) {
    auto nv = dag.get_node(node_idx);
    std::visit([](auto node) { node.remove(); }, nv);
  }
}

static void remove_edges_not_kept(phylo_dag& dag,
                                  std::vector<bool> const& edge_keep) {
  std::vector<std::size_t> remove_edges;
  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto edge_idx = edge.index();
          if (edge_idx >= edge_keep.size() || !edge_keep[edge_idx]) {
            remove_edges.push_back(edge_idx);
          }
        },
        ev);
  }
  for (auto edge_idx : remove_edges) {
    auto ev = dag.get_edge(edge_idx);
    std::visit([](auto edge) { edge.remove(); }, ev);
  }
}

static std::vector<compact_genome> collect_leaf_compact_genomes_for_taxa(
    phylo_dag& source, clade_grammar const& grammar) {
  std::vector<compact_genome> result(grammar.taxa.id_to_sample_id.size());
  std::vector<bool> seen(grammar.taxa.id_to_sample_id.size(), false);
  for (auto nv : source.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            auto it = grammar.taxa.sample_id_to_id.find(
                std::string{node.sample_id()});
            if (it == grammar.taxa.sample_id_to_id.end()) return;
            auto taxon = it->second;
            if (taxon >= result.size()) {
              throw std::runtime_error(
                  "chart B&B trim apply: taxon id out of range while "
                  "collecting leaf compact genomes");
            }
            if (seen[taxon] && !(result[taxon] == node.cg())) {
              throw std::runtime_error(
                  "chart B&B trim apply: conflicting compact genomes for "
                  "duplicate sample_id '" +
                  std::string{node.sample_id()} + "'");
            }
            result[taxon] = node.cg();
            seen[taxon] = true;
          }
        },
        nv);
  }
  for (std::size_t taxon = 0; taxon < seen.size(); ++taxon) {
    if (!seen[taxon]) {
      throw std::runtime_error(
          "chart B&B trim apply: source DAG is missing leaf sample_id '" +
          grammar.taxa.id_to_sample_id[taxon] + "'");
    }
  }
  return result;
}

static std::size_t add_materialized_tree_edge(phylo_dag& tree,
                                              std::size_t parent_idx,
                                              std::size_t child_idx,
                                              std::size_t clade_index,
                                              float edge_weight) {
  auto edge = tree.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_index;
  edge.edge_weight() = edge_weight;
  std::visit([&](auto parent) { edge.set_parent(parent); },
             tree.get_node(parent_idx));
  std::visit([&](auto child) { edge.set_child(child); },
             tree.get_node(child_idx));
  return edge.index();
}

static std::size_t build_materialized_tree_subtree(
    phylo_dag& tree, clade_grammar const& grammar,
    grammar_topology const& topology,
    std::vector<compact_genome> const& leaf_cgs, clade_id clade,
    std::vector<std::uint8_t>& state, float edge_weight) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error(
        "chart B&B trim apply: topology subtree clade out of range");
  }
  if (state[clade] == 1) {
    throw std::runtime_error(
        "chart B&B trim apply: cycle in materialized topology");
  }
  if (state[clade] == 2) {
    throw std::runtime_error(
        "chart B&B trim apply: materialized topology reuses a clade; "
        "expected a concrete tree topology");
  }
  state[clade] = 1;

  std::size_t node_idx = no_idx;
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    auto taxon = key.taxa.front();
    if (taxon >= grammar.taxa.id_to_sample_id.size() ||
        taxon >= leaf_cgs.size()) {
      throw std::runtime_error(
          "chart B&B trim apply: materialized topology leaf taxon out of "
          "range");
    }
    auto leaf = tree.append_node<node_kind::leaf>();
    leaf.sample_id() = grammar.taxa.id_to_sample_id[taxon];
    leaf.cg() = leaf_cgs[taxon];
    node_idx = leaf.index();
  } else {
    auto pid = topology.selected_production_by_clade[clade];
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "chart B&B trim apply: materialized topology is missing a "
          "production for a non-singleton clade");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "chart B&B trim apply: selected production parent mismatch");
    }
    chart_trim_detail::validate_binary_production_for_trim(grammar, prod, pid);
    auto inner = tree.append_node<node_kind::inner>();
    node_idx = inner.index();
    for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
      auto child_idx = build_materialized_tree_subtree(
          tree, grammar, topology, leaf_cgs, prod.children[child_i], state,
          edge_weight);
      add_materialized_tree_edge(tree, node_idx, child_idx, child_i,
                                 edge_weight);
    }
  }

  state[clade] = 2;
  return node_idx;
}

static phylo_dag materialize_grammar_topology_tree_impl(
    phylo_dag& source, clade_grammar const& grammar,
    grammar_topology const& topology, bool validate_tree,
    float edge_weight = 0.0f) {
  (void)validate_grammar_topology(grammar, topology);
  auto leaf_cgs = collect_leaf_compact_genomes_for_taxa(source, grammar);

  phylo_dag tree;
  auto ua = tree.append_node<node_kind::ua>();
  ua.reference_sequence() = get_reference_sequence(source);
  tree.set_root(ua);

  std::vector<std::uint8_t> state(grammar.clades.size(), 0);
  auto root_child = build_materialized_tree_subtree(
      tree, grammar, topology, leaf_cgs, grammar.root_clade, state,
      edge_weight);
  add_materialized_tree_edge(tree, ua.index(), root_child, 0, edge_weight);

  build_clade_offsets(tree);
  fitch_assign_compact_genomes(tree);
  recompute_edge_mutations(tree);
  build_clade_offsets(tree);
  if (validate_tree) {
    if (!is_tree(tree)) {
      throw std::runtime_error(
          "chart B&B trim apply: materialized topology DAG is not a tree");
    }
    validate_dag(tree, "chart B&B materialized grammar topology tree",
                 thread_pool::get_default());
  }
  return tree;
}

static std::size_t append_copied_edge(phylo_dag& out, phylo_dag& tree,
                                      std::size_t tree_edge_idx,
                                      std::size_t parent_idx,
                                      std::size_t child_idx) {
  auto src_edge = tree.get_edge_as<edge_kind::clade>(tree_edge_idx);
  auto edge = out.append_edge<edge_kind::clade>();
  edge.clade_index() = src_edge.clade_index();
  edge.edge_weight() = src_edge.edge_weight();
  edge.mutations() = src_edge.mutations();
  std::visit([&](auto parent) { edge.set_parent(parent); },
             out.get_node(parent_idx));
  std::visit([&](auto child) { edge.set_child(child); },
             out.get_node(child_idx));
  return edge.index();
}

static phylo_dag merge_grammar_topology_trees_identity_preserving_impl(
    phylo_dag& source, clade_grammar const& grammar,
    std::vector<grammar_topology> const& topologies,
    bool validate_generated_trees) {
  if (topologies.empty()) {
    throw std::runtime_error(
        "chart B&B trim apply: no topologies to materialize");
  }

  auto leaf_cgs = collect_leaf_compact_genomes_for_taxa(source, grammar);

  phylo_dag out;
  auto ua = out.append_node<node_kind::ua>();
  ua.reference_sequence() = get_reference_sequence(source);
  out.set_root(ua);
  auto out_root = ua.index();

  std::unordered_map<std::string, std::size_t> leaf_by_sample;
  for (std::size_t taxon = 0; taxon < grammar.taxa.id_to_sample_id.size();
       ++taxon) {
    auto leaf = out.append_node<node_kind::leaf>();
    leaf.sample_id() = grammar.taxa.id_to_sample_id[taxon];
    leaf.cg() = leaf_cgs[taxon];
    leaf_by_sample.emplace(leaf.sample_id(), leaf.index());
  }

  for (auto const& topology : topologies) {
    auto tree = materialize_grammar_topology_tree_impl(
        source, grammar, topology, validate_generated_trees, 0.0f);
    std::vector<std::size_t> node_map(tree.node_high_mark(), no_idx);
    node_map[get_root_idx(tree)] = out_root;

    for (auto nv : tree.get_all_nodes()) {
      std::visit(
          [&](auto node) {
            auto node_idx = node.index();
            if constexpr (requires { node.reference_sequence(); }) {
              node_map[node_idx] = out_root;
            } else if constexpr (requires {
                                   node.sample_id();
                                   node.cg();
                                 }) {
              auto found = leaf_by_sample.find(std::string{node.sample_id()});
              if (found == leaf_by_sample.end()) {
                throw std::runtime_error(
                    "chart B&B trim apply: materialized tree leaf sample_id "
                    "not in source grammar");
              }
              node_map[node_idx] = found->second;
            } else if constexpr (requires { node.cg(); }) {
              auto inner = out.append_node<node_kind::inner>();
              inner.cg() = node.cg();
              node_map[node_idx] = inner.index();
            }
          },
          nv);
    }

    for (auto ev : tree.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            auto parent = get_parent_idx(tree, edge.index());
            auto child = get_child_idx(tree, edge.index());
            if (parent >= node_map.size() || child >= node_map.size() ||
                node_map[parent] == no_idx || node_map[child] == no_idx) {
              throw std::runtime_error(
                  "chart B&B trim apply: incomplete node map while copying "
                  "materialized tree");
            }
            append_copied_edge(out, tree, edge.index(), node_map[parent],
                               node_map[child]);
          },
          ev);
    }
  }

  build_clade_offsets(out);
  if (validate_generated_trees) {
    validate_dag(out, "chart B&B identity-preserving topology materialization",
                 thread_pool::get_default());
  }
  return out;
}

static clade_grammar_build_result rebuild_output_grammar(phylo_dag& dag) {
  clade_grammar_options opts;
  return build_clade_grammar_with_audit(dag, opts);
}

static void validate_same_taxa(clade_grammar const& expected,
                               clade_grammar const& rebuilt) {
  std::vector<std::string> lhs = expected.taxa.id_to_sample_id;
  std::vector<std::string> rhs = rebuilt.taxa.id_to_sample_id;
  std::sort(lhs.begin(), lhs.end());
  std::sort(rhs.begin(), rhs.end());
  if (lhs != rhs) {
    throw std::runtime_error(
        "chart B&B trim apply: rebuilt output grammar has different taxa");
  }
}

static void verify_rebuilt_production_signatures(
    clade_grammar const& requested_grammar, std::vector<bool> const& keep_mask,
    clade_grammar const& rebuilt_grammar,
    chart_bnb_trim_apply_result& result) {
  validate_same_taxa(requested_grammar, rebuilt_grammar);
  auto rebuilt_keys = production_signature_set(rebuilt_grammar);

  for (std::size_t pid = 0; pid < requested_grammar.productions.size(); ++pid) {
    auto key = production_signature(requested_grammar,
                                    static_cast<production_id>(pid));
    bool present = rebuilt_keys.find(key) != rebuilt_keys.end();
    if (keep_mask[pid]) {
      if (!present) {
        throw std::runtime_error(
            "chart B&B trim apply: intended kept production signature is "
            "missing after DAG pruning");
      }
      ++result.kept_productions_rebuilt;
    } else if (present) {
      ++result.masked_productions_reappeared;
    }
  }
}

using topology_taxon_signature = std::vector<production_taxon_signature>;

static topology_taxon_signature topology_signature(
    clade_grammar const& grammar, grammar_topology const& topology) {
  (void)validate_grammar_topology(grammar, topology);
  topology_taxon_signature signature;
  for (std::size_t pid = 0; pid < topology.used_production.size(); ++pid) {
    if (!topology.used_production[pid]) continue;
    signature.push_back(production_signature(grammar,
                                             static_cast<production_id>(pid)));
  }
  std::sort(signature.begin(), signature.end());
  return signature;
}

static std::set<topology_taxon_signature> topology_signature_set(
    clade_grammar const& grammar,
    std::vector<grammar_topology> const& topologies) {
  std::set<topology_taxon_signature> signatures;
  for (auto const& topology : topologies) {
    signatures.insert(topology_signature(grammar, topology));
  }
  return signatures;
}

struct small_fixture_validation_result {
  bool ran = false;
  bool truncated = false;
  std::uint64_t min_score = multisite_score_inf;
  bool all_topologies_optimal = true;
  std::vector<grammar_topology> topologies;
};

static std::vector<grammar_topology> enumerate_topologies_for_small_validation(
    clade_grammar const& grammar, std::size_t cap, bool& truncated) {
  truncated = false;
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error(
        "chart B&B trim apply: rebuilt grammar root clade out of range");
  }
  std::size_t enumeration_cap = cap == std::numeric_limits<std::size_t>::max()
                                    ? std::size_t{0}
                                    : cap + 1;
  try {
    auto topologies = chart_multisite_detail::enumerate_topologies(
        grammar, grammar.root_clade, enumeration_cap);
    chart_multisite_detail::sort_and_unique_topologies(topologies);
    if (topologies.size() > cap) {
      truncated = true;
      return {};
    }
    return topologies;
  } catch (std::runtime_error const&) {
    truncated = true;
    return {};
  }
}

static std::uint64_t fitch_score_materialized_topology(
    phylo_dag& source, clade_grammar const& grammar,
    grammar_topology const& topology, chart_options const& chart_opts) {
  auto tree = materialize_grammar_topology_tree_impl(
      source, grammar, topology, false, 0.0f);
  if (chart_opts.score_ua_edge) {
    parsimony_score_ops ops;
    subtree_weight<parsimony_score_ops> scorer(tree);
    return scorer.compute_weight_below(get_root_idx(tree), ops);
  }
  ua_free_parsimony_score_ops ops;
  subtree_weight<ua_free_parsimony_score_ops> scorer(tree);
  return scorer.compute_weight_below(get_root_idx(tree), ops);
}

static small_fixture_validation_result validate_output_parsimony_with_fitch(
    phylo_dag& dag, clade_grammar const& grammar,
    chart_options const& chart_opts, std::uint64_t expected_optimum,
    bool require_equal_to_expected, std::size_t topology_cap = 10000) {
  small_fixture_validation_result result;
  bool truncated = false;
  auto topologies = enumerate_topologies_for_small_validation(
      grammar, topology_cap, truncated);
  if (truncated) {
    result.truncated = true;
    return result;
  }
  result.ran = true;
  result.topologies = std::move(topologies);
  if (result.topologies.empty()) {
    throw std::runtime_error(
        "chart B&B trim apply: small-fixture validation found no output "
        "topologies");
  }

  for (auto const& topology : result.topologies) {
    auto score = fitch_score_materialized_topology(dag, grammar, topology,
                                                   chart_opts);
    result.min_score = std::min(result.min_score, score);
    if (score != expected_optimum) result.all_topologies_optimal = false;
  }
  if (require_equal_to_expected && result.min_score != expected_optimum) {
    throw std::runtime_error(
        "chart B&B trim apply: independent Fitch output minimum " +
        std::to_string(result.min_score) +
        " does not match B&B optimum " + std::to_string(expected_optimum));
  }
  return result;
}

static void record_validation_success(chart_bnb_trim_apply_result& result) {
  result.validation_succeeded = true;
  result.validation_status = "success";
}

static std::uint64_t validate_output_parsimony_with_rebuilt_bnb(
    phylo_dag& dag, chart_options const& chart_opts,
    std::uint64_t expected_optimum,
    bool require_equal_to_expected) {
  auto rebuilt = rebuild_output_grammar(dag);
  site_pattern_options pattern_opts;
  auto patterns = build_site_patterns(dag, rebuilt.grammar, pattern_opts);
  multisite_trim_options trim_opts;
  trim_opts.dominance_mode = multisite_dominance_mode::off;
  trim_opts.require_exact_keep_mask = false;
  auto trim = build_multisite_trim(rebuilt.grammar, patterns, chart_opts,
                                   trim_opts);
  if (require_equal_to_expected && trim.optimum != expected_optimum) {
    throw std::runtime_error(
        "chart B&B trim apply: output DAG validation optimum " +
        std::to_string(trim.optimum) +
        " does not match B&B optimum " + std::to_string(expected_optimum));
  }
  return trim.optimum;
}

static chart_bnb_trim_apply_result apply_production_mask_superset(
    phylo_dag& source, polytomy_refinement_result const& refinement,
    chart_options const& chart_opts, multisite_trim_result const& trim,
    chart_bnb_trim_apply_options const& options) {
  auto const& grammar = refinement.grammar;
  validate_trim_mask_for_apply(grammar, trim);
  validate_no_synthetic_provenance_for_direct_mask(refinement);

  chart_bnb_trim_apply_result result;
  result.mode = chart_bnb_trim_application_mode::production_mask_superset;
  result.production_mask_superset = true;
  result.refinement_exactness = refinement_exactness_label(refinement.audit);
  result.bnb_optimum = trim.optimum;
  result.kept_productions_requested = count_true(trim.keep_production);
  result.output_contains_only_optimal_topologies = "unknown";

  // Production witnesses and grammar.node_to_clade are indexed in the source
  // DAG's original index space.  Validate and compute the source-edge keep mask
  // before cloning, then explicitly remap kept edges into the dense clone.
  validate_all_witnesses_edge_addressable(source, grammar);
  auto represented = represented_source_edges(source, grammar);
  std::vector<bool> source_edge_keep(source.edge_high_mark(), false);
  mark_kept_production_witness_edges(grammar, trim.keep_production,
                                     source_edge_keep);
  mark_unrepresented_root_paths_to_kept_root_productions(
      source, grammar, trim.keep_production, represented, source_edge_keep);

  while (close_edge_keep_for_reachable_dag_validity(source, source_edge_keep)) {
  }

  auto cloned = clone_phylo_dag(source);
  auto edge_keep = remap_source_edge_mask_to_clone(source_edge_keep, cloned);
  phylo_dag dag = std::move(cloned.dag);
  auto before_edges = edge_count(dag);
  auto before_nodes = node_count(dag);

  remove_edges_not_kept(dag, edge_keep);
  remove_unreachable_nodes(dag);
  compact_clade_indices(dag);

  if (options.validate_output_dag) {
    validate_dag(dag, "chart B&B production-mask output",
                 thread_pool::get_default());
  }

  result.source_edges_removed = before_edges >= edge_count(dag)
                                    ? before_edges - edge_count(dag)
                                    : 0;
  result.source_nodes_removed = before_nodes >= node_count(dag)
                                    ? before_nodes - node_count(dag)
                                    : 0;

  auto rebuilt = rebuild_output_grammar(dag);
  if (options.rebuild_and_verify_grammar) {
    verify_rebuilt_production_signatures(grammar, trim.keep_production,
                                         rebuilt.grammar, result);
  }

  auto small_validation = validate_output_parsimony_with_fitch(
      dag, rebuilt.grammar, chart_opts, trim.optimum, true);
  if (small_validation.ran) {
    result.validated_output_parsimony_min = small_validation.min_score;
    result.validated_output_parsimony_min_exact = true;
    result.validation_oracle = "brute_force_topology_enumeration_fitch";
    result.validation_strength = "exact_small_fixture_independent";
    result.output_contains_only_optimal_topologies =
        small_validation.all_topologies_optimal ? "true" : "false";
  } else {
    result.validated_output_parsimony_min =
        validate_output_parsimony_with_rebuilt_bnb(
            dag, chart_opts, trim.optimum, true);
    result.validated_output_parsimony_min_exact = true;
    result.validation_oracle = "rebuilt_collapsed_grammar_chart_bnb";
    result.validation_strength =
        "exact_for_rebuilt_collapsed_grammar_not_independent";
  }
  record_validation_success(result);

  result.dag = std::move(dag);
  return result;
}

static chart_bnb_trim_apply_result apply_optimal_topology_materialize(
    phylo_dag& source, polytomy_refinement_result const& refinement,
    site_pattern_set const& patterns, chart_options const& chart_opts,
    multisite_trim_result const& trim,
    chart_bnb_trim_apply_options const& options) {
  auto const& grammar = refinement.grammar;
  if (trim.optimum >= multisite_score_inf) {
    throw std::runtime_error(
        "chart B&B trim apply: cannot materialize topologies for infinite "
        "B&B optimum");
  }

  chart_bnb_trim_apply_result result;
  result.mode = chart_bnb_trim_application_mode::optimal_topology_materialize;
  result.refinement_exactness = refinement_exactness_label(refinement.audit);
  result.bnb_optimum = trim.optimum;
  result.kept_productions_requested = count_true(trim.keep_production);
  result.identity_preserving_tree_set = true;
  result.grammar_topology_exact = true;
  result.source_history_topology_exact = false;
  result.topology_exact = false;
  result.production_mask_superset = false;
  result.output_contains_only_optimal_topologies = "true";

  constexpr std::size_t default_topology_cap = 1000;
  auto topology_cap = options.max_exact_topologies_to_materialize.value_or(
      default_topology_cap);

  multisite_topology_trace_options trace_opts;
  trace_opts.keep_provenance = true;
  trace_opts.max_optimal_topologies = topology_cap;
  trace_opts.trim_options.dominance_mode = multisite_dominance_mode::off;
  trace_opts.trim_options.require_exact_keep_mask = true;
  trace_opts.trim_options.known_exact_optimum = trim.optimum;

  auto trace = build_multisite_optimal_topologies(grammar, patterns, chart_opts,
                                                  trace_opts);
  result.topology_cap_truncated = trace.topology_cap_truncated;
  if (trace.topology_cap_truncated) {
    throw std::runtime_error(
        "chart B&B trim apply: optimal topology materialization cap was "
        "truncated; rerun with a larger --chart-bnb-max-exact-topologies "
        "or explicit 0 for unlimited enumeration");
  }
  if (trace.optimum != trim.optimum) {
    throw std::runtime_error(
        "chart B&B trim apply: topology trace optimum does not match trim "
        "optimum");
  }

  for (auto const& topology : trace.topologies) {
    auto score = score_selected_topology(grammar, patterns, topology,
                                         chart_opts);
    if (score != trim.optimum) {
      throw std::runtime_error(
          "chart B&B trim apply: emitted topology failed exact re-score");
    }
  }

  auto dag = merge_grammar_topology_trees_identity_preserving_impl(
      source, grammar, trace.topologies, options.validate_output_dag);
  result.materialized_topologies = trace.topologies.size();
  result.kept_productions_rebuilt = count_true(trace.keep_production);

  auto rebuilt = rebuild_output_grammar(dag);
  if (options.rebuild_and_verify_grammar) {
    validate_same_taxa(grammar, rebuilt.grammar);
    auto rebuilt_keys = production_signature_set(rebuilt.grammar);
    for (std::size_t pid = 0; pid < trace.keep_production.size(); ++pid) {
      if (!trace.keep_production[pid]) continue;
      auto key = production_signature(grammar,
                                      static_cast<production_id>(pid));
      if (rebuilt_keys.find(key) == rebuilt_keys.end()) {
        throw std::runtime_error(
            "chart B&B trim apply: materialized topology production is "
            "missing after output grammar rebuild");
      }
    }
  }

  auto small_validation = validate_output_parsimony_with_fitch(
      dag, rebuilt.grammar, chart_opts, trim.optimum, true);
  if (small_validation.ran) {
    auto expected_topologies = topology_signature_set(grammar, trace.topologies);
    auto observed_topologies = topology_signature_set(rebuilt.grammar,
                                                      small_validation.topologies);
    if (observed_topologies != expected_topologies) {
      throw std::runtime_error(
          "chart B&B trim apply: materialized output topology set differs "
          "from emitted optimal topology set");
    }
    if (!small_validation.all_topologies_optimal) {
      throw std::runtime_error(
          "chart B&B trim apply: materialized output contains a non-optimal "
          "topology under independent Fitch validation");
    }
    result.validated_output_parsimony_min = small_validation.min_score;
    result.validated_output_parsimony_min_exact = true;
    result.validation_oracle =
        "brute_force_topology_enumeration_fitch_and_topology_set_compare";
    result.validation_strength =
        "exact_small_fixture_independent_materialized_grammar_topologies";
  } else {
    result.validated_output_parsimony_min =
        validate_output_parsimony_with_rebuilt_bnb(
            dag, chart_opts, trim.optimum, true);
    result.validated_output_parsimony_min_exact = true;
    result.validation_oracle =
        "identity_preserving_tree_set_materialization_plus_rebuilt_chart_bnb";
    result.validation_strength =
        "exact_materialized_grammar_topologies_not_source_history_variants";
  }
  record_validation_success(result);

  result.dag = std::move(dag);
  return result;
}

}  // namespace chart_bnb_trim_apply_detail

phylo_dag materialize_grammar_topology_tree(
    phylo_dag& source_dag, clade_grammar const& grammar,
    grammar_topology const& topology) {
  return chart_bnb_trim_apply_detail::materialize_grammar_topology_tree_impl(
      source_dag, grammar, topology, true, 0.0f);
}

phylo_dag merge_grammar_topology_trees_identity_preserving(
    phylo_dag& source_dag, clade_grammar const& grammar,
    std::vector<grammar_topology> const& topologies) {
  return chart_bnb_trim_apply_detail::
      merge_grammar_topology_trees_identity_preserving_impl(source_dag, grammar,
                                                            topologies, true);
}

chart_bnb_trim_apply_result apply_chart_bnb_trim(
    phylo_dag& source_dag, polytomy_refinement_result const& refinement,
    site_pattern_set const& patterns, chart_options const& chart_options,
    multisite_trim_result const& trim,
    chart_bnb_trim_apply_options const& options) {
  switch (options.mode) {
    case chart_bnb_trim_application_mode::production_mask_superset:
      return chart_bnb_trim_apply_detail::apply_production_mask_superset(
          source_dag, refinement, chart_options, trim, options);
    case chart_bnb_trim_application_mode::optimal_topology_materialize:
      return chart_bnb_trim_apply_detail::apply_optimal_topology_materialize(
          source_dag, refinement, patterns, chart_options, trim, options);
  }
  throw std::runtime_error("chart B&B trim apply: unknown application mode");
}

}  // namespace larch

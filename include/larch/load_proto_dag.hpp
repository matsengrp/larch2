#pragma once

#include <larch/phylo_dag.hpp>
#include <larch/compute.hpp>
#include <larch/protobuf.hpp>
#include <larch/thread_pool.hpp>

#include <algorithm>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace larch {

// --- Proto message structs matching dag.proto ---

struct dag_mut {
  int32_t position;
  int32_t par_nuc;
  std::vector<int32_t> mut_nuc;
  std::string chromosome;
};

struct dag_edge {
  int64_t edge_id;
  int64_t parent_node;
  int64_t parent_clade;
  int64_t child_node;
  std::vector<dag_mut> edge_mutations;
  float edge_weight;
};

struct iupac_site {
  int32_t position;
  int32_t state_set;
};

struct dag_node_name {
  int64_t node_id;
  std::vector<std::string> condensed_leaves;
  std::vector<int32_t> ambiguous_sites;
  std::vector<iupac_site> iupac_sites;
};

struct dag_data {
  std::vector<dag_edge> edges;
  std::vector<dag_node_name> node_names;
  std::string reference_id;
  std::string reference_seq;
};

// Non-edge fields only, for decoding metadata while edges are decoded in
// parallel.
struct dag_data_meta {
  std::vector<dag_node_name> node_names;
  std::string reference_id;
  std::string reference_seq;
};

}  // namespace larch

// dag_mut has non-sequential field numbers (1,2,3,5 — gap at 4)
namespace pb {
template <>
inline constexpr auto field_numbers<larch::dag_mut> =
    std::array<uint32_t, 4>{1, 2, 3, 5};

// dag_data_meta fields correspond to dag_data fields 2, 3, 4 (skipping edges at
// field 1)
template <>
inline constexpr auto field_numbers<larch::dag_data_meta> =
    std::array<uint32_t, 3>{2, 3, 4};
}  // namespace pb

namespace larch {

inline phylo_dag load_proto_dag(std::string_view path) {
  // Get raw data via mmap (plain .pb) or read_file (.pb.gz)
  std::optional<mmap_file> mf;
  std::vector<char> file_bytes;
  std::span<const uint8_t> data;
  if (!is_gzipped(path)) {
    mf.emplace(path);
    data = mf->span();
  } else {
    file_bytes = read_file(path);
    data = {reinterpret_cast<const uint8_t*>(file_bytes.data()),
            file_bytes.size()};
  }

  // Parallel decode edges (field 1 = repeated dag_edge)
  auto edge_spans = pb::collect_field_spans(data, 1);
  std::vector<dag_edge> edges(edge_spans.size());
  {
    std::vector<std::size_t> indices(edge_spans.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    parallel_for_each(indices, [&](std::size_t i) {
      edges[i] = pb::decode<dag_edge>(edge_spans[i]);
    });
  }

  // Decode remaining fields sequentially (node_names, reference_id,
  // reference_seq)
  auto meta = pb::decode<dag_data_meta>(data);

  phylo_dag d;

  // Create UA node with reference sequence
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::move(meta.reference_seq);
  d.set_root(ua);

  // Collect sample IDs from node_names
  scoped_arena<32768> arena;
  auto* mr = arena.get();
  std::pmr::unordered_map<std::size_t, std::string> node_sample_ids(mr);
  std::pmr::unordered_map<std::size_t, std::map<mutation_position, uint8_t>>
      node_ambiguity_sets(mr);
  for (auto& nn : meta.node_names) {
    auto node_id = static_cast<std::size_t>(nn.node_id);
    if (!nn.condensed_leaves.empty()) {
      node_sample_ids[node_id] = nn.condensed_leaves[0];
    }
    if (!nn.ambiguous_sites.empty()) {
      auto& sets = node_ambiguity_sets[node_id];
      for (auto pos : nn.ambiguous_sites)
        sets[static_cast<mutation_position>(pos)] = 0b1111;
    }
    for (auto const& site : nn.iupac_sites) {
      auto state_set = static_cast<uint8_t>(site.state_set) & 0b1111;
      if (state_set != 0) {
        node_ambiguity_sets[node_id][static_cast<mutation_position>(
            site.position)] = state_set;
      }
    }
  }

  // Collect all unique node IDs from edges
  std::pmr::unordered_map<std::size_t, bool> all_node_ids(mr);
  std::pmr::unordered_map<std::size_t, bool> is_child(mr);
  std::pmr::unordered_map<std::size_t, bool> is_parent(mr);
  for (auto& e : edges) {
    auto p = static_cast<std::size_t>(e.parent_node);
    auto c = static_cast<std::size_t>(e.child_node);
    all_node_ids[p] = true;
    all_node_ids[c] = true;
    is_child[c] = true;
    is_parent[p] = true;
  }

  // Find proto root (parent but not child) -- this IS the UA
  std::size_t proto_root_id = 0;
  for (auto [id, _] : all_node_ids) {
    if (!is_child.contains(id)) {
      proto_root_id = id;
      break;
    }
  }

  // Create DAG nodes. Proto root maps to our UA node.
  std::pmr::unordered_map<std::size_t, std::size_t> proto_to_dag(mr);
  proto_to_dag[proto_root_id] = ua.index();

  for (auto [proto_id, _] : all_node_ids) {
    if (proto_id == proto_root_id) continue;  // Already mapped to UA

    bool has_sample = node_sample_ids.contains(proto_id);
    bool is_leaf_node = !is_parent.contains(proto_id);

    if (is_leaf_node || has_sample) {
      auto leaf = d.append_node<node_kind::leaf>();
      if (has_sample) leaf.sample_id() = node_sample_ids[proto_id];
      proto_to_dag[proto_id] = leaf.index();
    } else {
      auto inner = d.append_node<node_kind::inner>();
      proto_to_dag[proto_id] = inner.index();
    }
  }

  // Sort edges by parent_node so that each parent's children list grows
  // contiguously at the high-water mark of the neighbors chain, avoiding
  // O(k^2) reallocation per high-degree node.
  std::sort(edges.begin(), edges.end(),
            [](auto& a, auto& b) { return a.parent_node < b.parent_node; });

  // Phase 1: Create edges, set mutations, and link parents (builds children
  // lists contiguously since edges are sorted by parent).
  struct pending_child {
    std::size_t edge_idx;
    std::size_t child_dag_idx;
  };
  std::pmr::vector<pending_child> pending(mr);
  pending.reserve(edges.size());

  for (auto& re : edges) {
    auto parent_idx = proto_to_dag[static_cast<std::size_t>(re.parent_node)];
    auto child_idx = proto_to_dag[static_cast<std::size_t>(re.child_node)];

    edge_mutations muts;
    for (auto& m : re.edge_mutations) {
      muts[static_cast<mutation_position>(m.position)] = {
          nuc_base::from_proto(m.par_nuc),
          nuc_base::from_proto(m.mut_nuc.empty() ? 0 : m.mut_nuc[0])};
    }

    auto edge = d.append_edge<edge_kind::clade>();
    edge.mutations() = std::move(muts);
    edge.clade_index() = static_cast<std::size_t>(re.parent_clade);

    std::visit([&](auto p) { edge.set_parent(p); }, d.get_node(parent_idx));
    pending.push_back({edge.index(), child_idx});
  }

  // Phase 2: Link children, sorted by child node so that each child's
  // parents list also grows contiguously.
  std::sort(pending.begin(), pending.end(),
            [](auto& a, auto& b) { return a.child_dag_idx < b.child_dag_idx; });

  for (auto& pc : pending) {
    auto ev = d.get_edge(pc.edge_idx);
    std::visit(
        [&](auto edge) {
          std::visit([&](auto c) { edge.set_child(c); },
                     d.get_node(pc.child_dag_idx));
        },
        ev);
  }

  recompute_compact_genomes(d);

  for (auto [proto_id, ambiguity_sets] : node_ambiguity_sets) {
    auto it = proto_to_dag.find(proto_id);
    if (it == proto_to_dag.end()) continue;
    auto dag_idx = it->second;
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.cg(); }) {
            node.cg().set_ambiguity_sets(std::move(ambiguity_sets));
          }
        },
        d.get_node(dag_idx));
  }

  // Set sample IDs for leaves without one.
  // Use CG string as base, but disambiguate duplicates so that
  // distinct leaf nodes with the same CG remain distinguishable
  // during merge (which uses sample_id for leaf identity).
  {
    std::unordered_map<std::string, std::size_t> cg_str_count;
    for (auto nv : d.get_all_nodes()) {
      std::visit(
          [&](auto n) {
            if constexpr (requires {
                            n.sample_id();
                            n.cg();
                          }) {
              if (n.sample_id().empty()) {
                auto cg_str = n.cg().to_string();
                auto count = cg_str_count[cg_str]++;
                if (count > 0) {
                  n.sample_id() = cg_str + "#" + std::to_string(count);
                } else {
                  n.sample_id() = cg_str;
                }
              }
            }
          },
          nv);
    }
  }

  build_clade_offsets(d);
  return d;
}

}  // namespace larch

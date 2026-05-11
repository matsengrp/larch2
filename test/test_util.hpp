#pragma once

#include <larch/compact_genome.hpp>
#include <larch/compute.hpp>
#include <larch/phylo_dag.hpp>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <unistd.h>

namespace larch::test {

inline std::filesystem::path source_path(std::string_view relative) {
  std::filesystem::path rel{std::string{relative}};
  if (std::filesystem::exists(rel)) return rel;

  if (auto* source_dir = std::getenv("LARCH_SOURCE_DIR")) {
    auto candidate = std::filesystem::path{source_dir} / rel;
    if (std::filesystem::exists(candidate)) return candidate;
  }

  auto dir = std::filesystem::current_path();
  while (!dir.empty()) {
    auto candidate = dir / rel;
    if (std::filesystem::exists(candidate)) return candidate;
    auto parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }

  return rel;
}

inline std::string source_path_string(std::string_view relative) {
  return source_path(relative).string();
}

inline std::filesystem::path unique_temp_path(std::string_view test_name,
                                              std::string_view extension) {
  return std::filesystem::temp_directory_path() /
         (std::string{"larch_"} + std::string{test_name} + "_" +
          std::to_string(static_cast<long long>(getpid())) +
          std::string{extension});
}

inline compact_genome cg_from_sequence(std::string_view seq,
                                       std::string_view reference) {
  assert(seq.size() == reference.size());
  std::map<mutation_position, nuc_base> mutations;
  for (std::size_t i = 0; i < seq.size(); ++i) {
    if (seq[i] != reference[i]) {
      mutations[i + 1] = nuc_base::from_char(seq[i]);
    }
  }
  return compact_genome{std::move(mutations)};
}

inline std::string sequence_from_cg(compact_genome const& cg,
                                    std::string_view reference) {
  std::string seq{reference};
  for (std::size_t pos = 1; pos <= reference.size(); ++pos)
    seq[pos - 1] = cg.get_base(pos, reference).to_char();
  return seq;
}

inline std::string node_sequence(phylo_dag& dag, std::size_t node_idx) {
  auto const& reference = get_reference_sequence(dag);
  return std::visit(
      [&](auto node) -> std::string {
        if constexpr (requires { node.reference_sequence(); }) {
          return node.reference_sequence();
        } else if constexpr (requires { node.cg(); }) {
          return sequence_from_cg(node.cg(), reference);
        } else {
          assert(false && "node has no sequence annotation");
          std::unreachable();
        }
      },
      dag.get_node(node_idx));
}

inline std::size_t add_clade_edge(phylo_dag& dag, std::size_t parent_idx,
                                  std::size_t child_idx,
                                  std::size_t clade_idx) {
  auto edge = dag.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  std::visit([&](auto parent) { edge.set_parent(parent); },
             dag.get_node(parent_idx));
  std::visit([&](auto child) { edge.set_child(child); },
             dag.get_node(child_idx));
  return edge.index();
}

// Shared 4-leaf 20-base tree used by the ML/SPR tests.
inline phylo_dag make_test_tree() {
  constexpr std::string_view ref = "ACGTACGTACGTACGTACGT";
  phylo_dag dag;

  auto ua = dag.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  dag.set_root(ua);

  auto root = dag.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("ACGTACGTACGTACGTACGT", ref);

  auto i1 = dag.append_node<node_kind::inner>();
  i1.cg() = cg_from_sequence("TCGTACGTACGTACGTACGT", ref);

  auto i2 = dag.append_node<node_kind::inner>();
  i2.cg() = cg_from_sequence("ACGTACGTACGCACGTACGT", ref);

  auto l1 = dag.append_node<node_kind::leaf>();
  l1.cg() = cg_from_sequence("TCGTACGTACGTACGTACGT", ref);
  l1.sample_id() = "L1";

  auto l2 = dag.append_node<node_kind::leaf>();
  l2.cg() = cg_from_sequence("TCGCACGTACGTACGTACGT", ref);
  l2.sample_id() = "L2";

  auto l3 = dag.append_node<node_kind::leaf>();
  l3.cg() = cg_from_sequence("ACGTACGTACGCACGTACGT", ref);
  l3.sample_id() = "L3";

  auto l4 = dag.append_node<node_kind::leaf>();
  l4.cg() = cg_from_sequence("ACGTACGTACGCACGCACGT", ref);
  l4.sample_id() = "L4";

  add_clade_edge(dag, ua.index(), root.index(), 0);
  add_clade_edge(dag, root.index(), i1.index(), 0);
  add_clade_edge(dag, root.index(), i2.index(), 1);
  add_clade_edge(dag, i1.index(), l1.index(), 0);
  add_clade_edge(dag, i1.index(), l2.index(), 1);
  add_clade_edge(dag, i2.index(), l3.index(), 0);
  add_clade_edge(dag, i2.index(), l4.index(), 1);

  recompute_edge_mutations(dag);
  return dag;
}

inline std::size_t find_inner_node_by_sequence(phylo_dag& dag,
                                               std::string_view sequence) {
  std::size_t found_idx = 0;
  std::size_t found_count = 0;
  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr ((requires { node.cg(); }) &&
                        !(requires { node.sample_id(); })) {
            if (node_sequence(dag, node.index()) == sequence) {
              found_idx = node.index();
              ++found_count;
            }
          }
        },
        nv);
  }
  assert(found_count == 1);
  return found_idx;
}

inline std::size_t find_leaf_by_sample_id(phylo_dag& dag,
                                          std::string_view sample_id) {
  std::size_t found_idx = 0;
  std::size_t found_count = 0;
  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            if (node.sample_id() == sample_id) {
              found_idx = node.index();
              ++found_count;
            }
          }
        },
        nv);
  }
  assert(found_count == 1);
  return found_idx;
}

}  // namespace larch::test

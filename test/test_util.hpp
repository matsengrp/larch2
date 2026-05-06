#pragma once

#include <larch/compact_genome.hpp>
#include <larch/compute.hpp>
#include <larch/phylo_dag.hpp>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
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
          return {};
        }
      },
      dag.get_node(node_idx));
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

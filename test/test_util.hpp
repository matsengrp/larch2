#pragma once

#include <larch/compact_genome.hpp>
#include <larch/compute.hpp>
#include <larch/merge.hpp>
#include <larch/phylo_dag.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

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

struct tiny_tree_node {
  std::string name;
  std::string sequence;
  std::vector<tiny_tree_node> children;
};

inline tiny_tree_node tiny_leaf(std::string sample_id, std::string sequence) {
  return tiny_tree_node{std::move(sample_id), std::move(sequence), {}};
}

inline tiny_tree_node tiny_inner(std::string name, std::string sequence,
                                 std::vector<tiny_tree_node> children) {
  return tiny_tree_node{std::move(name), std::move(sequence),
                        std::move(children)};
}

struct tiny_dag_node {
  std::string name;
  std::string sequence;
  std::string sample_id;  // non-empty => leaf; empty => inner
};

struct tiny_dag_edge {
  std::string parent;
  std::string child;
  std::size_t clade_index = 0;
};

inline std::string tiny_sequence_or_reference(std::string const& sequence,
                                              std::string_view reference) {
  return sequence.empty() ? std::string{reference} : sequence;
}

inline std::size_t append_tiny_tree_subtree(phylo_dag& dag,
                                            tiny_tree_node const& spec,
                                            std::string_view reference) {
  auto sequence = tiny_sequence_or_reference(spec.sequence, reference);
  if (spec.children.empty()) {
    auto leaf = dag.append_node<node_kind::leaf>();
    leaf.cg() = cg_from_sequence(sequence, reference);
    leaf.sample_id() = spec.name;
    return leaf.index();
  }

  auto inner = dag.append_node<node_kind::inner>();
  inner.cg() = cg_from_sequence(sequence, reference);
  auto parent_idx = inner.index();
  for (std::size_t ci = 0; ci < spec.children.size(); ++ci) {
    auto child_idx = append_tiny_tree_subtree(dag, spec.children[ci], reference);
    add_clade_edge(dag, parent_idx, child_idx, ci);
  }
  return parent_idx;
}

inline phylo_dag make_tiny_labelled_tree(std::string_view reference,
                                         tiny_tree_node const& root_spec) {
  phylo_dag dag;
  auto ua = dag.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{reference};
  dag.set_root(ua);

  auto root_idx = append_tiny_tree_subtree(dag, root_spec, reference);
  add_clade_edge(dag, ua.index(), root_idx, 0);

  recompute_edge_mutations(dag);
  build_clade_offsets(dag);
  return dag;
}

inline phylo_dag make_tiny_labelled_dag(
    std::string_view reference, std::string_view root_name,
    std::vector<tiny_dag_node> const& nodes,
    std::vector<tiny_dag_edge> const& edges) {
  phylo_dag dag;
  auto ua = dag.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{reference};
  dag.set_root(ua);

  std::unordered_map<std::string, std::size_t> node_indices;
  node_indices.reserve(nodes.size());
  for (auto const& spec : nodes) {
    if (spec.name.empty()) throw std::runtime_error("empty tiny DAG node name");
    if (node_indices.contains(spec.name))
      throw std::runtime_error("duplicate tiny DAG node name: " + spec.name);

    auto sequence = tiny_sequence_or_reference(spec.sequence, reference);
    if (spec.sample_id.empty()) {
      auto inner = dag.append_node<node_kind::inner>();
      inner.cg() = cg_from_sequence(sequence, reference);
      node_indices.emplace(spec.name, inner.index());
    } else {
      auto leaf = dag.append_node<node_kind::leaf>();
      leaf.cg() = cg_from_sequence(sequence, reference);
      leaf.sample_id() = spec.sample_id;
      node_indices.emplace(spec.name, leaf.index());
    }
  }

  auto root_it = node_indices.find(std::string{root_name});
  if (root_it == node_indices.end())
    throw std::runtime_error("unknown tiny DAG root node: " +
                             std::string{root_name});
  add_clade_edge(dag, ua.index(), root_it->second, 0);

  for (auto const& edge : edges) {
    auto parent_it = node_indices.find(edge.parent);
    auto child_it = node_indices.find(edge.child);
    if (parent_it == node_indices.end())
      throw std::runtime_error("unknown tiny DAG edge parent: " + edge.parent);
    if (child_it == node_indices.end())
      throw std::runtime_error("unknown tiny DAG edge child: " + edge.child);
    add_clade_edge(dag, parent_it->second, child_it->second,
                   edge.clade_index);
  }

  recompute_edge_mutations(dag);
  build_clade_offsets(dag);
  return dag;
}

inline phylo_dag merge_tiny_trees(std::vector<phylo_dag>& trees) {
  if (trees.empty()) throw std::runtime_error("merge_tiny_trees: no trees");

  auto const reference = get_reference_sequence(trees.front());
  merge merger{reference};
  for (auto& tree : trees) {
    if (get_reference_sequence(tree) != reference)
      throw std::runtime_error("merge_tiny_trees: reference mismatch");
    build_clade_offsets(tree);
    merger.add_dag(tree);
  }

  phylo_dag result = std::move(merger.get_result());
  build_clade_offsets(result);
  return result;
}

inline phylo_dag merge_tiny_trees(std::vector<phylo_dag>&& trees) {
  return merge_tiny_trees(trees);
}

using represented_tree_edge_set = std::vector<std::size_t>;

inline std::vector<represented_tree_edge_set> enumerate_represented_tree_edges(
    phylo_dag& dag, std::size_t node_idx, std::size_t max_trees,
    std::vector<std::optional<std::vector<represented_tree_edge_set>>>& memo) {
  if (node_idx < memo.size() && memo[node_idx].has_value())
    return *memo[node_idx];

  std::vector<represented_tree_edge_set> result;
  if (is_leaf(dag, node_idx)) {
    result.push_back({});
  } else {
    result.push_back({});
    auto clades = get_clades(dag, node_idx);
    for (auto const& clade_edges : clades) {
      if (clade_edges.empty())
        throw std::runtime_error("empty clade while enumerating tiny DAG");

      std::vector<represented_tree_edge_set> clade_choices;
      for (auto edge_idx : clade_edges) {
        auto child_idx = get_child_idx(dag, edge_idx);
        auto child_choices = enumerate_represented_tree_edges(
            dag, child_idx, max_trees, memo);
        for (auto child_edges : child_choices) {
          represented_tree_edge_set choice;
          choice.reserve(child_edges.size() + 1);
          choice.push_back(edge_idx);
          choice.insert(choice.end(), child_edges.begin(), child_edges.end());
          clade_choices.push_back(std::move(choice));
          if (clade_choices.size() > max_trees)
            throw std::runtime_error("tiny DAG tree enumeration exceeded cap");
        }
      }

      std::vector<represented_tree_edge_set> next;
      for (auto const& prefix : result) {
        for (auto const& suffix : clade_choices) {
          represented_tree_edge_set combined;
          combined.reserve(prefix.size() + suffix.size());
          combined.insert(combined.end(), prefix.begin(), prefix.end());
          combined.insert(combined.end(), suffix.begin(), suffix.end());
          next.push_back(std::move(combined));
          if (next.size() > max_trees)
            throw std::runtime_error("tiny DAG tree enumeration exceeded cap");
        }
      }
      result = std::move(next);
    }
  }

  if (node_idx >= memo.size()) memo.resize(node_idx + 1);
  memo[node_idx] = result;
  return result;
}

inline std::vector<represented_tree_edge_set> enumerate_represented_tree_edges(
    phylo_dag& dag, std::size_t max_trees = 10000) {
  build_clade_offsets(dag);
  std::vector<std::optional<std::vector<represented_tree_edge_set>>> memo(
      dag.node_high_mark());
  return enumerate_represented_tree_edges(dag, get_root_idx(dag), max_trees,
                                          memo);
}

inline phylo_dag sample_represented_tree(phylo_dag& dag,
                                         std::uint32_t seed = 1) {
  tree_count_ops ops;
  subtree_weight<tree_count_ops> sampler(dag, seed);
  return sampler.uniform_sample_tree(ops);
}

inline std::vector<std::size_t> sample_represented_tree_edges(
    phylo_dag& dag, std::uint32_t seed = 1) {
  tree_count_ops ops;
  subtree_weight<tree_count_ops> sampler(dag, seed);
  return sampler.sample_tree_edges(ops);
}

template <typename Ops>
inline typename Ops::weight_type score_tree_with_ops(phylo_dag& tree,
                                                     Ops const& ops) {
  build_clade_offsets(tree);
  subtree_weight<Ops> scorer(tree);
  return scorer.compute_weight_below(get_root_idx(tree), ops);
}

inline std::size_t score_tree_parsimony(phylo_dag& tree,
                                        bool score_ua_edge = true) {
  if (score_ua_edge) {
    parsimony_score_ops ops;
    return score_tree_with_ops(tree, ops);
  }
  ua_free_parsimony_score_ops ops;
  return score_tree_with_ops(tree, ops);
}

inline std::size_t score_tree_fitch_parsimony(phylo_dag& tree,
                                              bool score_ua_edge = false) {
  build_clade_offsets(tree);
  fitch_assign_compact_genomes(tree);
  recompute_edge_mutations(tree);
  build_clade_offsets(tree);
  return score_tree_parsimony(tree, score_ua_edge);
}

inline std::size_t score_edge_set_parsimony(
    phylo_dag& dag, std::vector<std::size_t> const& edge_indices,
    bool score_ua_edge = true) {
  std::size_t score = 0;
  parsimony_score_ops ops;
  for (auto edge_idx : edge_indices) {
    if (!score_ua_edge && is_ua(dag, get_parent_idx(dag, edge_idx))) continue;
    score += ops.compute_edge(dag, edge_idx);
  }
  return score;
}

inline std::uint8_t strict_decode_acgt(char c) {
  switch (c) {
    case 'A':
    case 'a':
      return nuc_base::A;
    case 'C':
    case 'c':
      return nuc_base::C;
    case 'G':
    case 'g':
      return nuc_base::G;
    case 'T':
    case 't':
      return nuc_base::T;
    default:
      throw std::runtime_error(std::string{"non-ACGT nucleotide: "} + c);
  }
}

inline std::uint8_t strict_decode_acgt(nuc_base base) {
  if (base.raw() > nuc_base::T)
    throw std::runtime_error("invalid nuc_base raw value: " +
                             std::to_string(base.raw()));
  return base.raw();
}

struct strict_nucleotide_audit_result {
  std::size_t reference_bases = 0;
  std::size_t leaf_count = 0;
  std::size_t leaf_bases = 0;
  std::size_t leaf_mutation_records = 0;
};

inline strict_nucleotide_audit_result audit_strict_nucleotides(
    phylo_dag& dag) {
  strict_nucleotide_audit_result result;
  auto const& reference = get_reference_sequence(dag);

  for (char c : reference) {
    (void)strict_decode_acgt(c);
    ++result.reference_bases;
  }

  for (auto nv : dag.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            ++result.leaf_count;
            for (auto const& [pos, base] : node.cg()) {
              if (pos == 0 || pos > reference.size())
                throw std::runtime_error("compact-genome mutation position " +
                                         std::to_string(pos) +
                                         " outside reference length " +
                                         std::to_string(reference.size()));
              (void)strict_decode_acgt(base);
              ++result.leaf_mutation_records;
            }
            for (std::size_t pos = 1; pos <= reference.size(); ++pos) {
              (void)strict_decode_acgt(node.cg().get_base(pos, reference));
              ++result.leaf_bases;
            }
          }
        },
        nv);
  }

  return result;
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

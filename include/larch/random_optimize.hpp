#pragma once

#include <larch/compute.hpp>
#include <larch/merge.hpp>
#include <larch/sample_method.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace larch {

// Move strategy concept: given a sampled tree and an RNG, produce zero or more
// alternative trees to merge back into the DAG.  Each returned tree must have
// compact genomes and sample_ids set on leaf nodes.
template <typename F>
concept MoveStrategy = requires(F& f, phylo_dag& tree, std::mt19937& rng) {
  { f(tree, rng) } -> std::convertible_to<std::vector<phylo_dag>>;
};

struct radius_result {
  std::size_t radius;
  std::size_t moves_found;
  std::size_t moves_applied;
  std::size_t parsimony_score;  // score of re-sampled tree (0 if last radius)
};

struct optimize_result {
  std::size_t iteration;
  std::size_t dag_node_count;
  std::size_t dag_edge_count;
  std::size_t trees_merged;
  std::size_t parsimony_score;
  std::vector<radius_result> radii;
  larch::objective_kind objective_kind = larch::objective_kind::parsimony;
  double objective_score = 0.0;
};

// Iterative DAG optimiser.
//
// Each iteration:
//   1. Build / refresh the merged result DAG
//   2. Sample a min-weight (parsimony) tree
//   3. Hand the tree to the move strategy, which returns alternative trees
//   4. Merge the alternatives and the sampled tree back into the DAG
//
// Without a move strategy that produces genuinely new topologies (e.g. SPR),
// the loop is idempotent — re-merging already-known trees is a no-op thanks
// to hash-chain deduplication in `merge`.
template <MoveStrategy Strategy>
std::vector<optimize_result> optimize_dag(
    merge& m, Strategy& strategy, std::size_t num_iterations,
    std::optional<std::uint32_t> seed = std::nullopt) {
  std::mt19937 rng(seed.value_or(std::random_device{}()));
  std::vector<optimize_result> results;
  results.reserve(num_iterations);

  for (std::size_t iter = 0; iter < num_iterations; ++iter) {
    // 1. Materialise the current merged DAG (build_result computes edge
    //    mutations from compact genomes).
    auto& dag = m.get_result();

    // 2. Compute min parsimony score & sample a min-weight tree.
    parsimony_score_ops pops;
    std::uint32_t iter_seed = rng();
    subtree_weight<parsimony_score_ops> sw(dag, iter_seed);

    auto root_idx = get_root_idx(dag);
    auto min_score = sw.compute_weight_below(root_idx, pops);

    auto sampled = sw.min_weight_sample_tree(pops);
    fitch_assign_compact_genomes(sampled);
    recompute_edge_mutations(sampled);
    set_sample_ids_from_cg(sampled);

    // 3. Ask the strategy for alternative trees.
    auto new_trees = strategy(sampled, rng);

    // 4. Merge alternatives, then the sampled tree itself.
    for (auto& t : new_trees) m.add_dag(t);
    m.add_dag(sampled);

    results.push_back(optimize_result{
        .iteration = iter,
        .dag_node_count = m.result_node_count(),
        .dag_edge_count = m.result_edge_count(),
        .trees_merged = new_trees.size(),
        .parsimony_score = min_score,
        .objective_kind = larch::objective_kind::parsimony,
        .objective_score = static_cast<double>(min_score),
    });
  }

  return results;
}

}  // namespace larch

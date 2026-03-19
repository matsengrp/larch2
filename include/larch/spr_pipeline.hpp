#pragma once

#include <larch/overlay_spr.hpp>
#include <larch/merge.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/spr_move.hpp>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

namespace larch {

// A MoveProducer takes a sampled tree, a callback, and an RNG.
// It finds moves (using whatever algorithm) and calls the callback for each.
template <typename F>
concept MoveProducer =
    requires(F& f, phylo_dag& tree, move_callback cb, std::mt19937& rng) {
      { f(tree, cb, rng) };
    };

// Iterative DAG optimiser using fragment-based SPR pipeline.
//
// Each iteration:
//   1. Build / refresh the merged result DAG
//   2. Sample a min-weight (parsimony) tree
//   3. Create a callback that extracts fragments and merges them
//   4. Call each producer with the sampled tree and callback
//   5. Merge the sampled tree itself
//
// Supports any number of heterogeneous MoveProducers.
template <MoveProducer... Producers>
std::vector<optimize_result> optimize_dag_v2(merge& m,
                                             std::size_t num_iterations,
                                             std::optional<std::uint32_t> seed,
                                             Producers&... producers) {
  std::mt19937 rng(seed.value_or(std::random_device{}()));
  std::vector<optimize_result> results;
  results.reserve(num_iterations);

  for (std::size_t iter = 0; iter < num_iterations; ++iter) {
    // 1. Materialise the current merged DAG
    auto& dag = m.get_result();

    // 2. Compute min parsimony score & sample a min-weight tree
    parsimony_score_ops pops;
    std::uint32_t iter_seed = rng();
    scoped_arena<4096> sw_arena;
    subtree_weight<parsimony_score_ops> sw(dag, iter_seed, sw_arena.get());

    auto root_idx = get_root_idx(dag);
    auto min_score = sw.compute_weight_below(root_idx, pops);

    auto sampled = sw.min_weight_sample_tree(pops);
    fitch_assign_compact_genomes(sampled);
    recompute_edge_mutations(sampled);
    set_sample_ids_from_cg(sampled);

    // 3. Collect moves from each producer
    std::vector<spr_move> collected_moves;
    move_callback collector = [&](spr_move const& move) {
      collected_moves.push_back(move);
    };
    (producers(sampled, collector, rng), ...);

    // 4. Generate fragments in parallel
    std::vector<phylo_dag> fragments(collected_moves.size());
    {
      std::vector<std::size_t> indices(collected_moves.size());
      std::iota(indices.begin(), indices.end(), std::size_t{0});
      parallel_for_each(indices, [&](std::size_t i) {
        fragments[i] = apply_spr_as_fragment(sampled, collected_moves[i]);
      });
    }

    // 5. Submit fragments to merge
    for (auto& frag : fragments) m.add_dag(std::move(frag));
    std::size_t trees_merged = collected_moves.size();

    // 6. Merge the sampled tree itself
    m.add_dag(sampled);

    results.push_back(optimize_result{
        .iteration = iter,
        .dag_node_count = m.result_node_count(),
        .dag_edge_count = m.result_edge_count(),
        .trees_merged = trees_merged,
        .parsimony_score = min_score,
    });
  }

  return results;
}

}  // namespace larch

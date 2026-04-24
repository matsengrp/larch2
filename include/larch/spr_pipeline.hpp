#pragma once

#include <larch/overlay_spr.hpp>
#include <larch/merge.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/spr_move.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
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

// Phase 33: A FragmentProducer takes a sampled tree, a fragment callback, and
// an RNG.  It runs in-place SPR moves internally and emits complete fragment
// DAGs (not individual moves) — needed because moves are relative to an
// evolving internal work_tree, not the caller's sampled tree.
template <typename F>
concept FragmentProducer =
    requires(F& f, phylo_dag& tree, std::function<void(phylo_dag)> cb,
             std::mt19937& rng) {
      { f(tree, cb, rng) };
    };

// A Producer is either a MoveProducer or a FragmentProducer.
template <typename F>
concept Producer = MoveProducer<F> || FragmentProducer<F>;

// Iterative DAG optimiser using fragment-based SPR pipeline.
//
// Each iteration:
//   1. Build / refresh the merged result DAG
//   2. Sample a min-weight (parsimony) tree
//   3. Dispatch each producer:
//      - MoveProducers: collect spr_move descriptors, generate fragments
//      - FragmentProducers: collect complete fragment DAGs directly
//   4. Merge all fragments + the sampled tree itself
//
// Supports any mix of MoveProducers and FragmentProducers.
template <Producer... Producers>
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

    // 3. Dispatch producers: collect moves and/or direct fragments
    std::vector<spr_move> collected_moves;
    std::vector<phylo_dag> direct_fragments;

    auto dispatch = [&]<typename P>(P& p) {
      if constexpr (MoveProducer<P>) {
        p(sampled,
          [&](spr_move const& move) { collected_moves.push_back(move); }, rng);
      } else {
        static_assert(FragmentProducer<P>);
        p(sampled,
          [&](phylo_dag frag) { direct_fragments.push_back(std::move(frag)); },
          rng);
      }
    };
    (dispatch(producers), ...);

    // 4. Generate fragments from collected moves (MoveProducer path)
    std::vector<phylo_dag> fragments(collected_moves.size());
    if (!collected_moves.empty()) {
      std::vector<std::size_t> indices(collected_moves.size());
      std::iota(indices.begin(), indices.end(), std::size_t{0});
      parallel_for_each(indices, [&](std::size_t i) {
        fragments[i] = apply_spr_as_fragment(sampled, collected_moves[i]);
      });
    }

    // 5. Merge all fragments
    for (auto& frag : fragments) m.add_dag(std::move(frag));
    for (auto& frag : direct_fragments) m.add_dag(std::move(frag));
    std::size_t trees_merged =
        collected_moves.size() + direct_fragments.size();

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

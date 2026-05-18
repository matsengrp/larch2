// rotaA_diagnostic_test.cpp — Diagnostic test suite for the rotavirusA
// 1-step parsimony score gap (larch2#33).
//
// Requires repro data directory set via ROTAA_REPRO_DIR env var.
// The test exits 77 (CTest skip) when the external repro data is not
// explicitly requested, keeping the default sanitizer suite self-contained.
//
// Experiments:
//   1. Fragment ground-truth audit (H1/H3: scoring bugs)
//   2. Pre/post trim score check  (H2: trim removes valid paths)
//   3. DAG weight DP validation   (H4: compute_weight_below bug)

#include <larch/native_optimize.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/merge.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/compute.hpp>
#include <larch/thread_pool.hpp>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <random>
#include <string>
#include <variant>
#include <vector>

using namespace larch;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string get_repro_dir() {
  auto* env = std::getenv("ROTAA_REPRO_DIR");
  if (env != nullptr && *env != '\0') {
    std::filesystem::path p{env};
    if (std::filesystem::exists(p)) return p.string();
    std::println(stderr, "SKIP: ROTAA_REPRO_DIR does not exist: {}", p.string());
    std::exit(77);
  }

  std::println(stderr,
               "SKIP: set ROTAA_REPRO_DIR to run rotaA external-data diagnostics");
  std::exit(77);
}

static std::size_t count_tree_mutations(phylo_dag& tree) {
  std::size_t total = 0;
  for (auto ev : tree.get_all_edges()) {
    std::visit([&](auto edge) { total += edge.mutations().size(); }, ev);
  }
  return total;
}

// Run a few optimization iterations on the merge to reach the target score.
// Returns the min parsimony score after the last iteration.
static std::size_t run_iterations(merge& m, std::size_t n_iters,
                                  std::uint32_t seed, int score_threshold) {
  std::mt19937 rng(seed);
  std::size_t min_score = 0;

  for (std::size_t iter = 0; iter < n_iters; ++iter) {
    auto& dag = m.get_result();
    parsimony_score_ops pops;
    std::uint32_t iter_seed = rng();
    scoped_arena<4096> sw_arena;
    subtree_weight<parsimony_score_ops> sw(dag, iter_seed, sw_arena.get());
    auto root_idx = get_root_idx(dag);
    min_score = sw.compute_weight_below(root_idx, pops);

    auto sampled = sw.min_weight_sample_tree(pops);
    fitch_assign_compact_genomes(sampled);
    recompute_edge_mutations(sampled);
    set_sample_ids_from_cg(sampled);

    std::println("  iter {}: score {} ({} nodes, {} edges)",
                 iter + 1, min_score, node_count(dag), edge_count(dag));

    // Score moves
    tree_index idx{sampled};
    move_enumerator enumerator{idx, score_threshold};
    auto radius = compute_tree_max_depth(sampled) * 2;
    if (radius == 0) radius = 1;

    for (std::size_t r = 2; r <= radius; r *= 2) {
      std::vector<profitable_move> moves;
      enumerator.find_all_moves(r, [&](auto& mv) { moves.push_back(mv); });

      std::sort(moves.begin(), moves.end(),
                [](auto& a, auto& b) { return a.score_change < b.score_change; });
      if (moves.size() > 150) moves.resize(150);

      for (auto& mv : moves) {
        auto frag = apply_spr_move(sampled, mv.src, mv.dst);
        m.add_dag(std::move(frag));
      }
    }
    m.add_dag(std::move(sampled));
  }
  return min_score;
}

// ---------------------------------------------------------------------------
// Experiment 1: Fragment Ground-Truth Audit
//
// Tests H1 (score prediction mismatch) and H3 (cached vs reference).
// Loads seed42 DAG, iterates to score 630, then validates predicted scores
// against actual fragment mutation counts.
// ---------------------------------------------------------------------------

static void test_fragment_ground_truth() {
  std::println("=== Experiment 1: Fragment Ground-Truth Audit ===");

  auto repro_dir = get_repro_dir();
  auto dag_path = repro_dir + "/seed42/dag/merged.dag.pb.gz";
  std::println("  Loading {}...", dag_path);
  auto dag = load_proto_dag(dag_path);
  auto const& ref = get_reference_sequence(dag);

  merge m{ref};
  m.add_dag(dag);

  // Run a few iterations to reach score 630 (seed42 reaches 630 by iter 3)
  std::println("  Running 3 warmup iterations (seed=42, threshold=0)...");
  auto score = run_iterations(m, 3, 42, /*score_threshold=*/0);
  std::println("  After warmup: min score = {}", score);

  // Sample a min-weight tree
  auto& result_dag = m.get_result();
  parsimony_score_ops pops;
  scoped_arena<4096> sw_arena;
  subtree_weight<parsimony_score_ops> sw(result_dag, 42, sw_arena.get());
  auto root_idx = get_root_idx(result_dag);
  auto min_score = sw.compute_weight_below(root_idx, pops);
  std::println("  DAG min score = {}", min_score);

  auto sampled = sw.min_weight_sample_tree(pops);
  fitch_assign_compact_genomes(sampled);
  recompute_edge_mutations(sampled);
  set_sample_ids_from_cg(sampled);

  auto original_score = count_tree_mutations(sampled);
  std::println("  Sampled tree mutation count = {}", original_score);
  assert(original_score == min_score);

  // Build tree index and enumerate moves
  tree_index idx{sampled};
  move_enumerator enumerator{idx, /*score_threshold=*/0};
  auto radius = compute_tree_max_depth(sampled) * 2;
  if (radius == 0) radius = 1;

  std::vector<profitable_move> all_moves;
  enumerator.find_all_moves(radius,
                            [&](auto& mv) { all_moves.push_back(mv); });

  std::println("  Total profitable moves (threshold<=0): {}", all_moves.size());

  // Sort by score_change (most negative first)
  std::sort(all_moves.begin(), all_moves.end(),
            [](auto& a, auto& b) { return a.score_change < b.score_change; });

  // Check the top 200 moves (or all if fewer)
  std::size_t n_check = std::min(all_moves.size(), std::size_t{200});
  std::println("  Checking top {} moves...", n_check);

  std::size_t cached_vs_ref_mismatches = 0;
  std::size_t predicted_vs_actual_mismatches = 0;
  std::size_t moves_with_neg_predicted = 0;

  for (std::size_t i = 0; i < n_check; ++i) {
    auto& mv = all_moves[i];

    // Cached score (already in mv.score_change)
    int predicted = mv.score_change;

    // Reference score (non-cached implementation)
    int reference = enumerator.compute_move_score(mv.src, mv.dst, mv.lca);

    // Ground truth: apply SPR and count mutations
    auto fragment = apply_spr_move(sampled, mv.src, mv.dst);
    auto actual_score = static_cast<int>(count_tree_mutations(fragment));
    int actual_change = actual_score - static_cast<int>(original_score);

    if (predicted < 0) moves_with_neg_predicted++;

    if (predicted != reference) {
      cached_vs_ref_mismatches++;
      std::println("  CACHED!=REF: move #{} src={} dst={} lca={} "
                   "cached={} ref={} actual={}",
                   i, mv.src, mv.dst, mv.lca, predicted, reference,
                   actual_change);
    }

    if (predicted != actual_change) {
      predicted_vs_actual_mismatches++;
      std::println("  PRED!=ACTUAL: move #{} src={} dst={} lca={} "
                   "predicted={} actual={} (ref={}, tree_score={}->{})",
                   i, mv.src, mv.dst, mv.lca, predicted, actual_change,
                   reference, original_score, actual_score);
    }
  }

  std::println("  Moves with predicted < 0: {}/{}", moves_with_neg_predicted,
               n_check);
  std::println("  Cached vs reference mismatches: {}/{}", cached_vs_ref_mismatches,
               n_check);
  std::println("  Predicted vs actual mismatches: {}/{}\n",
               predicted_vs_actual_mismatches, n_check);

  if (cached_vs_ref_mismatches > 0)
    std::println("  ** H3 CONFIRMED: cached scorer disagrees with reference **");
  if (predicted_vs_actual_mismatches > 0)
    std::println("  ** H1 CONFIRMED: predicted score != actual fragment score **");
  if (cached_vs_ref_mismatches == 0 && predicted_vs_actual_mismatches == 0)
    std::println("  H1/H3 ruled out: all {} checked scores are correct", n_check);

  // Don't assert — we need to continue to the condensing test
  std::println("");
}

// ---------------------------------------------------------------------------
// Experiment 1b: Condensing Bug Detection
//
// The condensing in tree_index removes identical sibling leaves from the
// children array, turning a binary parent into a unary node. This causes
// compute_initial_removal to take the non-binary path, computing
// new_fitch = fitch_set_from_counts(counts, 0) = 0 (empty). This empty
// fitch propagates to all ancestors, corrupting score predictions for all
// moves where src has a condensed parent.
//
// This test checks ALL moves (not just top 200) from condensed-parent
// sources, comparing predicted vs actual scores.
// ---------------------------------------------------------------------------

static void test_condensing_bug() {
  std::println("=== Experiment 1b: Condensing Bug Detection ===");

  auto repro_dir = get_repro_dir();
  auto dag_path = repro_dir + "/seed42/dag/merged.dag.pb.gz";
  std::println("  Loading {}...", dag_path);
  auto dag = load_proto_dag(dag_path);
  auto const& ref = get_reference_sequence(dag);

  merge m{ref};
  m.add_dag(dag);

  std::println("  Running 3 warmup iterations...");
  auto score = run_iterations(m, 3, 42, 0);
  std::println("  After warmup: min score = {}", score);

  auto& result_dag = m.get_result();
  parsimony_score_ops pops;
  scoped_arena<4096> sw_arena;
  subtree_weight<parsimony_score_ops> sw(result_dag, 42, sw_arena.get());
  auto root_idx = get_root_idx(result_dag);
  sw.compute_weight_below(root_idx, pops);

  auto sampled = sw.min_weight_sample_tree(pops);
  fitch_assign_compact_genomes(sampled);
  recompute_edge_mutations(sampled);
  set_sample_ids_from_cg(sampled);

  auto original_score = count_tree_mutations(sampled);
  std::println("  Sampled tree: {} mutations", original_score);

  tree_index idx{sampled};
  std::println("  Condensed leaves: {}", idx.num_condensed_leaves());

  // Also check a few other sampled trees for condensed leaves
  {
    std::size_t trees_with_condensed = 0;
    for (std::uint32_t s = 0; s < 10; ++s) {
      scoped_arena<4096> a2;
      subtree_weight<parsimony_score_ops> sw2(result_dag, s * 31 + 7, a2.get());
      sw2.compute_weight_below(root_idx, pops);
      auto t = sw2.min_weight_sample_tree(pops);
      fitch_assign_compact_genomes(t);
      recompute_edge_mutations(t);
      set_sample_ids_from_cg(t);
      tree_index ti{t};
      if (ti.num_condensed_leaves() > 0) {
        trees_with_condensed++;
        std::println("    seed {}: {} condensed", s * 31 + 7,
                     ti.num_condensed_leaves());
      }
    }
    std::println("  Trees with condensed leaves: {}/10", trees_with_condensed);
  }

  if (idx.num_condensed_leaves() == 0) {
    std::println("  No condensed leaves — condensing bug cannot trigger here");
    std::println("  Checking ALL moves for hidden improvements anyway...");
  }

  // Identify src nodes whose parent was affected by condensing.
  // These are searchable nodes whose parent has fewer children in the
  // tree_index than in the original tree.
  // Alternatively: find nodes whose parent has a condensed sibling.
  // A simpler approach: find all searchable nodes, check each one's parent,
  // see if any sibling of the node was condensed.
  auto& searchable = idx.get_searchable_nodes();
  std::vector<std::size_t> condensed_parent_srcs;
  for (auto src : searchable) {
    if (src == idx.get_tree_root()) continue;
    auto parent = idx.get_parent(src);
    auto& siblings = idx.get_children(parent);
    // In the current code, condensed leaves were REMOVED from children.
    // A binary parent that had a condensed child now has nc=1.
    // If nc=1 and this is an internal node (not UA), it was likely condensed.
    if (siblings.size() == 1) {
      condensed_parent_srcs.push_back(src);
    }
  }

  std::println("  Nodes with unary parent (condensed binary): {}",
               condensed_parent_srcs.size());

  if (condensed_parent_srcs.empty()) {
    std::println("  No unary parents found — checking all moves instead");
  }

  // Score ALL moves and check for actual improvements missed by prediction
  move_enumerator enumerator{idx, 0};
  auto radius = compute_tree_max_depth(sampled) * 2;
  if (radius == 0) radius = 1;

  std::vector<profitable_move> all_moves;
  enumerator.find_all_moves(radius,
                            [&](auto& mv) { all_moves.push_back(mv); });

  std::println("  Total moves with predicted <= 0: {}", all_moves.size());
  std::println("  Checking ALL moves for actual improvements...");

  std::size_t mismatches = 0;
  std::size_t hidden_improvements = 0;
  std::size_t checked = 0;

  for (auto& mv : all_moves) {
    auto fragment = apply_spr_move(sampled, mv.src, mv.dst);
    auto actual_score = static_cast<int>(count_tree_mutations(fragment));
    int actual_change = actual_score - static_cast<int>(original_score);

    if (mv.score_change != actual_change) {
      mismatches++;
      if (mismatches <= 10) {
        bool is_condensed_src =
            !condensed_parent_srcs.empty() &&
            std::find(condensed_parent_srcs.begin(),
                      condensed_parent_srcs.end(),
                      mv.src) != condensed_parent_srcs.end();
        std::println(
            "  MISMATCH: src={} dst={} lca={} predicted={} actual={} "
            "condensed_parent_src={}",
            mv.src, mv.dst, mv.lca, mv.score_change, actual_change,
            is_condensed_src);
      }
      if (actual_change < 0 && mv.score_change >= 0) hidden_improvements++;
    }
    checked++;
    if (checked % 500 == 0)
      std::println("    ...checked {}/{}", checked, all_moves.size());
  }

  std::println("  Total mismatches: {}/{}", mismatches, all_moves.size());
  std::println("  Hidden improvements (predicted>=0 but actual<0): {}",
               hidden_improvements);

  if (hidden_improvements > 0) {
    std::println(
        "  ** CONDENSING BUG CONFIRMED: {} moves scored as non-improving "
        "are actually improving **",
        hidden_improvements);
    std::println(
        "  This explains the basin isolation: the optimizer cannot see "
        "these improving moves.");
  } else if (mismatches > 0) {
    std::println(
        "  Scoring mismatches exist but no hidden improvements found.");
  } else {
    std::println("  All scores match — condensing does not cause mismatches.");
  }

  std::println("  DONE\n");
}

// ---------------------------------------------------------------------------
// Experiment 2: Pre/Post Trim Score Check
//
// Tests H2: does trim_inconsistent_clade_edges remove valid 629-score paths?
// Loads seed43 DAG (which reaches 629), samples a 629-score tree, merges it
// into seed42's DAG, and checks scores before and after trim.
// ---------------------------------------------------------------------------

static void test_trim_score_loss() {
  std::println("=== Experiment 2: Pre/Post Trim Score Check ===");

  auto repro_dir = get_repro_dir();

  // Load seed43 and optimize to reach 629
  std::println("  Loading seed43 DAG...");
  auto dag43 = load_proto_dag(repro_dir + "/seed43/dag/merged.dag.pb.gz");
  auto const& ref43 = get_reference_sequence(dag43);
  merge m43{ref43};
  m43.add_dag(dag43);

  std::println("  Running 10 iterations on seed43 (seed=43, threshold=0)...");
  auto score43 = run_iterations(m43, 10, 43, 0);
  std::println("  Seed43 score after 10 iters: {}", score43);

  // Sample a min-weight tree from seed43 — should be 629
  auto& dag43_result = m43.get_result();
  {
    parsimony_score_ops pops;
    scoped_arena<4096> arena;
    subtree_weight<parsimony_score_ops> sw(dag43_result, 43, arena.get());
    auto min43 = sw.compute_weight_below(get_root_idx(dag43_result), pops);
    std::println("  Seed43 DAG min score: {}", min43);
    if (min43 > 629) {
      std::println("  WARNING: seed43 didn't reach 629 in 10 iters, "
                   "running 10 more...");
      score43 = run_iterations(m43, 10, 430, 0);
      auto& dag43_r2 = m43.get_result();
      subtree_weight<parsimony_score_ops> sw2(dag43_r2, 43, arena.get());
      min43 = sw2.compute_weight_below(get_root_idx(dag43_r2), pops);
      std::println("  Seed43 DAG min score after 20 iters: {}", min43);
    }
  }

  // Extract a 629-score tree from seed43
  auto& dag43_final = m43.get_result();
  parsimony_score_ops pops;
  scoped_arena<4096> arena;
  subtree_weight<parsimony_score_ops> sw43(dag43_final, 999, arena.get());
  auto min43_final = sw43.compute_weight_below(get_root_idx(dag43_final), pops);
  auto tree629 = sw43.min_weight_sample_tree(pops);
  fitch_assign_compact_genomes(tree629);
  recompute_edge_mutations(tree629);
  set_sample_ids_from_cg(tree629);
  auto tree629_score = count_tree_mutations(tree629);
  std::println("  Extracted tree from seed43: {} mutations", tree629_score);

  if (tree629_score > 629) {
    std::println("  SKIP: could not extract a 629-score tree from seed43");
    std::println("  INCONCLUSIVE\n");
    return;
  }

  // Load seed42 and optimize to reach 630
  std::println("  Loading seed42 DAG...");
  auto dag42 = load_proto_dag(repro_dir + "/seed42/dag/merged.dag.pb.gz");
  auto const& ref42 = get_reference_sequence(dag42);
  merge m42{ref42};
  m42.add_dag(dag42);

  std::println("  Running 3 iterations on seed42...");
  run_iterations(m42, 3, 42, 0);

  // Now merge the 629-score tree into seed42's merge
  std::println("  Merging 629-score tree into seed42's DAG...");
  m42.add_dag(tree629);

  // Build result (which includes trim). Check min score after.
  auto& merged_result = m42.get_result();
  {
    scoped_arena<4096> arena2;
    subtree_weight<parsimony_score_ops> sw(merged_result, 42, arena2.get());
    auto merged_min =
        sw.compute_weight_below(get_root_idx(merged_result), pops);
    std::println("  Score AFTER merging 629 tree + trim: {}", merged_min);

    if (merged_min > 629) {
      std::println(
          "  ** H2 LIKELY: merged DAG score is {} (expected 629) **",
          merged_min);
      std::println("  The 629-score path was lost during merge or trim.");
    } else {
      std::println("  H2 ruled out: merged DAG correctly contains 629 path");
    }
  }

  std::println("  DONE\n");
}

// ---------------------------------------------------------------------------
// Experiment 3: DAG Weight DP Validation
//
// Tests H4: does compute_weight_below correctly compute the minimum?
// Samples many trees from seed42's DAG and verifies mutation counts match
// the DP-reported minimum.
// ---------------------------------------------------------------------------

static void test_dp_validation() {
  std::println("=== Experiment 3: DAG Weight DP Validation ===");

  auto repro_dir = get_repro_dir();

  std::println("  Loading seed42 DAG...");
  auto dag42 = load_proto_dag(repro_dir + "/seed42/dag/merged.dag.pb.gz");
  auto const& ref = get_reference_sequence(dag42);
  merge m{ref};
  m.add_dag(dag42);

  std::println("  Running 3 warmup iterations...");
  run_iterations(m, 3, 42, 0);

  auto& result_dag = m.get_result();
  parsimony_score_ops pops;
  scoped_arena<4096> arena;
  subtree_weight<parsimony_score_ops> sw(result_dag, 42, arena.get());
  auto root_idx = get_root_idx(result_dag);
  auto dp_min = sw.compute_weight_below(root_idx, pops);
  std::println("  DP-reported min score: {}", dp_min);

  // Sample many min-weight trees and verify
  std::size_t n_samples = 100;
  std::size_t min_mismatch = 0;
  std::size_t actual_min_seen = dp_min;

  for (std::uint32_t s = 0; s < n_samples; ++s) {
    scoped_arena<4096> a2;
    subtree_weight<parsimony_score_ops> sw2(result_dag, s, a2.get());
    sw2.compute_weight_below(root_idx, pops);
    auto tree = sw2.min_weight_sample_tree(pops);
    fitch_assign_compact_genomes(tree);
    recompute_edge_mutations(tree);
    set_sample_ids_from_cg(tree);
    auto actual = count_tree_mutations(tree);
    if (actual != dp_min) {
      min_mismatch++;
      if (min_mismatch <= 5) {
        std::println("  MIN_MISMATCH: seed={} dp_min={} actual={}", s, dp_min,
                     actual);
      }
    }
    if (actual < actual_min_seen) actual_min_seen = actual;
  }

  std::println("  Min-weight sample mismatches: {}/{}", min_mismatch, n_samples);

  // Also sample random (non-min-weight) trees to check for hidden low scores
  std::size_t n_random = 200;
  std::size_t lower_than_dp = 0;
  std::size_t random_min_seen = dp_min;

  for (std::uint32_t s = 0; s < n_random; ++s) {
    scoped_arena<4096> a3;
    subtree_weight<parsimony_score_ops> sw3(result_dag, s + 10000, a3.get());
    sw3.compute_weight_below(root_idx, pops);
    auto tree = sw3.sample_tree(pops);
    fitch_assign_compact_genomes(tree);
    recompute_edge_mutations(tree);
    set_sample_ids_from_cg(tree);
    auto actual = count_tree_mutations(tree);
    if (actual < dp_min) {
      lower_than_dp++;
      if (lower_than_dp <= 5) {
        std::println("  LOWER_THAN_DP: seed={} dp_min={} actual={}", s + 10000,
                     dp_min, actual);
      }
    }
    if (actual < random_min_seen) random_min_seen = actual;
  }

  std::println("  Random samples lower than DP min: {}/{}", lower_than_dp,
               n_random);
  std::println("  Lowest score seen (min-weight): {}", actual_min_seen);
  std::println("  Lowest score seen (random): {}", random_min_seen);

  if (min_mismatch > 0)
    std::println("  ** H4 CONFIRMED: min-weight trees don't match DP score **");
  if (lower_than_dp > 0)
    std::println("  ** H4 CONFIRMED: random trees found below DP minimum **");
  if (min_mismatch == 0 && lower_than_dp == 0)
    std::println("  H4 ruled out: DP computes correct minimum");

  assert(min_mismatch == 0);
  assert(lower_than_dp == 0);
  std::println("  PASS\n");
}

// ---------------------------------------------------------------------------
// Experiment 4: Basin Analysis
//
// The key finding from Experiment 1: zero moves have predicted < 0, meaning
// the sampled 630-score tree is a local optimum for single-SPR search.
// This experiment checks whether ALL 630-score trees in seed42's DAG are
// stuck, or just the one that was sampled.
// ---------------------------------------------------------------------------

static void test_basin_analysis() {
  std::println("=== Experiment 4: Basin Analysis ===");

  auto repro_dir = get_repro_dir();
  auto dag = load_proto_dag(repro_dir + "/seed42/dag/merged.dag.pb.gz");
  auto const& ref = get_reference_sequence(dag);
  merge m{ref};
  m.add_dag(dag);

  // Run 5 iterations to grow the DAG a bit more
  std::println("  Running 5 warmup iterations...");
  run_iterations(m, 5, 42, 0);

  auto& result_dag = m.get_result();
  parsimony_score_ops pops;
  {
    scoped_arena<4096> arena;
    subtree_weight<parsimony_score_ops> sw(result_dag, 42, arena.get());
    auto dp_min = sw.compute_weight_below(get_root_idx(result_dag), pops);
    std::println("  DAG min score: {} ({} nodes, {} edges)", dp_min,
                 node_count(result_dag), edge_count(result_dag));
  }

  // Sample many different 630-score trees and check each for improving moves
  std::size_t n_trees = 20;
  std::size_t trees_with_neg_moves = 0;
  std::size_t total_neg_moves = 0;

  for (std::uint32_t s = 0; s < n_trees; ++s) {
    scoped_arena<4096> arena;
    subtree_weight<parsimony_score_ops> sw(result_dag, s * 97 + 1, arena.get());
    sw.compute_weight_below(get_root_idx(result_dag), pops);
    auto sampled = sw.min_weight_sample_tree(pops);
    fitch_assign_compact_genomes(sampled);
    recompute_edge_mutations(sampled);
    set_sample_ids_from_cg(sampled);

    tree_index idx{sampled};
    // Use threshold -1 to only find strictly improving moves
    move_enumerator enumerator{idx, -1};
    auto radius = compute_tree_max_depth(sampled) * 2;
    if (radius == 0) radius = 1;

    std::size_t neg_count = 0;
    std::size_t total_moves = 0;
    enumerator.find_all_moves(radius, [&](auto& mv) {
      total_moves++;
      if (mv.score_change < 0) neg_count++;
    });

    if (neg_count > 0) {
      trees_with_neg_moves++;
      total_neg_moves += neg_count;
      std::println("  tree seed={}: {} improving moves out of {} total",
                   s * 97 + 1, neg_count, total_moves);
    }
  }

  std::println("  Trees with improving moves: {}/{}", trees_with_neg_moves,
               n_trees);
  std::println("  Total improving moves found: {}", total_neg_moves);

  if (trees_with_neg_moves == 0) {
    std::println(
        "  ** BASIN ISOLATION CONFIRMED: no single SPR from any sampled "
        "630-score tree reaches 629 **");
    std::println(
        "  Root cause: the 630 basin in seed42's DAG is separated from the "
        "629 basin");
    std::println(
        "  by a parsimony barrier that requires >1 SPR step to cross.");
  } else {
    std::println(
        "  Some 630-score trees DO have improving moves — the issue may be "
        "in which tree gets sampled or how moves are filtered.");
  }

  std::println("  DONE\n");
}

// ---------------------------------------------------------------------------
// Experiment 5: Non-Optimal Sampling
//
// The DAG contains trees at various score levels (630, 631, 632, ...).
// A tree at score 631 might have an SPR move with score_change = -2 that
// reaches 629 — a path invisible from any 630-score tree.
// This tests whether sampling non-optimal trees from the DAG exposes
// improving moves that cross the basin barrier.
// ---------------------------------------------------------------------------

static void test_nonoptimal_sampling() {
  std::println("=== Experiment 5: Non-Optimal Sampling ===");

  auto repro_dir = get_repro_dir();
  auto dag = load_proto_dag(repro_dir + "/seed42/dag/merged.dag.pb.gz");
  auto const& ref = get_reference_sequence(dag);
  merge m{ref};
  m.add_dag(dag);

  // Run 5 iterations to build a DAG with structural diversity
  std::println("  Running 5 warmup iterations...");
  run_iterations(m, 5, 42, 0);

  auto& result_dag = m.get_result();
  parsimony_score_ops pops;
  {
    scoped_arena<4096> arena;
    subtree_weight<parsimony_score_ops> sw(result_dag, 42, arena.get());
    auto dp_min = sw.compute_weight_below(get_root_idx(result_dag), pops);
    std::println("  DAG min score: {} ({} nodes, {} edges)", dp_min,
                 node_count(result_dag), edge_count(result_dag));
  }

  // Sample random (non-min-weight) trees and check their SPR neighborhoods
  auto root_idx = get_root_idx(result_dag);
  std::size_t n_random = 100;
  std::map<std::size_t, std::size_t> score_histogram;
  std::size_t trees_with_sub630_moves = 0;
  std::size_t best_reachable = 9999;

  for (std::uint32_t s = 0; s < n_random; ++s) {
    scoped_arena<4096> arena;
    subtree_weight<parsimony_score_ops> sw(result_dag, s * 73 + 5, arena.get());
    sw.compute_weight_below(root_idx, pops);
    auto sampled = sw.sample_tree(pops);  // random, NOT min-weight
    fitch_assign_compact_genomes(sampled);
    recompute_edge_mutations(sampled);
    set_sample_ids_from_cg(sampled);

    auto tree_score = count_tree_mutations(sampled);
    score_histogram[tree_score]++;

    // Only check trees with score > 630 (non-optimal) — these are
    // the ones that might bridge to the 629 basin
    if (tree_score <= 630) continue;

    tree_index idx{sampled};
    // Use threshold -1: only strictly improving moves
    move_enumerator enumerator{idx, -1};
    auto radius = compute_tree_max_depth(sampled) * 2;
    if (radius == 0) radius = 1;

    int best_change = 0;
    enumerator.find_all_moves(radius, [&](auto& mv) {
      if (mv.score_change < best_change) best_change = mv.score_change;
    });

    auto reachable_score =
        static_cast<std::size_t>(static_cast<int>(tree_score) + best_change);
    if (reachable_score < 630) {
      trees_with_sub630_moves++;
      std::println("  BRIDGE FOUND: tree score={}, best move={}, reaches {}",
                   tree_score, best_change, reachable_score);
    }
    if (reachable_score < best_reachable) best_reachable = reachable_score;
  }

  std::println("\n  Score distribution of {} random samples:", n_random);
  for (auto [score, count] : score_histogram) {
    std::println("    score {}: {} trees", score, count);
  }

  std::println("\n  Trees with moves reaching below 630: {}", trees_with_sub630_moves);
  std::println("  Best reachable score from non-optimal trees: {}",
               best_reachable);

  if (trees_with_sub630_moves > 0) {
    std::println(
        "\n  ** NON-OPTIMAL SAMPLING CAN ESCAPE THE BASIN **");
    std::println(
        "  Sampling higher-score trees from the DAG exposes SPR moves that");
    std::println(
        "  cross the 630 barrier and reach 629 or below.");
  } else {
    std::println(
        "\n  Non-optimal sampling did not find bridge moves in {} samples.",
        n_random);
    std::println(
        "  The barrier may require more than one step of worsening, or");
    std::println(
        "  the DAG may not contain the right structural elements yet.");
  }

  std::println("  DONE\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  std::println("rotaA_diagnostic_test");
  std::println("repro dir: {}\n", get_repro_dir());

  test_nonoptimal_sampling();

  std::println("All experiments completed.");
  return 0;
}

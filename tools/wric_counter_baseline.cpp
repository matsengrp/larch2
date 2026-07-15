// Generates the WRIC DAG-native SPR plan Phase 0 baseline snapshot of
// chart_spr_search_counters for one search run on data/test_5_trees/ in the
// conservative rebuild_after_accept = true mode.
//
// IMPORTANT: this program emits ONLY the fenced snapshot block (the counter
// block, not the surrounding prose).  It must NOT be piped directly
// over doc/WRIC-CHART-SPR-SEARCH-COUNTER-BASELINE.md: doing so would delete
// the hand-written prose in that file.  Instead, regenerate via
// tools/regen_counter_baseline.sh, which splices this program's stdout between
// the `<!-- wric-counter-baseline: snapshot begin/end -->` markers in the doc
// and leaves every line of prose untouched.  See the "How to regenerate"
// section of the doc and the header of that script.
//
// This program is intentionally NOT a build target or a run-ctest (its output
// is a checked-in snapshot, and running it does a full search).  It IS covered
// by the compile-only ctest `wric_counter_baseline_compiles`, so a renamed
// counter field breaks CI before it breaks regeneration.
//
// Build (from the repo root, after configuring the project):
//   cmake --build build -j  # ensure liblarch.a + generated/version.hpp exist
//   g++-trunk -std=c++26 -freflection -I include -I build/generated \
//       tools/wric_counter_baseline.cpp build/liblarch.a -lz -lpthread \
//       -static-libstdc++ -static-libgcc -o build/wric_counter_baseline
// Run (from the repo root, so data/test_5_trees/ resolves):
//   ./build/wric_counter_baseline            # prints the snapshot block
//   ./tools/regen_counter_baseline.sh        # splices it into the doc

#include <larch/chart_spr_search.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/compute.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/merge.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/version.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <print>
#include <string>
#include <vector>

namespace {

constexpr std::array<char const*, 5> kTreePaths = {
    "data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
    "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
    "data/test_5_trees/tree_4.pb.gz",
};

larch::phylo_dag load_merged_test_5_trees() {
  std::vector<larch::phylo_dag> trees;
  trees.reserve(kTreePaths.size());
  for (auto* path : kTreePaths) {
    trees.emplace_back(larch::load_proto_dag(path));
    larch::recompute_compact_genomes(trees.back());
    larch::set_sample_ids_from_cg(trees.back());
  }

  larch::merge merger{larch::get_reference_sequence(trees.front())};
  for (auto& t : trees) merger.add_dag(t);
  return larch::phylo_dag{std::move(merger.get_result())};
}

// The merged test_5_trees DAG contains soft polytomies, so chart search needs
// a binary-chart-compatible grammar.  Mirror the dagutil ctest configuration
// for this fixture (expand_soft_bounded, max_shapes_per_polytomy = 1) so the
// baseline is the same binary-grammar configuration the existing CI smoke
// test exercises.
larch::clade_grammar build_search_grammar(larch::phylo_dag& dag) {
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "wric_counter_baseline");
  return refinement.grammar;
}

void print_counter_line(std::string const& key, std::size_t value) {
  std::println("  {:60} {}", key + ":", value);
}

}  // namespace

int main() {
  auto dag = load_merged_test_5_trees();
  auto grammar = build_search_grammar(dag);

  larch::chart_spr_search_options options;
  // Conservative mode is the current default; make it explicit so the
  // baseline is unambiguous.  exact_multisite + lower_bound_top_k are the
  // option defaults.
  options.rebuild_after_accept = true;
  options.max_iterations = 3;
  options.seed = 1;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  auto const& c = search.counters;
  auto const& s = search.summary;

  // The `[snapshot]` suffix distinguishes this auto-generated block header
  // (printed verbatim into the fenced block in the doc) from the doc's own H1
  // title.  tools/regen_counter_baseline.sh reproduces this exact line, so the
  // two must stay in sync; see the doc's "How to regenerate" section.
  std::println("# WRIC chart-SPR search counter baseline (Phase 0) [snapshot]");
  std::println();
  std::println("Fixture: data/test_5_trees/ (tree_0..tree_4 merged)");
  std::println("Mode: rebuild_after_accept = true (conservative)");
  std::println("polytomy_mode: expand_soft_bounded (max_shapes_per_polytomy=1)");
  std::println("acceptance_mode: {}",
               larch::chart_spr_acceptance_mode_name(options.acceptance_mode));
  std::println("candidate_selection: {}",
               larch::chart_spr_candidate_selection_mode_name(
                   options.candidate_selection));
  std::println("max_iterations: {}", options.max_iterations);
  std::println("seed: {}", options.seed);
  std::println("larch_version: {}", larch::version);
  std::println();
  std::println("initial_grammar_clades: {}", s.initial_grammar_clade_count);
  std::println("initial_grammar_productions: {}",
               s.initial_grammar_production_count);
  std::println("active_pattern_count: {}", s.active_pattern_count);
  std::println("iterations_run: {}", s.iterations);
  std::println("initial_score: {}", s.initial_score);
  std::println("final_score: {}", s.final_score);
  std::println();
  std::println("chart_spr_search_counters:");
  print_counter_line("grammar_rebuilds", c.grammar_rebuilds);
  print_counter_line("pattern_rebuilds", c.pattern_rebuilds);
  print_counter_line("base_chart_cache_rebuilds",
                     c.base_chart_cache_rebuilds);
  print_counter_line("full_overlay_materializations",
                     c.full_overlay_materializations);
  print_counter_line("overlay_materializations_for_oracle",
                     c.overlay_materializations_for_oracle);
  print_counter_line("overlay_materializations_for_local_scoring_bridge",
                     c.overlay_materializations_for_local_scoring_bridge);
  print_counter_line("overlay_materializations_for_exact_verification",
                     c.overlay_materializations_for_exact_verification);
  print_counter_line("overlay_materializations_for_accept_materialization",
                     c.overlay_materializations_for_accept_materialization);
  print_counter_line("overlay_materializations_for_final_compaction",
                     c.overlay_materializations_for_final_compaction);
  print_counter_line("sidecar_rebuilds_after_accept",
                     c.sidecar_rebuilds_after_accept);
  print_counter_line("local_commit_accepted_moves",
                     c.local_commit_accepted_moves);
  print_counter_line("local_commit_tombstone_scope_skips",
                     c.local_commit_tombstone_scope_skips);
  print_counter_line("inside_rows_recomputed_on_commit",
                     c.inside_rows_recomputed_on_commit);
  print_counter_line("outside_rows_recomputed_on_commit",
                     c.outside_rows_recomputed_on_commit);
  print_counter_line("lazy_inside_rows_computed",
                     c.lazy_inside_rows_computed);
  print_counter_line("lazy_outside_rows_computed",
                     c.lazy_outside_rows_computed);
  print_counter_line("lazy_patterns_merged_max",
                     c.lazy_patterns_merged_max);
  print_counter_line("lazy_remerge_collisions",
                     c.lazy_remerge_collisions);
  print_counter_line("lazy_inside_rows_recomputed_on_commit",
                     c.lazy_inside_rows_recomputed_on_commit);
  print_counter_line("lazy_outside_rows_recomputed_on_commit",
                     c.lazy_outside_rows_recomputed_on_commit);
  print_counter_line("lazy_incremental_rows_recomputed",
                     c.lazy_incremental_rows_recomputed);
  print_counter_line("lazy_structural_class_count_max",
                     c.lazy_structural_class_count_max);
  print_counter_line("lazy_chart_memory_budget_bytes",
                     c.lazy_chart_memory_budget_bytes);
  print_counter_line("lazy_chart_inside_max_admitted_slots",
                     c.lazy_chart_inside_max_admitted_slots);
  print_counter_line("lazy_chart_outside_max_admitted_slots",
                     c.lazy_chart_outside_max_admitted_slots);
  print_counter_line("lazy_chart_inside_admission_waves",
                     c.lazy_chart_inside_admission_waves);
  print_counter_line("lazy_chart_outside_admission_waves",
                     c.lazy_chart_outside_admission_waves);
  print_counter_line("lazy_chart_inside_memory_limited_levels",
                     c.lazy_chart_inside_memory_limited_levels);
  print_counter_line("lazy_chart_outside_memory_limited_levels",
                     c.lazy_chart_outside_memory_limited_levels);
  print_counter_line("lazy_chart_inside_reused_slot_waves",
                     c.lazy_chart_inside_reused_slot_waves);
  print_counter_line("lazy_chart_outside_reused_slot_waves",
                     c.lazy_chart_outside_reused_slot_waves);
  print_counter_line("lazy_chart_inside_workspace_evictions",
                     c.lazy_chart_inside_workspace_evictions);
  print_counter_line("lazy_chart_outside_workspace_evictions",
                     c.lazy_chart_outside_workspace_evictions);
  print_counter_line("lazy_chart_preflight_peak_bytes",
                     c.lazy_chart_preflight_peak_bytes);
  print_counter_line("lazy_chart_actual_peak_bytes",
                     c.lazy_chart_actual_peak_bytes);
  print_counter_line("lazy_chart_pre_submit_rejections",
                     c.lazy_chart_pre_submit_rejections);
  print_counter_line("multifurcation_productions_scored",
                     c.multifurcation_productions_scored);
  print_counter_line("local_commit_two_chart_oracle_runs",
                     c.local_commit_two_chart_oracle_runs);
  print_counter_line("local_commit_tip_grammar_refreshes",
                     c.local_commit_tip_grammar_refreshes);
  print_counter_line("fixed_topology_selected_cache_hits",
                     c.fixed_topology_selected_cache_hits);
  print_counter_line("fixed_topology_selected_cache_misses",
                     c.fixed_topology_selected_cache_misses);
  print_counter_line("fixed_topology_selected_rows_computed",
                     c.fixed_topology_selected_rows_computed);
  print_counter_line("selected_topology_class_rows_computed",
                     c.selected_topology_class_rows_computed);
  print_counter_line("fixed_topology_persistent_cache_verifications",
                     c.fixed_topology_persistent_cache_verifications);
  print_counter_line("fixed_topology_persistent_cache_fallbacks",
                     c.fixed_topology_persistent_cache_fallbacks);
  print_counter_line("fixed_topology_persistent_cache_oracle_mismatches",
                     c.fixed_topology_persistent_cache_oracle_mismatches);
  // Phase 10 counter-hygiene: the production per-pattern gate, icache
  // participation, and chain-objective diagnostics are part of the contract
  // surface, so they are pinned in the baseline (all zero in this conservative-
  // mode snapshot; printed so a renamed field breaks regeneration before it
  // breaks a CI table).
  print_counter_line(
      "fixed_topology_persistent_cache_direct_oracle_mismatches",
      c.fixed_topology_persistent_cache_direct_oracle_mismatches);
  print_counter_line("fixed_topology_icache_rows_reused",
                     c.fixed_topology_icache_rows_reused);
  print_counter_line("fixed_topology_icache_rows_recomputed_affected",
                     c.fixed_topology_icache_rows_recomputed_affected);
  print_counter_line("fixed_topology_chain_objective_before_mismatches",
                     c.fixed_topology_chain_objective_before_mismatches);
  // Phase 9 (Work item 4a, technique 2): transient chain extension for
  // exact_multisite verification.  Zero in this conservative-mode baseline
  // (the transient verifier is installed only when a local-commit substrate
  // is active, i.e. rebuild_after_accept = false); printed here so the counter
  // contract is complete and a regression to "dense materialize per candidate"
  // under a renamed counter stays visible.  See the Phase-9 caveat in
  // doc/WRIC-SPR-SEARCH.md: as shipped these measure the transient-extension
  // substrate, not a wall-clock win.
  print_counter_line("transient_chain_extensions_for_verification",
                     c.transient_chain_extensions_for_verification);
  print_counter_line("transient_chain_diagnostic_cache_extensions",
                     c.transient_chain_diagnostic_cache_extensions);
  print_counter_line("transient_chain_extension_fallbacks",
                     c.transient_chain_extension_fallbacks);
  print_counter_line("transient_chain_extension_oracle_mismatches",
                     c.transient_chain_extension_oracle_mismatches);
  print_counter_line("full_composite_rebuilds", c.full_composite_rebuilds);
  print_counter_line("local_candidate_scores", c.local_candidate_scores);
  print_counter_line("local_rows_recomputed", c.local_rows_recomputed);
  print_counter_line("local_unit_fitch_fast_path_productions_scored",
                     c.local_unit_fitch_fast_path_productions_scored);
  print_counter_line("local_leaf_state_view_uses",
                     c.local_leaf_state_view_uses);
  print_counter_line("local_leaf_state_owned_copies",
                     c.local_leaf_state_owned_copies);
  print_counter_line("local_row_scratch_capacity_growths",
                     c.local_row_scratch_capacity_growths);
  print_counter_line("local_score_parallel_batches",
                     c.local_score_parallel_batches);
  print_counter_line("local_score_worker_tasks", c.local_score_worker_tasks);
  print_counter_line("candidate_batches_scored", c.candidate_batches_scored);
  print_counter_line("pattern_batch_cache_builds",
                     c.pattern_batch_cache_builds);
  print_counter_line("initial_state_inside_charts_built",
                     c.initial_state_inside_charts_built);
  print_counter_line("inside_cache_inside_charts_built",
                     c.inside_cache_inside_charts_built);
  print_counter_line("inside_cache_resident_inside_charts_consumed",
                     c.inside_cache_resident_inside_charts_consumed);
  print_counter_line("exact_setup_builds", c.exact_setup_builds);
  print_counter_line("exact_setup_inside_charts_built",
                     c.exact_setup_inside_charts_built);
  print_counter_line("exact_setup_resident_inside_charts_consumed",
                     c.exact_setup_resident_inside_charts_consumed);
  print_counter_line("exact_setup_active_leaf_state_vectors_copied",
                     c.exact_setup_active_leaf_state_vectors_copied);
  print_counter_line("exact_setup_active_leaf_states_copied",
                     c.exact_setup_active_leaf_states_copied);
  print_counter_line("exact_setup_outside_boundary_charts_built",
                     c.exact_setup_outside_boundary_charts_built);
  print_counter_line("exact_setup_upper_bound_topologies_generated",
                     c.exact_setup_upper_bound_topologies_generated);
  print_counter_line("exact_setup_upper_bound_topologies_unique",
                     c.exact_setup_upper_bound_topologies_unique);
  print_counter_line("exact_setup_frontier_passes",
                     c.exact_setup_frontier_passes);
  print_counter_line("exact_trim_lazy_chart_uses",
                     c.exact_trim_lazy_chart_uses);
  print_counter_line("outside_cache_inside_charts_built",
                     c.outside_cache_inside_charts_built);
  print_counter_line("outside_cache_inside_charts_reused",
                     c.outside_cache_inside_charts_reused);
  print_counter_line("outside_cache_outside_charts_built",
                     c.outside_cache_outside_charts_built);
  print_counter_line("exact_verifications", c.exact_verifications);
  print_counter_line("accepted_moves", c.accepted_moves);
  print_counter_line("candidate_accepts_attempted",
                     c.candidate_accepts_attempted);
  print_counter_line("rejected_moves", c.rejected_moves);
  print_counter_line("post_materialization_rejections",
                     c.post_materialization_rejections);
  print_counter_line("skipped_invariant_sites", c.skipped_invariant_sites);
  print_counter_line("candidate_source_productions_considered",
                     c.candidate_source_productions_considered);
  print_counter_line("upward_path_iterator_steps",
                     c.upward_path_iterator_steps);
  print_counter_line("upward_paths_completed", c.upward_paths_completed);
  print_counter_line("path_pairs_considered", c.path_pairs_considered);
  print_counter_line("candidates_constructed", c.candidates_constructed);
  print_counter_line("candidates_pruned_before_construction",
                     c.candidates_pruned_before_construction);
  print_counter_line("candidates_pruned_after_construction",
                     c.candidates_pruned_after_construction);
  print_counter_line("candidates_generated_after_dedup",
                     c.candidates_generated_after_dedup);
  print_counter_line("candidates_pruned_root_or_trivial",
                     c.candidates_pruned_root_or_trivial);
  print_counter_line("candidates_pruned_moved_size",
                     c.candidates_pruned_moved_size);
  print_counter_line("candidates_pruned_target_size",
                     c.candidates_pruned_target_size);
  print_counter_line("candidates_pruned_overlap", c.candidates_pruned_overlap);
  print_counter_line("candidates_pruned_affected_estimate",
                     c.candidates_pruned_affected_estimate);
  print_counter_line("candidates_pruned_immediate_reversal",
                     c.candidates_pruned_immediate_reversal);
  print_counter_line("candidates_pruned_duplicate",
                     c.candidates_pruned_duplicate);
  print_counter_line("candidates_pruned_invalid", c.candidates_pruned_invalid);
  print_counter_line("candidate_cap_cutoffs", c.candidate_cap_cutoffs);
  print_counter_line("path_budget_cutoffs", c.path_budget_cutoffs);
  print_counter_line("overlay_reachability_validations",
                     c.overlay_reachability_validations);
  print_counter_line("reachable_clades_traversed",
                     c.reachable_clades_traversed);
  print_counter_line("reachable_productions_traversed",
                     c.reachable_productions_traversed);
  print_counter_line("reachable_temp_clades_traversed",
                     c.reachable_temp_clades_traversed);
  print_counter_line("reachable_temp_productions_traversed",
                     c.reachable_temp_productions_traversed);
  print_counter_line("reachability_full_grammar_like_passes",
                     c.reachability_full_grammar_like_passes);
  std::println();
  std::println("summary_state_rebuilds:");
  print_counter_line("initial_search_state_rebuilds",
                     s.initial_search_state_rebuilds);
  print_counter_line("full_search_state_rebuilds",
                     s.full_search_state_rebuilds);
  print_counter_line("final_compaction_rebuilds",
                     s.final_compaction_rebuilds);

  return 0;
}

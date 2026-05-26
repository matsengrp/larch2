#include <larch/chart_spr_search.hpp>
#include <larch/rank3_rewrite.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {
namespace {

double chart_spr_elapsed_ms(std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point stop) {
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

void validate_chart_spr_search_loop_options(
    chart_spr_search_options const& options) {
  validate_supported_chart_cache_options(options.cache);
  if (!options.materialize_accepted_moves) {
    throw std::runtime_error(
        "chart SPR search: fully unmaterialized accepted moves are not "
        "implemented yet; materialize_accepted_moves must remain true");
  }
  if (!options.rebuild_after_accept &&
      options.enumeration.source != chart_spr_candidate_source::grammar &&
      options.max_iterations > 1) {
    throw std::runtime_error(
        "chart SPR search: Phase-9 local accepted-state updates currently "
        "support multi-iteration search only with grammar-native candidate "
        "generation; sampled-tree/hybrid sources need a rebuilt/materialized "
        "DAG before the next iteration");
  }
}

bool chart_spr_rebuild_after_accept_needs_exact_trim(
    chart_spr_search_options const& options) {
  return options.acceptance_mode ==
         chart_spr_acceptance_mode::exact_multisite;
}

void chart_spr_add_search_state_rebuild_counters(
    chart_spr_search_counters& accumulated,
    chart_spr_search_counters const& rebuild_counters,
    bool update_current_skipped_invariant_sites) {
  accumulated.grammar_rebuilds += rebuild_counters.grammar_rebuilds;
  accumulated.pattern_rebuilds += rebuild_counters.pattern_rebuilds;
  accumulated.base_chart_cache_rebuilds +=
      rebuild_counters.base_chart_cache_rebuilds;
  accumulated.pattern_batch_cache_builds +=
      rebuild_counters.pattern_batch_cache_builds;
  if (update_current_skipped_invariant_sites) {
    accumulated.skipped_invariant_sites =
        rebuild_counters.skipped_invariant_sites;
  }
}

chart_spr_search_state rebuild_chart_spr_search_state_after_accept(
    chart_spr_search_state const& previous_state, phylo_dag& rebuilt_dag,
    clade_grammar rebuilt_grammar, chart_spr_search_options const& options,
    bool& reused_patterns) {
  validate_supported_chart_cache_options(options.cache);
  bool build_exact = chart_spr_rebuild_after_accept_needs_exact_trim(options);
  reused_patterns =
      !options.force_pattern_fingerprint_mismatch_for_tests &&
      chart_spr_pattern_source_fingerprint_matches(
          rebuilt_dag, rebuilt_grammar,
          previous_state.pattern_source_fingerprint);

  if (reused_patterns) {
    chart_spr_active_pattern_build_result active_build;
    active_build.active_patterns = previous_state.active_patterns;
    active_build.pattern_source_fingerprint =
        previous_state.pattern_source_fingerprint;
    active_build.invariant_constant_offset =
        previous_state.invariant_constant_offset;
    active_build.skipped_invariant_site_count =
        previous_state.skipped_invariant_site_count;
    return build_chart_spr_search_state_from_active(
        rebuilt_dag, std::move(rebuilt_grammar), std::move(active_build),
        options.chart, build_exact, options.exact_trim, options.cache);
  }

  auto active_build = make_active_search_patterns(
      rebuilt_dag, rebuilt_grammar, options.chart);
  auto state = build_chart_spr_search_state_from_active(
      rebuilt_dag, std::move(rebuilt_grammar), std::move(active_build),
      options.chart, build_exact, options.exact_trim, options.cache);
  ++state.counters.pattern_rebuilds;
  return state;
}

chart_spr_production_signature chart_spr_production_signature_for_id(
    clade_grammar const& grammar, production_id pid) {
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error(
        "chart SPR fixed-topology post-materialization check: production "
        "id out of range");
  }
  auto const& prod = grammar.productions[pid];
  chart_spr_production_signature signature;
  signature.parent_taxa = grammar.clades[prod.parent].taxa;
  std::sort(signature.parent_taxa.begin(), signature.parent_taxa.end());
  signature.child_taxa.reserve(prod.children.size());
  for (auto child : prod.children) {
    auto child_taxa = grammar.clades[child].taxa;
    std::sort(child_taxa.begin(), child_taxa.end());
    signature.child_taxa.push_back(std::move(child_taxa));
  }
  std::sort(signature.child_taxa.begin(), signature.child_taxa.end());
  return signature;
}

production_id chart_spr_find_unique_production_by_signature(
    clade_grammar const& grammar,
    chart_spr_production_signature const& signature) {
  production_id match = no_production;
  for (std::size_t i = 0; i < grammar.productions.size(); ++i) {
    auto pid = static_cast<production_id>(i);
    if (chart_spr_production_signature_for_id(grammar, pid) == signature) {
      if (match != no_production) {
        throw std::runtime_error(
            "chart SPR fixed-topology post-materialization check: selected "
            "production signature is ambiguous in rebuilt grammar");
      }
      match = pid;
    }
  }
  if (match == no_production) {
    throw std::runtime_error(
        "chart SPR fixed-topology post-materialization check: selected "
        "production signature is missing from rebuilt grammar");
  }
  return match;
}

std::uint64_t chart_spr_rebuilt_fixed_topology_score_with_invariants(
    chart_spr_search_state const& rebuilt_state,
    chart_spr_candidate_score const& accepted) {
  if (!accepted.topology_selection.certificate) {
    throw std::runtime_error(
        "chart SPR fixed-topology post-materialization check requires the "
        "accepted candidate's complete topology certificate");
  }
  auto const& certificate = *accepted.topology_selection.certificate;
  if (certificate.after_signatures.empty()) {
    throw std::runtime_error(
        "chart SPR fixed-topology post-materialization check requires "
        "after-topology production signatures");
  }

  rebuilt_state.active_patterns.assert_no_skipped_invariant_metadata();
  std::vector<production_id> rebuilt_after_ids;
  rebuilt_after_ids.reserve(certificate.after_signatures.size());
  for (auto const& signature : certificate.after_signatures) {
    rebuilt_after_ids.push_back(
        chart_spr_find_unique_production_by_signature(rebuilt_state.grammar,
                                                      signature));
  }
  auto topology = grammar_topology_from_productions(rebuilt_state.grammar,
                                                    rebuilt_after_ids);
  auto active_score = score_selected_topology(
      rebuilt_state.grammar, rebuilt_state.active_patterns.patterns, topology,
      rebuilt_state.chart_opts);
  return chart_spr_add_invariant_offset(
      active_score, rebuilt_state,
      "chart-SPR fixed-topology rebuilt-score invariant offset");
}

std::uint64_t chart_spr_post_materialization_objective_score(
    chart_spr_search_state const& rebuilt_state,
    chart_spr_search_options const& options,
    chart_spr_candidate_score const& accepted) {
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::lower_bound_heuristic) {
    return rebuilt_state.composite_lower_bound_with_invariants;
  }
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::fixed_topology_exact) {
    return chart_spr_rebuilt_fixed_topology_score_with_invariants(
        rebuilt_state, accepted);
  }
  return chart_spr_state_exact_score_with_invariants(rebuilt_state,
                                                     options.exact_trim);
}

rank3_option_b_result materialize_chart_spr_accepted_candidate(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& accepted) {
  if (state.dag == nullptr) {
    throw std::runtime_error(
        "chart SPR search: current state has no source DAG");
  }

  rank3_option_b_options option_b;
  try {
    return materialize_rank3_option_b(*state.dag, state.grammar,
                                      accepted.candidate, option_b);
  } catch (std::exception const& option_b_error) {
    rank3_option_a_options option_a;
    option_a.include_original_dag = option_b.include_original_dag;
    option_a.validate = option_b.validate;
    option_a.generated_edge_weight = option_b.added_edge_weight;
    option_a.require_intended_productions_present =
        option_b.require_intended_productions_present;
    option_a.rebuild_grammar_options = option_b.rebuild_grammar_options;
    try {
      auto fallback = materialize_rank3_option_a(
          *state.dag, state.grammar, accepted.candidate, option_a);
      rank3_option_b_result converted;
      converted.dag = std::move(fallback.dag);
      converted.rebuilt = std::move(fallback.rebuilt);
      converted.materialized_tree_count = fallback.materialized_tree_count;
      converted.staged_in_overlay = false;
      converted.used_source_tree_move = false;
      converted.intended_productions =
          std::move(fallback.intended_productions);
      converted.intended_production_present =
          std::move(fallback.intended_production_present);
      return converted;
    } catch (std::exception const& option_a_error) {
      throw std::runtime_error(
          std::string{"chart SPR search: accepted candidate materialization "
                      "failed with Option B ('"} +
          option_b_error.what() + "') and Option A ('" +
          option_a_error.what() + "')");
    }
  }
}

std::uint64_t chart_spr_active_score_from_full(
    std::uint64_t full_score, chart_spr_search_state const& state,
    std::string const& label) {
  if (full_score < state.invariant_constant_offset) {
    throw std::runtime_error(label +
                             ": full score is smaller than invariant offset");
  }
  return full_score - state.invariant_constant_offset;
}

pattern_chart_cache_entry chart_spr_cache_entry_from_chart(
    clade_grammar const& grammar, site_pattern const& pattern,
    chart_options const& chart_opts, single_site_chart chart) {
  pattern_chart_cache_entry entry;
  entry.chart = std::move(chart);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= entry.chart.inside.size()) {
    throw std::runtime_error(
        "chart SPR local accept update: root clade out of updated chart "
        "range");
  }
  entry.root_row = entry.chart.inside[grammar.root_clade];
  entry.root_min_excluding_ua =
      entry.chart.root_min_excluding_ua(grammar.root_clade);
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    entry.root_min_by_reference_state[reference_state] =
        entry.chart.root_min_with_reference_edge(grammar.root_clade,
                                                 reference_state);
    entry.reference_state_counts[reference_state] =
        pattern.reference_state_counts[reference_state];
  }
  entry.weighted_root_score =
      chart_multisite_detail::weighted_root_score_from_row(
          entry.root_row, pattern, chart_opts);
  return entry;
}

pattern_chart_cache_entry chart_spr_locally_updated_cache_entry(
    chart_spr_search_state const& state,
    overlay_materialization_result const& materialized,
    pattern_chart_cache_entry const& old_entry, site_pattern const& pattern,
    local_overlay_chart_rows const& rows) {
  single_site_chart updated_chart;
  updated_chart.inside.assign(materialized.grammar.clades.size(),
                              parsimony_chart_detail::make_inf_row());
  if (materialized.dense_clade_to_ref.size() !=
      materialized.grammar.clades.size()) {
    throw std::runtime_error(
        "chart SPR local accept update: dense clade/ref map size mismatch");
  }
  for (std::size_t dense = 0; dense < materialized.dense_clade_to_ref.size();
       ++dense) {
    auto ref = materialized.dense_clade_to_ref[dense];
    updated_chart.inside[dense] =
        local_overlay_chart_row(rows, old_entry.chart, ref);
  }
  return chart_spr_cache_entry_from_chart(materialized.grammar, pattern,
                                          state.chart_opts,
                                          std::move(updated_chart));
}

void chart_spr_append_materialized_temp_production_keys(
    overlay_materialization_result const& materialized,
    std::vector<rank3_production_taxa_key>& keys) {
  for (auto dense_pid : materialized.temp_production_to_dense) {
    if (dense_pid == no_production) continue;
    rank3_detail::append_unique_key(
        keys, rank3_detail::production_key_from_id(materialized.grammar,
                                                   dense_pid));
  }
}

void chart_spr_verify_local_accept_update_against_full(
    chart_spr_search_state const& updated_state,
    chart_spr_search_options const& options) {
  if (updated_state.dag == nullptr) return;
  chart_spr_active_pattern_build_result active_build;
  active_build.active_patterns = updated_state.active_patterns;
  active_build.pattern_source_fingerprint =
      updated_state.pattern_source_fingerprint;
  active_build.invariant_constant_offset =
      updated_state.invariant_constant_offset;
  active_build.skipped_invariant_site_count =
      updated_state.skipped_invariant_site_count;

  chart_cache_options all_cache;
  auto full_state = build_chart_spr_search_state_from_active(
      *updated_state.dag, updated_state.grammar, std::move(active_build),
      options.chart, false, options.exact_trim, all_cache);
  if (full_state.composite_lower_bound_with_invariants !=
      updated_state.composite_lower_bound_with_invariants) {
    throw std::runtime_error(
        "chart SPR local accept update: rebuilt full-cache lower bound does "
        "not match locally updated cache");
  }
  if (updated_state.cache_strategy !=
      chart_spr_cache_strategy::all_active_patterns) {
    return;
  }
  if (full_state.pattern_charts.size() !=
      updated_state.pattern_charts.size()) {
    throw std::runtime_error(
        "chart SPR local accept update: rebuilt pattern-cache size does not "
        "match locally updated cache");
  }
  for (std::size_t i = 0; i < full_state.pattern_charts.size(); ++i) {
    if (full_state.pattern_charts[i].chart.inside !=
        updated_state.pattern_charts[i].chart.inside) {
      throw std::runtime_error(
          "chart SPR local accept update: rebuilt chart rows do not match "
          "locally updated rows");
    }
  }
}

struct chart_spr_local_accept_update_result {
  chart_spr_search_state state;
  std::vector<rank3_production_taxa_key> accepted_temp_production_keys;
};

chart_spr_local_accept_update_result
chart_spr_update_search_state_after_accept_locally(
    chart_spr_search_state const& previous_state,
    chart_spr_candidate_score const& accepted,
    chart_spr_search_options const& options) {
  local_spr_score_options local_options;
  local_options.verify_against_full_overlay = false;
  local_options.validate_cached_chart_shapes = false;

  auto delta = build_spr_overlay_delta(previous_state.grammar,
                                       accepted.candidate, local_options);
  auto overlay = overlay_from_candidate(previous_state.grammar,
                                        accepted.candidate);
  // Phase-9 local-update mode avoids rebuilding the search sidecar after an
  // accepted move, but it does not yet keep a persistent base+overlay chain:
  // the accepted overlay is densely materialized into the next search grammar.
  auto materialized = materialize_overlay_grammar(overlay);

  chart_spr_local_accept_update_result result;
  chart_spr_append_materialized_temp_production_keys(
      materialized, result.accepted_temp_production_keys);

  auto& updated = result.state;
  updated.dag = previous_state.dag;
  updated.grammar = materialized.grammar;
  updated.active_patterns = previous_state.active_patterns;
  updated.pattern_source_fingerprint =
      previous_state.pattern_source_fingerprint;
  updated.invariant_constant_offset =
      previous_state.invariant_constant_offset;
  updated.skipped_invariant_site_count =
      previous_state.skipped_invariant_site_count;
  updated.chart_opts = previous_state.chart_opts;
  updated.cache_opts = previous_state.cache_opts;
  updated.estimated_full_pattern_cache_bytes =
      estimate_chart_spr_full_pattern_cache_bytes(updated.grammar,
                                                  updated.active_patterns);
  updated.effective_pattern_batch_size = choose_chart_spr_pattern_batch_size(
      updated.grammar, updated.active_patterns, updated.cache_opts);
  updated.cache_strategy = choose_chart_spr_cache_strategy(
      updated.grammar, updated.active_patterns, updated.cache_opts);
  if (previous_state.cache_strategy ==
      chart_spr_cache_strategy::pattern_batches) {
    // There are no resident base rows to update in place.  Keep the state in
    // pattern-batch mode so the next scoring batch builds base rows once per
    // pattern batch for the locally updated grammar rather than forcing a full
    // accepted-state cache rebuild.
    updated.cache_strategy = chart_spr_cache_strategy::pattern_batches;
  }
  updated.effective_candidate_batch_size =
      previous_state.effective_candidate_batch_size;
  updated.counters = previous_state.counters;
  ++updated.counters.full_overlay_materializations;
  ++updated.counters.overlay_materializations_for_accept_materialization;
  chart_spr_search_detail::record_overlay_delta_reachability_counters(
      updated.counters, delta.reachability_stats);

  auto accepted_active_lower_bound = chart_spr_active_score_from_full(
      accepted.lower_bound.value.new_score, previous_state,
      "chart SPR local accept update lower-bound score");

  std::uint64_t active_total = accepted_active_lower_bound;
  if (previous_state.cache_strategy ==
      chart_spr_cache_strategy::all_active_patterns) {
    if (previous_state.pattern_charts.size() !=
        previous_state.active_patterns.patterns.patterns.size()) {
      throw std::runtime_error(
          "chart SPR local accept update: resident pattern chart cache size "
          "mismatch");
    }

    std::vector<pattern_chart_cache_entry> updated_entries;
    if (updated.cache_strategy ==
        chart_spr_cache_strategy::all_active_patterns) {
      updated_entries.reserve(previous_state.pattern_charts.size());
    }
    active_total = 0;
    chart_spr_local_score_scratch scratch;
    auto chart_build_options = previous_state.chart_opts;
    chart_build_options.keep_trace = false;
    chart_build_options.max_trace_choices = 0;
    auto const& patterns = previous_state.active_patterns.patterns.patterns;
    for (std::size_t i = 0; i < patterns.size(); ++i) {
      auto const& pattern = patterns[i];
      leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
      build_local_overlay_chart_rows_into(
          delta, previous_state.pattern_charts[i].chart, states,
          scratch.rows, chart_build_options, false);
      updated.counters.local_rows_recomputed +=
          delta.affected_order.size();
      auto entry = chart_spr_locally_updated_cache_entry(
          previous_state, materialized, previous_state.pattern_charts[i],
          pattern, scratch.rows);
      active_total = chart_multisite_detail::checked_add_u64(
          active_total, entry.weighted_root_score,
          "chart-SPR local accept updated active lower bound");
      if (updated.cache_strategy ==
          chart_spr_cache_strategy::all_active_patterns) {
        updated_entries.push_back(std::move(entry));
      }
    }
    if (active_total != accepted_active_lower_bound) {
      throw std::runtime_error(
          "chart SPR local accept update: updated cache lower bound does not "
          "match the accepted candidate's local score");
    }
    updated.pattern_charts = std::move(updated_entries);
  }

  updated.composite_lower_bound_without_invariants = active_total;
  updated.composite_lower_bound_with_invariants =
      chart_spr_add_invariant_offset(
          active_total, updated,
          "chart-SPR local accept updated invariant offset");
  if (updated.composite_lower_bound_with_invariants !=
      accepted.lower_bound.value.new_score) {
    throw std::runtime_error(
        "chart SPR local accept update: full lower bound does not match the "
        "accepted candidate score");
  }

  if (updated.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    updated.resident_pattern_cache_bytes =
        estimate_chart_spr_pattern_cache_bytes(updated);
  } else {
    updated.resident_pattern_cache_bytes =
        updated.effective_pattern_batch_size *
        estimate_chart_spr_pattern_row_cache_bytes(updated.grammar);
  }

  updated.exact_trim_active_only.reset();
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite &&
      accepted.exact) {
    multisite_trim_result trim;
    trim.optimum = chart_spr_active_score_from_full(
        accepted.exact->value.new_score, updated,
        "chart SPR local accept update exact score");
    trim.composite_lower_bound =
        updated.composite_lower_bound_without_invariants;
    trim.active_pattern_count =
        updated.active_patterns.patterns.patterns.size();
    updated.exact_trim_active_only = std::move(trim);
  }

  if (options.verify_local_against_full_for_tests) {
    chart_spr_verify_local_accept_update_against_full(updated, options);
  }
  return result;
}

std::uint64_t chart_spr_local_accept_update_post_score(
    chart_spr_search_state const& updated_state,
    chart_spr_search_options const& options,
    chart_spr_candidate_score const& accepted) {
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return updated_state.composite_lower_bound_with_invariants;
    case chart_spr_acceptance_mode::exact_multisite:
    case chart_spr_acceptance_mode::fixed_topology_exact:
      if (!accepted.exact) {
        throw std::runtime_error(
            "chart SPR local accept update: exact accepted score missing");
      }
      return accepted.exact->value.new_score;
  }
  return updated_state.composite_lower_bound_with_invariants;
}

std::vector<production_id> chart_spr_existing_productions_for_keys(
    clade_grammar const& grammar,
    std::vector<rank3_production_taxa_key> const& keys) {
  std::vector<production_id> ids;
  for (auto key : keys) {
    rank3_detail::normalize_production_key(key);
    for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
      if (rank3_detail::production_key_from_id(
              grammar, static_cast<production_id>(pid)) == key) {
        ids.push_back(static_cast<production_id>(pid));
        break;
      }
    }
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

rank3_topology chart_spr_fixed_topology_from_last_accept(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const* last_accepted) {
  if (last_accepted == nullptr || !last_accepted->topology_selection.certificate) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: fixed-topology "
        "mode requires the last accepted candidate's topology certificate");
  }
  auto const& certificate = *last_accepted->topology_selection.certificate;
  if (certificate.after_signatures.empty()) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: fixed-topology "
        "certificate has no after-topology signatures");
  }

  std::vector<production_id> after_ids;
  after_ids.reserve(certificate.after_signatures.size());
  for (auto const& signature : certificate.after_signatures) {
    after_ids.push_back(
        chart_spr_find_unique_production_by_signature(state.grammar,
                                                      signature));
  }
  auto topology = grammar_topology_from_productions(state.grammar, after_ids);
  (void)validate_grammar_topology(state.grammar, topology);
  return topology;
}

rank3_topology chart_spr_choose_local_update_compaction_topology(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    std::vector<rank3_production_taxa_key> const& preferred_keys,
    chart_spr_candidate_score const* last_accepted) {
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    multisite_topology_trace_options trace_options;
    trace_options.max_optimal_topologies = 1;
    trace_options.trim_options = options.exact_trim;
    auto trace = build_multisite_optimal_topologies(
        state.grammar, state.active_patterns.patterns, state.chart_opts,
        trace_options);
    if (trace.topologies.empty()) {
      throw std::runtime_error(
          "chart SPR local accepted-state final compaction: exact topology "
          "trace produced no topology");
    }
    return trace.topologies.front();
  }

  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::fixed_topology_exact) {
    return chart_spr_fixed_topology_from_last_accept(state, last_accepted);
  }

  auto preferred_ids = chart_spr_existing_productions_for_keys(
      state.grammar, preferred_keys);
  if (preferred_ids.empty()) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: no accepted "
        "temporary productions are available to choose a concrete topology");
  }
  return rank3_topology_preferring_productions(state.grammar, preferred_ids);
}

phylo_dag chart_spr_materialize_local_update_state_to_dag(
    phylo_dag& source, chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    std::vector<rank3_production_taxa_key> const& preferred_keys,
    chart_spr_candidate_score const* last_accepted) {
  auto topology = chart_spr_choose_local_update_compaction_topology(
      state, options, preferred_keys, last_accepted);
  // Current Phase-9 compaction is intentionally tree-valued: choose one
  // concrete topology, materialize it, then rebuild/check the objective.  This
  // is score-safe but does not preserve all productions/topologies present in
  // the locally updated accepted-state grammar.
  auto dag = materialize_rank3_tree_from_topology(
      source, state.grammar, topology, true,
      std::numeric_limits<float>::max());
  validate_dag(dag, "chart SPR local accepted-state compacted DAG",
               thread_pool::get_default());
  return dag;
}

std::uint64_t chart_spr_local_update_final_expected_score(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    chart_spr_candidate_score const* last_accepted) {
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::exact_multisite:
      return chart_spr_state_exact_score_with_invariants(state,
                                                         options.exact_trim);
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return state.composite_lower_bound_with_invariants;
    case chart_spr_acceptance_mode::fixed_topology_exact:
      if (last_accepted == nullptr || !last_accepted->exact) {
        throw std::runtime_error(
            "chart SPR local accepted-state final compaction: fixed-topology "
            "mode requires the last accepted exact score");
      }
      return last_accepted->exact->value.new_score;
  }
  return state.composite_lower_bound_with_invariants;
}

std::uint64_t chart_spr_local_update_final_rebuilt_score(
    chart_spr_search_state const& rebuilt_state,
    chart_spr_search_options const& options,
    chart_spr_candidate_score const* last_accepted) {
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::exact_multisite:
      return chart_spr_state_exact_score_with_invariants(rebuilt_state,
                                                         options.exact_trim);
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return rebuilt_state.composite_lower_bound_with_invariants;
    case chart_spr_acceptance_mode::fixed_topology_exact:
      if (last_accepted == nullptr) {
        throw std::runtime_error(
            "chart SPR local accepted-state final compaction: fixed-topology "
            "mode requires the last accepted candidate");
      }
      return chart_spr_rebuilt_fixed_topology_score_with_invariants(
          rebuilt_state, *last_accepted);
  }
  return rebuilt_state.composite_lower_bound_with_invariants;
}

struct chart_spr_local_update_compaction_gate_result {
  phylo_dag dag;
  chart_spr_search_state rebuilt_state;
  std::uint64_t rebuilt_score = 0;
  bool reused_patterns = false;
};

chart_spr_local_update_compaction_gate_result
chart_spr_compact_and_verify_local_update_state(
    phylo_dag& source, chart_spr_search_state const& local_state,
    chart_spr_search_options const& options,
    std::vector<rank3_production_taxa_key> const& preferred_keys,
    chart_spr_candidate_score const* last_accepted) {
  auto expected_score = chart_spr_local_update_final_expected_score(
      local_state, options, last_accepted);

  chart_spr_local_update_compaction_gate_result result;
  result.dag = chart_spr_materialize_local_update_state_to_dag(
      source, local_state, options, preferred_keys, last_accepted);
  auto rebuilt_grammar = build_clade_grammar(result.dag);
  result.rebuilt_state = rebuild_chart_spr_search_state_after_accept(
      local_state, result.dag, std::move(rebuilt_grammar), options,
      result.reused_patterns);
  result.rebuilt_score = chart_spr_local_update_final_rebuilt_score(
      result.rebuilt_state, options, last_accepted);
  if (options.override_final_compaction_rebuilt_score_for_tests) {
    result.rebuilt_score =
        *options.override_final_compaction_rebuilt_score_for_tests;
  }

  if (result.rebuilt_score != expected_score) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: rebuilt objective " +
        std::to_string(result.rebuilt_score) +
        " does not match local sidecar objective " +
        std::to_string(expected_score));
  }
  return result;
}

void chart_spr_refresh_search_summary_from_counters(
    chart_spr_search_summary& summary,
    chart_spr_search_counters const& counters) {
  summary.accepted_moves = counters.accepted_moves;
  summary.candidates_locally_scored = counters.local_candidate_scores;
  summary.local_rows_recomputed = counters.local_rows_recomputed;
  summary.candidate_batches_scored = counters.candidate_batches_scored;
  summary.pattern_batch_cache_builds = counters.pattern_batch_cache_builds;
  summary.exact_verifications = counters.exact_verifications;
  summary.overlay_materializations_for_exact_verification =
      counters.overlay_materializations_for_exact_verification;
  summary.overlay_materializations_for_accept_materialization =
      counters.overlay_materializations_for_accept_materialization;
  summary.sidecar_rebuilds_after_accept =
      counters.sidecar_rebuilds_after_accept;
  summary.candidate_accepts_attempted = counters.candidate_accepts_attempted;
  summary.post_materialization_rejections =
      counters.post_materialization_rejections;
  summary.full_search_state_rebuilds =
      summary.initial_search_state_rebuilds +
      summary.sidecar_rebuilds_after_accept;
}

}  // namespace

chart_spr_search_result run_chart_spr_search(
    phylo_dag initial_dag, clade_grammar initial_grammar,
    chart_spr_search_options options) {
  validate_chart_spr_search_loop_options(options);

  auto total_start = std::chrono::steady_clock::now();
  chart_spr_search_result result;
  result.dag = std::move(initial_dag);
  result.summary.acceptance_mode = options.acceptance_mode;
  result.summary.candidate_selection = options.candidate_selection;
  result.summary.initial_search_state_rebuilds = 1;

  auto cache_start = std::chrono::steady_clock::now();
  auto state = build_chart_spr_search_state(
      result.dag, std::move(initial_grammar), options);
  ++state.counters.grammar_rebuilds;
  result.summary.cache_build_ms = chart_spr_elapsed_ms(
      cache_start, std::chrono::steady_clock::now());
  result.summary.initial_score =
      chart_spr_iteration_state_score_before(state, options);
  result.summary.final_score = result.summary.initial_score;
  result.summary.active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  result.summary.initial_grammar_clade_count = state.grammar.clades.size();
  result.summary.initial_grammar_production_count =
      state.grammar.productions.size();
  result.summary.final_grammar_clade_count =
      result.summary.initial_grammar_clade_count;
  result.summary.final_grammar_production_count =
      result.summary.initial_grammar_production_count;
  result.summary.chart_cache_estimated_full_bytes =
      state.estimated_full_pattern_cache_bytes;
  result.summary.chart_cache_resident_bytes =
      state.resident_pattern_cache_bytes;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
  result.summary.local_score_worker_count =
      chart_spr_search_detail::normalize_chart_spr_worker_count(
          options.local_score_worker_count);
  std::vector<std::size_t> aggregate_affected_counts;
  std::vector<rank3_production_taxa_key> local_update_preferred_keys;
  std::optional<chart_spr_candidate_score> last_local_update_accepted;
  bool used_local_accept_updates = false;

  std::string immediate_reversal_key_to_skip;
  for (std::size_t iter = 0; iter < options.max_iterations; ++iter) {
    auto iteration_options = options;
    iteration_options.enumeration.immediate_reversal_candidate_key_to_skip =
        immediate_reversal_key_to_skip;
    auto iteration_seed = options.seed + static_cast<std::uint32_t>(iter);
    iteration_options.seed = iteration_seed;
    iteration_options.enumeration.seed = iteration_seed;
    auto iteration = run_chart_spr_acceptance_iteration(state,
                                                        iteration_options,
                                                        iter);
    result.summary.candidates_generated += iteration.candidates_generated;
    result.summary.local_scoring_ms += iteration.local_scoring_ms;
    result.summary.exact_verification_ms += iteration.exact_verification_ms;
    aggregate_affected_counts.insert(
        aggregate_affected_counts.end(), iteration.affected_clade_counts.begin(),
        iteration.affected_clade_counts.end());

    if (!iteration.accepted) {
      result.summary.final_score = iteration.state_score_after;
      result.iterations.push_back(std::move(iteration));
      break;
    }

    if (options.acceptance_mode ==
            chart_spr_acceptance_mode::fixed_topology_exact &&
        result.iterations.empty()) {
      // Fixed-topology exact mode scores the selected before/after topology
      // for each accepted candidate rather than a single grammar-wide state
      // optimum.  Once a move is selected, report the same selected-topology
      // convention for the summary's initial/final scores.
      result.summary.initial_score = iteration.state_score_before;
    }

    auto attempt_counters = state.counters;
    auto materialize_start = std::chrono::steady_clock::now();
    try {
      if (options.rebuild_after_accept) {
        auto materialized = materialize_chart_spr_accepted_candidate(
            state, *iteration.accepted);
        ++attempt_counters.full_overlay_materializations;
        ++attempt_counters.overlay_materializations_for_accept_materialization;
        ++attempt_counters.grammar_rebuilds;
        ++attempt_counters.sidecar_rebuilds_after_accept;

        bool reused_patterns = false;
        auto tentative_state = rebuild_chart_spr_search_state_after_accept(
            state, materialized.dag, std::move(materialized.rebuilt.grammar),
            options, reused_patterns);
        chart_spr_add_search_state_rebuild_counters(
            attempt_counters, tentative_state.counters, true);
        tentative_state.counters = attempt_counters;
        result.summary.accepted_rebuild_ms += chart_spr_elapsed_ms(
            materialize_start, std::chrono::steady_clock::now());

        auto check_start = std::chrono::steady_clock::now();
        auto rebuilt_score = chart_spr_post_materialization_objective_score(
            tentative_state, options, *iteration.accepted);
        if (options.override_post_materialization_rebuilt_score_for_tests) {
          rebuilt_score =
              *options.override_post_materialization_rebuilt_score_for_tests;
        }
        result.summary.post_materialization_check_ms += chart_spr_elapsed_ms(
            check_start, std::chrono::steady_clock::now());
        iteration.post_materialization_rebuilt_score = rebuilt_score;
        iteration.reused_patterns_after_accept = reused_patterns;

        if (rebuilt_score > iteration.state_score_before) {
          ++attempt_counters.post_materialization_rejections;
          state.counters = attempt_counters;
          iteration.post_materialization_rejected = true;
          iteration.accepted_move_committed = false;
          iteration.state_score_after = iteration.state_score_before;
          iteration.no_accept_reason =
              "post-materialization rebuilt objective worsened";
          iteration.post_materialization_rejection_reason =
              "rebuilt objective " + std::to_string(rebuilt_score) +
              " exceeds pre-accept objective " +
              std::to_string(iteration.state_score_before);
          result.summary.final_score = iteration.state_score_after;
          result.iterations.push_back(std::move(iteration));
          break;
        }

        immediate_reversal_key_to_skip =
            chart_spr_candidate_immediate_reverse_key(
                state.grammar, iteration.accepted->candidate);
        ++attempt_counters.accepted_moves;
        result.dag = std::move(materialized.dag);
        tentative_state.dag = &result.dag;
        tentative_state.counters = attempt_counters;
        state = std::move(tentative_state);
        iteration.accepted_move_committed = true;
        iteration.state_score_after = rebuilt_score;
        result.summary.final_score = rebuilt_score;
        result.iterations.push_back(std::move(iteration));
      } else {
        auto local_update = chart_spr_update_search_state_after_accept_locally(
            state, *iteration.accepted, options);
        auto tentative_state = std::move(local_update.state);
        attempt_counters = tentative_state.counters;
        result.summary.accepted_rebuild_ms += chart_spr_elapsed_ms(
            materialize_start, std::chrono::steady_clock::now());

        auto check_start = std::chrono::steady_clock::now();
        auto rebuilt_score = chart_spr_local_accept_update_post_score(
            tentative_state, options, *iteration.accepted);
        if (options.override_post_materialization_rebuilt_score_for_tests) {
          rebuilt_score =
              *options.override_post_materialization_rebuilt_score_for_tests;
        }
        result.summary.post_materialization_check_ms += chart_spr_elapsed_ms(
            check_start, std::chrono::steady_clock::now());
        iteration.post_materialization_rebuilt_score = rebuilt_score;
        iteration.reused_patterns_after_accept = true;

        if (rebuilt_score > iteration.state_score_before) {
          ++attempt_counters.post_materialization_rejections;
          state.counters = attempt_counters;
          iteration.post_materialization_rejected = true;
          iteration.accepted_move_committed = false;
          iteration.state_score_after = iteration.state_score_before;
          iteration.no_accept_reason =
              "local accepted-state update objective worsened";
          iteration.post_materialization_rejection_reason =
              "locally updated objective " + std::to_string(rebuilt_score) +
              " exceeds pre-accept objective " +
              std::to_string(iteration.state_score_before);
          result.summary.final_score = iteration.state_score_after;
          result.iterations.push_back(std::move(iteration));
          break;
        }

        immediate_reversal_key_to_skip =
            chart_spr_candidate_immediate_reverse_key(
                state.grammar, iteration.accepted->candidate);
        for (auto const& key : local_update.accepted_temp_production_keys) {
          rank3_detail::append_unique_key(local_update_preferred_keys, key);
        }
        ++attempt_counters.accepted_moves;
        tentative_state.dag = &result.dag;
        tentative_state.counters = attempt_counters;
        state = std::move(tentative_state);
        last_local_update_accepted = *iteration.accepted;
        used_local_accept_updates = true;
        iteration.accepted_move_committed = true;
        iteration.state_score_after = rebuilt_score;
        result.summary.final_score = rebuilt_score;
        result.iterations.push_back(std::move(iteration));
      }
    } catch (std::exception const& e) {
      result.summary.accepted_rebuild_ms += chart_spr_elapsed_ms(
          materialize_start, std::chrono::steady_clock::now());
      ++attempt_counters.post_materialization_rejections;
      state.counters = attempt_counters;
      iteration.post_materialization_rejected = true;
      iteration.accepted_move_committed = false;
      iteration.state_score_after = iteration.state_score_before;
      iteration.no_accept_reason =
          "accepted candidate failed materialization/rebuild";
      iteration.post_materialization_rejection_reason = e.what();
      result.summary.final_score = iteration.state_score_after;
      result.iterations.push_back(std::move(iteration));
      break;
    }
  }

  if (used_local_accept_updates) {
    auto compact_start = std::chrono::steady_clock::now();
    auto preserved_counters = state.counters;
    auto compacted = chart_spr_compact_and_verify_local_update_state(
        result.dag, state, options, local_update_preferred_keys,
        last_local_update_accepted ? &*last_local_update_accepted : nullptr);
    result.dag = std::move(compacted.dag);
    compacted.rebuilt_state.dag = &result.dag;
    compacted.rebuilt_state.counters = preserved_counters;
    state = std::move(compacted.rebuilt_state);
    result.summary.final_score = compacted.rebuilt_score;
    result.summary.final_compaction_rebuilds = 1;
    result.summary.final_compaction_ms += chart_spr_elapsed_ms(
        compact_start, std::chrono::steady_clock::now());
  }

  result.counters = state.counters;
  result.summary.iterations = result.iterations.size();
  chart_spr_refresh_search_summary_from_counters(result.summary,
                                                 result.counters);
  result.summary.active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  result.summary.final_grammar_clade_count = state.grammar.clades.size();
  result.summary.final_grammar_production_count =
      state.grammar.productions.size();
  result.summary.chart_cache_estimated_full_bytes =
      state.estimated_full_pattern_cache_bytes;
  result.summary.chart_cache_resident_bytes =
      state.resident_pattern_cache_bytes;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
  result.summary.total_ms = chart_spr_elapsed_ms(
      total_start, std::chrono::steady_clock::now());
  result.summary.effective_candidate_batch_size =
      state.effective_candidate_batch_size;
  if (result.summary.local_scoring_ms > 0.0) {
    auto seconds = result.summary.local_scoring_ms / 1000.0;
    result.summary.local_candidates_per_second =
        static_cast<double>(result.summary.candidates_locally_scored) /
        seconds;
    result.summary.local_rows_recomputed_per_second =
        static_cast<double>(result.summary.local_rows_recomputed) / seconds;
  }
  result.summary.affected_distribution =
      summarize_affected_clade_counts(std::move(aggregate_affected_counts));
  return result;
}

}  // namespace larch

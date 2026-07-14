#include <larch/chart_spr_search.hpp>
#include <larch/chart_two_chart_oracle.hpp>
#include <larch/inside_chart_cache.hpp>
#include <larch/outside_chart_cache.hpp>
#include <larch/overlay_chain.hpp>
#include <larch/overlay_chain_compaction.hpp>
#include <larch/phase10_report.hpp>
#include <larch/rank3_rewrite.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
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
        "chart SPR search: Phase-4 local commit currently supports "
        "multi-iteration search only with grammar-native candidate "
        "generation; sampled-tree/hybrid sources need a rebuilt/materialized "
        "DAG before the next iteration");
  }
  // Phase 4 local-commit gate enforcement (Work item 1 exactness contract):
  // a local commit is only admitted under an EXACT acceptance gate.  Admitting
  // `lower_bound_heuristic`-gated accepts would carry a chain whose recorded
  // objective is a lower bound only -- the deferred-verification extension,
  // which is explicitly out of the initial scope and must never be a silent
  // choice.  The labelled throw below fires BEFORE the first accept so a
  // misconfigured run performs zero commits.
  if (!options.rebuild_after_accept && options.acceptance_mode ==
                                           chart_spr_acceptance_mode::
                                               lower_bound_heuristic) {
    throw std::runtime_error(
        "chart SPR search: local commit (rebuild_after_accept = false) under "
        "the lower_bound_heuristic gate is not supported; a locally-committed "
        "chain's recorded objective must be exact (fixed_topology_exact or "
        "exact_multisite).  The deferred-verification extension admitting "
        "heuristic-gated local commits is out of scope.");
  }
  // Phase 10 commit-mode label enforcement (no silent fallback).  The search
  // loop's candidate generator always produces SPR overlay-delta commits, so a
  // run configured with commit_mode == option_c cannot be honored: accepting a
  // move would append an SPR overlay delta while the report claims
  // commit_mode: option_c, a direct contradiction.  Option-C commits are
  // reachable only through the library API (option_c_commit_via_chain), which
  // is the documented path and is exercised by option_c_chain_commit_test.
  // Throwing here keeps `option_c` as a parseable, distinctly-labelled library
  // mode without letting the search loop silently misreport it.
  if (options.commit_mode == chart_spr_commit_mode::option_c) {
    throw std::runtime_error(
        "chart SPR search: commit_mode == option_c is not supported by the "
        "search loop; the candidate generator always produces SPR overlay-delta "
        "commits.  Option-C commits are reachable only through the library API "
        "(option_c_commit_via_chain).  This is a labelled unsupported-mode "
        "throw, not a silent fallback to overlay_delta.");
  }
  if (!options.rebuild_after_accept && options.chart.score_ua_edge) {
    throw std::runtime_error(
        "chart SPR search: local commit (rebuild_after_accept = false) with "
        "score_ua_edge=true is a Phase 4 limitation: the persistent outside "
        "cache needs a documented per-pattern reference-state convention; use "
        "rebuild_after_accept=true or score_ua_edge=false.  This is a labelled "
        "unsupported-mode throw, not a silent fallback.");
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
  accumulated.chart_execution_plan_builds +=
      rebuild_counters.chart_execution_plan_builds;
  accumulated.chart_execution_plan_cache_hits +=
      rebuild_counters.chart_execution_plan_cache_hits;
  accumulated.candidate_execution_plan_builds +=
      rebuild_counters.candidate_execution_plan_builds;
  accumulated.candidate_execution_plan_cache_hits +=
      rebuild_counters.candidate_execution_plan_cache_hits;
  accumulated.full_grammar_validations +=
      rebuild_counters.full_grammar_validations;
  accumulated.production_index_validations +=
      rebuild_counters.production_index_validations;
  accumulated.production_partition_validations +=
      rebuild_counters.production_partition_validations;
  accumulated.dynamic_overlay_payload_partition_validations +=
      rebuild_counters.dynamic_overlay_payload_partition_validations;
  accumulated.candidate_partition_validations +=
      rebuild_counters.candidate_partition_validations;
  accumulated.clade_order_sorts += rebuild_counters.clade_order_sorts;
  accumulated.production_descriptors_compiled +=
      rebuild_counters.production_descriptors_compiled;
  accumulated.plan_mismatch_rejections +=
      rebuild_counters.plan_mismatch_rejections;
  accumulated.candidate_pattern_full_grammar_validations +=
      rebuild_counters.candidate_pattern_full_grammar_validations;
  accumulated.candidate_pattern_partition_validations +=
      rebuild_counters.candidate_pattern_partition_validations;
  accumulated.candidate_pattern_clade_order_sorts +=
      rebuild_counters.candidate_pattern_clade_order_sorts;
  accumulated.multifurcation_productions_scored +=
      rebuild_counters.multifurcation_productions_scored;
  accumulated.pattern_batch_cache_builds +=
      rebuild_counters.pattern_batch_cache_builds;
  accumulated.initial_state_inside_charts_built +=
      rebuild_counters.initial_state_inside_charts_built;
  accumulated.inside_cache_inside_charts_built +=
      rebuild_counters.inside_cache_inside_charts_built;
  accumulated.inside_cache_resident_inside_charts_consumed +=
      rebuild_counters.inside_cache_resident_inside_charts_consumed;
  accumulated.exact_setup_builds += rebuild_counters.exact_setup_builds;
  accumulated.exact_setup_inside_charts_built +=
      rebuild_counters.exact_setup_inside_charts_built;
  accumulated.exact_setup_resident_inside_charts_consumed +=
      rebuild_counters.exact_setup_resident_inside_charts_consumed;
  accumulated.exact_setup_active_leaf_state_vectors_copied +=
      rebuild_counters.exact_setup_active_leaf_state_vectors_copied;
  accumulated.exact_setup_active_leaf_states_copied +=
      rebuild_counters.exact_setup_active_leaf_states_copied;
  accumulated.exact_setup_outside_boundary_charts_built +=
      rebuild_counters.exact_setup_outside_boundary_charts_built;
  accumulated.exact_setup_upper_bound_topologies_generated +=
      rebuild_counters.exact_setup_upper_bound_topologies_generated;
  accumulated.exact_setup_upper_bound_topologies_unique +=
      rebuild_counters.exact_setup_upper_bound_topologies_unique;
  accumulated.exact_setup_frontier_passes +=
      rebuild_counters.exact_setup_frontier_passes;
  accumulated.exact_trim_lazy_chart_uses +=
      rebuild_counters.exact_trim_lazy_chart_uses;
  accumulated.outside_cache_inside_charts_built +=
      rebuild_counters.outside_cache_inside_charts_built;
  accumulated.outside_cache_inside_charts_reused +=
      rebuild_counters.outside_cache_inside_charts_reused;
  accumulated.outside_cache_outside_charts_built +=
      rebuild_counters.outside_cache_outside_charts_built;
  accumulated.lazy_inside_rows_computed +=
      rebuild_counters.lazy_inside_rows_computed;
  accumulated.lazy_outside_rows_computed +=
      rebuild_counters.lazy_outside_rows_computed;
  accumulated.lazy_patterns_merged_max = std::max(
      accumulated.lazy_patterns_merged_max,
      rebuild_counters.lazy_patterns_merged_max);
  accumulated.lazy_remerge_collisions +=
      rebuild_counters.lazy_remerge_collisions;
  accumulated.lazy_structural_class_count_max = std::max(
      accumulated.lazy_structural_class_count_max,
      rebuild_counters.lazy_structural_class_count_max);
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
    auto state = build_chart_spr_search_state_from_active(
        rebuilt_dag, std::move(rebuilt_grammar), std::move(active_build),
        options.chart, build_exact, options.exact_trim, options.cache);
    state.exact_verifier_concurrency =
        previous_state.exact_verifier_concurrency;
    return state;
  }

  auto active_build = make_active_search_patterns(
      rebuilt_dag, rebuilt_grammar, options.chart);
  auto state = build_chart_spr_search_state_from_active(
      rebuilt_dag, std::move(rebuilt_grammar), std::move(active_build),
      options.chart, build_exact, options.exact_trim, options.cache);
  state.exact_verifier_concurrency = previous_state.exact_verifier_concurrency;
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
  if (chart_spr_search_detail::chart_spr_grammar_has_multifurcation(
          state.grammar) ||
      chart_spr_detail::grammar_spr_candidate_involves_multifurcation(
          state.grammar, accepted.candidate)) {
    option_b.rebuild_grammar_options.allow_polytomies = true;
  }
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
  entry.weighted_root_score = chart_spr_weighted_root_score_from_row(
      entry.root_row, pattern, chart_opts);
  return entry;
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

rank3_topology chart_spr_topology_from_certificate_after_signatures(
    chart_spr_search_state const& state,
    chart_spr_topology_certificate const& certificate,
    std::string const& context) {
  if (certificate.after_signatures.empty()) {
    throw std::runtime_error(context +
                             ": topology certificate has no after-topology "
                             "signatures");
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

rank3_topology chart_spr_fixed_topology_from_last_accept(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const* last_accepted) {
  if (last_accepted == nullptr || !last_accepted->topology_selection.certificate) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: fixed-topology "
        "mode requires the last accepted candidate's topology certificate");
  }
  return chart_spr_topology_from_certificate_after_signatures(
      state, *last_accepted->topology_selection.certificate,
      "chart SPR local accepted-state final compaction: fixed-topology "
      "last accepted certificate");
}

rank3_production_taxa_key chart_spr_key_from_production_signature(
    chart_spr_production_signature signature) {
  rank3_production_taxa_key key;
  key.parent = std::move(signature.parent_taxa);
  key.children = std::move(signature.child_taxa);
  rank3_detail::normalize_production_key(key);
  return key;
}

std::vector<rank3_production_taxa_key>
chart_spr_topology_certificate_after_key_set(
    chart_spr_topology_certificate const& certificate) {
  if (certificate.after_signatures.empty()) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: accepted topology "
        "certificate has no after-topology signatures");
  }
  std::vector<rank3_production_taxa_key> keys;
  keys.reserve(certificate.after_signatures.size());
  for (auto const& signature : certificate.after_signatures) {
    rank3_detail::append_unique_key(
        keys, chart_spr_key_from_production_signature(signature));
  }
  return keys;
}

std::uint64_t chart_spr_score_rebuilt_topology_key_set_with_invariants(
    clade_grammar const& grammar, active_site_pattern_set const& active,
    chart_options const& chart_opts, std::uint64_t invariant_offset,
    std::vector<rank3_production_taxa_key> const& key_set,
    std::string const& context) {
  std::vector<production_id> pids;
  pids.reserve(key_set.size());
  for (auto key : key_set) {
    pids.push_back(overlay_chain_compaction_detail::find_dense_production_by_key(
        grammar, std::move(key), context));
  }
  auto topology = rank3_topology_from_productions(grammar, pids);
  (void)rank3_detail::validate_topology(grammar, topology);

  std::uint64_t active_total = 0;
  for (auto const& pattern : active.patterns.patterns) {
    auto row = chart_multisite_detail::restricted_topology_row(
        grammar, pattern, topology);
    auto score = chart_spr_weighted_root_score_from_row(row, pattern,
                                                        chart_opts);
    active_total = chart_multisite_detail::checked_add_u64(
        active_total, score,
        context + " selected-topology active score");
  }
  return chart_multisite_detail::checked_add_u64(
      active_total, invariant_offset,
      context + " selected-topology invariant offset");
}

rank3_topology chart_spr_choose_local_update_compaction_topology(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    std::vector<rank3_production_taxa_key> const& preferred_keys,
    chart_spr_candidate_score const* last_accepted) {
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
    multisite_topology_trace_options trace_options;
    trace_options.max_optimal_topologies = 1;
    trace_options.trim_options =
        overlay_chain_compaction_trace_trim_options(options.exact_trim);
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

std::vector<rank3_topology>
chart_spr_collect_local_update_compaction_witness_topologies(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    chart_spr_candidate_score const* last_accepted) {
  std::vector<rank3_topology> witnesses;

  // Keep the final exact-objective witness used by the grammar-valued safety
  // check.  For exact_multisite this is a deterministic B&B optimum of the
  // final chain tip; for fixed_topology_exact it is the last accepted
  // certificate resolved in the final chain tip when possible.  Every
  // historical accepted topology (fixed-topology certificate or exact-multisite
  // B&B witness captured at accept time) is passed separately as a stable
  // production-key set because later deltas may tombstone productions it used,
  // making it intentionally unresolvable in the final chain grammar until
  // compaction augments the materialization grammar.
  auto final_witness = chart_spr_choose_local_update_compaction_topology(
      state, options, {}, last_accepted);
  overlay_chain_compaction_detail::append_unique_topology(
      witnesses, state.grammar, std::move(final_witness));

  return witnesses;
}

struct chart_spr_recorded_chain_objective {
  std::uint64_t value = 0;
  chart_spr_score_kind kind = chart_spr_score_kind::composite_lower_bound;
  chart_spr_score_convention convention =
      chart_spr_score_convention::full_with_invariants;
  std::uint64_t invariant_offset_applied = 0;
};

void chart_spr_validate_recorded_chain_objective_kind(
    chart_spr_search_options const& options,
    chart_spr_recorded_chain_objective const& recorded,
    std::string const& context) {
  if (recorded.convention !=
      chart_spr_score_convention::full_with_invariants) {
    throw std::runtime_error(
        context + ": recorded chain objective is not full_with_invariants");
  }
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::exact_multisite:
      if (recorded.kind != chart_spr_score_kind::grammar_exact) {
        throw std::runtime_error(
            context + ": exact_multisite recorded objective is not "
                      "grammar_exact");
      }
      return;
    case chart_spr_acceptance_mode::fixed_topology_exact:
      if (recorded.kind != chart_spr_score_kind::fixed_topology_exact) {
        throw std::runtime_error(
            context + ": fixed_topology_exact recorded objective has the "
                      "wrong exactness label");
      }
      return;
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      if (recorded.kind != chart_spr_score_kind::composite_lower_bound) {
        throw std::runtime_error(
            context + ": lower_bound_heuristic recorded objective has the "
                      "wrong label");
      }
      return;
  }
}

chart_spr_recorded_chain_objective
chart_spr_recorded_chain_objective_from_accept(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    chart_spr_candidate_score const& accepted) {
  if (!accepted.exact) {
    throw std::runtime_error(
        "chart SPR local commit: exact accepted score missing while recording "
        "the chain objective");
  }
  chart_spr_recorded_chain_objective recorded;
  recorded.value = accepted.exact->value.new_score;
  recorded.kind = accepted.exact->kind;
  recorded.convention = accepted.exact->convention;
  recorded.invariant_offset_applied =
      accepted.exact->invariant_offset_applied;
  chart_spr_validate_recorded_chain_objective_kind(
      options, recorded, "chart SPR local commit");
  if (recorded.invariant_offset_applied != state.invariant_constant_offset) {
    throw std::runtime_error(
        "chart SPR local commit: recorded chain objective used a different "
        "invariant offset than the committed state");
  }
  if (options.override_local_commit_recorded_objective_for_tests) {
    recorded.value = *options.override_local_commit_recorded_objective_for_tests;
  }
  return recorded;
}

std::uint64_t chart_spr_local_update_final_expected_score(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    chart_spr_recorded_chain_objective const* recorded_objective) {
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::exact_multisite:
    case chart_spr_acceptance_mode::fixed_topology_exact:
      if (recorded_objective == nullptr) {
        throw std::runtime_error(
            "chart SPR local accepted-state final compaction: exact local "
            "commit requires the recorded chain objective from the last "
            "accepted move");
      }
      chart_spr_validate_recorded_chain_objective_kind(
          options, *recorded_objective,
          "chart SPR local accepted-state final compaction");
      (void)state;
      return recorded_objective->value;
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      return state.composite_lower_bound_with_invariants;
  }
  return state.composite_lower_bound_with_invariants;
}

void chart_spr_check_exact_multisite_recorded_objective_diagnostic(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    chart_spr_recorded_chain_objective const* recorded_objective) {
  if (options.acceptance_mode != chart_spr_acceptance_mode::exact_multisite) {
    return;
  }
  if (recorded_objective == nullptr) return;
  auto fresh_score = chart_spr_state_exact_score_with_invariants(
      state, options.exact_trim);
  if (fresh_score != recorded_objective->value) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: recorded exact "
        "chain objective " +
        std::to_string(recorded_objective->value) +
        " disagrees with a fresh exact diagnostic recomputation on the "
        "chain tip " + std::to_string(fresh_score) +
        "; the output-DAG oracle is checked against the recorded objective, "
        "not this diagnostic value");
  }
}

std::vector<rank3_production_taxa_key>
chart_spr_collect_accepted_topology_key_set_after_local_commit(
    chart_spr_search_state const& state,
    chart_spr_search_options const& options,
    chart_spr_candidate_score const& accepted) {
  switch (options.acceptance_mode) {
    case chart_spr_acceptance_mode::fixed_topology_exact:
      if (!accepted.topology_selection.certificate) {
        throw std::runtime_error(
            "chart SPR local commit: fixed-topology accept is missing its "
            "topology certificate witness");
      }
      return chart_spr_topology_certificate_after_key_set(
          *accepted.topology_selection.certificate);
    case chart_spr_acceptance_mode::exact_multisite: {
      if (!accepted.exact ||
          accepted.exact->kind != chart_spr_score_kind::grammar_exact) {
        throw std::runtime_error(
            "chart SPR local commit: exact_multisite accept is missing its "
            "grammar_exact recorded score while collecting a topology "
            "witness");
      }
      multisite_topology_trace_options trace_options;
      trace_options.max_optimal_topologies = 1;
      trace_options.trim_options =
          overlay_chain_compaction_trace_trim_options(options.exact_trim);
      auto trace = build_multisite_optimal_topologies(
          state.grammar, state.active_patterns.patterns, state.chart_opts,
          trace_options);
      if (trace.topologies.empty()) {
        throw std::runtime_error(
            "chart SPR local commit: exact_multisite topology witness trace "
            "produced no topology");
      }
      auto trace_full = chart_spr_add_invariant_offset(
          trace.optimum, state,
          "chart-SPR exact_multisite accepted topology witness invariant "
          "offset");
      if (trace_full != accepted.exact->value.new_score) {
        throw std::runtime_error(
            "chart SPR local commit: exact_multisite accepted topology "
            "witness score " +
            std::to_string(trace_full) +
            " disagrees with the accepted recorded exact score " +
            std::to_string(accepted.exact->value.new_score));
      }
      return overlay_chain_compaction_detail::topology_production_keys(
          state.grammar, trace.topologies.front());
    }
    case chart_spr_acceptance_mode::lower_bound_heuristic:
      throw std::runtime_error(
          "chart SPR local commit: lower_bound_heuristic accepts do not have "
          "an exact topology witness");
  }
  return {};
}

struct chart_spr_local_update_compaction_gate_result {
  phylo_dag dag;
  chart_spr_search_state rebuilt_state;
  std::uint64_t rebuilt_score = 0;
  multisite_keep_mask_kind exactness_kind = multisite_keep_mask_kind::none;
  bool reused_patterns = false;
  std::size_t materialized_tree_count = 0;
};

chart_spr_local_update_compaction_gate_result
chart_spr_compact_and_verify_local_update_state(
    phylo_dag& source, overlay_chain const& chain,
    chart_spr_search_state const& local_state,
    chart_spr_search_options const& options,
    std::vector<std::vector<rank3_production_taxa_key>> const&
        accepted_topology_key_sets,
    chart_spr_candidate_score const* last_accepted,
    chart_spr_recorded_chain_objective const* recorded_objective,
    chart_spr_search_counters& counters) {
  auto expected_score = chart_spr_local_update_final_expected_score(
      local_state, options, recorded_objective);
  chart_spr_check_exact_multisite_recorded_objective_diagnostic(
      local_state, options, recorded_objective);

  overlay_chain_compaction_options compaction_options;
  compaction_options.validate = true;
  compaction_options.generated_edge_weight =
      std::numeric_limits<float>::max();
  if (chart_spr_search_detail::chart_spr_grammar_has_multifurcation(
          local_state.grammar)) {
    compaction_options.rebuild_grammar_options.allow_polytomies = true;
  }
  compaction_options.witness_topologies =
      chart_spr_collect_local_update_compaction_witness_topologies(
          local_state, options, last_accepted);
  compaction_options.witness_topology_key_sets =
      accepted_topology_key_sets;

  ++counters.full_overlay_materializations;
  ++counters.overlay_materializations_for_final_compaction;
  auto compacted = compact_overlay_chain_to_dag(source, chain,
                                                compaction_options);
  counters.materialization_final_compaction_ms +=
      compacted.materialization_ms;
  if (!compacted.all_witness_topologies_present()) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: compacted output "
        "DAG failed to preserve every accepted topology witness");
  }

  chart_spr_local_update_compaction_gate_result result;
  result.materialized_tree_count = compacted.materialized_tree_count;

  auto multifurcating_output =
      chart_spr_search_detail::chart_spr_grammar_has_multifurcation(
          compacted.rebuilt.grammar);
  if (options.acceptance_mode ==
          chart_spr_acceptance_mode::fixed_topology_exact &&
      multifurcating_output) {
    // WI6 gates grammar-exact multisite scoring on multifurcating grammars.
    // For fixed_topology_exact local commits, the chain objective is already
    // the verified selected-topology score and compaction above preserves that
    // complete witness topology in the output DAG.  Do not route the final
    // check through the binary-only grammar-exact trim path.
    if (accepted_topology_key_sets.empty()) {
      throw std::runtime_error(
          "chart SPR local accepted-state final compaction: fixed-topology "
          "multifurcation output is missing an accepted topology witness");
    }
    result.rebuilt_score =
        chart_spr_score_rebuilt_topology_key_set_with_invariants(
            compacted.rebuilt.grammar, local_state.active_patterns,
            local_state.chart_opts, local_state.invariant_constant_offset,
            accepted_topology_key_sets.back(),
            "chart SPR local accepted-state final compaction");
    if (result.rebuilt_score != expected_score) {
      throw std::runtime_error(
          "chart SPR local accepted-state final compaction: rebuilt "
          "fixed-topology witness score " +
          std::to_string(result.rebuilt_score) +
          " does not match chain recorded objective " +
          std::to_string(expected_score));
    }
    result.exactness_kind = multisite_keep_mask_kind::none;
  } else {
    auto oracle = grammar_level_exact_parsimony(
        compacted.rebuilt.grammar, local_state.active_patterns,
        local_state.chart_opts, local_state.invariant_constant_offset,
        options.exact_trim);
    record_multisite_exact_trim_work(counters, oracle.trim);
    result.rebuilt_score = oracle.value;
    result.exactness_kind = oracle.exactness_kind;

    if (result.rebuilt_score > expected_score) {
      throw std::runtime_error(
          "chart SPR local accepted-state final compaction: grammar-level "
          "output DAG optimum " +
          std::to_string(result.rebuilt_score) +
          " exceeds chain recorded objective " +
          std::to_string(expected_score));
    }
    if (options.acceptance_mode ==
            chart_spr_acceptance_mode::exact_multisite &&
        result.rebuilt_score != expected_score) {
      throw std::runtime_error(
          "chart SPR local accepted-state final compaction: grammar-level "
          "output DAG optimum " +
          std::to_string(result.rebuilt_score) +
          " does not match exact chain objective " +
          std::to_string(expected_score));
    }
  }

  result.dag = std::move(compacted.dag);
  result.rebuilt_state = rebuild_chart_spr_search_state_after_accept(
      local_state, result.dag, std::move(compacted.rebuilt.grammar), options,
      result.reused_patterns);
  // The rebuilt state is moved into the result after this gate, but its local
  // plan/chart-cache construction counters must first be folded into the
  // cumulative snapshot.  The caller subsequently installs that cumulative
  // snapshot on rebuilt_state; assigning it without this merge would erase the
  // final-compaction plan build and cache work.
  chart_spr_add_search_state_rebuild_counters(
      counters, result.rebuilt_state.counters,
      /*update_current_skipped_invariant_sites=*/false);
  return result;
}

// ---------------------------------------------------------------------------
// Phase 4 local-commit machinery (Work items 1 + 3 integration).
// ---------------------------------------------------------------------------
//
// Accepted SPR moves commit to the overlay chain (Phase 1) plus the
// persistent inside/outside caches (Phases 2/3) instead of dense-materializing
// per accept.  The substrate -- chain + caches -- lives here (in the .cpp)
// rather than on `chart_spr_search_state` because the cache headers include
// chart_spr_search.hpp, so the state struct cannot hold them without a
// circular include.  The substrate is owned by run_chart_spr_search; the
// state's `grammar` / `pattern_charts` are a DERIVED VIEW of the chain tip,
// refreshed on each commit, so candidate generation and local scoring operate
// on the chain tip exactly as they operate on a single candidate today.
//
// Epoch / snapshot barrier (cross-cutting concurrency model).  Scoring readers
// (run_chart_spr_acceptance_iteration, possibly multi-worker) read the frozen
// `state.pattern_charts` snapshot; commits are serialized at a barrier and
// refresh that snapshot.  Because the search loop runs an acceptance iteration
// to completion (all scoring futures joined) before committing, no scoring
// worker is ever mid-flight when a commit lands: the barrier is structural.
// The caches' `commit_epoch` (== chain.size() after a paired commit) is the
// snapshot ordinal readers implicitly read against.  Phase 9's transient chain
// extensions (reader-local, never mutating the shared cache) will bypass this
// barrier.

// Whether a runtime_error message is one of the labelled overlay-chain
// rejections a sequential local-commit search may legitimately encounter and
// skip: a tombstone that does not resolve to a frozen-base production, or a
// double tombstone.  These are the committability gate's labelled skips
// (Phase 4 tombstone scope, resolution (a)); they are never masked as silent
// no-ops.  Mirrors inside/outside_chart_cache_test.
bool chart_spr_is_local_commit_tombstone_scope_rejection(
    std::string const& msg) {
  return msg.find("overlay_chain") != std::string::npos &&
         (msg.find("not a frozen-base production") != std::string::npos ||
          msg.find("double tombstone") != std::string::npos);
}

class chart_spr_fixed_topology_cache_invariant_error
    : public std::runtime_error {
 public:
  explicit chart_spr_fixed_topology_cache_invariant_error(std::string message)
      : std::runtime_error(std::move(message)) {}
};

struct chart_spr_selected_topology_cache_entry {
  std::vector<std::array<chart_cost, nuc_state_count>> rows_by_pattern;
};

// Phase-8 view coupling the persistent inside cache with the tip->chain clade
// ref map.  The selected-topology recurrence runs in TIP space (state.grammar
// clade ids), but the persistent inside cache is keyed by chain overlay refs
// (frozen-base + chain-temp), which diverge from tip ids after the first local
// commit.  Cross-checking a base clade ref therefore requires translating the
// tip id through dense_clade_to_chain_ref before reading the cache; this view
// carries both so the cross-check is correct across commits, not just on the
// first iteration.
struct chart_spr_persistent_inside_cache_view {
  inside_chart_cache const* icache = nullptr;
  std::vector<overlay_clade_ref> const* dense_clade_to_chain_ref = nullptr;
};

struct chart_spr_selected_topology_row_cache {
  // Structural selected-subtree key -> all-active-pattern rows for that exact
  // rooted topology.  Keys are taxon/topology based rather than dense-id based,
  // so unchanged selected subtrees survive tip materialization and can be
  // reused by later candidate certificates in the same local-commit run.
  std::map<std::string, chart_spr_selected_topology_cache_entry> rows_by_key;
};

// The Phase 4 local-commit substrate: frozen base grammar + overlay chain +
// persistent inside/outside caches.  Non-movable once the chain/caches are
// emplaced: they hold pointers (`base`) into `base_grammar`, so moving the
// substrate would dangle them.  Allocated on the heap (unique_ptr) so its
// address is stable for the search run.
struct chart_spr_local_commit_substrate {
  clade_grammar base_grammar;
  // Immutable plan compiled for the frozen grammar before the chain and
  // caches are created.  The frozen grammar is a generation-preserving copy
  // of the initial search tip, so this is a copy of the resident state plan,
  // not a second plan build.
  chart_execution_plan base_execution_plan;
  std::optional<checked_chart_execution_plan_ref> checked_base_execution_plan;
  std::optional<overlay_chain> chain;
  std::optional<inside_chart_cache> icache;
  std::optional<outside_chart_cache> ocache;

  // Current materialized-tip dense ids -> persistent chain overlay refs.  The
  // search state's candidate generator names existing clades/productions in the
  // dense tip grammar, while the persistent inside/outside caches are keyed in
  // frozen-base + merged-temp overlay space.  Phase 8's fixed-topology scorer
  // uses these maps to read cached rows instead of dense-rebuilding charts.
  std::vector<overlay_clade_ref> dense_clade_to_chain_ref;
  std::vector<overlay_production_ref> dense_production_to_chain_ref;

  // Publication stamp for current-tip row projection.  The frozen cache base
  // keeps its initial generation, while these fields identify the dense tip
  // whose clade ids are mapped by dense_clade_to_chain_ref.  Exact-setup and
  // pattern-cache providers validate the complete stamp before reading rows.
  std::size_t published_tip_epoch = 0;
  std::uint64_t published_tip_execution_generation = 0;
  chart_plan_fingerprint published_tip_execution_fingerprint;
  inside_chart_cache_active_pattern_fingerprint
      published_active_pattern_fingerprint;

  double inside_cache_initialization_ms = 0.0;
  double outside_cache_initialization_ms = 0.0;

  // Phase 8 per-pattern selected-topology production cache.  This is separate
  // from the grammar-min inside/outside caches: it stores rows for one exact
  // structural selected subtree, so an unchanged fixed-topology subtree can be
  // read without recomputing that subtree.  Its counters are exposed alongside
  // the persistent-cache counters; the verifier must not hide an additional
  // full selected-tree oracle behind this cache path.
  chart_spr_selected_topology_row_cache selected_topology_cache;
  bool verify_materialized_fixed_topology_oracle_for_tests = false;
  bool force_independent_sm_bug_for_tests = false;
  // Phase 9 transient-extension verifier options (mirrored from
  // chart_spr_search_options by the substrate builder).
  bool verify_transient_chain_extension_oracle_for_tests = false;
  bool force_transient_chain_extension_oracle_mismatch_for_tests = false;
  std::size_t cache_multifurcation_productions_scored_reported = 0;
};

void chart_spr_set_identity_tip_maps(chart_spr_local_commit_substrate& sub) {
  sub.dense_clade_to_chain_ref.clear();
  sub.dense_clade_to_chain_ref.reserve(sub.base_grammar.clades.size());
  for (clade_id cid = 0; cid < sub.base_grammar.clades.size(); ++cid) {
    sub.dense_clade_to_chain_ref.push_back(base_clade_ref(cid));
  }
  sub.dense_production_to_chain_ref.clear();
  sub.dense_production_to_chain_ref.reserve(
      sub.base_grammar.productions.size());
  for (production_id pid = 0; pid < sub.base_grammar.productions.size(); ++pid) {
    sub.dense_production_to_chain_ref.push_back(base_production_ref(pid));
  }
}

void chart_spr_set_tip_maps_from_materialization(
    chart_spr_local_commit_substrate& sub,
    overlay_materialization_result const& materialized) {
  sub.dense_clade_to_chain_ref = materialized.dense_clade_to_ref;
  sub.dense_production_to_chain_ref = materialized.dense_production_to_ref;
}

void chart_spr_publish_local_commit_tip_identity(
    chart_spr_local_commit_substrate& sub,
    chart_spr_search_state const& state) {
  if (!sub.chain || !sub.icache || !sub.ocache) {
    throw std::runtime_error(
        "chart SPR local commit: cannot publish an incomplete cache tip");
  }
  if (sub.icache->commit_epoch != sub.chain->size() ||
      sub.ocache->commit_epoch != sub.chain->size()) {
    throw std::runtime_error(
        "chart SPR local commit: cannot publish mismatched cache epochs");
  }
  if (sub.dense_clade_to_chain_ref.size() != state.grammar.clades.size() ||
      sub.dense_production_to_chain_ref.size() !=
          state.grammar.productions.size()) {
    throw std::runtime_error(
        "chart SPR local commit: cannot publish mismatched dense tip maps");
  }
  auto const active_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(
          state.active_patterns);
  if (sub.icache->active_pattern_fingerprint != active_fingerprint ||
      sub.icache->patterns.size() !=
          state.active_patterns.patterns.patterns.size() ||
      sub.icache->taxon_count != state.active_patterns.patterns.taxon_count) {
    throw std::runtime_error(
        "chart SPR local commit: cannot publish mismatched active patterns");
  }
  sub.published_tip_epoch = sub.chain->size();
  sub.published_tip_execution_generation =
      state.execution_plan.grammar_generation();
  sub.published_tip_execution_fingerprint = state.execution_plan.fingerprint();
  sub.published_active_pattern_fingerprint = active_fingerprint;
}

void chart_spr_assert_local_commit_tip_identity(
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    std::string_view consumer) {
  checked_state.assert_same(state.grammar, state.execution_plan);
  if (!sub.chain || !sub.icache || !sub.ocache) {
    throw std::runtime_error(std::string{consumer} +
                             ": local substrate is incomplete");
  }
  if (sub.published_tip_epoch != sub.chain->size() ||
      sub.icache->commit_epoch != sub.published_tip_epoch ||
      sub.ocache->commit_epoch != sub.published_tip_epoch) {
    throw std::runtime_error(std::string{consumer} +
                             ": chain/cache epoch mismatch");
  }
  if (sub.published_tip_execution_generation !=
          state.execution_plan.grammar_generation() ||
      sub.published_tip_execution_fingerprint !=
          state.execution_plan.fingerprint()) {
    throw std::runtime_error(std::string{consumer} +
                             ": published tip execution mismatch");
  }
  auto const active_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(
          state.active_patterns);
  if (sub.published_active_pattern_fingerprint != active_fingerprint ||
      sub.icache->active_pattern_fingerprint != active_fingerprint ||
      sub.icache->patterns.size() !=
          state.active_patterns.patterns.patterns.size() ||
      sub.icache->taxon_count != state.active_patterns.patterns.taxon_count) {
    throw std::runtime_error(std::string{consumer} +
                             ": active-pattern identity mismatch");
  }
  if (sub.dense_clade_to_chain_ref.size() != state.grammar.clades.size() ||
      sub.dense_production_to_chain_ref.size() !=
          state.grammar.productions.size()) {
    throw std::runtime_error(std::string{consumer} +
                             ": dense tip map shape mismatch");
  }
}

overlay_clade_ref chart_spr_chain_ref_for_dense_clade(
    chart_spr_local_commit_substrate const& sub, clade_id dense) {
  if (dense == no_clade || dense >= sub.dense_clade_to_chain_ref.size()) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: dense clade ref out of "
        "current tip map range");
  }
  auto ref = sub.dense_clade_to_chain_ref[dense];
  if (ref.id == no_clade) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: dense clade maps to "
        "no_clade");
  }
  return ref;
}

chart_cost chart_spr_min_inside_plus_outside(
    std::array<chart_cost, nuc_state_count> const& inside,
    std::array<chart_cost, nuc_state_count> const& outside) {
  chart_cost best = chart_inf;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    best = std::min(best, parsimony_chart_detail::saturated_add(
                              inside[state], outside[state]));
  }
  return best;
}

std::string chart_spr_selected_topology_leaf_key(taxon_id taxon) {
  return "L" + std::to_string(taxon) + ";";
}

std::string chart_spr_selected_topology_internal_key(
    std::vector<std::string> child_keys) {
  if (child_keys.empty()) {
    throw std::runtime_error(
        "fixed_topology_exact selected-topology cache: internal key has no "
        "children");
  }
  std::sort(child_keys.begin(), child_keys.end());
  std::string key = "I";
  for (auto const& child_key : child_keys) {
    key += "(";
    key += child_key;
    key += ")";
  }
  return key;
}

struct chart_spr_selected_topology_node {
  std::string key;
  chart_spr_selected_topology_cache_entry const* entry = nullptr;
};

struct chart_spr_overlay_ref_active_guard {
  std::set<overlay_clade_ref>& active;
  overlay_clade_ref ref;

  chart_spr_overlay_ref_active_guard(std::set<overlay_clade_ref>& active_refs,
                                     overlay_clade_ref clade)
      : active(active_refs), ref(clade) {
    if (!active.insert(ref).second) {
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: cycle in selected "
          "topology");
    }
  }

  ~chart_spr_overlay_ref_active_guard() { active.erase(ref); }

  chart_spr_overlay_ref_active_guard(
      chart_spr_overlay_ref_active_guard const&) = delete;
  chart_spr_overlay_ref_active_guard& operator=(
      chart_spr_overlay_ref_active_guard const&) = delete;
};

chart_spr_selected_topology_node chart_spr_selected_topology_rows_for_clade(
    chart_spr_selected_topology_row_cache& cache,
    chart_spr_search_state const& state,
    grammar_spr_candidate const& candidate,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref clade, std::set<overlay_clade_ref>& active_refs,
    chart_spr_persistent_inside_cache_view persistent_icache) {
  chart_spr_overlay_ref_active_guard guard(active_refs, clade);
  auto const& active = state.active_patterns.patterns.patterns;
  auto taxa = chart_spr_clade_taxa_for_ref(state.grammar, candidate, clade);
  if (taxa.empty()) {
    throw std::runtime_error(
        "fixed_topology_exact selected-topology cache: selected clade has "
        "no taxa");
  }

  auto try_cached_node = [&](std::string const& key)
      -> std::optional<chart_spr_selected_topology_node> {
    auto it = cache.rows_by_key.find(key);
    if (it == cache.rows_by_key.end()) return std::nullopt;
    if (it->second.rows_by_pattern.size() != active.size()) {
      throw chart_spr_fixed_topology_cache_invariant_error(
          "fixed_topology_exact selected-topology cache: cached row "
          "pattern count mismatch");
    }
    ++state.counters.fixed_topology_selected_cache_hits;
    return chart_spr_selected_topology_node{key, &it->second};
  };
  auto insert_new_node = [&](std::string key,
                             chart_spr_selected_topology_cache_entry entry,
                             bool multifurcation_row = false) {
    if (entry.rows_by_pattern.size() != active.size()) {
      throw chart_spr_fixed_topology_cache_invariant_error(
          "fixed_topology_exact selected-topology cache: new row pattern "
          "count mismatch");
    }
    ++state.counters.fixed_topology_selected_cache_misses;
    state.counters.fixed_topology_selected_rows_computed +=
        entry.rows_by_pattern.size();
    state.counters.selected_topology_class_rows_computed +=
        entry.rows_by_pattern.size();
    if (multifurcation_row) {
      state.counters.selected_topology_multifurcation_rows +=
          entry.rows_by_pattern.size();
    }
    auto [inserted, ok] = cache.rows_by_key.emplace(key, std::move(entry));
    if (!ok) {
      throw chart_spr_fixed_topology_cache_invariant_error(
          "fixed_topology_exact selected-topology cache: duplicate insert");
    }
    return chart_spr_selected_topology_node{inserted->first,
                                            &inserted->second};
  };

  // Phase-8 affected-row participation of the persistent inside cache: for a
  // base clade ref, cross-check the just-computed selected-production row
  // against the grammar-min inside row stored in the persistent cache.  When
  // they agree the selected production is the optimal one at this clade, so
  // the persistent cache row is a valid source for the selected-topology delta
  // (an UNAFFECTED row, reused from the persistent cache); when they disagree
  // the selected production is locally suboptimal and this is an AFFECTED row
  // whose value must come from the selected-production recurrence.  The
  // selected row's value is authoritative either way (the persistent cache is
  // grammar-min and could be lower); this cross-check is what makes the delta
  // "from persistent inside+outside cache, restricted to affected rows"
  // observable rather than asserted.
  auto cross_check_persistent_icache =
      [&](chart_spr_selected_topology_cache_entry& entry) {
        if (persistent_icache.icache == nullptr) {
          return;
        }
        // Temp clade refs are candidate-local additions, not present in the
        // persistent inside cache; they are always AFFECTED (recomputed).
        if (clade.space != overlay_id_space::base) {
          state.counters.fixed_topology_icache_rows_recomputed_affected +=
              entry.rows_by_pattern.size();
          return;
        }
        // Translate the tip-space base clade id to its chain overlay ref before
        // reading the persistent inside cache (the caches are chain-keyed, and
        // tip ids diverge from frozen-base ids after the first local commit).
        overlay_clade_ref chain_ref{};
        if (persistent_icache.dense_clade_to_chain_ref == nullptr ||
            clade.id >= persistent_icache.dense_clade_to_chain_ref->size()) {
          // No mapping available (e.g. a clade the substrate does not track);
          // treat as affected rather than guessing the cache key.
          state.counters.fixed_topology_icache_rows_recomputed_affected +=
              entry.rows_by_pattern.size();
          return;
        }
        chain_ref = (*persistent_icache.dense_clade_to_chain_ref)[clade.id];
        if (chain_ref.id == no_clade) {
          state.counters.fixed_topology_icache_rows_recomputed_affected +=
              entry.rows_by_pattern.size();
          return;
        }
        bool all_match = true;
        for (std::size_t p = 0; p < entry.rows_by_pattern.size(); ++p) {
          if (entry.rows_by_pattern[p] !=
              persistent_icache.icache->row(p, chain_ref)) {
            all_match = false;
            break;
          }
        }
        if (all_match) {
          // The persistent cache row equals the selected row; it is a valid
          // source for the selected-topology delta (an UNAFFECTED row, reused
          // from the persistent inside cache).
          state.counters.fixed_topology_icache_rows_reused +=
              entry.rows_by_pattern.size();
        } else {
          // The selected production is locally suboptimal at this clade; this
          // is an AFFECTED row whose value must come from the selected
          // recurrence (the grammar-min cache could be lower).
          state.counters.fixed_topology_icache_rows_recomputed_affected +=
              entry.rows_by_pattern.size();
        }
      };

  if (taxa.size() == 1) {
    auto taxon = taxa.front();
    auto key = chart_spr_selected_topology_leaf_key(taxon);
    if (auto cached = try_cached_node(key)) return *cached;
    chart_spr_selected_topology_cache_entry entry;
    entry.rows_by_pattern.reserve(active.size());
    for (std::size_t p = 0; p < active.size(); ++p) {
      if (taxon >= active[p].state_by_taxon.size()) {
        throw std::runtime_error(
            "fixed_topology_exact selected-topology cache: leaf taxon out "
            "of state range");
      }
      auto observed = active[p].state_by_taxon[taxon];
      parsimony_chart_detail::validate_state(
          observed, "fixed_topology_exact selected-topology cache leaf state");
      auto row = parsimony_chart_detail::make_inf_row();
      row[observed] = 0;
      entry.rows_by_pattern.push_back(row);
    }
    cross_check_persistent_icache(entry);
    return insert_new_node(std::move(key), std::move(entry));
  }

  auto it = selected.find(clade);
  if (it == selected.end()) {
    throw std::runtime_error(
        "fixed_topology_exact selected-topology cache: selected topology "
        "missing production for non-singleton clade");
  }
  validate_chart_spr_selected_overlay_production_for_fixed_topology(
      state.grammar, candidate, it->second,
      "fixed_topology_exact selected-topology cache");
  auto children = chart_spr_overlay_production_children(state.grammar,
                                                        candidate,
                                                        it->second);
  std::vector<chart_spr_selected_topology_node> child_nodes;
  child_nodes.reserve(children.size());
  std::vector<std::string> child_keys;
  child_keys.reserve(children.size());
  for (auto child : children) {
    child_nodes.push_back(chart_spr_selected_topology_rows_for_clade(
        cache, state, candidate, selected, child, active_refs,
        persistent_icache));
    if (child_nodes.back().entry == nullptr) {
      throw chart_spr_fixed_topology_cache_invariant_error(
          "fixed_topology_exact selected-topology cache: child cache entry "
          "missing");
    }
    child_keys.push_back(child_nodes.back().key);
  }

  auto key = chart_spr_selected_topology_internal_key(std::move(child_keys));
  if (auto cached = try_cached_node(key)) return *cached;
  chart_spr_selected_topology_cache_entry entry;
  entry.rows_by_pattern.reserve(active.size());
  for (std::size_t p = 0; p < active.size(); ++p) {
    std::vector<chart_multisite_detail::chart_row> child_rows;
    child_rows.reserve(child_nodes.size());
    for (auto const& child_node : child_nodes) {
      child_rows.push_back(child_node.entry->rows_by_pattern[p]);
    }
    entry.rows_by_pattern.push_back(chart_multisite_detail::combine_rows(
        std::span<chart_multisite_detail::chart_row const>{
            child_rows.data(), child_rows.size()}));
  }
  cross_check_persistent_icache(entry);
  return insert_new_node(std::move(key), std::move(entry),
                         children.size() != 2);
}

struct chart_spr_selected_topology_root_entries {
  chart_spr_selected_topology_cache_entry const* before = nullptr;
  chart_spr_selected_topology_cache_entry const* after = nullptr;
};

chart_spr_selected_topology_root_entries
chart_spr_selected_topology_root_entries_from_cache(
    chart_spr_selected_topology_row_cache& cache,
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_persistent_inside_cache_view persistent_icache) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "fixed_topology_exact selected-topology cache requires a complete "
        "topology certificate");
  }
  auto const& certificate = *candidate.topology_selection.certificate;
  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate, certificate);

  auto before_selected = chart_spr_before_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.before_overlay_productions);
  auto after_selected = chart_spr_overlay_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.after_overlay_productions);

  std::set<overlay_clade_ref> active_refs;
  auto before_root = chart_spr_selected_topology_rows_for_clade(
      cache, state, candidate.candidate, before_selected,
      base_clade_ref(state.grammar.root_clade), active_refs,
      persistent_icache);
  active_refs.clear();
  auto after_root = chart_spr_selected_topology_rows_for_clade(
      cache, state, candidate.candidate, after_selected,
      base_clade_ref(state.grammar.root_clade), active_refs,
      persistent_icache);
  if (before_root.entry == nullptr || after_root.entry == nullptr) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "fixed_topology_exact selected-topology cache: root cache entry "
        "missing");
  }
  return chart_spr_selected_topology_root_entries{before_root.entry,
                                                 after_root.entry};
}

chart_spr_fixed_topology_pattern_scores
chart_spr_fixed_topology_materialized_oracle_pattern_scores(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "fixed_topology_exact materialized oracle requires a complete "
        "topology certificate");
  }
  auto const& certificate = *candidate.topology_selection.certificate;
  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate, certificate);

  auto before_ids = chart_spr_base_production_ids_from_refs(
      certificate.before_overlay_productions);
  auto before_topology = grammar_topology_from_productions(state.grammar,
                                                          before_ids);
  (void)validate_grammar_topology(state.grammar, before_topology);

  auto overlay = overlay_from_candidate(state.grammar, candidate.candidate);
  overlay_materialization_result materialized;
  {
    chart_spr_elapsed_accumulator materialization_timer{
        state.counters.materialization_exact_verification_ms};
    materialized = materialize_overlay_grammar(overlay);
  }
  ++state.counters.full_overlay_materializations;
  ++state.counters.overlay_materializations_for_oracle;

  std::vector<production_id> after_ids;
  after_ids.reserve(certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    after_ids.push_back(chart_spr_dense_production_id_for_ref(materialized,
                                                              ref));
  }
  auto after_topology = grammar_topology_from_productions(
      materialized.grammar, after_ids);
  (void)validate_grammar_topology(materialized.grammar, after_topology);

  chart_spr_fixed_topology_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  for (std::size_t pattern_index = 0; pattern_index < active.size();
       ++pattern_index) {
    auto const& pattern = active[pattern_index];
    if (state.chart_opts.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(
          pattern, pattern_index);
    }
    auto old_row = chart_multisite_detail::restricted_topology_row(
        state.grammar, pattern, before_topology);
    auto new_row = chart_multisite_detail::restricted_topology_row(
        materialized.grammar, pattern, after_topology);
    auto old_score = chart_spr_weighted_root_score_from_row(
        old_row, pattern, state.chart_opts);
    auto new_score = chart_spr_weighted_root_score_from_row(
        new_row, pattern, state.chart_opts);
    scores.old_pattern_scores.push_back(old_score);
    scores.new_pattern_scores.push_back(new_score);
    scores.old_active_total = chart_multisite_detail::checked_add_u64(
        scores.old_active_total, old_score,
        "fixed_topology_exact materialized oracle old active total");
    scores.new_active_total = chart_multisite_detail::checked_add_u64(
        scores.new_active_total, new_score,
        "fixed_topology_exact materialized oracle new active total");
  }
  return scores;
}

std::optional<std::string> chart_spr_fixed_topology_first_pattern_mismatch(
    chart_spr_fixed_topology_pattern_scores const& lhs,
    chart_spr_fixed_topology_pattern_scores const& rhs,
    std::string const& lhs_label, std::string const& rhs_label) {
  if (lhs.old_pattern_scores.size() != rhs.old_pattern_scores.size() ||
      lhs.new_pattern_scores.size() != rhs.new_pattern_scores.size() ||
      lhs.old_pattern_scores.size() != lhs.new_pattern_scores.size() ||
      rhs.old_pattern_scores.size() != rhs.new_pattern_scores.size()) {
    return "persistent-cache fixed-topology per-pattern oracle size mismatch "
           "between " +
           lhs_label + " and " + rhs_label;
  }
  for (std::size_t p = 0; p < lhs.old_pattern_scores.size(); ++p) {
    if (lhs.old_pattern_scores[p] != rhs.old_pattern_scores[p] ||
        lhs.new_pattern_scores[p] != rhs.new_pattern_scores[p]) {
      return "persistent-cache fixed-topology per-pattern oracle mismatch at "
             "pattern " +
             std::to_string(p) + " (" + lhs_label + " old/new=" +
             std::to_string(lhs.old_pattern_scores[p]) + "/" +
             std::to_string(lhs.new_pattern_scores[p]) + ", " + rhs_label +
             " old/new=" + std::to_string(rhs.old_pattern_scores[p]) + "/" +
             std::to_string(rhs.new_pattern_scores[p]) + ")";
    }
  }
  if (lhs.old_active_total != rhs.old_active_total ||
      lhs.new_active_total != rhs.new_active_total) {
    return "persistent-cache fixed-topology active-total oracle mismatch "
           "between " +
           lhs_label + " and " + rhs_label;
  }
  return std::nullopt;
}

struct chart_spr_fixed_topology_cache_pattern_scores {
  std::vector<std::uint64_t> old_pattern_scores;
  std::vector<std::uint64_t> new_pattern_scores;
  std::uint64_t old_active_total = 0;
  std::uint64_t new_active_total = 0;
  std::optional<chart_spr_fixed_topology_pattern_scores>
      materialized_oracle_scores;
  // Production per-pattern gate (Phase 8): the independent direct overlay
  // selected-topology scorer, which recomputes the selected before/after rows
  // without materializing an overlay grammar.  The cache score is labelled
  // fixed_topology_exact only when it agrees with this per pattern; otherwise
  // the direct oracle's value is the from-scratch authority for the selected
  // topology and the cache value is not trusted.
  std::optional<chart_spr_fixed_topology_pattern_scores>
      direct_oracle_scores;
  // True when the persistent selected-topology cache score can be used for
  // the fixed_topology_exact label: the production direct-overlay per-pattern
  // oracle agreed (and, when the diagnostic materialized oracle was requested,
  // it agreed too).  If false, the caller must take the strongest available
  // oracle result instead of the cache value.
  bool cache_score_ready_for_exact_label = false;
  std::string oracle_mismatch_reason;
};

std::array<chart_cost, nuc_state_count>
chart_spr_selected_overlay_child_outside_row(
    std::array<chart_cost, nuc_state_count> const& parent_outside,
    std::array<chart_cost, nuc_state_count> const& sibling_inside) {
  auto row = parsimony_chart_detail::make_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    auto base_cost = parent_outside[parent_state];
    if (base_cost >= chart_inf) continue;
    chart_cost sibling_best = chart_inf;
    for (std::uint8_t sib_state = 0; sib_state < nuc_state_count;
         ++sib_state) {
      sibling_best = std::min(
          sibling_best, parsimony_chart_detail::saturated_add(
                            sibling_inside[sib_state],
                            parsimony_chart_detail::transition_cost(
                                parent_state, sib_state)));
    }
    if (sibling_best >= chart_inf) continue;
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      auto candidate = chart_trim_detail::add3(
          base_cost, sibling_best,
          parsimony_chart_detail::transition_cost(parent_state, child_state));
      if (candidate < row[child_state]) row[child_state] = candidate;
    }
  }
  return row;
}

bool chart_spr_selected_overlay_outside_row_dfs(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    site_pattern const& pattern,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref clade, overlay_clade_ref target,
    std::array<chart_cost, nuc_state_count> const& clade_outside,
    std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>&
        base_inside_memo,
    std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>&
        temp_inside_memo,
    std::vector<std::uint8_t>& base_inside_state,
    std::vector<std::uint8_t>& temp_inside_state,
    std::set<overlay_clade_ref>& active,
    std::optional<std::array<chart_cost, nuc_state_count>>& result) {
  chart_spr_overlay_ref_active_guard guard(active, clade);

  if (clade == target) {
    result = clade_outside;
    return true;
  }

  auto taxa = chart_spr_clade_taxa_for_ref(base, candidate, clade);
  if (taxa.size() == 1) return false;

  auto it = selected.find(clade);
  if (it == selected.end()) {
    throw std::runtime_error(
        "fixed_topology_exact selected outside scorer: selected topology "
        "missing production for non-singleton clade");
  }
  validate_chart_spr_selected_overlay_production_for_fixed_topology(
      base, candidate, it->second,
      "fixed_topology_exact selected outside scorer");
  auto children = chart_spr_overlay_production_children(base, candidate,
                                                        it->second);
  std::vector<std::array<chart_cost, nuc_state_count>> child_inside_rows;
  child_inside_rows.reserve(children.size());
  for (auto child : children) {
    child_inside_rows.push_back(chart_spr_restricted_overlay_topology_row_impl(
        base, candidate, pattern, selected, child, base_inside_memo,
        temp_inside_memo, base_inside_state, temp_inside_state));
  }
  auto child_outside_rows = chart_spr_selected_overlay_child_outside_rows(
      clade_outside, child_inside_rows);

  for (std::size_t child_i = 0; child_i < children.size(); ++child_i) {
    if (chart_spr_selected_overlay_outside_row_dfs(
            base, candidate, pattern, selected, children[child_i], target,
            child_outside_rows[child_i], base_inside_memo, temp_inside_memo,
            base_inside_state, temp_inside_state, active, result)) {
      return true;
    }
  }
  return false;
}

std::array<chart_cost, nuc_state_count>
chart_spr_selected_overlay_outside_row(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    site_pattern const& pattern,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref target, chart_options const& options) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "fixed_topology_exact selected outside scorer: score_ua_edge=true is "
        "not supported by the shared-s_M test scorer");
  }
  std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>
      base_inside_memo(base.clades.size());
  std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>
      temp_inside_memo(candidate.added_clades.size());
  std::vector<std::uint8_t> base_inside_state(base.clades.size(), 0);
  std::vector<std::uint8_t> temp_inside_state(candidate.added_clades.size(), 0);
  std::set<overlay_clade_ref> active;
  std::optional<std::array<chart_cost, nuc_state_count>> result;
  std::array<chart_cost, nuc_state_count> root_outside{};
  root_outside.fill(0);
  (void)chart_spr_selected_overlay_outside_row_dfs(
      base, candidate, pattern, selected, base_clade_ref(base.root_clade),
      target, root_outside, base_inside_memo, temp_inside_memo,
      base_inside_state, temp_inside_state, active, result);
  if (!result) {
    throw std::runtime_error(
        "fixed_topology_exact selected outside scorer: target clade is not "
        "reachable in selected topology");
  }
  return *result;
}

chart_cost chart_spr_min_sum3_over_shared_state(
    std::array<chart_cost, nuc_state_count> const& a,
    std::array<chart_cost, nuc_state_count> const& b,
    std::array<chart_cost, nuc_state_count> const& c) {
  chart_cost best = chart_inf;
  for (std::uint8_t s = 0; s < nuc_state_count; ++s) {
    best = std::min(best, chart_trim_detail::add3(a[s], b[s], c[s]));
  }
  return best;
}

chart_cost chart_spr_min_sum2_over_state(
    std::array<chart_cost, nuc_state_count> const& a,
    std::array<chart_cost, nuc_state_count> const& b) {
  chart_cost best = chart_inf;
  for (std::uint8_t s = 0; s < nuc_state_count; ++s) {
    best = std::min(best,
                    parsimony_chart_detail::saturated_add(a[s], b[s]));
  }
  return best;
}

chart_cost chart_spr_min_row_over_state(
    std::array<chart_cost, nuc_state_count> const& row) {
  return *std::min_element(row.begin(), row.end());
}

bool chart_spr_apply_independent_sm_bug_for_tests(
    chart_spr_fixed_topology_cache_pattern_scores& scores,
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  // Test-only buggy scorer for the Phase-8 shared-s_M guard.  This runs the
  // actual independent-state bug class rather than an arbitrary perturbation:
  // for the moved clade M, derive the selected before/after outside context
  // rows and compare the correct shared-state term
  //   min_sM moved[sM] + detach_ctx[sM] + reattach_ctx[sM]
  // with buggy independent-state terms that minimize the detach and
  // reattach contexts independently (either with the moved row double-counted,
  // or with a separately minimized moved-row scalar plus independently
  // minimized contexts).
  // If the buggy term under-counts any active pattern, lower that pattern's
  // cached new score by the under-count (saturating at zero for test-only
  // corruption).  The materialized per-pattern oracle
  // must reject the result and force the fallback path.  The return value is
  // load-bearing test instrumentation: true means the real independent-s_M
  // under-count was applied; false means this candidate/pattern set had no
  // witness and no corruption was injected.
  auto const& certificate = *candidate.topology_selection.certificate;
  auto before_selected = chart_spr_before_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.before_overlay_productions);
  auto after_selected = chart_spr_overlay_selected_production_by_parent(
      state.grammar, candidate.candidate,
      certificate.after_overlay_productions);
  auto moved = candidate.candidate.moved_clade;
  auto const& active = state.active_patterns.patterns.patterns;
  for (std::size_t p = 0; p < active.size(); ++p) {
    // For the shared-s_M cell we need M's selected-subtree row; compute it
    // directly through the same restricted-topology recurrence rooted at M.
    std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>
        base_memo(state.grammar.clades.size());
    std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>
        temp_memo(candidate.candidate.added_clades.size());
    std::vector<std::uint8_t> base_state(state.grammar.clades.size(), 0);
    std::vector<std::uint8_t> temp_state(candidate.candidate.added_clades.size(),
                                         0);
    auto moved_inside = chart_spr_restricted_overlay_topology_row_impl(
        state.grammar, candidate.candidate, active[p], after_selected, moved,
        base_memo, temp_memo, base_state, temp_state);
    auto before_outside = chart_spr_selected_overlay_outside_row(
        state.grammar, candidate.candidate, active[p], before_selected, moved,
        state.chart_opts);
    auto after_outside = chart_spr_selected_overlay_outside_row(
        state.grammar, candidate.candidate, active[p], after_selected, moved,
        state.chart_opts);
    auto shared = chart_spr_min_sum3_over_shared_state(
        moved_inside, before_outside, after_outside);
    auto detach_only = chart_spr_min_sum2_over_state(moved_inside,
                                                     before_outside);
    auto reattach_only = chart_spr_min_sum2_over_state(moved_inside,
                                                       after_outside);
    auto independent = parsimony_chart_detail::saturated_add(detach_only,
                                                             reattach_only);
    if (independent < shared) {
      auto diff = static_cast<chart_cost>(shared - independent);
      auto weighted_diff = chart_multisite_detail::checked_mul_cost(
          active[p].weight, diff,
          "fixed_topology_exact independent-s_M bug weighted diff");
      auto applied_diff = std::min<std::uint64_t>(
          scores.new_pattern_scores[p], weighted_diff);
      if (applied_diff != 0) {
        scores.new_pattern_scores[p] -= applied_diff;
        scores.new_active_total -= applied_diff;
        ++state.counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
        return true;
      }
    }
    auto scalar_independent = chart_trim_detail::add3(
        chart_spr_min_row_over_state(moved_inside),
        chart_spr_min_row_over_state(before_outside),
        chart_spr_min_row_over_state(after_outside));
    if (scalar_independent < shared) {
      auto diff = static_cast<chart_cost>(shared - scalar_independent);
      auto weighted_diff = chart_multisite_detail::checked_mul_cost(
          active[p].weight, diff,
          "fixed_topology_exact scalar independent-s_M bug weighted diff");
      auto applied_diff = std::min<std::uint64_t>(
          scores.new_pattern_scores[p], weighted_diff);
      if (applied_diff != 0) {
        scores.new_pattern_scores[p] -= applied_diff;
        scores.new_active_total -= applied_diff;
        ++state.counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
        return true;
      }
    }

    // Also test the production-local form of the same bug class: detach and
    // reattach production terms each see the same moved-subtree row, so they
    // must not minimize over independent moved root states.  This isolates the
    // old sibling and new target/sibling contexts, which makes the corruption
    // hook load-bearing on small directed fixtures instead of relying on an
    // arbitrary perturbation.
    auto selected_inside = [&](auto const& selected_map,
                               overlay_clade_ref clade_ref) {
      std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>
          local_base_memo(state.grammar.clades.size());
      std::vector<std::optional<std::array<chart_cost, nuc_state_count>>>
          local_temp_memo(candidate.candidate.added_clades.size());
      std::vector<std::uint8_t> local_base_state(state.grammar.clades.size(),
                                                 0);
      std::vector<std::uint8_t> local_temp_state(
          candidate.candidate.added_clades.size(), 0);
      return chart_spr_restricted_overlay_topology_row_impl(
          state.grammar, candidate.candidate, active[p], selected_map,
          clade_ref, local_base_memo, local_temp_memo, local_base_state,
          local_temp_state);
    };
    auto old_sibling_inside = selected_inside(
        before_selected, candidate.candidate.old_sibling);
    auto new_sibling_inside = selected_inside(
        after_selected, candidate.candidate.new_sibling_or_target);
    std::array<chart_cost, nuc_state_count> zero_parent_context{};
    zero_parent_context.fill(0);
    auto detach_context = chart_spr_selected_overlay_child_outside_row(
        zero_parent_context, old_sibling_inside);
    auto reattach_context = chart_spr_selected_overlay_child_outside_row(
        zero_parent_context, new_sibling_inside);
    auto local_shared = chart_spr_min_sum3_over_shared_state(
        moved_inside, detach_context, reattach_context);
    auto local_detach_only = chart_spr_min_sum2_over_state(moved_inside,
                                                           detach_context);
    auto local_reattach_only = chart_spr_min_sum2_over_state(
        moved_inside, reattach_context);
    auto local_independent = parsimony_chart_detail::saturated_add(
        local_detach_only, local_reattach_only);
    if (local_independent < local_shared) {
      auto diff = static_cast<chart_cost>(local_shared - local_independent);
      auto weighted_diff = chart_multisite_detail::checked_mul_cost(
          active[p].weight, diff,
          "fixed_topology_exact production-local independent-s_M bug weighted "
          "diff");
      auto applied_diff = std::min<std::uint64_t>(
          scores.new_pattern_scores[p], weighted_diff);
      if (applied_diff != 0) {
        scores.new_pattern_scores[p] -= applied_diff;
        scores.new_active_total -= applied_diff;
        ++state.counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
        return true;
      }
    }
    auto local_scalar_independent = chart_trim_detail::add3(
        chart_spr_min_row_over_state(moved_inside),
        chart_spr_min_row_over_state(detach_context),
        chart_spr_min_row_over_state(reattach_context));
    if (local_scalar_independent < local_shared) {
      auto diff = static_cast<chart_cost>(local_shared -
                                          local_scalar_independent);
      auto weighted_diff = chart_multisite_detail::checked_mul_cost(
          active[p].weight, diff,
          "fixed_topology_exact production-local scalar independent-s_M bug "
          "weighted diff");
      auto applied_diff = std::min<std::uint64_t>(
          scores.new_pattern_scores[p], weighted_diff);
      if (applied_diff != 0) {
        scores.new_pattern_scores[p] -= applied_diff;
        scores.new_active_total -= applied_diff;
        ++state.counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
        return true;
      }
    }
  }

  // No arbitrary perturbation fallback: a test that claims shared-s_M coverage
  // must choose a fixture/candidate set with a real independent-state witness.
  return false;
}

// Phase-8 fixed-topology edge-term convention (per move class).
//
// * Binary SPR without collapse: the before certificate contains the old
//   parent production P_old -> (M, S_old) and the ancestor path above P_old;
//   the after certificate replaces it with the reattachment production
//   P_new -> (M, S_new) plus the corresponding ancestor-path productions.
//   The detach edge term is the transition on P_old -> M in the before row;
//   the reattach edge term is the transition on P_new -> M in the after row.
//
// * SPR with collapse/split: the before certificate additionally contains the
//   production that will be collapsed at the old parent and the after
//   certificate contains the split production that introduces the new parent.
//   The split/merge terms are exactly the parent->child transitions of those
//   selected binary productions; there is no separate scalar edge term outside
//   the selected-production recurrence.
//
// * Option-C child-set rewrite (when represented as an overlay delta): the
//   before and after child-set productions are scored as the old and new
//   selected binary productions at the rewritten parent; their two
//   parent->child transitions are the complete edge-term enumeration.
//
// In all three cases a moved subtree M is one overlay clade ref.  Every
// selected production that touches M reads the same fixed-topology row for that
// ref, so the moved-subtree root state s_M is a single shared minimization
// variable inside the row.  A scorer that minimized the detach and reattach
// terms with independent s_M values would no longer match the materialized
// selected-topology per-pattern oracle and would trip the Phase-8 tests.
chart_spr_fixed_topology_cache_pattern_scores
chart_spr_fixed_topology_pattern_scores_from_persistent_cache(
    chart_spr_local_commit_substrate& sub,
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  if (!sub.icache || !sub.ocache || !sub.chain) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: local-commit substrate is "
        "not initialized");
  }
  auto const& icache = *sub.icache;
  auto const& ocache = *sub.ocache;
  if (state.chart_opts.score_ua_edge) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: score_ua_edge=true is not "
        "supported by the persistent-cache verifier");
  }
  try {
    state.active_patterns.assert_no_skipped_invariant_metadata();
  } catch (std::exception const& e) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        std::string{"chart SPR fixed-topology cache verifier: active pattern "
                    "metadata invariant failed: "} +
        e.what());
  }
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier requires a complete topology "
        "certificate");
  }
  if (icache.commit_epoch != sub.chain->size() ||
      ocache.commit_epoch != sub.chain->size()) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: persistent cache epochs do "
        "not match the chain tip");
  }
  if (icache.patterns.size() !=
          state.active_patterns.patterns.patterns.size() ||
      ocache.patterns.size() != icache.patterns.size()) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: active pattern cache size "
        "mismatch");
  }
  if (sub.dense_clade_to_chain_ref.size() != state.grammar.clades.size()) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: dense clade map does not "
        "match current state grammar");
  }
  if (sub.dense_production_to_chain_ref.size() !=
      state.grammar.productions.size()) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: dense production map does "
        "not match current state grammar");
  }

  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate,
      *candidate.topology_selection.certificate);

  chart_spr_fixed_topology_cache_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  auto root_chain_ref = chart_spr_chain_ref_for_dense_clade(
      sub, state.grammar.root_clade);

  // Phase 8 fixed-topology cache path.  The grammar-min inside cache cannot be
  // read for a selected topology in a general DAG: an unchanged selected
  // subtree may be locally suboptimal, so `icache.row(...)` would silently
  // substitute a different production.  Instead the persistent selected-
  // topology cache stores rows keyed by the structural rooted topology and is
  // the production cache for fixed_topology_exact.  The outside cache still
  // supplies the root outside row, keeping the score in the same inside +
  // outside convention as the local-commit cache.  No dense overlay
  // materialization or B&B is performed here.
  chart_spr_selected_topology_root_entries roots;
  try {
    chart_spr_persistent_inside_cache_view icache_view;
    icache_view.icache = &*sub.icache;
    icache_view.dense_clade_to_chain_ref = &sub.dense_clade_to_chain_ref;
    roots = chart_spr_selected_topology_root_entries_from_cache(
        sub.selected_topology_cache, state, candidate, icache_view);
  } catch (chart_spr_fixed_topology_cache_invariant_error const&) {
    throw;
  } catch (std::exception const& e) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        std::string{"chart SPR fixed-topology cache verifier: selected-"
                    "topology cache access failed: "} +
        e.what());
  }
  if (roots.before == nullptr || roots.after == nullptr) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: selected-topology root "
        "entry missing");
  }
  if (roots.before->rows_by_pattern.size() != active.size() ||
      roots.after->rows_by_pattern.size() != active.size()) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "chart SPR fixed-topology cache verifier: selected-topology root "
        "entry pattern count mismatch");
  }
  for (std::size_t p = 0; p < active.size(); ++p) {
    std::array<chart_cost, nuc_state_count> root_outside;
    try {
      root_outside = ocache.row(p, root_chain_ref);
    } catch (chart_spr_fixed_topology_cache_invariant_error const&) {
      throw;
    } catch (std::exception const& e) {
      throw chart_spr_fixed_topology_cache_invariant_error(
          std::string{"chart SPR fixed-topology cache verifier: outside root "
                      "row access failed: "} +
          e.what());
    }
    auto old_min = chart_spr_min_inside_plus_outside(
        roots.before->rows_by_pattern[p], root_outside);
    auto new_min = chart_spr_min_inside_plus_outside(
        roots.after->rows_by_pattern[p], root_outside);
    auto old_score = chart_multisite_detail::checked_mul_cost(
        active[p].weight, old_min,
        "fixed_topology_exact persistent-cache old pattern score");
    auto new_score = chart_multisite_detail::checked_mul_cost(
        active[p].weight, new_min,
        "fixed_topology_exact persistent-cache new pattern score");
    scores.old_pattern_scores.push_back(old_score);
    scores.new_pattern_scores.push_back(new_score);
    scores.old_active_total = chart_multisite_detail::checked_add_u64(
        scores.old_active_total, old_score,
        "fixed_topology_exact persistent-cache old active total");
    scores.new_active_total = chart_multisite_detail::checked_add_u64(
        scores.new_active_total, new_score,
        "fixed_topology_exact persistent-cache new active total");
  }

  if (sub.force_independent_sm_bug_for_tests) {
    chart_spr_apply_independent_sm_bug_for_tests(scores, state, candidate);
  }

  chart_spr_fixed_topology_pattern_scores cache_scores;
  cache_scores.old_pattern_scores = scores.old_pattern_scores;
  cache_scores.new_pattern_scores = scores.new_pattern_scores;
  cache_scores.old_active_total = scores.old_active_total;
  cache_scores.new_active_total = scores.new_active_total;

  // Phase-8 production per-pattern gate (Work item 4a).  The persistent
  // selected-topology cache score is labelled fixed_topology_exact ONLY when
  // it agrees, per pattern, with the independent direct overlay selected-
  // topology scorer.  The direct scorer recomputes the selected before/after
  // rows in overlay space WITHOUT materializing an overlay grammar, so this
  // gate does not bump full_overlay_materializations; it catches structural-
  // cache corruption (a stale or wrongly-keyed selected row) that the cache
  // path alone could not detect.  On per-pattern mismatch the direct oracle's
  // value is retained as the from-scratch authority for the selected topology
  // and the cache value is not trusted.
  scores.direct_oracle_scores =
      fixed_topology_direct_selected_pattern_scores(state, candidate);
  if (auto direct_mismatch =
          chart_spr_fixed_topology_first_pattern_mismatch(
              cache_scores, *scores.direct_oracle_scores,
              "persistent-cache", "direct-overlay-selected-oracle")) {
    ++state.counters.fixed_topology_persistent_cache_direct_oracle_mismatches;
    scores.oracle_mismatch_reason = *direct_mismatch;
    // Fall through to the optional materialized oracle so that, when the
    // stronger from-scratch oracle is requested, its value is the one used as
    // the mismatch authority (issue 4); otherwise the direct oracle value
    // above is the authority and the caller takes it.
  }

  // Optional Phase-8 materialized from-scratch oracle: materialize the
  // candidate's extended grammar and score the same selected before/after
  // topology per pattern.  This is the strongest independent oracle (it does
  // not share overlay-space row machinery with the cache path), so when it is
  // enabled and finds a mismatch its result is the authority used (issue 4),
  // not a re-run of the direct overlay scorer.  It is diagnostic/test-only by
  // default because it materializes; the independent-s_M corruption hook
  // forces it on, because the hook exists to prove the oracle rejects the
  // buggy scorer.
  if (sub.verify_materialized_fixed_topology_oracle_for_tests ||
      sub.force_independent_sm_bug_for_tests) {
    scores.materialized_oracle_scores =
        chart_spr_fixed_topology_materialized_oracle_pattern_scores(state,
                                                                   candidate);
    if (auto mismatch = chart_spr_fixed_topology_first_pattern_mismatch(
            cache_scores, *scores.materialized_oracle_scores,
            "persistent-cache", "materialized-selected-oracle")) {
      scores.oracle_mismatch_reason = *mismatch;
      return scores;
    }
  }

  // The cache value is labelled exact only when the production per-pattern
  // direct oracle agreed (and, when requested, the materialized oracle agreed
  // too).  Otherwise the caller takes the strongest available oracle value.
  if (!scores.direct_oracle_scores ||
      chart_spr_fixed_topology_first_pattern_mismatch(
          cache_scores, *scores.direct_oracle_scores,
          "persistent-cache", "direct-overlay-selected-oracle")) {
    return scores;
  }
  scores.cache_score_ready_for_exact_label = true;
  return scores;
}

// Build a fixed_topology_exact objective score from an independent per-pattern
// selected-topology oracle result (direct overlay scorer or materialized
// from-scratch oracle).  Used when the persistent selected-topology cache
// could not be trusted (per-pattern mismatch) -- the oracle's value is the
// from-scratch authority for the selected topology, so it is still labelled
// fixed_topology_exact.  Issue 4: on a materialized-oracle mismatch the
// materialized oracle's own scores are used here, not a re-run of the direct
// overlay scorer that shares machinery with the cache path.
chart_spr_candidate_score
chart_spr_build_exact_from_oracle_pattern_scores(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_spr_fixed_topology_pattern_scores const& oracle_scores) {
  auto old_full = chart_spr_add_invariant_offset(
      oracle_scores.old_active_total, state,
      "chart-SPR fixed-topology oracle old invariant offset");
  auto new_full = chart_spr_add_invariant_offset(
      oracle_scores.new_active_total, state,
      "chart-SPR fixed-topology oracle new invariant offset");
  candidate.exact = make_chart_spr_objective_score(
      spr_score_result{chart_spr_detail::signed_delta(old_full, new_full),
                       old_full, new_full, true},
      chart_spr_score_kind::fixed_topology_exact,
      chart_spr_score_convention::full_with_invariants,
      state.invariant_constant_offset);
  return candidate;
}

chart_spr_candidate_score
chart_spr_verify_fixed_topology_direct_fallback_after_counting(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_spr_fixed_topology_cache_pattern_scores const& cache_scores) {
  auto const& reason = cache_scores.oracle_mismatch_reason;
  ++state.counters.fixed_topology_persistent_cache_fallbacks;
  // Issue 4: when the materialized from-scratch oracle ran and mismatched, its
  // value is the authority -- use it directly rather than re-running the
  // direct overlay scorer (which shares overlay-space row machinery with the
  // cache path and is therefore a weaker fallback than the Phase-8 from-scratch
  // oracle contract).  Otherwise the production direct-overlay gate caught
  // the mismatch; its value is the authority for the selected topology.
  try {
    if (cache_scores.materialized_oracle_scores) {
      ++state.counters.fixed_topology_persistent_cache_oracle_mismatches;
      return chart_spr_build_exact_from_oracle_pattern_scores(
          state, std::move(candidate),
          *cache_scores.materialized_oracle_scores);
    }
    if (cache_scores.direct_oracle_scores) {
      return chart_spr_build_exact_from_oracle_pattern_scores(
          state, std::move(candidate), *cache_scores.direct_oracle_scores);
    }
    // No oracle ran (e.g. the verifier failed before reaching the gate).  Use
    // the conservative from-scratch direct scorer as the last resort.
    auto delta = fixed_topology_delta_direct_selected_topology(state, candidate);
    candidate.exact = make_chart_spr_objective_score(
        delta, chart_spr_score_kind::fixed_topology_exact,
        chart_spr_score_convention::full_with_invariants,
        state.invariant_constant_offset);
    return candidate;
  } catch (std::exception const& e) {
    throw std::runtime_error(
        "fixed_topology_exact persistent-cache verifier: oracle/fallback "
        "failed after cache-oracle mismatch (" +
        reason + "): " + e.what());
  }
}

chart_spr_candidate_score
chart_spr_verify_candidate_fixed_topology_exact_from_persistent_cache(
    chart_spr_local_commit_substrate& sub,
    chart_spr_search_state const& state,
    chart_spr_candidate_score candidate) {
  if (!candidate.valid) return candidate;
  if (!chart_spr_topology_selection_has_certificate_or_selector(
          candidate.topology_selection)) {
    candidate.valid = false;
    candidate.invalid_reason =
        "fixed_topology_exact acceptance requires an explicit complete "
        "topology certificate or a recorded deterministic topology selector; "
        "a bare grammar_spr_candidate is not exact";
    return candidate;
  }
  if (!candidate.topology_selection.certificate) {
    candidate.valid = false;
    candidate.invalid_reason =
        "fixed_topology_exact deterministic selectors must be resolved to a "
        "complete topology certificate before verification";
    return candidate;
  }

  ++state.counters.exact_verifications;
  ++state.counters.fixed_topology_persistent_cache_verifications;
  try {
    auto scores = chart_spr_fixed_topology_pattern_scores_from_persistent_cache(
        sub, state, candidate);
    if (!scores.cache_score_ready_for_exact_label) {
      return chart_spr_verify_fixed_topology_direct_fallback_after_counting(
          state, std::move(candidate), scores);
    }
    auto old_full = chart_spr_add_invariant_offset(
        scores.old_active_total, state,
        "chart-SPR fixed-topology persistent-cache old invariant offset");
    auto new_full = chart_spr_add_invariant_offset(
        scores.new_active_total, state,
        "chart-SPR fixed-topology persistent-cache new invariant offset");
    candidate.exact = make_chart_spr_objective_score(
        spr_score_result{chart_spr_detail::signed_delta(old_full, new_full),
                         old_full, new_full, true},
        chart_spr_score_kind::fixed_topology_exact,
        chart_spr_score_convention::full_with_invariants,
        state.invariant_constant_offset);
  } catch (chart_spr_fixed_topology_cache_invariant_error const&) {
    throw;
  } catch (std::exception const& e) {
    candidate.valid = false;
    candidate.invalid_reason = e.what();
  }
  return candidate;
}

// Phase 9 (Work item 4a, technique 2): transient chain extension for
// grammar-exact verification.
//
// A reader-local snapshot-extended view of the committed chain, advanced by
// exactly one unaccepted candidate delta.  It never mutates the shared
// substrate, so the extension bypasses the Phase 4 commit barrier and can run
// alongside other scoring readers under the epoch/snapshot model.
//
// The exact B&B consumes the materialized extended grammar, not the persistent
// per-pattern caches.  Consequently, production verification copies and
// advances only the chain.  Scratch inside/outside caches are copied and
// advanced only when the opt-in two-chart oracle (or its corruption hook) needs
// them.  That diagnostic path deliberately uses the same paired commit
// primitives as a real commit, preserving its affected-set oracle coverage
// without charging the dead cache work to production candidates.
//
// The materialized-grammar B&B still rebuilds its own exact setup; feeding the
// persistent caches into that frontier is separate warm-started-B&B work.  The
// transient chain work remains counted under
// `transient_chain_extensions_for_verification`, never under
// `full_overlay_materializations`; diagnostic cache work has its own
// `transient_chain_diagnostic_cache_extensions` counter.
struct chart_spr_transient_extension {
  // Scratch chain = copy of the committed chain + appended candidate delta.
  // Reader-local; the committed chain is untouched.
  overlay_chain chain;
  // Diagnostic-only copies of the persistent caches, advanced one paired
  // commit to the extended tip.  Absent on the production path because exact
  // B&B does not consume them.
  std::optional<inside_chart_cache> icache;
  std::optional<outside_chart_cache> ocache;
  // Materialized extended tip grammar + dense->overlay-ref maps.  Built by
  // `materialize_overlay_chain` on the scratch chain; the grammar is identical
  // to `materialize_overlay_grammar(overlay_from_candidate(tip, candidate))`.
  // Built as one checked publication: the dynamic chain payload is validated,
  // then the dense grammar receives a fresh generation and exactly one output
  // plan.  Keeping the pair intact is the capability used by fresh-plan trim
  // and provenance helpers without a redundant fingerprint scan.
  planned_overlay_materialization_result planned;
};

// Exact verification and commit need only the append vocabulary.  The
// candidate was already fully validated and descriptor-compiled during local
// scoring, so rebuilding reachability, affected order, indices, and recurrence
// descriptors here would multiply candidate-plan construction up to threefold.
spr_overlay_delta chart_spr_build_validated_append_payload(
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    grammar_spr_candidate const& candidate) {
  // Exact verification and commit are checked state boundaries, not
  // candidate-pattern hot loops.  Check the fingerprint as well as the token
  // so a same-generation in-place mutation cannot be appended through a stale
  // resident plan.
  checked_state.assert_same(state.grammar, state.execution_plan);

  spr_overlay_delta delta;
  delta.base = &state.grammar;
  delta.temp_clades = candidate.added_clades;
  delta.temp_productions = candidate.added_productions;
  delta.removed_base_productions.reserve(candidate.removed_productions.size());
  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base || ref.id == no_production ||
        ref.id >= state.grammar.productions.size()) {
      throw std::runtime_error(
          "chart SPR append payload: invalid removed base production");
    }
    delta.removed_base_productions.push_back(ref.id);
  }
  std::sort(delta.removed_base_productions.begin(),
            delta.removed_base_productions.end());
  delta.removed_base_productions.erase(
      std::unique(delta.removed_base_productions.begin(),
                  delta.removed_base_productions.end()),
      delta.removed_base_productions.end());
  return delta;
}

chart_spr_transient_extension chart_spr_build_transient_extension(
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_candidate_score const& candidate) {
  chart_spr_transient_extension ext;
  // Copy the committed chain (reader-local).  The chain's base pointer still
  // references the substrate's frozen base grammar, which lives for the run.
  ext.chain = *sub.chain;
  // Build the candidate delta against the CURRENT tip (state.grammar is the
  // materialized chain tip the candidate was generated/scored against).
  auto delta =
      chart_spr_build_validated_append_payload(
          state, checked_state, candidate.candidate);
  // Append to the scratch chain.  A tombstone-scope rejection (the candidate
  // tombstones a production that does not resolve to a frozen-base production)
  // throws here; the caller treats it as an invalid candidate, exactly as the
  // cold path's materialize would surface an unreachable overlay.
  ext.chain.append(delta);
  // The production B&B below cannot consume persistent cache rows.  Copy and
  // advance them only for diagnostics that actually inspect both charts.  The
  // pairing guards pass because the source caches are at the committed epoch
  // and the scratch chain is exactly one delta ahead.
  auto const build_diagnostic_caches =
      sub.verify_transient_chain_extension_oracle_for_tests ||
      sub.force_transient_chain_extension_oracle_mismatch_for_tests;
  if (build_diagnostic_caches) {
    if (!sub.icache || !sub.ocache) {
      throw std::runtime_error(
          "chart SPR transient extension: diagnostic cache source missing");
    }
    ext.icache.emplace(*sub.icache);
    ext.ocache.emplace(*sub.ocache);
    apply_commit_to_inside_cache(ext.chain, *ext.icache);
    apply_commit_to_outside_cache(ext.chain, *ext.ocache, *ext.icache);
  }
  // Materialize the extended tip (transient, reader-local).  This is accounted
  // by the historical transient-extension counter, not the umbrella
  // full_overlay_materializations counter, even though it publishes the exact
  // grammar consumed by B&B.
  //
  // TODO(phase-12 / perf): `materialize_overlay_chain(ext.chain)` folds the
  // WHOLE chain onto the base.  The cold path reaches an identical extended
  // grammar more cheaply via `overlay_from_candidate(state.grammar, candidate)`
  // (one delta onto the already-materialized tip).  The chain fold is
  // unnecessary extra work.  Diagnostic cache copies are now gated above; a
  // future warm-started B&B can make the chain + caches authoritative instead
  // of keeping this materialized-grammar bridge.
  overlay_payload_validation_stats completed_payload_validation_stats;
  try {
    chart_spr_elapsed_accumulator materialization_timer{
        state.counters.materialization_exact_verification_ms};
    if (!sub.checked_base_execution_plan) {
      throw std::runtime_error(
          "chart SPR transient extension: missing checked frozen-base plan");
    }
    auto planned = materialize_overlay_chain_with_plan(
        ext.chain, *sub.checked_base_execution_plan, nullptr,
        [&] { materialization_timer.finish(); },
        &completed_payload_validation_stats);
    ext.planned = std::move(planned);
  } catch (...) {
    record_overlay_payload_validation_stats(
        state.counters, completed_payload_validation_stats);
    throw;
  }
  record_planned_overlay_materialization_stats(state.counters,
                                               ext.planned);
  if (build_diagnostic_caches) {
    ++state.counters.transient_chain_diagnostic_cache_extensions;
  }
  return ext;
}

// Per-candidate two-chart oracle for the transient extension (Work item 4a
// correctness invariant).  Recomputes BOTH charts from scratch on the extended
// grammar via Phase 0's `recompute_both_charts_from_scratch` and asserts the
// scratch caches agree on every reachable clade, every active pattern; also
// runs a cold from-scratch B&B on a freshly materialized candidate overlay and
// asserts the exact optimum agrees.  An inside-only oracle could not catch
// outside under-inclusion, so both halves are checked.  Returns the cold
// optimum so the verifier can fall back to the authoritative value on
// mismatch.
struct chart_spr_transient_oracle_result {
  bool ok = true;
  std::uint64_t cold_new_optimum = 0;
  std::string mismatch_reason;
};

chart_spr_transient_oracle_result chart_spr_check_transient_extension_oracle(
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_transient_extension const& ext,
    std::uint64_t transient_new_optimum,
    multisite_trim_options const& trim_options) {
  chart_spr_transient_oracle_result result;
  if (!ext.icache || !ext.ocache) {
    throw std::runtime_error(
        "transient oracle: diagnostic scratch caches were not constructed");
  }
  auto const& icache = *ext.icache;
  auto const& ocache = *ext.ocache;
  auto const& grammar = ext.planned.materialized.grammar;
  if (ext.planned.materialized.dense_clade_to_ref.size() !=
      grammar.clades.size()) {
    result.ok = false;
    result.mismatch_reason =
        "transient oracle: extended grammar dense clade map size mismatch";
    return result;
  }

  // Cold from-scratch optimum on the same extended grammar.  Counted under the
  // oracle bucket (diagnostic), mirroring Phase 8's materialized-oracle
  // counting so a regression is visible without being folded into the
  // transient extension counter.
  auto cold_overlay = overlay_from_candidate(state.grammar, candidate.candidate);
  overlay_materialization_result cold_materialized;
  {
    chart_spr_elapsed_accumulator materialization_timer{
        state.counters.materialization_exact_verification_ms};
    cold_materialized = materialize_overlay_grammar(cold_overlay);
  }
  ++state.counters.full_overlay_materializations;
  ++state.counters.overlay_materializations_for_oracle;
  auto cold_trim = build_multisite_trim_active(
      cold_materialized.grammar, state.active_patterns, state.chart_opts,
      trim_options);
  record_multisite_exact_trim_work(state.counters, cold_trim);
  result.cold_new_optimum = cold_trim.optimum;

  // Both-charts check: scratch caches vs from-scratch on the extended grammar.
  for (std::size_t p = 0; p < icache.patterns.size(); ++p) {
    leaf_site_states states;
    states.state_by_taxon = icache.patterns[p].state_by_taxon;
    auto oracle = recompute_both_charts_from_scratch(
        grammar, states, icache.chart_opts);
    if (oracle.first.inside.size() != grammar.clades.size() ||
        oracle.second.outside.size() != grammar.clades.size()) {
      result.ok = false;
      result.mismatch_reason =
          "transient oracle: from-scratch chart size mismatch at pattern " +
          std::to_string(p);
      return result;
    }
    for (std::size_t dense = 0;
         dense < ext.planned.materialized.dense_clade_to_ref.size(); ++dense) {
      auto ref = ext.planned.materialized.dense_clade_to_ref[dense];
      ++state.counters.transient_chain_extension_oracle_rows_checked_for_tests;
      if (icache.row(p, ref) != oracle.first.inside[dense]) {
        result.ok = false;
        result.mismatch_reason =
            "transient oracle: inside scratch row mismatch at pattern " +
            std::to_string(p) + " dense clade " + std::to_string(dense);
        return result;
      }
      if (ocache.row(p, ref) != oracle.second.outside[dense]) {
        result.ok = false;
        result.mismatch_reason =
            "transient oracle: outside scratch row mismatch at pattern " +
            std::to_string(p) + " dense clade " + std::to_string(dense);
        return result;
      }
    }
  }
  // Exact-score check: the transient B&B optimum on the extended grammar must
  // equal the cold from-scratch B&B optimum.  This is the load-bearing
  // equality the Phase 9 exit criterion asserts ("transient-extension exact
  // score equals from-scratch exact score"); a difference here means the
  // transient grammar diverges from the cold grammar or the B&B is
  // nondeterministic, either of which is a correctness bug.
  if (transient_new_optimum != result.cold_new_optimum) {
    result.ok = false;
    result.mismatch_reason =
        "transient oracle: exact optimum mismatch (transient " +
        std::to_string(transient_new_optimum) + " vs cold " +
        std::to_string(result.cold_new_optimum) + ")";
    return result;
  }
  return result;
}

// Phase 9 exact_multisite verifier: score a candidate by transiently extending
// the chain in reader-local scratch, reading the exact frontier on the extended
// grammar via `build_multisite_trim_active`, and discarding.  Production does
// not copy the persistent caches because B&B cannot consume them; the opt-in
// two-chart oracle constructs its diagnostic cache extension explicitly.  The
// transient work is counted
// under `transient_chain_extensions_for_verification`, never under
// `full_overlay_materializations`.  When the test-only oracle flag is set, the
// result is cross-checked against the cold from-scratch path (both charts +
// exact optimum); on mismatch the cold result is authoritative and the
// transient count is recorded as a fallback.
chart_spr_candidate_score
chart_spr_verify_candidate_exact_multisite_from_transient_extension(
    chart_spr_local_commit_substrate& sub,
    chart_spr_search_state const& state,
    chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    multisite_trim_options const& trim_options) {
  if (!candidate.valid) return candidate;

  chart_spr_transient_extension ext;
  try {
    ext = chart_spr_build_transient_extension(
        sub, state, checked_state, candidate);
  } catch (std::runtime_error const& e) {
    // The candidate delta cannot be appended to the scratch chain (tombstone
    // scope: it tombstones a production that does not resolve to a frozen-base
    // production).  The transient extension is unavailable for this candidate;
    // fall back to the cold from-scratch path so the candidate can still be
    // verified, accepted, and reach the commit-time tombstone-scope skip
    // (Phase 4 resolution (a)).  This is NOT a silent degradation: the cold
    // path is the authoritative from-scratch verifier, and the tombstone-scope
    // skip remains a labelled, counted outcome.  Any other append error is a
    // hard correctness failure and is rethrown.
    if (chart_spr_is_local_commit_tombstone_scope_rejection(e.what())) {
      return verify_candidate_exact_against_state(
          state, std::move(candidate), checked_state, trim_options);
    }
    throw;
  }

  // The transient extension succeeded: this candidate's delta resolves to
  // frozen-base productions, so it could be committed.  Count it under the
  // transient-extension counter (never under full_overlay_materializations).
  ++state.counters.exact_verifications;
  ++state.counters.transient_chain_extensions_for_verification;
  multisite_trim_result new_trim;
  std::uint64_t authoritative_new_optimum = multisite_score_inf;
  try {
    // Old score: the current tip's exact optimum (cached in state, lazily
    // built).  Same source the cold path reads.
    auto const& old_trim =
        ensure_chart_spr_state_exact_trim(state, checked_state, trim_options);

    // New score: B&B exact optimum of the extended grammar.
    new_trim =
        state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart
            ? build_lazy_multisite_trim_active_from_scratch(
                  ext.planned, state.active_patterns, state.chart_opts,
                  trim_options)
            : build_multisite_trim_active(ext.planned.execution_plan,
                                          state.active_patterns,
                                          state.chart_opts, trim_options);
    if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
      ++state.counters.exact_trim_lazy_chart_uses;
    }
    record_multisite_exact_trim_work(state.counters, new_trim);
    ++state.counters.chart_execution_plan_cache_hits;

    // Optional corruption hook: perturb a scratch outside row so the two-chart
    // oracle catches the disagreement and the verifier falls back to the cold
    // path.  The perturbation is applied to the SCRATCH cache only (never the
    // shared cache); the oracle's from-scratch chart is unaffected, so the
    // mismatch is deterministic.
    if (sub.force_transient_chain_extension_oracle_mismatch_for_tests) {
      if (!ext.ocache) {
        throw std::runtime_error(
            "chart SPR transient extension: forced mismatch missing diagnostic "
            "outside cache");
      }
      if (!ext.ocache->base_rows.empty() &&
          !ext.ocache->base_rows[0].empty()) {
        auto& row = ext.ocache->base_rows[0][0];
        if (row[0] < larch::chart_inf) {
          row[0] = row[0] + 1;
        } else {
          row[0] = larch::chart_cost{0};
        }
      }
    }

    // Per-candidate oracle (test/diagnostic): both charts + cold exact
    // optimum.  The transient result is trusted unless the oracle finds a
    // mismatch; on mismatch the cold result is authoritative.
    //
    // Plan wording deviation (Phase 9 exit criterion 3), recorded explicitly:
    // the plan says on oracle mismatch the exactness label is "otherwise
    // weakened and the from-scratch path is used."  This implementation does
    // NOT weaken the label: it substitutes the cold exact optimum and KEEPS
    // `grammar_exact`.  That is the right call because the cold B&B is
    // genuinely exact, so weakening would mislabel a correct value as a mere
    // lower bound.  The substantive requirement -- "do not trust a wrong
    // transient result; use the authoritative cold value" -- is met; only the
    // literal "weakened" is not honored.  The work IS recorded as a fallback
    // (`transient_chain_extension_fallbacks`) so a regression to a wrong
    // transient result is visible in the counters.
    authoritative_new_optimum = new_trim.optimum;
    if (sub.verify_transient_chain_extension_oracle_for_tests ||
        sub.force_transient_chain_extension_oracle_mismatch_for_tests) {
      auto oracle = chart_spr_check_transient_extension_oracle(
          sub, state, candidate, ext, new_trim.optimum, trim_options);
      if (!oracle.ok) {
        ++state.counters.transient_chain_extension_oracle_mismatches;
        ++state.counters.transient_chain_extension_fallbacks;
        authoritative_new_optimum = oracle.cold_new_optimum;
      }
    }

    auto old_full = chart_spr_add_invariant_offset(
        old_trim.optimum, state,
        "chart-SPR transient exact old-score invariant offset");
    auto new_full = chart_spr_add_invariant_offset(
        authoritative_new_optimum, state,
        "chart-SPR transient exact new-score invariant offset");
    candidate.exact = make_chart_spr_objective_score(
        spr_score_result{chart_spr_detail::signed_delta(old_full, new_full),
                         old_full, new_full, true},
        chart_spr_score_kind::grammar_exact,
        chart_spr_score_convention::full_with_invariants,
        state.invariant_constant_offset);
  } catch (std::exception const& e) {
    candidate.valid = false;
    candidate.invalid_reason = e.what();
  }
  // Report construction is deliberately outside the verifier catch.  A
  // canonicalization failure is a hard oracle failure, never an algorithmic
  // invalid-candidate outcome.
  if (candidate.valid && candidate.exact &&
      candidate.canonical_stream_index !=
          (std::numeric_limits<std::size_t>::max)()) {
    chart_spr_force_canonical_evidence_failure_for_tests(candidate);
    if (authoritative_new_optimum == new_trim.optimum) {
      candidate.canonical_exact_evidence =
          std::make_shared<chart_spr_canonical_exact_evidence>(
              chart_spr_canonicalize_search_trim_evidence(
                  ext.planned, state.active_patterns, state.chart_opts,
                  trim_options, new_trim,
                  state.invariant_constant_offset));
      if (new_trim.keep_production_exact) {
        ++state.counters.chart_execution_plan_cache_hits;
      }
    } else {
      chart_spr_canonical_exact_evidence evidence;
      evidence.evidence_kind =
          "grammar_exact_oracle_fallback_frontier_unavailable";
      evidence.keep_mask_kind = "unavailable_after_oracle_fallback";
      evidence.optimum_active = authoritative_new_optimum;
      evidence.invariant_offset = state.invariant_constant_offset;
      candidate.canonical_exact_evidence =
          std::make_shared<chart_spr_canonical_exact_evidence>(
              std::move(evidence));
    }
  }
  return candidate;
}

multisite_exact_setup chart_spr_build_exact_setup_from_persistent_inside_cache(
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state) {
  chart_spr_assert_local_commit_tip_identity(
      sub, state, checked_state, "chart SPR persistent-inside exact setup");
  if (!sub.icache) {
    throw std::runtime_error(
        "chart SPR persistent-inside exact setup: missing inside cache");
  }

  single_site_chart scratch;
  scratch.inside.resize(state.execution_plan.clades().size());
  return build_multisite_exact_setup_from_resident_inside(
      state.execution_plan, state.active_patterns.patterns,
      [&](std::size_t pattern_index,
          site_pattern const&) -> single_site_chart const& {
        if (pattern_index >= sub.icache->patterns.size()) {
          throw std::runtime_error(
              "chart SPR persistent-inside exact setup: pattern index out of "
              "range");
        }
        for (std::size_t dense = 0; dense < sub.dense_clade_to_chain_ref.size();
             ++dense) {
          scratch.inside[dense] = sub.icache->row(
              pattern_index, sub.dense_clade_to_chain_ref[dense]);
        }
        scratch.multifurcation_productions_scored = 0;
        return scratch;
      },
      state.chart_opts);
}

std::unique_ptr<chart_spr_local_commit_substrate>
chart_spr_make_local_commit_substrate(chart_spr_search_state const& state,
                                      chart_spr_search_options const& options) {
  if (state.chart_opts.score_ua_edge) {
    throw std::runtime_error(
        "chart SPR local commit: score_ua_edge=true is not yet supported in "
        "local-commit mode (the persistent outside cache needs a per-pattern "
        "reference state the search state does not own); use "
        "rebuild_after_accept=true or score_ua_edge=false.  This is a labelled "
        "unsupported-mode throw, not a silent fallback to a cheaper mode.");
  }
  auto sub = std::make_unique<chart_spr_local_commit_substrate>();
  // Frozen copy of the initial grammar; the chain and caches reference it for
  // the whole run.
  sub->base_grammar = state.grammar;
  sub->base_execution_plan = state.execution_plan;
  sub->checked_base_execution_plan.emplace(check_chart_execution_plan(
      sub->base_grammar, sub->base_execution_plan));
  sub->chain.emplace(sub->base_grammar);
  auto checked_source =
      check_chart_execution_plan(state.grammar, state.execution_plan);
  auto resident_source = make_inside_chart_cache_resident_source_identity(
      state.grammar, checked_source, state.active_patterns);
  // build_*_chart_cache return by value; their `base` pointer points at the
  // grammar passed in (&sub->base_grammar), which is stable for the run.  The
  // move into the optional copies the pointer, still valid.
  auto const inside_start = std::chrono::steady_clock::now();
  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    auto const pattern_count = state.active_patterns.patterns.patterns.size();
    if (state.pattern_charts.size() != pattern_count) {
      throw std::runtime_error(
          "chart SPR local commit: resident pattern chart count mismatch");
    }
    sub->icache = build_inside_chart_cache_from_resident_inside(
        sub->base_grammar, *sub->checked_base_execution_plan, state.grammar,
        checked_source, resident_source, state.active_patterns,
        state.chart_opts, state.invariant_constant_offset,
        [&](std::size_t pattern_index,
            site_pattern const&) -> single_site_chart const& {
          return state.pattern_charts.at(pattern_index).chart;
        });
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::lazy_multisite_chart) {
    if (!state.lazy_chart) {
      throw std::runtime_error(
          "chart SPR local commit: lazy strategy has no resident lazy chart");
    }
    single_site_chart scratch;
    scratch.inside.resize(state.execution_plan.clades().size());
    sub->icache = build_inside_chart_cache_from_resident_inside(
        sub->base_grammar, *sub->checked_base_execution_plan, state.grammar,
        checked_source, resident_source, state.active_patterns,
        state.chart_opts, state.invariant_constant_offset,
        [&](std::size_t pattern_index,
            site_pattern const&) -> single_site_chart const& {
          for (clade_id clade = 0; clade < state.execution_plan.clades().size();
               ++clade) {
            scratch.inside[clade] =
                state.lazy_chart->inside_row(clade, pattern_index);
          }
          scratch.multifurcation_productions_scored = 0;
          return scratch;
        });
  } else {
    sub->icache = build_inside_chart_cache(
        sub->base_grammar, *sub->checked_base_execution_plan,
        state.active_patterns, state.chart_opts,
        state.invariant_constant_offset);
  }
  sub->inside_cache_initialization_ms =
      chart_spr_elapsed_ms(inside_start, std::chrono::steady_clock::now());
  ++state.counters.chart_execution_plan_cache_hits;
  auto const& inside_build = sub->icache->build_stats;
  auto const active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  if (inside_build.inside_charts_built +
          inside_build.resident_inside_charts_consumed !=
      active_pattern_count) {
    throw std::runtime_error(
        "chart SPR local commit: inside-cache build accounting does not match "
        "the active pattern count");
  }
  record_inside_chart_cache_build_work(
      state.counters, inside_build.inside_charts_built,
      inside_build.resident_inside_charts_consumed);

  auto const outside_start = std::chrono::steady_clock::now();
  sub->ocache = build_outside_chart_cache(
      sub->base_grammar, *sub->checked_base_execution_plan,
      *sub->icache, state.chart_opts);
  sub->outside_cache_initialization_ms =
      chart_spr_elapsed_ms(outside_start, std::chrono::steady_clock::now());
  ++state.counters.chart_execution_plan_cache_hits;
  auto const& outside_build = sub->ocache->build_stats;
  if (outside_build.inside_charts_built != 0 ||
      outside_build.inside_charts_reused != active_pattern_count ||
      outside_build.outside_charts_built != active_pattern_count) {
    throw std::runtime_error(
        "chart SPR local commit: resident-inside outside-cache build violated "
        "the Phase-2B reuse contract");
  }
  state.counters.outside_cache_inside_charts_built +=
      outside_build.inside_charts_built;
  state.counters.outside_cache_inside_charts_reused +=
      outside_build.inside_charts_reused;
  state.counters.outside_cache_outside_charts_built +=
      outside_build.outside_charts_built;
  sub->cache_multifurcation_productions_scored_reported =
      sub->icache->multifurcation_productions_scored +
      sub->ocache->multifurcation_productions_scored;
  state.counters.multifurcation_productions_scored +=
      sub->cache_multifurcation_productions_scored_reported;
  chart_spr_set_identity_tip_maps(*sub);
  chart_spr_publish_local_commit_tip_identity(*sub, state);
  sub->verify_materialized_fixed_topology_oracle_for_tests =
      options.verify_fixed_topology_materialized_oracle_for_tests;
  sub->force_independent_sm_bug_for_tests =
      options.force_fixed_topology_independent_sm_bug_for_tests;
  sub->verify_transient_chain_extension_oracle_for_tests =
      options.verify_transient_chain_extension_oracle_for_tests;
  sub->force_transient_chain_extension_oracle_mismatch_for_tests =
      options.force_transient_chain_extension_oracle_mismatch_for_tests;
  if (options.force_fixed_topology_cache_epoch_mismatch_for_tests) {
    // Test-only corruption of a substrate invariant.  The verifier must throw
    // a hard labelled error instead of returning an invalid candidate.
    sub->icache->commit_epoch = sub->chain->size() + 1;
  }
  return sub;
}

enum class chart_spr_local_commit_outcome {
  committed,
  tombstone_scope_skipped,
};

// Local-commit failures after the committability gate are hard correctness
// errors, not ordinary post-materialization rejections: they would indicate a
// broken chain/cache transaction.  The search loop catches this type
// separately and rethrows it so callers never receive a partially-updated
// result disguised as a rejected move.
class chart_spr_local_commit_hard_error : public std::runtime_error {
 public:
  explicit chart_spr_local_commit_hard_error(std::string message)
      : std::runtime_error(std::move(message)) {}
};

struct chart_spr_local_commit_result {
  chart_spr_local_commit_outcome outcome =
      chart_spr_local_commit_outcome::committed;
  std::string skip_reason;
};

struct chart_spr_lazy_commit_stats {
  std::size_t inside_rows_recomputed = 0;
  std::size_t outside_rows_recomputed = 0;
  std::size_t multifurcation_productions_scored = 0;
};

void chart_spr_recompute_lazy_inside_summary_counters(
    lazy_multisite_chart& chart) {
  chart.lazy_inside_rows_computed = 0;
  chart.lazy_patterns_merged_max = 0;
  chart.lazy_remerge_collisions = 0;
  chart.lazy_structural_class_count_max = 0;
  for (std::size_t clade = 0; clade < chart.inside_rows_by_clade.size();
       ++clade) {
    auto const class_count = chart.inside_rows_by_clade[clade].size();
    chart.lazy_inside_rows_computed += class_count;
    if (class_count <= chart.pattern_count) {
      chart.lazy_patterns_merged_max =
          std::max(chart.lazy_patterns_merged_max,
                   chart.pattern_count - class_count);
    }
    auto structural_count =
        clade < chart.structural_class_count_by_clade.size()
            ? chart.structural_class_count_by_clade[clade]
            : std::size_t{0};
    chart.lazy_structural_class_count_max =
        std::max(chart.lazy_structural_class_count_max, structural_count);
    if (structural_count > class_count) {
      chart.lazy_remerge_collisions += structural_count - class_count;
    }
  }
}

void chart_spr_recompute_lazy_outside_summary_counters(
    lazy_multisite_chart& chart) {
  chart.lazy_outside_rows_computed = 0;
  for (auto const& rows : chart.outside_rows_by_clade) {
    chart.lazy_outside_rows_computed += rows.size();
  }
}

lazy_multisite_chart chart_spr_project_lazy_chart_to_materialized(
    lazy_multisite_chart const& previous,
    overlay_materialization_result const& materialized,
    std::vector<overlay_clade_ref> const& previous_dense_clade_to_ref) {
  if (previous_dense_clade_to_ref.size() !=
      previous.inside_rows_by_clade.size()) {
    throw std::runtime_error(
        "chart SPR lazy local commit: previous dense clade map size mismatch");
  }

  lazy_multisite_chart next;
  auto clade_count = materialized.grammar.clades.size();
  next.inside_rows_by_clade.resize(clade_count);
  next.outside_rows_by_clade.resize(clade_count);
  next.class_index_by_pattern_by_clade.resize(clade_count);
  next.structural_class_index_by_pattern_by_clade.resize(clade_count);
  next.outside_class_index_by_pattern_by_clade.resize(clade_count);
  next.structural_class_count_by_clade.assign(clade_count, 0);
  next.class_weight_by_clade.resize(clade_count);
  next.outside_class_weight_by_clade.resize(clade_count);
  next.outside_global_min_by_pattern = previous.outside_global_min_by_pattern;
  next.pattern_count = previous.pattern_count;
  next.total_pattern_weight = previous.total_pattern_weight;
  next.multifurcation_productions_scored =
      previous.multifurcation_productions_scored;
  next.outside_multifurcation_productions_scored =
      previous.outside_multifurcation_productions_scored;

  std::map<overlay_clade_ref, clade_id> previous_dense_by_ref;
  for (clade_id dense = 0; dense < previous_dense_clade_to_ref.size();
       ++dense) {
    previous_dense_by_ref.emplace(previous_dense_clade_to_ref[dense], dense);
  }

  auto copy_slot = [](auto const& from, auto& to, clade_id old_dense,
                      clade_id new_dense) {
    if (old_dense < from.size() && new_dense < to.size()) {
      to[new_dense] = from[old_dense];
    }
  };

  for (clade_id dense = 0; dense < materialized.dense_clade_to_ref.size();
       ++dense) {
    auto it = previous_dense_by_ref.find(materialized.dense_clade_to_ref[dense]);
    if (it == previous_dense_by_ref.end()) continue;
    auto old_dense = it->second;
    copy_slot(previous.inside_rows_by_clade, next.inside_rows_by_clade,
              old_dense, dense);
    copy_slot(previous.outside_rows_by_clade, next.outside_rows_by_clade,
              old_dense, dense);
    copy_slot(previous.class_index_by_pattern_by_clade,
              next.class_index_by_pattern_by_clade, old_dense, dense);
    copy_slot(previous.structural_class_index_by_pattern_by_clade,
              next.structural_class_index_by_pattern_by_clade, old_dense,
              dense);
    copy_slot(previous.outside_class_index_by_pattern_by_clade,
              next.outside_class_index_by_pattern_by_clade, old_dense, dense);
    copy_slot(previous.structural_class_count_by_clade,
              next.structural_class_count_by_clade, old_dense, dense);
    copy_slot(previous.class_weight_by_clade, next.class_weight_by_clade,
              old_dense, dense);
    copy_slot(previous.outside_class_weight_by_clade,
              next.outside_class_weight_by_clade, old_dense, dense);
  }

  return next;
}

void chart_spr_clear_lazy_inside_clade(lazy_multisite_chart& chart,
                                       clade_id clade) {
  chart.inside_rows_by_clade[clade].clear();
  chart.class_index_by_pattern_by_clade[clade] = std::nullopt;
  chart.structural_class_index_by_pattern_by_clade[clade] = std::nullopt;
  chart.structural_class_count_by_clade[clade] = 0;
  chart.class_weight_by_clade[clade].clear();
}

void chart_spr_clear_lazy_outside_clade(lazy_multisite_chart& chart,
                                        clade_id clade) {
  chart.outside_rows_by_clade[clade].clear();
  chart.outside_class_index_by_pattern_by_clade[clade] = std::nullopt;
  chart.outside_class_weight_by_clade[clade].clear();
}

void chart_spr_initialize_lazy_root_outside(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, chart_options const& options) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "chart SPR lazy local commit: score_ua_edge=true outside refresh "
        "requires a reference-state convention");
  }
  auto root = plan.root_clade();
  if (root == no_clade || root >= plan.clades().size()) {
    throw std::runtime_error(
        "chart SPR lazy local commit: root clade out of range");
  }
  auto root_row = parsimony_chart_detail::make_inf_row();
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    root_row[state] = 0;
  }
  chart.outside_rows_by_clade[root].push_back(root_row);
  chart.outside_class_index_by_pattern_by_clade[root] =
      std::vector<std::size_t>(chart.pattern_count, 0);
  chart.outside_class_weight_by_clade[root].push_back(0);
  chart.outside_global_min_by_pattern.assign(chart.pattern_count, chart_inf);
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    lazy_chart_detail::checked_add_weight(
        chart.outside_class_weight_by_clade[root].front(),
        patterns.patterns[pattern].weight, "root outside class");
    auto const& inside_root = chart.inside_row(root, pattern);
    chart_cost best = chart_inf;
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      best = std::min(best, parsimony_chart_detail::saturated_add(
                                inside_root[state], root_row[state]));
    }
    chart.outside_global_min_by_pattern[pattern] = best;
  }
}

chart_spr_lazy_commit_stats chart_spr_refresh_lazy_chart_after_local_commit(
    chart_spr_search_state& state, overlay_chain const& chain,
    overlay_materialization_result const& materialized,
    chart_execution_plan const& execution_plan,
    std::vector<overlay_clade_ref> const& previous_dense_clade_to_ref) {
  if (state.cache_strategy != chart_spr_cache_strategy::lazy_multisite_chart) {
    return {};
  }
  if (!state.lazy_chart) {
    throw std::runtime_error(
        "chart SPR lazy local commit: missing lazy chart");
  }
  if (state.chart_opts.score_ua_edge) {
    throw std::runtime_error(
        "chart SPR lazy local commit: score_ua_edge=true is not supported");
  }

  chart_spr_lazy_commit_stats stats;
  auto next = chart_spr_project_lazy_chart_to_materialized(
      *state.lazy_chart, materialized, previous_dense_clade_to_ref);
  auto const& patterns = state.active_patterns.patterns;

  lazy_chart_options lazy_options;
  lazy_options.chart = state.chart_opts;
  lazy_options.chart.keep_trace = false;
  lazy_options.chart.max_trace_choices = 0;
  lazy_options.retain_all_inside_class_maps = true;

  auto inside_multifurcation_before =
      next.multifurcation_productions_scored;
  auto inside_affected = compute_chain_inside_affected_set(chain);
  for (auto ref : inside_affected) {
    auto dense = chart_spr_detail::dense_clade_id(materialized, ref);
    chart_spr_clear_lazy_inside_clade(next, dense);
    if (execution_plan.clade(dense).is_leaf()) {
      lazy_chart_detail::assign_plan_leaf_classes(
          next, execution_plan, patterns, dense, lazy_options);
    } else {
      auto keys = lazy_chart_detail::collect_plan_parent_keys(
          next, execution_plan, patterns, dense, nullptr);
      lazy_chart_detail::assign_plan_internal_classes(
          next, execution_plan, patterns, dense, keys);
    }
    stats.inside_rows_recomputed += next.inside_rows_by_clade[dense].size();
  }
  stats.multifurcation_productions_scored +=
      next.multifurcation_productions_scored -
      inside_multifurcation_before;
  chart_spr_recompute_lazy_inside_summary_counters(next);

  auto outside_multifurcation_before =
      next.outside_multifurcation_productions_scored;
  auto outside_affected = compute_chain_outside_affected_set(chain);
  for (auto ref : outside_affected) {
    auto dense = chart_spr_detail::dense_clade_id(materialized, ref);
    chart_spr_clear_lazy_outside_clade(next, dense);
    if (dense == execution_plan.root_clade()) {
      chart_spr_initialize_lazy_root_outside(
          next, execution_plan, patterns, state.chart_opts);
    } else {
      lazy_chart_detail::assign_outside_classes_for_clade(
          next, execution_plan, patterns, dense);
    }
    stats.outside_rows_recomputed +=
        next.outside_rows_by_clade[dense].size();
  }
  stats.multifurcation_productions_scored +=
      next.outside_multifurcation_productions_scored -
      outside_multifurcation_before;
  chart_spr_recompute_lazy_outside_summary_counters(next);

  state.lazy_chart = std::move(next);
  return stats;
}

// Phase 3 two-chart oracle self-check: recompute BOTH charts from scratch on
// the materialized chain and assert the persistent caches agree on every
// reachable clade, every active pattern.  This is the load-bearing guard
// against inside/outside affected-set under-inclusion (the most likely silent
// bug).  Mirrors assert_cache_both_charts_match_from_scratch in the Phase 3
// test; kept in the .cpp so enabling it is a test-only flag.
void chart_spr_assert_local_commit_two_chart_oracle(
    overlay_chain const& chain, inside_chart_cache const& icache,
    outside_chart_cache const& ocache, std::string const& context,
    lazy_multisite_chart const* lazy = nullptr) {
  auto materialized = materialize_overlay_chain(chain);
  auto const& grammar = materialized.grammar;
  if (materialized.dense_clade_to_ref.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "chart SPR local-commit two-chart oracle [" + context +
        "]: dense clade map size mismatch");
  }
  for (std::size_t p = 0; p < icache.patterns.size(); ++p) {
    leaf_site_states states;
    states.state_by_taxon = icache.patterns[p].state_by_taxon;
    auto oracle =
        recompute_both_charts_from_scratch(grammar, states, icache.chart_opts);
    if (oracle.first.inside.size() != materialized.dense_clade_to_ref.size() ||
        oracle.second.outside.size() !=
            materialized.dense_clade_to_ref.size()) {
      throw std::runtime_error(
          "chart SPR local-commit two-chart oracle [" + context +
          "]: oracle chart size mismatch");
    }
    for (std::size_t dense = 0; dense < materialized.dense_clade_to_ref.size();
         ++dense) {
      auto ref = materialized.dense_clade_to_ref[dense];
      if (icache.row(p, ref) != oracle.first.inside[dense]) {
        throw std::runtime_error(
            "chart SPR local-commit two-chart oracle [" + context +
            "]: inside row mismatch at pattern " + std::to_string(p) +
            " dense clade " + std::to_string(dense));
      }
      if (ocache.row(p, ref) != oracle.second.outside[dense]) {
        throw std::runtime_error(
            "chart SPR local-commit two-chart oracle [" + context +
            "]: outside row mismatch at pattern " + std::to_string(p) +
            " dense clade " + std::to_string(dense));
      }
      if (lazy != nullptr &&
          lazy->inside_row(dense, p) != oracle.first.inside[dense]) {
        throw std::runtime_error(
            "chart SPR local-commit two-chart oracle [" + context +
            "]: lazy inside row mismatch at pattern " + std::to_string(p) +
            " dense clade " + std::to_string(dense));
      }
      if (lazy != nullptr &&
          lazy->outside_row(dense, p) != oracle.second.outside[dense]) {
        throw std::runtime_error(
            "chart SPR local-commit two-chart oracle [" + context +
            "]: lazy outside row mismatch at pattern " + std::to_string(p) +
            " dense clade " + std::to_string(dense));
      }
    }
    if (outside_cache_global_min(ocache, icache, p) !=
        oracle.second.global_min) {
      throw std::runtime_error(
          "chart SPR local-commit two-chart oracle [" + context +
          "]: global_min mismatch at pattern " + std::to_string(p));
    }
    if (lazy != nullptr &&
        lazy->outside_global_min(p) != oracle.second.global_min) {
      throw std::runtime_error(
          "chart SPR local-commit two-chart oracle [" + context +
          "]: lazy global_min mismatch at pattern " + std::to_string(p));
    }
  }
}

// Refresh the derived tip view on the state (grammar + pattern_charts +
// composite bounds + size estimates) from the chain + caches, so the next
// iteration's candidate generation and local scoring operate on the chain tip.
// No chart recurrence runs here: pattern_charts is a flat projection of the
// authoritative icache rows keyed by the materialization's dense_clade_to_ref
// map, and the composite lower bound is read from the icache root rows.
void chart_spr_refresh_state_tip_view_after_local_commit(
    chart_spr_search_state& state,
    overlay_materialization_result const& materialized,
    chart_execution_plan next_execution_plan,
    inside_chart_cache const& icache,
    chart_spr_search_counters& counters) {
  auto old_strategy = state.cache_strategy;
  auto const old_execution_generation = state.grammar.execution_generation;
  auto next_grammar = materialized.grammar;
  if (next_grammar.execution_generation == 0 ||
      next_grammar.execution_generation == old_execution_generation) {
    throw std::runtime_error(
        "chart SPR local commit: accepted tip grammar did not publish a fresh "
        "execution generation");
  }
  state.grammar = std::move(next_grammar);
  state.execution_plan = std::move(next_execution_plan);

  state.estimated_full_pattern_cache_bytes =
      estimate_chart_spr_full_pattern_cache_bytes(state);
  state.effective_pattern_batch_size = choose_chart_spr_pattern_batch_size(
      state.grammar, state.active_patterns, state.cache_opts);
  auto new_strategy = choose_chart_spr_cache_strategy(
      state.grammar, state.active_patterns, state.cache_opts);
  // Preserve a caller-requested pattern-batch mode across commits: do not
  // silently switch an explicitly batched run into
  // all-active just because the grammar shrank.  If an all-active run grows or
  // its budget is recalculated into pattern_batches, switch to the batched view
  // before touching resident pattern_charts so stale full rows cannot remain
  // resident or be reported in resident-byte accounting.
  state.cache_strategy =
      old_strategy == chart_spr_cache_strategy::pattern_batches
          ? chart_spr_cache_strategy::pattern_batches
          : new_strategy;

  auto const& patterns = state.active_patterns.patterns.patterns;
  if (patterns.size() != icache.patterns.size()) {
    throw std::runtime_error(
        "chart SPR local-commit tip refresh: active pattern count mismatch");
  }

  if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    std::vector<pattern_chart_cache_entry> refreshed;
    refreshed.reserve(patterns.size());
    for (std::size_t p = 0; p < patterns.size(); ++p) {
      single_site_chart chart;
      chart.inside.assign(materialized.grammar.clades.size(),
                          parsimony_chart_detail::make_inf_row());
      for (std::size_t dense = 0;
           dense < materialized.dense_clade_to_ref.size(); ++dense) {
        chart.inside[dense] =
            icache.row(p, materialized.dense_clade_to_ref[dense]);
      }
      refreshed.push_back(chart_spr_cache_entry_from_chart(
          materialized.grammar, patterns[p], state.chart_opts, std::move(chart)));
    }
    state.pattern_charts = std::move(refreshed);
    state.resident_pattern_cache_bytes =
        estimate_chart_spr_pattern_cache_bytes(state);
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::lazy_multisite_chart) {
    if (!state.lazy_chart) {
      throw std::runtime_error(
          "chart SPR local-commit tip refresh: missing lazy chart");
    }
    if (state.lazy_chart->inside_rows_by_clade.size() !=
        materialized.grammar.clades.size()) {
      throw std::runtime_error(
          "chart SPR local-commit tip refresh: lazy chart clade count "
          "mismatch");
    }
    std::vector<pattern_chart_cache_entry>{}.swap(state.pattern_charts);
    state.resident_pattern_cache_bytes =
        estimate_chart_spr_pattern_cache_bytes(state);
  } else {
    // pattern_batches strategy: there are no resident base rows to refresh; the
    // next scoring batch rebuilds base rows once per pattern batch from the
    // refreshed state.grammar.  Clear any all-active rows left over from the
    // previous strategy so resident-byte reporting matches the strategy switch.
    std::vector<pattern_chart_cache_entry>{}.swap(state.pattern_charts);
    state.resident_pattern_cache_bytes =
        state.effective_pattern_batch_size *
        estimate_chart_spr_pattern_row_cache_bytes(state.grammar);
  }

  std::uint64_t composite_without_invariants = 0;
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    composite_without_invariants = lazy_composite_lower_bound(
        state.execution_plan, state.active_patterns.patterns, *state.lazy_chart,
        state.chart_opts);
    ++counters.chart_execution_plan_cache_hits;
  } else {
    auto composite_with_invariants =
        inside_cache_composite_lower_bound_with_invariants(icache);
    if (icache.invariant_constant_offset > composite_with_invariants) {
      throw std::runtime_error(
          "chart SPR local-commit tip refresh: composite below invariant "
          "offset");
    }
    composite_without_invariants =
        composite_with_invariants - state.invariant_constant_offset;
  }
  state.composite_lower_bound_without_invariants =
      composite_without_invariants;
  state.composite_lower_bound_with_invariants =
      chart_multisite_detail::checked_add_u64(
          composite_without_invariants, state.invariant_constant_offset,
          "chart-SPR local commit lazy lower bound invariant offset");

  // Exact-trim cache: invalidated by the commit (Phase 2 hook); recomputed
  // lazily by the next exact gate.  Leaving it absent here is the WI3
  // lazy-invalidation rule.
  state.exact_trim_active_only.reset();
}

// Commit an accepted candidate to the chain + caches and refresh the state's
// tip view.  Returns the outcome (committed, or a labelled tombstone-scope
// skip).  `counters` is the running attempt-counters the caller snapshots from
// state.counters; the cumulative cache counters are mirrored onto it.
chart_spr_local_commit_result chart_spr_commit_accepted_locally(
    chart_spr_local_commit_substrate& sub, chart_spr_search_state& state,
    chart_spr_candidate_score const& accepted,
    chart_spr_search_options const& options,
    chart_spr_search_counters& counters) {
  // Defensive gate check (the loop validator already rejects this combo, but a
  // locally-committed chain's recorded objective must be exact -- never trust a
  // caller to re-establish the invariant).  This is a hard configuration error,
  // not an ordinary post-materialization rejection.
  if (options.acceptance_mode == chart_spr_acceptance_mode::lower_bound_heuristic) {
    throw chart_spr_local_commit_hard_error(
        "chart SPR local commit: refusing to commit a lower_bound_heuristic-"
        "gated accept; local commit requires an exact gate");
  }

  chart_spr_local_commit_result result;

  // Build the single-candidate delta against the CURRENT tip (state.grammar is
  // the materialized chain tip the candidate was generated/scored against).
  // An accepted candidate that cannot be reconstructed here indicates a broken
  // search invariant, so surface it as a hard local-commit error.  The checked
  // capability is deliberately scoped to this immutable append-payload build;
  // it cannot accidentally authorize work after the chain starts mutating.
  spr_overlay_delta delta = [&]() -> spr_overlay_delta {
    auto checked_state = [&] {
      try {
        return check_chart_execution_plan(state.grammar,
                                          state.execution_plan);
      } catch (std::exception const& e) {
        throw chart_spr_local_commit_hard_error(
            std::string{"chart SPR local commit: stale resident state before "
                        "append: "} +
            e.what());
      }
    }();
    try {
      return chart_spr_build_validated_append_payload(
          state, checked_state, accepted.candidate);
    } catch (std::exception const& e) {
      throw chart_spr_local_commit_hard_error(
          std::string{"chart SPR local commit: failed to build accepted "
                      "candidate delta before commit: "} +
          e.what());
    }
  }();

  // Committability gate (Phase 4 tombstone scope).  The overlay vocabulary has
  // no removed-temp-productions field, so only candidates whose tombstones all
  // resolve to frozen-base productions may commit.  The chain enforces this;
  // a rejection is a labelled, counted skip -- never a silent no-op (matches
  // the no-silent-fallback discipline).  This is the ONLY local-commit failure
  // translated into a normal search outcome.
  //
  // Skip semantics: this skip is reported to the search loop, which TERMINATES
  // the run on it rather than trying the next-best candidate.  The plan's
  // Phase-4 design note chose resolution (a) over (b) (revisiting the overlay
  // vocabulary to admit removed temp productions, or a candidate-fallback
  // loop) -- both deferred unless (a) starves the search on the benchmark
  // fixtures.  So here "skip" means "stop the search," not "try the next
  // candidate."  On fixtures with disjoint committable moves (e.g. the Phase-4
  // three-misplaced-groups fixture) this does not starve the k >= 3 criterion;
  // on inputs where move #1 is non-committable but move #2 is, the run stops
  // early, which is the documented known limitation.
  try {
    sub.chain->append(delta);
  } catch (std::runtime_error const& e) {
    if (chart_spr_is_local_commit_tombstone_scope_rejection(e.what())) {
      result.outcome = chart_spr_local_commit_outcome::tombstone_scope_skipped;
      result.skip_reason = e.what();
      return result;
    }
    throw chart_spr_local_commit_hard_error(
        std::string{"chart SPR local commit: overlay-chain append failed "
                    "outside the tombstone-scope committability gate: "} +
        e.what());
  }

  // From this point on the chain/cache/state update is an in-place commit.  Any
  // exception is a hard correctness failure and MUST NOT be converted by the
  // outer accept-path catch into a post-materialization rejection (there is no
  // rollback path for a partially refreshed chain/cache snapshot).
  try {
    if (options.force_local_commit_post_append_failure_for_tests) {
      throw std::runtime_error(
          "forced local commit post-append failure for tests");
    }

    // Paired cache commit: inside first (Phase 2), then outside (Phase 3).  Both
    // are affected-set scoped -- no full chart rebuild per accept.  The exact-
    // trim cache is invalidated by the inside commit (Phase 2 hook).
    apply_commit_to_inside_cache(*sub.chain, *sub.icache);
    apply_commit_to_outside_cache(*sub.chain, *sub.ocache, *sub.icache);

    // Refresh the derived tip view (grammar + pattern_charts + bounds).  This is
    // a grammar-only materialization (no chart rescoring); NOT counted under
    // full_overlay_materializations.  Eliminating it entirely (direct in-place
    // splice) is Phase 6/7 scope; the persistent caches already remove the
    // expensive per-accept chart rescoring.
    auto previous_dense_clade_to_chain_ref = sub.dense_clade_to_chain_ref;
    planned_overlay_materialization_result planned;
    overlay_payload_validation_stats completed_payload_validation_stats;
    try {
      chart_spr_elapsed_accumulator materialization_timer{
          counters.materialization_accepted_update_ms};
      if (!sub.checked_base_execution_plan) {
        throw std::runtime_error(
            "chart SPR local commit: missing checked frozen-base plan");
      }
      planned = materialize_overlay_chain_with_plan(
          *sub.chain, *sub.checked_base_execution_plan, nullptr,
          [&] { materialization_timer.finish(); },
          &completed_payload_validation_stats);
    } catch (...) {
      record_overlay_payload_validation_stats(
          counters, completed_payload_validation_stats);
      throw;
    }
    record_planned_overlay_materialization_stats(counters, planned);
    auto materialized = std::move(planned.materialized);
    auto next_execution_plan = std::move(planned.execution_plan);
    // The caller's attempt counter is the authoritative snapshot and is
    // copied back onto state after commit.  Record this distinct materialized
    // grammar plan exactly once here, before any consumer reuses it.
    auto const refreshed_lazy_plan =
        state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart;
    auto lazy_stats = chart_spr_refresh_lazy_chart_after_local_commit(
        state, *sub.chain, materialized, next_execution_plan,
        previous_dense_clade_to_chain_ref);
    if (refreshed_lazy_plan) {
      ++counters.chart_execution_plan_cache_hits;
    }
    chart_spr_set_tip_maps_from_materialization(sub, materialized);
    ++counters.local_commit_tip_grammar_refreshes;
    chart_spr_refresh_state_tip_view_after_local_commit(
        state, materialized, std::move(next_execution_plan), *sub.icache,
        counters);
    chart_spr_publish_local_commit_tip_identity(sub, state);

    // Mirror cumulative cache counters onto the running attempt-counters (the
    // caches persist across accepts; their counters are cumulative).
    counters.inside_rows_recomputed_on_commit =
        sub.icache->inside_rows_recomputed_on_commit;
    counters.outside_rows_recomputed_on_commit =
        sub.ocache->outside_rows_recomputed_on_commit;
    counters.lazy_inside_rows_recomputed_on_commit +=
        lazy_stats.inside_rows_recomputed;
    counters.lazy_outside_rows_recomputed_on_commit +=
        lazy_stats.outside_rows_recomputed;
    counters.lazy_incremental_rows_recomputed +=
        lazy_stats.inside_rows_recomputed + lazy_stats.outside_rows_recomputed;
    auto cache_multifurcation_productions_scored =
        sub.icache->multifurcation_productions_scored +
        sub.ocache->multifurcation_productions_scored;
    if (cache_multifurcation_productions_scored <
        sub.cache_multifurcation_productions_scored_reported) {
      throw std::runtime_error(
          "chart SPR local commit: cache multifurcation production counter "
          "moved backwards");
    }
    counters.multifurcation_productions_scored +=
        cache_multifurcation_productions_scored -
        sub.cache_multifurcation_productions_scored_reported +
        lazy_stats.multifurcation_productions_scored;
    sub.cache_multifurcation_productions_scored_reported =
        cache_multifurcation_productions_scored;

    // Two-chart oracle self-check (Work item 3 correctness invariant).
    if (options.verify_local_commit_two_chart_oracle_for_tests) {
      chart_spr_assert_local_commit_two_chart_oracle(
          *sub.chain, *sub.icache, *sub.ocache,
          "after commit " + std::to_string(sub.chain->size()),
          state.lazy_chart ? &*state.lazy_chart : nullptr);
      ++counters.local_commit_two_chart_oracle_runs;
    }
  } catch (std::exception const& e) {
    throw chart_spr_local_commit_hard_error(
        std::string{"chart SPR local commit: chain/cache update failed after "
                    "the append committed; aborting rather than returning a "
                    "post-materialization rejection with mutated state: "} +
        e.what());
  }

  result.outcome = chart_spr_local_commit_outcome::committed;
  return result;
}

void chart_spr_refresh_search_summary_from_counters(
    chart_spr_search_summary& summary,
    chart_spr_search_counters const& counters) {
  summary.accepted_moves = counters.accepted_moves;
  summary.candidates_locally_scored = counters.local_candidate_scores;
  summary.local_rows_recomputed = counters.local_rows_recomputed;
  summary.local_unit_fitch_fast_path_productions_scored =
      counters.local_unit_fitch_fast_path_productions_scored;
  summary.local_leaf_state_view_uses = counters.local_leaf_state_view_uses;
  summary.local_leaf_state_owned_copies =
      counters.local_leaf_state_owned_copies;
  summary.local_row_scratch_capacity_growths =
      counters.local_row_scratch_capacity_growths;
  summary.multifurcation_productions_scored =
      counters.multifurcation_productions_scored;
  summary.candidate_batches_scored = counters.candidate_batches_scored;
  summary.pattern_batch_cache_builds = counters.pattern_batch_cache_builds;
  summary.initial_state_inside_charts_built =
      counters.initial_state_inside_charts_built;
  summary.inside_cache_inside_charts_built =
      counters.inside_cache_inside_charts_built;
  summary.inside_cache_resident_inside_charts_consumed =
      counters.inside_cache_resident_inside_charts_consumed;
  summary.exact_setup_builds = counters.exact_setup_builds;
  summary.exact_setup_inside_charts_built =
      counters.exact_setup_inside_charts_built;
  summary.exact_setup_resident_inside_charts_consumed =
      counters.exact_setup_resident_inside_charts_consumed;
  summary.exact_setup_active_leaf_state_vectors_copied =
      counters.exact_setup_active_leaf_state_vectors_copied;
  summary.exact_setup_active_leaf_states_copied =
      counters.exact_setup_active_leaf_states_copied;
  summary.exact_setup_outside_boundary_charts_built =
      counters.exact_setup_outside_boundary_charts_built;
  summary.exact_setup_upper_bound_topologies_generated =
      counters.exact_setup_upper_bound_topologies_generated;
  summary.exact_setup_upper_bound_topologies_unique =
      counters.exact_setup_upper_bound_topologies_unique;
  summary.exact_setup_frontier_passes =
      counters.exact_setup_frontier_passes;
  summary.exact_trim_lazy_chart_uses = counters.exact_trim_lazy_chart_uses;
  summary.outside_cache_inside_charts_built =
      counters.outside_cache_inside_charts_built;
  summary.outside_cache_inside_charts_reused =
      counters.outside_cache_inside_charts_reused;
  summary.outside_cache_outside_charts_built =
      counters.outside_cache_outside_charts_built;
  summary.chart_execution_plan_builds = counters.chart_execution_plan_builds;
  summary.chart_execution_plan_cache_hits =
      counters.chart_execution_plan_cache_hits;
  summary.candidate_execution_plan_builds =
      counters.candidate_execution_plan_builds;
  summary.candidate_execution_plan_cache_hits =
      counters.candidate_execution_plan_cache_hits;
  summary.full_grammar_validations = counters.full_grammar_validations;
  summary.production_index_validations = counters.production_index_validations;
  summary.production_partition_validations =
      counters.production_partition_validations;
  summary.dynamic_overlay_payload_partition_validations =
      counters.dynamic_overlay_payload_partition_validations;
  summary.candidate_partition_validations =
      counters.candidate_partition_validations;
  summary.clade_order_sorts = counters.clade_order_sorts;
  summary.production_descriptors_compiled =
      counters.production_descriptors_compiled;
  summary.plan_mismatch_rejections = counters.plan_mismatch_rejections;
  summary.candidate_pattern_full_grammar_validations =
      counters.candidate_pattern_full_grammar_validations;
  summary.candidate_pattern_partition_validations =
      counters.candidate_pattern_partition_validations;
  summary.candidate_pattern_clade_order_sorts =
      counters.candidate_pattern_clade_order_sorts;
  summary.exact_verifications = counters.exact_verifications;
  summary.overlay_materializations_for_exact_verification =
      counters.overlay_materializations_for_exact_verification;
  summary.overlay_materializations_for_accept_materialization =
      counters.overlay_materializations_for_accept_materialization;
  summary.overlay_materializations_for_final_compaction =
      counters.overlay_materializations_for_final_compaction;
  summary.materialization_exact_verification_ms =
      counters.materialization_exact_verification_ms;
  summary.materialization_accepted_update_ms =
      counters.materialization_accepted_update_ms;
  summary.materialization_final_compaction_ms =
      counters.materialization_final_compaction_ms;
  summary.materialization_ms =
      summary.materialization_exact_verification_ms +
      summary.materialization_accepted_update_ms +
      summary.materialization_final_compaction_ms;
  summary.sidecar_rebuilds_after_accept =
      counters.sidecar_rebuilds_after_accept;
  summary.candidate_accepts_attempted = counters.candidate_accepts_attempted;
  summary.post_materialization_rejections =
      counters.post_materialization_rejections;
  summary.local_commit_accepted_moves = counters.local_commit_accepted_moves;
  summary.local_commit_tombstone_scope_skips =
      counters.local_commit_tombstone_scope_skips;
  summary.inside_rows_recomputed_on_commit =
      counters.inside_rows_recomputed_on_commit;
  summary.outside_rows_recomputed_on_commit =
      counters.outside_rows_recomputed_on_commit;
  summary.lazy_inside_rows_computed = counters.lazy_inside_rows_computed;
  summary.lazy_outside_rows_computed = counters.lazy_outside_rows_computed;
  summary.lazy_patterns_merged_max = counters.lazy_patterns_merged_max;
  summary.lazy_remerge_collisions = counters.lazy_remerge_collisions;
  summary.lazy_inside_rows_recomputed_on_commit =
      counters.lazy_inside_rows_recomputed_on_commit;
  summary.lazy_outside_rows_recomputed_on_commit =
      counters.lazy_outside_rows_recomputed_on_commit;
  summary.lazy_incremental_rows_recomputed =
      counters.lazy_incremental_rows_recomputed;
  summary.lazy_structural_class_count_max =
      counters.lazy_structural_class_count_max;
  summary.local_commit_two_chart_oracle_runs =
      counters.local_commit_two_chart_oracle_runs;
  summary.local_commit_tip_grammar_refreshes =
      counters.local_commit_tip_grammar_refreshes;
  summary.fixed_topology_selected_cache_hits =
      counters.fixed_topology_selected_cache_hits;
  summary.fixed_topology_selected_cache_misses =
      counters.fixed_topology_selected_cache_misses;
  summary.fixed_topology_selected_rows_computed =
      counters.fixed_topology_selected_rows_computed;
  summary.selected_topology_class_rows_computed =
      counters.selected_topology_class_rows_computed;
  summary.selected_topology_multifurcation_rows =
      counters.selected_topology_multifurcation_rows;
  summary.spr_multifurcation_moves_generated =
      counters.spr_multifurcation_moves_generated;
  summary.fixed_topology_persistent_cache_verifications =
      counters.fixed_topology_persistent_cache_verifications;
  summary.fixed_topology_persistent_cache_fallbacks =
      counters.fixed_topology_persistent_cache_fallbacks;
  summary.fixed_topology_persistent_cache_oracle_mismatches =
      counters.fixed_topology_persistent_cache_oracle_mismatches;
  summary.fixed_topology_persistent_cache_direct_oracle_mismatches =
      counters.fixed_topology_persistent_cache_direct_oracle_mismatches;
  summary.fixed_topology_icache_rows_reused =
      counters.fixed_topology_icache_rows_reused;
  summary.fixed_topology_icache_rows_recomputed_affected =
      counters.fixed_topology_icache_rows_recomputed_affected;
  summary.fixed_topology_chain_objective_before_mismatches =
      counters.fixed_topology_chain_objective_before_mismatches;
  summary.transient_chain_extensions_for_verification =
      counters.transient_chain_extensions_for_verification;
  summary.transient_chain_diagnostic_cache_extensions =
      counters.transient_chain_diagnostic_cache_extensions;
  summary.transient_chain_extension_fallbacks =
      counters.transient_chain_extension_fallbacks;
  summary.transient_chain_extension_oracle_mismatches =
      counters.transient_chain_extension_oracle_mismatches;
  summary.full_search_state_rebuilds =
      summary.initial_search_state_rebuilds +
      summary.sidecar_rebuilds_after_accept;
}

void chart_spr_refresh_search_summary_from_current_lazy_chart(
    chart_spr_search_summary& summary, chart_spr_search_state const& state) {
  auto pattern_count = state.active_patterns.patterns.patterns.size();
  auto clade_count = state.grammar.clades.size();
  summary.lazy_merge_ratio = 0.0;
  summary.lazy_internal_structural_class_ratio = 0.0;
  summary.lazy_internal_structural_class_count_max = 0;
  if (state.cache_strategy != chart_spr_cache_strategy::lazy_multisite_chart ||
      !state.lazy_chart) {
    summary.lazy_inside_rows_computed = 0;
    summary.lazy_outside_rows_computed = 0;
    summary.lazy_patterns_merged_max = 0;
    summary.lazy_remerge_collisions = 0;
    summary.lazy_structural_class_count_max = 0;
    return;
  }

  auto const& lazy = *state.lazy_chart;
  summary.lazy_inside_rows_computed = lazy.lazy_inside_rows_computed;
  summary.lazy_outside_rows_computed = lazy.lazy_outside_rows_computed;
  summary.lazy_patterns_merged_max = lazy.lazy_patterns_merged_max;
  summary.lazy_remerge_collisions = lazy.lazy_remerge_collisions;
  summary.lazy_structural_class_count_max =
      lazy.lazy_structural_class_count_max;
  for (std::size_t clade = 0;
       clade < lazy.structural_class_count_by_clade.size() &&
       clade < state.grammar.clades.size();
       ++clade) {
    auto taxon_count = state.grammar.clades[clade].taxa.size();
    if (clade == state.grammar.root_clade || taxon_count <= 1) continue;
    summary.lazy_internal_structural_class_count_max =
        std::max(summary.lazy_internal_structural_class_count_max,
                 lazy.structural_class_count_by_clade[clade]);
  }
  auto denominator =
      static_cast<double>(pattern_count) * static_cast<double>(clade_count);
  if (denominator != 0.0) {
    summary.lazy_merge_ratio =
        static_cast<double>(summary.lazy_inside_rows_computed) / denominator;
  }
  if (pattern_count != 0) {
    summary.lazy_internal_structural_class_ratio =
        static_cast<double>(summary.lazy_internal_structural_class_count_max) /
        static_cast<double>(pattern_count);
  }
}

}  // namespace

chart_spr_fixed_topology_pattern_scores
fixed_topology_selected_cache_pattern_scores_for_tests(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "fixed_topology_exact selected-topology cache test helper requires a "
        "complete topology certificate");
  }
  auto const& certificate = *candidate.topology_selection.certificate;
  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate, certificate);

  chart_spr_selected_topology_row_cache cache;
  chart_spr_persistent_inside_cache_view icache_view;
  auto roots = chart_spr_selected_topology_root_entries_from_cache(
      cache, state, candidate, icache_view);
  if (roots.before == nullptr || roots.after == nullptr) {
    throw chart_spr_fixed_topology_cache_invariant_error(
        "fixed_topology_exact selected-topology cache test helper: root cache "
        "entry missing");
  }

  chart_spr_fixed_topology_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto old_score = chart_spr_weighted_root_score_from_row(
        roots.before->rows_by_pattern[p], active[p], state.chart_opts);
    auto new_score = chart_spr_weighted_root_score_from_row(
        roots.after->rows_by_pattern[p], active[p], state.chart_opts);
    scores.old_pattern_scores.push_back(old_score);
    scores.new_pattern_scores.push_back(new_score);
    scores.old_active_total = chart_multisite_detail::checked_add_u64(
        scores.old_active_total, old_score,
        "fixed_topology_exact selected cache test old active total");
    scores.new_active_total = chart_multisite_detail::checked_add_u64(
        scores.new_active_total, new_score,
        "fixed_topology_exact selected cache test new active total");
  }
  return scores;
}

chart_spr_search_result run_chart_spr_search(
    phylo_dag initial_dag, clade_grammar initial_grammar,
    chart_spr_search_options options) {
  validate_chart_spr_search_loop_options(options);

  auto total_start = std::chrono::steady_clock::now();
  chart_spr_search_result result;
  result.dag = std::move(initial_dag);
  result.summary.acceptance_mode = options.acceptance_mode;
  result.summary.candidate_selection = options.candidate_selection;
  // Phase 10 cross-cutting surface: mirror the selected commit / verification
  // modes and the chain's per-accept exactness label into the summary so the
  // report carries the contracted mode labels.  The per-accept label equals
  // the acceptance mode for local-commit runs (fixed_topology_exact /
  // exact_multisite); for the conservative materialize-rebuild path it is the
  // constant `none_conservative_materialize_rebuild` regardless of acceptance
  // mode, because there is no overlay chain and therefore no per-accept chain
  // exactness to report.  (This label is the chain's per-accept label, not the
  // objective's exactness kind: a lower_bound_heuristic gate still reports its
  // score with kind composite_lower_bound via chart_spr_score_kind; it is
  // simply never admitted to local commit -- see
  // validate_chart_spr_search_loop_options.)  `chart_spr_acceptance_mode_name`
  // is declared in the header this translation unit already includes.
  result.summary.commit_mode = options.commit_mode;
  result.summary.verification_mode = options.verification_mode;
  result.summary.chain_per_accept_exactness_label =
      options.rebuild_after_accept
          ? std::string{"none_conservative_materialize_rebuild"}
          : std::string{chart_spr_acceptance_mode_name(options.acceptance_mode)};
  result.summary.initial_search_state_rebuilds = 1;

  auto cache_start = std::chrono::steady_clock::now();
  auto active_build =
      make_active_search_patterns(result.dag, initial_grammar, options.chart);
  chart_spr_search_detail::chart_spr_state_build_policy state_build_policy;
  state_build_policy.defer_pattern_batch_bootstrap_to_local_cache =
      !options.rebuild_after_accept;
  auto const build_exact_during_state_publication =
      options.rebuild_after_accept &&
      options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite;
  auto state = build_chart_spr_search_state_from_active(
      result.dag, std::move(initial_grammar), std::move(active_build),
      options.chart, build_exact_during_state_publication, options.exact_trim,
      options.cache, state_build_policy);
  ++state.counters.pattern_rebuilds;
  ++state.counters.grammar_rebuilds;
  result.summary.cache_build_ms = chart_spr_elapsed_ms(
      cache_start, std::chrono::steady_clock::now());

  // Build the local substrate before the first exact score. Pattern-batch
  // local mode deliberately publishes a two-stage state so this inside cache
  // is the sole owner of the initial dense recurrence. The installed provider
  // projects current-tip rows into an owning exact setup after validating the
  // chain/cache/plan publication stamp.
  std::unique_ptr<chart_spr_local_commit_substrate> local_commit_substrate;
  if (!options.rebuild_after_accept) {
    local_commit_substrate =
        chart_spr_make_local_commit_substrate(state, options);
    auto* substrate_ptr = local_commit_substrate.get();
    state.exact_setup_provider =
        [substrate_ptr](chart_spr_search_state const& provider_state,
                        checked_chart_execution_plan_ref const& checked_state) {
          return chart_spr_build_exact_setup_from_persistent_inside_cache(
              *substrate_ptr, provider_state, checked_state);
        };
    if (state.pattern_batch_bootstrap_deferred) {
      chart_spr_search_detail::finalize_deferred_pattern_batch_bootstrap(
          state, inside_cache_composite_lower_bound_with_invariants(
                     *local_commit_substrate->icache));
    }
    result.summary.local_inside_cache_initialization_ms =
        local_commit_substrate->inside_cache_initialization_ms;
    result.summary.local_outside_cache_initialization_ms =
        local_commit_substrate->outside_cache_initialization_ms;

    if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
      auto const exact_start = std::chrono::steady_clock::now();
      (void)ensure_chart_spr_state_exact_trim(state, options.exact_trim);
      state.exact_initialization_ms +=
          chart_spr_elapsed_ms(exact_start, std::chrono::steady_clock::now());
    }

    if (state.cache_strategy !=
        chart_spr_cache_strategy::lazy_multisite_chart) {
      auto const pattern_count = state.active_patterns.patterns.patterns.size();
      auto const recurrence_builds =
          state.counters.initial_state_inside_charts_built +
          state.counters.exact_setup_inside_charts_built +
          state.counters.inside_cache_inside_charts_built;
      if (recurrence_builds != pattern_count) {
        throw std::runtime_error(
            "chart SPR local commit: initial dense inside charts were not "
            "built exactly once per active pattern");
      }
    }
  }
  result.summary.initial_chart_construction_ms =
      state.chart_construction_ms;
  result.summary.exact_initialization_ms = state.exact_initialization_ms;
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
  result.summary.cache_strategy = state.cache_strategy;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
  result.summary.requested_worker_count =
      chart_spr_search_detail::requested_chart_spr_worker_count(options);
  result.summary.resolved_worker_count =
      chart_spr_search_detail::normalize_chart_spr_worker_count(
          result.summary.requested_worker_count);
  result.summary.local_score_worker_count =
      result.summary.resolved_worker_count;
  chart_spr_refresh_search_summary_from_current_lazy_chart(result.summary,
                                                          state);
  if (options.semantic_capture != chart_spr_semantic_capture_mode::off) {
    chart_spr_canonical_report canonical;
    canonical.capture_mode = options.semantic_capture;
    auto& contract = canonical.contract;
    contract.acceptance =
        chart_spr_acceptance_mode_name(options.acceptance_mode);
    switch (options.acceptance_mode) {
      case chart_spr_acceptance_mode::exact_multisite:
        contract.objective = "grammar_exact";
        break;
      case chart_spr_acceptance_mode::fixed_topology_exact:
        contract.objective = "fixed_topology_exact";
        break;
      case chart_spr_acceptance_mode::lower_bound_heuristic:
        contract.objective = "composite_lower_bound_heuristic";
        break;
    }
    contract.candidate_selection =
        chart_spr_candidate_selection_mode_name(options.candidate_selection);
    contract.candidate_source =
        chart_spr_candidate_source_name(options.enumeration.source);
    contract.topology_selection =
        options.acceptance_mode ==
                chart_spr_acceptance_mode::fixed_topology_exact
            ? "deterministic_selector:" +
                  options.fixed_topology_selector_name
            : "none";
    contract.commit_mode = chart_spr_commit_mode_name(options.commit_mode);
    contract.accepted_state_update =
        options.rebuild_after_accept ? "materialize_rebuild"
                                     : "overlay_chain_local_commit";
    contract.verification_mode =
        chart_spr_verification_mode_name(options.verification_mode);
    contract.chain_per_accept_exactness =
        result.summary.chain_per_accept_exactness_label;
    contract.score_convention =
        "active_cache_plus_single_invariant_offset";
    contract.dominance_mode =
        multisite_dominance_mode_name(options.exact_trim.dominance_mode);
    contract.keep_mask_contract = options.exact_trim.require_exact_keep_mask
                                      ? "exact_required"
                                      : "score_only_allowed";
    contract.polytomy_mode = options.semantic_polytomy_mode;
    contract.refinement_exactness = options.semantic_refinement_exactness;
    contract.candidate_cap_semantics =
        options.enumeration.max_candidates_is_post_dedup ? "post_dedup"
                                                         : "pre_dedup";
    contract.max_iterations = options.max_iterations;
    contract.max_candidates = options.max_candidates_per_iteration != 0
                                  ? options.max_candidates_per_iteration
                                  : options.enumeration.max_candidates;
    contract.top_k_exact = options.top_k_exact_verify;
    contract.seed = options.seed;
    contract.score_ua_edge = options.chart.score_ua_edge;
    contract.use_bound_pruning = options.exact_trim.use_bound_pruning;
    contract.require_exact_keep_mask =
        options.exact_trim.require_exact_keep_mask;
    contract.randomize_order = options.enumeration.randomize_order;
    contract.reservoir_sample = options.enumeration.reservoir_sample;
    contract.include_immediate_reversals =
        options.enumeration.include_immediate_reversal_candidates;
    contract.include_root_moves = options.enumeration.include_root_moves;
    contract.include_neutral_or_reversal_candidates =
        options.enumeration.include_neutral_or_reversal_candidates;
    contract.sampled_tree_count = options.enumeration.sampled_tree_count;
    contract.sampled_tree_radius =
        options.enumeration.sampled_tree_spr_radius;
    contract.sampled_tree_score_threshold =
        options.enumeration.sampled_tree_score_threshold;
    contract.max_upward_path_expansions =
        options.enumeration.max_upward_path_expansions;
    contract.max_path_pairs =
        options.enumeration.max_path_pairs_considered;
    contract.min_moved_clade_size =
        options.enumeration.min_moved_clade_size;
    contract.max_moved_clade_size =
        options.enumeration.max_moved_clade_size;
    contract.min_target_clade_size =
        options.enumeration.min_target_clade_size;
    contract.max_target_clade_size =
        options.enumeration.max_target_clade_size;
    contract.max_affected_clades =
        options.enumeration.max_estimated_affected_clades;
    contract.max_frontier_entries =
        options.exact_trim.max_frontier_entries_per_clade;
    contract.polytomy_max_exact_arity =
        options.semantic_polytomy_max_exact_arity;
    contract.polytomy_max_shapes = options.semantic_polytomy_max_shapes;
    contract.polytomy_max_productions =
        options.semantic_polytomy_max_productions;
    contract.polytomy_max_clades = options.semantic_polytomy_max_clades;
    canonical.active_pattern_count =
        state.active_patterns.patterns.patterns.size();
    canonical.skipped_invariant_site_count =
        state.skipped_invariant_site_count;
    canonical.invariant_constant_offset = state.invariant_constant_offset;
    canonical.initial_score = result.summary.initial_score;
    canonical.chain_base_production_keys =
        chart_spr_canonical_grammar_production_keys(state.grammar);
    result.canonical_report = std::move(canonical);
  }
  std::vector<std::size_t> aggregate_affected_counts;
  std::optional<chart_spr_candidate_score> last_local_update_accepted;
  std::optional<chart_spr_recorded_chain_objective>
      last_local_update_recorded_objective;
  std::vector<std::vector<rank3_production_taxa_key>>
      local_update_accepted_topology_key_sets;
  bool used_local_accept_updates = false;

  // Finish installing candidate verifiers on the substrate built before the
  // initial exact score. The accept path commits to it instead of
  // dense-materializing per accept; state.grammar/pattern_charts remain a
  // derived current-tip view.
  if (local_commit_substrate != nullptr) {
    auto* substrate_ptr = local_commit_substrate.get();
    state.fixed_topology_exact_verifier =
        [substrate_ptr](chart_spr_search_state const& verifier_state,
                        chart_spr_candidate_score candidate) {
          return chart_spr_verify_candidate_fixed_topology_exact_from_persistent_cache(
              *substrate_ptr, verifier_state, std::move(candidate));
        };
    // Phase 9: install the transient-extension exact_multisite verifier.  It
    // verifies each candidate by transiently extending the chain in
    // reader-local scratch (never mutating the shared cache, bypassing the
    // Phase 4 commit barrier), reading the exact frontier on the extended
    // grammar, and discarding.  The work is counted under
    // `transient_chain_extensions_for_verification`, never under
    // `full_overlay_materializations`.
    //
    // Scratch caches are constructed only for the opt-in two-chart diagnostic;
    // production B&B consumes the materialized extended grammar alone.
    //
    // Phase 10 (verification-mode choice): the transient verifier is the
    // default, but a caller may select `chart_spr_verification_mode::cold` to
    // force the from-scratch `verify_candidate_exact_against_state` path
    // (a dense materialization per verified candidate, counted under
    // `overlay_materializations_for_exact_verification`).  This is the named
    // verification-mode choice the Phase-10 report surfaces.
    if (options.verification_mode ==
        chart_spr_verification_mode::transient) {
      state.exact_multisite_verifier =
          [substrate_ptr](chart_spr_search_state const& verifier_state,
                          chart_spr_candidate_score candidate,
                          checked_chart_execution_plan_ref const& checked_state,
                          multisite_trim_options const& trim_options) {
            return chart_spr_verify_candidate_exact_multisite_from_transient_extension(
                *substrate_ptr, verifier_state, std::move(candidate),
                checked_state, trim_options);
          };
    }
  }

  chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      acceptance_workspace;
  std::string immediate_reversal_key_to_skip;
  for (std::size_t iter = 0; iter < options.max_iterations; ++iter) {
    auto iteration_options = options;
    iteration_options.enumeration.immediate_reversal_candidate_key_to_skip =
        immediate_reversal_key_to_skip;
    auto iteration_seed = options.seed + static_cast<std::uint32_t>(iter);
    iteration_options.seed = iteration_seed;
    iteration_options.enumeration.seed = iteration_seed;
    auto iteration = run_chart_spr_acceptance_iteration(
        state, iteration_options, iter, acceptance_workspace);
    result.summary.candidates_generated += iteration.candidates_generated;
    result.summary.candidate_generation_ms +=
        iteration.candidate_generation_ms;
    result.summary.local_scoring_ms += iteration.local_scoring_ms;
    result.summary.exact_verification_ms += iteration.exact_verification_ms;
    for (double candidate_ms :
         iteration.exact_candidate_verification_ms) {
      if (result.summary.exact_candidate_timing_count == 0) {
        result.summary.exact_candidate_verification_ms_min = candidate_ms;
        result.summary.exact_candidate_verification_ms_max = candidate_ms;
      } else {
        result.summary.exact_candidate_verification_ms_min = std::min(
            result.summary.exact_candidate_verification_ms_min,
            candidate_ms);
        result.summary.exact_candidate_verification_ms_max = std::max(
            result.summary.exact_candidate_verification_ms_max,
            candidate_ms);
      }
      ++result.summary.exact_candidate_timing_count;
    }
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
    bool local_commit_mutated_shared_state = false;
    try {
      if (options.rebuild_after_accept) {
        rank3_option_b_result materialized;
        {
          chart_spr_elapsed_accumulator materialization_timer{
              attempt_counters.materialization_accepted_update_ms};
          materialized = materialize_chart_spr_accepted_candidate(
              state, *iteration.accepted);
        }
        ++attempt_counters.full_overlay_materializations;
        ++attempt_counters.overlay_materializations_for_accept_materialization;
        ++attempt_counters.grammar_rebuilds;
        ++attempt_counters.sidecar_rebuilds_after_accept;

        bool reused_patterns = false;
        auto tentative_state = rebuild_chart_spr_search_state_after_accept(
            state, materialized.dag, std::move(materialized.rebuilt.grammar),
            options, reused_patterns);
        result.summary.exact_initialization_ms +=
            tentative_state.exact_initialization_ms;
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
        // Phase 4: commit the accepted move to the overlay chain + persistent
        // inside/outside caches (Work items 1 + 3) instead of dense-
        // materializing per accept.  The post-accept objective is the accepted
        // candidate's exact score (verified improving by the acceptance gate);
        // it is checked BEFORE the commit so a forced-worsening test hook can
        // reject without advancing the chain.
        auto check_start = std::chrono::steady_clock::now();
        auto rebuilt_score = chart_spr_local_accept_update_post_score(
            state, options, *iteration.accepted);
        if (options.override_post_materialization_rebuilt_score_for_tests) {
          rebuilt_score =
              *options.override_post_materialization_rebuilt_score_for_tests;
        }
        result.summary.post_materialization_check_ms += chart_spr_elapsed_ms(
            check_start, std::chrono::steady_clock::now());
        iteration.post_materialization_rebuilt_score = rebuilt_score;
        iteration.reused_patterns_after_accept = true;

        // Phase 8 (Work item 1 exactness contract): for fixed_topology_exact
        // local commit, the gate baseline is the recorded chain objective
        // (the previous accepted after-topology score), NOT the candidate's
        // own selected before-topology score.  Sequential commits must be
        // monotone against the chain objective; comparing against the
        // candidate's selected before-topology could accept a move whose
        // before-topology is cheaper than the chain tip and mask a real
        // regression.  When the candidate's selected before-topology score
        // does not equal the chain objective it is recorded as a diagnostic
        // (the before certificate is not the chain tip's topology), not a
        // hard error -- the gate still uses the chain objective.
        std::uint64_t local_commit_gate_baseline =
            iteration.state_score_before;
        if (options.acceptance_mode ==
                chart_spr_acceptance_mode::fixed_topology_exact &&
            last_local_update_recorded_objective) {
          local_commit_gate_baseline =
              last_local_update_recorded_objective->value;
          iteration.state_score_before = local_commit_gate_baseline;
          if (iteration.accepted->exact &&
              iteration.accepted->exact->value.old_score !=
                  last_local_update_recorded_objective->value) {
            ++attempt_counters.fixed_topology_chain_objective_before_mismatches;
          }
        }

        if (rebuilt_score > local_commit_gate_baseline) {
          ++attempt_counters.post_materialization_rejections;
          state.counters = attempt_counters;
          iteration.post_materialization_rejected = true;
          iteration.accepted_move_committed = false;
          iteration.state_score_after = local_commit_gate_baseline;
          iteration.no_accept_reason =
              "local commit objective worsened";
          iteration.post_materialization_rejection_reason =
              "locally committed objective " + std::to_string(rebuilt_score) +
              " exceeds chain objective " +
              std::to_string(local_commit_gate_baseline);
          result.summary.final_score = iteration.state_score_after;
          result.iterations.push_back(std::move(iteration));
          break;
        }

        // The immediate-reversal skip key must be computed against the same
        // PRE-COMMIT grammar that generated/scored the candidate.  The local
        // commit refreshes state.grammar in place; using the refreshed grammar
        // would interpret the candidate's dense production IDs in the wrong
        // grammar (or throw) after the chain/cache mutation.
        std::string accepted_immediate_reversal_key;
        try {
          accepted_immediate_reversal_key =
              chart_spr_candidate_immediate_reverse_key(
                  state.grammar, iteration.accepted->candidate);
        } catch (std::exception const& e) {
          throw chart_spr_local_commit_hard_error(
              std::string{"chart SPR local commit: failed to compute "
                          "pre-commit immediate-reversal key for accepted "
                          "candidate: "} +
              e.what());
        }

        // Commit to the chain + caches.  Refreshes state.grammar /
        // state.pattern_charts in place (the derived tip view).  A
        // tombstone-scope skip leaves the chain, caches, and state pristine.
        auto commit = chart_spr_commit_accepted_locally(
            *local_commit_substrate, state, *iteration.accepted, options,
            attempt_counters);
        result.summary.accepted_rebuild_ms += chart_spr_elapsed_ms(
            materialize_start, std::chrono::steady_clock::now());

        if (commit.outcome ==
            chart_spr_local_commit_outcome::tombstone_scope_skipped) {
          // Phase 4 tombstone-scope gate: the accepted (best) candidate's
          // tombstones do not all resolve to frozen-base productions.  This is
          // a labelled, counted skip that TERMINATES the search rather than
          // trying the next-best candidate (resolution (a); candidate-fallback
          // / overlay-vocabulary extension (b) is deferred -- see the design
          // note on chart_spr_commit_accepted_locally).  The chain, caches,
          // and state are pristine (the append threw before mutating).
          ++attempt_counters.local_commit_tombstone_scope_skips;
          state.counters = attempt_counters;
          iteration.post_materialization_rejected = true;
          iteration.accepted_move_committed = false;
          iteration.state_score_after = iteration.state_score_before;
          iteration.no_accept_reason =
              "local commit skipped (terminates search): accepted candidate "
              "tombstones a production that does not resolve to a frozen-base "
              "production (Phase 4 tombstone-scope gate; the search stops "
              "rather than trying the next-best candidate -- resolution (b), "
              "candidate fallback / admitting removed temp productions, is "
              "deferred; direct temp-production removal is Phase 6/7 scope)";
          iteration.post_materialization_rejection_reason = commit.skip_reason;
          result.summary.final_score = iteration.state_score_after;
          result.iterations.push_back(std::move(iteration));
          break;
        }

        local_commit_mutated_shared_state = true;
        immediate_reversal_key_to_skip = accepted_immediate_reversal_key;
        ++attempt_counters.local_commit_accepted_moves;
        ++attempt_counters.accepted_moves;
        // state.grammar / pattern_charts / bounds were refreshed in place by
        // the commit; sync the counters.
        state.counters = attempt_counters;
        local_update_accepted_topology_key_sets.push_back(
            chart_spr_collect_accepted_topology_key_set_after_local_commit(
                state, options, *iteration.accepted));
        last_local_update_recorded_objective =
            chart_spr_recorded_chain_objective_from_accept(
                state, options, *iteration.accepted);
        last_local_update_accepted = *iteration.accepted;
        used_local_accept_updates = true;
        iteration.accepted_move_committed = true;
        iteration.state_score_after = rebuilt_score;
        result.summary.final_score = rebuilt_score;
        result.iterations.push_back(std::move(iteration));
      }
    } catch (chart_spr_local_commit_hard_error const&) {
      throw;
    } catch (std::exception const& e) {
      if (local_commit_mutated_shared_state) {
        throw chart_spr_local_commit_hard_error(
            std::string{"chart SPR local commit: post-commit bookkeeping "
                        "failed after the shared chain/cache/state was "
                        "mutated; aborting rather than returning an ordinary "
                        "post-materialization rejection: "} +
            e.what());
      }
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
        result.dag, *local_commit_substrate->chain, state, options,
        local_update_accepted_topology_key_sets,
        last_local_update_accepted ? &*last_local_update_accepted : nullptr,
        last_local_update_recorded_objective
            ? &*last_local_update_recorded_objective
            : nullptr,
        preserved_counters);
    result.dag = std::move(compacted.dag);
    compacted.rebuilt_state.dag = &result.dag;
    compacted.rebuilt_state.counters = preserved_counters;
    state = std::move(compacted.rebuilt_state);
    result.summary.final_score = compacted.rebuilt_score;
    result.summary.final_compaction_rebuilds = 1;
    result.summary.final_compaction_exactness_kind = compacted.exactness_kind;
    result.summary.final_compaction_ms += chart_spr_elapsed_ms(
        compact_start, std::chrono::steady_clock::now());
  }

  // Phase 10 identity surface: emit the JSON identity report of the overlay
  // chain when local-commit mode was used (so the chain existed).  The keys
  // are stable across materialize / rebuild / report round trips; an empty
  // chain (no accepts) yields an empty-entries report carrying just the base
  // keys, which is still a faithful identity reference.
  if (local_commit_substrate != nullptr) {
    auto identity = build_phase10_chain_identity_report(
        *local_commit_substrate->chain);
    result.chain_identity_report_json =
        emit_phase10_chain_identity_report_json(identity);
    if (result.canonical_report) {
      auto canonical_key = [&](rank3_production_taxa_key const& key) {
        return chart_spr_canonical_production_sample_key(
            state.grammar, key.parent, key.children);
      };
      result.canonical_report->chain_base_production_keys.clear();
      for (auto const& key : identity.base_production_keys) {
        result.canonical_report->chain_base_production_keys.push_back(
            canonical_key(key));
      }
      for (auto const& source : identity.entries) {
        chart_spr_canonical_chain_entry entry;
        entry.position = source.position;
        entry.commit_source = source.commit_source;
        for (auto const& key : source.added_production_keys) {
          entry.added_production_keys.push_back(canonical_key(key));
        }
        for (auto const& key : source.tombstoned_production_keys) {
          entry.tombstoned_production_keys.push_back(canonical_key(key));
        }
        result.canonical_report->chain_entries.push_back(std::move(entry));
      }
    }
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
  result.summary.cache_strategy = state.cache_strategy;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
  result.summary.total_ms = chart_spr_elapsed_ms(
      total_start, std::chrono::steady_clock::now());
  result.summary.peak_concurrent_exact_verifiers =
      state.exact_verifier_concurrency
          ? state.exact_verifier_concurrency->peak()
          : 0;
  result.summary.effective_candidate_batch_size =
      state.effective_candidate_batch_size;
  chart_spr_refresh_search_summary_from_current_lazy_chart(result.summary,
                                                          state);
  if (result.summary.local_scoring_ms > 0.0) {
    auto seconds = result.summary.local_scoring_ms / 1000.0;
    result.summary.local_candidates_per_second =
        static_cast<double>(result.summary.candidates_locally_scored) /
        seconds;
    result.summary.local_rows_recomputed_per_second =
        static_cast<double>(result.summary.local_rows_recomputed) / seconds;
  }
  if (result.summary.exact_candidate_timing_count != 0) {
    result.summary.exact_candidate_verification_ms_mean =
        result.summary.exact_verification_ms /
        static_cast<double>(result.summary.exact_candidate_timing_count);
  }
  result.summary.affected_distribution =
      summarize_affected_clade_counts(std::move(aggregate_affected_counts));
  if (result.canonical_report) {
    auto& canonical = *result.canonical_report;
    canonical.initial_score = result.summary.initial_score;
    canonical.iterations.reserve(result.iterations.size());
    for (auto const& source : result.iterations) {
      chart_spr_canonical_iteration_record iteration;
      iteration.iteration = source.iteration;
      iteration.seed = source.canonical_seed;
      iteration.state_score_before = source.state_score_before;
      iteration.state_score_after = source.state_score_after;
      iteration.generation_stop_reason =
          chart_spr_candidate_stop_reason_name(
              source.candidate_generation.stop_reason);
      iteration.candidates_generated = source.candidates_generated;
      iteration.candidates_scored = source.candidates_scored;
      iteration.candidates_exact_verified =
          source.candidates_exact_verified;
      iteration.unverified_candidates_may_contain_improvements =
          source.unverified_candidates_may_contain_improvements;
      iteration.candidates = source.canonical_candidates;
      iteration.ranked_stream_indices =
          source.canonical_ranked_stream_indices;
      iteration.exact_verified_stream_indices =
          source.canonical_exact_verified_stream_indices;
      iteration.accepted_move_present = source.accepted.has_value();
      iteration.accepted_move_committed = source.accepted_move_committed;
      iteration.post_materialization_rejected =
          source.post_materialization_rejected;
      iteration.no_accept_reason = source.no_accept_reason;
      iteration.post_materialization_rejection_reason =
          source.post_materialization_rejection_reason;
      iteration.state_exact_before = source.canonical_state_exact_before;
      if (source.accepted) {
        if (source.accepted->canonical_stream_index ==
            (std::numeric_limits<std::size_t>::max)()) {
          throw std::logic_error(
              "chart-SPR canonical report: accepted candidate missing "
              "stream index");
        }
        auto stream_index = source.accepted->canonical_stream_index;
        if (stream_index >= iteration.candidates.size()) {
          throw std::logic_error(
              "chart-SPR canonical report: accepted stream index out of "
              "range");
        }
        iteration.selected_stream_index = stream_index;
        iteration.selected_signature =
            iteration.candidates[stream_index].signature;
      }
      canonical.iterations.push_back(std::move(iteration));
    }
    canonical.final_score = result.summary.final_score;
    canonical.accepted_moves = result.summary.accepted_moves;
    canonical.final_clade_keys =
        chart_spr_canonical_grammar_clade_keys(state.grammar);
    canonical.final_production_keys =
        chart_spr_canonical_grammar_production_keys(state.grammar);
    if (options.acceptance_mode ==
        chart_spr_acceptance_mode::exact_multisite) {
      auto checked_final =
          check_chart_execution_plan(state.grammar, state.execution_plan);
      auto const& final_trim =
          ensure_chart_spr_state_exact_trim(state, checked_final,
                                            options.exact_trim);
      canonical.final_exact = chart_spr_canonicalize_search_trim_evidence(
          state.grammar, checked_final, state.active_patterns,
          state.chart_opts, options.exact_trim, final_trim,
          state.invariant_constant_offset);
      if (final_trim.keep_production_exact) {
        ++state.counters.chart_execution_plan_cache_hits;
      }
    } else if (options.acceptance_mode ==
                   chart_spr_acceptance_mode::fixed_topology_exact &&
               last_local_update_accepted &&
               last_local_update_accepted->exact) {
      canonical.final_exact =
          chart_spr_canonicalize_fixed_topology_evidence(
              state.grammar,
              last_local_update_accepted->topology_selection,
              state.invariant_constant_offset);
      canonical.final_exact->optimum_active =
          last_local_update_accepted->exact->value.new_score -
          state.invariant_constant_offset;
    }
    result.canonical_digest =
        build_chart_spr_semantic_digest_report(canonical);
  }
  // Canonical provenance is constructed after the ordinary end-of-run
  // counter snapshot.  It can reuse the resident plan for one companion trim
  // (and a previously absent final exact trim can be built here), so resnapshot
  // to expose every successful plan use in the returned full-run counters.
  result.counters = state.counters;
  chart_spr_refresh_search_summary_from_counters(result.summary,
                                                 result.counters);
  // The counter refresh above intentionally restores cumulative accounting,
  // but the public lazy summary fields describe the final resident chart.
  // Reapply that current-state view so its raw fields and derived ratios stay
  // mutually consistent after canonical post-processing.
  chart_spr_refresh_search_summary_from_current_lazy_chart(result.summary,
                                                          state);
  return result;
}

}  // namespace larch

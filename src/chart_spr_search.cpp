#include <larch/chart_spr_search.hpp>
#include <larch/chart_two_chart_oracle.hpp>
#include <larch/inside_chart_cache.hpp>
#include <larch/outside_chart_cache.hpp>
#include <larch/overlay_chain.hpp>
#include <larch/overlay_chain_compaction.hpp>
#include <larch/rank3_rewrite.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
  compaction_options.witness_topologies =
      chart_spr_collect_local_update_compaction_witness_topologies(
          local_state, options, last_accepted);
  compaction_options.witness_topology_key_sets =
      accepted_topology_key_sets;

  ++counters.full_overlay_materializations;
  ++counters.overlay_materializations_for_final_compaction;
  auto compacted = compact_overlay_chain_to_dag(source, chain,
                                                compaction_options);
  if (!compacted.all_witness_topologies_present()) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: compacted output "
        "DAG failed to preserve every accepted topology witness");
  }

  auto oracle = grammar_level_exact_parsimony(
      compacted.rebuilt.grammar, local_state.active_patterns,
      local_state.chart_opts, local_state.invariant_constant_offset,
      options.exact_trim);

  chart_spr_local_update_compaction_gate_result result;
  result.rebuilt_score = oracle.value;
  result.exactness_kind = oracle.exactness_kind;
  result.materialized_tree_count = compacted.materialized_tree_count;

  if (result.rebuilt_score > expected_score) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: grammar-level "
        "output DAG optimum " +
        std::to_string(result.rebuilt_score) +
        " exceeds chain recorded objective " +
        std::to_string(expected_score));
  }
  if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite &&
      result.rebuilt_score != expected_score) {
    throw std::runtime_error(
        "chart SPR local accepted-state final compaction: grammar-level "
        "output DAG optimum " +
        std::to_string(result.rebuilt_score) +
        " does not match exact chain objective " +
        std::to_string(expected_score));
  }

  result.dag = std::move(compacted.dag);
  result.rebuilt_state = rebuild_chart_spr_search_state_after_accept(
      local_state, result.dag, std::move(compacted.rebuilt.grammar), options,
      result.reused_patterns);
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

struct chart_spr_selected_topology_cache_entry {
  std::vector<std::array<chart_cost, nuc_state_count>> rows_by_pattern;
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

  // Phase 8 per-pattern selected-topology oracle cache.  This is separate
  // from the grammar-min inside/outside caches: it stores rows for one exact
  // structural selected subtree, so an unchanged fixed-topology subtree can be
  // read instead of recomputed for every candidate oracle.
  chart_spr_selected_topology_row_cache selected_topology_cache;
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

overlay_clade_ref chart_spr_chain_ref_for_dense_clade(
    chart_spr_local_commit_substrate const& sub, clade_id dense) {
  if (dense == no_clade || dense >= sub.dense_clade_to_chain_ref.size()) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: dense clade ref out of "
        "current tip map range");
  }
  auto ref = sub.dense_clade_to_chain_ref[dense];
  if (ref.id == no_clade) {
    throw std::runtime_error(
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

std::string chart_spr_selected_topology_internal_key(std::string left,
                                                     std::string right) {
  if (right < left) std::swap(left, right);
  return "I(" + left + ")(" + right + ")";
}

std::map<overlay_clade_ref, overlay_production_ref>
chart_spr_before_selected_production_by_parent(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::vector<overlay_production_ref> const& production_refs) {
  std::map<overlay_clade_ref, overlay_production_ref> selected;
  for (auto ref : production_refs) {
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: before-topology "
          "production must refer to the current base grammar");
    }
    validate_chart_spr_selected_overlay_production_for_fixed_topology(
        base, candidate, ref,
        "fixed_topology_exact selected-topology cache before");
    auto parent = chart_spr_overlay_production_parent(base, candidate, ref);
    auto [it, inserted] = selected.emplace(parent, ref);
    if (!inserted && it->second != ref) {
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: conflicting "
          "before-topology production choices for one clade");
    }
  }
  return selected;
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
    overlay_clade_ref clade, std::set<overlay_clade_ref>& active_refs) {
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
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: cached row "
          "pattern count mismatch");
    }
    ++state.counters.fixed_topology_selected_cache_hits;
    return chart_spr_selected_topology_node{key, &it->second};
  };
  auto insert_new_node = [&](std::string key,
                             chart_spr_selected_topology_cache_entry entry) {
    if (entry.rows_by_pattern.size() != active.size()) {
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: new row pattern "
          "count mismatch");
    }
    ++state.counters.fixed_topology_selected_cache_misses;
    state.counters.fixed_topology_selected_rows_computed +=
        entry.rows_by_pattern.size();
    auto [inserted, ok] = cache.rows_by_key.emplace(key, std::move(entry));
    if (!ok) {
      throw std::runtime_error(
          "fixed_topology_exact selected-topology cache: duplicate insert");
    }
    return chart_spr_selected_topology_node{inserted->first,
                                            &inserted->second};
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
  auto left = chart_spr_selected_topology_rows_for_clade(
      cache, state, candidate, selected, children[0], active_refs);
  auto right = chart_spr_selected_topology_rows_for_clade(
      cache, state, candidate, selected, children[1], active_refs);
  if (left.entry == nullptr || right.entry == nullptr) {
    throw std::runtime_error(
        "fixed_topology_exact selected-topology cache: child cache entry "
        "missing");
  }

  auto key = chart_spr_selected_topology_internal_key(left.key, right.key);
  if (auto cached = try_cached_node(key)) return *cached;
  chart_spr_selected_topology_cache_entry entry;
  entry.rows_by_pattern.reserve(active.size());
  for (std::size_t p = 0; p < active.size(); ++p) {
    entry.rows_by_pattern.push_back(
        chart_multisite_detail::combine_binary_rows(
            left.entry->rows_by_pattern[p], right.entry->rows_by_pattern[p]));
  }
  return insert_new_node(std::move(key), std::move(entry));
}

chart_spr_fixed_topology_pattern_scores
chart_spr_fixed_topology_selected_pattern_scores_from_cache(
    chart_spr_selected_topology_row_cache& cache,
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
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
      base_clade_ref(state.grammar.root_clade), active_refs);
  active_refs.clear();
  auto after_root = chart_spr_selected_topology_rows_for_clade(
      cache, state, candidate.candidate, after_selected,
      base_clade_ref(state.grammar.root_clade), active_refs);
  if (before_root.entry == nullptr || after_root.entry == nullptr) {
    throw std::runtime_error(
        "fixed_topology_exact selected-topology cache: root cache entry "
        "missing");
  }

  chart_spr_fixed_topology_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  for (std::size_t p = 0; p < active.size(); ++p) {
    if (state.chart_opts.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(active[p], p);
    }
    auto old_score = chart_spr_weighted_root_score_from_row(
        before_root.entry->rows_by_pattern[p], active[p], state.chart_opts);
    auto new_score = chart_spr_weighted_root_score_from_row(
        after_root.entry->rows_by_pattern[p], active[p], state.chart_opts);
    scores.old_pattern_scores.push_back(old_score);
    scores.new_pattern_scores.push_back(new_score);
    scores.old_active_total = chart_multisite_detail::checked_add_u64(
        scores.old_active_total, old_score,
        "fixed_topology_exact selected-cache old active total");
    scores.new_active_total = chart_multisite_detail::checked_add_u64(
        scores.new_active_total, new_score,
        "fixed_topology_exact selected-cache new active total");
  }
  return scores;
}

struct chart_spr_fixed_topology_cache_pattern_scores {
  std::vector<std::uint64_t> old_pattern_scores;
  std::vector<std::uint64_t> new_pattern_scores;
  std::uint64_t old_active_total = 0;
  std::uint64_t new_active_total = 0;
  chart_spr_fixed_topology_pattern_scores selected_oracle_scores;
  bool per_pattern_oracle_green = false;
  std::string oracle_mismatch_reason;
};

std::array<chart_cost, nuc_state_count> const&
chart_spr_fixed_cache_inside_row(
    chart_spr_local_commit_substrate const& sub, inside_chart_cache const& icache,
    spr_overlay_delta const& delta,
    std::vector<std::array<chart_cost, nuc_state_count>> const& scratch,
    std::size_t pattern, overlay_clade_ref ref) {
  auto slot = ref.space == overlay_id_space::base
                  ? delta.affected_base_row_slot.at(ref.id)
                  : delta.affected_temp_row_slot.at(ref.id);
  if (slot != chart_spr_overlay_row_npos) {
    if (slot >= scratch.size()) {
      throw std::runtime_error(
          "chart SPR fixed-topology cache verifier: affected row slot out "
          "of range");
    }
    return scratch[slot];
  }
  if (ref.space != overlay_id_space::base) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: selected topology reached "
        "an unaffected temp clade (affected-set under-inclusion)");
  }
  return icache.row(pattern, chart_spr_chain_ref_for_dense_clade(sub, ref.id));
}

// Phase-8 fixed-topology edge-term convention.  Binary SPR, SPR-with-
// collapse, and the child-set rewrite shape all arrive here as selected binary
// productions in overlay space.  The detach/reattach/split/merge transition
// terms are therefore exactly the two parent->child transition terms added by
// `combine_binary_rows` for each selected production.  A moved subtree M is
// addressed by one overlay clade ref; every selected production that touches M
// reads the same cached/scratch row for that ref, so its root state s_M is a
// single shared minimization variable inside the row rather than two
// independently minimized detach/reattach scalars.  The per-pattern oracle
// below compares this grammar-cache calculation to the persistent structural
// selected-topology row cache before the grammar-cache result may carry the
// fixed_topology_exact label.
std::array<chart_cost, nuc_state_count>
chart_spr_recompute_selected_affected_row_from_cache(
    chart_spr_local_commit_substrate const& sub, chart_spr_search_state const& state,
    inside_chart_cache const& icache, spr_overlay_delta const& delta,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    std::vector<std::array<chart_cost, nuc_state_count>> const& scratch,
    std::size_t pattern, overlay_clade_ref ref) {
  auto taxa = chart_spr_clade_taxa_for_ref(state.grammar, *delta.candidate, ref);
  if (taxa.size() == 1) {
    auto taxon = taxa.front();
    auto const& site = icache.patterns[pattern];
    if (taxon >= site.state_by_taxon.size()) {
      throw std::runtime_error(
          "chart SPR fixed-topology cache verifier: leaf taxon out of state "
          "range");
    }
    auto observed = site.state_by_taxon[taxon];
    parsimony_chart_detail::validate_state(
        observed, "fixed_topology_exact cache leaf state");
    auto row = parsimony_chart_detail::make_inf_row();
    row[observed] = 0;
    return row;
  }

  auto it = selected.find(ref);
  if (it == selected.end()) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: selected topology missing "
        "production for affected non-singleton clade");
  }
  validate_chart_spr_selected_overlay_production_for_fixed_topology(
      state.grammar, *delta.candidate, it->second,
      "chart SPR fixed-topology cache verifier");
  auto children = chart_spr_overlay_production_children(state.grammar,
                                                        *delta.candidate,
                                                        it->second);
  auto const& left = chart_spr_fixed_cache_inside_row(
      sub, icache, delta, scratch, pattern, children[0]);
  auto const& right = chart_spr_fixed_cache_inside_row(
      sub, icache, delta, scratch, pattern, children[1]);
  return chart_multisite_detail::combine_binary_rows(left, right);
}

chart_spr_fixed_topology_cache_pattern_scores
chart_spr_fixed_topology_pattern_scores_from_persistent_cache(
    chart_spr_local_commit_substrate& sub,
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate) {
  if (!sub.icache || !sub.ocache || !sub.chain) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: local-commit substrate is "
        "not initialized");
  }
  auto const& icache = *sub.icache;
  auto const& ocache = *sub.ocache;
  if (state.chart_opts.score_ua_edge) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: score_ua_edge=true is not "
        "supported by the persistent-cache verifier");
  }
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (!candidate.topology_selection.certificate) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier requires a complete topology "
        "certificate");
  }
  if (icache.commit_epoch != sub.chain->size() ||
      ocache.commit_epoch != sub.chain->size()) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: persistent cache epochs do "
        "not match the chain tip");
  }
  if (icache.patterns.size() !=
          state.active_patterns.patterns.patterns.size() ||
      ocache.patterns.size() != icache.patterns.size()) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: active pattern cache size "
        "mismatch");
  }
  if (sub.dense_clade_to_chain_ref.size() != state.grammar.clades.size()) {
    throw std::runtime_error(
        "chart SPR fixed-topology cache verifier: dense clade map does not "
        "match current state grammar");
  }

  validate_chart_spr_topology_certificate_signatures(
      state.grammar, candidate.candidate,
      *candidate.topology_selection.certificate);
  auto selected = chart_spr_overlay_selected_production_by_parent(
      state.grammar, candidate.candidate,
      candidate.topology_selection.certificate->after_overlay_productions);
  auto delta = build_spr_overlay_delta(state.grammar, candidate.candidate);

  chart_spr_fixed_topology_cache_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  auto root_ref = base_clade_ref(state.grammar.root_clade);
  auto root_chain_ref = chart_spr_chain_ref_for_dense_clade(
      sub, state.grammar.root_clade);

  std::vector<std::array<chart_cost, nuc_state_count>> scratch;
  for (std::size_t p = 0; p < active.size(); ++p) {
    scratch.assign(delta.affected_order.size(),
                   parsimony_chart_detail::make_inf_row());
    for (std::size_t slot = 0; slot < delta.affected_order.size(); ++slot) {
      scratch[slot] = chart_spr_recompute_selected_affected_row_from_cache(
          sub, state, icache, delta, selected, scratch, p,
          delta.affected_order[slot]);
    }

    auto const& old_root_inside = icache.row(p, root_chain_ref);
    auto const& old_root_outside = ocache.row(p, root_chain_ref);
    auto const& new_root_inside = chart_spr_fixed_cache_inside_row(
        sub, icache, delta, scratch, p, root_ref);
    auto old_min = chart_spr_min_inside_plus_outside(old_root_inside,
                                                     old_root_outside);
    auto new_min = chart_spr_min_inside_plus_outside(new_root_inside,
                                                     old_root_outside);
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

  scores.selected_oracle_scores =
      chart_spr_fixed_topology_selected_pattern_scores_from_cache(
          sub.selected_topology_cache, state, candidate);
  scores.per_pattern_oracle_green =
      scores.old_pattern_scores ==
          scores.selected_oracle_scores.old_pattern_scores &&
      scores.new_pattern_scores ==
          scores.selected_oracle_scores.new_pattern_scores;
  if (!scores.per_pattern_oracle_green) {
    for (std::size_t p = 0; p < active.size(); ++p) {
      if (scores.old_pattern_scores[p] !=
              scores.selected_oracle_scores.old_pattern_scores[p] ||
          scores.new_pattern_scores[p] !=
              scores.selected_oracle_scores.new_pattern_scores[p]) {
        scores.oracle_mismatch_reason =
            "persistent-cache fixed-topology per-pattern oracle mismatch at "
            "pattern " + std::to_string(p) + " (grammar-cache old/new=" +
            std::to_string(scores.old_pattern_scores[p]) + "/" +
            std::to_string(scores.new_pattern_scores[p]) +
            ", selected-cache old/new=" +
            std::to_string(
                scores.selected_oracle_scores.old_pattern_scores[p]) +
            "/" +
            std::to_string(
                scores.selected_oracle_scores.new_pattern_scores[p]) +
            ")";
        break;
      }
    }
    if (scores.oracle_mismatch_reason.empty()) {
      scores.oracle_mismatch_reason =
          "persistent-cache fixed-topology per-pattern oracle mismatch";
    }
  }
  return scores;
}

chart_spr_candidate_score
chart_spr_verify_fixed_topology_direct_fallback_after_counting(
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    std::string const& reason) {
  // Strict Phase-8 label discipline: if the persistent grammar-cache path and
  // the persistent selected-topology cache disagree per pattern, do not label
  // either cached value as exact.  Fall back to the conservative from-scratch
  // selected-topology scorer (full selected rows recomputed, no cached
  // selected-subtree rows consulted) and label only that independently
  // recomputed result fixed_topology_exact.  This keeps full selected-topology
  // recomputation off the green path while preserving the "oracle green or
  // from-scratch fallback" contract.
  try {
    auto delta = fixed_topology_delta_direct_selected_topology(state, candidate);
    candidate.exact = make_chart_spr_objective_score(
        delta, chart_spr_score_kind::fixed_topology_exact,
        chart_spr_score_convention::full_with_invariants,
        state.invariant_constant_offset);
    return candidate;
  } catch (std::exception const& e) {
    throw std::runtime_error(
        "fixed_topology_exact persistent-cache verifier: from-scratch "
        "selected-topology fallback failed after cache-oracle mismatch (" +
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
  try {
    auto scores = chart_spr_fixed_topology_pattern_scores_from_persistent_cache(
        sub, state, candidate);
    if (!scores.per_pattern_oracle_green) {
      return chart_spr_verify_fixed_topology_direct_fallback_after_counting(
          state, std::move(candidate), scores.oracle_mismatch_reason);
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
  } catch (std::exception const& e) {
    candidate.valid = false;
    candidate.invalid_reason = e.what();
  }
  return candidate;
}

std::unique_ptr<chart_spr_local_commit_substrate>
chart_spr_make_local_commit_substrate(chart_spr_search_state const& state) {
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
  sub->chain.emplace(sub->base_grammar);
  // build_*_chart_cache return by value; their `base` pointer points at the
  // grammar passed in (&sub->base_grammar), which is stable for the run.  The
  // move into the optional copies the pointer, still valid.
  sub->icache = build_inside_chart_cache(sub->base_grammar, state.active_patterns,
                                          state.chart_opts,
                                          state.invariant_constant_offset);
  sub->ocache = build_outside_chart_cache(sub->base_grammar,
                                           state.active_patterns,
                                           state.chart_opts);
  chart_spr_set_identity_tip_maps(*sub);
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

// Phase 3 two-chart oracle self-check: recompute BOTH charts from scratch on
// the materialized chain and assert the persistent caches agree on every
// reachable clade, every active pattern.  This is the load-bearing guard
// against inside/outside affected-set under-inclusion (the most likely silent
// bug).  Mirrors assert_cache_both_charts_match_from_scratch in the Phase 3
// test; kept in the .cpp so enabling it is a test-only flag.
void chart_spr_assert_local_commit_two_chart_oracle(
    overlay_chain const& chain, inside_chart_cache const& icache,
    outside_chart_cache const& ocache, std::string const& context) {
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
    }
    if (outside_cache_global_min(ocache, icache, p) !=
        oracle.second.global_min) {
      throw std::runtime_error(
          "chart SPR local-commit two-chart oracle [" + context +
          "]: global_min mismatch at pattern " + std::to_string(p));
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
    inside_chart_cache const& icache) {
  auto old_strategy = state.cache_strategy;
  state.grammar = materialized.grammar;

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

  // Composite lower bound from the authoritative icache root rows (Phase 4
  // local commit currently rejects score_ua_edge=true before substrate build;
  // conservative mode continues to support the UA-edge convention).
  auto composite_with_invariants =
      inside_cache_composite_lower_bound_with_invariants(icache);
  if (icache.invariant_constant_offset > composite_with_invariants) {
    throw std::runtime_error(
        "chart SPR local-commit tip refresh: composite below invariant offset");
  }
  state.composite_lower_bound_with_invariants = composite_with_invariants;
  state.composite_lower_bound_without_invariants =
      composite_with_invariants - state.invariant_constant_offset;

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
  // search invariant, so surface it as a hard local-commit error.
  spr_overlay_delta delta;
  try {
    local_spr_score_options local_options;
    local_options.verify_against_full_overlay = false;
    local_options.validate_cached_chart_shapes = false;
    delta = build_spr_overlay_delta(state.grammar, accepted.candidate,
                                    local_options);
  } catch (std::exception const& e) {
    throw chart_spr_local_commit_hard_error(
        std::string{"chart SPR local commit: failed to build accepted "
                    "candidate delta before commit: "} +
        e.what());
  }

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
    auto materialized = materialize_overlay_chain(*sub.chain);
    chart_spr_set_tip_maps_from_materialization(sub, materialized);
    ++counters.local_commit_tip_grammar_refreshes;
    chart_spr_refresh_state_tip_view_after_local_commit(state, materialized,
                                                         *sub.icache);

    // Mirror cumulative cache counters onto the running attempt-counters (the
    // caches persist across accepts; their counters are cumulative).
    counters.inside_rows_recomputed_on_commit =
        sub.icache->inside_rows_recomputed_on_commit;
    counters.outside_rows_recomputed_on_commit =
        sub.ocache->outside_rows_recomputed_on_commit;

    // Two-chart oracle self-check (Work item 3 correctness invariant).
    if (options.verify_local_commit_two_chart_oracle_for_tests) {
      chart_spr_assert_local_commit_two_chart_oracle(
          *sub.chain, *sub.icache, *sub.ocache,
          "after commit " + std::to_string(sub.chain->size()));
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
  summary.candidate_batches_scored = counters.candidate_batches_scored;
  summary.pattern_batch_cache_builds = counters.pattern_batch_cache_builds;
  summary.exact_verifications = counters.exact_verifications;
  summary.overlay_materializations_for_exact_verification =
      counters.overlay_materializations_for_exact_verification;
  summary.overlay_materializations_for_accept_materialization =
      counters.overlay_materializations_for_accept_materialization;
  summary.overlay_materializations_for_final_compaction =
      counters.overlay_materializations_for_final_compaction;
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
  summary.local_commit_two_chart_oracle_runs =
      counters.local_commit_two_chart_oracle_runs;
  summary.local_commit_tip_grammar_refreshes =
      counters.local_commit_tip_grammar_refreshes;
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
  std::optional<chart_spr_candidate_score> last_local_update_accepted;
  std::optional<chart_spr_recorded_chain_objective>
      last_local_update_recorded_objective;
  std::vector<std::vector<rank3_production_taxa_key>>
      local_update_accepted_topology_key_sets;
  bool used_local_accept_updates = false;

  // Phase 4 local-commit substrate: the overlay chain + persistent inside /
  // outside caches.  Built once from the frozen initial grammar when
  // rebuild_after_accept = false; the accept path commits to it instead of
  // dense-materializing per accept.  State.grammar / state.pattern_charts are a
  // derived view of the chain tip, refreshed on each commit.
  std::unique_ptr<chart_spr_local_commit_substrate> local_commit_substrate;
  if (!options.rebuild_after_accept) {
    local_commit_substrate = chart_spr_make_local_commit_substrate(state);
    auto* substrate_ptr = local_commit_substrate.get();
    state.fixed_topology_exact_verifier =
        [substrate_ptr](chart_spr_search_state const& verifier_state,
                        chart_spr_candidate_score candidate) {
          return chart_spr_verify_candidate_fixed_topology_exact_from_persistent_cache(
              *substrate_ptr, verifier_state, std::move(candidate));
        };
  }

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
    bool local_commit_mutated_shared_state = false;
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

        if (rebuilt_score > iteration.state_score_before) {
          ++attempt_counters.post_materialization_rejections;
          state.counters = attempt_counters;
          iteration.post_materialization_rejected = true;
          iteration.accepted_move_committed = false;
          iteration.state_score_after = iteration.state_score_before;
          iteration.no_accept_reason =
              "local commit objective worsened";
          iteration.post_materialization_rejection_reason =
              "locally committed objective " + std::to_string(rebuilt_score) +
              " exceeds pre-accept objective " +
              std::to_string(iteration.state_score_before);
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

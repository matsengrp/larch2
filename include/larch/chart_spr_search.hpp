#pragma once

#include <larch/chart_spr.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

// Phase-0 instrumentation for the DAG-native chart-SPR search path.  These
// counters deliberately separate accepted-state rebuilds from diagnostic/oracle
// overlay materializations and exact-verification materializations so benchmark
// output can detect accidental "full rebuild per rejected candidate" behavior.
struct chart_spr_search_counters {
  std::size_t grammar_rebuilds = 0;
  std::size_t pattern_rebuilds = 0;
  std::size_t base_chart_cache_rebuilds = 0;
  // Total overlay materializations plus reason-coded splits.  Exact
  // verification materializations are expensive, but they are not accepted-
  // state sidecar rebuilds and must be reported separately.
  std::size_t full_overlay_materializations = 0;
  std::size_t overlay_materializations_for_oracle = 0;
  // Phase-1 bridge path: local scorer reuses cached base charts but still
  // materializes one dense overlay per candidate until Phase-3 overlay-delta
  // scoring replaces this implementation detail.  Count it separately from
  // oracle/debug materializations.
  std::size_t overlay_materializations_for_local_scoring_bridge = 0;
  std::size_t overlay_materializations_for_exact_verification = 0;
  std::size_t overlay_materializations_for_accept_materialization = 0;
  std::size_t sidecar_rebuilds_after_accept = 0;
  std::size_t full_composite_rebuilds = 0;
  std::size_t local_candidate_scores = 0;
  std::size_t exact_verifications = 0;
  std::size_t accepted_moves = 0;
  std::size_t candidate_accepts_attempted = 0;
  std::size_t rejected_moves = 0;
  std::size_t post_materialization_rejections = 0;
  std::size_t skipped_invariant_sites = 0;

  // Candidate-generation/path-explosion counters.
  std::size_t candidate_source_productions_considered = 0;
  std::size_t upward_path_iterator_steps = 0;
  std::size_t upward_paths_completed = 0;
  std::size_t path_pairs_considered = 0;
  std::size_t candidates_constructed = 0;
  std::size_t candidates_pruned_before_construction = 0;
  std::size_t candidates_pruned_after_construction = 0;
  std::size_t candidates_generated_after_dedup = 0;
  std::size_t candidate_cap_cutoffs = 0;
  std::size_t path_budget_cutoffs = 0;

  // Overlay validation/reachability counters.
  std::size_t overlay_reachability_validations = 0;
  std::size_t reachable_clades_traversed = 0;
  std::size_t reachable_productions_traversed = 0;
  std::size_t reachable_temp_clades_traversed = 0;
  std::size_t reachable_temp_productions_traversed = 0;
  std::size_t reachability_full_grammar_like_passes = 0;
};

enum class chart_spr_acceptance_mode {
  exact_multisite,
  fixed_topology_exact,
  lower_bound_heuristic,
};

enum class chart_spr_candidate_selection_mode {
  exhaustive_exact,
  lower_bound_top_k,
  lower_bound_first_improvement,
  sampled_or_randomized,
};

enum class chart_spr_topology_selection_kind {
  none,
  explicit_certificate,
  deterministic_selector,
};

struct chart_spr_production_signature {
  // Stable across dense overlay materialization/rebuild.  In-process reports
  // use taxon IDs; cross-run reports can translate them to sample IDs.
  std::vector<taxon_id> parent_taxa;
  std::vector<std::vector<taxon_id>> child_taxa;
};

struct chart_spr_topology_certificate {
  // Overlay refs are stable inside one candidate overlay and avoid tying the
  // certificate to dense materialized production IDs.
  std::vector<overlay_production_ref> before_overlay_productions;
  std::vector<overlay_production_ref> after_overlay_productions;

  // Taxon-key signatures let the certificate survive overlay materialization,
  // accepted-state rebuilds, and diagnostic JSON round trips.
  std::vector<chart_spr_production_signature> before_signatures;
  std::vector<chart_spr_production_signature> after_signatures;
};

struct chart_spr_topology_selection {
  chart_spr_topology_selection_kind kind =
      chart_spr_topology_selection_kind::none;
  std::string selector_name;
  std::optional<chart_spr_topology_certificate> certificate;
};

struct chart_cache_options {
  std::size_t max_cached_patterns = 0;  // 0 = all patterns
  std::size_t memory_budget_bytes = 0;  // 0 = no explicit budget
  std::size_t candidate_batch_size = 0; // 0 = choose from memory budget
};

struct chart_spr_search_options {
  std::size_t max_iterations = 1;
  std::size_t max_candidates_per_iteration = 0;  // 0 = unlimited
  std::size_t top_k_exact_verify = 16;
  chart_spr_acceptance_mode acceptance_mode =
      chart_spr_acceptance_mode::exact_multisite;
  chart_spr_candidate_selection_mode candidate_selection =
      chart_spr_candidate_selection_mode::lower_bound_top_k;

  grammar_spr_enumeration_options enumeration = {};
  chart_options chart = {};
  multisite_trim_options exact_trim = {};
  chart_cache_options cache = {};

  bool materialize_accepted_moves = true;
  bool rebuild_after_accept = true;  // conservative first production default
  bool verify_local_against_full_for_tests = false;
  std::uint32_t seed = 1;
};

enum class chart_spr_score_kind {
  composite_lower_bound,
  fixed_topology_exact,
  grammar_exact,
};

enum class chart_spr_score_convention {
  active_only,
  full_with_invariants,
};

struct chart_spr_objective_score {
  spr_score_result value;
  chart_spr_score_kind kind = chart_spr_score_kind::composite_lower_bound;
  chart_spr_score_convention convention =
      chart_spr_score_convention::full_with_invariants;

  // Nonzero only when convention == full_with_invariants.  Keeping this field
  // beside the score makes active-only vs full-objective comparisons explicit.
  std::uint64_t invariant_offset_applied = 0;
};

struct chart_spr_candidate_score {
  grammar_spr_candidate candidate;
  chart_spr_objective_score lower_bound;
  std::optional<chart_spr_objective_score> exact;
  chart_spr_topology_selection topology_selection;
  std::size_t affected_clade_count = 0;
  double local_score_ms = 0.0;
};

struct affected_clade_distribution {
  double mean = 0.0;
  std::size_t p50 = 0;
  std::size_t p95 = 0;
  std::size_t max = 0;
};

struct chart_spr_iteration_result {
  std::size_t iteration = 0;
  std::size_t candidates_generated = 0;
  std::size_t candidates_scored = 0;
  std::size_t candidates_exact_verified = 0;
  affected_clade_distribution affected_distribution;
  std::optional<chart_spr_candidate_score> accepted;
  std::uint64_t state_score_before = 0;
  std::uint64_t state_score_after = 0;
};

struct chart_spr_search_result {
  phylo_dag dag;
  std::vector<chart_spr_iteration_result> iterations;
  chart_spr_search_counters counters;
};

struct active_site_pattern_set {
  site_pattern_set patterns;

  // Search-state internals are active/topology-informative only.  Invariant
  // sites and their topology-independent score contribution live on
  // chart_spr_search_state, so this wrapper must not carry skipped-invariant
  // metadata or invariant patterns into active-only chart/B&B calls.
  void assert_no_skipped_invariant_metadata() const {
    if (patterns.skipped_invariant_site_count != 0) {
      throw std::runtime_error(
          "chart SPR active patterns: skipped invariant site metadata must be "
          "owned by the search state");
    }
    if (patterns.skipped_invariant_constant_score_with_reference_edge != 0) {
      throw std::runtime_error(
          "chart SPR active patterns: skipped invariant score metadata must be "
          "owned by the search state");
    }
    if (patterns.invariant_site_count != 0 ||
        patterns.invariant_constant_score_excluding_ua != 0 ||
        patterns.invariant_constant_score_with_reference_edge != 0) {
      throw std::runtime_error(
          "chart SPR active patterns: invariant-site metadata must be owned "
          "by the search state");
    }
    for (std::size_t i = 0; i < patterns.patterns.size(); ++i) {
      if (is_invariant_site_pattern(patterns.patterns[i])) {
        throw std::runtime_error(
            "chart SPR active patterns: invariant pattern at active index " +
            std::to_string(i));
      }
    }
  }
};

struct chart_spr_pattern_source_fingerprint {
  std::uint64_t reference_hash = 0;
  std::uint64_t sample_id_hash = 0;
  std::uint64_t compact_genome_hash = 0;
  std::uint64_t taxon_registry_hash = 0;

  bool operator==(chart_spr_pattern_source_fingerprint const&) const = default;
};

struct pattern_chart_cache_entry {
  single_site_chart chart;  // no trace in hot path

  // Keep the whole root row, not just a scalar root minimum.  With
  // score_ua_edge=true, one compressed leaf-state pattern can contain
  // positions with different UA/reference states.
  std::array<chart_cost, nuc_state_count> root_row =
      parsimony_chart_detail::make_inf_row();
  chart_cost root_min_excluding_ua = chart_inf;
  std::array<chart_cost, nuc_state_count> root_min_by_reference_state =
      parsimony_chart_detail::make_inf_row();
  std::array<std::uint64_t, nuc_state_count> reference_state_counts{};
  std::uint64_t weighted_root_score = 0;
};

struct chart_spr_active_pattern_build_result {
  active_site_pattern_set active_patterns;
  chart_spr_pattern_source_fingerprint pattern_source_fingerprint;
  std::uint64_t invariant_constant_offset = 0;
  std::size_t skipped_invariant_site_count = 0;
};

namespace chart_spr_search_detail {

inline std::uint64_t mix_u64(std::uint64_t seed, std::uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  return seed;
}

inline std::uint64_t fnv1a_append_byte(std::uint64_t seed,
                                        std::uint8_t byte) {
  return (seed ^ static_cast<std::uint64_t>(byte)) * 1099511628211ULL;
}

inline std::uint64_t hash_string_u64(std::string const& value) {
  std::uint64_t seed = 1469598103934665603ULL;
  for (unsigned char byte : value) seed = fnv1a_append_byte(seed, byte);
  return seed;
}

inline void validate_binary_chart_compatible_grammar(
    clade_grammar const& grammar) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error("chart SPR search: root clade out of range");
  }
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto const& clade = grammar.clades[cid];
    auto const& productions = grammar.productions_by_parent[cid];
    if (clade.taxa.size() == 1) {
      if (!productions.empty()) {
        throw std::runtime_error(
            "chart SPR search: singleton clade " + std::to_string(cid) +
            " has productions; expected leaf clades to be productionless");
      }
    } else if (productions.empty()) {
      throw std::runtime_error(
          "chart SPR search: non-singleton clade " + std::to_string(cid) +
          " has no productions; DAG-native chart-SPR requires a binary "
          "chart-compatible grammar");
    }
  }

  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& prod = grammar.productions[pid];
    if (prod.children.size() != 2) {
      throw std::runtime_error(
          "chart SPR search: production " + std::to_string(pid) +
          " has arity " + std::to_string(prod.children.size()) +
          "; DAG-native chart-SPR requires a binary chart-compatible grammar "
          "(run/refine polytomies first or choose reject mode)");
    }
    parsimony_chart_detail::validate_binary_production_partition(
        grammar, prod, static_cast<production_id>(pid));
  }
}

inline std::uint64_t invariant_pattern_reference_edge_offset(
    site_pattern const& pattern) {
  if (pattern.state_by_taxon.empty()) {
    throw std::runtime_error(
        "chart SPR active patterns: invariant pattern has no taxa");
  }
  auto invariant_state = pattern.state_by_taxon.front();
  parsimony_chart_detail::validate_state(invariant_state,
                                         "invariant pattern state");
  std::uint64_t total = 0;
  std::uint64_t reference_count_sum = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    auto count = static_cast<std::uint64_t>(
        pattern.reference_state_counts[reference_state]);
    reference_count_sum += count;
    if (count == 0) continue;
    auto cost = parsimony_chart_detail::transition_cost(reference_state,
                                                        invariant_state);
    total = chart_multisite_detail::checked_add_u64(
        total,
        chart_multisite_detail::checked_mul_cost(
            count, cost, "chart-SPR invariant UA-edge offset"),
        "chart-SPR invariant UA-edge offset total");
  }
  if (reference_count_sum != pattern.weight) {
    throw std::runtime_error(
        "chart SPR active patterns: invariant reference-state counts do not "
        "sum to pattern weight");
  }
  return total;
}

inline void append_active_pattern_metadata(site_pattern_set& active,
                                           site_pattern const& pattern) {
  active.patterns.push_back(pattern);
  active.total_site_count += pattern.weight;
  active.variable_site_count += pattern.weight;
  if (is_binary_variable_site_pattern(pattern)) {
    active.binary_variable_site_count += pattern.weight;
  } else {
    active.nonbinary_variable_site_count += pattern.weight;
  }
}

inline pattern_chart_cache_entry build_pattern_chart_cache_entry(
    clade_grammar const& grammar, site_pattern const& pattern,
    chart_options const& chart_opts, chart_options const& chart_build_opts) {
  leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
  pattern_chart_cache_entry entry;
  entry.chart = build_single_site_chart(grammar, states, chart_build_opts);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= entry.chart.inside.size()) {
    throw std::runtime_error(
        "chart SPR search state: root clade out of chart range");
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

}  // namespace chart_spr_search_detail

inline void validate_supported_chart_cache_options(
    chart_cache_options const& cache) {
  if (cache.max_cached_patterns != 0 || cache.memory_budget_bytes != 0 ||
      cache.candidate_batch_size != 0) {
    throw std::runtime_error(
        "chart SPR search: bounded/batched chart cache options are not "
        "implemented yet; leave max_cached_patterns, memory_budget_bytes, and "
        "candidate_batch_size at 0 for the current all-active-pattern cache");
  }
}

inline chart_spr_pattern_source_fingerprint
build_chart_spr_pattern_source_fingerprint(phylo_dag& dag,
                                           clade_grammar const& grammar) {
  using chart_spr_search_detail::hash_string_u64;
  using chart_spr_search_detail::mix_u64;

  chart_spr_pattern_source_fingerprint fp;
  fp.reference_hash = hash_string_u64(get_reference_sequence(dag));

  std::uint64_t sample_seed = grammar.taxa.id_to_sample_id.size();
  for (auto const& sample_id : grammar.taxa.id_to_sample_id) {
    sample_seed = mix_u64(sample_seed, hash_string_u64(sample_id));
  }
  fp.sample_id_hash = sample_seed;

  std::vector<std::pair<std::string, std::uint64_t>> leaf_hashes;
  auto reachable = detail::collect_reachable(dag);
  for (auto node_idx : reachable.nodes) {
    auto nv = dag.get_node(node_idx);
    if (!detail::is_leaf_node(nv)) continue;
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            std::uint64_t cg_seed = 1469598103934665603ULL;
            for (auto const& [pos, base] : node.cg()) {
              cg_seed = mix_u64(cg_seed, static_cast<std::uint64_t>(pos));
              cg_seed = mix_u64(cg_seed,
                                static_cast<std::uint64_t>(base.raw()));
            }
            leaf_hashes.emplace_back(std::string{node.sample_id()}, cg_seed);
          } else {
            throw std::runtime_error(
                "chart SPR pattern fingerprint: reachable leaf node lacks "
                "sample_id/cg annotations");
          }
        },
        nv);
  }
  std::sort(leaf_hashes.begin(), leaf_hashes.end());
  std::uint64_t cg_seed = leaf_hashes.size();
  for (auto const& [sample_id, hash] : leaf_hashes) {
    cg_seed = mix_u64(cg_seed, hash_string_u64(sample_id));
    cg_seed = mix_u64(cg_seed, hash);
  }
  fp.compact_genome_hash = cg_seed;

  std::uint64_t registry_seed = grammar.taxa.id_to_sample_id.size();
  for (std::size_t id = 0; id < grammar.taxa.id_to_sample_id.size(); ++id) {
    registry_seed = mix_u64(registry_seed, static_cast<std::uint64_t>(id));
    registry_seed = mix_u64(registry_seed,
                            hash_string_u64(grammar.taxa.id_to_sample_id[id]));
  }
  std::vector<std::pair<std::string, taxon_id>> taxon_map_entries;
  taxon_map_entries.reserve(grammar.taxa.sample_id_to_id.size());
  for (auto const& [sample_id, id] : grammar.taxa.sample_id_to_id) {
    taxon_map_entries.emplace_back(sample_id, id);
  }
  std::sort(taxon_map_entries.begin(), taxon_map_entries.end());
  for (auto const& [sample_id, id] : taxon_map_entries) {
    registry_seed = mix_u64(registry_seed, hash_string_u64(sample_id));
    registry_seed = mix_u64(registry_seed, static_cast<std::uint64_t>(id));
  }
  fp.taxon_registry_hash = registry_seed;
  return fp;
}

inline bool chart_spr_pattern_source_fingerprint_matches(
    phylo_dag& dag, clade_grammar const& grammar,
    chart_spr_pattern_source_fingerprint const& expected) {
  return build_chart_spr_pattern_source_fingerprint(dag, grammar) == expected;
}

inline chart_spr_active_pattern_build_result make_active_search_patterns(
    site_pattern_set const& source_patterns,
    chart_options const& chart_opts = {}) {
  chart_spr_active_pattern_build_result result;
  auto& active = result.active_patterns.patterns;
  active.taxon_count = source_patterns.taxon_count;

  result.skipped_invariant_site_count =
      source_patterns.skipped_invariant_site_count;
  if (chart_opts.score_ua_edge) {
    result.invariant_constant_offset =
        chart_multisite_detail::checked_add_u64(
            result.invariant_constant_offset,
            source_patterns
                .skipped_invariant_constant_score_with_reference_edge,
            "chart-SPR skipped invariant UA-edge offset");
  }

  active.patterns.reserve(source_patterns.patterns.size());
  for (std::size_t pattern_index = 0;
       pattern_index < source_patterns.patterns.size(); ++pattern_index) {
    auto const& pattern = source_patterns.patterns[pattern_index];
    if (chart_opts.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(pattern,
                                                                pattern_index);
    }
    if (is_invariant_site_pattern(pattern)) {
      result.skipped_invariant_site_count += pattern.weight;
      if (chart_opts.score_ua_edge) {
        result.invariant_constant_offset =
            chart_multisite_detail::checked_add_u64(
                result.invariant_constant_offset,
                chart_spr_search_detail::
                    invariant_pattern_reference_edge_offset(pattern),
                "chart-SPR invariant pattern UA-edge offset");
      }
      continue;
    }
    chart_spr_search_detail::append_active_pattern_metadata(active, pattern);
  }

  active.exact_pattern_to_normalized_binary_pattern.assign(
      active.patterns.size(), no_site_pattern);
  active.exact_pattern_to_normalized_binary_state_map.assign(
      active.patterns.size(), normalized_binary_state_map{});

  result.active_patterns.assert_no_skipped_invariant_metadata();
  return result;
}

inline chart_spr_active_pattern_build_result make_active_search_patterns(
    phylo_dag& dag, clade_grammar const& grammar,
    chart_options const& chart_opts = {},
    site_pattern_options pattern_opts = {}) {
  pattern_opts.skip_invariant_sites = true;
  auto raw_patterns = build_site_patterns(dag, grammar, pattern_opts);
  auto result = make_active_search_patterns(raw_patterns, chart_opts);
  result.pattern_source_fingerprint =
      build_chart_spr_pattern_source_fingerprint(dag, grammar);
  return result;
}

inline composite_chart_score build_composite_chart_score_active(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_options const& options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_composite_chart_score(grammar, patterns.patterns, options);
}

inline multisite_trim_result build_multisite_trim_active(
    clade_grammar const& grammar, active_site_pattern_set const& patterns,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  patterns.assert_no_skipped_invariant_metadata();
  return build_multisite_trim(grammar, patterns.patterns, options,
                              trim_options);
}

struct chart_spr_search_state {
  phylo_dag* dag = nullptr;
  clade_grammar grammar;

  // Active/topology-informative patterns only.  Invariant-site metadata is
  // stored as invariant_constant_offset/skipped_invariant_site_count below and
  // added exactly once at objective/reporting boundaries.
  active_site_pattern_set active_patterns;
  chart_spr_pattern_source_fingerprint pattern_source_fingerprint;
  std::uint64_t invariant_constant_offset = 0;
  std::size_t skipped_invariant_site_count = 0;

  chart_options chart_opts;
  std::vector<pattern_chart_cache_entry> pattern_charts;
  std::uint64_t composite_lower_bound_without_invariants = 0;
  std::uint64_t composite_lower_bound_with_invariants = 0;

  // Active-pattern-only exact result.  Add invariant_constant_offset exactly
  // once when comparing/reporting the full objective.
  std::optional<multisite_trim_result> exact_trim_active_only;

  chart_spr_search_counters counters;
};

inline std::size_t estimate_chart_spr_pattern_cache_bytes(
    chart_spr_search_state const& state) {
  std::size_t total = state.pattern_charts.size() *
                      sizeof(pattern_chart_cache_entry);
  for (auto const& entry : state.pattern_charts) {
    total += entry.chart.inside.size() * sizeof(entry.chart.inside.front());
    total += entry.chart.optimal_choices.size() *
             sizeof(decltype(entry.chart.optimal_choices)::value_type);
  }
  return total;
}

inline chart_spr_search_state build_chart_spr_search_state_from_active(
    phylo_dag& dag, clade_grammar grammar,
    chart_spr_active_pattern_build_result active_build,
    chart_options options = {}, bool build_exact_trim = false,
    multisite_trim_options const& trim_options = {}) {
  chart_spr_search_detail::validate_binary_chart_compatible_grammar(grammar);
  active_build.active_patterns.assert_no_skipped_invariant_metadata();
  chart_multisite_detail::validate_multisite_inputs(
      grammar, active_build.active_patterns.patterns, options);

  chart_spr_search_state state;
  state.dag = &dag;
  state.grammar = std::move(grammar);
  state.active_patterns = std::move(active_build.active_patterns);
  state.pattern_source_fingerprint =
      active_build.pattern_source_fingerprint;
  state.invariant_constant_offset = active_build.invariant_constant_offset;
  state.skipped_invariant_site_count =
      active_build.skipped_invariant_site_count;
  state.chart_opts = options;
  ++state.counters.base_chart_cache_rebuilds;
  state.counters.skipped_invariant_sites =
      state.skipped_invariant_site_count;

  auto chart_build_options = options;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;
  state.pattern_charts.reserve(
      state.active_patterns.patterns.patterns.size());

  std::uint64_t active_total = 0;
  for (auto const& pattern : state.active_patterns.patterns.patterns) {
    auto entry = chart_spr_search_detail::build_pattern_chart_cache_entry(
        state.grammar, pattern, options, chart_build_options);
    active_total = chart_multisite_detail::checked_add_u64(
        active_total, entry.weighted_root_score,
        "chart-SPR cached active-pattern lower bound");
    state.pattern_charts.push_back(std::move(entry));
  }

  state.composite_lower_bound_without_invariants = active_total;
  state.composite_lower_bound_with_invariants =
      chart_multisite_detail::checked_add_u64(
          active_total, state.invariant_constant_offset,
          "chart-SPR cached lower bound invariant offset");

  if (build_exact_trim) {
    state.exact_trim_active_only = build_multisite_trim_active(
        state.grammar, state.active_patterns, options, trim_options);
  }
  return state;
}

inline chart_spr_search_state build_chart_spr_search_state(
    phylo_dag& dag, clade_grammar grammar, site_pattern_set patterns,
    chart_options options = {}) {
  auto active_build = make_active_search_patterns(patterns, options);
  active_build.pattern_source_fingerprint =
      build_chart_spr_pattern_source_fingerprint(dag, grammar);
  return build_chart_spr_search_state_from_active(
      dag, std::move(grammar), std::move(active_build), options);
}

inline chart_spr_search_state build_chart_spr_search_state(
    phylo_dag& dag, clade_grammar grammar, chart_options options = {}) {
  auto active_build = make_active_search_patterns(dag, grammar, options);
  auto state = build_chart_spr_search_state_from_active(
      dag, std::move(grammar), std::move(active_build), options);
  ++state.counters.pattern_rebuilds;
  return state;
}

inline chart_spr_search_state build_chart_spr_search_state(
    phylo_dag& dag, clade_grammar grammar,
    chart_spr_search_options const& options) {
  validate_supported_chart_cache_options(options.cache);
  auto active_build = make_active_search_patterns(dag, grammar, options.chart);
  bool build_exact =
      options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite;
  auto state = build_chart_spr_search_state_from_active(
      dag, std::move(grammar), std::move(active_build), options.chart,
      build_exact, options.exact_trim);
  ++state.counters.pattern_rebuilds;
  return state;
}

// Phase-1 local scoring entry point.  This uses the existing materialized
// overlay-local recompute as the implementation bridge until the Phase-3
// lightweight overlay-delta scorer replaces the materialization step.  It
// reuses the state's cached base charts and does not call
// build_composite_chart_score() per rejected candidate.  Bridge overlay
// materializations are counted as local-scoring bridge work, not oracle work.
inline chart_spr_candidate_score score_candidate_locally(
    chart_spr_search_state& state, grammar_spr_candidate const& candidate) {
  state.active_patterns.assert_no_skipped_invariant_metadata();
  if (state.pattern_charts.size() !=
      state.active_patterns.patterns.patterns.size()) {
    throw std::runtime_error(
        "chart SPR local score: pattern chart cache size mismatch");
  }

  auto overlay = overlay_from_candidate(state.grammar, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  ++state.counters.full_overlay_materializations;
  ++state.counters.overlay_materializations_for_local_scoring_bridge;
  ++state.counters.local_candidate_scores;

  std::uint64_t new_active_score = 0;
  std::size_t affected_count = 0;
  auto chart_build_options = state.chart_opts;
  chart_build_options.keep_trace = false;
  chart_build_options.max_trace_choices = 0;

  for (std::size_t pattern_index = 0;
       pattern_index < state.active_patterns.patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern =
        state.active_patterns.patterns.patterns[pattern_index];
    auto const& cache_entry = state.pattern_charts[pattern_index];
    leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
    auto local = build_single_site_overlay_chart_locally(
        overlay, materialized, cache_entry.chart, states, chart_build_options);
    affected_count = std::max(affected_count, local.affected_clade_count);
    new_active_score = chart_multisite_detail::checked_add_u64(
        new_active_score,
        chart_multisite_detail::weighted_root_score_from_row(
            local.chart.inside[materialized.grammar.root_clade], pattern,
            state.chart_opts),
        "chart-SPR local candidate active lower bound");
  }

  auto new_score = chart_multisite_detail::checked_add_u64(
      new_active_score, state.invariant_constant_offset,
      "chart-SPR local candidate invariant offset");
  auto old_score = state.composite_lower_bound_with_invariants;
  chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.lower_bound.value = spr_score_result{
      chart_spr_detail::signed_delta(old_score, new_score), old_score,
      new_score, false};
  scored.lower_bound.kind = chart_spr_score_kind::composite_lower_bound;
  scored.lower_bound.convention =
      chart_spr_score_convention::full_with_invariants;
  scored.lower_bound.invariant_offset_applied =
      state.invariant_constant_offset;
  scored.affected_clade_count = affected_count;
  return scored;
}

inline affected_clade_distribution summarize_affected_clade_counts(
    std::vector<std::size_t> counts) {
  affected_clade_distribution summary;
  if (counts.empty()) return summary;
  std::sort(counts.begin(), counts.end());
  std::uint64_t sum = 0;
  for (auto count : counts) sum += count;
  summary.mean = static_cast<double>(sum) / static_cast<double>(counts.size());
  summary.p50 = counts[counts.size() / 2];
  auto p95_index = (counts.size() * 95 + 99) / 100;
  if (p95_index == 0) p95_index = 1;
  summary.p95 = counts[p95_index - 1];
  summary.max = counts.back();
  return summary;
}

inline void record_chart_spr_local_candidate_score(
    chart_spr_search_counters& counters) {
  ++counters.local_candidate_scores;
}

inline void record_chart_spr_rejected_candidate(
    chart_spr_search_counters& counters) {
  ++counters.rejected_moves;
}

// Diagnostic/oracle equivalent of the legacy lower-bound helper.  It
// materializes a dense overlay grammar and calls build_composite_chart_score()
// on both the base grammar and overlay grammar, counting each completed
// expensive operation so failed candidates do not over-report completed work.
inline spr_score_result score_multisite_spr_candidate_lower_bound_oracle(
    clade_grammar const& base, site_pattern_set const& patterns,
    grammar_spr_candidate const& candidate, chart_options const& options = {},
    chart_spr_search_counters* counters = nullptr) {
  auto overlay = overlay_from_candidate(base, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  if (counters != nullptr) {
    ++counters->full_overlay_materializations;
    ++counters->overlay_materializations_for_oracle;
  }

  auto old_score =
      build_composite_chart_score(base, patterns, options).weighted_lower_bound;
  if (counters != nullptr) ++counters->full_composite_rebuilds;
  auto new_score = build_composite_chart_score(materialized.grammar, patterns,
                                               options)
                       .weighted_lower_bound;
  if (counters != nullptr) ++counters->full_composite_rebuilds;
  return spr_score_result{
      chart_spr_detail::signed_delta(old_score, new_score), old_score,
      new_score, false};
}

// Diagnostic/oracle equivalent of the legacy exact helper.  It recomputes both
// old and new exact trims and materializes the overlay; it is suitable for small
// oracle checks, not production top-K exact verification.
inline spr_score_result score_multisite_spr_candidate_exact_oracle(
    clade_grammar const& base, site_pattern_set const& patterns,
    grammar_spr_candidate const& candidate, chart_options const& options = {},
    multisite_trim_options const& trim_options = {},
    chart_spr_search_counters* counters = nullptr) {
  auto overlay = overlay_from_candidate(base, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  if (counters != nullptr) {
    ++counters->full_overlay_materializations;
    ++counters->overlay_materializations_for_oracle;
  }

  auto old_score = build_multisite_trim(base, patterns, options, trim_options)
                       .optimum;
  auto new_score = build_multisite_trim(materialized.grammar, patterns, options,
                                        trim_options)
                       .optimum;
  return spr_score_result{
      chart_spr_detail::signed_delta(old_score, new_score), old_score,
      new_score, true};
}

// Phase-0 diagnostic local recompute path used by guardrail tests and dagutil
// reporting until the lightweight overlay-delta scorer lands.  It exercises the
// existing local recompute oracle and records local candidate-score accounting;
// it must not increment full_composite_rebuilds.
inline single_site_overlay_recompute_result
score_rejected_candidate_with_local_recompute_oracle(
    overlay_clade_grammar const& overlay, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, chart_options const& options = {},
    chart_spr_search_counters* counters = nullptr) {
  auto result = build_single_site_overlay_chart_locally(
      overlay, base_chart, leaf_states, options);
  if (counters != nullptr) {
    ++counters->local_candidate_scores;
    ++counters->full_overlay_materializations;
    ++counters->overlay_materializations_for_oracle;
  }
  return result;
}

struct chart_spr_eager_candidate_enumeration_result {
  std::vector<grammar_spr_candidate> candidates;
  chart_spr_candidate_generation_stats stats;
};

namespace chart_spr_search_detail {

inline void add_path_stats(std::vector<chart_spr_detail::upward_path> const& paths,
                           chart_spr_candidate_generation_stats& stats,
                           chart_spr_search_counters* counters) {
  stats.upward_paths_completed += paths.size();
  if (counters != nullptr) counters->upward_paths_completed += paths.size();
  for (auto const& path : paths) {
    stats.upward_path_iterator_steps += path.size();
    if (counters != nullptr) counters->upward_path_iterator_steps += path.size();
  }
}

inline void note_pruned_before(chart_spr_candidate_generation_stats& stats,
                               chart_spr_search_counters* counters) {
  ++stats.candidates_pruned_before_construction;
  if (counters != nullptr) ++counters->candidates_pruned_before_construction;
}

inline void note_pruned_after(chart_spr_candidate_generation_stats& stats,
                              chart_spr_search_counters* counters) {
  ++stats.candidates_pruned_after_construction;
  if (counters != nullptr) ++counters->candidates_pruned_after_construction;
}

}  // namespace chart_spr_search_detail

// Phase-0 diagnostic enumerator that mirrors the current eager helper behavior
// while exposing counters.  It intentionally precomputes all upward paths before
// respecting max_candidates; benchmark output can therefore reveal the old
// source-path x destination-path hazard.  This is not the Phase-1 streaming API.
inline chart_spr_eager_candidate_enumeration_result
enumerate_grammar_spr_candidates_eager_diagnostic(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options options = {},
    chart_spr_search_counters* counters = nullptr) {
  using namespace chart_spr_detail;
  using namespace chart_spr_search_detail;
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);

  chart_spr_eager_candidate_enumeration_result result;
  auto base_lookup = build_clade_lookup(grammar);
  std::set<std::string> seen;

  std::vector<std::vector<upward_path>> paths_to_root(grammar.clades.size());
  for (clade_id cid = 0; cid < grammar.clades.size(); ++cid) {
    paths_to_root[cid] = enumerate_upward_paths_to_root(grammar, cid);
    add_path_stats(paths_to_root[cid], result.stats, counters);
  }

  for (std::size_t source_pid_raw = 0;
       source_pid_raw < grammar.productions.size(); ++source_pid_raw) {
    auto source_pid = static_cast<production_id>(source_pid_raw);
    if (counters != nullptr) ++counters->candidate_source_productions_considered;
    auto const& source_prod = grammar.productions[source_pid];
    if (source_prod.children.size() != 2) continue;

    for (std::size_t moved_i = 0; moved_i < 2; ++moved_i) {
      auto moved = source_prod.children[moved_i];
      auto old_sibling = source_prod.children[1 - moved_i];
      auto const& moved_taxa = grammar.clades[moved].taxa;

      for (clade_id target = 0; target < grammar.clades.size(); ++target) {
        if (target == moved || target == old_sibling ||
            target == source_prod.parent) {
          note_pruned_before(result.stats, counters);
          continue;
        }
        auto const& target_taxa = grammar.clades[target].taxa;
        if (!disjoint_taxa(moved_taxa, target_taxa)) {
          note_pruned_before(result.stats, counters);
          continue;
        }

        for (auto const& source_path : paths_to_root[source_prod.parent]) {
          for (auto const& dest_path : paths_to_root[target]) {
            ++result.stats.path_pairs_considered;
            if (counters != nullptr) ++counters->path_pairs_considered;

            auto candidate = make_general_spr_candidate(
                grammar, base_lookup, source_pid, moved, old_sibling, target,
                source_path, dest_path);
            if (!candidate) {
              note_pruned_after(result.stats, counters);
              continue;
            }
            ++result.stats.candidates_constructed;
            if (counters != nullptr) ++counters->candidates_constructed;

            auto signature = candidate_signature(*candidate);
            if (!seen.insert(std::move(signature)).second) {
              note_pruned_after(result.stats, counters);
              continue;
            }

            result.candidates.push_back(std::move(*candidate));
            ++result.stats.candidates_generated_after_dedup;
            if (counters != nullptr) ++counters->candidates_generated_after_dedup;

            if (options.max_candidates != 0 &&
                result.candidates.size() >= options.max_candidates) {
              result.stats.stop_reason =
                  chart_spr_candidate_stop_reason::candidate_cap;
              if (counters != nullptr) ++counters->candidate_cap_cutoffs;
              return result;
            }
          }
        }
      }
    }
  }

  result.stats.stop_reason = chart_spr_candidate_stop_reason::exhausted;
  return result;
}

}  // namespace larch

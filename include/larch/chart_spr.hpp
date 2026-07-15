#pragma once

#include <larch/chart_scheduler.hpp>
#include <larch/chart_trim.hpp>
#include <larch/native_optimize.hpp>
#include <larch/site_patterns.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace larch {

// Single chart-SPR root-row scoring entry point.  It delegates to the checked
// multi-site chart helper so every search cache/local scorer handles
// score_ua_edge=true compressed patterns the same way: reference-state counts
// are applied to the full root row, not to a pre-collapsed scalar root minimum.
inline std::uint64_t chart_spr_weighted_root_score_from_row(
    std::array<chart_cost, nuc_state_count> const& root_row,
    site_pattern const& pattern, chart_options const& options) {
  return chart_multisite_detail::weighted_root_score_from_row(root_row, pattern,
                                                              options);
}

// Sidecar SPR / rewrite overlay IDs deliberately keep base and temporary ID
// spaces distinct at API boundaries.  Dense integer remaps are built only as an
// implementation detail when a chart over an overlay grammar is required.
enum class overlay_id_space : std::uint8_t { base, temp };

struct overlay_production_ref {
  overlay_id_space space = overlay_id_space::base;
  production_id id = no_production;

  bool operator==(overlay_production_ref const&) const = default;
  auto operator<=>(overlay_production_ref const& other) const {
    if (space != other.space) return space <=> other.space;
    return id <=> other.id;
  }
};

struct overlay_clade_ref {
  overlay_id_space space = overlay_id_space::base;
  clade_id id = no_clade;

  bool operator==(overlay_clade_ref const&) const = default;
  auto operator<=>(overlay_clade_ref const& other) const {
    if (space != other.space) return space <=> other.space;
    return id <=> other.id;
  }
};

struct overlay_clade_ref_hash {
  std::size_t operator()(overlay_clade_ref ref) const noexcept {
    auto space = static_cast<std::size_t>(ref.space);
    return (space << 32U) ^ static_cast<std::size_t>(ref.id);
  }
};

struct overlay_production_ref_hash {
  std::size_t operator()(overlay_production_ref ref) const noexcept {
    auto space = static_cast<std::size_t>(ref.space);
    return (space << 32U) ^ static_cast<std::size_t>(ref.id);
  }
};

inline overlay_clade_ref base_clade_ref(clade_id id) {
  return overlay_clade_ref{overlay_id_space::base, id};
}

inline overlay_clade_ref temp_clade_ref(clade_id id) {
  return overlay_clade_ref{overlay_id_space::temp, id};
}

inline overlay_production_ref base_production_ref(production_id id) {
  return overlay_production_ref{overlay_id_space::base, id};
}

inline overlay_production_ref temp_production_ref(production_id id) {
  return overlay_production_ref{overlay_id_space::temp, id};
}

struct overlay_grammar_production {
  overlay_clade_ref parent;
  std::vector<overlay_clade_ref> children;
  std::vector<production_witness> witnesses;
  std::uint64_t multiplicity = 0;
};

struct overlay_clade_grammar {
  clade_grammar const* base = nullptr;
  std::vector<clade_key> temp_clades;
  std::vector<overlay_grammar_production> temp_productions;
  std::vector<production_id> removed_base_productions;
};

struct grammar_spr_candidate {
  overlay_clade_ref moved_clade;
  overlay_clade_ref old_parent;
  overlay_clade_ref old_sibling;
  overlay_clade_ref new_sibling_or_target;
  std::vector<overlay_production_ref> removed_productions;
  std::vector<clade_key> added_clades;
  std::vector<overlay_grammar_production> added_productions;

  // Optional source move from a sampled tree, when a candidate was bootstrapped
  // from the existing tree-centric SPR enumerator.
  std::optional<spr_move> source_tree_move;

  // When source_tree_move is present, these optional complete topology
  // certificates identify the exact before/after sampled-tree topology that
  // produced the move.  Fixed-topology exact scoring can consume these refs so
  // sampled-tree benchmarks score the sampled SPR move, not an arbitrary
  // deterministic reachable topology from the overlay grammar.
  std::optional<std::vector<overlay_production_ref>>
      source_before_topology_productions;
  std::optional<std::vector<overlay_production_ref>>
      source_after_topology_productions;
};

struct spr_score_result {
  std::int64_t delta = 0;
  std::uint64_t old_score = 0;
  std::uint64_t new_score = 0;
  bool exact_multisite = false;

  [[nodiscard]] bool improves() const { return delta < 0; }
};

struct overlay_materialization_result {
  clade_grammar grammar;
  std::vector<overlay_clade_ref> dense_clade_to_ref;
  std::vector<overlay_production_ref> dense_production_to_ref;
  std::vector<clade_id> base_clade_to_dense;
  std::vector<clade_id> temp_clade_to_dense;
  std::vector<production_id> base_production_to_dense;
  std::vector<production_id> temp_production_to_dense;
};

struct overlay_payload_validation_stats {
  // Dynamic temp productions checked before reachability filtering.  This
  // includes unreachable payload that the dense output plan cannot observe.
  std::size_t production_partition_validations = 0;
};

// A dense overlay grammar and the immutable plan compiled at the same
// publication boundary.  Trusted search paths use this paired result so the
// freshly produced grammar is validated exactly once (by plan construction)
// before either object can escape to a recurrence consumer.
struct planned_overlay_materialization_result {
  overlay_materialization_result materialized;
  chart_execution_plan execution_plan;
  overlay_payload_validation_stats payload_validation_stats;
};

struct single_site_overlay_recompute_result {
  single_site_chart chart;
  std::vector<bool> affected_clade;
  std::size_t affected_clade_count = 0;
  bool used_full_rebuild = false;
};

enum class chart_spr_candidate_source {
  grammar,
  sampled_tree,
  hybrid,
};

inline char const* chart_spr_candidate_source_name(
    chart_spr_candidate_source source) {
  switch (source) {
    case chart_spr_candidate_source::grammar:
      return "grammar";
    case chart_spr_candidate_source::sampled_tree:
      return "sampled_tree";
    case chart_spr_candidate_source::hybrid:
      return "hybrid";
  }
  return "unknown";
}

// Candidate cap/budget stop reason for the public streaming enumerator.
enum class chart_spr_candidate_stop_reason {
  exhausted,
  candidate_cap,
  path_budget,
  callback_stop,
};

inline char const* chart_spr_candidate_stop_reason_name(
    chart_spr_candidate_stop_reason reason) {
  switch (reason) {
    case chart_spr_candidate_stop_reason::exhausted:
      return "exhausted";
    case chart_spr_candidate_stop_reason::candidate_cap:
      return "candidate_cap";
    case chart_spr_candidate_stop_reason::path_budget:
      return "path_budget";
    case chart_spr_candidate_stop_reason::callback_stop:
      return "callback_stop";
  }
  return "unknown";
}

struct grammar_spr_enumeration_options {
  // Candidate cap semantics are post-dedup by default because that is what the
  // optimizer consumes.  Diagnostics should also report pre-dedup constructed
  // and pruned counts from chart_spr_candidate_generation_stats.
  std::size_t max_candidates = 0;  // 0 = unlimited
  bool max_candidates_is_post_dedup = true;

  // Path budgets stop lazy expansion before constructing later candidates.
  // 0 means unlimited.
  std::size_t max_upward_path_expansions = 0;
  std::size_t max_path_pairs_considered = 0;

  // Taxon-count filters; 0 upper bound means unlimited.
  std::size_t min_moved_clade_size = 1;
  std::size_t max_moved_clade_size = 0;
  std::size_t min_target_clade_size = 1;
  std::size_t max_target_clade_size = 0;
  std::size_t max_estimated_affected_clades = 0;

  bool include_root_moves = false;
  bool include_neutral_or_reversal_candidates = false;
  bool include_immediate_reversal_candidates = false;
  chart_spr_candidate_source source = chart_spr_candidate_source::grammar;

  // Source-mode controls for sampled-tree and hybrid enumeration.  The
  // representative topologies are sampled from the current grammar, while leaf
  // compact genomes/reference sequence come from sampled_tree_source_dag so the
  // native tree-SPR move scores remain parsimony-compatible with the DAG.
  // Search-state and dagutil entry points wire this automatically; direct
  // sampled-tree/hybrid callers must provide it.
  phylo_dag* sampled_tree_source_dag = nullptr;
  std::size_t sampled_tree_count = 1;
  std::size_t sampled_tree_spr_radius = 0;  // 0 = tree depth * 2
  int sampled_tree_score_threshold = std::numeric_limits<int>::max();

  // Stable taxon-key reversal filter populated by the search loop after an
  // accepted move.  Candidates with this key are skipped unless
  // include_immediate_reversal_candidates is true.
  std::string immediate_reversal_candidate_key_to_skip;

  bool randomize_order = false;
  bool reservoir_sample = false;
  std::uint32_t seed = 1;

  // Phase-8 sampled-tree projection uses the search's one persistent chart
  // scheduler when supplied. A null scheduler preserves the direct serial
  // library fallback. The scheduler and every borrowed input must outlive the
  // complete call.
  chart_scheduler* sampled_tree_projection_scheduler = nullptr;
  // Zero is the historical unlimited policy. A finite value covers external
  // resident bytes plus the prepared tree/jobs, bounded wave result payload,
  // active projection scratch, and scheduler ownership/operation envelope.
  std::size_t sampled_tree_projection_memory_budget_bytes = 0;
  std::size_t sampled_tree_projection_external_resident_bytes = 0;

  // Projection-only deterministic test hooks. They never alter production
  // ordering, and are invoked only after finite admission succeeds.
  std::function<void(std::size_t)> before_sampled_tree_projection_for_tests =
      {};
  std::function<void()>
      before_sampled_tree_projection_workspace_allocation_for_tests = {};
  std::optional<std::size_t>
      force_sampled_tree_projection_submit_failure_after_for_tests;
};

struct chart_spr_candidate_generation_stats {
  std::size_t upward_path_iterator_steps = 0;
  std::size_t upward_paths_completed = 0;
  std::size_t path_pairs_considered = 0;
  std::size_t candidates_constructed = 0;
  std::size_t candidates_pruned_before_construction = 0;
  std::size_t candidates_pruned_after_construction = 0;
  std::size_t candidates_generated_after_dedup = 0;

  // Reason-coded prune counters for Phase-6 diagnostics.  The aggregate
  // before/after counters above remain the stable compatibility fields.
  std::size_t candidates_pruned_root_or_trivial = 0;
  std::size_t candidates_pruned_moved_size = 0;
  std::size_t candidates_pruned_target_size = 0;
  std::size_t candidates_pruned_overlap = 0;
  std::size_t candidates_pruned_affected_estimate = 0;
  std::size_t candidates_pruned_immediate_reversal = 0;
  std::size_t candidates_pruned_duplicate = 0;
  std::size_t candidates_pruned_invalid = 0;
  std::size_t spr_multifurcation_moves_generated = 0;

  // Phase-8 sampled-tree projection diagnostics. Legacy candidate/prune/stop
  // counters above advance only during the serial gather. Speculative work
  // completed after a mid-wave stop is isolated here.
  std::size_t sampled_tree_projection_moves_preassigned = 0;
  std::size_t sampled_tree_projection_move_enumeration_visits = 0;
  std::size_t sampled_tree_projection_enumeration_passes = 0;
  std::size_t sampled_tree_projection_waves = 0;
  std::size_t sampled_tree_projection_scheduler_operations = 0;
  std::size_t sampled_tree_projection_parallel_operations = 0;
  std::size_t sampled_tree_projection_ranges = 0;
  std::size_t sampled_tree_projection_worker_tasks = 0;
  std::size_t sampled_tree_projection_active_worker_high_water = 0;
  std::size_t sampled_tree_projection_peak_wave_size = 0;
  std::size_t sampled_tree_projection_speculative_discarded = 0;
  std::size_t sampled_tree_projection_estimated_peak_bytes = 0;

  chart_spr_candidate_stop_reason stop_reason =
      chart_spr_candidate_stop_reason::exhausted;
};

struct tree_spr_bootstrap_options {
  // 0 means compute_tree_max_depth(tree) * 2, matching existing SPR defaults.
  std::size_t radius = 0;
  // 0 means unlimited.
  std::size_t max_candidates = 0;
  // move_enumerator emits moves with score_change <= score_threshold.  The
  // bootstrap default enumerates every bounded move, not only improving moves,
  // so validation can compare projected candidates broadly.
  int score_threshold = std::numeric_limits<int>::max();
};

struct sampled_tree_projection_memory_estimate {
  std::size_t external_resident_bytes = 0;
  // The sampled tree is owned by the enclosing sample iteration even though
  // the prepared context only borrows it. The base grammar and the original
  // sampled-tree source DAG remain external borrows and are counted only by
  // external_resident_bytes, avoiding accidental double accounting.
  std::size_t sampled_tree_resident_bytes = 0;
  std::size_t prepared_owned_and_job_bytes = 0;
  std::size_t serial_enumeration_scratch_bytes = 0;
  std::size_t wave_slot_and_payload_bytes = 0;
  std::size_t active_projection_scratch_bytes = 0;
  std::size_t scheduler_bytes = 0;
  std::size_t error_and_exception_bytes = 0;
  std::size_t required_peak_bytes = 0;
  std::size_t wave_size = 0;
  std::size_t active_projection_count = 0;
  bool safely_bounded = true;
};

class sampled_tree_projection_budget_error : public std::runtime_error {
 public:
  sampled_tree_projection_budget_error(std::size_t required_bytes,
                                       std::size_t budget_bytes)
      : std::runtime_error(
            "chart SPR sampled-tree projection admission requires " +
            std::to_string(required_bytes) +
            " bytes, exceeding configured projection budget " +
            std::to_string(budget_bytes)),
        required_bytes_{required_bytes},
        budget_bytes_{budget_bytes} {}

  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }
  [[nodiscard]] std::size_t budget_bytes() const noexcept {
    return budget_bytes_;
  }

 private:
  std::size_t required_bytes_;
  std::size_t budget_bytes_;
};

inline std::string chart_spr_candidate_taxon_signature(
    clade_grammar const& base, grammar_spr_candidate const& candidate);

namespace chart_spr_detail {

inline std::string clade_ref_to_string(overlay_clade_ref ref) {
  std::ostringstream out;
  out << (ref.space == overlay_id_space::base ? "base:" : "temp:") << ref.id;
  return out.str();
}

inline std::string production_ref_to_string(overlay_production_ref ref) {
  std::ostringstream out;
  out << (ref.space == overlay_id_space::base ? "base:" : "temp:") << ref.id;
  return out.str();
}

inline void validate_clade_key(clade_key const& key,
                               std::size_t taxon_count,
                               std::string const& label) {
  if (key.taxa.empty())
    throw std::runtime_error("chart SPR: empty clade key for " + label);
  if (!std::is_sorted(key.taxa.begin(), key.taxa.end()) ||
      std::adjacent_find(key.taxa.begin(), key.taxa.end()) != key.taxa.end()) {
    throw std::runtime_error(
        "chart SPR: clade key taxa must be sorted and unique for " + label);
  }
  for (auto taxon : key.taxa) {
    if (taxon >= taxon_count)
      throw std::runtime_error("chart SPR: taxon id out of range for " + label);
  }
}

inline void validate_clade_ref(overlay_clade_grammar const& overlay,
                               overlay_clade_ref ref,
                               std::string const& label) {
  if (overlay.base == nullptr)
    throw std::runtime_error("chart SPR: overlay has no base grammar");
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= overlay.base->clades.size()) {
      throw std::runtime_error("chart SPR: invalid base clade ref " +
                               clade_ref_to_string(ref) + " for " + label);
    }
  } else {
    if (ref.id == no_clade || ref.id >= overlay.temp_clades.size()) {
      throw std::runtime_error("chart SPR: invalid temp clade ref " +
                               clade_ref_to_string(ref) + " for " + label);
    }
  }
}

inline void validate_production_ref(overlay_clade_grammar const& overlay,
                                    overlay_production_ref ref,
                                    std::string const& label) {
  if (overlay.base == nullptr)
    throw std::runtime_error("chart SPR: overlay has no base grammar");
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_production || ref.id >= overlay.base->productions.size()) {
      throw std::runtime_error("chart SPR: invalid base production ref " +
                               production_ref_to_string(ref) + " for " + label);
    }
  } else {
    if (ref.id == no_production || ref.id >= overlay.temp_productions.size()) {
      throw std::runtime_error("chart SPR: invalid temp production ref " +
                               production_ref_to_string(ref) + " for " + label);
    }
  }
}

inline clade_key const& clade_key_for_ref(overlay_clade_grammar const& overlay,
                                          overlay_clade_ref ref) {
  validate_clade_ref(overlay, ref, "clade-key lookup");
  return ref.space == overlay_id_space::base ? overlay.base->clades[ref.id]
                                             : overlay.temp_clades[ref.id];
}

inline clade_id dense_clade_id(overlay_materialization_result const& mat,
                               overlay_clade_ref ref) {
  clade_id dense = no_clade;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= mat.base_clade_to_dense.size())
      throw std::runtime_error("chart SPR: base clade ref out of dense range");
    dense = mat.base_clade_to_dense[ref.id];
  } else {
    if (ref.id == no_clade || ref.id >= mat.temp_clade_to_dense.size())
      throw std::runtime_error("chart SPR: temp clade ref out of dense range");
    dense = mat.temp_clade_to_dense[ref.id];
  }
  if (dense == no_clade || dense >= mat.grammar.clades.size()) {
    throw std::runtime_error(
        "chart SPR: clade ref is not reachable in dense overlay grammar");
  }
  return dense;
}

inline std::vector<production_id> normalized_removed_base_productions(
    overlay_clade_grammar const& overlay) {
  if (overlay.base == nullptr)
    throw std::runtime_error("chart SPR: overlay has no base grammar");

  std::vector<production_id> removed = overlay.removed_base_productions;
  std::sort(removed.begin(), removed.end());
  removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
  for (auto pid : removed) {
    if (pid == no_production || pid >= overlay.base->productions.size()) {
      throw std::runtime_error("chart SPR: removed base production out of range");
    }
  }
  return removed;
}

inline void validate_overlay_payload(
    overlay_clade_grammar const& overlay,
    overlay_payload_validation_stats* stats = nullptr) {
  if (overlay.base == nullptr)
    throw std::runtime_error("chart SPR: overlay has no base grammar");

  if (overlay.temp_clades.size() >= static_cast<std::size_t>(no_clade)) {
    throw std::runtime_error("chart SPR: too many temporary overlay clades");
  }
  if (overlay.temp_productions.size() >=
      static_cast<std::size_t>(no_production)) {
    throw std::runtime_error(
        "chart SPR: too many temporary overlay productions");
  }

  auto taxon_count = overlay.base->taxa.id_to_sample_id.size();
  for (std::size_t i = 0; i < overlay.temp_clades.size(); ++i) {
    validate_clade_key(overlay.temp_clades[i], taxon_count,
                       "temp clade " + std::to_string(i));
  }
  (void)normalized_removed_base_productions(overlay);

  for (std::size_t i = 0; i < overlay.temp_productions.size(); ++i) {
    auto const& prod = overlay.temp_productions[i];
    validate_clade_ref(overlay, prod.parent,
                       "temp production " + std::to_string(i) + " parent");
    if (prod.children.size() < 2) {
      throw std::runtime_error("chart SPR: temp production " +
                               std::to_string(i) +
                               " has fewer than two children");
    }
    for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
      validate_clade_ref(overlay, prod.children[child_i],
                         "temp production " + std::to_string(i) +
                             " child " + std::to_string(child_i));
    }

    // Validate every dynamic production, including unreachable payload that
    // will not appear in the dense result and therefore cannot be checked by
    // the freshly built output plan.
    if (stats != nullptr) ++stats->production_partition_validations;
    chart_execution_plan_detail::
        record_dynamic_overlay_partition_validation();
    auto const& parent_taxa = clade_key_for_ref(overlay, prod.parent).taxa;
    std::vector<taxon_id> union_taxa;
    for (auto child_ref : prod.children) {
      auto const& child_taxa = clade_key_for_ref(overlay, child_ref).taxa;
      std::vector<taxon_id> overlap;
      std::set_intersection(union_taxa.begin(), union_taxa.end(),
                            child_taxa.begin(), child_taxa.end(),
                            std::back_inserter(overlap));
      if (!overlap.empty()) {
        throw std::runtime_error(
            "chart SPR: temp production " + std::to_string(i) +
            " children overlap");
      }
      std::vector<taxon_id> next;
      next.reserve(union_taxa.size() + child_taxa.size());
      std::set_union(union_taxa.begin(), union_taxa.end(),
                     child_taxa.begin(), child_taxa.end(),
                     std::back_inserter(next));
      union_taxa = std::move(next);
    }
    if (union_taxa != parent_taxa) {
      throw std::runtime_error(
          "chart SPR: temp production " + std::to_string(i) +
          " children do not partition the parent clade");
    }
  }
}

inline void validate_overlay(overlay_clade_grammar const& overlay) {
  if (overlay.base == nullptr)
    throw std::runtime_error("chart SPR: overlay has no base grammar");
  parsimony_chart_detail::validate_chart_grammar(*overlay.base);
  chart_execution_plan_detail::record_legacy_production_index_validation();
  chart_trim_detail::validate_production_indices(*overlay.base);
  validate_overlay_payload(overlay);
}

inline bool has_overlay_temp_production(
    std::vector<overlay_grammar_production> const& productions,
    overlay_clade_ref parent, std::vector<overlay_clade_ref> children) {
  std::sort(children.begin(), children.end());
  for (auto const& prod : productions) {
    auto prod_children = prod.children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod.parent == parent && prod_children == children) return true;
  }
  return false;
}

inline std::vector<taxon_id> set_union_taxa(std::vector<taxon_id> lhs,
                                            std::vector<taxon_id> const& rhs) {
  std::vector<taxon_id> out;
  out.reserve(lhs.size() + rhs.size());
  std::set_union(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                 std::back_inserter(out));
  return out;
}

inline std::vector<taxon_id> set_difference_taxa(
    std::vector<taxon_id> const& lhs, std::vector<taxon_id> const& rhs) {
  std::vector<taxon_id> out;
  std::set_difference(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                      std::back_inserter(out));
  return out;
}

inline bool disjoint_taxa(std::vector<taxon_id> const& lhs,
                          std::vector<taxon_id> const& rhs) {
  return std::none_of(lhs.begin(), lhs.end(), [&](taxon_id taxon) {
    return std::binary_search(rhs.begin(), rhs.end(), taxon);
  });
}

inline std::map<std::vector<taxon_id>, clade_id> build_clade_lookup(
    clade_grammar const& grammar) {
  std::map<std::vector<taxon_id>, clade_id> lookup;
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid)
    lookup.emplace(grammar.clades[cid].taxa, static_cast<clade_id>(cid));
  return lookup;
}

inline std::int64_t signed_delta(std::uint64_t old_score,
                                 std::uint64_t new_score) {
  auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (new_score >= old_score) {
    auto diff = new_score - old_score;
    return diff > max ? std::numeric_limits<std::int64_t>::max()
                      : static_cast<std::int64_t>(diff);
  }
  auto diff = old_score - new_score;
  return diff > max ? std::numeric_limits<std::int64_t>::min()
                    : -static_cast<std::int64_t>(diff);
}

inline std::uint64_t chart_root_score(single_site_chart const& chart,
                                      clade_id root,
                                      chart_options const& options,
                                      std::optional<std::uint8_t> reference) {
  chart_cost cost = chart_inf;
  if (options.score_ua_edge) {
    if (!reference) {
      throw std::runtime_error(
          "chart SPR: reference state is required when score_ua_edge is true");
    }
    cost = chart.root_min_with_reference_edge(root, *reference);
  } else {
    cost = chart.root_min_excluding_ua(root);
  }
  if (cost >= chart_inf)
    throw std::runtime_error("chart SPR: infinite root score");
  return cost;
}

inline std::array<chart_cost, nuc_state_count> leaf_row(
    clade_grammar const& grammar, clade_id clade,
    leaf_site_states const& leaf_states) {
  if (grammar.clades[clade].taxa.size() != 1) {
    throw std::runtime_error("chart SPR: leaf row requested for non-leaf clade");
  }
  auto taxon = grammar.clades[clade].taxa.front();
  if (taxon >= leaf_states.state_by_taxon.size()) {
    throw std::runtime_error("chart SPR: leaf taxon out of state range");
  }
  auto observed = leaf_states.state_by_taxon[taxon];
  parsimony_chart_detail::validate_state(observed, "overlay leaf state");
  auto row = parsimony_chart_detail::make_inf_row();
  row[observed] = 0;
  return row;
}

inline void recompute_single_inside_row(clade_grammar const& grammar,
                                        leaf_site_states const& leaf_states,
                                        single_site_chart& chart,
                                        clade_id clade) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("chart SPR: recompute clade out of range");
  }

  if (grammar.clades[clade].taxa.size() == 1) {
    if (!grammar.productions_by_parent[clade].empty()) {
      throw std::runtime_error(
          "chart SPR: singleton clade has productions in overlay grammar");
    }
    chart.inside[clade] = leaf_row(grammar, clade, leaf_states);
    return;
  }

  auto const& parent_productions = grammar.productions_by_parent[clade];
  if (parent_productions.empty()) {
    throw std::runtime_error(
        "chart SPR: non-singleton overlay clade has no productions");
  }

  auto row = parsimony_chart_detail::make_inf_row();
  for (auto pid : parent_productions) {
    if (pid == no_production || pid >= grammar.productions.size())
      throw std::runtime_error("chart SPR: production id out of range");
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade)
      throw std::runtime_error("chart SPR: production parent mismatch");
    parsimony_chart_detail::validate_production_inside_row_inputs(
        grammar, prod, pid, "chart SPR");
    if (prod.children.size() != 2) {
      ++chart.multifurcation_productions_scored;
    }

    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      auto row_provider = [&](clade_id child) -> auto const& {
        if (child == no_clade || child >= chart.inside.size()) {
          throw std::runtime_error("chart SPR: production child out of range");
        }
        return chart.inside[child];
      };
      auto total = parsimony_chart_detail::combine_production_inside_row(
          prod, parent_state, row_provider);
      row[parent_state] = std::min(row[parent_state], total);
    }
  }
  chart.inside[clade] = row;
}

inline std::string candidate_signature(grammar_spr_candidate const& candidate) {
  std::ostringstream out;
  out << "m=" << clade_ref_to_string(candidate.moved_clade)
      << ";op=" << clade_ref_to_string(candidate.old_parent)
      << ";os=" << clade_ref_to_string(candidate.old_sibling)
      << ";nt=" << clade_ref_to_string(candidate.new_sibling_or_target)
      << ";rm=";
  auto removed = candidate.removed_productions;
  std::sort(removed.begin(), removed.end());
  for (auto ref : removed) out << production_ref_to_string(ref) << ",";
  out << ";clades=";
  for (auto const& key : candidate.added_clades) {
    out << "{";
    for (auto taxon : key.taxa) out << taxon << ",";
    out << "}";
  }
  out << ";prods=";
  for (auto const& prod : candidate.added_productions) {
    out << clade_ref_to_string(prod.parent) << "->";
    auto children = prod.children;
    std::sort(children.begin(), children.end());
    for (auto child : children) out << clade_ref_to_string(child) << ",";
    out << ";";
  }
  return out.str();
}

inline std::vector<taxon_id> convert_tree_clade_taxa_to_base_taxa(
    clade_grammar const& base, clade_grammar const& tree_grammar,
    clade_id tree_clade) {
  if (tree_clade == no_clade || tree_clade >= tree_grammar.clades.size()) {
    throw std::runtime_error("chart SPR: tree clade out of range");
  }
  std::vector<taxon_id> converted;
  converted.reserve(tree_grammar.clades[tree_clade].taxa.size());
  for (auto tree_taxon : tree_grammar.clades[tree_clade].taxa) {
    if (tree_taxon >= tree_grammar.taxa.id_to_sample_id.size()) {
      throw std::runtime_error("chart SPR: tree taxon out of range");
    }
    auto const& sample_id = tree_grammar.taxa.id_to_sample_id[tree_taxon];
    auto found = base.taxa.sample_id_to_id.find(sample_id);
    if (found == base.taxa.sample_id_to_id.end()) {
      throw std::runtime_error("chart SPR: tree taxon '" + sample_id +
                               "' is absent from base grammar");
    }
    converted.push_back(found->second);
  }
  std::sort(converted.begin(), converted.end());
  converted.erase(std::unique(converted.begin(), converted.end()),
                  converted.end());
  return converted;
}

inline std::optional<clade_id> tree_node_to_base_clade(
    clade_grammar const& base, clade_grammar const& tree_grammar,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::size_t tree_node) {
  if (tree_node >= tree_grammar.node_to_clade.size()) return std::nullopt;
  auto tree_clade = tree_grammar.node_to_clade[tree_node];
  if (tree_clade == no_clade) return std::nullopt;
  auto base_taxa = convert_tree_clade_taxa_to_base_taxa(base, tree_grammar,
                                                        tree_clade);
  auto found = base_lookup.find(base_taxa);
  if (found == base_lookup.end()) return std::nullopt;
  return found->second;
}

inline overlay_clade_ref add_or_get_candidate_clade(
    clade_grammar const& grammar, grammar_spr_candidate& candidate,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::map<std::vector<taxon_id>, clade_id>& temp_lookup,
    std::vector<taxon_id> taxa) {
  std::sort(taxa.begin(), taxa.end());
  taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
  auto base_found = base_lookup.find(taxa);
  if (base_found != base_lookup.end()) return base_clade_ref(base_found->second);

  auto temp_found = temp_lookup.find(taxa);
  if (temp_found != temp_lookup.end()) return temp_clade_ref(temp_found->second);

  if (candidate.added_clades.size() >= static_cast<std::size_t>(no_clade)) {
    throw std::runtime_error("chart SPR: too many temp candidate clades");
  }
  auto temp_id = static_cast<clade_id>(candidate.added_clades.size());
  candidate.added_clades.push_back(clade_key{taxa});
  temp_lookup.emplace(std::move(taxa), temp_id);
  (void)grammar;
  return temp_clade_ref(temp_id);
}

inline std::vector<clade_id> refs_to_base_children_if_possible(
    std::vector<overlay_clade_ref> const& refs) {
  std::vector<clade_id> ids;
  ids.reserve(refs.size());
  for (auto ref : refs) {
    if (ref.space != overlay_id_space::base) return {};
    ids.push_back(ref.id);
  }
  return ids;
}

inline bool candidate_removes_base_production(
    grammar_spr_candidate const& candidate, production_id pid) {
  return std::any_of(candidate.removed_productions.begin(),
                     candidate.removed_productions.end(), [&](auto ref) {
                       return ref.space == overlay_id_space::base &&
                              ref.id == pid;
                     });
}

inline bool has_available_base_production(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate,
    clade_id parent, std::vector<clade_id> children,
    std::optional<production_id> ignore = {}) {
  if (parent == no_clade || parent >= grammar.productions_by_parent.size())
    return false;
  std::sort(children.begin(), children.end());
  for (auto pid : grammar.productions_by_parent[parent]) {
    if (ignore && *ignore == pid) continue;
    if (candidate_removes_base_production(candidate, pid)) continue;
    auto prod_children = grammar.productions[pid].children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == children) return true;
  }
  return false;
}

inline void maybe_add_candidate_production(
    clade_grammar const& grammar, grammar_spr_candidate& candidate,
    overlay_clade_ref parent, std::vector<overlay_clade_ref> children,
    std::optional<production_id> ignore_base_pid = {}) {
  std::sort(children.begin(), children.end());

  if (parent.space == overlay_id_space::base) {
    auto base_children = refs_to_base_children_if_possible(children);
    if (!base_children.empty() &&
        has_available_base_production(grammar, candidate, parent.id,
                                      base_children, ignore_base_pid)) {
      return;
    }
  }
  if (has_overlay_temp_production(candidate.added_productions, parent,
                                  children)) {
    return;
  }

  overlay_grammar_production prod;
  prod.parent = parent;
  prod.children = std::move(children);
  prod.multiplicity = 1;
  candidate.added_productions.push_back(std::move(prod));
}

struct production_taxa_key {
  std::vector<taxon_id> parent;
  std::vector<std::vector<taxon_id>> children;

  bool operator==(production_taxa_key const&) const = default;
  bool operator<(production_taxa_key const& other) const {
    return std::tie(parent, children) < std::tie(other.parent, other.children);
  }
};

inline void normalize_production_taxa_key(production_taxa_key& key) {
  std::sort(key.parent.begin(), key.parent.end());
  key.parent.erase(std::unique(key.parent.begin(), key.parent.end()),
                   key.parent.end());
  for (auto& child : key.children) {
    std::sort(child.begin(), child.end());
    child.erase(std::unique(child.begin(), child.end()), child.end());
  }
  std::sort(key.children.begin(), key.children.end());
}

inline production_taxa_key production_key_from_tree_in_base_taxa(
    clade_grammar const& base, clade_grammar const& tree_grammar,
    grammar_production const& prod) {
  production_taxa_key key;
  key.parent = convert_tree_clade_taxa_to_base_taxa(base, tree_grammar,
                                                    prod.parent);
  key.children.reserve(prod.children.size());
  for (auto child : prod.children) {
    key.children.push_back(
        convert_tree_clade_taxa_to_base_taxa(base, tree_grammar, child));
  }
  normalize_production_taxa_key(key);
  return key;
}

inline std::optional<production_id> find_base_production_by_key(
    clade_grammar const& grammar,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    production_taxa_key const& key) {
  auto parent_it = base_lookup.find(key.parent);
  if (parent_it == base_lookup.end()) return std::nullopt;

  std::vector<clade_id> child_ids;
  child_ids.reserve(key.children.size());
  for (auto const& child_taxa : key.children) {
    auto child_it = base_lookup.find(child_taxa);
    if (child_it == base_lookup.end()) return std::nullopt;
    child_ids.push_back(child_it->second);
  }
  std::sort(child_ids.begin(), child_ids.end());

  for (auto pid : grammar.productions_by_parent[parent_it->second]) {
    auto prod_children = grammar.productions[pid].children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == child_ids) return pid;
  }
  return std::nullopt;
}

inline std::vector<taxon_id> candidate_clade_ref_taxa_copy(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_clade_ref ref) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= base.clades.size()) {
      throw std::runtime_error(
          "chart SPR sampled-tree certificate: base clade ref out of range");
    }
    return base.clades[ref.id].taxa;
  }
  if (ref.id == no_clade || ref.id >= candidate.added_clades.size()) {
    throw std::runtime_error(
        "chart SPR sampled-tree certificate: temp clade ref out of range");
  }
  return candidate.added_clades[ref.id].taxa;
}

inline production_taxa_key production_key_from_candidate_production(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_grammar_production const& prod) {
  production_taxa_key key;
  key.parent = candidate_clade_ref_taxa_copy(base, candidate, prod.parent);
  key.children.reserve(prod.children.size());
  for (auto child : prod.children) {
    key.children.push_back(
        candidate_clade_ref_taxa_copy(base, candidate, child));
  }
  normalize_production_taxa_key(key);
  return key;
}

inline std::optional<overlay_production_ref>
find_available_candidate_production_by_key(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    production_taxa_key const& key) {
  auto base_pid = find_base_production_by_key(base, base_lookup, key);
  if (base_pid && !candidate_removes_base_production(candidate, *base_pid)) {
    return base_production_ref(*base_pid);
  }
  for (std::size_t i = 0; i < candidate.added_productions.size(); ++i) {
    if (production_key_from_candidate_production(
            base, candidate, candidate.added_productions[i]) == key) {
      return temp_production_ref(static_cast<production_id>(i));
    }
  }
  return std::nullopt;
}

inline std::optional<std::vector<overlay_production_ref>>
source_before_topology_refs_from_tree_keys(
    clade_grammar const& base,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::vector<production_taxa_key> const& keys) {
  std::vector<overlay_production_ref> refs;
  refs.reserve(keys.size());
  for (auto const& key : keys) {
    auto base_pid = find_base_production_by_key(base, base_lookup, key);
    if (!base_pid) return std::nullopt;
    refs.push_back(base_production_ref(*base_pid));
  }
  return refs;
}

inline std::optional<std::vector<overlay_production_ref>>
source_after_topology_refs_from_tree_keys(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::vector<production_taxa_key> const& keys) {
  std::vector<overlay_production_ref> refs;
  refs.reserve(keys.size());
  for (auto const& key : keys) {
    auto ref = find_available_candidate_production_by_key(
        base, candidate, base_lookup, key);
    if (!ref) return std::nullopt;
    refs.push_back(*ref);
  }
  return refs;
}

// Immutable before-side state shared by every projected move from one sampled
// tree.  Preparation is deliberately the only operation that mutates the
// source tree (to publish clade offsets); projection itself only reads this
// snapshot and edits a task-local clone.  The base and tree are pinned borrows:
// both objects must remain alive, at the same addresses, and unmodified until
// every projection task using the context has joined.
class sampled_tree_projection_context {
 public:
  sampled_tree_projection_context(sampled_tree_projection_context const&) =
      delete;
  sampled_tree_projection_context& operator=(
      sampled_tree_projection_context const&) = delete;
  sampled_tree_projection_context(sampled_tree_projection_context&&) = delete;
  sampled_tree_projection_context& operator=(
      sampled_tree_projection_context&&) = delete;

  [[nodiscard]] clade_grammar const& base() const noexcept { return *base_; }
  [[nodiscard]] phylo_dag const& source_tree() const noexcept { return *tree_; }
  [[nodiscard]] tree_index const& index() const noexcept { return index_; }
  [[nodiscard]] std::map<std::vector<taxon_id>, clade_id> const& base_lookup()
      const noexcept {
    return base_lookup_;
  }
  [[nodiscard]] std::vector<clade_id> const& node_to_base_clade()
      const noexcept {
    return node_to_base_clade_;
  }
  [[nodiscard]] std::set<production_taxa_key> const& before_keys()
      const noexcept {
    return before_keys_;
  }
  [[nodiscard]] std::vector<production_taxa_key> const& ordered_before_keys()
      const noexcept {
    return ordered_before_keys_;
  }
  [[nodiscard]] std::optional<std::vector<overlay_production_ref>> const&
  source_before_topology_refs() const noexcept {
    return source_before_topology_refs_;
  }

 private:
  struct prepared_tag {};

  sampled_tree_projection_context(prepared_tag, clade_grammar const& base,
                                  phylo_dag& tree,
                                  clade_grammar before_tree_grammar)
      : base_{&base},
        tree_{&tree},
        base_lookup_{build_clade_lookup(base)},
        index_{tree} {
    node_to_base_clade_.assign(before_tree_grammar.node_to_clade.size(),
                               no_clade);
    for (std::size_t node = 0; node < before_tree_grammar.node_to_clade.size();
         ++node) {
      auto tree_clade = before_tree_grammar.node_to_clade[node];
      if (tree_clade == no_clade) continue;
      auto base_taxa = convert_tree_clade_taxa_to_base_taxa(
          base, before_tree_grammar, tree_clade);
      auto found = base_lookup_.find(base_taxa);
      if (found != base_lookup_.end())
        node_to_base_clade_[node] = found->second;
    }

    ordered_before_keys_.reserve(before_tree_grammar.productions.size());
    for (auto const& prod : before_tree_grammar.productions) {
      auto key = production_key_from_tree_in_base_taxa(
          base, before_tree_grammar, prod);
      before_keys_.insert(key);
      ordered_before_keys_.push_back(std::move(key));
    }
    source_before_topology_refs_ = source_before_topology_refs_from_tree_keys(
        base, base_lookup_, ordered_before_keys_);
  }

  friend sampled_tree_projection_context prepare_sampled_tree_projection(
      clade_grammar const&, phylo_dag&);

  clade_grammar const* base_ = nullptr;
  phylo_dag const* tree_ = nullptr;
  std::map<std::vector<taxon_id>, clade_id> base_lookup_;
  std::vector<clade_id> node_to_base_clade_;
  std::set<production_taxa_key> before_keys_;
  std::vector<production_taxa_key> ordered_before_keys_;
  std::optional<std::vector<overlay_production_ref>>
      source_before_topology_refs_;
  tree_index index_;
};

inline sampled_tree_projection_context prepare_sampled_tree_projection(
    clade_grammar const& base, phylo_dag& tree) {
  build_clade_offsets(tree);
  auto before_tree_grammar = build_clade_grammar(tree);
  return sampled_tree_projection_context{
      sampled_tree_projection_context::prepared_tag{}, base, tree,
      std::move(before_tree_grammar)};
}

inline sampled_tree_projection_context prepare_sampled_tree_projection(
    clade_grammar const&&, phylo_dag&) = delete;

struct sampled_tree_projection_job {
  std::size_t ordinal = 0;
  profitable_move move{};
};

struct sampled_tree_projection_execution_stats {
  std::size_t moves_preassigned = 0;
  std::size_t waves = 0;
  std::size_t scheduler_operations = 0;
  std::size_t parallel_operations = 0;
  std::size_t ranges = 0;
  std::size_t worker_tasks = 0;
  std::size_t active_worker_high_water = 0;
  std::size_t peak_wave_size = 0;
  std::size_t speculative_discarded = 0;
  sampled_tree_projection_memory_estimate memory;
};

struct sampled_tree_projection_preassignment {
  std::vector<sampled_tree_projection_job> jobs;
  sampled_tree_projection_memory_estimate memory;
  chart_scheduler const* admitted_scheduler = nullptr;
  std::size_t move_enumeration_visits = 0;
  std::size_t enumeration_passes = 0;
};

enum class sampled_tree_projection_gather_decision {
  continue_projection,
  stop_after_current,
  stop_before_current,
};

inline std::size_t sampled_tree_projection_saturating_add(
    std::size_t lhs, std::size_t rhs, bool& safely_bounded) noexcept {
  if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
    safely_bounded = false;
    return (std::numeric_limits<std::size_t>::max)();
  }
  return lhs + rhs;
}

inline std::size_t sampled_tree_projection_saturating_multiply(
    std::size_t lhs, std::size_t rhs, bool& safely_bounded) noexcept {
  if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
    safely_bounded = false;
    return (std::numeric_limits<std::size_t>::max)();
  }
  return lhs * rhs;
}

inline std::size_t sampled_tree_projection_saturating_scale(
    std::size_t value, std::size_t first, std::size_t second,
    bool& safely_bounded) noexcept {
  return sampled_tree_projection_saturating_multiply(
      sampled_tree_projection_saturating_multiply(value, first, safely_bounded),
      second, safely_bounded);
}

inline std::size_t estimate_sampled_tree_projection_sampled_tree_bytes(
    sampled_tree_projection_context const& prepared,
    bool& safely_bounded) noexcept {
  auto const node_count = prepared.source_tree().node_high_mark();
  auto const edge_count = prepared.source_tree().edge_high_mark();
  auto const taxon_count = prepared.base().taxa.id_to_sample_id.size();
  auto const variable_sites = prepared.index().num_variable_sites();
  std::size_t total = sizeof(phylo_dag);
  auto add_product = [&](std::size_t count, std::size_t bytes) {
    total = sampled_tree_projection_saturating_add(
        total,
        sampled_tree_projection_saturating_multiply(count, bytes,
                                                    safely_bounded),
        safely_bounded);
  };
  // Synthetic sampled-tree topology, leaf compact genomes/sample IDs, and UA
  // reference ownership. The original source DAG and base grammar are not
  // included: both are external borrows covered by external resident bytes.
  add_product(node_count, 24 * sizeof(void*) + 256);
  add_product(edge_count, 16 * sizeof(void*) + 128);
  add_product(taxon_count, 4 * sizeof(void*) + 128);
  add_product(taxon_count,
              sampled_tree_projection_saturating_scale(
                  variable_sites, 2, sizeof(std::uint32_t), safely_bounded));
  auto const& reference =
      prepared.source_tree().get_root_as<node_kind::ua>().reference_sequence();
  total = sampled_tree_projection_saturating_add(
      total,
      sampled_tree_projection_saturating_add(reference.size(), 1,
                                             safely_bounded),
      safely_bounded);
  for (auto const& sample_id : prepared.base().taxa.id_to_sample_id) {
    total = sampled_tree_projection_saturating_add(
        total,
        sampled_tree_projection_saturating_add(sample_id.size(), 1,
                                               safely_bounded),
        safely_bounded);
  }
  return total;
}

inline std::size_t
estimate_sampled_tree_projection_prepared_owned_and_job_bytes(
    sampled_tree_projection_context const& prepared, std::size_t job_count,
    bool& safely_bounded) noexcept {
  auto const node_count = prepared.source_tree().node_high_mark();
  auto const taxon_count = prepared.base().taxa.id_to_sample_id.size();
  auto const production_count = prepared.base().productions.size();
  auto const variable_sites = prepared.index().num_variable_sites();

  std::size_t total = sizeof(sampled_tree_projection_context) +
                      sizeof(std::vector<sampled_tree_projection_job>);
  auto add_product = [&](std::size_t count, std::size_t bytes) {
    total = sampled_tree_projection_saturating_add(
        total,
        sampled_tree_projection_saturating_multiply(count, bytes,
                                                    safely_bounded),
        safely_bounded);
  };
  // Only storage owned by the prepared context is charged here: base lookup,
  // node mapping, before production keys/certificates, and tree-index
  // recurrence tables. The borrowed tree is charged separately and borrowed
  // base/source payload is part of external resident bytes.
  add_product(taxon_count, 8 * sizeof(void*) + 128);
  add_product(node_count, 20 * sizeof(void*) + 256);
  add_product(node_count,
              sampled_tree_projection_saturating_scale(
                  taxon_count, 8, sizeof(taxon_id), safely_bounded));
  add_product(production_count, 16 * sizeof(void*) + 128);
  add_product(node_count, sampled_tree_projection_saturating_multiply(
                              variable_sites, 64, safely_bounded));
  // One bounded searchable-node ordering remains live while the exact move
  // count is replayed into the job vector. Its node-count envelope is included
  // in the allocation-free irreducible-base preflight.
  add_product(node_count, sizeof(std::size_t));
  add_product(job_count, sizeof(sampled_tree_projection_job));
  return total;
}

inline std::size_t estimate_sampled_tree_projection_task_scratch_bytes(
    sampled_tree_projection_context const& prepared,
    bool& safely_bounded) noexcept {
  auto const node_count = prepared.source_tree().node_high_mark();
  auto const edge_count = prepared.source_tree().edge_high_mark();
  auto const taxon_count = prepared.base().taxa.id_to_sample_id.size();
  auto const production_count =
      std::max(node_count, prepared.base().productions.size());
  std::size_t total =
      sizeof(phylo_dag) + sizeof(clade_grammar) + sizeof(grammar_spr_candidate);
  auto add_product = [&](std::size_t count, std::size_t bytes) {
    total = sampled_tree_projection_saturating_add(
        total,
        sampled_tree_projection_saturating_multiply(count, bytes,
                                                    safely_bounded),
        safely_bounded);
  };
  // Sparse clone topology, old-to-new map, after-grammar construction, and
  // before/after production-key maps. The taxon matrix dominates the latter.
  add_product(node_count, 20 * sizeof(void*) + 256);
  add_product(edge_count, 12 * sizeof(void*) + 128);
  add_product(production_count, 24 * sizeof(void*) + 256);
  add_product(node_count,
              sampled_tree_projection_saturating_scale(
                  taxon_count, 12, sizeof(taxon_id), safely_bounded));
  return total;
}

inline std::size_t estimate_sampled_tree_projection_enumeration_scratch_bytes(
    sampled_tree_projection_context const& prepared,
    bool& safely_bounded) noexcept {
  auto const variable_sites = prepared.index().num_variable_sites();
  auto total = sampled_tree_projection_saturating_add(
      sizeof(move_enumerator) + sizeof(scratch_buffers) +
          sizeof(move_enumerator::callback_t),
      512, safely_bounded);
  // find_moves_for_source owns five uint8_t site vectors in scratch_buffers.
  total = sampled_tree_projection_saturating_add(
      total,
      sampled_tree_projection_saturating_multiply(
          variable_sites, 5 * sizeof(std::uint8_t), safely_bounded),
      safely_bounded);
  return total;
}

inline std::size_t estimate_sampled_tree_projection_retained_candidate_bytes(
    sampled_tree_projection_context const& prepared,
    bool& safely_bounded) noexcept {
  auto const node_count = prepared.source_tree().node_high_mark();
  auto const taxon_count = prepared.base().taxa.id_to_sample_id.size();
  auto const production_count =
      std::max(node_count, prepared.base().productions.size());
  std::size_t total = sizeof(grammar_spr_candidate);
  auto add_product = [&](std::size_t count, std::size_t bytes) {
    total = sampled_tree_projection_saturating_add(
        total,
        sampled_tree_projection_saturating_multiply(count, bytes,
                                                    safely_bounded),
        safely_bounded);
  };
  // Worst-case owning clade taxa, added productions, tombstones, and complete
  // before/after topology certificates retained in one ordinal slot.
  add_product(node_count, sizeof(clade_key) + 4 * sizeof(void*));
  add_product(node_count, sampled_tree_projection_saturating_multiply(
                              taxon_count, sizeof(taxon_id), safely_bounded));
  add_product(production_count,
              sizeof(overlay_grammar_production) + 12 * sizeof(void*));
  add_product(production_count, 4 * sizeof(overlay_production_ref));
  return total;
}

inline std::size_t bounded_sampled_tree_projection_wave_size(
    chart_scheduler const* scheduler, std::size_t job_count) noexcept {
  if (job_count == 0) return 0;
  // Keep the direct-library fallback exactly serial: it neither speculates
  // beyond the next gathered move nor allocates a multi-result wave.
  if (scheduler == nullptr) return 1;
  auto const workers = scheduler->worker_resolution().resolved_workers;
  auto const bounded_workers = std::max<std::size_t>(1, workers);
  auto const maximum =
      bounded_workers > (std::numeric_limits<std::size_t>::max)() / 4
          ? (std::numeric_limits<std::size_t>::max)()
          : bounded_workers * 4;
  return std::min(job_count, maximum);
}

inline sampled_tree_projection_memory_estimate
estimate_sampled_tree_projection_memory(
    sampled_tree_projection_context const& prepared,
    chart_scheduler const* scheduler, std::size_t job_count,
    std::size_t wave_size, std::size_t external_resident_bytes) noexcept {
  sampled_tree_projection_memory_estimate result;
  result.external_resident_bytes = external_resident_bytes;
  result.wave_size = wave_size;
  result.safely_bounded = true;

  result.sampled_tree_resident_bytes =
      estimate_sampled_tree_projection_sampled_tree_bytes(
          prepared, result.safely_bounded);
  result.prepared_owned_and_job_bytes =
      estimate_sampled_tree_projection_prepared_owned_and_job_bytes(
          prepared, job_count, result.safely_bounded);
  result.serial_enumeration_scratch_bytes =
      estimate_sampled_tree_projection_enumeration_scratch_bytes(
          prepared, result.safely_bounded);
  auto const task_scratch = estimate_sampled_tree_projection_task_scratch_bytes(
      prepared, result.safely_bounded);
  auto const retained =
      estimate_sampled_tree_projection_retained_candidate_bytes(
          prepared, result.safely_bounded);

  chart_indexed_range_plan plan;
  if (scheduler != nullptr && wave_size != 0) {
    plan = scheduler->plan_indexed_ranges(
        wave_size, {.minimum_grain = 1, .target_ranges_per_worker = 1});
    result.active_projection_count =
        plan.range_count <= 1 ? std::size_t{1} : plan.worker_task_limit;
  } else if (wave_size != 0) {
    result.active_projection_count = 1;
  }

  result.wave_slot_and_payload_bytes =
      sizeof(std::vector<std::optional<grammar_spr_candidate>>);
  auto const per_slot = sampled_tree_projection_saturating_add(
      sizeof(std::optional<grammar_spr_candidate>), retained,
      result.safely_bounded);
  result.wave_slot_and_payload_bytes = sampled_tree_projection_saturating_add(
      result.wave_slot_and_payload_bytes,
      sampled_tree_projection_saturating_multiply(wave_size, per_slot,
                                                  result.safely_bounded),
      result.safely_bounded);
  result.active_projection_scratch_bytes =
      sampled_tree_projection_saturating_multiply(
          result.active_projection_count, task_scratch, result.safely_bounded);

  if (scheduler != nullptr) {
    result.scheduler_bytes =
        estimate_chart_scheduler_implementation_resident_bytes();
    try {
      result.scheduler_bytes = sampled_tree_projection_saturating_add(
          result.scheduler_bytes,
          estimate_chart_scheduler_pool_owning_heap_bytes(
              scheduler->worker_resolution().resolved_workers),
          result.safely_bounded);
      if (plan.range_count > 1 && plan.worker_task_limit > 1) {
        result.scheduler_bytes = sampled_tree_projection_saturating_add(
            result.scheduler_bytes,
            estimate_chart_scheduler_operation_peak_bytes(plan),
            result.safely_bounded);
      }
    } catch (std::overflow_error const&) {
      result.scheduler_bytes = (std::numeric_limits<std::size_t>::max)();
      result.safely_bounded = false;
    }
  }

  // The scheduler operation estimator includes its per-range exception slots.
  // Charge the caller-side failed summary, one retained exception/error
  // envelope, and test-hook/function wrappers explicitly as well. No failure
  // path may need unbudgeted slot-vector capacity.
  result.error_and_exception_bytes =
      sizeof(chart_scheduler_run_summary) + sizeof(std::exception_ptr) + 512;
  result.error_and_exception_bytes = sampled_tree_projection_saturating_add(
      result.error_and_exception_bytes, 2 * sizeof(std::function<void()>),
      result.safely_bounded);

  result.required_peak_bytes = result.external_resident_bytes;
  result.required_peak_bytes = sampled_tree_projection_saturating_add(
      result.required_peak_bytes, result.sampled_tree_resident_bytes,
      result.safely_bounded);
  result.required_peak_bytes = sampled_tree_projection_saturating_add(
      result.required_peak_bytes, result.prepared_owned_and_job_bytes,
      result.safely_bounded);
  result.required_peak_bytes = sampled_tree_projection_saturating_add(
      result.required_peak_bytes, result.serial_enumeration_scratch_bytes,
      result.safely_bounded);
  result.required_peak_bytes = sampled_tree_projection_saturating_add(
      result.required_peak_bytes, result.wave_slot_and_payload_bytes,
      result.safely_bounded);
  result.required_peak_bytes = sampled_tree_projection_saturating_add(
      result.required_peak_bytes, result.active_projection_scratch_bytes,
      result.safely_bounded);
  result.required_peak_bytes = sampled_tree_projection_saturating_add(
      result.required_peak_bytes, result.scheduler_bytes,
      result.safely_bounded);
  result.required_peak_bytes = sampled_tree_projection_saturating_add(
      result.required_peak_bytes, result.error_and_exception_bytes,
      result.safely_bounded);
  return result;
}

inline sampled_tree_projection_memory_estimate
admit_sampled_tree_projection_memory(
    sampled_tree_projection_context const& prepared,
    chart_scheduler const* scheduler, std::size_t job_count,
    std::size_t external_resident_bytes, std::size_t budget_bytes) {
  auto const maximum_wave =
      bounded_sampled_tree_projection_wave_size(scheduler, job_count);
  auto estimate = [&](std::size_t wave_size) {
    return estimate_sampled_tree_projection_memory(
        prepared, scheduler, job_count, wave_size, external_resident_bytes);
  };
  auto fits = [&](sampled_tree_projection_memory_estimate const& candidate) {
    return candidate.safely_bounded &&
           (budget_bytes == 0 || candidate.required_peak_bytes <= budget_bytes);
  };

  auto maximum = estimate(maximum_wave);
  if (budget_bytes == 0 || fits(maximum)) return maximum;
  if (maximum_wave == 0) {
    throw sampled_tree_projection_budget_error{maximum.required_peak_bytes,
                                               budget_bytes};
  }

  // Wave working storage is monotone in the admitted ordinal count. Preserve
  // as much concurrency as the finite budget permits before rejecting the
  // irreducible prepared/jobs + one active projection working set.
  auto minimum = estimate(1);
  if (!fits(minimum)) {
    throw sampled_tree_projection_budget_error{minimum.required_peak_bytes,
                                               budget_bytes};
  }
  std::size_t low = 1;
  std::size_t high = maximum_wave;
  auto selected = minimum;
  while (low < high) {
    auto const midpoint = low + (high - low + 1) / 2;
    auto candidate = estimate(midpoint);
    if (fits(candidate)) {
      low = midpoint;
      selected = candidate;
    } else {
      high = midpoint - 1;
    }
  }
  if (selected.wave_size != low) selected = estimate(low);
  return selected;
}

inline void add_candidate_production_by_taxa(
    clade_grammar const& grammar, grammar_spr_candidate& candidate,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::map<std::vector<taxon_id>, clade_id>& temp_lookup,
    std::vector<taxon_id> parent_taxa,
    std::vector<std::vector<taxon_id>> child_taxa) {
  auto parent_ref = add_or_get_candidate_clade(grammar, candidate, base_lookup,
                                               temp_lookup,
                                               std::move(parent_taxa));
  std::vector<overlay_clade_ref> child_refs;
  child_refs.reserve(child_taxa.size());
  for (auto& child : child_taxa) {
    child_refs.push_back(add_or_get_candidate_clade(
        grammar, candidate, base_lookup, temp_lookup, std::move(child)));
  }
  maybe_add_candidate_production(grammar, candidate, parent_ref,
                                 std::move(child_refs));
}

inline std::optional<grammar_spr_candidate> make_candidate_from_tree_diff(
    clade_grammar const& base, clade_grammar const& before_tree,
    clade_grammar const& after_tree, grammar_spr_candidate candidate) {
  auto base_lookup = build_clade_lookup(base);
  std::map<std::vector<taxon_id>, clade_id> temp_lookup;

  std::set<production_taxa_key> before_keys;
  std::vector<production_taxa_key> ordered_before_keys;
  ordered_before_keys.reserve(before_tree.productions.size());
  for (auto const& prod : before_tree.productions) {
    auto key = production_key_from_tree_in_base_taxa(base, before_tree, prod);
    before_keys.insert(key);
    ordered_before_keys.push_back(std::move(key));
  }

  std::set<production_taxa_key> after_keys;
  std::vector<production_taxa_key> ordered_after_keys;
  ordered_after_keys.reserve(after_tree.productions.size());
  for (auto const& prod : after_tree.productions) {
    auto key = production_key_from_tree_in_base_taxa(base, after_tree, prod);
    after_keys.insert(key);
    ordered_after_keys.push_back(std::move(key));
  }

  for (auto const& prod : before_tree.productions) {
    auto key = production_key_from_tree_in_base_taxa(base, before_tree, prod);
    if (after_keys.contains(key)) continue;
    auto base_pid = find_base_production_by_key(base, base_lookup, key);
    if (!base_pid) return std::nullopt;
    candidate.removed_productions.push_back(base_production_ref(*base_pid));
  }
  std::sort(candidate.removed_productions.begin(),
            candidate.removed_productions.end());
  candidate.removed_productions.erase(
      std::unique(candidate.removed_productions.begin(),
                  candidate.removed_productions.end()),
      candidate.removed_productions.end());

  for (auto const& key : ordered_after_keys) {
    auto base_pid = find_base_production_by_key(base, base_lookup, key);
    if (before_keys.contains(key) ||
        (base_pid && !candidate_removes_base_production(candidate, *base_pid))) {
      continue;
    }
    add_candidate_production_by_taxa(base, candidate, base_lookup, temp_lookup,
                                     key.parent, key.children);
  }

  if (candidate.removed_productions.empty() &&
      candidate.added_productions.empty()) {
    return std::nullopt;
  }

  auto source_before = source_before_topology_refs_from_tree_keys(
      base, base_lookup, ordered_before_keys);
  auto source_after = source_after_topology_refs_from_tree_keys(
      base, candidate, base_lookup, ordered_after_keys);
  if (source_before && source_after) {
    candidate.source_before_topology_productions = std::move(*source_before);
    candidate.source_after_topology_productions = std::move(*source_after);
  }
  return candidate;
}

// Prepared equivalent of make_candidate_from_tree_diff().  The before-side
// production keys, base lookup, node mapping, and topology certificate were
// computed once at the sampled-tree publication boundary.
inline std::optional<grammar_spr_candidate>
make_candidate_from_tree_diff_prepared(
    sampled_tree_projection_context const& prepared,
    clade_grammar const& after_tree, grammar_spr_candidate candidate) {
  auto const& base = prepared.base();
  auto const& base_lookup = prepared.base_lookup();
  std::map<std::vector<taxon_id>, clade_id> temp_lookup;

  std::set<production_taxa_key> after_keys;
  std::vector<production_taxa_key> ordered_after_keys;
  ordered_after_keys.reserve(after_tree.productions.size());
  for (auto const& prod : after_tree.productions) {
    auto key = production_key_from_tree_in_base_taxa(base, after_tree, prod);
    after_keys.insert(key);
    ordered_after_keys.push_back(std::move(key));
  }

  for (auto const& key : prepared.ordered_before_keys()) {
    if (after_keys.contains(key)) continue;
    auto base_pid = find_base_production_by_key(base, base_lookup, key);
    if (!base_pid) return std::nullopt;
    candidate.removed_productions.push_back(base_production_ref(*base_pid));
  }
  std::sort(candidate.removed_productions.begin(),
            candidate.removed_productions.end());
  candidate.removed_productions.erase(
      std::unique(candidate.removed_productions.begin(),
                  candidate.removed_productions.end()),
      candidate.removed_productions.end());

  for (auto const& key : ordered_after_keys) {
    auto base_pid = find_base_production_by_key(base, base_lookup, key);
    if (prepared.before_keys().contains(key) ||
        (base_pid &&
         !candidate_removes_base_production(candidate, *base_pid))) {
      continue;
    }
    add_candidate_production_by_taxa(base, candidate, base_lookup, temp_lookup,
                                     key.parent, key.children);
  }

  if (candidate.removed_productions.empty() &&
      candidate.added_productions.empty()) {
    return std::nullopt;
  }

  auto source_after = source_after_topology_refs_from_tree_keys(
      base, candidate, base_lookup, ordered_after_keys);
  if (prepared.source_before_topology_refs() && source_after) {
    candidate.source_before_topology_productions =
        *prepared.source_before_topology_refs();
    candidate.source_after_topology_productions = std::move(*source_after);
  }
  return candidate;
}

struct upward_path_step {
  production_id production = no_production;
  clade_id parent = no_clade;
  clade_id child = no_clade;
  std::vector<clade_id> cochildren;
};

using upward_path = std::vector<upward_path_step>;

inline std::optional<std::vector<clade_id>> cochildren_of(
    clade_grammar const& grammar, production_id pid, clade_id child) {
  if (pid == no_production || pid >= grammar.productions.size())
    return std::nullopt;
  auto const& prod = grammar.productions[pid];
  std::vector<clade_id> cochildren;
  cochildren.reserve(prod.children.size());
  bool found = false;
  for (auto candidate : prod.children) {
    if (candidate == child) {
      found = true;
    } else {
      cochildren.push_back(candidate);
    }
  }
  if (!found || cochildren.empty()) return std::nullopt;
  return cochildren;
}

inline std::vector<upward_path> enumerate_upward_paths_to_root(
    clade_grammar const& grammar, clade_id start) {
  std::vector<upward_path> paths;
  upward_path current;
  std::set<clade_id> active;

  auto dfs = [&](auto&& self, clade_id clade) -> void {
    if (clade == grammar.root_clade) {
      paths.push_back(current);
      return;
    }
    if (!active.insert(clade).second) return;
    for (auto pid : grammar.productions_by_child[clade]) {
      auto const& prod = grammar.productions[pid];
      auto cochildren = cochildren_of(grammar, pid, clade);
      if (!cochildren) continue;
      if (grammar.clades[prod.parent].taxa.size() <=
          grammar.clades[clade].taxa.size()) {
        continue;
      }
      current.push_back(
          upward_path_step{pid, prod.parent, clade, std::move(*cochildren)});
      self(self, prod.parent);
      current.pop_back();
    }
    active.erase(clade);
  };

  dfs(dfs, start);
  return paths;
}

inline std::vector<clade_id> clade_ancestry_from_path(clade_id start,
                                                       upward_path const& path) {
  std::vector<clade_id> ancestry;
  ancestry.reserve(path.size() + 1);
  ancestry.push_back(start);
  for (auto const& step : path) ancestry.push_back(step.parent);
  return ancestry;
}

inline std::optional<std::pair<std::size_t, std::size_t>> first_common_ancestor(
    std::vector<clade_id> const& lhs, std::vector<clade_id> const& rhs) {
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    for (std::size_t j = 0; j < rhs.size(); ++j) {
      if (lhs[i] == rhs[j]) return std::pair{i, j};
    }
  }
  return std::nullopt;
}

inline void append_removed_base_production(grammar_spr_candidate& candidate,
                                           production_id pid) {
  if (pid == no_production) return;
  auto ref = base_production_ref(pid);
  if (std::find(candidate.removed_productions.begin(),
                candidate.removed_productions.end(), ref) ==
      candidate.removed_productions.end()) {
    candidate.removed_productions.push_back(ref);
  }
}

inline std::vector<overlay_clade_ref> base_child_refs(
    std::vector<clade_id> const& children) {
  std::vector<overlay_clade_ref> refs;
  refs.reserve(children.size());
  for (auto child : children) refs.push_back(base_clade_ref(child));
  return refs;
}

inline std::vector<taxon_id> candidate_ref_taxa_copy(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate,
    overlay_clade_ref ref) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= grammar.clades.size()) {
      throw std::runtime_error("chart SPR: base clade ref out of range");
    }
    return grammar.clades[ref.id].taxa;
  }
  if (ref.id == no_clade || ref.id >= candidate.added_clades.size()) {
    throw std::runtime_error("chart SPR: temp clade ref out of range");
  }
  return candidate.added_clades[ref.id].taxa;
}

struct candidate_child_group {
  overlay_clade_ref ref;
  std::vector<taxon_id> taxa;
};

inline std::optional<candidate_child_group> ensure_candidate_group_for_refs(
    clade_grammar const& grammar, grammar_spr_candidate& candidate,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::map<std::vector<taxon_id>, clade_id>& temp_lookup,
    std::vector<overlay_clade_ref> children,
    std::optional<production_id> ignore_base_pid = {},
    bool add_production = true) {
  if (children.empty()) return std::nullopt;

  std::vector<taxon_id> covered;
  for (auto child : children) {
    auto child_taxa = candidate_ref_taxa_copy(grammar, candidate, child);
    if (!disjoint_taxa(covered, child_taxa)) return std::nullopt;
    covered = set_union_taxa(std::move(covered), child_taxa);
  }

  if (children.size() == 1) {
    return candidate_child_group{children.front(), std::move(covered)};
  }

  auto parent_ref = add_or_get_candidate_clade(
      grammar, candidate, base_lookup, temp_lookup, covered);
  if (add_production) {
    maybe_add_candidate_production(grammar, candidate, parent_ref,
                                   std::move(children), ignore_base_pid);
  }
  return candidate_child_group{
      parent_ref, candidate_ref_taxa_copy(grammar, candidate, parent_ref)};
}

inline std::optional<candidate_child_group> ensure_candidate_group_for_base_children(
    clade_grammar const& grammar, grammar_spr_candidate& candidate,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    std::map<std::vector<taxon_id>, clade_id>& temp_lookup,
    std::vector<clade_id> const& children,
    std::optional<production_id> ignore_base_pid = {},
    bool add_production = true) {
  return ensure_candidate_group_for_refs(
      grammar, candidate, base_lookup, temp_lookup, base_child_refs(children),
      ignore_base_pid, add_production);
}

inline bool append_lca_child_if_disjoint(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate,
    std::vector<overlay_clade_ref>& children,
    std::vector<std::vector<taxon_id>>& child_taxa, overlay_clade_ref ref) {
  auto taxa = candidate_ref_taxa_copy(grammar, candidate, ref);
  for (auto const& existing : child_taxa) {
    if (!disjoint_taxa(existing, taxa)) return false;
  }
  children.push_back(ref);
  child_taxa.push_back(std::move(taxa));
  return true;
}

inline void append_lca_base_cochildren_if_disjoint(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate,
    std::vector<overlay_clade_ref>& children,
    std::vector<std::vector<taxon_id>>& child_taxa,
    std::vector<clade_id> const& cochildren) {
  for (auto cochild : cochildren) {
    (void)append_lca_child_if_disjoint(
        grammar, candidate, children, child_taxa, base_clade_ref(cochild));
  }
}

inline std::optional<grammar_spr_candidate> make_general_spr_candidate(
    clade_grammar const& grammar,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    production_id source_pid, clade_id moved, clade_id target,
    upward_path const& source_path, upward_path const& dest_path) {
  if (source_pid == no_production || source_pid >= grammar.productions.size())
    return std::nullopt;
  auto const& source_prod = grammar.productions[source_pid];
  auto old_parent = source_prod.parent;
  auto source_cochildren = cochildren_of(grammar, source_pid, moved);
  if (!source_cochildren) return std::nullopt;
  if (target == moved || target == old_parent) return std::nullopt;
  if (source_cochildren->size() == 1 && target == source_cochildren->front())
    return std::nullopt;

  auto const& moved_taxa = grammar.clades[moved].taxa;
  auto const& target_taxa = grammar.clades[target].taxa;
  if (!disjoint_taxa(moved_taxa, target_taxa)) return std::nullopt;

  auto source_ancestry = clade_ancestry_from_path(old_parent, source_path);
  auto dest_ancestry = clade_ancestry_from_path(target, dest_path);
  auto common = first_common_ancestor(source_ancestry, dest_ancestry);
  if (!common) return std::nullopt;
  auto [source_lca_index, dest_lca_index] = *common;
  auto lca = source_ancestry[source_lca_index];

  grammar_spr_candidate candidate;
  candidate.moved_clade = base_clade_ref(moved);
  candidate.old_parent = base_clade_ref(old_parent);
  candidate.new_sibling_or_target = base_clade_ref(target);
  append_removed_base_production(candidate, source_pid);

  std::map<std::vector<taxon_id>, clade_id> temp_lookup;
  auto source_remaining = ensure_candidate_group_for_base_children(
      grammar, candidate, base_lookup, temp_lookup, *source_cochildren, {},
      source_lca_index != 0);
  if (!source_remaining) return std::nullopt;
  candidate.old_sibling = source_remaining->ref;
  auto source_current_ref = source_remaining->ref;

  // Transform the source branch up to, but not including, the LCA.  The final
  // LCA production is rebuilt after the destination branch has been expanded.
  for (std::size_t i = 0; i < source_lca_index; ++i) {
    append_removed_base_production(candidate, source_path[i].production);
    if (i + 1 == source_lca_index) break;

    auto parent_taxa = set_difference_taxa(
        grammar.clades[source_path[i].parent].taxa, moved_taxa);
    std::vector<overlay_clade_ref> children;
    children.reserve(source_path[i].cochildren.size() + 1);
    children.push_back(source_current_ref);
    auto cochild_refs = base_child_refs(source_path[i].cochildren);
    children.insert(children.end(), cochild_refs.begin(), cochild_refs.end());
    auto parent_group = ensure_candidate_group_for_refs(
        grammar, candidate, base_lookup, temp_lookup, std::move(children));
    if (!parent_group || parent_group->taxa != parent_taxa)
      return std::nullopt;
    source_current_ref = parent_group->ref;
  }

  auto dest_current = ensure_candidate_group_for_refs(
      grammar, candidate, base_lookup, temp_lookup,
      {base_clade_ref(moved), base_clade_ref(target)});
  if (!dest_current) return std::nullopt;
  auto dest_current_taxa = dest_current->taxa;
  auto dest_current_ref = dest_current->ref;

  bool destination_already_rebuilt_lca = (dest_current_taxa == grammar.clades[lca].taxa);
  for (std::size_t i = 0; i < dest_lca_index && !destination_already_rebuilt_lca;
       ++i) {
    append_removed_base_production(candidate, dest_path[i].production);
    if (dest_path[i].parent == lca) break;

    auto parent_taxa = set_union_taxa(grammar.clades[dest_path[i].parent].taxa,
                                      moved_taxa);
    std::vector<overlay_clade_ref> children;
    children.reserve(dest_path[i].cochildren.size() + 1);
    children.push_back(dest_current_ref);
    auto cochild_refs = base_child_refs(dest_path[i].cochildren);
    children.insert(children.end(), cochild_refs.begin(), cochild_refs.end());
    auto parent_group = ensure_candidate_group_for_refs(
        grammar, candidate, base_lookup, temp_lookup, std::move(children));
    if (!parent_group || parent_group->taxa != parent_taxa)
      return std::nullopt;
    dest_current_taxa = std::move(parent_group->taxa);
    dest_current_ref = parent_group->ref;
    destination_already_rebuilt_lca =
        (dest_current_taxa == grammar.clades[lca].taxa);
  }

  if (!destination_already_rebuilt_lca) {
    std::vector<overlay_clade_ref> lca_children;
    std::vector<std::vector<taxon_id>> lca_child_taxa;
    lca_children.reserve(source_prod.children.size() + dest_path.size() + 2);
    lca_child_taxa.reserve(lca_children.capacity());

    if (source_lca_index != 0) {
      if (!append_lca_child_if_disjoint(
              grammar, candidate, lca_children, lca_child_taxa,
              source_current_ref)) {
        return std::nullopt;
      }
    }
    if (!append_lca_child_if_disjoint(
            grammar, candidate, lca_children, lca_child_taxa,
            dest_current_ref)) {
      return std::nullopt;
    }
    if (source_lca_index == 0) {
      append_lca_base_cochildren_if_disjoint(
          grammar, candidate, lca_children, lca_child_taxa,
          *source_cochildren);
    } else {
      append_lca_base_cochildren_if_disjoint(
          grammar, candidate, lca_children, lca_child_taxa,
          source_path[source_lca_index - 1].cochildren);
    }
    if (dest_lca_index > 0) {
      append_lca_base_cochildren_if_disjoint(
          grammar, candidate, lca_children, lca_child_taxa,
          dest_path[dest_lca_index - 1].cochildren);
    }

    auto lca_group = ensure_candidate_group_for_refs(
        grammar, candidate, base_lookup, temp_lookup, std::move(lca_children));
    if (!lca_group || lca_group->ref != base_clade_ref(lca) ||
        lca_group->taxa != grammar.clades[lca].taxa) {
      return std::nullopt;
    }
  }

  std::sort(candidate.removed_productions.begin(),
            candidate.removed_productions.end());
  candidate.removed_productions.erase(
      std::unique(candidate.removed_productions.begin(),
                  candidate.removed_productions.end()),
      candidate.removed_productions.end());

  if (candidate.removed_productions.empty() &&
      candidate.added_productions.empty()) {
    return std::nullopt;
  }
  return candidate;
}

inline void append_deduplicated_candidate(
    std::vector<grammar_spr_candidate>& candidates,
    std::set<std::string>& seen_signatures, grammar_spr_candidate candidate,
    std::size_t max_candidates) {
  auto signature = candidate_signature(candidate);
  if (!seen_signatures.insert(signature).second) return;
  if (max_candidates != 0 && candidates.size() >= max_candidates) return;
  candidates.push_back(std::move(candidate));
}

inline void append_deduplicated_candidate_by_taxa(
    clade_grammar const& grammar, std::vector<grammar_spr_candidate>& candidates,
    std::set<std::string>& seen_signatures, grammar_spr_candidate candidate,
    std::size_t max_candidates) {
  auto signature = chart_spr_candidate_taxon_signature(grammar, candidate);
  if (!seen_signatures.insert(signature).second) return;
  if (max_candidates != 0 && candidates.size() >= max_candidates) return;
  candidates.push_back(std::move(candidate));
}

inline bool clade_size_allowed(std::size_t size, std::size_t min_size,
                               std::size_t max_size) {
  if (size < min_size) return false;
  if (max_size != 0 && size > max_size) return false;
  return true;
}

inline void note_pruned_before(
    chart_spr_candidate_generation_stats& stats,
    std::size_t chart_spr_candidate_generation_stats::*reason = nullptr) {
  ++stats.candidates_pruned_before_construction;
  if (reason != nullptr) ++(stats.*reason);
}

inline void note_pruned_after(
    chart_spr_candidate_generation_stats& stats,
    std::size_t chart_spr_candidate_generation_stats::*reason = nullptr) {
  ++stats.candidates_pruned_after_construction;
  if (reason != nullptr) ++(stats.*reason);
}

template <typename T>
inline void shuffle_if_requested(std::vector<T>& values,
                                 grammar_spr_enumeration_options const& options,
                                 std::mt19937* rng) {
  if (options.randomize_order && rng != nullptr && values.size() > 1) {
    std::shuffle(values.begin(), values.end(), *rng);
  }
}

inline std::size_t estimate_candidate_affected_clades(
    grammar_spr_candidate const& candidate) {
  return candidate.added_clades.size() + candidate.added_productions.size() +
         candidate.removed_productions.size();
}

inline bool grammar_spr_candidate_involves_multifurcation(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate) {
  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base) continue;
    if (ref.id == no_production || ref.id >= grammar.productions.size()) {
      continue;
    }
    if (grammar.productions[ref.id].children.size() != 2) return true;
  }
  return std::any_of(candidate.added_productions.begin(),
                     candidate.added_productions.end(), [](auto const& prod) {
                       return prod.children.size() != 2;
                     });
}

inline std::size_t estimate_candidate_affected_clades_before_construction(
    upward_path const& source_path, upward_path const& dest_path) {
  // Cheap path-length proxy used only as an early filter before
  // grammar_spr_candidate construction.  The final affected-clade count is
  // candidate/overlay dependent and is reported by the scorer.
  return 1 + source_path.size() + dest_path.size();
}

template <typename F>
bool invoke_candidate_callback(F& callback,
                               grammar_spr_candidate const& candidate) {
  if constexpr (std::is_void_v<decltype(callback(candidate))>) {
    callback(candidate);
    return true;
  } else {
    return static_cast<bool>(callback(candidate));
  }
}

struct lazy_upward_path_control {
  bool budget_exhausted = false;
  bool callback_stop = false;
};

template <typename F>
void for_each_upward_path_to_root_lazy(
    clade_grammar const& grammar, clade_id start,
    grammar_spr_enumeration_options const& options,
    chart_spr_candidate_generation_stats& stats, F&& callback,
    lazy_upward_path_control& control, std::mt19937* rng = nullptr) {
  upward_path current;
  std::set<clade_id> active;

  auto dfs = [&](auto&& self, clade_id clade) -> bool {
    if (control.budget_exhausted || control.callback_stop) return false;
    if (clade == grammar.root_clade) {
      ++stats.upward_paths_completed;
      if (!callback(current)) {
        control.callback_stop = true;
        return false;
      }
      return true;
    }
    if (!active.insert(clade).second) return true;

    auto parent_productions = grammar.productions_by_child[clade];
    shuffle_if_requested(parent_productions, options, rng);
    for (auto pid : parent_productions) {
      auto const& prod = grammar.productions[pid];
      auto cochildren = cochildren_of(grammar, pid, clade);
      if (!cochildren) continue;
      if (grammar.clades[prod.parent].taxa.size() <=
          grammar.clades[clade].taxa.size()) {
        continue;
      }
      if (options.max_upward_path_expansions != 0 &&
          stats.upward_path_iterator_steps >=
              options.max_upward_path_expansions) {
        control.budget_exhausted = true;
        active.erase(clade);
        return false;
      }
      ++stats.upward_path_iterator_steps;
      current.push_back(
          upward_path_step{pid, prod.parent, clade, std::move(*cochildren)});
      if (!self(self, prod.parent)) {
        current.pop_back();
        active.erase(clade);
        return false;
      }
      current.pop_back();
    }

    active.erase(clade);
    return true;
  };

  (void)dfs(dfs, start);
}

}  // namespace chart_spr_detail

inline std::vector<taxon_id> const& chart_spr_clade_taxa_for_ref(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    overlay_clade_ref ref) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_clade || ref.id >= base.clades.size()) {
      throw std::runtime_error(
          "chart SPR candidate signature: base clade ref out of range");
    }
    return base.clades[ref.id].taxa;
  }
  if (ref.id == no_clade || ref.id >= candidate.added_clades.size()) {
    throw std::runtime_error(
        "chart SPR candidate signature: temp clade ref out of range");
  }
  return candidate.added_clades[ref.id].taxa;
}

inline std::vector<taxon_id> chart_spr_normalize_taxa(
    std::vector<taxon_id> taxa) {
  std::sort(taxa.begin(), taxa.end());
  taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
  return taxa;
}

inline std::vector<taxon_id> chart_spr_union_taxa(
    std::vector<taxon_id> lhs, std::vector<taxon_id> const& rhs) {
  lhs.insert(lhs.end(), rhs.begin(), rhs.end());
  return chart_spr_normalize_taxa(std::move(lhs));
}

inline void chart_spr_append_taxa_key(std::ostringstream& out,
                                      std::vector<taxon_id> taxa) {
  taxa = chart_spr_normalize_taxa(std::move(taxa));
  out << "{";
  for (auto taxon : taxa) out << taxon << ",";
  out << "}";
}

inline std::string chart_spr_escape_sample_id(std::string const& sample_id) {
  std::string escaped;
  escaped.reserve(sample_id.size());
  for (char c : sample_id) {
    if (c == '\\' || c == '{' || c == '}' || c == ',' || c == ';' ||
        c == '=' || c == '-' || c == '>') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

inline void chart_spr_append_sample_taxa_key(std::ostringstream& out,
                                             clade_grammar const& base,
                                             std::vector<taxon_id> taxa) {
  taxa = chart_spr_normalize_taxa(std::move(taxa));
  out << "{";
  for (auto taxon : taxa) {
    if (taxon >= base.taxa.id_to_sample_id.size()) {
      throw std::runtime_error(
          "chart SPR sample signature: taxon id out of range");
    }
    out << chart_spr_escape_sample_id(base.taxa.id_to_sample_id[taxon])
        << ",";
  }
  out << "}";
}

inline void chart_spr_append_signature_taxa_key(
    std::ostringstream& out, clade_grammar const& base,
    std::vector<taxon_id> taxa, bool use_sample_ids) {
  if (use_sample_ids) {
    chart_spr_append_sample_taxa_key(out, base, std::move(taxa));
  } else {
    chart_spr_append_taxa_key(out, std::move(taxa));
  }
}

inline void chart_spr_append_optional_ref_taxa_key(
    std::ostringstream& out, clade_grammar const& base,
    grammar_spr_candidate const& candidate, overlay_clade_ref ref,
    bool use_sample_ids = false) {
  if (ref.id == no_clade) {
    chart_spr_append_signature_taxa_key(
        out, base, std::vector<taxon_id>{}, use_sample_ids);
    return;
  }
  chart_spr_append_signature_taxa_key(
      out, base, chart_spr_clade_taxa_for_ref(base, candidate, ref),
      use_sample_ids);
}

inline void chart_spr_append_production_taxa_signature(
    std::ostringstream& out, clade_grammar const& base,
    std::vector<taxon_id> parent_taxa,
    std::vector<std::vector<taxon_id>> child_taxa,
    bool use_sample_ids = false) {
  parent_taxa = chart_spr_normalize_taxa(std::move(parent_taxa));
  for (auto& child : child_taxa) {
    child = chart_spr_normalize_taxa(std::move(child));
  }
  std::sort(child_taxa.begin(), child_taxa.end());

  chart_spr_append_signature_taxa_key(out, base, parent_taxa,
                                      use_sample_ids);
  out << "->";
  for (auto const& child : child_taxa) {
    chart_spr_append_signature_taxa_key(out, base, child, use_sample_ids);
  }
}

inline std::vector<std::string> chart_spr_candidate_removed_production_signatures(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    bool use_sample_ids = false) {
  std::vector<std::string> removed;
  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(
          "chart SPR candidate signature: removed production is not base");
    }
    if (ref.id == no_production || ref.id >= base.productions.size()) {
      throw std::runtime_error(
          "chart SPR candidate signature: removed production out of range");
    }
    auto const& prod = base.productions[ref.id];
    std::vector<std::vector<taxon_id>> child_taxa;
    child_taxa.reserve(prod.children.size());
    for (auto child : prod.children) child_taxa.push_back(base.clades[child].taxa);
    std::ostringstream sig;
    chart_spr_append_production_taxa_signature(
        sig, base, base.clades[prod.parent].taxa, std::move(child_taxa),
        use_sample_ids);
    removed.push_back(sig.str());
  }
  std::sort(removed.begin(), removed.end());
  return removed;
}

inline std::vector<std::string> chart_spr_candidate_added_production_signatures(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    bool use_sample_ids = false) {
  std::vector<std::string> added;
  added.reserve(candidate.added_productions.size());
  for (auto const& prod : candidate.added_productions) {
    std::vector<std::vector<taxon_id>> child_taxa;
    child_taxa.reserve(prod.children.size());
    for (auto child : prod.children) {
      child_taxa.push_back(chart_spr_clade_taxa_for_ref(base, candidate, child));
    }
    std::ostringstream sig;
    chart_spr_append_production_taxa_signature(
        sig, base, chart_spr_clade_taxa_for_ref(base, candidate, prod.parent),
        std::move(child_taxa), use_sample_ids);
    added.push_back(sig.str());
  }
  std::sort(added.begin(), added.end());
  return added;
}

inline void chart_spr_append_signature_list(
    std::ostringstream& out, std::vector<std::string> const& signatures) {
  for (auto const& signature : signatures) out << signature << ";";
}

inline std::string chart_spr_candidate_signature_impl(
    clade_grammar const& base, grammar_spr_candidate const& candidate,
    bool use_sample_ids) {
  std::ostringstream out;
  out << "m=";
  chart_spr_append_optional_ref_taxa_key(out, base, candidate,
                                         candidate.moved_clade,
                                         use_sample_ids);
  out << ";op=";
  chart_spr_append_optional_ref_taxa_key(out, base, candidate,
                                         candidate.old_parent,
                                         use_sample_ids);
  out << ";os=";
  chart_spr_append_optional_ref_taxa_key(out, base, candidate,
                                         candidate.old_sibling,
                                         use_sample_ids);
  out << ";nt=";
  chart_spr_append_optional_ref_taxa_key(
      out, base, candidate, candidate.new_sibling_or_target, use_sample_ids);

  std::vector<std::vector<taxon_id>> added_clades;
  added_clades.reserve(candidate.added_clades.size());
  for (auto const& key : candidate.added_clades) added_clades.push_back(key.taxa);
  for (auto& taxa : added_clades) taxa = chart_spr_normalize_taxa(std::move(taxa));
  std::sort(added_clades.begin(), added_clades.end());
  out << ";clades=";
  for (auto const& taxa : added_clades) {
    chart_spr_append_signature_taxa_key(out, base, taxa, use_sample_ids);
  }

  out << ";rm=";
  chart_spr_append_signature_list(
      out, chart_spr_candidate_removed_production_signatures(
               base, candidate, use_sample_ids));
  out << ";add=";
  chart_spr_append_signature_list(
      out, chart_spr_candidate_added_production_signatures(
               base, candidate, use_sample_ids));
  return out.str();
}

// Stable candidate identities for deduplication and reports.  Dense clade and
// production IDs are valid only inside one grammar build, so public diagnostics
// use normalized moved/parent/sibling/target taxa plus removed/added production
// taxon keys.  Use the sample signature for cross-run/JSON reports when sample
// IDs are more stable than in-process taxon IDs.
inline std::string chart_spr_candidate_taxon_signature(
    clade_grammar const& base, grammar_spr_candidate const& candidate) {
  return chart_spr_candidate_signature_impl(base, candidate,
                                            false /* use_sample_ids */);
}

inline std::string chart_spr_candidate_sample_signature(
    clade_grammar const& base, grammar_spr_candidate const& candidate) {
  return chart_spr_candidate_signature_impl(base, candidate,
                                            true /* use_sample_ids */);
}

inline void chart_spr_append_reversal_key_piece(
    std::ostringstream& out, char const* label,
    std::vector<taxon_id> taxa) {
  out << label << "=";
  chart_spr_append_taxa_key(out, std::move(taxa));
}

inline std::string chart_spr_candidate_reversal_key(
    clade_grammar const& base, grammar_spr_candidate const& candidate) {
  std::ostringstream out;
  chart_spr_append_reversal_key_piece(
      out, "m", chart_spr_clade_taxa_for_ref(base, candidate,
                                               candidate.moved_clade));
  out << ";";
  chart_spr_append_reversal_key_piece(
      out, "op", chart_spr_clade_taxa_for_ref(base, candidate,
                                                candidate.old_parent));
  out << ";";
  chart_spr_append_reversal_key_piece(
      out, "os", chart_spr_clade_taxa_for_ref(base, candidate,
                                                candidate.old_sibling));
  out << ";";
  chart_spr_append_reversal_key_piece(
      out, "nt", chart_spr_clade_taxa_for_ref(
                       base, candidate, candidate.new_sibling_or_target));
  out << ";rm=";
  chart_spr_append_signature_list(
      out, chart_spr_candidate_removed_production_signatures(base, candidate));
  out << ";add=";
  chart_spr_append_signature_list(
      out, chart_spr_candidate_added_production_signatures(base, candidate));
  return out.str();
}

inline std::string chart_spr_candidate_immediate_reverse_key(
    clade_grammar const& base, grammar_spr_candidate const& candidate) {
  auto moved_taxa = chart_spr_clade_taxa_for_ref(base, candidate,
                                                 candidate.moved_clade);
  auto old_sibling_taxa = chart_spr_clade_taxa_for_ref(base, candidate,
                                                       candidate.old_sibling);
  auto new_target_taxa = chart_spr_clade_taxa_for_ref(
      base, candidate, candidate.new_sibling_or_target);
  auto reverse_old_parent_taxa = chart_spr_union_taxa(moved_taxa,
                                                      new_target_taxa);

  std::ostringstream out;
  chart_spr_append_reversal_key_piece(out, "m", moved_taxa);
  out << ";";
  chart_spr_append_reversal_key_piece(out, "op", reverse_old_parent_taxa);
  out << ";";
  chart_spr_append_reversal_key_piece(out, "os", new_target_taxa);
  out << ";";
  chart_spr_append_reversal_key_piece(out, "nt", old_sibling_taxa);
  out << ";rm=";
  chart_spr_append_signature_list(
      out, chart_spr_candidate_added_production_signatures(base, candidate));
  out << ";add=";
  chart_spr_append_signature_list(
      out, chart_spr_candidate_removed_production_signatures(base, candidate));
  return out.str();
}

template <typename F>
chart_spr_candidate_generation_stats for_each_sampled_tree_spr_candidate(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback);

template <typename F>
chart_spr_candidate_generation_stats for_each_hybrid_spr_candidate(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback);

namespace chart_spr_detail {

template <typename F>
chart_spr_candidate_generation_stats
for_each_sampled_tree_spr_candidate_stream(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback);

template <typename F>
chart_spr_candidate_generation_stats for_each_hybrid_spr_candidate_stream(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback);

template <typename F>
chart_spr_candidate_generation_stats for_each_grammar_spr_candidate_stream(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  chart_spr_candidate_generation_stats stats;
  auto base_lookup = build_clade_lookup(grammar);
  std::set<std::string> seen;

  auto request_stop = [&](chart_spr_candidate_stop_reason reason) -> bool {
    if (stats.stop_reason == chart_spr_candidate_stop_reason::exhausted) {
      stats.stop_reason = reason;
    }
    return false;
  };
  auto stopped = [&]() {
    return stats.stop_reason != chart_spr_candidate_stop_reason::exhausted;
  };
  auto pre_dedup_cap_reached = [&]() {
    return options.max_candidates != 0 &&
           !options.max_candidates_is_post_dedup &&
           stats.candidates_constructed >= options.max_candidates;
  };
  auto post_dedup_cap_reached = [&]() {
    return options.max_candidates != 0 &&
           options.max_candidates_is_post_dedup &&
           stats.candidates_generated_after_dedup >= options.max_candidates;
  };

  std::mt19937 rng(options.seed);
  std::vector<production_id> source_order;
  source_order.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    source_order.push_back(static_cast<production_id>(pid));
  }
  shuffle_if_requested(source_order, options, &rng);

  std::vector<clade_id> target_order;
  target_order.reserve(grammar.clades.size());
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    target_order.push_back(static_cast<clade_id>(cid));
  }
  shuffle_if_requested(target_order, options, &rng);

  for (auto source_pid : source_order) {
    if (stopped()) break;
    if (pre_dedup_cap_reached()) {
      request_stop(chart_spr_candidate_stop_reason::candidate_cap);
      break;
    }
    auto const& source_prod = grammar.productions[source_pid];
    if (source_prod.children.size() < 2) continue;
    if (!options.include_root_moves &&
        source_prod.parent == grammar.root_clade) {
      note_pruned_before(stats,
                         &chart_spr_candidate_generation_stats::
                             candidates_pruned_root_or_trivial);
      continue;
    }

    std::vector<std::size_t> moved_order(source_prod.children.size());
    std::iota(moved_order.begin(), moved_order.end(), std::size_t{0});
    if (options.randomize_order) {
      std::shuffle(moved_order.begin(), moved_order.end(), rng);
    }
    for (auto moved_i : moved_order) {
      if (stopped()) break;
      auto moved = source_prod.children[moved_i];
      auto source_cochildren = cochildren_of(grammar, source_pid, moved);
      if (!source_cochildren) continue;
      auto const& moved_taxa = grammar.clades[moved].taxa;
      if (!clade_size_allowed(moved_taxa.size(),
                              options.min_moved_clade_size,
                              options.max_moved_clade_size)) {
        note_pruned_before(stats,
                           &chart_spr_candidate_generation_stats::
                               candidates_pruned_moved_size);
        continue;
      }

      for (auto target : target_order) {
        if (stopped()) break;
        if (pre_dedup_cap_reached()) {
          request_stop(chart_spr_candidate_stop_reason::candidate_cap);
          break;
        }
        if (target == moved ||
            (source_cochildren->size() == 1 &&
             target == source_cochildren->front()) ||
            target == source_prod.parent ||
            (!options.include_root_moves && target == grammar.root_clade)) {
          note_pruned_before(stats,
                             &chart_spr_candidate_generation_stats::
                                 candidates_pruned_root_or_trivial);
          continue;
        }
        auto const& target_taxa = grammar.clades[target].taxa;
        if (!clade_size_allowed(target_taxa.size(),
                                options.min_target_clade_size,
                                options.max_target_clade_size)) {
          note_pruned_before(stats,
                             &chart_spr_candidate_generation_stats::
                                 candidates_pruned_target_size);
          continue;
        }
        if (!disjoint_taxa(moved_taxa, target_taxa)) {
          note_pruned_before(stats,
                             &chart_spr_candidate_generation_stats::
                                 candidates_pruned_overlap);
          continue;
        }

        lazy_upward_path_control source_control;
        for_each_upward_path_to_root_lazy(
            grammar, source_prod.parent, options, stats,
            [&](upward_path const& source_path) -> bool {
              if (stopped()) return false;
              if (options.max_estimated_affected_clades != 0 &&
                  1 + source_path.size() >
                      options.max_estimated_affected_clades) {
                note_pruned_before(
                    stats,
                    &chart_spr_candidate_generation_stats::
                        candidates_pruned_affected_estimate);
                return true;
              }
              lazy_upward_path_control dest_control;
              for_each_upward_path_to_root_lazy(
                  grammar, target, options, stats,
                  [&](upward_path const& dest_path) -> bool {
                    if (stopped()) return false;
                    if (options.max_path_pairs_considered != 0 &&
                        stats.path_pairs_considered >=
                            options.max_path_pairs_considered) {
                      return request_stop(
                          chart_spr_candidate_stop_reason::path_budget);
                    }
                    ++stats.path_pairs_considered;
                    auto affected_estimate =
                        estimate_candidate_affected_clades_before_construction(
                            source_path, dest_path);
                    if (options.max_estimated_affected_clades != 0 &&
                        affected_estimate >
                            options.max_estimated_affected_clades) {
                      note_pruned_before(
                          stats,
                          &chart_spr_candidate_generation_stats::
                              candidates_pruned_affected_estimate);
                      return true;
                    }

                    auto candidate = make_general_spr_candidate(
                        grammar, base_lookup, source_pid, moved, target,
                        source_path, dest_path);
                    if (!candidate) {
                      note_pruned_after(
                          stats,
                          &chart_spr_candidate_generation_stats::
                              candidates_pruned_invalid);
                      return true;
                    }
                    ++stats.candidates_constructed;

                    if (options.max_estimated_affected_clades != 0 &&
                        estimate_candidate_affected_clades(*candidate) >
                            options.max_estimated_affected_clades) {
                      note_pruned_after(
                          stats,
                          &chart_spr_candidate_generation_stats::
                              candidates_pruned_affected_estimate);
                      if (pre_dedup_cap_reached()) {
                        return request_stop(
                            chart_spr_candidate_stop_reason::candidate_cap);
                      }
                      return true;
                    }

                    if (!options.include_immediate_reversal_candidates &&
                        !options.immediate_reversal_candidate_key_to_skip
                             .empty() &&
                        chart_spr_candidate_reversal_key(grammar, *candidate) ==
                            options.immediate_reversal_candidate_key_to_skip) {
                      note_pruned_after(
                          stats,
                          &chart_spr_candidate_generation_stats::
                              candidates_pruned_immediate_reversal);
                      if (pre_dedup_cap_reached()) {
                        return request_stop(
                            chart_spr_candidate_stop_reason::candidate_cap);
                      }
                      return true;
                    }

                    auto signature = chart_spr_candidate_taxon_signature(
                        grammar, *candidate);
                    if (!seen.insert(std::move(signature)).second) {
                      note_pruned_after(
                          stats,
                          &chart_spr_candidate_generation_stats::
                              candidates_pruned_duplicate);
                      if (pre_dedup_cap_reached()) {
                        return request_stop(
                            chart_spr_candidate_stop_reason::candidate_cap);
                      }
                      return true;
                    }

                    ++stats.candidates_generated_after_dedup;
                    if (grammar_spr_candidate_involves_multifurcation(
                            grammar, *candidate)) {
                      ++stats.spr_multifurcation_moves_generated;
                    }
                    if (!invoke_candidate_callback(callback, *candidate)) {
                      return request_stop(
                          chart_spr_candidate_stop_reason::callback_stop);
                    }
                    if (pre_dedup_cap_reached() || post_dedup_cap_reached()) {
                      return request_stop(
                          chart_spr_candidate_stop_reason::candidate_cap);
                    }
                    return true;
                  },
                  dest_control, &rng);

              if (dest_control.budget_exhausted) {
                return request_stop(
                    chart_spr_candidate_stop_reason::path_budget);
              }
              return !stopped();
            },
            source_control, &rng);

        if (source_control.budget_exhausted) {
          request_stop(chart_spr_candidate_stop_reason::path_budget);
        }
      }
    }
  }

  return stats;
}

}  // namespace chart_spr_detail

namespace chart_spr_detail {

template <typename F>
chart_spr_candidate_generation_stats for_each_candidate_source_stream(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  if (options.source == chart_spr_candidate_source::sampled_tree) {
    return for_each_sampled_tree_spr_candidate_stream(
        grammar, options, std::forward<F>(callback));
  }
  if (options.source == chart_spr_candidate_source::hybrid) {
    return for_each_hybrid_spr_candidate_stream(
        grammar, options, std::forward<F>(callback));
  }
  return for_each_grammar_spr_candidate_stream(
      grammar, options, std::forward<F>(callback));
}

template <typename F>
chart_spr_candidate_generation_stats for_each_grammar_spr_candidate_checked(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  if (options.source == chart_spr_candidate_source::grammar &&
      options.include_neutral_or_reversal_candidates) {
    throw std::runtime_error(
        "chart SPR candidate enumeration: neutral/reversal candidates are not "
        "implemented in the grammar stream");
  }

  if (options.reservoir_sample && options.max_candidates != 0) {
    auto stream_options = options;
    stream_options.reservoir_sample = false;
    stream_options.max_candidates = 0;
    std::vector<grammar_spr_candidate> reservoir;
    reservoir.reserve(options.max_candidates);
    std::mt19937 reservoir_rng(options.seed ^ 0x9e3779b9U);
    std::size_t seen_candidates = 0;
    auto reservoir_callback = [&](grammar_spr_candidate const& candidate) {
      ++seen_candidates;
      if (reservoir.size() < options.max_candidates) {
        reservoir.push_back(candidate);
      } else {
        std::uniform_int_distribution<std::size_t> dist(
            0, seen_candidates - 1);
        auto slot = dist(reservoir_rng);
        if (slot < options.max_candidates) reservoir[slot] = candidate;
      }
      return true;
    };
    auto stats = for_each_candidate_source_stream(
        grammar, stream_options, reservoir_callback);
    if (options.randomize_order && reservoir.size() > 1) {
      std::shuffle(reservoir.begin(), reservoir.end(), reservoir_rng);
    }
    for (auto const& candidate : reservoir) {
      if (!invoke_candidate_callback(callback, candidate)) {
        if (stats.stop_reason == chart_spr_candidate_stop_reason::exhausted) {
          stats.stop_reason = chart_spr_candidate_stop_reason::callback_stop;
        }
        break;
      }
    }
    return stats;
  }

  return for_each_candidate_source_stream(
      grammar, options, std::forward<F>(callback));
}

}  // namespace chart_spr_detail

// Streaming grammar-native SPR candidate enumeration.  Unlike the legacy eager
// vector helper, this enumerates upward paths lazily for the source/target pair
// currently under consideration and honors candidate/path budgets before
// exploring unrelated clades.
template <typename F>
chart_spr_candidate_generation_stats for_each_grammar_spr_candidate(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  using namespace chart_spr_detail;
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_execution_plan_detail::record_legacy_production_index_validation();
  chart_trim_detail::validate_production_indices(grammar);
  return chart_spr_detail::for_each_grammar_spr_candidate_checked(
      grammar, options, std::forward<F>(callback));
}

// Search-internal overload.  The capability proves that this exact grammar
// and resident plan were fingerprint-checked at the acceptance-iteration
// publication boundary; source dispatch (including reservoir and hybrid
// children) therefore performs no additional grammar-wide validation.
template <typename F>
chart_spr_candidate_generation_stats for_each_grammar_spr_candidate(
    clade_grammar const& grammar,
    checked_chart_execution_plan_ref const& checked,
    grammar_spr_enumeration_options const& options, F&& callback) {
  checked.assert_same(grammar, checked.plan());
  return chart_spr_detail::for_each_grammar_spr_candidate_checked(
      grammar, options, std::forward<F>(callback));
}

namespace chart_spr_detail {

inline overlay_clade_grammar overlay_from_candidate_payload(
    clade_grammar const& base, grammar_spr_candidate const& candidate) {
  overlay_clade_grammar overlay;
  overlay.base = &base;
  overlay.temp_clades = candidate.added_clades;
  overlay.temp_productions = candidate.added_productions;

  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base) {
      throw std::runtime_error(
          "chart SPR: candidates may tombstone only base productions");
    }
    if (ref.id == no_production || ref.id >= base.productions.size()) {
      throw std::runtime_error(
          "chart SPR: candidate removed base production out of range");
    }
    overlay.removed_base_productions.push_back(ref.id);
  }
  std::sort(overlay.removed_base_productions.begin(),
            overlay.removed_base_productions.end());
  overlay.removed_base_productions.erase(
      std::unique(overlay.removed_base_productions.begin(),
                  overlay.removed_base_productions.end()),
      overlay.removed_base_productions.end());
  return overlay;
}

inline overlay_materialization_result materialize_overlay_grammar_core(
    overlay_clade_grammar const& overlay);

}  // namespace chart_spr_detail

inline overlay_clade_grammar overlay_from_candidate(
    clade_grammar const& base, grammar_spr_candidate const& candidate) {
  auto overlay =
      chart_spr_detail::overlay_from_candidate_payload(base, candidate);
  chart_spr_detail::validate_overlay(overlay);
  return overlay;
}

inline overlay_clade_grammar overlay_from_candidate(
    clade_grammar const& base,
    checked_chart_execution_plan_ref const& checked,
    grammar_spr_candidate const& candidate) {
  checked.assert_same(base, checked.plan());
  auto overlay =
      chart_spr_detail::overlay_from_candidate_payload(base, candidate);
  chart_spr_detail::validate_overlay_payload(overlay);
  return overlay;
}

inline overlay_materialization_result
chart_spr_detail::materialize_overlay_grammar_core(
    overlay_clade_grammar const& overlay) {
  overlay_materialization_result result;
  auto const& base = *overlay.base;
  auto& grammar = result.grammar;

  grammar.taxa = base.taxa;

  auto removed = chart_spr_detail::normalized_removed_base_productions(overlay);
  result.base_clade_to_dense.assign(base.clades.size(), no_clade);
  result.temp_clade_to_dense.assign(overlay.temp_clades.size(), no_clade);
  result.base_production_to_dense.assign(base.productions.size(), no_production);
  result.temp_production_to_dense.assign(overlay.temp_productions.size(),
                                         no_production);

  // Only materialize the grammar reachable from the overlay root.  SPR
  // tombstones often remove the only production of clades that were on the old
  // source/destination paths; those clades are dead in the rewritten grammar and
  // must not be presented to build_single_site_chart(), which deliberately
  // rejects reachable non-singletons without productions.
  std::set<overlay_clade_ref> reachable_clades;
  std::set<overlay_production_ref> reachable_productions;
  std::vector<overlay_clade_ref> stack{base_clade_ref(base.root_clade)};
  while (!stack.empty()) {
    auto ref = stack.back();
    stack.pop_back();
    if (!reachable_clades.insert(ref).second) continue;

    if (ref.space == overlay_id_space::base) {
      for (auto pid : base.productions_by_parent[ref.id]) {
        if (std::binary_search(removed.begin(), removed.end(), pid)) continue;
        auto prod_ref = base_production_ref(pid);
        reachable_productions.insert(prod_ref);
        for (auto child : base.productions[pid].children)
          stack.push_back(base_clade_ref(child));
      }
    }

    for (std::size_t i = 0; i < overlay.temp_productions.size(); ++i) {
      auto const& prod = overlay.temp_productions[i];
      if (prod.parent != ref) continue;
      auto prod_ref = temp_production_ref(static_cast<production_id>(i));
      reachable_productions.insert(prod_ref);
      for (auto child : prod.children) stack.push_back(child);
    }
  }

  result.dense_clade_to_ref.reserve(reachable_clades.size());
  for (std::size_t cid = 0; cid < base.clades.size(); ++cid) {
    auto ref = base_clade_ref(static_cast<clade_id>(cid));
    if (!reachable_clades.contains(ref)) continue;
    if (grammar.clades.size() >= static_cast<std::size_t>(no_clade)) {
      throw std::runtime_error("chart SPR: too many dense overlay clades");
    }
    auto dense = static_cast<clade_id>(grammar.clades.size());
    result.base_clade_to_dense[cid] = dense;
    grammar.clades.push_back(base.clades[cid]);
    result.dense_clade_to_ref.push_back(ref);
  }
  for (std::size_t i = 0; i < overlay.temp_clades.size(); ++i) {
    auto ref = temp_clade_ref(static_cast<clade_id>(i));
    if (!reachable_clades.contains(ref)) continue;
    if (grammar.clades.size() >= static_cast<std::size_t>(no_clade)) {
      throw std::runtime_error("chart SPR: too many dense overlay clades");
    }
    auto dense = static_cast<clade_id>(grammar.clades.size());
    result.temp_clade_to_dense[i] = dense;
    grammar.clades.push_back(overlay.temp_clades[i]);
    result.dense_clade_to_ref.push_back(ref);
  }

  grammar.root_clade =
      chart_spr_detail::dense_clade_id(result,
                                       base_clade_ref(base.root_clade));
  grammar.node_to_clade.assign(base.node_to_clade.size(), no_clade);
  for (std::size_t node = 0; node < base.node_to_clade.size(); ++node) {
    auto cid = base.node_to_clade[node];
    if (cid == no_clade || cid >= result.base_clade_to_dense.size()) continue;
    grammar.node_to_clade[node] = result.base_clade_to_dense[cid];
  }

  for (std::size_t pid = 0; pid < base.productions.size(); ++pid) {
    auto ref = base_production_ref(static_cast<production_id>(pid));
    if (!reachable_productions.contains(ref)) continue;
    if (grammar.productions.size() >= static_cast<std::size_t>(no_production)) {
      throw std::runtime_error("chart SPR: too many dense overlay productions");
    }
    auto const& base_prod = base.productions[pid];
    grammar_production prod = base_prod;
    prod.parent = chart_spr_detail::dense_clade_id(
        result, base_clade_ref(base_prod.parent));
    for (auto& child : prod.children)
      child = chart_spr_detail::dense_clade_id(result, base_clade_ref(child));

    auto dense_pid = static_cast<production_id>(grammar.productions.size());
    result.base_production_to_dense[pid] = dense_pid;
    grammar.productions.push_back(std::move(prod));
    result.dense_production_to_ref.push_back(ref);
  }

  for (std::size_t i = 0; i < overlay.temp_productions.size(); ++i) {
    auto ref = temp_production_ref(static_cast<production_id>(i));
    if (!reachable_productions.contains(ref)) continue;
    if (grammar.productions.size() >= static_cast<std::size_t>(no_production)) {
      throw std::runtime_error("chart SPR: too many dense overlay productions");
    }
    auto const& overlay_prod = overlay.temp_productions[i];
    grammar_production prod;
    prod.parent = chart_spr_detail::dense_clade_id(result,
                                                   overlay_prod.parent);
    prod.children.reserve(overlay_prod.children.size());
    for (auto child_ref : overlay_prod.children)
      prod.children.push_back(
          chart_spr_detail::dense_clade_id(result, child_ref));
    prod.witnesses = overlay_prod.witnesses;
    prod.multiplicity = overlay_prod.multiplicity;

    auto dense_pid = static_cast<production_id>(grammar.productions.size());
    result.temp_production_to_dense[i] = dense_pid;
    grammar.productions.push_back(std::move(prod));
    result.dense_production_to_ref.push_back(ref);
  }

  grammar.productions_by_parent.assign(grammar.clades.size(), {});
  grammar.productions_by_child.assign(grammar.clades.size(), {});
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto dense_pid = static_cast<production_id>(pid);
    auto const& prod = grammar.productions[pid];
    if (prod.parent == no_clade || prod.parent >= grammar.clades.size()) {
      throw std::runtime_error("chart SPR: dense production parent out of range");
    }
    grammar.productions_by_parent[prod.parent].push_back(dense_pid);

    std::vector<clade_id> unique_children = prod.children;
    std::sort(unique_children.begin(), unique_children.end());
    unique_children.erase(
        std::unique(unique_children.begin(), unique_children.end()),
        unique_children.end());
    for (auto child : unique_children) {
      if (child == no_clade || child >= grammar.clades.size()) {
        throw std::runtime_error("chart SPR: dense production child out of range");
      }
      grammar.productions_by_child[child].push_back(dense_pid);
    }
  }

  return result;
}

inline overlay_materialization_result materialize_overlay_grammar(
    overlay_clade_grammar const& overlay) {
  chart_spr_detail::validate_overlay(overlay);
  auto result = chart_spr_detail::materialize_overlay_grammar_core(overlay);
  parsimony_chart_detail::validate_chart_grammar(result.grammar);
  chart_execution_plan_detail::record_legacy_production_index_validation();
  chart_trim_detail::validate_production_indices(result.grammar);
  result.grammar.execution_generation =
      detail::allocate_clade_grammar_execution_generation();
  return result;
}

template <typename DenseMaterializationFinished>
inline planned_overlay_materialization_result
materialize_overlay_grammar_with_plan_impl(
    overlay_clade_grammar const& overlay,
    checked_chart_execution_plan_ref const& checked_base,
    bool* dense_materialization_completed,
    overlay_payload_validation_stats* completed_payload_validation_stats,
    DenseMaterializationFinished&& dense_materialization_finished) {
  if (dense_materialization_completed != nullptr) {
    *dense_materialization_completed = false;
  }
  if (completed_payload_validation_stats != nullptr) {
    *completed_payload_validation_stats = {};
  }
  planned_overlay_materialization_result result;
  auto* payload_validation_stats =
      completed_payload_validation_stats != nullptr
          ? completed_payload_validation_stats
          : &result.payload_validation_stats;
  try {
    if (overlay.base == nullptr) {
      throw std::runtime_error("chart SPR: overlay has no base grammar");
    }
    checked_base.assert_same(*overlay.base, checked_base.plan());
    chart_spr_detail::validate_overlay_payload(overlay,
                                               payload_validation_stats);

    result.materialized =
        chart_spr_detail::materialize_overlay_grammar_core(overlay);
    result.materialized.grammar.execution_generation =
        detail::allocate_clade_grammar_execution_generation();
    if (dense_materialization_completed != nullptr) {
      *dense_materialization_completed = true;
    }
  } catch (...) {
    std::forward<DenseMaterializationFinished>(
        dense_materialization_finished)();
    throw;
  }
  std::forward<DenseMaterializationFinished>(
      dense_materialization_finished)();
  if (completed_payload_validation_stats != nullptr) {
    result.payload_validation_stats = *completed_payload_validation_stats;
  }
  // This is the sole full validation of the output grammar.  No unplanned
  // dense grammar escapes if construction rejects it.
  result.execution_plan =
      build_chart_execution_plan(result.materialized.grammar);
  return result;
}


inline planned_overlay_materialization_result
materialize_overlay_grammar_with_plan(
    overlay_clade_grammar const& overlay,
    checked_chart_execution_plan_ref const& checked_base,
    bool* dense_materialization_completed = nullptr,
    overlay_payload_validation_stats* completed_payload_validation_stats =
        nullptr) {
  return materialize_overlay_grammar_with_plan_impl(
      overlay, checked_base, dense_materialization_completed,
      completed_payload_validation_stats, [] {});
}

template <typename DenseMaterializationFinished>
  requires std::invocable<DenseMaterializationFinished&>
inline planned_overlay_materialization_result
materialize_overlay_grammar_with_plan(
    overlay_clade_grammar const& overlay,
    checked_chart_execution_plan_ref const& checked_base,
    bool* dense_materialization_completed,
    DenseMaterializationFinished&& dense_materialization_finished,
    overlay_payload_validation_stats* completed_payload_validation_stats =
        nullptr) {
  return materialize_overlay_grammar_with_plan_impl(
      overlay, checked_base, dense_materialization_completed,
      completed_payload_validation_stats,
      std::forward<DenseMaterializationFinished>(
          dense_materialization_finished));
}

inline planned_overlay_materialization_result
materialize_candidate_overlay_grammar_with_plan(
    clade_grammar const& base,
    checked_chart_execution_plan_ref const& checked_base,
    grammar_spr_candidate const& candidate,
    bool* dense_materialization_completed = nullptr,
    overlay_payload_validation_stats* completed_payload_validation_stats =
        nullptr) {
  if (dense_materialization_completed != nullptr) {
    *dense_materialization_completed = false;
  }
  if (completed_payload_validation_stats != nullptr) {
    *completed_payload_validation_stats = {};
  }
  checked_base.assert_same(base, checked_base.plan());
  auto overlay =
      chart_spr_detail::overlay_from_candidate_payload(base, candidate);
  return materialize_overlay_grammar_with_plan(
      overlay, checked_base, dense_materialization_completed,
      completed_payload_validation_stats);
}


template <typename DenseMaterializationFinished>
  requires std::invocable<DenseMaterializationFinished&>
inline planned_overlay_materialization_result
materialize_candidate_overlay_grammar_with_plan(
    clade_grammar const& base,
    checked_chart_execution_plan_ref const& checked_base,
    grammar_spr_candidate const& candidate,
    bool* dense_materialization_completed,
    DenseMaterializationFinished&& dense_materialization_finished,
    overlay_payload_validation_stats* completed_payload_validation_stats =
        nullptr) {
  if (dense_materialization_completed != nullptr) {
    *dense_materialization_completed = false;
  }
  if (completed_payload_validation_stats != nullptr) {
    *completed_payload_validation_stats = {};
  }
  checked_base.assert_same(base, checked_base.plan());
  auto overlay =
      chart_spr_detail::overlay_from_candidate_payload(base, candidate);
  return materialize_overlay_grammar_with_plan(
      overlay, checked_base, dense_materialization_completed,
      std::forward<DenseMaterializationFinished>(
          dense_materialization_finished),
      completed_payload_validation_stats);
}

inline single_site_chart build_single_site_overlay_chart(
    overlay_clade_grammar const& overlay, leaf_site_states const& leaf_states,
    chart_options const& options = {}) {
  auto materialized = materialize_overlay_grammar(overlay);
  return build_single_site_chart(materialized.grammar, leaf_states, options);
}

inline single_site_overlay_recompute_result build_single_site_overlay_chart_locally(
    overlay_clade_grammar const& overlay,
    overlay_materialization_result const& materialized,
    single_site_chart const& base_chart, leaf_site_states const& leaf_states,
    chart_options const& options = {}) {
  using namespace chart_spr_detail;
  validate_overlay(overlay);
  auto const& dense = materialized.grammar;
  auto const& base = *overlay.base;

  if (options.keep_trace) {
    single_site_overlay_recompute_result fallback;
    fallback.chart = build_single_site_chart(dense, leaf_states, options);
    fallback.affected_clade.assign(dense.clades.size(), true);
    fallback.affected_clade_count = dense.clades.size();
    fallback.used_full_rebuild = true;
    return fallback;
  }

  chart_trim_detail::validate_chart_shapes(base, base_chart);
  if (leaf_states.state_by_taxon.size() != base.taxa.id_to_sample_id.size()) {
    throw std::runtime_error("chart SPR: leaf state count mismatch");
  }

  single_site_overlay_recompute_result result;
  result.chart.inside.assign(dense.clades.size(),
                             parsimony_chart_detail::make_inf_row());
  for (std::size_t cid = 0; cid < base.clades.size(); ++cid) {
    auto dense_cid = materialized.base_clade_to_dense[cid];
    if (dense_cid != no_clade) result.chart.inside[dense_cid] = base_chart.inside[cid];
  }

  result.affected_clade.assign(dense.clades.size(), false);
  std::vector<clade_id> queue;
  auto mark = [&](clade_id clade) {
    if (clade == no_clade || clade >= result.affected_clade.size()) {
      throw std::runtime_error("chart SPR: affected clade out of range");
    }
    if (!result.affected_clade[clade]) {
      result.affected_clade[clade] = true;
      queue.push_back(clade);
    }
  };

  for (auto dense_temp : materialized.temp_clade_to_dense) {
    if (dense_temp != no_clade) mark(dense_temp);
  }
  for (auto pid : overlay.removed_base_productions) {
    if (pid == no_production || pid >= base.productions.size()) {
      throw std::runtime_error("chart SPR: removed production out of range");
    }
    auto dense_parent = materialized.base_clade_to_dense[base.productions[pid].parent];
    if (dense_parent != no_clade) mark(dense_parent);
  }
  for (auto dense_pid : materialized.temp_production_to_dense) {
    if (dense_pid == no_production) continue;
    if (dense_pid >= dense.productions.size()) {
      throw std::runtime_error("chart SPR: temp dense production out of range");
    }
    mark(dense.productions[dense_pid].parent);
  }

  for (std::size_t head = 0; head < queue.size(); ++head) {
    auto child = queue[head];
    for (auto pid : dense.productions_by_child[child]) {
      if (pid == no_production || pid >= dense.productions.size()) {
        throw std::runtime_error("chart SPR: coboundary production out of range");
      }
      mark(dense.productions[pid].parent);
    }
  }

  std::vector<clade_id> affected;
  for (clade_id cid = 0; cid < result.affected_clade.size(); ++cid) {
    if (result.affected_clade[cid]) affected.push_back(cid);
  }
  std::stable_sort(affected.begin(), affected.end(), [&](clade_id lhs,
                                                         clade_id rhs) {
    auto lsize = dense.clades[lhs].taxa.size();
    auto rsize = dense.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize < rsize;
    return lhs < rhs;
  });

  for (auto clade : affected)
    recompute_single_inside_row(dense, leaf_states, result.chart, clade);

  result.affected_clade_count = affected.size();
  return result;
}

inline single_site_overlay_recompute_result build_single_site_overlay_chart_locally(
    overlay_clade_grammar const& overlay, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, chart_options const& options = {}) {
  auto materialized = materialize_overlay_grammar(overlay);
  return build_single_site_overlay_chart_locally(
      overlay, materialized, base_chart, leaf_states, options);
}

inline bool overlay_local_recompute_matches_full(
    overlay_clade_grammar const& overlay, single_site_chart const& base_chart,
    leaf_site_states const& leaf_states, chart_options const& options = {}) {
  chart_options no_trace = options;
  no_trace.keep_trace = false;
  no_trace.max_trace_choices = 0;
  auto local = build_single_site_overlay_chart_locally(overlay, base_chart,
                                                       leaf_states, no_trace);
  auto full = build_single_site_overlay_chart(overlay, leaf_states, no_trace);
  return local.chart.inside == full.inside;
}

inline spr_score_result score_single_site_overlay(
    clade_grammar const& base, leaf_site_states const& leaf_states,
    overlay_clade_grammar const& overlay, chart_options const& options = {},
    std::optional<std::uint8_t> reference_state = std::nullopt) {
  if (overlay.base != &base) {
    throw std::runtime_error(
        "chart SPR: overlay does not point at the supplied base grammar");
  }
  auto old_chart = build_single_site_chart(base, leaf_states, options);
  auto materialized = materialize_overlay_grammar(overlay);
  auto new_chart = build_single_site_chart(materialized.grammar, leaf_states,
                                           options);

  auto old_score = chart_spr_detail::chart_root_score(
      old_chart, base.root_clade, options, reference_state);
  auto new_score = chart_spr_detail::chart_root_score(
      new_chart, materialized.grammar.root_clade, options, reference_state);

  return spr_score_result{chart_spr_detail::signed_delta(old_score, new_score),
                          old_score, new_score, false};
}

inline spr_score_result score_single_site_spr_candidate(
    clade_grammar const& base, leaf_site_states const& leaf_states,
    grammar_spr_candidate const& candidate, chart_options const& options = {}) {
  auto overlay = overlay_from_candidate(base, candidate);
  return score_single_site_overlay(base, leaf_states, overlay, options,
                                   std::nullopt);
}

inline spr_score_result score_single_site_spr_candidate(
    clade_grammar const& base, leaf_site_states const& leaf_states,
    grammar_spr_candidate const& candidate, chart_options const& options,
    std::uint8_t reference_state) {
  auto overlay = overlay_from_candidate(base, candidate);
  return score_single_site_overlay(base, leaf_states, overlay, options,
                                   reference_state);
}

// Diagnostic/oracle helper, not production hot-loop scoring.  This builds a
// dense overlay grammar for the candidate and then calls
// build_composite_chart_score() on both the base grammar and the overlay
// grammar.  Used naively, rejected candidates pay two full composite chart
// rebuilds each; production chart-SPR search must use chart_spr_search_state
// and the overlay-delta local scorer instead.  The result is a composite lower
// bound, not an exact coupled multi-site objective, so it is appropriate for
// tests/diagnostics but not for an accept/reject optimizer loop.
inline spr_score_result score_multisite_spr_candidate_lower_bound(
    clade_grammar const& base, site_pattern_set const& patterns,
    grammar_spr_candidate const& candidate, chart_options const& options = {}) {
  auto overlay = overlay_from_candidate(base, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  auto old_score =
      build_composite_chart_score(base, patterns, options).weighted_lower_bound;
  auto new_score = build_composite_chart_score(materialized.grammar, patterns,
                                               options)
                       .weighted_lower_bound;
  return spr_score_result{chart_spr_detail::signed_delta(old_score, new_score),
                          old_score, new_score, false};
}

// Diagnostic/oracle helper, not production top-K verification.  This
// materializes the candidate overlay and recomputes both the old exact trim and
// the new exact trim from scratch.  Production exact verification should reuse
// the search state's cached old active-only exact score, materialize/count only
// the candidate's new overlay objective for the small verified set, and add any
// invariant offset exactly once at the comparison boundary.
inline spr_score_result score_multisite_spr_candidate_exact(
    clade_grammar const& base, site_pattern_set const& patterns,
    grammar_spr_candidate const& candidate, chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  auto overlay = overlay_from_candidate(base, candidate);
  auto materialized = materialize_overlay_grammar(overlay);
  auto old_score = build_multisite_trim(base, patterns, options, trim_options)
                       .optimum;
  auto new_score = build_multisite_trim(materialized.grammar, patterns, options,
                                        trim_options)
                       .optimum;
  return spr_score_result{chart_spr_detail::signed_delta(old_score, new_score),
                          old_score, new_score, true};
}

inline std::vector<grammar_spr_candidate> enumerate_grammar_spr_candidates(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options options = {}) {
  std::vector<grammar_spr_candidate> candidates;
  (void)for_each_grammar_spr_candidate(
      grammar, options, [&](grammar_spr_candidate const& candidate) {
        candidates.push_back(candidate);
        return true;
      });
  return candidates;
}

inline std::optional<grammar_spr_candidate> project_tree_spr_move_to_candidate(
    chart_spr_detail::sampled_tree_projection_context const& prepared,
    spr_move const& move) {
  using namespace chart_spr_detail;
  auto const& base = prepared.base();
  auto& tree = prepared.source_tree();
  auto const& index = prepared.index();

  if (!index.is_valid(move.src) || !index.is_valid(move.dst)) return std::nullopt;
  if (move.src == move.dst || move.src == index.get_tree_root())
    return std::nullopt;
  if (index.is_ancestor(move.src, move.dst)) return std::nullopt;

  auto src_parent_node = index.get_parent(move.src);
  if (index.get_num_children(src_parent_node) != 2) return std::nullopt;

  auto mapped_clade = [&](std::size_t node) -> std::optional<clade_id> {
    if (node >= prepared.node_to_base_clade().size()) return std::nullopt;
    auto clade = prepared.node_to_base_clade()[node];
    if (clade == no_clade) return std::nullopt;
    return clade;
  };
  auto moved = mapped_clade(move.src);
  auto old_parent = mapped_clade(src_parent_node);
  auto target = mapped_clade(move.dst);
  if (!moved || !old_parent || !target) return std::nullopt;

  std::optional<clade_id> old_sibling;
  for (auto child : index.get_children(src_parent_node)) {
    if (child == move.src) continue;
    old_sibling = mapped_clade(child);
  }
  if (!old_sibling) return std::nullopt;

  grammar_spr_candidate candidate;
  candidate.moved_clade = base_clade_ref(*moved);
  candidate.old_parent = base_clade_ref(*old_parent);
  candidate.old_sibling = base_clade_ref(*old_sibling);
  candidate.new_sibling_or_target = base_clade_ref(*target);
  candidate.source_tree_move = move;

  auto after_tree = apply_spr_move_topology_only(tree, move.src, move.dst);
  // The topology edit invalidates the clone's inherited CSR offsets.  Rebuild
  // on this task-local object before deriving descendant-taxon clades.
  build_clade_offsets(after_tree);
  auto after_tree_grammar = build_clade_grammar(after_tree);

  return make_candidate_from_tree_diff_prepared(prepared, after_tree_grammar,
                                                std::move(candidate));
}

inline std::optional<grammar_spr_candidate> project_tree_spr_move_to_candidate(
    clade_grammar const& base, phylo_dag& tree, spr_move const& move) {
  auto prepared = chart_spr_detail::prepare_sampled_tree_projection(base, tree);
  return project_tree_spr_move_to_candidate(prepared, move);
}

inline std::optional<grammar_spr_candidate> project_tree_spr_move_to_candidate(
    chart_spr_detail::sampled_tree_projection_context const& prepared,
    profitable_move const& move) {
  spr_move source{.src = move.src,
                  .dst = move.dst,
                  .lca = move.lca,
                  .score_change = move.score_change};
  return project_tree_spr_move_to_candidate(prepared, source);
}

inline std::optional<grammar_spr_candidate> project_tree_spr_move_to_candidate(
    clade_grammar const& base, phylo_dag& tree, profitable_move const& move) {
  spr_move source{.src = move.src,
                  .dst = move.dst,
                  .lca = move.lca,
                  .score_change = move.score_change};
  return project_tree_spr_move_to_candidate(base, tree, source);
}

inline std::vector<grammar_spr_candidate> bootstrap_spr_candidates_from_tree(
    clade_grammar const& base, phylo_dag& tree,
    tree_spr_bootstrap_options options = {}) {
  using namespace chart_spr_detail;
  auto projection = prepare_sampled_tree_projection(base, tree);
  auto const& index = projection.index();
  move_enumerator enumerator{index, options.score_threshold};
  auto radius = options.radius > 0 ? options.radius
                                   : compute_tree_max_depth(tree) * 2;
  if (radius == 0) radius = 1;

  std::vector<profitable_move> moves;
  enumerator.find_all_moves(radius, [&](profitable_move const& move) {
    moves.push_back(move);
  });
  std::sort(moves.begin(), moves.end(), [](auto const& lhs, auto const& rhs) {
    if (lhs.score_change != rhs.score_change)
      return lhs.score_change < rhs.score_change;
    if (lhs.src != rhs.src) return lhs.src < rhs.src;
    return lhs.dst < rhs.dst;
  });

  std::vector<grammar_spr_candidate> candidates;
  std::set<std::string> seen;
  for (auto const& move : moves) {
    auto projected = project_tree_spr_move_to_candidate(projection, move);
    if (!projected) continue;
    append_deduplicated_candidate_by_taxa(base, candidates, seen,
                                          std::move(*projected),
                                          options.max_candidates);
    if (options.max_candidates != 0 &&
        candidates.size() >= options.max_candidates) {
      break;
    }
  }
  return candidates;
}

namespace chart_spr_detail {

inline void append_synthetic_tree_edge(phylo_dag& tree,
                                       std::size_t parent_idx,
                                       std::size_t child_idx,
                                       std::size_t clade_index) {
  auto edge = tree.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_index;
  std::visit([&](auto parent) { edge.set_parent(parent); },
             tree.get_node(parent_idx));
  std::visit([&](auto child) { edge.set_child(child); },
             tree.get_node(child_idx));
}

inline std::vector<compact_genome> collect_sampled_tree_leaf_compact_genomes(
    phylo_dag& source, clade_grammar const& grammar) {
  std::vector<compact_genome> result(grammar.taxa.id_to_sample_id.size());
  std::vector<bool> seen(grammar.taxa.id_to_sample_id.size(), false);

  auto reachable = larch::detail::collect_reachable(source);
  for (auto node_idx : reachable.nodes) {
    auto nv = source.get_node(node_idx);
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            std::string sample_id{node.sample_id()};
            auto it = grammar.taxa.sample_id_to_id.find(sample_id);
            if (it == grammar.taxa.sample_id_to_id.end()) return;
            auto taxon = it->second;
            if (taxon >= result.size()) {
              throw std::runtime_error(
                  "chart SPR sampled-tree source: taxon id out of range for "
                  "sample '" +
                  sample_id + "'");
            }
            if (seen[taxon] && !(result[taxon] == node.cg())) {
              throw std::runtime_error(
                  "chart SPR sampled-tree source: conflicting compact genomes "
                  "for duplicate sample '" +
                  sample_id + "'");
            }
            result[taxon] = node.cg();
            seen[taxon] = true;
          }
        },
        nv);
  }

  for (std::size_t taxon = 0; taxon < seen.size(); ++taxon) {
    if (!seen[taxon]) {
      throw std::runtime_error(
          "chart SPR sampled-tree source: source DAG is missing compact "
          "genome for sample '" +
          grammar.taxa.id_to_sample_id[taxon] + "'");
    }
  }
  return result;
}

inline phylo_dag build_sampled_tree_from_grammar(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options,
    std::size_t sample_index, std::mt19937& rng) {
  if (options.sampled_tree_source_dag == nullptr) {
    throw std::runtime_error(
        "chart SPR sampled-tree source requires sampled_tree_source_dag for "
        "score-compatible compact genomes");
  }
  auto& source = *options.sampled_tree_source_dag;
  auto leaf_cgs = collect_sampled_tree_leaf_compact_genomes(source, grammar);

  phylo_dag tree;
  auto ua = tree.append_node<node_kind::ua>();
  ua.reference_sequence() = get_reference_sequence(source);
  tree.set_root(ua);

  std::set<clade_id> active;
  auto build_subtree = [&](auto&& self, clade_id clade) -> std::size_t {
    if (clade == no_clade || clade >= grammar.clades.size()) {
      throw std::runtime_error(
          "chart SPR sampled-tree source: clade out of range");
    }
    if (!active.insert(clade).second) {
      throw std::runtime_error(
          "chart SPR sampled-tree source: cycle in grammar clades");
    }

    auto const& key = grammar.clades[clade];
    std::size_t node_idx = 0;
    if (key.taxa.size() == 1) {
      auto taxon = key.taxa.front();
      if (taxon >= grammar.taxa.id_to_sample_id.size()) {
        throw std::runtime_error(
            "chart SPR sampled-tree source: taxon out of range");
      }
      auto leaf = tree.append_node<node_kind::leaf>();
      leaf.cg() = leaf_cgs[taxon];
      leaf.sample_id() = grammar.taxa.id_to_sample_id[taxon];
      node_idx = leaf.index();
    } else {
      auto const& productions = grammar.productions_by_parent[clade];
      if (productions.empty()) {
        throw std::runtime_error(
            "chart SPR sampled-tree source: non-singleton clade has no "
            "productions");
      }
      auto inner = tree.append_node<node_kind::inner>();
      inner.cg() = compact_genome{};
      node_idx = inner.index();

      production_id chosen = productions.front();
      if (options.randomize_order && productions.size() > 1) {
        std::uniform_int_distribution<std::size_t> dist(
            0, productions.size() - 1);
        chosen = productions[dist(rng)];
      } else if (productions.size() > 1) {
        chosen = productions[(sample_index + clade) % productions.size()];
      }
      auto const& prod = grammar.productions[chosen];
      if (prod.children.size() != 2) {
        throw std::runtime_error(
            "chart SPR sampled-tree source: representative tree requires "
            "binary productions");
      }
      for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
        auto child_idx = self(self, prod.children[child_i]);
        append_synthetic_tree_edge(tree, node_idx, child_idx, child_i);
      }
    }

    active.erase(clade);
    return node_idx;
  };

  auto root_idx = build_subtree(build_subtree, grammar.root_clade);
  append_synthetic_tree_edge(tree, ua.index(), root_idx, 0);
  fitch_assign_compact_genomes(tree);
  recompute_edge_mutations(tree);
  build_clade_offsets(tree);
  return tree;
}

inline sampled_tree_projection_preassignment
preassign_sampled_tree_projection_jobs(
    sampled_tree_projection_context const& prepared,
    grammar_spr_enumeration_options const& options, std::size_t radius,
    std::mt19937& rng) {
  auto const& index = prepared.index();
  if (radius == 0) radius = 1;

  // Reject an irreducible base overrun before even the bounded source-order
  // copy grows. This zero-job/zero-wave estimate already charges its
  // node-count envelope in prepared_owned_and_job_bytes.
  auto const budget = options.sampled_tree_projection_memory_budget_bytes;
  if (budget != 0) {
    auto const base_preflight = estimate_sampled_tree_projection_memory(
        prepared, options.sampled_tree_projection_scheduler, 0, 0,
        options.sampled_tree_projection_external_resident_bytes);
    if (!base_preflight.safely_bounded ||
        base_preflight.required_peak_bytes > budget) {
      throw sampled_tree_projection_budget_error{
          base_preflight.required_peak_bytes, budget};
    }
  }
  auto source_order = index.get_searchable_nodes();
  shuffle_if_requested(source_order, options, &rng);
  std::size_t job_count = 0;
  std::uint64_t counted_fingerprint = 1469598103934665603ULL;
  auto fingerprint_move = [](std::uint64_t& fingerprint,
                             profitable_move const& move) noexcept {
    auto mix = [&](std::uint64_t value) {
      fingerprint ^= value;
      fingerprint *= 1099511628211ULL;
    };
    mix(static_cast<std::uint64_t>(move.src));
    mix(static_cast<std::uint64_t>(move.dst));
    mix(static_cast<std::uint64_t>(move.lca));
    mix(static_cast<std::uint32_t>(move.score_change));
  };
  move_enumerator counting_enumerator{index,
                                      options.sampled_tree_score_threshold};
  for (auto source_node : source_order) {
    counting_enumerator.find_moves_for_source(
        source_node, radius, [&](profitable_move const& move) {
          if (job_count == (std::numeric_limits<std::size_t>::max)()) {
            throw std::overflow_error(
                "chart SPR sampled-tree projection move-count overflow");
          }
          fingerprint_move(counted_fingerprint, move);
          ++job_count;
        });
  }

  sampled_tree_projection_preassignment result;
  result.admitted_scheduler = options.sampled_tree_projection_scheduler;
  result.memory = admit_sampled_tree_projection_memory(
      prepared, options.sampled_tree_projection_scheduler, job_count,
      options.sampled_tree_projection_external_resident_bytes,
      options.sampled_tree_projection_memory_budget_bytes);

  // Finite full admission precedes the all-moves vector, result slots, and
  // every scheduler-owned operation/pool allocation.
  if (job_count != 0 &&
      options.before_sampled_tree_projection_workspace_allocation_for_tests) {
    options.before_sampled_tree_projection_workspace_allocation_for_tests();
  }
  result.jobs.reserve(job_count);
  std::size_t next_ordinal = 0;
  std::uint64_t filled_fingerprint = 1469598103934665603ULL;
  move_enumerator filling_enumerator{index,
                                     options.sampled_tree_score_threshold};
  for (auto source_node : source_order) {
    filling_enumerator.find_moves_for_source(
        source_node, radius, [&](profitable_move const& move) {
          fingerprint_move(filled_fingerprint, move);
          result.jobs.push_back(
              sampled_tree_projection_job{next_ordinal++, move});
        });
  }
  if (result.jobs.size() != job_count ||
      filled_fingerprint != counted_fingerprint) {
    throw std::logic_error(
        "chart SPR sampled-tree projection enumeration was not repeatable");
  }
  result.enumeration_passes = 2;
  result.move_enumeration_visits =
      job_count > (std::numeric_limits<std::size_t>::max)() / 2
          ? (std::numeric_limits<std::size_t>::max)()
          : job_count * 2;

  // Ordinals are assigned on the caller thread in the exact historical
  // source/move traversal. The filled vector is therefore already in the one
  // canonical gather order and needs no allocating/reordering sort.
  for (std::size_t ordinal = 0; ordinal < result.jobs.size(); ++ordinal) {
    if (result.jobs[ordinal].ordinal != ordinal) {
      throw std::logic_error(
          "chart SPR sampled-tree projection ordinal order changed");
    }
  }
  return result;
}

template <typename Gather>
sampled_tree_projection_execution_stats project_preassigned_sampled_tree_moves(
    sampled_tree_projection_context const& prepared,
    sampled_tree_projection_preassignment const& preassignment,
    grammar_spr_enumeration_options const& options, Gather&& gather) {
  auto const jobs = std::span<sampled_tree_projection_job const>{
      preassignment.jobs.data(), preassignment.jobs.size()};
  sampled_tree_projection_execution_stats result;
  result.moves_preassigned = jobs.size();
  auto* scheduler = options.sampled_tree_projection_scheduler;
  if (scheduler != preassignment.admitted_scheduler) {
    throw std::logic_error(
        "chart SPR sampled-tree projection scheduler changed after admission");
  }
  auto const wave_size = preassignment.memory.wave_size;
  result.peak_wave_size = wave_size;
  result.memory = preassignment.memory;
  if (jobs.empty()) return result;

  // Admission was completed before the all-moves vector; it therefore also
  // precedes this owning result wave and scheduler operation storage.
  std::vector<std::optional<grammar_spr_candidate>> slots(wave_size);

  if (scheduler != nullptr &&
      options.force_sampled_tree_projection_submit_failure_after_for_tests) {
    chart_scheduler_test_detail::access::fail_submission_after(
        *scheduler,
        *options.force_sampled_tree_projection_submit_failure_after_for_tests);
  }

  constexpr chart_indexed_range_options range_options{
      .minimum_grain = 1, .target_ranges_per_worker = 1};
  for (std::size_t wave_begin = 0; wave_begin < jobs.size();) {
    auto const count = std::min(wave_size, jobs.size() - wave_begin);
    for (std::size_t local = 0; local < count; ++local) {
      slots[local].reset();
    }

    if (scheduler == nullptr) {
      auto const& job = jobs[wave_begin];
      if (options.before_sampled_tree_projection_for_tests) {
        options.before_sampled_tree_projection_for_tests(job.ordinal);
      }
      slots[0] = project_tree_spr_move_to_candidate(prepared, job.move);
    } else {
      chart_scheduler_run_summary failed_summary;
      auto summary = scheduler->for_each_indexed_range(
          count, range_options,
          [&](chart_indexed_range const& range, std::size_t,
              chart_scheduler_cancellation_token const& cancellation) {
            for (auto local = range.begin; local < range.end; ++local) {
              // A submitted runner owns its first ordinal even when a later
              // submit fails. Subsequent ordinals cooperate with cancellation.
              if (local != range.begin && cancellation.stop_requested()) break;
              auto const& job = jobs[wave_begin + local];
              if (options.before_sampled_tree_projection_for_tests) {
                options.before_sampled_tree_projection_for_tests(job.ordinal);
              }
              slots[local] =
                  project_tree_spr_move_to_candidate(prepared, job.move);
            }
          },
          &failed_summary);
      ++result.scheduler_operations;
      result.parallel_operations += summary.parallel_branch_entered ? 1 : 0;
      result.ranges += summary.range_count;
      result.worker_tasks += summary.worker_tasks_submitted;
      result.active_worker_high_water =
          std::max(result.active_worker_high_water, summary.active_workers);
    }
    ++result.waves;

    for (std::size_t local = 0; local < count; ++local) {
      auto gather_result = std::invoke(gather, jobs[wave_begin + local].ordinal,
                                       std::as_const(slots[local]));
      auto decision = [&] {
        if constexpr (std::is_same_v<
                          std::remove_cvref_t<decltype(gather_result)>, bool>) {
          return gather_result ? sampled_tree_projection_gather_decision::
                                     continue_projection
                               : sampled_tree_projection_gather_decision::
                                     stop_after_current;
        } else {
          return gather_result;
        }
      }();
      if (decision !=
          sampled_tree_projection_gather_decision::continue_projection) {
        // These results were computed and retained but deliberately never
        // enter legacy candidate/prune/dedup counters or callbacks.
        result.speculative_discarded += count - local - 1;
        if (decision ==
            sampled_tree_projection_gather_decision::stop_before_current) {
          ++result.speculative_discarded;
        }
        return result;
      }
    }
    wave_begin += count;
  }
  return result;
}

inline bool candidate_passes_postconstruction_filters(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate,
    grammar_spr_enumeration_options const& options,
    chart_spr_candidate_generation_stats& stats) {
  if (!options.include_root_moves &&
      ((candidate.old_parent.space == overlay_id_space::base &&
        candidate.old_parent.id == grammar.root_clade) ||
       (candidate.new_sibling_or_target.space == overlay_id_space::base &&
        candidate.new_sibling_or_target.id == grammar.root_clade))) {
    note_pruned_after(stats,
                      &chart_spr_candidate_generation_stats::
                          candidates_pruned_root_or_trivial);
    return false;
  }
  auto const& moved_taxa = chart_spr_clade_taxa_for_ref(
      grammar, candidate, candidate.moved_clade);
  auto const& target_taxa = chart_spr_clade_taxa_for_ref(
      grammar, candidate, candidate.new_sibling_or_target);
  if (!clade_size_allowed(moved_taxa.size(), options.min_moved_clade_size,
                          options.max_moved_clade_size)) {
    note_pruned_after(stats,
                      &chart_spr_candidate_generation_stats::
                          candidates_pruned_moved_size);
    return false;
  }
  if (!clade_size_allowed(target_taxa.size(), options.min_target_clade_size,
                          options.max_target_clade_size)) {
    note_pruned_after(stats,
                      &chart_spr_candidate_generation_stats::
                          candidates_pruned_target_size);
    return false;
  }
  if (!disjoint_taxa(moved_taxa, target_taxa)) {
    note_pruned_after(stats,
                      &chart_spr_candidate_generation_stats::
                          candidates_pruned_overlap);
    return false;
  }
  if (options.max_estimated_affected_clades != 0 &&
      estimate_candidate_affected_clades(candidate) >
          options.max_estimated_affected_clades) {
    note_pruned_after(stats,
                      &chart_spr_candidate_generation_stats::
                          candidates_pruned_affected_estimate);
    return false;
  }
  if (!options.include_immediate_reversal_candidates &&
      !options.immediate_reversal_candidate_key_to_skip.empty() &&
      chart_spr_candidate_reversal_key(grammar, candidate) ==
          options.immediate_reversal_candidate_key_to_skip) {
    note_pruned_after(stats,
                      &chart_spr_candidate_generation_stats::
                          candidates_pruned_immediate_reversal);
    return false;
  }
  return true;
}

inline void add_generation_count_stats(
    chart_spr_candidate_generation_stats& dst,
    chart_spr_candidate_generation_stats const& src) {
  dst.upward_path_iterator_steps += src.upward_path_iterator_steps;
  dst.upward_paths_completed += src.upward_paths_completed;
  dst.path_pairs_considered += src.path_pairs_considered;
  dst.candidates_constructed += src.candidates_constructed;
  dst.candidates_pruned_before_construction +=
      src.candidates_pruned_before_construction;
  dst.candidates_pruned_after_construction +=
      src.candidates_pruned_after_construction;
  dst.candidates_pruned_root_or_trivial +=
      src.candidates_pruned_root_or_trivial;
  dst.candidates_pruned_moved_size += src.candidates_pruned_moved_size;
  dst.candidates_pruned_target_size += src.candidates_pruned_target_size;
  dst.candidates_pruned_overlap += src.candidates_pruned_overlap;
  dst.candidates_pruned_affected_estimate +=
      src.candidates_pruned_affected_estimate;
  dst.candidates_pruned_immediate_reversal +=
      src.candidates_pruned_immediate_reversal;
  dst.candidates_pruned_duplicate += src.candidates_pruned_duplicate;
  dst.candidates_pruned_invalid += src.candidates_pruned_invalid;

  dst.sampled_tree_projection_moves_preassigned +=
      src.sampled_tree_projection_moves_preassigned;
  dst.sampled_tree_projection_move_enumeration_visits +=
      src.sampled_tree_projection_move_enumeration_visits;
  dst.sampled_tree_projection_enumeration_passes +=
      src.sampled_tree_projection_enumeration_passes;
  dst.sampled_tree_projection_waves += src.sampled_tree_projection_waves;
  dst.sampled_tree_projection_scheduler_operations +=
      src.sampled_tree_projection_scheduler_operations;
  dst.sampled_tree_projection_parallel_operations +=
      src.sampled_tree_projection_parallel_operations;
  dst.sampled_tree_projection_ranges += src.sampled_tree_projection_ranges;
  dst.sampled_tree_projection_worker_tasks +=
      src.sampled_tree_projection_worker_tasks;
  dst.sampled_tree_projection_active_worker_high_water =
      std::max(dst.sampled_tree_projection_active_worker_high_water,
               src.sampled_tree_projection_active_worker_high_water);
  dst.sampled_tree_projection_peak_wave_size =
      std::max(dst.sampled_tree_projection_peak_wave_size,
               src.sampled_tree_projection_peak_wave_size);
  dst.sampled_tree_projection_speculative_discarded +=
      src.sampled_tree_projection_speculative_discarded;
  dst.sampled_tree_projection_estimated_peak_bytes =
      std::max(dst.sampled_tree_projection_estimated_peak_bytes,
               src.sampled_tree_projection_estimated_peak_bytes);
}

}  // namespace chart_spr_detail

template <typename F>
chart_spr_candidate_generation_stats
chart_spr_detail::for_each_sampled_tree_spr_candidate_stream(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  using namespace chart_spr_detail;

  chart_spr_candidate_generation_stats stats;
  std::set<std::string> seen;
  auto request_stop = [&](chart_spr_candidate_stop_reason reason) -> bool {
    if (stats.stop_reason == chart_spr_candidate_stop_reason::exhausted) {
      stats.stop_reason = reason;
    }
    return false;
  };
  auto stopped = [&]() {
    return stats.stop_reason != chart_spr_candidate_stop_reason::exhausted;
  };
  auto pre_dedup_cap_reached = [&]() {
    return options.max_candidates != 0 &&
           !options.max_candidates_is_post_dedup &&
           stats.candidates_constructed >= options.max_candidates;
  };
  auto post_dedup_cap_reached = [&]() {
    return options.max_candidates != 0 &&
           options.max_candidates_is_post_dedup &&
           stats.candidates_generated_after_dedup >= options.max_candidates;
  };

  std::mt19937 rng(options.seed);
  for (std::size_t sample_i = 0;
       sample_i < options.sampled_tree_count && !stopped(); ++sample_i) {
    auto tree =
        build_sampled_tree_from_grammar(grammar, options, sample_i, rng);
    auto radius = options.sampled_tree_spr_radius > 0
                      ? options.sampled_tree_spr_radius
                      : compute_tree_max_depth(tree) * 2;
    if (radius == 0) radius = 1;
    auto projection = prepare_sampled_tree_projection(grammar, tree);
    auto preassignment = preassign_sampled_tree_projection_jobs(
        projection, options, radius, rng);
    auto execution = project_preassigned_sampled_tree_moves(
        projection, preassignment, options,
        [&](std::size_t, std::optional<grammar_spr_candidate> const& projected)
            -> sampled_tree_projection_gather_decision {
          using decision = sampled_tree_projection_gather_decision;
          if (stopped()) return decision::stop_before_current;
          if (pre_dedup_cap_reached()) {
            request_stop(chart_spr_candidate_stop_reason::candidate_cap);
            return decision::stop_before_current;
          }
          if (!projected) {
            note_pruned_after(stats, &chart_spr_candidate_generation_stats::
                                         candidates_pruned_invalid);
            return decision::continue_projection;
          }
          ++stats.candidates_constructed;
          if (!candidate_passes_postconstruction_filters(grammar, *projected,
                                                         options, stats)) {
            if (pre_dedup_cap_reached()) {
              request_stop(chart_spr_candidate_stop_reason::candidate_cap);
              return decision::stop_after_current;
            }
            return decision::continue_projection;
          }
          auto signature =
              chart_spr_candidate_taxon_signature(grammar, *projected);
          if (!seen.insert(std::move(signature)).second) {
            note_pruned_after(stats, &chart_spr_candidate_generation_stats::
                                         candidates_pruned_duplicate);
            if (pre_dedup_cap_reached()) {
              request_stop(chart_spr_candidate_stop_reason::candidate_cap);
              return decision::stop_after_current;
            }
            return decision::continue_projection;
          }
          ++stats.candidates_generated_after_dedup;
          if (grammar_spr_candidate_involves_multifurcation(grammar,
                                                            *projected)) {
            ++stats.spr_multifurcation_moves_generated;
          }
          if (!invoke_candidate_callback(callback, *projected)) {
            request_stop(chart_spr_candidate_stop_reason::callback_stop);
            return decision::stop_after_current;
          }
          if (pre_dedup_cap_reached() || post_dedup_cap_reached()) {
            request_stop(chart_spr_candidate_stop_reason::candidate_cap);
            return decision::stop_after_current;
          }
          return decision::continue_projection;
        });
    stats.sampled_tree_projection_moves_preassigned +=
        execution.moves_preassigned;
    stats.sampled_tree_projection_move_enumeration_visits +=
        preassignment.move_enumeration_visits;
    stats.sampled_tree_projection_enumeration_passes +=
        preassignment.enumeration_passes;
    stats.sampled_tree_projection_waves += execution.waves;
    stats.sampled_tree_projection_scheduler_operations +=
        execution.scheduler_operations;
    stats.sampled_tree_projection_parallel_operations +=
        execution.parallel_operations;
    stats.sampled_tree_projection_ranges += execution.ranges;
    stats.sampled_tree_projection_worker_tasks += execution.worker_tasks;
    stats.sampled_tree_projection_active_worker_high_water =
        std::max(stats.sampled_tree_projection_active_worker_high_water,
                 execution.active_worker_high_water);
    stats.sampled_tree_projection_peak_wave_size = std::max(
        stats.sampled_tree_projection_peak_wave_size, execution.peak_wave_size);
    stats.sampled_tree_projection_speculative_discarded +=
        execution.speculative_discarded;
    stats.sampled_tree_projection_estimated_peak_bytes =
        std::max(stats.sampled_tree_projection_estimated_peak_bytes,
                 execution.memory.required_peak_bytes);
  }
  return stats;
}

template <typename F>
chart_spr_candidate_generation_stats
chart_spr_detail::for_each_hybrid_spr_candidate_stream(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  using namespace chart_spr_detail;

  chart_spr_candidate_generation_stats combined;
  std::set<std::string> emitted;

  auto stopped = [&]() {
    return combined.stop_reason != chart_spr_candidate_stop_reason::exhausted;
  };
  auto request_stop = [&](chart_spr_candidate_stop_reason reason) -> bool {
    if (combined.stop_reason == chart_spr_candidate_stop_reason::exhausted) {
      combined.stop_reason = reason;
    }
    return false;
  };
  auto emit_unique = [&](grammar_spr_candidate const& candidate) -> bool {
    auto signature = chart_spr_candidate_taxon_signature(grammar, candidate);
    if (!emitted.insert(signature).second) {
      note_pruned_after(combined,
                        &chart_spr_candidate_generation_stats::
                            candidates_pruned_duplicate);
      return true;
    }
    ++combined.candidates_generated_after_dedup;
    if (grammar_spr_candidate_involves_multifurcation(grammar, candidate)) {
      ++combined.spr_multifurcation_moves_generated;
    }
    if (!invoke_candidate_callback(callback, candidate)) {
      return request_stop(chart_spr_candidate_stop_reason::callback_stop);
    }
    if (options.max_candidates != 0 && options.max_candidates_is_post_dedup &&
        combined.candidates_generated_after_dedup >= options.max_candidates) {
      return request_stop(chart_spr_candidate_stop_reason::candidate_cap);
    }
    return true;
  };

  auto child_options_for = [&](chart_spr_candidate_source source) {
    auto child = options;
    child.source = source;
    child.reservoir_sample = false;
    if (options.max_candidates != 0 &&
        !options.max_candidates_is_post_dedup) {
      if (combined.candidates_constructed >= options.max_candidates) {
        request_stop(chart_spr_candidate_stop_reason::candidate_cap);
        child.max_candidates = 0;
      } else {
        child.max_candidates = options.max_candidates -
                               combined.candidates_constructed;
        child.max_candidates_is_post_dedup = false;
      }
    } else {
      // Post-dedup caps are global across both sources and are enforced by
      // emit_unique(); per-source caps would stop too early when the grammar
      // source first produces duplicates of sampled-tree candidates.
      child.max_candidates = 0;
    }
    return child;
  };

  auto propagate_child_stop = [&](chart_spr_candidate_generation_stats const& s) {
    if (stopped()) return;
    if (s.stop_reason == chart_spr_candidate_stop_reason::path_budget) {
      request_stop(chart_spr_candidate_stop_reason::path_budget);
    } else if (s.stop_reason == chart_spr_candidate_stop_reason::candidate_cap) {
      request_stop(chart_spr_candidate_stop_reason::candidate_cap);
    } else if (s.stop_reason == chart_spr_candidate_stop_reason::callback_stop) {
      request_stop(chart_spr_candidate_stop_reason::callback_stop);
    }
  };

  auto sampled_options = child_options_for(
      chart_spr_candidate_source::sampled_tree);
  if (!stopped()) {
    auto sampled_stats = for_each_sampled_tree_spr_candidate_stream(
        grammar, sampled_options,
        [&](grammar_spr_candidate const& candidate) {
          return emit_unique(candidate) && !stopped();
        });
    add_generation_count_stats(combined, sampled_stats);
    propagate_child_stop(sampled_stats);
  }
  if (stopped()) return combined;

  auto grammar_options = child_options_for(chart_spr_candidate_source::grammar);
  if (!stopped()) {
    auto grammar_stats = chart_spr_detail::for_each_grammar_spr_candidate_stream(
        grammar, grammar_options,
        [&](grammar_spr_candidate const& candidate) {
          return emit_unique(candidate) && !stopped();
        });
    add_generation_count_stats(combined, grammar_stats);
    propagate_child_stop(grammar_stats);
  }
  return combined;
}

template <typename F>
chart_spr_candidate_generation_stats for_each_sampled_tree_spr_candidate(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_execution_plan_detail::record_legacy_production_index_validation();
  chart_trim_detail::validate_production_indices(grammar);
  return chart_spr_detail::for_each_sampled_tree_spr_candidate_stream(
      grammar, options, std::forward<F>(callback));
}

template <typename F>
chart_spr_candidate_generation_stats for_each_hybrid_spr_candidate(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_execution_plan_detail::record_legacy_production_index_validation();
  chart_trim_detail::validate_production_indices(grammar);
  return chart_spr_detail::for_each_hybrid_spr_candidate_stream(
      grammar, options, std::forward<F>(callback));
}

}  // namespace larch

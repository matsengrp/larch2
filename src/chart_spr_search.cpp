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
#include <locale>
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

namespace chart_spr_search_detail {

class chart_spr_lazy_policy_rebuild_token {
 public:
  explicit chart_spr_lazy_policy_rebuild_token(
      chart_spr_lazy_policy_diagnostics diagnostics) noexcept
      : diagnostics_(diagnostics) {}

 private:
  chart_spr_lazy_policy_diagnostics diagnostics_;

  friend chart_spr_lazy_policy_diagnostics const&
  chart_spr_lazy_policy_from_rebuild_token(
      chart_spr_lazy_policy_rebuild_token const& token) noexcept;
};

chart_spr_lazy_policy_diagnostics const&
chart_spr_lazy_policy_from_rebuild_token(
    chart_spr_lazy_policy_rebuild_token const& token) noexcept {
  return token.diagnostics_;
}

}  // namespace chart_spr_search_detail

namespace {

class chart_spr_cache_budget_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The authoritative state-core estimator counts std::function wrapper objects
// inline in sizeof(chart_spr_search_state), but intentionally has no opaque
// allowance for callable-target allocations. Production state callbacks are
// stateless or capture one raw pointer. Frozen libstdc++ stores trivially
// copyable callables of at most its two-pointer local buffer without
// allocation; keep that implementation contract compile-time guarded at every
// production installation site.
template <class Function, class Callable>
void chart_spr_install_state_callback(chart_spr_search_state& state,
                                      Function& destination,
                                      Callable callback) {
  static_assert(sizeof(std::function<void()>) == 4 * sizeof(void*));
  static_assert(std::is_trivially_copyable_v<Callable>);
  static_assert(sizeof(Callable) <= 2 * sizeof(void*));
  static_assert(alignof(Callable) <= alignof(void*));
  destination = std::move(callback);
  // Frozen-libstdc++ stores the guarded target in the wrapper's local buffer,
  // so this installation contributes no opaque persistent allocation.
  state.callback_target_resident_contract_function_count =
      chart_spr_state_callback_function_count(state);
  state.callback_target_resident_contract_revisions =
      chart_spr_state_callback_revisions(state);
  state.callback_target_resident_safely_bounded = true;
}

double chart_spr_elapsed_ms(std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point stop) {
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

std::size_t chart_spr_checked_cache_bytes_add(std::size_t lhs, std::size_t rhs,
                                              char const* context) {
  if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
    throw std::overflow_error(context);
  }
  return lhs + rhs;
}

std::size_t chart_spr_checked_cache_bytes_multiply(std::size_t lhs,
                                                   std::size_t rhs,
                                                   char const* context) {
  if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
    throw std::overflow_error(context);
  }
  return lhs * rhs;
}

template <class Vector>
std::size_t chart_spr_vector_capacity_bytes(Vector const& values,
                                            char const* context) {
  return chart_spr_checked_cache_bytes_multiply(
      values.capacity(), sizeof(typename Vector::value_type), context);
}

std::size_t chart_spr_pattern_payload_bytes(
    std::vector<site_pattern> const& patterns) {
  auto total = chart_spr_vector_capacity_bytes(
      patterns, "chart SPR pattern-cache pattern vector byte overflow");
  for (auto const& pattern : patterns) {
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_vector_capacity_bytes(
            pattern.state_by_taxon,
            "chart SPR pattern-cache leaf-state byte overflow"),
        "chart SPR pattern-cache pattern payload byte overflow");
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_vector_capacity_bytes(
            pattern.positions,
            "chart SPR pattern-cache position byte overflow"),
        "chart SPR pattern-cache pattern payload byte overflow");
  }
  return total;
}

template <class NestedRows>
std::size_t chart_spr_nested_row_storage_bytes(NestedRows const& rows) {
  auto total = chart_spr_vector_capacity_bytes(
      rows, "chart SPR pattern-cache row-vector byte overflow");
  for (auto const& pattern_rows : rows) {
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_vector_capacity_bytes(
            pattern_rows, "chart SPR pattern-cache row byte overflow"),
        "chart SPR pattern-cache nested-row byte overflow");
  }
  return total;
}

std::size_t chart_spr_inside_cache_resident_bytes(
    inside_chart_cache const& cache) {
  auto total = sizeof(cache);
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_pattern_payload_bytes(cache.patterns),
      "chart SPR inside-cache resident byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_nested_row_storage_bytes(cache.base_rows),
      "chart SPR inside-cache resident byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_nested_row_storage_bytes(cache.temp_rows),
      "chart SPR inside-cache resident byte overflow");
  return total;
}

std::size_t chart_spr_outside_cache_resident_bytes(
    outside_chart_cache const& cache) {
  auto total = sizeof(cache);
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_pattern_payload_bytes(cache.patterns),
      "chart SPR outside-cache resident byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_vector_capacity_bytes(
          cache.reference_state_by_pattern,
          "chart SPR outside-cache reference-state byte overflow"),
      "chart SPR outside-cache resident byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_nested_row_storage_bytes(cache.base_rows),
      "chart SPR outside-cache resident byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_nested_row_storage_bytes(cache.temp_rows),
      "chart SPR outside-cache resident byte overflow");
  return total;
}

std::size_t chart_spr_local_commit_cache_resident_bytes(
    inside_chart_cache const& inside, outside_chart_cache const& outside) {
  return chart_spr_checked_cache_bytes_add(
      chart_spr_inside_cache_resident_bytes(inside),
      chart_spr_outside_cache_resident_bytes(outside),
      "chart SPR local-commit cache resident byte overflow");
}

std::size_t chart_spr_minimum_local_commit_cache_bytes(
    chart_spr_search_state const& state) {
  // Two full row surfaces are unavoidable in the current local-commit
  // substrate. Metadata overhead is checked against the budget again after
  // construction; this lower bound prevents knowingly allocating a pair that
  // cannot fit even before vector/pattern overhead.
  return chart_spr_checked_cache_bytes_multiply(
      state.estimated_full_pattern_cache_bytes, 2,
      "chart SPR local-commit cache admission byte overflow");
}

template <class Vector>
std::size_t chart_spr_libstdcxx_capacity_after_resize(Vector const& values,
                                                      std::size_t target_size,
                                                      char const* context) {
  if (target_size <= values.capacity()) return values.capacity();

  // This build is frozen to the GCC-trunk/libstdc++ toolchain.  Its vector
  // resize growth is size + max(size, target-size), i.e. max(2*size, target).
  // Model that policy instead of charging only the logical rows: otherwise a
  // one-row accepted update can pass admission and then double every
  // per-pattern temp-row allocation after the chain has already mutated.
  auto const doubled_size =
      chart_spr_checked_cache_bytes_multiply(values.size(), 2, context);
  return std::max(target_size, doubled_size);
}

template <class Cache>
std::size_t chart_spr_projected_persistent_cache_bytes_after_resize(
    Cache const& cache, std::size_t target_temp_clade_count,
    std::size_t current_resident_bytes, char const* context) {
  using temp_row_vector =
      typename std::remove_cvref_t<decltype(cache.temp_rows)>::value_type;
  auto projected = current_resident_bytes;
  for (auto const& rows : cache.temp_rows) {
    auto const projected_capacity = chart_spr_libstdcxx_capacity_after_resize(
        rows, target_temp_clade_count, context);
    auto const added_capacity = projected_capacity - rows.capacity();
    projected = chart_spr_checked_cache_bytes_add(
        projected,
        chart_spr_checked_cache_bytes_multiply(
            added_capacity, sizeof(typename temp_row_vector::value_type),
            context),
        context);
  }
  return projected;
}

std::size_t chart_spr_conservative_lazy_cache_bytes(std::size_t clade_count,
                                                    std::size_t pattern_count) {
  using chart_type = lazy_multisite_chart;
  using row_type = chart_type::row_type;
  using row_vector = std::vector<row_type>;
  using index_map = std::optional<std::vector<std::size_t>>;
  using weight_vector = std::vector<std::uint32_t>;

  // Fresh projection vectors are resized once and the affected inner vectors
  // grow from empty.  Charging twice the logical cardinality bounds the
  // libstdc++ geometric capacities (and is deliberately conservative for the
  // exact-size map assignments).  Every compressed class count is at most the
  // number of active patterns, including the no-merge worst case.
  auto const doubled_clade_count = chart_spr_checked_cache_bytes_multiply(
      clade_count, 2, "chart SPR lazy local-commit clade capacity overflow");
  auto const doubled_pattern_count = chart_spr_checked_cache_bytes_multiply(
      pattern_count, 2,
      "chart SPR lazy local-commit pattern capacity overflow");
  // GCC 17's allocate_at_least rounds allocations smaller than 16 bytes up to
  // that quantum.  Four uint32_t elements may therefore be resident for a
  // one-pattern weight/global-min vector; use the same simple bound for every
  // inner payload and for the top-level vectors.
  auto const clade_capacity =
      clade_count == 0 ? std::size_t{0}
                       : std::max<std::size_t>(4, doubled_clade_count);
  auto const pattern_capacity =
      pattern_count == 0 ? std::size_t{0}
                         : std::max<std::size_t>(4, doubled_pattern_count);

  auto total = sizeof(chart_type);
  auto const top_level_bytes_per_clade =
      2 * sizeof(row_vector) + 3 * sizeof(index_map) + sizeof(std::size_t) +
      2 * sizeof(weight_vector);
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_checked_cache_bytes_multiply(
          clade_capacity, top_level_bytes_per_clade,
          "chart SPR lazy local-commit container byte overflow"),
      "chart SPR lazy local-commit byte overflow");

  auto const inner_bytes_per_clade = chart_spr_checked_cache_bytes_multiply(
      pattern_capacity,
      2 * sizeof(row_type) + 3 * sizeof(std::size_t) +
          2 * sizeof(std::uint32_t),
      "chart SPR lazy local-commit inner byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_checked_cache_bytes_multiply(
          clade_count, inner_bytes_per_clade,
          "chart SPR lazy local-commit inner byte overflow"),
      "chart SPR lazy local-commit byte overflow");
  return chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_checked_cache_bytes_multiply(
          pattern_capacity, sizeof(chart_cost),
          "chart SPR lazy local-commit root-min byte overflow"),
      "chart SPR lazy local-commit byte overflow");
}

template <class OptionalMaps>
std::size_t chart_spr_optional_map_capacity_bytes(OptionalMaps const& maps,
                                                  char const* context) {
  auto total = chart_spr_vector_capacity_bytes(maps, context);
  for (auto const& map : maps) {
    if (!map) continue;
    total = chart_spr_checked_cache_bytes_add(
        total, chart_spr_vector_capacity_bytes(*map, context), context);
  }
  return total;
}

std::size_t chart_spr_lazy_cache_capacity_bytes(
    lazy_multisite_chart const& chart) {
  auto total = sizeof(chart);
  auto add = [&](std::size_t bytes, char const* context) {
    total = chart_spr_checked_cache_bytes_add(total, bytes, context);
  };
  add(chart_spr_nested_row_storage_bytes(chart.inside_rows_by_clade),
      "chart SPR lazy inside-row capacity byte overflow");
  add(chart_spr_nested_row_storage_bytes(chart.outside_rows_by_clade),
      "chart SPR lazy outside-row capacity byte overflow");
  add(chart_spr_optional_map_capacity_bytes(
          chart.class_index_by_pattern_by_clade,
          "chart SPR lazy inside-map capacity byte overflow"),
      "chart SPR lazy cache capacity byte overflow");
  add(chart_spr_optional_map_capacity_bytes(
          chart.structural_class_index_by_pattern_by_clade,
          "chart SPR lazy structural-map capacity byte overflow"),
      "chart SPR lazy cache capacity byte overflow");
  add(chart_spr_optional_map_capacity_bytes(
          chart.outside_class_index_by_pattern_by_clade,
          "chart SPR lazy outside-map capacity byte overflow"),
      "chart SPR lazy cache capacity byte overflow");
  add(chart_spr_vector_capacity_bytes(
          chart.structural_class_count_by_clade,
          "chart SPR lazy structural-count capacity byte overflow"),
      "chart SPR lazy cache capacity byte overflow");
  add(chart_spr_nested_row_storage_bytes(chart.class_weight_by_clade),
      "chart SPR lazy inside-weight capacity byte overflow");
  add(chart_spr_nested_row_storage_bytes(chart.outside_class_weight_by_clade),
      "chart SPR lazy outside-weight capacity byte overflow");
  add(chart_spr_vector_capacity_bytes(
          chart.outside_global_min_by_pattern,
          "chart SPR lazy root-min capacity byte overflow"),
      "chart SPR lazy cache capacity byte overflow");
  return total;
}

std::size_t chart_spr_all_active_cache_capacity_bytes(
    chart_spr_search_state const& state) {
  auto total = chart_spr_vector_capacity_bytes(
      state.pattern_charts,
      "chart SPR all-active entry capacity byte overflow");
  for (auto const& entry : state.pattern_charts) {
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_vector_capacity_bytes(
            entry.chart.inside,
            "chart SPR all-active inside capacity byte overflow"),
        "chart SPR all-active cache capacity byte overflow");
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_vector_capacity_bytes(
            entry.chart.optimal_choices,
            "chart SPR all-active trace capacity byte overflow"),
        "chart SPR all-active cache capacity byte overflow");
    for (auto const& choices_by_state : entry.chart.optimal_choices) {
      for (auto const& choices : choices_by_state) {
        total = chart_spr_checked_cache_bytes_add(
            total,
            chart_spr_vector_capacity_bytes(
                choices,
                "chart SPR all-active trace-choice capacity byte overflow"),
            "chart SPR all-active cache capacity byte overflow");
      }
    }
  }
  return total;
}

std::size_t chart_spr_projected_local_commit_bytes_after_delta(
    chart_spr_search_state const& state, inside_chart_cache const& inside,
    outside_chart_cache const& outside, spr_overlay_delta const& delta) {
  using row_type = std::array<chart_cost, nuc_state_count>;
  auto const added_clades = delta.temp_clades.size();
  auto const active_patterns = state.active_patterns.patterns.patterns.size();
  if (inside.temp_clade_count != outside.temp_clade_count) {
    throw std::runtime_error(
        "chart SPR local-commit admission: inside/outside temp-row counts "
        "differ");
  }
  if (state.local_commit_persistent_cache_bytes >
      state.resident_pattern_cache_bytes) {
    throw std::runtime_error(
        "chart SPR local-commit admission: persistent cache bytes exceed "
        "resident cache bytes");
  }
  if (inside.base == nullptr || outside.base != inside.base) {
    throw std::runtime_error(
        "chart SPR local-commit admission: persistent caches do not share a "
        "frozen base");
  }
  auto const target_temp_clade_count = chart_spr_checked_cache_bytes_add(
      inside.temp_clade_count, added_clades,
      "chart SPR local-commit temp-clade count overflow");
  // Materialization is reachability-filtered.  A rebased append can reactivate
  // frozen/old-temp descendants absent from the current dense grammar, so
  // current_dense+added is not an upper bound.  All frozen clades plus every
  // merged old/new temp clade is.
  auto const target_dense_clade_count = chart_spr_checked_cache_bytes_add(
      inside.base->clades.size(), target_temp_clade_count,
      "chart SPR local-commit dense-clade count overflow");

  auto projected_persistent =
      chart_spr_projected_persistent_cache_bytes_after_resize(
          inside, target_temp_clade_count,
          chart_spr_inside_cache_resident_bytes(inside),
          "chart SPR local-commit inside capacity byte overflow");
  projected_persistent = chart_spr_checked_cache_bytes_add(
      projected_persistent,
      chart_spr_projected_persistent_cache_bytes_after_resize(
          outside, target_temp_clade_count,
          chart_spr_outside_cache_resident_bytes(outside),
          "chart SPR local-commit outside capacity byte overflow"),
      "chart SPR local-commit persistent capacity byte overflow");

  auto const persistent_row_view = state.local_commit_inside_rows.valid();
  if (persistent_row_view &&
      state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    throw std::runtime_error(
        "chart SPR local-commit admission: lazy strategy published a dense "
        "inside-row view");
  }
  if (persistent_row_view) {
    state.local_commit_inside_rows.assert_compatible(state.execution_plan,
                                                     state.active_patterns);
    if (!state.pattern_charts.empty() ||
        state.resident_pattern_cache_bytes !=
            state.local_commit_persistent_cache_bytes) {
      throw std::runtime_error(
          "chart SPR local-commit admission: inside-row view retained a "
          "duplicate scoring representation");
    }
  }
  auto current_scoring_bytes =
      persistent_row_view ? std::size_t{0}
                          : state.resident_pattern_cache_bytes -
                                state.local_commit_persistent_cache_bytes;
  if (!persistent_row_view &&
      state.cache_strategy == chart_spr_cache_strategy::all_active_patterns) {
    current_scoring_bytes = chart_spr_all_active_cache_capacity_bytes(state);
  } else if (!persistent_row_view &&
             state.cache_strategy ==
                 chart_spr_cache_strategy::lazy_multisite_chart) {
    if (!state.lazy_chart) {
      throw std::runtime_error(
          "chart SPR local-commit admission: lazy strategy has no chart");
    }
    current_scoring_bytes =
        chart_spr_lazy_cache_capacity_bytes(*state.lazy_chart);
  }
  std::size_t projected_scoring_bytes = 0;
  if (!persistent_row_view) switch (state.cache_strategy) {
      case chart_spr_cache_strategy::all_active_patterns: {
        // The refreshed vector is constructed before it replaces the old one,
        // so account both representations at the update peak.  Search charts
        // are trace-free; their only dynamic payload is the dense inside row.
        auto const entries = chart_spr_checked_cache_bytes_multiply(
            active_patterns, 2,
            "chart SPR local-commit scoring entry capacity overflow");
        auto const entry_bytes = chart_spr_checked_cache_bytes_multiply(
            entries, sizeof(pattern_chart_cache_entry),
            "chart SPR local-commit scoring entry byte overflow");
        auto const row_bytes = chart_spr_checked_cache_bytes_multiply(
            chart_spr_checked_cache_bytes_multiply(
                active_patterns, target_dense_clade_count,
                "chart SPR local-commit scoring row-count overflow"),
            sizeof(row_type),
            "chart SPR local-commit scoring row byte overflow");
        projected_scoring_bytes = chart_spr_checked_cache_bytes_add(
            current_scoring_bytes,
            chart_spr_checked_cache_bytes_add(
                entry_bytes, row_bytes,
                "chart SPR local-commit scoring byte overflow"),
            "chart SPR local-commit scoring peak byte overflow");
        break;
      }
      case chart_spr_cache_strategy::pattern_batches: {
        auto const projected_row_bytes = chart_spr_checked_cache_bytes_multiply(
            target_dense_clade_count, sizeof(row_type),
            "chart SPR local-commit batch row byte overflow");
        auto const projected_entry_bytes = chart_spr_checked_cache_bytes_add(
            projected_row_bytes, sizeof(pattern_chart_cache_entry),
            "chart SPR local-commit batch entry byte overflow");
        auto const projected_batch_bytes =
            chart_spr_checked_cache_bytes_multiply(
                state.effective_pattern_batch_size, projected_entry_bytes,
                "chart SPR local-commit batch byte overflow");
        projected_scoring_bytes =
            std::max(current_scoring_bytes, projected_batch_bytes);
        break;
      }
      case chart_spr_cache_strategy::lazy_multisite_chart:
        // Projection owns both old and new charts until publication.  The new
        // bound includes row classes, all three per-pattern class maps, both
        // weight vectors, structural counts, and the top-level vector objects.
        projected_scoring_bytes = chart_spr_checked_cache_bytes_add(
            current_scoring_bytes,
            chart_spr_conservative_lazy_cache_bytes(target_dense_clade_count,
                                                    active_patterns),
            "chart SPR lazy local-commit scoring peak byte overflow");
        break;
    }
  return chart_spr_checked_cache_bytes_add(
      projected_persistent, projected_scoring_bytes,
      "chart SPR local-commit projected cache byte overflow");
}

void chart_spr_require_cache_budget(std::size_t bytes,
                                    chart_cache_options const& options,
                                    char const* context) {
  if (options.memory_budget_bytes != 0 && bytes > options.memory_budget_bytes) {
    throw chart_spr_cache_budget_error(
        std::string{context} + ": estimated resident chart caches require " +
        std::to_string(bytes) + " bytes, exceeding configured budget " +
        std::to_string(options.memory_budget_bytes));
  }
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
  add_chart_spr_scheduler_axis_counters(accumulated.scheduler_axes,
                                        rebuild_counters.scheduler_axes);
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
  accumulated.exact_bnb_levels += rebuild_counters.exact_bnb_levels;
  accumulated.exact_bnb_clades += rebuild_counters.exact_bnb_clades;
  accumulated.exact_bnb_product_combinations +=
      rebuild_counters.exact_bnb_product_combinations;
  accumulated.exact_bnb_frontier_entries +=
      rebuild_counters.exact_bnb_frontier_entries;
  accumulated.exact_bnb_ms += rebuild_counters.exact_bnb_ms;
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
  accumulated.lazy_chart_memory_budget_bytes =
      std::max(accumulated.lazy_chart_memory_budget_bytes,
               rebuild_counters.lazy_chart_memory_budget_bytes);
  accumulated.lazy_chart_inside_max_admitted_slots =
      std::max(accumulated.lazy_chart_inside_max_admitted_slots,
               rebuild_counters.lazy_chart_inside_max_admitted_slots);
  accumulated.lazy_chart_outside_max_admitted_slots =
      std::max(accumulated.lazy_chart_outside_max_admitted_slots,
               rebuild_counters.lazy_chart_outside_max_admitted_slots);
  accumulated.lazy_chart_inside_admission_waves +=
      rebuild_counters.lazy_chart_inside_admission_waves;
  accumulated.lazy_chart_outside_admission_waves +=
      rebuild_counters.lazy_chart_outside_admission_waves;
  accumulated.lazy_chart_inside_memory_limited_levels +=
      rebuild_counters.lazy_chart_inside_memory_limited_levels;
  accumulated.lazy_chart_outside_memory_limited_levels +=
      rebuild_counters.lazy_chart_outside_memory_limited_levels;
  accumulated.lazy_chart_inside_reused_slot_waves +=
      rebuild_counters.lazy_chart_inside_reused_slot_waves;
  accumulated.lazy_chart_outside_reused_slot_waves +=
      rebuild_counters.lazy_chart_outside_reused_slot_waves;
  accumulated.lazy_chart_inside_workspace_evictions +=
      rebuild_counters.lazy_chart_inside_workspace_evictions;
  accumulated.lazy_chart_outside_workspace_evictions +=
      rebuild_counters.lazy_chart_outside_workspace_evictions;
  accumulated.lazy_chart_inside_dependency_ready_executions +=
      rebuild_counters.lazy_chart_inside_dependency_ready_executions;
  accumulated.lazy_chart_outside_dependency_ready_executions +=
      rebuild_counters.lazy_chart_outside_dependency_ready_executions;
  accumulated.lazy_chart_inside_dependency_ready_jobs +=
      rebuild_counters.lazy_chart_inside_dependency_ready_jobs;
  accumulated.lazy_chart_outside_dependency_ready_jobs +=
      rebuild_counters.lazy_chart_outside_dependency_ready_jobs;
  accumulated.lazy_chart_inside_dependency_ready_scheduler_operations +=
      rebuild_counters
          .lazy_chart_inside_dependency_ready_scheduler_operations;
  accumulated.lazy_chart_outside_dependency_ready_scheduler_operations +=
      rebuild_counters
          .lazy_chart_outside_dependency_ready_scheduler_operations;
  accumulated.lazy_chart_inside_dependency_ready_capacity_resident_bytes_max =
      std::max(
          accumulated
              .lazy_chart_inside_dependency_ready_capacity_resident_bytes_max,
          rebuild_counters
              .lazy_chart_inside_dependency_ready_capacity_resident_bytes_max);
  accumulated.lazy_chart_outside_dependency_ready_capacity_resident_bytes_max =
      std::max(
          accumulated
              .lazy_chart_outside_dependency_ready_capacity_resident_bytes_max,
          rebuild_counters
              .lazy_chart_outside_dependency_ready_capacity_resident_bytes_max);
  accumulated.lazy_chart_preflight_peak_bytes =
      std::max(accumulated.lazy_chart_preflight_peak_bytes,
               rebuild_counters.lazy_chart_preflight_peak_bytes);
  accumulated.lazy_chart_actual_peak_bytes =
      std::max(accumulated.lazy_chart_actual_peak_bytes,
               rebuild_counters.lazy_chart_actual_peak_bytes);
  accumulated.lazy_chart_pre_submit_rejections +=
      rebuild_counters.lazy_chart_pre_submit_rejections;
  accumulated.lazy_policy_pilot_runs += rebuild_counters.lazy_policy_pilot_runs;
  accumulated.lazy_policy_frozen_reuses +=
      rebuild_counters.lazy_policy_frozen_reuses;
  if (update_current_skipped_invariant_sites) {
    accumulated.skipped_invariant_sites =
        rebuild_counters.skipped_invariant_sites;
  }
}

chart_spr_search_state rebuild_chart_spr_search_state_after_accept(
    chart_spr_search_state const& previous_state, phylo_dag& rebuilt_dag,
    clade_grammar rebuilt_grammar, chart_spr_search_options const& options,
    bool& reused_patterns, chart_scheduler& scheduler,
    chart_spr_scheduler_axis_counters* failed_scheduler_axes = nullptr) {
  validate_supported_chart_cache_options(options.cache);
  bool build_exact = chart_spr_rebuild_after_accept_needs_exact_trim(options);
  reused_patterns =
      !options.force_pattern_fingerprint_mismatch_for_tests &&
      chart_spr_pattern_source_fingerprint_matches(
          rebuilt_dag, rebuilt_grammar,
          previous_state.pattern_source_fingerprint);
  std::optional<chart_spr_search_detail::chart_spr_lazy_policy_rebuild_token>
      lazy_policy_rebuild_token;
  // Auto is a once-per-search representation decision. Carry only an actual
  // automatic resolution through conservative accepted rebuilds and final
  // compaction, including the pattern-fingerprint-mismatch path. Explicit
  // off/on policies are cheap, immutable requests and do not count as frozen
  // auto reuse.
  if (previous_state.lazy_policy.requested ==
      chart_spr_lazy_policy::automatic) {
    lazy_policy_rebuild_token.emplace(previous_state.lazy_policy);
  }
  auto const overlapping_published_state_bytes =
      estimate_chart_spr_published_state_resident_bytes(previous_state);

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
        options.chart, build_exact, options.exact_trim, options.cache, {},
        &scheduler, failed_scheduler_axes,
        lazy_policy_rebuild_token ? &*lazy_policy_rebuild_token : nullptr,
        overlapping_published_state_bytes);
    state.exact_verifier_concurrency =
        previous_state.exact_verifier_concurrency;
    return state;
  }

  auto active_build = make_active_search_patterns(
      rebuilt_dag, rebuilt_grammar, scheduler, options.chart);
  auto state = build_chart_spr_search_state_from_active(
      rebuilt_dag, std::move(rebuilt_grammar), std::move(active_build),
      options.chart, build_exact, options.exact_trim, options.cache, {},
      &scheduler, failed_scheduler_axes,
      lazy_policy_rebuild_token ? &*lazy_policy_rebuild_token : nullptr,
      overlapping_published_state_bytes);
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
    chart_spr_search_counters& counters, chart_scheduler& scheduler) {
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
      result.reused_patterns, scheduler);
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
  // so shared selected subtrees in the before/after certificates can be reused
  // within one verifier invocation without depending on dense ids.
  std::map<std::string, chart_spr_selected_topology_cache_entry> rows_by_key;
};

std::size_t chart_spr_estimate_selected_topology_cache_admission_bytes(
    chart_spr_search_state const& state,
    grammar_spr_candidate const& candidate) {
  auto const before_nodes = state.grammar.clades.size();
  auto const after_nodes = chart_spr_checked_cache_bytes_add(
      before_nodes, candidate.added_clades.size(),
      "chart SPR selected-topology cache node-count overflow");
  auto const entry_count = chart_spr_checked_cache_bytes_add(
      before_nodes, after_nodes,
      "chart SPR selected-topology cache entry-count overflow");
  auto const pattern_count = state.active_patterns.patterns.patterns.size();
  using row_type = std::array<chart_cost, nuc_state_count>;
  auto const row_count = chart_spr_checked_cache_bytes_multiply(
      entry_count, pattern_count,
      "chart SPR selected-topology cache row-count overflow");
  auto total = chart_spr_checked_cache_bytes_multiply(
      row_count, sizeof(row_type),
      "chart SPR selected-topology cache row-byte overflow");

  // Each entry owns one row vector and one ordered-map node. Four pointers are
  // a conservative allowance for parent/left/right/color/alignment metadata.
  using map_type = decltype(chart_spr_selected_topology_row_cache::rows_by_key);
  auto const entry_overhead = chart_spr_checked_cache_bytes_add(
      sizeof(typename map_type::value_type), 4 * sizeof(void*),
      "chart SPR selected-topology cache entry-overhead overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_checked_cache_bytes_multiply(
          entry_count, entry_overhead,
          "chart SPR selected-topology cache entry-byte overflow"),
      "chart SPR selected-topology cache admission overflow");

  // Structural keys recursively contain their descendant keys. Bound every
  // node by the largest possible selected-tree key, then allow 2x string
  // capacity rounding. This is intentionally conservative but still tiny next
  // to pattern rows on real workloads.
  auto const taxon_count = state.active_patterns.patterns.taxon_count;
  auto const taxon_digits = std::to_string(taxon_count).size();
  auto max_key_bytes = chart_spr_checked_cache_bytes_multiply(
      taxon_count, taxon_digits + 4,
      "chart SPR selected-topology cache key-byte overflow");
  max_key_bytes = chart_spr_checked_cache_bytes_add(
      max_key_bytes,
      chart_spr_checked_cache_bytes_multiply(
          after_nodes, 3,
          "chart SPR selected-topology cache key-byte overflow"),
      "chart SPR selected-topology cache key-byte overflow");
  auto const all_key_bytes = chart_spr_checked_cache_bytes_multiply(
      entry_count,
      chart_spr_checked_cache_bytes_multiply(
          max_key_bytes, 2,
          "chart SPR selected-topology cache key-capacity overflow"),
      "chart SPR selected-topology cache total-key overflow");
  return chart_spr_checked_cache_bytes_add(
      total, all_key_bytes,
      "chart SPR selected-topology cache admission overflow");
}

std::size_t chart_spr_selected_topology_cache_resident_bytes(
    chart_spr_selected_topology_row_cache const& cache) {
  using map_type = decltype(cache.rows_by_key);
  auto total = sizeof(cache);
  for (auto const& [key, entry] : cache.rows_by_key) {
    total = chart_spr_checked_cache_bytes_add(
        total, sizeof(typename map_type::value_type) + 4 * sizeof(void*),
        "chart SPR selected-topology cache resident overflow");
    total = chart_spr_checked_cache_bytes_add(
        total, key.capacity() * sizeof(char),
        "chart SPR selected-topology cache resident overflow");
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_vector_capacity_bytes(
            entry.rows_by_pattern,
            "chart SPR selected-topology cache resident row overflow"),
        "chart SPR selected-topology cache resident overflow");
  }
  return total;
}

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
  std::size_t resident_cache_bytes = 0;

  bool verify_materialized_fixed_topology_oracle_for_tests = false;
  bool force_independent_sm_bug_for_tests = false;
  // Phase 9 transient-extension verifier options (mirrored from
  // chart_spr_search_options by the substrate builder).
  bool verify_transient_chain_extension_oracle_for_tests = false;
  bool force_transient_chain_extension_oracle_mismatch_for_tests = false;
  std::size_t cache_multifurcation_productions_scored_reported = 0;
};

std::size_t chart_spr_copy_capacity_bytes(std::size_t count,
                                          std::size_t element_bytes,
                                          char const* context) {
  if (count == 0) return 0;
  auto capacity = chart_spr_checked_cache_bytes_multiply(count, 2, context);
  capacity = std::max<std::size_t>(4, capacity);
  return chart_spr_checked_cache_bytes_multiply(capacity, element_bytes,
                                                context);
}

std::size_t chart_spr_transient_clade_copy_bytes(clade_key const& clade) {
  return chart_spr_copy_capacity_bytes(
      clade.taxa.size(), sizeof(taxon_id),
      "chart SPR transient chain clade-copy overflow");
}

std::size_t chart_spr_transient_production_copy_bytes(
    overlay_grammar_production const& production) {
  auto total = chart_spr_copy_capacity_bytes(
      production.children.size(), sizeof(overlay_clade_ref),
      "chart SPR transient chain production-copy overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_copy_capacity_bytes(
          production.witnesses.size(), sizeof(production_witness),
          "chart SPR transient chain witness-copy overflow"),
      "chart SPR transient chain production-copy overflow");
  for (auto const& witness : production.witnesses) {
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_copy_capacity_bytes(
            witness.children.size(), sizeof(production_child_witness),
            "chart SPR transient chain witness-child-copy overflow"),
        "chart SPR transient chain production-copy overflow");
    for (auto const& child : witness.children) {
      total = chart_spr_checked_cache_bytes_add(
          total,
          chart_spr_copy_capacity_bytes(
              child.edge_alternatives.size(), sizeof(std::size_t),
              "chart SPR transient chain witness-edge-copy overflow"),
          "chart SPR transient chain production-copy overflow");
    }
  }
  return total;
}

template <class Clades, class Productions, class Removed>
std::size_t chart_spr_transient_delta_payload_copy_bytes(
    Clades const& clades, Productions const& productions,
    Removed const& removed, std::size_t commit_source_size = 0) {
  auto total = chart_spr_copy_capacity_bytes(
      clades.size(), sizeof(clade_key),
      "chart SPR transient chain clade-vector-copy overflow");
  for (auto const& clade : clades) {
    total = chart_spr_checked_cache_bytes_add(
        total, chart_spr_transient_clade_copy_bytes(clade),
        "chart SPR transient chain delta-copy overflow");
  }
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_copy_capacity_bytes(
          productions.size(), sizeof(overlay_grammar_production),
          "chart SPR transient chain production-vector-copy overflow"),
      "chart SPR transient chain delta-copy overflow");
  for (auto const& production : productions) {
    total = chart_spr_checked_cache_bytes_add(
        total, chart_spr_transient_production_copy_bytes(production),
        "chart SPR transient chain delta-copy overflow");
  }
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_copy_capacity_bytes(
          removed.size(), sizeof(typename Removed::value_type),
          "chart SPR transient chain tombstone-copy overflow"),
      "chart SPR transient chain delta-copy overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_copy_capacity_bytes(
          commit_source_size, sizeof(char),
          "chart SPR transient chain label-copy overflow"),
      "chart SPR transient chain delta-copy overflow");
  return total;
}

std::size_t chart_spr_cache_commit_transaction_peak_bytes(
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state, spr_overlay_delta const& delta,
    chart_scheduler const& scheduler,
    chart_indexed_range_options range_options) {
  if (!sub.chain || !sub.icache || !sub.ocache) {
    throw std::logic_error(
        "chart SPR cache-commit memory estimator: incomplete substrate");
  }
  using row_type = std::array<chart_cost, nuc_state_count>;
  auto const pattern_count = sub.icache->patterns.size();
  if (sub.ocache->patterns.size() != pattern_count ||
      state.active_patterns.patterns.patterns.size() != pattern_count) {
    throw std::runtime_error(
        "chart SPR cache-commit memory estimator: pattern counts differ");
  }
  auto const target_temp_clades = chart_spr_checked_cache_bytes_add(
      sub.icache->temp_clade_count, delta.temp_clades.size(),
      "chart SPR cache-commit target temp-clade overflow");
  auto const target_clades = chart_spr_checked_cache_bytes_add(
      sub.base_grammar.clades.size(), target_temp_clades,
      "chart SPR cache-commit target clade overflow");

  // Both staged directions are simultaneously resident between the outside
  // join and publication.  Charging every possible tip clade is a safe
  // pre-append bound on the dependency-derived affected subsets.
  auto const staged_row_count = chart_spr_checked_cache_bytes_multiply(
      chart_spr_checked_cache_bytes_multiply(
          pattern_count, target_clades,
          "chart SPR cache-commit staged row-count overflow"),
      2, "chart SPR cache-commit staged direction overflow");
  auto total = chart_spr_checked_cache_bytes_multiply(
      staged_row_count, sizeof(row_type),
      "chart SPR cache-commit staged row-byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_checked_cache_bytes_multiply(
          pattern_count, sizeof(outside_recurrence_work_stats),
          "chart SPR cache-commit recurrence-work overflow"),
      "chart SPR cache-commit transient overflow");

  // Four ref->position tables, two affected-ref vectors, reachability/closure
  // work bitsets, and four production-index vector surfaces.  The vector
  // payload term below separately covers the copied merged overlay values.
  auto const metadata_per_clade =
      4 * sizeof(std::size_t) + 2 * sizeof(overlay_clade_ref) +
      8 * sizeof(bool) + 4 * sizeof(std::vector<std::size_t>);
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_checked_cache_bytes_multiply(
          target_clades, metadata_per_clade,
          "chart SPR cache-commit plan metadata overflow"),
      "chart SPR cache-commit transient overflow");

  // `overlay_chain::tip()` owns a merged overlay copy.  Account every existing
  // delta plus the proposed append with the same frozen-libstdc++ payload
  // bound used by transient exact verification, and retain a second copy for
  // ordered tombstone/index construction at the plan-build peak.
  std::size_t merged_payload = 0;
  for (std::size_t position = 0; position < sub.chain->size(); ++position) {
    auto const& committed = sub.chain->at(position);
    merged_payload = chart_spr_checked_cache_bytes_add(
        merged_payload,
        chart_spr_transient_delta_payload_copy_bytes(
            committed.temp_clades, committed.temp_productions,
            committed.removed_base_productions,
            committed.commit_source.size()),
        "chart SPR cache-commit merged payload overflow");
  }
  merged_payload = chart_spr_checked_cache_bytes_add(
      merged_payload,
      chart_spr_transient_delta_payload_copy_bytes(
          delta.temp_clades, delta.temp_productions,
          delta.removed_base_productions, delta.commit_source.size()),
      "chart SPR cache-commit proposed payload overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_checked_cache_bytes_multiply(
          merged_payload, 2,
          "chart SPR cache-commit merged working-set overflow"),
      "chart SPR cache-commit transient overflow");

  // Inside and outside scheduler operations are separated by a join and use
  // the same range plan, so one operation peak (not their sum) is resident.
  auto const scheduler_plan =
      scheduler.plan_indexed_ranges(pattern_count, range_options);
  total = chart_spr_checked_cache_bytes_add(
      total, estimate_chart_scheduler_operation_peak_bytes(scheduler_plan),
      "chart SPR cache-commit scheduler-operation overflow");
  return total;
}

std::size_t chart_spr_transient_verifier_extra_memory_bound(
    chart_spr_local_commit_substrate const& sub,
    grammar_spr_candidate const& candidate) {
  if (!sub.chain) {
    throw std::logic_error(
        "chart SPR transient memory estimator: missing overlay chain");
  }
  auto chain_copy = sizeof(overlay_chain);
  using base_lookup_value = std::pair<std::vector<taxon_id> const, clade_id>;
  for (auto const& clade : sub.base_grammar.clades) {
    chain_copy = chart_spr_checked_cache_bytes_add(
        chain_copy, sizeof(base_lookup_value) + 4 * sizeof(void*),
        "chart SPR transient base-lookup-copy overflow");
    chain_copy = chart_spr_checked_cache_bytes_add(
        chain_copy, chart_spr_transient_clade_copy_bytes(clade),
        "chart SPR transient base-lookup-copy overflow");
  }
  chain_copy = chart_spr_checked_cache_bytes_add(
      chain_copy,
      chart_spr_copy_capacity_bytes(
          sub.chain->size(), sizeof(spr_overlay_delta),
          "chart SPR transient delta-vector-copy overflow"),
      "chart SPR transient chain-copy overflow");
  for (std::size_t position = 0; position < sub.chain->size(); ++position) {
    auto const& delta = sub.chain->at(position);
    chain_copy = chart_spr_checked_cache_bytes_add(
        chain_copy,
        chart_spr_transient_delta_payload_copy_bytes(
            delta.temp_clades, delta.temp_productions,
            delta.removed_base_productions, delta.commit_source.size()),
        "chart SPR transient chain-copy overflow");
  }
  auto candidate_payload = chart_spr_transient_delta_payload_copy_bytes(
      candidate.added_clades, candidate.added_productions,
      candidate.removed_productions);
  auto working_payload = chart_spr_checked_cache_bytes_add(
      chain_copy, candidate_payload,
      "chart SPR transient working-payload overflow");

  // Four copies cover: the reader-local chain, append-time ordered maps/sets,
  // the merged `tip()` overlay, and vector/string capacity rounding while the
  // planned dense materialization is published. The latter's grammar/plan/B&B
  // storage is charged by the generic estimator, not here.
  auto extra = chart_spr_checked_cache_bytes_multiply(
      working_payload, 4, "chart SPR transient chain working-set overflow");
  if (sub.verify_transient_chain_extension_oracle_for_tests ||
      sub.force_transient_chain_extension_oracle_mismatch_for_tests) {
    // Diagnostic inside/outside cache copies are extended while the originals
    // remain resident. Allow one full copy plus geometric growth.
    extra = chart_spr_checked_cache_bytes_add(
        extra,
        chart_spr_checked_cache_bytes_multiply(
            sub.resident_cache_bytes, 2,
            "chart SPR transient diagnostic cache-copy overflow"),
        "chart SPR transient verifier extra-memory overflow");
  }
  return extra;
}

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
    overlay_materialization_result& materialized) {
  sub.dense_clade_to_chain_ref = std::move(materialized.dense_clade_to_ref);
  sub.dense_production_to_chain_ref =
      std::move(materialized.dense_production_to_ref);
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

std::array<chart_cost, nuc_state_count> const&
chart_spr_read_persistent_inside_dense_row(void const* context,
                                           std::size_t pattern,
                                           clade_id dense) {
  auto const* sub =
      static_cast<chart_spr_local_commit_substrate const*>(context);
  if (sub == nullptr || !sub->icache) {
    throw std::runtime_error(
        "chart SPR inside-row view: missing persistent inside cache");
  }
  if (pattern >= sub->icache->patterns.size() || dense == no_clade ||
      dense >= sub->dense_clade_to_chain_ref.size()) {
    throw std::runtime_error(
        "chart SPR inside-row view: dense row index out of range");
  }
  auto const ref = sub->dense_clade_to_chain_ref[dense];
  if (ref.id == no_clade) {
    throw std::runtime_error(
        "chart SPR inside-row view: dense clade maps to no_clade");
  }
  return sub->icache->row(pattern, ref);
}

void chart_spr_publish_persistent_inside_row_view(
    chart_spr_search_state& state,
    chart_spr_local_commit_substrate const& sub) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    state.local_commit_inside_rows = {};
    return;
  }
  auto checked_state =
      check_chart_execution_plan(state.grammar, state.execution_plan);
  chart_spr_assert_local_commit_tip_identity(
      sub, state, checked_state, "chart SPR inside-row view publication");
  state.local_commit_inside_rows = chart_spr_inside_row_view{
      .context = &sub,
      .reader = &chart_spr_read_persistent_inside_dense_row,
      .pattern_count = state.active_patterns.patterns.patterns.size(),
      .clade_count = state.execution_plan.clades().size(),
      .execution_generation = state.execution_plan.grammar_generation(),
      .execution_fingerprint = state.execution_plan.fingerprint(),
      .active_pattern_fingerprint = sub.published_active_pattern_fingerprint,
  };
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
    chart_spr_persistent_inside_cache_view persistent_icache,
    chart_spr_search_counters& counters) {
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
    ++counters.fixed_topology_selected_cache_hits;
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
    ++counters.fixed_topology_selected_cache_misses;
    counters.fixed_topology_selected_rows_computed +=
        entry.rows_by_pattern.size();
    counters.selected_topology_class_rows_computed +=
        entry.rows_by_pattern.size();
    if (multifurcation_row) {
      counters.selected_topology_multifurcation_rows +=
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
          counters.fixed_topology_icache_rows_recomputed_affected +=
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
          counters.fixed_topology_icache_rows_recomputed_affected +=
              entry.rows_by_pattern.size();
          return;
        }
        chain_ref = (*persistent_icache.dense_clade_to_chain_ref)[clade.id];
        if (chain_ref.id == no_clade) {
          counters.fixed_topology_icache_rows_recomputed_affected +=
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
          counters.fixed_topology_icache_rows_reused +=
              entry.rows_by_pattern.size();
        } else {
          // The selected production is locally suboptimal at this clade; this
          // is an AFFECTED row whose value must come from the selected
          // recurrence (the grammar-min cache could be lower).
          counters.fixed_topology_icache_rows_recomputed_affected +=
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
        persistent_icache, counters));
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
    chart_spr_persistent_inside_cache_view persistent_icache,
    chart_spr_search_counters& counters) {
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
      base_clade_ref(state.grammar.root_clade), active_refs, persistent_icache,
      counters);
  active_refs.clear();
  auto after_root = chart_spr_selected_topology_rows_for_clade(
      cache, state, candidate.candidate, after_selected,
      base_clade_ref(state.grammar.root_clade), active_refs, persistent_icache,
      counters);
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
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters) {
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
        counters.materialization_exact_verification_ms};
    materialized = materialize_overlay_grammar(overlay);
  }
  ++counters.full_overlay_materializations;
  ++counters.overlay_materializations_for_oracle;

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
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters) {
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
        ++counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
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
        ++counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
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
        ++counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
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
        ++counters.fixed_topology_independent_sm_bug_witnesses_for_tests;
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
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_exact_verification_context& context) {
  auto& counters = context.counters;
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

  auto const selected_cache_admission =
      chart_spr_estimate_selected_topology_cache_admission_bytes(
          state, candidate.candidate);
  auto const selected_cache_projected_resident =
      chart_spr_checked_cache_bytes_add(
          state.resident_pattern_cache_bytes, selected_cache_admission,
          "chart SPR selected-topology cache projected byte overflow");
  chart_spr_require_cache_budget(selected_cache_projected_resident,
                                 state.cache_opts,
                                 "chart SPR selected-topology cache");

  chart_spr_fixed_topology_cache_pattern_scores scores;
  auto const& active = state.active_patterns.patterns.patterns;
  scores.old_pattern_scores.reserve(active.size());
  scores.new_pattern_scores.reserve(active.size());
  auto root_chain_ref = chart_spr_chain_ref_for_dense_clade(
      sub, state.grammar.root_clade);

  // Phase 8 fixed-topology cache path.  The grammar-min inside cache cannot be
  // read for a selected topology in a general DAG: an unchanged selected
  // subtree may be locally suboptimal, so `icache.row(...)` would silently
  // substitute a different production.  Instead an invocation-local selected-
  // topology cache stores rows keyed by the structural rooted topology and is
  // the production cache for fixed_topology_exact.  Its lifetime ends after
  // the root scores have been extracted, before either oracle runs.  The
  // outside cache still supplies the root outside row, keeping the score in
  // the same inside + outside convention as the local-commit cache.  No dense
  // overlay materialization or B&B is performed here.
  {
    chart_spr_selected_topology_row_cache selected_topology_cache;
    chart_spr_selected_topology_root_entries roots;
    try {
      chart_spr_persistent_inside_cache_view icache_view;
      icache_view.icache = &*sub.icache;
      icache_view.dense_clade_to_chain_ref = &sub.dense_clade_to_chain_ref;
      roots = chart_spr_selected_topology_root_entries_from_cache(
          selected_topology_cache, state, candidate, icache_view, counters);
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
    auto const selected_cache_resident =
        chart_spr_selected_topology_cache_resident_bytes(
            selected_topology_cache);
    if (selected_cache_resident > selected_cache_admission) {
      throw chart_spr_fixed_topology_cache_invariant_error(
          "chart SPR fixed-topology cache verifier: selected-topology cache "
          "exceeded its conservative admission estimate");
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
  }

  if (sub.force_independent_sm_bug_for_tests) {
    chart_spr_apply_independent_sm_bug_for_tests(scores, state, candidate,
                                                 counters);
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
      context.inner_scheduler != nullptr
          ? fixed_topology_direct_selected_pattern_scores(
                state, candidate, counters, *context.inner_scheduler)
          : fixed_topology_direct_selected_pattern_scores(state, candidate,
                                                          counters);
  if (auto direct_mismatch = chart_spr_fixed_topology_first_pattern_mismatch(
          cache_scores, *scores.direct_oracle_scores, "persistent-cache",
          "direct-overlay-selected-oracle")) {
    ++counters.fixed_topology_persistent_cache_direct_oracle_mismatches;
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
        chart_spr_fixed_topology_materialized_oracle_pattern_scores(
            state, candidate, counters);
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
    chart_spr_fixed_topology_cache_pattern_scores const& cache_scores,
    chart_spr_search_counters& counters) {
  auto const& reason = cache_scores.oracle_mismatch_reason;
  ++counters.fixed_topology_persistent_cache_fallbacks;
  // Issue 4: when the materialized from-scratch oracle ran and mismatched, its
  // value is the authority -- use it directly rather than re-running the
  // direct overlay scorer (which shares overlay-space row machinery with the
  // cache path and is therefore a weaker fallback than the Phase-8 from-scratch
  // oracle contract).  Otherwise the production direct-overlay gate caught
  // the mismatch; its value is the authority for the selected topology.
  try {
    if (cache_scores.materialized_oracle_scores) {
      ++counters.fixed_topology_persistent_cache_oracle_mismatches;
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
    auto delta = fixed_topology_delta_direct_selected_topology(state, candidate,
                                                               counters);
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
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    chart_spr_exact_verification_context& context) {
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

  auto& counters = context.counters;
  ++counters.exact_verifications;
  ++counters.fixed_topology_persistent_cache_verifications;
  try {
    auto scores = chart_spr_fixed_topology_pattern_scores_from_persistent_cache(
        sub, state, candidate, context);
    if (!scores.cache_score_ready_for_exact_label) {
      return chart_spr_verify_fixed_topology_direct_fallback_after_counting(
          state, std::move(candidate), scores, counters);
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
  } catch (chart_scheduler_submit_error const&) {
    // No scheduler run summary exists for a submission failure; preserve it
    // as a hard infrastructure error so global and semantic-axis accounting
    // cannot diverge behind an invalid-candidate result.
    throw;
  } catch (chart_spr_cache_budget_error const&) {
    throw;
  } catch (std::overflow_error const&) {
    throw;
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
    chart_spr_candidate_score const& candidate,
    chart_spr_search_counters& counters) {
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
        counters.materialization_exact_verification_ms};
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
    record_overlay_payload_validation_stats(counters,
                                            completed_payload_validation_stats);
    throw;
  }
  record_planned_overlay_materialization_stats(counters, ext.planned);
  if (build_diagnostic_caches) {
    ++counters.transient_chain_diagnostic_cache_extensions;
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
    multisite_trim_options const& trim_options,
    chart_spr_search_counters& counters) {
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
        counters.materialization_exact_verification_ms};
    cold_materialized = materialize_overlay_grammar(cold_overlay);
  }
  ++counters.full_overlay_materializations;
  ++counters.overlay_materializations_for_oracle;
  auto cold_trim = build_multisite_trim_active(cold_materialized.grammar,
                                               state.active_patterns,
                                               state.chart_opts, trim_options);
  record_multisite_exact_trim_work(counters, cold_trim);
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
      ++counters.transient_chain_extension_oracle_rows_checked_for_tests;
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
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state, chart_spr_candidate_score candidate,
    checked_chart_execution_plan_ref const& checked_state,
    chart_spr_exact_verification_context& context,
    multisite_trim_options const& trim_options) {
  if (!candidate.valid) return candidate;
  auto& counters = context.counters;
  auto* scheduler = context.inner_scheduler;

  chart_spr_transient_extension ext;
  try {
    ext = chart_spr_build_transient_extension(sub, state, checked_state,
                                              candidate, counters);
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
      return verify_candidate_exact_against_state_impl(
          state, std::move(candidate), checked_state, context, trim_options);
    }
    throw;
  }

  // The transient extension succeeded: this candidate's delta resolves to
  // frozen-base productions, so it could be committed.  Count it under the
  // transient-extension counter (never under full_overlay_materializations).
  ++counters.exact_verifications;
  ++counters.transient_chain_extensions_for_verification;
  multisite_trim_result new_trim;
  multisite_trim_scheduler_run_summaries scheduler_runs;
  chart_spr_scheduler_run_axis_publisher publish_setup_runs{
      counters.scheduler_axes.exact_setup_patterns, scheduler_runs.exact_setup};
  chart_spr_scheduler_run_axis_publisher publish_frontier_runs{
      counters.scheduler_axes.exact_frontier_clades,
      scheduler_runs.frontier_clades};
  std::uint64_t authoritative_new_optimum = multisite_score_inf;
  bool transient_trim_authoritative = true;
  try {
    // Old score: the current tip's exact optimum (cached in state, lazily
    // built).  Same source the cold path reads.
    auto const& old_trim =
        context.published_old_trim != nullptr ? *context.published_old_trim
        : scheduler != nullptr
            ? ensure_chart_spr_state_exact_trim(state, checked_state,
                                                *scheduler, trim_options)
            : ensure_chart_spr_state_exact_trim(state, checked_state,
                                                trim_options);

    // New score: B&B exact optimum of the extended grammar.
    if (state.cache_strategy ==
        chart_spr_cache_strategy::lazy_multisite_chart) {
      chart_spr_force_candidate_exact_bnb_overflow_for_tests(candidate);
      new_trim = build_lazy_multisite_trim_active_from_scratch(
          ext.planned, state.active_patterns, state.chart_opts, trim_options);
    } else if (scheduler != nullptr) {
      state.active_patterns.assert_no_skipped_invariant_metadata();
      chart_spr_force_candidate_exact_bnb_overflow_for_tests(candidate);
      new_trim = build_multisite_trim(
          ext.planned.execution_plan, state.active_patterns.patterns,
          *scheduler, state.chart_opts, trim_options, &scheduler_runs);
    } else {
      chart_spr_force_candidate_exact_bnb_overflow_for_tests(candidate);
      new_trim = build_multisite_trim_active(ext.planned.execution_plan,
                                             state.active_patterns,
                                             state.chart_opts, trim_options);
    }
    if (state.cache_strategy ==
        chart_spr_cache_strategy::lazy_multisite_chart) {
      ++counters.exact_trim_lazy_chart_uses;
    }
    record_multisite_exact_trim_work(counters, new_trim);
    ++counters.chart_execution_plan_cache_hits;

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
          sub, state, candidate, ext, new_trim.optimum, trim_options, counters);
      if (!oracle.ok) {
        ++counters.transient_chain_extension_oracle_mismatches;
        ++counters.transient_chain_extension_fallbacks;
        authoritative_new_optimum = oracle.cold_new_optimum;
        transient_trim_authoritative = false;
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
  } catch (chart_scheduler_submit_error const&) {
    throw;
  } catch (multisite_optimal_root_provenance_capture_error const&) {
    throw;
  } catch (std::bad_alloc const&) {
    throw;
  } catch (std::overflow_error const&) {
    // Score/size arithmetic overflow invalidates the computation, not one
    // candidate.  Never turn it into an ordinary invalid-candidate outcome.
    throw;
  } catch (std::logic_error const&) {
    // Scheduler lifecycle/concurrent-use and other invariant failures are hard
    // errors, not properties that can invalidate one biological candidate.
    throw;
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
        ++counters.chart_execution_plan_cache_hits;
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
  if (candidate.valid && candidate.exact && transient_trim_authoritative &&
      state.retain_verified_exact_trim_for_local_commit) {
    candidate.reusable_exact_trim = make_chart_spr_reusable_exact_trim(
        ext.planned.execution_plan, state.active_patterns, state.chart_opts,
        trim_options, state.invariant_constant_offset, std::move(new_trim));
  }
  return candidate;
}

multisite_exact_setup
chart_spr_build_exact_setup_from_persistent_inside_cache_serial(
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

multisite_exact_setup chart_spr_build_exact_setup_from_persistent_inside_cache(
    chart_spr_local_commit_substrate const& sub,
    chart_spr_search_state const& state,
    checked_chart_execution_plan_ref const& checked_state,
    chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary>* run_summaries) {
  chart_spr_assert_local_commit_tip_identity(
      sub, state, checked_state, "chart SPR persistent-inside exact setup");
  if (!sub.icache) {
    throw std::runtime_error(
        "chart SPR persistent-inside exact setup: missing inside cache");
  }

  std::vector<single_site_chart> scratch_by_slot(
      scheduler.worker_resolution().resolved_workers);
  for (auto& scratch : scratch_by_slot) {
    scratch.inside.resize(state.execution_plan.clades().size());
  }
  return build_multisite_exact_setup_from_resident_inside(
      state.execution_plan, state.active_patterns.patterns, scheduler,
      [&](std::size_t pattern_index, site_pattern const&,
          std::size_t stable_slot) -> single_site_chart const& {
        if (pattern_index >= sub.icache->patterns.size()) {
          throw std::runtime_error(
              "chart SPR persistent-inside exact setup: pattern index out of "
              "range");
        }
        if (stable_slot >= scratch_by_slot.size()) {
          throw std::runtime_error(
              "chart SPR persistent-inside exact setup: scheduler slot out "
              "of range");
        }
        auto& scratch = scratch_by_slot[stable_slot];
        for (std::size_t dense = 0; dense < sub.dense_clade_to_chain_ref.size();
             ++dense) {
          scratch.inside[dense] = sub.icache->row(
              pattern_index, sub.dense_clade_to_chain_ref[dense]);
        }
        scratch.multifurcation_productions_scored = 0;
        return scratch;
      },
      state.chart_opts, run_summaries);
}

std::unique_ptr<chart_spr_local_commit_substrate>
chart_spr_make_local_commit_substrate(chart_spr_search_state const& state,
                                      chart_spr_search_options const& options,
                                      chart_scheduler& scheduler) {
  if (state.chart_opts.score_ua_edge) {
    throw std::runtime_error(
        "chart SPR local commit: score_ua_edge=true is not yet supported in "
        "local-commit mode (the persistent outside cache needs a per-pattern "
        "reference state the search state does not own); use "
        "rebuild_after_accept=true or score_ua_edge=false.  This is a labelled "
        "unsupported-mode throw, not a silent fallback to a cheaper mode.");
  }
  auto const initial_scoring_resident_bytes =
      state.pattern_batch_bootstrap_deferred
          ? std::size_t{0}
          : state.resident_pattern_cache_bytes;
  auto const minimum_projected_resident_bytes =
      chart_spr_checked_cache_bytes_add(
          initial_scoring_resident_bytes,
          chart_spr_minimum_local_commit_cache_bytes(state),
          "chart SPR local-commit cache admission byte overflow");
  chart_spr_require_cache_budget(minimum_projected_resident_bytes,
                                 options.cache, "chart SPR local commit");
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
  chart_scheduler_run_summary inside_run;
  bool inside_was_scheduled = false;
  auto const active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  auto const cache_range_options = chart_spr_phase4_pattern_range_options(
      active_pattern_count, scheduler.worker_resolution().resolved_workers);
  auto const inside_start = std::chrono::steady_clock::now();
  if (state.pattern_batch_bootstrap_deferred) {
    sub->icache = build_inside_chart_cache(
        sub->base_grammar, *sub->checked_base_execution_plan,
        state.active_patterns, state.chart_opts,
        state.invariant_constant_offset, scheduler, cache_range_options,
        &inside_run);
    inside_was_scheduled = true;
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::all_active_patterns) {
    auto const pattern_count = state.active_patterns.patterns.patterns.size();
    if (state.pattern_charts.size() != pattern_count) {
      throw std::runtime_error(
          "chart SPR local commit: resident pattern chart count mismatch");
    }
    sub->icache = build_inside_chart_cache_from_resident_inside(
        sub->base_grammar, *sub->checked_base_execution_plan, state.grammar,
        checked_source, resident_source, state.active_patterns,
        state.chart_opts, state.invariant_constant_offset,
        [&](std::size_t pattern_index, site_pattern const&,
            std::size_t) -> single_site_chart const& {
          return state.pattern_charts.at(pattern_index).chart;
        },
        scheduler, cache_range_options, &inside_run);
    inside_was_scheduled = true;
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
        state.invariant_constant_offset, scheduler, cache_range_options,
        &inside_run);
    inside_was_scheduled = true;
  }
  sub->inside_cache_initialization_ms =
      chart_spr_elapsed_ms(inside_start, std::chrono::steady_clock::now());
  auto const& inside_build = sub->icache->build_stats;
  if (inside_build.inside_charts_built +
          inside_build.resident_inside_charts_consumed !=
      active_pattern_count) {
    throw std::runtime_error(
        "chart SPR local commit: inside-cache build accounting does not match "
        "the active pattern count");
  }

  chart_scheduler_run_summary outside_run;
  auto const outside_start = std::chrono::steady_clock::now();
  sub->ocache = build_outside_chart_cache(
      sub->base_grammar, *sub->checked_base_execution_plan, *sub->icache,
      state.chart_opts, {}, scheduler, cache_range_options, &outside_run);
  sub->outside_cache_initialization_ms =
      chart_spr_elapsed_ms(outside_start, std::chrono::steady_clock::now());
  auto const& outside_build = sub->ocache->build_stats;
  if (outside_build.inside_charts_built != 0 ||
      outside_build.inside_charts_reused != active_pattern_count ||
      outside_build.outside_charts_built != active_pattern_count) {
    throw std::runtime_error(
        "chart SPR local commit: resident-inside outside-cache build violated "
        "the Phase-2B reuse contract");
  }
  sub->resident_cache_bytes =
      chart_spr_local_commit_cache_resident_bytes(*sub->icache, *sub->ocache);
  auto const projected_resident_bytes = chart_spr_checked_cache_bytes_add(
      initial_scoring_resident_bytes, sub->resident_cache_bytes,
      "chart SPR local-commit cache resident byte overflow");
  chart_spr_require_cache_budget(projected_resident_bytes, options.cache,
                                 "chart SPR local commit");
  sub->cache_multifurcation_productions_scored_reported =
      sub->icache->multifurcation_productions_scored +
      sub->ocache->multifurcation_productions_scored;

  // Publish all cache/counter state only after the inside->outside barrier has
  // completed successfully. A failed outside build must not leak a completed
  // inside operation into the search's public accounting.
  state.counters.chart_execution_plan_cache_hits += 2;
  record_inside_chart_cache_build_work(
      state.counters, inside_build.inside_charts_built,
      inside_build.resident_inside_charts_consumed);
  state.counters.outside_cache_inside_charts_built +=
      outside_build.inside_charts_built;
  state.counters.outside_cache_inside_charts_reused +=
      outside_build.inside_charts_reused;
  state.counters.outside_cache_outside_charts_built +=
      outside_build.outside_charts_built;
  if (inside_was_scheduled) {
    record_chart_spr_scheduler_axis_run(
        state.counters.scheduler_axes.inside_cache_patterns, inside_run);
  }
  record_chart_spr_scheduler_axis_run(
      state.counters.scheduler_axes.outside_cache_patterns, outside_run);
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
    lazy_multisite_chart& previous,
    overlay_materialization_result const& materialized,
    std::vector<overlay_clade_ref> const& previous_dense_clade_to_ref,
    chart_cache_commit_plan const& cache_commit_plan) {
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
  next.outside_global_min_by_pattern =
      std::move(previous.outside_global_min_by_pattern);
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

  auto move_slot = [](auto& from, auto& to, clade_id old_dense,
                      clade_id new_dense) {
    if (old_dense < from.size() && new_dense < to.size()) {
      to[new_dense] = std::move(from[old_dense]);
    }
  };

  for (clade_id dense = 0; dense < materialized.dense_clade_to_ref.size();
       ++dense) {
    auto it = previous_dense_by_ref.find(materialized.dense_clade_to_ref[dense]);
    if (it == previous_dense_by_ref.end()) continue;
    auto old_dense = it->second;
    auto ref = materialized.dense_clade_to_ref[dense];
    if (cache_commit_plan.inside_position(ref) ==
        chart_cache_commit_plan::no_position) {
      move_slot(previous.inside_rows_by_clade, next.inside_rows_by_clade,
                old_dense, dense);
      move_slot(previous.class_index_by_pattern_by_clade,
                next.class_index_by_pattern_by_clade, old_dense, dense);
      move_slot(previous.structural_class_index_by_pattern_by_clade,
                next.structural_class_index_by_pattern_by_clade, old_dense,
                dense);
      move_slot(previous.structural_class_count_by_clade,
                next.structural_class_count_by_clade, old_dense, dense);
      move_slot(previous.class_weight_by_clade, next.class_weight_by_clade,
                old_dense, dense);
    }
    if (cache_commit_plan.outside_position(ref) ==
        chart_cache_commit_plan::no_position) {
      move_slot(previous.outside_rows_by_clade, next.outside_rows_by_clade,
                old_dense, dense);
      move_slot(previous.outside_class_index_by_pattern_by_clade,
                next.outside_class_index_by_pattern_by_clade, old_dense,
                dense);
      move_slot(previous.outside_class_weight_by_clade,
                next.outside_class_weight_by_clade, old_dense, dense);
    }
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
    std::vector<overlay_clade_ref> const& previous_dense_clade_to_ref,
    chart_cache_commit_plan const& cache_commit_plan) {
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
      *state.lazy_chart, materialized, previous_dense_clade_to_ref,
      cache_commit_plan);
  auto const& patterns = state.active_patterns.patterns;

  lazy_chart_options lazy_options;
  lazy_options.chart = state.chart_opts;
  lazy_options.chart.keep_trace = false;
  lazy_options.chart.max_trace_choices = 0;
  lazy_options.retain_all_inside_class_maps = true;

  auto inside_multifurcation_before =
      next.multifurcation_productions_scored;
  if (cache_commit_plan.base() != &chain.base() ||
      cache_commit_plan.chain_size() != chain.size()) {
    throw std::runtime_error(
        "chart SPR lazy local commit: stale cache transaction plan");
  }
  auto const& inside_affected = cache_commit_plan.inside_affected();
  std::set<overlay_clade_ref> previously_reachable_refs(
      previous_dense_clade_to_ref.begin(), previous_dense_clade_to_ref.end());
  std::vector<bool> recompute_inside(materialized.grammar.clades.size(), false);
  std::vector<bool> recompute_outside(materialized.grammar.clades.size(),
                                      false);
  // A rebased append can reactivate a frozen or old-temp subtree absent
  // from the prior materialized tip.  Projection has no rows/maps to copy for
  // those clades.  Seed both passes with every newly reachable ref, not merely
  // the delta's ordinary affected closure.
  for (std::size_t dense = 0; dense < materialized.dense_clade_to_ref.size();
       ++dense) {
    if (!previously_reachable_refs.contains(
            materialized.dense_clade_to_ref[dense])) {
      recompute_inside[dense] = true;
      recompute_outside[dense] = true;
    }
  }
  for (auto ref : inside_affected) {
    recompute_inside[chart_spr_detail::dense_clade_id(materialized, ref)] =
        true;
  }
  {
    lazy_chart_detail::plan_parent_key_workspace inside_key_workspace;
    for (auto dense : execution_plan.bottom_up_order()) {
      if (!recompute_inside[dense]) continue;
      chart_spr_clear_lazy_inside_clade(next, dense);
      if (execution_plan.clade(dense).is_leaf()) {
        lazy_chart_detail::assign_plan_leaf_classes(
            next, execution_plan, patterns, dense, lazy_options);
      } else {
        auto keys = lazy_chart_detail::collect_plan_parent_keys(
            next, execution_plan, patterns, dense, inside_key_workspace);
        lazy_chart_detail::assign_plan_internal_classes(
            next, execution_plan, patterns, dense, keys);
      }
      stats.inside_rows_recomputed += next.inside_rows_by_clade[dense].size();
    }
  }
  stats.multifurcation_productions_scored +=
      next.multifurcation_productions_scored -
      inside_multifurcation_before;
  chart_spr_recompute_lazy_inside_summary_counters(next);

  auto outside_multifurcation_before =
      next.outside_multifurcation_productions_scored;
  auto const& outside_affected = cache_commit_plan.outside_affected();
  for (auto ref : outside_affected) {
    recompute_outside[chart_spr_detail::dense_clade_id(materialized, ref)] =
        true;
  }
  {
    lazy_chart_detail::outside_context_key_workspace outside_key_workspace;
    for (auto dense : execution_plan.top_down_order()) {
      if (!recompute_outside[dense]) continue;
      chart_spr_clear_lazy_outside_clade(next, dense);
      if (dense == execution_plan.root_clade()) {
        chart_spr_initialize_lazy_root_outside(
            next, execution_plan, patterns, state.chart_opts);
      } else {
        lazy_chart_detail::assign_outside_classes_for_clade(
            next, execution_plan, patterns, dense, outside_key_workspace);
      }
      stats.outside_rows_recomputed +=
          next.outside_rows_by_clade[dense].size();
    }
  }
  // The tight outside dependency set correctly omits the root when its
  // constant outside row/class map did not change.  The cached global optimum
  // also reads inside[root], however, so refresh that scalar surface after
  // every inside commit independently of root-outside recomputation.
  auto const root = execution_plan.root_clade();
  if (root == no_clade || root >= execution_plan.clades().size()) {
    throw std::runtime_error(
        "chart SPR lazy local commit: root clade out of range while "
        "refreshing global minima");
  }
  next.outside_global_min_by_pattern.assign(next.pattern_count, chart_inf);
  for (std::size_t pattern = 0; pattern < next.pattern_count; ++pattern) {
    auto const& inside_root = next.inside_row(root, pattern);
    auto const& outside_root = next.outside_row(root, pattern);
    chart_cost best = chart_inf;
    for (std::uint8_t state_index = 0; state_index < nuc_state_count;
         ++state_index) {
      best = std::min(best, parsimony_chart_detail::saturated_add(
                                inside_root[state_index],
                                outside_root[state_index]));
    }
    next.outside_global_min_by_pattern[pattern] = best;
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

std::optional<multisite_trim_result>
chart_spr_take_compatible_accepted_exact_trim(
    chart_spr_search_state const& state,
    overlay_materialization_result const& materialized,
    chart_execution_plan const& next_execution_plan,
    chart_spr_candidate_score& accepted,
    chart_spr_search_options const& options,
    chart_spr_search_counters& counters) {
  if (!accepted.reusable_exact_trim) return std::nullopt;

  auto reject = [&]() -> std::optional<multisite_trim_result> {
    ++counters.accepted_exact_trim_reuse_rejections;
    accepted.reusable_exact_trim.reset();
    return std::nullopt;
  };
  if (options.acceptance_mode != chart_spr_acceptance_mode::exact_multisite ||
      !accepted.exact) {
    return reject();
  }

  auto& payload = *accepted.reusable_exact_trim;
  auto const active_fingerprint =
      inside_chart_cache_detail::fingerprint_active_pattern_set(
          state.active_patterns);
  auto const& trim = payload.trim;
  if (payload.target_execution_fingerprint !=
          next_execution_plan.fingerprint() ||
      payload.active_pattern_fingerprint != active_fingerprint ||
      !chart_spr_chart_options_equal(payload.chart_opts, state.chart_opts) ||
      !chart_spr_exact_trim_options_equal(payload.trim_options,
                                          options.exact_trim) ||
      payload.invariant_constant_offset != state.invariant_constant_offset ||
      trim.invariant_constant_offset != 0 ||
      trim.active_pattern_count !=
          state.active_patterns.patterns.patterns.size() ||
      trim.frontier_sizes_by_clade.size() !=
          materialized.grammar.clades.size() ||
      trim.dominance_mode != options.exact_trim.dominance_mode) {
    return reject();
  }
  if (trim.keep_production_exact) {
    if (trim.keep_production.size() !=
        materialized.grammar.productions.size()) {
      return reject();
    }
  } else if (options.exact_trim.require_exact_keep_mask) {
    return reject();
  }
  auto const full_optimum = chart_spr_add_invariant_offset(
      trim.optimum, state, "chart SPR accepted exact-trim reuse");
  if (full_optimum != accepted.exact->value.new_score) return reject();

  std::optional<multisite_trim_result> result;
  result.emplace(std::move(payload.trim));
  accepted.reusable_exact_trim.reset();
  ++counters.accepted_exact_trims_reused;
  return result;
}

// Refresh the derived tip view on the state (grammar + row-view identity +
// composite bounds + size estimates) from the chain + caches, so the next
// iteration's candidate generation and local scoring operate on the chain tip.
// No chart recurrence or row projection runs here; the composite lower bound
// is read from the authoritative persistent inside-cache root rows.
void chart_spr_refresh_state_tip_view_after_local_commit(
    chart_spr_search_state& state,
    overlay_materialization_result const& materialized,
    chart_execution_plan next_execution_plan, inside_chart_cache const& icache,
    outside_chart_cache const& ocache, chart_spr_search_counters& counters,
    std::optional<multisite_trim_result> next_exact_trim) {
  auto old_strategy = state.cache_strategy;
  auto const old_effective_pattern_batch_size =
      state.effective_pattern_batch_size;
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
  auto const persistent_row_view =
      state.local_commit_inside_rows.valid() &&
      old_strategy != chart_spr_cache_strategy::lazy_multisite_chart;
  if (persistent_row_view) {
    // The mandatory persistent cache already supplies every active-pattern
    // row. Preserve the published policy label and batch-size diagnostics, but
    // do not reserve or repilot a second scoring representation.
    state.cache_strategy = old_strategy;
    state.effective_pattern_batch_size = old_effective_pattern_batch_size;
  } else {
    auto cache_selection_options = state.cache_opts;
    // A local commit refreshes the already-published state in place. It must
    // preserve that state's representation without repiloting or consulting an
    // unresolved public auto request.
    cache_selection_options.use_lazy_multisite_chart = false;
    cache_selection_options.lazy_policy = state.lazy_policy.resolved;
    if (state.cache_opts.memory_budget_bytes != 0) {
      auto const mandatory_pair_bytes = chart_spr_checked_cache_bytes_multiply(
          state.estimated_full_pattern_cache_bytes, 2,
          "chart SPR local-commit refreshed mandatory cache byte overflow");
      auto const scoring_pattern_bytes =
          estimate_chart_spr_pattern_entry_cache_bytes(state.grammar);
      auto const mandatory_with_one_pattern = chart_spr_checked_cache_bytes_add(
          mandatory_pair_bytes, scoring_pattern_bytes,
          "chart SPR local-commit refreshed mandatory cache byte overflow");
      if (mandatory_with_one_pattern > state.cache_opts.memory_budget_bytes) {
        throw chart_spr_cache_budget_error(
            "chart SPR local-commit tip refresh: configured cache budget "
            "cannot hold the mandatory full inside/outside caches plus one "
            "scoring pattern");
      }
      cache_selection_options.memory_budget_bytes =
          state.cache_opts.memory_budget_bytes - mandatory_pair_bytes;
    }
    state.effective_pattern_batch_size = choose_chart_spr_pattern_batch_size(
        state.grammar, state.active_patterns, cache_selection_options);
    if (old_strategy == chart_spr_cache_strategy::pattern_batches) {
      // A committed update may shrink the materialized grammar, but growing an
      // automatic batch here would make the pre-commit admission depend on a
      // post-commit topology size. Preserve the established upper bound; a
      // growing grammar may still reduce it under the reserved scoring budget.
      state.effective_pattern_batch_size = std::min(
          old_effective_pattern_batch_size, state.effective_pattern_batch_size);
    }
    auto new_strategy = choose_chart_spr_cache_strategy(
        state.grammar, state.active_patterns, cache_selection_options);
    // Preserve a caller-requested pattern-batch mode across commits: do not
    // silently switch an explicitly batched run into all-active just because
    // the grammar shrank.
    state.cache_strategy =
        old_strategy == chart_spr_cache_strategy::pattern_batches
            ? chart_spr_cache_strategy::pattern_batches
            : new_strategy;
  }

  auto const& patterns = state.active_patterns.patterns.patterns;
  if (patterns.size() != icache.patterns.size()) {
    throw std::runtime_error(
        "chart SPR local-commit tip refresh: active pattern count mismatch");
  }

  if (persistent_row_view) {
    std::vector<pattern_chart_cache_entry>{}.swap(state.pattern_charts);
    state.resident_pattern_cache_bytes = 0;
  } else if (state.cache_strategy ==
             chart_spr_cache_strategy::all_active_patterns) {
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
    // Standalone pattern_batches state: there are no resident base rows to
    // refresh. Local-commit states take the persistent-row-view branch above.
    std::vector<pattern_chart_cache_entry>{}.swap(state.pattern_charts);
    state.resident_pattern_cache_bytes =
        estimate_chart_spr_pattern_batch_cache_bytes(
            state.grammar, state.effective_pattern_batch_size);
  }
  state.local_commit_persistent_cache_bytes =
      chart_spr_local_commit_cache_resident_bytes(icache, ocache);
  state.resident_pattern_cache_bytes = chart_spr_checked_cache_bytes_add(
      state.resident_pattern_cache_bytes,
      state.local_commit_persistent_cache_bytes,
      "chart SPR local-commit refreshed resident byte overflow");
  chart_spr_require_cache_budget(state.resident_pattern_cache_bytes,
                                 state.cache_opts,
                                 "chart SPR local-commit tip refresh");

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

  // A built-in exact verifier has already paid for the accepted grammar's
  // complete frontier.  Publish that result only after its independently
  // materialized structural/options identity passed the compatibility gate;
  // custom or rejected payloads retain the conservative lazy-invalidation
  // behavior.
  state.exact_trim_active_only = std::move(next_exact_trim);
}

// Commit an accepted candidate to the chain + caches and refresh the state's
// tip view.  Returns the outcome (committed, or a labelled tombstone-scope
// skip).  `counters` is the running attempt-counters the caller snapshots from
// state.counters; the cumulative cache counters are mirrored onto it.
chart_spr_local_commit_result chart_spr_commit_accepted_locally(
    chart_spr_local_commit_substrate& sub, chart_spr_search_state& state,
    chart_spr_candidate_score& accepted,
    chart_spr_search_options const& options,
    chart_spr_search_counters& counters, chart_scheduler& scheduler) {
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

  auto const cache_range_options = chart_spr_phase4_pattern_range_options(
      sub.icache->patterns.size(),
      scheduler.worker_resolution().resolved_workers);

  // Admission precedes the first shared mutation. Both persistent caches grow
  // one row per active pattern and added clade; the scoring representation's
  // corresponding worst-case growth and selected-cache admission are included
  // as well. The post-refresh capacity check remains a backstop, not the first
  // time a committed update discovers it exceeded the configured budget.
  std::size_t projected_cache_bytes = 0;
  try {
    projected_cache_bytes = chart_spr_projected_local_commit_bytes_after_delta(
        state, *sub.icache, *sub.ocache, delta);
    projected_cache_bytes = chart_spr_checked_cache_bytes_add(
        projected_cache_bytes,
        chart_spr_cache_commit_transaction_peak_bytes(
            sub, state, delta, scheduler, cache_range_options),
        "chart SPR local-commit transaction peak overflow");
  } catch (chart_spr_cache_budget_error const&) {
    throw;
  } catch (std::exception const& e) {
    throw chart_spr_local_commit_hard_error(
        std::string{"chart SPR local commit: cache admission failed before "
                    "append: "} +
        e.what());
  }
  chart_spr_require_cache_budget(projected_cache_bytes, state.cache_opts,
                                 "chart SPR local-commit accepted update");

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

    // Build one immutable merged-tip transaction plan, then run the paired
    // cache commit as two pattern barriers.  Workers stage only affected rows;
    // neither cache surface nor epoch is published until both joins succeed.
    auto cache_commit_plan = build_chart_cache_commit_plan(
        *sub.chain, outside_affected_policy::three_term_tight);
    chart_cache_commit_run_summary cache_commit_run;
    apply_commit_to_chart_caches(
        *sub.chain, cache_commit_plan, *sub.icache, *sub.ocache, scheduler,
        cache_range_options, &cache_commit_run);

    // Refresh the derived tip view (grammar + pattern_charts + bounds).  This is
    // a grammar-only materialization (no chart rescoring); NOT counted under
    // full_overlay_materializations.  Eliminating it entirely (direct in-place
    // splice) is Phase 6/7 scope; the persistent caches already remove the
    // expensive per-accept chart rescoring.
    auto previous_dense_clade_to_chain_ref =
        std::move(sub.dense_clade_to_chain_ref);
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
    auto next_exact_trim = chart_spr_take_compatible_accepted_exact_trim(
        state, materialized, next_execution_plan, accepted, options, counters);
    // The caller's attempt counter is the authoritative snapshot and is
    // copied back onto state after commit.  Record this distinct materialized
    // grammar plan exactly once here, before any consumer reuses it.
    auto const refreshed_lazy_plan =
        state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart;
    auto lazy_stats = chart_spr_refresh_lazy_chart_after_local_commit(
        state, *sub.chain, materialized, next_execution_plan,
        previous_dense_clade_to_chain_ref, cache_commit_plan);
    if (refreshed_lazy_plan) {
      ++counters.chart_execution_plan_cache_hits;
    }
    ++counters.local_commit_tip_grammar_refreshes;
    chart_spr_refresh_state_tip_view_after_local_commit(
        state, materialized, std::move(next_execution_plan), *sub.icache,
        *sub.ocache, counters, std::move(next_exact_trim));
    chart_spr_set_tip_maps_from_materialization(sub, materialized);
    if (state.exact_trim_active_only) {
      require_chart_spr_retained_exact_state_memory_budget(
          state, *state.exact_trim_active_only, scheduler,
          options.cache.memory_budget_bytes);
    }
    sub.resident_cache_bytes = state.local_commit_persistent_cache_bytes;
    chart_spr_publish_local_commit_tip_identity(sub, state);
    chart_spr_publish_persistent_inside_row_view(state, sub);

    // Mirror cumulative cache counters onto the running attempt-counters (the
    // caches persist across accepts; their counters are cumulative).
    counters.inside_rows_recomputed_on_commit =
        sub.icache->inside_rows_recomputed_on_commit;
    counters.outside_rows_recomputed_on_commit =
        sub.ocache->outside_rows_recomputed_on_commit;
    record_chart_spr_scheduler_axis_run(
        counters.scheduler_axes.inside_cache_patterns,
        cache_commit_run.inside_patterns);
    record_chart_spr_scheduler_axis_run(
        counters.scheduler_axes.outside_cache_patterns,
        cache_commit_run.outside_patterns);
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
  summary.scheduler_axes = counters.scheduler_axes;
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
  summary.lazy_local_admission_waves = counters.lazy_local_admission_waves;
  summary.lazy_local_parallel_waves = counters.lazy_local_parallel_waves;
  summary.lazy_local_memory_limited_waves =
      counters.lazy_local_memory_limited_waves;
  summary.lazy_local_requested_concurrency_max =
      counters.lazy_local_requested_concurrency_max;
  summary.lazy_local_effective_concurrency_max =
      counters.lazy_local_effective_concurrency_max;
  summary.lazy_local_bandwidth_capped_batches =
      counters.lazy_local_bandwidth_capped_batches;
  summary.lazy_local_admitted_concurrency_max =
      counters.lazy_local_admitted_concurrency_max;
  summary.lazy_local_prepared_tasks = counters.lazy_local_prepared_tasks;
  summary.lazy_local_reused_prepared_tasks =
      counters.lazy_local_reused_prepared_tasks;
  summary.lazy_local_pre_submit_budget_failures =
      counters.lazy_local_pre_submit_budget_failures;
  summary.lazy_local_peak_admitted_bytes =
      counters.lazy_local_peak_admitted_bytes;
  summary.lazy_local_peak_projected_resident_bytes =
      counters.lazy_local_peak_projected_resident_bytes;
  summary.lazy_local_preparation_peak_bytes =
      counters.lazy_local_preparation_peak_bytes;
  summary.lazy_local_result_output_resident_bytes_max =
      counters.lazy_local_result_output_resident_bytes_max;
  summary.lazy_local_retained_exact_trim_bytes_max =
      counters.lazy_local_retained_exact_trim_bytes_max;
  summary.lazy_local_canonical_exact_evidence_resident_bytes_max =
      counters.lazy_local_canonical_exact_evidence_resident_bytes_max;
  summary.lazy_local_canonical_exact_evidence_construction_peak_bytes_max =
      counters
          .lazy_local_canonical_exact_evidence_construction_peak_bytes_max;
  summary.lazy_local_runtime_transient_reservation_bytes_max =
      counters.lazy_local_runtime_transient_reservation_bytes_max;
  summary.lazy_local_iteration_envelope_bytes_max =
      counters.lazy_local_iteration_envelope_bytes_max;
  summary.lazy_local_iteration_generation_phase_bytes_max =
      counters.lazy_local_iteration_generation_phase_bytes_max;
  summary.lazy_local_iteration_evidence_phase_bytes_max =
      counters.lazy_local_iteration_evidence_phase_bytes_max;
  summary.lazy_local_ranked_candidate_exact_evidence_bytes_max =
      counters.lazy_local_ranked_candidate_exact_evidence_bytes_max;
  summary.lazy_local_iteration_task_stable_bytes_max =
      counters.lazy_local_iteration_task_stable_bytes_max;
  summary.lazy_local_iteration_task_preparation_peak_bytes_max =
      counters.lazy_local_iteration_task_preparation_peak_bytes_max;
  summary.candidate_batches_scored = counters.candidate_batches_scored;
  summary.candidate_pipeline_batches_generated =
      counters.candidate_pipeline_batches_generated;
  summary.candidate_pipeline_batches_scored =
      counters.candidate_pipeline_batches_scored;
  summary.candidate_pipeline_serial_overlap_batches =
      counters.candidate_pipeline_serial_overlap_batches;
  summary.candidate_pipeline_scheduler_projection_overlap_batches =
      counters.candidate_pipeline_scheduler_projection_overlap_batches;
  summary.candidate_pipeline_producer_stalls =
      counters.candidate_pipeline_producer_stalls;
  summary.candidate_pipeline_consumer_stalls =
      counters.candidate_pipeline_consumer_stalls;
  summary.candidate_pipeline_producer_stall_ms =
      static_cast<double>(
          counters.candidate_pipeline_producer_stall_nanoseconds) /
      1'000'000.0;
  summary.candidate_pipeline_consumer_stall_ms =
      static_cast<double>(
          counters.candidate_pipeline_consumer_stall_nanoseconds) /
      1'000'000.0;
  summary.candidate_pipeline_cancellations =
      counters.candidate_pipeline_cancellations;
  summary.candidate_pipeline_stale_batches_discarded =
      counters.candidate_pipeline_stale_batches_discarded;
  summary.candidate_pipeline_stale_candidates_discarded =
      counters.candidate_pipeline_stale_candidates_discarded;
  summary.candidate_pipeline_state_epoch_rejections =
      counters.candidate_pipeline_state_epoch_rejections;
  summary.candidate_pipeline_generation_errors =
      counters.candidate_pipeline_generation_errors;
  summary.candidate_pipeline_estimated_peak_bytes =
      counters.candidate_pipeline_estimated_peak_bytes;
  summary.pattern_batch_cache_builds = counters.pattern_batch_cache_builds;
  summary.local_commit_inside_row_view_pattern_visits =
      counters.local_commit_inside_row_view_pattern_visits;
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
  summary.exact_bnb_levels = counters.exact_bnb_levels;
  summary.exact_bnb_clades = counters.exact_bnb_clades;
  summary.exact_bnb_product_combinations =
      counters.exact_bnb_product_combinations;
  summary.exact_bnb_frontier_entries = counters.exact_bnb_frontier_entries;
  summary.exact_bnb_ms = counters.exact_bnb_ms;
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
  summary.accepted_exact_trims_reused =
      counters.accepted_exact_trims_reused;
  summary.accepted_exact_trim_reuse_rejections =
      counters.accepted_exact_trim_reuse_rejections;
  summary.exact_candidate_admission_batches =
      counters.exact_candidate_admission_batches;
  summary.exact_candidate_parallel_batches =
      counters.exact_candidate_parallel_batches;
  summary.exact_candidate_inner_parallel_batches =
      counters.exact_candidate_inner_parallel_batches;
  summary.exact_candidate_memory_limited_batches =
      counters.exact_candidate_memory_limited_batches;
  summary.exact_candidate_peak_admitted_bytes =
      counters.exact_candidate_peak_admitted_bytes;
  summary.exact_candidate_peak_projected_resident_bytes =
      counters.exact_candidate_peak_projected_resident_bytes;
  summary.exact_candidate_queued_for_memory_ms =
      counters.exact_candidate_queued_for_memory_ms;
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
  summary.lazy_chart_memory_budget_bytes =
      counters.lazy_chart_memory_budget_bytes;
  summary.lazy_chart_inside_max_admitted_slots =
      counters.lazy_chart_inside_max_admitted_slots;
  summary.lazy_chart_outside_max_admitted_slots =
      counters.lazy_chart_outside_max_admitted_slots;
  summary.lazy_chart_inside_admission_waves =
      counters.lazy_chart_inside_admission_waves;
  summary.lazy_chart_outside_admission_waves =
      counters.lazy_chart_outside_admission_waves;
  summary.lazy_chart_inside_memory_limited_levels =
      counters.lazy_chart_inside_memory_limited_levels;
  summary.lazy_chart_outside_memory_limited_levels =
      counters.lazy_chart_outside_memory_limited_levels;
  summary.lazy_chart_inside_reused_slot_waves =
      counters.lazy_chart_inside_reused_slot_waves;
  summary.lazy_chart_outside_reused_slot_waves =
      counters.lazy_chart_outside_reused_slot_waves;
  summary.lazy_chart_inside_workspace_evictions =
      counters.lazy_chart_inside_workspace_evictions;
  summary.lazy_chart_outside_workspace_evictions =
      counters.lazy_chart_outside_workspace_evictions;
  summary.lazy_chart_inside_dependency_ready_executions =
      counters.lazy_chart_inside_dependency_ready_executions;
  summary.lazy_chart_outside_dependency_ready_executions =
      counters.lazy_chart_outside_dependency_ready_executions;
  summary.lazy_chart_inside_dependency_ready_jobs =
      counters.lazy_chart_inside_dependency_ready_jobs;
  summary.lazy_chart_outside_dependency_ready_jobs =
      counters.lazy_chart_outside_dependency_ready_jobs;
  summary.lazy_chart_inside_dependency_ready_scheduler_operations =
      counters.lazy_chart_inside_dependency_ready_scheduler_operations;
  summary.lazy_chart_outside_dependency_ready_scheduler_operations =
      counters.lazy_chart_outside_dependency_ready_scheduler_operations;
  summary.lazy_chart_inside_dependency_ready_capacity_resident_bytes_max =
      counters
          .lazy_chart_inside_dependency_ready_capacity_resident_bytes_max;
  summary.lazy_chart_outside_dependency_ready_capacity_resident_bytes_max =
      counters
          .lazy_chart_outside_dependency_ready_capacity_resident_bytes_max;
  summary.lazy_chart_preflight_peak_bytes =
      counters.lazy_chart_preflight_peak_bytes;
  summary.lazy_chart_actual_peak_bytes = counters.lazy_chart_actual_peak_bytes;
  summary.lazy_chart_pre_submit_rejections =
      counters.lazy_chart_pre_submit_rejections;
  summary.lazy_policy_pilot_runs = counters.lazy_policy_pilot_runs;
  summary.lazy_policy_frozen_reuses = counters.lazy_policy_frozen_reuses;
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

chart_spr_search_state
chart_spr_search_detail::rebuild_chart_spr_search_state_after_accept_for_tests(
    chart_spr_search_state const& previous_state, phylo_dag& rebuilt_dag,
    clade_grammar rebuilt_grammar, chart_spr_search_options const& options,
    chart_scheduler& scheduler) {
  bool reused_patterns = false;
  return rebuild_chart_spr_search_state_after_accept(
      previous_state, rebuilt_dag, std::move(rebuilt_grammar), options,
      reused_patterns, scheduler);
}

namespace chart_spr_search_detail {

std::size_t effective_lazy_local_admission_budget_bytes(
    chart_spr_search_state const& state,
    local_spr_score_options const& options) noexcept {
  auto budget = options.admission_memory_budget_bytes;
  if (state.cache_opts.memory_budget_bytes != 0) {
    budget = budget == 0
                 ? state.cache_opts.memory_budget_bytes
                 : std::min(budget, state.cache_opts.memory_budget_bytes);
  }
  return budget;
}

namespace {

struct lazy_local_candidate_preflight {
  std::size_t stable_dynamic_capacity_bytes = 0;
  std::size_t preparation_peak_dynamic_capacity_bytes = 0;
  std::size_t descriptor_stable_dynamic_capacity_bytes = 0;
  std::size_t descriptor_preparation_peak_dynamic_capacity_bytes = 0;
  std::size_t worker_stable_dynamic_capacity_bytes = 0;
  std::size_t worker_preparation_peak_dynamic_capacity_bytes = 0;
};

// Allocation-free upper envelope from a grammar-native candidate's structural
// shape. A factor of two covers frozen libstdc++ vector growth for push-built
// flat/nested arrays; the preparation envelope additionally covers old+new
// reserve publication and the explicit packed-grouping staging reports.
lazy_local_candidate_preflight estimate_lazy_local_candidate_shape_preflight(
    chart_spr_search_state const& state, std::size_t temp_clades,
    std::size_t temp_productions, std::size_t removed_productions,
    std::size_t temp_children, std::size_t candidate_owned_dynamic_bytes) {
  auto add = [](std::size_t lhs, std::size_t rhs, std::string_view context) {
    return local_capacity_checked_add(lhs, rhs, context);
  };
  auto multiply = [](std::size_t lhs, std::size_t rhs,
                     std::string_view context) {
    return local_capacity_checked_multiply(lhs, rhs, context);
  };
  auto vector_growth_bytes = [&](std::size_t count, std::size_t width,
                                 std::string_view context) {
    return multiply(2, multiply(count, width, context), context);
  };

  auto const base_clades = state.grammar.clades.size();
  auto const total_clades = add(base_clades, temp_clades,
                                "chart SPR lazy-local preflight clade count");
  auto const base_productions = state.grammar.productions.size();
  auto const total_productions =
      add(base_productions, temp_productions,
          "chart SPR lazy-local preflight production count");

  std::size_t base_children = 0;
  for (auto const& production : state.grammar.productions) {
    base_children = add(base_children, production.children.size(),
                        "chart SPR lazy-local preflight base children");
  }
  auto const total_children = add(base_children, temp_children,
                                  "chart SPR lazy-local preflight child count");

  // Candidate payload copied into the descriptor, including every nested
  // clade/production/witness vector. The source capacities dominate copied
  // logical sizes; doubling covers destination outer-vector growth.
  auto descriptor =
      multiply(2, candidate_owned_dynamic_bytes,
               "chart SPR lazy-local preflight copied candidate payload");

  auto add_descriptor_vector = [&](std::size_t count, std::size_t width,
                                   std::string_view context) {
    descriptor =
        add(descriptor, vector_growth_bytes(count, width, context), context);
  };
  add_descriptor_vector(removed_productions, sizeof(production_id),
                        "chart SPR lazy-local preflight removed productions");
  add_descriptor_vector(total_clades, sizeof(overlay_clade_ref),
                        "chart SPR lazy-local preflight affected order");
  // local_owned_dynamic_capacity_bytes(vector<bool>) intentionally measures
  // its bit-capacity as bytes. Allow one allocator word of rounding per bool
  // vector before applying the normal growth factor.
  auto const rounded_base_bits =
      add(base_clades, 64, "chart SPR lazy-local preflight base flags");
  auto const rounded_temp_bits =
      add(temp_clades, 64, "chart SPR lazy-local preflight temp flags");
  auto const rounded_production_bits = add(
      base_productions, 64, "chart SPR lazy-local preflight production flags");
  add_descriptor_vector(multiply(2, rounded_base_bits,
                                 "chart SPR lazy-local preflight base flags"),
                        sizeof(bool),
                        "chart SPR lazy-local preflight base flags");
  add_descriptor_vector(multiply(2, rounded_temp_bits,
                                 "chart SPR lazy-local preflight temp flags"),
                        sizeof(bool),
                        "chart SPR lazy-local preflight temp flags");
  add_descriptor_vector(rounded_production_bits, sizeof(bool),
                        "chart SPR lazy-local preflight production flags");
  add_descriptor_vector(base_clades, sizeof(std::size_t),
                        "chart SPR lazy-local preflight base row slots");
  add_descriptor_vector(temp_clades, sizeof(std::size_t),
                        "chart SPR lazy-local preflight temp row slots");
  add_descriptor_vector(total_clades, sizeof(candidate_chart_row_descriptor),
                        "chart SPR lazy-local preflight compiled rows");
  add_descriptor_vector(total_productions,
                        sizeof(candidate_chart_production_descriptor),
                        "chart SPR lazy-local preflight compiled productions");
  add_descriptor_vector(total_children,
                        sizeof(candidate_chart_child_descriptor),
                        "chart SPR lazy-local preflight compiled children");

  // Four temporary-production indices own two base-sized and two temp-sized
  // outer arrays. Across each parent/child pair a temporary production id is
  // stored exactly once per parent and once per child.
  auto const index_outer_rows =
      multiply(2, total_clades, "chart SPR lazy-local preflight index rows");
  add_descriptor_vector(index_outer_rows, sizeof(std::vector<production_id>),
                        "chart SPR lazy-local preflight index rows");
  auto const index_entries =
      add(temp_productions, temp_children,
          "chart SPR lazy-local preflight index entries");
  add_descriptor_vector(index_entries, sizeof(production_id),
                        "chart SPR lazy-local preflight index entries");

  // Serial builder traversal buffers. The reachability stack can retain all
  // pending child occurrences, while the affected queue contains at most one
  // entry per clade.
  add_descriptor_vector(total_children, sizeof(overlay_clade_ref),
                        "chart SPR lazy-local preflight reachability stack");
  add_descriptor_vector(total_clades, sizeof(overlay_clade_ref),
                        "chart SPR lazy-local preflight affected queue");
  descriptor = add(descriptor, lazy_local_invalid_reason_owned_capacity_bound(),
                   "chart SPR lazy-local preflight invalid-reason capacity");

  auto const patterns = state.active_patterns.patterns.patterns.size();
  auto const key_components =
      add(total_clades, total_children,
          "chart SPR lazy-local preflight key components");
  auto const key_width = add(
      1,
      multiply(2, key_components, "chart SPR lazy-local preflight key width"),
      "chart SPR lazy-local preflight key width");
  auto const grouping =
      lazy_key_grouping_detail::estimate_packed_key_grouping_memory(patterns,
                                                                    key_width);
  auto scratch = multiply(2, grouping.worst_case_logical_total_resident_bytes,
                          "chart SPR lazy-local preflight grouping capacity");
  scratch = add(
      scratch,
      vector_growth_bytes(
          key_width, sizeof(lazy_overlay_context_key_component),
          "chart SPR lazy-local preflight context-key components"),
      "chart SPR lazy-local preflight context-key components");
  scratch = add(
      scratch,
      vector_growth_bytes(patterns, sizeof(lazy_overlay_context_accumulator),
                          "chart SPR lazy-local preflight contexts"),
      "chart SPR lazy-local preflight contexts");
  scratch =
      add(scratch,
          vector_growth_bytes(total_clades,
                              sizeof(std::array<chart_cost, nuc_state_count>),
                              "chart SPR lazy-local preflight rows"),
          "chart SPR lazy-local preflight rows");

  auto const worker_stable =
      add(scratch, lazy_local_worker_error_transient_capacity_bound(),
          "chart SPR lazy-local worker error-path transient");
  auto stable = add(descriptor, worker_stable,
                    "chart SPR lazy-local preflight stable capacity");
  auto const descriptor_preparation = multiply(
      2, descriptor, "chart SPR lazy-local preflight descriptor preparation");
  auto const worker_preparation =
      add(multiply(4, scratch,
                   "chart SPR lazy-local preflight scratch preparation"),
          lazy_local_worker_error_transient_capacity_bound(),
          "chart SPR lazy-local preparation error transient");
  auto const preparation =
      add(descriptor_preparation, worker_preparation,
          "chart SPR lazy-local preflight preparation peak");
  return lazy_local_candidate_preflight{
      .stable_dynamic_capacity_bytes = stable,
      .preparation_peak_dynamic_capacity_bytes = preparation,
      .descriptor_stable_dynamic_capacity_bytes = descriptor,
      .descriptor_preparation_peak_dynamic_capacity_bytes =
          descriptor_preparation,
      .worker_stable_dynamic_capacity_bytes = worker_stable,
      .worker_preparation_peak_dynamic_capacity_bytes = worker_preparation,
  };
}

lazy_local_candidate_preflight estimate_lazy_local_candidate_preflight(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    local_spr_score_options const& options) {
  if (options.verify_against_full_overlay) {
    // The full-materialization oracle is test-only and has a separate dense
    // grammar/chart allocation graph. It is deliberately unsupported under a
    // finite local admission budget rather than guessed at here.
    return lazy_local_candidate_preflight{
        .stable_dynamic_capacity_bytes =
            (std::numeric_limits<std::size_t>::max)(),
        .preparation_peak_dynamic_capacity_bytes =
            (std::numeric_limits<std::size_t>::max)(),
    };
  }
  std::size_t temp_children = 0;
  for (auto const& production : candidate.added_productions) {
    temp_children = local_capacity_checked_add(
        temp_children, production.children.size(),
        "chart SPR lazy-local preflight temporary children");
  }
  return estimate_lazy_local_candidate_shape_preflight(
      state, candidate.added_clades.size(), candidate.added_productions.size(),
      candidate.removed_productions.size(), temp_children,
      local_owned_dynamic_capacity_bytes(candidate));
}

// Own the public accounting and retained-task cleanup for the entire lazy
// wave operation. Worker counters are drained before a slot is reused and
// zeroed immediately, so this final sweep folds only payload that survived an
// exceptional exit. Finite operations also shed every task high-water on all
// exits; unlimited operations retain their historical reuse behavior.
class lazy_local_wave_operation_guard {
 public:
  lazy_local_wave_operation_guard(
      chart_spr_search_state const& state,
      chart_spr_local_score_workspace& workspace, std::size_t task_count,
      bool release_retained_storage,
      chart_spr_search_counters& aggregate,
      lazy_local_admission_test_observer* observer,
      bool verify_capacity_ledger) noexcept
      : state_(state),
        workspace_(workspace),
        task_count_(task_count),
        release_retained_storage_(release_retained_storage),
        aggregate_(aggregate),
        observer_(observer),
        verify_capacity_ledger_(verify_capacity_ledger) {}

  lazy_local_wave_operation_guard(lazy_local_wave_operation_guard const&) =
      delete;
  lazy_local_wave_operation_guard& operator=(
      lazy_local_wave_operation_guard const&) = delete;

  ~lazy_local_wave_operation_guard() noexcept {
    for (std::size_t slot = 0; slot < task_count_; ++slot) {
      fold_worker(slot);
      if (release_retained_storage_) {
        local_score_workspace_access::release_task_retained_storage(workspace_,
                                                                    slot);
        try {
          (void)local_score_workspace_access::
              refresh_task_capacity_ledger_slot(workspace_, slot);
          if (observer_ != nullptr) {
            ++observer_->task_capacity_ledger_slot_refreshes;
          }
        } catch (...) {
          std::terminate();
        }
      }
    }
    verify_ledger_noexcept();
    add_chart_spr_search_counters(state_.counters, aggregate_);
  }

  void fold_worker(std::size_t slot) noexcept {
    auto& counters =
        local_score_workspace_access::worker(workspace_, slot).counters;
    add_chart_spr_search_counters(aggregate_, counters);
    counters = {};
  }

  void release_slot(std::size_t slot) {
    fold_worker(slot);
    local_score_workspace_access::release_task_retained_storage(workspace_,
                                                                slot);
    (void)local_score_workspace_access::refresh_task_capacity_ledger_slot(
        workspace_, slot);
    if (observer_ != nullptr) {
      ++observer_->task_capacity_ledger_slot_refreshes;
    }
    verify_ledger();
  }

  void release_slots(std::size_t count) {
    for (std::size_t slot = 0; slot < count; ++slot) release_slot(slot);
  }

  void keep_retained_storage_on_success() noexcept {
    release_retained_storage_ = false;
  }

  std::size_t refresh_slot(std::size_t slot,
                           std::size_t* worker_capacity_bytes = nullptr) {
    auto const capacity =
        local_score_workspace_access::refresh_task_capacity_ledger_slot(
            workspace_, slot, worker_capacity_bytes);
    if (observer_ != nullptr) {
      ++observer_->task_capacity_ledger_slot_refreshes;
    }
    return capacity;
  }

  void verify_ledger() {
    if (!verify_capacity_ledger_) return;
    if (observer_ != nullptr) {
      ++observer_->task_capacity_ledger_full_walk_verifications;
    }
    if (!local_score_workspace_access::verify_task_capacity_ledger(
            workspace_, task_count_)) {
      if (observer_ != nullptr) {
        ++observer_->task_capacity_ledger_mismatches;
      }
      throw std::logic_error(
          "chart SPR lazy-local capacity ledger mismatch");
    }
  }

 private:
  void verify_ledger_noexcept() noexcept {
    if (!verify_capacity_ledger_) return;
    if (observer_ != nullptr) {
      ++observer_->task_capacity_ledger_full_walk_verifications;
    }
    try {
      if (local_score_workspace_access::verify_task_capacity_ledger(
              workspace_, task_count_)) {
        return;
      }
    } catch (...) {
    }
    if (observer_ != nullptr) {
      ++observer_->task_capacity_ledger_mismatches;
    }
  }

  chart_spr_search_state const& state_;
  chart_spr_local_score_workspace& workspace_;
  std::size_t task_count_;
  bool release_retained_storage_;
  chart_spr_search_counters& aggregate_;
  lazy_local_admission_test_observer* observer_;
  bool verify_capacity_ledger_;
};

}  // namespace

void score_candidates_locally_lazy_waves_into(
    chart_spr_search_state const& state,
    std::span<grammar_spr_candidate const> candidates,
    std::span<chart_spr_local_score_result> results,
    chart_spr_local_score_workspace& workspace,
    local_spr_score_options const& options, chart_scheduler* scheduler,
    std::size_t task_limit,
    checked_chart_execution_plan_ref const& checked_state) {
  chart_spr_search_counters aggregate;
  if (candidates.empty()) return;
  ++aggregate.candidate_batches_scored;
  auto const resolved_workers =
      scheduler == nullptr
          ? std::size_t{1}
          : scheduler->worker_resolution().resolved_workers;
  auto const concurrency = plan_lazy_local_candidate_concurrency(
      resolved_workers, candidates.size(),
      state.active_patterns.patterns.patterns.size());
  if (task_limit != concurrency.effective_tasks) {
    throw std::logic_error(
        "chart SPR lazy-local runtime concurrency diverged from admission "
        "plan");
  }
  task_limit =
      std::max<std::size_t>(1, std::min(task_limit, candidates.size()));

  auto const budget =
      effective_lazy_local_admission_budget_bytes(state, options);
  auto const enforce_budget = budget != 0;
  auto const retain_finite_task_storage =
      enforce_budget && options.admission_retain_lazy_local_task_storage;
  local_score_workspace_access::initialize_task_capacity_ledger(workspace,
                                                                task_limit);
  if (options.lazy_local_admission_observer_for_tests != nullptr) {
    ++options.lazy_local_admission_observer_for_tests
          ->task_capacity_ledger_initializations;
  }
  lazy_local_wave_operation_guard operation_guard{
      state,
      workspace,
      task_limit,
      enforce_budget,
      aggregate,
      options.lazy_local_admission_observer_for_tests,
      options.verify_lazy_local_task_capacity_ledger_for_tests};
  operation_guard.verify_ledger();
  auto const published_state_resident =
      options.admission_published_state_resident_bytes
          ? *options.admission_published_state_resident_bytes
          : estimate_chart_spr_published_state_resident_bytes(state);
  auto resident_base = local_capacity_checked_add(
      published_state_resident,
      options.admission_additional_resident_bytes,
      "chart SPR lazy-local shared resident capacity");
  resident_base = local_capacity_checked_add(
      resident_base,
      local_score_workspace_access::fixed_resident_capacity_bytes(workspace),
      "chart SPR lazy-local workspace resident capacity");
  auto const range_options = chart_indexed_range_options{
      .minimum_grain = 1, .target_ranges_per_worker = 1};
  auto planned_scheduler_operation_peak = [&](std::size_t item_count) {
    if (scheduler == nullptr) return std::size_t{0};
    return estimate_chart_scheduler_operation_peak_bytes(
        scheduler->plan_indexed_ranges(item_count, range_options));
  };

  auto fail_budget = [&](std::size_t candidate, std::size_t required,
                         std::size_t available) -> void {
    ++aggregate.lazy_local_pre_submit_budget_failures;
    throw chart_spr_lazy_local_budget_error(candidate, required, available);
  };
  if (enforce_budget && resident_base > budget) {
    fail_budget(0, resident_base, budget);
  }

  auto slots_dynamic_capacity = [&]() {
    return local_score_workspace_access::task_capacity_ledger_total(workspace);
  };
  auto runtime_transient_reservation = [](std::size_t count) {
    return local_capacity_checked_multiply(
        count, lazy_local_worker_error_transient_capacity_bound(),
        "chart SPR lazy-local simultaneous worker transient reservation");
  };

  for (std::size_t begin = 0; begin < candidates.size();) {
    std::size_t admitted = 0;
    std::size_t admitted_dynamic = 0;
    bool memory_limited = false;

    while (admitted < task_limit && begin + admitted < candidates.size()) {
      auto const candidate_index = begin + admitted;
      auto const prospective_scheduler_peak =
          planned_scheduler_operation_peak(admitted + 1);
      lazy_local_candidate_preflight preflight{
          .stable_dynamic_capacity_bytes =
              (std::numeric_limits<std::size_t>::max)(),
          .preparation_peak_dynamic_capacity_bytes =
              (std::numeric_limits<std::size_t>::max)(),
      };
      if (enforce_budget) {
        preflight = estimate_lazy_local_candidate_preflight(
            state, candidates[candidate_index], options);
      }

      struct candidate_preflight_fit {
        bool preparation = true;
        bool stable = true;
        std::size_t preparation_available =
            (std::numeric_limits<std::size_t>::max)();
        std::size_t stable_available =
            (std::numeric_limits<std::size_t>::max)();
      };
      auto check_preflight = [&]() {
        candidate_preflight_fit fit;
        if (!enforce_budget) return fit;
        auto const slots_dynamic = slots_dynamic_capacity();
        auto const old_slot =
            local_score_workspace_access::task_capacity_ledger_slot(workspace,
                                                                    admitted);
        auto const other_slots = slots_dynamic - old_slot;
        auto const preparation_base = local_capacity_checked_add(
            resident_base, slots_dynamic,
            "chart SPR lazy-local retained preparation base");
        fit.preparation_available =
            preparation_base <= budget ? budget - preparation_base : 0;
        fit.preparation = preparation_base <= budget &&
                          preflight.preparation_peak_dynamic_capacity_bytes <=
                              fit.preparation_available;

        auto stable_base = local_capacity_checked_add(
            resident_base, other_slots,
            "chart SPR lazy-local retained stable base");
        stable_base = local_capacity_checked_add(
            stable_base, runtime_transient_reservation(admitted),
            "chart SPR lazy-local prior worker transient reservations");
        stable_base = local_capacity_checked_add(
            stable_base, prospective_scheduler_peak,
            "chart SPR lazy-local planned scheduler operation");
        fit.stable_available = stable_base <= budget ? budget - stable_base : 0;
        auto const old_slot_runtime = local_capacity_checked_add(
            old_slot, runtime_transient_reservation(1),
            "chart SPR lazy-local retained task runtime reservation");
        fit.stable =
            stable_base <= budget &&
            std::max(old_slot_runtime,
                     preflight.stable_dynamic_capacity_bytes) <=
                fit.stable_available;
        return fit;
      };

      auto preflight_fit = check_preflight();
      if (enforce_budget &&
          (!preflight_fit.preparation || !preflight_fit.stable)) {
        // Retain already admitted slots, but shed the current and other
        // not-yet-admitted high-water slots before falling back to a cold
        // candidate preparation. This makes reuse the ordinary large-budget
        // path without allowing stale capacity to squeeze a later wave.
        for (std::size_t slot = admitted; slot < task_limit; ++slot) {
          operation_guard.release_slot(slot);
        }
        preflight_fit = check_preflight();
      }
      if (enforce_budget &&
          (!preflight_fit.preparation || !preflight_fit.stable)) {
        if (admitted == 0) {
          if (!preflight_fit.preparation) {
            fail_budget(candidate_index,
                        preflight.preparation_peak_dynamic_capacity_bytes,
                        preflight_fit.preparation_available);
          }
          fail_budget(candidate_index, preflight.stable_dynamic_capacity_bytes,
                      preflight_fit.stable_available);
        }
        memory_limited = true;
        break;
      }

      auto& prepared =
          local_score_workspace_access::prepared(workspace, admitted);
      auto& worker = local_score_workspace_access::worker(workspace, admitted);
      prepared.reset_for_prepare();
      worker.reset_for_operation();
      auto const prepare_start = std::chrono::steady_clock::now();
      auto const before_dynamic =
          local_score_workspace_access::task_capacity_ledger_slot(workspace,
                                                                  admitted);
      auto const before_slots_dynamic = slots_dynamic_capacity();
      auto const reused_prepared_storage =
          worker.lazy_local_task_retained_payload &&
          (begin != 0 || retain_finite_task_storage);
      lazy_local_scratch_preparation_report scratch_report;
      try {
        if (enforce_budget) prepared.reserve_finite_invalid_reason();
        prepare_local_candidate_score_into(state, candidates[candidate_index],
                                           options, &aggregate, checked_state,
                                           prepared);
        if (prepared.valid_for_accumulation) {
          try {
            scratch_report = prepare_lazy_local_score_scratch_for_candidate(
                state, prepared.delta(), worker.scratch, &aggregate);
          } catch (chart_spr_lazy_local_budget_error const&) {
            throw;
          } catch (chart_spr_lazy_local_enumeration_budget_error const&) {
            throw;
          } catch (chart_spr_exact_candidate_budget_error const&) {
            throw;
          } catch (chart_spr_exact_state_budget_error const&) {
            throw;
          } catch (std::bad_alloc const&) {
            throw;
          } catch (std::overflow_error const&) {
            throw;
          } catch (std::length_error const&) {
            throw;
          }
        }
      } catch (...) {
        throw;
      }
      prepared.result.local_score_ms =
          chart_spr_elapsed_ms(prepare_start, std::chrono::steady_clock::now());
      std::size_t worker_dynamic = 0;
      auto const task_dynamic =
          operation_guard.refresh_slot(admitted, &worker_dynamic);
      operation_guard.verify_ledger();
      auto prep_peak = local_capacity_checked_add(
          before_dynamic, task_dynamic,
          "chart SPR lazy-local descriptor preparation peak");
      if (scratch_report.observed_prepublication_peak_dynamic_capacity_bytes !=
          0) {
        prep_peak = std::max(
            prep_peak,
            local_capacity_checked_add(
                task_dynamic - worker_dynamic,
                scratch_report
                    .observed_prepublication_peak_dynamic_capacity_bytes,
                "chart SPR lazy-local scratch preparation peak"));
      }
      auto const projected_prep_peak = local_capacity_checked_add(
          resident_base,
          local_capacity_checked_add(
              before_slots_dynamic - before_dynamic, prep_peak,
              "chart SPR lazy-local wave preparation peak"),
          "chart SPR lazy-local projected preparation peak");
      aggregate.lazy_local_preparation_peak_bytes = std::max(
          aggregate.lazy_local_preparation_peak_bytes, projected_prep_peak);

      bool fits = true;
      std::size_t projected_stable = 0;
      if (enforce_budget) {
        // Measured capacities are the post-allocation enforcement backstop for
        // the conservative preflight. Check both stable admission and the
        // observed old+new preparation high-water before publishing the task.
        // Also fail closed if a frozen-toolchain assumption in the preflight
        // ever underestimates an observed capacity despite a looser budget.
        auto const after_slots_dynamic = slots_dynamic_capacity();
        projected_stable = local_capacity_checked_add(
            local_capacity_checked_add(
                resident_base,
                local_capacity_checked_add(
                    after_slots_dynamic,
                    runtime_transient_reservation(admitted + 1),
                    "chart SPR lazy-local measured worker transients"),
                "chart SPR lazy-local measured stable resident"),
            prospective_scheduler_peak,
            "chart SPR lazy-local measured operation resident");
        auto const stable_assumption = std::max(
            local_capacity_checked_add(
                before_dynamic, runtime_transient_reservation(1),
                "chart SPR lazy-local retained stable assumption"),
            preflight.stable_dynamic_capacity_bytes);
        auto const measured_task_runtime = local_capacity_checked_add(
            task_dynamic, runtime_transient_reservation(1),
            "chart SPR lazy-local measured task runtime reservation");
        auto const preparation_assumption = local_capacity_checked_add(
            before_dynamic, preflight.preparation_peak_dynamic_capacity_bytes,
            "chart SPR lazy-local preparation assumption");
        fits = projected_prep_peak <= budget && projected_stable <= budget &&
               measured_task_runtime <= stable_assumption &&
               prep_peak <= preparation_assumption;
      }
      if (!fits) {
        // This is not an ordinary memory-limited prefix: allocation has
        // already demonstrated that a frozen preflight assumption was wrong.
        // Preserve the hard integrity failure and leave no retained task
        // payload live at the public boundary.
        fail_budget(candidate_index,
                    std::max(projected_prep_peak, projected_stable), budget);
      }
      worker.lazy_local_task_retained_payload = true;
      admitted_dynamic = local_capacity_checked_add(
          admitted_dynamic,
          local_capacity_checked_add(
              task_dynamic, runtime_transient_reservation(1),
              "chart SPR lazy-local admitted task and transient"),
          "chart SPR lazy-local admitted capacity");
      ++admitted;
      ++aggregate.lazy_local_prepared_tasks;
      if (reused_prepared_storage) {
        ++aggregate.lazy_local_reused_prepared_tasks;
      }
    }

    if (admitted == 0) {
      throw std::logic_error(
          "chart SPR lazy-local admission produced an empty wave");
    }
    ++aggregate.lazy_local_admission_waves;
    aggregate.lazy_local_admitted_concurrency_max =
        std::max(aggregate.lazy_local_admitted_concurrency_max, admitted);
    aggregate.lazy_local_peak_admitted_bytes =
        std::max(aggregate.lazy_local_peak_admitted_bytes, admitted_dynamic);
    auto const wave_runtime_transient_reservation =
        runtime_transient_reservation(admitted);
    aggregate.lazy_local_runtime_transient_reservation_bytes_max = std::max(
        aggregate.lazy_local_runtime_transient_reservation_bytes_max,
        wave_runtime_transient_reservation);
    auto const scheduler_operation_peak =
        planned_scheduler_operation_peak(admitted);
    auto const retained_wave_dynamic = local_capacity_checked_add(
        slots_dynamic_capacity(), wave_runtime_transient_reservation,
        "chart SPR lazy-local retained wave and worker transients");
    aggregate.lazy_local_peak_projected_resident_bytes =
        std::max(aggregate.lazy_local_peak_projected_resident_bytes,
                 local_capacity_checked_add(
                     local_capacity_checked_add(
                         resident_base, retained_wave_dynamic,
                         "chart SPR lazy-local retained wave resident"),
                     scheduler_operation_peak,
                     "chart SPR lazy-local admitted projected resident"));
    if (memory_limited) ++aggregate.lazy_local_memory_limited_waves;

    auto run_one = [&](std::size_t slot) {
      auto& prepared = local_score_workspace_access::prepared(workspace, slot);
      if (!prepared.valid_for_accumulation) return;
      auto& worker = local_score_workspace_access::worker(workspace, slot);
      if (options.force_all_lazy_worker_invariant_failures_for_tests ||
          options.force_lazy_worker_invariant_failure_for_tests == slot) {
        worker.scratch.stage_lazy_worker_failure(
            lazy_local_worker_failure_kind::forced_for_tests);
        prepared.valid_for_accumulation = false;
        return;
      }
      auto const start = std::chrono::steady_clock::now();
      accumulate_prepared_local_candidate_lazy_prepared(
          state, prepared, options, &worker.counters, worker.scratch,
          checked_state,
          admitted > 1
              ? lazy_key_grouping_detail::packed_key_grouping_sort_policy::
                    parallel_wide_radix
              : lazy_key_grouping_detail::packed_key_grouping_sort_policy::
                    adaptive);
      prepared.result.local_score_ms +=
          chart_spr_elapsed_ms(start, std::chrono::steady_clock::now());
    };

    try {
      if (admitted == 1) {
        run_one(0);
      } else {
        if (scheduler == nullptr) {
          throw std::logic_error(
              "chart SPR lazy-local wave: parallel wave without scheduler");
        }
        auto const range_plan =
            scheduler->plan_indexed_ranges(admitted, range_options);
        auto const scheduler_before = scheduler->metrics();
        try {
          auto run =
              run_local_score_scheduler_operation(*scheduler, options, [&] {
                return scheduler->for_each_indexed_range(
                    admitted, range_options,
                    [&](chart_indexed_range const& range, std::size_t,
                        chart_scheduler_cancellation_token const&) {
                      if (range.end != range.begin + 1) {
                        throw std::logic_error(
                            "chart SPR lazy-local wave: non-unit task range");
                      }
                      local_score_worker_arrive_and_wait_for_tests(
                          options, range.begin);
                      run_one(range.begin);
                    });
              });
          record_chart_spr_scheduler_axis_run(
              aggregate.scheduler_axes.local_score_candidates, run);
          if (run.used_parallel_workers()) {
            ++aggregate.local_score_parallel_batches;
            ++aggregate.lazy_local_parallel_waves;
            aggregate.local_score_worker_tasks += run.worker_tasks_submitted;
          }
        } catch (...) {
          record_chart_spr_scheduler_axis_failed_run(
              aggregate.scheduler_axes.local_score_candidates, range_plan,
              scheduler_before, scheduler->metrics());
          // The scheduler has joined every accepted runner. The operation
          // guard folds each worker payload once and publishes this failed-run
          // axis delta before the infrastructure error escapes.
          throw;
        }
      }
    } catch (...) {
      throw;
    }

    for (std::size_t slot = 0; slot < admitted; ++slot) {
      operation_guard.refresh_slot(slot);
    }
    operation_guard.verify_ledger();
    std::size_t observed_admitted_dynamic = 0;
    for (std::size_t slot = 0; slot < admitted; ++slot) {
      observed_admitted_dynamic = local_capacity_checked_add(
          observed_admitted_dynamic,
          local_capacity_checked_add(
              local_score_workspace_access::task_capacity_ledger_slot(
                  workspace, slot),
              runtime_transient_reservation(1),
              "chart SPR lazy-local observed task and worker transient"),
          "chart SPR lazy-local observed wave capacity");
    }
    aggregate.lazy_local_peak_admitted_bytes = std::max(
        aggregate.lazy_local_peak_admitted_bytes, observed_admitted_dynamic);
    auto const observed_retained_dynamic = local_capacity_checked_add(
        slots_dynamic_capacity(), wave_runtime_transient_reservation,
        "chart SPR lazy-local observed retained worker transients");
    auto const observed_projected = local_capacity_checked_add(
        local_capacity_checked_add(resident_base, observed_retained_dynamic,
                                   "chart SPR lazy-local observed resident"),
        scheduler_operation_peak,
        "chart SPR lazy-local observed projected resident");
    aggregate.lazy_local_peak_projected_resident_bytes = std::max(
        aggregate.lazy_local_peak_projected_resident_bytes, observed_projected);
    if (enforce_budget && observed_projected > budget) {
      fail_budget(begin,
                  local_capacity_checked_add(
                      observed_retained_dynamic, scheduler_operation_peak,
                      "chart SPR lazy-local observed operation capacity"),
                  budget - resident_base);
    }

    try {
      for (std::size_t slot = 0; slot < admitted; ++slot) {
        auto& prepared =
            local_score_workspace_access::prepared(workspace, slot);
        auto& worker = local_score_workspace_access::worker(workspace, slot);
        finalize_lazy_local_grouping_status(state, prepared, worker.scratch,
                                            slot);
        if (options.force_lazy_finish_overflow_for_tests == slot) {
          throw std::overflow_error(
              "chart SPR lazy-local forced finish overflow");
        }
        if (options.force_lazy_finish_allocation_for_tests == slot) {
          throw std::bad_alloc{};
        }
        finalize_prepared_local_candidate_score(state, prepared);
      }
    } catch (...) {
      throw;
    }

    for (std::size_t slot = 0; slot < admitted; ++slot) {
      auto& prepared = local_score_workspace_access::prepared(workspace, slot);
      auto& worker = local_score_workspace_access::worker(workspace, slot);
      results[begin + slot] = prepared.result;
      operation_guard.fold_worker(slot);
      worker.reset_for_operation();
      prepared.release_operation_borrows();
    }
    begin += admitted;
  }
  if (retain_finite_task_storage) {
    // Reaching this point proves that every published result and every
    // throw-capable finalization completed. The operation-level measured
    // capacity checks above bound the retained HWM; the finite iteration
    // owner separately admits its overlap with later generation stages.
    operation_guard.keep_retained_storage_on_success();
  }
}

}  // namespace chart_spr_search_detail

std::size_t estimate_chart_spr_lazy_cache_admission_bytes(
    std::size_t clade_count, std::size_t pattern_count) {
  return chart_spr_conservative_lazy_cache_bytes(clade_count, pattern_count);
}

namespace {

struct chart_spr_saturating_size {
  std::size_t value = 0;
  bool saturated = false;
};

chart_spr_saturating_size chart_spr_saturating_add(
    chart_spr_saturating_size lhs, std::size_t rhs,
    std::size_t limit = (std::numeric_limits<std::size_t>::max)()) {
  if (lhs.saturated || rhs > limit || lhs.value > limit - rhs) {
    return chart_spr_saturating_size{limit, true};
  }
  lhs.value += rhs;
  return lhs;
}

chart_spr_saturating_size chart_spr_saturating_add(
    chart_spr_saturating_size lhs, chart_spr_saturating_size rhs,
    std::size_t limit = (std::numeric_limits<std::size_t>::max)()) {
  auto result = chart_spr_saturating_add(lhs, rhs.value, limit);
  result.saturated = result.saturated || rhs.saturated;
  return result;
}

chart_spr_saturating_size chart_spr_saturating_multiply(
    std::size_t lhs, std::size_t rhs,
    std::size_t limit = (std::numeric_limits<std::size_t>::max)()) {
  if (lhs != 0 && rhs > limit / lhs) {
    return chart_spr_saturating_size{limit, true};
  }
  return chart_spr_saturating_size{lhs * rhs, false};
}

chart_spr_saturating_size chart_spr_saturating_multiply(
    chart_spr_saturating_size lhs, std::size_t rhs,
    std::size_t limit = (std::numeric_limits<std::size_t>::max)()) {
  auto result = chart_spr_saturating_multiply(lhs.value, rhs, limit);
  result.saturated = result.saturated || lhs.saturated;
  return result;
}

chart_spr_saturating_size chart_spr_saturating_max(
    chart_spr_saturating_size lhs, chart_spr_saturating_size rhs) noexcept {
  return chart_spr_saturating_size{std::max(lhs.value, rhs.value),
                                   lhs.saturated || rhs.saturated};
}

std::size_t chart_spr_bit_capacity_bytes(std::size_t bit_capacity) {
  constexpr auto bits_per_word = sizeof(unsigned long) * 8;
  auto words = bit_capacity / bits_per_word;
  if (bit_capacity % bits_per_word != 0) ++words;
  return chart_spr_checked_cache_bytes_multiply(
      words, sizeof(unsigned long),
      "chart SPR exact-candidate bit-capacity byte overflow");
}

template <class Vector>
std::size_t chart_spr_vector_capacity_bytes(Vector const& values) {
  return chart_spr_checked_cache_bytes_multiply(
      values.capacity(), sizeof(typename Vector::value_type),
      "chart SPR exact-candidate vector-capacity byte overflow");
}

struct chart_spr_virtual_topology_bound {
  std::size_t clade_count = 0;
  std::size_t production_count = 0;
  std::size_t child_occurrence_count = 0;
  std::size_t total_frontier_entries = 0;
  std::size_t root_frontier_entries = 0;
  bool saturated = false;
};

chart_spr_virtual_topology_bound chart_spr_candidate_topology_bound(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    std::size_t entry_limit) {
  auto const base_clades = state.grammar.clades.size();
  auto const temp_clades = candidate.added_clades.size();
  chart_spr_virtual_topology_bound result;
  result.clade_count = chart_spr_checked_cache_bytes_add(
      base_clades, temp_clades,
      "chart SPR exact-candidate virtual clade-count overflow");

  std::vector<bool> removed(state.grammar.productions.size(), false);
  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base || ref.id >= removed.size()) {
      result.saturated = true;
      continue;
    }
    removed[ref.id] = true;
  }
  result.production_count = candidate.added_productions.size();
  for (std::size_t pid = 0; pid < state.grammar.productions.size(); ++pid) {
    if (!removed[pid]) {
      result.production_count = chart_spr_checked_cache_bytes_add(
          result.production_count, 1,
          "chart SPR exact-candidate virtual production-count overflow");
    }
  }
  for (std::size_t pid = 0; pid < state.grammar.productions.size(); ++pid) {
    if (!removed[pid]) {
      result.child_occurrence_count = chart_spr_checked_cache_bytes_add(
          result.child_occurrence_count,
          state.grammar.productions[pid].children.size(),
          "chart SPR exact-candidate child-occurrence overflow");
    }
  }
  for (auto const& production : candidate.added_productions) {
    result.child_occurrence_count = chart_spr_checked_cache_bytes_add(
        result.child_occurrence_count, production.children.size(),
        "chart SPR exact-candidate child-occurrence overflow");
  }

  auto dense_index = [&](overlay_clade_ref ref) -> std::optional<std::size_t> {
    if (ref.space == overlay_id_space::base) {
      if (ref.id >= base_clades) return std::nullopt;
      return ref.id;
    }
    if (ref.id >= temp_clades) return std::nullopt;
    return base_clades + ref.id;
  };
  auto ref_for_dense = [&](std::size_t dense) {
    return dense < base_clades
               ? base_clade_ref(static_cast<clade_id>(dense))
               : temp_clade_ref(static_cast<clade_id>(dense - base_clades));
  };
  auto is_leaf = [&](overlay_clade_ref ref) {
    return ref.space == overlay_id_space::base
               ? ref.id < state.grammar.clades.size() &&
                     state.grammar.clades[ref.id].taxa.size() == 1
               : ref.id < candidate.added_clades.size() &&
                     candidate.added_clades[ref.id].taxa.size() == 1;
  };

  std::vector<std::size_t> memo(result.clade_count, 0);
  std::vector<std::uint8_t> visit(result.clade_count, 0);
  auto count_ref = [&](auto&& self, overlay_clade_ref ref) -> std::size_t {
    auto dense = dense_index(ref);
    if (!dense) {
      result.saturated = true;
      return entry_limit;
    }
    if (visit[*dense] == 2) return memo[*dense];
    if (visit[*dense] == 1) {
      result.saturated = true;
      return entry_limit;
    }
    visit[*dense] = 1;
    if (is_leaf(ref)) {
      memo[*dense] = 1;
      visit[*dense] = 2;
      return 1;
    }

    chart_spr_saturating_size total;
    auto add_production = [&](auto const& children) {
      chart_spr_saturating_size product{1, false};
      for (auto child_ref : children) {
        auto child_count = self(self, child_ref);
        auto multiplied = chart_spr_saturating_multiply(
            product.value, child_count, entry_limit);
        product.value = multiplied.value;
        product.saturated = product.saturated || multiplied.saturated;
      }
      total = chart_spr_saturating_add(total, product.value, entry_limit);
      total.saturated = total.saturated || product.saturated;
    };
    if (ref.space == overlay_id_space::base) {
      for (auto pid : state.grammar.productions_by_parent[ref.id]) {
        if (pid >= state.grammar.productions.size() || removed[pid]) continue;
        std::vector<overlay_clade_ref> children;
        children.reserve(state.grammar.productions[pid].children.size());
        for (auto child : state.grammar.productions[pid].children) {
          children.push_back(base_clade_ref(child));
        }
        add_production(children);
      }
    }
    for (auto const& production : candidate.added_productions) {
      if (production.parent == ref) add_production(production.children);
    }
    if (total.value == 0) {
      // Invalid/unreachable internal clades fail before a frontier can grow;
      // retain one entry as a safe structural minimum for preflight.
      total.value = 1;
    }
    result.saturated = result.saturated || total.saturated;
    memo[*dense] = total.value;
    visit[*dense] = 2;
    return memo[*dense];
  };

  chart_spr_saturating_size all_entries;
  for (std::size_t dense = 0; dense < result.clade_count; ++dense) {
    all_entries = chart_spr_saturating_add(
        all_entries, count_ref(count_ref, ref_for_dense(dense)), entry_limit);
  }
  result.total_frontier_entries = all_entries.value;
  result.saturated = result.saturated || all_entries.saturated;
  if (state.grammar.root_clade == no_clade ||
      state.grammar.root_clade >= base_clades) {
    result.saturated = true;
    result.root_frontier_entries = entry_limit;
  } else {
    result.root_frontier_entries =
        count_ref(count_ref, base_clade_ref(state.grammar.root_clade));
  }
  return result;
}

chart_spr_saturating_size chart_spr_estimate_candidate_structural_bytes(
    chart_spr_search_state const& state, grammar_spr_candidate const& candidate,
    chart_spr_virtual_topology_bound const& topology) {
  chart_spr_saturating_size total{
      sizeof(clade_grammar) + sizeof(chart_execution_plan), false};
  auto add_product = [&](std::size_t count, std::size_t bytes) {
    auto product = chart_spr_saturating_multiply(count, bytes);
    total = chart_spr_saturating_add(total, product.value);
    total.saturated = total.saturated || product.saturated;
  };

  // Frozen-libstdc++ operational upper bound for the materialized grammar,
  // its dense maps, execution plan, and plan-builder scratch. Nested source
  // witness/string payloads are walked below; the factor two covers geometric
  // growth while building rather than copy-constructing the vectors.
  add_product(topology.clade_count,
              2 * (sizeof(clade_key) + sizeof(chart_plan_clade_descriptor) +
                   8 * sizeof(void*)));
  add_product(
      topology.production_count,
      2 * (sizeof(grammar_production) +
           sizeof(chart_plan_production_descriptor) + 10 * sizeof(void*)));
  add_product(topology.child_occurrence_count,
              2 * (sizeof(clade_id) + sizeof(production_id) +
                   sizeof(chart_plan_child_occurrence)));
  add_product(topology.clade_count,
              12 * (sizeof(clade_id) + sizeof(std::size_t)));
  add_product(topology.production_count, 6 * sizeof(production_id));

  for (auto const& sample : state.grammar.taxa.id_to_sample_id) {
    auto sample_bytes = chart_spr_saturating_add({sample.size(), false}, 1);
    total.saturated = total.saturated || sample_bytes.saturated;
    add_product(sample_bytes.value, 4);
  }
  for (auto const& clade : state.grammar.clades) {
    add_product(clade.taxa.size(), 4 * sizeof(taxon_id));
  }
  for (auto const& clade : candidate.added_clades) {
    add_product(clade.taxa.size(), 4 * sizeof(taxon_id));
  }
  for (auto const& production : state.grammar.productions) {
    add_product(production.children.size(), 4 * sizeof(clade_id));
    for (auto const& witness : production.witnesses) {
      add_product(1, 4 * sizeof(production_witness));
      for (auto const& child : witness.children) {
        add_product(1, 4 * sizeof(production_child_witness));
        add_product(child.edge_alternatives.size(), 4 * sizeof(std::size_t));
      }
    }
  }
  for (auto const& production : candidate.added_productions) {
    add_product(production.children.size(), 4 * sizeof(overlay_clade_ref));
    for (auto const& witness : production.witnesses) {
      add_product(1, 4 * sizeof(production_witness));
      for (auto const& child : witness.children) {
        add_product(1, 4 * sizeof(production_child_witness));
        add_product(child.edge_alternatives.size(), 4 * sizeof(std::size_t));
      }
    }
  }
  return total;
}

chart_spr_saturating_size chart_spr_virtual_key_bytes(
    chart_spr_search_state const& state,
    chart_spr_virtual_topology_bound const& topology) {
  chart_spr_saturating_size all_taxon_text_bytes;
  for (auto const& sample_id : state.grammar.taxa.id_to_sample_id) {
    all_taxon_text_bytes = chart_spr_saturating_add(
        all_taxon_text_bytes,
        chart_spr_saturating_add({sample_id.size(), false}, 16));
  }
  auto const clade_key_bytes = chart_spr_saturating_add(
      {256, false}, chart_spr_saturating_multiply(all_taxon_text_bytes, 2));
  auto clade_keys = chart_spr_saturating_multiply(topology.clade_count,
                                                  clade_key_bytes.value);
  clade_keys.saturated = clade_keys.saturated || clade_key_bytes.saturated;

  // A production key renders its parent and every child clade key.  Bounding
  // each rendered clade by the full taxon universe is deliberately loose but
  // derives from the actual sample-id lengths instead of a fixed character
  // guess.
  auto production_key_occurrences = chart_spr_saturating_add(
      {topology.production_count, false}, topology.child_occurrence_count);
  auto production_keys = chart_spr_saturating_multiply(
      production_key_occurrences, clade_key_bytes.value);
  production_keys = chart_spr_saturating_add(
      production_keys,
      chart_spr_saturating_multiply(topology.production_count, 256));
  return chart_spr_saturating_add(clade_keys, production_keys);
}

void chart_spr_exact_resident_add(std::size_t& total, std::size_t bytes) {
  total = chart_spr_checked_cache_bytes_add(
      total, bytes, "chart SPR exact-loop resident-input byte overflow");
}

template <class Vector>
void chart_spr_exact_resident_add_vector(std::size_t& total,
                                         Vector const& values) {
  chart_spr_exact_resident_add(
      total,
      chart_spr_vector_capacity_bytes(
          values, "chart SPR exact-loop vector-capacity byte overflow"));
}

void chart_spr_exact_resident_add_string(std::size_t& total,
                                         std::string const& value) {
  // basic_string::capacity excludes the terminating null. Charging it even
  // for an SSO string is deliberately conservative because the containing
  // object has already been charged by its owning vector/object surface.
  auto const chars = chart_spr_checked_cache_bytes_add(
      value.capacity(), 1,
      "chart SPR exact-loop string-capacity byte overflow");
  chart_spr_exact_resident_add(total, chars);
}

void chart_spr_exact_resident_add_string_vector(
    std::size_t& total, std::vector<std::string> const& values) {
  chart_spr_exact_resident_add_vector(total, values);
  for (auto const& value : values) {
    chart_spr_exact_resident_add_string(total, value);
  }
}

std::size_t chart_spr_canonical_exact_evidence_dynamic_bytes(
    chart_spr_canonical_exact_evidence const& evidence) {
  std::size_t total = 0;
  chart_spr_exact_resident_add_string(total, evidence.evidence_kind);
  chart_spr_exact_resident_add_string(total, evidence.keep_mask_kind);
  chart_spr_exact_resident_add_string(total,
                                      evidence.topology_selection_kind);
  chart_spr_exact_resident_add_string(total, evidence.topology_selector);
  chart_spr_exact_resident_add_string_vector(
      total, evidence.kept_production_keys);
  chart_spr_exact_resident_add_vector(total, evidence.frontier_sizes);
  for (auto const& [key, size] : evidence.frontier_sizes) {
    (void)size;
    chart_spr_exact_resident_add_string(total, key);
  }
  chart_spr_exact_resident_add_vector(
      total, evidence.optimal_root_provenance_classes);
  for (auto const& provenance : evidence.optimal_root_provenance_classes) {
    chart_spr_exact_resident_add_vector(total, provenance.cost);
    chart_spr_exact_resident_add_string_vector(total,
                                                provenance.production_keys);
  }
  chart_spr_exact_resident_add_string_vector(
      total, evidence.before_topology_production_keys);
  chart_spr_exact_resident_add_string_vector(
      total, evidence.after_topology_production_keys);
  return total;
}

std::size_t chart_spr_grammar_candidate_dynamic_bytes(
    grammar_spr_candidate const& candidate) {
  std::size_t total = 0;
  chart_spr_exact_resident_add_vector(total, candidate.removed_productions);
  chart_spr_exact_resident_add_vector(total, candidate.added_clades);
  for (auto const& clade : candidate.added_clades) {
    chart_spr_exact_resident_add_vector(total, clade.taxa);
  }
  chart_spr_exact_resident_add_vector(total, candidate.added_productions);
  for (auto const& production : candidate.added_productions) {
    chart_spr_exact_resident_add_vector(total, production.children);
    chart_spr_exact_resident_add_vector(total, production.witnesses);
    for (auto const& witness : production.witnesses) {
      chart_spr_exact_resident_add_vector(total, witness.children);
      for (auto const& child : witness.children) {
        chart_spr_exact_resident_add_vector(total, child.edge_alternatives);
      }
    }
  }
  if (candidate.source_before_topology_productions) {
    chart_spr_exact_resident_add_vector(
        total, *candidate.source_before_topology_productions);
  }
  if (candidate.source_after_topology_productions) {
    chart_spr_exact_resident_add_vector(
        total, *candidate.source_after_topology_productions);
  }
  return total;
}

void chart_spr_exact_resident_add_production_signature(
    std::size_t& total, chart_spr_production_signature const& signature) {
  chart_spr_exact_resident_add_vector(total, signature.parent_taxa);
  chart_spr_exact_resident_add_vector(total, signature.child_taxa);
  for (auto const& child : signature.child_taxa) {
    chart_spr_exact_resident_add_vector(total, child);
  }
}

std::size_t chart_spr_topology_selection_dynamic_bytes(
    chart_spr_topology_selection const& selection) {
  std::size_t total = 0;
  chart_spr_exact_resident_add_string(total, selection.selector_name);
  if (!selection.certificate) return total;
  auto const& certificate = *selection.certificate;
  chart_spr_exact_resident_add_vector(
      total, certificate.before_overlay_productions);
  chart_spr_exact_resident_add_vector(
      total, certificate.after_overlay_productions);
  chart_spr_exact_resident_add_vector(total, certificate.before_signatures);
  for (auto const& signature : certificate.before_signatures) {
    chart_spr_exact_resident_add_production_signature(total, signature);
  }
  chart_spr_exact_resident_add_vector(total, certificate.after_signatures);
  for (auto const& signature : certificate.after_signatures) {
    chart_spr_exact_resident_add_production_signature(total, signature);
  }
  return total;
}

std::size_t chart_spr_candidate_score_dynamic_bytes(
    chart_spr_candidate_score const& candidate, bool include_shared_evidence) {
  std::size_t total = chart_spr_grammar_candidate_dynamic_bytes(
      candidate.candidate);
  chart_spr_exact_resident_add(
      total,
      chart_spr_topology_selection_dynamic_bytes(candidate.topology_selection));
  chart_spr_exact_resident_add_string(total, candidate.invalid_reason);
  if (include_shared_evidence && candidate.canonical_exact_evidence) {
    chart_spr_exact_resident_add(
        total, sizeof(chart_spr_canonical_exact_evidence));
    chart_spr_exact_resident_add(
        total, chart_spr_canonical_exact_evidence_dynamic_bytes(
                   *candidate.canonical_exact_evidence));
    // Frozen-libstdc++ shared_ptr make_shared control block, allocator header,
    // and alignment allowance. The evidence object itself is charged above.
    chart_spr_exact_resident_add(total, 8 * sizeof(void*));
  }
  return total;
}

void chart_spr_exact_resident_add_canonical_score(
    std::size_t& total, chart_spr_canonical_score const& score) {
  chart_spr_exact_resident_add_string(total, score.kind);
  chart_spr_exact_resident_add_string(total, score.convention);
}

std::size_t chart_spr_canonical_candidate_record_dynamic_bytes(
    chart_spr_canonical_candidate_record const& record) {
  std::size_t total = 0;
  chart_spr_exact_resident_add_string(total, record.signature);
  chart_spr_exact_resident_add_string(total, record.invalid_reason);
  chart_spr_exact_resident_add_canonical_score(total, record.lower_bound);
  if (record.exact) {
    chart_spr_exact_resident_add_canonical_score(total, *record.exact);
  }
  if (record.exact_evidence) {
    chart_spr_exact_resident_add(
        total,
        chart_spr_canonical_exact_evidence_dynamic_bytes(
            *record.exact_evidence));
  }
  return total;
}

std::size_t chart_spr_candidate_estimator_scratch_bytes(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    std::size_t base_child_occurrence_count) {
  auto const clade_count = chart_spr_checked_cache_bytes_add(
      state.grammar.clades.size(), candidate.candidate.added_clades.size(),
      "chart SPR exact estimator clade-count overflow");
  std::size_t total =
      3 * sizeof(std::vector<std::size_t>) +
      sizeof(chart_spr_virtual_topology_bound);
  chart_spr_exact_resident_add(
      total,
      chart_spr_bit_capacity_bytes(state.grammar.productions.size()));
  chart_spr_exact_resident_add(
      total, chart_spr_checked_cache_bytes_multiply(
                 clade_count, sizeof(std::size_t),
                 "chart SPR exact estimator memo byte overflow"));
  chart_spr_exact_resident_add(
      total, chart_spr_checked_cache_bytes_multiply(
                 clade_count, sizeof(std::uint8_t),
                 "chart SPR exact estimator visit byte overflow"));
  // Base-production child vectors stay live along recursive calls. Charging
  // every child occurrence bounds their aggregate retained capacities.
  chart_spr_exact_resident_add(
      total, chart_spr_checked_cache_bytes_multiply(
                 base_child_occurrence_count, sizeof(overlay_clade_ref),
                 "chart SPR exact estimator child scratch overflow"));
  // The recursive generic-lambda frames and associative/string temporaries are
  // stack/SSO on this toolchain. Include a per-clade envelope so the unified
  // operational bound does not silently exclude them.
  chart_spr_exact_resident_add(
      total, chart_spr_checked_cache_bytes_multiply(
                 clade_count, 12 * sizeof(void*) + 8 * sizeof(std::size_t),
                 "chart SPR exact estimator frame scratch overflow"));
  chart_spr_exact_resident_add(total, 256);
  return total;
}

}  // namespace

std::size_t estimate_chart_spr_trim_dynamic_resident_bytes(
    multisite_trim_result const& trim) {
  std::size_t total = 0;
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_bit_capacity_bytes(trim.keep_production.capacity()),
      "chart SPR resident exact trim byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_vector_capacity_bytes(trim.frontier_sizes_by_clade),
      "chart SPR resident exact trim byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_vector_capacity_bytes(
          trim.lazy_structural_class_count_by_clade),
      "chart SPR resident exact trim byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total, chart_spr_vector_capacity_bytes(trim.frontier_level_diagnostics),
      "chart SPR resident exact trim byte overflow");
  total = chart_spr_checked_cache_bytes_add(
      total,
      chart_spr_vector_capacity_bytes(trim.optimal_root_provenance_classes),
      "chart SPR resident exact trim byte overflow");
  for (auto const& provenance : trim.optimal_root_provenance_classes) {
    total = chart_spr_checked_cache_bytes_add(
        total, chart_spr_vector_capacity_bytes(provenance.cost),
        "chart SPR resident exact trim byte overflow");
    total = chart_spr_checked_cache_bytes_add(
        total,
        chart_spr_bit_capacity_bytes(provenance.used_production.capacity()),
        "chart SPR resident exact trim byte overflow");
  }
  return total;
}

std::size_t estimate_chart_spr_trim_resident_bytes(
    multisite_trim_result const& trim) {
  return chart_spr_checked_cache_bytes_add(
      sizeof(trim), estimate_chart_spr_trim_dynamic_resident_bytes(trim),
      "chart SPR resident exact trim byte overflow");
}

std::size_t estimate_chart_spr_canonical_exact_evidence_resident_bytes(
    chart_spr_canonical_exact_evidence const& evidence) {
  auto total = sizeof(evidence);
  auto add = [&](std::size_t bytes) {
    total = chart_spr_checked_cache_bytes_add(
        total, bytes, "chart SPR canonical exact-evidence resident overflow");
  };
  auto add_string = [&](std::string const& value) {
    add(chart_spr_checked_cache_bytes_multiply(
        chart_spr_checked_cache_bytes_add(
            value.capacity(), 1,
            "chart SPR canonical exact-evidence string overflow"),
        sizeof(char),
        "chart SPR canonical exact-evidence string overflow"));
  };
  auto add_string_vector = [&](std::vector<std::string> const& values) {
    add(chart_spr_vector_capacity_bytes(values));
    for (auto const& value : values) add_string(value);
  };

  add_string(evidence.evidence_kind);
  add_string(evidence.keep_mask_kind);
  add_string(evidence.topology_selection_kind);
  add_string(evidence.topology_selector);
  add_string_vector(evidence.kept_production_keys);
  add(chart_spr_vector_capacity_bytes(evidence.frontier_sizes));
  for (auto const& [key, size] : evidence.frontier_sizes) {
    (void)size;
    add_string(key);
  }
  add(chart_spr_vector_capacity_bytes(
      evidence.optimal_root_provenance_classes));
  for (auto const& provenance : evidence.optimal_root_provenance_classes) {
    add(chart_spr_vector_capacity_bytes(provenance.cost));
    add_string_vector(provenance.production_keys);
  }
  add_string_vector(evidence.before_topology_production_keys);
  add_string_vector(evidence.after_topology_production_keys);
  return total;
}

chart_spr_canonical_exact_evidence_memory_estimate
estimate_chart_spr_canonical_exact_evidence_memory(
    clade_grammar const& grammar, multisite_trim_result const& trim) {
  auto add = [](std::size_t lhs, std::size_t rhs, char const* context) {
    return chart_spr_checked_cache_bytes_add(lhs, rhs, context);
  };
  auto multiply = [](std::size_t lhs, std::size_t rhs,
                     char const* context) {
    return chart_spr_checked_cache_bytes_multiply(lhs, rhs, context);
  };
  auto string_capacity = [&](std::size_t encoded_size) {
    auto const sso = std::string{}.capacity();
    // Frozen libstdc++ `_M_create_plus` doubles the current SSO capacity when
    // an explicit reserve just exceeds SSO. Its C++26 std::allocator<char>
    // then exposes the default-new-alignment rounding returned by
    // allocate_at_least through string::capacity(). Every canonical output
    // string starts from its SSO representation.
    auto const minimum_capacity =
        encoded_size <= sso
            ? sso
            : std::max(encoded_size,
                       multiply(2, sso,
                                "chart SPR canonical string SSO growth"));
    if (minimum_capacity == sso) return add(sso, 1, "chart SPR SSO bytes");
    constexpr auto allocation_quantum = alignof(std::max_align_t);
    auto const requested = add(
        minimum_capacity, 1,
        "chart SPR canonical exact-evidence string allocation");
    auto const rounded = add(
        requested, allocation_quantum - 1,
        "chart SPR canonical exact-evidence string allocation rounding");
    return multiply(
        rounded / allocation_quantum, allocation_quantum,
        "chart SPR canonical exact-evidence string allocation rounding");
  };

  std::size_t retained = sizeof(chart_spr_canonical_exact_evidence);
  auto add_retained = [&](std::size_t bytes) {
    retained = add(retained, bytes,
                   "chart SPR canonical exact-evidence retained bytes");
  };

  auto const evidence_kind_size =
      trim.keep_production_exact
          ? std::string_view{
                "grammar_exact_frontier_provenance_companion"}
                .size()
          : std::string_view{
                "grammar_exact_score_only_frontier_statistics"}
                .size();
  add_retained(string_capacity(evidence_kind_size));
  add_retained(string_capacity(
      std::string_view{multisite_keep_mask_kind_name(trim.keep_mask_kind)}
          .size()));
  add_retained(string_capacity(std::string_view{"none"}.size()));
  add_retained(string_capacity(0));

  if (trim.keep_production_exact) {
    if (trim.keep_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          "chart-SPR canonical evidence estimate: exact keep mask size "
          "mismatch");
    }
    std::size_t kept_count = 0;
    for (bool keep : trim.keep_production) {
      if (keep) ++kept_count;
    }
    add_retained(multiply(kept_count, sizeof(std::string),
                          "chart SPR canonical kept-key vector"));
    for (std::size_t pid = 0; pid < trim.keep_production.size(); ++pid) {
      if (!trim.keep_production[pid]) continue;
      add_retained(string_capacity(
          chart_spr_semantic_detail::production_key_size(
              grammar, static_cast<production_id>(pid))));
    }
  }

  if (trim.frontier_sizes_by_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "chart-SPR canonical evidence estimate: frontier-size vector size "
        "mismatch");
  }
  std::size_t frontier_count = 0;
  for (auto const& clade : grammar.clades) {
    if (!clade.taxa.empty()) ++frontier_count;
  }
  add_retained(multiply(
      frontier_count, sizeof(std::pair<std::string, std::size_t>),
      "chart SPR canonical frontier vector"));
  for (auto const& clade : grammar.clades) {
    if (clade.taxa.empty()) continue;
    add_retained(string_capacity(
        chart_spr_semantic_detail::sample_set_key_size(grammar, clade.taxa)));
  }

  add_retained(multiply(
      trim.optimal_root_provenance_classes.size(),
      sizeof(chart_spr_canonical_root_provenance_class),
      "chart SPR canonical provenance vector"));
  for (auto const& provenance : trim.optimal_root_provenance_classes) {
    if (provenance.used_production.size() != grammar.productions.size()) {
      throw std::runtime_error(
          "chart-SPR canonical evidence estimate: provenance mask size "
          "mismatch");
    }
    add_retained(multiply(provenance.cost.size(), sizeof(std::uint64_t),
                          "chart SPR canonical provenance cost vector"));
    std::size_t used_count = 0;
    for (bool used : provenance.used_production) {
      if (used) ++used_count;
    }
    add_retained(multiply(used_count, sizeof(std::string),
                          "chart SPR canonical provenance key vector"));
    for (std::size_t pid = 0; pid < provenance.used_production.size(); ++pid) {
      if (!provenance.used_production[pid]) continue;
      add_retained(string_capacity(
          chart_spr_semantic_detail::production_key_size(
              grammar, static_cast<production_id>(pid))));
    }
  }

  auto const decimal_scratch = string_capacity(
      chart_spr_semantic_detail::decimal_digit_count(
          (std::numeric_limits<std::size_t>::max)()));
  auto clade_key_scratch = [&](std::span<taxon_id const> taxa) {
    std::size_t scratch = multiply(
        taxa.size(), sizeof(taxon_id),
        "chart SPR canonical clade-key taxon scratch");
    scratch = add(
        scratch,
        multiply(taxa.size(), sizeof(std::string),
                 "chart SPR canonical clade-key sample vector"),
        "chart SPR canonical clade-key scratch");
    for (auto taxon : taxa) {
      if (taxon >= grammar.taxa.id_to_sample_id.size()) {
        throw std::runtime_error(
            "chart-SPR canonical evidence estimate: taxon out of range");
      }
      scratch = add(scratch,
                    string_capacity(
                        grammar.taxa.id_to_sample_id[taxon].size()),
                    "chart SPR canonical clade-key sample copies");
    }
    scratch = add(
        scratch,
        string_capacity(chart_spr_semantic_detail::sample_set_key_size(
            grammar, taxa)),
        "chart SPR canonical clade-key result");
    return add(scratch, decimal_scratch,
               "chart SPR canonical clade-key decimal scratch");
  };

  std::size_t maximum_key_scratch = 0;
  for (auto const& clade : grammar.clades) {
    if (clade.taxa.empty()) continue;
    maximum_key_scratch =
        std::max(maximum_key_scratch, clade_key_scratch(clade.taxa));
  }
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& production = grammar.productions[pid];
    if (production.parent == no_clade ||
        production.parent >= grammar.clades.size()) {
      throw std::runtime_error(
          "chart-SPR canonical evidence estimate: production parent out of "
          "range");
    }
    auto const& parent = grammar.clades[production.parent].taxa;
    std::size_t scratch = multiply(
        multiply(2, parent.size(),
                 "chart SPR canonical production parent copies"),
        sizeof(taxon_id), "chart SPR canonical production parent copies");
    scratch = add(
        scratch,
        multiply(production.children.size(), sizeof(std::vector<taxon_id>),
                 "chart SPR canonical production child-taxa vector"),
        "chart SPR canonical production scratch");
    scratch = add(
        scratch,
        multiply(production.children.size(), sizeof(std::string),
                 "chart SPR canonical production child-key vector"),
        "chart SPR canonical production scratch");
    auto maximum_sample_scratch = clade_key_scratch(parent);
    auto const parent_key_size =
        chart_spr_semantic_detail::sample_set_key_size(grammar, parent);
    scratch = add(scratch, string_capacity(parent_key_size),
                  "chart SPR canonical production parent key");
    for (auto child : production.children) {
      if (child == no_clade || child >= grammar.clades.size()) {
        throw std::runtime_error(
            "chart-SPR canonical evidence estimate: production child out of "
            "range");
      }
      auto const& child_taxa = grammar.clades[child].taxa;
      scratch = add(
          scratch,
          multiply(child_taxa.size(), sizeof(taxon_id),
                   "chart SPR canonical production child-taxa copies"),
          "chart SPR canonical production scratch");
      scratch = add(
          scratch,
          string_capacity(chart_spr_semantic_detail::sample_set_key_size(
              grammar, child_taxa)),
          "chart SPR canonical production child keys");
      maximum_sample_scratch =
          std::max(maximum_sample_scratch, clade_key_scratch(child_taxa));
    }
    scratch = add(scratch, maximum_sample_scratch,
                  "chart SPR canonical production sample scratch");
    scratch = add(
        scratch,
        string_capacity(chart_spr_semantic_detail::production_key_size(
            grammar, static_cast<production_id>(pid))),
        "chart SPR canonical production result");
    scratch = add(scratch, decimal_scratch,
                  "chart SPR canonical production decimal scratch");
    maximum_key_scratch = std::max(maximum_key_scratch, scratch);
  }

  return chart_spr_canonical_exact_evidence_memory_estimate{
      .retained_bytes = retained,
      .construction_peak_bytes = add(
          retained, maximum_key_scratch,
          "chart SPR canonical exact-evidence construction peak"),
  };
}

std::size_t chart_spr_search_detail::estimate_exact_loop_resident_input_bytes(
    std::vector<chart_spr_candidate_score> const& ranked,
    chart_spr_iteration_result const& iteration) {
  // The vector/iteration objects themselves are live stack/result storage;
  // their owning buffers are charged from actual frozen-toolchain capacity.
  std::size_t total = sizeof(ranked) + sizeof(iteration);
  chart_spr_exact_resident_add_vector(total, ranked);

  std::size_t accepted_copy_dynamic_peak = 0;
  for (auto const& candidate : ranked) {
    chart_spr_exact_resident_add(
        total, chart_spr_candidate_score_dynamic_bytes(
                   candidate, /*include_shared_evidence=*/true));
    // result.accepted contains its candidate object inline. Copying the
    // winner duplicates every owning overlay/certificate/string allocation,
    // while shared canonical evidence continues to alias the same allocation.
    accepted_copy_dynamic_peak =
        std::max(accepted_copy_dynamic_peak,
                 chart_spr_candidate_score_dynamic_bytes(
                     candidate, /*include_shared_evidence=*/false));
  }
  chart_spr_exact_resident_add(total, accepted_copy_dynamic_peak);

  chart_spr_exact_resident_add_string(total, iteration.no_accept_reason);
  chart_spr_exact_resident_add_string(total,
                                      iteration.accepted_candidate_signature);
  chart_spr_exact_resident_add_vector(total,
                                      iteration.affected_clade_counts);
  chart_spr_exact_resident_add_string(
      total, iteration.post_materialization_rejection_reason);
  chart_spr_exact_resident_add_vector(
      total, iteration.exact_candidate_verification_ms);

  chart_spr_exact_resident_add_vector(total,
                                      iteration.canonical_candidates);
  for (auto const& record : iteration.canonical_candidates) {
    chart_spr_exact_resident_add(
        total, chart_spr_canonical_candidate_record_dynamic_bytes(record));
  }
  chart_spr_exact_resident_add_vector(
      total, iteration.canonical_ranked_stream_indices);
  chart_spr_exact_resident_add_vector(
      total, iteration.canonical_exact_verified_stream_indices);
  if (iteration.canonical_state_exact_before) {
    // The optional evidence object is inline in sizeof(iteration).
    chart_spr_exact_resident_add(
        total, chart_spr_canonical_exact_evidence_dynamic_bytes(
                   *iteration.canonical_state_exact_before));
  }
  if (iteration.accepted) {
    // Normally disengaged at exact-loop entry, but keep the walker valid for
    // direct/internal callers and future loop rearrangements.
    chart_spr_exact_resident_add(
        total, chart_spr_candidate_score_dynamic_bytes(
                   *iteration.accepted, /*include_shared_evidence=*/true));
  }
  return total;
}

std::size_t
chart_spr_search_detail::estimate_exact_loop_accepted_candidate_dynamic_bytes(
    std::span<chart_spr_candidate_score const> ranked) {
  std::size_t peak = 0;
  for (auto const& candidate : ranked) {
    // Exact evidence is admitted separately from each candidate estimate.
    // This bound covers the pre-existing overlay, topology certificate, keys,
    // and strings copied into result.accepted after ranked is released.
    peak = std::max(peak, chart_spr_candidate_score_dynamic_bytes(
                              candidate, /*include_shared_evidence=*/false));
  }
  return peak;
}

std::size_t
chart_spr_search_detail::estimate_grammar_spr_enumeration_fixed_live_bytes(
    clade_grammar const& grammar, chart_execution_plan const& plan) {
  auto add = [](std::size_t lhs, std::size_t rhs) {
    return chart_spr_checked_cache_bytes_add(
        lhs, rhs, "chart SPR enumeration live-envelope overflow");
  };
  auto multiply = [](std::size_t lhs, std::size_t rhs) {
    return chart_spr_checked_cache_bytes_multiply(
        lhs, rhs, "chart SPR enumeration live-envelope overflow");
  };
  using lookup_type = std::map<std::vector<taxon_id>, clade_id>;
  using seen_type = std::set<std::string>;
  std::size_t total = sizeof(lookup_type) + sizeof(seen_type);
  for (auto const& clade : grammar.clades) {
    total = add(total, sizeof(lookup_type::value_type) + 4 * sizeof(void*));
    total = add(total, multiply(clade.taxa.size(), sizeof(taxon_id)));
  }
  total =
      add(total, multiply(grammar.productions.size(), sizeof(production_id)));
  total = add(total, multiply(grammar.clades.size(), sizeof(clade_id)));

  std::size_t maximum_arity = 0;
  std::size_t maximum_parent_productions = 0;
  for (auto const& production : grammar.productions) {
    maximum_arity = std::max(maximum_arity, production.children.size());
  }
  for (auto const& parent_row : grammar.productions_by_child) {
    maximum_parent_productions =
        std::max(maximum_parent_productions, parent_row.size());
  }
  std::size_t depth = 0;
  for (auto const& clade : plan.clades()) {
    depth = std::max(depth, clade.dependency_level);
  }
  // Two nested upward traversals retain their path/active sets and each stack
  // frame's copied parent-production row until the callback returns. Vector
  // growth is bounded by twice the logical size on frozen libstdc++.
  total = add(total, multiply(multiply(4, depth),
                              sizeof(chart_spr_detail::upward_path_step)));
  total = add(total, multiply(multiply(multiply(4, depth), maximum_arity),
                              sizeof(clade_id)));
  total = add(total, multiply(multiply(2, depth),
                              sizeof(clade_id) + 4 * sizeof(void*)));
  total = add(total,
              multiply(multiply(multiply(4, depth), maximum_parent_productions),
                       sizeof(production_id)));
  total = add(total, multiply(multiply(3, maximum_arity), sizeof(std::size_t)));
  return total;
}

std::size_t
chart_spr_search_detail::estimate_grammar_spr_enumeration_signature_live_bytes(
    clade_grammar const& grammar, grammar_spr_candidate const& candidate,
    std::size_t encoded_length_prefix_for_tests, bool use_binary_encoding) {
  auto add = [](std::size_t lhs, std::size_t rhs) {
    return chart_spr_checked_cache_bytes_add(
        lhs, rhs, "chart SPR enumeration signature length overflow");
  };
  auto multiply = [](std::size_t lhs, std::size_t rhs) {
    return chart_spr_checked_cache_bytes_multiply(
        lhs, rhs, "chart SPR enumeration signature length overflow");
  };
  if (use_binary_encoding) {
    auto normalized_taxa_count =
        [](std::vector<taxon_id> const& taxa) noexcept {
          if (chart_spr_detail::chart_spr_taxa_are_sorted_unique(taxa)) {
            return taxa.size();
          }
          std::size_t count = 0;
          for (std::size_t index = 0; index < taxa.size(); ++index) {
            bool appeared_before = false;
            for (std::size_t previous = 0; previous < index; ++previous) {
              if (taxa[previous] == taxa[index]) {
                appeared_before = true;
                break;
              }
            }
            if (!appeared_before) ++count;
          }
          return count;
        };
    auto taxa_key_length = [&](std::vector<taxon_id> const& taxa) {
      return add(sizeof(std::size_t),
                 multiply(normalized_taxa_count(taxa), sizeof(taxon_id)));
    };
    auto required_ref_taxa =
        [&](overlay_clade_ref ref) -> std::vector<taxon_id> const& {
      return chart_spr_clade_taxa_for_ref(grammar, candidate, ref);
    };
    auto optional_ref_length = [&](overlay_clade_ref ref) {
      return ref.id == no_clade ? sizeof(std::size_t)
                                : taxa_key_length(required_ref_taxa(ref));
    };
    auto production_length = [&](std::vector<taxon_id> const& parent,
                                 auto const& children,
                                 auto&& resolve_child) {
      auto length = add(taxa_key_length(parent), sizeof(std::size_t));
      for (auto const& child : children) {
        length = add(length, taxa_key_length(resolve_child(child)));
      }
      return length;
    };

    auto encoded_length = add(
        encoded_length_prefix_for_tests,
        sizeof(chart_spr_detail::chart_spr_binary_taxon_dedup_key_prefix) - 1);
    encoded_length =
        add(encoded_length, optional_ref_length(candidate.moved_clade));
    encoded_length =
        add(encoded_length, optional_ref_length(candidate.old_parent));
    encoded_length =
        add(encoded_length, optional_ref_length(candidate.old_sibling));
    encoded_length = add(
        encoded_length,
        optional_ref_length(candidate.new_sibling_or_target));
    encoded_length = add(encoded_length, sizeof(std::size_t));
    for (auto const& clade : candidate.added_clades) {
      encoded_length = add(encoded_length, taxa_key_length(clade.taxa));
    }

    encoded_length = add(encoded_length, sizeof(std::size_t));
    for (auto ref : candidate.removed_productions) {
      if (ref.space != overlay_id_space::base || ref.id == no_production ||
          ref.id >= grammar.productions.size()) {
        throw std::runtime_error(
            "chart SPR enumeration signature: removed production out of "
            "range");
      }
      auto const& production = grammar.productions[ref.id];
      if (production.parent == no_clade ||
          production.parent >= grammar.clades.size()) {
        throw std::runtime_error(
            "chart SPR enumeration signature: production parent out of range");
      }
      auto const length = production_length(
          grammar.clades[production.parent].taxa, production.children,
          [&](clade_id child) -> std::vector<taxon_id> const& {
            if (child == no_clade || child >= grammar.clades.size()) {
              throw std::runtime_error(
                  "chart SPR enumeration signature: production child out of "
                  "range");
            }
            return grammar.clades[child].taxa;
          });
      encoded_length =
          add(encoded_length, add(sizeof(std::size_t), length));
    }

    encoded_length = add(encoded_length, sizeof(std::size_t));
    for (auto const& production : candidate.added_productions) {
      auto const length = production_length(
          required_ref_taxa(production.parent), production.children,
          [&](overlay_clade_ref child) -> std::vector<taxon_id> const& {
            return required_ref_taxa(child);
          });
      encoded_length =
          add(encoded_length, add(sizeof(std::size_t), length));
    }

    // The binary serializer reserves 256 bytes and may geometrically grow once
    // more while appending. This is the same frozen-libstdc++ retained-capacity
    // bound used by sampled-tree admission, specialized to the realized
    // candidate without allocating a second key in the admission callback.
    constexpr std::size_t reserve_floor = 256;
    constexpr std::size_t allocation_quantum = 16;
    auto allocation_bytes =
        add(std::max(reserve_floor, multiply(encoded_length, 2)),
            std::size_t{1});
    allocation_bytes = add(allocation_bytes, allocation_quantum - 1);
    allocation_bytes =
        multiply(allocation_bytes / allocation_quantum, allocation_quantum);
    return add(sizeof(std::set<std::string>::value_type) + 4 * sizeof(void*),
               allocation_bytes);
  }
  auto decimal_digits = [](taxon_id value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
      value /= 10;
      ++digits;
    }
    return digits;
  };
  auto taxa_key_length = [&](std::vector<taxon_id> const& taxa) {
    std::size_t length = 2;  // '{' and '}'
    if (chart_spr_detail::chart_spr_taxa_are_sorted_unique(taxa)) {
      for (auto taxon : taxa) {
        length = add(length, decimal_digits(taxon));
        length = add(length, 1);  // trailing comma
      }
      return length;
    }
    // Normalization changes ordering and removes duplicates, neither of which
    // requires constructing the normalized vector to calculate encoded size.
    for (std::size_t index = 0; index < taxa.size(); ++index) {
      bool appeared_before = false;
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (taxa[previous] == taxa[index]) {
          appeared_before = true;
          break;
        }
      }
      if (appeared_before) continue;
      length = add(length, decimal_digits(taxa[index]));
      length = add(length, 1);  // trailing comma
    }
    return length;
  };
  auto required_ref_taxa =
      [&](overlay_clade_ref ref) -> std::vector<taxon_id> const& {
    return chart_spr_clade_taxa_for_ref(grammar, candidate, ref);
  };
  auto optional_ref_length = [&](overlay_clade_ref ref) {
    return ref.id == no_clade ? std::size_t{2}
                              : taxa_key_length(required_ref_taxa(ref));
  };
  auto production_length = [&](std::vector<taxon_id> const& parent,
                               auto const& children, auto&& resolve_child) {
    auto length = add(taxa_key_length(parent), 2);  // "->"
    for (auto const& child : children) {
      length = add(length, taxa_key_length(resolve_child(child)));
    }
    return length;
  };

  std::size_t encoded_length = encoded_length_prefix_for_tests;
  auto add_literal = [&](std::string_view literal) {
    encoded_length = add(encoded_length, literal.size());
  };
  add_literal("m=");
  encoded_length =
      add(encoded_length, optional_ref_length(candidate.moved_clade));
  add_literal(";op=");
  encoded_length =
      add(encoded_length, optional_ref_length(candidate.old_parent));
  add_literal(";os=");
  encoded_length =
      add(encoded_length, optional_ref_length(candidate.old_sibling));
  add_literal(";nt=");
  encoded_length =
      add(encoded_length, optional_ref_length(candidate.new_sibling_or_target));
  add_literal(";clades=");
  for (auto const& clade : candidate.added_clades) {
    encoded_length = add(encoded_length, taxa_key_length(clade.taxa));
  }
  add_literal(";rm=");
  for (auto ref : candidate.removed_productions) {
    if (ref.space != overlay_id_space::base || ref.id == no_production ||
        ref.id >= grammar.productions.size()) {
      throw std::runtime_error(
          "chart SPR enumeration signature: removed production out of "
          "range");
    }
    auto const& production = grammar.productions[ref.id];
    if (production.parent == no_clade ||
        production.parent >= grammar.clades.size()) {
      throw std::runtime_error(
          "chart SPR enumeration signature: production parent out of range");
    }
    auto const length = production_length(
        grammar.clades[production.parent].taxa, production.children,
        [&](clade_id child) -> std::vector<taxon_id> const& {
          if (child == no_clade || child >= grammar.clades.size()) {
            throw std::runtime_error(
                "chart SPR enumeration signature: production child out of "
                "range");
          }
          return grammar.clades[child].taxa;
        });
    encoded_length = add(encoded_length, add(length, 1));  // ';'
  }
  add_literal(";add=");
  for (auto const& production : candidate.added_productions) {
    auto const length = production_length(
        required_ref_taxa(production.parent), production.children,
        [&](overlay_clade_ref child) -> std::vector<taxon_id> const& {
          return required_ref_taxa(child);
        });
    encoded_length = add(encoded_length, add(length, 1));  // ';'
  }

  // The required GCC-trunk/libstdc++ allocator uses allocate_at_least for
  // non-SSO strings and exposes the 16-byte allocation quantum through
  // basic_string::capacity().  Charge that retained slack exactly: N encoded
  // bytes require round_up(N + 1, 16) allocation bytes and therefore retain a
  // capacity one byte smaller.  Querying the SSO capacity remains
  // allocation-free.
  auto string_capacity = std::string{}.capacity();
  if (encoded_length > string_capacity) {
    constexpr std::size_t allocation_quantum = 16;
    auto allocation_bytes = add(encoded_length, 1);
    allocation_bytes = add(allocation_bytes, allocation_quantum - 1);
    allocation_bytes =
        (allocation_bytes / allocation_quantum) * allocation_quantum;
    string_capacity = allocation_bytes - 1;
  }
  return chart_spr_checked_cache_bytes_add(
      sizeof(std::set<std::string>::value_type) + 4 * sizeof(void*),
      chart_spr_checked_cache_bytes_add(
          string_capacity, 1,
          "chart SPR enumeration signature capacity overflow"),
      "chart SPR enumeration signature node overflow");
}

chart_spr_search_detail::grammar_spr_finite_iteration_memory_envelope
chart_spr_search_detail::estimate_grammar_spr_finite_iteration_memory_envelope(
    chart_spr_search_state const& state, std::size_t candidate_limit,
    std::size_t candidate_batch_size, std::size_t ranked_limit,
    bool capture_semantics, chart_scheduler const& scheduler,
    std::size_t local_task_slots,
    grammar_spr_enumeration_options const* source_options,
    std::size_t candidate_buffer_count, bool include_pipeline_control,
    std::size_t source_wave_size, std::size_t projection_wave_size,
    std::size_t grammar_candidate_wave_size,
    std::optional<std::size_t> published_state_resident_bytes) {
  auto add = [](std::size_t lhs, std::size_t rhs) {
    return chart_spr_checked_cache_bytes_add(
        lhs, rhs, "chart SPR finite grammar iteration envelope overflow");
  };
  auto multiply = [](std::size_t lhs, std::size_t rhs) {
    return chart_spr_checked_cache_bytes_multiply(
        lhs, rhs, "chart SPR finite grammar iteration envelope overflow");
  };
  auto doubled_vector = [&](std::size_t count, std::size_t width) {
    return multiply(2, multiply(count, width));
  };
  if (candidate_buffer_count == 0 || candidate_buffer_count > 2) {
    throw std::invalid_argument(
        "chart SPR finite iteration envelope: candidate buffer count must be "
        "one or two");
  }
  auto const source = source_options == nullptr
                          ? chart_spr_candidate_source::grammar
                          : source_options->source;
  auto const resolved_workers =
      scheduler.worker_resolution().resolved_workers;
  auto published_state_resident_cache = published_state_resident_bytes;
  auto published_state_resident = [&]() {
    if (!published_state_resident_cache) {
      published_state_resident_cache =
          estimate_chart_spr_published_state_resident_bytes(state);
    }
    return *published_state_resident_cache;
  };
  if (source != chart_spr_candidate_source::grammar && resolved_workers > 1 &&
      std::locale{} != std::locale::classic()) {
    throw std::invalid_argument(
        "chart SPR finite parallel sampled-tree admission requires the "
        "classic global locale");
  }
  auto decimal_digits = [](std::size_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
      value /= 10;
      ++digits;
    }
    return digits;
  };

  auto const& grammar = state.grammar;
  auto const taxa = grammar.taxa.id_to_sample_id.size();
  std::size_t maximum_arity = 0;
  for (auto const& production : grammar.productions) {
    maximum_arity = std::max(maximum_arity, production.children.size());
  }

  // A grammar-native candidate rebuilds at most one group per side/path step,
  // plus the source, destination, and LCA groups. Along every legal upward
  // edge the immutable plan dependency level increases by at least one, so
  // its maximum level bounds either path without substituting total clades.
  std::size_t maximum_dependency_depth = 0;
  for (auto const& clade : state.execution_plan.clades()) {
    maximum_dependency_depth =
        std::max(maximum_dependency_depth, clade.dependency_level);
  }
  auto const candidate_clades = add(multiply(2, maximum_dependency_depth), 3);
  auto const candidate_productions = candidate_clades;
  auto const removed_productions =
      add(multiply(2, maximum_dependency_depth), 1);
  // Every ordinary rebuilt group replaces one child of a base production by
  // the current rebuilt branch and keeps that production's cochildren, hence
  // has at most A children. The sole LCA rebuild combines at most two rebuilt
  // branches and the two traversed productions' cochild sets, bounded by 2A;
  // the initial moved+target group has two. Thus max(2, 2A) is a complete
  // per-added-production bound for make_general_spr_candidate().
  auto const candidate_maximum_arity =
      std::max<std::size_t>(2, multiply(2, maximum_arity));

  std::size_t candidate_dynamic = 0;
  candidate_dynamic =
      add(candidate_dynamic,
          doubled_vector(removed_productions, sizeof(overlay_production_ref)));
  candidate_dynamic = add(candidate_dynamic,
                          doubled_vector(candidate_clades, sizeof(clade_key)));
  candidate_dynamic =
      add(candidate_dynamic,
          multiply(candidate_clades, doubled_vector(taxa, sizeof(taxon_id))));
  candidate_dynamic = add(candidate_dynamic,
                          doubled_vector(candidate_productions,
                                         sizeof(overlay_grammar_production)));
  candidate_dynamic = add(candidate_dynamic,
                          multiply(candidate_productions,
                                   doubled_vector(candidate_maximum_arity,
                                                  sizeof(overlay_clade_ref))));
  auto const maximum_temp_children =
      multiply(candidate_productions, candidate_maximum_arity);
  auto const local_task = estimate_lazy_local_candidate_shape_preflight(
      state, candidate_clades, candidate_productions, removed_productions,
      maximum_temp_children, candidate_dynamic);

  auto numeric_taxa_key =
      add(2, multiply(taxa, add(decimal_digits(taxa == 0 ? 0 : taxa - 1), 1)));
  std::size_t sample_taxa_key = 2;
  for (auto const& sample_id : grammar.taxa.id_to_sample_id) {
    // Every byte may require one escape byte, followed by the delimiter.
    sample_taxa_key =
        add(sample_taxa_key, add(multiply(2, sample_id.size()), 1));
  }
  auto signature_bound = [&](std::size_t taxa_key_bytes) {
    auto key_occurrences = add(4, candidate_clades);
    key_occurrences = add(key_occurrences,
                          multiply(removed_productions, add(1, maximum_arity)));
    key_occurrences =
        add(key_occurrences,
            multiply(candidate_productions, add(1, candidate_maximum_arity)));
    auto length = add(64, multiply(key_occurrences, taxa_key_bytes));
    length = add(length,
                 multiply(add(removed_productions, candidate_productions), 3));
    return length;
  };
  auto const numeric_signature = signature_bound(numeric_taxa_key);
  auto const sample_signature = signature_bound(sample_taxa_key);
  auto const sso_capacity = std::string{}.capacity();
  auto string_capacity_bound = [&](std::size_t maximum_size) {
    if (maximum_size <= sso_capacity) return add(sso_capacity, 1);
    constexpr std::size_t allocation_quantum = 16;
    auto allocation_bytes = add(maximum_size, 1);
    allocation_bytes = add(allocation_bytes, allocation_quantum - 1);
    allocation_bytes = multiply(allocation_bytes / allocation_quantum,
                                allocation_quantum);
    return allocation_bytes;
  };
  auto const numeric_signature_capacity =
      string_capacity_bound(numeric_signature);
  auto const sample_signature_capacity =
      string_capacity_bound(sample_signature);
  std::size_t binary_signature_capacity = 0;
  if (resolved_workers > 1) {
    bool binary_signature_safely_bounded = true;
    binary_signature_capacity = chart_spr_detail::
        estimate_chart_spr_binary_taxon_dedup_key_capacity_bytes(
            taxa, candidate_clades, removed_productions, maximum_arity,
            candidate_productions, candidate_maximum_arity,
            binary_signature_safely_bounded);
    if (!binary_signature_safely_bounded) {
      throw std::overflow_error(
          "chart SPR finite binary dedup signature envelope overflow");
    }
  }
  auto const invalid_reason_capacity =
      lazy_local_invalid_reason_owned_capacity_bound();
  // Before exact verification, a ranked score owns the grammar candidate,
  // its policy-bounded invalid reason, and the (normally empty) topology
  // selector string. The numeric tie-break signature is transient and is
  // charged separately below while two comparator operands overlap.
  auto candidate_record_dynamic =
      add(candidate_dynamic, invalid_reason_capacity);
  candidate_record_dynamic =
      add(candidate_record_dynamic, string_capacity_bound(0));
  auto canonical_score_dynamic =
      string_capacity_bound(std::string_view{"composite_lower_bound"}.size());
  canonical_score_dynamic = add(
      canonical_score_dynamic,
      string_capacity_bound(std::string_view{"full_with_invariants"}.size()));
  auto canonical_record_dynamic =
      add(sample_signature_capacity, invalid_reason_capacity);
  canonical_record_dynamic =
      add(canonical_record_dynamic, canonical_score_dynamic);
  auto const no_accept_reason_capacity = string_capacity_bound(std::max(
      {std::string_view{"no candidates scored"}.size(),
       std::string_view{"no lower-bound-improving candidate"}.size(),
       std::string_view{"no valid locally scored candidates retained"}.size(),
       std::string_view{"no exact-improving verified candidate"}.size()}));
  auto const grammar_dedup_signature_capacity =
      resolved_workers > 1
          ? std::max(numeric_signature_capacity, binary_signature_capacity)
          : numeric_signature_capacity;
  auto const signature_node =
      add(sizeof(std::set<std::string>::value_type) + 4 * sizeof(void*),
          grammar_dedup_signature_capacity);
  auto sampled_dedup_signature_capacity =
      resolved_workers > 1
          ? std::max(numeric_signature_capacity, binary_signature_capacity)
          : numeric_signature_capacity;
  auto const candidate_live =
      add(sizeof(grammar_spr_candidate), candidate_dynamic);

  auto const grammar_enumerator =
      estimate_grammar_spr_enumeration_fixed_live_bytes(grammar,
                                                        state.execution_plan);
  auto const has_grammar_source =
      source != chart_spr_candidate_source::sampled_tree;
  std::size_t grammar_wave_width = 0;
  std::size_t grammar_wave_owned = 0;
  std::size_t grammar_scheduler_operation = 0;
  if (has_grammar_source &&
      scheduler.worker_resolution().resolved_workers > 1) {
    auto const workers = scheduler.worker_resolution().resolved_workers;
    grammar_wave_width = workers > (std::numeric_limits<std::size_t>::max)() / 4
                             ? (std::numeric_limits<std::size_t>::max)()
                             : workers * 4;
    if (source_options != nullptr &&
        source_options->grammar_candidate_maximum_wave_size != 0) {
      grammar_wave_width =
          std::min(grammar_wave_width,
                   source_options->grammar_candidate_maximum_wave_size);
    }
    if (grammar_candidate_wave_size != 0) {
      grammar_wave_width =
          std::min(grammar_wave_width, grammar_candidate_wave_size);
    }
    grammar_wave_width = std::max<std::size_t>(1, grammar_wave_width);

    auto one_path_dynamic = doubled_vector(
        maximum_dependency_depth, sizeof(chart_spr_detail::upward_path_step));
    one_path_dynamic =
        add(one_path_dynamic,
            multiply(maximum_dependency_depth,
                     doubled_vector(maximum_arity, sizeof(clade_id))));
    auto const descriptor_dynamic = multiply(2, one_path_dynamic);
    grammar_wave_owned = add(
        sizeof(std::vector<chart_spr_detail::grammar_spr_parallel_work_item>),
        multiply(grammar_wave_width,
                 sizeof(chart_spr_detail::grammar_spr_parallel_work_item)));
    grammar_wave_owned = add(grammar_wave_owned,
                             multiply(grammar_wave_width, descriptor_dynamic));
    grammar_wave_owned = add(
        grammar_wave_owned,
        sizeof(
            std::vector<chart_spr_detail::grammar_spr_parallel_output_slot>));
    grammar_wave_owned = add(
        grammar_wave_owned,
        multiply(grammar_wave_width,
                 add(sizeof(chart_spr_detail::grammar_spr_parallel_output_slot),
                     add(candidate_dynamic, std::size_t{512}))));

    auto const grammar_plan = scheduler.plan_indexed_ranges(
        grammar_wave_width,
        {.minimum_grain = 1, .target_ranges_per_worker = 1});
    if (grammar_plan.range_count > 1 && grammar_plan.worker_task_limit > 1) {
      grammar_scheduler_operation =
          estimate_chart_scheduler_operation_peak_bytes(grammar_plan);
    }
  }
  auto const one_dedup_set = multiply(candidate_limit, signature_node);
  sampled_tree_source_wave_memory_estimate sampled_wave;
  if (source != chart_spr_candidate_source::grammar) {
    if (source_options == nullptr ||
        source_options->sampled_tree_source_dag == nullptr) {
      throw std::invalid_argument(
          "chart SPR finite iteration envelope: sampled/hybrid source "
          "requires a source DAG");
    }
    sampled_wave =
        chart_spr_detail::estimate_sampled_tree_source_memory_bound(
            grammar, *source_options->sampled_tree_source_dag, &scheduler,
            source_wave_size, projection_wave_size);
    if (!sampled_wave.safely_bounded) {
      throw std::overflow_error(
          "chart SPR finite sampled source-wave shape overflow");
    }
    if (resolved_workers > 1) {
      sampled_dedup_signature_capacity = std::max(
          sampled_dedup_signature_capacity,
          sampled_wave
              .planned_retained_taxon_signature_bytes_per_slot);
    }
  }
  auto const sampled_dedup_signature_node =
      add(sizeof(std::set<std::string>::value_type) + 4 * sizeof(void*),
          sampled_dedup_signature_capacity);
  std::size_t sampled_waiting = 0;
  if (source != chart_spr_candidate_source::grammar) {
    sampled_waiting = add(sampled_waiting,
                          sampled_wave.sampled_tree_resident_bytes);
    sampled_waiting = add(sampled_waiting, sampled_wave.prepared_owned_bytes);
    sampled_waiting = add(sampled_waiting, sampled_wave.source_order_bytes);
    sampled_waiting = add(sampled_waiting,
                          sampled_wave.source_slot_and_move_bytes);
    sampled_waiting = add(sampled_waiting,
                          sampled_wave.projection_job_bytes);
    sampled_waiting = add(sampled_waiting,
                          sampled_wave.projection_completion_bytes);
    sampled_waiting = add(sampled_waiting,
                          sampled_wave.projection_slot_and_payload_bytes);
    sampled_waiting = add(
        sampled_waiting,
        sampled_wave.projection_stable_slot_workspace_bytes);
    sampled_waiting = add(sampled_waiting,
                          sampled_wave.error_and_exception_bytes);
  }
  // A hybrid child clears its own post-dedup cap so duplicates of candidates
  // emitted by the earlier child do not stop the combined stream too soon.
  // Its `seen` identity nevertheless has exactly the same equality relation
  // as the outer hybrid taxon signature, and the outer callback stops the
  // child synchronously at the global cap. Every child-unique equality class
  // therefore either belongs to the outer set at child entry or grows that
  // set; the two disjoint groups contain at most `candidate_limit` classes in
  // total. Structural source and path-pair visit bounds count
  // filtered/duplicate visits and are not a retained child-dedup bound.
  auto const sampled_child_signature_count = candidate_limit;
  auto const grammar_child_signature_count = candidate_limit;
  auto const sampled_child_dedup =
      multiply(sampled_child_signature_count,
               sampled_dedup_signature_node);
  auto const grammar_child_dedup =
      multiply(grammar_child_signature_count, signature_node);
  std::size_t source_owned = 0;
  switch (source) {
    case chart_spr_candidate_source::grammar:
      source_owned =
          add(grammar_wave_owned, add(grammar_enumerator, grammar_child_dedup));
      break;
    case chart_spr_candidate_source::sampled_tree:
      source_owned = add(sampled_waiting, sampled_child_dedup);
      break;
    case chart_spr_candidate_source::hybrid:
      // The outer emitted set persists across both sequential child streams.
      // Sampled and grammar child ownership are temporal alternatives.
      source_owned =
          add(one_dedup_set,
              std::max(add(sampled_waiting, sampled_child_dedup),
                       add(grammar_wave_owned,
                           add(grammar_enumerator, grammar_child_dedup))));
      break;
  }
  std::size_t future = source_owned;

  // One candidate is being assembled and signed before it can reach the batch
  // callback. Four candidate payloads cover the published candidate, temp
  // lookup keys, path/group temporaries, and vector old+new growth. Eight
  // encoded strings cover ostringstream storage, normalized taxa/signature
  // lists, and final-string publication on frozen libstdc++.
  auto candidate_construction_peak =
      add(sizeof(grammar_spr_candidate), multiply(4, candidate_dynamic));
  candidate_construction_peak =
      add(candidate_construction_peak,
          multiply(candidate_clades,
                   sizeof(std::pair<std::vector<taxon_id> const, clade_id>) +
                       4 * sizeof(void*)));
  auto signature_construction_peak =
      multiply(8, std::max({numeric_signature_capacity,
                            sample_signature_capacity,
                            binary_signature_capacity}));
  signature_construction_peak =
      add(signature_construction_peak,
          doubled_vector(add(removed_productions, candidate_productions),
                         sizeof(std::string)));
  auto const serial_construction_peak =
      add(candidate_construction_peak, signature_construction_peak);
  auto enumeration_callback_concurrent = serial_construction_peak;
  if (source != chart_spr_candidate_source::grammar) {
    enumeration_callback_concurrent =
        add(enumeration_callback_concurrent,
            sampled_wave.source_construction_scratch_bytes);
  }

  // Candidate/copy slots retain high-water nested payload across one batch.
  // Ranked and canonical records retain across every batch. The accepted-copy
  // allowance mirrors the exact-loop resident walker used at each callback.
  auto const candidate_buffer_nested =
      multiply(multiply(2, candidate_batch_size), candidate_dynamic);
  future =
      add(future, multiply(candidate_buffer_count, candidate_buffer_nested));
  future = add(future, multiply(candidate_batch_size, invalid_reason_capacity));
  future = add(future, multiply(ranked_limit, candidate_record_dynamic));
  future = add(future, candidate_record_dynamic);
  future = add(future, no_accept_reason_capacity);
  future = add(future, sample_signature_capacity);
  chart_spr_canonical_exact_evidence_memory_estimate
      canonical_state_exact_evidence;
  std::size_t ranked_candidate_exact_evidence_bytes = 0;
  if (capture_semantics) {
    future = add(future, multiply(candidate_limit, canonical_record_dynamic));
    if (state.exact_trim_active_only) {
      canonical_state_exact_evidence =
          estimate_chart_spr_canonical_exact_evidence_memory(
              grammar, *state.exact_trim_active_only);
      // Evidence publication is deferred until enumeration/local/exact
      // scratch has been released. Its construction peak belongs to the
      // post-release phase below, not this generation-phase future reserve.
    }
  }

  auto acceptance_outer = sizeof(chart_spr_acceptance_iteration_workspace) -
                          sizeof(chart_spr_local_score_workspace);
  auto candidate_buffer_owned =
      doubled_vector(candidate_batch_size, sizeof(grammar_spr_candidate));
  candidate_buffer_owned =
      add(candidate_buffer_owned,
          doubled_vector(candidate_batch_size,
                         sizeof(grammar_spr_candidate_copy_scratch)));
  candidate_buffer_owned = add(candidate_buffer_owned, candidate_buffer_nested);
  acceptance_outer =
      add(acceptance_outer,
          multiply(candidate_buffer_count,
                   candidate_buffer_owned - candidate_buffer_nested));
  acceptance_outer = add(acceptance_outer,
                         doubled_vector(candidate_batch_size,
                                        sizeof(chart_spr_local_score_result)));
  std::size_t pipeline_control = 0;
  if (include_pipeline_control) {
    // The native coordinator stack/TLS follows the scheduler-worker contract:
    // it is excluded from chart bytes and governed by the RSS gate. Every
    // coordinator-controlled object and conservative error/stop-state heap is
    // charged here.
    pipeline_control = add(sizeof(chart_spr_candidate_pipeline_controller) +
                               sizeof(std::jthread) + sizeof(std::stop_source),
                           3072);
  }

  auto exact_input_outer = sizeof(std::vector<chart_spr_candidate_score>) +
                           sizeof(chart_spr_iteration_result);
  exact_input_outer =
      add(exact_input_outer,
          doubled_vector(ranked_limit, sizeof(chart_spr_candidate_score)));
  exact_input_outer = add(exact_input_outer,
                          doubled_vector(candidate_limit, sizeof(std::size_t)));
  if (capture_semantics) {
    exact_input_outer =
        add(exact_input_outer,
            doubled_vector(candidate_limit,
                           sizeof(chart_spr_canonical_candidate_record)));
    // Built after enumeration in ranked order. It is an independent owning
    // result vector, not part of the canonical-candidate outer reserve.
    exact_input_outer = add(exact_input_outer,
                            doubled_vector(ranked_limit, sizeof(std::size_t)));
  }

  auto const lazy_local =
      state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart;
  auto const persistent_row_view = state.local_commit_inside_rows.valid();
  auto const local_prepared_slots =
      lazy_local ? local_task_slots
                 : std::max(candidate_batch_size, resolved_workers);
  auto const local_worker_slots =
      lazy_local ? local_task_slots : resolved_workers;
  auto const weighted_candidate_order =
      lazy_local ? std::size_t{0}
                 : doubled_vector(candidate_batch_size, sizeof(std::size_t));
  auto const pattern_count = state.active_patterns.patterns.patterns.size();
  std::size_t local_tile_result_slots = 0;
  std::size_t maximum_all_active_tile_items = 0;
  std::size_t maximum_pattern_batch_items = 0;
  if ((state.cache_strategy == chart_spr_cache_strategy::all_active_patterns ||
       persistent_row_view) &&
      resolved_workers > 1 && pattern_count > 1) {
    auto const target_total = multiply(resolved_workers, 4);
    auto const maximum_tiled_batch =
        std::min(candidate_batch_size, resolved_workers - 1);
    for (std::size_t batch = 1; batch <= maximum_tiled_batch; ++batch) {
      auto const target_per_candidate =
          std::max<std::size_t>(1, target_total / batch);
      auto const pattern_grain =
          add(pattern_count, target_per_candidate - 1) / target_per_candidate;
      auto const tiles_per_candidate =
          add(pattern_count, pattern_grain - 1) / pattern_grain;
      auto const tile_items = multiply(batch, tiles_per_candidate);
      local_tile_result_slots = std::max(local_tile_result_slots, tile_items);
      maximum_all_active_tile_items =
          std::max(maximum_all_active_tile_items, tile_items);
    }
  } else if (state.cache_strategy ==
                 chart_spr_cache_strategy::pattern_batches &&
             !persistent_row_view) {
    auto const maximum_batch_patterns =
        std::min(pattern_count,
                 std::max<std::size_t>(1, state.effective_pattern_batch_size));
    maximum_pattern_batch_items = maximum_batch_patterns;
    if (resolved_workers > 1 && maximum_batch_patterns > 1) {
      auto const fusion_plan = scheduler.plan_indexed_ranges(
          maximum_batch_patterns,
          chart_spr_phase4_pattern_range_options(maximum_batch_patterns,
                                                 resolved_workers));
      if (fusion_plan.range_count > 1) {
        local_tile_result_slots =
            multiply(std::min(candidate_batch_size, resolved_workers - 1),
                     maximum_batch_patterns);
      }
    }
  }
  auto const pattern_batch_construction_scratch =
      state.cache_strategy == chart_spr_cache_strategy::pattern_batches &&
              !persistent_row_view
          ? add(doubled_vector(maximum_pattern_batch_items,
                               sizeof(std::exception_ptr)),
                doubled_vector(maximum_pattern_batch_items,
                               sizeof(std::uint8_t)))
          : std::size_t{0};

  auto local_workspace = sizeof(chart_spr_local_score_workspace);
  local_workspace = add(local_workspace,
                        doubled_vector(local_prepared_slots,
                                       sizeof(prepared_local_candidate_score)));
  local_workspace = add(
      local_workspace,
      doubled_vector(local_worker_slots, sizeof(local_score_worker_workspace)));
  local_workspace = add(
      local_workspace,
      doubled_vector(local_tile_result_slots, sizeof(local_score_tile_result)));

  std::size_t scheduler_operation = 0;
  auto observe_scheduler_operation = [&](std::size_t item_count,
                                         chart_indexed_range_options options) {
    if (item_count == 0) return;
    scheduler_operation =
        std::max(scheduler_operation,
                 estimate_chart_scheduler_operation_peak_bytes(
                     scheduler.plan_indexed_ranges(item_count, options)));
  };
  if (lazy_local) {
    observe_scheduler_operation(
        local_task_slots, {.minimum_grain = 1, .target_ranges_per_worker = 1});
  } else {
    scheduler_operation =
        std::max(scheduler_operation,
                 estimate_chart_spr_scheduler_operation_peak_for_any_items(
                     scheduler, candidate_batch_size,
                     {.minimum_grain = 1, .target_ranges_per_worker = 32}));
    scheduler_operation =
        std::max(scheduler_operation,
                 estimate_chart_spr_scheduler_operation_peak_for_any_items(
                     scheduler, maximum_all_active_tile_items,
                     {.minimum_grain = 1, .target_ranges_per_worker = 4}));
    scheduler_operation =
        std::max(scheduler_operation,
                 estimate_chart_spr_scheduler_operation_peak_for_any_items(
                     scheduler, maximum_pattern_batch_items,
                     {.minimum_grain = 1, .target_ranges_per_worker = 4}));
  }
  auto const scheduler_resident =
      estimate_chart_spr_scheduler_resident_bytes(scheduler);
  std::size_t local_task_peak = 0;
  std::size_t untiled_concurrent_preparation_peak = 0;
  std::size_t local_retained_stable_capacity = 0;
  std::size_t lazy_local_reusable_stable_capacity = 0;
  if (lazy_local) {
    lazy_local_reusable_stable_capacity =
        multiply(local_task_slots, local_task.stable_dynamic_capacity_bytes);
    auto const stable_wave = add(
        multiply(local_task_slots, local_task.stable_dynamic_capacity_bytes),
        scheduler_operation);
    auto preparation_wave = local_task.preparation_peak_dynamic_capacity_bytes;
    if (local_task_slots > 1) {
      preparation_wave = add(
          preparation_wave, multiply(local_task_slots - 1,
                                     local_task.stable_dynamic_capacity_bytes));
    }
    local_task_peak = std::max(stable_wave, preparation_wave);
  } else {
    auto const tile_result_dynamic =
        multiply(local_tile_result_slots, invalid_reason_capacity);
    auto const stable_descriptors =
        multiply(local_prepared_slots,
                 local_task.descriptor_stable_dynamic_capacity_bytes);
    auto const stable_workers = multiply(
        local_worker_slots, local_task.worker_stable_dynamic_capacity_bytes);
    local_retained_stable_capacity = add(stable_descriptors, stable_workers);
    local_retained_stable_capacity =
        add(local_retained_stable_capacity, tile_result_dynamic);
    auto stable_wave = add(stable_descriptors, stable_workers);
    stable_wave = add(stable_wave, tile_result_dynamic);
    stable_wave = add(stable_wave, weighted_candidate_order);
    stable_wave = add(stable_wave, pattern_batch_construction_scratch);
    stable_wave = add(stable_wave, scheduler_operation);

    auto descriptor_preparation =
        add(local_task.descriptor_preparation_peak_dynamic_capacity_bytes,
            local_prepared_slots > 1
                ? multiply(local_prepared_slots - 1,
                           local_task.descriptor_stable_dynamic_capacity_bytes)
                : 0);
    descriptor_preparation = add(descriptor_preparation, stable_workers);
    descriptor_preparation = add(descriptor_preparation, tile_result_dynamic);
    descriptor_preparation =
        add(descriptor_preparation, weighted_candidate_order);

    auto worker_preparation = add(
        stable_descriptors,
        multiply(local_worker_slots,
                 local_task.worker_preparation_peak_dynamic_capacity_bytes));
    worker_preparation = add(worker_preparation, tile_result_dynamic);
    worker_preparation = add(worker_preparation, weighted_candidate_order);
    worker_preparation =
        add(worker_preparation, pattern_batch_construction_scratch);
    worker_preparation = add(worker_preparation, scheduler_operation);

    if (state.cache_strategy == chart_spr_cache_strategy::all_active_patterns ||
        persistent_row_view) {
      // Untiled all-active batches prepare the descriptor and row scratch
      // inside each worker task.  Up to min(B,W) workers can therefore own
      // both old+new preparation envelopes simultaneously.  Prepared/worker
      // slots not active in a short batch may still retain a prior stable
      // high-water, as may tile-result slots from another batch shape.
      auto const active_slots =
          std::min(candidate_batch_size, local_worker_slots);
      auto const active_slot_preparation =
          add(local_task.descriptor_preparation_peak_dynamic_capacity_bytes,
              local_task.worker_preparation_peak_dynamic_capacity_bytes);
      untiled_concurrent_preparation_peak =
          multiply(active_slots, active_slot_preparation);
      untiled_concurrent_preparation_peak =
          add(untiled_concurrent_preparation_peak,
              multiply(local_prepared_slots - active_slots,
                       local_task.descriptor_stable_dynamic_capacity_bytes));
      untiled_concurrent_preparation_peak =
          add(untiled_concurrent_preparation_peak,
              multiply(local_worker_slots - active_slots,
                       local_task.worker_stable_dynamic_capacity_bytes));
      untiled_concurrent_preparation_peak =
          add(untiled_concurrent_preparation_peak, tile_result_dynamic);
      untiled_concurrent_preparation_peak =
          add(untiled_concurrent_preparation_peak, weighted_candidate_order);
      untiled_concurrent_preparation_peak =
          add(untiled_concurrent_preparation_peak, scheduler_operation);
    }
    local_task_peak =
        std::max({stable_wave, descriptor_preparation, worker_preparation,
                  untiled_concurrent_preparation_peak});
  }
  // Tree construction and canonical gather are serial producer work and can
  // overlap scoring. Source enumeration, projection, and joined parallel
  // postprocessing all use the one scheduler handoff, so their operations are
  // temporal alternatives to each other and to score operations. The
  // persistent scheduler core is charged exactly once by generation_phase.
  auto const serial_overlap_peak =
      add(enumeration_callback_concurrent, local_task_peak);
  auto const source_active_peak =
      add(sampled_wave.active_enumeration_scratch_bytes,
          sampled_wave.source_scheduler_operation_bytes);
  auto const projection_active_peak =
      add(sampled_wave.active_projection_scratch_bytes,
          sampled_wave.projection_scheduler_operation_bytes);
  auto const postprocessing_active_peak =
      add(sampled_wave.active_postprocessing_scratch_bytes,
          sampled_wave.postprocessing_scheduler_operation_bytes);
  // A completed dense score retains descriptor, worker, and tile nested
  // capacities in the reusable local workspace. Later source-enumeration and
  // projection stages both overlap that stable HWM. Finite lazy waves use the
  // cold-release envelope by default; the additive reusable envelope below
  // admits the same overlap only when the full iteration has enough headroom.
  auto const generation_stage_peak =
      std::max({source_active_peak, projection_active_peak,
                postprocessing_active_peak, grammar_scheduler_operation});
  auto const generation_wave_with_retained_local_peak =
      add(generation_stage_peak, local_retained_stable_capacity);
  auto saturating_add = [](std::size_t lhs, std::size_t rhs) noexcept {
    return rhs > (std::numeric_limits<std::size_t>::max)() - lhs
               ? (std::numeric_limits<std::size_t>::max)()
               : lhs + rhs;
  };
  auto const generation_wave_with_reusable_lazy_local_peak = saturating_add(
      generation_stage_peak,
      saturating_add(local_retained_stable_capacity,
                     lazy_local_reusable_stable_capacity));
  auto const sampled_source_admitted_peak =
      source == chart_spr_candidate_source::grammar
          ? std::size_t{0}
          : sampled_wave.required_peak_bytes -
                sampled_wave.source_construction_scratch_bytes;
  auto const generation_transient_peak =
      std::max(serial_overlap_peak, generation_wave_with_retained_local_peak);
  auto const reusable_generation_transient_peak = std::max(
      serial_overlap_peak, generation_wave_with_reusable_lazy_local_peak);
  auto const reusable_future =
      saturating_add(future, reusable_generation_transient_peak);
  future = add(future, generation_transient_peak);
  auto generation_fixed =
      add(published_state_resident(), acceptance_outer);
  generation_fixed = add(generation_fixed, scheduler_resident);
  generation_fixed = add(generation_fixed, pipeline_control);
  generation_fixed = add(generation_fixed, exact_input_outer);
  generation_fixed = add(generation_fixed, local_workspace);
  auto const generation_phase = add(generation_fixed, future);
  auto const reusable_generation_phase =
      saturating_add(generation_fixed, reusable_future);

  // After enumeration and exact aggregation, ranked/task/workspace storage is
  // explicitly released. Keep only the result capacities that survive into
  // canonical old-state evidence publication. Evidence construction includes
  // its eventual retained payload and therefore composes with, rather than
  // adds to, the earlier generation high-water phase.
  auto post_release_result = sizeof(chart_spr_acceptance_iteration_workspace) +
                             sizeof(std::vector<chart_spr_candidate_score>) +
                             sizeof(chart_spr_iteration_result);
  post_release_result =
      add(post_release_result,
          doubled_vector(candidate_limit, sizeof(std::size_t)));
  post_release_result =
      add(post_release_result, doubled_vector(ranked_limit, sizeof(double)));
  post_release_result = add(post_release_result, candidate_record_dynamic);
  post_release_result = add(post_release_result, no_accept_reason_capacity);
  post_release_result = add(post_release_result, sample_signature_capacity);
  if (capture_semantics) {
    post_release_result =
        add(post_release_result,
            doubled_vector(candidate_limit,
                           sizeof(chart_spr_canonical_candidate_record)));
    post_release_result =
        add(post_release_result,
            multiply(candidate_limit, canonical_record_dynamic));
    // Ranked-stream indices and exact-verified stream indices are distinct
    // owning result vectors retained through evidence publication.
    post_release_result =
        add(post_release_result,
            doubled_vector(ranked_limit, 2 * sizeof(std::size_t)));
    post_release_result =
        add(post_release_result, ranked_candidate_exact_evidence_bytes);
  }
  auto evidence_phase =
      add(published_state_resident(), scheduler_resident);
  evidence_phase = add(evidence_phase, post_release_result);
  evidence_phase = add(evidence_phase,
                       canonical_state_exact_evidence.construction_peak_bytes);
  auto const planned = std::max(generation_phase, evidence_phase);
  auto const reusable_planned =
      std::max(reusable_generation_phase, evidence_phase);
  return grammar_spr_finite_iteration_memory_envelope{
      .planned_required_bytes = planned,
      .planned_generation_phase_required_bytes = generation_phase,
      .planned_evidence_phase_required_bytes = evidence_phase,
      .future_dynamic_bytes = future,
      .planned_lazy_local_reusable_required_bytes = reusable_planned,
      .planned_lazy_local_reusable_generation_phase_required_bytes =
          reusable_generation_phase,
      .planned_lazy_local_reusable_future_dynamic_bytes = reusable_future,
      .planned_lazy_local_reusable_stable_capacity_bytes =
          lazy_local_reusable_stable_capacity,
      .planned_post_release_result_bytes = post_release_result,
      .planned_ranked_candidate_exact_evidence_bytes =
          ranked_candidate_exact_evidence_bytes,
      .planned_accepted_candidate_signature_bytes = sample_signature_capacity,
      .planned_local_workspace_resident_bytes = local_workspace,
      .planned_signature_node_bytes = signature_node,
      .planned_candidate_live_bytes = candidate_live,
      .planned_canonical_record_dynamic_bytes =
          capture_semantics ? canonical_record_dynamic : 0,
      .planned_canonical_state_exact_evidence_resident_bytes =
          canonical_state_exact_evidence.retained_bytes,
      .planned_canonical_state_exact_evidence_construction_peak_bytes =
          canonical_state_exact_evidence.construction_peak_bytes,
      .planned_scheduler_operation_peak_bytes = scheduler_operation,
      .planned_scheduler_resident_bytes = scheduler_resident,
      .planned_local_task_stable_bytes =
          local_task.stable_dynamic_capacity_bytes,
      .planned_local_task_preparation_peak_bytes =
          local_task.preparation_peak_dynamic_capacity_bytes,
      .planned_local_weighted_candidate_order_bytes = weighted_candidate_order,
      .planned_pattern_batch_construction_scratch_bytes =
          pattern_batch_construction_scratch,
      .planned_local_untiled_concurrent_preparation_peak_bytes =
          untiled_concurrent_preparation_peak,
      .planned_local_retained_stable_capacity_bytes =
          local_retained_stable_capacity,
      .planned_generation_wave_with_retained_local_peak_bytes =
          generation_wave_with_retained_local_peak,
      .planned_cache_strategy = state.cache_strategy,
      .planned_candidate_batch_size = candidate_batch_size,
      .planned_local_prepared_slots = local_prepared_slots,
      .planned_local_worker_slots = local_worker_slots,
      .planned_local_tile_result_slots = local_tile_result_slots,
      .planned_source_owned_bytes = source_owned,
      .planned_enumeration_callback_concurrent_bytes =
          enumeration_callback_concurrent,
      .planned_candidate_buffer_owned_bytes = candidate_buffer_owned,
      .planned_pipeline_control_bytes = pipeline_control,
      .planned_sampled_source_waiting_bytes = sampled_waiting,
      .planned_sampled_dedup_signature_node_bytes =
          source == chart_spr_candidate_source::grammar
              ? std::size_t{0}
              : sampled_dedup_signature_node,
      .planned_sampled_source_active_scratch_bytes =
          sampled_wave.active_enumeration_scratch_bytes,
      .planned_sampled_source_scheduler_operation_peak_bytes =
          sampled_wave.source_scheduler_operation_bytes,
      .planned_sampled_projection_active_scratch_bytes =
          sampled_wave.active_projection_scratch_bytes,
      .planned_sampled_projection_scheduler_operation_peak_bytes =
          sampled_wave.projection_scheduler_operation_bytes,
      .planned_sampled_postprocessing_active_scratch_bytes =
          sampled_wave.active_postprocessing_scratch_bytes,
      .planned_sampled_postprocessing_scheduler_operation_peak_bytes =
          sampled_wave.postprocessing_scheduler_operation_bytes,
      .planned_sampled_source_admitted_peak_bytes =
          sampled_source_admitted_peak,
      .planned_sampled_source_count_bound = sampled_wave.source_count,
      .planned_sampled_destination_bound_per_source =
          sampled_wave.destination_bound_per_source,
      .planned_sampled_source_wave_size = sampled_wave.source_wave_size,
      .planned_sampled_projection_wave_size = sampled_wave.projection_wave_size,
      .planned_grammar_candidate_wave_owned_bytes = grammar_wave_owned,
      .planned_grammar_candidate_scheduler_operation_peak_bytes =
          grammar_scheduler_operation,
      .planned_grammar_candidate_admitted_wave_bytes = grammar_wave_owned,
      .planned_grammar_candidate_wave_size = grammar_wave_width,
      .candidate_buffer_count = candidate_buffer_count,
  };
}

std::size_t
chart_spr_search_detail::estimate_exact_loop_estimator_peak_scratch_bytes(
    chart_spr_search_state const& state,
    std::span<chart_spr_candidate_score const> ranked) {
  std::size_t base_child_occurrence_count = 0;
  for (auto const& production : state.grammar.productions) {
    base_child_occurrence_count = chart_spr_checked_cache_bytes_add(
        base_child_occurrence_count, production.children.size(),
        "chart SPR exact estimator base-child count overflow");
  }

  std::size_t peak = 0;
  for (auto const& candidate : ranked) {
    // Invalid candidates bypass the estimator in the production loop.
    if (!candidate.valid) continue;
    peak = std::max(
        peak, chart_spr_candidate_estimator_scratch_bytes(
                  state, candidate, base_child_occurrence_count));
  }
  return peak;
}

chart_spr_topology_selection_memory_estimate
estimate_chart_spr_topology_selection_memory(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_options const& options) {
  chart_spr_topology_selection_memory_estimate estimate;
  if (!candidate.valid || options.acceptance_mode !=
                              chart_spr_acceptance_mode::fixed_topology_exact) {
    return estimate;
  }

  try {
    auto topology = chart_spr_candidate_topology_bound(
        state, candidate.candidate,
        (std::numeric_limits<std::size_t>::max)());
    auto structural = chart_spr_estimate_candidate_structural_bytes(
        state, candidate.candidate, topology);
    auto key_bytes = chart_spr_virtual_key_bytes(state, topology);
    auto taxon_slots = chart_spr_saturating_add(
        chart_spr_saturating_multiply(topology.production_count, 2),
        chart_spr_saturating_multiply(topology.child_occurrence_count, 2));
    taxon_slots = chart_spr_saturating_multiply(
        taxon_slots, state.grammar.taxa.id_to_sample_id.size());

    auto retained = chart_spr_saturating_add(
        {sizeof(chart_spr_topology_selection) +
             sizeof(chart_spr_topology_certificate) + 256,
         false},
        key_bytes);
    retained = chart_spr_saturating_add(
        retained,
        chart_spr_saturating_multiply(
            topology.production_count,
            2 * sizeof(overlay_production_ref) +
                2 * sizeof(chart_spr_production_signature) +
                8 * sizeof(void*)));
    retained = chart_spr_saturating_add(
        retained,
        chart_spr_saturating_multiply(taxon_slots, 2 * sizeof(taxon_id)));

    // Built-in selection owns recursive state arrays, ordered maps/sets and
    // signature construction temporaries while the complete certificate is
    // already resident.  The factors model frozen-libstdc++ node/control and
    // geometric-growth overhead, not allocator-independent RSS.
    auto scratch = chart_spr_saturating_multiply(structural, 2);
    scratch = chart_spr_saturating_add(
        scratch,
        chart_spr_saturating_multiply(
            topology.clade_count,
            8 * sizeof(std::pair<overlay_clade_ref,
                                 overlay_production_ref>) +
                32 * sizeof(void*)));
    scratch = chart_spr_saturating_add(
        scratch,
        chart_spr_saturating_multiply(
            topology.production_count,
            8 * sizeof(overlay_production_ref) + 32 * sizeof(void*)));

    if (options.topology_selection_provider) {
      if (!options.topology_selection_additional_memory_estimator) {
        estimate.safely_bounded = false;
      } else {
        auto additional =
            options.topology_selection_additional_memory_estimator(
                state, candidate.candidate);
        scratch = chart_spr_saturating_add(scratch,
                                           additional.scratch_bytes);
        retained = chart_spr_saturating_add(
            retained, additional.retained_result_bytes);
        estimate.safely_bounded = additional.safely_bounded;
      }
    }
    estimate.scratch_bytes = scratch.value;
    estimate.retained_result_bytes = retained.value;
    estimate.safely_bounded = estimate.safely_bounded &&
                              !topology.saturated && !structural.saturated &&
                              !key_bytes.saturated && !taxon_slots.saturated &&
                              !scratch.saturated && !retained.saturated;
  } catch (std::overflow_error const&) {
    estimate.scratch_bytes = (std::numeric_limits<std::size_t>::max)();
    estimate.retained_result_bytes =
        (std::numeric_limits<std::size_t>::max)();
    estimate.safely_bounded = false;
  }
  return estimate;
}

chart_spr_exact_candidate_memory_estimate
estimate_chart_spr_exact_candidate_memory(
    chart_spr_search_state const& state,
    chart_spr_candidate_score const& candidate,
    chart_spr_search_options const& options, std::size_t resolved_workers) {
  chart_spr_exact_candidate_memory_estimate estimate;
  try {
    auto const pattern_count = state.active_patterns.patterns.patterns.size();
    auto const taxon_count = state.grammar.taxa.id_to_sample_id.size();
    bool safely_bounded = true;
    chart_spr_saturating_size custom_retained;

    auto cost_component_count =
        chart_spr_saturating_multiply(pattern_count, nuc_state_count);
    auto cost_bytes =
        chart_spr_saturating_multiply(cost_component_count, sizeof(chart_cost));
    auto minimum_entry_bytes =
        chart_spr_saturating_add({sizeof(frontier_entry), false}, cost_bytes);
    safely_bounded = safely_bounded && !minimum_entry_bytes.saturated;
    minimum_entry_bytes.value =
        std::max<std::size_t>(1, minimum_entry_bytes.value);
    auto topology_limit =
        options.cache.memory_budget_bytes == 0
            ? (std::numeric_limits<std::size_t>::max)() /
                  minimum_entry_bytes.value
            : options.cache.memory_budget_bytes / minimum_entry_bytes.value;
    if (topology_limit != (std::numeric_limits<std::size_t>::max)()) {
      ++topology_limit;
    }
    topology_limit = std::max<std::size_t>(1, topology_limit);
    auto topology = chart_spr_candidate_topology_bound(
        state, candidate.candidate, topology_limit);
    auto structural = chart_spr_estimate_candidate_structural_bytes(
        state, candidate.candidate, topology);
    auto const row_bytes = sizeof(chart_multisite_detail::chart_row);

    if (options.acceptance_mode ==
        chart_spr_acceptance_mode::fixed_topology_exact) {
      auto selected = chart_spr_saturating_size{
          chart_spr_estimate_selected_topology_cache_admission_bytes(
              state, candidate.candidate),
          false};
      // The finalized selected-row cache is not the construction high-water:
      // recursive selection simultaneously owns copied taxa, before/after
      // maps and reached sets, signature/key vectors, and one active child-row
      // chain.  Bound that frozen-libstdc++ transient surface separately and
      // take the larger peak.
      auto selected_construction = chart_spr_saturating_add(
          selected, chart_spr_saturating_multiply(structural, 2));
      selected_construction = chart_spr_saturating_add(
          selected_construction, chart_spr_virtual_key_bytes(state, topology));
      selected_construction = chart_spr_saturating_add(
          selected_construction,
          chart_spr_saturating_multiply(
              topology.production_count,
              8 * sizeof(std::pair<overlay_production_ref,
                                   overlay_production_ref>) +
                  32 * sizeof(void*)));
      selected_construction = chart_spr_saturating_add(
          selected_construction,
          chart_spr_saturating_multiply(
              topology.clade_count,
              8 * sizeof(std::pair<overlay_clade_ref,
                                   overlay_production_ref>) +
                  32 * sizeof(void*)));
      selected = chart_spr_saturating_max(selected, selected_construction);
      auto per_worker_direct = chart_spr_saturating_multiply(
          topology.clade_count,
          2 * row_bytes + 4 * sizeof(std::size_t) + 8 * sizeof(void*));
      auto const direct_scratch_object_bytes =
          sizeof(
              std::vector<std::optional<chart_multisite_detail::chart_row>>) +
          sizeof(chart_spr_restricted_overlay_topology_row_scratch);
      auto direct_serial = chart_spr_saturating_add(
          per_worker_direct, direct_scratch_object_bytes);
      auto direct_inner = chart_spr_saturating_multiply(
          per_worker_direct,
          std::min(resolved_workers, std::max<std::size_t>(1, pattern_count)));
      direct_inner = chart_spr_saturating_add(
          direct_inner,
          chart_spr_saturating_multiply(
              resolved_workers,
              sizeof(chart_spr_search_counters) + direct_scratch_object_bytes));

      // Cache, cache-copy, direct-oracle and optional materialized-oracle
      // results can retain eight old/new per-pattern score arrays at once.
      auto score_arrays = chart_spr_saturating_multiply(
          pattern_count, 8 * sizeof(std::uint64_t));
      if (options.verify_fixed_topology_materialized_oracle_for_tests ||
          options.force_fixed_topology_independent_sm_bug_for_tests) {
        direct_serial = chart_spr_saturating_add(direct_serial, structural);
        direct_inner = chart_spr_saturating_add(direct_inner, structural);
      }
      auto serial = chart_spr_saturating_add(
          score_arrays, chart_spr_saturating_max(selected, direct_serial));
      auto inner = chart_spr_saturating_add(
          score_arrays, chart_spr_saturating_max(selected, direct_inner));
      if (state.fixed_topology_exact_verifier ||
          state.contextual_fixed_topology_exact_verifier) {
        if (!state.fixed_topology_exact_additional_memory_estimator ||
            !state
                 .fixed_topology_exact_additional_retained_memory_estimator) {
          safely_bounded = false;
        } else {
          auto additional = chart_spr_saturating_size{
              state.fixed_topology_exact_additional_memory_estimator(
                  candidate.candidate),
              false};
          serial = chart_spr_saturating_add(serial, additional);
          inner = chart_spr_saturating_add(inner, additional);
          custom_retained = chart_spr_saturating_size{
              state
                  .fixed_topology_exact_additional_retained_memory_estimator(
                      candidate.candidate),
              false};
        }
      }
      estimate.serial_scratch_bytes = serial.value;
      estimate.inner_parallel_scratch_bytes = inner.value;
      safely_bounded = safely_bounded && !serial.saturated && !inner.saturated;
      if (options.verify_fixed_topology_materialized_oracle_for_tests ||
          options.force_fixed_topology_independent_sm_bug_for_tests) {
        safely_bounded = safely_bounded && !structural.saturated;
      }
    } else {
      auto const mask_bytes =
          chart_spr_bit_capacity_bytes(topology.production_count);
      auto entry_bytes = chart_spr_saturating_add(
          chart_spr_saturating_multiply(sizeof(frontier_entry), 4),
          chart_spr_saturating_multiply(cost_bytes, 2));
      entry_bytes = chart_spr_saturating_add(entry_bytes, mask_bytes);
      entry_bytes = chart_spr_saturating_add(
          entry_bytes, sizeof(frontier_provenance_choice) + 16 * sizeof(void*));
      auto frontier = chart_spr_saturating_multiply(
          topology.total_frontier_entries, entry_bytes.value);
      frontier.saturated = frontier.saturated || entry_bytes.saturated;
      // One frontier build owns up to C level diagnostics while the trim
      // result pre-reserves one retained diagnostic per level/pass.  The plan
      // overload reserves the complete result surface before pass one, so the
      // explicit peak factor is 2C for one pass and 3C for two passes.
      auto const diagnostic_peak_factor =
          options.exact_trim.dominance_mode ==
                  multisite_dominance_mode::two_pass_exact_mask
              ? std::size_t{3}
              : std::size_t{2};
      frontier = chart_spr_saturating_add(
          frontier,
          chart_spr_saturating_multiply(
              topology.clade_count,
              sizeof(std::vector<frontier_entry>) + sizeof(std::size_t) +
                  diagnostic_peak_factor *
                      sizeof(multisite_frontier_level_diagnostic) +
                  sizeof(std::exception_ptr)));
      auto combined_cost_scratch =
          chart_spr_saturating_multiply(cost_bytes, topology.clade_count);
      combined_cost_scratch =
          chart_spr_saturating_multiply(combined_cost_scratch, 2);
      frontier = chart_spr_saturating_add(frontier, combined_cost_scratch);
      frontier = chart_spr_saturating_add(
          frontier,
          chart_spr_bit_capacity_bytes(topology.total_frontier_entries));

      auto const outside_multiplier =
          state.chart_opts.score_ua_edge ? nuc_state_count : 1;
      auto outside_rows =
          chart_spr_saturating_multiply(topology.clade_count, row_bytes);
      outside_rows =
          chart_spr_saturating_multiply(outside_rows, outside_multiplier);
      auto per_pattern_setup = chart_spr_saturating_add(
          {sizeof(chart_multisite_detail::active_pattern_info), false},
          taxon_count);
      // active_pattern_info always owns one dense inside chart reconstructed
      // from the selected representation, including the lazy bridge.
      per_pattern_setup = chart_spr_saturating_add(
          per_pattern_setup,
          chart_spr_saturating_multiply(topology.clade_count, row_bytes));
      per_pattern_setup =
          chart_spr_saturating_add(per_pattern_setup, outside_rows);
      auto setup =
          chart_spr_saturating_multiply(per_pattern_setup, pattern_count);
      auto pattern_plus_one =
          chart_spr_saturating_add({pattern_count, false}, 1);
      auto upper_topologies =
          chart_spr_saturating_multiply(pattern_plus_one, topology.clade_count);
      upper_topologies = chart_spr_saturating_multiply(
          upper_topologies, sizeof(production_id) + sizeof(std::size_t));
      upper_topologies = chart_spr_saturating_add(
          upper_topologies,
          chart_spr_saturating_multiply(pattern_plus_one, mask_bytes));
      setup = chart_spr_saturating_add(setup, upper_topologies);

      auto setup_serial_scratch = chart_spr_saturating_multiply(
          topology.clade_count,
          8 * row_bytes +
              4 * sizeof(std::optional<chart_multisite_detail::chart_row>));
      auto inner_worker_count =
          std::min(resolved_workers, std::max<std::size_t>(1, pattern_count));
      auto setup_inner_extra =
          chart_spr_saturating_multiply(topology.clade_count, 2 * row_bytes);
      setup_inner_extra =
          chart_spr_saturating_multiply(setup_inner_extra, inner_worker_count);
      setup_inner_extra = chart_spr_saturating_add(
          setup_inner_extra,
          chart_spr_saturating_multiply(
              pattern_count,
              512 + sizeof(std::exception_ptr) + sizeof(std::size_t)));
      auto upper_matrix =
          chart_spr_saturating_multiply(pattern_plus_one, pattern_count);
      upper_matrix = chart_spr_saturating_multiply(
          upper_matrix, sizeof(std::uint64_t) + sizeof(std::exception_ptr));
      setup_inner_extra =
          chart_spr_saturating_add(setup_inner_extra, upper_matrix);

      auto setup_peak = chart_spr_saturating_add(setup, setup_serial_scratch);
      auto frontier_peak = chart_spr_saturating_add(setup, frontier);
      auto verification_serial = chart_spr_saturating_add(
          structural, chart_spr_saturating_max(setup_peak, frontier_peak));
      auto verification_inner =
          chart_spr_saturating_add(verification_serial, setup_inner_extra);

      if (state.cache_strategy ==
          chart_spr_cache_strategy::lazy_multisite_chart) {
        auto lazy_cache =
            chart_spr_saturating_size{chart_spr_conservative_lazy_cache_bytes(
                                          topology.clade_count, pattern_count),
                                      false};
        auto lazy_build_peak = chart_spr_saturating_add(
            structural,
            chart_spr_saturating_multiply(
                lazy_cache, state.chart_opts.score_ua_edge ? 6 : 2));
        lazy_build_peak = chart_spr_saturating_add(lazy_build_peak, setup_peak);
        auto lazy_frontier_peak = chart_spr_saturating_add(
            structural, chart_spr_saturating_multiply(lazy_cache, 2));
        lazy_frontier_peak =
            chart_spr_saturating_add(lazy_frontier_peak, frontier_peak);
        verification_serial =
            chart_spr_saturating_max(lazy_build_peak, lazy_frontier_peak);
        // The current lazy verifier does not submit inner exact work.
        verification_inner = verification_serial;
      }

      auto serial = verification_serial;
      auto inner = verification_inner;
      if (options.verification_mode ==
              chart_spr_verification_mode::transient &&
          (state.exact_multisite_verifier ||
           state.contextual_exact_multisite_verifier)) {
        if (!state.exact_multisite_transient_memory_estimator ||
            !state.exact_multisite_transient_retained_memory_estimator) {
          safely_bounded = false;
        } else {
          auto transient_extra = chart_spr_saturating_size{
              state.exact_multisite_transient_memory_estimator(
                  candidate.candidate),
              false};
          serial = chart_spr_saturating_add(serial, transient_extra);
          inner = chart_spr_saturating_add(inner, transient_extra);
          custom_retained = chart_spr_saturating_size{
              state.exact_multisite_transient_retained_memory_estimator(
                  candidate.candidate),
              false};
          if (options.verify_transient_chain_extension_oracle_for_tests ||
              options
                  .force_transient_chain_extension_oracle_mismatch_for_tests) {
            // The primary trim and extended materialization remain live while
            // the diagnostic cold B&B is constructed.
            serial = chart_spr_saturating_add(serial, verification_serial);
            inner = chart_spr_saturating_add(inner, verification_serial);
          }
        }
      }
      estimate.serial_scratch_bytes = serial.value;
      estimate.inner_parallel_scratch_bytes = inner.value;
      safely_bounded = safely_bounded && !topology.saturated &&
                       !structural.saturated && !frontier.saturated &&
                       !setup.saturated && !serial.saturated &&
                       !inner.saturated;

      if (state.retain_verified_exact_trim_for_local_commit) {
        // Built-in grammar-exact verification transfers the finalized trim
        // into the candidate result. Its frontier work arrays are already
        // covered by scratch above; charge the owning result surface
        // separately so every completed rank remains admitted until stable
        // winner selection. A shared winner copy does not duplicate it.
        auto retained_trim = chart_spr_saturating_size{
            sizeof(chart_spr_reusable_exact_trim_payload) +
                4 * sizeof(void*) + 2 * sizeof(std::size_t),
            false};
        retained_trim = chart_spr_saturating_add(
            retained_trim,
            chart_spr_saturating_multiply(
                topology.clade_count,
                2 * sizeof(std::size_t) +
                    (options.exact_trim.dominance_mode ==
                             multisite_dominance_mode::two_pass_exact_mask
                         ? 2
                         : 1) *
                        sizeof(multisite_frontier_level_diagnostic)));
        retained_trim = chart_spr_saturating_add(
            retained_trim,
            chart_spr_bit_capacity_bytes(topology.production_count));
        if (options.exact_trim.capture_optimal_root_provenance) {
          auto provenance_class = chart_spr_saturating_add(
              {sizeof(multisite_optimal_root_provenance_class), false},
              cost_bytes);
          provenance_class = chart_spr_saturating_add(
              provenance_class,
              chart_spr_bit_capacity_bytes(topology.production_count));
          retained_trim = chart_spr_saturating_add(
              retained_trim,
              chart_spr_saturating_multiply(topology.root_frontier_entries,
                                            provenance_class.value));
          retained_trim.saturated =
              retained_trim.saturated || provenance_class.saturated;
        }
        // Builders reserve exact logical bounds, but keep a second copy-sized
        // allowance for allocator rounding and any future geometric capacity
        // growth without weakening finite admission.
        retained_trim = chart_spr_saturating_multiply(retained_trim, 2);
        estimate.retained_result_bytes = retained_trim.value;
        safely_bounded = safely_bounded && !retained_trim.saturated;
      }
    }

    if (options.semantic_capture != chart_spr_semantic_capture_mode::off) {
      auto key_bytes = chart_spr_virtual_key_bytes(state, topology);
      auto retained = chart_spr_saturating_add(
          {sizeof(chart_spr_canonical_exact_evidence) + 128, false},
          key_bytes);
      if (options.acceptance_mode ==
          chart_spr_acceptance_mode::fixed_topology_exact) {
        // Before/after selected-topology keys, with a full virtual-key set as
        // a conservative bound for each side.
        retained = chart_spr_saturating_add(retained, key_bytes);
        retained = chart_spr_saturating_add(
            retained,
            chart_spr_saturating_multiply(
                topology.production_count, 4 * sizeof(std::string)));
      } else {
        retained = chart_spr_saturating_add(
            retained, chart_spr_saturating_multiply(
                          topology.clade_count,
                          2 * sizeof(std::pair<std::string, std::size_t>)));
        retained = chart_spr_saturating_add(
            retained,
            chart_spr_saturating_multiply(
                topology.production_count, 2 * sizeof(std::string)));
        auto root_production_string_slots = chart_spr_saturating_multiply(
            topology.production_count, 2 * sizeof(std::string));
        auto root_class_bytes = chart_spr_saturating_add(
            chart_spr_saturating_multiply(cost_bytes, 2), key_bytes);
        root_class_bytes = chart_spr_saturating_add(
            root_class_bytes, root_production_string_slots);
        root_class_bytes = chart_spr_saturating_add(
            root_class_bytes,
            2 * sizeof(chart_spr_canonical_root_provenance_class));
        retained = chart_spr_saturating_add(
            retained,
            chart_spr_saturating_multiply(topology.root_frontier_entries,
                                          root_class_bytes.value));
        retained.saturated = retained.saturated ||
                             root_production_string_slots.saturated ||
                             root_class_bytes.saturated;
      }
      // Aggregation copies task evidence into the canonical candidate record
      // while the verifier result remains live in `verified`.
      retained = chart_spr_saturating_multiply(retained, 2);
      auto retained_with_trim = chart_spr_saturating_add(
          {estimate.retained_result_bytes, false}, retained);
      estimate.retained_result_bytes = retained_with_trim.value;
      safely_bounded = safely_bounded && !retained.saturated;
      safely_bounded = safely_bounded && !retained_with_trim.saturated;
    }
    // A provider-created owning payload first lives in the stable result slot
    // and can then be copy-constructed into result.accepted while the verified
    // vector remains live. The pre-loop accepted-copy walker sees only the
    // pre-verifier candidate, so charge two copies of provider output here.
    auto custom_retained_with_accepted_copy =
        chart_spr_saturating_multiply(custom_retained, 2);
    auto retained_with_provider = chart_spr_saturating_add(
        {estimate.retained_result_bytes, false},
        custom_retained_with_accepted_copy);
    estimate.retained_result_bytes = retained_with_provider.value;
    safely_bounded = safely_bounded && !custom_retained.saturated &&
                     !custom_retained_with_accepted_copy.saturated &&
                     !retained_with_provider.saturated;
    estimate.safely_bounded = safely_bounded;
  } catch (std::overflow_error const&) {
    estimate.serial_scratch_bytes = (std::numeric_limits<std::size_t>::max)();
    estimate.inner_parallel_scratch_bytes =
        (std::numeric_limits<std::size_t>::max)();
    estimate.retained_result_bytes = (std::numeric_limits<std::size_t>::max)();
    estimate.safely_bounded = false;
  }
  if (estimate.inner_parallel_scratch_bytes < estimate.serial_scratch_bytes) {
    estimate.inner_parallel_scratch_bytes = estimate.serial_scratch_bytes;
  }
  return estimate;
}

std::size_t chart_spr_search_detail::
    estimate_chart_spr_exact_candidate_inner_scheduler_scratch_bytes(
        chart_spr_search_state const& state,
        chart_spr_candidate_score const& candidate,
        chart_spr_search_options const& options,
        chart_scheduler const& scheduler) {
  if (state.cache_strategy == chart_spr_cache_strategy::lazy_multisite_chart) {
    // Current lazy fixed and multisite verifiers are serial special cases.
    return 0;
  }
  auto const pattern_count = state.active_patterns.patterns.patterns.size();
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::fixed_topology_exact) {
    if (state.fixed_topology_exact_verifier &&
        !state.contextual_fixed_topology_exact_verifier) {
      // The legacy callback signature has no scheduler/context parameter.
      return 0;
    }
    auto const range_options = chart_spr_phase4_pattern_range_options(
        pattern_count, scheduler.worker_resolution().resolved_workers);
    return estimate_chart_scheduler_operation_peak_bytes(
        scheduler.plan_indexed_ranges(pattern_count, range_options));
  }

  auto const candidate_clade_count = chart_spr_checked_cache_bytes_add(
      state.grammar.clades.size(), candidate.candidate.added_clades.size(),
      "chart SPR candidate exact scheduler clade count overflow");
  auto const range_options =
      chart_multisite_detail::multisite_exact_setup_range_options();
  auto peak = estimate_chart_scheduler_operation_peak_bytes(
      scheduler.plan_indexed_ranges(pattern_count, range_options));
  auto const topology_count_bound = chart_spr_checked_cache_bytes_add(
      pattern_count, 1,
      "chart SPR candidate exact upper-topology count overflow");
  auto const upper_item_bound = chart_spr_checked_cache_bytes_multiply(
      pattern_count, topology_count_bound,
      "chart SPR candidate exact upper-topology item overflow");
  peak =
      std::max(peak, estimate_chart_spr_scheduler_operation_peak_for_any_items(
                         scheduler, upper_item_bound, range_options));
  peak = std::max(
      peak,
      estimate_chart_spr_scheduler_operation_peak_for_any_items(
          scheduler, candidate_clade_count,
          chart_multisite_detail::multisite_frontier_clade_range_options()));

  // Exact setup retains up to two summaries while the frontier appends one
  // summary per dependency level/pass. A candidate plan has at most C levels.
  auto const frontier_pass_count =
      options.exact_trim.dominance_mode ==
              multisite_dominance_mode::two_pass_exact_mask
          ? std::size_t{2}
          : std::size_t{1};
  auto const frontier_summary_count = chart_spr_checked_cache_bytes_multiply(
      candidate_clade_count, frontier_pass_count,
      "chart SPR candidate exact frontier summary count overflow");
  auto const summary_count = chart_spr_checked_cache_bytes_add(
      2, frontier_summary_count,
      "chart SPR candidate exact scheduler summary count overflow");
  auto const summary_bytes = chart_spr_checked_cache_bytes_multiply(
      summary_count, sizeof(chart_scheduler_run_summary),
      "chart SPR candidate exact scheduler summary bytes overflow");
  return chart_spr_checked_cache_bytes_add(
      peak, summary_bytes,
      "chart SPR candidate exact inner scheduler scratch overflow");
}

chart_spr_exact_candidate_memory_estimate estimate_chart_spr_state_exact_memory(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options,
    std::size_t resolved_workers, std::size_t memory_budget_bytes) {
  chart_spr_search_options options;
  options.acceptance_mode = chart_spr_acceptance_mode::exact_multisite;
  options.cache = state.cache_opts;
  options.cache.memory_budget_bytes = memory_budget_bytes;
  options.chart = state.chart_opts;
  options.exact_trim = trim_options;
  options.verification_mode = chart_spr_verification_mode::cold;
  options.semantic_capture = chart_spr_semantic_capture_mode::off;
  chart_spr_candidate_score identity_candidate;
  auto estimate = estimate_chart_spr_exact_candidate_memory(
      state, identity_candidate, options, resolved_workers);
  if (state.exact_setup_provider || state.scheduled_exact_setup_provider) {
    if (!state.exact_setup_provider_additional_memory_estimator) {
      estimate.safely_bounded = false;
    } else {
      auto const additional =
          state.exact_setup_provider_additional_memory_estimator(
              trim_options, resolved_workers);
      auto serial = chart_spr_saturating_add(
          {estimate.serial_scratch_bytes, false}, additional);
      auto inner = chart_spr_saturating_add(
          {estimate.inner_parallel_scratch_bytes, false}, additional);
      estimate.serial_scratch_bytes = serial.value;
      estimate.inner_parallel_scratch_bytes = inner.value;
      estimate.safely_bounded = estimate.safely_bounded && !serial.saturated &&
                                !inner.saturated;
    }
  }
  return estimate;
}

chart_spr_exact_candidate_memory_estimate estimate_chart_spr_state_exact_memory(
    chart_spr_search_state const& state,
    multisite_trim_options const& trim_options,
    std::size_t resolved_workers) {
  return estimate_chart_spr_state_exact_memory(
      state, trim_options, resolved_workers,
      state.cache_opts.memory_budget_bytes);
}

void refresh_chart_spr_lazy_chart_after_local_commit_for_tests(
    chart_spr_search_state& state, overlay_chain const& chain,
    overlay_materialization_result const& materialized,
    chart_execution_plan const& execution_plan,
    std::vector<overlay_clade_ref> const& previous_dense_clade_to_ref) {
  auto cache_commit_plan = build_chart_cache_commit_plan(
      chain, outside_affected_policy::three_term_tight);
  (void)chart_spr_refresh_lazy_chart_after_local_commit(
      state, chain, materialized, execution_plan, previous_dense_clade_to_ref,
      cache_commit_plan);
}

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
      cache, state, candidate, icache_view, state.counters);
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

namespace {

// These fields describe work performed by a committed accept transaction, not
// merely the candidate selected by the acceptance gate.  Keep the selected
// candidate (`accepted` and canonical candidate records) intact for semantic
// reporting, while making an aborted transaction release all commit evidence.
void chart_spr_discard_uncommitted_accept_transaction_evidence(
    chart_spr_iteration_result& iteration) noexcept {
  iteration.accepted_inside_rows_recomputed = 0;
  iteration.accepted_outside_rows_recomputed = 0;
  std::string{}.swap(iteration.accepted_candidate_signature);
}

}  // namespace

chart_spr_search_result run_chart_spr_search(
    phylo_dag initial_dag, clade_grammar initial_grammar,
    chart_spr_search_options options) {
  configure_chart_spr_primary_exact_provenance(options);
  validate_chart_spr_search_loop_options(options);

  auto total_start = std::chrono::steady_clock::now();
  auto const requested_workers =
      chart_spr_search_detail::requested_chart_spr_worker_count(options);
  chart_scheduler scheduler{
      chart_spr_search_detail::chart_spr_search_scheduler_options(
          requested_workers)};
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
  auto active_build = make_active_search_patterns(
      result.dag, initial_grammar, scheduler, options.chart);
  chart_spr_search_detail::chart_spr_state_build_policy state_build_policy;
  state_build_policy.defer_pattern_batch_bootstrap_to_local_cache =
      !options.rebuild_after_accept;
  auto const build_exact_during_state_publication =
      options.rebuild_after_accept &&
      options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite;
  auto state = build_chart_spr_search_state_from_active(
      result.dag, std::move(initial_grammar), std::move(active_build),
      options.chart, build_exact_during_state_publication, options.exact_trim,
      options.cache, state_build_policy, &scheduler);
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
        chart_spr_make_local_commit_substrate(state, options, scheduler);
    state.retain_verified_exact_trim_for_local_commit = true;
    state.local_commit_persistent_cache_bytes =
        local_commit_substrate->resident_cache_bytes;
    state.resident_pattern_cache_bytes = chart_spr_checked_cache_bytes_add(
        state.resident_pattern_cache_bytes,
        state.local_commit_persistent_cache_bytes,
        "chart SPR local-commit total resident byte overflow");
    auto* substrate_ptr = local_commit_substrate.get();
    chart_spr_install_state_callback(
        state, state.exact_setup_provider,
        [substrate_ptr](chart_spr_search_state const& provider_state,
                        checked_chart_execution_plan_ref const& checked_state) {
          return chart_spr_build_exact_setup_from_persistent_inside_cache_serial(
              *substrate_ptr, provider_state, checked_state);
        });
    chart_spr_install_state_callback(
        state, state.scheduled_exact_setup_provider,
        [substrate_ptr](chart_spr_search_state const& provider_state,
                        checked_chart_execution_plan_ref const& checked_state,
                        chart_scheduler& provider_scheduler,
                        std::vector<chart_scheduler_run_summary>* runs) {
          return chart_spr_build_exact_setup_from_persistent_inside_cache(
              *substrate_ptr, provider_state, checked_state, provider_scheduler,
              runs);
        });
    // The generic state-exact estimate already covers the finalized setup and
    // its construction scratch. This internal provider adds no independent
    // allocation surface beyond that bound.
    chart_spr_install_state_callback(
        state, state.exact_setup_provider_additional_memory_estimator,
        [](multisite_trim_options const&, std::size_t) { return 0; });
    if (state.pattern_batch_bootstrap_deferred) {
      chart_spr_search_detail::finalize_deferred_pattern_batch_bootstrap(
          state, inside_cache_composite_lower_bound_with_invariants(
                     *local_commit_substrate->icache));
    }
    chart_spr_publish_persistent_inside_row_view(state,
                                                 *local_commit_substrate);
    if (state.local_commit_inside_rows.valid()) {
      // The persistent inside cache is now the authoritative non-lazy scoring
      // surface. Assert/release any defensive empty projection before exact
      // setup and candidate scoring begin.
      std::vector<pattern_chart_cache_entry>{}.swap(state.pattern_charts);
      state.resident_pattern_cache_bytes =
          state.local_commit_persistent_cache_bytes;
      chart_spr_require_cache_budget(state.resident_pattern_cache_bytes,
                                     state.cache_opts,
                                     "chart SPR local-commit row view");
    }
    if (options.force_fixed_topology_cache_epoch_mismatch_for_tests) {
      // Corrupt only after the genuine initial tip passed row-view
      // publication. The fixed-topology verifier must diagnose its own epoch
      // contract; moving this hook earlier would make setup fail for the wrong
      // reason and leave that verifier path untested.
      local_commit_substrate->icache->commit_epoch =
          local_commit_substrate->chain->size() + 1;
    }
    result.summary.local_inside_cache_initialization_ms =
        local_commit_substrate->inside_cache_initialization_ms;
    result.summary.local_outside_cache_initialization_ms =
        local_commit_substrate->outside_cache_initialization_ms;

    if (options.acceptance_mode == chart_spr_acceptance_mode::exact_multisite) {
      auto const exact_start = std::chrono::steady_clock::now();
      auto checked =
          check_chart_execution_plan(state.grammar, state.execution_plan);
      (void)ensure_chart_spr_state_exact_trim(state, checked, scheduler,
                                              options.exact_trim);
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
  result.summary.lazy_policy = state.lazy_policy;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
  result.summary.requested_worker_count = requested_workers;
  result.summary.resolved_worker_count =
      scheduler.worker_resolution().resolved_workers;
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
  double exact_candidate_verification_ms_sum = 0.0;
  std::optional<chart_spr_candidate_score> last_local_update_accepted;
  std::optional<chart_spr_recorded_chain_objective>
      last_local_update_recorded_objective;
  std::vector<std::vector<rank3_production_taxa_key>>
      local_update_accepted_topology_key_sets;
  bool used_local_accept_updates = false;

  // Finish installing candidate verifiers on the substrate built before the
  // initial exact score. The accept path commits to it instead of
  // dense-materializing per accept; state.grammar and the persistent row view
  // remain the derived current-tip scoring surface.
  if (local_commit_substrate != nullptr) {
    auto* substrate_ptr = local_commit_substrate.get();
    chart_spr_install_state_callback(
        state, state.contextual_fixed_topology_exact_verifier,
        [substrate_ptr](chart_spr_search_state const& verifier_state,
                        chart_spr_candidate_score candidate,
                        chart_spr_exact_verification_context& context) {
          return chart_spr_verify_candidate_fixed_topology_exact_from_persistent_cache(
              *substrate_ptr, verifier_state, std::move(candidate), context);
        });
    state.fixed_topology_exact_verifier_parallel_safe = true;
    // The generic fixed-topology estimate explicitly includes the persistent
    // selected-cache/direct-oracle phases used by this internal callback.
    chart_spr_install_state_callback(
        state, state.fixed_topology_exact_additional_memory_estimator,
        [](grammar_spr_candidate const&) { return 0; });
    chart_spr_install_state_callback(
        state, state.fixed_topology_exact_additional_retained_memory_estimator,
        [](grammar_spr_candidate const&) { return 0; });
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
    if (options.verification_mode == chart_spr_verification_mode::transient) {
      chart_spr_install_state_callback(
          state, state.exact_multisite_transient_memory_estimator,
          [substrate_ptr](grammar_spr_candidate const& candidate) {
            return chart_spr_transient_verifier_extra_memory_bound(
                *substrate_ptr, candidate);
          });
      chart_spr_install_state_callback(
          state, state.exact_multisite_transient_retained_memory_estimator,
          [](grammar_spr_candidate const&) { return 0; });
      chart_spr_install_state_callback(
          state, state.contextual_exact_multisite_verifier,
          [substrate_ptr](chart_spr_search_state const& verifier_state,
                          chart_spr_candidate_score candidate,
                          checked_chart_execution_plan_ref const& checked_state,
                          chart_spr_exact_verification_context& context,
                          multisite_trim_options const& trim_options) {
            return chart_spr_verify_candidate_exact_multisite_from_transient_extension(
                *substrate_ptr, verifier_state, std::move(candidate),
                checked_state, context, trim_options);
          });
      state.exact_multisite_verifier_parallel_safe = true;
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
        state, iteration_options, iter, acceptance_workspace, scheduler);
    result.summary.candidates_generated += iteration.candidates_generated;
    result.summary.candidate_generation_ms +=
        iteration.candidate_generation_ms;
    result.summary.local_scoring_ms += iteration.local_scoring_ms;
    result.summary.exact_verification_ms += iteration.exact_verification_ms;
    for (double candidate_ms : iteration.exact_candidate_verification_ms) {
      exact_candidate_verification_ms_sum += candidate_ms;
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
    chart_spr_scheduler_axis_counters failed_rebuild_scheduler_axes;
    auto materialize_start = std::chrono::steady_clock::now();
    bool local_commit_mutated_shared_state = false;
    bool rebuild_scheduler_call_started = false;
    bool rebuild_scheduler_call_completed = false;
    std::uint64_t scheduler_operations_before_rebuild = 0;
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
        rebuild_scheduler_call_started = true;
        scheduler_operations_before_rebuild = scheduler.metrics().operations;
        auto tentative_state = rebuild_chart_spr_search_state_after_accept(
            state, materialized.dag, std::move(materialized.rebuilt.grammar),
            options, reused_patterns, scheduler,
            &failed_rebuild_scheduler_axes);
        rebuild_scheduler_call_completed = true;
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
          chart_spr_discard_uncommitted_accept_transaction_evidence(iteration);
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
          chart_spr_discard_uncommitted_accept_transaction_evidence(iteration);
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
        auto const inside_rows_before_commit =
            attempt_counters.inside_rows_recomputed_on_commit;
        auto const outside_rows_before_commit =
            attempt_counters.outside_rows_recomputed_on_commit;
        auto commit = chart_spr_commit_accepted_locally(
            *local_commit_substrate, state, *iteration.accepted, options,
            attempt_counters, scheduler);
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
          chart_spr_discard_uncommitted_accept_transaction_evidence(iteration);
          result.summary.final_score = iteration.state_score_after;
          result.iterations.push_back(std::move(iteration));
          break;
        }

        if (attempt_counters.inside_rows_recomputed_on_commit <
                inside_rows_before_commit ||
            attempt_counters.outside_rows_recomputed_on_commit <
                outside_rows_before_commit) {
          throw chart_spr_local_commit_hard_error(
              "chart SPR local commit: persistent affected-row counter moved "
              "backwards");
        }
        iteration.accepted_inside_rows_recomputed =
            attempt_counters.inside_rows_recomputed_on_commit -
            inside_rows_before_commit;
        iteration.accepted_outside_rows_recomputed =
            attempt_counters.outside_rows_recomputed_on_commit -
            outside_rows_before_commit;

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
    } catch (chart_scheduler_submit_error const&) {
      // A submit failure has no returned run summary. It is scheduler
      // infrastructure failure, not an invalid biological candidate, and
      // must not be converted into a nominally successful search whose global
      // metrics cannot be classified by semantic axis.
      throw;
    } catch (multisite_optimal_root_provenance_capture_error const&) {
      // Primary-trim root provenance remains report-only.  A capture failure
      // cannot reject an otherwise accepted candidate during state rebuild.
      throw;
    } catch (std::bad_alloc const&) {
      throw;
    } catch (std::overflow_error const&) {
      // Rebuilding an accepted exact state is an integrity computation.  An
      // arithmetic overflow cannot be downgraded to an ordinary biological
      // post-materialization rejection.
      throw;
    } catch (std::logic_error const&) {
      // Scheduler lifecycle/concurrent-use and invariant failures are hard.
      throw;
    } catch (chart_spr_cache_budget_error const&) {
      throw;
    } catch (chart_spr_local_commit_hard_error const&) {
      throw;
    } catch (std::exception const& e) {
      if (rebuild_scheduler_call_started && !rebuild_scheduler_call_completed) {
        auto const scheduler_operations_after_rebuild =
            scheduler.metrics().operations;
        auto const operation_delta =
            scheduler_operations_after_rebuild >=
                    scheduler_operations_before_rebuild
                ? scheduler_operations_after_rebuild -
                      scheduler_operations_before_rebuild
                : (std::numeric_limits<std::uint64_t>::max)();
        auto const classified_operations =
            chart_spr_scheduler_axis_operation_count(
                failed_rebuild_scheduler_axes);
        if (operation_delta != classified_operations) {
          throw chart_scheduler_submit_error(
              std::string{"chart SPR accepted rebuild: scheduler operation "
                          "failed before semantic-axis publication: "} +
              e.what());
        }
      }
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
      add_chart_spr_scheduler_axis_counters(attempt_counters.scheduler_axes,
                                            failed_rebuild_scheduler_axes);
      ++attempt_counters.post_materialization_rejections;
      state.counters = attempt_counters;
      iteration.post_materialization_rejected = true;
      iteration.accepted_move_committed = false;
      iteration.state_score_after = iteration.state_score_before;
      iteration.no_accept_reason =
          "accepted candidate failed materialization/rebuild";
      iteration.post_materialization_rejection_reason = e.what();
      chart_spr_discard_uncommitted_accept_transaction_evidence(iteration);
      result.summary.final_score = iteration.state_score_after;
      result.iterations.push_back(std::move(iteration));
      break;
    }
  }

  if (used_local_accept_updates) {
    auto compact_start = std::chrono::steady_clock::now();
    auto preserved_counters = state.counters;
    auto const preserved_local_commit_cache_bytes =
        local_commit_substrate->resident_cache_bytes;
    auto compacted = chart_spr_compact_and_verify_local_update_state(
        result.dag, *local_commit_substrate->chain, state, options,
        local_update_accepted_topology_key_sets,
        last_local_update_accepted ? &*last_local_update_accepted : nullptr,
        last_local_update_recorded_objective
            ? &*last_local_update_recorded_objective
            : nullptr,
        preserved_counters, scheduler);
    result.dag = std::move(compacted.dag);
    compacted.rebuilt_state.dag = &result.dag;
    compacted.rebuilt_state.counters = preserved_counters;
    state = std::move(compacted.rebuilt_state);
    state.local_commit_persistent_cache_bytes =
        preserved_local_commit_cache_bytes;
    state.resident_pattern_cache_bytes = chart_spr_checked_cache_bytes_add(
        state.resident_pattern_cache_bytes,
        state.local_commit_persistent_cache_bytes,
        "chart SPR final-compaction resident cache byte overflow");
    chart_spr_require_cache_budget(state.resident_pattern_cache_bytes,
                                   state.cache_opts,
                                   "chart SPR final compaction");
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
  result.summary.lazy_policy = state.lazy_policy;
  result.summary.effective_pattern_batch_size =
      state.effective_pattern_batch_size;
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
        exact_candidate_verification_ms_sum /
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
      auto const& final_trim = ensure_chart_spr_state_exact_trim(
          state, checked_final, scheduler, options.exact_trim);
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

  // Keep the scheduler alive through final exact/canonical work. Returned
  // results, however, own no worker threads or pending tasks.
  scheduler.shutdown();
  result.summary.scheduler = scheduler.metrics();
  auto const& scheduler_metrics = result.summary.scheduler;
  auto const& axes = state.counters.scheduler_axes;
  std::array<chart_spr_scheduler_axis_metrics const*, 13> axis_list{
      &axes.initial_chart_patterns,
      &axes.candidate_generation,
      &axes.exact_setup_patterns,
      &axes.exact_frontier_clades,
      &axes.exact_candidates,
      &axes.lazy_inside_clades,
      &axes.lazy_outside_clades,
      &axes.inside_cache_patterns,
      &axes.outside_cache_patterns,
      &axes.fixed_topology_patterns,
      &axes.local_score_candidates,
      &axes.local_score_candidate_patterns,
      &axes.other,
  };
  std::uint64_t axis_operations = 0;
  std::uint64_t axis_parallel_operations = 0;
  std::uint64_t axis_ranges = 0;
  std::uint64_t axis_tasks = 0;
  std::size_t axis_active_high_water = 0;
  std::size_t axis_minimum_grain = 0;
  std::size_t axis_maximum_grain = 0;
  for (auto const* axis : axis_list) {
    axis_operations += axis->operations;
    axis_parallel_operations += axis->parallel_operations;
    axis_ranges += axis->ranges;
    axis_tasks += axis->worker_tasks;
    axis_active_high_water =
        std::max(axis_active_high_water, axis->active_worker_high_water);
    if (axis->minimum_effective_grain != 0) {
      axis_minimum_grain =
          axis_minimum_grain == 0
              ? axis->minimum_effective_grain
              : std::min(axis_minimum_grain, axis->minimum_effective_grain);
    }
    axis_maximum_grain =
        std::max(axis_maximum_grain, axis->maximum_effective_grain);
    if (axis->parallel_operations > axis->operations ||
        (axis->ranges == 0 && (axis->minimum_effective_grain != 0 ||
                               axis->maximum_effective_grain != 0)) ||
        (axis->ranges != 0 &&
         (axis->minimum_effective_grain == 0 ||
          axis->minimum_effective_grain > axis->maximum_effective_grain))) {
      throw std::logic_error(
          "chart SPR search: invalid per-axis scheduler metrics");
    }
  }
  if (scheduler_metrics.requested_workers !=
          result.summary.requested_worker_count ||
      scheduler_metrics.resolved_workers !=
          result.summary.resolved_worker_count ||
      scheduler_metrics.operations != scheduler_metrics.parallel_operations +
                                          scheduler_metrics.serial_fallbacks ||
      scheduler_metrics.ranges_created !=
          scheduler_metrics.ranges_completed +
              scheduler_metrics.ranges_cancelled ||
      scheduler_metrics.tasks_submitted != scheduler_metrics.tasks_completed ||
      scheduler_metrics.tasks_submitted != scheduler_metrics.tasks_joined ||
      scheduler_metrics.pending_tasks != 0 ||
      scheduler_metrics.pending_tasks_at_shutdown != 0 ||
      scheduler_metrics.live_pool_threads != 0 ||
      scheduler_metrics.pool_lifetimes > 1 ||
      scheduler_metrics.pool_lifetimes !=
          scheduler_metrics.pool_lifetimes_stopped ||
      !scheduler_metrics.shutdown ||
      axis_operations != scheduler_metrics.operations ||
      axis_parallel_operations != scheduler_metrics.parallel_operations ||
      axis_ranges != scheduler_metrics.ranges_created ||
      axis_tasks != scheduler_metrics.tasks_submitted ||
      axis_active_high_water != scheduler_metrics.active_worker_high_water ||
      axis_minimum_grain != scheduler_metrics.minimum_effective_grain ||
      axis_maximum_grain != scheduler_metrics.maximum_effective_grain) {
    throw std::logic_error(
        "chart SPR search: scheduler returned incomplete or unclassified "
        "shutdown metrics");
  }
  result.summary.total_ms =
      chart_spr_elapsed_ms(total_start, std::chrono::steady_clock::now());

  // Canonical provenance is constructed after the ordinary end-of-run
  // counter snapshot.  It can reuse the resident plan for one companion trim
  // (and a previously absent final exact trim can be built here), so resnapshot
  // to expose every successful plan use in the returned full-run counters.
  result.counters = state.counters;
  chart_spr_refresh_search_summary_from_counters(result.summary,
                                                 result.counters);
  result.summary.scheduler_axes = result.counters.scheduler_axes;
  // The counter refresh above intentionally restores cumulative accounting,
  // but the public lazy summary fields describe the final resident chart.
  // Reapply that current-state view so its raw fields and derived ratios stay
  // mutually consistent after canonical post-processing.
  chart_spr_refresh_search_summary_from_current_lazy_chart(result.summary,
                                                          state);
  return result;
}

}  // namespace larch

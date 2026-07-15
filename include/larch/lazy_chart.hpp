#pragma once

#include <larch/chart_execution_plan.hpp>
#include <larch/chart_spr.hpp>
#include <larch/lazy_key_grouping.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/site_patterns.hpp>
#include <larch/chart_trim.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larch {

struct lazy_chart_options {
  chart_options chart;

  // Default builds keep the root maps and discard child maps once construction
  // has consumed their last parent dependency. Tests and diagnostics can opt in
  // to retaining every clade map for dense-vs-lazy row recovery.
  bool retain_all_inside_class_maps = false;
};

struct lazy_multisite_chart {
  using row_type = std::array<chart_cost, nuc_state_count>;

  std::vector<std::vector<row_type>> inside_rows_by_clade;
  std::vector<std::vector<row_type>> outside_rows_by_clade;
  std::vector<std::optional<std::vector<std::size_t>>>
      class_index_by_pattern_by_clade;
  std::vector<std::optional<std::vector<std::size_t>>>
      structural_class_index_by_pattern_by_clade;
  std::vector<std::optional<std::vector<std::size_t>>>
      outside_class_index_by_pattern_by_clade;
  std::vector<std::size_t> structural_class_count_by_clade;
  std::vector<std::vector<std::uint32_t>> class_weight_by_clade;
  std::vector<std::vector<std::uint32_t>> outside_class_weight_by_clade;
  std::vector<chart_cost> outside_global_min_by_pattern;

  std::size_t pattern_count = 0;
  std::size_t total_pattern_weight = 0;
  std::size_t lazy_inside_rows_computed = 0;
  std::size_t lazy_outside_rows_computed = 0;
  std::size_t lazy_patterns_merged_max = 0;
  std::size_t lazy_remerge_collisions = 0;
  std::size_t lazy_structural_class_count_max = 0;
  std::size_t multifurcation_productions_scored = 0;
  std::size_t outside_multifurcation_productions_scored = 0;
  outside_recurrence_work_stats outside_recurrence_work;

  [[nodiscard]] row_type const& inside_row(clade_id clade,
                                           std::size_t pattern) const {
    if (clade == no_clade || clade >= inside_rows_by_clade.size()) {
      throw std::runtime_error("lazy chart: clade id out of range");
    }
    if (pattern >= pattern_count) {
      throw std::runtime_error("lazy chart: pattern index out of range");
    }
    auto const& map = class_index_by_pattern_by_clade[clade];
    if (!map) {
      throw std::runtime_error(
          "lazy chart: inside class map was not retained for clade " +
          std::to_string(clade));
    }
    auto class_index = (*map)[pattern];
    auto const& rows = inside_rows_by_clade[clade];
    if (class_index >= rows.size()) {
      throw std::runtime_error("lazy chart: inside class index out of range");
    }
    return rows[class_index];
  }

  [[nodiscard]] row_type const& outside_row(clade_id clade,
                                            std::size_t pattern) const {
    if (clade == no_clade || clade >= outside_rows_by_clade.size()) {
      throw std::runtime_error("lazy chart: clade id out of range");
    }
    if (pattern >= pattern_count) {
      throw std::runtime_error("lazy chart: pattern index out of range");
    }
    auto const& map = outside_class_index_by_pattern_by_clade[clade];
    if (!map) {
      throw std::runtime_error(
          "lazy chart: outside class map was not retained for clade " +
          std::to_string(clade));
    }
    auto class_index = (*map)[pattern];
    auto const& rows = outside_rows_by_clade[clade];
    if (class_index >= rows.size()) {
      throw std::runtime_error("lazy chart: outside class index out of range");
    }
    return rows[class_index];
  }

  [[nodiscard]] std::size_t inside_class_count(clade_id clade) const {
    if (clade == no_clade || clade >= inside_rows_by_clade.size()) {
      throw std::runtime_error("lazy chart: clade id out of range");
    }
    return inside_rows_by_clade[clade].size();
  }

  [[nodiscard]] std::size_t outside_class_count(clade_id clade) const {
    if (clade == no_clade || clade >= outside_rows_by_clade.size()) {
      throw std::runtime_error("lazy chart: clade id out of range");
    }
    return outside_rows_by_clade[clade].size();
  }

  [[nodiscard]] std::size_t structural_class_count(clade_id clade) const {
    if (clade == no_clade || clade >= structural_class_count_by_clade.size()) {
      throw std::runtime_error("lazy chart: clade id out of range");
    }
    return structural_class_count_by_clade[clade];
  }

  [[nodiscard]] chart_cost outside_global_min(std::size_t pattern) const {
    if (pattern >= outside_global_min_by_pattern.size()) {
      throw std::runtime_error("lazy chart: pattern index out of range");
    }
    return outside_global_min_by_pattern[pattern];
  }
};

namespace lazy_chart_detail {

using row_type = lazy_multisite_chart::row_type;

inline std::vector<clade_id> clades_by_increasing_size(
    clade_grammar const& grammar) {
  std::vector<clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs,
                                                   clade_id rhs) {
    auto const& ltaxa = grammar.clades[lhs].taxa;
    auto const& rtaxa = grammar.clades[rhs].taxa;
    if (ltaxa.size() != rtaxa.size()) return ltaxa.size() < rtaxa.size();
    return lhs < rhs;
  });
  return order;
}

inline void checked_add_weight(std::uint32_t& target, std::uint32_t weight,
                               std::string_view context) {
  if (target > std::numeric_limits<std::uint32_t>::max() - weight) {
    throw std::runtime_error("lazy chart: uint32 weight overflow while adding " +
                             std::string{context});
  }
  target += weight;
}

inline void validate_patterns(clade_grammar const& grammar,
                              site_pattern_set const& patterns) {
  auto taxon_count = grammar.taxa.id_to_sample_id.size();
  if (patterns.taxon_count != 0 && patterns.taxon_count != taxon_count) {
    throw std::runtime_error(
        "lazy chart: pattern taxon_count does not match grammar");
  }
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (pattern.state_by_taxon.size() != taxon_count) {
      throw std::runtime_error(
          "lazy chart: pattern " + std::to_string(pattern_index) +
          " leaf state count does not match grammar taxon count");
    }
    for (std::size_t tid = 0; tid < pattern.state_by_taxon.size(); ++tid) {
      auto const state = pattern.state_by_taxon[tid];
      if (state >= nuc_state_count) {
        parsimony_chart_detail::validate_state(
            state, "lazy chart pattern " + std::to_string(pattern_index) +
                       " taxon " + std::to_string(tid));
      }
    }
  }
}

// Pattern validation for the trusted structural-plan path.  The immutable plan
// has already checked every grammar ID, clade, production index, and partition;
// only the per-build pattern payload remains to be checked here.
inline void validate_patterns(chart_execution_plan const& plan,
                              site_pattern_set const& patterns) {
  plan.assert_valid();
  auto const taxon_count = plan.taxon_count();
  if (patterns.taxon_count != 0 && patterns.taxon_count != taxon_count) {
    throw std::runtime_error(
        "lazy chart: pattern taxon_count does not match execution plan");
  }
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (pattern.state_by_taxon.size() != taxon_count) {
      throw std::runtime_error(
          "lazy chart: pattern " + std::to_string(pattern_index) +
          " leaf state count does not match execution plan taxon count");
    }
    for (std::size_t tid = 0; tid < pattern.state_by_taxon.size(); ++tid) {
      auto const state = pattern.state_by_taxon[tid];
      if (state >= nuc_state_count) {
        parsimony_chart_detail::validate_state(
            state, "lazy chart pattern " + std::to_string(pattern_index) +
                       " taxon " + std::to_string(tid));
      }
    }
  }
}

inline std::size_t leaf_class_index_for_pattern(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade, std::size_t pattern) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error("lazy chart: clade id out of range");
  }
  if (pattern >= patterns.patterns.size()) {
    throw std::runtime_error("lazy chart: pattern index out of range");
  }
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() != 1) {
    throw std::runtime_error(
        "lazy chart: missing class map for non-leaf clade " +
        std::to_string(clade));
  }
  auto taxon = key.taxa.front();
  auto observed = patterns.patterns[pattern].state_by_taxon[taxon];
  auto const& rows = chart.inside_rows_by_clade[clade];
  for (std::size_t class_index = 0; class_index < rows.size(); ++class_index) {
    if (rows[class_index][observed] == 0) return class_index;
  }
  throw std::runtime_error("lazy chart: leaf class row not found");
}

inline std::size_t inside_class_index_for_pattern(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade, std::size_t pattern) {
  if (clade == no_clade || clade >= chart.class_index_by_pattern_by_clade.size()) {
    throw std::runtime_error("lazy chart: clade id out of range");
  }
  auto const& map = chart.class_index_by_pattern_by_clade[clade];
  if (map) return (*map)[pattern];
  return leaf_class_index_for_pattern(chart, grammar, patterns, clade, pattern);
}

inline std::size_t structural_class_index_for_pattern(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade, std::size_t pattern) {
  if (clade == no_clade ||
      clade >= chart.structural_class_index_by_pattern_by_clade.size()) {
    throw std::runtime_error("lazy chart: clade id out of range");
  }
  auto const& map = chart.structural_class_index_by_pattern_by_clade[clade];
  if (map) return (*map)[pattern];
  return leaf_class_index_for_pattern(chart, grammar, patterns, clade, pattern);
}

inline void assign_leaf_classes(lazy_multisite_chart& chart,
                                clade_grammar const& grammar,
                                site_pattern_set const& patterns,
                                clade_id clade,
                                lazy_chart_options const& options) {
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() != 1) {
    throw std::runtime_error("lazy chart: leaf class assignment got non-leaf");
  }
  if (!grammar.productions_by_parent[clade].empty()) {
    throw std::runtime_error("lazy chart: singleton leaf clade has productions");
  }
  auto taxon = key.taxa.front();
  if (taxon >= grammar.taxa.id_to_sample_id.size()) {
    throw std::runtime_error("lazy chart: singleton taxon id out of range");
  }

  std::array<std::size_t, nuc_state_count> class_by_state{};
  class_by_state.fill(std::numeric_limits<std::size_t>::max());
  bool retain_map =
      options.retain_all_inside_class_maps || clade == grammar.root_clade;
  std::vector<std::size_t> class_map;
  std::vector<std::size_t> structural_map;
  if (retain_map) {
    class_map.assign(chart.pattern_count, 0);
    structural_map.assign(chart.pattern_count, 0);
  }
  auto& rows = chart.inside_rows_by_clade[clade];
  auto& weights = chart.class_weight_by_clade[clade];

  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto observed = patterns.patterns[pattern_index].state_by_taxon[taxon];
    auto& class_index = class_by_state[observed];
    if (class_index == std::numeric_limits<std::size_t>::max()) {
      class_index = rows.size();
      auto row = parsimony_chart_detail::make_inf_row();
      row[observed] = 0;
      rows.push_back(row);
      weights.push_back(0);
    }
    if (retain_map) {
      class_map[pattern_index] = class_index;
      structural_map[pattern_index] = class_index;
    }
    checked_add_weight(weights[class_index],
                       patterns.patterns[pattern_index].weight, "leaf class");
  }

  if (retain_map) {
    chart.class_index_by_pattern_by_clade[clade] = std::move(class_map);
    chart.structural_class_index_by_pattern_by_clade[clade] =
        std::move(structural_map);
  }
  chart.structural_class_count_by_clade[clade] = rows.size();
}

inline void finalize_inside_counters(lazy_multisite_chart& chart) {
  chart.lazy_inside_rows_computed = 0;
  chart.lazy_patterns_merged_max = 0;
  chart.lazy_structural_class_count_max = 0;

  for (std::size_t clade = 0; clade < chart.inside_rows_by_clade.size();
       ++clade) {
    auto class_count = chart.inside_rows_by_clade[clade].size();
    chart.lazy_inside_rows_computed += class_count;
    if (chart.pattern_count >= class_count) {
      chart.lazy_patterns_merged_max = std::max(
          chart.lazy_patterns_merged_max, chart.pattern_count - class_count);
    }
    chart.lazy_structural_class_count_max =
        std::max(chart.lazy_structural_class_count_max,
                 chart.structural_class_count_by_clade[clade]);
  }
}

inline void maybe_discard_nonroot_maps(lazy_multisite_chart& chart,
                                       clade_grammar const& grammar,
                                       lazy_chart_options const& options) {
  if (options.retain_all_inside_class_maps) return;
  for (std::size_t clade = 0; clade < chart.inside_rows_by_clade.size();
       ++clade) {
    if (clade == grammar.root_clade) continue;
    chart.class_index_by_pattern_by_clade[clade] = std::nullopt;
    chart.structural_class_index_by_pattern_by_clade[clade] = std::nullopt;
  }
}

struct map_dependency_counts {
  std::vector<std::size_t> inside;
  std::vector<std::size_t> structural;
};

inline map_dependency_counts count_map_dependencies(
    clade_grammar const& grammar) {
  map_dependency_counts counts;
  counts.inside.assign(grammar.clades.size(), 0);
  counts.structural.assign(grammar.clades.size(), 0);

  for (clade_id parent = 0; parent < grammar.productions_by_parent.size();
       ++parent) {
    auto const& production_ids = grammar.productions_by_parent[parent];
    if (production_ids.empty()) continue;

    bool counted_structural = false;
    for (auto pid : production_ids) {
      if (pid == no_production || pid >= grammar.productions.size()) {
        throw std::runtime_error("lazy chart: production id out of range");
      }
      auto const& prod = grammar.productions[pid];
      if (prod.parent != parent) {
        throw std::runtime_error(
            "lazy chart: productions_by_parent contains mismatched parent");
      }
      for (auto child : prod.children) {
        if (child == no_clade || child >= counts.inside.size()) {
          throw std::runtime_error(
              "lazy chart: production child clade id out of range");
        }
        ++counts.inside[child];
      }

      if (!counted_structural) {
        for (auto child : prod.children) ++counts.structural[child];
        counted_structural = true;
      }
    }
  }
  return counts;
}

inline void consume_inside_child_map(lazy_multisite_chart& chart,
                                     clade_grammar const& grammar,
                                     clade_id child,
                                     map_dependency_counts& remaining) {
  if (remaining.inside[child] == 0) {
    throw std::runtime_error(
        "lazy chart: inside map dependency count underflow");
  }
  --remaining.inside[child];
  if (remaining.inside[child] == 0 && child != grammar.root_clade) {
    chart.class_index_by_pattern_by_clade[child] = std::nullopt;
  }
}

inline void consume_structural_child_map(lazy_multisite_chart& chart,
                                         clade_grammar const& grammar,
                                         clade_id child,
                                         map_dependency_counts& remaining) {
  if (remaining.structural[child] == 0) {
    throw std::runtime_error(
        "lazy chart: structural map dependency count underflow");
  }
  --remaining.structural[child];
  if (remaining.structural[child] == 0 && child != grammar.root_clade) {
    chart.structural_class_index_by_pattern_by_clade[child] = std::nullopt;
  }
}

// The helpers below are the lazy-inside counterpart of the trusted dense
// recurrence in parsimony_chart.hpp.  They consume only descriptors compiled
// into chart_execution_plan.  In particular, none of them consults a grammar,
// validates a production partition, or constructs a clade order.

inline std::size_t plan_leaf_class_index_for_pattern(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id clade, std::size_t pattern) {
  if (pattern >= patterns.patterns.size()) {
    throw std::runtime_error("lazy chart: pattern index out of range");
  }
  auto const& descriptor = plan.clade(clade);
  if (!descriptor.is_leaf()) {
    throw std::runtime_error(
        "lazy chart: missing class map for non-leaf clade " +
        std::to_string(clade));
  }
  auto const observed =
      patterns.patterns[pattern].state_by_taxon[descriptor.leaf_taxon];
  auto const& rows = chart.inside_rows_by_clade[clade];
  for (std::size_t class_index = 0; class_index < rows.size(); ++class_index) {
    if (rows[class_index][observed] == 0) return class_index;
  }
  throw std::runtime_error("lazy chart: leaf class row not found");
}

inline std::size_t plan_inside_class_index_for_pattern(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id clade, std::size_t pattern) {
  if (clade == no_clade ||
      clade >= chart.class_index_by_pattern_by_clade.size()) {
    throw std::runtime_error("lazy chart: clade id out of range");
  }
  auto const& map = chart.class_index_by_pattern_by_clade[clade];
  if (map) return (*map)[pattern];
  return plan_leaf_class_index_for_pattern(chart, plan, patterns, clade,
                                           pattern);
}

inline std::size_t plan_structural_class_index_for_pattern(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id clade, std::size_t pattern) {
  if (clade == no_clade ||
      clade >= chart.structural_class_index_by_pattern_by_clade.size()) {
    throw std::runtime_error("lazy chart: clade id out of range");
  }
  auto const& map = chart.structural_class_index_by_pattern_by_clade[clade];
  if (map) return (*map)[pattern];
  return plan_leaf_class_index_for_pattern(chart, plan, patterns, clade,
                                           pattern);
}

inline map_dependency_counts count_map_dependencies(
    chart_execution_plan const& plan) {
  map_dependency_counts counts;
  counts.inside.assign(plan.clades().size(), 0);
  counts.structural.assign(plan.clades().size(), 0);

  for (clade_id parent = 0; parent < plan.clades().size(); ++parent) {
    auto const production_ids = plan.productions_for_parent(parent);
    for (std::size_t prod_i = 0; prod_i < production_ids.size(); ++prod_i) {
      for (auto child : plan.children(production_ids[prod_i])) {
        ++counts.inside[child];
        if (prod_i == 0) ++counts.structural[child];
      }
    }
  }
  return counts;
}

inline void consume_plan_inside_child_map(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    clade_id child, map_dependency_counts& remaining) {
  if (remaining.inside[child] == 0) {
    throw std::runtime_error(
        "lazy chart: inside map dependency count underflow");
  }
  --remaining.inside[child];
  if (remaining.inside[child] == 0 && child != plan.root_clade()) {
    chart.class_index_by_pattern_by_clade[child] = std::nullopt;
  }
}

inline void consume_plan_structural_child_map(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    clade_id child, map_dependency_counts& remaining) {
  if (remaining.structural[child] == 0) {
    throw std::runtime_error(
        "lazy chart: structural map dependency count underflow");
  }
  --remaining.structural[child];
  if (remaining.structural[child] == 0 && child != plan.root_clade()) {
    chart.structural_class_index_by_pattern_by_clade[child] = std::nullopt;
  }
}

inline void consume_plan_parent_map_dependencies(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    clade_id parent, map_dependency_counts& remaining) {
  auto const production_ids = plan.productions_for_parent(parent);
  for (std::size_t prod_i = 0; prod_i < production_ids.size(); ++prod_i) {
    for (auto child : plan.children(production_ids[prod_i])) {
      consume_plan_inside_child_map(chart, plan, child, remaining);
      if (prod_i == 0) {
        consume_plan_structural_child_map(chart, plan, child, remaining);
      }
    }
  }
}

struct plan_parent_key_grouping_memory_accounting {
  std::size_t structural_key_count = 0;
  std::size_t structural_key_width = 0;
  std::size_t row_key_count = 0;
  std::size_t row_key_width = 0;
  std::size_t row_key_class_count = 0;
  std::size_t logical_resident_bytes = 0;
  std::size_t actual_capacity_resident_bytes = 0;
  std::size_t observed_prepublication_peak_capacity_resident_bytes = 0;
  lazy_key_grouping_detail::packed_key_word_buffer_preparation_report
      structural_word_preparation;
  lazy_key_grouping_detail::packed_key_grouping_preparation_report
      structural_grouping_preparation;
  lazy_key_grouping_detail::packed_key_word_buffer_preparation_report
      row_word_preparation;
  lazy_key_grouping_detail::packed_key_grouping_preparation_report
      row_grouping_preparation;
};

struct plan_parent_key_workspace {
  std::vector<lazy_key_grouping_detail::packed_key_word> packed_words;
  lazy_key_grouping_detail::packed_key_grouping_workspace grouping_workspace;
  lazy_key_grouping_detail::packed_key_grouping_result structural_grouping;
  lazy_key_grouping_detail::packed_key_grouping_result row_grouping;
  // Internal rows are deduplicated by the same stable packed-key machinery as
  // structural/context keys.  These buffers replace the task-local ordered
  // row map and its temporary class vectors.  Finite-budget scheduled builds
  // prepare every capacity on the coordinator before admitting the worker.
  lazy_key_grouping_detail::packed_key_grouping_result row_value_grouping;
  std::vector<row_type> computed_rows;
  std::vector<std::size_t> structural_to_row_class;
  std::vector<std::size_t> row_key_to_row_class;

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    using lazy_key_grouping_detail::checked_packed_key_bytes_add;
    using lazy_key_grouping_detail::checked_packed_key_bytes_multiply;
    auto total =
        lazy_key_grouping_detail::packed_key_word_buffer_dynamic_capacity_bytes(
            packed_words);
    total = checked_packed_key_bytes_add(
        total, grouping_workspace.dynamic_capacity_bytes(),
        "lazy plan parent-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total, structural_grouping.dynamic_capacity_bytes(),
        "lazy plan parent-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total, row_grouping.dynamic_capacity_bytes(),
        "lazy plan parent-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total, row_value_grouping.dynamic_capacity_bytes(),
        "lazy plan parent-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total,
        checked_packed_key_bytes_multiply(
            computed_rows.capacity(), sizeof(row_type),
            "lazy plan computed-row scratch capacity"),
        "lazy plan parent-key workspace capacity");
    for (auto const* values :
         {&structural_to_row_class, &row_key_to_row_class}) {
      total = checked_packed_key_bytes_add(
          total,
          checked_packed_key_bytes_multiply(
              values->capacity(), sizeof(std::size_t),
              "lazy plan row-class scratch capacity"),
          "lazy plan parent-key workspace capacity");
    }
    return total;
  }

  [[nodiscard]] std::size_t resident_bytes() const {
    return lazy_key_grouping_detail::checked_packed_key_bytes_add(
        sizeof(*this), dynamic_capacity_bytes(),
        "lazy plan parent-key workspace resident");
  }

  [[nodiscard]] bool has_capacity_for(std::size_t pattern_count,
                                      std::size_t maximum_row_key_width) const {
    using lazy_key_grouping_detail::checked_packed_key_count_multiply;
    auto const key_width =
        std::max<std::size_t>(maximum_row_key_width, nuc_state_count);
    auto const word_count = checked_packed_key_count_multiply(
        pattern_count, key_width, "lazy inside reusable packed words");
    return packed_words.capacity() >= word_count &&
           grouping_workspace.has_capacity_for(pattern_count) &&
           structural_grouping.has_worst_case_capacity_for(pattern_count) &&
           row_grouping.has_worst_case_capacity_for(pattern_count) &&
           row_value_grouping.has_worst_case_capacity_for(pattern_count) &&
           computed_rows.capacity() >= pattern_count &&
           structural_to_row_class.capacity() >= pattern_count &&
           row_key_to_row_class.capacity() >= pattern_count;
  }
};

struct packed_plan_parent_keys {
  plan_parent_key_workspace* storage = nullptr;
  std::unique_ptr<plan_parent_key_workspace> owned_storage;
  std::size_t structural_key_width = 0;
  std::size_t row_key_width = 0;
  plan_parent_key_grouping_memory_accounting memory;

  packed_plan_parent_keys() = default;
  packed_plan_parent_keys(packed_plan_parent_keys&&) noexcept = default;
  packed_plan_parent_keys& operator=(packed_plan_parent_keys&&) noexcept =
      default;
  packed_plan_parent_keys(packed_plan_parent_keys const&) = delete;
  packed_plan_parent_keys& operator=(packed_plan_parent_keys const&) = delete;

  [[nodiscard]] plan_parent_key_workspace const& workspace() const {
    if (storage == nullptr) {
      throw std::logic_error("lazy chart: missing packed parent-key storage");
    }
    return *storage;
  }

  [[nodiscard]] lazy_key_grouping_detail::packed_key_grouping_result const&
  structural_classes() const {
    return workspace().structural_grouping;
  }

  [[nodiscard]] lazy_key_grouping_detail::packed_key_grouping_result const&
  row_key_classes() const {
    return workspace().row_grouping;
  }

  [[nodiscard]] std::span<lazy_key_grouping_detail::packed_key_word const>
  row_key_words() const {
    return workspace().packed_words;
  }
};

inline std::size_t estimate_plan_parent_key_grouping_logical_resident_bytes(
    std::size_t pattern_count, std::size_t structural_width,
    std::size_t structural_class_count, std::size_t row_width,
    std::size_t row_key_class_count) {
  using namespace lazy_key_grouping_detail;
  auto const structural =
      estimate_packed_key_grouping_memory(pattern_count, structural_width);
  auto const rows =
      estimate_packed_key_grouping_memory(structural_class_count, row_width);

  auto total = checked_packed_key_bytes_add(
      sizeof(plan_parent_key_workspace), sizeof(packed_plan_parent_keys),
      "lazy plan parent-key fixed resident estimate");
  total = checked_packed_key_bytes_add(
      total,
      std::max(structural.packed_payload_logical_bytes,
               rows.packed_payload_logical_bytes),
      "lazy plan parent-key payload estimate");
  total = checked_packed_key_bytes_add(
      total,
      std::max(structural.workspace_logical_dynamic_bytes,
               rows.workspace_logical_dynamic_bytes),
      "lazy plan parent-key grouping-workspace estimate");
  total = checked_packed_key_bytes_add(
      total,
      estimate_packed_key_grouping_result_logical_dynamic_bytes(
          pattern_count, structural_class_count),
      "lazy plan structural-grouping result estimate");
  return checked_packed_key_bytes_add(
      total,
      estimate_packed_key_grouping_result_logical_dynamic_bytes(
          structural_class_count, row_key_class_count),
      "lazy plan row-grouping result estimate");
}

inline void record_plan_parent_key_preparation_peak(
    plan_parent_key_grouping_memory_accounting& accounting,
    std::size_t current_workspace_resident_bytes,
    std::size_t previous_component_resident_bytes,
    std::size_t observed_component_peak_resident_bytes) {
  if (observed_component_peak_resident_bytes <
      previous_component_resident_bytes) {
    throw std::logic_error(
        "lazy chart: packed-key preparation peak is below prior resident");
  }
  auto const staged_extra = observed_component_peak_resident_bytes -
                            previous_component_resident_bytes;
  auto peak = lazy_key_grouping_detail::checked_packed_key_bytes_add(
      sizeof(packed_plan_parent_keys), current_workspace_resident_bytes,
      "lazy plan parent-key preparation peak");
  peak = lazy_key_grouping_detail::checked_packed_key_bytes_add(
      peak, staged_extra, "lazy plan parent-key preparation peak");
  accounting.observed_prepublication_peak_capacity_resident_bytes = std::max(
      accounting.observed_prepublication_peak_capacity_resident_bytes, peak);
}

inline void require_plan_parent_key_grouping_success(
    lazy_key_grouping_detail::packed_key_grouping_prepared_status status) {
  if (!status.succeeded()) {
    lazy_key_grouping_detail::throw_packed_key_grouping_prepared_failure(
        status);
  }
}

inline packed_plan_parent_keys collect_plan_parent_keys(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id parent,
    plan_parent_key_workspace& workspace) {
  auto const production_ids = plan.productions_for_parent(parent);
  if (production_ids.empty()) {
    throw std::runtime_error(
        "lazy chart: non-singleton clade has no productions");
  }

  using namespace lazy_key_grouping_detail;
  auto const structural_children = plan.children(production_ids.front());
  auto const structural_width = structural_children.size();
  std::size_t row_width = 0;
  for (auto production : production_ids) {
    row_width = checked_packed_key_count_add(row_width,
                                             plan.children(production).size(),
                                             "lazy plan parent row-key width");
  }

  packed_plan_parent_keys keys;
  keys.storage = &workspace;
  keys.structural_key_width = structural_width;
  keys.row_key_width = row_width;
  keys.memory.structural_key_count = chart.pattern_count;
  keys.memory.structural_key_width = structural_width;

  auto const structural_estimate = estimate_packed_key_grouping_memory(
      chart.pattern_count, structural_width);
  auto workspace_before = workspace.resident_bytes();
  keys.memory.structural_word_preparation = prepare_packed_key_word_buffer(
      structural_estimate.packed_word_count, workspace.packed_words);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.structural_word_preparation.previous_capacity_resident_bytes,
      keys.memory.structural_word_preparation
          .observed_prepublication_peak_capacity_resident_bytes);
  workspace.packed_words.resize(structural_estimate.packed_word_count);
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    auto output = pattern * structural_width;
    for (auto child : structural_children) {
      workspace.packed_words[output++] =
          checked_packed_key_word(plan_structural_class_index_for_pattern(
                                      chart, plan, patterns, child, pattern),
                                  "lazy plan structural class index");
    }
  }

  workspace_before = workspace.resident_bytes();
  keys.memory.structural_grouping_preparation =
      prepare_packed_key_grouping_storage(chart.pattern_count,
                                          workspace.grouping_workspace,
                                          workspace.structural_grouping);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.structural_grouping_preparation
          .previous_owned_capacity_resident_bytes,
      keys.memory.structural_grouping_preparation
          .observed_prepublication_peak_owned_capacity_resident_bytes);
  auto structural_view = packed_key_matrix_view{
      .key_count = chart.pattern_count,
      .key_width = structural_width,
      .words = workspace.packed_words,
  };
  require_plan_parent_key_grouping_success(try_group_packed_keys_prepared(
      structural_view, workspace.grouping_workspace,
      workspace.structural_grouping,
      keys.memory.structural_grouping_preparation
          .prepared_owned_capacity_resident_bytes));

  auto const structural_class_count =
      workspace.structural_grouping.class_count();
  keys.memory.row_key_count = structural_class_count;
  keys.memory.row_key_width = row_width;
  auto const row_estimate =
      estimate_packed_key_grouping_memory(structural_class_count, row_width);
  workspace_before = workspace.resident_bytes();
  keys.memory.row_word_preparation = prepare_packed_key_word_buffer(
      row_estimate.packed_word_count, workspace.packed_words);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.row_word_preparation.previous_capacity_resident_bytes,
      keys.memory.row_word_preparation
          .observed_prepublication_peak_capacity_resident_bytes);
  workspace.packed_words.resize(row_estimate.packed_word_count);
  for (std::size_t structural_class = 0;
       structural_class < structural_class_count; ++structural_class) {
    auto const representative =
        workspace.structural_grouping.representative_by_class[structural_class];
    auto output = structural_class * row_width;
    for (auto production : production_ids) {
      for (auto child : plan.children(production)) {
        workspace.packed_words[output++] = checked_packed_key_word(
            plan_inside_class_index_for_pattern(chart, plan, patterns, child,
                                                representative),
            "lazy plan inside-row class index");
      }
    }
  }

  workspace_before = workspace.resident_bytes();
  keys.memory.row_grouping_preparation = prepare_packed_key_grouping_storage(
      structural_class_count, workspace.grouping_workspace,
      workspace.row_grouping);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.row_grouping_preparation
          .previous_owned_capacity_resident_bytes,
      keys.memory.row_grouping_preparation
          .observed_prepublication_peak_owned_capacity_resident_bytes);
  auto row_view = packed_key_matrix_view{
      .key_count = structural_class_count,
      .key_width = row_width,
      .words = workspace.packed_words,
  };
  require_plan_parent_key_grouping_success(try_group_packed_keys_prepared(
      row_view, workspace.grouping_workspace, workspace.row_grouping,
      keys.memory.row_grouping_preparation
          .prepared_owned_capacity_resident_bytes));
  keys.memory.row_key_class_count = workspace.row_grouping.class_count();

  keys.memory.logical_resident_bytes =
      estimate_plan_parent_key_grouping_logical_resident_bytes(
          chart.pattern_count, structural_width, structural_class_count,
          row_width, keys.memory.row_key_class_count);
  keys.memory.actual_capacity_resident_bytes = checked_packed_key_bytes_add(
      sizeof(packed_plan_parent_keys), workspace.resident_bytes(),
      "lazy plan parent-key actual resident");
  keys.memory.observed_prepublication_peak_capacity_resident_bytes =
      std::max(keys.memory.observed_prepublication_peak_capacity_resident_bytes,
               keys.memory.actual_capacity_resident_bytes);
  return keys;
}

// The local-commit compatibility boundary has no caller-owned scratch in its
// current API. Keep that source-compatible call safe by making the returned
// handle own its storage; ordinary full construction reuses an external
// workspace across every clade.
inline packed_plan_parent_keys collect_plan_parent_keys(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id parent) {
  auto owned_storage = std::make_unique<plan_parent_key_workspace>();
  auto keys =
      collect_plan_parent_keys(chart, plan, patterns, parent, *owned_storage);
  keys.owned_storage = std::move(owned_storage);
  return keys;
}

// Grammar-backed and execution-plan-backed inside builders intentionally share
// the same staged storage and accounting contract. A packed handle borrows its
// workspace until the caller has consumed the class maps and row-key words.
using grammar_parent_key_grouping_memory_accounting =
    plan_parent_key_grouping_memory_accounting;
using grammar_parent_key_workspace = plan_parent_key_workspace;
using packed_grammar_parent_keys = packed_plan_parent_keys;

inline std::size_t estimate_grammar_parent_key_grouping_logical_resident_bytes(
    std::size_t pattern_count, std::size_t structural_width,
    std::size_t structural_class_count, std::size_t row_width,
    std::size_t row_key_class_count) {
  return estimate_plan_parent_key_grouping_logical_resident_bytes(
      pattern_count, structural_width, structural_class_count, row_width,
      row_key_class_count);
}

// All productions and children must already have been validated and built.
// Keeping recursion outside this function is important: its returned handle
// borrows the one workspace reused by the recursive grammar builder.
inline packed_grammar_parent_keys collect_ready_grammar_parent_keys(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id parent,
    map_dependency_counts* remaining, grammar_parent_key_workspace& workspace) {
  auto const& production_ids = grammar.productions_by_parent[parent];
  if (production_ids.empty()) {
    throw std::runtime_error(
        "lazy chart: non-singleton clade has no productions");
  }

  using namespace lazy_key_grouping_detail;
  auto const& structural_children =
      grammar.productions[production_ids.front()].children;
  auto const structural_width = structural_children.size();
  std::size_t row_width = 0;
  for (auto production : production_ids) {
    row_width = checked_packed_key_count_add(
        row_width, grammar.productions[production].children.size(),
        "lazy grammar parent row-key width");
  }

  packed_grammar_parent_keys keys;
  keys.storage = &workspace;
  keys.structural_key_width = structural_width;
  keys.row_key_width = row_width;
  keys.memory.structural_key_count = chart.pattern_count;
  keys.memory.structural_key_width = structural_width;

  auto const structural_estimate = estimate_packed_key_grouping_memory(
      chart.pattern_count, structural_width);
  auto workspace_before = workspace.resident_bytes();
  keys.memory.structural_word_preparation = prepare_packed_key_word_buffer(
      structural_estimate.packed_word_count, workspace.packed_words);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.structural_word_preparation.previous_capacity_resident_bytes,
      keys.memory.structural_word_preparation
          .observed_prepublication_peak_capacity_resident_bytes);
  workspace.packed_words.resize(structural_estimate.packed_word_count);
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    auto output = pattern * structural_width;
    for (auto child : structural_children) {
      workspace.packed_words[output++] =
          checked_packed_key_word(structural_class_index_for_pattern(
                                      chart, grammar, patterns, child, pattern),
                                  "lazy grammar structural class index");
    }
  }

  workspace_before = workspace.resident_bytes();
  keys.memory.structural_grouping_preparation =
      prepare_packed_key_grouping_storage(chart.pattern_count,
                                          workspace.grouping_workspace,
                                          workspace.structural_grouping);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.structural_grouping_preparation
          .previous_owned_capacity_resident_bytes,
      keys.memory.structural_grouping_preparation
          .observed_prepublication_peak_owned_capacity_resident_bytes);
  auto const structural_view = packed_key_matrix_view{
      .key_count = chart.pattern_count,
      .key_width = structural_width,
      .words = workspace.packed_words,
  };
  require_plan_parent_key_grouping_success(try_group_packed_keys_prepared(
      structural_view, workspace.grouping_workspace,
      workspace.structural_grouping,
      keys.memory.structural_grouping_preparation
          .prepared_owned_capacity_resident_bytes));

  auto const structural_class_count =
      workspace.structural_grouping.class_count();
  keys.memory.row_key_count = structural_class_count;
  keys.memory.row_key_width = row_width;
  auto const row_estimate =
      estimate_packed_key_grouping_memory(structural_class_count, row_width);
  workspace_before = workspace.resident_bytes();
  keys.memory.row_word_preparation = prepare_packed_key_word_buffer(
      row_estimate.packed_word_count, workspace.packed_words);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.row_word_preparation.previous_capacity_resident_bytes,
      keys.memory.row_word_preparation
          .observed_prepublication_peak_capacity_resident_bytes);
  workspace.packed_words.resize(row_estimate.packed_word_count);
  for (std::size_t structural_class = 0;
       structural_class < structural_class_count; ++structural_class) {
    auto const representative =
        workspace.structural_grouping.representative_by_class[structural_class];
    auto output = structural_class * row_width;
    for (auto production : production_ids) {
      for (auto child : grammar.productions[production].children) {
        workspace.packed_words[output++] = checked_packed_key_word(
            inside_class_index_for_pattern(chart, grammar, patterns, child,
                                           representative),
            "lazy grammar inside-row class index");
      }
    }
  }

  workspace_before = workspace.resident_bytes();
  keys.memory.row_grouping_preparation = prepare_packed_key_grouping_storage(
      structural_class_count, workspace.grouping_workspace,
      workspace.row_grouping);
  record_plan_parent_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.row_grouping_preparation
          .previous_owned_capacity_resident_bytes,
      keys.memory.row_grouping_preparation
          .observed_prepublication_peak_owned_capacity_resident_bytes);
  auto const row_view = packed_key_matrix_view{
      .key_count = structural_class_count,
      .key_width = row_width,
      .words = workspace.packed_words,
  };
  require_plan_parent_key_grouping_success(try_group_packed_keys_prepared(
      row_view, workspace.grouping_workspace, workspace.row_grouping,
      keys.memory.row_grouping_preparation
          .prepared_owned_capacity_resident_bytes));
  keys.memory.row_key_class_count = workspace.row_grouping.class_count();

  // Sparse child maps stay live until both packed stages have published their
  // results. Only then may the dependency counters release them.
  if (remaining != nullptr) {
    for (std::size_t prod_i = 0; prod_i < production_ids.size(); ++prod_i) {
      for (auto child : grammar.productions[production_ids[prod_i]].children) {
        consume_inside_child_map(chart, grammar, child, *remaining);
        if (prod_i == 0) {
          consume_structural_child_map(chart, grammar, child, *remaining);
        }
      }
    }
  }

  keys.memory.logical_resident_bytes =
      estimate_grammar_parent_key_grouping_logical_resident_bytes(
          chart.pattern_count, structural_width, structural_class_count,
          row_width, keys.memory.row_key_class_count);
  keys.memory.actual_capacity_resident_bytes = checked_packed_key_bytes_add(
      sizeof(packed_grammar_parent_keys), workspace.resident_bytes(),
      "lazy grammar parent-key actual resident");
  keys.memory.observed_prepublication_peak_capacity_resident_bytes =
      std::max(keys.memory.observed_prepublication_peak_capacity_resident_bytes,
               keys.memory.actual_capacity_resident_bytes);
  return keys;
}

inline row_type compute_grammar_internal_inside_row_from_keys(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    clade_id clade, packed_grammar_parent_keys const& keys,
    std::size_t structural_class, std::size_t& multifurcation_counter) {
  auto const& structural_classes = keys.structural_classes();
  if (structural_class >= structural_classes.class_count()) {
    throw std::runtime_error("lazy chart: structural class index out of range");
  }
  auto const words = keys.row_key_words();
  auto const expected_word_count =
      lazy_key_grouping_detail::checked_packed_key_count_multiply(
          structural_classes.class_count(), keys.row_key_width,
          "lazy grammar row-key matrix");
  if (words.size() != expected_word_count) {
    throw std::runtime_error("lazy chart: packed row-key size mismatch");
  }

  auto row = parsimony_chart_detail::make_inf_row();
  auto const& production_ids = grammar.productions_by_parent[clade];
  auto const key_begin = structural_class * keys.row_key_width;
  std::size_t row_key_offset = 0;
  for (auto pid : production_ids) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("lazy chart: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "lazy chart: productions_by_parent contains mismatched parent");
    }
    parsimony_chart_detail::validate_production_inside_row_inputs(
        grammar, prod, pid, "lazy inside chart");
    if (prod.children.size() != 2) ++multifurcation_counter;

    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      std::size_t child_i = 0;
      auto row_provider = [&](clade_id child) -> row_type const& {
        if (child_i >= prod.children.size() ||
            prod.children[child_i] != child) {
          throw std::runtime_error(
              "lazy chart: packed row provider child order mismatch");
        }
        auto const class_index = static_cast<std::size_t>(
            words[key_begin + row_key_offset + child_i++]);
        auto const& child_rows = chart.inside_rows_by_clade[child];
        if (class_index >= child_rows.size()) {
          throw std::runtime_error(
              "lazy chart: child inside class index out of range");
        }
        return child_rows[class_index];
      };
      auto const candidate =
          parsimony_chart_detail::combine_production_inside_row(
              prod, parent_state, row_provider);
      row[parent_state] = std::min(row[parent_state], candidate);
    }
    row_key_offset += prod.children.size();
  }
  if (row_key_offset != keys.row_key_width) {
    throw std::runtime_error("lazy chart: packed row-key width mismatch");
  }
  return row;
}

inline void assign_grammar_internal_classes_from_keys(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade,
    packed_grammar_parent_keys const& keys) {
  auto const& structural_classes = keys.structural_classes();
  auto const& row_key_classes = keys.row_key_classes();
  if (structural_classes.class_by_input.size() != chart.pattern_count ||
      row_key_classes.class_by_input.size() !=
          structural_classes.class_count()) {
    throw std::runtime_error("lazy chart: packed parent-key count mismatch");
  }

  auto structural_map = structural_classes.class_by_input;
  std::vector<std::size_t> structural_to_row_class(
      structural_classes.class_count(), 0);
  std::vector<std::size_t> row_key_to_row_class(row_key_classes.class_count(),
                                                0);
  std::map<row_type, std::size_t> row_class_by_row;
  auto& rows = chart.inside_rows_by_clade[clade];

  // Both grouping stages assign first-occurrence IDs. Iterating row-key class
  // IDs therefore exactly matches the former structural-class traversal while
  // computing a candidate row only once per distinct row key. No ordered-map
  // traversal participated in the old grammar-inside numbering contract.
  for (std::size_t row_key_class = 0;
       row_key_class < row_key_classes.class_count(); ++row_key_class) {
    auto const structural_class =
        row_key_classes.representative_by_class[row_key_class];
    auto const row = compute_grammar_internal_inside_row_from_keys(
        chart, grammar, clade, keys, structural_class,
        chart.multifurcation_productions_scored);
    auto [class_it, inserted] =
        row_class_by_row.emplace(row, row_class_by_row.size());
    if (inserted) rows.push_back(row);
    row_key_to_row_class[row_key_class] = class_it->second;
  }
  for (std::size_t structural_class = 0;
       structural_class < structural_classes.class_count();
       ++structural_class) {
    structural_to_row_class[structural_class] =
        row_key_to_row_class[row_key_classes.class_by_input[structural_class]];
  }

  std::vector<std::size_t> class_map(chart.pattern_count, 0);
  auto& weights = chart.class_weight_by_clade[clade];
  weights.assign(rows.size(), 0);
  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto const structural_class = structural_map[pattern_index];
    auto const row_class = structural_to_row_class[structural_class];
    class_map[pattern_index] = row_class;
    checked_add_weight(weights[row_class],
                       patterns.patterns[pattern_index].weight,
                       "internal class");
  }

  chart.structural_class_count_by_clade[clade] =
      structural_classes.class_count();
  if (structural_classes.class_count() > rows.size()) {
    chart.lazy_remerge_collisions +=
        structural_classes.class_count() - rows.size();
  }
  chart.class_index_by_pattern_by_clade[clade] = std::move(class_map);
  chart.structural_class_index_by_pattern_by_clade[clade] =
      std::move(structural_map);
}

inline void assign_internal_classes(lazy_multisite_chart& chart,
                                    clade_grammar const& grammar,
                                    site_pattern_set const& patterns,
                                    clade_id clade,
                                    grammar_parent_key_workspace& workspace) {
  auto const& production_ids = grammar.productions_by_parent[clade];
  if (production_ids.empty()) {
    throw std::runtime_error(
        "lazy chart: non-singleton clade has no productions");
  }
  for (auto pid : production_ids) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("lazy chart: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "lazy chart: productions_by_parent contains mismatched parent");
    }
    parsimony_chart_detail::validate_production_inside_row_inputs(
        grammar, prod, pid, "lazy inside chart");
  }

  auto const keys = collect_ready_grammar_parent_keys(
      chart, grammar, patterns, clade, nullptr, workspace);
  assign_grammar_internal_classes_from_keys(chart, grammar, patterns, clade,
                                            keys);
}

template <class BuildChild>
inline packed_grammar_parent_keys collect_sparse_grammar_parent_keys(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id parent, BuildChild&& build_child,
    map_dependency_counts& remaining, grammar_parent_key_workspace& workspace) {
  auto const& production_ids = grammar.productions_by_parent[parent];
  if (production_ids.empty()) {
    throw std::runtime_error(
        "lazy chart: non-singleton clade has no productions");
  }

  // Finish all recursive children before borrowing the shared workspace for
  // this parent. Validation and child discovery retain production/child order.
  for (auto pid : production_ids) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("lazy chart: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != parent) {
      throw std::runtime_error(
          "lazy chart: productions_by_parent contains mismatched parent");
    }
    parsimony_chart_detail::validate_production_inside_row_inputs(
        grammar, prod, pid, "lazy inside chart");
    for (auto child : prod.children) build_child(child);
  }

  return collect_ready_grammar_parent_keys(chart, grammar, patterns, parent,
                                           &remaining, workspace);
}

inline void materialize_inside_class_maps(lazy_multisite_chart& chart,
                                          clade_grammar const& grammar,
                                          site_pattern_set const& patterns) {
  grammar_parent_key_workspace key_workspace;
  for (auto clade : clades_by_increasing_size(grammar)) {
    auto const& key = grammar.clades[clade];
    if (key.taxa.empty()) {
      throw std::runtime_error("lazy chart: empty clade is invalid");
    }

    if (key.taxa.size() == 1) {
      if (!chart.class_index_by_pattern_by_clade[clade]) {
        std::vector<std::size_t> class_map(chart.pattern_count, 0);
        for (std::size_t pattern = 0; pattern < chart.pattern_count;
             ++pattern) {
          class_map[pattern] = leaf_class_index_for_pattern(
              chart, grammar, patterns, clade, pattern);
        }
        chart.class_index_by_pattern_by_clade[clade] = class_map;
        chart.structural_class_index_by_pattern_by_clade[clade] =
            std::move(class_map);
      }
      continue;
    }

    if (chart.class_index_by_pattern_by_clade[clade] &&
        chart.structural_class_index_by_pattern_by_clade[clade]) {
      continue;
    }

    auto const& production_ids = grammar.productions_by_parent[clade];
    if (production_ids.empty()) {
      throw std::runtime_error(
          "lazy chart: non-singleton clade has no productions");
    }
    for (auto pid : production_ids) {
      if (pid == no_production || pid >= grammar.productions.size()) {
        throw std::runtime_error("lazy chart: production id out of range");
      }
      auto const& prod = grammar.productions[pid];
      if (prod.parent != clade) {
        throw std::runtime_error(
            "lazy chart: productions_by_parent contains mismatched parent");
      }
      parsimony_chart_detail::validate_production_inside_row_inputs(
          grammar, prod, pid, "lazy inside chart materialize maps");
    }

    auto const keys = collect_ready_grammar_parent_keys(
        chart, grammar, patterns, clade, nullptr, key_workspace);
    auto const& structural_classes = keys.structural_classes();
    auto structural_map = structural_classes.class_by_input;
    if (chart.structural_class_count_by_clade[clade] !=
        structural_classes.class_count()) {
      throw std::runtime_error(
          "lazy chart: materialized structural class count mismatch");
    }

    std::map<row_type, std::size_t> row_class_by_row;
    auto const& rows = chart.inside_rows_by_clade[clade];
    for (std::size_t class_index = 0; class_index < rows.size();
         ++class_index) {
      row_class_by_row.emplace(rows[class_index], class_index);
    }

    std::vector<std::size_t> structural_to_row_class(
        structural_classes.class_count(), 0);
    for (std::size_t structural_class = 0;
         structural_class < structural_classes.class_count();
         ++structural_class) {
      std::size_t ignored_multifurcation_counter = 0;
      auto const row = compute_grammar_internal_inside_row_from_keys(
          chart, grammar, clade, keys, structural_class,
          ignored_multifurcation_counter);
      auto const row_it = row_class_by_row.find(row);
      if (row_it == row_class_by_row.end()) {
        throw std::runtime_error(
            "lazy chart: materialized inside row not found in stored classes");
      }
      structural_to_row_class[structural_class] = row_it->second;
    }

    std::vector<std::size_t> class_map(chart.pattern_count, 0);
    for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
      class_map[pattern] = structural_to_row_class[structural_map[pattern]];
    }
    chart.class_index_by_pattern_by_clade[clade] = std::move(class_map);
    chart.structural_class_index_by_pattern_by_clade[clade] =
        std::move(structural_map);
  }
}

inline row_type compute_plan_internal_inside_row_from_keys(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    clade_id clade, packed_plan_parent_keys const& keys,
    std::size_t structural_class, std::size_t& multifurcation_counter) {
  auto const& structural_classes = keys.structural_classes();
  if (structural_class >= structural_classes.class_count()) {
    throw std::runtime_error("lazy chart: structural class index out of range");
  }
  auto const words = keys.row_key_words();
  auto const expected_word_count =
      lazy_key_grouping_detail::checked_packed_key_count_multiply(
          structural_classes.class_count(), keys.row_key_width,
          "lazy plan row-key matrix");
  if (words.size() != expected_word_count) {
    throw std::runtime_error("lazy chart: packed row-key size mismatch");
  }

  auto row = parsimony_chart_detail::make_inf_row();
  auto const production_ids = plan.productions_for_parent(clade);
  auto const key_begin = structural_class * keys.row_key_width;
  std::size_t row_key_offset = 0;
  for (std::size_t prod_i = 0; prod_i < production_ids.size(); ++prod_i) {
    auto const pid = production_ids[prod_i];
    auto const& production = plan.production(pid);
    auto const children = plan.children(pid);
    if (!production.is_binary()) ++multifurcation_counter;

    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      chart_cost candidate = 0;
      for (std::size_t child_i = 0; child_i < children.size(); ++child_i) {
        auto const child = children[child_i];
        auto const class_index = static_cast<std::size_t>(
            words[key_begin + row_key_offset + child_i]);
        auto const& child_rows = chart.inside_rows_by_clade[child];
        if (class_index >= child_rows.size()) {
          throw std::runtime_error(
              "lazy chart: child inside class index out of range");
        }

        chart_cost best_child = chart_inf;
        auto const& child_row = child_rows[class_index];
        for (std::uint8_t child_state = 0; child_state < nuc_state_count;
             ++child_state) {
          best_child = std::min(
              best_child,
              parsimony_chart_detail::saturated_add(
                  child_row[child_state],
                  static_cast<chart_cost>(
                      plan.transition_cost(parent_state, child_state))));
        }
        candidate =
            parsimony_chart_detail::saturated_add(candidate, best_child);
      }
      row[parent_state] = std::min(row[parent_state], candidate);
    }
    row_key_offset += children.size();
  }
  if (row_key_offset != keys.row_key_width) {
    throw std::runtime_error("lazy chart: packed row-key width mismatch");
  }
  return row;
}

inline void assign_plan_leaf_classes(lazy_multisite_chart& chart,
                                     chart_execution_plan const& plan,
                                     site_pattern_set const& patterns,
                                     clade_id clade,
                                     lazy_chart_options const& options) {
  auto const& descriptor = plan.clade(clade);
  if (!descriptor.is_leaf()) {
    throw std::runtime_error("lazy chart: leaf class assignment got non-leaf");
  }

  std::array<std::size_t, nuc_state_count> class_by_state{};
  class_by_state.fill(std::numeric_limits<std::size_t>::max());
  auto const retain_map =
      options.retain_all_inside_class_maps || clade == plan.root_clade();
  auto& class_map = chart.class_index_by_pattern_by_clade[clade];
  auto& structural_map =
      chart.structural_class_index_by_pattern_by_clade[clade];
  if (retain_map) {
    if (!class_map) class_map.emplace();
    if (!structural_map) structural_map.emplace();
    class_map->assign(chart.pattern_count, 0);
    structural_map->assign(chart.pattern_count, 0);
  } else {
    class_map = std::nullopt;
    structural_map = std::nullopt;
  }
  auto& rows = chart.inside_rows_by_clade[clade];
  auto& weights = chart.class_weight_by_clade[clade];
  rows.clear();
  weights.clear();

  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto const observed = patterns.patterns[pattern_index]
                              .state_by_taxon[descriptor.leaf_taxon];
    auto& class_index = class_by_state[observed];
    if (class_index == std::numeric_limits<std::size_t>::max()) {
      class_index = rows.size();
      auto row = parsimony_chart_detail::make_inf_row();
      row[observed] = 0;
      rows.push_back(row);
      weights.push_back(0);
    }
    if (retain_map) {
      (*class_map)[pattern_index] = class_index;
      (*structural_map)[pattern_index] = class_index;
    }
    checked_add_weight(weights[class_index],
                       patterns.patterns[pattern_index].weight, "leaf class");
  }

  chart.structural_class_count_by_clade[clade] = rows.size();
}

struct plan_inside_clade_work_stats {
  std::size_t multifurcation_productions_scored = 0;
  std::size_t lazy_remerge_collisions = 0;
};

inline void assign_plan_internal_classes_task_local(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id clade,
    packed_plan_parent_keys const& keys, plan_inside_clade_work_stats& stats) {
  auto const& structural_classes = keys.structural_classes();
  auto const& row_key_classes = keys.row_key_classes();
  if (structural_classes.class_by_input.size() != chart.pattern_count ||
      row_key_classes.class_by_input.size() !=
          structural_classes.class_count()) {
    throw std::runtime_error("lazy chart: packed parent-key count mismatch");
  }
  auto& workspace = *keys.storage;
  auto& structural_to_row_class = workspace.structural_to_row_class;
  auto& row_key_to_row_class = workspace.row_key_to_row_class;
  auto& computed_rows = workspace.computed_rows;
  structural_to_row_class.assign(structural_classes.class_count(), 0);
  row_key_to_row_class.assign(row_key_classes.class_count(), 0);
  computed_rows.resize(row_key_classes.class_count());
  auto& rows = chart.inside_rows_by_clade[clade];
  rows.clear();

  // Row-key class IDs are first-occurrence IDs over structural classes. This
  // is exactly the former structural-class traversal with duplicate key
  // computations skipped, without depending on lexicographic map order.
  for (std::size_t row_key_class = 0;
       row_key_class < row_key_classes.class_count(); ++row_key_class) {
    auto const structural_class =
        row_key_classes.representative_by_class[row_key_class];
    computed_rows[row_key_class] = compute_plan_internal_inside_row_from_keys(
        chart, plan, clade, keys, structural_class,
        stats.multifurcation_productions_scored);
  }

  using namespace lazy_key_grouping_detail;
  auto const row_word_count =
      checked_packed_key_count_multiply(computed_rows.size(), nuc_state_count,
                                        "lazy plan internal row-value keys");
  prepare_packed_key_word_buffer(row_word_count, workspace.packed_words);
  workspace.packed_words.resize(row_word_count);
  auto word = workspace.packed_words.begin();
  for (auto const& row : computed_rows) {
    for (auto cost : row) *word++ = cost;
  }
  auto const row_value_preparation = prepare_packed_key_grouping_storage(
      computed_rows.size(), workspace.grouping_workspace,
      workspace.row_value_grouping);
  auto const row_value_view = packed_key_matrix_view{
      .key_count = computed_rows.size(),
      .key_width = nuc_state_count,
      .words = workspace.packed_words,
  };
  require_plan_parent_key_grouping_success(try_group_packed_keys_prepared(
      row_value_view, workspace.grouping_workspace,
      workspace.row_value_grouping,
      row_value_preparation.prepared_owned_capacity_resident_bytes));
  auto const& row_value_classes = workspace.row_value_grouping;
  rows.resize(row_value_classes.class_count());
  for (std::size_t row_class = 0; row_class < rows.size(); ++row_class) {
    rows[row_class] =
        computed_rows[row_value_classes.representative_by_class[row_class]];
  }
  for (std::size_t row_key_class = 0;
       row_key_class < row_key_classes.class_count(); ++row_key_class) {
    row_key_to_row_class[row_key_class] =
        row_value_classes.class_by_input[row_key_class];
  }
  for (std::size_t structural_class = 0;
       structural_class < structural_classes.class_count();
       ++structural_class) {
    structural_to_row_class[structural_class] =
        row_key_to_row_class[row_key_classes.class_by_input[structural_class]];
  }

  auto& class_map = chart.class_index_by_pattern_by_clade[clade];
  auto& structural_map =
      chart.structural_class_index_by_pattern_by_clade[clade];
  if (!class_map) class_map.emplace();
  if (!structural_map) structural_map.emplace();
  class_map->assign(chart.pattern_count, 0);
  structural_map->assign(structural_classes.class_by_input.begin(),
                         structural_classes.class_by_input.end());
  auto& weights = chart.class_weight_by_clade[clade];
  weights.assign(rows.size(), 0);
  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto const structural_class = (*structural_map)[pattern_index];
    auto const row_class = structural_to_row_class[structural_class];
    (*class_map)[pattern_index] = row_class;
    checked_add_weight(weights[row_class],
                       patterns.patterns[pattern_index].weight,
                       "internal class");
  }

  chart.structural_class_count_by_clade[clade] =
      structural_classes.class_count();
  if (structural_classes.class_count() > rows.size()) {
    stats.lazy_remerge_collisions +=
        structural_classes.class_count() - rows.size();
  }
}

inline void add_plan_inside_clade_work_stats(
    lazy_multisite_chart& chart, plan_inside_clade_work_stats const& stats) {
  chart.multifurcation_productions_scored +=
      stats.multifurcation_productions_scored;
  chart.lazy_remerge_collisions += stats.lazy_remerge_collisions;
}

inline void assign_plan_internal_classes(lazy_multisite_chart& chart,
                                         chart_execution_plan const& plan,
                                         site_pattern_set const& patterns,
                                         clade_id clade,
                                         packed_plan_parent_keys const& keys) {
  plan_inside_clade_work_stats stats;
  try {
    assign_plan_internal_classes_task_local(chart, plan, patterns, clade, keys,
                                            stats);
  } catch (...) {
    add_plan_inside_clade_work_stats(chart, stats);
    throw;
  }
  add_plan_inside_clade_work_stats(chart, stats);
}

inline void maybe_discard_nonroot_maps(lazy_multisite_chart& chart,
                                       chart_execution_plan const& plan,
                                       lazy_chart_options const& options) {
  if (options.retain_all_inside_class_maps) return;
  for (clade_id clade = 0; clade < chart.inside_rows_by_clade.size(); ++clade) {
    if (clade == plan.root_clade()) continue;
    chart.class_index_by_pattern_by_clade[clade] = std::nullopt;
    chart.structural_class_index_by_pattern_by_clade[clade] = std::nullopt;
  }
}

inline void materialize_inside_class_maps(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns) {
  plan.assert_valid();
  validate_patterns(plan, patterns);
  auto const clade_count = plan.clades().size();
  if (chart.pattern_count != patterns.patterns.size()) {
    throw std::runtime_error(
        "lazy chart: pattern count does not match lazy inside chart");
  }
  if (chart.inside_rows_by_clade.size() != clade_count ||
      chart.class_index_by_pattern_by_clade.size() != clade_count ||
      chart.structural_class_index_by_pattern_by_clade.size() != clade_count ||
      chart.structural_class_count_by_clade.size() != clade_count) {
    throw std::runtime_error("lazy chart: inside chart clade count mismatch");
  }

  plan_parent_key_workspace key_workspace;
  for (auto clade : plan.bottom_up_order()) {
    auto const& descriptor = plan.clade(clade);
    if (descriptor.is_leaf()) {
      if (!chart.class_index_by_pattern_by_clade[clade]) {
        std::vector<std::size_t> class_map(chart.pattern_count, 0);
        for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
          class_map[pattern] = plan_leaf_class_index_for_pattern(
              chart, plan, patterns, clade, pattern);
        }
        chart.class_index_by_pattern_by_clade[clade] = class_map;
        chart.structural_class_index_by_pattern_by_clade[clade] =
            std::move(class_map);
      }
      continue;
    }

    if (chart.class_index_by_pattern_by_clade[clade] &&
        chart.structural_class_index_by_pattern_by_clade[clade]) {
      continue;
    }

    auto const keys =
        collect_plan_parent_keys(chart, plan, patterns, clade, key_workspace);
    auto const& structural_classes = keys.structural_classes();
    auto structural_map = structural_classes.class_by_input;
    if (chart.structural_class_count_by_clade[clade] !=
        structural_classes.class_count()) {
      throw std::runtime_error(
          "lazy chart: materialized structural class count mismatch");
    }

    std::map<row_type, std::size_t> row_class_by_row;
    auto const& rows = chart.inside_rows_by_clade[clade];
    for (std::size_t class_index = 0; class_index < rows.size(); ++class_index) {
      row_class_by_row.emplace(rows[class_index], class_index);
    }

    std::vector<std::size_t> structural_to_row_class(
        structural_classes.class_count(), 0);
    for (std::size_t structural_class = 0;
         structural_class < structural_classes.class_count();
         ++structural_class) {
      std::size_t ignored_multifurcation_counter = 0;
      auto const row = compute_plan_internal_inside_row_from_keys(
          chart, plan, clade, keys, structural_class,
          ignored_multifurcation_counter);
      auto const row_it = row_class_by_row.find(row);
      if (row_it == row_class_by_row.end()) {
        throw std::runtime_error(
            "lazy chart: materialized inside row not found in stored classes");
      }
      structural_to_row_class[structural_class] = row_it->second;
    }

    std::vector<std::size_t> class_map(chart.pattern_count, 0);
    for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
      class_map[pattern] = structural_to_row_class[structural_map[pattern]];
    }
    chart.class_index_by_pattern_by_clade[clade] = std::move(class_map);
    chart.structural_class_index_by_pattern_by_clade[clade] =
        std::move(structural_map);
  }
}

inline std::size_t outside_class_index_for_pattern(
    lazy_multisite_chart const& chart, clade_id clade, std::size_t pattern) {
  if (clade == no_clade ||
      clade >= chart.outside_class_index_by_pattern_by_clade.size()) {
    throw std::runtime_error("lazy chart: clade id out of range");
  }
  if (pattern >= chart.pattern_count) {
    throw std::runtime_error("lazy chart: pattern index out of range");
  }
  auto const& map = chart.outside_class_index_by_pattern_by_clade[clade];
  if (!map) {
    throw std::runtime_error(
        "lazy chart: missing parent outside class map for clade " +
        std::to_string(clade));
  }
  return (*map)[pattern];
}

struct outside_context_key_grouping_memory_accounting {
  std::size_t key_count = 0;
  std::size_t key_width = 0;
  std::size_t class_count = 0;
  std::size_t logical_resident_bytes = 0;
  std::size_t actual_capacity_resident_bytes = 0;
  std::size_t observed_prepublication_peak_capacity_resident_bytes = 0;
  lazy_key_grouping_detail::packed_key_word_buffer_preparation_report
      word_preparation;
  lazy_key_grouping_detail::packed_key_grouping_preparation_report
      grouping_preparation;
};

struct outside_context_key_workspace {
  std::vector<lazy_key_grouping_detail::packed_key_word> packed_words;
  lazy_key_grouping_detail::packed_key_grouping_workspace grouping_workspace;
  lazy_key_grouping_detail::packed_key_grouping_result grouping;
  std::vector<row_type> outside_by_pattern;
  chart_trim_detail::generic_outside_recurrence_scratch recurrence_scratch;
  std::size_t outside_pattern_capacity_growths = 0;

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    using lazy_key_grouping_detail::checked_packed_key_bytes_add;
    using lazy_key_grouping_detail::checked_packed_key_bytes_multiply;
    auto total =
        lazy_key_grouping_detail::packed_key_word_buffer_dynamic_capacity_bytes(
            packed_words);
    total = checked_packed_key_bytes_add(
        total, grouping_workspace.dynamic_capacity_bytes(),
        "lazy outside context-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total, grouping.dynamic_capacity_bytes(),
        "lazy outside context-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total,
        checked_packed_key_bytes_multiply(
            outside_by_pattern.capacity(), sizeof(row_type),
            "lazy outside pattern-row scratch capacity"),
        "lazy outside context-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total,
        checked_packed_key_bytes_multiply(
            recurrence_scratch.result.capacity(), sizeof(row_type),
            "lazy outside recurrence result capacity"),
        "lazy outside context-key workspace capacity");
    for (auto const capacity : {recurrence_scratch.child_best.capacity(),
                                recurrence_scratch.prefix.capacity(),
                                recurrence_scratch.suffix.capacity()}) {
      total = checked_packed_key_bytes_add(
          total,
          checked_packed_key_bytes_multiply(
              capacity, sizeof(chart_cost),
              "lazy outside recurrence scalar capacity"),
          "lazy outside context-key workspace capacity");
    }
    return total;
  }

  [[nodiscard]] std::size_t resident_bytes() const {
    return lazy_key_grouping_detail::checked_packed_key_bytes_add(
        sizeof(*this), dynamic_capacity_bytes(),
        "lazy outside context-key workspace resident");
  }

  [[nodiscard]] bool has_capacity_for(std::size_t pattern_count,
                                      std::size_t maximum_context_key_width,
                                      std::size_t maximum_arity) const {
    using lazy_key_grouping_detail::checked_packed_key_count_add;
    using lazy_key_grouping_detail::checked_packed_key_count_multiply;
    auto const key_width =
        std::max<std::size_t>(maximum_context_key_width, nuc_state_count);
    auto const word_count = checked_packed_key_count_multiply(
        pattern_count, key_width, "lazy outside reusable packed words");
    auto const prefix_size = checked_packed_key_count_add(
        maximum_arity, 1, "lazy outside reusable recurrence width");
    return packed_words.capacity() >= word_count &&
           grouping_workspace.has_capacity_for(pattern_count) &&
           grouping.has_worst_case_capacity_for(pattern_count) &&
           outside_by_pattern.capacity() >= pattern_count &&
           recurrence_scratch.result.capacity() >= maximum_arity &&
           recurrence_scratch.child_best.capacity() >= maximum_arity &&
           recurrence_scratch.prefix.capacity() >= prefix_size &&
           recurrence_scratch.suffix.capacity() >= prefix_size;
  }
};

struct packed_outside_context_keys {
  outside_context_key_workspace* storage = nullptr;
  std::size_t key_width = 0;
  outside_context_key_grouping_memory_accounting memory;

  [[nodiscard]] outside_context_key_workspace const& workspace() const {
    if (storage == nullptr) {
      throw std::logic_error(
          "lazy chart: missing packed outside context-key storage");
    }
    return *storage;
  }

  [[nodiscard]] lazy_key_grouping_detail::packed_key_grouping_result const&
  classes() const {
    return workspace().grouping;
  }

  [[nodiscard]] std::span<lazy_key_grouping_detail::packed_key_word const>
  key_words() const {
    return workspace().packed_words;
  }
};

inline std::size_t estimate_outside_context_key_grouping_logical_resident_bytes(
    std::size_t key_count, std::size_t key_width,
    std::size_t class_count) {
  using namespace lazy_key_grouping_detail;
  auto const estimate = estimate_packed_key_grouping_memory(key_count,
                                                             key_width);
  auto total = checked_packed_key_bytes_add(
      sizeof(outside_context_key_workspace),
      sizeof(packed_outside_context_keys),
      "lazy outside context-key fixed resident estimate");
  total = checked_packed_key_bytes_add(
      total, estimate.packed_payload_logical_bytes,
      "lazy outside context-key payload estimate");
  total = checked_packed_key_bytes_add(
      total, estimate.workspace_logical_dynamic_bytes,
      "lazy outside context-key grouping-workspace estimate");
  return checked_packed_key_bytes_add(
      total,
      estimate_packed_key_grouping_result_logical_dynamic_bytes(key_count,
                                                                class_count),
      "lazy outside context-key grouping-result estimate");
}

inline void record_outside_context_key_preparation_peak(
    outside_context_key_grouping_memory_accounting& accounting,
    std::size_t current_workspace_resident_bytes,
    std::size_t previous_component_resident_bytes,
    std::size_t observed_component_peak_resident_bytes) {
  if (observed_component_peak_resident_bytes <
      previous_component_resident_bytes) {
    throw std::logic_error(
        "lazy chart: packed-key preparation peak is below prior resident");
  }
  auto const staged_extra = observed_component_peak_resident_bytes -
                            previous_component_resident_bytes;
  auto peak = lazy_key_grouping_detail::checked_packed_key_bytes_add(
      sizeof(packed_outside_context_keys), current_workspace_resident_bytes,
      "lazy outside context-key preparation peak");
  peak = lazy_key_grouping_detail::checked_packed_key_bytes_add(
      peak, staged_extra, "lazy outside context-key preparation peak");
  accounting.observed_prepublication_peak_capacity_resident_bytes = std::max(
      accounting.observed_prepublication_peak_capacity_resident_bytes, peak);
}

inline void require_outside_context_key_grouping_success(
    lazy_key_grouping_detail::packed_key_grouping_prepared_status status) {
  if (!status.succeeded()) {
    lazy_key_grouping_detail::throw_packed_key_grouping_prepared_failure(
        status);
  }
}

template <class PopulateWords>
inline packed_outside_context_keys collect_packed_outside_context_keys(
    std::size_t key_count, std::size_t key_width,
    outside_context_key_workspace& workspace, PopulateWords&& populate_words) {
  using namespace lazy_key_grouping_detail;
  packed_outside_context_keys keys;
  keys.storage = &workspace;
  keys.key_width = key_width;
  keys.memory.key_count = key_count;
  keys.memory.key_width = key_width;

  auto const estimate = estimate_packed_key_grouping_memory(key_count,
                                                             key_width);
  auto workspace_before = workspace.resident_bytes();
  keys.memory.word_preparation = prepare_packed_key_word_buffer(
      estimate.packed_word_count, workspace.packed_words);
  record_outside_context_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.word_preparation.previous_capacity_resident_bytes,
      keys.memory.word_preparation
          .observed_prepublication_peak_capacity_resident_bytes);
  workspace.packed_words.resize(estimate.packed_word_count);
  std::forward<PopulateWords>(populate_words)(
      std::span<packed_key_word>{workspace.packed_words});

  workspace_before = workspace.resident_bytes();
  keys.memory.grouping_preparation = prepare_packed_key_grouping_storage(
      key_count, workspace.grouping_workspace, workspace.grouping);
  record_outside_context_key_preparation_peak(
      keys.memory, workspace_before,
      keys.memory.grouping_preparation.previous_owned_capacity_resident_bytes,
      keys.memory.grouping_preparation
          .observed_prepublication_peak_owned_capacity_resident_bytes);
  auto const view = packed_key_matrix_view{
      .key_count = key_count,
      .key_width = key_width,
      .words = workspace.packed_words,
  };
  require_outside_context_key_grouping_success(try_group_packed_keys_prepared(
      view, workspace.grouping_workspace, workspace.grouping,
      keys.memory.grouping_preparation
          .prepared_owned_capacity_resident_bytes));
  keys.memory.class_count = workspace.grouping.class_count();

  keys.memory.logical_resident_bytes =
      estimate_outside_context_key_grouping_logical_resident_bytes(
          key_count, key_width, keys.memory.class_count);
  keys.memory.actual_capacity_resident_bytes = checked_packed_key_bytes_add(
      sizeof(packed_outside_context_keys), workspace.resident_bytes(),
      "lazy outside context-key actual resident");
  keys.memory.observed_prepublication_peak_capacity_resident_bytes = std::max(
      keys.memory.observed_prepublication_peak_capacity_resident_bytes,
      keys.memory.actual_capacity_resident_bytes);
  return keys;
}

inline row_type compute_child_outside_contribution(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, grammar_production const& prod,
    std::size_t child_slot, std::size_t representative_pattern,
    std::size_t parent_outside_class,
    outside_recurrence_work_stats& recurrence_work) {
  if (child_slot >= prod.children.size()) {
    throw std::runtime_error("lazy chart: child slot out of range");
  }
  auto parent = prod.parent;
  auto const& parent_outside_rows = chart.outside_rows_by_clade[parent];
  if (parent_outside_class >= parent_outside_rows.size()) {
    throw std::runtime_error("lazy chart: parent outside class out of range");
  }
  auto const& parent_outside = parent_outside_rows[parent_outside_class];
  auto result = parsimony_chart_detail::make_inf_row();

  auto inside_provider = [&](clade_id child) -> row_type const& {
    auto class_index = inside_class_index_for_pattern(
        chart, grammar, patterns, child, representative_pattern);
    auto const& child_rows = chart.inside_rows_by_clade[child];
    if (class_index >= child_rows.size()) {
      throw std::runtime_error(
          "lazy chart: child inside class index out of range");
    }
    return child_rows[class_index];
  };
  auto consume_row = [&](std::size_t candidate_slot,
                         row_type const& contribution) {
    if (candidate_slot != child_slot) return;
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      result[child_state] =
          std::min(result[child_state], contribution[child_state]);
    }
  };
  chart_trim_detail::scatter_production_outside_rows(
      prod, parent_outside, inside_provider, consume_row, recurrence_work);

  return result;
}

inline packed_outside_context_keys collect_outside_context_keys(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, grammar_production const& prod,
    outside_context_key_workspace& workspace) {
  using namespace lazy_key_grouping_detail;
  auto const key_width = checked_packed_key_count_add(
      prod.children.size(), 1, "lazy grammar outside context-key width");
  return collect_packed_outside_context_keys(
      chart.pattern_count, key_width, workspace,
      [&](std::span<packed_key_word> words) {
        auto output = words.begin();
        for (std::size_t pattern = 0; pattern < chart.pattern_count;
             ++pattern) {
          *output++ = checked_packed_key_word(
              outside_class_index_for_pattern(chart, prod.parent, pattern),
              "lazy grammar parent outside class index");
          for (auto child : prod.children) {
            *output++ = checked_packed_key_word(
                inside_class_index_for_pattern(chart, grammar, patterns, child,
                                               pattern),
                "lazy grammar outside-context inside class index");
          }
        }
        if (output != words.end()) {
          throw std::logic_error(
              "lazy chart: grammar outside context-key width mismatch");
        }
      });
}

inline void assign_outside_classes_for_clade(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade,
    outside_context_key_workspace& key_workspace) {
  std::vector<row_type> outside_by_pattern(chart.pattern_count,
                                           parsimony_chart_detail::make_inf_row());

  for (auto pid : grammar.productions_by_child[clade]) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("lazy chart: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    auto child_it = std::find(prod.children.begin(), prod.children.end(), clade);
    if (child_it == prod.children.end()) {
      throw std::runtime_error(
          "lazy chart: productions_by_child contains mismatched child");
    }
    auto child_slot =
        static_cast<std::size_t>(std::distance(prod.children.begin(), child_it));
    parsimony_chart_detail::validate_production_inside_row_inputs(
        grammar, prod, pid, "lazy outside chart");

    auto const context_keys = collect_outside_context_keys(
        chart, grammar, patterns, prod, key_workspace);
    auto const& context_classes = context_keys.classes();

    // The former ordered map traversed contexts lexicographically. Packed
    // class IDs are first-occurrence IDs, so use the explicit lexicographic
    // order to preserve contribution, counter, and exception order exactly.
    for (auto context_class : context_classes.lexicographic_class_order) {
      auto const representative =
          context_classes.representative_by_class[context_class];
      auto parent_outside_class =
          outside_class_index_for_pattern(chart, prod.parent, representative);
      if (prod.children.size() != 2) {
        ++chart.outside_multifurcation_productions_scored;
      }
      auto contribution = compute_child_outside_contribution(
          chart, grammar, patterns, prod, child_slot, representative,
          parent_outside_class, chart.outside_recurrence_work);
      for (auto pattern : context_classes.members_for_class(context_class)) {
        auto& row = outside_by_pattern[pattern];
        for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
          row[state] = std::min(row[state], contribution[state]);
        }
      }
    }
  }

  std::map<row_type, std::size_t> class_by_row;
  std::vector<std::size_t> class_map(chart.pattern_count, 0);
  auto& rows = chart.outside_rows_by_clade[clade];
  auto& weights = chart.outside_class_weight_by_clade[clade];
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    auto const& row = outside_by_pattern[pattern];
    auto [it, inserted] = class_by_row.emplace(row, class_by_row.size());
    if (inserted) {
      rows.push_back(row);
      weights.push_back(0);
    }
    auto class_index = it->second;
    class_map[pattern] = class_index;
    checked_add_weight(weights[class_index], patterns.patterns[pattern].weight,
                       "outside class");
  }

  chart.outside_class_index_by_pattern_by_clade[clade] = std::move(class_map);
}

// Keep the existing single-clade boundary source compatible. Full outside
// construction passes reusable scratch explicitly; isolated refresh callers
// own scratch for the duration of this one assignment.
inline void assign_outside_classes_for_clade(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade) {
  outside_context_key_workspace key_workspace;
  assign_outside_classes_for_clade(chart, grammar, patterns, clade,
                                   key_workspace);
}

// Trusted-plan counterparts of the lazy outside helpers above.  The compiled
// child occurrence records supply both the production and child slot, so this
// path never scans productions_by_child or searches a production's children.
inline row_type compute_child_outside_contribution(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, production_id pid, std::size_t child_slot,
    std::size_t representative_pattern, std::size_t parent_outside_class,
    outside_recurrence_work_stats& recurrence_work,
    chart_trim_detail::generic_outside_recurrence_scratch& recurrence_scratch) {
  auto const& production = plan.production(pid);
  auto const children = plan.children(pid);
  if (child_slot >= children.size()) {
    throw std::runtime_error("lazy chart: child slot out of range");
  }
  auto const& parent_outside_rows =
      chart.outside_rows_by_clade[production.parent];
  if (parent_outside_class >= parent_outside_rows.size()) {
    throw std::runtime_error("lazy chart: parent outside class out of range");
  }
  auto const& parent_outside = parent_outside_rows[parent_outside_class];
  auto result = parsimony_chart_detail::make_inf_row();

  auto inside_provider = [&](clade_id child) -> row_type const& {
    auto const class_index = plan_inside_class_index_for_pattern(
        chart, plan, patterns, child, representative_pattern);
    auto const& child_rows = chart.inside_rows_by_clade[child];
    if (class_index >= child_rows.size()) {
      throw std::runtime_error(
          "lazy chart: child inside class index out of range");
    }
    return child_rows[class_index];
  };
  auto consume_row = [&](std::size_t candidate_slot,
                         row_type const& contribution) {
    if (candidate_slot != child_slot) return;
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      result[child_state] =
          std::min(result[child_state], contribution[child_state]);
    }
  };
  chart_trim_detail::scatter_production_outside_rows(
      plan, children, parent_outside, inside_provider, consume_row,
      recurrence_work, recurrence_scratch);

  return result;
}

inline packed_outside_context_keys collect_outside_context_keys(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, production_id pid,
    outside_context_key_workspace& workspace) {
  using namespace lazy_key_grouping_detail;
  auto const& production = plan.production(pid);
  auto const children = plan.children(pid);
  auto const key_width = checked_packed_key_count_add(
      children.size(), 1, "lazy plan outside context-key width");
  return collect_packed_outside_context_keys(
      chart.pattern_count, key_width, workspace,
      [&](std::span<packed_key_word> words) {
        auto output = words.begin();
        for (std::size_t pattern = 0; pattern < chart.pattern_count;
             ++pattern) {
          *output++ = checked_packed_key_word(
              outside_class_index_for_pattern(chart, production.parent,
                                              pattern),
              "lazy plan parent outside class index");
          for (auto child : children) {
            *output++ = checked_packed_key_word(
                plan_inside_class_index_for_pattern(chart, plan, patterns,
                                                    child, pattern),
                "lazy plan outside-context inside class index");
          }
        }
        if (output != words.end()) {
          throw std::logic_error(
              "lazy chart: plan outside context-key width mismatch");
        }
      });
}

struct plan_outside_clade_work_stats {
  std::size_t multifurcation_productions_scored = 0;
  outside_recurrence_work_stats recurrence_work;
};

inline void assign_plan_outside_classes_for_clade_task_local(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id clade,
    outside_context_key_workspace& key_workspace,
    plan_outside_clade_work_stats& stats) {
  auto& outside_by_pattern = key_workspace.outside_by_pattern;
  if (outside_by_pattern.capacity() < chart.pattern_count) {
    outside_by_pattern.reserve(chart.pattern_count);
    ++key_workspace.outside_pattern_capacity_growths;
  }
  outside_by_pattern.assign(chart.pattern_count,
                            parsimony_chart_detail::make_inf_row());

  for (auto const occurrence : plan.child_occurrences_for_clade(clade)) {
    auto const pid = occurrence.production;
    auto const child_slot = occurrence.child_slot;
    auto const& production = plan.production(pid);

    auto const context_keys = collect_outside_context_keys(
        chart, plan, patterns, pid, key_workspace);
    auto const& context_classes = context_keys.classes();

    for (auto context_class : context_classes.lexicographic_class_order) {
      auto const representative =
          context_classes.representative_by_class[context_class];
      auto const parent_outside_class = outside_class_index_for_pattern(
          chart, production.parent, representative);
      if (!production.is_binary()) {
        ++stats.multifurcation_productions_scored;
      }
      auto const contribution = compute_child_outside_contribution(
          chart, plan, patterns, pid, child_slot, representative,
          parent_outside_class, stats.recurrence_work,
          key_workspace.recurrence_scratch);
      for (auto pattern : context_classes.members_for_class(context_class)) {
        auto& row = outside_by_pattern[pattern];
        for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
          row[state] = std::min(row[state], contribution[state]);
        }
      }
    }
  }

  using namespace lazy_key_grouping_detail;
  auto const row_word_count = checked_packed_key_count_multiply(
      chart.pattern_count, nuc_state_count, "lazy plan outside row-value keys");
  prepare_packed_key_word_buffer(row_word_count, key_workspace.packed_words);
  key_workspace.packed_words.resize(row_word_count);
  auto word = key_workspace.packed_words.begin();
  for (auto const& row : outside_by_pattern) {
    for (auto cost : row) *word++ = cost;
  }
  auto const row_grouping_preparation = prepare_packed_key_grouping_storage(
      chart.pattern_count, key_workspace.grouping_workspace,
      key_workspace.grouping);
  auto const row_view = packed_key_matrix_view{
      .key_count = chart.pattern_count,
      .key_width = nuc_state_count,
      .words = key_workspace.packed_words,
  };
  require_outside_context_key_grouping_success(try_group_packed_keys_prepared(
      row_view, key_workspace.grouping_workspace, key_workspace.grouping,
      row_grouping_preparation.prepared_owned_capacity_resident_bytes));

  auto& class_map = chart.outside_class_index_by_pattern_by_clade[clade];
  if (!class_map) class_map.emplace();
  class_map->assign(key_workspace.grouping.class_by_input.begin(),
                    key_workspace.grouping.class_by_input.end());
  auto& rows = chart.outside_rows_by_clade[clade];
  auto& weights = chart.outside_class_weight_by_clade[clade];
  rows.resize(key_workspace.grouping.class_count());
  for (std::size_t row_class = 0; row_class < rows.size(); ++row_class) {
    rows[row_class] =
        outside_by_pattern[key_workspace.grouping
                               .representative_by_class[row_class]];
  }
  weights.assign(rows.size(), 0);
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    auto const class_index = (*class_map)[pattern];
    checked_add_weight(weights[class_index], patterns.patterns[pattern].weight,
                       "outside class");
  }
}

inline void add_plan_outside_clade_work_stats(
    lazy_multisite_chart& chart, plan_outside_clade_work_stats const& stats) {
  chart.outside_multifurcation_productions_scored +=
      stats.multifurcation_productions_scored;
  chart.outside_recurrence_work += stats.recurrence_work;
}

inline void assign_outside_classes_for_clade(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id clade,
    outside_context_key_workspace& key_workspace) {
  plan_outside_clade_work_stats stats;
  try {
    assign_plan_outside_classes_for_clade_task_local(
        chart, plan, patterns, clade, key_workspace, stats);
  } catch (...) {
    add_plan_outside_clade_work_stats(chart, stats);
    throw;
  }
  add_plan_outside_clade_work_stats(chart, stats);
}

inline void assign_outside_classes_for_clade(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, clade_id clade) {
  outside_context_key_workspace key_workspace;
  assign_outside_classes_for_clade(chart, plan, patterns, clade,
                                   key_workspace);
}

inline void finalize_outside_counters(lazy_multisite_chart& chart) {
  chart.lazy_outside_rows_computed = 0;
  for (auto const& rows : chart.outside_rows_by_clade) {
    chart.lazy_outside_rows_computed += rows.size();
  }
}

inline void validate_lazy_inside_score_inputs(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options,
    std::string_view context) {
  chart_multisite_detail::validate_multisite_inputs(grammar, patterns, options);
  if (chart.pattern_count != patterns.patterns.size()) {
    throw std::runtime_error(std::string{context} +
                             ": pattern count does not match lazy chart");
  }
  if (chart.inside_rows_by_clade.size() != grammar.clades.size() ||
      chart.class_weight_by_clade.size() != grammar.clades.size() ||
      chart.class_index_by_pattern_by_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(std::string{context} +
                             ": lazy inside chart clade count mismatch");
  }
}

// Root scoring consumes no leaf observations or production structure.  Its
// plan overload therefore checks only the immutable plan and the associations
// between the pattern set and stored chart.  The checked grammar overload
// above retains the full multisite-input validation contract.
inline void validate_lazy_inside_score_inputs(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, std::string_view context) {
  plan.assert_valid();
  if (patterns.taxon_count != 0 && patterns.taxon_count != plan.taxon_count()) {
    throw std::runtime_error(std::string{context} +
                             ": pattern taxon count does not match plan");
  }
  if (chart.pattern_count != patterns.patterns.size()) {
    throw std::runtime_error(std::string{context} +
                             ": pattern count does not match lazy chart");
  }
  auto const clade_count = plan.clades().size();
  if (chart.inside_rows_by_clade.size() != clade_count ||
      chart.class_weight_by_clade.size() != clade_count ||
      chart.class_index_by_pattern_by_clade.size() != clade_count) {
    throw std::runtime_error(std::string{context} +
                             ": lazy inside chart clade count mismatch");
  }
}

inline lazy_multisite_chart::row_type const& root_inside_row_for_pattern(
    lazy_multisite_chart const& chart, clade_id root, std::size_t pattern) {
  if (root == no_clade || root >= chart.inside_rows_by_clade.size()) {
    throw std::runtime_error("lazy root score: root clade out of range");
  }
  if (pattern >= chart.pattern_count) {
    throw std::runtime_error("lazy root score: pattern index out of range");
  }
  auto const& map = chart.class_index_by_pattern_by_clade[root];
  if (!map) {
    throw std::runtime_error(
        "lazy root score: root inside class map was not retained");
  }
  auto class_index = (*map)[pattern];
  auto const& rows = chart.inside_rows_by_clade[root];
  if (class_index >= rows.size()) {
    throw std::runtime_error("lazy root score: root class index out of range");
  }
  return rows[class_index];
}

inline std::uint64_t lazy_root_class_score_total(
    site_pattern_set const& patterns, lazy_multisite_chart const& chart,
    clade_id root, chart_options const& options, std::string_view context) {
  auto const& root_rows = chart.inside_rows_by_clade[root];
  if (root >= chart.class_weight_by_clade.size() ||
      chart.class_weight_by_clade[root].size() != root_rows.size()) {
    throw std::runtime_error(std::string{context} +
                             ": root class weights mismatch");
  }

  std::uint64_t total = 0;
  if (!options.score_ua_edge) {
    for (std::size_t class_index = 0; class_index < root_rows.size();
         ++class_index) {
      auto contribution = chart_multisite_detail::checked_mul_cost(
          chart.class_weight_by_clade[root][class_index],
          chart_multisite_detail::row_min(root_rows[class_index]),
          "lazy composite weighted root cost");
      total = chart_multisite_detail::checked_add_u64(
          total, contribution, "lazy composite lower bound");
    }
    return total;
  }

  auto const& root_map = chart.class_index_by_pattern_by_clade[root];
  if (!root_map) {
    throw std::runtime_error(std::string{context} +
                             ": root inside class map was not retained");
  }
  std::vector<std::array<std::uint64_t, nuc_state_count>> counts_by_class(
      root_rows.size());
  for (auto& counts : counts_by_class) counts.fill(0);
  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto class_index = (*root_map)[pattern_index];
    if (class_index >= root_rows.size()) {
      throw std::runtime_error(std::string{context} +
                               ": root class index out of range");
    }
    auto const& pattern = patterns.patterns[pattern_index];
    for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
         ++reference_state) {
      counts_by_class[class_index][reference_state] =
          chart_multisite_detail::checked_add_u64(
              counts_by_class[class_index][reference_state],
              pattern.reference_state_counts[reference_state],
              "lazy composite reference-state count");
    }
  }
  for (std::size_t class_index = 0; class_index < root_rows.size();
       ++class_index) {
    auto const& row = root_rows[class_index];
    for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
         ++reference_state) {
      auto count = counts_by_class[class_index][reference_state];
      if (count == 0) continue;
      chart_cost best = chart_inf;
      for (std::uint8_t root_state = 0; root_state < nuc_state_count;
           ++root_state) {
        best = std::min(
            best, parsimony_chart_detail::saturated_add(
                      row[root_state],
                      parsimony_chart_detail::transition_cost(reference_state,
                                                              root_state)));
      }
      total = chart_multisite_detail::checked_add_u64(
          total,
          chart_multisite_detail::checked_mul_cost(
              count, best, "lazy composite weighted root-edge cost"),
          "lazy composite root-edge lower bound");
    }
  }
  return total;
}

struct plan_lazy_chart_scheduler_test_hooks {
  std::function<void(clade_id, std::size_t, std::size_t)> before_inside_clade;
  std::function<void(clade_id, std::size_t, std::size_t)> after_inside_clade;
  std::function<void(lazy_multisite_chart const&, clade_id, std::size_t,
                     std::size_t)>
      observe_completed_inside_clade;
  std::function<void(lazy_multisite_chart const&, std::size_t)>
      observe_inside_level_join;
  std::function<void(lazy_multisite_chart const&, std::size_t)>
      observe_inside_level_reclamation;
  std::function<void(lazy_multisite_chart const&, std::size_t)>
      observe_inside_level_failure_cleanup;
  std::function<void(clade_id, std::size_t, std::size_t)> before_outside_clade;
  std::function<void(clade_id, std::size_t, std::size_t)> after_outside_clade;
  std::function<void(lazy_multisite_chart const&, std::size_t)>
      observe_outside_level_failure_cleanup;
};

// A finite lazy-chart build treats the configured byte limit as one live-set
// envelope.  retained_resident_bytes is storage owned by the caller/state that
// overlaps this build; the builders add their chart, coordinator arrays,
// outputs, and measured scratch capacities to it.
struct plan_lazy_chart_memory_options {
  std::size_t memory_budget_bytes = 0;  // 0 preserves unlimited behaviour.
  std::size_t retained_resident_bytes = 0;
};

struct plan_lazy_chart_memory_report {
  std::size_t memory_budget_bytes = 0;
  std::size_t inside_max_admitted_slots = 0;
  std::size_t outside_max_admitted_slots = 0;
  std::size_t inside_admission_waves = 0;
  std::size_t outside_admission_waves = 0;
  std::size_t inside_memory_limited_levels = 0;
  std::size_t outside_memory_limited_levels = 0;
  std::size_t inside_reused_slot_waves = 0;
  std::size_t outside_reused_slot_waves = 0;
  std::size_t inside_workspace_evictions = 0;
  std::size_t outside_workspace_evictions = 0;
  std::size_t inside_coordinator_capacity_resident_bytes = 0;
  std::size_t outside_coordinator_capacity_resident_bytes = 0;
  std::size_t preflight_peak_capacity_resident_bytes = 0;
  std::size_t actual_peak_capacity_resident_bytes = 0;
  std::size_t pre_submit_rejections = 0;
  std::size_t scheduler_submissions = 0;
};

enum class plan_lazy_chart_memory_phase : std::uint8_t {
  inside,
  outside,
};

// Fixed-size so the required singleton can be rejected without allocating an
// error message (and, critically, before invoking the scheduler).
class plan_lazy_chart_memory_budget_error : public std::exception {
 public:
  plan_lazy_chart_memory_budget_error(plan_lazy_chart_memory_phase phase,
                                      std::size_t required_bytes,
                                      std::size_t budget_bytes) noexcept
      : phase_(phase),
        required_bytes_(required_bytes),
        budget_bytes_(budget_bytes) {}

  [[nodiscard]] plan_lazy_chart_memory_phase phase() const noexcept {
    return phase_;
  }
  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }
  [[nodiscard]] std::size_t budget_bytes() const noexcept {
    return budget_bytes_;
  }
  [[nodiscard]] char const* what() const noexcept override {
    return "lazy chart scheduled state-build capacity exceeds its budget";
  }

 private:
  plan_lazy_chart_memory_phase phase_;
  std::size_t required_bytes_;
  std::size_t budget_bytes_;
};

inline std::size_t plan_lazy_chart_capacity_add(std::size_t lhs,
                                                std::size_t rhs,
                                                std::string_view context) {
  return lazy_key_grouping_detail::checked_packed_key_bytes_add(lhs, rhs,
                                                                context);
}

inline std::size_t plan_lazy_chart_capacity_multiply(std::size_t lhs,
                                                     std::size_t rhs,
                                                     std::string_view context) {
  return lazy_key_grouping_detail::checked_packed_key_bytes_multiply(lhs, rhs,
                                                                     context);
}

template <class Vector>
inline std::size_t plan_lazy_chart_vector_capacity_bytes(
    Vector const& values, std::string_view context) {
  return plan_lazy_chart_capacity_multiply(
      values.capacity(), sizeof(typename Vector::value_type), context);
}

inline std::size_t lazy_multisite_chart_capacity_resident_bytes(
    lazy_multisite_chart const& chart) {
  std::size_t total = sizeof(chart);
  auto add_vector = [&](auto const& values, std::string_view context) {
    total = plan_lazy_chart_capacity_add(
        total, plan_lazy_chart_vector_capacity_bytes(values, context),
        "lazy chart capacity resident");
  };
  auto add_nested = [&](auto const& nested, std::string_view context) {
    add_vector(nested, context);
    for (auto const& values : nested) add_vector(values, context);
  };
  auto add_optional_maps = [&](auto const& maps, std::string_view context) {
    add_vector(maps, context);
    for (auto const& map : maps) {
      if (map) add_vector(*map, context);
    }
  };
  add_nested(chart.inside_rows_by_clade, "lazy inside rows");
  add_nested(chart.outside_rows_by_clade, "lazy outside rows");
  add_optional_maps(chart.class_index_by_pattern_by_clade,
                    "lazy inside class maps");
  add_optional_maps(chart.structural_class_index_by_pattern_by_clade,
                    "lazy structural class maps");
  add_optional_maps(chart.outside_class_index_by_pattern_by_clade,
                    "lazy outside class maps");
  add_vector(chart.structural_class_count_by_clade,
             "lazy structural class counts");
  add_nested(chart.class_weight_by_clade, "lazy inside class weights");
  add_nested(chart.outside_class_weight_by_clade, "lazy outside class weights");
  add_vector(chart.outside_global_min_by_pattern, "lazy outside global minima");
  return total;
}

struct plan_lazy_chart_scheduler_workspace {
  std::vector<plan_parent_key_workspace> inside_by_slot;
  std::vector<outside_context_key_workspace> outside_by_slot;

  void prepare_inside_slots(std::size_t slot_count) {
    if (inside_by_slot.size() < slot_count) inside_by_slot.resize(slot_count);
  }

  void prepare_outside_slots(std::size_t slot_count) {
    if (outside_by_slot.size() < slot_count) {
      outside_by_slot.resize(slot_count);
    }
  }

  void hard_bound_inside_slots(std::size_t slot_count) {
    if (inside_by_slot.size() == slot_count &&
        inside_by_slot.capacity() <= slot_count) {
      return;
    }
    std::vector<plan_parent_key_workspace> replacement(slot_count);
    inside_by_slot.swap(replacement);
  }

  void hard_bound_outside_slots(std::size_t slot_count) {
    if (outside_by_slot.size() == slot_count &&
        outside_by_slot.capacity() <= slot_count) {
      return;
    }
    std::vector<outside_context_key_workspace> replacement(slot_count);
    outside_by_slot.swap(replacement);
  }

  void release_inside() noexcept {
    std::vector<plan_parent_key_workspace>{}.swap(inside_by_slot);
  }

  void release_outside() noexcept {
    std::vector<outside_context_key_workspace>{}.swap(outside_by_slot);
  }

  [[nodiscard]] bool can_reuse_inside_slots(
      std::size_t slot_count, std::size_t pattern_count,
      std::size_t maximum_row_key_width) const {
    return inside_by_slot.size() >= slot_count &&
           std::ranges::all_of(inside_by_slot.begin(),
                               inside_by_slot.begin() + slot_count,
                               [&](auto const& workspace) {
                                 return workspace.has_capacity_for(
                                     pattern_count, maximum_row_key_width);
                               });
  }

  [[nodiscard]] bool can_reuse_outside_slots(
      std::size_t slot_count, std::size_t pattern_count,
      std::size_t maximum_context_key_width, std::size_t maximum_arity) const {
    return outside_by_slot.size() >= slot_count &&
           std::ranges::all_of(
               outside_by_slot.begin(), outside_by_slot.begin() + slot_count,
               [&](auto const& workspace) {
                 return workspace.has_capacity_for(
                     pattern_count, maximum_context_key_width, maximum_arity);
               });
  }

  [[nodiscard]] std::size_t inside_capacity_resident_bytes() const {
    auto total = plan_lazy_chart_capacity_add(
        sizeof(*this),
        plan_lazy_chart_vector_capacity_bytes(inside_by_slot,
                                              "lazy inside slot array"),
        "lazy inside scheduler workspace");
    for (auto const& workspace : inside_by_slot) {
      total = plan_lazy_chart_capacity_add(total,
                                           workspace.dynamic_capacity_bytes(),
                                           "lazy inside scheduler workspace");
    }
    return total;
  }

  [[nodiscard]] std::size_t outside_capacity_resident_bytes() const {
    auto total = plan_lazy_chart_capacity_add(
        sizeof(*this),
        plan_lazy_chart_vector_capacity_bytes(outside_by_slot,
                                              "lazy outside slot array"),
        "lazy outside scheduler workspace");
    for (auto const& workspace : outside_by_slot) {
      total = plan_lazy_chart_capacity_add(total,
                                           workspace.dynamic_capacity_bytes(),
                                           "lazy outside scheduler workspace");
    }
    return total;
  }
};

inline std::size_t logical_plan_inside_slot_resident_bytes(
    std::size_t pattern_count, std::size_t maximum_row_key_width) {
  using namespace lazy_key_grouping_detail;
  auto const key_width =
      std::max<std::size_t>(maximum_row_key_width, nuc_state_count);
  auto const words = checked_packed_key_count_multiply(
      pattern_count, key_width, "lazy inside admitted packed words");
  std::size_t total = sizeof(plan_parent_key_workspace);
  total = plan_lazy_chart_capacity_add(
      total,
      checked_packed_key_bytes_multiply(words, sizeof(packed_key_word),
                                        "lazy inside admitted packed words"),
      "lazy inside admitted slot");
  total = plan_lazy_chart_capacity_add(
      total,
      estimate_packed_key_grouping_workspace_logical_dynamic_bytes(
          pattern_count),
      "lazy inside admitted slot");
  auto const grouping =
      estimate_packed_key_grouping_result_logical_dynamic_bytes(pattern_count,
                                                                pattern_count);
  total = plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(grouping, 3, "lazy inside groupings"),
      "lazy inside admitted slot");
  total = plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(pattern_count, sizeof(row_type),
                                        "lazy inside computed rows"),
      "lazy inside admitted slot");
  return plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(pattern_count, 2 * sizeof(std::size_t),
                                        "lazy inside row-class maps"),
      "lazy inside admitted slot");
}

inline std::size_t logical_plan_inside_slot_preparation_extra_bytes(
    std::size_t pattern_count) {
  // Preparing row_grouping and row_value_grouping stages a fresh grouping
  // workspace together with the new result while the already prepared shared
  // workspace and prior results remain live.
  auto const dynamic = lazy_key_grouping_detail::
      estimate_packed_key_grouping_workspace_logical_dynamic_bytes(
          pattern_count);
  return plan_lazy_chart_capacity_add(
      dynamic,
      sizeof(lazy_key_grouping_detail::packed_key_grouping_workspace) +
          sizeof(lazy_key_grouping_detail::packed_key_grouping_result),
      "lazy inside grouping staging fixed resident");
}

inline std::size_t logical_plan_outside_slot_preparation_extra_bytes() {
  // Even a cold slot stages grouping ownership before publication. Its dynamic
  // capacity is already present in the final slot envelope, but the temporary
  // workspace/result objects overlap that final capacity.
  return sizeof(lazy_key_grouping_detail::packed_key_grouping_workspace) +
         sizeof(lazy_key_grouping_detail::packed_key_grouping_result);
}

inline std::size_t logical_plan_output_preparation_extra_bytes() {
  // Output vectors are prepared one at a time through an unpublished staged
  // vector. Dynamic storage is included in the final output envelope.
  return sizeof(std::vector<row_type>);
}

inline std::size_t logical_plan_outside_slot_resident_bytes(
    std::size_t pattern_count, std::size_t maximum_context_key_width,
    std::size_t maximum_arity) {
  using namespace lazy_key_grouping_detail;
  auto const key_width =
      std::max<std::size_t>(maximum_context_key_width, nuc_state_count);
  auto const words = checked_packed_key_count_multiply(
      pattern_count, key_width, "lazy outside admitted packed words");
  std::size_t total = sizeof(outside_context_key_workspace);
  total = plan_lazy_chart_capacity_add(
      total,
      checked_packed_key_bytes_multiply(words, sizeof(packed_key_word),
                                        "lazy outside admitted packed words"),
      "lazy outside admitted slot");
  total = plan_lazy_chart_capacity_add(
      total,
      estimate_packed_key_grouping_workspace_logical_dynamic_bytes(
          pattern_count),
      "lazy outside admitted slot");
  total = plan_lazy_chart_capacity_add(
      total,
      estimate_packed_key_grouping_result_logical_dynamic_bytes(pattern_count,
                                                                pattern_count),
      "lazy outside admitted slot");
  total = plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(pattern_count, sizeof(row_type),
                                        "lazy outside pattern rows"),
      "lazy outside admitted slot");
  total = plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(maximum_arity, sizeof(row_type),
                                        "lazy outside recurrence rows"),
      "lazy outside admitted slot");
  auto const scalar_count = checked_packed_key_count_add(
      maximum_arity,
      checked_packed_key_count_multiply(
          checked_packed_key_count_add(maximum_arity, 1,
                                       "lazy outside recurrence width"),
          2, "lazy outside recurrence prefix/suffix"),
      "lazy outside recurrence scalar count");
  return plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(scalar_count, sizeof(chart_cost),
                                        "lazy outside recurrence scalars"),
      "lazy outside admitted slot");
}

struct plan_lazy_slot_preparation_report {
  std::size_t stable_resident_bytes = 0;
  std::size_t peak_resident_bytes = 0;
};

[[noreturn]] inline void throw_rebased_plan_lazy_preparation_budget_error(
    lazy_key_grouping_detail::packed_key_grouping_budget_error const& error,
    std::size_t resident_bytes_outside_component,
    std::size_t rebased_budget_bytes, std::string_view context) {
  auto const required = plan_lazy_chart_capacity_add(
      resident_bytes_outside_component, error.required_bytes(), context);
  auto const observed =
      error.observed_peak_bytes() == 0
          ? 0
          : plan_lazy_chart_capacity_add(resident_bytes_outside_component,
                                         error.observed_peak_bytes(), context);
  throw lazy_key_grouping_detail::packed_key_grouping_budget_error(
      required, rebased_budget_bytes, observed);
}

inline std::size_t plan_lazy_rejected_preparation_actual_peak(
    lazy_key_grouping_detail::packed_key_grouping_budget_error const& error,
    std::size_t resident_bytes_outside_component,
    std::size_t published_resident_bytes, std::string_view context) {
  if (error.observed_peak_bytes() == 0) return published_resident_bytes;
  return std::max(
      published_resident_bytes,
      plan_lazy_chart_capacity_add(resident_bytes_outside_component,
                                   error.observed_peak_bytes(), context));
}

// Serial staging for vector capacities that become worker-owned output or
// scratch.  The frozen C++26 libstdc++ vector reserve contract is projected as
// exactly the requested capacity; the measured capacity is nevertheless
// checked before publication.  Rejection destroys staging and preserves the
// caller's vector, so an admitted worker never discovers allocator growth.
template <class Vector>
inline lazy_key_grouping_detail::packed_key_word_buffer_preparation_report
prepare_plan_lazy_vector_storage(
    std::size_t count, Vector& values,
    std::size_t measured_capacity_acceptance_limit_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  using namespace lazy_key_grouping_detail;
  auto resident = [&](Vector const& vector, std::string_view context) {
    return checked_packed_key_bytes_add(
        sizeof(vector),
        checked_packed_key_bytes_multiply(
            vector.capacity(), sizeof(typename Vector::value_type), context),
        context);
  };
  auto const previous = resident(values, "lazy prepared vector capacity");
  if (values.capacity() >= count) {
    if (previous > measured_capacity_acceptance_limit_bytes) {
      throw packed_key_grouping_budget_error(
          previous, measured_capacity_acceptance_limit_bytes);
    }
    return {
        .previous_capacity_resident_bytes = previous,
        .prepared_capacity_resident_bytes = previous,
        .observed_prepublication_peak_capacity_resident_bytes = previous,
        .newly_allocated_dynamic_capacity_bytes = 0,
        .reused_existing_capacity = true,
    };
  }

  auto const projected = checked_packed_key_bytes_add(
      sizeof(values),
      checked_packed_key_bytes_multiply(
          count, sizeof(typename Vector::value_type),
          "lazy prepared vector projected capacity"),
      "lazy prepared vector projected capacity");
  auto const projected_peak = checked_packed_key_bytes_add(
      previous, projected, "lazy prepared vector projected peak");
  if (projected_peak > measured_capacity_acceptance_limit_bytes) {
    throw packed_key_grouping_budget_error(
        projected_peak, measured_capacity_acceptance_limit_bytes);
  }

  Vector staged;
  staged.reserve(count);
  auto const prepared = resident(staged, "lazy prepared vector capacity");
  auto const measured_peak = checked_packed_key_bytes_add(
      previous, prepared, "lazy prepared vector measured peak");
  if (measured_peak > measured_capacity_acceptance_limit_bytes) {
    throw packed_key_grouping_budget_error(
        measured_peak, measured_capacity_acceptance_limit_bytes, measured_peak);
  }
  auto const allocated = checked_packed_key_bytes_multiply(
      staged.capacity(), sizeof(typename Vector::value_type),
      "lazy prepared vector allocated capacity");
  values.swap(staged);
  return {
      .previous_capacity_resident_bytes = previous,
      .prepared_capacity_resident_bytes = prepared,
      .observed_prepublication_peak_capacity_resident_bytes = measured_peak,
      .newly_allocated_dynamic_capacity_bytes = allocated,
      .reused_existing_capacity = false,
  };
}

inline plan_lazy_slot_preparation_report prepare_plan_inside_slot(
    plan_parent_key_workspace& workspace, std::size_t pattern_count,
    std::size_t maximum_row_key_width,
    std::size_t workspace_peak_acceptance_limit_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  using namespace lazy_key_grouping_detail;
  auto const key_width =
      std::max<std::size_t>(maximum_row_key_width, nuc_state_count);
  auto const word_count = checked_packed_key_count_multiply(
      pattern_count, key_width, "lazy inside prepared packed words");
  plan_lazy_slot_preparation_report report;
  auto record_component_peak = [&](std::size_t before,
                                   std::size_t component_before,
                                   std::size_t component_peak) {
    if (component_peak < component_before) {
      throw std::logic_error(
          "lazy inside slot preparation peak below prior component");
    }
    report.peak_resident_bytes = std::max(
        report.peak_resident_bytes,
        plan_lazy_chart_capacity_add(before, component_peak - component_before,
                                     "lazy inside slot preparation peak"));
  };
  auto component_limit = [&](std::size_t before, std::size_t component_before) {
    auto const other = before - component_before;
    if (other > workspace_peak_acceptance_limit_bytes) {
      throw packed_key_grouping_budget_error(
          before, workspace_peak_acceptance_limit_bytes);
    }
    return workspace_peak_acceptance_limit_bytes - other;
  };
  auto before = workspace.resident_bytes();
  auto const word_before =
      packed_key_word_buffer_resident_bytes(workspace.packed_words);
  packed_key_word_buffer_preparation_report word_report;
  try {
    word_report =
        prepare_packed_key_word_buffer(word_count, workspace.packed_words,
                                       component_limit(before, word_before));
  } catch (packed_key_grouping_budget_error const& error) {
    throw_rebased_plan_lazy_preparation_budget_error(
        error, before - word_before, workspace_peak_acceptance_limit_bytes,
        "lazy inside slot rejected word preparation");
  }
  record_component_peak(
      before, word_report.previous_capacity_resident_bytes,
      word_report.observed_prepublication_peak_capacity_resident_bytes);
  auto prepare_grouping = [&](packed_key_grouping_result& result) {
    auto const component_before = packed_key_grouping_owned_resident_bytes(
        workspace.grouping_workspace, result);
    auto const workspace_before = workspace.resident_bytes();
    packed_key_grouping_preparation_report grouping_report;
    try {
      grouping_report = prepare_packed_key_grouping_storage(
          pattern_count, workspace.grouping_workspace, result,
          component_limit(workspace_before, component_before));
    } catch (packed_key_grouping_budget_error const& error) {
      throw_rebased_plan_lazy_preparation_budget_error(
          error, workspace_before - component_before,
          workspace_peak_acceptance_limit_bytes,
          "lazy inside slot rejected grouping preparation");
    }
    record_component_peak(
        workspace_before, component_before,
        grouping_report
            .observed_prepublication_peak_owned_capacity_resident_bytes);
  };
  prepare_grouping(workspace.structural_grouping);
  prepare_grouping(workspace.row_grouping);
  prepare_grouping(workspace.row_value_grouping);
  auto prepare_vector = [&](auto& values, std::size_t count) {
    auto const vector_before =
        plan_lazy_chart_capacity_add(sizeof(values),
                                     plan_lazy_chart_vector_capacity_bytes(
                                         values, "lazy inside slot vector"),
                                     "lazy inside slot vector");
    auto const workspace_before = workspace.resident_bytes();
    lazy_key_grouping_detail::packed_key_word_buffer_preparation_report
        vector_report;
    try {
      vector_report = prepare_plan_lazy_vector_storage(
          count, values, component_limit(workspace_before, vector_before));
    } catch (packed_key_grouping_budget_error const& error) {
      throw_rebased_plan_lazy_preparation_budget_error(
          error, workspace_before - vector_before,
          workspace_peak_acceptance_limit_bytes,
          "lazy inside slot rejected vector preparation");
    }
    record_component_peak(
        workspace_before, vector_report.previous_capacity_resident_bytes,
        vector_report.observed_prepublication_peak_capacity_resident_bytes);
  };
  prepare_vector(workspace.computed_rows, pattern_count);
  prepare_vector(workspace.structural_to_row_class, pattern_count);
  prepare_vector(workspace.row_key_to_row_class, pattern_count);
  report.stable_resident_bytes = workspace.resident_bytes();
  report.peak_resident_bytes =
      std::max(report.peak_resident_bytes, report.stable_resident_bytes);
  return report;
}

inline plan_lazy_slot_preparation_report prepare_plan_outside_slot(
    outside_context_key_workspace& workspace, std::size_t pattern_count,
    std::size_t maximum_context_key_width, std::size_t maximum_arity,
    std::size_t workspace_peak_acceptance_limit_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  using namespace lazy_key_grouping_detail;
  auto const key_width =
      std::max<std::size_t>(maximum_context_key_width, nuc_state_count);
  auto const word_count = checked_packed_key_count_multiply(
      pattern_count, key_width, "lazy outside prepared packed words");
  plan_lazy_slot_preparation_report report;
  auto component_limit = [&](std::size_t before, std::size_t component_before) {
    auto const other = before - component_before;
    if (other > workspace_peak_acceptance_limit_bytes) {
      throw packed_key_grouping_budget_error(
          before, workspace_peak_acceptance_limit_bytes);
    }
    return workspace_peak_acceptance_limit_bytes - other;
  };
  auto before = workspace.resident_bytes();
  auto const word_before =
      packed_key_word_buffer_resident_bytes(workspace.packed_words);
  packed_key_word_buffer_preparation_report word_report;
  try {
    word_report =
        prepare_packed_key_word_buffer(word_count, workspace.packed_words,
                                       component_limit(before, word_before));
  } catch (packed_key_grouping_budget_error const& error) {
    throw_rebased_plan_lazy_preparation_budget_error(
        error, before - word_before, workspace_peak_acceptance_limit_bytes,
        "lazy outside slot rejected word preparation");
  }
  report.peak_resident_bytes = std::max(
      report.peak_resident_bytes,
      plan_lazy_chart_capacity_add(
          before,
          word_report.observed_prepublication_peak_capacity_resident_bytes -
              word_report.previous_capacity_resident_bytes,
          "lazy outside slot word preparation peak"));
  auto const component_before = packed_key_grouping_owned_resident_bytes(
      workspace.grouping_workspace, workspace.grouping);
  before = workspace.resident_bytes();
  packed_key_grouping_preparation_report grouping_report;
  try {
    grouping_report = prepare_packed_key_grouping_storage(
        pattern_count, workspace.grouping_workspace, workspace.grouping,
        component_limit(before, component_before));
  } catch (packed_key_grouping_budget_error const& error) {
    throw_rebased_plan_lazy_preparation_budget_error(
        error, before - component_before, workspace_peak_acceptance_limit_bytes,
        "lazy outside slot rejected grouping preparation");
  }
  report.peak_resident_bytes = std::max(
      report.peak_resident_bytes,
      plan_lazy_chart_capacity_add(
          before,
          grouping_report
                  .observed_prepublication_peak_owned_capacity_resident_bytes -
              component_before,
          "lazy outside slot grouping preparation peak"));
  auto record_vector = [&](auto& values, std::size_t count) {
    auto const vector_before =
        plan_lazy_chart_capacity_add(sizeof(values),
                                     plan_lazy_chart_vector_capacity_bytes(
                                         values, "lazy outside slot vector"),
                                     "lazy outside slot vector");
    auto const workspace_before = workspace.resident_bytes();
    packed_key_word_buffer_preparation_report vector_report;
    try {
      vector_report = prepare_plan_lazy_vector_storage(
          count, values, component_limit(workspace_before, vector_before));
    } catch (packed_key_grouping_budget_error const& error) {
      throw_rebased_plan_lazy_preparation_budget_error(
          error, workspace_before - vector_before,
          workspace_peak_acceptance_limit_bytes,
          "lazy outside slot rejected vector preparation");
    }
    report.peak_resident_bytes = std::max(
        report.peak_resident_bytes,
        plan_lazy_chart_capacity_add(
            workspace_before,
            vector_report.observed_prepublication_peak_capacity_resident_bytes -
                vector_report.previous_capacity_resident_bytes,
            "lazy outside slot vector preparation peak"));
  };
  record_vector(workspace.outside_by_pattern, pattern_count);
  record_vector(workspace.recurrence_scratch.result, maximum_arity);
  record_vector(workspace.recurrence_scratch.child_best, maximum_arity);
  auto const prefix_size = checked_packed_key_count_add(
      maximum_arity, 1, "lazy outside prepared recurrence width");
  record_vector(workspace.recurrence_scratch.prefix, prefix_size);
  record_vector(workspace.recurrence_scratch.suffix, prefix_size);
  report.stable_resident_bytes = workspace.resident_bytes();
  report.peak_resident_bytes =
      std::max(report.peak_resident_bytes, report.stable_resident_bytes);
  return report;
}

inline std::size_t plan_inside_clade_row_key_width(
    chart_execution_plan const& plan, clade_id clade) {
  std::size_t width = 0;
  for (auto production : plan.productions_for_parent(clade)) {
    width = lazy_key_grouping_detail::checked_packed_key_count_add(
        width, plan.children(production).size(),
        "lazy inside clade row-key width");
  }
  return width;
}

inline std::pair<std::size_t, std::size_t> plan_outside_clade_shape(
    chart_execution_plan const& plan, clade_id clade) {
  std::size_t context_width = nuc_state_count;
  std::size_t maximum_arity = 0;
  for (auto const occurrence : plan.child_occurrences_for_clade(clade)) {
    auto const arity = plan.children(occurrence.production).size();
    maximum_arity = std::max(maximum_arity, arity);
    context_width = std::max(
        context_width, lazy_key_grouping_detail::checked_packed_key_count_add(
                           arity, 1, "lazy outside clade context-key width"));
  }
  return {context_width, maximum_arity};
}

inline plan_lazy_slot_preparation_report prepare_plan_inside_clade_output(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    clade_id clade, lazy_chart_options const& options,
    std::size_t chart_peak_acceptance_limit_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  auto const pattern_count = chart.pattern_count;
  auto const leaf = plan.clade(clade).is_leaf();
  auto const row_count =
      leaf ? std::min<std::size_t>(pattern_count, nuc_state_count)
           : pattern_count;
  plan_lazy_slot_preparation_report report;
  auto prepare_vector = [&](auto& values, std::size_t count) {
    auto const component_before =
        plan_lazy_chart_capacity_add(sizeof(values),
                                     plan_lazy_chart_vector_capacity_bytes(
                                         values, "lazy inside output vector"),
                                     "lazy inside output vector");
    auto const chart_before =
        lazy_multisite_chart_capacity_resident_bytes(chart);
    auto const other = chart_before - component_before;
    if (other > chart_peak_acceptance_limit_bytes) {
      throw lazy_key_grouping_detail::packed_key_grouping_budget_error(
          chart_before, chart_peak_acceptance_limit_bytes);
    }
    lazy_key_grouping_detail::packed_key_word_buffer_preparation_report
        vector_report;
    try {
      vector_report = prepare_plan_lazy_vector_storage(
          count, values, chart_peak_acceptance_limit_bytes - other);
    } catch (lazy_key_grouping_detail::packed_key_grouping_budget_error const&
                 error) {
      throw_rebased_plan_lazy_preparation_budget_error(
          error, other, chart_peak_acceptance_limit_bytes,
          "lazy inside rejected output preparation");
    }
    report.peak_resident_bytes = std::max(
        report.peak_resident_bytes,
        plan_lazy_chart_capacity_add(
            chart_before,
            vector_report.observed_prepublication_peak_capacity_resident_bytes -
                vector_report.previous_capacity_resident_bytes,
            "lazy inside output preparation peak"));
  };
  prepare_vector(chart.inside_rows_by_clade[clade], row_count);
  prepare_vector(chart.class_weight_by_clade[clade], row_count);
  auto const retain_map = !leaf || options.retain_all_inside_class_maps ||
                          clade == plan.root_clade();
  if (retain_map) {
    auto& class_map = chart.class_index_by_pattern_by_clade[clade];
    auto& structural_map =
        chart.structural_class_index_by_pattern_by_clade[clade];
    if (!class_map) class_map.emplace();
    if (!structural_map) structural_map.emplace();
    prepare_vector(*class_map, pattern_count);
    prepare_vector(*structural_map, pattern_count);
  }
  report.stable_resident_bytes =
      lazy_multisite_chart_capacity_resident_bytes(chart);
  report.peak_resident_bytes =
      std::max(report.peak_resident_bytes, report.stable_resident_bytes);
  return report;
}

inline plan_lazy_slot_preparation_report prepare_plan_outside_clade_output(
    lazy_multisite_chart& chart, clade_id clade,
    std::size_t chart_peak_acceptance_limit_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  auto const pattern_count = chart.pattern_count;
  plan_lazy_slot_preparation_report report;
  auto prepare_vector = [&](auto& values) {
    auto const component_before =
        plan_lazy_chart_capacity_add(sizeof(values),
                                     plan_lazy_chart_vector_capacity_bytes(
                                         values, "lazy outside output vector"),
                                     "lazy outside output vector");
    auto const chart_before =
        lazy_multisite_chart_capacity_resident_bytes(chart);
    auto const other = chart_before - component_before;
    if (other > chart_peak_acceptance_limit_bytes) {
      throw lazy_key_grouping_detail::packed_key_grouping_budget_error(
          chart_before, chart_peak_acceptance_limit_bytes);
    }
    lazy_key_grouping_detail::packed_key_word_buffer_preparation_report
        vector_report;
    try {
      vector_report = prepare_plan_lazy_vector_storage(
          pattern_count, values, chart_peak_acceptance_limit_bytes - other);
    } catch (lazy_key_grouping_detail::packed_key_grouping_budget_error const&
                 error) {
      throw_rebased_plan_lazy_preparation_budget_error(
          error, other, chart_peak_acceptance_limit_bytes,
          "lazy outside rejected output preparation");
    }
    report.peak_resident_bytes = std::max(
        report.peak_resident_bytes,
        plan_lazy_chart_capacity_add(
            chart_before,
            vector_report.observed_prepublication_peak_capacity_resident_bytes -
                vector_report.previous_capacity_resident_bytes,
            "lazy outside output preparation peak"));
  };
  prepare_vector(chart.outside_rows_by_clade[clade]);
  prepare_vector(chart.outside_class_weight_by_clade[clade]);
  auto& class_map = chart.outside_class_index_by_pattern_by_clade[clade];
  if (!class_map) class_map.emplace();
  prepare_vector(*class_map);
  report.stable_resident_bytes =
      lazy_multisite_chart_capacity_resident_bytes(chart);
  report.peak_resident_bytes =
      std::max(report.peak_resident_bytes, report.stable_resident_bytes);
  return report;
}

inline void record_plan_lazy_chart_preflight(
    plan_lazy_chart_memory_report* report, std::size_t bytes) noexcept {
  if (report != nullptr) {
    report->preflight_peak_capacity_resident_bytes =
        std::max(report->preflight_peak_capacity_resident_bytes, bytes);
  }
}

inline void record_plan_lazy_chart_actual_peak(
    plan_lazy_chart_memory_report* report, std::size_t bytes) noexcept {
  if (report != nullptr) {
    report->actual_peak_capacity_resident_bytes =
        std::max(report->actual_peak_capacity_resident_bytes, bytes);
  }
}

[[noreturn]] inline void reject_plan_lazy_chart_already_observed(
    plan_lazy_chart_memory_options const& options,
    plan_lazy_chart_memory_report* report, plan_lazy_chart_memory_phase phase,
    std::size_t required) {
  record_plan_lazy_chart_preflight(report, required);
  record_plan_lazy_chart_actual_peak(report, required);
  if (report != nullptr) ++report->pre_submit_rejections;
  throw plan_lazy_chart_memory_budget_error(phase, required,
                                            options.memory_budget_bytes);
}

[[noreturn]] inline void reject_plan_lazy_chart_projected(
    plan_lazy_chart_memory_options const& options,
    plan_lazy_chart_memory_report* report, plan_lazy_chart_memory_phase phase,
    std::size_t required, std::size_t observed_live) {
  record_plan_lazy_chart_preflight(report, required);
  record_plan_lazy_chart_actual_peak(report, observed_live);
  if (report != nullptr) ++report->pre_submit_rejections;
  throw plan_lazy_chart_memory_budget_error(phase, required,
                                            options.memory_budget_bytes);
}

inline void require_plan_lazy_chart_memory_budget(
    plan_lazy_chart_memory_options const& options,
    plan_lazy_chart_memory_report* report, plan_lazy_chart_memory_phase phase,
    std::size_t required) {
  record_plan_lazy_chart_preflight(report, required);
  if (options.memory_budget_bytes != 0 &&
      required > options.memory_budget_bytes) {
    if (report != nullptr) ++report->pre_submit_rejections;
    throw plan_lazy_chart_memory_budget_error(phase, required,
                                              options.memory_budget_bytes);
  }
}

inline std::size_t logical_plan_inside_clade_output_bytes(
    chart_execution_plan const& plan, clade_id clade, std::size_t pattern_count,
    lazy_chart_options const& options) {
  auto const leaf = plan.clade(clade).is_leaf();
  auto const row_count =
      leaf ? std::min<std::size_t>(pattern_count, nuc_state_count)
           : pattern_count;
  auto total = plan_lazy_chart_capacity_multiply(
      row_count, sizeof(row_type) + sizeof(std::uint32_t),
      "lazy inside admitted output rows");
  auto const retain_map = !leaf || options.retain_all_inside_class_maps ||
                          clade == plan.root_clade();
  if (retain_map) {
    total =
        plan_lazy_chart_capacity_add(total,
                                     plan_lazy_chart_capacity_multiply(
                                         pattern_count, 2 * sizeof(std::size_t),
                                         "lazy inside admitted output maps"),
                                     "lazy inside admitted output");
  }
  return total;
}

inline chart_indexed_range_options plan_lazy_chart_clade_range_options();
inline void reserve_plan_lazy_chart_run_summaries(
    std::vector<chart_scheduler_run_summary>* runs, std::size_t count,
    std::string_view context);
inline void clear_plan_inside_clade_output(lazy_multisite_chart& chart,
                                           clade_id clade) noexcept;
inline void clear_plan_outside_clade_output(lazy_multisite_chart& chart,
                                            clade_id clade) noexcept;

inline std::size_t logical_plan_outside_clade_output_bytes(
    std::size_t pattern_count) {
  return plan_lazy_chart_capacity_multiply(
      pattern_count,
      sizeof(row_type) + sizeof(std::uint32_t) + sizeof(std::size_t),
      "lazy outside admitted output");
}

inline std::size_t plan_lazy_chart_scheduler_resident_bytes(
    chart_scheduler const& scheduler) {
  return plan_lazy_chart_capacity_add(
      estimate_chart_scheduler_implementation_resident_bytes(),
      estimate_chart_scheduler_pool_owning_heap_bytes(
          scheduler.worker_resolution().resolved_workers),
      "lazy chart scheduler resident capacity");
}

inline std::size_t plan_lazy_inside_coordinator_capacity_bytes(
    std::vector<plan_inside_clade_work_stats> const& stats,
    std::vector<std::exception_ptr> const& errors,
    std::optional<map_dependency_counts> const& dependencies,
    std::vector<chart_scheduler_run_summary> const* runs) {
  std::size_t total = 0;
  total = plan_lazy_chart_capacity_add(
      total, plan_lazy_chart_vector_capacity_bytes(stats, "lazy inside stats"),
      "lazy inside coordinator arrays");
  total = plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_vector_capacity_bytes(errors, "lazy inside errors"),
      "lazy inside coordinator arrays");
  if (dependencies) {
    total = plan_lazy_chart_capacity_add(
        total,
        plan_lazy_chart_vector_capacity_bytes(dependencies->inside,
                                              "lazy inside dependencies"),
        "lazy inside coordinator arrays");
    total = plan_lazy_chart_capacity_add(
        total,
        plan_lazy_chart_vector_capacity_bytes(dependencies->structural,
                                              "lazy structural dependencies"),
        "lazy inside coordinator arrays");
  }
  if (runs != nullptr) {
    total =
        plan_lazy_chart_capacity_add(total,
                                     plan_lazy_chart_vector_capacity_bytes(
                                         *runs, "lazy inside run summaries"),
                                     "lazy inside coordinator arrays");
  }
  return total;
}

inline std::size_t logical_plan_inside_coordinator_bytes(
    std::size_t clade_count, bool sparse,
    std::vector<chart_scheduler_run_summary> const* runs) {
  std::size_t total = plan_lazy_chart_capacity_multiply(
      clade_count,
      sizeof(plan_inside_clade_work_stats) + sizeof(std::exception_ptr),
      "lazy inside coordinator arrays");
  if (sparse) {
    total = plan_lazy_chart_capacity_add(
        total,
        plan_lazy_chart_capacity_multiply(clade_count, 2 * sizeof(std::size_t),
                                          "lazy inside dependency arrays"),
        "lazy inside coordinator arrays");
  }
  if (runs != nullptr) {
    auto const target = plan_lazy_chart_capacity_add(
        runs->size(), clade_count, "lazy inside run summary count");
    auto const new_bytes = plan_lazy_chart_capacity_multiply(
        target, sizeof(chart_scheduler_run_summary),
        "lazy inside run summaries");
    auto const old_bytes = plan_lazy_chart_vector_capacity_bytes(
        *runs, "lazy inside old run summaries");
    total = plan_lazy_chart_capacity_add(
        total,
        runs->capacity() < target
            ? plan_lazy_chart_capacity_add(
                  old_bytes, new_bytes,
                  "lazy inside run-summary preparation peak")
            : old_bytes,
        "lazy inside coordinator arrays");
  }
  return total;
}

inline lazy_multisite_chart build_plan_lazy_inside_chart_scheduled_finite(
    lazy_multisite_chart chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, lazy_chart_options const& options,
    chart_scheduler& scheduler, std::span<clade_id const> level_order,
    std::span<std::size_t const> level_offsets,
    std::vector<chart_scheduler_run_summary>* level_runs,
    plan_lazy_chart_scheduler_test_hooks const* test_hooks,
    plan_lazy_chart_scheduler_workspace& scheduler_workspace,
    plan_lazy_chart_memory_options const& memory_options,
    plan_lazy_chart_memory_report* memory_report) {
  auto const clade_count = plan.clades().size();
  auto const level_count = level_offsets.size() - 1;
  auto const worker_count = scheduler.worker_resolution().resolved_workers;
  if (worker_count == 0) {
    throw std::logic_error("lazy inside chart: scheduler has no worker slots");
  }
  if (memory_report != nullptr) {
    memory_report->memory_budget_bytes = memory_options.memory_budget_bytes;
  }
  auto const retained_resident_bytes = plan_lazy_chart_capacity_add(
      memory_options.retained_resident_bytes,
      plan_lazy_chart_scheduler_resident_bytes(scheduler),
      "lazy inside retained scheduler capacity");

  // Reuse prepared finite scratch when it can participate in this scheduler.
  // An oversized slot array cannot be used by the resolved worker topology and
  // is evicted before coordinator admission (the stale-W8/W1 case).
  if (scheduler_workspace.inside_by_slot.size() > worker_count) {
    scheduler_workspace.release_inside();
    if (memory_report != nullptr) ++memory_report->inside_workspace_evictions;
  }
  auto const sparse = !options.retain_all_inside_class_maps;
  auto coordinator_projection =
      logical_plan_inside_coordinator_bytes(clade_count, sparse, level_runs);
  auto project_coordinator = [&] {
    auto projection = plan_lazy_chart_capacity_add(
        retained_resident_bytes,
        lazy_multisite_chart_capacity_resident_bytes(chart),
        "lazy inside coordinator preflight");
    projection =
        plan_lazy_chart_capacity_add(projection, coordinator_projection,
                                     "lazy inside coordinator preflight");
    return plan_lazy_chart_capacity_add(
        projection, scheduler_workspace.inside_capacity_resident_bytes(),
        "lazy inside retained scratch preflight");
  };
  auto coordinator_preflight = project_coordinator();
  if (coordinator_preflight > memory_options.memory_budget_bytes &&
      scheduler_workspace.inside_by_slot.capacity() != 0) {
    scheduler_workspace.release_inside();
    if (memory_report != nullptr) ++memory_report->inside_workspace_evictions;
    coordinator_preflight = project_coordinator();
  }
  require_plan_lazy_chart_memory_budget(memory_options, memory_report,
                                        plan_lazy_chart_memory_phase::inside,
                                        coordinator_preflight);

  auto const old_run_capacity_bytes =
      level_runs == nullptr ? std::size_t{0}
                            : plan_lazy_chart_vector_capacity_bytes(
                                  *level_runs, "lazy inside old run summaries");
  auto const run_capacity_grows =
      level_runs != nullptr &&
      level_runs->capacity() < level_runs->size() + clade_count;
  if (level_runs != nullptr) {
    reserve_plan_lazy_chart_run_summaries(level_runs, clade_count,
                                          "lazy inside chart");
  }
  std::optional<map_dependency_counts> remaining_dependencies;
  if (sparse) remaining_dependencies = count_map_dependencies(plan);
  std::vector<plan_inside_clade_work_stats> stats_by_clade(clade_count);
  std::vector<std::exception_ptr> errors_by_clade(clade_count);
  auto const coordinator_bytes = plan_lazy_inside_coordinator_capacity_bytes(
      stats_by_clade, errors_by_clade, remaining_dependencies, level_runs);
  if (memory_report != nullptr) {
    memory_report->inside_coordinator_capacity_resident_bytes =
        coordinator_bytes;
  }
  auto actual = plan_lazy_chart_capacity_add(
      retained_resident_bytes,
      lazy_multisite_chart_capacity_resident_bytes(chart),
      "lazy inside coordinator capacity");
  actual = plan_lazy_chart_capacity_add(actual, coordinator_bytes,
                                        "lazy inside coordinator capacity");
  actual = plan_lazy_chart_capacity_add(
      actual, scheduler_workspace.inside_capacity_resident_bytes(),
      "lazy inside retained scratch capacity");
  if (run_capacity_grows) {
    actual = plan_lazy_chart_capacity_add(
        actual, old_run_capacity_bytes,
        "lazy inside coordinator preparation peak");
  }
  record_plan_lazy_chart_actual_peak(memory_report, actual);
  require_plan_lazy_chart_memory_budget(memory_options, memory_report,
                                        plan_lazy_chart_memory_phase::inside,
                                        actual);

  for (std::size_t level = 0; level < level_count; ++level) {
    auto const level_begin = level_offsets[level];
    auto const level_end = level_offsets[level + 1];
    auto const level_item_count = level_end - level_begin;
    if (level_item_count == 0) continue;
    bool level_memory_limited = false;

    for (std::size_t wave_begin = 0; wave_begin < level_item_count;) {
      auto const remaining = level_item_count - wave_begin;
      std::size_t admitted_items = 0;
      std::size_t admitted_slots = 0;
      std::size_t admitted_row_width = 0;
      std::size_t admitted_projection = 0;
      bool admitted_reuses_slots = false;
      auto project_cold_wave = [&](std::size_t candidate,
                                   std::size_t maximum_row_width,
                                   std::size_t output_bytes) {
        auto const slot_count = std::min(worker_count, candidate);
        auto projection = plan_lazy_chart_capacity_add(
            retained_resident_bytes,
            lazy_multisite_chart_capacity_resident_bytes(chart),
            "lazy inside wave preflight");
        projection = plan_lazy_chart_capacity_add(projection, coordinator_bytes,
                                                  "lazy inside wave preflight");
        projection = plan_lazy_chart_capacity_add(projection, output_bytes,
                                                  "lazy inside wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, logical_plan_output_preparation_extra_bytes(),
            "lazy inside output preparation high-water");
        projection = plan_lazy_chart_capacity_add(
            projection,
            plan_lazy_chart_capacity_multiply(
                slot_count,
                logical_plan_inside_slot_resident_bytes(chart.pattern_count,
                                                        maximum_row_width),
                "lazy inside admitted slots"),
            "lazy inside wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection,
            logical_plan_inside_slot_preparation_extra_bytes(
                chart.pattern_count),
            "lazy inside slot preparation high-water");
        projection = plan_lazy_chart_capacity_add(
            projection, sizeof(plan_lazy_chart_scheduler_workspace),
            "lazy inside scheduler workspace object");
        return plan_lazy_chart_capacity_add(
            projection,
            estimate_chart_scheduler_operation_peak_bytes(
                scheduler.plan_indexed_ranges(
                    candidate, plan_lazy_chart_clade_range_options())),
            "lazy inside scheduler operation");
      };
      auto project_reused_wave = [&](std::size_t candidate,
                                     std::size_t output_bytes) {
        auto projection = plan_lazy_chart_capacity_add(
            retained_resident_bytes,
            lazy_multisite_chart_capacity_resident_bytes(chart),
            "lazy inside reused wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, coordinator_bytes, "lazy inside reused wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, output_bytes, "lazy inside reused wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, logical_plan_output_preparation_extra_bytes(),
            "lazy inside output preparation high-water");
        projection = plan_lazy_chart_capacity_add(
            projection, scheduler_workspace.inside_capacity_resident_bytes(),
            "lazy inside reused slots");
        return plan_lazy_chart_capacity_add(
            projection,
            estimate_chart_scheduler_operation_peak_bytes(
                scheduler.plan_indexed_ranges(
                    candidate, plan_lazy_chart_clade_range_options())),
            "lazy inside scheduler operation");
      };
      for (std::size_t candidate = remaining; candidate != 0; --candidate) {
        std::size_t output_bytes = 0;
        std::size_t maximum_row_width = 0;
        for (std::size_t local = 0; local < candidate; ++local) {
          auto const clade = level_order[level_begin + wave_begin + local];
          output_bytes = plan_lazy_chart_capacity_add(
              output_bytes,
              logical_plan_inside_clade_output_bytes(
                  plan, clade, chart.pattern_count, options),
              "lazy inside wave output");
          if (!plan.clade(clade).is_leaf()) {
            maximum_row_width =
                std::max(maximum_row_width,
                         plan_inside_clade_row_key_width(plan, clade));
          }
        }
        auto const slot_count = std::min(worker_count, candidate);
        auto const reusable = scheduler_workspace.can_reuse_inside_slots(
            slot_count, chart.pattern_count, maximum_row_width);
        if (reusable) {
          auto const projection = project_reused_wave(candidate, output_bytes);
          if (projection <= memory_options.memory_budget_bytes) {
            admitted_items = candidate;
            admitted_slots = slot_count;
            admitted_row_width = maximum_row_width;
            admitted_projection = projection;
            admitted_reuses_slots = true;
            break;
          }
        }
        auto const cold_projection =
            project_cold_wave(candidate, maximum_row_width, output_bytes);
        if (cold_projection <= memory_options.memory_budget_bytes) {
          admitted_items = candidate;
          admitted_slots = slot_count;
          admitted_row_width = maximum_row_width;
          admitted_projection = cold_projection;
          break;
        }
      }
      if (admitted_items == 0) {
        auto const clade = level_order[level_begin + wave_begin];
        auto const required = project_cold_wave(
            1,
            plan.clade(clade).is_leaf()
                ? std::size_t{0}
                : plan_inside_clade_row_key_width(plan, clade),
            logical_plan_inside_clade_output_bytes(
                plan, clade, chart.pattern_count, options));
        require_plan_lazy_chart_memory_budget(
            memory_options, memory_report, plan_lazy_chart_memory_phase::inside,
            required);
        throw std::logic_error("lazy inside chart: unreachable admission");
      }
      if (admitted_items < remaining) level_memory_limited = true;

      // Prepare output and every worker-owned vector before scheduler entry.
      if (!admitted_reuses_slots &&
          scheduler_workspace.inside_by_slot.capacity() != 0) {
        scheduler_workspace.release_inside();
        if (memory_report != nullptr) {
          ++memory_report->inside_workspace_evictions;
        }
      } else if (admitted_reuses_slots && memory_report != nullptr) {
        ++memory_report->inside_reused_slot_waves;
      }
      require_plan_lazy_chart_memory_budget(
          memory_options, memory_report, plan_lazy_chart_memory_phase::inside,
          admitted_projection);
      if (!admitted_reuses_slots) {
        scheduler_workspace.hard_bound_inside_slots(admitted_slots);
      }
      for (std::size_t slot = 0; slot < admitted_slots; ++slot) {
        auto& workspace = scheduler_workspace.inside_by_slot[slot];
        auto workspace_live = plan_lazy_chart_capacity_add(
            retained_resident_bytes,
            lazy_multisite_chart_capacity_resident_bytes(chart),
            "lazy inside slot preparation");
        workspace_live = plan_lazy_chart_capacity_add(
            workspace_live, coordinator_bytes, "lazy inside slot preparation");
        workspace_live = plan_lazy_chart_capacity_add(
            workspace_live,
            scheduler_workspace.inside_capacity_resident_bytes(),
            "lazy inside slot preparation");
        auto const workspace_resident = workspace.resident_bytes();
        if (workspace_live < workspace_resident) {
          throw std::logic_error(
              "lazy inside slot is absent from scheduler live capacity");
        }
        auto const outside_workspace = workspace_live - workspace_resident;
        if (outside_workspace > memory_options.memory_budget_bytes) {
          reject_plan_lazy_chart_already_observed(
              memory_options, memory_report,
              plan_lazy_chart_memory_phase::inside, workspace_live);
        }
        plan_lazy_slot_preparation_report preparation;
        try {
          preparation = prepare_plan_inside_slot(
              workspace, chart.pattern_count, admitted_row_width,
              memory_options.memory_budget_bytes - outside_workspace);
        } catch (
            lazy_key_grouping_detail::packed_key_grouping_budget_error const&
                error) {
          auto const required = plan_lazy_chart_capacity_add(
              outside_workspace, error.required_bytes(),
              "lazy inside rejected slot preparation");
          record_plan_lazy_chart_preflight(memory_report, required);
          record_plan_lazy_chart_actual_peak(
              memory_report,
              plan_lazy_rejected_preparation_actual_peak(
                  error, outside_workspace,
                  plan_lazy_chart_capacity_add(
                      outside_workspace, workspace.resident_bytes(),
                      "lazy inside rejected slot capacity"),
                  "lazy inside rejected slot actual peak"));
          if (memory_report != nullptr) ++memory_report->pre_submit_rejections;
          throw plan_lazy_chart_memory_budget_error(
              plan_lazy_chart_memory_phase::inside, required,
              memory_options.memory_budget_bytes);
        }
        auto preparation_live = plan_lazy_chart_capacity_add(
            outside_workspace, preparation.peak_resident_bytes,
            "lazy inside slot preparation");
        record_plan_lazy_chart_actual_peak(memory_report, preparation_live);
        if (preparation_live > memory_options.memory_budget_bytes) {
          reject_plan_lazy_chart_already_observed(
              memory_options, memory_report,
              plan_lazy_chart_memory_phase::inside, preparation_live);
        }
      }
      auto const output_outside_chart = plan_lazy_chart_capacity_add(
          plan_lazy_chart_capacity_add(retained_resident_bytes,
                                       coordinator_bytes,
                                       "lazy inside output preparation"),
          scheduler_workspace.inside_capacity_resident_bytes(),
          "lazy inside output preparation");
      auto const output_live = plan_lazy_chart_capacity_add(
          output_outside_chart,
          lazy_multisite_chart_capacity_resident_bytes(chart),
          "lazy inside output live capacity");
      if (output_live > memory_options.memory_budget_bytes) {
        reject_plan_lazy_chart_already_observed(
            memory_options, memory_report, plan_lazy_chart_memory_phase::inside,
            output_live);
      }
      auto const chart_peak_limit =
          memory_options.memory_budget_bytes - output_outside_chart;
      for (std::size_t local = 0; local < admitted_items; ++local) {
        try {
          auto const preparation = prepare_plan_inside_clade_output(
              chart, plan, level_order[level_begin + wave_begin + local],
              options, chart_peak_limit);
          record_plan_lazy_chart_actual_peak(
              memory_report,
              plan_lazy_chart_capacity_add(output_outside_chart,
                                           preparation.peak_resident_bytes,
                                           "lazy inside output preparation"));
        } catch (
            lazy_key_grouping_detail::packed_key_grouping_budget_error const&
                error) {
          auto const required = plan_lazy_chart_capacity_add(
              output_outside_chart, error.required_bytes(),
              "lazy inside rejected output preparation");
          record_plan_lazy_chart_preflight(memory_report, required);
          record_plan_lazy_chart_actual_peak(
              memory_report,
              plan_lazy_rejected_preparation_actual_peak(
                  error, output_outside_chart,
                  plan_lazy_chart_capacity_add(
                      output_outside_chart,
                      lazy_multisite_chart_capacity_resident_bytes(chart),
                      "lazy inside rejected output capacity"),
                  "lazy inside rejected output actual peak"));
          if (memory_report != nullptr) ++memory_report->pre_submit_rejections;
          throw plan_lazy_chart_memory_budget_error(
              plan_lazy_chart_memory_phase::inside, required,
              memory_options.memory_budget_bytes);
        }
      }
      actual = plan_lazy_chart_capacity_add(
          retained_resident_bytes,
          lazy_multisite_chart_capacity_resident_bytes(chart),
          "lazy inside prepared wave");
      actual = plan_lazy_chart_capacity_add(actual, coordinator_bytes,
                                            "lazy inside prepared wave");
      actual = plan_lazy_chart_capacity_add(
          actual, scheduler_workspace.inside_capacity_resident_bytes(),
          "lazy inside prepared wave");
      record_plan_lazy_chart_actual_peak(memory_report, actual);
      auto const scheduler_projection = plan_lazy_chart_capacity_add(
          actual,
          estimate_chart_scheduler_operation_peak_bytes(
              scheduler.plan_indexed_ranges(
                  admitted_items, plan_lazy_chart_clade_range_options())),
          "lazy inside scheduler operation");
      record_plan_lazy_chart_preflight(memory_report, scheduler_projection);
      if (scheduler_projection > memory_options.memory_budget_bytes) {
        reject_plan_lazy_chart_projected(memory_options, memory_report,
                                         plan_lazy_chart_memory_phase::inside,
                                         scheduler_projection, actual);
      }
      // The operation envelope becomes part of the observed high-water only
      // after admission guarantees that scheduler entry will be attempted.
      record_plan_lazy_chart_actual_peak(memory_report, scheduler_projection);

      chart_scheduler_run_summary failed_run;
      chart_scheduler_run_summary run;
      if (memory_report != nullptr) {
        ++memory_report->inside_admission_waves;
        memory_report->inside_max_admitted_slots =
            std::max(memory_report->inside_max_admitted_slots, admitted_slots);
        ++memory_report->scheduler_submissions;
      }
      try {
        run = scheduler.for_each_indexed_range(
            admitted_items, plan_lazy_chart_clade_range_options(),
            [&](chart_indexed_range const& range, std::size_t stable_slot,
                chart_scheduler_cancellation_token const&) {
              if (stable_slot >= scheduler_workspace.inside_by_slot.size()) {
                throw std::logic_error(
                    "lazy inside chart: scheduler slot out of range");
              }
              auto& workspace = scheduler_workspace.inside_by_slot[stable_slot];
              for (std::size_t item = range.begin; item < range.end; ++item) {
                auto const clade = level_order[level_begin + wave_begin + item];
                try {
                  if (test_hooks != nullptr &&
                      test_hooks->before_inside_clade) {
                    test_hooks->before_inside_clade(clade, level, stable_slot);
                  }
                  if (plan.clade(clade).is_leaf()) {
                    assign_plan_leaf_classes(chart, plan, patterns, clade,
                                             options);
                  } else {
                    auto keys = collect_plan_parent_keys(chart, plan, patterns,
                                                         clade, workspace);
                    assign_plan_internal_classes_task_local(
                        chart, plan, patterns, clade, keys,
                        stats_by_clade[clade]);
                  }
                  if (test_hooks != nullptr &&
                      test_hooks->observe_completed_inside_clade) {
                    test_hooks->observe_completed_inside_clade(
                        chart, clade, level, stable_slot);
                  }
                  if (test_hooks != nullptr && test_hooks->after_inside_clade) {
                    test_hooks->after_inside_clade(clade, level, stable_slot);
                  }
                } catch (...) {
                  errors_by_clade[clade] = std::current_exception();
                  break;
                }
              }
            },
            &failed_run);
        if (level_runs != nullptr) level_runs->push_back(run);
      } catch (...) {
        for (std::size_t item = 0; item < level_item_count; ++item) {
          clear_plan_inside_clade_output(chart,
                                         level_order[level_begin + item]);
        }
        if (test_hooks != nullptr &&
            test_hooks->observe_inside_level_failure_cleanup) {
          test_hooks->observe_inside_level_failure_cleanup(chart, level);
        }
        if (level_runs != nullptr && failed_run.failed) {
          level_runs->push_back(failed_run);
        }
        throw;
      }

      std::exception_ptr selected_error;
      for (std::size_t item = 0; item < level_item_count; ++item) {
        auto const clade = level_order[level_begin + item];
        if (errors_by_clade[clade]) {
          selected_error = errors_by_clade[clade];
          break;
        }
      }
      if (selected_error) {
        for (std::size_t item = 0; item < level_item_count; ++item) {
          clear_plan_inside_clade_output(chart,
                                         level_order[level_begin + item]);
        }
        if (test_hooks != nullptr &&
            test_hooks->observe_inside_level_failure_cleanup) {
          test_hooks->observe_inside_level_failure_cleanup(chart, level);
        }
        std::rethrow_exception(selected_error);
      }
      wave_begin += admitted_items;
    }

    if (memory_report != nullptr && level_memory_limited) {
      ++memory_report->inside_memory_limited_levels;
    }
    if (test_hooks != nullptr && test_hooks->observe_inside_level_join) {
      test_hooks->observe_inside_level_join(chart, level);
    }
    for (std::size_t item = 0; item < level_item_count; ++item) {
      auto const clade = level_order[level_begin + item];
      if (remaining_dependencies && !plan.clade(clade).is_leaf()) {
        consume_plan_parent_map_dependencies(chart, plan, clade,
                                             *remaining_dependencies);
      }
      add_plan_inside_clade_work_stats(chart, stats_by_clade[clade]);
    }
    if (test_hooks != nullptr && test_hooks->observe_inside_level_reclamation) {
      test_hooks->observe_inside_level_reclamation(chart, level);
    }
  }

  finalize_inside_counters(chart);
  maybe_discard_nonroot_maps(chart, plan, options);
  return chart;
}

inline std::size_t logical_plan_outside_coordinator_bytes(
    std::size_t clade_count, std::size_t level_count,
    std::vector<chart_scheduler_run_summary> const* runs) {
  auto total = plan_lazy_chart_capacity_multiply(
      clade_count,
      sizeof(clade_id) + sizeof(plan_outside_clade_work_stats) +
          sizeof(std::exception_ptr),
      "lazy outside coordinator arrays");
  total = plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(level_count + 1, sizeof(std::size_t),
                                        "lazy outside level offsets"),
      "lazy outside coordinator arrays");
  if (runs != nullptr) {
    auto const target = plan_lazy_chart_capacity_add(
        runs->size(), clade_count, "lazy outside run summary count");
    auto const new_bytes = plan_lazy_chart_capacity_multiply(
        target, sizeof(chart_scheduler_run_summary),
        "lazy outside run summaries");
    auto const old_bytes = plan_lazy_chart_vector_capacity_bytes(
        *runs, "lazy outside old run summaries");
    total = plan_lazy_chart_capacity_add(
        total,
        runs->capacity() < target
            ? plan_lazy_chart_capacity_add(
                  old_bytes, new_bytes,
                  "lazy outside run-summary preparation peak")
            : old_bytes,
        "lazy outside coordinator arrays");
  }
  return total;
}

inline std::size_t plan_lazy_outside_coordinator_capacity_bytes(
    std::vector<clade_id> const& order, std::vector<std::size_t> const& offsets,
    std::vector<plan_outside_clade_work_stats> const& stats,
    std::vector<std::exception_ptr> const& errors,
    std::vector<chart_scheduler_run_summary> const* runs) {
  std::size_t total = 0;
  auto add = [&](auto const& values, std::string_view context) {
    total = plan_lazy_chart_capacity_add(
        total, plan_lazy_chart_vector_capacity_bytes(values, context),
        "lazy outside coordinator arrays");
  };
  add(order, "lazy outside level order");
  add(offsets, "lazy outside level offsets");
  add(stats, "lazy outside stats");
  add(errors, "lazy outside errors");
  if (runs != nullptr) add(*runs, "lazy outside run summaries");
  return total;
}

inline void build_plan_lazy_outside_chart_scheduled_finite(
    lazy_multisite_chart& chart, chart_execution_plan const& plan,
    site_pattern_set const& patterns, chart_scheduler& scheduler,
    std::span<clade_id const> level_order,
    std::span<std::size_t const> level_offsets,
    std::vector<chart_scheduler_run_summary>* level_runs,
    plan_lazy_chart_scheduler_test_hooks const* test_hooks,
    plan_lazy_chart_scheduler_workspace& scheduler_workspace,
    plan_lazy_chart_memory_options const& memory_options,
    plan_lazy_chart_memory_report* memory_report) {
  auto const clade_count = plan.clades().size();
  auto const level_count = level_offsets.size() - 1;
  auto const worker_count = scheduler.worker_resolution().resolved_workers;
  if (worker_count == 0) {
    throw std::logic_error("lazy outside chart: scheduler has no worker slots");
  }
  if (memory_report != nullptr) {
    memory_report->memory_budget_bytes = memory_options.memory_budget_bytes;
  }
  auto const retained_resident_bytes = plan_lazy_chart_capacity_add(
      memory_options.retained_resident_bytes,
      plan_lazy_chart_scheduler_resident_bytes(scheduler),
      "lazy outside retained scheduler capacity");

  if (scheduler_workspace.outside_by_slot.size() > worker_count) {
    scheduler_workspace.release_outside();
    if (memory_report != nullptr) ++memory_report->outside_workspace_evictions;
  }
  auto coordinator_projection = logical_plan_outside_coordinator_bytes(
      clade_count, level_count, level_runs);
  auto project_coordinator = [&] {
    auto projection = plan_lazy_chart_capacity_add(
        retained_resident_bytes,
        lazy_multisite_chart_capacity_resident_bytes(chart),
        "lazy outside coordinator preflight");
    projection =
        plan_lazy_chart_capacity_add(projection, coordinator_projection,
                                     "lazy outside coordinator preflight");
    return plan_lazy_chart_capacity_add(
        projection, scheduler_workspace.outside_capacity_resident_bytes(),
        "lazy outside retained scratch preflight");
  };
  auto coordinator_preflight = project_coordinator();
  if (coordinator_preflight > memory_options.memory_budget_bytes &&
      scheduler_workspace.outside_by_slot.capacity() != 0) {
    scheduler_workspace.release_outside();
    if (memory_report != nullptr) ++memory_report->outside_workspace_evictions;
    coordinator_preflight = project_coordinator();
  }
  require_plan_lazy_chart_memory_budget(memory_options, memory_report,
                                        plan_lazy_chart_memory_phase::outside,
                                        coordinator_preflight);

  auto const old_run_capacity_bytes =
      level_runs == nullptr
          ? std::size_t{0}
          : plan_lazy_chart_vector_capacity_bytes(
                *level_runs, "lazy outside old run summaries");
  auto const run_capacity_grows =
      level_runs != nullptr &&
      level_runs->capacity() < level_runs->size() + clade_count;
  if (level_runs != nullptr) {
    reserve_plan_lazy_chart_run_summaries(level_runs, clade_count,
                                          "lazy outside chart");
  }
  auto const root = plan.root_clade();
  std::vector<clade_id> nonroot_level_order;
  nonroot_level_order.reserve(clade_count - 1);
  std::vector<std::size_t> nonroot_level_offsets;
  nonroot_level_offsets.reserve(level_count + 1);
  nonroot_level_offsets.push_back(0);
  for (std::size_t level = 0; level < level_count; ++level) {
    for (std::size_t item = level_offsets[level];
         item < level_offsets[level + 1]; ++item) {
      auto const clade = level_order[item];
      if (clade != root) nonroot_level_order.push_back(clade);
    }
    nonroot_level_offsets.push_back(nonroot_level_order.size());
  }
  std::vector<plan_outside_clade_work_stats> stats_by_clade(clade_count);
  std::vector<std::exception_ptr> errors_by_clade(clade_count);
  auto const coordinator_bytes = plan_lazy_outside_coordinator_capacity_bytes(
      nonroot_level_order, nonroot_level_offsets, stats_by_clade,
      errors_by_clade, level_runs);
  if (memory_report != nullptr) {
    memory_report->outside_coordinator_capacity_resident_bytes =
        coordinator_bytes;
  }
  auto actual = plan_lazy_chart_capacity_add(
      retained_resident_bytes,
      lazy_multisite_chart_capacity_resident_bytes(chart),
      "lazy outside coordinator capacity");
  actual = plan_lazy_chart_capacity_add(actual, coordinator_bytes,
                                        "lazy outside coordinator capacity");
  actual = plan_lazy_chart_capacity_add(
      actual, scheduler_workspace.outside_capacity_resident_bytes(),
      "lazy outside retained scratch capacity");
  if (run_capacity_grows) {
    actual = plan_lazy_chart_capacity_add(
        actual, old_run_capacity_bytes,
        "lazy outside coordinator preparation peak");
  }
  record_plan_lazy_chart_actual_peak(memory_report, actual);
  require_plan_lazy_chart_memory_budget(memory_options, memory_report,
                                        plan_lazy_chart_memory_phase::outside,
                                        actual);

  for (std::size_t level = 0; level < level_count; ++level) {
    auto const level_begin = nonroot_level_offsets[level];
    auto const level_end = nonroot_level_offsets[level + 1];
    auto const level_item_count = level_end - level_begin;
    if (level_item_count == 0) continue;
    bool level_memory_limited = false;

    for (std::size_t wave_begin = 0; wave_begin < level_item_count;) {
      auto const remaining = level_item_count - wave_begin;
      std::size_t admitted_items = 0;
      std::size_t admitted_slots = 0;
      std::size_t admitted_context_width = nuc_state_count;
      std::size_t admitted_arity = 0;
      std::size_t admitted_projection = 0;
      bool admitted_reuses_slots = false;
      auto project_cold_wave = [&](std::size_t candidate,
                                   std::size_t maximum_context_width,
                                   std::size_t maximum_arity) {
        auto const slot_count = std::min(worker_count, candidate);
        auto projection = plan_lazy_chart_capacity_add(
            retained_resident_bytes,
            lazy_multisite_chart_capacity_resident_bytes(chart),
            "lazy outside wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, coordinator_bytes, "lazy outside wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection,
            plan_lazy_chart_capacity_multiply(
                candidate,
                logical_plan_outside_clade_output_bytes(chart.pattern_count),
                "lazy outside wave outputs"),
            "lazy outside wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, logical_plan_output_preparation_extra_bytes(),
            "lazy outside output preparation high-water");
        projection = plan_lazy_chart_capacity_add(
            projection,
            plan_lazy_chart_capacity_multiply(
                slot_count,
                logical_plan_outside_slot_resident_bytes(
                    chart.pattern_count, maximum_context_width, maximum_arity),
                "lazy outside admitted slots"),
            "lazy outside wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, logical_plan_outside_slot_preparation_extra_bytes(),
            "lazy outside slot preparation high-water");
        projection = plan_lazy_chart_capacity_add(
            projection, sizeof(plan_lazy_chart_scheduler_workspace),
            "lazy outside scheduler workspace object");
        return plan_lazy_chart_capacity_add(
            projection,
            estimate_chart_scheduler_operation_peak_bytes(
                scheduler.plan_indexed_ranges(
                    candidate, plan_lazy_chart_clade_range_options())),
            "lazy outside scheduler operation");
      };
      auto project_reused_wave = [&](std::size_t candidate) {
        auto projection = plan_lazy_chart_capacity_add(
            retained_resident_bytes,
            lazy_multisite_chart_capacity_resident_bytes(chart),
            "lazy outside reused wave preflight");
        projection =
            plan_lazy_chart_capacity_add(projection, coordinator_bytes,
                                         "lazy outside reused wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection,
            plan_lazy_chart_capacity_multiply(
                candidate,
                logical_plan_outside_clade_output_bytes(chart.pattern_count),
                "lazy outside wave outputs"),
            "lazy outside reused wave preflight");
        projection = plan_lazy_chart_capacity_add(
            projection, logical_plan_output_preparation_extra_bytes(),
            "lazy outside output preparation high-water");
        projection = plan_lazy_chart_capacity_add(
            projection, scheduler_workspace.outside_capacity_resident_bytes(),
            "lazy outside reused slots");
        return plan_lazy_chart_capacity_add(
            projection,
            estimate_chart_scheduler_operation_peak_bytes(
                scheduler.plan_indexed_ranges(
                    candidate, plan_lazy_chart_clade_range_options())),
            "lazy outside scheduler operation");
      };
      for (std::size_t candidate = remaining; candidate != 0; --candidate) {
        std::size_t maximum_context_width = nuc_state_count;
        std::size_t maximum_arity = 0;
        for (std::size_t local = 0; local < candidate; ++local) {
          auto const [context_width, arity] = plan_outside_clade_shape(
              plan, nonroot_level_order[level_begin + wave_begin + local]);
          maximum_context_width =
              std::max(maximum_context_width, context_width);
          maximum_arity = std::max(maximum_arity, arity);
        }
        auto const slot_count = std::min(worker_count, candidate);
        auto const reusable = scheduler_workspace.can_reuse_outside_slots(
            slot_count, chart.pattern_count, maximum_context_width,
            maximum_arity);
        if (reusable) {
          auto const projection = project_reused_wave(candidate);
          if (projection <= memory_options.memory_budget_bytes) {
            admitted_items = candidate;
            admitted_slots = slot_count;
            admitted_context_width = maximum_context_width;
            admitted_arity = maximum_arity;
            admitted_projection = projection;
            admitted_reuses_slots = true;
            break;
          }
        }
        auto const cold_projection =
            project_cold_wave(candidate, maximum_context_width, maximum_arity);
        if (cold_projection <= memory_options.memory_budget_bytes) {
          admitted_items = candidate;
          admitted_slots = slot_count;
          admitted_context_width = maximum_context_width;
          admitted_arity = maximum_arity;
          admitted_projection = cold_projection;
          break;
        }
      }
      if (admitted_items == 0) {
        auto const clade = nonroot_level_order[level_begin + wave_begin];
        auto const [context_width, arity] =
            plan_outside_clade_shape(plan, clade);
        auto const required = project_cold_wave(1, context_width, arity);
        require_plan_lazy_chart_memory_budget(
            memory_options, memory_report,
            plan_lazy_chart_memory_phase::outside, required);
        throw std::logic_error("lazy outside chart: unreachable admission");
      }
      if (admitted_items < remaining) level_memory_limited = true;

      if (!admitted_reuses_slots &&
          scheduler_workspace.outside_by_slot.capacity() != 0) {
        scheduler_workspace.release_outside();
        if (memory_report != nullptr) {
          ++memory_report->outside_workspace_evictions;
        }
      } else if (admitted_reuses_slots && memory_report != nullptr) {
        ++memory_report->outside_reused_slot_waves;
      }
      require_plan_lazy_chart_memory_budget(
          memory_options, memory_report, plan_lazy_chart_memory_phase::outside,
          admitted_projection);
      if (!admitted_reuses_slots) {
        scheduler_workspace.hard_bound_outside_slots(admitted_slots);
      }
      for (std::size_t slot = 0; slot < admitted_slots; ++slot) {
        auto& workspace = scheduler_workspace.outside_by_slot[slot];
        auto workspace_live = plan_lazy_chart_capacity_add(
            retained_resident_bytes,
            lazy_multisite_chart_capacity_resident_bytes(chart),
            "lazy outside slot preparation");
        workspace_live = plan_lazy_chart_capacity_add(
            workspace_live, coordinator_bytes, "lazy outside slot preparation");
        workspace_live = plan_lazy_chart_capacity_add(
            workspace_live,
            scheduler_workspace.outside_capacity_resident_bytes(),
            "lazy outside slot preparation");
        auto const workspace_resident = workspace.resident_bytes();
        if (workspace_live < workspace_resident) {
          throw std::logic_error(
              "lazy outside slot is absent from scheduler live capacity");
        }
        auto const outside_workspace = workspace_live - workspace_resident;
        if (outside_workspace > memory_options.memory_budget_bytes) {
          reject_plan_lazy_chart_already_observed(
              memory_options, memory_report,
              plan_lazy_chart_memory_phase::outside, workspace_live);
        }
        plan_lazy_slot_preparation_report preparation;
        try {
          preparation = prepare_plan_outside_slot(
              workspace, chart.pattern_count, admitted_context_width,
              admitted_arity,
              memory_options.memory_budget_bytes - outside_workspace);
        } catch (
            lazy_key_grouping_detail::packed_key_grouping_budget_error const&
                error) {
          auto const required = plan_lazy_chart_capacity_add(
              outside_workspace, error.required_bytes(),
              "lazy outside rejected slot preparation");
          record_plan_lazy_chart_preflight(memory_report, required);
          record_plan_lazy_chart_actual_peak(
              memory_report,
              plan_lazy_rejected_preparation_actual_peak(
                  error, outside_workspace,
                  plan_lazy_chart_capacity_add(
                      outside_workspace, workspace.resident_bytes(),
                      "lazy outside rejected slot capacity"),
                  "lazy outside rejected slot actual peak"));
          if (memory_report != nullptr) ++memory_report->pre_submit_rejections;
          throw plan_lazy_chart_memory_budget_error(
              plan_lazy_chart_memory_phase::outside, required,
              memory_options.memory_budget_bytes);
        }
        auto preparation_live = plan_lazy_chart_capacity_add(
            outside_workspace, preparation.peak_resident_bytes,
            "lazy outside slot preparation");
        record_plan_lazy_chart_actual_peak(memory_report, preparation_live);
        if (preparation_live > memory_options.memory_budget_bytes) {
          reject_plan_lazy_chart_already_observed(
              memory_options, memory_report,
              plan_lazy_chart_memory_phase::outside, preparation_live);
        }
      }
      auto const output_outside_chart = plan_lazy_chart_capacity_add(
          plan_lazy_chart_capacity_add(retained_resident_bytes,
                                       coordinator_bytes,
                                       "lazy outside output preparation"),
          scheduler_workspace.outside_capacity_resident_bytes(),
          "lazy outside output preparation");
      auto const output_live = plan_lazy_chart_capacity_add(
          output_outside_chart,
          lazy_multisite_chart_capacity_resident_bytes(chart),
          "lazy outside output live capacity");
      if (output_live > memory_options.memory_budget_bytes) {
        reject_plan_lazy_chart_already_observed(
            memory_options, memory_report,
            plan_lazy_chart_memory_phase::outside, output_live);
      }
      auto const chart_peak_limit =
          memory_options.memory_budget_bytes - output_outside_chart;
      for (std::size_t local = 0; local < admitted_items; ++local) {
        try {
          auto const preparation = prepare_plan_outside_clade_output(
              chart, nonroot_level_order[level_begin + wave_begin + local],
              chart_peak_limit);
          record_plan_lazy_chart_actual_peak(
              memory_report,
              plan_lazy_chart_capacity_add(output_outside_chart,
                                           preparation.peak_resident_bytes,
                                           "lazy outside output preparation"));
        } catch (
            lazy_key_grouping_detail::packed_key_grouping_budget_error const&
                error) {
          auto const required = plan_lazy_chart_capacity_add(
              output_outside_chart, error.required_bytes(),
              "lazy outside rejected output preparation");
          record_plan_lazy_chart_preflight(memory_report, required);
          record_plan_lazy_chart_actual_peak(
              memory_report,
              plan_lazy_rejected_preparation_actual_peak(
                  error, output_outside_chart,
                  plan_lazy_chart_capacity_add(
                      output_outside_chart,
                      lazy_multisite_chart_capacity_resident_bytes(chart),
                      "lazy outside rejected output capacity"),
                  "lazy outside rejected output actual peak"));
          if (memory_report != nullptr) ++memory_report->pre_submit_rejections;
          throw plan_lazy_chart_memory_budget_error(
              plan_lazy_chart_memory_phase::outside, required,
              memory_options.memory_budget_bytes);
        }
      }
      actual = plan_lazy_chart_capacity_add(
          retained_resident_bytes,
          lazy_multisite_chart_capacity_resident_bytes(chart),
          "lazy outside prepared wave");
      actual = plan_lazy_chart_capacity_add(actual, coordinator_bytes,
                                            "lazy outside prepared wave");
      actual = plan_lazy_chart_capacity_add(
          actual, scheduler_workspace.outside_capacity_resident_bytes(),
          "lazy outside prepared wave");
      record_plan_lazy_chart_actual_peak(memory_report, actual);
      auto const scheduler_projection = plan_lazy_chart_capacity_add(
          actual,
          estimate_chart_scheduler_operation_peak_bytes(
              scheduler.plan_indexed_ranges(
                  admitted_items, plan_lazy_chart_clade_range_options())),
          "lazy outside scheduler operation");
      record_plan_lazy_chart_preflight(memory_report, scheduler_projection);
      if (scheduler_projection > memory_options.memory_budget_bytes) {
        reject_plan_lazy_chart_projected(memory_options, memory_report,
                                         plan_lazy_chart_memory_phase::outside,
                                         scheduler_projection, actual);
      }
      record_plan_lazy_chart_actual_peak(memory_report, scheduler_projection);

      chart_scheduler_run_summary failed_run;
      chart_scheduler_run_summary run;
      if (memory_report != nullptr) {
        ++memory_report->outside_admission_waves;
        memory_report->outside_max_admitted_slots =
            std::max(memory_report->outside_max_admitted_slots, admitted_slots);
        ++memory_report->scheduler_submissions;
      }
      try {
        run = scheduler.for_each_indexed_range(
            admitted_items, plan_lazy_chart_clade_range_options(),
            [&](chart_indexed_range const& range, std::size_t stable_slot,
                chart_scheduler_cancellation_token const&) {
              if (stable_slot >= scheduler_workspace.outside_by_slot.size()) {
                throw std::logic_error(
                    "lazy outside chart: scheduler slot out of range");
              }
              auto& workspace =
                  scheduler_workspace.outside_by_slot[stable_slot];
              for (std::size_t item = range.begin; item < range.end; ++item) {
                auto const clade =
                    nonroot_level_order[level_begin + wave_begin + item];
                try {
                  if (test_hooks != nullptr &&
                      test_hooks->before_outside_clade) {
                    test_hooks->before_outside_clade(clade, level, stable_slot);
                  }
                  assign_plan_outside_classes_for_clade_task_local(
                      chart, plan, patterns, clade, workspace,
                      stats_by_clade[clade]);
                  if (test_hooks != nullptr &&
                      test_hooks->after_outside_clade) {
                    test_hooks->after_outside_clade(clade, level, stable_slot);
                  }
                } catch (...) {
                  errors_by_clade[clade] = std::current_exception();
                  break;
                }
              }
            },
            &failed_run);
        if (level_runs != nullptr) level_runs->push_back(run);
      } catch (...) {
        for (std::size_t item = 0; item < level_item_count; ++item) {
          clear_plan_outside_clade_output(
              chart, nonroot_level_order[level_begin + item]);
        }
        if (test_hooks != nullptr &&
            test_hooks->observe_outside_level_failure_cleanup) {
          test_hooks->observe_outside_level_failure_cleanup(chart, level);
        }
        if (level_runs != nullptr && failed_run.failed) {
          level_runs->push_back(failed_run);
        }
        throw;
      }

      std::exception_ptr selected_error;
      for (std::size_t item = 0; item < level_item_count; ++item) {
        auto const clade = nonroot_level_order[level_begin + item];
        if (errors_by_clade[clade]) {
          selected_error = errors_by_clade[clade];
          break;
        }
      }
      if (selected_error) {
        for (std::size_t item = 0; item < level_item_count; ++item) {
          clear_plan_outside_clade_output(
              chart, nonroot_level_order[level_begin + item]);
        }
        if (test_hooks != nullptr &&
            test_hooks->observe_outside_level_failure_cleanup) {
          test_hooks->observe_outside_level_failure_cleanup(chart, level);
        }
        std::rethrow_exception(selected_error);
      }
      wave_begin += admitted_items;
    }

    if (memory_report != nullptr && level_memory_limited) {
      ++memory_report->outside_memory_limited_levels;
    }
    for (std::size_t item = 0; item < level_item_count; ++item) {
      add_plan_outside_clade_work_stats(
          chart, stats_by_clade[nonroot_level_order[level_begin + item]]);
    }
  }
  finalize_outside_counters(chart);
}

inline chart_indexed_range_options plan_lazy_chart_clade_range_options() {
  return chart_indexed_range_options{
      .minimum_grain = 1,
      .target_ranges_per_worker = 4,
  };
}

inline void validate_plan_lazy_chart_levels(
    std::span<clade_id const> order, std::span<std::size_t const> offsets,
    std::size_t clade_count, std::string_view context) {
  if (offsets.empty() || offsets.front() != 0 ||
      offsets.back() != order.size() || order.size() != clade_count) {
    throw std::runtime_error(std::string{context} +
                             ": invalid dependency-level execution plan");
  }
  for (std::size_t level = 0; level + 1 < offsets.size(); ++level) {
    if (offsets[level] > offsets[level + 1]) {
      throw std::runtime_error(std::string{context} +
                               ": invalid dependency-level offsets");
    }
  }
}

inline void reserve_plan_lazy_chart_run_summaries(
    std::vector<chart_scheduler_run_summary>* runs, std::size_t count,
    std::string_view context) {
  if (runs == nullptr) return;
  if (count > runs->max_size() - runs->size()) {
    throw std::length_error(std::string{context} +
                            ": scheduler summary overflow");
  }
  runs->reserve(runs->size() + count);
}

inline void clear_plan_inside_clade_output(lazy_multisite_chart& chart,
                                           clade_id clade) noexcept {
  chart.inside_rows_by_clade[clade].clear();
  chart.class_index_by_pattern_by_clade[clade] = std::nullopt;
  chart.structural_class_index_by_pattern_by_clade[clade] = std::nullopt;
  chart.structural_class_count_by_clade[clade] = 0;
  chart.class_weight_by_clade[clade].clear();
}

inline void clear_plan_outside_clade_output(lazy_multisite_chart& chart,
                                            clade_id clade) noexcept {
  chart.outside_rows_by_clade[clade].clear();
  chart.outside_class_index_by_pattern_by_clade[clade] = std::nullopt;
  chart.outside_class_weight_by_clade[clade].clear();
}

inline std::size_t logical_plan_lazy_inside_initial_chart_bytes(
    std::size_t clade_count) {
  auto const per_clade = sizeof(std::vector<row_type>) +
                         2 * sizeof(std::optional<std::vector<std::size_t>>) +
                         sizeof(std::size_t) +
                         sizeof(std::vector<std::uint32_t>);
  return plan_lazy_chart_capacity_add(
      sizeof(lazy_multisite_chart),
      plan_lazy_chart_capacity_multiply(clade_count, per_clade,
                                        "lazy inside initial chart surfaces"),
      "lazy inside initial chart surfaces");
}

// Allocation-free direct-API guard. It validates first, then proves that the
// conservative final output surface, coordinator arrays, and the largest cold
// singleton transient fit before initialize_plan_lazy_inside_chart() may
// allocate or any scheduler operation may begin. Existing scratch may remain
// warm when its full retained capacity fits; otherwise it is evicted before
// chart growth and the cold envelope is checked instead.
inline void preflight_plan_lazy_inside_direct_finite(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_chart_options const& options, chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary> const* level_runs,
    plan_lazy_chart_scheduler_workspace* scheduler_workspace,
    plan_lazy_chart_memory_options const& memory_options,
    plan_lazy_chart_memory_report* memory_report) {
  if (memory_options.memory_budget_bytes == 0) return;
  if (memory_report != nullptr) {
    memory_report->memory_budget_bytes = memory_options.memory_budget_bytes;
  }
  plan.assert_valid();
  if (options.chart.keep_trace) {
    throw std::runtime_error(
        "lazy inside chart: keep_trace uses the binary choice layer; lazy "
        "inside rows are row-only");
  }
  validate_patterns(plan, patterns);

  auto const clade_count = plan.clades().size();
  auto const worker_count = scheduler.worker_resolution().resolved_workers;
  if (worker_count == 0) {
    throw std::logic_error("lazy inside chart: scheduler has no worker slots");
  }
  auto const pattern_count = patterns.patterns.size();
  // Scratch for the opposite phase cannot participate in an inside wave. Drop
  // it deterministically before any chart growth rather than double-counting
  // the shared workspace object's fixed resident size.
  if (scheduler_workspace != nullptr &&
      scheduler_workspace->outside_by_slot.capacity() != 0) {
    scheduler_workspace->release_outside();
    if (memory_report != nullptr) ++memory_report->outside_workspace_evictions;
  }
  auto final_chart_bytes =
      logical_plan_lazy_inside_initial_chart_bytes(clade_count);
  std::size_t maximum_row_width = 0;
  for (auto clade : plan.bottom_up_level_order()) {
    final_chart_bytes =
        plan_lazy_chart_capacity_add(final_chart_bytes,
                                     logical_plan_inside_clade_output_bytes(
                                         plan, clade, pattern_count, options),
                                     "lazy inside final output surface");
    if (!plan.clade(clade).is_leaf()) {
      maximum_row_width = std::max(
          maximum_row_width, plan_inside_clade_row_key_width(plan, clade));
    }
  }
  auto const coordinator = logical_plan_inside_coordinator_bytes(
      clade_count, !options.retain_all_inside_class_maps, level_runs);
  auto base = plan_lazy_chart_capacity_add(
      memory_options.retained_resident_bytes,
      plan_lazy_chart_scheduler_resident_bytes(scheduler),
      "lazy inside direct preflight");
  base = plan_lazy_chart_capacity_add(base, final_chart_bytes,
                                      "lazy inside direct preflight");
  base = plan_lazy_chart_capacity_add(base, coordinator,
                                      "lazy inside direct preflight");

  auto scratch_bytes = [&] {
    return scheduler_workspace != nullptr
               ? scheduler_workspace->inside_capacity_resident_bytes()
               : sizeof(plan_lazy_chart_scheduler_workspace);
  };
  if (scheduler_workspace != nullptr &&
      scheduler_workspace->inside_by_slot.size() > worker_count) {
    scheduler_workspace->release_inside();
    if (memory_report != nullptr) ++memory_report->inside_workspace_evictions;
  }
  auto retained_live = plan_lazy_chart_capacity_add(
      base, scratch_bytes(), "lazy inside direct retained scratch");
  if (retained_live > memory_options.memory_budget_bytes &&
      scheduler_workspace != nullptr &&
      scheduler_workspace->inside_by_slot.capacity() != 0) {
    scheduler_workspace->release_inside();
    if (memory_report != nullptr) ++memory_report->inside_workspace_evictions;
    retained_live = plan_lazy_chart_capacity_add(
        base, scratch_bytes(), "lazy inside direct cold scratch");
  }

  if (plan.bottom_up_level_order().empty()) {
    throw std::logic_error("lazy inside chart: empty dependency order");
  }
  auto cold_singleton = plan_lazy_chart_capacity_add(
      base, sizeof(plan_lazy_chart_scheduler_workspace),
      "lazy inside direct singleton preflight");
  cold_singleton = plan_lazy_chart_capacity_add(
      cold_singleton,
      logical_plan_inside_slot_resident_bytes(pattern_count, maximum_row_width),
      "lazy inside direct singleton preflight");
  cold_singleton = plan_lazy_chart_capacity_add(
      cold_singleton,
      logical_plan_inside_slot_preparation_extra_bytes(pattern_count),
      "lazy inside direct singleton preflight");
  cold_singleton = plan_lazy_chart_capacity_add(
      cold_singleton, logical_plan_output_preparation_extra_bytes(),
      "lazy inside direct singleton preflight");
  auto const required = std::max(retained_live, cold_singleton);
  require_plan_lazy_chart_memory_budget(memory_options, memory_report,
                                        plan_lazy_chart_memory_phase::inside,
                                        required);
}

inline lazy_multisite_chart initialize_plan_lazy_inside_chart(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_chart_options const& options) {
  plan.assert_valid();
  if (options.chart.keep_trace) {
    throw std::runtime_error(
        "lazy inside chart: keep_trace uses the binary choice layer; lazy "
        "inside rows are row-only");
  }
  validate_patterns(plan, patterns);

  lazy_multisite_chart chart;
  chart.pattern_count = patterns.patterns.size();
  for (auto const& pattern : patterns.patterns) {
    chart.total_pattern_weight += pattern.weight;
  }
  auto const clade_count = plan.clades().size();
  chart.inside_rows_by_clade.resize(clade_count);
  chart.class_index_by_pattern_by_clade.resize(clade_count);
  chart.structural_class_index_by_pattern_by_clade.resize(clade_count);
  chart.structural_class_count_by_clade.assign(clade_count, 0);
  chart.class_weight_by_clade.resize(clade_count);
  return chart;
}

}  // namespace lazy_chart_detail

inline lazy_multisite_chart build_lazy_inside_chart(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_chart_options const& options = {}) {
  using namespace lazy_chart_detail;

  parsimony_chart_detail::validate_chart_grammar(grammar);
  if (options.chart.keep_trace) {
    throw std::runtime_error(
        "lazy inside chart: keep_trace uses the binary choice layer; lazy "
        "inside rows are row-only");
  }
  validate_patterns(grammar, patterns);

  lazy_multisite_chart chart;
  chart.pattern_count = patterns.patterns.size();
  for (auto const& pattern : patterns.patterns) {
    chart.total_pattern_weight += pattern.weight;
  }
  chart.inside_rows_by_clade.resize(grammar.clades.size());
  chart.class_index_by_pattern_by_clade.resize(grammar.clades.size());
  chart.structural_class_index_by_pattern_by_clade.resize(grammar.clades.size());
  chart.structural_class_count_by_clade.assign(grammar.clades.size(), 0);
  chart.class_weight_by_clade.resize(grammar.clades.size());

  auto remaining_dependencies = count_map_dependencies(grammar);
  grammar_parent_key_workspace key_workspace;
  std::vector<bool> built(grammar.clades.size(), false);
  auto build_clade = [&](auto&& self, clade_id clade) -> void {
    if (clade == no_clade || clade >= grammar.clades.size()) {
      throw std::runtime_error("lazy chart: clade id out of range");
    }
    if (built[clade]) return;

    auto const& key = grammar.clades[clade];
    if (key.taxa.empty()) {
      throw std::runtime_error("lazy chart: empty clade is invalid");
    }

    if (key.taxa.size() == 1) {
      assign_leaf_classes(chart, grammar, patterns, clade, options);
    } else {
      if (options.retain_all_inside_class_maps) {
        for (auto pid : grammar.productions_by_parent[clade]) {
          if (pid == no_production || pid >= grammar.productions.size()) {
            throw std::runtime_error("lazy chart: production id out of range");
          }
          auto const& prod = grammar.productions[pid];
          if (prod.parent != clade) {
            throw std::runtime_error(
                "lazy chart: productions_by_parent contains mismatched parent");
          }
          parsimony_chart_detail::validate_production_inside_row_inputs(
              grammar, prod, pid, "lazy inside chart");
          for (auto child : prod.children) self(self, child);
        }
        assign_internal_classes(chart, grammar, patterns, clade,
                                key_workspace);
      } else {
        auto build_child = [&](clade_id child) { self(self, child); };
        auto keys = collect_sparse_grammar_parent_keys(
            chart, grammar, patterns, clade, build_child,
            remaining_dependencies, key_workspace);
        assign_grammar_internal_classes_from_keys(chart, grammar, patterns,
                                                  clade, keys);
      }
    }
    built[clade] = true;
  };

  build_clade(build_clade, grammar.root_clade);
  for (auto clade : clades_by_increasing_size(grammar)) {
    build_clade(build_clade, clade);
  }

  finalize_inside_counters(chart);
  maybe_discard_nonroot_maps(chart, grammar, options);
  return chart;
}

// Trusted lazy-inside recurrence over an immutable checked execution plan.
// Unlike the grammar overload, this path performs no grammar validation,
// production-partition validation, recursive descriptor discovery, or clade
// sorting.  Pattern payloads are still checked because they are build inputs.
inline lazy_multisite_chart build_lazy_inside_chart(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_chart_options const& options = {}) {
  using namespace lazy_chart_detail;

  auto chart = initialize_plan_lazy_inside_chart(plan, patterns, options);

  std::optional<map_dependency_counts> remaining_dependencies;
  if (!options.retain_all_inside_class_maps) {
    remaining_dependencies = count_map_dependencies(plan);
  }

  plan_parent_key_workspace key_workspace;
  for (auto clade : plan.bottom_up_order()) {
    if (plan.clade(clade).is_leaf()) {
      assign_plan_leaf_classes(chart, plan, patterns, clade, options);
      continue;
    }
    auto keys =
        collect_plan_parent_keys(chart, plan, patterns, clade, key_workspace);
    if (remaining_dependencies) {
      consume_plan_parent_map_dependencies(chart, plan, clade,
                                           *remaining_dependencies);
    }
    assign_plan_internal_classes(chart, plan, patterns, clade, keys);
  }

  finalize_inside_counters(chart);
  maybe_discard_nonroot_maps(chart, plan, options);
  return chart;
}

// Dependency-level scheduled trusted-plan build. Each worker publishes only a
// disjoint clade payload. Sparse child maps and global counters remain
// coordinator-owned and are consumed/folded only after the whole level joins.
inline lazy_multisite_chart build_lazy_inside_chart_scheduled(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_chart_options const& options, chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary>* level_runs = nullptr,
    lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks const* test_hooks =
        nullptr,
    lazy_chart_detail::plan_lazy_chart_scheduler_workspace*
        scheduler_workspace = nullptr,
    lazy_chart_detail::plan_lazy_chart_memory_options const& memory_options =
        {},
    lazy_chart_detail::plan_lazy_chart_memory_report* memory_report = nullptr) {
  using namespace lazy_chart_detail;

  preflight_plan_lazy_inside_direct_finite(plan, patterns, options, scheduler,
                                           level_runs, scheduler_workspace,
                                           memory_options, memory_report);
  auto chart = initialize_plan_lazy_inside_chart(plan, patterns, options);
  auto const level_order = plan.bottom_up_level_order();
  auto const level_offsets = plan.bottom_up_level_offsets();
  auto const clade_count = plan.clades().size();
  validate_plan_lazy_chart_levels(level_order, level_offsets, clade_count,
                                  "lazy inside chart");
  auto const level_count = level_offsets.size() - 1;
  if (memory_options.memory_budget_bytes != 0) {
    plan_lazy_chart_scheduler_workspace local_scheduler_workspace;
    auto& shared_scheduler_workspace = scheduler_workspace != nullptr
                                           ? *scheduler_workspace
                                           : local_scheduler_workspace;
    return build_plan_lazy_inside_chart_scheduled_finite(
        std::move(chart), plan, patterns, options, scheduler, level_order,
        level_offsets, level_runs, test_hooks, shared_scheduler_workspace,
        memory_options, memory_report);
  }
  reserve_plan_lazy_chart_run_summaries(level_runs, level_count,
                                        "lazy inside chart");

  std::optional<map_dependency_counts> remaining_dependencies;
  if (!options.retain_all_inside_class_maps) {
    remaining_dependencies = count_map_dependencies(plan);
  }

  auto const slot_count = scheduler.worker_resolution().resolved_workers;
  if (slot_count == 0) {
    throw std::logic_error("lazy inside chart: scheduler has no worker slots");
  }
  plan_lazy_chart_scheduler_workspace local_scheduler_workspace;
  auto& shared_scheduler_workspace = scheduler_workspace != nullptr
                                         ? *scheduler_workspace
                                         : local_scheduler_workspace;
  shared_scheduler_workspace.prepare_inside_slots(slot_count);
  auto& workspaces = shared_scheduler_workspace.inside_by_slot;
  std::vector<plan_inside_clade_work_stats> stats_by_clade(clade_count);
  std::vector<std::exception_ptr> errors_by_clade(clade_count);

  for (std::size_t level = 0; level < level_count; ++level) {
    auto const begin = level_offsets[level];
    auto const end = level_offsets[level + 1];
    auto const item_count = end - begin;
    if (item_count == 0) continue;
    chart_scheduler_run_summary failed_run;
    chart_scheduler_run_summary run;
    try {
      run = scheduler.for_each_indexed_range(
          item_count, plan_lazy_chart_clade_range_options(),
          [&](chart_indexed_range const& range, std::size_t stable_slot,
              chart_scheduler_cancellation_token const&) {
            if (stable_slot >= workspaces.size()) {
              throw std::logic_error(
                  "lazy inside chart: scheduler slot out of range");
            }
            auto& workspace = workspaces[stable_slot];
            for (std::size_t item = range.begin; item < range.end; ++item) {
              auto const clade = level_order[begin + item];
              try {
                if (test_hooks != nullptr && test_hooks->before_inside_clade) {
                  test_hooks->before_inside_clade(clade, level, stable_slot);
                }
                if (plan.clade(clade).is_leaf()) {
                  assign_plan_leaf_classes(chart, plan, patterns, clade,
                                           options);
                } else {
                  auto keys = collect_plan_parent_keys(chart, plan, patterns,
                                                       clade, workspace);
                  assign_plan_internal_classes_task_local(
                      chart, plan, patterns, clade, keys,
                      stats_by_clade[clade]);
                }
                if (test_hooks != nullptr &&
                    test_hooks->observe_completed_inside_clade) {
                  test_hooks->observe_completed_inside_clade(
                      chart, clade, level, stable_slot);
                }
                if (test_hooks != nullptr && test_hooks->after_inside_clade) {
                  test_hooks->after_inside_clade(clade, level, stable_slot);
                }
              } catch (...) {
                errors_by_clade[clade] = std::current_exception();
                break;
              }
            }
          },
          &failed_run);
      if (level_runs != nullptr) level_runs->push_back(run);
    } catch (...) {
      for (std::size_t item = 0; item < item_count; ++item) {
        clear_plan_inside_clade_output(chart, level_order[begin + item]);
      }
      if (level_runs != nullptr && failed_run.failed) {
        level_runs->push_back(failed_run);
      }
      throw;
    }

    std::exception_ptr selected_error;
    for (std::size_t item = 0; item < item_count; ++item) {
      auto const clade = level_order[begin + item];
      if (errors_by_clade[clade]) {
        selected_error = errors_by_clade[clade];
        break;
      }
    }
    if (selected_error) {
      for (std::size_t item = 0; item < item_count; ++item) {
        clear_plan_inside_clade_output(chart, level_order[begin + item]);
      }
      std::rethrow_exception(selected_error);
    }

    // The level join above is the read barrier for every shared child map.
    // Reclamation is stable and serial, so siblings that share one child can
    // never invalidate one another's packed-key collection.
    if (test_hooks != nullptr && test_hooks->observe_inside_level_join) {
      test_hooks->observe_inside_level_join(chart, level);
    }
    for (std::size_t item = 0; item < item_count; ++item) {
      auto const clade = level_order[begin + item];
      if (remaining_dependencies && !plan.clade(clade).is_leaf()) {
        consume_plan_parent_map_dependencies(chart, plan, clade,
                                             *remaining_dependencies);
      }
      add_plan_inside_clade_work_stats(chart, stats_by_clade[clade]);
    }
    if (test_hooks != nullptr && test_hooks->observe_inside_level_reclamation) {
      test_hooks->observe_inside_level_reclamation(chart, level);
    }
  }

  finalize_inside_counters(chart);
  maybe_discard_nonroot_maps(chart, plan, options);
  return chart;
}

template <class ActivePatternSet>
inline lazy_multisite_chart build_lazy_inside_chart_active(
    clade_grammar const& grammar, ActivePatternSet const& active_patterns,
    lazy_chart_options const& options = {}) {
  active_patterns.assert_no_skipped_invariant_metadata();
  return build_lazy_inside_chart(grammar, active_patterns.patterns, options);
}

template <class ActivePatternSet>
inline lazy_multisite_chart build_lazy_inside_chart_active(
    chart_execution_plan const& plan, ActivePatternSet const& active_patterns,
    lazy_chart_options const& options = {}) {
  active_patterns.assert_no_skipped_invariant_metadata();
  return build_lazy_inside_chart(plan, active_patterns.patterns, options);
}

inline std::uint64_t lazy_weighted_root_score_from_row(
    lazy_multisite_chart const& chart, clade_id root, std::size_t pattern_index,
    site_pattern const& pattern, chart_options const& options = {}) {
  if (options.score_ua_edge) {
    chart_multisite_detail::validate_pattern_reference_counts(pattern,
                                                              pattern_index);
  }
  auto const& row = lazy_chart_detail::root_inside_row_for_pattern(
      chart, root, pattern_index);
  return chart_spr_weighted_root_score_from_row(row, pattern, options);
}

inline std::uint64_t lazy_weighted_root_score_from_row(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, std::size_t pattern_index,
    chart_options const& options = {}) {
  lazy_chart_detail::validate_lazy_inside_score_inputs(
      grammar, patterns, chart, options, "lazy weighted root score");
  if (pattern_index >= patterns.patterns.size()) {
    throw std::runtime_error(
        "lazy weighted root score: pattern index out of range");
  }
  return lazy_weighted_root_score_from_row(
      chart, grammar.root_clade, pattern_index, patterns.patterns[pattern_index],
      options);
}

inline std::uint64_t lazy_weighted_root_score_from_row(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, std::size_t pattern_index,
    chart_options const& options = {}) {
  lazy_chart_detail::validate_lazy_inside_score_inputs(
      plan, patterns, chart, "lazy weighted root score");
  if (pattern_index >= patterns.patterns.size()) {
    throw std::runtime_error(
        "lazy weighted root score: pattern index out of range");
  }
  return lazy_weighted_root_score_from_row(
      chart, plan.root_clade(), pattern_index,
      patterns.patterns[pattern_index], options);
}

inline composite_chart_score lazy_composite_chart_score(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  lazy_chart_detail::validate_lazy_inside_score_inputs(
      grammar, patterns, chart, options, "lazy composite chart score");

  auto root = grammar.root_clade;
  auto const& root_rows = chart.inside_rows_by_clade[root];

  composite_chart_score result;
  result.multifurcation_productions_scored =
      chart.multifurcation_productions_scored;
  result.per_pattern_root_min.reserve(patterns.patterns.size());
  result.per_pattern_root_min_by_reference_state.reserve(
      patterns.patterns.size());

  std::uint64_t total = lazy_chart_detail::lazy_root_class_score_total(
      patterns, chart, root, options, "lazy composite chart score");

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& row = lazy_chart_detail::root_inside_row_for_pattern(
        chart, root, pattern_index);
    auto const& pattern = patterns.patterns[pattern_index];
    std::array<chart_cost, nuc_state_count> by_reference{};
    by_reference.fill(chart_inf);
    chart_cost diagnostic_min = chart_inf;
    if (!options.score_ua_edge) {
      diagnostic_min = chart_multisite_detail::row_min(row);
      by_reference.fill(diagnostic_min);
    } else {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        if (pattern.reference_state_counts[reference_state] == 0) continue;
        chart_cost best = chart_inf;
        for (std::uint8_t root_state = 0; root_state < nuc_state_count;
             ++root_state) {
          best = std::min(
              best, parsimony_chart_detail::saturated_add(
                        row[root_state],
                        parsimony_chart_detail::transition_cost(
                            reference_state, root_state)));
        }
        by_reference[reference_state] = best;
        diagnostic_min = std::min(diagnostic_min, best);
      }
    }
    result.per_pattern_root_min.push_back(diagnostic_min);
    result.per_pattern_root_min_by_reference_state.push_back(by_reference);
  }

  if (options.score_ua_edge) {
    total = chart_multisite_detail::checked_add_u64(
        total, patterns.skipped_invariant_constant_score_with_reference_edge,
        "lazy composite skipped invariant UA-edge offset");
  }
  result.weighted_lower_bound = total;
  return result;
}

inline composite_chart_score lazy_composite_chart_score(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  lazy_chart_detail::validate_lazy_inside_score_inputs(
      plan, patterns, chart, "lazy composite chart score");
  if (options.score_ua_edge) {
    for (std::size_t pattern_index = 0;
         pattern_index < patterns.patterns.size(); ++pattern_index) {
      chart_multisite_detail::validate_pattern_reference_counts(
          patterns.patterns[pattern_index], pattern_index);
    }
  }

  auto const root = plan.root_clade();
  composite_chart_score result;
  result.multifurcation_productions_scored =
      chart.multifurcation_productions_scored;
  result.per_pattern_root_min.reserve(patterns.patterns.size());
  result.per_pattern_root_min_by_reference_state.reserve(
      patterns.patterns.size());

  std::uint64_t total = lazy_chart_detail::lazy_root_class_score_total(
      patterns, chart, root, options, "lazy composite chart score");

  for (std::size_t pattern_index = 0;
       pattern_index < patterns.patterns.size(); ++pattern_index) {
    auto const& row = lazy_chart_detail::root_inside_row_for_pattern(
        chart, root, pattern_index);
    auto const& pattern = patterns.patterns[pattern_index];
    std::array<chart_cost, nuc_state_count> by_reference{};
    by_reference.fill(chart_inf);
    chart_cost diagnostic_min = chart_inf;
    if (!options.score_ua_edge) {
      diagnostic_min = chart_multisite_detail::row_min(row);
      by_reference.fill(diagnostic_min);
    } else {
      for (std::uint8_t reference_state = 0;
           reference_state < nuc_state_count; ++reference_state) {
        if (pattern.reference_state_counts[reference_state] == 0) continue;
        chart_cost best = chart_inf;
        for (std::uint8_t root_state = 0; root_state < nuc_state_count;
             ++root_state) {
          best = std::min(
              best, parsimony_chart_detail::saturated_add(
                        row[root_state],
                        static_cast<chart_cost>(plan.transition_cost(
                            reference_state, root_state))));
        }
        by_reference[reference_state] = best;
        diagnostic_min = std::min(diagnostic_min, best);
      }
    }
    result.per_pattern_root_min.push_back(diagnostic_min);
    result.per_pattern_root_min_by_reference_state.push_back(by_reference);
  }

  if (options.score_ua_edge) {
    total = chart_multisite_detail::checked_add_u64(
        total, patterns.skipped_invariant_constant_score_with_reference_edge,
        "lazy composite skipped invariant UA-edge offset");
  }
  result.weighted_lower_bound = total;
  return result;
}

inline std::uint64_t lazy_composite_lower_bound(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  lazy_chart_detail::validate_lazy_inside_score_inputs(
      grammar, patterns, chart, options, "lazy composite lower bound");
  auto total = lazy_chart_detail::lazy_root_class_score_total(
      patterns, chart, grammar.root_clade, options,
      "lazy composite lower bound");
  if (options.score_ua_edge) {
    total = chart_multisite_detail::checked_add_u64(
        total, patterns.skipped_invariant_constant_score_with_reference_edge,
        "lazy composite skipped invariant UA-edge offset");
  }
  return total;
}

inline std::uint64_t lazy_composite_lower_bound(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  lazy_chart_detail::validate_lazy_inside_score_inputs(
      plan, patterns, chart, "lazy composite lower bound");
  if (options.score_ua_edge) {
    for (std::size_t pattern_index = 0;
         pattern_index < patterns.patterns.size(); ++pattern_index) {
      chart_multisite_detail::validate_pattern_reference_counts(
          patterns.patterns[pattern_index], pattern_index);
    }
  }
  auto total = lazy_chart_detail::lazy_root_class_score_total(
      patterns, chart, plan.root_clade(), options,
      "lazy composite lower bound");
  if (options.score_ua_edge) {
    total = chart_multisite_detail::checked_add_u64(
        total, patterns.skipped_invariant_constant_score_with_reference_edge,
        "lazy composite skipped invariant UA-edge offset");
  }
  return total;
}

template <class ActivePatternSet>
inline std::uint64_t lazy_composite_lower_bound_active(
    clade_grammar const& grammar, ActivePatternSet const& active_patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  active_patterns.assert_no_skipped_invariant_metadata();
  return lazy_composite_lower_bound(grammar, active_patterns.patterns, chart,
                                    options);
}

template <class ActivePatternSet>
inline std::uint64_t lazy_composite_lower_bound_active(
    chart_execution_plan const& plan, ActivePatternSet const& active_patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  active_patterns.assert_no_skipped_invariant_metadata();
  return lazy_composite_lower_bound(plan, active_patterns.patterns, chart,
                                    options);
}

inline void build_lazy_outside_chart_in_place(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart& chart, chart_options const& options,
    std::uint8_t reference_state) {
  using namespace lazy_chart_detail;

  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  validate_patterns(grammar, patterns);
  if (options.score_ua_edge) {
    parsimony_chart_detail::validate_state(reference_state, "reference");
  }
  if (chart.pattern_count != patterns.patterns.size()) {
    throw std::runtime_error(
        "lazy outside chart: pattern count does not match lazy inside chart");
  }
  if (chart.inside_rows_by_clade.size() != grammar.clades.size()) {
    throw std::runtime_error(
        "lazy outside chart: inside row clade count mismatch");
  }
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error("lazy outside chart: root clade out of range");
  }
  materialize_inside_class_maps(chart, grammar, patterns);

  chart.outside_rows_by_clade.assign(grammar.clades.size(), {});
  chart.outside_class_index_by_pattern_by_clade.assign(grammar.clades.size(),
                                                       std::nullopt);
  chart.outside_class_weight_by_clade.assign(grammar.clades.size(), {});
  chart.outside_global_min_by_pattern.assign(chart.pattern_count, chart_inf);
  chart.lazy_outside_rows_computed = 0;
  chart.outside_multifurcation_productions_scored = 0;
  chart.outside_recurrence_work = {};

  auto root = grammar.root_clade;
  auto root_row = parsimony_chart_detail::make_inf_row();
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    root_row[state] =
        options.score_ua_edge
            ? parsimony_chart_detail::transition_cost(reference_state, state)
            : chart_cost{0};
  }
  chart.outside_rows_by_clade[root].push_back(root_row);
  chart.outside_class_index_by_pattern_by_clade[root] =
      std::vector<std::size_t>(chart.pattern_count, 0);
  chart.outside_class_weight_by_clade[root].push_back(0);
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    checked_add_weight(chart.outside_class_weight_by_clade[root].front(),
                       patterns.patterns[pattern].weight,
                       "root outside class");
    auto const& inside_root = chart.inside_row(root, pattern);
    chart_cost best = chart_inf;
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      best = std::min(best, parsimony_chart_detail::saturated_add(
                                inside_root[state], root_row[state]));
    }
    chart.outside_global_min_by_pattern[pattern] = best;
  }

  outside_context_key_workspace context_key_workspace;
  auto order = chart_trim_detail::clades_by_decreasing_size(grammar);
  for (auto clade : order) {
    if (clade == root) continue;
    assign_outside_classes_for_clade(chart, grammar, patterns, clade,
                                     context_key_workspace);
  }

  finalize_outside_counters(chart);
}

inline void build_lazy_outside_chart_in_place(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart& chart, chart_options const& options = {}) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "lazy outside chart: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  build_lazy_outside_chart_in_place(grammar, patterns, chart, options,
                                    std::uint8_t{0});
}

inline lazy_multisite_chart build_lazy_outside_chart(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart chart, chart_options const& options,
    std::uint8_t reference_state) {
  build_lazy_outside_chart_in_place(grammar, patterns, chart, options,
                                    reference_state);
  return chart;
}

inline lazy_multisite_chart build_lazy_outside_chart(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart chart, chart_options const& options = {}) {
  build_lazy_outside_chart_in_place(grammar, patterns, chart, options);
  return chart;
}

namespace lazy_chart_detail {

inline std::size_t logical_plan_lazy_outside_initial_growth_bytes(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan) {
  auto const clade_count = plan.clades().size();
  auto const pattern_count = chart.pattern_count;
  auto total = plan_lazy_chart_capacity_multiply(
      clade_count,
      sizeof(std::vector<row_type>) +
          sizeof(std::optional<std::vector<std::size_t>>) +
          sizeof(std::vector<std::uint32_t>),
      "lazy outside initial clade surfaces");
  total = plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(pattern_count, sizeof(chart_cost),
                                        "lazy outside initial global minima"),
      "lazy outside initial surfaces");
  total = plan_lazy_chart_capacity_add(total,
                                       sizeof(row_type) + sizeof(std::uint32_t),
                                       "lazy outside initial root rows");
  return plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(pattern_count, sizeof(std::size_t),
                                        "lazy outside initial root class map"),
      "lazy outside initial surfaces");
}

inline std::size_t logical_plan_lazy_missing_inside_map_preparation_bytes(
    lazy_multisite_chart const& chart, chart_execution_plan const& plan) {
  auto const pattern_count = chart.pattern_count;
  std::size_t missing_count = 0;
  std::size_t maximum_row_width = 0;
  bool missing_internal_map = false;
  for (auto clade : plan.bottom_up_order()) {
    if (!chart.class_index_by_pattern_by_clade[clade] ||
        !chart.structural_class_index_by_pattern_by_clade[clade]) {
      ++missing_count;
      if (!plan.clade(clade).is_leaf()) {
        missing_internal_map = true;
        maximum_row_width = std::max(
            maximum_row_width, plan_inside_clade_row_key_width(plan, clade));
      }
    }
  }
  if (missing_count == 0) return 0;
  auto const selected_index_map_bytes = lazy_key_grouping_detail::
      frozen_libstdcxx_allocate_at_least_capacity_bytes<std::size_t>(
          pattern_count, "lazy outside missing inside map capacity");
  auto total = plan_lazy_chart_capacity_multiply(
      missing_count,
      plan_lazy_chart_capacity_multiply(selected_index_map_bytes, 2,
                                        "lazy outside missing inside maps"),
      "lazy outside missing inside maps");
  total = plan_lazy_chart_capacity_add(
      total,
      logical_plan_inside_slot_resident_bytes(pattern_count, maximum_row_width),
      "lazy outside inside-map materialization scratch");
  total = plan_lazy_chart_capacity_add(
      total, logical_plan_inside_slot_preparation_extra_bytes(pattern_count),
      "lazy outside inside-map materialization scratch");
  if (missing_internal_map) {
    // materialize_inside_class_maps() keeps both published class maps and the
    // reusable packed-key workspace live while constructing this independent
    // structural-class -> stored-row lookup.  It is serial across clades, so
    // one worst-case pattern-sized vector is the complete overlapping bound.
    // Charge the frozen allocator-selected capacity rather than only logical
    // elements: three size_t values request 24 bytes but own 32 in GCC 17.
    auto const structural_to_row_capacity = lazy_key_grouping_detail::
        frozen_libstdcxx_allocate_at_least_capacity_bytes<std::size_t>(
            pattern_count, "lazy outside structural-row lookup");
    total = plan_lazy_chart_capacity_add(
        total,
        plan_lazy_chart_capacity_add(sizeof(std::vector<std::size_t>),
                                     structural_to_row_capacity,
                                     "lazy outside structural-row lookup"),
        "lazy outside inside-map materialization scratch");
  }
  // The compatibility materializer still owns an ordered row lookup. Charge a
  // conservative red-black node/control envelope until that serial boundary is
  // migrated to the prepared grouping seam.
  return plan_lazy_chart_capacity_add(
      total,
      plan_lazy_chart_capacity_multiply(
          pattern_count,
          sizeof(std::pair<row_type const, std::size_t>) + 4 * sizeof(void*),
          "lazy outside materialized row lookup"),
      "lazy outside inside-map materialization scratch");
}

inline void preflight_plan_lazy_outside_direct_finite(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options,
    std::uint8_t reference_state, chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary> const* level_runs,
    plan_lazy_chart_scheduler_workspace* scheduler_workspace,
    plan_lazy_chart_memory_options const& memory_options,
    plan_lazy_chart_memory_report* memory_report) {
  if (memory_options.memory_budget_bytes == 0) return;
  if (memory_report != nullptr) {
    memory_report->memory_budget_bytes = memory_options.memory_budget_bytes;
  }
  plan.assert_valid();
  validate_patterns(plan, patterns);
  if (options.score_ua_edge) {
    parsimony_chart_detail::validate_state(reference_state, "reference");
  }
  auto const clade_count = plan.clades().size();
  if (chart.pattern_count != patterns.patterns.size()) {
    throw std::runtime_error(
        "lazy outside chart: pattern count does not match lazy inside chart");
  }
  if (chart.inside_rows_by_clade.size() != clade_count ||
      chart.class_index_by_pattern_by_clade.size() != clade_count ||
      chart.structural_class_index_by_pattern_by_clade.size() != clade_count ||
      chart.structural_class_count_by_clade.size() != clade_count) {
    throw std::runtime_error(
        "lazy outside chart: inside chart clade count mismatch");
  }
  auto const worker_count = scheduler.worker_resolution().resolved_workers;
  if (worker_count == 0) {
    throw std::logic_error("lazy outside chart: scheduler has no worker slots");
  }
  // The inside slot array is unusable during outside recurrence. Releasing it
  // before initialization also enforces the phase-lifetime contract for direct
  // callers that do not follow the search-state wrapper.
  if (scheduler_workspace != nullptr &&
      scheduler_workspace->inside_by_slot.capacity() != 0) {
    scheduler_workspace->release_inside();
    if (memory_report != nullptr) ++memory_report->inside_workspace_evictions;
  }

  auto chart_peak = lazy_multisite_chart_capacity_resident_bytes(chart);
  chart_peak = plan_lazy_chart_capacity_add(
      chart_peak,
      logical_plan_lazy_missing_inside_map_preparation_bytes(chart, plan),
      "lazy outside direct initialization peak");
  chart_peak = plan_lazy_chart_capacity_add(
      chart_peak, logical_plan_lazy_outside_initial_growth_bytes(chart, plan),
      "lazy outside direct initialization peak");
  bool has_nonroot = false;
  std::size_t maximum_context_width = nuc_state_count;
  std::size_t maximum_arity = 0;
  for (auto clade : plan.top_down_level_order()) {
    if (clade == plan.root_clade()) continue;
    has_nonroot = true;
    chart_peak = plan_lazy_chart_capacity_add(
        chart_peak,
        logical_plan_outside_clade_output_bytes(chart.pattern_count),
        "lazy outside final output surface");
    auto const [context_width, arity] = plan_outside_clade_shape(plan, clade);
    maximum_context_width = std::max(maximum_context_width, context_width);
    maximum_arity = std::max(maximum_arity, arity);
  }
  auto base = plan_lazy_chart_capacity_add(
      memory_options.retained_resident_bytes,
      plan_lazy_chart_scheduler_resident_bytes(scheduler),
      "lazy outside direct preflight");
  base = plan_lazy_chart_capacity_add(base, chart_peak,
                                      "lazy outside direct preflight");
  base = plan_lazy_chart_capacity_add(
      base,
      logical_plan_outside_coordinator_bytes(
          clade_count, plan.top_down_level_offsets().size() - 1, level_runs),
      "lazy outside direct preflight");

  auto scratch_bytes = [&] {
    return scheduler_workspace != nullptr
               ? scheduler_workspace->outside_capacity_resident_bytes()
               : sizeof(plan_lazy_chart_scheduler_workspace);
  };
  if (scheduler_workspace != nullptr &&
      scheduler_workspace->outside_by_slot.size() > worker_count) {
    scheduler_workspace->release_outside();
    if (memory_report != nullptr) ++memory_report->outside_workspace_evictions;
  }
  auto retained_live = plan_lazy_chart_capacity_add(
      base, scratch_bytes(), "lazy outside direct retained scratch");
  if (retained_live > memory_options.memory_budget_bytes &&
      scheduler_workspace != nullptr &&
      scheduler_workspace->outside_by_slot.capacity() != 0) {
    scheduler_workspace->release_outside();
    if (memory_report != nullptr) ++memory_report->outside_workspace_evictions;
    retained_live = plan_lazy_chart_capacity_add(
        base, scratch_bytes(), "lazy outside direct cold scratch");
  }

  auto cold_singleton = base;
  if (has_nonroot) {
    cold_singleton = plan_lazy_chart_capacity_add(
        cold_singleton, sizeof(plan_lazy_chart_scheduler_workspace),
        "lazy outside direct singleton preflight");
    cold_singleton = plan_lazy_chart_capacity_add(
        cold_singleton,
        logical_plan_outside_slot_resident_bytes(
            chart.pattern_count, maximum_context_width, maximum_arity),
        "lazy outside direct singleton preflight");
    cold_singleton = plan_lazy_chart_capacity_add(
        cold_singleton, logical_plan_outside_slot_preparation_extra_bytes(),
        "lazy outside direct singleton preflight");
    cold_singleton = plan_lazy_chart_capacity_add(
        cold_singleton, logical_plan_output_preparation_extra_bytes(),
        "lazy outside direct singleton preflight");
  }
  require_plan_lazy_chart_memory_budget(
      memory_options, memory_report, plan_lazy_chart_memory_phase::outside,
      std::max(retained_live, cold_singleton));
}

inline void initialize_plan_lazy_outside_chart(chart_execution_plan const& plan,
                                               site_pattern_set const& patterns,
                                               lazy_multisite_chart& chart,
                                               chart_options const& options,
                                               std::uint8_t reference_state) {
  plan.assert_valid();
  validate_patterns(plan, patterns);
  if (options.score_ua_edge) {
    parsimony_chart_detail::validate_state(reference_state, "reference");
  }
  if (chart.pattern_count != patterns.patterns.size()) {
    throw std::runtime_error(
        "lazy outside chart: pattern count does not match lazy inside chart");
  }
  auto const clade_count = plan.clades().size();
  if (chart.inside_rows_by_clade.size() != clade_count) {
    throw std::runtime_error(
        "lazy outside chart: inside row clade count mismatch");
  }
  materialize_inside_class_maps(chart, plan, patterns);

  chart.outside_rows_by_clade.assign(clade_count, {});
  chart.outside_class_index_by_pattern_by_clade.assign(clade_count,
                                                       std::nullopt);
  chart.outside_class_weight_by_clade.assign(clade_count, {});
  chart.outside_global_min_by_pattern.assign(chart.pattern_count, chart_inf);
  chart.lazy_outside_rows_computed = 0;
  chart.outside_multifurcation_productions_scored = 0;
  chart.outside_recurrence_work = {};

  auto const root = plan.root_clade();
  auto root_row = parsimony_chart_detail::make_inf_row();
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    root_row[state] =
        options.score_ua_edge
            ? static_cast<chart_cost>(
                  plan.transition_cost(reference_state, state))
            : chart_cost{0};
  }
  chart.outside_rows_by_clade[root].push_back(root_row);
  chart.outside_class_index_by_pattern_by_clade[root] =
      std::vector<std::size_t>(chart.pattern_count, 0);
  chart.outside_class_weight_by_clade[root].push_back(0);
  for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
    checked_add_weight(chart.outside_class_weight_by_clade[root].front(),
                       patterns.patterns[pattern].weight,
                       "root outside class");
    auto const& inside_root = chart.inside_row(root, pattern);
    chart_cost best = chart_inf;
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      best = std::min(best, parsimony_chart_detail::saturated_add(
                                inside_root[state], root_row[state]));
    }
    chart.outside_global_min_by_pattern[pattern] = best;
  }
}

}  // namespace lazy_chart_detail

inline void build_lazy_outside_chart_in_place(chart_execution_plan const& plan,
                                              site_pattern_set const& patterns,
                                              lazy_multisite_chart& chart,
                                              chart_options const& options,
                                              std::uint8_t reference_state) {
  using namespace lazy_chart_detail;

  initialize_plan_lazy_outside_chart(plan, patterns, chart, options,
                                     reference_state);
  auto const root = plan.root_clade();
  outside_context_key_workspace context_key_workspace;
  for (auto clade : plan.top_down_order()) {
    if (clade == root) continue;
    assign_outside_classes_for_clade(chart, plan, patterns, clade,
                                     context_key_workspace);
  }

  finalize_outside_counters(chart);
}

// Root initialization remains serial. Every subsequent top-down dependency
// level is one joined scheduler operation whose tasks write disjoint clades;
// recurrence counters are folded only after the complete level succeeds.
inline void build_lazy_outside_chart_in_place_scheduled(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart& chart, chart_options const& options,
    std::uint8_t reference_state, chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary>* level_runs = nullptr,
    lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks const* test_hooks =
        nullptr,
    lazy_chart_detail::plan_lazy_chart_scheduler_workspace*
        scheduler_workspace = nullptr,
    lazy_chart_detail::plan_lazy_chart_memory_options const& memory_options =
        {},
    lazy_chart_detail::plan_lazy_chart_memory_report* memory_report = nullptr) {
  using namespace lazy_chart_detail;

  preflight_plan_lazy_outside_direct_finite(
      plan, patterns, chart, options, reference_state, scheduler, level_runs,
      scheduler_workspace, memory_options, memory_report);
  initialize_plan_lazy_outside_chart(plan, patterns, chart, options,
                                     reference_state);
  auto const level_order = plan.top_down_level_order();
  auto const level_offsets = plan.top_down_level_offsets();
  auto const clade_count = plan.clades().size();
  validate_plan_lazy_chart_levels(level_order, level_offsets, clade_count,
                                  "lazy outside chart");
  auto const level_count = level_offsets.size() - 1;
  if (memory_options.memory_budget_bytes != 0) {
    plan_lazy_chart_scheduler_workspace local_scheduler_workspace;
    auto& shared_scheduler_workspace = scheduler_workspace != nullptr
                                           ? *scheduler_workspace
                                           : local_scheduler_workspace;
    build_plan_lazy_outside_chart_scheduled_finite(
        chart, plan, patterns, scheduler, level_order, level_offsets,
        level_runs, test_hooks, shared_scheduler_workspace, memory_options,
        memory_report);
    return;
  }
  reserve_plan_lazy_chart_run_summaries(level_runs, level_count,
                                        "lazy outside chart");

  auto const root = plan.root_clade();
  std::vector<clade_id> nonroot_level_order;
  nonroot_level_order.reserve(clade_count - 1);
  std::vector<std::size_t> nonroot_level_offsets;
  nonroot_level_offsets.reserve(level_count + 1);
  nonroot_level_offsets.push_back(0);
  for (std::size_t level = 0; level < level_count; ++level) {
    for (std::size_t item = level_offsets[level];
         item < level_offsets[level + 1]; ++item) {
      auto const clade = level_order[item];
      if (clade != root) nonroot_level_order.push_back(clade);
    }
    nonroot_level_offsets.push_back(nonroot_level_order.size());
  }

  auto const slot_count = scheduler.worker_resolution().resolved_workers;
  if (slot_count == 0) {
    throw std::logic_error("lazy outside chart: scheduler has no worker slots");
  }
  plan_lazy_chart_scheduler_workspace local_scheduler_workspace;
  auto& shared_scheduler_workspace = scheduler_workspace != nullptr
                                         ? *scheduler_workspace
                                         : local_scheduler_workspace;
  shared_scheduler_workspace.prepare_outside_slots(slot_count);
  auto& workspaces = shared_scheduler_workspace.outside_by_slot;
  std::vector<plan_outside_clade_work_stats> stats_by_clade(clade_count);
  std::vector<std::exception_ptr> errors_by_clade(clade_count);

  for (std::size_t level = 0; level < level_count; ++level) {
    auto const begin = nonroot_level_offsets[level];
    auto const end = nonroot_level_offsets[level + 1];
    auto const item_count = end - begin;
    if (item_count == 0) continue;

    chart_scheduler_run_summary failed_run;
    chart_scheduler_run_summary run;
    try {
      run = scheduler.for_each_indexed_range(
          item_count, plan_lazy_chart_clade_range_options(),
          [&](chart_indexed_range const& range, std::size_t stable_slot,
              chart_scheduler_cancellation_token const&) {
            if (stable_slot >= workspaces.size()) {
              throw std::logic_error(
                  "lazy outside chart: scheduler slot out of range");
            }
            auto& workspace = workspaces[stable_slot];
            for (std::size_t item = range.begin; item < range.end; ++item) {
              auto const clade = nonroot_level_order[begin + item];
              try {
                if (test_hooks != nullptr && test_hooks->before_outside_clade) {
                  test_hooks->before_outside_clade(clade, level, stable_slot);
                }
                assign_plan_outside_classes_for_clade_task_local(
                    chart, plan, patterns, clade, workspace,
                    stats_by_clade[clade]);
                if (test_hooks != nullptr && test_hooks->after_outside_clade) {
                  test_hooks->after_outside_clade(clade, level, stable_slot);
                }
              } catch (...) {
                errors_by_clade[clade] = std::current_exception();
                break;
              }
            }
          },
          &failed_run);
      if (level_runs != nullptr) level_runs->push_back(run);
    } catch (...) {
      for (std::size_t item = 0; item < item_count; ++item) {
        clear_plan_outside_clade_output(chart,
                                        nonroot_level_order[begin + item]);
      }
      if (level_runs != nullptr && failed_run.failed) {
        level_runs->push_back(failed_run);
      }
      throw;
    }

    std::exception_ptr selected_error;
    for (std::size_t item = 0; item < item_count; ++item) {
      auto const clade = nonroot_level_order[begin + item];
      if (errors_by_clade[clade]) {
        selected_error = errors_by_clade[clade];
        break;
      }
    }
    if (selected_error) {
      for (std::size_t item = 0; item < item_count; ++item) {
        clear_plan_outside_clade_output(chart,
                                        nonroot_level_order[begin + item]);
      }
      std::rethrow_exception(selected_error);
    }

    for (std::size_t item = 0; item < item_count; ++item) {
      add_plan_outside_clade_work_stats(
          chart, stats_by_clade[nonroot_level_order[begin + item]]);
    }
  }

  finalize_outside_counters(chart);
}

inline void build_lazy_outside_chart_in_place_scheduled(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart& chart, chart_options const& options,
    chart_scheduler& scheduler,
    std::vector<chart_scheduler_run_summary>* level_runs = nullptr,
    lazy_chart_detail::plan_lazy_chart_scheduler_test_hooks const* test_hooks =
        nullptr,
    lazy_chart_detail::plan_lazy_chart_scheduler_workspace*
        scheduler_workspace = nullptr,
    lazy_chart_detail::plan_lazy_chart_memory_options const& memory_options =
        {},
    lazy_chart_detail::plan_lazy_chart_memory_report* memory_report = nullptr) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "lazy outside chart: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, chart, options, std::uint8_t{0}, scheduler, level_runs,
      test_hooks, scheduler_workspace, memory_options, memory_report);
}

inline void build_lazy_outside_chart_in_place(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart& chart, chart_options const& options = {}) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "lazy outside chart: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  build_lazy_outside_chart_in_place(plan, patterns, chart, options,
                                    std::uint8_t{0});
}

inline lazy_multisite_chart build_lazy_outside_chart(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart chart, chart_options const& options,
    std::uint8_t reference_state) {
  build_lazy_outside_chart_in_place(plan, patterns, chart, options,
                                    reference_state);
  return chart;
}

inline lazy_multisite_chart build_lazy_outside_chart(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart chart, chart_options const& options = {}) {
  build_lazy_outside_chart_in_place(plan, patterns, chart, options);
  return chart;
}

inline composite_chart_score build_composite_chart_score(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  return lazy_composite_chart_score(grammar, patterns, chart, options);
}

inline composite_chart_score build_composite_chart_score(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  return lazy_composite_chart_score(plan, patterns, chart, options);
}

namespace lazy_chart_detail {

inline lazy_multisite_chart prepare_lazy_chart_for_multisite_frontiers(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& source, chart_options const& options) {
  validate_lazy_inside_score_inputs(grammar, patterns, source, options,
                                    "lazy multi-site trim");
  lazy_multisite_chart prepared = source;
  materialize_inside_class_maps(prepared, grammar, patterns);
  if (!options.score_ua_edge) {
    build_lazy_outside_chart_in_place(grammar, patterns, prepared, options);
  }
  return prepared;
}

inline single_site_chart single_site_chart_from_lazy(
    clade_grammar const& grammar, lazy_multisite_chart const& chart,
    std::size_t pattern_index) {
  single_site_chart result;
  result.inside.assign(grammar.clades.size(),
                       parsimony_chart_detail::make_inf_row());
  for (clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    result.inside[clade] = chart.inside_row(clade, pattern_index);
  }
  return result;
}

inline single_site_outside_chart single_site_outside_chart_from_lazy(
    clade_grammar const& grammar, lazy_multisite_chart const& chart,
    std::size_t pattern_index) {
  single_site_outside_chart result;
  result.outside.assign(grammar.clades.size(),
                        parsimony_chart_detail::make_inf_row());
  for (clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    result.outside[clade] = chart.outside_row(clade, pattern_index);
  }
  result.global_min = chart.outside_global_min(pattern_index);
  return result;
}

inline std::vector<chart_multisite_detail::active_pattern_info>
build_active_pattern_info_from_lazy(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& prepared, chart_options const& options) {
  chart_multisite_detail::validate_multisite_inputs(grammar, patterns,
                                                    options);
  std::vector<chart_multisite_detail::active_pattern_info> active;
  std::array<lazy_multisite_chart, nuc_state_count> outside_by_reference;
  std::array<bool, nuc_state_count> outside_reference_built{};
  outside_reference_built.fill(false);

  auto outside_for_reference = [&](std::uint8_t reference_state)
      -> lazy_multisite_chart const& {
    if (!outside_reference_built[reference_state]) {
      outside_by_reference[reference_state] = prepared;
      build_lazy_outside_chart_in_place(
          grammar, patterns, outside_by_reference[reference_state], options,
          reference_state);
      outside_reference_built[reference_state] = true;
    }
    return outside_by_reference[reference_state];
  };

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (options.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(pattern,
                                                                pattern_index);
    }
    if (!chart_multisite_detail::is_active_pattern(pattern)) continue;

    chart_multisite_detail::active_pattern_info info;
    info.pattern_index = pattern_index;
    info.weight = pattern.weight;
    info.reference_state_counts = pattern.reference_state_counts;
    info.state_by_taxon = pattern.state_by_taxon;
    info.chart = single_site_chart_from_lazy(grammar, prepared, pattern_index);
    if (options.score_ua_edge) {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        if (info.reference_state_counts[reference_state] == 0) continue;
        info.outside_by_reference[reference_state] =
            single_site_outside_chart_from_lazy(
                grammar, outside_for_reference(reference_state),
                pattern_index);
      }
    } else {
      info.outside_ua_free =
          single_site_outside_chart_from_lazy(grammar, prepared, pattern_index);
    }
    active.push_back(std::move(info));
  }
  return active;
}

inline std::size_t lazy_outside_rows_computed_for_trim_diagnostic(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& prepared, chart_options const& options) {
  if (!options.score_ua_edge) return prepared.lazy_outside_rows_computed;

  std::size_t total = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    bool used = false;
    for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
         ++pattern_index) {
      auto const& pattern = patterns.patterns[pattern_index];
      if (!chart_multisite_detail::is_active_pattern(pattern)) continue;
      if (pattern.reference_state_counts[reference_state] != 0) {
        used = true;
        break;
      }
    }
    if (!used) continue;
    auto outside_chart = prepared;
    build_lazy_outside_chart_in_place(grammar, patterns, outside_chart, options,
                                      reference_state);
    total += outside_chart.lazy_outside_rows_computed;
  }
  return total;
}

inline lazy_multisite_chart prepare_lazy_chart_for_multisite_frontiers(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& source, chart_options const& options) {
  chart_multisite_detail::validate_multisite_inputs(plan, patterns, options);
  validate_lazy_inside_score_inputs(plan, patterns, source,
                                    "lazy multi-site trim");
  lazy_multisite_chart prepared = source;
  materialize_inside_class_maps(prepared, plan, patterns);
  if (!options.score_ua_edge) {
    build_lazy_outside_chart_in_place(plan, patterns, prepared, options);
  }
  return prepared;
}

inline single_site_chart single_site_chart_from_lazy(
    chart_execution_plan const& plan, lazy_multisite_chart const& chart,
    std::size_t pattern_index) {
  single_site_chart result;
  result.inside.assign(plan.clades().size(),
                       parsimony_chart_detail::make_inf_row());
  for (clade_id clade = 0; clade < plan.clades().size(); ++clade) {
    result.inside[clade] = chart.inside_row(clade, pattern_index);
  }
  return result;
}

inline single_site_outside_chart single_site_outside_chart_from_lazy(
    chart_execution_plan const& plan, lazy_multisite_chart const& chart,
    std::size_t pattern_index) {
  single_site_outside_chart result;
  result.outside.assign(plan.clades().size(),
                        parsimony_chart_detail::make_inf_row());
  for (clade_id clade = 0; clade < plan.clades().size(); ++clade) {
    result.outside[clade] = chart.outside_row(clade, pattern_index);
  }
  result.global_min = chart.outside_global_min(pattern_index);
  return result;
}

inline std::vector<chart_multisite_detail::active_pattern_info>
build_active_pattern_info_from_lazy(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& prepared, chart_options const& options) {
  chart_multisite_detail::validate_multisite_inputs(plan, patterns, options);
  std::vector<chart_multisite_detail::active_pattern_info> active;
  std::array<lazy_multisite_chart, nuc_state_count> outside_by_reference;
  std::array<bool, nuc_state_count> outside_reference_built{};
  outside_reference_built.fill(false);

  auto outside_for_reference = [&](std::uint8_t reference_state)
      -> lazy_multisite_chart const& {
    if (!outside_reference_built[reference_state]) {
      outside_by_reference[reference_state] = prepared;
      build_lazy_outside_chart_in_place(
          plan, patterns, outside_by_reference[reference_state], options,
          reference_state);
      outside_reference_built[reference_state] = true;
    }
    return outside_by_reference[reference_state];
  };

  for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
       ++pattern_index) {
    auto const& pattern = patterns.patterns[pattern_index];
    if (options.score_ua_edge) {
      chart_multisite_detail::validate_pattern_reference_counts(pattern,
                                                                pattern_index);
    }
    if (!chart_multisite_detail::is_active_pattern(pattern)) continue;

    chart_multisite_detail::active_pattern_info info;
    info.pattern_index = pattern_index;
    info.weight = pattern.weight;
    info.reference_state_counts = pattern.reference_state_counts;
    info.state_by_taxon = pattern.state_by_taxon;
    info.chart = single_site_chart_from_lazy(plan, prepared, pattern_index);
    if (options.score_ua_edge) {
      for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
           ++reference_state) {
        if (info.reference_state_counts[reference_state] == 0) continue;
        info.outside_by_reference[reference_state] =
            single_site_outside_chart_from_lazy(
                plan, outside_for_reference(reference_state), pattern_index);
      }
    } else {
      info.outside_ua_free =
          single_site_outside_chart_from_lazy(plan, prepared, pattern_index);
    }
    active.push_back(std::move(info));
  }
  return active;
}

inline std::size_t lazy_outside_rows_computed_for_trim_diagnostic(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& prepared, chart_options const& options) {
  if (!options.score_ua_edge) return prepared.lazy_outside_rows_computed;

  std::size_t total = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    bool used = false;
    for (std::size_t pattern_index = 0; pattern_index < patterns.patterns.size();
         ++pattern_index) {
      auto const& pattern = patterns.patterns[pattern_index];
      if (!chart_multisite_detail::is_active_pattern(pattern)) continue;
      if (pattern.reference_state_counts[reference_state] != 0) {
        used = true;
        break;
      }
    }
    if (!used) continue;
    auto outside_chart = prepared;
    build_lazy_outside_chart_in_place(plan, patterns, outside_chart, options,
                                      reference_state);
    total += outside_chart.lazy_outside_rows_computed;
  }
  return total;
}

inline void copy_lazy_trim_diagnostics(multisite_trim_result& result,
                                       lazy_multisite_chart const& chart,
                                       std::size_t outside_rows_computed) {
  result.lazy_chart_used = true;
  result.lazy_inside_rows_computed = chart.lazy_inside_rows_computed;
  result.lazy_outside_rows_computed = outside_rows_computed;
  result.lazy_patterns_merged_max = chart.lazy_patterns_merged_max;
  result.lazy_remerge_collisions = chart.lazy_remerge_collisions;
  result.lazy_structural_class_count_max =
      chart.lazy_structural_class_count_max;
  result.lazy_structural_class_count_by_clade =
      chart.structural_class_count_by_clade;
}

}  // namespace lazy_chart_detail

inline multisite_trim_result build_multisite_trim(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  auto prepared = lazy_chart_detail::prepare_lazy_chart_for_multisite_frontiers(
      grammar, patterns, chart, options);
  auto composite =
      lazy_composite_chart_score(grammar, patterns, prepared, options);
  auto result = chart_multisite_detail::build_multisite_trim_impl(
      grammar, patterns, options, trim_options,
      [&](chart_multisite_detail::multisite_frontier_build_options const&
              build_options,
          std::string const& context) {
        auto active = lazy_chart_detail::build_active_pattern_info_from_lazy(
            grammar, patterns, prepared, options);
        return chart_multisite_detail::build_multisite_frontiers_from_active(
            grammar, patterns, options, build_options, context,
            std::move(active), composite.weighted_lower_bound);
      });
  auto outside_rows_computed =
      lazy_chart_detail::lazy_outside_rows_computed_for_trim_diagnostic(
          grammar, patterns, prepared, options);
  lazy_chart_detail::copy_lazy_trim_diagnostics(result, prepared,
                                                outside_rows_computed);
  return result;
}

inline multisite_trim_result build_multisite_trim(
    chart_execution_plan const& plan, site_pattern_set const& patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  auto prepared = lazy_chart_detail::prepare_lazy_chart_for_multisite_frontiers(
      plan, patterns, chart, options);
  auto composite = lazy_composite_chart_score(plan, patterns, prepared, options);
  auto result = chart_multisite_detail::build_multisite_trim_impl(
      plan, patterns, options, trim_options,
      [&](chart_multisite_detail::multisite_frontier_build_options const&
              build_options,
          std::string const& context) {
        auto active = lazy_chart_detail::build_active_pattern_info_from_lazy(
            plan, patterns, prepared, options);
        return chart_multisite_detail::build_multisite_frontiers_from_active(
            plan, patterns, options, build_options, context,
            std::move(active), composite.weighted_lower_bound);
      });
  auto const outside_rows_computed =
      lazy_chart_detail::lazy_outside_rows_computed_for_trim_diagnostic(
          plan, patterns, prepared, options);
  lazy_chart_detail::copy_lazy_trim_diagnostics(result, prepared,
                                                outside_rows_computed);
  return result;
}

// Checked bridge for callers that still own both representations.  Compatibility
// is asserted exactly once here; every hot-path helper reached afterward consumes
// only the immutable execution plan and dynamic pattern/chart payloads.
inline multisite_trim_result build_multisite_trim(
    clade_grammar const& grammar, chart_execution_plan const& plan,
    site_pattern_set const& patterns, lazy_multisite_chart const& chart,
    chart_options const& options = {},
    multisite_trim_options const& trim_options = {}) {
  plan.assert_compatible(grammar);
  return build_multisite_trim(plan, patterns, chart, options, trim_options);
}

}  // namespace larch

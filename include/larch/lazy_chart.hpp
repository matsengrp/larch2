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
      parsimony_chart_detail::validate_state(
          pattern.state_by_taxon[tid],
          "lazy chart pattern " + std::to_string(pattern_index) + " taxon " +
              std::to_string(tid));
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
      parsimony_chart_detail::validate_state(
          pattern.state_by_taxon[tid],
          "lazy chart pattern " + std::to_string(pattern_index) + " taxon " +
              std::to_string(tid));
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

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    using lazy_key_grouping_detail::checked_packed_key_bytes_add;
    auto total =
        lazy_key_grouping_detail::packed_key_word_buffer_dynamic_capacity_bytes(
            packed_words);
    total = checked_packed_key_bytes_add(
        total, grouping_workspace.dynamic_capacity_bytes(),
        "lazy plan parent-key workspace capacity");
    total = checked_packed_key_bytes_add(
        total, structural_grouping.dynamic_capacity_bytes(),
        "lazy plan parent-key workspace capacity");
    return checked_packed_key_bytes_add(
        total, row_grouping.dynamic_capacity_bytes(),
        "lazy plan parent-key workspace capacity");
  }

  [[nodiscard]] std::size_t resident_bytes() const {
    return lazy_key_grouping_detail::checked_packed_key_bytes_add(
        sizeof(*this), dynamic_capacity_bytes(),
        "lazy plan parent-key workspace resident");
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
  auto structural_map = structural_classes.class_by_input;
  std::vector<std::size_t> structural_to_row_class(
      structural_classes.class_count(), 0);
  std::vector<std::size_t> row_key_to_row_class(row_key_classes.class_count(),
                                                0);
  std::map<row_type, std::size_t> row_class_by_row;
  auto& rows = chart.inside_rows_by_clade[clade];

  // Row-key class IDs are first-occurrence IDs over structural classes. This
  // is exactly the former structural-class traversal with duplicate key
  // computations skipped, without depending on lexicographic map order.
  for (std::size_t row_key_class = 0;
       row_key_class < row_key_classes.class_count(); ++row_key_class) {
    auto const structural_class =
        row_key_classes.representative_by_class[row_key_class];
    auto const row = compute_plan_internal_inside_row_from_keys(
        chart, plan, clade, keys, structural_class,
        stats.multifurcation_productions_scored);
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
    stats.lazy_remerge_collisions +=
        structural_classes.class_count() - rows.size();
  }
  chart.class_index_by_pattern_by_clade[clade] = std::move(class_map);
  chart.structural_class_index_by_pattern_by_clade[clade] =
      std::move(structural_map);
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
    auto const class_index = it->second;
    class_map[pattern] = class_index;
    checked_add_weight(weights[class_index], patterns.patterns[pattern].weight,
                       "outside class");
  }

  chart.outside_class_index_by_pattern_by_clade[clade] = std::move(class_map);
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
  std::function<void(clade_id, std::size_t, std::size_t)> before_outside_clade;
  std::function<void(clade_id, std::size_t, std::size_t)> after_outside_clade;
};

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

  void release_inside() noexcept {
    std::vector<plan_parent_key_workspace>{}.swap(inside_by_slot);
  }

  void release_outside() noexcept {
    std::vector<outside_context_key_workspace>{}.swap(outside_by_slot);
  }
};

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
        scheduler_workspace = nullptr) {
  using namespace lazy_chart_detail;

  auto chart = initialize_plan_lazy_inside_chart(plan, patterns, options);
  auto const level_order = plan.bottom_up_level_order();
  auto const level_offsets = plan.bottom_up_level_offsets();
  auto const clade_count = plan.clades().size();
  validate_plan_lazy_chart_levels(level_order, level_offsets, clade_count,
                                  "lazy inside chart");
  auto const level_count = level_offsets.size() - 1;
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
        scheduler_workspace = nullptr) {
  using namespace lazy_chart_detail;

  initialize_plan_lazy_outside_chart(plan, patterns, chart, options,
                                     reference_state);
  auto const level_order = plan.top_down_level_order();
  auto const level_offsets = plan.top_down_level_offsets();
  auto const clade_count = plan.clades().size();
  validate_plan_lazy_chart_levels(level_order, level_offsets, clade_count,
                                  "lazy outside chart");
  auto const level_count = level_offsets.size() - 1;
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
        scheduler_workspace = nullptr) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "lazy outside chart: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  build_lazy_outside_chart_in_place_scheduled(
      plan, patterns, chart, options, std::uint8_t{0}, scheduler, level_runs,
      test_hooks, scheduler_workspace);
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

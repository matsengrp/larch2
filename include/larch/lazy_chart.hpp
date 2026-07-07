#pragma once

#include <larch/chart_spr.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/site_patterns.hpp>
#include <larch/chart_trim.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
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

inline std::vector<std::size_t> structural_key_for_production(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, grammar_production const& prod,
    std::size_t pattern) {
  std::vector<std::size_t> key;
  key.reserve(prod.children.size());
  for (auto child : prod.children) {
    key.push_back(structural_class_index_for_pattern(chart, grammar, patterns,
                                                     child, pattern));
  }
  return key;
}

inline std::vector<std::size_t> row_key_for_all_productions(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns,
    std::vector<production_id> const& production_ids, std::size_t pattern) {
  std::vector<std::size_t> key;
  for (auto pid : production_ids) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error("lazy chart: production id out of range");
    }
    auto const& prod = grammar.productions[pid];
    key.reserve(key.size() + prod.children.size());
    for (auto child : prod.children) {
      key.push_back(
          inside_class_index_for_pattern(chart, grammar, patterns, child,
                                         pattern));
    }
  }
  return key;
}

inline row_type compute_internal_inside_row(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade,
    std::vector<production_id> const& production_ids,
    std::size_t representative_pattern, std::size_t& multifurcation_counter) {
  auto row = parsimony_chart_detail::make_inf_row();
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
      auto row_provider = [&](clade_id child) -> row_type const& {
        auto class_index = inside_class_index_for_pattern(
            chart, grammar, patterns, child, representative_pattern);
        auto const& child_rows = chart.inside_rows_by_clade[child];
        if (class_index >= child_rows.size()) {
          throw std::runtime_error(
              "lazy chart: child inside class index out of range");
        }
        return child_rows[class_index];
      };
      auto candidate = parsimony_chart_detail::combine_production_inside_row(
          prod, parent_state, row_provider);
      row[parent_state] = std::min(row[parent_state], candidate);
    }
  }
  return row;
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

inline void assign_internal_classes(lazy_multisite_chart& chart,
                                    clade_grammar const& grammar,
                                    site_pattern_set const& patterns,
                                    clade_id clade) {
  auto const& production_ids = grammar.productions_by_parent[clade];
  if (production_ids.empty()) {
    throw std::runtime_error("lazy chart: non-singleton clade has no productions");
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

  auto const& structural_prod = grammar.productions[production_ids.front()];
  std::map<std::vector<std::size_t>, std::size_t> structural_index_by_key;
  std::vector<std::size_t> structural_representative_pattern;
  std::vector<std::size_t> structural_map(chart.pattern_count, 0);

  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto key = structural_key_for_production(chart, grammar, patterns,
                                             structural_prod,
                                             pattern_index);
    auto [it, inserted] =
        structural_index_by_key.emplace(std::move(key),
                                        structural_index_by_key.size());
    if (inserted) structural_representative_pattern.push_back(pattern_index);
    structural_map[pattern_index] = it->second;
  }

  std::map<std::vector<std::size_t>, row_type> candidate_row_by_key;
  std::vector<std::size_t> structural_to_row_class(
      structural_representative_pattern.size(), 0);
  std::map<row_type, std::size_t> row_class_by_row;
  auto& rows = chart.inside_rows_by_clade[clade];

  for (std::size_t structural_class = 0;
       structural_class < structural_representative_pattern.size();
       ++structural_class) {
    auto representative = structural_representative_pattern[structural_class];
    auto row_key =
        row_key_for_all_productions(chart, grammar, patterns, production_ids,
                                    representative);
    auto row_it = candidate_row_by_key.find(row_key);
    if (row_it == candidate_row_by_key.end()) {
      auto row = compute_internal_inside_row(
          chart, grammar, patterns, clade, production_ids, representative,
          chart.multifurcation_productions_scored);
      row_it = candidate_row_by_key.emplace(std::move(row_key), row).first;
    }

    auto [class_it, inserted] =
        row_class_by_row.emplace(row_it->second, row_class_by_row.size());
    if (inserted) rows.push_back(row_it->second);
    structural_to_row_class[structural_class] = class_it->second;
  }

  std::vector<std::size_t> class_map(chart.pattern_count, 0);
  auto& weights = chart.class_weight_by_clade[clade];
  weights.assign(rows.size(), 0);
  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto structural_class = structural_map[pattern_index];
    auto row_class = structural_to_row_class[structural_class];
    class_map[pattern_index] = row_class;
    checked_add_weight(weights[row_class],
                       patterns.patterns[pattern_index].weight,
                       "internal class");
  }

  chart.structural_class_count_by_clade[clade] =
      structural_representative_pattern.size();
  if (structural_representative_pattern.size() > rows.size()) {
    chart.lazy_remerge_collisions +=
        structural_representative_pattern.size() - rows.size();
  }
  chart.class_index_by_pattern_by_clade[clade] = std::move(class_map);
  chart.structural_class_index_by_pattern_by_clade[clade] =
      std::move(structural_map);
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

struct sparse_parent_keys {
  std::vector<std::vector<std::size_t>> structural_key_by_pattern;
  std::vector<std::vector<std::size_t>> row_key_by_pattern;
  std::vector<std::vector<std::size_t>> row_key_offset_by_production_child;
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

inline row_type compute_internal_inside_row_from_sparse_keys(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    clade_id clade, std::vector<production_id> const& production_ids,
    sparse_parent_keys const& keys, std::size_t representative_pattern,
    std::size_t& multifurcation_counter) {
  auto row = parsimony_chart_detail::make_inf_row();
  for (std::size_t prod_i = 0; prod_i < production_ids.size(); ++prod_i) {
    auto pid = production_ids[prod_i];
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
        if (child_i >= prod.children.size() || prod.children[child_i] != child) {
          throw std::runtime_error(
              "lazy chart: sparse row provider child order mismatch");
        }
        auto offset =
            keys.row_key_offset_by_production_child[prod_i][child_i++];
        auto class_index = keys.row_key_by_pattern[representative_pattern]
                                                 [offset];
        auto const& child_rows = chart.inside_rows_by_clade[child];
        if (class_index >= child_rows.size()) {
          throw std::runtime_error(
              "lazy chart: child inside class index out of range");
        }
        return child_rows[class_index];
      };
      auto candidate = parsimony_chart_detail::combine_production_inside_row(
          prod, parent_state, row_provider);
      row[parent_state] = std::min(row[parent_state], candidate);
    }
  }
  return row;
}

inline void assign_internal_classes_from_sparse_keys(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade,
    sparse_parent_keys const& keys) {
  auto const& production_ids = grammar.productions_by_parent[clade];
  if (production_ids.empty()) {
    throw std::runtime_error("lazy chart: non-singleton clade has no productions");
  }

  std::map<std::vector<std::size_t>, std::size_t> structural_index_by_key;
  std::vector<std::size_t> structural_representative_pattern;
  std::vector<std::size_t> structural_map(chart.pattern_count, 0);

  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto [it, inserted] = structural_index_by_key.emplace(
        keys.structural_key_by_pattern[pattern_index],
        structural_index_by_key.size());
    if (inserted) structural_representative_pattern.push_back(pattern_index);
    structural_map[pattern_index] = it->second;
  }

  std::map<std::vector<std::size_t>, row_type> candidate_row_by_key;
  std::vector<std::size_t> structural_to_row_class(
      structural_representative_pattern.size(), 0);
  std::map<row_type, std::size_t> row_class_by_row;
  auto& rows = chart.inside_rows_by_clade[clade];

  for (std::size_t structural_class = 0;
       structural_class < structural_representative_pattern.size();
       ++structural_class) {
    auto representative = structural_representative_pattern[structural_class];
    auto const& row_key = keys.row_key_by_pattern[representative];
    auto row_it = candidate_row_by_key.find(row_key);
    if (row_it == candidate_row_by_key.end()) {
      auto row = compute_internal_inside_row_from_sparse_keys(
          chart, grammar, clade, production_ids, keys, representative,
          chart.multifurcation_productions_scored);
      row_it = candidate_row_by_key.emplace(row_key, row).first;
    }

    auto [class_it, inserted] =
        row_class_by_row.emplace(row_it->second, row_class_by_row.size());
    if (inserted) rows.push_back(row_it->second);
    structural_to_row_class[structural_class] = class_it->second;
  }

  std::vector<std::size_t> class_map(chart.pattern_count, 0);
  auto& weights = chart.class_weight_by_clade[clade];
  weights.assign(rows.size(), 0);
  for (std::size_t pattern_index = 0; pattern_index < chart.pattern_count;
       ++pattern_index) {
    auto structural_class = structural_map[pattern_index];
    auto row_class = structural_to_row_class[structural_class];
    class_map[pattern_index] = row_class;
    checked_add_weight(weights[row_class],
                       patterns.patterns[pattern_index].weight,
                       "internal class");
  }

  chart.structural_class_count_by_clade[clade] =
      structural_representative_pattern.size();
  if (structural_representative_pattern.size() > rows.size()) {
    chart.lazy_remerge_collisions +=
        structural_representative_pattern.size() - rows.size();
  }
  chart.class_index_by_pattern_by_clade[clade] = std::move(class_map);
  chart.structural_class_index_by_pattern_by_clade[clade] =
      std::move(structural_map);
}

template <class BuildChild>
inline sparse_parent_keys collect_sparse_parent_keys(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id parent, BuildChild&& build_child,
    map_dependency_counts& remaining) {
  auto const& production_ids = grammar.productions_by_parent[parent];
  if (production_ids.empty()) {
    throw std::runtime_error("lazy chart: non-singleton clade has no productions");
  }

  sparse_parent_keys keys;
  keys.structural_key_by_pattern.resize(chart.pattern_count);
  keys.row_key_by_pattern.resize(chart.pattern_count);
  keys.row_key_offset_by_production_child.reserve(production_ids.size());

  std::size_t next_row_offset = 0;
  for (std::size_t prod_i = 0; prod_i < production_ids.size(); ++prod_i) {
    auto pid = production_ids[prod_i];
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

    auto& offsets = keys.row_key_offset_by_production_child.emplace_back();
    offsets.reserve(prod.children.size());
    for (auto child : prod.children) {
      build_child(child);
      offsets.push_back(next_row_offset++);

      for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
        keys.row_key_by_pattern[pattern].push_back(
            inside_class_index_for_pattern(chart, grammar, patterns, child,
                                           pattern));
        if (prod_i == 0) {
          keys.structural_key_by_pattern[pattern].push_back(
              structural_class_index_for_pattern(chart, grammar, patterns,
                                                 child, pattern));
        }
      }

      consume_inside_child_map(chart, grammar, child, remaining);
      if (prod_i == 0) {
        consume_structural_child_map(chart, grammar, child, remaining);
      }
    }
  }
  return keys;
}

inline void materialize_inside_class_maps(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns) {
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
          class_map[pattern] =
              leaf_class_index_for_pattern(chart, grammar, patterns, clade,
                                           pattern);
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
      throw std::runtime_error("lazy chart: non-singleton clade has no productions");
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

    auto const& structural_prod = grammar.productions[production_ids.front()];
    std::map<std::vector<std::size_t>, std::size_t> structural_index_by_key;
    std::vector<std::size_t> structural_representative_pattern;
    std::vector<std::size_t> structural_map(chart.pattern_count, 0);
    for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
      auto key = structural_key_for_production(chart, grammar, patterns,
                                               structural_prod, pattern);
      auto [it, inserted] =
          structural_index_by_key.emplace(std::move(key),
                                          structural_index_by_key.size());
      if (inserted) structural_representative_pattern.push_back(pattern);
      structural_map[pattern] = it->second;
    }
    if (chart.structural_class_count_by_clade[clade] !=
        structural_representative_pattern.size()) {
      throw std::runtime_error(
          "lazy chart: materialized structural class count mismatch");
    }

    std::map<row_type, std::size_t> row_class_by_row;
    auto const& rows = chart.inside_rows_by_clade[clade];
    for (std::size_t class_index = 0; class_index < rows.size(); ++class_index) {
      row_class_by_row.emplace(rows[class_index], class_index);
    }

    std::vector<std::size_t> structural_to_row_class(
        structural_representative_pattern.size(), 0);
    for (std::size_t structural_class = 0;
         structural_class < structural_representative_pattern.size();
         ++structural_class) {
      auto representative = structural_representative_pattern[structural_class];
      std::size_t ignored_multifurcation_counter = 0;
      auto row = compute_internal_inside_row(
          chart, grammar, patterns, clade, production_ids, representative,
          ignored_multifurcation_counter);
      auto row_it = row_class_by_row.find(row);
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

inline row_type compute_child_outside_contribution(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, grammar_production const& prod,
    std::size_t child_slot, std::size_t representative_pattern,
    std::size_t parent_outside_class) {
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

  for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
       ++parent_state) {
    auto base = parent_outside[parent_state];
    if (base >= chart_inf) continue;
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
    auto outside_rows = chart_trim_detail::combine_production_outside_rows(
        prod, parent_state, base, inside_provider);
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      result[child_state] =
          std::min(result[child_state], outside_rows[child_slot][child_state]);
    }
  }

  return result;
}

inline std::vector<std::size_t> outside_context_key_for_pattern(
    lazy_multisite_chart const& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, grammar_production const& prod,
    std::size_t pattern) {
  std::vector<std::size_t> key;
  key.reserve(prod.children.size() + 1);
  key.push_back(outside_class_index_for_pattern(chart, prod.parent, pattern));
  for (auto child : prod.children) {
    key.push_back(
        inside_class_index_for_pattern(chart, grammar, patterns, child,
                                       pattern));
  }
  return key;
}

inline void assign_outside_classes_for_clade(
    lazy_multisite_chart& chart, clade_grammar const& grammar,
    site_pattern_set const& patterns, clade_id clade) {
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

    std::map<std::vector<std::size_t>, std::vector<std::size_t>>
        patterns_by_context;
    for (std::size_t pattern = 0; pattern < chart.pattern_count; ++pattern) {
      auto key =
          outside_context_key_for_pattern(chart, grammar, patterns, prod,
                                          pattern);
      patterns_by_context[std::move(key)].push_back(pattern);
    }

    for (auto const& [key, members] : patterns_by_context) {
      (void)key;
      auto representative = members.front();
      auto parent_outside_class =
          outside_class_index_for_pattern(chart, prod.parent, representative);
      if (prod.children.size() != 2) {
        ++chart.outside_multifurcation_productions_scored;
      }
      auto contribution = compute_child_outside_contribution(
          chart, grammar, patterns, prod, child_slot, representative,
          parent_outside_class);
      for (auto pattern : members) {
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
        assign_internal_classes(chart, grammar, patterns, clade);
      } else {
        auto build_child = [&](clade_id child) { self(self, child); };
        auto keys = collect_sparse_parent_keys(chart, grammar, patterns, clade,
                                               build_child,
                                               remaining_dependencies);
        assign_internal_classes_from_sparse_keys(chart, grammar, patterns,
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

template <class ActivePatternSet>
inline lazy_multisite_chart build_lazy_inside_chart_active(
    clade_grammar const& grammar, ActivePatternSet const& active_patterns,
    lazy_chart_options const& options = {}) {
  active_patterns.assert_no_skipped_invariant_metadata();
  return build_lazy_inside_chart(grammar, active_patterns.patterns, options);
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

template <class ActivePatternSet>
inline std::uint64_t lazy_composite_lower_bound_active(
    clade_grammar const& grammar, ActivePatternSet const& active_patterns,
    lazy_multisite_chart const& chart, chart_options const& options = {}) {
  active_patterns.assert_no_skipped_invariant_metadata();
  return lazy_composite_lower_bound(grammar, active_patterns.patterns, chart,
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

  auto order = chart_trim_detail::clades_by_decreasing_size(grammar);
  for (auto clade : order) {
    if (clade == root) continue;
    assign_outside_classes_for_clade(chart, grammar, patterns, clade);
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

}  // namespace larch

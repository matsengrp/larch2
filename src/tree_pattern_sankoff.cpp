#include <larch/tree_pattern_sankoff.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace larch {
namespace {

using cost_row = std::array<std::uint64_t, nuc_state_count>;

std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs,
                          std::string_view context) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    throw std::runtime_error("canonical-tree Sankoff: uint64 overflow at " +
                             std::string{context});
  }
  return lhs + rhs;
}

std::uint64_t checked_multiply(std::uint64_t lhs, std::uint64_t rhs,
                               std::string_view context) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::runtime_error("canonical-tree Sankoff: uint64 overflow at " +
                             std::string{context});
  }
  return lhs * rhs;
}

cost_row pattern_cost(
    topology::rooted_tree const& node,
    std::map<std::string, std::size_t, std::less<>> const& taxon_index,
    tree_sankoff_pattern const& pattern) {
  if (node.is_leaf()) {
    auto found = taxon_index.find(node.label);
    if (found == taxon_index.end() ||
        found->second >= pattern.state_mask_by_taxon.size()) {
      throw std::runtime_error(
          "canonical-tree Sankoff: leaf missing from pattern taxon order");
    }
    auto mask = pattern.state_mask_by_taxon[found->second];
    if (mask == 0 || (mask & 0xF0U) != 0) {
      throw std::runtime_error(
          "canonical-tree Sankoff: leaf state mask is not nonempty A/C/G/T");
    }
    cost_row result{};
    for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
      result[state] = (mask & static_cast<std::uint8_t>(1U << state)) == 0;
    }
    return result;
  }

  cost_row result{};
  for (auto const& child : node.children) {
    auto child_cost = pattern_cost(child, taxon_index, pattern);
    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      auto best = std::numeric_limits<std::uint64_t>::max();
      for (std::uint8_t child_state = 0; child_state < nuc_state_count;
           ++child_state) {
        auto candidate = checked_add(
            child_cost[child_state], parent_state != child_state,
            "transition");
        best = std::min(best, candidate);
      }
      result[parent_state] =
          checked_add(result[parent_state], best, "child accumulation");
    }
  }
  return result;
}

}  // namespace

std::uint64_t score_rooted_tree_sankoff(
    topology::rooted_tree const& tree,
    std::span<std::string const> taxon_labels,
    std::span<tree_sankoff_pattern const> patterns,
    bool score_reference_edge,
    std::uint64_t skipped_invariant_reference_edge_offset) {
  topology::validate(tree);
  auto tree_taxa = topology::taxa(tree);
  if (taxon_labels.size() != tree_taxa.size()) {
    throw std::runtime_error(
        "canonical-tree Sankoff: taxon-order size differs from tree");
  }
  std::map<std::string, std::size_t, std::less<>> taxon_index;
  for (std::size_t index = 0; index < taxon_labels.size(); ++index) {
    if (!taxon_index.emplace(taxon_labels[index], index).second) {
      throw std::runtime_error(
          "canonical-tree Sankoff: duplicate taxon-order label");
    }
  }
  if (!std::ranges::equal(tree_taxa, taxon_index | std::views::keys)) {
    throw std::runtime_error(
        "canonical-tree Sankoff: taxon-order labels differ from tree");
  }

  std::uint64_t total =
      score_reference_edge ? skipped_invariant_reference_edge_offset : 0;
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    auto const& pattern = patterns[index];
    if (pattern.weight == 0 ||
        pattern.state_mask_by_taxon.size() != taxon_labels.size()) {
      throw std::runtime_error(
          "canonical-tree Sankoff: invalid pattern weight or taxon count");
    }
    std::uint64_t reference_total = 0;
    for (auto count : pattern.reference_state_counts) {
      reference_total = checked_add(reference_total, count,
                                    "reference-count total");
    }
    if (reference_total != pattern.weight) {
      throw std::runtime_error(
          "canonical-tree Sankoff: reference counts do not sum to weight");
    }

    auto root_cost = pattern_cost(tree, taxon_index, pattern);
    if (!score_reference_edge) {
      auto site_cost = *std::ranges::min_element(root_cost);
      total = checked_add(total,
                          checked_multiply(site_cost, pattern.weight,
                                           "pattern weight"),
                          "pattern total");
      continue;
    }
    for (std::uint8_t reference_state = 0;
         reference_state < nuc_state_count; ++reference_state) {
      auto count = pattern.reference_state_counts[reference_state];
      if (count == 0) continue;
      auto best = std::numeric_limits<std::uint64_t>::max();
      for (std::uint8_t root_state = 0; root_state < nuc_state_count;
           ++root_state) {
        best = std::min(best, checked_add(
                                  root_cost[root_state],
                                  root_state != reference_state,
                                  "reference-root transition"));
      }
      total = checked_add(
          total, checked_multiply(best, count, "reference-state count"),
          "reference-root pattern total");
    }
  }
  return total;
}

std::uint64_t score_rooted_tree_sankoff(
    topology::rooted_tree const& tree,
    std::span<std::string const> taxon_labels,
    site_pattern_set const& patterns,
    chart_options const& options) {
  if (patterns.taxon_count != taxon_labels.size()) {
    throw std::runtime_error(
        "canonical-tree Sankoff: site-pattern taxon count mismatch");
  }
  std::vector<tree_sankoff_pattern> converted;
  converted.reserve(patterns.patterns.size());
  for (auto const& pattern : patterns.patterns) {
    tree_sankoff_pattern item;
    item.weight = pattern.weight;
    item.state_mask_by_taxon.reserve(pattern.state_by_taxon.size());
    for (auto state : pattern.state_by_taxon) {
      if (state >= nuc_state_count) {
        throw std::runtime_error(
            "canonical-tree Sankoff: site-pattern state out of range");
      }
      item.state_mask_by_taxon.push_back(
          static_cast<std::uint8_t>(1U << state));
    }
    for (std::size_t state = 0; state < nuc_state_count; ++state) {
      item.reference_state_counts[state] =
          pattern.reference_state_counts[state];
    }
    converted.push_back(std::move(item));
  }
  return score_rooted_tree_sankoff(
      tree, taxon_labels, converted, options.score_ua_edge,
      patterns.skipped_invariant_constant_score_with_reference_edge);
}

}  // namespace larch

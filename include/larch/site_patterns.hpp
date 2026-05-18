#pragma once

#include <larch/parsimony_chart.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

inline constexpr std::size_t no_site_pattern =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::uint8_t no_nuc_state =
    std::numeric_limits<std::uint8_t>::max();

struct site_pattern {
  std::vector<std::uint8_t> state_by_taxon;
  std::vector<mutation_position> positions;
  std::uint32_t weight = 0;

  // Counts of UA/reference states among positions represented by this exact
  // leaf-state pattern.  Inside charts are shared by leaf states alone, but a
  // caller that scores the UA/reference edge must still account for the
  // reference state at each original site.
  std::array<std::uint32_t, nuc_state_count> reference_state_counts{};
};

struct normalized_binary_state_map {
  std::size_t exact_pattern = no_site_pattern;
  std::size_t normalized_binary_pattern = no_site_pattern;

  // Full nucleotide-state permutation for reusing a normalized chart.  The
  // normalized chart uses A/C (0/1) for the two observed binary labels; G/T
  // (2/3) are deterministic placeholders for unobserved states so root and
  // UA/reference-edge scores can be permuted without losing labels.
  std::array<std::uint8_t, nuc_state_count> normalized_to_original{
      no_nuc_state, no_nuc_state, no_nuc_state, no_nuc_state};
  std::array<std::uint8_t, nuc_state_count> original_to_normalized{
      no_nuc_state, no_nuc_state, no_nuc_state, no_nuc_state};
};

struct normalized_binary_site_pattern {
  // Normalized 0/1 states by taxon.  Taxon 0's observed state is always 0,
  // so complementary binary sites induce the same key.  The per-exact-pattern
  // state maps below are required for actual chart reuse; this is not merely a
  // count-only grouping.
  std::vector<std::uint8_t> state_by_taxon;
  std::vector<mutation_position> positions;
  std::uint32_t weight = 0;
  std::vector<std::size_t> exact_pattern_indices;
  std::vector<normalized_binary_state_map> exact_state_maps;
};

struct site_pattern_set {
  std::vector<site_pattern> patterns;

  // Indexed by original site offset (mutation_position - 1).  Entries are
  // pattern IDs in patterns, or no_site_pattern for sites intentionally skipped
  // by site_pattern_options::skip_invariant_sites.
  std::vector<std::size_t> original_site_to_pattern;

  // Optional binary partition compression, populated only when requested.
  std::vector<normalized_binary_site_pattern> normalized_binary_patterns;
  std::vector<std::size_t> exact_pattern_to_normalized_binary_pattern;
  std::vector<normalized_binary_state_map>
      exact_pattern_to_normalized_binary_state_map;

  std::size_t taxon_count = 0;
  std::size_t total_site_count = 0;
  std::size_t invariant_site_count = 0;
  std::size_t variable_site_count = 0;
  std::size_t binary_variable_site_count = 0;
  std::size_t nonbinary_variable_site_count = 0;
  std::size_t skipped_invariant_site_count = 0;

  // Invariant sites are topology-independent.  Under the default UA-excluded
  // convention they contribute zero; under UA-included scoring each invariant
  // site contributes c(reference_state, invariant_leaf_state).
  std::uint64_t invariant_constant_score_excluding_ua = 0;
  std::uint64_t invariant_constant_score_with_reference_edge = 0;
  std::uint64_t skipped_invariant_constant_score_with_reference_edge = 0;
};

struct site_pattern_options {
  bool skip_invariant_sites = false;
  bool build_normalized_binary_patterns = false;
};

namespace site_patterns_detail {

struct vector_uint8_hash {
  std::size_t operator()(std::vector<std::uint8_t> const& values) const noexcept {
    std::size_t h = values.size();
    for (auto value : values) {
      h ^= std::hash<std::uint8_t>{}(value) + 0x9e3779b97f4a7c15ULL +
           (h << 6) + (h >> 2);
    }
    return h;
  }
};

inline std::uint8_t strict_decode_reference_state(char c,
                                                  mutation_position pos) {
  switch (c) {
    case 'A':
    case 'a':
      return nuc_base::A;
    case 'C':
    case 'c':
      return nuc_base::C;
    case 'G':
    case 'g':
      return nuc_base::G;
    case 'T':
    case 't':
      return nuc_base::T;
    default:
      throw std::runtime_error(std::string{"site patterns: non-ACGT reference "
                                           "nucleotide '"} +
                               c + "' at position " + std::to_string(pos));
  }
}

inline std::uint8_t strict_decode_mutation_state(nuc_base base,
                                                 std::string_view sample_id,
                                                 mutation_position pos) {
  if (base.raw() > nuc_base::T) {
    throw std::runtime_error(
        "site patterns: invalid nuc_base raw value " +
        std::to_string(base.raw()) + " for sample '" +
        std::string{sample_id} + "' at position " + std::to_string(pos));
  }
  return base.raw();
}

inline std::vector<std::uint8_t> strict_reference_states(
    std::string_view reference) {
  std::vector<std::uint8_t> states;
  states.reserve(reference.size());
  for (std::size_t i = 0; i < reference.size(); ++i) {
    states.push_back(strict_decode_reference_state(reference[i], i + 1));
  }
  return states;
}

inline void increment_u32(std::uint32_t& value, std::string_view label) {
  if (value == std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("site patterns: uint32 counter overflow for " +
                             std::string{label});
  }
  ++value;
}

inline std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs,
                                 std::string_view label) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    throw std::runtime_error("site patterns: uint64 overflow while adding " +
                             std::string{label});
  }
  return lhs + rhs;
}

inline std::uint64_t checked_mul(std::uint64_t lhs, std::uint64_t rhs,
                                 std::string_view label) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::runtime_error("site patterns: uint64 overflow while multiplying " +
                             std::string{label});
  }
  return lhs * rhs;
}

inline void validate_taxon_registry(taxon_registry const& taxa) {
  if (taxa.id_to_sample_id.empty())
    throw std::runtime_error("site patterns: grammar has no taxa");
  if (taxa.sample_id_to_id.size() != taxa.id_to_sample_id.size()) {
    throw std::runtime_error(
        "site patterns: taxon registry sample_id map size mismatch");
  }
  for (std::size_t tid = 0; tid < taxa.id_to_sample_id.size(); ++tid) {
    auto const& sample_id = taxa.id_to_sample_id[tid];
    auto it = taxa.sample_id_to_id.find(sample_id);
    if (it == taxa.sample_id_to_id.end() || it->second != tid) {
      throw std::runtime_error(
          "site patterns: taxon registry is not internally consistent for '" +
          sample_id + "'");
    }
  }
}

struct taxon_mutations {
  std::vector<std::pair<mutation_position, std::uint8_t>> mutations;
  bool seen = false;
};

inline std::vector<taxon_mutations> collect_taxon_mutations(
    phylo_dag& dag, clade_grammar const& grammar,
    std::size_t reference_length) {
  validate_taxon_registry(grammar.taxa);

  std::vector<taxon_mutations> result(grammar.taxa.id_to_sample_id.size());
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
            std::string sample_id{node.sample_id()};
            auto taxon_it = grammar.taxa.sample_id_to_id.find(sample_id);
            if (taxon_it == grammar.taxa.sample_id_to_id.end()) {
              throw std::runtime_error(
                  "site patterns: reachable leaf sample_id '" + sample_id +
                  "' is not present in the supplied clade grammar");
            }
            auto tid = taxon_it->second;
            if (tid >= result.size()) {
              throw std::runtime_error(
                  "site patterns: taxon id out of range for sample '" +
                  sample_id + "'");
            }

            std::vector<std::pair<mutation_position, std::uint8_t>> observed;
            for (auto const& [pos, base] : node.cg()) {
              if (pos == 0 || pos > reference_length) {
                throw std::runtime_error(
                    "site patterns: compact-genome mutation position " +
                    std::to_string(pos) + " for sample '" + sample_id +
                    "' outside reference length " +
                    std::to_string(reference_length));
              }
              observed.emplace_back(
                  pos, strict_decode_mutation_state(base, sample_id, pos));
            }

            if (result[tid].seen && result[tid].mutations != observed) {
              throw std::runtime_error(
                  "site patterns: duplicate sample_id '" + sample_id +
                  "' has conflicting compact-genome mutations");
            }
            if (!result[tid].seen) {
              result[tid].mutations = std::move(observed);
              result[tid].seen = true;
            }
          } else {
            throw std::runtime_error(
                "site patterns: expected leaf node annotation");
          }
        },
        nv);
  }

  for (std::size_t tid = 0; tid < result.size(); ++tid) {
    if (!result[tid].seen) {
      throw std::runtime_error("site patterns: missing reachable leaf for taxon '" +
                               grammar.taxa.id_to_sample_id[tid] + "'");
    }
  }
  return result;
}

inline std::size_t distinct_state_count(std::vector<std::uint8_t> const& states) {
  std::array<bool, nuc_state_count> seen{};
  std::size_t count = 0;
  for (auto state : states) {
    parsimony_chart_detail::validate_state(state, "site pattern");
    if (!seen[state]) {
      seen[state] = true;
      ++count;
    }
  }
  return count;
}

inline normalized_binary_state_map make_normalized_binary_state_map(
    std::vector<std::uint8_t> const& states) {
  if (states.empty())
    throw std::runtime_error("site patterns: cannot normalize empty pattern");
  if (distinct_state_count(states) != 2) {
    throw std::runtime_error(
        "site patterns: normalized binary state map requested for non-binary site");
  }

  auto anchor = states.front();
  parsimony_chart_detail::validate_state(anchor, "binary anchor state");
  std::uint8_t other = no_nuc_state;
  for (auto state : states) {
    parsimony_chart_detail::validate_state(state, "binary observed state");
    if (state != anchor) {
      other = state;
      break;
    }
  }
  if (other == no_nuc_state)
    throw std::runtime_error("site patterns: binary state map lost second state");

  normalized_binary_state_map map;
  map.normalized_to_original[0] = anchor;
  map.normalized_to_original[1] = other;

  std::array<bool, nuc_state_count> used{};
  used[anchor] = true;
  used[other] = true;
  std::uint8_t next_normalized = 2;
  for (std::uint8_t original = 0; original < nuc_state_count; ++original) {
    if (used[original]) continue;
    map.normalized_to_original[next_normalized++] = original;
  }

  for (std::uint8_t normalized = 0; normalized < nuc_state_count;
       ++normalized) {
    auto original = map.normalized_to_original[normalized];
    parsimony_chart_detail::validate_state(original,
                                           "normalized-to-original state map");
    map.original_to_normalized[original] = normalized;
  }
  return map;
}

inline void validate_normalized_binary_state_map(
    normalized_binary_state_map const& map) {
  std::array<bool, nuc_state_count> seen_original{};
  std::array<bool, nuc_state_count> seen_normalized{};
  for (std::uint8_t normalized = 0; normalized < nuc_state_count;
       ++normalized) {
    auto original = map.normalized_to_original[normalized];
    parsimony_chart_detail::validate_state(original,
                                           "normalized-to-original state map");
    if (seen_original[original])
      throw std::runtime_error(
          "site patterns: normalized-to-original state map is not a permutation");
    seen_original[original] = true;

    auto inverse_normalized = map.original_to_normalized[original];
    parsimony_chart_detail::validate_state(inverse_normalized,
                                           "original-to-normalized state map");
    if (inverse_normalized != normalized) {
      throw std::runtime_error(
          "site patterns: binary state map inverse mismatch");
    }
  }

  for (std::uint8_t original = 0; original < nuc_state_count; ++original) {
    auto normalized = map.original_to_normalized[original];
    parsimony_chart_detail::validate_state(normalized,
                                           "original-to-normalized state map");
    if (seen_normalized[normalized])
      throw std::runtime_error(
          "site patterns: original-to-normalized state map is not a permutation");
    seen_normalized[normalized] = true;
  }
}

inline std::vector<std::uint8_t> normalized_binary_key(
    std::vector<std::uint8_t> const& states) {
  auto state_map = make_normalized_binary_state_map(states);
  std::vector<std::uint8_t> key;
  key.reserve(states.size());
  for (auto state : states) key.push_back(state_map.original_to_normalized[state]);
  return key;
}

inline void append_position(site_pattern& pattern, mutation_position pos,
                            std::uint8_t reference_state) {
  increment_u32(pattern.weight, "site-pattern weight");
  pattern.positions.push_back(pos);
  increment_u32(pattern.reference_state_counts[reference_state],
                "site-pattern reference-state count");
}

inline void append_position(normalized_binary_site_pattern& pattern,
                            mutation_position pos) {
  increment_u32(pattern.weight, "normalized binary site-pattern weight");
  pattern.positions.push_back(pos);
}

template <typename F>
void invoke_site_pattern_callback(F& callback, std::size_t index,
                                  site_pattern const& pattern) {
  if constexpr (std::is_invocable_v<F&, std::size_t, site_pattern const&>) {
    callback(index, pattern);
  } else if constexpr (std::is_invocable_v<F&, site_pattern const&>) {
    callback(pattern);
  } else {
    static_assert(std::is_invocable_v<F&, std::size_t, site_pattern const&> ||
                      std::is_invocable_v<F&, site_pattern const&>,
                  "site-pattern callback must accept either "
                  "(std::size_t, site_pattern const&) or "
                  "(site_pattern const&)");
  }
}

inline void build_normalized_binary_patterns(site_pattern_set& set) {
  set.normalized_binary_patterns.clear();
  set.exact_pattern_to_normalized_binary_pattern.assign(set.patterns.size(),
                                                        no_site_pattern);
  set.exact_pattern_to_normalized_binary_state_map.assign(
      set.patterns.size(), normalized_binary_state_map{});

  std::unordered_map<std::vector<std::uint8_t>, std::size_t, vector_uint8_hash>
      index_by_key;

  for (std::size_t exact_idx = 0; exact_idx < set.patterns.size(); ++exact_idx) {
    auto const& exact = set.patterns[exact_idx];
    if (distinct_state_count(exact.state_by_taxon) != 2) continue;

    auto state_map = make_normalized_binary_state_map(exact.state_by_taxon);
    auto key = normalized_binary_key(exact.state_by_taxon);
    auto map_it = index_by_key.find(key);
    std::size_t normalized_idx = no_site_pattern;
    if (map_it == index_by_key.end()) {
      normalized_idx = set.normalized_binary_patterns.size();
      auto [inserted_it, inserted] = index_by_key.emplace(key, normalized_idx);
      (void)inserted_it;
      (void)inserted;

      normalized_binary_site_pattern normalized;
      normalized.state_by_taxon = std::move(key);
      set.normalized_binary_patterns.push_back(std::move(normalized));
    } else {
      normalized_idx = map_it->second;
    }

    state_map.exact_pattern = exact_idx;
    state_map.normalized_binary_pattern = normalized_idx;
    validate_normalized_binary_state_map(state_map);

    auto& normalized = set.normalized_binary_patterns[normalized_idx];
    for (auto pos : exact.positions) append_position(normalized, pos);
    normalized.exact_pattern_indices.push_back(exact_idx);
    normalized.exact_state_maps.push_back(state_map);
    set.exact_pattern_to_normalized_binary_pattern[exact_idx] = normalized_idx;
    set.exact_pattern_to_normalized_binary_state_map[exact_idx] = state_map;
  }

  for (auto& normalized : set.normalized_binary_patterns) {
    std::sort(normalized.positions.begin(), normalized.positions.end());
  }
}

}  // namespace site_patterns_detail

inline bool is_invariant_site_pattern(site_pattern const& pattern) {
  if (pattern.state_by_taxon.empty()) return false;
  return std::all_of(pattern.state_by_taxon.begin() + 1,
                     pattern.state_by_taxon.end(), [&](std::uint8_t state) {
                       return state == pattern.state_by_taxon.front();
                     });
}

inline bool is_binary_variable_site_pattern(site_pattern const& pattern) {
  return site_patterns_detail::distinct_state_count(pattern.state_by_taxon) == 2;
}

inline site_pattern_set build_site_patterns(
    phylo_dag& dag, clade_grammar const& grammar,
    site_pattern_options options = {}) {
  using namespace site_patterns_detail;

  auto const& reference = get_reference_sequence(dag);
  auto reference_states = strict_reference_states(reference);
  auto taxon_mutations =
      collect_taxon_mutations(dag, grammar, reference_states.size());

  site_pattern_set result;
  result.taxon_count = grammar.taxa.id_to_sample_id.size();
  result.total_site_count = reference_states.size();
  result.original_site_to_pattern.assign(reference_states.size(), no_site_pattern);

  std::vector<std::size_t> next_mutation_index(result.taxon_count, 0);
  std::unordered_map<std::vector<std::uint8_t>, std::size_t, vector_uint8_hash>
      pattern_index;

  std::vector<std::uint8_t> key(result.taxon_count, nuc_base::A);
  for (mutation_position pos = 1; pos <= reference_states.size(); ++pos) {
    auto reference_state = reference_states[pos - 1];
    std::fill(key.begin(), key.end(), reference_state);

    for (std::size_t tid = 0; tid < result.taxon_count; ++tid) {
      auto const& muts = taxon_mutations[tid].mutations;
      auto& mut_idx = next_mutation_index[tid];
      while (mut_idx < muts.size() && muts[mut_idx].first < pos) ++mut_idx;
      if (mut_idx < muts.size() && muts[mut_idx].first == pos) {
        key[tid] = muts[mut_idx].second;
      }
    }

    auto distinct = distinct_state_count(key);
    bool invariant = distinct == 1;
    if (invariant) {
      ++result.invariant_site_count;
      auto invariant_state = key.front();
      auto ua_edge_cost =
          parsimony_chart_detail::transition_cost(reference_state,
                                                  invariant_state);
      result.invariant_constant_score_with_reference_edge = checked_add(
          result.invariant_constant_score_with_reference_edge, ua_edge_cost,
          "invariant UA-edge constant");
    } else {
      ++result.variable_site_count;
      if (distinct == 2)
        ++result.binary_variable_site_count;
      else
        ++result.nonbinary_variable_site_count;
    }

    if (invariant && options.skip_invariant_sites) {
      ++result.skipped_invariant_site_count;
      auto invariant_state = key.front();
      auto ua_edge_cost =
          parsimony_chart_detail::transition_cost(reference_state,
                                                  invariant_state);
      result.skipped_invariant_constant_score_with_reference_edge = checked_add(
          result.skipped_invariant_constant_score_with_reference_edge,
          ua_edge_cost, "skipped invariant UA-edge constant");
      continue;
    }

    auto found = pattern_index.find(key);
    std::size_t pattern_id = no_site_pattern;
    if (found == pattern_index.end()) {
      pattern_id = result.patterns.size();
      auto [_, inserted] = pattern_index.emplace(key, pattern_id);
      (void)_;
      (void)inserted;

      site_pattern pattern;
      pattern.state_by_taxon = key;
      append_position(pattern, pos, reference_state);
      result.patterns.push_back(std::move(pattern));
    } else {
      pattern_id = found->second;
      append_position(result.patterns[pattern_id], pos, reference_state);
    }
    result.original_site_to_pattern[pos - 1] = pattern_id;
  }

  if (options.build_normalized_binary_patterns) {
    build_normalized_binary_patterns(result);
  } else {
    result.exact_pattern_to_normalized_binary_pattern.assign(
        result.patterns.size(), no_site_pattern);
    result.exact_pattern_to_normalized_binary_state_map.assign(
        result.patterns.size(), normalized_binary_state_map{});
  }

  return result;
}

inline std::vector<single_site_chart> build_pattern_charts(
    clade_grammar const& grammar, site_pattern_set const& patterns,
    chart_options options = {}) {
  std::vector<single_site_chart> charts;
  charts.reserve(patterns.patterns.size());
  for (auto const& pattern : patterns.patterns) {
    leaf_site_states states;
    states.state_by_taxon = pattern.state_by_taxon;
    charts.push_back(build_single_site_chart(grammar, states, options));
  }
  return charts;
}

template <typename F>
void for_each_site_pattern(site_pattern_set const& patterns, F&& callback) {
  for (std::size_t i = 0; i < patterns.patterns.size(); ++i) {
    site_patterns_detail::invoke_site_pattern_callback(callback, i,
                                                       patterns.patterns[i]);
  }
}

template <typename F>
void for_each_site_pattern(phylo_dag& dag, clade_grammar const& grammar,
                           site_pattern_options options, F&& callback) {
  // Memory-lean exact-pattern streaming: intern compressed patterns, then invoke
  // the callback one pattern at a time.  This deliberately does not allocate
  // original_site_to_pattern, normalized binary grouping, or chart vectors.
  using namespace site_patterns_detail;

  auto const& reference = get_reference_sequence(dag);
  auto reference_states = strict_reference_states(reference);
  auto taxon_mutations =
      collect_taxon_mutations(dag, grammar, reference_states.size());
  auto taxon_count = grammar.taxa.id_to_sample_id.size();

  std::vector<std::size_t> next_mutation_index(taxon_count, 0);
  std::unordered_map<std::vector<std::uint8_t>, std::size_t, vector_uint8_hash>
      pattern_index;
  std::vector<site_pattern> patterns;
  std::vector<std::uint8_t> key(taxon_count, nuc_base::A);

  for (mutation_position pos = 1; pos <= reference_states.size(); ++pos) {
    auto reference_state = reference_states[pos - 1];
    std::fill(key.begin(), key.end(), reference_state);

    for (std::size_t tid = 0; tid < taxon_count; ++tid) {
      auto const& muts = taxon_mutations[tid].mutations;
      auto& mut_idx = next_mutation_index[tid];
      while (mut_idx < muts.size() && muts[mut_idx].first < pos) ++mut_idx;
      if (mut_idx < muts.size() && muts[mut_idx].first == pos)
        key[tid] = muts[mut_idx].second;
    }

    bool invariant = distinct_state_count(key) == 1;
    if (invariant && options.skip_invariant_sites) continue;

    auto found = pattern_index.find(key);
    if (found == pattern_index.end()) {
      auto pattern_id = patterns.size();
      auto [_, inserted] = pattern_index.emplace(key, pattern_id);
      (void)_;
      (void)inserted;

      site_pattern pattern;
      pattern.state_by_taxon = key;
      append_position(pattern, pos, reference_state);
      patterns.push_back(std::move(pattern));
    } else {
      append_position(patterns[found->second], pos, reference_state);
    }
  }

  for (std::size_t i = 0; i < patterns.size(); ++i)
    invoke_site_pattern_callback(callback, i, patterns[i]);
}

template <typename F>
void for_each_site_pattern(phylo_dag& dag, clade_grammar const& grammar,
                           F&& callback) {
  site_pattern_options options;
  for_each_site_pattern(dag, grammar, options, std::forward<F>(callback));
}

inline std::array<chart_cost, nuc_state_count> remap_normalized_binary_chart_row(
    std::array<chart_cost, nuc_state_count> const& normalized_row,
    normalized_binary_state_map const& state_map) {
  site_patterns_detail::validate_normalized_binary_state_map(state_map);
  std::array<chart_cost, nuc_state_count> exact_row{};
  exact_row.fill(chart_inf);
  for (std::uint8_t original = 0; original < nuc_state_count; ++original) {
    exact_row[original] =
        normalized_row[state_map.original_to_normalized[original]];
  }
  return exact_row;
}

inline std::uint64_t weighted_root_min_from_normalized_binary_chart(
    single_site_chart const& normalized_chart, site_pattern const& exact_pattern,
    normalized_binary_state_map const& state_map, clade_id root,
    chart_options const& options = {}) {
  using namespace site_patterns_detail;

  validate_normalized_binary_state_map(state_map);
  if (root == no_clade || root >= normalized_chart.inside.size())
    throw std::runtime_error("site patterns: normalized chart root out of range");

  if (!options.score_ua_edge) {
    auto cost = normalized_chart.root_min_excluding_ua(root);
    if (cost >= chart_inf)
      throw std::runtime_error("site patterns: infinite normalized root cost");
    return checked_mul(exact_pattern.weight, cost,
                       "weighted normalized binary root cost");
  }

  std::uint64_t total = 0;
  std::uint64_t reference_count_sum = 0;
  for (std::uint8_t original_reference_state = 0;
       original_reference_state < nuc_state_count; ++original_reference_state) {
    auto count = exact_pattern.reference_state_counts[original_reference_state];
    reference_count_sum += count;
    if (count == 0) continue;

    auto normalized_reference_state =
        state_map.original_to_normalized[original_reference_state];
    parsimony_chart_detail::validate_state(
        normalized_reference_state, "normalized reference state");
    auto cost = normalized_chart.root_min_with_reference_edge(
        root, normalized_reference_state);
    if (cost >= chart_inf)
      throw std::runtime_error("site patterns: infinite normalized root cost");
    total = checked_add(
        total,
        checked_mul(count, cost,
                    "weighted normalized binary root-edge cost"),
        "weighted normalized binary root-edge total");
  }
  if (reference_count_sum != exact_pattern.weight) {
    throw std::runtime_error(
        "site patterns: exact pattern reference-state counts do not sum to weight");
  }
  return total;
}

inline std::uint64_t weighted_root_min(single_site_chart const& chart,
                                       site_pattern const& pattern,
                                       clade_id root,
                                       chart_options const& options = {}) {
  using namespace site_patterns_detail;

  if (!options.score_ua_edge) {
    auto cost = chart.root_min_excluding_ua(root);
    if (cost >= chart_inf)
      throw std::runtime_error("site patterns: infinite pattern root cost");
    return checked_mul(pattern.weight, cost, "weighted pattern root cost");
  }

  std::uint64_t total = 0;
  std::uint64_t reference_count_sum = 0;
  for (std::uint8_t reference_state = 0; reference_state < nuc_state_count;
       ++reference_state) {
    auto count = pattern.reference_state_counts[reference_state];
    reference_count_sum += count;
    if (count == 0) continue;
    auto cost = chart.root_min_with_reference_edge(root, reference_state);
    if (cost >= chart_inf)
      throw std::runtime_error("site patterns: infinite pattern root cost");
    total = checked_add(
        total, checked_mul(count, cost, "weighted pattern root-edge cost"),
        "weighted pattern root-edge total");
  }
  if (reference_count_sum != pattern.weight) {
    throw std::runtime_error(
        "site patterns: pattern reference-state counts do not sum to weight");
  }
  return total;
}

inline std::uint64_t weighted_pattern_chart_total(
    std::vector<single_site_chart> const& charts, site_pattern_set const& patterns,
    clade_id root, chart_options const& options = {}) {
  using namespace site_patterns_detail;

  if (charts.size() != patterns.patterns.size()) {
    throw std::runtime_error(
        "site patterns: chart count does not match site-pattern count");
  }

  std::uint64_t total = 0;
  for (std::size_t i = 0; i < charts.size(); ++i) {
    total = checked_add(total,
                        weighted_root_min(charts[i], patterns.patterns[i], root,
                                          options),
                        "weighted pattern chart total");
  }

  if (options.score_ua_edge) {
    total = checked_add(total,
                        patterns.skipped_invariant_constant_score_with_reference_edge,
                        "skipped invariant UA-edge total");
  }
  return total;
}

inline std::ostream& print_site_pattern_summary(
    std::ostream& out, site_pattern_set const& patterns) {
  out << "site_patterns:\n";
  out << "  total_sites: " << patterns.total_site_count << "\n";
  out << "  taxa: " << patterns.taxon_count << "\n";
  out << "  exact_patterns: " << patterns.patterns.size() << "\n";
  out << "  invariant_sites: " << patterns.invariant_site_count << "\n";
  out << "  variable_sites: " << patterns.variable_site_count << "\n";
  out << "  binary_variable_sites: " << patterns.binary_variable_site_count
      << "\n";
  out << "  nonbinary_variable_sites: "
      << patterns.nonbinary_variable_site_count << "\n";
  out << "  skipped_invariant_sites: "
      << patterns.skipped_invariant_site_count << "\n";
  out << "  normalized_binary_patterns: "
      << patterns.normalized_binary_patterns.size() << "\n";
  out << "  invariant_constant_score_excluding_ua: "
      << patterns.invariant_constant_score_excluding_ua << "\n";
  out << "  invariant_constant_score_with_reference_edge: "
      << patterns.invariant_constant_score_with_reference_edge << "\n";
  out << "  skipped_invariant_constant_score_with_reference_edge: "
      << patterns.skipped_invariant_constant_score_with_reference_edge << "\n";
  return out;
}

}  // namespace larch

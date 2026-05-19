#pragma once

#include <larch/clade_grammar.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {

enum class polytomy_mode : std::uint8_t {
  reject,
  audit_kary,
  expand_soft_exact_or_fail,
  expand_soft_bounded,
};

struct polytomy_refinement_options {
  polytomy_mode mode = polytomy_mode::reject;

  // Exact expansion uses compact subset-closure.  If a polytomy exceeds these
  // caps, exact_or_fail throws and bounded mode (implemented in a later phase)
  // will truncate with diagnostics.
  std::size_t max_exact_arity = 6;
  std::size_t max_new_clades_per_polytomy = 256;
  std::size_t max_new_productions_per_polytomy = 1024;
  std::size_t max_total_new_clades = 100000;
  std::size_t max_total_new_productions = 1000000;

  std::size_t max_shapes_per_polytomy = 16;
  std::size_t max_bounded_productions_per_polytomy = 64;

  bool canonicalize_binary_children = true;
  bool expand_child_group_alternatives = false;
};

enum class refined_clade_origin : std::uint8_t {
  observed_input,
  synthetic_polytomy_intermediate,
  observed_and_synthetic,
};

enum class refined_production_origin : std::uint8_t {
  observed_binary,
  observed_kary_unexpanded,
  synthetic_polytomy_refinement,
  observed_and_synthetic,
};

struct refined_clade_info {
  refined_clade_origin origin = refined_clade_origin::observed_input;
  std::vector<production_id> source_kary_productions;
  std::vector<std::size_t> source_parent_nodes;
};

struct refined_production_info {
  refined_production_origin origin = refined_production_origin::observed_binary;
  std::vector<production_id> source_productions;
  std::vector<std::size_t> source_parent_nodes;
  bool exact_refinement_component = true;
};

struct polytomy_event_audit {
  production_id source_production = no_production;
  clade_id parent = no_clade;
  std::size_t arity = 0;
  std::uint64_t source_multiplicity = 0;

  std::size_t new_clades_added = 0;
  std::size_t new_productions_added = 0;
  std::size_t selected_seed_shape_count = 0;
  std::uint64_t represented_refinement_count = 0;
  bool refinement_count_saturated = false;

  bool expanded = false;
  bool exact = false;
  bool truncated_by_shape_cap = false;
  bool truncated_by_production_cap = false;
  bool refused_by_exact_cap = false;
};

struct polytomy_refinement_audit {
  std::size_t source_clade_count = 0;
  std::size_t source_production_count = 0;
  std::size_t source_kary_production_count = 0;

  std::size_t refined_clade_count = 0;
  std::size_t refined_production_count = 0;
  std::size_t synthetic_clade_count = 0;
  std::size_t synthetic_production_count = 0;

  bool contains_kary_productions = false;
  bool binary_chart_compatible = true;
  bool exact_for_soft_polytomies = true;
  bool any_truncated = false;
  bool any_refused = false;

  std::vector<polytomy_event_audit> events;
};

struct polytomy_refinement_result {
  clade_grammar grammar;
  polytomy_refinement_audit audit;
  clade_grammar_audit source_grammar_audit;
  std::vector<refined_clade_info> clade_info;
  std::vector<refined_production_info> production_info;

  // Maps from source grammar IDs to returned grammar IDs.  Expanded k-ary
  // source productions use no_production because they have no one-to-one copy.
  std::vector<clade_id> source_clade_to_refined;
  std::vector<production_id> source_production_to_refined;
};

inline std::vector<production_id> kary_productions(
    clade_grammar const& grammar) {
  std::vector<production_id> result;
  for (std::size_t i = 0; i < grammar.productions.size(); ++i) {
    if (grammar.productions[i].children.size() > 2) {
      if (i >= static_cast<std::size_t>(no_production))
        throw std::runtime_error("polytomy refinement: too many productions");
      result.push_back(static_cast<production_id>(i));
    }
  }
  return result;
}

inline bool grammar_has_kary_productions(clade_grammar const& grammar) {
  return std::any_of(
      grammar.productions.begin(), grammar.productions.end(),
      [](grammar_production const& prod) { return prod.children.size() > 2; });
}

inline bool grammar_is_binary_chart_compatible(clade_grammar const& grammar);

namespace polytomy_refinement_detail {

inline bool sorted_unique_taxa(std::vector<taxon_id> const& taxa) {
  return !taxa.empty() && std::is_sorted(taxa.begin(), taxa.end()) &&
         std::adjacent_find(taxa.begin(), taxa.end()) == taxa.end();
}

inline bool production_children_partition_parent(
    clade_grammar const& grammar, grammar_production const& prod) {
  if (prod.parent == no_clade || prod.parent >= grammar.clades.size())
    return false;

  auto const& parent_taxa = grammar.clades[prod.parent].taxa;
  std::vector<taxon_id> covered;
  for (auto child : prod.children) {
    if (child == no_clade || child >= grammar.clades.size()) return false;
    auto const& child_taxa = grammar.clades[child].taxa;
    if (!std::includes(parent_taxa.begin(), parent_taxa.end(),
                       child_taxa.begin(), child_taxa.end())) {
      return false;
    }

    std::vector<taxon_id> overlap;
    std::set_intersection(covered.begin(), covered.end(), child_taxa.begin(),
                          child_taxa.end(), std::back_inserter(overlap));
    if (!overlap.empty()) return false;

    std::vector<taxon_id> next;
    std::set_union(covered.begin(), covered.end(), child_taxa.begin(),
                   child_taxa.end(), std::back_inserter(next));
    covered = std::move(next);
  }
  return covered == parent_taxa;
}

inline std::vector<std::size_t> production_parent_nodes(
    grammar_production const& prod) {
  std::vector<std::size_t> nodes;
  nodes.reserve(prod.witnesses.size());
  for (auto const& witness : prod.witnesses) {
    if (witness.parent_node != std::numeric_limits<std::size_t>::max())
      nodes.push_back(witness.parent_node);
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

template <class T>
inline void sort_unique(std::vector<T>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

inline void validate_phase0_public_maps(polytomy_refinement_result const& r) {
  for (auto cid : r.source_clade_to_refined) {
    if (cid == no_clade || cid >= r.grammar.clades.size()) {
      throw std::runtime_error(
          "polytomy refinement: invalid source_clade_to_refined entry");
    }
  }
  for (auto pid : r.source_production_to_refined) {
    if (pid == no_production || pid >= r.grammar.productions.size()) {
      throw std::runtime_error(
          "polytomy refinement: invalid source_production_to_refined entry");
    }
  }
}

inline polytomy_refinement_result make_phase0_result(
    clade_grammar_build_result&& built, polytomy_mode mode) {
  polytomy_refinement_result result;
  result.grammar = std::move(built.grammar);
  result.source_grammar_audit = std::move(built.audit);

  auto const& grammar = result.grammar;
  auto const kary = kary_productions(grammar);

  result.audit.source_clade_count = grammar.clades.size();
  result.audit.source_production_count = grammar.productions.size();
  result.audit.source_kary_production_count = kary.size();
  result.audit.refined_clade_count = grammar.clades.size();
  result.audit.refined_production_count = grammar.productions.size();
  result.audit.contains_kary_productions = !kary.empty();
  result.audit.binary_chart_compatible =
      grammar_is_binary_chart_compatible(grammar);
  result.audit.exact_for_soft_polytomies =
      mode != polytomy_mode::audit_kary && result.audit.binary_chart_compatible;

  result.clade_info.assign(grammar.clades.size(), refined_clade_info{});
  result.production_info.assign(grammar.productions.size(),
                                refined_production_info{});

  result.source_clade_to_refined.resize(grammar.clades.size());
  for (std::size_t i = 0; i < result.source_clade_to_refined.size(); ++i)
    result.source_clade_to_refined[i] = static_cast<clade_id>(i);

  result.source_production_to_refined.resize(grammar.productions.size());
  for (std::size_t i = 0; i < result.source_production_to_refined.size(); ++i)
    result.source_production_to_refined[i] = static_cast<production_id>(i);

  for (std::size_t i = 0; i < grammar.productions.size(); ++i) {
    auto pid = static_cast<production_id>(i);
    auto const& prod = grammar.productions[i];
    auto& info = result.production_info[i];
    info.source_productions = {pid};
    info.source_parent_nodes = production_parent_nodes(prod);
    sort_unique(info.source_productions);
    sort_unique(info.source_parent_nodes);

    if (prod.children.size() == 2) {
      info.origin = refined_production_origin::observed_binary;
      info.exact_refinement_component = true;
    } else if (prod.children.size() > 2 && mode == polytomy_mode::audit_kary) {
      info.origin = refined_production_origin::observed_kary_unexpanded;
      info.exact_refinement_component = false;

      polytomy_event_audit event;
      event.source_production = pid;
      event.parent = prod.parent;
      event.arity = prod.children.size();
      event.source_multiplicity = prod.multiplicity;
      event.expanded = false;
      event.exact = false;
      result.audit.events.push_back(event);
    }
  }

  std::sort(result.audit.events.begin(), result.audit.events.end(),
            [](polytomy_event_audit const& a, polytomy_event_audit const& b) {
              return a.source_production < b.source_production;
            });

  validate_phase0_public_maps(result);
  return result;
}

inline refined_clade_origin merge_clade_origin(refined_clade_origin current,
                                               refined_clade_origin added) {
  if (current == added) return current;
  if (current == refined_clade_origin::observed_and_synthetic) return current;
  if (added == refined_clade_origin::observed_input) {
    return current == refined_clade_origin::synthetic_polytomy_intermediate
               ? refined_clade_origin::observed_and_synthetic
               : current;
  }
  return refined_clade_origin::observed_and_synthetic;
}

inline refined_production_origin merge_production_origin(
    refined_production_origin current, refined_production_origin added) {
  if (current == added) return current;
  if (current == refined_production_origin::observed_and_synthetic)
    return current;
  if (added == refined_production_origin::observed_and_synthetic) return added;
  if ((current == refined_production_origin::observed_binary &&
       added == refined_production_origin::synthetic_polytomy_refinement) ||
      (current == refined_production_origin::synthetic_polytomy_refinement &&
       added == refined_production_origin::observed_binary)) {
    return refined_production_origin::observed_and_synthetic;
  }
  return added;
}

inline bool clade_id_key_less(clade_grammar const& grammar, clade_id lhs,
                              clade_id rhs) {
  auto const& lkey = grammar.clades[lhs];
  auto const& rkey = grammar.clades[rhs];
  if (detail::clade_key_less(lkey, rkey)) return true;
  if (detail::clade_key_less(rkey, lkey)) return false;
  return lhs < rhs;
}

inline void canonicalize_production_children_by_clade_key(
    clade_grammar const& draft, grammar_production& prod) {
  if (prod.children.size() <= 1) return;

  std::vector<std::size_t> permutation(prod.children.size());
  std::iota(permutation.begin(), permutation.end(), std::size_t{0});
  std::stable_sort(permutation.begin(), permutation.end(),
                   [&](std::size_t lhs, std::size_t rhs) {
                     return clade_id_key_less(draft, prod.children[lhs],
                                              prod.children[rhs]);
                   });

  auto old_children = prod.children;
  for (std::size_t i = 0; i < permutation.size(); ++i)
    prod.children[i] = old_children[permutation[i]];

  for (auto& witness : prod.witnesses) {
    if (witness.children.empty()) continue;
    if (witness.children.size() != permutation.size()) {
      throw std::runtime_error(
          "polytomy refinement: observed witness arity does not match "
          "production arity");
    }
    auto old_witness_children = witness.children;
    for (std::size_t i = 0; i < permutation.size(); ++i)
      witness.children[i] = std::move(old_witness_children[permutation[i]]);
  }
}

struct draft_production_record {
  grammar_production prod;
  refined_production_info info;
  bool has_observed_direct_witness = false;
};

struct exact_expansion_context {
  clade_grammar const& source;
  polytomy_refinement_options const& options;

  clade_grammar draft;
  std::vector<refined_clade_info> clade_info;
  std::vector<draft_production_record> productions;
  std::map<std::vector<taxon_id>, clade_id> clade_by_taxa;
  std::map<detail::production_map_key, production_id> production_by_key;

  std::vector<clade_id> source_clade_to_draft;
  std::vector<production_id> source_production_to_draft;

  std::size_t total_new_clades = 0;
  std::size_t total_new_productions = 0;

  exact_expansion_context(clade_grammar const& source_,
                          polytomy_refinement_options const& options_)
      : source(source_), options(options_) {
    draft.taxa = source.taxa;
    draft.clades = source.clades;
    draft.node_to_clade = source.node_to_clade;
    draft.root_clade = source.root_clade;

    clade_info.assign(draft.clades.size(), refined_clade_info{});
    source_clade_to_draft.resize(source.clades.size(), no_clade);
    for (std::size_t cid = 0; cid < source.clades.size(); ++cid) {
      auto id = static_cast<clade_id>(cid);
      source_clade_to_draft[cid] = id;
      auto [_, inserted] = clade_by_taxa.emplace(source.clades[cid].taxa, id);
      if (!inserted) {
        throw std::runtime_error(
            "polytomy refinement: duplicate source clade taxon key");
      }
    }

    source_production_to_draft.assign(source.productions.size(), no_production);
  }

  std::pair<clade_id, bool> ensure_refined_clade(
      std::vector<taxon_id> taxa, refined_clade_origin origin,
      production_id source_prod, std::vector<std::size_t> const& parent_nodes,
      polytomy_event_audit* event) {
    std::sort(taxa.begin(), taxa.end());
    taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
    if (!sorted_unique_taxa(taxa)) {
      throw std::runtime_error(
          "polytomy refinement: attempted to intern invalid clade taxon key");
    }

    auto found = clade_by_taxa.find(taxa);
    if (found != clade_by_taxa.end()) {
      auto cid = found->second;
      auto& info = clade_info[cid];
      info.origin = merge_clade_origin(info.origin, origin);
      if (origin != refined_clade_origin::observed_input &&
          source_prod != no_production) {
        info.source_kary_productions.push_back(source_prod);
        info.source_parent_nodes.insert(info.source_parent_nodes.end(),
                                        parent_nodes.begin(),
                                        parent_nodes.end());
      }
      return {cid, false};
    }

    if (draft.clades.size() >= static_cast<std::size_t>(no_clade)) {
      throw std::runtime_error("polytomy refinement: too many refined clades");
    }
    if (origin != refined_clade_origin::observed_input) {
      if (event != nullptr &&
          event->new_clades_added + 1 > options.max_new_clades_per_polytomy) {
        event->refused_by_exact_cap = true;
        throw std::runtime_error(
            "polytomy refinement: exact expansion exceeds per-polytomy "
            "new-clade cap");
      }
      if (total_new_clades + 1 > options.max_total_new_clades) {
        if (event != nullptr) event->refused_by_exact_cap = true;
        throw std::runtime_error(
            "polytomy refinement: exact expansion exceeds total new-clade cap");
      }
    }

    auto cid = static_cast<clade_id>(draft.clades.size());
    draft.clades.push_back(clade_key{taxa});
    auto [_, inserted] = clade_by_taxa.emplace(draft.clades.back().taxa, cid);
    if (!inserted) {
      throw std::runtime_error(
          "polytomy refinement: internal clade interning error");
    }

    refined_clade_info info;
    info.origin = origin;
    if (origin != refined_clade_origin::observed_input &&
        source_prod != no_production) {
      info.source_kary_productions.push_back(source_prod);
      info.source_parent_nodes = parent_nodes;
      ++total_new_clades;
      if (event != nullptr) ++event->new_clades_added;
    }
    clade_info.push_back(std::move(info));
    return {cid, true};
  }

  grammar_production remap_observed_production(grammar_production const& prod) {
    grammar_production remapped;
    remapped.parent = source_clade_to_draft.at(prod.parent);
    remapped.children.reserve(prod.children.size());
    for (auto child : prod.children) {
      if (child == no_clade || child >= source_clade_to_draft.size() ||
          source_clade_to_draft[child] == no_clade) {
        throw std::runtime_error(
            "polytomy refinement: source production child out of range");
      }
      remapped.children.push_back(source_clade_to_draft[child]);
    }
    remapped.witnesses = prod.witnesses;
    remapped.multiplicity = prod.multiplicity;
    for (auto& witness : remapped.witnesses) {
      for (auto& child_witness : witness.children) {
        auto child = child_witness.child;
        if (child == no_clade || child >= source_clade_to_draft.size() ||
            source_clade_to_draft[child] == no_clade) {
          throw std::runtime_error(
              "polytomy refinement: source witness child out of range");
        }
        child_witness.child = source_clade_to_draft[child];
      }
    }
    if (options.canonicalize_binary_children)
      canonicalize_production_children_by_clade_key(draft, remapped);
    return remapped;
  }

  std::pair<production_id, bool> add_production(
      grammar_production candidate, refined_production_origin origin,
      std::vector<production_id> source_productions,
      std::vector<std::size_t> source_parent_nodes,
      polytomy_event_audit* event) {
    if (candidate.parent == no_clade ||
        candidate.parent >= draft.clades.size()) {
      throw std::runtime_error(
          "polytomy refinement: production parent out of range");
    }
    for (auto child : candidate.children) {
      if (child == no_clade || child >= draft.clades.size()) {
        throw std::runtime_error(
            "polytomy refinement: production child out of range");
      }
    }
    if (options.canonicalize_binary_children)
      canonicalize_production_children_by_clade_key(draft, candidate);

    detail::production_map_key key{candidate.parent, candidate.children};
    auto found = production_by_key.find(key);
    bool synthetic =
        origin == refined_production_origin::synthetic_polytomy_refinement;
    if (found == production_by_key.end()) {
      if (productions.size() >= static_cast<std::size_t>(no_production)) {
        throw std::runtime_error(
            "polytomy refinement: too many refined productions");
      }
      if (synthetic) {
        if (event != nullptr && event->new_productions_added + 1 >
                                    options.max_new_productions_per_polytomy) {
          event->refused_by_exact_cap = true;
          throw std::runtime_error(
              "polytomy refinement: exact expansion exceeds per-polytomy "
              "new-production cap");
        }
        if (total_new_productions + 1 > options.max_total_new_productions) {
          if (event != nullptr) event->refused_by_exact_cap = true;
          throw std::runtime_error(
              "polytomy refinement: exact expansion exceeds total "
              "new-production cap");
        }
      }

      draft_production_record record;
      record.prod = std::move(candidate);
      record.info.origin = origin;
      record.info.source_productions = std::move(source_productions);
      record.info.source_parent_nodes = std::move(source_parent_nodes);
      record.info.exact_refinement_component = true;
      record.has_observed_direct_witness =
          origin == refined_production_origin::observed_binary;

      auto pid = static_cast<production_id>(productions.size());
      productions.push_back(std::move(record));
      production_by_key.emplace(std::move(key), pid);
      if (synthetic) {
        ++total_new_productions;
        if (event != nullptr) ++event->new_productions_added;
      }
      return {pid, true};
    }

    auto pid = found->second;
    auto& record = productions[pid];
    bool const observed_candidate =
        origin == refined_production_origin::observed_binary;
    bool has_new_observed_source = false;
    if (observed_candidate) {
      for (auto source_prod : source_productions) {
        if (std::find(record.info.source_productions.begin(),
                      record.info.source_productions.end(),
                      source_prod) == record.info.source_productions.end()) {
          has_new_observed_source = true;
          break;
        }
      }
    }

    record.info.origin = merge_production_origin(record.info.origin, origin);
    record.info.source_productions.insert(record.info.source_productions.end(),
                                          source_productions.begin(),
                                          source_productions.end());
    record.info.source_parent_nodes.insert(
        record.info.source_parent_nodes.end(), source_parent_nodes.begin(),
        source_parent_nodes.end());
    if (observed_candidate && has_new_observed_source) {
      record.prod.witnesses.insert(record.prod.witnesses.end(),
                                   candidate.witnesses.begin(),
                                   candidate.witnesses.end());
      record.prod.multiplicity += candidate.multiplicity;
      record.has_observed_direct_witness = true;
    }

    return {pid, false};
  }
};

inline std::uint64_t rooted_binary_refinement_count(std::size_t arity,
                                                    bool& saturated) {
  if (arity <= 2) return 1;
  std::uint64_t result = 1;
  for (std::size_t value = 3; value <= 2 * arity - 3; value += 2)
    result = detail::saturated_mul(result, static_cast<std::uint64_t>(value),
                                   saturated);
  return result;
}

inline std::uint64_t subset_refinement_count(std::size_t arity,
                                             bool& saturated) {
  if (arity == 0) return 0;
  if (arity > 63) {
    saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }

  auto full = (std::uint64_t{1} << arity) - 1;
  std::vector<std::uint64_t> counts(full + 1, 0);
  for (std::uint64_t mask = 1; mask <= full; ++mask) {
    auto bits = std::popcount(mask);
    if (bits == 1) {
      counts[mask] = 1;
      continue;
    }

    auto lowest = mask & (~mask + 1);
    std::uint64_t total = 0;
    for (auto sub = (mask - 1) & mask; sub != 0; sub = (sub - 1) & mask) {
      if ((sub & lowest) == 0) continue;
      auto other = mask ^ sub;
      if (other == 0) continue;
      auto product =
          detail::saturated_mul(counts[sub], counts[other], saturated);
      total = detail::saturated_add(total, product, saturated);
    }
    counts[mask] = total;
  }
  return counts[full];
}

inline std::uint64_t represented_refinement_count_for_exact_grammar(
    std::size_t arity, bool& saturated) {
  auto expected = rooted_binary_refinement_count(arity, saturated);
  // The subset-DP count is a useful self-check on small events, but allocating
  // 2^k rows for large user-raised caps is unnecessary: exact subset-closure
  // represents the rooted unordered binary refinement count by construction.
  if (arity <= 20) {
    bool dp_saturated = false;
    auto dp_count = subset_refinement_count(arity, dp_saturated);
    saturated = saturated || dp_saturated;
    if (!saturated && dp_count != expected) {
      throw std::runtime_error(
          "polytomy refinement: internal refinement-count mismatch");
    }
  }
  return expected;
}

inline std::uint64_t saturated_pow_u64(std::uint64_t base, std::size_t exp,
                                       bool& saturated) {
  std::uint64_t result = 1;
  for (std::size_t i = 0; i < exp; ++i)
    result = detail::saturated_mul(result, base, saturated);
  return result;
}

inline std::uint64_t exact_subset_clade_upper_bound(std::size_t arity,
                                                    bool& saturated) {
  if (arity < 3) return 0;
  auto pow2 = saturated_pow_u64(2, arity, saturated);
  if (saturated || pow2 < arity + 2) {
    saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }
  return pow2 - static_cast<std::uint64_t>(arity) - 2;
}

inline std::uint64_t exact_subset_production_count(std::size_t arity,
                                                   bool& saturated) {
  if (arity < 2) return 0;
  auto pow3 = saturated_pow_u64(3, arity, saturated);
  auto pow2_next = saturated_pow_u64(2, arity + 1, saturated);
  if (saturated || pow3 < pow2_next - 1) {
    saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }
  return (pow3 - pow2_next + 1) / 2;
}

inline bool addition_exceeds_cap(std::size_t current, std::uint64_t add,
                                 std::size_t cap) {
  if (add >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return true;
  }
  auto add_size = static_cast<std::size_t>(add);
  return current > cap || add_size > cap - current;
}

[[noreturn]] inline void throw_exact_cap(polytomy_event_audit& event,
                                         std::string const& message) {
  event.refused_by_exact_cap = true;
  throw std::runtime_error(message);
}

inline std::vector<taxon_id> union_taxa_for_child_mask(
    clade_grammar const& source, grammar_production const& prod,
    std::uint64_t mask) {
  std::vector<taxon_id> taxa;
  for (std::size_t i = 0; i < prod.children.size(); ++i) {
    if ((mask & (std::uint64_t{1} << i)) == 0) continue;
    auto child = prod.children[i];
    auto const& child_taxa = source.clades[child].taxa;
    taxa.insert(taxa.end(), child_taxa.begin(), child_taxa.end());
  }
  std::sort(taxa.begin(), taxa.end());
  taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
  return taxa;
}

inline bool local_clade_id_key_less(
    std::vector<std::vector<taxon_id>> const& local_taxa_by_clade, clade_id lhs,
    clade_id rhs) {
  if (lhs >= local_taxa_by_clade.size() || rhs >= local_taxa_by_clade.size()) {
    throw std::runtime_error(
        "polytomy refinement: preflight clade id out of range");
  }
  auto const& ltaxa = local_taxa_by_clade[lhs];
  auto const& rtaxa = local_taxa_by_clade[rhs];
  if (ltaxa.size() != rtaxa.size()) return ltaxa.size() < rtaxa.size();
  if (ltaxa != rtaxa) return ltaxa < rtaxa;
  return lhs < rhs;
}

inline void preflight_exact_kary_expansion(exact_expansion_context const& ctx,
                                           grammar_production const& prod,
                                           polytomy_event_audit& event) {
  bool theoretical_saturated = false;
  auto theoretical_clades =
      exact_subset_clade_upper_bound(event.arity, theoretical_saturated);
  auto theoretical_productions =
      exact_subset_production_count(event.arity, theoretical_saturated);
  if (theoretical_saturated) {
    throw_exact_cap(
        event,
        "polytomy refinement: exact expansion theoretical size saturated");
  }

  // Cheap lower-bound rejection before subset/split enumeration.  Existing
  // draft clades/productions can absorb at most their current counts, so these
  // checks catch obviously impossible user-raised caps without mutating the
  // returned grammar or enumerating an enormous closure.
  auto clade_reuse_capacity = std::min<std::uint64_t>(
      theoretical_clades, static_cast<std::uint64_t>(ctx.clade_by_taxa.size()));
  auto production_reuse_capacity = std::min<std::uint64_t>(
      theoretical_productions,
      static_cast<std::uint64_t>(ctx.production_by_key.size()));
  auto min_new_clades = theoretical_clades - clade_reuse_capacity;
  auto min_new_productions =
      theoretical_productions - production_reuse_capacity;
  if (addition_exceeds_cap(event.new_clades_added, min_new_clades,
                           ctx.options.max_new_clades_per_polytomy) ||
      addition_exceeds_cap(ctx.total_new_clades, min_new_clades,
                           ctx.options.max_total_new_clades)) {
    throw_exact_cap(
        event, "polytomy refinement: exact expansion exceeds new-clade cap");
  }
  if (addition_exceeds_cap(event.new_productions_added, min_new_productions,
                           ctx.options.max_new_productions_per_polytomy) ||
      addition_exceeds_cap(ctx.total_new_productions, min_new_productions,
                           ctx.options.max_total_new_productions)) {
    throw_exact_cap(
        event,
        "polytomy refinement: exact expansion exceeds new-production cap");
  }

  auto full = (std::uint64_t{1} << event.arity) - 1;
  std::map<std::vector<taxon_id>, clade_id> local_clade_by_taxa =
      ctx.clade_by_taxa;
  std::vector<std::vector<taxon_id>> local_taxa_by_clade;
  local_taxa_by_clade.reserve(ctx.draft.clades.size());
  for (auto const& clade : ctx.draft.clades)
    local_taxa_by_clade.push_back(clade.taxa);

  std::map<std::uint64_t, clade_id> mask_to_clade;
  for (std::size_t i = 0; i < event.arity; ++i) {
    auto child = prod.children[i];
    if (child == no_clade || child >= ctx.source_clade_to_draft.size()) {
      throw std::runtime_error(
          "polytomy refinement: k-ary child clade out of range");
    }
    mask_to_clade.emplace(std::uint64_t{1} << i,
                          ctx.source_clade_to_draft[child]);
  }

  std::size_t exact_new_clades = 0;
  for (std::uint64_t mask = 1; mask <= full; ++mask) {
    if (std::popcount(mask) < 2) continue;
    auto taxa = union_taxa_for_child_mask(ctx.source, prod, mask);
    auto found = local_clade_by_taxa.find(taxa);
    clade_id cid = no_clade;
    if (found != local_clade_by_taxa.end()) {
      cid = found->second;
    } else {
      if (mask == full) {
        throw std::runtime_error(
            "polytomy refinement: full subset did not resolve to parent "
            "clade during preflight");
      }
      if (local_taxa_by_clade.size() >= static_cast<std::size_t>(no_clade)) {
        throw std::runtime_error(
            "polytomy refinement: too many refined clades during preflight");
      }
      cid = static_cast<clade_id>(local_taxa_by_clade.size());
      local_clade_by_taxa.emplace(taxa, cid);
      local_taxa_by_clade.push_back(std::move(taxa));
      ++exact_new_clades;
      if (addition_exceeds_cap(event.new_clades_added, exact_new_clades,
                               ctx.options.max_new_clades_per_polytomy) ||
          addition_exceeds_cap(ctx.total_new_clades, exact_new_clades,
                               ctx.options.max_total_new_clades)) {
        throw_exact_cap(
            event,
            "polytomy refinement: exact expansion exceeds new-clade cap");
      }
    }
    mask_to_clade[mask] = cid;
  }

  std::map<detail::production_map_key, bool> local_new_productions;
  std::size_t exact_new_productions = 0;
  for (std::uint64_t mask = 1; mask <= full; ++mask) {
    if (std::popcount(mask) < 2) continue;
    auto parent = mask_to_clade.at(mask);
    auto lowest = mask & (~mask + 1);
    for (auto sub = (mask - 1) & mask; sub != 0; sub = (sub - 1) & mask) {
      if ((sub & lowest) == 0) continue;
      auto other = mask ^ sub;
      if (other == 0) continue;

      std::vector<clade_id> children{mask_to_clade.at(sub),
                                     mask_to_clade.at(other)};
      if (ctx.options.canonicalize_binary_children) {
        std::stable_sort(
            children.begin(), children.end(), [&](clade_id lhs, clade_id rhs) {
              return local_clade_id_key_less(local_taxa_by_clade, lhs, rhs);
            });
      }

      detail::production_map_key key{parent, std::move(children)};
      if (ctx.production_by_key.find(key) != ctx.production_by_key.end())
        continue;
      if (!local_new_productions.emplace(std::move(key), true).second) continue;
      ++exact_new_productions;
      if (addition_exceeds_cap(event.new_productions_added,
                               exact_new_productions,
                               ctx.options.max_new_productions_per_polytomy) ||
          addition_exceeds_cap(ctx.total_new_productions, exact_new_productions,
                               ctx.options.max_total_new_productions)) {
        throw_exact_cap(event,
                        "polytomy refinement: exact expansion exceeds "
                        "new-production cap");
      }
    }
  }
}

inline void copy_observed_binary_productions(exact_expansion_context& ctx) {
  for (std::size_t i = 0; i < ctx.source.productions.size(); ++i) {
    auto pid = static_cast<production_id>(i);
    auto const& source_prod = ctx.source.productions[i];
    if (source_prod.children.size() != 2) continue;

    auto candidate = ctx.remap_observed_production(source_prod);
    auto [draft_pid, _] = ctx.add_production(
        std::move(candidate), refined_production_origin::observed_binary, {pid},
        production_parent_nodes(source_prod), nullptr);
    ctx.source_production_to_draft[i] = draft_pid;
  }
}

inline polytomy_event_audit expand_kary_production_exact(
    exact_expansion_context& ctx, production_id source_pid) {
  auto const& source = ctx.source;
  if (source_pid == no_production || source_pid >= source.productions.size()) {
    throw std::runtime_error(
        "polytomy refinement: source k-ary production out of range");
  }
  auto const& prod = source.productions[source_pid];

  polytomy_event_audit event;
  event.source_production = source_pid;
  event.parent = ctx.source_clade_to_draft.at(prod.parent);
  event.arity = prod.children.size();
  event.source_multiplicity = prod.multiplicity;
  event.expanded = true;
  event.exact = true;

  if (event.arity < 3) {
    throw std::runtime_error(
        "polytomy refinement: exact expansion expected arity >= 3");
  }
  if (ctx.options.expand_child_group_alternatives) {
    throw std::runtime_error(
        "polytomy refinement: child-group alternative expansion is not "
        "implemented");
  }
  if (event.arity > ctx.options.max_exact_arity || event.arity > 63) {
    event.refused_by_exact_cap = true;
    throw std::runtime_error(
        "polytomy refinement: exact expansion exceeds arity cap");
  }
  if (!production_children_partition_parent(source, prod)) {
    throw std::runtime_error(
        "polytomy refinement: k-ary production children do not partition "
        "the parent clade");
  }

  preflight_exact_kary_expansion(ctx, prod, event);

  auto source_parent_nodes = production_parent_nodes(prod);
  auto full = (std::uint64_t{1} << event.arity) - 1;
  std::map<std::uint64_t, clade_id> mask_to_clade;
  for (std::size_t i = 0; i < event.arity; ++i) {
    auto child = prod.children[i];
    if (child == no_clade || child >= ctx.source_clade_to_draft.size()) {
      throw std::runtime_error(
          "polytomy refinement: k-ary child clade out of range");
    }
    mask_to_clade.emplace(std::uint64_t{1} << i,
                          ctx.source_clade_to_draft[child]);
  }

  for (std::uint64_t mask = 1; mask <= full; ++mask) {
    if (std::popcount(mask) < 2) continue;
    auto taxa = union_taxa_for_child_mask(source, prod, mask);
    auto origin = mask == full
                      ? refined_clade_origin::observed_input
                      : refined_clade_origin::synthetic_polytomy_intermediate;
    auto [cid, _] = ctx.ensure_refined_clade(
        std::move(taxa), origin, source_pid, source_parent_nodes, &event);
    mask_to_clade[mask] = cid;
  }

  for (std::uint64_t mask = 1; mask <= full; ++mask) {
    if (std::popcount(mask) < 2) continue;
    auto parent = mask_to_clade.at(mask);
    auto lowest = mask & (~mask + 1);
    for (auto sub = (mask - 1) & mask; sub != 0; sub = (sub - 1) & mask) {
      if ((sub & lowest) == 0) continue;
      auto other = mask ^ sub;
      if (other == 0) continue;

      grammar_production candidate;
      candidate.parent = parent;
      candidate.children = {mask_to_clade.at(sub), mask_to_clade.at(other)};
      candidate.multiplicity = 0;

      (void)ctx.add_production(
          std::move(candidate),
          refined_production_origin::synthetic_polytomy_refinement,
          {source_pid}, source_parent_nodes, &event);
    }
  }

  bool saturated = false;
  event.represented_refinement_count =
      represented_refinement_count_for_exact_grammar(event.arity, saturated);
  event.refinement_count_saturated = saturated;
  return event;
}

inline void sort_unique_provenance(polytomy_refinement_result& result) {
  for (auto& info : result.clade_info) {
    sort_unique(info.source_kary_productions);
    sort_unique(info.source_parent_nodes);
  }
  for (auto& info : result.production_info) {
    sort_unique(info.source_productions);
    sort_unique(info.source_parent_nodes);
  }
}

inline void validate_expanded_public_maps(polytomy_refinement_result const& r,
                                          clade_grammar const& source) {
  for (auto cid : r.source_clade_to_refined) {
    if (cid == no_clade || cid >= r.grammar.clades.size()) {
      throw std::runtime_error(
          "polytomy refinement: invalid source_clade_to_refined entry");
    }
  }
  if (r.source_production_to_refined.size() != source.productions.size()) {
    throw std::runtime_error(
        "polytomy refinement: source production map size mismatch");
  }
  for (std::size_t i = 0; i < r.source_production_to_refined.size(); ++i) {
    auto pid = r.source_production_to_refined[i];
    bool source_was_expanded_kary = source.productions[i].children.size() > 2;
    if (source_was_expanded_kary) {
      if (pid != no_production) {
        throw std::runtime_error(
            "polytomy refinement: expanded k-ary production unexpectedly has "
            "a one-to-one production map");
      }
      continue;
    }
    if (pid == no_production || pid >= r.grammar.productions.size()) {
      throw std::runtime_error(
          "polytomy refinement: invalid source_production_to_refined entry");
    }
  }
}

inline polytomy_refinement_result finalize_exact_expansion(
    clade_grammar_build_result&& built, exact_expansion_context& ctx,
    std::vector<polytomy_event_audit> events) {
  polytomy_refinement_result result;
  result.source_grammar_audit = std::move(built.audit);
  auto const& source = ctx.source;

  std::vector<clade_id> draft_to_final(ctx.draft.clades.size(), no_clade);
  std::vector<clade_id> order;
  order.reserve(ctx.draft.clades.size());
  for (std::size_t cid = 0; cid < ctx.draft.clades.size(); ++cid)
    order.push_back(static_cast<clade_id>(cid));
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    return clade_id_key_less(ctx.draft, lhs, rhs);
  });

  result.grammar.taxa = ctx.draft.taxa;
  result.grammar.clades.resize(ctx.draft.clades.size());
  result.clade_info.resize(ctx.draft.clades.size());
  for (std::size_t new_i = 0; new_i < order.size(); ++new_i) {
    auto old = order[new_i];
    draft_to_final[old] = static_cast<clade_id>(new_i);
    result.grammar.clades[new_i] = ctx.draft.clades[old];
    result.clade_info[new_i] = std::move(ctx.clade_info[old]);
  }

  result.source_clade_to_refined.resize(source.clades.size(), no_clade);
  for (std::size_t cid = 0; cid < source.clades.size(); ++cid) {
    auto draft_cid = ctx.source_clade_to_draft[cid];
    result.source_clade_to_refined[cid] = draft_to_final[draft_cid];
  }

  result.grammar.node_to_clade.assign(source.node_to_clade.size(), no_clade);
  for (std::size_t node = 0; node < source.node_to_clade.size(); ++node) {
    auto source_cid = source.node_to_clade[node];
    if (source_cid == no_clade) continue;
    result.grammar.node_to_clade[node] =
        result.source_clade_to_refined[source_cid];
  }
  result.grammar.root_clade = result.source_clade_to_refined[source.root_clade];

  std::map<detail::production_map_key, std::size_t> final_order;
  std::vector<grammar_production> remapped_productions(ctx.productions.size());
  std::vector<refined_production_info> remapped_info(ctx.productions.size());
  for (std::size_t old_pid = 0; old_pid < ctx.productions.size(); ++old_pid) {
    auto const& record = ctx.productions[old_pid];
    auto prod = record.prod;
    prod.parent = draft_to_final[prod.parent];
    for (auto& child : prod.children) child = draft_to_final[child];
    for (auto& witness : prod.witnesses) {
      for (auto& child_witness : witness.children)
        child_witness.child = draft_to_final[child_witness.child];
    }

    detail::production_map_key key{prod.parent, prod.children};
    auto [_, inserted] = final_order.emplace(std::move(key), old_pid);
    if (!inserted) {
      throw std::runtime_error(
          "polytomy refinement: duplicate production after final remap");
    }
    remapped_productions[old_pid] = std::move(prod);
    remapped_info[old_pid] = record.info;
  }

  std::vector<production_id> draft_production_to_final(ctx.productions.size(),
                                                       no_production);
  result.grammar.productions.reserve(ctx.productions.size());
  result.production_info.reserve(ctx.productions.size());
  for (auto const& [_, old_pid] : final_order) {
    auto final_pid =
        static_cast<production_id>(result.grammar.productions.size());
    draft_production_to_final[old_pid] = final_pid;
    result.grammar.productions.push_back(
        std::move(remapped_productions[old_pid]));
    result.production_info.push_back(std::move(remapped_info[old_pid]));
  }

  result.source_production_to_refined.resize(source.productions.size(),
                                             no_production);
  for (std::size_t pid = 0; pid < source.productions.size(); ++pid) {
    auto draft_pid = ctx.source_production_to_draft[pid];
    if (draft_pid != no_production)
      result.source_production_to_refined[pid] =
          draft_production_to_final[draft_pid];
  }

  result.grammar.productions_by_parent.assign(result.grammar.clades.size(), {});
  result.grammar.productions_by_child.assign(result.grammar.clades.size(), {});
  for (std::size_t i = 0; i < result.grammar.productions.size(); ++i) {
    auto pid = static_cast<production_id>(i);
    auto const& prod = result.grammar.productions[i];
    result.grammar.productions_by_parent[prod.parent].push_back(pid);
    auto unique_children = prod.children;
    std::sort(unique_children.begin(), unique_children.end());
    unique_children.erase(
        std::unique(unique_children.begin(), unique_children.end()),
        unique_children.end());
    for (auto child : unique_children)
      result.grammar.productions_by_child[child].push_back(pid);
  }

  for (auto& event : events) event.parent = draft_to_final[event.parent];
  std::sort(
      events.begin(), events.end(),
      [](polytomy_event_audit const& lhs, polytomy_event_audit const& rhs) {
        return lhs.source_production < rhs.source_production;
      });
  result.audit.events = std::move(events);

  result.audit.source_clade_count = source.clades.size();
  result.audit.source_production_count = source.productions.size();
  result.audit.source_kary_production_count = kary_productions(source).size();
  result.audit.refined_clade_count = result.grammar.clades.size();
  result.audit.refined_production_count = result.grammar.productions.size();
  result.audit.synthetic_clade_count = ctx.total_new_clades;
  result.audit.synthetic_production_count = ctx.total_new_productions;
  result.audit.contains_kary_productions =
      grammar_has_kary_productions(result.grammar);
  result.audit.binary_chart_compatible =
      grammar_is_binary_chart_compatible(result.grammar);
  result.audit.exact_for_soft_polytomies =
      !result.audit.contains_kary_productions &&
      result.audit.binary_chart_compatible;
  result.audit.any_truncated = false;
  result.audit.any_refused = false;

  sort_unique_provenance(result);
  validate_expanded_public_maps(result, source);
  if (!result.audit.binary_chart_compatible) {
    throw std::runtime_error(
        "polytomy refinement: exact expansion did not produce a binary "
        "chart-compatible grammar");
  }
  return result;
}

inline polytomy_refinement_result make_exact_expansion_result(
    clade_grammar_build_result&& built,
    polytomy_refinement_options const& refinement_opts) {
  exact_expansion_context ctx{built.grammar, refinement_opts};
  copy_observed_binary_productions(ctx);

  std::vector<polytomy_event_audit> events;
  for (auto pid : kary_productions(ctx.source))
    events.push_back(expand_kary_production_exact(ctx, pid));

  return finalize_exact_expansion(std::move(built), ctx, std::move(events));
}

}  // namespace polytomy_refinement_detail

inline bool grammar_is_binary_chart_compatible(clade_grammar const& grammar) {
  if (grammar.clades.size() >= static_cast<std::size_t>(no_clade)) return false;
  if (grammar.productions.size() >= static_cast<std::size_t>(no_production))
    return false;
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    return false;
  }
  if (grammar.productions_by_parent.size() != grammar.clades.size())
    return false;
  if (grammar.productions_by_child.size() != grammar.clades.size())
    return false;

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto const& clade = grammar.clades[cid];
    if (!polytomy_refinement_detail::sorted_unique_taxa(clade.taxa))
      return false;
    for (auto taxon : clade.taxa) {
      if (taxon >= grammar.taxa.id_to_sample_id.size()) return false;
    }
    if (clade.taxa.size() == 1) {
      if (!grammar.productions_by_parent[cid].empty()) return false;
    } else if (grammar.productions_by_parent[cid].empty()) {
      return false;
    }
  }

  for (std::size_t pid_i = 0; pid_i < grammar.productions.size(); ++pid_i) {
    auto pid = static_cast<production_id>(pid_i);
    auto const& prod = grammar.productions[pid_i];
    if (prod.parent == no_clade || prod.parent >= grammar.clades.size())
      return false;
    if (prod.children.size() != 2) return false;
    auto const& by_parent = grammar.productions_by_parent[prod.parent];
    if (std::find(by_parent.begin(), by_parent.end(), pid) == by_parent.end())
      return false;
    for (auto child : prod.children) {
      if (child == no_clade || child >= grammar.clades.size()) return false;
      if (grammar.clades[child].taxa.size() >=
          grammar.clades[prod.parent].taxa.size()) {
        return false;
      }
      auto const& by_child = grammar.productions_by_child[child];
      if (std::find(by_child.begin(), by_child.end(), pid) == by_child.end())
        return false;
    }
    if (!polytomy_refinement_detail::production_children_partition_parent(
            grammar, prod)) {
      return false;
    }
  }

  for (std::size_t cid = 0; cid < grammar.productions_by_parent.size(); ++cid) {
    for (auto pid : grammar.productions_by_parent[cid]) {
      if (pid == no_production || pid >= grammar.productions.size())
        return false;
      if (grammar.productions[pid].parent != cid) return false;
    }
  }
  for (std::size_t cid = 0; cid < grammar.productions_by_child.size(); ++cid) {
    for (auto pid : grammar.productions_by_child[cid]) {
      if (pid == no_production || pid >= grammar.productions.size())
        return false;
      auto const& children = grammar.productions[pid].children;
      if (std::find(children.begin(), children.end(),
                    static_cast<clade_id>(cid)) == children.end()) {
        return false;
      }
    }
  }

  return true;
}

inline polytomy_refinement_result build_polytomy_refined_clade_grammar(
    phylo_dag& dag, clade_grammar_options grammar_opts = {},
    polytomy_refinement_options refinement_opts = {}) {
  switch (refinement_opts.mode) {
    case polytomy_mode::reject: {
      grammar_opts.allow_polytomies = false;
      auto built = build_clade_grammar_with_audit(dag, grammar_opts);
      return polytomy_refinement_detail::make_phase0_result(
          std::move(built), refinement_opts.mode);
    }
    case polytomy_mode::audit_kary: {
      grammar_opts.allow_polytomies = true;
      auto built = build_clade_grammar_with_audit(dag, grammar_opts);
      return polytomy_refinement_detail::make_phase0_result(
          std::move(built), refinement_opts.mode);
    }
    case polytomy_mode::expand_soft_exact_or_fail: {
      grammar_opts.allow_polytomies = true;
      auto built = build_clade_grammar_with_audit(dag, grammar_opts);
      if (!grammar_has_kary_productions(built.grammar)) {
        return polytomy_refinement_detail::make_phase0_result(
            std::move(built), refinement_opts.mode);
      }
      return polytomy_refinement_detail::make_exact_expansion_result(
          std::move(built), refinement_opts);
    }
    case polytomy_mode::expand_soft_bounded: {
      grammar_opts.allow_polytomies = true;
      auto built = build_clade_grammar_with_audit(dag, grammar_opts);
      if (!grammar_has_kary_productions(built.grammar)) {
        return polytomy_refinement_detail::make_phase0_result(
            std::move(built), refinement_opts.mode);
      }
      throw std::runtime_error(
          "polytomy refinement: bounded soft-polytomy expansion is not "
          "implemented yet; use expand_soft_exact_or_fail for exact "
          "subset-closure expansion of small polytomies");
    }
  }

  throw std::runtime_error("polytomy refinement: unknown polytomy mode");
}

inline std::ostream& print_polytomy_refinement_audit(
    std::ostream& out, polytomy_refinement_audit const& audit) {
  out << "polytomy_refinement:\n";
  out << "  source_clades: " << audit.source_clade_count << "\n";
  out << "  source_productions: " << audit.source_production_count << "\n";
  out << "  source_kary_productions: " << audit.source_kary_production_count
      << "\n";
  out << "  refined_clades: " << audit.refined_clade_count << "\n";
  out << "  refined_productions: " << audit.refined_production_count << "\n";
  out << "  synthetic_clades: " << audit.synthetic_clade_count << "\n";
  out << "  synthetic_productions: " << audit.synthetic_production_count
      << "\n";
  out << "  contains_kary_productions: "
      << (audit.contains_kary_productions ? "true" : "false") << "\n";
  out << "  binary_chart_compatible: "
      << (audit.binary_chart_compatible ? "true" : "false") << "\n";
  out << "  exact_for_soft_polytomies: "
      << (audit.exact_for_soft_polytomies ? "true" : "false") << "\n";
  out << "  any_truncated: " << (audit.any_truncated ? "true" : "false")
      << "\n";
  out << "  any_refused: " << (audit.any_refused ? "true" : "false") << "\n";
  out << "  events: " << audit.events.size() << "\n";
  return out;
}

}  // namespace larch

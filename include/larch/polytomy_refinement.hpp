#pragma once

#include <larch/clade_grammar.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
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

  // Exact expansion is implemented in later multifurcation phases.  The caps
  // live in the Phase-0 API so callers can configure and persist one option
  // object before expansion support is enabled.
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

  // Identity maps in Phase 0.  Later expansion modes may set an expanded k-ary
  // source production to no_production because it has no one-to-one copy.
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
  return std::any_of(grammar.productions.begin(), grammar.productions.end(),
                     [](grammar_production const& prod) {
                       return prod.children.size() > 2;
                     });
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
  result.audit.binary_chart_compatible = grammar_is_binary_chart_compatible(grammar);
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
            [](polytomy_event_audit const& a,
               polytomy_event_audit const& b) {
              return a.source_production < b.source_production;
            });

  validate_phase0_public_maps(result);
  return result;
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
    case polytomy_mode::expand_soft_exact_or_fail:
    case polytomy_mode::expand_soft_bounded: {
      grammar_opts.allow_polytomies = true;
      auto built = build_clade_grammar_with_audit(dag, grammar_opts);
      if (!grammar_has_kary_productions(built.grammar)) {
        return polytomy_refinement_detail::make_phase0_result(
            std::move(built), refinement_opts.mode);
      }
      throw std::runtime_error(
          "polytomy refinement: soft-polytomy expansion is not implemented in "
          "MF Phase 0; use polytomy_mode::audit_kary for diagnostic k-ary "
          "grammar audits");
    }
  }

  throw std::runtime_error("polytomy refinement: unknown polytomy mode");
}

inline std::ostream& print_polytomy_refinement_audit(
    std::ostream& out, polytomy_refinement_audit const& audit) {
  out << "polytomy_refinement:\n";
  out << "  source_clades: " << audit.source_clade_count << "\n";
  out << "  source_productions: " << audit.source_production_count << "\n";
  out << "  source_kary_productions: "
      << audit.source_kary_production_count << "\n";
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
  out << "  any_refused: " << (audit.any_refused ? "true" : "false")
      << "\n";
  out << "  events: " << audit.events.size() << "\n";
  return out;
}

}  // namespace larch

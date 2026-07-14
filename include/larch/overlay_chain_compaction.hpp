#pragma once

// Phase 5: grammar-valued compaction for the DAG-native overlay chain.
//
// The Phase-4 local-commit path kept the live search state as a frozen base
// grammar plus an append-only overlay chain, but final compaction still chose a
// single concrete tree and checked that tree's score.  This header provides the
// Phase-5 helper layer: materialize the whole chain once, emit a multi-tree DAG
// whose witnesses include every accepted overlay production key and every
// supplied accepted topology certificate, and compute the output DAG's
// grammar-level exact parsimony via the B&B trim.

#include <larch/overlay_chain.hpp>
#include <larch/rank3_rewrite.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace larch {

struct overlay_chain_compaction_options {
  bool validate = true;
  float generated_edge_weight = std::numeric_limits<float>::max();
  clade_grammar_options rebuild_grammar_options = {};

  // Concrete dense-chain topologies that must be represented in the compacted
  // DAG.  The search loop supplies the chain's exact score witness here (B&B
  // optimum for exact_multisite, selected certificate for fixed_topology_exact)
  // so the grammar-level output optimum is score-safe while the helper also
  // adds per-production witness topologies for every accepted temp production.
  std::vector<rank3_topology> witness_topologies;

  // Optional complete topology witnesses expressed as stable production taxon
  // keys.  Standalone compaction derives prefix topology witnesses for every
  // accepted intended production on its own; callers can supply extra key-sets
  // here when they also need to preserve a particular complete accepted
  // topology (for example the exact topology selected by an acceptance gate).
  // Phase-5 compaction augments the topology-materialization grammar with any
  // missing witness productions and then verifies the rebuilt output DAG still
  // represents each complete key-set topology.
  std::vector<std::vector<rank3_production_taxa_key>> witness_topology_key_sets;
};

struct overlay_chain_compaction_result {
  overlay_materialization_result materialized;
  // Wall time of the one dense materialize_overlay_chain() call below.  This
  // excludes witness derivation, DAG merging, grammar rebuilding, and exact
  // final validation performed by the rest of compaction.
  double materialization_ms = 0.0;
  phylo_dag dag;
  clade_grammar_build_result rebuilt;
  std::vector<rank3_topology> materialized_topologies;
  std::vector<rank3_production_taxa_key> intended_productions;
  std::vector<bool> intended_production_present;
  // Complete production-key sets for every concrete topology merged into the
  // compacted output DAG (caller-provided accepted witnesses plus the
  // deterministic per-production witnesses).  These are checked after the DAG
  // rebuild, not just before materialization, so compaction preserves complete
  // witness topologies rather than merely preserving their individual
  // production keys somewhere in the output grammar.
  std::vector<std::vector<rank3_production_taxa_key>> witness_topology_keys;
  std::vector<bool> witness_topology_present;
  std::size_t materialized_tree_count = 0;

  [[nodiscard]] bool all_intended_productions_present() const {
    return std::all_of(intended_production_present.begin(),
                       intended_production_present.end(),
                       [](bool present) { return present; });
  }

  [[nodiscard]] bool all_witness_topologies_present() const {
    return std::all_of(witness_topology_present.begin(),
                       witness_topology_present.end(),
                       [](bool present) { return present; });
  }
};

struct grammar_level_exact_parsimony_result {
  std::uint64_t active_value = multisite_score_inf;
  std::uint64_t value = multisite_score_inf;
  multisite_keep_mask_kind exactness_kind = multisite_keep_mask_kind::none;
  bool keep_production_exact = false;
  multisite_trim_result trim;
};

namespace overlay_chain_compaction_detail {

inline rank3_production_taxa_key production_key_from_overlay_production(
    overlay_clade_grammar const& overlay,
    overlay_grammar_production const& prod, std::string const& context) {
  rank3_production_taxa_key key;
  key.parent = rank3_detail::overlay_taxa_for_ref(
      overlay, prod.parent, context + " parent");
  key.children.reserve(prod.children.size());
  for (auto child : prod.children) {
    key.children.push_back(rank3_detail::overlay_taxa_for_ref(
        overlay, child, context + " child"));
  }
  rank3_detail::normalize_production_key(key);
  return key;
}

inline rank3_production_taxa_key production_key_from_overlay_temp_production(
    overlay_clade_grammar const& overlay, std::size_t temp_pid) {
  if (temp_pid >= overlay.temp_productions.size()) {
    throw std::runtime_error(
        "overlay-chain compaction: temp production out of range");
  }
  return production_key_from_overlay_production(
      overlay, overlay.temp_productions[temp_pid],
      "overlay-chain compaction temp production");
}

inline production_id find_dense_production_by_key(
    clade_grammar const& grammar, rank3_production_taxa_key key,
    std::string const& context) {
  rank3_detail::normalize_production_key(key);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto dense_pid = static_cast<production_id>(pid);
    if (rank3_detail::production_key_from_id(grammar, dense_pid) == key) {
      return dense_pid;
    }
  }
  throw std::runtime_error(
      context + ": intended production key is absent from dense grammar: " +
      rank3_detail::production_key_to_string(key));
}

inline clade_id find_clade_by_taxa(clade_grammar const& grammar,
                                   std::vector<taxon_id> taxa) {
  std::sort(taxa.begin(), taxa.end());
  taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto existing = grammar.clades[cid].taxa;
    std::sort(existing.begin(), existing.end());
    existing.erase(std::unique(existing.begin(), existing.end()),
                   existing.end());
    if (existing == taxa) return static_cast<clade_id>(cid);
  }
  return no_clade;
}

inline clade_id ensure_clade_by_taxa(clade_grammar& grammar,
                                     std::vector<taxon_id> taxa,
                                     std::string const& context) {
  std::sort(taxa.begin(), taxa.end());
  taxa.erase(std::unique(taxa.begin(), taxa.end()), taxa.end());
  if (taxa.empty()) {
    throw std::runtime_error(context + ": empty clade taxon set");
  }
  if (auto existing = find_clade_by_taxa(grammar, taxa); existing != no_clade) {
    return existing;
  }
  if (grammar.clades.size() >= static_cast<std::size_t>(no_clade)) {
    throw std::runtime_error(context + ": clade id space exhausted");
  }
  auto cid = static_cast<clade_id>(grammar.clades.size());
  grammar.clades.push_back(clade_key{std::move(taxa)});
  grammar.productions_by_parent.resize(grammar.clades.size());
  grammar.productions_by_child.resize(grammar.clades.size());
  return cid;
}

inline production_id find_production_by_key_or_none(
    clade_grammar const& grammar, rank3_production_taxa_key key) {
  rank3_detail::normalize_production_key(key);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto dense_pid = static_cast<production_id>(pid);
    if (rank3_detail::production_key_from_id(grammar, dense_pid) == key) {
      return dense_pid;
    }
  }
  return no_production;
}

inline production_id ensure_production_by_key(
    clade_grammar& grammar, rank3_production_taxa_key key,
    std::string const& context) {
  rank3_detail::normalize_production_key(key);
  if (key.children.size() < 2) {
    throw std::runtime_error(
        context +
        ": witness topology production has fewer than 2 children: " +
        rank3_detail::production_key_to_string(key));
  }
  if (auto existing = find_production_by_key_or_none(grammar, key);
      existing != no_production) {
    return existing;
  }
  auto parent = ensure_clade_by_taxa(grammar, key.parent, context);
  std::vector<clade_id> children;
  children.reserve(key.children.size());
  for (auto child_taxa : key.children) {
    children.push_back(ensure_clade_by_taxa(grammar, std::move(child_taxa),
                                            context));
  }
  if (grammar.productions.size() >= static_cast<std::size_t>(no_production)) {
    throw std::runtime_error(context + ": production id space exhausted");
  }
  grammar_production prod;
  prod.parent = parent;
  prod.children = std::move(children);
  auto pid = static_cast<production_id>(grammar.productions.size());
  grammar.productions.push_back(std::move(prod));
  grammar.productions_by_parent[parent].push_back(pid);
  for (auto child : grammar.productions[pid].children) {
    grammar.productions_by_child[child].push_back(pid);
  }
  return pid;
}

inline clade_grammar augment_grammar_with_topology_keys(
    clade_grammar grammar,
    std::vector<std::vector<rank3_production_taxa_key>> const& topology_keys) {
  for (std::size_t i = 0; i < topology_keys.size(); ++i) {
    for (auto key : topology_keys[i]) {
      (void)ensure_production_by_key(
          grammar, std::move(key),
          "overlay-chain compaction: augmenting grammar for required witness "
          "topology " +
              std::to_string(i));
    }
  }
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  grammar.execution_generation =
      detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

inline void append_unique_topology(std::vector<rank3_topology>& topologies,
                                   clade_grammar const& grammar,
                                   rank3_topology topology) {
  (void)validate_grammar_topology(grammar, topology);
  auto it = std::find_if(topologies.begin(), topologies.end(),
                         [&](rank3_topology const& existing) {
                           return grammar_topology_equal(existing, topology);
                         });
  if (it == topologies.end()) topologies.push_back(std::move(topology));
}

inline void select_topology_production(rank3_topology& topology,
                                       clade_grammar const& grammar,
                                       production_id pid,
                                       std::string const& context) {
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error(context + ": production out of range");
  }
  auto parent = grammar.productions[pid].parent;
  if (parent == no_clade || parent >= grammar.clades.size()) {
    throw std::runtime_error(context + ": production parent out of range");
  }
  auto& selected = topology.selected_production_by_clade[parent];
  if (selected != no_production && selected != pid) {
    throw std::runtime_error(
        context + ": conflicting concrete-topology choices for clade " +
        std::to_string(parent));
  }
  selected = pid;
  topology.used_production[pid] = true;
}

inline bool find_ancestor_path_to_clade(
    clade_grammar const& grammar, clade_id clade, clade_id target,
    std::vector<std::pair<clade_id, production_id>>& reversed_path,
    std::vector<std::uint8_t>& state) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error(
        "overlay-chain compaction: ancestor-path clade out of range");
  }
  if (clade == target) return true;
  if (state[clade] == 1) {
    throw std::runtime_error(
        "overlay-chain compaction: cycle while finding ancestor path for "
        "production witness topology");
  }
  if (state[clade] == 2) return false;
  state[clade] = 1;

  for (auto pid : grammar.productions_by_parent[clade]) {
    if (pid == no_production || pid >= grammar.productions.size()) {
      throw std::runtime_error(
          "overlay-chain compaction: ancestor-path production out of range");
    }
    auto const& prod = grammar.productions[pid];
    if (prod.parent != clade) {
      throw std::runtime_error(
          "overlay-chain compaction: ancestor-path production parent "
          "mismatch");
    }
    for (auto child : prod.children) {
      if (find_ancestor_path_to_clade(grammar, child, target, reversed_path,
                                      state)) {
        reversed_path.push_back({clade, pid});
        state[clade] = 2;
        return true;
      }
    }
  }

  state[clade] = 2;
  return false;
}

inline void fill_concrete_topology_from_selected_choices(
    clade_grammar const& grammar, rank3_topology& topology, clade_id clade,
    std::vector<std::uint8_t>& state) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    throw std::runtime_error(
        "overlay-chain compaction: concrete-topology clade out of range");
  }
  if (state[clade] == 1) {
    throw std::runtime_error(
        "overlay-chain compaction: cycle while filling production witness "
        "topology");
  }
  if (state[clade] == 2) {
    throw std::runtime_error(
        "overlay-chain compaction: selected production witness topology "
        "reuses a clade");
  }
  state[clade] = 1;

  if (grammar.clades[clade].taxa.size() != 1) {
    auto chosen = topology.selected_production_by_clade[clade];
    if (chosen == no_production) {
      auto const& productions = grammar.productions_by_parent[clade];
      if (productions.empty()) {
        throw std::runtime_error(
            "overlay-chain compaction: non-singleton clade has no "
            "production while filling witness topology");
      }
      chosen = productions.front();
    }
    if (grammar.productions[chosen].parent != clade) {
      throw std::runtime_error(
          "overlay-chain compaction: selected witness production parent "
          "mismatch while filling topology");
    }
    select_topology_production(topology, grammar, chosen,
                               "overlay-chain compaction");
    for (auto child : grammar.productions[chosen].children) {
      fill_concrete_topology_from_selected_choices(grammar, topology, child,
                                                   state);
    }
  }

  state[clade] = 2;
}

inline rank3_topology concrete_topology_containing_production(
    clade_grammar const& grammar, production_id required_pid) {
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  if (grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    throw std::runtime_error(
        "overlay-chain compaction: root clade out of range");
  }
  if (required_pid == no_production ||
      required_pid >= grammar.productions.size()) {
    throw std::runtime_error(
        "overlay-chain compaction: required production out of range");
  }

  auto target_parent = grammar.productions[required_pid].parent;
  std::vector<std::pair<clade_id, production_id>> ancestor_path;
  if (target_parent != grammar.root_clade) {
    std::vector<std::uint8_t> path_state(grammar.clades.size(), 0);
    if (!find_ancestor_path_to_clade(grammar, grammar.root_clade,
                                     target_parent, ancestor_path,
                                     path_state)) {
      throw std::runtime_error(
          "overlay-chain compaction: required production parent clade is not "
          "reachable from the root: production " +
          std::to_string(required_pid));
    }
    std::reverse(ancestor_path.begin(), ancestor_path.end());
  }

  auto topology = make_empty_rank3_topology(grammar);
  for (auto const& [clade, pid] : ancestor_path) {
    (void)clade;
    select_topology_production(topology, grammar, pid,
                               "overlay-chain compaction");
  }
  select_topology_production(topology, grammar, required_pid,
                             "overlay-chain compaction");

  std::vector<std::uint8_t> fill_state(grammar.clades.size(), 0);
  fill_concrete_topology_from_selected_choices(grammar, topology,
                                               grammar.root_clade, fill_state);
  auto reachable = rank3_detail::validate_topology(grammar, topology);
  if (!reachable[required_pid]) {
    throw std::runtime_error(
        "overlay-chain compaction: required production " +
        std::to_string(required_pid) +
        " is not reachable in constructed witness topology");
  }
  return topology;
}

inline std::vector<rank3_production_taxa_key> topology_production_keys(
    clade_grammar const& grammar, rank3_topology const& topology) {
  auto reachable = rank3_detail::validate_topology(grammar, topology);
  std::vector<rank3_production_taxa_key> keys;
  for (std::size_t pid = 0; pid < reachable.size(); ++pid) {
    if (!reachable[pid]) continue;
    rank3_detail::append_unique_key(
        keys, rank3_detail::production_key_from_id(
                  grammar, static_cast<production_id>(pid)));
  }
  return keys;
}

inline std::string topology_key_set_to_string(
    std::vector<rank3_production_taxa_key> const& keys) {
  std::string out{"["};
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != 0) out += ", ";
    out += rank3_detail::production_key_to_string(keys[i]);
  }
  out += "]";
  return out;
}

inline void validate_topology_key_set_present(
    clade_grammar const& grammar,
    std::vector<rank3_production_taxa_key> const& keys,
    std::string const& context) {
  std::vector<production_id> pids;
  pids.reserve(keys.size());
  for (auto key : keys) {
    pids.push_back(find_dense_production_by_key(grammar, std::move(key),
                                                context));
  }
  auto topology = rank3_topology_from_productions(grammar, pids);
  auto reachable = rank3_detail::validate_topology(grammar, topology);
  for (auto pid : pids) {
    if (pid == no_production || pid >= reachable.size() || !reachable[pid]) {
      throw std::runtime_error(
          context + ": witness production is not reachable in the rebuilt "
                    "concrete topology");
    }
  }
}

}  // namespace overlay_chain_compaction_detail

inline multisite_trim_options overlay_chain_compaction_exact_trim_options(
    multisite_trim_options trim_options) {
  trim_options.require_exact_keep_mask = true;
  if (trim_options.dominance_mode == multisite_dominance_mode::score_only) {
    trim_options.dominance_mode = multisite_dominance_mode::off;
  }
  return trim_options;
}

inline multisite_trim_options overlay_chain_compaction_trace_trim_options(
    multisite_trim_options trim_options) {
  trim_options = overlay_chain_compaction_exact_trim_options(trim_options);
  if (trim_options.dominance_mode ==
      multisite_dominance_mode::two_pass_exact_mask) {
    trim_options.dominance_mode = multisite_dominance_mode::off;
  }
  return trim_options;
}

namespace overlay_chain_compaction_detail {

inline overlay_clade_grammar overlay_chain_prefix_tip(
    overlay_chain const& chain, std::size_t prefix_size) {
  if (prefix_size > chain.size()) {
    throw std::runtime_error(
        "overlay-chain compaction: prefix size exceeds chain size");
  }

  overlay_clade_grammar overlay;
  overlay.base = &chain.base();

  for (std::size_t pos = 0; pos < prefix_size; ++pos) {
    auto const& delta = chain.at(pos);
    overlay.temp_clades.insert(overlay.temp_clades.end(),
                               delta.temp_clades.begin(),
                               delta.temp_clades.end());
  }

  std::set<std::pair<overlay_clade_ref, std::vector<overlay_clade_ref>>>
      seen_productions;
  for (std::size_t pos = 0; pos < prefix_size; ++pos) {
    auto const& delta = chain.at(pos);
    for (auto const& prod : delta.temp_productions) {
      auto children_key = prod.children;
      std::sort(children_key.begin(), children_key.end());
      if (seen_productions
              .insert(std::make_pair(prod.parent, children_key))
              .second) {
        overlay.temp_productions.push_back(prod);
      }
    }
  }

  std::set<production_id> tombstones;
  for (std::size_t pos = 0; pos < prefix_size; ++pos) {
    auto const& delta = chain.at(pos);
    for (auto pid : delta.removed_base_productions) tombstones.insert(pid);
  }
  overlay.removed_base_productions.assign(tombstones.begin(),
                                          tombstones.end());
  return overlay;
}

inline overlay_grammar_production overlay_production_shape_for_ref(
    overlay_clade_grammar const& overlay, overlay_production_ref ref,
    std::string const& context) {
  chart_spr_detail::validate_overlay(overlay);
  overlay_grammar_production shape;
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_production || ref.id >= overlay.base->productions.size()) {
      throw std::runtime_error(context + ": base production ref out of range");
    }
    auto const& prod = overlay.base->productions[ref.id];
    shape.parent = base_clade_ref(prod.parent);
    shape.children.reserve(prod.children.size());
    for (auto child : prod.children) {
      shape.children.push_back(base_clade_ref(child));
    }
    shape.witnesses = prod.witnesses;
    shape.multiplicity = prod.multiplicity;
  } else {
    if (ref.id == no_production || ref.id >= overlay.temp_productions.size()) {
      throw std::runtime_error(context + ": temp production ref out of range");
    }
    shape = overlay.temp_productions[ref.id];
  }
  return shape;
}

inline rank3_production_taxa_key overlay_production_key_from_ref(
    overlay_clade_grammar const& overlay, overlay_production_ref ref,
    std::string const& context) {
  if (ref.space == overlay_id_space::base) {
    if (ref.id == no_production || ref.id >= overlay.base->productions.size()) {
      throw std::runtime_error(context + ": base production ref out of range");
    }
    return rank3_detail::production_key_from_id(*overlay.base, ref.id);
  }
  auto shape = overlay_production_shape_for_ref(overlay, ref, context);
  return production_key_from_overlay_production(overlay, shape, context);
}

inline std::vector<overlay_production_ref> active_overlay_productions_by_parent(
    overlay_clade_grammar const& overlay, overlay_clade_ref parent) {
  chart_spr_detail::validate_overlay(overlay);
  std::vector<overlay_production_ref> refs;
  auto removed = chart_spr_detail::normalized_removed_base_productions(overlay);
  if (parent.space == overlay_id_space::base) {
    if (parent.id == no_clade || parent.id >= overlay.base->clades.size()) {
      throw std::runtime_error(
          "overlay-chain compaction: active-production parent out of range");
    }
    for (auto pid : overlay.base->productions_by_parent[parent.id]) {
      if (std::binary_search(removed.begin(), removed.end(), pid)) continue;
      refs.push_back(base_production_ref(pid));
    }
  } else {
    if (parent.id == no_clade || parent.id >= overlay.temp_clades.size()) {
      throw std::runtime_error(
          "overlay-chain compaction: active-production temp parent out of "
          "range");
    }
  }
  for (std::size_t i = 0; i < overlay.temp_productions.size(); ++i) {
    if (overlay.temp_productions[i].parent == parent) {
      refs.push_back(temp_production_ref(static_cast<production_id>(i)));
    }
  }
  return refs;
}

inline std::vector<overlay_production_ref> active_overlay_productions(
    overlay_clade_grammar const& overlay) {
  chart_spr_detail::validate_overlay(overlay);
  std::vector<overlay_production_ref> refs;
  auto removed = chart_spr_detail::normalized_removed_base_productions(overlay);
  for (std::size_t pid = 0; pid < overlay.base->productions.size(); ++pid) {
    auto dense_pid = static_cast<production_id>(pid);
    if (std::binary_search(removed.begin(), removed.end(), dense_pid)) continue;
    refs.push_back(base_production_ref(dense_pid));
  }
  for (std::size_t i = 0; i < overlay.temp_productions.size(); ++i) {
    refs.push_back(temp_production_ref(static_cast<production_id>(i)));
  }
  return refs;
}

inline void select_overlay_topology_production(
    overlay_clade_grammar const& overlay,
    std::map<overlay_clade_ref, overlay_production_ref>& selected,
    overlay_production_ref ref, std::string const& context) {
  auto shape = overlay_production_shape_for_ref(overlay, ref, context);
  auto [it, inserted] = selected.emplace(shape.parent, ref);
  if (!inserted && it->second != ref) {
    throw std::runtime_error(
        context + ": conflicting concrete-topology choices for overlay clade");
  }
}

inline bool find_overlay_ancestor_path_to_clade(
    overlay_clade_grammar const& overlay, overlay_clade_ref clade,
    overlay_clade_ref target, std::vector<overlay_production_ref>& reversed_path,
    std::map<overlay_clade_ref, std::uint8_t>& state) {
  chart_spr_detail::validate_clade_ref(overlay, clade,
                                       "overlay-chain compaction ancestor path");
  if (clade == target) return true;
  auto& mark = state[clade];
  if (mark == 1) {
    throw std::runtime_error(
        "overlay-chain compaction: cycle while finding overlay ancestor path "
        "for production witness topology");
  }
  if (mark == 2) return false;
  mark = 1;

  for (auto prod_ref : active_overlay_productions_by_parent(overlay, clade)) {
    auto shape = overlay_production_shape_for_ref(
        overlay, prod_ref, "overlay-chain compaction ancestor path");
    for (auto child : shape.children) {
      if (find_overlay_ancestor_path_to_clade(overlay, child, target,
                                              reversed_path, state)) {
        reversed_path.push_back(prod_ref);
        mark = 2;
        return true;
      }
    }
  }

  mark = 2;
  return false;
}

inline void fill_overlay_topology_key_set_from_selected_choices(
    overlay_clade_grammar const& overlay,
    std::map<overlay_clade_ref, overlay_production_ref> const& selected,
    overlay_clade_ref clade, std::map<overlay_clade_ref, std::uint8_t>& state,
    std::vector<rank3_production_taxa_key>& keys) {
  chart_spr_detail::validate_clade_ref(overlay, clade,
                                       "overlay-chain compaction fill topology");
  auto& mark = state[clade];
  if (mark == 1) {
    throw std::runtime_error(
        "overlay-chain compaction: cycle while filling overlay witness "
        "topology");
  }
  if (mark == 2) {
    throw std::runtime_error(
        "overlay-chain compaction: selected overlay witness topology reuses a "
        "clade");
  }
  mark = 1;

  auto taxa = rank3_detail::overlay_taxa_for_ref(
      overlay, clade, "overlay-chain compaction fill topology clade");
  if (taxa.size() == 1) {
    if (selected.contains(clade)) {
      throw std::runtime_error(
          "overlay-chain compaction: singleton overlay clade has a selected "
          "production while filling witness topology");
    }
    mark = 2;
    return;
  }

  overlay_production_ref chosen;
  if (auto it = selected.find(clade); it != selected.end()) {
    chosen = it->second;
  } else {
    auto productions = active_overlay_productions_by_parent(overlay, clade);
    if (productions.empty()) {
      throw std::runtime_error(
          "overlay-chain compaction: non-singleton overlay clade has no "
          "active production while filling witness topology");
    }
    chosen = productions.front();
  }

  auto shape = overlay_production_shape_for_ref(
      overlay, chosen, "overlay-chain compaction fill topology");
  if (shape.parent != clade) {
    throw std::runtime_error(
        "overlay-chain compaction: selected overlay witness production parent "
        "mismatch while filling topology");
  }
  rank3_detail::append_unique_key(
      keys, overlay_production_key_from_ref(
                overlay, chosen,
                "overlay-chain compaction fill topology production"));
  for (auto child : shape.children) {
    fill_overlay_topology_key_set_from_selected_choices(overlay, selected,
                                                        child, state, keys);
  }
  mark = 2;
}

inline std::vector<rank3_production_taxa_key>
overlay_topology_key_set_containing_production_key(
    overlay_clade_grammar const& overlay, rank3_production_taxa_key key,
    std::string const& context) {
  chart_spr_detail::validate_overlay(overlay);
  rank3_detail::normalize_production_key(key);
  auto root = base_clade_ref(overlay.base->root_clade);

  std::string last_error;
  bool saw_matching_key = false;
  for (auto required_ref : active_overlay_productions(overlay)) {
    auto required_key = overlay_production_key_from_ref(
        overlay, required_ref, context + " required production lookup");
    if (required_key != key) continue;
    saw_matching_key = true;
    try {
      auto required_shape = overlay_production_shape_for_ref(
          overlay, required_ref, context + " required production");
      std::vector<overlay_production_ref> ancestor_path;
      if (required_shape.parent != root) {
        std::map<overlay_clade_ref, std::uint8_t> path_state;
        if (!find_overlay_ancestor_path_to_clade(overlay, root,
                                                 required_shape.parent,
                                                 ancestor_path, path_state)) {
          throw std::runtime_error(
              context + ": required production parent clade is not reachable "
                        "from the overlay root");
        }
        std::reverse(ancestor_path.begin(), ancestor_path.end());
      }

      std::map<overlay_clade_ref, overlay_production_ref> selected;
      for (auto path_ref : ancestor_path) {
        select_overlay_topology_production(overlay, selected, path_ref,
                                           context);
      }
      select_overlay_topology_production(overlay, selected, required_ref,
                                         context);

      std::vector<rank3_production_taxa_key> keys;
      std::map<overlay_clade_ref, std::uint8_t> fill_state;
      fill_overlay_topology_key_set_from_selected_choices(
          overlay, selected, root, fill_state, keys);
      if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        throw std::runtime_error(
            context + ": required production key is not reachable in the "
                      "constructed overlay witness topology");
      }
      return keys;
    } catch (std::runtime_error const& e) {
      last_error = e.what();
    }
  }

  if (!saw_matching_key) {
    throw std::runtime_error(context + ": accepted production key is absent "
                                       "from the overlay prefix: " +
                             rank3_detail::production_key_to_string(key));
  }
  throw std::runtime_error(
      context + ": no reachable overlay witness topology contains accepted "
                "production key " +
      rank3_detail::production_key_to_string(key) +
      (last_error.empty() ? std::string{} : "; last error: " + last_error));
}

}  // namespace overlay_chain_compaction_detail

inline std::vector<std::vector<rank3_production_taxa_key>>
overlay_chain_intended_production_key_sets(overlay_chain const& chain) {
  auto overlay = chain.tip();
  std::vector<std::vector<rank3_production_taxa_key>> key_sets;
  key_sets.reserve(chain.size());
  for (std::size_t pos = 0; pos < chain.size(); ++pos) {
    auto const& delta = chain.at(pos);
    std::vector<rank3_production_taxa_key> keys;
    keys.reserve(delta.temp_productions.size());
    for (auto const& prod : delta.temp_productions) {
      rank3_detail::append_unique_key(
          keys,
          overlay_chain_compaction_detail::production_key_from_overlay_production(
              overlay, prod,
              "overlay-chain compaction accepted delta " +
                  std::to_string(pos) + " intended production"));
    }
    if (!keys.empty()) key_sets.push_back(std::move(keys));
  }
  return key_sets;
}

inline std::vector<rank3_production_taxa_key>
overlay_chain_intended_production_keys(overlay_chain const& chain) {
  std::vector<rank3_production_taxa_key> keys;
  for (auto const& key_set : overlay_chain_intended_production_key_sets(chain)) {
    for (auto key : key_set) {
      rank3_detail::append_unique_key(keys, std::move(key));
    }
  }
  return keys;
}

// Derive each accepted-production witness in the overlay prefix that first
// represented it, and keep only that complete topology's taxon-key set.  This
// deliberately does not call materialize_overlay_grammar(): final compaction's
// one dense materialization is the materialize_overlay_chain() call below.
inline std::vector<std::vector<rank3_production_taxa_key>>
overlay_chain_prefix_witness_topology_key_sets(overlay_chain const& chain) {
  std::vector<std::vector<rank3_production_taxa_key>> key_sets;
  for (std::size_t pos = 0; pos < chain.size(); ++pos) {
    auto prefix_overlay =
        overlay_chain_compaction_detail::overlay_chain_prefix_tip(chain,
                                                                  pos + 1);
    for (auto const& prod : chain.at(pos).temp_productions) {
      auto key = overlay_chain_compaction_detail::
          production_key_from_overlay_production(
              prefix_overlay, prod,
              "overlay-chain compaction accepted prefix " +
                  std::to_string(pos) + " intended production");
      key_sets.push_back(overlay_chain_compaction_detail::
                             overlay_topology_key_set_containing_production_key(
                                 prefix_overlay, std::move(key),
                                 "overlay-chain compaction: deriving prefix "
                                 "witness topology for accepted production at "
                                 "chain position " +
                                     std::to_string(pos)));
    }
  }
  return key_sets;
}

inline overlay_chain_compaction_result compact_overlay_chain_to_dag(
    phylo_dag& source, overlay_chain const& chain,
    overlay_chain_compaction_options const& options = {}) {
  overlay_chain_compaction_result result;
  result.intended_productions = overlay_chain_intended_production_keys(chain);

  // The single dense overlay materialization for Phase-5 compaction.  Search
  // counters are bumped at the call site so this helper remains side-effect
  // free with respect to instrumentation.
  auto const materialization_start = std::chrono::steady_clock::now();
  result.materialized = materialize_overlay_chain(chain);
  result.materialization_ms =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - materialization_start)
          .count();
  auto const& grammar = result.materialized.grammar;

  auto prefix_witness_topology_key_sets =
      overlay_chain_prefix_witness_topology_key_sets(chain);

  std::vector<std::vector<rank3_production_taxa_key>> required_topology_keys;
  required_topology_keys.reserve(
      prefix_witness_topology_key_sets.size() +
      options.witness_topologies.size() +
      options.witness_topology_key_sets.size());
  for (auto key_set : prefix_witness_topology_key_sets) {
    required_topology_keys.push_back(std::move(key_set));
  }
  for (auto const& topology : options.witness_topologies) {
    required_topology_keys.push_back(
        overlay_chain_compaction_detail::topology_production_keys(grammar,
                                                                  topology));
  }
  for (auto key_set : options.witness_topology_key_sets) {
    for (auto& key : key_set) rank3_detail::normalize_production_key(key);
    required_topology_keys.push_back(std::move(key_set));
  }

  // Accepted topology certificates and prefix/intended-production witnesses
  // are stable taxon-key sets.  Some historical witnesses can contain
  // productions that are unreachable in the final chain after later tombstones,
  // so the topology-materialization grammar is the final chain grammar
  // augmented with exactly the complete witness topology key sets.  Intended
  // productions are not added as standalone global choices here: each accepted
  // intended production is carried by the complete topology key set derived in
  // the prefix grammar that originally represented it, preventing synthetic
  // historical+later hybrid topologies.
  auto topology_grammar =
      overlay_chain_compaction_detail::augment_grammar_with_topology_keys(
          grammar, required_topology_keys);

  std::vector<rank3_topology> topologies;
  for (std::size_t i = 0; i < required_topology_keys.size(); ++i) {
    auto topology = rank3_topology_from_productions(
        topology_grammar,
        [&]() {
          std::vector<production_id> pids;
          pids.reserve(required_topology_keys[i].size());
          for (auto key : required_topology_keys[i]) {
            pids.push_back(
                overlay_chain_compaction_detail::find_dense_production_by_key(
                    topology_grammar, std::move(key),
                    "overlay-chain compaction: required witness topology " +
                        std::to_string(i)));
          }
          return pids;
        }());
    (void)rank3_detail::validate_topology(topology_grammar, topology);
    overlay_chain_compaction_detail::append_unique_topology(
        topologies, topology_grammar, std::move(topology));
  }

  if (topologies.empty()) {
    topologies.push_back(first_rank3_topology(topology_grammar));
  }

  result.witness_topology_keys.reserve(topologies.size());
  for (auto const& topology : topologies) {
    result.witness_topology_keys.push_back(
        overlay_chain_compaction_detail::topology_production_keys(
            topology_grammar, topology));
  }

  rank3_option_a_options merge_options;
  merge_options.include_original_dag = false;
  merge_options.validate = options.validate;
  merge_options.generated_edge_weight = options.generated_edge_weight;
  merge_options.require_intended_productions_present = true;
  merge_options.rebuild_grammar_options = options.rebuild_grammar_options;

  auto merged = merge_rank3_trees_option_a(source, topology_grammar, topologies,
                                           merge_options);
  result.materialized_topologies = std::move(topologies);
  result.materialized_tree_count = merged.materialized_tree_count;
  result.dag = std::move(merged.dag);
  result.rebuilt = std::move(merged.rebuilt);
  result.intended_production_present = rank3_detail::production_key_presence(
      result.rebuilt.grammar, result.intended_productions);

  for (std::size_t i = 0; i < result.intended_productions.size(); ++i) {
    if (result.intended_production_present[i]) continue;
    throw std::runtime_error(
        "overlay-chain compaction: intended production missing from compacted "
        "output DAG after grammar rebuild: " +
        rank3_detail::production_key_to_string(result.intended_productions[i]));
  }

  result.witness_topology_present.reserve(result.witness_topology_keys.size());
  for (std::size_t i = 0; i < result.witness_topology_keys.size(); ++i) {
    try {
      overlay_chain_compaction_detail::validate_topology_key_set_present(
          result.rebuilt.grammar, result.witness_topology_keys[i],
          "overlay-chain compaction: required witness topology " +
              std::to_string(i) + " missing from compacted output DAG");
      result.witness_topology_present.push_back(true);
    } catch (std::exception const& e) {
      result.witness_topology_present.push_back(false);
      throw std::runtime_error(
          std::string{e.what()} + " (topology_keys=" +
          overlay_chain_compaction_detail::topology_key_set_to_string(
              result.witness_topology_keys[i]) +
          ")");
    }
  }
  return result;
}

inline grammar_level_exact_parsimony_result grammar_level_exact_parsimony(
    clade_grammar const& grammar, active_site_pattern_set const& active_patterns,
    chart_options const& chart_opts = {}, std::uint64_t invariant_offset = 0,
    multisite_trim_options const& trim_options = {}) {
  active_patterns.assert_no_skipped_invariant_metadata();
  grammar_level_exact_parsimony_result result;
  auto exact_trim_options =
      overlay_chain_compaction_exact_trim_options(trim_options);
  result.trim = build_multisite_trim_active(grammar, active_patterns,
                                            chart_opts, exact_trim_options);
  result.active_value = result.trim.optimum;
  result.value = chart_multisite_detail::checked_add_u64(
      result.active_value, invariant_offset,
      "overlay-chain compaction grammar-level exact parsimony invariant offset");
  result.exactness_kind = result.trim.keep_mask_kind;
  result.keep_production_exact = result.trim.keep_production_exact;
  if (result.exactness_kind !=
          multisite_keep_mask_kind::exact_optimal_production_union ||
      !result.keep_production_exact) {
    throw std::runtime_error(
        "overlay-chain compaction grammar-level oracle did not produce an "
        "exact optimal production union (kind=" +
        std::string{multisite_keep_mask_kind_name(result.exactness_kind)} +
        ")");
  }
  return result;
}

inline grammar_level_exact_parsimony_result grammar_level_exact_parsimony(
    phylo_dag& dag, active_site_pattern_set const& active_patterns,
    chart_options const& chart_opts = {}, std::uint64_t invariant_offset = 0,
    multisite_trim_options const& trim_options = {},
    clade_grammar_options const& grammar_options = {}) {
  auto grammar = build_clade_grammar(dag, grammar_options);
  return grammar_level_exact_parsimony(grammar, active_patterns, chart_opts,
                                       invariant_offset, trim_options);
}

}  // namespace larch

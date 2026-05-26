#pragma once

#include <larch/chart_trim.hpp>
#include <larch/native_optimize.hpp>
#include <larch/site_patterns.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace larch {

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

inline void validate_overlay(overlay_clade_grammar const& overlay) {
  if (overlay.base == nullptr)
    throw std::runtime_error("chart SPR: overlay has no base grammar");
  parsimony_chart_detail::validate_chart_grammar(*overlay.base);
  chart_trim_detail::validate_production_indices(*overlay.base);

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
    if (prod.children.empty()) {
      throw std::runtime_error("chart SPR: temp production " +
                               std::to_string(i) + " has no children");
    }
    for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
      validate_clade_ref(overlay, prod.children[child_i],
                         "temp production " + std::to_string(i) +
                             " child " + std::to_string(child_i));
    }
  }
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
    if (prod.children.size() != 2) {
      throw std::runtime_error("chart SPR: local recompute supports binary "
                               "productions only");
    }
    parsimony_chart_detail::validate_binary_production_partition(grammar, prod,
                                                                 pid);

    for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      chart_cost total = 0;
      for (std::size_t child_i = 0; child_i < 2; ++child_i) {
        auto child = prod.children[child_i];
        if (child == no_clade || child >= chart.inside.size()) {
          throw std::runtime_error("chart SPR: production child out of range");
        }
        chart_cost best_child = chart_inf;
        for (std::uint8_t child_state = 0; child_state < nuc_state_count;
             ++child_state) {
          best_child = std::min(
              best_child,
              parsimony_chart_detail::saturated_add(
                  chart.inside[child][child_state],
                  parsimony_chart_detail::transition_cost(parent_state,
                                                          child_state)));
        }
        total = parsimony_chart_detail::saturated_add(total, best_child);
      }
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

struct upward_path_step {
  production_id production = no_production;
  clade_id parent = no_clade;
  clade_id child = no_clade;
  clade_id sibling = no_clade;
};

using upward_path = std::vector<upward_path_step>;

inline std::optional<clade_id> binary_sibling_for_child(
    clade_grammar const& grammar, production_id pid, clade_id child) {
  if (pid == no_production || pid >= grammar.productions.size())
    return std::nullopt;
  auto const& prod = grammar.productions[pid];
  if (prod.children.size() != 2) return std::nullopt;
  if (prod.children[0] == child) return prod.children[1];
  if (prod.children[1] == child) return prod.children[0];
  return std::nullopt;
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
      auto sibling = binary_sibling_for_child(grammar, pid, clade);
      if (!sibling) continue;
      if (grammar.clades[prod.parent].taxa.size() <=
          grammar.clades[clade].taxa.size()) {
        continue;
      }
      current.push_back(upward_path_step{pid, prod.parent, clade, *sibling});
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

inline std::optional<grammar_spr_candidate> make_general_spr_candidate(
    clade_grammar const& grammar,
    std::map<std::vector<taxon_id>, clade_id> const& base_lookup,
    production_id source_pid, clade_id moved, clade_id old_sibling,
    clade_id target, upward_path const& source_path,
    upward_path const& dest_path) {
  if (source_pid == no_production || source_pid >= grammar.productions.size())
    return std::nullopt;
  auto const& source_prod = grammar.productions[source_pid];
  auto old_parent = source_prod.parent;
  if (source_prod.children.size() != 2) return std::nullopt;
  if (target == moved || target == old_sibling) return std::nullopt;

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
  candidate.old_sibling = base_clade_ref(old_sibling);
  candidate.new_sibling_or_target = base_clade_ref(target);
  append_removed_base_production(candidate, source_pid);

  std::map<std::vector<taxon_id>, clade_id> temp_lookup;
  auto source_current_taxa = grammar.clades[old_sibling].taxa;
  auto source_current_ref = base_clade_ref(old_sibling);

  // Transform the source branch up to, but not including, the LCA.  The final
  // LCA production is rebuilt after the destination branch has been expanded.
  for (std::size_t i = 0; i < source_lca_index; ++i) {
    append_removed_base_production(candidate, source_path[i].production);
    if (i + 1 == source_lca_index) break;

    auto parent_taxa = set_difference_taxa(
        grammar.clades[source_path[i].parent].taxa, moved_taxa);
    auto sibling_taxa = grammar.clades[source_path[i].sibling].taxa;
    if (!disjoint_taxa(source_current_taxa, sibling_taxa)) return std::nullopt;
    auto parent_ref = add_or_get_candidate_clade(
        grammar, candidate, base_lookup, temp_lookup, parent_taxa);
    maybe_add_candidate_production(
        grammar, candidate, parent_ref,
        {source_current_ref, base_clade_ref(source_path[i].sibling)});
    source_current_taxa = std::move(parent_taxa);
    source_current_ref = parent_ref;
  }

  auto dest_current_taxa = set_union_taxa(moved_taxa, target_taxa);
  auto dest_current_ref = add_or_get_candidate_clade(
      grammar, candidate, base_lookup, temp_lookup, dest_current_taxa);
  maybe_add_candidate_production(
      grammar, candidate, dest_current_ref,
      {base_clade_ref(moved), base_clade_ref(target)});

  bool destination_already_rebuilt_lca = (dest_current_taxa == grammar.clades[lca].taxa);
  for (std::size_t i = 0; i < dest_lca_index && !destination_already_rebuilt_lca;
       ++i) {
    append_removed_base_production(candidate, dest_path[i].production);
    if (dest_path[i].parent == lca) break;

    auto sibling_taxa = grammar.clades[dest_path[i].sibling].taxa;
    if (!disjoint_taxa(moved_taxa, sibling_taxa)) return std::nullopt;
    auto parent_taxa = set_union_taxa(grammar.clades[dest_path[i].parent].taxa,
                                      moved_taxa);
    auto parent_ref = add_or_get_candidate_clade(
        grammar, candidate, base_lookup, temp_lookup, parent_taxa);
    maybe_add_candidate_production(
        grammar, candidate, parent_ref,
        {dest_current_ref, base_clade_ref(dest_path[i].sibling)});
    dest_current_taxa = std::move(parent_taxa);
    dest_current_ref = parent_ref;
    destination_already_rebuilt_lca =
        (dest_current_taxa == grammar.clades[lca].taxa);
  }

  if (!destination_already_rebuilt_lca) {
    if (!disjoint_taxa(source_current_taxa, dest_current_taxa)) return std::nullopt;
    maybe_add_candidate_production(grammar, candidate, base_clade_ref(lca),
                                   {source_current_ref, dest_current_ref});
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
      auto sibling = binary_sibling_for_child(grammar, pid, clade);
      if (!sibling) continue;
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
      current.push_back(upward_path_step{pid, prod.parent, clade, *sibling});
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
    if (source_prod.children.size() != 2) continue;
    if (!options.include_root_moves &&
        source_prod.parent == grammar.root_clade) {
      note_pruned_before(stats,
                         &chart_spr_candidate_generation_stats::
                             candidates_pruned_root_or_trivial);
      continue;
    }

    std::array<std::size_t, 2> moved_order{0, 1};
    if (options.randomize_order) {
      std::shuffle(moved_order.begin(), moved_order.end(), rng);
    }
    for (auto moved_i : moved_order) {
      if (stopped()) break;
      auto moved = source_prod.children[moved_i];
      auto old_sibling = source_prod.children[1 - moved_i];
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
        if (target == moved || target == old_sibling ||
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
                        grammar, base_lookup, source_pid, moved, old_sibling,
                        target, source_path, dest_path);
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
  chart_trim_detail::validate_production_indices(grammar);

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
    chart_spr_candidate_generation_stats stats;
    if (stream_options.source == chart_spr_candidate_source::grammar) {
      stats = chart_spr_detail::for_each_grammar_spr_candidate_stream(
          grammar, stream_options, reservoir_callback);
    } else if (stream_options.source ==
               chart_spr_candidate_source::sampled_tree) {
      stats = for_each_sampled_tree_spr_candidate(
          grammar, stream_options, reservoir_callback);
    } else {
      stats = for_each_hybrid_spr_candidate(
          grammar, stream_options, reservoir_callback);
    }
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

  if (options.source == chart_spr_candidate_source::sampled_tree) {
    return for_each_sampled_tree_spr_candidate(grammar, options,
                                               std::forward<F>(callback));
  }
  if (options.source == chart_spr_candidate_source::hybrid) {
    return for_each_hybrid_spr_candidate(grammar, options,
                                         std::forward<F>(callback));
  }
  return chart_spr_detail::for_each_grammar_spr_candidate_stream(
      grammar, options, std::forward<F>(callback));
}

inline overlay_clade_grammar overlay_from_candidate(
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
  chart_spr_detail::validate_overlay(overlay);
  return overlay;
}

inline overlay_materialization_result materialize_overlay_grammar(
    overlay_clade_grammar const& overlay) {
  using namespace chart_spr_detail;
  validate_overlay(overlay);

  overlay_materialization_result result;
  auto const& base = *overlay.base;
  auto& grammar = result.grammar;

  grammar.taxa = base.taxa;

  auto removed = normalized_removed_base_productions(overlay);
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

  grammar.root_clade = dense_clade_id(result, base_clade_ref(base.root_clade));
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
    prod.parent = dense_clade_id(result, base_clade_ref(base_prod.parent));
    for (auto& child : prod.children)
      child = dense_clade_id(result, base_clade_ref(child));

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
    prod.parent = dense_clade_id(result, overlay_prod.parent);
    prod.children.reserve(overlay_prod.children.size());
    for (auto child_ref : overlay_prod.children)
      prod.children.push_back(dense_clade_id(result, child_ref));
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

  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);
  return result;
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
// rebuilds each; production chart-SPR search must use cached local scoring
// instead.  The result is a composite lower bound, not an exact coupled
// multi-site objective.
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
// the search state's cached old exact score and build only the candidate's new
// exact objective for the small verified set.
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
    clade_grammar const& base, phylo_dag& tree, spr_move const& move) {
  using namespace chart_spr_detail;
  build_clade_offsets(tree);
  auto before_tree_grammar = build_clade_grammar(tree);
  auto base_lookup = build_clade_lookup(base);

  tree_index index{tree};
  if (!index.is_valid(move.src) || !index.is_valid(move.dst)) return std::nullopt;
  if (move.src == move.dst || move.src == index.get_tree_root())
    return std::nullopt;
  if (index.is_ancestor(move.src, move.dst)) return std::nullopt;

  auto src_parent_node = index.get_parent(move.src);
  if (index.get_num_children(src_parent_node) != 2) return std::nullopt;

  auto moved = tree_node_to_base_clade(base, before_tree_grammar, base_lookup,
                                       move.src);
  auto old_parent = tree_node_to_base_clade(base, before_tree_grammar,
                                           base_lookup, src_parent_node);
  auto target = tree_node_to_base_clade(base, before_tree_grammar, base_lookup,
                                        move.dst);
  if (!moved || !old_parent || !target) return std::nullopt;

  std::optional<clade_id> old_sibling;
  for (auto child : index.get_children(src_parent_node)) {
    if (child == move.src) continue;
    old_sibling = tree_node_to_base_clade(base, before_tree_grammar,
                                          base_lookup, child);
  }
  if (!old_sibling) return std::nullopt;

  grammar_spr_candidate candidate;
  candidate.moved_clade = base_clade_ref(*moved);
  candidate.old_parent = base_clade_ref(*old_parent);
  candidate.old_sibling = base_clade_ref(*old_sibling);
  candidate.new_sibling_or_target = base_clade_ref(*target);
  candidate.source_tree_move = move;

  auto after_tree = apply_spr_move(tree, move.src, move.dst);
  build_clade_offsets(after_tree);
  auto after_tree_grammar = build_clade_grammar(after_tree);

  return make_candidate_from_tree_diff(base, before_tree_grammar,
                                       after_tree_grammar,
                                       std::move(candidate));
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
  build_clade_offsets(tree);
  tree_index index{tree};
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
    auto projected = project_tree_spr_move_to_candidate(base, tree, move);
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
}

}  // namespace chart_spr_detail

template <typename F>
chart_spr_candidate_generation_stats for_each_sampled_tree_spr_candidate(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  using namespace chart_spr_detail;
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);

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
    auto tree = build_sampled_tree_from_grammar(grammar, options, sample_i, rng);
    tree_index index{tree};
    move_enumerator enumerator{index, options.sampled_tree_score_threshold};
    auto radius = options.sampled_tree_spr_radius > 0
                      ? options.sampled_tree_spr_radius
                      : compute_tree_max_depth(tree) * 2;
    if (radius == 0) radius = 1;

    struct sampled_tree_enumeration_stop {};
    auto stop_now = [&]() -> void { throw sampled_tree_enumeration_stop{}; };
    auto process_move = [&](profitable_move const& move) {
      if (stopped()) stop_now();
      if (pre_dedup_cap_reached()) {
        request_stop(chart_spr_candidate_stop_reason::candidate_cap);
        stop_now();
      }
      auto projected = project_tree_spr_move_to_candidate(grammar, tree, move);
      if (!projected) {
        note_pruned_after(stats,
                          &chart_spr_candidate_generation_stats::
                              candidates_pruned_invalid);
        return;
      }
      ++stats.candidates_constructed;
      if (!candidate_passes_postconstruction_filters(grammar, *projected,
                                                     options, stats)) {
        if (pre_dedup_cap_reached()) {
          request_stop(chart_spr_candidate_stop_reason::candidate_cap);
          stop_now();
        }
        return;
      }
      auto signature = chart_spr_candidate_taxon_signature(grammar, *projected);
      if (!seen.insert(std::move(signature)).second) {
        note_pruned_after(stats,
                          &chart_spr_candidate_generation_stats::
                              candidates_pruned_duplicate);
        if (pre_dedup_cap_reached()) {
          request_stop(chart_spr_candidate_stop_reason::candidate_cap);
          stop_now();
        }
        return;
      }
      ++stats.candidates_generated_after_dedup;
      if (!invoke_candidate_callback(callback, *projected)) {
        request_stop(chart_spr_candidate_stop_reason::callback_stop);
        stop_now();
      }
      if (pre_dedup_cap_reached() || post_dedup_cap_reached()) {
        request_stop(chart_spr_candidate_stop_reason::candidate_cap);
        stop_now();
      }
    };

    auto source_order = index.get_searchable_nodes();
    shuffle_if_requested(source_order, options, &rng);
    for (auto source_node : source_order) {
      if (stopped()) break;
      try {
        enumerator.find_moves_for_source(
            source_node, radius,
            [&](profitable_move const& move) { process_move(move); });
      } catch (sampled_tree_enumeration_stop const&) {
        break;
      }
    }
  }
  return stats;
}

template <typename F>
chart_spr_candidate_generation_stats for_each_hybrid_spr_candidate(
    clade_grammar const& grammar,
    grammar_spr_enumeration_options const& options, F&& callback) {
  using namespace chart_spr_detail;
  parsimony_chart_detail::validate_chart_grammar(grammar);
  chart_trim_detail::validate_production_indices(grammar);

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
    auto sampled_stats = for_each_sampled_tree_spr_candidate(
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

}  // namespace larch

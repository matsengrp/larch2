#pragma once

#include <larch/clade_grammar.hpp>
#include <larch/sha256.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larch {

// Canonical chart-SPR reporting is deliberately opt-in.  Normal product runs
// leave this off and do not retain candidate or exact-frontier evidence.
enum class chart_spr_semantic_capture_mode { off, digest, full };

struct chart_spr_canonical_score {
  std::string kind;
  std::string convention;
  std::int64_t delta = 0;
  std::uint64_t old_score = 0;
  std::uint64_t new_score = 0;
  std::uint64_t invariant_offset = 0;
  bool exact_multisite = false;
};

// One equality-deduplicated optimal root-frontier class.  `cost` is in the
// stable active-pattern/state layout recorded by the report contract;
// production keys are the merged tied provenance carried by that class.
struct chart_spr_canonical_root_provenance_class {
  std::vector<std::uint64_t> cost;
  std::vector<std::string> production_keys;
};

struct chart_spr_canonical_exact_evidence {
  std::string evidence_kind = "none";
  std::string keep_mask_kind = "none";
  bool keep_production_exact = false;
  std::uint64_t optimum_active = 0;
  std::uint64_t invariant_offset = 0;
  std::vector<std::string> kept_production_keys;
  std::vector<std::pair<std::string, std::size_t>> frontier_sizes;
  std::vector<chart_spr_canonical_root_provenance_class>
      optimal_root_provenance_classes;
  // Fixed-topology exact evidence is a complete selected before/after
  // certificate rather than a grammar-wide keep mask.
  std::string topology_selection_kind = "none";
  std::string topology_selector;
  std::vector<std::string> before_topology_production_keys;
  std::vector<std::string> after_topology_production_keys;
};

struct chart_spr_canonical_candidate_record {
  std::size_t stream_index = 0;
  std::string signature;
  bool valid = true;
  std::string invalid_reason;
  std::size_t affected_clade_count = 0;
  chart_spr_canonical_score lower_bound;
  std::optional<std::size_t> ranked_index;
  std::optional<std::size_t> exact_verification_index;
  std::optional<chart_spr_canonical_score> exact;
  std::optional<chart_spr_canonical_exact_evidence> exact_evidence;
};

struct chart_spr_canonical_iteration_record {
  std::size_t iteration = 0;
  std::uint32_t seed = 0;
  std::uint64_t state_score_before = 0;
  std::uint64_t state_score_after = 0;
  std::string generation_stop_reason;
  std::size_t candidates_generated = 0;
  std::size_t candidates_scored = 0;
  std::size_t candidates_exact_verified = 0;
  bool unverified_candidates_may_contain_improvements = false;
  std::vector<chart_spr_canonical_candidate_record> candidates;
  std::vector<std::size_t> ranked_stream_indices;
  std::vector<std::size_t> exact_verified_stream_indices;
  std::optional<std::size_t> selected_stream_index;
  std::string selected_signature;
  bool accepted_move_present = false;
  bool accepted_move_committed = false;
  bool post_materialization_rejected = false;
  std::string no_accept_reason;
  std::string post_materialization_rejection_reason;
  std::optional<chart_spr_canonical_exact_evidence> state_exact_before;
};

struct chart_spr_canonical_chain_entry {
  std::size_t position = 0;
  std::string commit_source;
  std::vector<std::string> added_production_keys;
  std::vector<std::string> tombstoned_production_keys;
};

struct chart_spr_canonical_contract {
  std::string acceptance;
  std::string objective;
  std::string candidate_selection;
  std::string candidate_source;
  std::string topology_selection;
  std::string commit_mode;
  std::string accepted_state_update;
  std::string verification_mode;
  std::string chain_per_accept_exactness;
  std::string score_convention;
  std::string dominance_mode;
  std::string keep_mask_contract;
  std::string polytomy_mode;
  std::string refinement_exactness;
  std::string candidate_cap_semantics;
  std::size_t max_iterations = 0;
  std::size_t max_candidates = 0;
  std::size_t top_k_exact = 0;
  std::uint32_t seed = 0;
  bool score_ua_edge = false;
  bool use_bound_pruning = false;
  bool require_exact_keep_mask = false;
  bool randomize_order = false;
  bool reservoir_sample = false;
  bool include_immediate_reversals = false;
  bool include_root_moves = false;
  bool include_neutral_or_reversal_candidates = false;
  std::size_t sampled_tree_count = 0;
  std::size_t sampled_tree_radius = 0;
  std::int64_t sampled_tree_score_threshold = 0;
  std::size_t max_upward_path_expansions = 0;
  std::size_t max_path_pairs = 0;
  std::size_t min_moved_clade_size = 0;
  std::size_t max_moved_clade_size = 0;
  std::size_t min_target_clade_size = 0;
  std::size_t max_target_clade_size = 0;
  std::size_t max_affected_clades = 0;
  std::size_t max_frontier_entries = 0;
  std::size_t polytomy_max_exact_arity = 0;
  std::size_t polytomy_max_shapes = 0;
  std::size_t polytomy_max_productions = 0;
  std::size_t polytomy_max_clades = 0;
};

struct chart_spr_canonical_report {
  chart_spr_semantic_capture_mode capture_mode =
      chart_spr_semantic_capture_mode::off;
  chart_spr_canonical_contract contract;
  std::size_t active_pattern_count = 0;
  std::size_t skipped_invariant_site_count = 0;
  std::uint64_t invariant_constant_offset = 0;
  std::uint64_t initial_score = 0;
  std::vector<chart_spr_canonical_iteration_record> iterations;
  std::vector<std::string> chain_base_production_keys;
  std::vector<chart_spr_canonical_chain_entry> chain_entries;
  std::uint64_t final_score = 0;
  std::size_t accepted_moves = 0;
  std::vector<std::string> final_clade_keys;
  std::vector<std::string> final_production_keys;
  std::optional<chart_spr_canonical_exact_evidence> final_exact;
};

struct chart_spr_semantic_digest_report {
  std::string semantic_sha256;
  std::string contract_sha256;
  std::string candidates_sha256;
  std::string exact_sha256;
  std::string acceptance_sha256;
  std::string chain_sha256;
  std::string final_topology_sha256;
  std::size_t record_count = 0;
  std::size_t candidate_count = 0;
  std::size_t exact_candidate_count = 0;
  std::size_t iteration_count = 0;
  std::string full_sidecar;
};

namespace chart_spr_semantic_detail {

inline void append_length_prefixed(std::string& out, std::string_view value) {
  out += std::to_string(value.size());
  out += ":";
  out.append(value);
}

inline std::string sample_set_key(clade_grammar const& grammar,
                                  std::vector<taxon_id> taxa) {
  std::vector<std::string> samples;
  samples.reserve(taxa.size());
  for (auto taxon : taxa) {
    if (taxon >= grammar.taxa.id_to_sample_id.size()) {
      throw std::runtime_error(
          "chart-SPR canonical report: taxon out of sample registry range");
    }
    samples.push_back(grammar.taxa.id_to_sample_id[taxon]);
  }
  std::sort(samples.begin(), samples.end());
  if (std::adjacent_find(samples.begin(), samples.end()) != samples.end()) {
    throw std::runtime_error(
        "chart-SPR canonical report: duplicate sample ID in clade key");
  }
  std::string result{"S"};
  result += std::to_string(samples.size());
  result += "[";
  for (auto const& sample : samples) append_length_prefixed(result, sample);
  result += "]";
  return result;
}

inline std::string production_key(
    clade_grammar const& grammar, std::vector<taxon_id> parent_taxa,
    std::vector<std::vector<taxon_id>> child_taxa);

inline std::string production_key(clade_grammar const& grammar,
                                  production_id pid) {
  if (pid == no_production || pid >= grammar.productions.size()) {
    throw std::runtime_error(
        "chart-SPR canonical report: production ID out of range");
  }
  auto const& production = grammar.productions[pid];
  if (production.parent == no_clade ||
      production.parent >= grammar.clades.size()) {
    throw std::runtime_error(
        "chart-SPR canonical report: production parent out of range");
  }
  std::vector<std::vector<taxon_id>> child_taxa;
  child_taxa.reserve(production.children.size());
  for (auto child : production.children) {
    if (child == no_clade || child >= grammar.clades.size()) {
      throw std::runtime_error(
          "chart-SPR canonical report: production child out of range");
    }
    child_taxa.push_back(grammar.clades[child].taxa);
  }
  return production_key(grammar, grammar.clades[production.parent].taxa,
                        std::move(child_taxa));
}

inline std::string production_key(
    clade_grammar const& grammar, std::vector<taxon_id> parent_taxa,
    std::vector<std::vector<taxon_id>> child_taxa) {
  std::vector<std::string> children;
  children.reserve(child_taxa.size());
  for (auto& child : child_taxa) {
    children.push_back(sample_set_key(grammar, std::move(child)));
  }
  std::sort(children.begin(), children.end());
  std::string result{"P"};
  append_length_prefixed(result,
                         sample_set_key(grammar, std::move(parent_taxa)));
  result += "[";
  for (auto const& child : children) append_length_prefixed(result, child);
  result += "]";
  return result;
}

inline std::vector<std::string> grammar_clade_keys(
    clade_grammar const& grammar) {
  std::vector<std::string> result;
  result.reserve(grammar.clades.size());
  for (auto const& clade : grammar.clades) {
    if (clade.taxa.empty()) continue;
    result.push_back(sample_set_key(grammar, clade.taxa));
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

inline std::vector<std::string> grammar_production_keys(
    clade_grammar const& grammar) {
  std::vector<std::string> result;
  result.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (grammar.productions[pid].parent == no_clade) continue;
    result.push_back(production_key(grammar, static_cast<production_id>(pid)));
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

inline void sort_exact_evidence(chart_spr_canonical_exact_evidence& evidence) {
  auto sort_unique = [](auto& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  };
  sort_unique(evidence.kept_production_keys);
  sort_unique(evidence.before_topology_production_keys);
  sort_unique(evidence.after_topology_production_keys);
  std::sort(evidence.frontier_sizes.begin(), evidence.frontier_sizes.end());
  for (auto& provenance : evidence.optimal_root_provenance_classes) {
    sort_unique(provenance.production_keys);
  }
  std::sort(evidence.optimal_root_provenance_classes.begin(),
            evidence.optimal_root_provenance_classes.end(),
            [](auto const& lhs, auto const& rhs) {
              if (lhs.cost != rhs.cost) return lhs.cost < rhs.cost;
              return lhs.production_keys < rhs.production_keys;
            });
  evidence.optimal_root_provenance_classes.erase(
      std::unique(evidence.optimal_root_provenance_classes.begin(),
                  evidence.optimal_root_provenance_classes.end(),
                  [](auto const& lhs, auto const& rhs) {
                    return lhs.cost == rhs.cost &&
                           lhs.production_keys == rhs.production_keys;
                  }),
      evidence.optimal_root_provenance_classes.end());
}

inline void append_json_string(std::string& out, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  out.push_back('"');
  for (unsigned char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20U) {
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 0x0fU]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
}

inline void append_json_key(std::string& out, std::string_view key) {
  append_json_string(out, key);
  out.push_back(':');
}

inline void append_json_bool(std::string& out, bool value) {
  out += value ? "true" : "false";
}

inline void append_json_string_array(std::string& out,
                                     std::vector<std::string> const& values) {
  out.push_back('[');
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out.push_back(',');
    append_json_string(out, values[i]);
  }
  out.push_back(']');
}

inline std::string score_json(std::string_view record,
                              std::size_t iteration,
                              std::size_t stream_index,
                              chart_spr_canonical_score const& score) {
  std::string out{"{"};
  append_json_key(out, "record"); append_json_string(out, record);
  out += ",\"iteration\":" + std::to_string(iteration);
  out += ",\"stream_index\":" + std::to_string(stream_index);
  append_json_key(out += ",", "kind"); append_json_string(out, score.kind);
  append_json_key(out += ",", "convention");
  append_json_string(out, score.convention);
  out += ",\"delta\":" + std::to_string(score.delta);
  out += ",\"old_score\":" + std::to_string(score.old_score);
  out += ",\"new_score\":" + std::to_string(score.new_score);
  out += ",\"invariant_offset\":" +
         std::to_string(score.invariant_offset);
  append_json_key(out += ",", "exact_multisite");
  append_json_bool(out, score.exact_multisite);
  out += "}\n";
  return out;
}

enum class section { contract, candidates, exact, acceptance, chain, final };

struct stream_emitter {
  explicit stream_emitter(bool retain_full) : retain_full(retain_full) {}

  void emit(section which, std::string const& line) {
    overall.update(line);
    hasher(which).update(line);
    if (retain_full) full += line;
    ++record_count;
  }

  sha256& hasher(section which) {
    switch (which) {
      case section::contract: return contract;
      case section::candidates: return candidates;
      case section::exact: return exact;
      case section::acceptance: return acceptance;
      case section::chain: return chain;
      case section::final: return final;
    }
    throw std::logic_error("unknown canonical report section");
  }

  bool retain_full = false;
  sha256 overall;
  sha256 contract;
  sha256 candidates;
  sha256 exact;
  sha256 acceptance;
  sha256 chain;
  sha256 final;
  std::size_t record_count = 0;
  std::string full;
};

inline void emit_exact_evidence(stream_emitter& emitter, std::size_t iteration,
                                std::size_t stream_index,
                                std::string_view scope,
                                chart_spr_canonical_exact_evidence evidence) {
  sort_exact_evidence(evidence);
  std::string line{"{\"record\":\"exact_evidence\",\"scope\":"};
  append_json_string(line, scope);
  line += ",\"iteration\":" + std::to_string(iteration);
  line += ",\"stream_index\":" + std::to_string(stream_index);
  append_json_key(line += ",", "evidence_kind");
  append_json_string(line, evidence.evidence_kind);
  append_json_key(line += ",", "keep_mask_kind");
  append_json_string(line, evidence.keep_mask_kind);
  append_json_key(line += ",", "keep_production_exact");
  append_json_bool(line, evidence.keep_production_exact);
  line += ",\"optimum_active\":" + std::to_string(evidence.optimum_active);
  line += ",\"invariant_offset\":" +
          std::to_string(evidence.invariant_offset);
  append_json_key(line += ",", "topology_selection_kind");
  append_json_string(line, evidence.topology_selection_kind);
  append_json_key(line += ",", "topology_selector");
  append_json_string(line, evidence.topology_selector);
  line += "}\n";
  emitter.emit(section::exact, line);

  for (auto const& key : evidence.kept_production_keys) {
    line = "{\"record\":\"exact_keep_production\",\"scope\":";
    append_json_string(line, scope);
    line += ",\"iteration\":" + std::to_string(iteration);
    line += ",\"stream_index\":" + std::to_string(stream_index);
    append_json_key(line += ",", "production_key");
    append_json_string(line, key);
    line += "}\n";
    emitter.emit(section::exact, line);
  }
  for (auto const& [key, size] : evidence.frontier_sizes) {
    line = "{\"record\":\"exact_frontier_size\",\"scope\":";
    append_json_string(line, scope);
    line += ",\"iteration\":" + std::to_string(iteration);
    line += ",\"stream_index\":" + std::to_string(stream_index);
    append_json_key(line += ",", "clade_key"); append_json_string(line, key);
    line += ",\"size\":" + std::to_string(size) + "}\n";
    emitter.emit(section::exact, line);
  }
  for (std::size_t i = 0;
       i < evidence.optimal_root_provenance_classes.size(); ++i) {
    auto const& provenance = evidence.optimal_root_provenance_classes[i];
    line = "{\"record\":\"exact_root_provenance_class\",\"scope\":";
    append_json_string(line, scope);
    line += ",\"iteration\":" + std::to_string(iteration);
    line += ",\"stream_index\":" + std::to_string(stream_index);
    line += ",\"class_index\":" + std::to_string(i) + ",\"cost\":[";
    for (std::size_t j = 0; j < provenance.cost.size(); ++j) {
      if (j != 0) line.push_back(',');
      line += std::to_string(provenance.cost[j]);
    }
    line += "],\"production_keys\":";
    append_json_string_array(line, provenance.production_keys);
    line += "}\n";
    emitter.emit(section::exact, line);
  }
  auto emit_certificate_keys = [&](std::string_view record,
                                   std::vector<std::string> const& keys) {
    for (auto const& key : keys) {
      line = "{\"record\":";
      append_json_string(line, record);
      line += ",\"scope\":";
      append_json_string(line, scope);
      line += ",\"iteration\":" + std::to_string(iteration);
      line += ",\"stream_index\":" + std::to_string(stream_index);
      append_json_key(line += ",", "production_key");
      append_json_string(line, key);
      line += "}\n";
      emitter.emit(section::exact, line);
    }
  };
  emit_certificate_keys("fixed_topology_before_production",
                        evidence.before_topology_production_keys);
  emit_certificate_keys("fixed_topology_after_production",
                        evidence.after_topology_production_keys);
}

}  // namespace chart_spr_semantic_detail

inline std::string chart_spr_canonical_clade_sample_key(
    clade_grammar const& grammar, std::vector<taxon_id> taxa) {
  return chart_spr_semantic_detail::sample_set_key(grammar, std::move(taxa));
}

inline std::string chart_spr_canonical_production_sample_key(
    clade_grammar const& grammar, production_id production) {
  return chart_spr_semantic_detail::production_key(grammar, production);
}

inline std::string chart_spr_canonical_production_sample_key(
    clade_grammar const& grammar, std::vector<taxon_id> parent_taxa,
    std::vector<std::vector<taxon_id>> child_taxa) {
  return chart_spr_semantic_detail::production_key(
      grammar, std::move(parent_taxa), std::move(child_taxa));
}

inline std::vector<std::string> chart_spr_canonical_grammar_clade_keys(
    clade_grammar const& grammar) {
  return chart_spr_semantic_detail::grammar_clade_keys(grammar);
}

inline std::vector<std::string> chart_spr_canonical_grammar_production_keys(
    clade_grammar const& grammar) {
  return chart_spr_semantic_detail::grammar_production_keys(grammar);
}

inline chart_spr_semantic_digest_report build_chart_spr_semantic_digest_report(
    chart_spr_canonical_report report) {
  using namespace chart_spr_semantic_detail;
  if (report.capture_mode == chart_spr_semantic_capture_mode::off) return {};

  stream_emitter emitter{report.capture_mode ==
                         chart_spr_semantic_capture_mode::full};
  emitter.emit(section::contract,
               "{\"record\":\"schema\",\"schema\":\"larch.chart_spr.semantic.ndjson\",\"schema_version\":1}\n");

  auto const& c = report.contract;
  std::string line{"{\"record\":\"contract\""};
  auto string_field = [&](std::string_view key, std::string_view value) {
    append_json_key(line += ",", key); append_json_string(line, value);
  };
  auto bool_field = [&](std::string_view key, bool value) {
    append_json_key(line += ",", key); append_json_bool(line, value);
  };
  auto size_field = [&](std::string_view key, auto value) {
    append_json_key(line += ",", key); line += std::to_string(value);
  };
  string_field("acceptance", c.acceptance);
  string_field("objective", c.objective);
  string_field("candidate_selection", c.candidate_selection);
  string_field("candidate_source", c.candidate_source);
  string_field("topology_selection", c.topology_selection);
  string_field("commit_mode", c.commit_mode);
  string_field("accepted_state_update", c.accepted_state_update);
  string_field("verification_mode", c.verification_mode);
  string_field("chain_per_accept_exactness", c.chain_per_accept_exactness);
  string_field("score_convention", c.score_convention);
  string_field("dominance_mode", c.dominance_mode);
  string_field("keep_mask_contract", c.keep_mask_contract);
  string_field("polytomy_mode", c.polytomy_mode);
  string_field("refinement_exactness", c.refinement_exactness);
  string_field("candidate_cap_semantics", c.candidate_cap_semantics);
  size_field("max_iterations", c.max_iterations);
  size_field("max_candidates", c.max_candidates);
  size_field("top_k_exact", c.top_k_exact);
  size_field("seed", c.seed);
  bool_field("score_ua_edge", c.score_ua_edge);
  bool_field("use_bound_pruning", c.use_bound_pruning);
  bool_field("require_exact_keep_mask", c.require_exact_keep_mask);
  bool_field("randomize_order", c.randomize_order);
  bool_field("reservoir_sample", c.reservoir_sample);
  bool_field("include_immediate_reversals", c.include_immediate_reversals);
  bool_field("include_root_moves", c.include_root_moves);
  bool_field("include_neutral_or_reversal_candidates",
             c.include_neutral_or_reversal_candidates);
  size_field("sampled_tree_count", c.sampled_tree_count);
  size_field("sampled_tree_radius", c.sampled_tree_radius);
  size_field("sampled_tree_score_threshold", c.sampled_tree_score_threshold);
  size_field("max_upward_path_expansions", c.max_upward_path_expansions);
  size_field("max_path_pairs", c.max_path_pairs);
  size_field("min_moved_clade_size", c.min_moved_clade_size);
  size_field("max_moved_clade_size", c.max_moved_clade_size);
  size_field("min_target_clade_size", c.min_target_clade_size);
  size_field("max_target_clade_size", c.max_target_clade_size);
  size_field("max_affected_clades", c.max_affected_clades);
  size_field("max_frontier_entries", c.max_frontier_entries);
  size_field("polytomy_max_exact_arity", c.polytomy_max_exact_arity);
  size_field("polytomy_max_shapes", c.polytomy_max_shapes);
  size_field("polytomy_max_productions", c.polytomy_max_productions);
  size_field("polytomy_max_clades", c.polytomy_max_clades);
  line += "}\n";
  emitter.emit(section::contract, line);

  line = "{\"record\":\"initial_state\",\"active_patterns\":" +
         std::to_string(report.active_pattern_count) +
         ",\"skipped_invariant_sites\":" +
         std::to_string(report.skipped_invariant_site_count) +
         ",\"invariant_offset\":" +
         std::to_string(report.invariant_constant_offset) +
         ",\"initial_score\":" + std::to_string(report.initial_score) +
         "}\n";
  emitter.emit(section::contract, line);

  std::size_t candidate_count = 0;
  std::size_t exact_candidate_count = 0;
  for (auto const& iteration : report.iterations) {
    line = "{\"record\":\"iteration_begin\",\"iteration\":" +
           std::to_string(iteration.iteration) + ",\"seed\":" +
           std::to_string(iteration.seed) + ",\"state_score_before\":" +
           std::to_string(iteration.state_score_before) + "}\n";
    emitter.emit(section::acceptance, line);
    if (iteration.state_exact_before) {
      emit_exact_evidence(emitter, iteration.iteration,
                          (std::numeric_limits<std::size_t>::max)(),
                          "state_before", *iteration.state_exact_before);
    }

    for (auto const& candidate : iteration.candidates) {
      ++candidate_count;
      line = "{\"record\":\"candidate\",\"iteration\":" +
             std::to_string(iteration.iteration) +
             ",\"stream_index\":" +
             std::to_string(candidate.stream_index);
      append_json_key(line += ",", "signature");
      append_json_string(line, candidate.signature);
      append_json_key(line += ",", "valid");
      append_json_bool(line, candidate.valid);
      append_json_key(line += ",", "invalid_reason");
      append_json_string(line, candidate.invalid_reason);
      line += ",\"affected_clade_count\":" +
              std::to_string(candidate.affected_clade_count) + "}\n";
      emitter.emit(section::candidates, line);
      emitter.emit(section::candidates,
                   score_json("candidate_lower_bound", iteration.iteration,
                              candidate.stream_index,
                              candidate.lower_bound));
      if (candidate.ranked_index) {
        line = "{\"record\":\"candidate_rank\",\"iteration\":" +
               std::to_string(iteration.iteration) +
               ",\"rank\":" + std::to_string(*candidate.ranked_index) +
               ",\"stream_index\":" +
               std::to_string(candidate.stream_index) + "}\n";
        emitter.emit(section::candidates, line);
      }
      if (candidate.exact_verification_index) {
        line = "{\"record\":\"candidate_exact_verification_rank\",\"iteration\":" +
               std::to_string(iteration.iteration) +
               ",\"exact_verification_index\":" +
               std::to_string(*candidate.exact_verification_index) +
               ",\"stream_index\":" +
               std::to_string(candidate.stream_index) + "}\n";
        emitter.emit(section::exact, line);
      }
      if (candidate.exact) {
        ++exact_candidate_count;
        emitter.emit(section::exact,
                     score_json("candidate_exact", iteration.iteration,
                                candidate.stream_index, *candidate.exact));
      }
      if (candidate.exact_evidence) {
        emit_exact_evidence(emitter, iteration.iteration,
                            candidate.stream_index, "candidate",
                            *candidate.exact_evidence);
      }
    }

    line = "{\"record\":\"iteration_outcome\",\"iteration\":" +
           std::to_string(iteration.iteration) +
           ",\"generation_stop_reason\":";
    append_json_string(line, iteration.generation_stop_reason);
    line += ",\"candidates_generated\":" +
            std::to_string(iteration.candidates_generated);
    line += ",\"candidates_scored\":" +
            std::to_string(iteration.candidates_scored);
    line += ",\"candidates_exact_verified\":" +
            std::to_string(iteration.candidates_exact_verified);
    append_json_key(line += ",", "unverified_candidates_may_contain_improvements");
    append_json_bool(line,
                     iteration.unverified_candidates_may_contain_improvements);
    append_json_key(line += ",", "accepted_move_present");
    append_json_bool(line, iteration.accepted_move_present);
    append_json_key(line += ",", "accepted_move_committed");
    append_json_bool(line, iteration.accepted_move_committed);
    append_json_key(line += ",", "post_materialization_rejected");
    append_json_bool(line, iteration.post_materialization_rejected);
    if (iteration.selected_stream_index) {
      line += ",\"selected_stream_index\":" +
              std::to_string(*iteration.selected_stream_index);
    } else {
      line += ",\"selected_stream_index\":null";
    }
    append_json_key(line += ",", "selected_signature");
    append_json_string(line, iteration.selected_signature);
    append_json_key(line += ",", "no_accept_reason");
    append_json_string(line, iteration.no_accept_reason);
    append_json_key(line += ",", "post_materialization_rejection_reason");
    append_json_string(line, iteration.post_materialization_rejection_reason);
    line += ",\"state_score_after\":" +
            std::to_string(iteration.state_score_after) + "}\n";
    emitter.emit(section::acceptance, line);
  }

  std::sort(report.chain_base_production_keys.begin(),
            report.chain_base_production_keys.end());
  report.chain_base_production_keys.erase(
      std::unique(report.chain_base_production_keys.begin(),
                  report.chain_base_production_keys.end()),
      report.chain_base_production_keys.end());
  for (auto const& key : report.chain_base_production_keys) {
    line = "{\"record\":\"chain_base_production\",\"production_key\":";
    append_json_string(line, key);
    line += "}\n";
    emitter.emit(section::chain, line);
  }
  std::sort(report.chain_entries.begin(), report.chain_entries.end(),
            [](auto const& lhs, auto const& rhs) {
              return lhs.position < rhs.position;
            });
  for (auto entry : report.chain_entries) {
    std::sort(entry.added_production_keys.begin(),
              entry.added_production_keys.end());
    entry.added_production_keys.erase(
        std::unique(entry.added_production_keys.begin(),
                    entry.added_production_keys.end()),
        entry.added_production_keys.end());
    std::sort(entry.tombstoned_production_keys.begin(),
              entry.tombstoned_production_keys.end());
    entry.tombstoned_production_keys.erase(
        std::unique(entry.tombstoned_production_keys.begin(),
                    entry.tombstoned_production_keys.end()),
        entry.tombstoned_production_keys.end());
    line = "{\"record\":\"chain_entry\",\"position\":" +
           std::to_string(entry.position);
    append_json_key(line += ",", "commit_source");
    append_json_string(line, entry.commit_source);
    line += ",\"added_production_keys\":";
    append_json_string_array(line, entry.added_production_keys);
    line += ",\"tombstoned_production_keys\":";
    append_json_string_array(line, entry.tombstoned_production_keys);
    line += "}\n";
    emitter.emit(section::chain, line);
  }

  std::sort(report.final_clade_keys.begin(), report.final_clade_keys.end());
  report.final_clade_keys.erase(
      std::unique(report.final_clade_keys.begin(), report.final_clade_keys.end()),
      report.final_clade_keys.end());
  std::sort(report.final_production_keys.begin(),
            report.final_production_keys.end());
  report.final_production_keys.erase(
      std::unique(report.final_production_keys.begin(),
                  report.final_production_keys.end()),
      report.final_production_keys.end());
  line = "{\"record\":\"final_state\",\"final_score\":" +
         std::to_string(report.final_score) + ",\"accepted_moves\":" +
         std::to_string(report.accepted_moves) + "}\n";
  emitter.emit(section::final, line);
  for (auto const& key : report.final_clade_keys) {
    line = "{\"record\":\"final_clade\",\"clade_key\":";
    append_json_string(line, key); line += "}\n";
    emitter.emit(section::final, line);
  }
  for (auto const& key : report.final_production_keys) {
    line = "{\"record\":\"final_production\",\"production_key\":";
    append_json_string(line, key); line += "}\n";
    emitter.emit(section::final, line);
  }
  if (report.final_exact) {
    emit_exact_evidence(emitter,
                        report.iterations.empty()
                            ? 0
                            : report.iterations.back().iteration,
                        (std::numeric_limits<std::size_t>::max)(),
                        "final_state", *report.final_exact);
  }

  chart_spr_semantic_digest_report result;
  result.semantic_sha256 = emitter.overall.hex_digest();
  result.contract_sha256 = emitter.contract.hex_digest();
  result.candidates_sha256 = emitter.candidates.hex_digest();
  result.exact_sha256 = emitter.exact.hex_digest();
  result.acceptance_sha256 = emitter.acceptance.hex_digest();
  result.chain_sha256 = emitter.chain.hex_digest();
  result.final_topology_sha256 = emitter.final.hex_digest();
  result.record_count = emitter.record_count;
  result.candidate_count = candidate_count;
  result.exact_candidate_count = exact_candidate_count;
  result.iteration_count = report.iterations.size();
  result.full_sidecar = std::move(emitter.full);
  return result;
}

inline std::string emit_chart_spr_semantic_digest_json(
    chart_spr_semantic_digest_report const& report) {
  using chart_spr_semantic_detail::append_json_key;
  using chart_spr_semantic_detail::append_json_string;
  std::string out{"{"};
  append_json_key(out, "schema");
  append_json_string(out, "larch.chart_spr.semantic_digest");
  out += ",\"schema_version\":1";
  append_json_key(out += ",", "digest_algorithm");
  append_json_string(out, "sha256");
  append_json_key(out += ",", "payload_encoding");
  append_json_string(out, "larch.chart_spr.semantic.ndjson.v1");
  auto digest_field = [&](std::string_view key, std::string_view value) {
    append_json_key(out += ",", key); append_json_string(out, value);
  };
  digest_field("semantic_sha256", report.semantic_sha256);
  digest_field("contract_sha256", report.contract_sha256);
  digest_field("candidates_sha256", report.candidates_sha256);
  digest_field("exact_sha256", report.exact_sha256);
  digest_field("acceptance_sha256", report.acceptance_sha256);
  digest_field("chain_sha256", report.chain_sha256);
  digest_field("final_topology_sha256", report.final_topology_sha256);
  out += ",\"record_count\":" + std::to_string(report.record_count);
  out += ",\"candidate_count\":" + std::to_string(report.candidate_count);
  out += ",\"exact_candidate_count\":" +
         std::to_string(report.exact_candidate_count);
  out += ",\"iteration_count\":" +
         std::to_string(report.iteration_count) + "}\n";
  return out;
}

struct canonical_dag_digest_report {
  std::string semantic_sha256;
  std::string clades_sha256;
  std::string productions_sha256;
  std::uint64_t parsimony_min = 0;
  std::size_t clade_count = 0;
  std::size_t production_count = 0;
};

// Canonical external-output oracle.  Unlike the chart-search digest, this is
// computed from the DAG that a validation subprocess actually loaded.  It
// deliberately contains no search-internal score or worker/cache metadata.
inline canonical_dag_digest_report build_canonical_dag_digest_report(
    clade_grammar const& grammar, std::uint64_t exact_parsimony_min) {
  auto clades = chart_spr_canonical_grammar_clade_keys(grammar);
  auto productions = chart_spr_canonical_grammar_production_keys(grammar);
  sha256 overall;
  sha256 clade_digest;
  sha256 production_digest;
  std::string line =
      "{\"record\":\"schema\",\"schema\":\"larch.dag.semantic.ndjson\",\"schema_version\":1}\n";
  overall.update(line);
  for (auto const& key : clades) {
    line = "{\"record\":\"clade\",\"sample_key\":";
    chart_spr_semantic_detail::append_json_string(line, key);
    line += "}\n";
    overall.update(line);
    clade_digest.update(line);
  }
  for (auto const& key : productions) {
    line = "{\"record\":\"production\",\"sample_key\":";
    chart_spr_semantic_detail::append_json_string(line, key);
    line += "}\n";
    overall.update(line);
    production_digest.update(line);
  }
  line = "{\"record\":\"exact_parsimony\",\"minimum\":" +
         std::to_string(exact_parsimony_min) + "}\n";
  overall.update(line);
  canonical_dag_digest_report result;
  result.semantic_sha256 = overall.hex_digest();
  result.clades_sha256 = clade_digest.hex_digest();
  result.productions_sha256 = production_digest.hex_digest();
  result.parsimony_min = exact_parsimony_min;
  result.clade_count = clades.size();
  result.production_count = productions.size();
  return result;
}

inline std::string emit_canonical_dag_digest_json(
    canonical_dag_digest_report const& report) {
  using chart_spr_semantic_detail::append_json_key;
  using chart_spr_semantic_detail::append_json_string;
  std::string out{"{"};
  append_json_key(out, "schema");
  append_json_string(out, "larch.dag.semantic_digest");
  out += ",\"schema_version\":1";
  append_json_key(out += ",", "digest_algorithm");
  append_json_string(out, "sha256");
  append_json_key(out += ",", "semantic_sha256");
  append_json_string(out, report.semantic_sha256);
  append_json_key(out += ",", "clades_sha256");
  append_json_string(out, report.clades_sha256);
  append_json_key(out += ",", "productions_sha256");
  append_json_string(out, report.productions_sha256);
  out += ",\"clade_count\":" + std::to_string(report.clade_count);
  out += ",\"production_count\":" +
         std::to_string(report.production_count);
  out += ",\"parsimony_min\":" + std::to_string(report.parsimony_min) +
         "}\n";
  return out;
}

}  // namespace larch

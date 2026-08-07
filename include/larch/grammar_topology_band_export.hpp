#pragma once

#include <larch/grammar_topology_band.hpp>
#include <larch/phylo_dag.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

struct topology_score_band_semantics {
  std::string alignment_sha256;
  std::string ambiguity_policy;
  std::string grammar_construction;
  std::string grammar_digest;
  std::string grammar_semantic_digest;
  std::string input_content_sha256;
  std::string parsimony_model;
  std::string reference_sha256;
  std::uint64_t score_baseline = 0;
  std::string site_pattern_digest;
  std::string taxon_labels_sha256;
  std::string ua_scoring;
  std::string universe;
};

struct topology_score_band_provenance {
  std::string command;
  std::string derivation_ref;
  std::string producer_commit;
  std::string producer_dirty;
  std::string producer_repository;
  std::string provider_input_sha256;
  std::string provider_legacy_dag_semantic_sha256;
  std::string reference_input_sha256;
  std::string seed_tree_input_sha256;
  std::string toolchain;
  // Producer inputs are re-read and hashed by the exporter.  The supplied
  // hashes are replay-manifest expectations, not trusted declarations.
  std::filesystem::path provider_input_path;
  std::filesystem::path reference_input_path;
  std::filesystem::path seed_tree_input_path;
};

struct topology_score_band_derived_semantics {
  std::string alignment_sha256;
  std::string ambiguity_policy;
  std::string grammar_digest;
  std::string grammar_semantic_digest;
  std::string input_content_sha256;
  std::string parsimony_model;
  std::string reference_sha256;
  std::string site_pattern_digest;
  std::string taxon_labels_sha256;
  std::string ua_scoring;
};

struct grammar_topology_band_export_options {
  std::filesystem::path output_directory;
  topology_score_band_semantics semantics;
  topology_score_band_provenance provenance;
  std::string production_origin = "unknown";
};

struct grammar_topology_band_export_row {
  std::uint64_t grammar_ordinal = 0;
  std::uint64_t absolute_score = 0;
  std::string canonical_bytes;
  std::string topology_sha256;
  std::string legacy_selection_sha256;
  std::string legacy_dag_semantic_sha256;
  std::string legacy_dag_clades_sha256;
  std::string legacy_dag_productions_sha256;
};

struct grammar_topology_band_export_result {
  std::filesystem::path output_directory;
  std::string semantics_hash;
  std::string provenance_id;
  std::string semantic_data_sha256;
  std::vector<grammar_topology_band_export_row> selected;
};

// Exact neutral taxon-set identity over byte-sorted labels. This helper lets
// producers populate the raw contract without duplicating its hash domain.
[[nodiscard]] std::string topology_score_band_taxon_labels_sha256(
    std::span<std::string const> labels);

// SHA-256 of exact artifact bytes (compressed bytes remain compressed). Throws
// on open/read failure.
[[nodiscard]] std::string topology_score_band_artifact_sha256(
    std::filesystem::path const& path);

// Derive every scientific content identity and fixed scoring-policy token from
// the actual normalized enumerator/scorer inputs.  The grammar-construction
// policy is included in the ordering-insensitive semantic grammar identity.
// The function also validates the exact taxon order/registry and site mapping
// certified by raw-v1.
[[nodiscard]] topology_score_band_derived_semantics
derive_topology_score_band_semantics(
    phylo_dag& source_dag, clade_grammar const& grammar,
    site_pattern_set const& patterns, chart_options const& chart_options,
    std::string_view grammar_construction);

// Legacy canonical-DAG compatibility identity, including the exact minimum
// edge-mutation parsimony score derived from the supplied provider DAG.
[[nodiscard]] std::uint64_t
topology_score_band_provider_stored_history_parsimony_min(
    phylo_dag& provider_dag, bool score_reference_edge);

[[nodiscard]] std::string
topology_score_band_provider_legacy_dag_semantic_sha256(
    phylo_dag& provider_dag, bool score_reference_edge);

// Serially materialize, score, protobuf-roundtrip, canonicalize, and publish
// one validated raw-v1 score-band handoff. The destination is never replaced.
// The complete census ledger and selected rows must originate from the same
// select_grammar_topology_band result.
[[nodiscard]] grammar_topology_band_export_result
export_grammar_topology_band_raw(
    phylo_dag& source_dag, clade_grammar const& grammar,
    site_pattern_set const& patterns, chart_options const& chart_options,
    grammar_topology_band_result const& band,
    grammar_topology_band_export_options const& options);

}  // namespace larch

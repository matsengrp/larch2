#pragma once

#include <larch/grammar_topology_band_export.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace larch {

struct topology_score_band_replay_manifest {
  std::filesystem::path source_path;
  std::string schema_name;
  std::uint64_t schema_version = 0;
  std::string grammar_construction;
  std::string universe;
  std::string provider_input_sha256;
  std::string provider_legacy_dag_semantic_sha256;
  std::uint64_t provider_stored_history_parsimony_min = 0;
  std::string seed_tree_input_sha256;
  std::string reference_input_sha256;
  topology_score_band_derived_semantics semantics;
  std::uint64_t grammar_topology_count = 0;
  std::uint64_t score_baseline = 0;
  std::uint64_t sankoff_verification_stride = 0;
  std::uint64_t sankoff_verified_topology_count = 0;
  std::filesystem::path prior_census_report_path;
  std::string prior_census_report_sha256;
  std::filesystem::path prior_histogram_path;
  std::string prior_histogram_sha256;
  std::filesystem::path prior_minima_path;
  std::string prior_minima_sha256;
};

struct topology_score_band_replay_minimum {
  std::uint64_t class_index = 0;
  std::uint64_t ordinal = 0;
  std::uint64_t exact_score = 0;
  std::string topology_sha256;
  std::string canonical_dag_semantic_sha256;
  std::string canonical_dag_clades_sha256;
  std::string canonical_dag_productions_sha256;
  std::string artifact_sha256;
};

struct topology_score_band_replay_preflight {
  topology_score_band_derived_semantics derived;
  std::string manifest_sha256;
  std::string provider_input_sha256;
  std::string provider_legacy_dag_semantic_sha256;
  std::uint64_t provider_stored_history_parsimony_min = 0;
  std::string seed_tree_input_sha256;
  std::string reference_input_sha256;
};

[[nodiscard]] topology_score_band_replay_manifest
read_topology_score_band_replay_manifest(
    std::filesystem::path const& path);

// Recompute and bind all identities available before the expensive census.
// The provider DAG must be the object loaded from provider_input_path.
[[nodiscard]] topology_score_band_replay_preflight
validate_topology_score_band_replay_preflight(
    topology_score_band_replay_manifest const& manifest,
    phylo_dag& provider_dag, clade_grammar const& grammar,
    site_pattern_set const& patterns, chart_options const& chart_options,
    std::filesystem::path const& provider_input_path,
    std::filesystem::path const& reference_input_path,
    std::filesystem::path const& seed_tree_input_path);

// Reconcile the fresh complete ledger with the manifest and the independently
// pinned historical report/histogram/minimum artifacts.
void validate_topology_score_band_replay_census(
    topology_score_band_replay_manifest const& manifest,
    grammar_topology_census_result const& census);

// Compare fresh serialized/reloaded minimum witnesses with every semantic and
// raw-artifact identity in the pinned historical minimum ledger.
void validate_topology_score_band_replay_minima(
    topology_score_band_replay_manifest const& manifest,
    std::span<topology_score_band_replay_minimum const> minima);

}  // namespace larch

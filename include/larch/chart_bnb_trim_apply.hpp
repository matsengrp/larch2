#pragma once

#include <larch/chart_trim.hpp>
#include <larch/polytomy_refinement.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace larch {

enum class chart_bnb_trim_application_mode {
  production_mask_superset,
  optimal_topology_materialize,
  // Compact exact Phase-5 packed frontier/provenance annotation.  This is not
  // an ordinary recombining phylo_dag output; it preserves coupling constraints
  // in chart-frontier space.
  annotated_optimal_trim,
};

char const* chart_bnb_trim_application_mode_name(
    chart_bnb_trim_application_mode mode);

struct chart_bnb_trim_apply_options {
  chart_bnb_trim_application_mode mode =
      chart_bnb_trim_application_mode::production_mask_superset;
  bool validate_output_dag = true;
  bool rebuild_and_verify_grammar = true;
  // unset = conservative default cap; explicit 0 from CLI = unlimited.  In
  // annotated_optimal_trim mode this bounds optional small-fixture validation
  // enumeration only; the packed annotation itself is not topology-capped.
  std::optional<std::size_t> max_exact_topologies_to_materialize;

  // Witness-beam bounds forwarded to the witness topology build (see
  // multisite_trim_options::beam_after_taxa).  Requires the class-compressed
  // witness builder; trades enumeration completeness for tractability on
  // grammars whose root-region frontiers are intrinsically huge.  Every
  // emitted topology is still exactly re-scored.
  std::size_t beam_after_taxa = 0;
  std::size_t beam_width = 0;

  // Route the witness build through the class-compressed builder
  // (constant-class factorization; required for the beam above).  larch2
  // mirrors the primary pass's --chart-bnb-class-compress flag here.
  bool class_compressed_score_pass = false;
};

struct chart_bnb_trim_apply_result {
  phylo_dag dag;
  chart_bnb_trim_application_mode mode =
      chart_bnb_trim_application_mode::production_mask_superset;
  bool output_dag_available = true;
  std::string output_artifact_kind = "phylo_dag";
  // Set only for the stronger source-history exact claim; grammar-topology
  // exactness is reported separately below.
  bool topology_exact = false;
  bool grammar_topology_exact = false;
  bool source_history_topology_exact = false;
  bool production_mask_superset = false;
  bool coupled_frontier_exact = false;
  bool annotated_optimal_trim = false;
  bool identity_preserving_tree_set = false;
  // True when topologies were recovered by the score-dominance witness path
  // (build_multisite_optimal_topology_witnesses) instead of the full exact
  // keep-mask provenance enumeration.  Emitted topologies are still exactly
  // re-scored, but the set may be a strict subset of all optimal topologies
  // when a finite cap truncates enumeration.
  bool witness_only_materialization = false;
  std::string refinement_exactness = "EXACT";
  std::size_t source_edges_removed = 0;
  std::size_t source_nodes_removed = 0;
  std::size_t materialized_topologies = 0;
  // Output-artifact topology cap (for topology materialization).  Annotated
  // trim stores a packed annotation and uses validation_topology_cap_truncated
  // for its optional validation enumeration cap.
  bool topology_cap_truncated = false;
  bool validation_topology_cap_truncated = false;
  std::size_t kept_productions_requested = 0;
  std::size_t kept_productions_rebuilt = 0;
  std::size_t masked_productions_reappeared = 0;
  std::uint64_t bnb_optimum = 0;
  std::uint64_t validated_output_parsimony_min = 0;
  bool validated_output_parsimony_min_exact = false;
  std::string validation_oracle;
  std::string validation_strength;
  std::string output_contains_only_optimal_topologies = "unknown";
  std::string validation_status = "not_run";
  bool validation_succeeded = false;
  multisite_coupled_frontier_trim_result coupled_frontier_annotation;
  std::size_t coupled_frontier_entries = 0;
  std::size_t coupled_provenance_choices = 0;
  std::size_t coupled_root_frontier_entries = 0;
};

phylo_dag materialize_grammar_topology_tree(phylo_dag& source_dag,
                                            clade_grammar const& grammar,
                                            grammar_topology const& topology);

phylo_dag merge_grammar_topology_trees_identity_preserving(
    phylo_dag& source_dag, clade_grammar const& grammar,
    std::vector<grammar_topology> const& topologies);

chart_bnb_trim_apply_result apply_chart_bnb_trim(
    phylo_dag& source_dag, polytomy_refinement_result const& refinement,
    site_pattern_set const& patterns, chart_options const& chart_options,
    multisite_trim_result const& trim,
    chart_bnb_trim_apply_options const& options = {});

}  // namespace larch

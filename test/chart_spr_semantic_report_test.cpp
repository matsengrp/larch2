#include <larch/chart_spr_semantic_report.hpp>

#include <stdexcept>
#include <string>

[[noreturn]] static void test_fail(char const* expression, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expression);
}

#define CHECK(expression)                                             \
  do {                                                                \
    if (!(expression)) test_fail(#expression, __FILE__, __LINE__);    \
  } while (false)

static larch::clade_grammar two_sample_grammar(bool reverse_registry) {
  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id =
      reverse_registry ? std::vector<std::string>{"A", "B"}
                       : std::vector<std::string>{"B", "A"};
  grammar.clades = reverse_registry
                       ? std::vector<larch::clade_key>{{{0}}, {{1}}, {{0, 1}}}
                       : std::vector<larch::clade_key>{{{1}}, {{0}}, {{0, 1}}};
  grammar.productions.push_back({.parent = 2, .children = {0, 1}});
  grammar.productions_by_parent = {{}, {}, {0}};
  grammar.productions_by_child = {{0}, {0}, {}};
  grammar.root_clade = 2;
  return grammar;
}

static larch::chart_spr_canonical_report synthetic_report(
    larch::chart_spr_semantic_capture_mode mode) {
  larch::chart_spr_canonical_report report;
  report.capture_mode = mode;
  report.contract.acceptance = "exact\n\"multisite";
  report.contract.objective = "grammar_exact";
  report.contract.candidate_selection = "lower_bound_top_k";
  report.contract.candidate_source = "grammar";
  report.contract.topology_selection = "none";
  report.contract.commit_mode = "overlay_delta";
  report.contract.verification_mode = "cold";
  report.contract.chain_per_accept_exactness =
      "none_conservative_materialize_rebuild";
  report.contract.score_convention =
      "active_cache_plus_single_invariant_offset";
  report.contract.dominance_mode = "off";
  report.contract.keep_mask_contract = "exact_required";
  report.contract.polytomy_mode = "reject";
  report.contract.refinement_exactness = "EXACT";
  report.contract.max_iterations = 1;
  report.contract.max_candidates = 1;
  report.contract.top_k_exact = 1;
  report.contract.seed = 7;
  report.active_pattern_count = 2;
  report.initial_score = 4;

  larch::chart_spr_canonical_iteration_record iteration;
  iteration.seed = 7;
  iteration.state_score_before = 4;
  iteration.state_score_after = 3;
  iteration.generation_stop_reason = "candidate_cap";
  iteration.candidates_generated = 1;
  iteration.candidates_scored = 1;
  iteration.candidates_exact_verified = 1;
  iteration.accepted_move_present = true;
  iteration.accepted_move_committed = true;
  iteration.selected_stream_index = 0;
  iteration.selected_signature = "candidate:A";

  larch::chart_spr_canonical_candidate_record candidate;
  candidate.signature = "candidate:A";
  candidate.affected_clade_count = 3;
  candidate.lower_bound = {
      .kind = "composite_lower_bound",
      .convention = "full_with_invariants",
      .delta = -1,
      .old_score = 4,
      .new_score = 3,
  };
  candidate.ranked_index = 0;
  candidate.exact_verification_index = 0;
  candidate.exact = larch::chart_spr_canonical_score{
      .kind = "grammar_exact",
      .convention = "full_with_invariants",
      .delta = -1,
      .old_score = 4,
      .new_score = 3,
      .exact_multisite = true,
  };
  larch::chart_spr_canonical_exact_evidence evidence;
  evidence.evidence_kind = "grammar_exact_frontier";
  evidence.keep_mask_kind = "exact_optimal_production_union";
  evidence.keep_production_exact = true;
  evidence.optimum_active = 3;
  evidence.kept_production_keys = {"z", "a", "a"};
  evidence.frontier_sizes = {{"clade-b", 2}, {"clade-a", 1}};
  evidence.optimal_root_provenance_classes = {
      {.cost = {1, 0}, .production_keys = {"z", "a"}}};
  candidate.exact_evidence = evidence;
  iteration.candidates.push_back(candidate);
  iteration.ranked_stream_indices.push_back(0);
  iteration.exact_verified_stream_indices.push_back(0);
  report.iterations.push_back(iteration);
  report.final_score = 3;
  report.accepted_moves = 1;
  report.final_clade_keys = {"z", "a"};
  report.final_production_keys = {"p"};
  report.final_exact = evidence;
  return report;
}

int main() {
  larch::sha256 empty;
  CHECK(empty.hex_digest() ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  larch::sha256 abc;
  abc.update("a");
  abc.update("bc");
  CHECK(abc.hex_digest() ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  larch::sha256 long_vector;
  long_vector.update(
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  CHECK(long_vector.hex_digest() ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  auto full = larch::build_chart_spr_semantic_digest_report(
      synthetic_report(larch::chart_spr_semantic_capture_mode::full));
  auto digest_only = larch::build_chart_spr_semantic_digest_report(
      synthetic_report(larch::chart_spr_semantic_capture_mode::digest));
  CHECK(!full.full_sidecar.empty());
  CHECK(digest_only.full_sidecar.empty());
  CHECK(full.semantic_sha256 == digest_only.semantic_sha256);
  larch::sha256 sidecar_sha;
  sidecar_sha.update(full.full_sidecar);
  CHECK(sidecar_sha.hex_digest() == full.semantic_sha256);
  CHECK(full.candidate_count == 1);
  CHECK(full.exact_candidate_count == 1);
  CHECK(full.full_sidecar.find("exact\\n\\\"multisite") !=
        std::string::npos);

  auto changed = synthetic_report(larch::chart_spr_semantic_capture_mode::full);
  changed.iterations.front().candidates.front().lower_bound.new_score = 2;
  CHECK(larch::build_chart_spr_semantic_digest_report(std::move(changed))
            .semantic_sha256 != full.semantic_sha256);

  auto grammar1 = two_sample_grammar(false);
  auto grammar2 = two_sample_grammar(true);
  auto dag1 = larch::build_canonical_dag_digest_report(grammar1, 5);
  auto dag2 = larch::build_canonical_dag_digest_report(grammar2, 5);
  CHECK(dag1.semantic_sha256 == dag2.semantic_sha256);
  CHECK(dag1.clades_sha256 == dag2.clades_sha256);
  CHECK(dag1.productions_sha256 == dag2.productions_sha256);
  CHECK(dag1.clade_count == 3);
  CHECK(dag1.production_count == 1);
  auto dag3 = larch::build_canonical_dag_digest_report(grammar2, 6);
  CHECK(dag1.semantic_sha256 != dag3.semantic_sha256);
}

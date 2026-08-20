#include <larch/chart_spr.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/polytomy_refinement.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <latch>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file, int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr) \
  do { \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

template <typename Base>
concept can_prepare_sampled_tree_projection =
    requires(Base&& base, larch::phylo_dag& tree) {
      larch::chart_spr_detail::prepare_sampled_tree_projection(
          std::forward<Base>(base), tree);
    };

using sampled_tree_projection_context =
    larch::chart_spr_detail::sampled_tree_projection_context;
static_assert(!std::is_copy_constructible_v<sampled_tree_projection_context>);
static_assert(!std::is_move_constructible_v<sampled_tree_projection_context>);
static_assert(can_prepare_sampled_tree_projection<larch::clade_grammar&>);
static_assert(!can_prepare_sampled_tree_projection<larch::clade_grammar>);
static_assert(std::is_same_v<
              decltype(std::declval<sampled_tree_projection_context const&>()
                           .source_tree()),
              larch::phylo_dag const&>);

static larch::chart_scheduler make_projection_scheduler(std::size_t workers) {
  return larch::chart_scheduler{
      larch::chart_scheduler_options{.requested_workers = workers,
                                     .default_minimum_grain = 1,
                                     .default_target_ranges_per_worker = 1},
      larch::chart_worker_topology_snapshot{
          .affinity_logical_cpu_count = workers,
          .affinity_physical_core_count = workers,
          .hardware_thread_count = workers}};
}

static larch::test::tiny_tree_node four_taxon_base_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node four_taxon_base_tree_root_swapped() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")}),
       tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")})});
}

static larch::test::tiny_tree_node four_taxon_cross_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node eight_taxon_balanced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner(
           "ABCD", "A",
           {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
            tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})}),
       tiny_inner(
           "EFGH", "A",
           {tiny_inner("EF", "A", {tiny_leaf("E", "A"), tiny_leaf("F", "A")}),
            tiny_inner("GH", "C",
                       {tiny_leaf("G", "C"), tiny_leaf("H", "C")})})});
}

static larch::test::tiny_tree_node eight_taxon_distinct_balanced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner(
           "ABCD", "AA",
           {tiny_inner("AB", "AA",
                       {tiny_leaf("A", "AA"), tiny_leaf("B", "AC")}),
            tiny_inner("CD", "AG",
                       {tiny_leaf("C", "AG"), tiny_leaf("D", "AT")})}),
       tiny_inner(
           "EFGH", "CA",
           {tiny_inner("EF", "CA",
                       {tiny_leaf("E", "CA"), tiny_leaf("F", "CC")}),
            tiny_inner("GH", "CG",
                       {tiny_leaf("G", "CG"), tiny_leaf("H", "CT")})})});
}

static larch::taxon_id taxon_for(larch::clade_grammar const& grammar,
                                 std::string const& sample_id) {
  auto it = grammar.taxa.sample_id_to_id.find(sample_id);
  CHECK(it != grammar.taxa.sample_id_to_id.end());
  return it->second;
}

static std::vector<larch::taxon_id> taxa_for(
    larch::clade_grammar const& grammar, std::vector<std::string> sample_ids) {
  std::vector<larch::taxon_id> ids;
  ids.reserve(sample_ids.size());
  for (auto const& sample_id : sample_ids) ids.push_back(taxon_for(grammar, sample_id));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

static larch::clade_id clade_for(larch::clade_grammar const& grammar,
                                 std::vector<std::string> sample_ids) {
  auto ids = taxa_for(grammar, std::move(sample_ids));
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa == ids) return static_cast<larch::clade_id>(cid);
  }
  CHECK(false && "missing clade");
  return larch::no_clade;
}

static larch::production_id production_id_for(
    larch::clade_grammar const& grammar, larch::clade_id parent,
    std::vector<larch::clade_id> children) {
  std::sort(children.begin(), children.end());
  for (auto pid : grammar.productions_by_parent[parent]) {
    auto prod_children = grammar.productions[pid].children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == children) return pid;
  }
  CHECK(false && "missing production");
  return larch::no_production;
}

static std::size_t leaf_node_for(larch::phylo_dag& dag,
                                 std::string const& sample_id) {
  for (auto nv : dag.get_all_nodes()) {
    std::optional<std::size_t> found;
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            if (node.sample_id() == sample_id) found = node.index();
          }
        },
        nv);
    if (found) return *found;
  }
  CHECK(false && "missing leaf node");
  return 0;
}

static std::string taxa_key(std::vector<larch::taxon_id> taxa) {
  std::sort(taxa.begin(), taxa.end());
  std::ostringstream out;
  for (auto taxon : taxa) out << taxon << ',';
  return out.str();
}

static std::set<std::string> clade_shape(larch::clade_grammar const& grammar) {
  std::set<std::string> result;
  for (auto const& clade : grammar.clades) result.insert(taxa_key(clade.taxa));
  return result;
}

static std::set<std::string> production_shape(larch::clade_grammar const& grammar) {
  std::set<std::string> result;
  for (auto const& prod : grammar.productions) {
    std::vector<std::string> children;
    for (auto child : prod.children) children.push_back(taxa_key(grammar.clades[child].taxa));
    std::sort(children.begin(), children.end());
    std::ostringstream out;
    out << taxa_key(grammar.clades[prod.parent].taxa) << "->";
    for (auto const& child : children) out << child << '|';
    result.insert(out.str());
  }
  return result;
}

struct projection_source_node_snapshot {
  std::size_t index = 0;
  int kind = 0;
  larch::compact_genome compact_genome;
  std::string sample_id;

  bool operator==(projection_source_node_snapshot const&) const = default;
};

struct projection_source_edge_snapshot {
  std::size_t index = 0;
  std::size_t parent = 0;
  std::size_t child = 0;
  std::size_t clade_index = 0;
  float edge_weight = 0;
  larch::edge_mutations mutations;

  bool operator==(projection_source_edge_snapshot const&) const = default;
};

struct projection_source_snapshot {
  std::size_t root = 0;
  std::size_t node_high_mark = 0;
  std::size_t edge_high_mark = 0;
  std::string reference;
  std::vector<projection_source_node_snapshot> nodes;
  std::vector<projection_source_edge_snapshot> edges;

  bool operator==(projection_source_snapshot const&) const = default;
};

static projection_source_snapshot snapshot_projection_source(
    larch::phylo_dag& tree) {
  projection_source_snapshot result;
  result.root = larch::get_root_idx(tree);
  result.node_high_mark = tree.node_high_mark();
  result.edge_high_mark = tree.edge_high_mark();
  result.reference = larch::get_reference_sequence(tree);
  for (auto node_variant : tree.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          projection_source_node_snapshot snapshot;
          snapshot.index = node.index();
          if constexpr (requires { node.reference_sequence(); }) {
            snapshot.kind = 0;
          } else if constexpr (requires {
                                 node.sample_id();
                                 node.cg();
                               }) {
            snapshot.kind = 2;
            snapshot.compact_genome = node.cg();
            snapshot.sample_id = node.sample_id();
          } else {
            snapshot.kind = 1;
            snapshot.compact_genome = node.cg();
          }
          result.nodes.push_back(std::move(snapshot));
        },
        node_variant);
  }
  for (auto edge_variant : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          result.edges.push_back(projection_source_edge_snapshot{
              .index = edge.index(),
              .parent = larch::get_parent_idx(tree, edge.index()),
              .child = larch::get_child_idx(tree, edge.index()),
              .clade_index = edge.clade_index(),
              .edge_weight = edge.edge_weight(),
              .mutations = edge.mutations()});
        },
        edge_variant);
  }
  return result;
}

static void check_projection_clone_annotations_are_sparse(
    larch::phylo_dag& tree) {
  std::size_t edge_count = 0;
  for (auto edge_variant : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          ++edge_count;
          CHECK(edge.mutations().empty());
          CHECK(edge.edge_weight() == 0.0F);
        },
        edge_variant);
  }
  CHECK(edge_count > 0);

  for (auto node_variant : tree.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (
              requires { node.cg(); } && !requires { node.sample_id(); }) {
            CHECK(node.cg() == larch::compact_genome{});
          }
        },
        node_variant);
  }
}

static void check_production_witness_equal(
    larch::production_witness const& lhs,
    larch::production_witness const& rhs) {
  CHECK(lhs.parent_node == rhs.parent_node);
  CHECK(lhs.children.size() == rhs.children.size());
  for (std::size_t i = 0; i < lhs.children.size(); ++i) {
    CHECK(lhs.children[i].child == rhs.children[i].child);
    CHECK(lhs.children[i].edge_alternatives ==
          rhs.children[i].edge_alternatives);
  }
}

static void check_overlay_production_equal(
    larch::overlay_grammar_production const& lhs,
    larch::overlay_grammar_production const& rhs) {
  CHECK(lhs.parent == rhs.parent);
  CHECK(lhs.children == rhs.children);
  CHECK(lhs.multiplicity == rhs.multiplicity);
  CHECK(lhs.witnesses.size() == rhs.witnesses.size());
  for (std::size_t i = 0; i < lhs.witnesses.size(); ++i) {
    check_production_witness_equal(lhs.witnesses[i], rhs.witnesses[i]);
  }
}

static void check_optional_source_move_equal(
    std::optional<larch::spr_move> const& lhs,
    std::optional<larch::spr_move> const& rhs) {
  CHECK(lhs.has_value() == rhs.has_value());
  if (!lhs) return;
  CHECK(lhs->src == rhs->src);
  CHECK(lhs->dst == rhs->dst);
  CHECK(lhs->lca == rhs->lca);
  CHECK(lhs->score_change == rhs->score_change);
}

static void check_candidate_projection_fields_equal(
    larch::grammar_spr_candidate const& lhs,
    larch::grammar_spr_candidate const& rhs) {
  CHECK(lhs.moved_clade == rhs.moved_clade);
  CHECK(lhs.old_parent == rhs.old_parent);
  CHECK(lhs.old_sibling == rhs.old_sibling);
  CHECK(lhs.new_sibling_or_target == rhs.new_sibling_or_target);
  CHECK(lhs.removed_productions == rhs.removed_productions);
  CHECK(lhs.added_clades == rhs.added_clades);
  CHECK(lhs.added_productions.size() == rhs.added_productions.size());
  for (std::size_t i = 0; i < lhs.added_productions.size(); ++i) {
    check_overlay_production_equal(lhs.added_productions[i],
                                   rhs.added_productions[i]);
  }
  check_optional_source_move_equal(lhs.source_tree_move, rhs.source_tree_move);
  CHECK(lhs.source_before_topology_productions ==
        rhs.source_before_topology_productions);
  CHECK(lhs.source_after_topology_productions ==
        rhs.source_after_topology_productions);
}

static void check_candidate_payload_equal(
    larch::clade_grammar const& base, larch::grammar_spr_candidate const& lhs,
    larch::grammar_spr_candidate const& rhs) {
  check_candidate_projection_fields_equal(lhs, rhs);
  CHECK(larch::chart_spr_candidate_taxon_signature(base, lhs) ==
        larch::chart_spr_candidate_taxon_signature(base, rhs));
  CHECK(larch::chart_spr_candidate_sample_signature(base, lhs) ==
        larch::chart_spr_candidate_sample_signature(base, rhs));

  auto lhs_materialized = larch::materialize_overlay_grammar(
      larch::overlay_from_candidate(base, lhs));
  auto rhs_materialized = larch::materialize_overlay_grammar(
      larch::overlay_from_candidate(base, rhs));
  CHECK(clade_shape(lhs_materialized.grammar) ==
        clade_shape(rhs_materialized.grammar));
  CHECK(production_shape(lhs_materialized.grammar) ==
        production_shape(rhs_materialized.grammar));
}

static void check_legacy_generation_stats_equal(
    larch::chart_spr_candidate_generation_stats const& lhs,
    larch::chart_spr_candidate_generation_stats const& rhs) {
  CHECK(lhs.candidates_generated_after_dedup ==
        rhs.candidates_generated_after_dedup);
  CHECK(lhs.upward_path_iterator_steps == rhs.upward_path_iterator_steps);
  CHECK(lhs.upward_paths_completed == rhs.upward_paths_completed);
  CHECK(lhs.path_pairs_considered == rhs.path_pairs_considered);
  CHECK(lhs.candidates_constructed == rhs.candidates_constructed);
  CHECK(lhs.candidates_pruned_before_construction ==
        rhs.candidates_pruned_before_construction);
  CHECK(lhs.candidates_pruned_after_construction ==
        rhs.candidates_pruned_after_construction);
  CHECK(lhs.candidates_pruned_root_or_trivial ==
        rhs.candidates_pruned_root_or_trivial);
  CHECK(lhs.candidates_pruned_moved_size == rhs.candidates_pruned_moved_size);
  CHECK(lhs.candidates_pruned_target_size == rhs.candidates_pruned_target_size);
  CHECK(lhs.candidates_pruned_overlap == rhs.candidates_pruned_overlap);
  CHECK(lhs.candidates_pruned_affected_estimate ==
        rhs.candidates_pruned_affected_estimate);
  CHECK(lhs.candidates_pruned_immediate_reversal ==
        rhs.candidates_pruned_immediate_reversal);
  CHECK(lhs.candidates_pruned_duplicate == rhs.candidates_pruned_duplicate);
  CHECK(lhs.candidates_pruned_invalid == rhs.candidates_pruned_invalid);
  CHECK(lhs.spr_multifurcation_moves_generated ==
        rhs.spr_multifurcation_moves_generated);
  CHECK(lhs.stop_reason == rhs.stop_reason);
}

// Reservoir publication happens only after an exhaustive child stream has
// completed. These counters distinguish that contract from a finite-cap child
// which happened to select the same final candidates. Scheduler stall
// durations are deliberately excluded because they are wall-clock values.
static void check_exhaustive_generation_work_equal(
    larch::chart_spr_candidate_generation_stats const& lhs,
    larch::chart_spr_candidate_generation_stats const& rhs) {
  check_legacy_generation_stats_equal(lhs, rhs);
  CHECK(lhs.sampled_tree_projection_moves_preassigned ==
        rhs.sampled_tree_projection_moves_preassigned);
  CHECK(lhs.sampled_tree_projection_move_enumeration_visits ==
        rhs.sampled_tree_projection_move_enumeration_visits);
  CHECK(lhs.sampled_tree_projection_enumeration_passes ==
        rhs.sampled_tree_projection_enumeration_passes);
  CHECK(lhs.sampled_tree_projection_waves == rhs.sampled_tree_projection_waves);
  CHECK(lhs.sampled_tree_projection_scheduler_operations ==
        rhs.sampled_tree_projection_scheduler_operations);
  CHECK(lhs.sampled_tree_projection_parallel_operations ==
        rhs.sampled_tree_projection_parallel_operations);
  CHECK(lhs.sampled_tree_projection_ranges ==
        rhs.sampled_tree_projection_ranges);
  CHECK(lhs.sampled_tree_projection_worker_tasks ==
        rhs.sampled_tree_projection_worker_tasks);
  CHECK(lhs.sampled_tree_projection_peak_wave_size ==
        rhs.sampled_tree_projection_peak_wave_size);
  CHECK(lhs.sampled_tree_projection_speculative_discarded ==
        rhs.sampled_tree_projection_speculative_discarded);
  CHECK(lhs.sampled_tree_projection_direct ==
        rhs.sampled_tree_projection_direct);
  CHECK(lhs.sampled_tree_projection_fallback ==
        rhs.sampled_tree_projection_fallback);
  CHECK(lhs.sampled_tree_projection_estimated_peak_bytes ==
        rhs.sampled_tree_projection_estimated_peak_bytes);
  CHECK(lhs.sampled_tree_projection_cancellations ==
        rhs.sampled_tree_projection_cancellations);
  CHECK(lhs.sampled_tree_source_waves == rhs.sampled_tree_source_waves);
  CHECK(lhs.sampled_tree_sources_enumerated ==
        rhs.sampled_tree_sources_enumerated);
  CHECK(lhs.sampled_tree_source_enumeration_operations ==
        rhs.sampled_tree_source_enumeration_operations);
  CHECK(lhs.sampled_tree_source_enumeration_parallel_operations ==
        rhs.sampled_tree_source_enumeration_parallel_operations);
  CHECK(lhs.sampled_tree_source_enumeration_ranges ==
        rhs.sampled_tree_source_enumeration_ranges);
  CHECK(lhs.sampled_tree_source_enumeration_worker_tasks ==
        rhs.sampled_tree_source_enumeration_worker_tasks);
  CHECK(lhs.sampled_tree_source_peak_wave_size ==
        rhs.sampled_tree_source_peak_wave_size);
  CHECK(lhs.sampled_tree_source_one_pass_move_visits ==
        rhs.sampled_tree_source_one_pass_move_visits);
  CHECK(lhs.sampled_tree_source_speculative_moves_discarded ==
        rhs.sampled_tree_source_speculative_moves_discarded);
  CHECK(lhs.sampled_tree_source_speculative_sources_discarded ==
        rhs.sampled_tree_source_speculative_sources_discarded);
  CHECK(lhs.sampled_tree_source_admitted_wave_width ==
        rhs.sampled_tree_source_admitted_wave_width);
  CHECK(lhs.sampled_tree_projection_admitted_subwave_width ==
        rhs.sampled_tree_projection_admitted_subwave_width);
  CHECK(lhs.sampled_tree_source_actual_peak_bytes ==
        rhs.sampled_tree_source_actual_peak_bytes);
  CHECK(lhs.grammar_candidate_construction_waves ==
        rhs.grammar_candidate_construction_waves);
  CHECK(lhs.grammar_candidate_scheduler_operations ==
        rhs.grammar_candidate_scheduler_operations);
  CHECK(lhs.grammar_candidate_parallel_operations ==
        rhs.grammar_candidate_parallel_operations);
  CHECK(lhs.grammar_candidate_ranges == rhs.grammar_candidate_ranges);
  CHECK(lhs.grammar_candidate_worker_tasks ==
        rhs.grammar_candidate_worker_tasks);
  CHECK(lhs.grammar_candidate_peak_wave_size ==
        rhs.grammar_candidate_peak_wave_size);
  CHECK(lhs.grammar_candidate_speculative_discarded ==
        rhs.grammar_candidate_speculative_discarded);
  CHECK(lhs.grammar_candidate_admitted_wave_width ==
        rhs.grammar_candidate_admitted_wave_width);
  CHECK(lhs.grammar_candidate_actual_peak_bytes ==
        rhs.grammar_candidate_actual_peak_bytes);
  CHECK(lhs.grammar_candidate_cancellations ==
        rhs.grammar_candidate_cancellations);
}

// Independent oracle for the pre-preparation projection algorithm.  It
// intentionally performs the annotated legacy SPR edit and recomputes every
// before-side lookup so differential tests can detect a bad prepared cache or
// an annotation-dependent topology result.
static std::optional<larch::grammar_spr_candidate>
project_tree_spr_move_annotated_oracle(larch::clade_grammar const& base,
                                       larch::phylo_dag& tree,
                                       larch::spr_move const& move) {
  using namespace larch;
  using namespace larch::chart_spr_detail;

  build_clade_offsets(tree);
  auto before_tree_grammar = build_clade_grammar(tree);
  auto base_lookup = build_clade_lookup(base);
  tree_index index{tree};
  if (!index.is_valid(move.src) || !index.is_valid(move.dst))
    return std::nullopt;
  if (move.src == move.dst || move.src == index.get_tree_root())
    return std::nullopt;
  if (index.is_ancestor(move.src, move.dst)) return std::nullopt;

  auto src_parent_node = index.get_parent(move.src);
  if (index.get_num_children(src_parent_node) != 2) return std::nullopt;
  auto moved =
      tree_node_to_base_clade(base, before_tree_grammar, base_lookup, move.src);
  auto old_parent = tree_node_to_base_clade(base, before_tree_grammar,
                                            base_lookup, src_parent_node);
  auto target =
      tree_node_to_base_clade(base, before_tree_grammar, base_lookup, move.dst);
  if (!moved || !old_parent || !target) return std::nullopt;

  std::optional<clade_id> old_sibling;
  for (auto child : index.get_children(src_parent_node)) {
    if (child == move.src) continue;
    old_sibling =
        tree_node_to_base_clade(base, before_tree_grammar, base_lookup, child);
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
  return make_candidate_from_tree_diff(
      base, before_tree_grammar, after_tree_grammar, std::move(candidate));
}

static std::vector<larch::grammar_spr_candidate> collect_candidates(
    larch::clade_grammar const& grammar,
    larch::grammar_spr_enumeration_options options,
    larch::chart_spr_candidate_generation_stats* stats_out = nullptr) {
  std::vector<larch::grammar_spr_candidate> candidates;
  auto stats = larch::for_each_grammar_spr_candidate(
      grammar, options, [&](larch::grammar_spr_candidate const& candidate) {
        candidates.push_back(candidate);
        return true;
      });
  if (stats_out != nullptr) *stats_out = stats;
  return candidates;
}

static std::vector<std::string> collect_candidate_signature_vector(
    larch::clade_grammar const& grammar,
    larch::grammar_spr_enumeration_options options,
    larch::chart_spr_candidate_generation_stats* stats_out = nullptr) {
  auto candidates = collect_candidates(grammar, options, stats_out);
  std::vector<std::string> signatures;
  signatures.reserve(candidates.size());
  for (auto const& candidate : candidates) {
    signatures.push_back(
        larch::chart_spr_candidate_taxon_signature(grammar, candidate));
  }
  return signatures;
}

static std::set<std::string> collect_candidate_signature_set(
    larch::clade_grammar const& grammar,
    larch::grammar_spr_enumeration_options options) {
  auto signatures = collect_candidate_signature_vector(grammar, options);
  return std::set<std::string>(signatures.begin(), signatures.end());
}

static larch::overlay_grammar_production temp_prod(
    larch::overlay_clade_ref parent,
    std::vector<larch::overlay_clade_ref> children) {
  std::sort(children.begin(), children.end());
  larch::overlay_grammar_production prod;
  prod.parent = parent;
  prod.children = std::move(children);
  prod.multiplicity = 1;
  return prod;
}

static larch::grammar_spr_candidate cross_candidate(
    larch::clade_grammar const& grammar) {
  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto root = grammar.root_clade;
  auto root_pid = production_id_for(grammar, root, {ab, cd});

  larch::grammar_spr_candidate candidate;
  candidate.moved_clade = larch::base_clade_ref(a);
  candidate.old_parent = larch::base_clade_ref(ab);
  candidate.old_sibling = larch::base_clade_ref(b);
  candidate.new_sibling_or_target = larch::base_clade_ref(c);
  candidate.removed_productions.push_back(larch::base_production_ref(root_pid));
  candidate.added_clades.push_back(larch::clade_key{taxa_for(grammar, {"A", "C"})});
  candidate.added_clades.push_back(larch::clade_key{taxa_for(grammar, {"B", "D"})});
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(0), {larch::base_clade_ref(a), larch::base_clade_ref(c)}));
  candidate.added_productions.push_back(
      temp_prod(larch::temp_clade_ref(1), {larch::base_clade_ref(b), larch::base_clade_ref(d)}));
  candidate.added_productions.push_back(
      temp_prod(larch::base_clade_ref(root), {larch::temp_clade_ref(0), larch::temp_clade_ref(1)}));
  return candidate;
}

static void test_phase8_binary_taxon_dedup_key_matches_legacy() {
  std::println("test_phase8_binary_taxon_dedup_key_matches_legacy");

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto dag = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(dag);

  std::size_t generated_checked = 0;
  std::vector<std::pair<std::string, std::string>> generated_keys;
  CHECK(std::locale{} == std::locale::classic());
  for (auto source : {larch::chart_spr_candidate_source::grammar,
                      larch::chart_spr_candidate_source::sampled_tree,
                      larch::chart_spr_candidate_source::hybrid}) {
    for (auto seed : {std::uint32_t{1}, std::uint32_t{7},
                      std::uint32_t{19}}) {
      larch::grammar_spr_enumeration_options options;
      options.source = source;
      options.sampled_tree_source_dag = &dag;
      options.sampled_tree_count = 1;
      options.sampled_tree_spr_radius = 8;
      options.sampled_tree_score_threshold =
          std::numeric_limits<int>::max();
      options.randomize_order = true;
      options.seed = seed;
      options.max_candidates = 12;
      options.max_candidates_is_post_dedup = true;

      auto candidates = collect_candidates(grammar, options);
      CHECK(!candidates.empty());
      for (auto const& candidate : candidates) {
        auto const legacy =
            larch::chart_spr_candidate_taxon_signature(grammar, candidate);
        auto const binary = larch::chart_spr_detail::
            chart_spr_candidate_taxon_dedup_key(grammar, candidate);
        CHECK(!binary.empty());
        CHECK(binary.front() == '\0');
        generated_keys.emplace_back(legacy, binary);
        ++generated_checked;
      }
    }
  }
  CHECK(generated_checked > 0);
  for (auto const& lhs : generated_keys) {
    for (auto const& rhs : generated_keys) {
      CHECK((lhs.first == rhs.first) == (lhs.second == rhs.second));
    }
  }

  auto check_binary_equality_equivalence =
      [&](auto const& lhs, auto const& rhs) {
        auto const legacy_equal =
            larch::chart_spr_candidate_taxon_signature(grammar, lhs) ==
            larch::chart_spr_candidate_taxon_signature(grammar, rhs);
        auto const binary_equal =
            larch::chart_spr_detail::chart_spr_candidate_taxon_dedup_key(
                grammar, lhs) ==
            larch::chart_spr_detail::chart_spr_candidate_taxon_dedup_key(
                grammar, rhs);
        CHECK(binary_equal == legacy_equal);
        return legacy_equal;
      };

  // Exercise decimal-width boundaries and ordering domains explicitly.  The
  // temp-ref order differs from numeric child-taxa order, and bytewise whole-
  // production ordering puts the signature beginning with "{10," before the
  // one beginning with "{9,".
  auto const max_taxon = std::numeric_limits<larch::taxon_id>::max();
  larch::grammar_spr_candidate edge_ids;
  edge_ids.added_clades = {
      larch::clade_key{{10}},
      larch::clade_key{{9}},
      larch::clade_key{{max_taxon}},
      larch::clade_key{{0, 9, 10, max_taxon}},
  };
  edge_ids.moved_clade = larch::temp_clade_ref(2);
  edge_ids.old_parent = larch::temp_clade_ref(0);
  edge_ids.old_sibling = larch::temp_clade_ref(1);
  edge_ids.new_sibling_or_target = larch::temp_clade_ref(3);
  edge_ids.added_productions.push_back(temp_prod(
      larch::temp_clade_ref(1),
      {larch::temp_clade_ref(0), larch::temp_clade_ref(2)}));
  edge_ids.added_productions.push_back(temp_prod(
      larch::temp_clade_ref(0),
      {larch::temp_clade_ref(2), larch::temp_clade_ref(1)}));

  auto const expected_edge_signature =
      std::string{"m={4294967295,};op={10,};os={9,};"
                  "nt={0,9,10,4294967295,};"
                  "clades={0,9,10,4294967295,}{9,}{10,}{4294967295,};"
                  "rm=;add={10,}->{9,}{4294967295,};"
                  "{9,}->{10,}{4294967295,};"};
  CHECK(larch::chart_spr_candidate_taxon_signature(grammar, edge_ids) ==
        expected_edge_signature);
  CHECK(check_binary_equality_equivalence(edge_ids, edge_ids));

  auto unnormalized = edge_ids;
  unnormalized.added_clades[3].taxa = {max_taxon, 10, 9, 10, 0};
  CHECK(larch::chart_spr_candidate_taxon_signature(grammar, unnormalized) ==
        expected_edge_signature);
  CHECK(check_binary_equality_equivalence(edge_ids, unnormalized));

  // Normalization applies inside optional refs and production parent/child
  // keys as well as the standalone added-clade multiset.
  auto malformed_production_taxa = edge_ids;
  malformed_production_taxa.added_clades[0].taxa = {10, 10};
  malformed_production_taxa.added_clades[1].taxa = {9, 9};
  malformed_production_taxa.added_clades[2].taxa = {max_taxon, max_taxon};
  CHECK(larch::chart_spr_candidate_taxon_signature(
            grammar, malformed_production_taxa) == expected_edge_signature);
  CHECK(check_binary_equality_equivalence(edge_ids,
                                          malformed_production_taxa));

  // Added clades, child refs, and productions are multisets in the legacy
  // identity. Their stored order is irrelevant, but duplicate elements remain
  // significant. Keep the decimal 9/10 edge in this permutation matrix so a
  // binary implementation cannot accidentally substitute string ordering for
  // numeric child-taxa ordering.
  auto permuted = edge_ids;
  std::reverse(permuted.added_productions.begin(),
               permuted.added_productions.end());
  for (auto& production : permuted.added_productions) {
    std::reverse(production.children.begin(), production.children.end());
  }
  CHECK(check_binary_equality_equivalence(edge_ids, permuted));

  larch::grammar_spr_candidate clade_multiset;
  clade_multiset.added_clades = {
      larch::clade_key{{10}}, larch::clade_key{{9}},
      larch::clade_key{{max_taxon}}};
  auto clade_multiset_permuted = clade_multiset;
  std::reverse(clade_multiset_permuted.added_clades.begin(),
               clade_multiset_permuted.added_clades.end());
  CHECK(check_binary_equality_equivalence(clade_multiset,
                                          clade_multiset_permuted));
  auto duplicate_clade = clade_multiset;
  duplicate_clade.added_clades.push_back(duplicate_clade.added_clades.front());
  CHECK(!check_binary_equality_equivalence(clade_multiset, duplicate_clade));

  auto duplicate_production = edge_ids;
  duplicate_production.added_productions.push_back(
      duplicate_production.added_productions.front());
  CHECK(!check_binary_equality_equivalence(edge_ids, duplicate_production));
  auto duplicate_child = edge_ids;
  duplicate_child.added_productions.front().children.push_back(
      duplicate_child.added_productions.front().children.front());
  CHECK(!check_binary_equality_equivalence(edge_ids, duplicate_child));

  auto swapped_optional_fields = edge_ids;
  std::swap(swapped_optional_fields.moved_clade,
            swapped_optional_fields.old_parent);
  CHECK(!check_binary_equality_equivalence(edge_ids,
                                           swapped_optional_fields));

  // The text key intentionally makes a missing optional ref equivalent to a
  // valid ref whose normalized taxa are empty. Preserve even this unusual
  // malformed-candidate equivalence.
  larch::grammar_spr_candidate empty_optional;
  empty_optional.added_clades.push_back(larch::clade_key{{}});
  auto explicit_empty_optional = empty_optional;
  explicit_empty_optional.moved_clade = larch::temp_clade_ref(0);
  CHECK(check_binary_equality_equivalence(empty_optional,
                                          explicit_empty_optional));

  CHECK(grammar.productions.size() >= 2);
  auto removed_forward = edge_ids;
  removed_forward.removed_productions = {
      larch::base_production_ref(0), larch::base_production_ref(1)};
  auto removed_reverse = removed_forward;
  std::reverse(removed_reverse.removed_productions.begin(),
               removed_reverse.removed_productions.end());
  auto const sorted_removed_signature =
      larch::chart_spr_candidate_taxon_signature(grammar, removed_forward);
  CHECK(larch::chart_spr_candidate_taxon_signature(grammar, removed_reverse) ==
        sorted_removed_signature);
  CHECK(check_binary_equality_equivalence(removed_forward, removed_reverse));
  auto duplicate_removed = removed_forward;
  duplicate_removed.removed_productions.push_back(
      duplicate_removed.removed_productions.front());
  CHECK(!check_binary_equality_equivalence(removed_forward,
                                           duplicate_removed));

  // The legacy ostream formatter inherits the process-global locale. Binary
  // dedup serialization must therefore fall back when a custom numeric facet
  // makes that locale observable, rather than changing W>1 dedup identities.
  struct grouped_taxon_ids : std::numpunct<char> {
   protected:
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
  };
  struct restore_global_locale {
    std::locale previous;
    ~restore_global_locale() { std::locale::global(previous); }
  };
  {
    restore_global_locale restore{std::locale{}};
    std::locale::global(
        std::locale{std::locale::classic(), new grouped_taxon_ids});
    auto const localized_legacy =
        larch::chart_spr_candidate_taxon_signature(grammar, edge_ids);
    CHECK(localized_legacy.find('_') != std::string::npos);
    CHECK(larch::chart_spr_detail::chart_spr_candidate_taxon_dedup_key(
              grammar, edge_ids) == localized_legacy);
  }

  auto exception_message = [](auto&& serialize) -> std::string {
    try {
      (void)serialize();
    } catch (std::runtime_error const& error) {
      return error.what();
    }
    CHECK(false && "signature serializer did not reject invalid candidate");
    return {};
  };
  auto check_matching_exception = [&](auto const& invalid) {
    auto const binary_message = exception_message([&] {
      return larch::chart_spr_detail::chart_spr_candidate_taxon_dedup_key(
          grammar, invalid);
    });
    auto const legacy_message = exception_message([&] {
      return larch::chart_spr_candidate_taxon_signature(grammar, invalid);
    });
    CHECK(!legacy_message.empty());
    CHECK(binary_message == legacy_message);
  };

  auto invalid_ref = edge_ids;
  invalid_ref.moved_clade = larch::temp_clade_ref(
      static_cast<larch::clade_id>(invalid_ref.added_clades.size()));
  check_matching_exception(invalid_ref);

  invalid_ref = edge_ids;
  invalid_ref.moved_clade = larch::base_clade_ref(
      static_cast<larch::clade_id>(grammar.clades.size()));
  check_matching_exception(invalid_ref);

  auto invalid_removed = edge_ids;
  invalid_removed.removed_productions.push_back(
      larch::temp_production_ref(0));
  check_matching_exception(invalid_removed);

  invalid_removed = edge_ids;
  invalid_removed.removed_productions.push_back(
      larch::base_production_ref(
          static_cast<larch::production_id>(grammar.productions.size())));
  check_matching_exception(invalid_removed);

  auto invalid_added = edge_ids;
  invalid_added.added_productions.front().children = {
      larch::temp_clade_ref(
          static_cast<larch::clade_id>(invalid_added.added_clades.size())),
      larch::base_clade_ref(
          static_cast<larch::clade_id>(grammar.clades.size()))};
  invalid_added.added_productions.front().parent = larch::base_clade_ref(
      static_cast<larch::clade_id>(grammar.clades.size()));
  // With multiple invalid refs the first child must still win, before either
  // a later child or the parent is inspected.
  check_matching_exception(invalid_added);

  invalid_added = edge_ids;
  invalid_added.added_productions.front().parent = larch::temp_clade_ref(
      static_cast<larch::clade_id>(invalid_added.added_clades.size()));
  check_matching_exception(invalid_added);

  std::println("  PASS");
}

static void test_phase8_grammar_binary_dedup_mode_selection() {
  std::println("test_phase8_grammar_binary_dedup_mode_selection");

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto dag = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(dag);

  auto run = [&](std::size_t workers) {
    auto scheduler = make_projection_scheduler(workers);
    larch::grammar_spr_enumeration_options options;
    options.source = larch::chart_spr_candidate_source::grammar;
    options.max_candidates = 12;
    options.max_candidates_is_post_dedup = true;
    options.sampled_tree_projection_scheduler = &scheduler;
    larch::chart_spr_candidate_generation_stats stats;
    auto candidates = collect_candidates(grammar, options, &stats);
    CHECK(scheduler.metrics().pending_tasks == 0);
    scheduler.shutdown();
    return std::pair{std::move(candidates), stats};
  };

  CHECK(std::locale{} == std::locale::classic());
  auto [w1_candidates, w1_stats] = run(1);
  auto [w8_candidates, w8_stats] = run(8);
  CHECK(!w1_candidates.empty());
  CHECK(w8_candidates.size() == w1_candidates.size());
  check_legacy_generation_stats_equal(w8_stats, w1_stats);
  for (std::size_t index = 0; index < w1_candidates.size(); ++index) {
    check_candidate_payload_equal(grammar, w8_candidates[index],
                                  w1_candidates[index]);
  }
  CHECK(w1_stats.grammar_candidate_binary_taxon_dedup_keys == 0);
  CHECK(w1_stats.grammar_candidate_text_taxon_dedup_keys > 0);
  CHECK(w8_stats.grammar_candidate_binary_taxon_dedup_keys ==
        w1_stats.grammar_candidate_text_taxon_dedup_keys);
  CHECK(w8_stats.grammar_candidate_text_taxon_dedup_keys == 0);

  struct grouped_taxon_ids : std::numpunct<char> {
   protected:
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
  };
  struct restore_global_locale {
    std::locale previous;
    ~restore_global_locale() { std::locale::global(previous); }
  };
  auto localized = [&] {
    restore_global_locale restore{std::locale{}};
    std::locale::global(
        std::locale{std::locale::classic(), new grouped_taxon_ids});
    return run(8);
  }();
  auto& [localized_candidates, localized_stats] = localized;
  CHECK(localized_candidates.size() == w1_candidates.size());
  check_legacy_generation_stats_equal(localized_stats, w1_stats);
  for (std::size_t index = 0; index < w1_candidates.size(); ++index) {
    check_candidate_payload_equal(grammar, localized_candidates[index],
                                  w1_candidates[index]);
  }
  CHECK(localized_stats.grammar_candidate_binary_taxon_dedup_keys == 0);
  CHECK(localized_stats.grammar_candidate_text_taxon_dedup_keys ==
        w1_stats.grammar_candidate_text_taxon_dedup_keys);

  std::println("  PASS");
}

static void test_overlay_temp_clades_and_single_site_score() {
  std::println("test_overlay_temp_clades_and_single_site_score");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto candidate = cross_candidate(grammar);

  auto overlay = larch::overlay_from_candidate(grammar, candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  CHECK(materialized.temp_clade_to_dense.size() == 2);
  CHECK(materialized.temp_clade_to_dense[0] != larch::no_clade);
  CHECK(materialized.temp_clade_to_dense[1] != larch::no_clade);
  CHECK(materialized.base_clade_to_dense[ab] == larch::no_clade);
  CHECK(materialized.base_clade_to_dense[cd] == larch::no_clade);
  CHECK(materialized.grammar.root_clade != larch::no_clade);
  CHECK(materialized.grammar.productions_by_parent[materialized.grammar.root_clade].size() == 1);

  auto score = larch::score_single_site_spr_candidate(grammar, states, candidate);
  CHECK(score.old_score == 1);
  CHECK(score.new_score == 2);
  CHECK(score.delta == 1);
  CHECK(!score.improves());
  CHECK(!score.exact_multisite);

  std::println("  PASS");
}

static void test_local_recompute_matches_full_rebuild() {
  std::println("test_local_recompute_matches_full_rebuild");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto base_chart = larch::build_single_site_chart(grammar, states);
  auto overlay = larch::overlay_from_candidate(grammar, cross_candidate(grammar));

  auto local = larch::build_single_site_overlay_chart_locally(overlay, base_chart, states);
  auto full = larch::build_single_site_overlay_chart(overlay, states);
  CHECK(local.chart.inside == full.inside);
  CHECK(local.affected_clade_count >= 3);
  CHECK(!local.used_full_rebuild);
  CHECK(larch::overlay_local_recompute_matches_full(overlay, base_chart, states));

  std::println("  PASS");
}

// TODO: add an exhaustive tiny-tree oracle here once grammar-native SPR
// enumeration is broad enough to compare complete before/after SPR sets.
static void test_grammar_native_candidate_enumeration() {
  std::println("test_grammar_native_candidate_enumeration");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto ad_taxa = taxa_for(grammar, {"A", "D"});
  auto acd_taxa = taxa_for(grammar, {"A", "C", "D"});

  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(candidates.size() >= 4);

  bool found_leaf_target_spr = false;
  for (auto const& candidate : candidates) {
    auto overlay = larch::overlay_from_candidate(grammar, candidate);
    auto materialized = larch::materialize_overlay_grammar(overlay);
    auto chart = larch::build_single_site_overlay_chart(overlay, states);
    CHECK(chart.root_min_excluding_ua(materialized.grammar.root_clade) < larch::chart_inf);

    bool has_ad = false;
    bool has_acd = false;
    for (auto const& key : candidate.added_clades) {
      has_ad = has_ad || key.taxa == ad_taxa;
      has_acd = has_acd || key.taxa == acd_taxa;
    }
    if (has_ad && has_acd) {
      found_leaf_target_spr = true;
      auto score = larch::score_single_site_spr_candidate(grammar, states, candidate);
      CHECK(score.old_score == 1);
      CHECK(score.new_score == 2);
    }
  }
  CHECK(found_leaf_target_spr);

  std::println("  PASS");
}

static void test_projected_tree_spr_matches_apply_spr_move() {
  std::println("test_projected_tree_spr_matches_apply_spr_move");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");
  larch::spr_move move{.src = src,
                       .dst = dst,
                       .lca = larch::compute_lca(dag, src, dst),
                       .score_change = std::nullopt};

  auto projected = larch::project_tree_spr_move_to_candidate(grammar, dag, move);
  CHECK(projected.has_value());
  auto overlay = larch::overlay_from_candidate(grammar, *projected);
  auto materialized = larch::materialize_overlay_grammar(overlay);

  auto after_tree = larch::apply_spr_move(dag, src, dst);
  larch::build_clade_offsets(after_tree);
  auto after_grammar = larch::build_clade_grammar(after_tree);

  CHECK(clade_shape(materialized.grammar) == clade_shape(after_grammar));
  CHECK(production_shape(materialized.grammar) == production_shape(after_grammar));

  auto projected_score = larch::score_single_site_spr_candidate(grammar, states, *projected);
  auto after_chart = larch::build_single_site_chart(after_grammar, states);
  auto after_score = after_chart.root_min_excluding_ua(after_grammar.root_clade);
  CHECK(projected_score.old_score == 1);
  CHECK(projected_score.new_score == after_score);

  larch::tree_index after_index{after_tree};
  CHECK(after_index.compute_parsimony_score() == static_cast<int>(after_score));

  std::println("  PASS");
}

static void test_bootstrap_projection_from_tree_validates_against_apply() {
  std::println("test_bootstrap_projection_from_tree_validates_against_apply");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto src = leaf_node_for(dag, "A");
  auto dst = leaf_node_for(dag, "D");

  larch::tree_spr_bootstrap_options options;
  options.radius = 4;
  options.score_threshold = std::numeric_limits<int>::max();
  auto candidates = larch::bootstrap_spr_candidates_from_tree(grammar, dag, options);
  CHECK(!candidates.empty());

  bool found_a_to_d = false;
  std::size_t checked = 0;
  for (auto const& candidate : candidates) {
    CHECK(candidate.source_tree_move.has_value());
    auto const& move = *candidate.source_tree_move;
    if (move.src == src && move.dst == dst) found_a_to_d = true;

    auto after_tree = larch::apply_spr_move(dag, move.src, move.dst);
    larch::build_clade_offsets(after_tree);
    auto after_grammar = larch::build_clade_grammar(after_tree);
    auto after_chart = larch::build_single_site_chart(after_grammar, states);
    auto after_score = after_chart.root_min_excluding_ua(after_grammar.root_clade);
    auto projected_score = larch::score_single_site_spr_candidate(grammar, states, candidate);
    CHECK(projected_score.new_score == after_score);
    if (++checked == 8) break;
  }
  CHECK(found_a_to_d);

  std::println("  PASS");
}

static void check_prepared_projection_fixture(std::string const& label,
                                              larch::clade_grammar const& base,
                                              larch::phylo_dag& tree,
                                              std::size_t expected_emitted,
                                              std::size_t expected_projected) {
  for (auto edge_variant : tree.get_all_edges()) {
    std::visit(
        [](auto edge) {
          edge.edge_weight() = static_cast<float>(edge.index() + 1);
        },
        edge_variant);
  }
  auto [oracle_tree, oracle_node_map] = larch::clone_tree(tree);
  auto [legacy_tree, legacy_node_map] = larch::clone_tree(tree);
  (void)oracle_node_map;
  (void)legacy_node_map;
  auto prepared =
      larch::chart_spr_detail::prepare_sampled_tree_projection(base, tree);
  auto prepared_source_snapshot = snapshot_projection_source(tree);

  auto radius = larch::compute_tree_max_depth(tree) * 2;
  if (radius == 0) radius = 1;
  larch::move_enumerator enumerator{prepared.index(),
                                    std::numeric_limits<int>::max()};
  std::vector<larch::profitable_move> moves;
  enumerator.find_all_moves(radius, [&](larch::profitable_move const& move) {
    moves.push_back(move);
  });
  CHECK(moves.size() == expected_emitted);

  larch::scratch_buffers reusable_enumeration_scratch;
  reusable_enumeration_scratch.resize(prepared.index().num_variable_sites());
  std::vector<larch::profitable_move> reused_moves;
  for (auto source_node : prepared.index().get_searchable_nodes()) {
    enumerator.find_moves_for_source(
        source_node, radius,
        [&](larch::profitable_move const& move) {
          reused_moves.push_back(move);
        },
        reusable_enumeration_scratch);
  }
  CHECK(reused_moves.size() == moves.size());
  for (std::size_t i = 0; i < moves.size(); ++i) {
    CHECK(reused_moves[i].src == moves[i].src);
    CHECK(reused_moves[i].dst == moves[i].dst);
    CHECK(reused_moves[i].lca == moves[i].lca);
    CHECK(reused_moves[i].score_change == moves[i].score_change);
  }

  std::size_t projected_count = 0;
  std::size_t direct_count = 0;
  std::size_t fallback_count = 0;
  larch::chart_spr_detail::sampled_tree_direct_workspace cached_workspace;
  larch::chart_spr_detail::sampled_tree_projection_output_slot cached_output;
  std::vector<std::optional<larch::grammar_spr_candidate>> sequential_results;
  sequential_results.reserve(moves.size());
  for (auto const& emitted : moves) {
    larch::spr_move move{.src = emitted.src,
                         .dst = emitted.dst,
                         .lca = emitted.lca,
                         .score_change = emitted.score_change};
    auto expected =
        project_tree_spr_move_annotated_oracle(base, oracle_tree, move);
    auto direct = larch::chart_spr_detail::project_sampled_tree_move_with_path(
        prepared, move);
    larch::chart_spr_detail::
        project_sampled_tree_move_with_path_into_with_cached_after_refs(
            prepared, move, cached_workspace, cached_output);
    CHECK(cached_output.path == direct.path);
    CHECK(cached_output.engaged == direct.candidate.has_value());
    if (cached_output.engaged) {
      check_candidate_payload_equal(base, *direct.candidate,
                                    *cached_output.candidate);
    }
    if (direct.path ==
        larch::chart_spr_detail::sampled_tree_projection_path::direct) {
      ++direct_count;
    } else if (direct.path ==
               larch::chart_spr_detail::sampled_tree_projection_path::
                   clone_fallback) {
      ++fallback_count;
    }
    auto actual = std::move(direct.candidate);
    auto profitable_actual =
        larch::project_tree_spr_move_to_candidate(prepared, emitted);
    auto legacy_actual =
        larch::project_tree_spr_move_to_candidate(base, legacy_tree, move);
    CHECK(actual.has_value() == expected.has_value());
    CHECK(profitable_actual.has_value() == actual.has_value());
    CHECK(legacy_actual.has_value() == actual.has_value());

    auto annotated_tree = larch::apply_spr_move(tree, move.src, move.dst);
    auto topology_only_tree =
        larch::apply_spr_move_topology_only(tree, move.src, move.dst);
    check_projection_clone_annotations_are_sparse(topology_only_tree);
    auto annotated_grammar = larch::build_clade_grammar(annotated_tree);
    auto topology_only_grammar = larch::build_clade_grammar(topology_only_tree);
    CHECK(clade_shape(annotated_grammar) == clade_shape(topology_only_grammar));
    CHECK(production_shape(annotated_grammar) ==
          production_shape(topology_only_grammar));

    sequential_results.push_back(actual);
    if (!actual) continue;
    ++projected_count;
    CHECK(actual->source_before_topology_productions.has_value());
    CHECK(actual->source_after_topology_productions.has_value());
    check_candidate_payload_equal(base, *expected, *actual);
    check_candidate_payload_equal(base, *actual, *profitable_actual);
    check_candidate_payload_equal(base, *actual, *legacy_actual);
  }
  CHECK(projected_count == expected_projected);
  CHECK(direct_count == expected_emitted);
  CHECK(fallback_count == 0);

  auto project_all = [&]() {
    std::vector<std::optional<larch::grammar_spr_candidate>> result;
    result.reserve(moves.size());
    for (auto const& move : moves) {
      result.push_back(
          larch::project_tree_spr_move_to_candidate(prepared, move));
    }
    return result;
  };
  auto first_future = std::async(std::launch::async, project_all);
  auto second_future = std::async(std::launch::async, project_all);
  auto first_parallel = first_future.get();
  auto second_parallel = second_future.get();
  CHECK(first_parallel.size() == sequential_results.size());
  CHECK(second_parallel.size() == sequential_results.size());
  for (std::size_t i = 0; i < sequential_results.size(); ++i) {
    CHECK(first_parallel[i].has_value() == sequential_results[i].has_value());
    CHECK(second_parallel[i].has_value() == sequential_results[i].has_value());
    if (!sequential_results[i]) continue;
    check_candidate_payload_equal(base, *sequential_results[i],
                                  *first_parallel[i]);
    check_candidate_payload_equal(base, *sequential_results[i],
                                  *second_parallel[i]);
  }
  CHECK(snapshot_projection_source(tree) == prepared_source_snapshot);
  std::println("  {}: {} emitted, {} projected", label, moves.size(),
               projected_count);
}

static void test_phase8_prepared_projection_differential_all_emitted_moves() {
  std::println(
      "test_phase8_prepared_projection_differential_all_emitted_moves");

  auto binary_tree =
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto binary_grammar = larch::build_clade_grammar(binary_tree);
  check_prepared_projection_fixture("binary", binary_grammar, binary_tree, 14,
                                    10);

  auto deeper_tree =
      larch::test::make_tiny_labelled_tree("A", eight_taxon_balanced_tree());
  auto deeper_grammar = larch::build_clade_grammar(deeper_tree);
  check_prepared_projection_fixture("eight-taxon", deeper_grammar, deeper_tree,
                                    98, 88);

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto multiparent_base =
      larch::test::merge_tiny_trees(std::move(source_trees));
  auto multiparent_grammar = larch::build_clade_grammar(multiparent_base);

  auto multiparent_member_tree =
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  check_prepared_projection_fixture("multiparent-base", multiparent_grammar,
                                    multiparent_member_tree, 14, 10);

  larch::grammar_spr_enumeration_options sample_options;
  sample_options.sampled_tree_source_dag = &multiparent_base;
  std::mt19937 rng(19);
  auto sampled_tree = larch::chart_spr_detail::build_sampled_tree_from_grammar(
      multiparent_grammar, sample_options, 1, rng);
  check_prepared_projection_fixture("sampled-tree", multiparent_grammar,
                                    sampled_tree, 22, 16);

  std::println("  PASS");
}

static void test_phase8_binary_direct_metadata_resolver_is_fail_closed() {
  std::println("test_phase8_binary_direct_metadata_resolver_is_fail_closed");
  using larch::chart_spr_detail::resolve_sampled_tree_binary_move_metadata;
  using status = larch::chart_spr_detail::sampled_tree_direct_metadata_status;

  auto tree =
      larch::test::make_tiny_labelled_tree("A", eight_taxon_balanced_tree());
  auto grammar = larch::build_clade_grammar(tree);
  auto prepared =
      larch::chart_spr_detail::prepare_sampled_tree_projection(grammar, tree);
  auto const& index = prepared.index();
  auto const& node_to_clade = prepared.node_to_base_clade();
  auto const& node_to_production = prepared.node_to_base_production();

  larch::production_id source_production = larch::no_production;
  larch::clade_id moved = larch::no_clade;
  larch::clade_id old_parent = larch::no_clade;
  larch::clade_id old_sibling = larch::no_clade;
  larch::clade_id target = larch::no_clade;
  larch::chart_spr_detail::sampled_tree_direct_move_metadata metadata;

  auto radius = larch::compute_tree_max_depth(tree) * 2;
  larch::move_enumerator enumerator{index, std::numeric_limits<int>::max()};
  enumerator.find_all_moves(radius, [&](larch::profitable_move const& emitted) {
    if (source_production != larch::no_production) return;
    auto const parent_node = index.get_parent(emitted.src);
    if (!index.is_valid(parent_node) ||
        index.get_num_children(parent_node) != 2 ||
        emitted.src >= node_to_clade.size() ||
        emitted.dst >= node_to_clade.size() ||
        parent_node >= node_to_clade.size() ||
        parent_node >= node_to_production.size()) {
      return;
    }
    auto const& children = index.get_children(parent_node);
    auto found_sibling =
        std::find_if(children.begin(), children.end(),
                     [&](auto child) { return child != emitted.src; });
    if (found_sibling == children.end() ||
        *found_sibling >= node_to_clade.size()) {
      return;
    }

    auto const candidate_source_production = node_to_production[parent_node];
    auto const candidate_moved = node_to_clade[emitted.src];
    auto const candidate_old_parent = node_to_clade[parent_node];
    auto const candidate_old_sibling = node_to_clade[*found_sibling];
    auto const candidate_target = node_to_clade[emitted.dst];
    larch::chart_spr_detail::sampled_tree_direct_move_metadata candidate;
    if (resolve_sampled_tree_binary_move_metadata(
            grammar, candidate_source_production, candidate_moved,
            candidate_old_parent, candidate_old_sibling, candidate_target,
            candidate) != status::ready ||
        candidate_old_parent == grammar.root_clade) {
      return;
    }
    source_production = candidate_source_production;
    moved = candidate_moved;
    old_parent = candidate_old_parent;
    old_sibling = candidate_old_sibling;
    target = candidate_target;
    metadata = candidate;
  });

  CHECK(source_production != larch::no_production);
  CHECK(metadata.moved_clade == larch::base_clade_ref(moved));
  CHECK(metadata.old_parent == larch::base_clade_ref(old_parent));
  CHECK(metadata.old_sibling == larch::base_clade_ref(old_sibling));
  CHECK(metadata.new_sibling_or_target == larch::base_clade_ref(target));

  auto resolve =
      [&](larch::clade_grammar const& selected_grammar,
          larch::production_id selected_production,
          larch::clade_id selected_moved, larch::clade_id selected_parent,
          larch::clade_id selected_sibling, larch::clade_id selected_target) {
        larch::chart_spr_detail::sampled_tree_direct_move_metadata ignored;
        return resolve_sampled_tree_binary_move_metadata(
            selected_grammar, selected_production, selected_moved,
            selected_parent, selected_sibling, selected_target, ignored);
      };

  CHECK(resolve(grammar, larch::no_production, moved, old_parent, old_sibling,
                target) == status::fallback);
  CHECK(resolve(grammar, source_production, larch::no_clade, old_parent,
                old_sibling, target) == status::fallback);
  CHECK(resolve(grammar, source_production, target, old_parent, old_sibling,
                moved) == status::fallback);
  CHECK(resolve(grammar, source_production, moved, target, old_sibling,
                target) == status::fallback);
  CHECK(resolve(grammar, source_production, moved, old_parent, target,
                target) == status::fallback);

  auto nonbinary = grammar;
  nonbinary.productions[source_production].children.push_back(target);
  CHECK(resolve(nonbinary, source_production, moved, old_parent, old_sibling,
                target) == status::fallback);
  auto duplicate_moved = grammar;
  duplicate_moved.productions[source_production].children = {moved, moved};
  CHECK(resolve(duplicate_moved, source_production, moved, old_parent,
                old_sibling, target) == status::fallback);

  CHECK(resolve(grammar, source_production, moved, old_parent, old_sibling,
                moved) == status::no_candidate);
  CHECK(resolve(grammar, source_production, moved, old_parent, old_sibling,
                old_parent) == status::no_candidate);
  CHECK(resolve(grammar, source_production, moved, old_parent, old_sibling,
                old_sibling) == status::no_candidate);
  CHECK(grammar.root_clade != moved);
  CHECK(grammar.root_clade != old_parent);
  CHECK(grammar.root_clade != old_sibling);
  CHECK(!larch::chart_spr_detail::disjoint_taxa(
      grammar.clades[moved].taxa, grammar.clades[grammar.root_clade].taxa));
  CHECK(resolve(grammar, source_production, moved, old_parent, old_sibling,
                grammar.root_clade) == status::no_candidate);
  std::println("  PASS");
}

static void test_phase8_parallel_sampled_projection_is_deterministic() {
  std::println("test_phase8_parallel_sampled_projection_is_deterministic");

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree_root_swapped()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto dag = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(dag);
  auto source_before = snapshot_projection_source(dag);

  for (auto seed : {std::uint32_t{1}, std::uint32_t{7}, std::uint32_t{19}}) {
    for (auto max_candidates : {std::size_t{0}, std::size_t{4}}) {
      std::vector<larch::grammar_spr_candidate> baseline;
      larch::chart_spr_candidate_generation_stats baseline_stats;
      for (auto workers :
           {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto scheduler = make_projection_scheduler(workers);
        larch::grammar_spr_enumeration_options options;
        options.source = larch::chart_spr_candidate_source::sampled_tree;
        options.sampled_tree_source_dag = &dag;
        options.sampled_tree_count = 1;
        options.sampled_tree_spr_radius = 8;
        options.sampled_tree_score_threshold =
            std::numeric_limits<int>::max();
        options.randomize_order = true;
        options.seed = seed;
        options.max_candidates = max_candidates;
        options.max_candidates_is_post_dedup = true;
        options.sampled_tree_projection_scheduler = &scheduler;

        std::atomic<std::size_t> hook_calls{0};
        std::atomic<std::size_t> source_hook_calls{0};
        std::latch first_wave_started{2};
        std::latch first_source_wave_started{2};
        if (workers > 1) {
          options.before_sampled_tree_source_enumeration_for_tests =
              [&](std::size_t, std::size_t) {
                auto const call =
                    source_hook_calls.fetch_add(1, std::memory_order_relaxed);
                if (call >= 2) return;
                first_source_wave_started.count_down();
                first_source_wave_started.wait();
              };
          options.before_sampled_tree_projection_for_tests =
              [&](std::size_t) {
                auto const call =
                    hook_calls.fetch_add(1, std::memory_order_relaxed);
                if (call >= 2) return;
                first_wave_started.count_down();
                first_wave_started.wait();
              };
        }

        larch::chart_spr_candidate_generation_stats stats;
        auto candidates = collect_candidates(grammar, options, &stats);
        CHECK(!candidates.empty());
        for (auto const& candidate : candidates) {
          CHECK(candidate.source_before_topology_productions.has_value());
          CHECK(candidate.source_after_topology_productions.has_value());
          CHECK(!candidate.source_before_topology_productions->empty());
          CHECK(candidate.source_after_topology_productions->size() ==
                candidate.source_before_topology_productions->size());
        }
        if (max_candidates != 0) {
          CHECK(candidates.size() == max_candidates);
          CHECK(stats.stop_reason ==
                larch::chart_spr_candidate_stop_reason::candidate_cap);
        }
        CHECK(stats.sampled_tree_projection_moves_preassigned >=
              std::min(workers, std::size_t{2}));
        CHECK(stats.sampled_tree_projection_move_enumeration_visits ==
              stats.sampled_tree_projection_moves_preassigned);
        CHECK(stats.sampled_tree_source_one_pass_move_visits ==
              stats.sampled_tree_projection_moves_preassigned);
        CHECK(stats.sampled_tree_projection_enumeration_passes == 1);
        CHECK(stats.sampled_tree_source_waves > 0);
        CHECK(stats.sampled_tree_sources_enumerated > 0);
        CHECK(stats.sampled_tree_source_admitted_wave_width <= workers);
        CHECK(stats.sampled_tree_source_admitted_wave_width >=
              stats.sampled_tree_source_peak_wave_size);
        CHECK(stats.sampled_tree_source_peak_wave_size >=
              std::min(workers, std::size_t{2}));
        if (max_candidates == 0) {
          CHECK(stats.sampled_tree_source_peak_wave_size ==
                stats.sampled_tree_source_admitted_wave_width);
          CHECK(stats.sampled_tree_source_adaptive_initial_wave_width == 0);
          CHECK(stats.sampled_tree_source_adaptive_widenings == 0);
        } else if (stats.sampled_tree_source_admitted_wave_width > 4) {
          CHECK(stats.sampled_tree_source_adaptive_initial_wave_width == 4);
          CHECK(stats.sampled_tree_source_peak_wave_size >= 4);
          if (stats.sampled_tree_source_adaptive_widenings == 0) {
            CHECK(stats.sampled_tree_source_peak_wave_size == 4);
          }
          if (stats.sampled_tree_source_full_width_waves != 0) {
            CHECK(stats.sampled_tree_source_peak_wave_size ==
                  stats.sampled_tree_source_admitted_wave_width);
          }
        }
        auto const projection_jobs_per_worker =
            workers == 1 ? std::size_t{4} : std::size_t{64};
        auto const maximum_projection_wave =
            workers * projection_jobs_per_worker;
        CHECK(stats.sampled_tree_projection_admitted_subwave_width <=
              maximum_projection_wave);
        CHECK(stats.sampled_tree_projection_admitted_subwave_width >=
              stats.sampled_tree_projection_peak_wave_size);
        CHECK(stats.sampled_tree_projection_scheduler_operations > 0);
        CHECK(stats.sampled_tree_projection_peak_wave_size <=
              stats.sampled_tree_projection_moves_preassigned);
        CHECK(stats.sampled_tree_projection_direct > 0);
        CHECK(stats.sampled_tree_projection_fallback == 0);
        CHECK(stats.sampled_tree_projection_direct +
                  stats.sampled_tree_projection_fallback ==
              stats.sampled_tree_projection_moves_preassigned);
        CHECK(stats.sampled_tree_projection_estimated_peak_bytes > 0);
        CHECK(stats.sampled_tree_source_actual_peak_bytes >=
              stats.sampled_tree_projection_estimated_peak_bytes);
        if (workers == 1) {
          CHECK(stats.sampled_tree_projection_parallel_operations == 0);
          CHECK(stats.sampled_tree_source_enumeration_parallel_operations ==
                0);
          baseline = candidates;
          baseline_stats = stats;
        } else {
          CHECK(source_hook_calls.load(std::memory_order_relaxed) >= 2);
          CHECK(stats.sampled_tree_source_enumeration_parallel_operations > 0);
          CHECK(stats.sampled_tree_source_enumeration_ranges >= 2);
          CHECK(stats.sampled_tree_source_enumeration_worker_tasks >= 2);
          CHECK(
              stats.sampled_tree_source_enumeration_active_worker_high_water >=
              2);
          CHECK(stats.sampled_tree_projection_parallel_operations > 0);
          CHECK(stats.sampled_tree_projection_ranges >= 2);
          CHECK(stats.sampled_tree_projection_worker_tasks >= 2);
          CHECK(stats.sampled_tree_projection_active_worker_high_water >= 2);
          CHECK(candidates.size() == baseline.size());
          check_legacy_generation_stats_equal(stats, baseline_stats);
          for (std::size_t i = 0; i < candidates.size(); ++i) {
            check_candidate_payload_equal(grammar, baseline[i],
                                          candidates[i]);
          }
        }
        auto metrics = scheduler.metrics();
        CHECK(metrics.pending_tasks == 0);
        CHECK(metrics.tasks_submitted == metrics.tasks_completed);
        CHECK(metrics.tasks_submitted == metrics.tasks_joined);
        CHECK(snapshot_projection_source(dag) == source_before);
      }
    }
  }

  std::println("  PASS");
}

static void
test_phase8_parallel_projection_cached_after_refs_preserves_fallback() {
  std::println(
      "test_phase8_parallel_projection_cached_after_refs_preserves_fallback");
  using larch::chart_spr_detail::preassign_sampled_tree_projection_jobs;
  using larch::chart_spr_detail::project_preassigned_sampled_tree_moves;

  // First select a real direct move and a production that the move leaves
  // untouched.  Making only that unrelated base production unfindable below
  // disables the direct prerequisite and complete topology certificate while
  // leaving the clone-diff candidate itself well defined.
  auto probe_tree =
      larch::test::make_tiny_labelled_tree("A", eight_taxon_balanced_tree());
  auto probe_grammar = larch::build_clade_grammar(probe_tree);
  auto probe_prepared =
      larch::chart_spr_detail::prepare_sampled_tree_projection(probe_grammar,
                                                               probe_tree);
  auto const radius = larch::compute_tree_max_depth(probe_tree) * 2;
  larch::move_enumerator enumerator{probe_prepared.index(),
                                    std::numeric_limits<int>::max()};
  std::optional<larch::profitable_move> selected_move;
  std::optional<larch::grammar_spr_candidate> expected_fallback_payload;
  larch::production_id unrelated_pid = larch::no_production;
  enumerator.find_all_moves(radius, [&](larch::profitable_move const& move) {
    if (selected_move) return;
    auto projected =
        larch::project_tree_spr_move_to_candidate(probe_prepared, move);
    if (!projected) return;
    for (std::size_t pid = 0; pid < probe_grammar.productions.size(); ++pid) {
      auto const typed_pid = static_cast<larch::production_id>(pid);
      if (probe_grammar.productions[pid].children.size() < 2 ||
          std::find(projected->removed_productions.begin(),
                    projected->removed_productions.end(),
                    larch::base_production_ref(typed_pid)) !=
              projected->removed_productions.end()) {
        continue;
      }
      selected_move = move;
      unrelated_pid = typed_pid;
      expected_fallback_payload = std::move(*projected);
      expected_fallback_payload->source_before_topology_productions.reset();
      expected_fallback_payload->source_after_topology_productions.reset();
      return;
    }
  });
  CHECK(selected_move.has_value());
  CHECK(expected_fallback_payload.has_value());
  CHECK(unrelated_pid != larch::no_production);

  auto fallback_tree =
      larch::test::make_tiny_labelled_tree("A", eight_taxon_balanced_tree());
  auto fallback_grammar = larch::build_clade_grammar(fallback_tree);
  CHECK(unrelated_pid < fallback_grammar.productions.size());
  auto& malformed_unrelated = fallback_grammar.productions[unrelated_pid];
  CHECK(malformed_unrelated.children.size() >= 2);
  malformed_unrelated.children[1] = malformed_unrelated.children[0];

  auto prepared = larch::chart_spr_detail::prepare_sampled_tree_projection(
      fallback_grammar, fallback_tree);
  CHECK(!prepared.direct_projection_ready());
  CHECK(!prepared.source_before_topology_refs().has_value());

  std::optional<larch::grammar_spr_candidate> w1_candidate;
  for (auto workers :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    auto scheduler = make_projection_scheduler(workers);
    larch::grammar_spr_enumeration_options options;
    options.sampled_tree_spr_radius = radius;
    options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
    options.sampled_tree_projection_scheduler = &scheduler;
    std::mt19937 rng(19);
    auto preassignment = preassign_sampled_tree_projection_jobs(
        prepared, options, radius, rng);
    auto selected = std::find_if(
        preassignment.jobs.begin(), preassignment.jobs.end(),
        [&](auto const& job) {
          return job.move.src == selected_move->src &&
                 job.move.dst == selected_move->dst &&
                 job.move.lca == selected_move->lca &&
                 job.move.score_change == selected_move->score_change;
        });
    CHECK(selected != preassignment.jobs.end());
    auto selected_job = *selected;
    selected_job.ordinal = 0;
    preassignment.jobs.assign(1, selected_job);

    std::optional<larch::grammar_spr_candidate> actual;
    auto execution = project_preassigned_sampled_tree_moves(
        prepared, preassignment, options,
        [&](std::size_t ordinal,
            std::optional<larch::grammar_spr_candidate> const& candidate) {
          CHECK(ordinal == 0);
          CHECK(!actual.has_value());
          actual = candidate;
          return true;
        });
    CHECK(execution.moves_preassigned == 1);
    CHECK(execution.direct_projections == 0);
    CHECK(execution.fallback_projections == 1);
    CHECK(actual.has_value());
    CHECK(!actual->source_before_topology_productions.has_value());
    CHECK(!actual->source_after_topology_productions.has_value());
    check_candidate_projection_fields_equal(*expected_fallback_payload,
                                            *actual);
    if (workers == 1) {
      w1_candidate = actual;
    } else {
      CHECK(w1_candidate.has_value());
      check_candidate_projection_fields_equal(*w1_candidate, *actual);
    }

    auto const metrics = scheduler.metrics();
    CHECK(metrics.pending_tasks == 0);
    CHECK(metrics.tasks_submitted == metrics.tasks_completed);
    CHECK(metrics.tasks_submitted == metrics.tasks_joined);
  }

  std::println("  PASS");
}

static void test_phase8_parallel_postprocessing_gather_matches_w1() {
  std::println("test_phase8_parallel_postprocessing_gather_matches_w1");
  using larch::chart_spr_detail::project_sampled_tree_moves_in_source_waves;
  using larch::chart_spr_detail::sampled_tree_projection_postprocessing;

  auto dag = larch::test::make_tiny_labelled_tree(
      "AA", eight_taxon_distinct_balanced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::grammar_spr_enumeration_options base_options;
  base_options.source = larch::chart_spr_candidate_source::sampled_tree;
  base_options.sampled_tree_source_dag = &dag;
  base_options.sampled_tree_count = 1;
  base_options.sampled_tree_spr_radius = 16;
  base_options.sampled_tree_score_threshold =
      std::numeric_limits<int>::max();
  // This fixture has both leaf-source rejections and accepted internal-clade
  // moves, so the test observes both postprocessing outcomes rather than only
  // checking pointer plumbing.
  base_options.min_moved_clade_size = 2;
  base_options.min_target_clade_size = 2;
  base_options.randomize_order = true;
  base_options.seed = 19;

  std::mt19937 tree_rng(base_options.seed);
  auto tree = larch::chart_spr_detail::build_sampled_tree_from_grammar(
      grammar, base_options, 0, tree_rng);
  auto const projection_rng_state = tree_rng;
  auto prepared =
      larch::chart_spr_detail::prepare_sampled_tree_projection(grammar, tree);

  struct run_result {
    std::vector<larch::grammar_spr_candidate> candidates;
    larch::chart_spr_candidate_generation_stats stats;
    std::size_t gather_calls = 0;
    std::size_t engaged_calls = 0;
    std::size_t null_postprocessing_calls = 0;
    std::size_t nonnull_postprocessing_calls = 0;
    std::size_t attempted_postprocessing_calls = 0;
    std::size_t filter_rejections = 0;
    std::size_t passing_signatures = 0;
    larch::chart_spr_detail::sampled_tree_source_wave_execution_stats
        execution;
  };

  auto run = [&](std::size_t workers) {
    auto scheduler = make_projection_scheduler(workers);
    auto options = base_options;
    options.sampled_tree_projection_scheduler = &scheduler;
    auto rng = projection_rng_state;
    run_result result;
    std::set<std::string> seen;
    result.execution = project_sampled_tree_moves_in_source_waves(
        prepared, options, base_options.sampled_tree_spr_radius, rng,
        [&](std::size_t,
            std::optional<larch::grammar_spr_candidate> const& projected,
            sampled_tree_projection_postprocessing* postprocessing) {
          ++result.gather_calls;
          if (postprocessing == nullptr) {
            ++result.null_postprocessing_calls;
          } else {
            ++result.nonnull_postprocessing_calls;
          }
          if (!projected) {
            if (postprocessing != nullptr) {
              CHECK(!postprocessing->attempted);
            }
            larch::chart_spr_detail::note_pruned_after(
                result.stats,
                &larch::chart_spr_candidate_generation_stats::
                    candidates_pruned_invalid);
            return true;
          }

          ++result.engaged_calls;
          ++result.stats.candidates_constructed;
          bool filters_passed = false;
          std::string signature;
          if (postprocessing == nullptr) {
            CHECK(workers == 1);
            filters_passed =
                larch::chart_spr_detail::candidate_passes_postconstruction_filters(
                    grammar, *projected, options, result.stats);
            if (filters_passed) {
              signature =
                  larch::chart_spr_candidate_taxon_signature(grammar,
                                                              *projected);
            }
          } else {
            CHECK(workers > 1);
            CHECK(postprocessing->attempted);
            CHECK(!postprocessing->failure);
            ++result.attempted_postprocessing_calls;
            larch::chart_spr_detail::add_generation_count_stats(
                result.stats, postprocessing->filter_stats);
            filters_passed = postprocessing->filter_passed;
            if (filters_passed) {
              auto const expected_signature = larch::chart_spr_detail::
                  chart_spr_candidate_taxon_dedup_key(grammar, *projected);
              CHECK(!postprocessing->taxon_signature.empty());
              CHECK(postprocessing->taxon_signature == expected_signature);
              signature = postprocessing->taxon_signature;
            } else {
              CHECK(postprocessing->taxon_signature.empty());
            }
          }

          if (!filters_passed) {
            ++result.filter_rejections;
            return true;
          }
          ++result.passing_signatures;
          if (!seen.insert(std::move(signature)).second) {
            larch::chart_spr_detail::note_pruned_after(
                result.stats,
                &larch::chart_spr_candidate_generation_stats::
                    candidates_pruned_duplicate);
            return true;
          }
          ++result.stats.candidates_generated_after_dedup;
          if (larch::chart_spr_detail::
                  grammar_spr_candidate_involves_multifurcation(
                      grammar, *projected)) {
            ++result.stats.spr_multifurcation_moves_generated;
          }
          result.candidates.push_back(*projected);
          return true;
        });

    CHECK(!result.execution.cancelled);
    CHECK(result.execution.projection_speculative_discarded == 0);
    CHECK(result.gather_calls == result.execution.moves_enumerated);
    CHECK(result.engaged_calls == result.stats.candidates_constructed);
    CHECK(result.filter_rejections > 0);
    CHECK(result.passing_signatures > 0);
    if (workers == 1) {
      CHECK(result.null_postprocessing_calls == result.gather_calls);
      CHECK(result.nonnull_postprocessing_calls == 0);
      CHECK(result.attempted_postprocessing_calls == 0);
      CHECK(result.execution.projection_scheduler_operations ==
            result.execution.projection_waves);
    } else {
      CHECK(result.null_postprocessing_calls == 0);
      CHECK(result.nonnull_postprocessing_calls == result.gather_calls);
      CHECK(result.attempted_postprocessing_calls == result.engaged_calls);
      CHECK(result.execution.projection_scheduler_operations ==
            2 * result.execution.projection_waves);
    }

    // Compare the low-level three-argument gather with the product collector,
    // including every legacy semantic counter that the serial gather owns.
    larch::chart_spr_candidate_generation_stats product_stats;
    auto product_candidates = collect_candidates(grammar, options,
                                                 &product_stats);
    check_legacy_generation_stats_equal(result.stats, product_stats);
    CHECK(result.candidates.size() == product_candidates.size());
    for (std::size_t i = 0; i < result.candidates.size(); ++i) {
      check_candidate_payload_equal(grammar, result.candidates[i],
                                    product_candidates[i]);
    }

    auto const metrics = scheduler.metrics();
    CHECK(metrics.pending_tasks == 0);
    CHECK(metrics.tasks_submitted == metrics.tasks_completed);
    CHECK(metrics.tasks_submitted == metrics.tasks_joined);
    return result;
  };

  auto const w1 = run(1);
  for (auto workers : {std::size_t{4}, std::size_t{8}}) {
    auto const parallel = run(workers);
    CHECK(parallel.candidates.size() == w1.candidates.size());
    check_legacy_generation_stats_equal(parallel.stats, w1.stats);
    for (std::size_t i = 0; i < parallel.candidates.size(); ++i) {
      check_candidate_payload_equal(grammar, w1.candidates[i],
                                    parallel.candidates[i]);
    }
  }

  std::println("  PASS");
}

static void test_phase8_grammar_and_hybrid_worker_seed_matrix() {
  std::println("test_phase8_grammar_and_hybrid_worker_seed_matrix");

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto dag = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(dag);
  auto source_before = snapshot_projection_source(dag);

  for (auto source : {larch::chart_spr_candidate_source::grammar,
                      larch::chart_spr_candidate_source::hybrid}) {
    for (auto seed : {std::uint32_t{1}, std::uint32_t{7}, std::uint32_t{19}}) {
      std::vector<larch::grammar_spr_candidate> baseline;
      larch::chart_spr_candidate_generation_stats baseline_stats;
      for (auto workers :
           {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto scheduler = make_projection_scheduler(workers);
        std::atomic<std::size_t> grammar_barrier_arrivals = 0;
        std::latch two_grammar_workers_started{2};
        larch::grammar_spr_enumeration_options options;
        options.source = source;
        options.sampled_tree_source_dag = &dag;
        options.sampled_tree_count = 1;
        options.sampled_tree_spr_radius = 8;
        options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
        options.randomize_order = true;
        options.seed = seed;
        options.max_candidates = 8;
        options.max_candidates_is_post_dedup = true;
        options.sampled_tree_projection_scheduler = &scheduler;
        if (source == larch::chart_spr_candidate_source::grammar &&
            workers > 1) {
          // The grammar work in this tiny fixture is short enough that one
          // worker can otherwise drain both ranges before a peer is scheduled,
          // especially when CTest itself runs in parallel. Hold the first two
          // construction callbacks at a test-only barrier so the active-worker
          // assertion below measures scheduler concurrency deterministically.
          options.before_grammar_candidate_construction_for_tests =
              [&](std::size_t) {
                auto const arrival = grammar_barrier_arrivals.fetch_add(
                    1, std::memory_order_relaxed);
                if (arrival >= 2) return;
                two_grammar_workers_started.count_down();
                two_grammar_workers_started.wait();
              };
        }

        larch::chart_spr_candidate_generation_stats stats;
        auto candidates = collect_candidates(grammar, options, &stats);
        CHECK(!candidates.empty());
        if (source == larch::chart_spr_candidate_source::grammar) {
          CHECK(stats.sampled_tree_projection_moves_preassigned == 0);
          CHECK(stats.sampled_tree_projection_scheduler_operations == 0);
          if (workers == 1) {
            CHECK(stats.grammar_candidate_scheduler_operations == 0);
            CHECK(stats.grammar_candidate_admitted_wave_width == 0);
          } else {
            CHECK(stats.grammar_candidate_construction_waves > 0);
            CHECK(stats.grammar_candidate_scheduler_operations > 0);
            CHECK(stats.grammar_candidate_parallel_operations > 0);
            CHECK(stats.grammar_candidate_ranges >= 2);
            CHECK(stats.grammar_candidate_worker_tasks >= 2);
            CHECK(stats.grammar_candidate_active_worker_high_water >= 2);
            CHECK(stats.grammar_candidate_peak_wave_size > 1);
            CHECK(stats.grammar_candidate_admitted_wave_width == workers * 4);
            CHECK(stats.grammar_candidate_actual_peak_bytes > 0);
          }
        } else {
          CHECK(stats.sampled_tree_projection_moves_preassigned > 0);
          CHECK(stats.sampled_tree_projection_scheduler_operations > 0);
          CHECK(stats.sampled_tree_projection_move_enumeration_visits ==
                stats.sampled_tree_projection_moves_preassigned);
          CHECK(stats.sampled_tree_projection_enumeration_passes == 1);
          CHECK(stats.sampled_tree_source_one_pass_move_visits ==
                stats.sampled_tree_projection_moves_preassigned);
          CHECK(stats.sampled_tree_source_waves > 0);
          CHECK(stats.sampled_tree_source_admitted_wave_width <= workers);
          CHECK(stats.sampled_tree_source_admitted_wave_width >=
                stats.sampled_tree_source_peak_wave_size);
          if (stats.sampled_tree_source_admitted_wave_width > 4) {
            CHECK(stats.sampled_tree_source_adaptive_initial_wave_width == 4);
            CHECK(stats.sampled_tree_source_peak_wave_size >= 4);
            if (stats.sampled_tree_source_adaptive_widenings == 0) {
              CHECK(stats.sampled_tree_source_peak_wave_size == 4);
            }
            if (stats.sampled_tree_source_full_width_waves != 0) {
              CHECK(stats.sampled_tree_source_peak_wave_size ==
                    stats.sampled_tree_source_admitted_wave_width);
            }
          }
        }

        if (workers == 1) {
          baseline = candidates;
          baseline_stats = stats;
        } else {
          CHECK(candidates.size() == baseline.size());
          check_legacy_generation_stats_equal(stats, baseline_stats);
          for (std::size_t i = 0; i < candidates.size(); ++i) {
            check_candidate_payload_equal(grammar, baseline[i], candidates[i]);
          }
        }
        CHECK(scheduler.metrics().pending_tasks == 0);
        CHECK(snapshot_projection_source(dag) == source_before);
      }
    }
  }

  std::println("  PASS");
}

static void test_phase8_parallel_grammar_stop_reservoir_and_failure() {
  std::println("test_phase8_parallel_grammar_stop_reservoir_and_failure");

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto dag = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(dag);

  auto base_options = larch::grammar_spr_enumeration_options{};
  base_options.source = larch::chart_spr_candidate_source::grammar;
  base_options.randomize_order = true;
  base_options.seed = 19;

  // A callback stop inside a full construction wave commits exactly the same
  // semantic prefix/counters and RNG boundary as the serial oracle; only the
  // dedicated speculative counter observes the drained tail.
  auto serial_options = base_options;
  auto serial_scheduler = make_projection_scheduler(1);
  serial_options.sampled_tree_projection_scheduler = &serial_scheduler;
  std::vector<larch::grammar_spr_candidate> serial_prefix;
  auto serial_stats = larch::for_each_grammar_spr_candidate(
      grammar, serial_options,
      [&](larch::grammar_spr_candidate const& candidate) {
        serial_prefix.push_back(candidate);
        return false;
      });
  CHECK(serial_prefix.size() == 1);

  auto parallel_options = base_options;
  auto parallel_scheduler = make_projection_scheduler(4);
  parallel_options.sampled_tree_projection_scheduler = &parallel_scheduler;
  std::vector<larch::grammar_spr_candidate> parallel_prefix;
  auto parallel_stats = larch::for_each_grammar_spr_candidate(
      grammar, parallel_options,
      [&](larch::grammar_spr_candidate const& candidate) {
        parallel_prefix.push_back(candidate);
        return false;
      });
  CHECK(parallel_prefix.size() == 1);
  check_candidate_payload_equal(grammar, serial_prefix.front(),
                                parallel_prefix.front());
  check_legacy_generation_stats_equal(serial_stats, parallel_stats);
  CHECK(parallel_stats.grammar_candidate_parallel_operations > 0);
  CHECK(parallel_stats.grammar_candidate_speculative_discarded > 0);
  CHECK(parallel_scheduler.metrics().pending_tasks == 0);

  // Path-pair stopping drains the already-enumerated canonical prefix before
  // publishing the path-budget reason and rolls back no-longer-reachable work.
  auto serial_path_options = base_options;
  serial_path_options.max_path_pairs_considered = 5;
  auto serial_path_scheduler = make_projection_scheduler(1);
  serial_path_options.sampled_tree_projection_scheduler =
      &serial_path_scheduler;
  larch::chart_spr_candidate_generation_stats serial_path_stats;
  auto serial_path = collect_candidates(grammar, serial_path_options,
                                        &serial_path_stats);
  auto parallel_path_options = serial_path_options;
  auto parallel_path_scheduler = make_projection_scheduler(4);
  parallel_path_options.sampled_tree_projection_scheduler =
      &parallel_path_scheduler;
  larch::chart_spr_candidate_generation_stats parallel_path_stats;
  auto parallel_path = collect_candidates(grammar, parallel_path_options,
                                          &parallel_path_stats);
  CHECK(parallel_path.size() == serial_path.size());
  check_legacy_generation_stats_equal(serial_path_stats, parallel_path_stats);
  for (std::size_t i = 0; i < serial_path.size(); ++i) {
    check_candidate_payload_equal(grammar, serial_path[i], parallel_path[i]);
  }
  CHECK(parallel_path_stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::path_budget);

  // Reservoir draws occur only during canonical gather. Exhaustive child
  // enumeration may be parallel, but every seed/worker count must select the
  // identical ordered subset and retain identical legacy counters.
  for (auto seed : {std::uint32_t{1}, std::uint32_t{7}, std::uint32_t{19}}) {
    std::vector<std::string> baseline;
    larch::chart_spr_candidate_generation_stats baseline_stats;
    for (auto workers :
         {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
      auto scheduler = make_projection_scheduler(workers);
      auto options = base_options;
      options.seed = seed;
      options.reservoir_sample = true;
      options.max_candidates = 4;
      options.sampled_tree_projection_scheduler = &scheduler;
      larch::chart_spr_candidate_generation_stats stats;
      auto selected = collect_candidate_signature_vector(grammar, options,
                                                         &stats);
      if (workers == 1) {
        baseline = selected;
        baseline_stats = stats;
      } else {
        CHECK(selected == baseline);
        check_legacy_generation_stats_equal(stats, baseline_stats);
        CHECK(stats.grammar_candidate_parallel_operations > 0);
      }
      CHECK(scheduler.metrics().pending_tasks == 0);
    }
  }

  // Multiple workers fail in one wave. Even when a later ordinal completes
  // first, gather rethrows the lowest canonical failing ordinal and leaves the
  // persistent scheduler reusable.
  auto failure_scheduler = make_projection_scheduler(4);
  auto failure_options = base_options;
  failure_options.sampled_tree_projection_scheduler = &failure_scheduler;
  std::latch later_failure_started{1};
  failure_options.before_grammar_candidate_construction_for_tests =
      [&](std::size_t ordinal) {
        if (ordinal == 0) {
          later_failure_started.wait();
          throw std::runtime_error("forced grammar failure 0");
        }
        if (ordinal == 4) {
          later_failure_started.count_down();
          throw std::runtime_error("forced grammar failure 4");
        }
      };
  std::string failure;
  try {
    (void)collect_candidates(grammar, failure_options);
  } catch (std::runtime_error const& error) {
    failure = error.what();
  }
  CHECK(failure == "forced grammar failure 0");
  CHECK(failure_scheduler.metrics().pending_tasks == 0);
  failure_options.before_grammar_candidate_construction_for_tests = {};
  auto recovered = collect_candidates(grammar, failure_options);
  CHECK(!recovered.empty());
  CHECK(failure_scheduler.metrics().pending_tasks == 0);

  std::println("  PASS");
}

static void test_phase8_production_small_grammar_waves_run_inline() {
  std::println("test_phase8_production_small_grammar_waves_run_inline");

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto dag = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(dag);

  auto run = [&](std::size_t minimum_grain, std::mutex& handoff) {
    larch::chart_scheduler scheduler{
        larch::chart_scheduler_options{
            .requested_workers = 8,
            .default_minimum_grain = minimum_grain,
            .default_target_ranges_per_worker = 4},
        larch::chart_worker_topology_snapshot{
            .affinity_logical_cpu_count = 8,
            .affinity_physical_core_count = 8,
            .hardware_thread_count = 8}};
    larch::grammar_spr_enumeration_options options;
    options.source = larch::chart_spr_candidate_source::grammar;
    options.max_candidates = 64;
    options.max_candidates_is_post_dedup = true;
    options.grammar_candidate_maximum_wave_size = 32;
    options.sampled_tree_projection_scheduler = &scheduler;
    options.sampled_tree_projection_scheduler_handoff_mutex = &handoff;
    larch::chart_spr_candidate_generation_stats stats;
    auto candidates = collect_candidates(grammar, options, &stats);
    auto const metrics = scheduler.metrics();
    CHECK(metrics.pending_tasks == 0);
    scheduler.shutdown();
    return std::tuple{std::move(candidates), stats, metrics};
  };

  std::mutex scheduled_handoff;
  auto [scheduled_candidates, scheduled_stats, scheduled_metrics] =
      run(1, scheduled_handoff);
  CHECK(!scheduled_candidates.empty());
  CHECK(scheduled_stats.grammar_candidate_construction_waves > 0);
  CHECK(scheduled_stats.grammar_candidate_scheduler_operations ==
        scheduled_stats.grammar_candidate_construction_waves);
  CHECK(scheduled_stats.grammar_candidate_parallel_operations > 0);
  CHECK(scheduled_metrics.operations ==
        scheduled_stats.grammar_candidate_scheduler_operations);

  std::mutex inline_handoff;
  auto [inline_candidates, inline_stats, inline_metrics] =
      run(64, inline_handoff);
  CHECK(inline_candidates.size() == scheduled_candidates.size());
  check_legacy_generation_stats_equal(inline_stats, scheduled_stats);
  for (std::size_t index = 0; index < inline_candidates.size(); ++index) {
    check_candidate_payload_equal(grammar, inline_candidates[index],
                                  scheduled_candidates[index]);
  }
  CHECK(inline_stats.grammar_candidate_construction_waves ==
        scheduled_stats.grammar_candidate_construction_waves);
  CHECK(inline_stats.grammar_candidate_scheduler_operations == 0);
  CHECK(inline_stats.grammar_candidate_parallel_operations == 0);
  CHECK(inline_stats.grammar_candidate_ranges == 0);
  CHECK(inline_stats.grammar_candidate_worker_tasks == 0);
  CHECK(inline_stats.grammar_candidate_active_worker_high_water == 0);
  CHECK(inline_stats.grammar_candidate_scheduler_handoff_stall_nanoseconds ==
        0);
  CHECK(inline_stats.grammar_candidate_peak_wave_size <= 32);
  CHECK(inline_stats.grammar_candidate_admitted_wave_width == 32);
  CHECK(inline_stats.grammar_candidate_speculative_discarded ==
        scheduled_stats.grammar_candidate_speculative_discarded);
  CHECK(inline_stats.grammar_candidate_actual_peak_bytes ==
        scheduled_stats.grammar_candidate_actual_peak_bytes);
  CHECK(inline_stats.grammar_candidate_cancellations ==
        scheduled_stats.grammar_candidate_cancellations);
  CHECK(inline_metrics.operations == 0);
  CHECK(inline_metrics.tasks_submitted == 0);

  std::mutex repeated_handoff;
  auto [repeated_candidates, repeated_stats, repeated_metrics] =
      run(64, repeated_handoff);
  CHECK(repeated_candidates.size() == inline_candidates.size());
  check_exhaustive_generation_work_equal(repeated_stats, inline_stats);
  for (std::size_t index = 0; index < inline_candidates.size(); ++index) {
    check_candidate_payload_equal(grammar, repeated_candidates[index],
                                  inline_candidates[index]);
  }
  CHECK(repeated_metrics.operations == 0);

  std::println("  PASS");
}

static void test_phase8_sampled_hybrid_reservoir_worker_seed_matrix() {
  std::println("test_phase8_sampled_hybrid_reservoir_worker_seed_matrix");

  auto dag =
      larch::test::make_tiny_labelled_tree("A", eight_taxon_balanced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto const source_before = snapshot_projection_source(dag);
  constexpr std::size_t reservoir_size = 4;

  for (auto source : {larch::chart_spr_candidate_source::sampled_tree,
                      larch::chart_spr_candidate_source::hybrid}) {
    for (auto seed : {std::uint32_t{1}, std::uint32_t{7}, std::uint32_t{19}}) {
      std::vector<larch::grammar_spr_candidate> w1_selected;
      larch::chart_spr_candidate_generation_stats w1_stats;
      for (auto workers :
           {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto make_options = [&](larch::chart_scheduler& scheduler) {
          larch::grammar_spr_enumeration_options options;
          options.source = source;
          options.sampled_tree_source_dag = &dag;
          options.sampled_tree_count = 1;
          options.sampled_tree_spr_radius = 32;
          options.sampled_tree_score_threshold =
              std::numeric_limits<int>::max();
          options.randomize_order = true;
          options.seed = seed;
          options.max_candidates_is_post_dedup = true;
          options.sampled_tree_projection_scheduler = &scheduler;
          return options;
        };

        // This is the exact child stream the reservoir wrapper must execute:
        // no finite cap and therefore no two-source lookahead restriction.
        auto scheduler = make_projection_scheduler(workers);
        auto exhaustive_options = make_options(scheduler);
        exhaustive_options.max_candidates = 0;
        larch::chart_spr_candidate_generation_stats exhaustive_stats;
        auto exhaustive =
            collect_candidates(grammar, exhaustive_options, &exhaustive_stats);
        CHECK(exhaustive.size() > reservoir_size);
        CHECK(exhaustive_stats.candidates_generated_after_dedup ==
              exhaustive.size());
        CHECK(exhaustive_stats.stop_reason ==
              larch::chart_spr_candidate_stop_reason::exhausted);
        CHECK(exhaustive_stats.sampled_tree_sources_enumerated >= workers);
        CHECK(exhaustive_stats.sampled_tree_source_admitted_wave_width ==
              workers);
        CHECK(exhaustive_stats.sampled_tree_source_peak_wave_size == workers);
        auto const projection_jobs_per_worker =
            workers == 1 ? std::size_t{4} : std::size_t{64};
        CHECK(exhaustive_stats.sampled_tree_projection_admitted_subwave_width <=
              workers * projection_jobs_per_worker);
        CHECK(exhaustive_stats.sampled_tree_projection_admitted_subwave_width >=
              exhaustive_stats.sampled_tree_projection_peak_wave_size);
        auto const exhaustive_metrics = scheduler.metrics();
        CHECK(exhaustive_metrics.pending_tasks == 0);
        CHECK(exhaustive_metrics.tasks_submitted ==
              exhaustive_metrics.tasks_completed);
        CHECK(exhaustive_metrics.tasks_submitted ==
              exhaustive_metrics.tasks_joined);

        // Replay Algorithm R independently over the exact child stream. This
        // freezes both replacement draws and the wrapper's final shuffle; a
        // seed-ignoring or first-K implementation cannot pass by returning an
        // arbitrary exhaustive subset.
        std::vector<larch::grammar_spr_candidate> expected_reservoir;
        expected_reservoir.reserve(reservoir_size);
        std::mt19937 reservoir_oracle_rng(seed ^ 0x9e3779b9U);
        std::size_t reservoir_oracle_seen = 0;
        for (auto const& candidate : exhaustive) {
          ++reservoir_oracle_seen;
          if (expected_reservoir.size() < reservoir_size) {
            expected_reservoir.push_back(candidate);
            continue;
          }
          std::uniform_int_distribution<std::size_t> distribution(
              0, reservoir_oracle_seen - 1);
          auto const slot = distribution(reservoir_oracle_rng);
          if (slot < reservoir_size) expected_reservoir[slot] = candidate;
        }
        std::shuffle(expected_reservoir.begin(), expected_reservoir.end(),
                     reservoir_oracle_rng);

        auto reservoir_options = make_options(scheduler);
        reservoir_options.reservoir_sample = true;
        reservoir_options.max_candidates = reservoir_size;
        larch::chart_spr_candidate_generation_stats reservoir_stats;
        auto selected =
            collect_candidates(grammar, reservoir_options, &reservoir_stats);
        CHECK(selected.size() == reservoir_size);
        CHECK(reservoir_stats.candidates_generated_after_dedup ==
              exhaustive.size());
        CHECK(reservoir_stats.stop_reason ==
              larch::chart_spr_candidate_stop_reason::exhausted);
        check_exhaustive_generation_work_equal(reservoir_stats,
                                               exhaustive_stats);
        CHECK(reservoir_stats.sampled_tree_source_admitted_wave_width ==
              workers);
        CHECK(reservoir_stats.sampled_tree_source_peak_wave_size == workers);
        CHECK(reservoir_stats.sampled_tree_projection_admitted_subwave_width ==
              exhaustive_stats.sampled_tree_projection_admitted_subwave_width);
        CHECK(selected.size() == expected_reservoir.size());
        for (std::size_t i = 0; i < selected.size(); ++i) {
          check_candidate_payload_equal(grammar, expected_reservoir[i],
                                        selected[i]);
        }

        for (auto const& candidate : selected) {
          auto const signature =
              larch::chart_spr_candidate_taxon_signature(grammar, candidate);
          CHECK(std::ranges::any_of(
              exhaustive, [&](larch::grammar_spr_candidate const& underlying) {
                return larch::chart_spr_candidate_taxon_signature(
                           grammar, underlying) == signature;
              }));
        }
        if (workers == 1) {
          w1_selected = selected;
          w1_stats = reservoir_stats;
        } else {
          CHECK(selected.size() == w1_selected.size());
          check_legacy_generation_stats_equal(reservoir_stats, w1_stats);
          for (std::size_t i = 0; i < selected.size(); ++i) {
            check_candidate_payload_equal(grammar, selected[i], w1_selected[i]);
          }
          CHECK(reservoir_stats
                    .sampled_tree_source_enumeration_parallel_operations > 0);
          CHECK(reservoir_stats.sampled_tree_projection_parallel_operations >
                0);
          if (source == larch::chart_spr_candidate_source::hybrid) {
            CHECK(reservoir_stats.grammar_candidate_parallel_operations > 0);
          }
        }
        auto const reservoir_metrics = scheduler.metrics();
        CHECK(reservoir_metrics.pending_tasks == 0);
        CHECK(reservoir_metrics.tasks_submitted ==
              reservoir_metrics.tasks_completed);
        CHECK(reservoir_metrics.tasks_submitted ==
              reservoir_metrics.tasks_joined);
        scheduler.shutdown();
        CHECK(snapshot_projection_source(dag) == source_before);
      }
    }
  }

  std::println("  PASS");
}

static void test_phase8_projection_budget_and_failure_atomicity() {
  std::println("test_phase8_projection_budget_and_failure_atomicity");
  using larch::chart_spr_detail::preassign_sampled_tree_projection_jobs;
  using larch::chart_spr_detail::project_preassigned_sampled_tree_moves;

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto source = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(source);

  larch::grammar_spr_enumeration_options sample_options;
  sample_options.sampled_tree_source_dag = &source;
  sample_options.sampled_tree_spr_radius = 8;
  sample_options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
  sample_options.randomize_order = true;
  sample_options.seed = 19;
  std::mt19937 tree_rng(sample_options.seed);
  auto tree = larch::chart_spr_detail::build_sampled_tree_from_grammar(
      grammar, sample_options, 0, tree_rng);
  auto const projection_rng_state = tree_rng;
  auto radius = sample_options.sampled_tree_spr_radius;
  auto prepared =
      larch::chart_spr_detail::prepare_sampled_tree_projection(grammar, tree);
  auto source_before = snapshot_projection_source(tree);

  auto collect_projection = [&](auto const& preassignment,
                                auto const& options) {
    std::vector<std::optional<larch::grammar_spr_candidate>> projected;
    auto execution = project_preassigned_sampled_tree_moves(
        prepared, preassignment, options,
        [&](std::size_t ordinal,
            std::optional<larch::grammar_spr_candidate> const& candidate) {
          CHECK(ordinal == projected.size());
          projected.push_back(candidate);
          return true;
        });
    CHECK(execution.moves_preassigned == projected.size());
    return projected;
  };
  auto check_projection_vectors =
      [&](std::vector<std::optional<larch::grammar_spr_candidate>> const& lhs,
          std::vector<std::optional<larch::grammar_spr_candidate>> const& rhs) {
        CHECK(lhs.size() == rhs.size());
        for (std::size_t i = 0; i < lhs.size(); ++i) {
          CHECK(lhs[i].has_value() == rhs[i].has_value());
          if (lhs[i]) check_candidate_payload_equal(grammar, *lhs[i], *rhs[i]);
        }
      };

  auto baseline_scheduler = make_projection_scheduler(4);
  auto baseline_options = sample_options;
  baseline_options.sampled_tree_projection_scheduler = &baseline_scheduler;
  auto baseline_rng = projection_rng_state;
  auto baseline_preassignment = preassign_sampled_tree_projection_jobs(
      prepared, baseline_options, radius, baseline_rng);
  CHECK(baseline_preassignment.jobs.size() >= 8);
  CHECK(baseline_preassignment.enumeration_passes == 2);
  CHECK(baseline_preassignment.move_enumeration_visits ==
        2 * baseline_preassignment.jobs.size());
  auto baseline = collect_projection(baseline_preassignment, baseline_options);
  std::size_t local_stop_calls = 0;
  auto local_stop = project_preassigned_sampled_tree_moves(
      prepared, baseline_preassignment, baseline_options,
      [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
        ++local_stop_calls;
        return false;
      });
  CHECK(local_stop_calls == 1);
  CHECK(local_stop.speculative_discarded + 1 ==
        baseline_preassignment.memory.wave_size);

  // E is the irreducible one-slot projection peak. E succeeds exactly; E-1
  // rejects before the all-moves vector, result wave, scheduler operation, or
  // projection/callback hooks.
  auto exact_scheduler = make_projection_scheduler(4);
  auto exact_options = sample_options;
  exact_options.sampled_tree_projection_scheduler = &exact_scheduler;
  auto const exact_estimate =
      larch::chart_spr_detail::estimate_sampled_tree_projection_memory(
          prepared, &exact_scheduler, baseline_preassignment.jobs.size(), 1, 0);
  CHECK(exact_estimate.safely_bounded);
  bool retained_witness_safely_bounded = true;
  auto const retained_witness_bytes = larch::chart_spr_detail::
      estimate_sampled_tree_projection_retained_witness_bytes(
          prepared, retained_witness_safely_bounded);
  CHECK(retained_witness_safely_bounded);
  CHECK(retained_witness_bytes > 0);
  // One ordinal retains both the active candidate and its spare high-water
  // pool.  This seam makes the exact E/E-1 admission boundary below depend on
  // nested witness-child and edge-alternative storage.
  CHECK(exact_estimate.retained_output_bytes >= 2 * retained_witness_bytes);
  auto const exact_budget = exact_estimate.required_peak_bytes;
  CHECK(exact_budget > 0);
  auto const nested_retained_charge = 2 * retained_witness_bytes;
  CHECK(exact_budget > nested_retained_charge);
  bool nested_backstop_rejected = false;
  try {
    (void)larch::chart_spr_detail::admit_sampled_tree_projection_memory(
        prepared, &exact_scheduler, baseline_preassignment.jobs.size(), 0,
        exact_budget - nested_retained_charge);
  } catch (larch::sampled_tree_projection_budget_error const& error) {
    nested_backstop_rejected = true;
    CHECK(error.required_bytes() == exact_budget);
    CHECK(error.budget_bytes() == exact_budget - nested_retained_charge);
  }
  CHECK(nested_backstop_rejected);
  std::size_t exact_workspace_hooks = 0;
  exact_options.sampled_tree_projection_memory_budget_bytes = exact_budget;
  exact_options.before_sampled_tree_projection_workspace_allocation_for_tests =
      [&] { ++exact_workspace_hooks; };
  auto exact_rng = projection_rng_state;
  auto exact_preassignment = preassign_sampled_tree_projection_jobs(
      prepared, exact_options, radius, exact_rng);
  CHECK(exact_workspace_hooks == 1);
  CHECK(exact_preassignment.memory.required_peak_bytes == exact_budget);
  CHECK(exact_preassignment.memory.wave_size == 1);
  auto exact = collect_projection(exact_preassignment, exact_options);
  check_projection_vectors(baseline, exact);

  auto rejected_scheduler = make_projection_scheduler(4);
  auto rejected_options = sample_options;
  rejected_options.sampled_tree_projection_scheduler = &rejected_scheduler;
  rejected_options.sampled_tree_projection_memory_budget_bytes =
      exact_budget - 1;
  std::size_t rejected_workspace_hooks = 0;
  std::atomic<std::size_t> rejected_projection_hooks{0};
  rejected_options
      .before_sampled_tree_projection_workspace_allocation_for_tests = [&] {
    ++rejected_workspace_hooks;
  };
  rejected_options.before_sampled_tree_projection_for_tests = [&](std::size_t) {
    rejected_projection_hooks.fetch_add(1, std::memory_order_relaxed);
  };
  auto rejected_rng = projection_rng_state;
  bool rejected = false;
  try {
    (void)preassign_sampled_tree_projection_jobs(prepared, rejected_options,
                                                 radius, rejected_rng);
  } catch (larch::sampled_tree_projection_budget_error const& error) {
    rejected = true;
    CHECK(error.required_bytes() == exact_budget);
    CHECK(error.budget_bytes() == exact_budget - 1);
  }
  CHECK(rejected);
  CHECK(rejected_workspace_hooks == 0);
  CHECK(rejected_projection_hooks.load(std::memory_order_relaxed) == 0);
  auto rejected_metrics = rejected_scheduler.metrics();
  CHECK(rejected_metrics.operations == 0);
  CHECK(rejected_metrics.pool_lifetimes == 0);
  CHECK(rejected_metrics.pending_tasks == 0);

  // A finite budget between the one-slot and maximum-wave peaks reduces wave
  // concurrency instead of rejecting an otherwise admissible projection.
  auto pressure_scheduler = make_projection_scheduler(4);
  auto pressure_options = sample_options;
  pressure_options.sampled_tree_projection_scheduler = &pressure_scheduler;
  auto const two_slot_estimate =
      larch::chart_spr_detail::estimate_sampled_tree_projection_memory(
          prepared, &pressure_scheduler, baseline_preassignment.jobs.size(), 2,
          0);
  pressure_options.sampled_tree_projection_memory_budget_bytes =
      two_slot_estimate.required_peak_bytes;
  auto pressure_rng = projection_rng_state;
  auto pressure_preassignment = preassign_sampled_tree_projection_jobs(
      prepared, pressure_options, radius, pressure_rng);
  CHECK(pressure_preassignment.memory.wave_size == 2);
  CHECK(pressure_preassignment.memory.required_peak_bytes ==
        two_slot_estimate.required_peak_bytes);

  // Multiple task failures rethrow the lowest canonical range/ordinal and
  // publish no gather result. Every submitted task joins before the throw, and
  // the same scheduler/context recovers on the next operation.
  auto failure_scheduler = make_projection_scheduler(4);
  auto failure_options = sample_options;
  failure_options.sampled_tree_projection_scheduler = &failure_scheduler;
  auto failure_rng = projection_rng_state;
  auto failure_preassignment = preassign_sampled_tree_projection_jobs(
      prepared, failure_options, radius, failure_rng);
  auto failure_plan = failure_scheduler.plan_indexed_ranges(
      failure_preassignment.memory.wave_size,
      {.minimum_grain = 1, .target_ranges_per_worker = 1});
  CHECK(failure_plan.range_count >= 2);
  auto const later_ordinal = failure_plan.effective_grain;
  std::latch later_failure_started{1};
  failure_options.before_sampled_tree_projection_for_tests =
      [&](std::size_t ordinal) {
        if (ordinal == 0) {
          later_failure_started.wait();
          throw std::runtime_error("lowest projection ordinal");
        }
        if (ordinal == later_ordinal) {
          later_failure_started.count_down();
          throw std::runtime_error("later projection ordinal");
        }
      };
  std::size_t failed_gather_calls = 0;
  bool stable_failure = false;
  try {
    (void)project_preassigned_sampled_tree_moves(
        prepared, failure_preassignment, failure_options,
        [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          ++failed_gather_calls;
          return true;
        });
  } catch (std::runtime_error const& error) {
    stable_failure = std::string{error.what()} == "lowest projection ordinal";
  }
  CHECK(stable_failure);
  CHECK(failed_gather_calls == 0);
  auto failed_metrics = failure_scheduler.metrics();
  CHECK(failed_metrics.pending_tasks == 0);
  CHECK(failed_metrics.tasks_submitted == failed_metrics.tasks_completed);
  CHECK(failed_metrics.tasks_submitted == failed_metrics.tasks_joined);
  failure_options.before_sampled_tree_projection_for_tests = {};
  auto recovered = collect_projection(failure_preassignment, failure_options);
  check_projection_vectors(baseline, recovered);

  // Inject failure after one accepted submit. Its stable first range may
  // finish speculatively, but zero results are gathered and the one task is
  // completed/joined before propagation. The one-shot hook then recovers.
  auto submit_scheduler = make_projection_scheduler(4);
  auto submit_options = sample_options;
  submit_options.sampled_tree_projection_scheduler = &submit_scheduler;
  auto submit_rng = projection_rng_state;
  auto submit_preassignment = preassign_sampled_tree_projection_jobs(
      prepared, submit_options, radius, submit_rng);
  submit_options.force_sampled_tree_projection_submit_failure_after_for_tests =
      1;
  larch::sampled_tree_projection_scheduler_diagnostics submit_diagnostics;
  submit_options.sampled_tree_projection_scheduler_diagnostics_sink =
      &submit_diagnostics;
  std::atomic<std::size_t> submit_application_failures{0};
  submit_options.before_sampled_tree_projection_for_tests =
      [&](std::size_t ordinal) {
        if (ordinal == 0) {
          submit_application_failures.fetch_add(1, std::memory_order_relaxed);
          throw std::runtime_error(
              "application failure behind scheduler submission failure");
        }
      };
  auto submit_before = submit_scheduler.metrics();
  std::size_t submit_gather_calls = 0;
  bool submit_failed = false;
  try {
    (void)project_preassigned_sampled_tree_moves(
        prepared, submit_preassignment, submit_options,
        [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          ++submit_gather_calls;
          return true;
        });
  } catch (larch::chart_scheduler_submit_error const&) {
    submit_failed = true;
  }
  CHECK(submit_failed);
  CHECK(submit_gather_calls == 0);
  CHECK(submit_application_failures.load(std::memory_order_relaxed) == 1);
  CHECK(submit_diagnostics.operations == 1);
  CHECK(submit_diagnostics.worker_tasks == 1);
  auto submit_after = submit_scheduler.metrics();
  CHECK(submit_after.tasks_submitted - submit_before.tasks_submitted == 1);
  CHECK(submit_after.tasks_completed - submit_before.tasks_completed == 1);
  CHECK(submit_after.tasks_joined - submit_before.tasks_joined == 1);
  CHECK(submit_after.pending_tasks == 0);
  submit_options.force_sampled_tree_projection_submit_failure_after_for_tests
      .reset();
  submit_options.before_sampled_tree_projection_for_tests = {};
  submit_options.sampled_tree_projection_scheduler_diagnostics_sink = nullptr;
  auto submit_recovered =
      collect_projection(submit_preassignment, submit_options);
  check_projection_vectors(baseline, submit_recovered);

  // Nested same-scheduler fallback inherits the outer stable slot even when
  // the inner projection plan has fewer worker tasks.  The admitted workspace
  // object domain must therefore cover all resolved slots, while only the
  // inner active count owns populated dynamic capacity. A failure quarantines
  // that inherited slot across every later nested range and a fresh invocation
  // can reuse it normally.
  auto nested_scheduler = make_projection_scheduler(8);
  auto nested_options = sample_options;
  nested_options.sampled_tree_projection_scheduler = &nested_scheduler;
  nested_options.sampled_tree_projection_maximum_wave_size = 2;
  auto nested_rng = projection_rng_state;
  auto nested_preassignment = preassign_sampled_tree_projection_jobs(
      prepared, nested_options, radius, nested_rng);
  CHECK(nested_preassignment.jobs.size() >= 2);
  CHECK(nested_preassignment.memory.wave_size == 2);
  CHECK(nested_preassignment.memory.stable_slot_count == 8);
  CHECK(nested_preassignment.memory.active_projection_count == 2);
  CHECK(
      nested_preassignment.memory.stable_slot_scratch_bytes >=
      sizeof(
          std::vector<larch::chart_spr_detail::sampled_tree_direct_workspace>) +
          8 * sizeof(larch::chart_spr_detail::sampled_tree_direct_workspace));
  std::atomic<std::size_t> nested_failure_hooks{0};
  nested_options.before_sampled_tree_projection_for_tests =
      [&](std::size_t ordinal) {
        nested_failure_hooks.fetch_add(1, std::memory_order_relaxed);
        if (ordinal == 0) {
          throw std::runtime_error("nested inherited-slot projection failure");
        }
      };
  std::string nested_failure;
  std::size_t nested_failed_gather_calls = 0;
  (void)nested_scheduler.for_each_indexed_range(
      8, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      [&](larch::chart_indexed_range const&, std::size_t stable_slot,
          larch::chart_scheduler_cancellation_token const&) {
        if (stable_slot != 7) return;
        try {
          (void)project_preassigned_sampled_tree_moves(
              prepared, nested_preassignment, nested_options,
              [&](std::size_t,
                  std::optional<larch::grammar_spr_candidate> const&) {
                ++nested_failed_gather_calls;
                return true;
              });
        } catch (std::runtime_error const& error) {
          nested_failure = error.what();
        }
      });
  CHECK(nested_failure == "nested inherited-slot projection failure");
  CHECK(nested_failure_hooks.load(std::memory_order_relaxed) == 1);
  CHECK(nested_failed_gather_calls == 0);
  CHECK(nested_scheduler.metrics().nested_serial_fallbacks > 0);

  // A later captured failure is not itself a completed speculative result. An
  // earlier canonical stop suppresses it and reports an exact zero discard
  // count; the quarantined slot does not attempt work beyond ordinal one.
  nested_failure_hooks.store(0, std::memory_order_relaxed);
  nested_options.before_sampled_tree_projection_for_tests =
      [&](std::size_t ordinal) {
        nested_failure_hooks.fetch_add(1, std::memory_order_relaxed);
        if (ordinal == 1) {
          throw std::runtime_error(
              "nested failure behind canonical projection stop");
        }
      };
  std::optional<
      larch::chart_spr_detail::sampled_tree_projection_execution_stats>
      nested_stopped;
  std::size_t nested_stopped_gather_calls = 0;
  (void)nested_scheduler.for_each_indexed_range(
      8, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      [&](larch::chart_indexed_range const&, std::size_t stable_slot,
          larch::chart_scheduler_cancellation_token const&) {
        if (stable_slot != 7) return;
        nested_stopped = project_preassigned_sampled_tree_moves(
            prepared, nested_preassignment, nested_options,
            [&](std::size_t,
                std::optional<larch::grammar_spr_candidate> const&) {
              ++nested_stopped_gather_calls;
              return false;
            });
      });
  CHECK(nested_stopped.has_value());
  CHECK(!nested_stopped->cancelled);
  CHECK(nested_stopped->waves == 1);
  CHECK(nested_stopped->speculative_discarded == 0);
  CHECK(nested_failure_hooks.load(std::memory_order_relaxed) == 2);
  CHECK(nested_stopped_gather_calls == 1);

  nested_options.before_sampled_tree_projection_for_tests = {};
  std::size_t nested_recovery_gather_calls = 0;
  (void)nested_scheduler.for_each_indexed_range(
      8, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      [&](larch::chart_indexed_range const&, std::size_t stable_slot,
          larch::chart_scheduler_cancellation_token const&) {
        if (stable_slot != 7) return;
        auto nested_recovered = project_preassigned_sampled_tree_moves(
            prepared, nested_preassignment, nested_options,
            [&](std::size_t,
                std::optional<larch::grammar_spr_candidate> const&) {
              ++nested_recovery_gather_calls;
              return true;
            });
        CHECK(!nested_recovered.cancelled);
      });
  CHECK(nested_recovery_gather_calls == nested_preassignment.jobs.size());
  auto const nested_metrics = nested_scheduler.metrics();
  CHECK(nested_metrics.pending_tasks == 0);
  CHECK(nested_metrics.tasks_submitted == nested_metrics.tasks_completed);
  CHECK(nested_metrics.tasks_submitted == nested_metrics.tasks_joined);

  CHECK(snapshot_projection_source(tree) == source_before);
  std::println("  PASS");
}

static void test_phase8_midwave_stop_preserves_legacy_counters() {
  std::println("test_phase8_midwave_stop_preserves_legacy_counters");

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto source = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(source);

  larch::grammar_spr_enumeration_options serial_options;
  serial_options.source = larch::chart_spr_candidate_source::sampled_tree;
  serial_options.sampled_tree_source_dag = &source;
  serial_options.sampled_tree_count = 1;
  serial_options.sampled_tree_spr_radius = 8;
  serial_options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
  serial_options.randomize_order = true;
  serial_options.seed = 7;
  std::vector<std::size_t> serial_source_ordinals;
  serial_options.before_sampled_tree_source_enumeration_for_tests =
      [&](std::size_t source_ordinal, std::size_t) {
        serial_source_ordinals.push_back(source_ordinal);
      };

  std::vector<larch::grammar_spr_candidate> serial_candidates;
  auto serial_stats = larch::for_each_grammar_spr_candidate(
      grammar, serial_options,
      [&](larch::grammar_spr_candidate const& candidate) {
        serial_candidates.push_back(candidate);
        return false;
      });
  CHECK(serial_candidates.size() == 1);
  CHECK(serial_stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::callback_stop);
  CHECK(serial_stats.sampled_tree_projection_peak_wave_size == 1);
  CHECK(serial_stats.sampled_tree_projection_speculative_discarded == 0);
  CHECK(serial_stats.sampled_tree_source_waves ==
        serial_stats.sampled_tree_sources_enumerated);
  CHECK(serial_source_ordinals.size() ==
        serial_stats.sampled_tree_sources_enumerated);
  CHECK(!serial_source_ordinals.empty());
  for (std::size_t i = 0; i < serial_source_ordinals.size(); ++i) {
    CHECK(serial_source_ordinals[i] == i);
  }

  auto scheduler = make_projection_scheduler(4);
  auto parallel_options = serial_options;
  parallel_options.sampled_tree_projection_scheduler = &scheduler;
  std::mutex parallel_source_mutex;
  std::vector<std::size_t> parallel_source_ordinals;
  parallel_options.before_sampled_tree_source_enumeration_for_tests =
      [&](std::size_t source_ordinal, std::size_t) {
        std::lock_guard lock{parallel_source_mutex};
        parallel_source_ordinals.push_back(source_ordinal);
      };
  std::vector<larch::grammar_spr_candidate> parallel_candidates;
  auto parallel_stats = larch::for_each_grammar_spr_candidate(
      grammar, parallel_options,
      [&](larch::grammar_spr_candidate const& candidate) {
        parallel_candidates.push_back(candidate);
        return false;
      });
  CHECK(parallel_candidates.size() == 1);
  check_candidate_payload_equal(grammar, serial_candidates.front(),
                                parallel_candidates.front());
  check_legacy_generation_stats_equal(serial_stats, parallel_stats);
  CHECK(parallel_stats.sampled_tree_projection_parallel_operations > 0);
  CHECK(parallel_stats.sampled_tree_projection_speculative_discarded > 0);
  CHECK(parallel_stats.sampled_tree_source_waves == 1);
  CHECK(parallel_stats.sampled_tree_sources_enumerated == 4);
  CHECK(parallel_stats.sampled_tree_source_speculative_moves_discarded > 0);
  std::sort(parallel_source_ordinals.begin(), parallel_source_ordinals.end());
  CHECK(parallel_source_ordinals == std::vector<std::size_t>({0, 1, 2, 3}));
  CHECK(scheduler.metrics().pending_tasks == 0);

  std::println("  PASS");
}

static void test_phase8_source_wave_admission_failure_and_cancellation() {
  std::println("test_phase8_source_wave_admission_failure_and_cancellation");
  using larch::chart_spr_detail::estimate_sampled_tree_source_wave_memory;
  using larch::chart_spr_detail::project_sampled_tree_moves_in_source_waves;

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto source = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(source);

  larch::grammar_spr_enumeration_options sample_options;
  sample_options.source = larch::chart_spr_candidate_source::sampled_tree;
  sample_options.sampled_tree_source_dag = &source;
  sample_options.sampled_tree_spr_radius = 8;
  sample_options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
  sample_options.randomize_order = true;
  sample_options.seed = 19;
  sample_options.max_candidates = 8;
  sample_options.max_candidates_is_post_dedup = true;
  std::mt19937 tree_rng(sample_options.seed);
  auto tree = larch::chart_spr_detail::build_sampled_tree_from_grammar(
      grammar, sample_options, 0, tree_rng);
  auto const projection_rng_state = tree_rng;
  auto prepared =
      larch::chart_spr_detail::prepare_sampled_tree_projection(grammar, tree);
  auto const source_count = prepared.index().get_searchable_nodes().size();
  CHECK(source_count >= 4);

  // E is the irreducible source-width-1/projection-width-1 peak.  E succeeds
  // and exposes both admitted widths; E-1 rejects before any workspace hook or
  // scheduler operation.
  auto exact_scheduler = make_projection_scheduler(4);
  auto exact_options = sample_options;
  exact_options.sampled_tree_projection_scheduler = &exact_scheduler;
  auto const exact_estimate = estimate_sampled_tree_source_wave_memory(
      prepared, &exact_scheduler, source_count, 1, 1, 0);
  CHECK(exact_estimate.safely_bounded);
  auto const exact_budget = exact_estimate.required_peak_bytes;
  CHECK(exact_budget > 0);
  exact_options.sampled_tree_projection_memory_budget_bytes = exact_budget;
  std::size_t exact_workspace_hooks = 0;
  exact_options.before_sampled_tree_projection_workspace_allocation_for_tests =
      [&] { ++exact_workspace_hooks; };
  auto exact_rng = projection_rng_state;
  std::size_t exact_gather_calls = 0;
  auto exact = project_sampled_tree_moves_in_source_waves(
      prepared, exact_options, sample_options.sampled_tree_spr_radius,
      exact_rng,
      [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
        ++exact_gather_calls;
        return true;
      });
  CHECK(exact_workspace_hooks == 1);
  CHECK(exact_gather_calls > 0);
  CHECK(exact.memory.source_wave_size == 1);
  CHECK(exact.memory.projection_wave_size == 1);
  CHECK(exact.memory.required_peak_bytes == exact_budget);
  CHECK(exact.memory.actual_peak_bytes <= exact_budget);
  CHECK(exact.memory.planned_container_capacity_bytes >=
        exact.memory.actual_container_capacity_bytes);
  CHECK(exact.memory.planned_retained_payload_bytes >=
        exact.memory.actual_retained_payload_bytes);
  CHECK(exact.move_enumeration_visits == exact.moves_enumerated);
  CHECK(exact.enumeration_passes == 1);

  // The two admission dimensions are independent.  A budget at the complete
  // source-width/one-projection boundary keeps all admitted source
  // concurrency without accidentally admitting a second projection slot;
  // adding exactly the second-slot delta changes only projection width.
  auto const maximum_admission =
      larch::chart_spr_detail::admit_sampled_tree_source_wave_memory(
          prepared, &exact_scheduler, source_count, 0, 0);
  CHECK(maximum_admission.source_wave_size > 1);
  CHECK(maximum_admission.projection_wave_size > 1);
  auto const source_only_estimate = estimate_sampled_tree_source_wave_memory(
      prepared, &exact_scheduler, source_count,
      maximum_admission.source_wave_size, 1, 0);
  auto const source_only_admission =
      larch::chart_spr_detail::admit_sampled_tree_source_wave_memory(
          prepared, &exact_scheduler, source_count, 0,
          source_only_estimate.required_peak_bytes);
  CHECK(source_only_admission.source_wave_size ==
        maximum_admission.source_wave_size);
  CHECK(source_only_admission.projection_wave_size == 1);
  auto const projection_two_estimate = estimate_sampled_tree_source_wave_memory(
      prepared, &exact_scheduler, source_count,
      maximum_admission.source_wave_size, 2, 0);
  auto const projection_two_admission =
      larch::chart_spr_detail::admit_sampled_tree_source_wave_memory(
          prepared, &exact_scheduler, source_count, 0,
          projection_two_estimate.required_peak_bytes);
  CHECK(projection_two_admission.source_wave_size ==
        maximum_admission.source_wave_size);
  CHECK(projection_two_admission.projection_wave_size == 2);

  auto rejected_scheduler = make_projection_scheduler(4);
  auto rejected_options = sample_options;
  rejected_options.sampled_tree_projection_scheduler = &rejected_scheduler;
  rejected_options.sampled_tree_projection_memory_budget_bytes =
      exact_budget - 1;
  std::size_t rejected_workspace_hooks = 0;
  rejected_options
      .before_sampled_tree_projection_workspace_allocation_for_tests = [&] {
    ++rejected_workspace_hooks;
  };
  auto rejected_rng = projection_rng_state;
  bool rejected = false;
  try {
    (void)project_sampled_tree_moves_in_source_waves(
        prepared, rejected_options, sample_options.sampled_tree_spr_radius,
        rejected_rng,
        [](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          return true;
        });
  } catch (larch::sampled_tree_projection_budget_error const& error) {
    rejected = true;
    CHECK(error.required_bytes() == exact_budget);
    CHECK(error.budget_bytes() == exact_budget - 1);
  }
  CHECK(rejected);
  CHECK(rejected_workspace_hooks == 0);
  CHECK(rejected_scheduler.metrics().operations == 0);
  CHECK(rejected_scheduler.metrics().pool_lifetimes == 0);

  // Simulated actual-capacity underestimation is detected after reserve but
  // before source shuffle/submission or projection publication.
  auto backstop_scheduler = make_projection_scheduler(4);
  auto backstop_options = sample_options;
  backstop_options.sampled_tree_projection_scheduler = &backstop_scheduler;
  backstop_options.sampled_tree_projection_memory_budget_bytes = exact_budget;
  backstop_options
      .sampled_tree_source_wave_actual_capacity_extra_bytes_for_tests = 1;
  std::size_t backstop_workspace_hooks = 0;
  std::atomic<std::size_t> backstop_projection_hooks{0};
  backstop_options
      .before_sampled_tree_projection_workspace_allocation_for_tests = [&] {
    ++backstop_workspace_hooks;
  };
  backstop_options.before_sampled_tree_projection_for_tests = [&](std::size_t) {
    backstop_projection_hooks.fetch_add(1, std::memory_order_relaxed);
  };
  auto backstop_rng = projection_rng_state;
  bool backstop_rejected = false;
  try {
    (void)project_sampled_tree_moves_in_source_waves(
        prepared, backstop_options, sample_options.sampled_tree_spr_radius,
        backstop_rng,
        [](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          return true;
        });
  } catch (larch::sampled_tree_projection_budget_error const& error) {
    backstop_rejected = true;
    CHECK(error.required_bytes() > exact_budget);
    CHECK(error.budget_bytes() == exact_budget);
  }
  CHECK(backstop_rejected);
  CHECK(backstop_workspace_hooks == 1);
  CHECK(backstop_projection_hooks.load(std::memory_order_relaxed) == 0);
  CHECK(backstop_scheduler.metrics().operations == 0);
  CHECK(backstop_scheduler.metrics().pool_lifetimes == 0);

  // Per-source caught failures select the lowest canonical source after the
  // complete wave joins, independent of worker count/completion order.
  for (auto workers :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    auto scheduler = make_projection_scheduler(workers);
    auto failure_options = sample_options;
    failure_options.sampled_tree_projection_scheduler = &scheduler;
    std::latch later_source_started{1};
    failure_options.before_sampled_tree_source_enumeration_for_tests =
        [&](std::size_t source_ordinal, std::size_t) {
          if (source_ordinal == 0) {
            if (workers > 1) later_source_started.wait();
            throw std::runtime_error("lowest source ordinal");
          }
          if (workers > 1 && source_ordinal == 1) {
            later_source_started.count_down();
            throw std::runtime_error("later source ordinal");
          }
        };
    auto failure_rng = projection_rng_state;
    std::size_t failure_gather_calls = 0;
    bool stable_failure = false;
    try {
      (void)project_sampled_tree_moves_in_source_waves(
          prepared, failure_options, sample_options.sampled_tree_spr_radius,
          failure_rng,
          [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
            ++failure_gather_calls;
            return true;
          });
    } catch (std::runtime_error const& error) {
      stable_failure = std::string{error.what()} == "lowest source ordinal";
    }
    CHECK(stable_failure);
    CHECK(failure_gather_calls == 0);
    auto metrics = scheduler.metrics();
    CHECK(metrics.pending_tasks == 0);
    CHECK(metrics.tasks_submitted == metrics.tasks_completed);
    CHECK(metrics.tasks_submitted == metrics.tasks_joined);
  }

  // The product source-wave runner also preserves stable projection-ordinal
  // failure selection. Invert completion of two projection ranges, publish no
  // gather result, then reuse the same persistent scheduler successfully.
  auto projection_failure_scheduler = make_projection_scheduler(4);
  auto projection_failure_options = sample_options;
  projection_failure_options.sampled_tree_projection_scheduler =
      &projection_failure_scheduler;
  auto const projection_memory =
      larch::chart_spr_detail::admit_sampled_tree_source_wave_memory(
          prepared, &projection_failure_scheduler, source_count, 0, 0);
  auto const projection_plan = projection_failure_scheduler.plan_indexed_ranges(
      projection_memory.projection_wave_size,
      {.minimum_grain = 1, .target_ranges_per_worker = 64});
  CHECK(projection_plan.range_count >= 2);
  auto const later_projection_ordinal = projection_plan.effective_grain;
  std::latch later_projection_started{1};
  projection_failure_options.before_sampled_tree_projection_for_tests =
      [&](std::size_t ordinal) {
        if (ordinal == 0) {
          later_projection_started.wait();
          throw std::runtime_error("lowest source-wave projection ordinal");
        }
        if (ordinal == later_projection_ordinal) {
          later_projection_started.count_down();
          throw std::runtime_error("later source-wave projection ordinal");
        }
      };
  auto projection_failure_rng = projection_rng_state;
  std::size_t projection_failure_gather_calls = 0;
  bool stable_projection_failure = false;
  try {
    (void)project_sampled_tree_moves_in_source_waves(
        prepared, projection_failure_options,
        sample_options.sampled_tree_spr_radius, projection_failure_rng,
        [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          ++projection_failure_gather_calls;
          return true;
        });
  } catch (std::runtime_error const& error) {
    stable_projection_failure =
        std::string{error.what()} == "lowest source-wave projection ordinal";
  }
  CHECK(stable_projection_failure);
  CHECK(projection_failure_gather_calls == 0);
  auto projection_failure_metrics = projection_failure_scheduler.metrics();
  CHECK(projection_failure_metrics.pending_tasks == 0);
  CHECK(projection_failure_metrics.tasks_submitted ==
        projection_failure_metrics.tasks_completed);
  CHECK(projection_failure_metrics.tasks_submitted ==
        projection_failure_metrics.tasks_joined);

  projection_failure_options.before_sampled_tree_projection_for_tests = {};
  auto projection_recovery_rng = projection_rng_state;
  std::size_t projection_recovery_gather_calls = 0;
  auto projection_recovery = project_sampled_tree_moves_in_source_waves(
      prepared, projection_failure_options,
      sample_options.sampled_tree_spr_radius, projection_recovery_rng,
      [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
        ++projection_recovery_gather_calls;
        return true;
      });
  CHECK(!projection_recovery.cancelled);
  CHECK(projection_recovery_gather_calls > 0);
  auto projection_recovery_metrics = projection_failure_scheduler.metrics();
  CHECK(projection_recovery_metrics.pending_tasks == 0);
  CHECK(projection_recovery_metrics.tasks_submitted ==
        projection_recovery_metrics.tasks_completed);
  CHECK(projection_recovery_metrics.tasks_submitted ==
        projection_recovery_metrics.tasks_joined);

  // The bounded source-wave runner has the same inherited-slot contract as
  // direct preassignment. First prove that this deterministic exhaustive run
  // launches a two-range first projection subwave, then enter it from outer
  // stable slot seven. Ordinal-zero failure must quarantine that slot before
  // the already-admitted ordinal-one range can touch the workspace.
  CHECK(source_count <= 8);
  auto nested_source_probe_scheduler = make_projection_scheduler(8);
  auto nested_source_options = sample_options;
  nested_source_options.max_candidates = 0;
  nested_source_options.sampled_tree_projection_scheduler =
      &nested_source_probe_scheduler;
  nested_source_options.sampled_tree_source_maximum_wave_size = source_count;
  nested_source_options.sampled_tree_projection_maximum_wave_size = 2;
  auto nested_source_probe_rng = projection_rng_state;
  std::size_t nested_source_probe_gather_calls = 0;
  auto nested_source_probe = project_sampled_tree_moves_in_source_waves(
      prepared, nested_source_options, sample_options.sampled_tree_spr_radius,
      nested_source_probe_rng,
      [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
        ++nested_source_probe_gather_calls;
        return false;
      });
  CHECK(!nested_source_probe.cancelled);
  CHECK(nested_source_probe_gather_calls == 1);
  CHECK(nested_source_probe.moves_enumerated >= 2);
  CHECK(nested_source_probe.memory.source_wave_size == source_count);
  CHECK(nested_source_probe.memory.projection_wave_size == 2);
  CHECK(nested_source_probe.memory.stable_slot_count == 8);
  CHECK(nested_source_probe.memory.active_projection_count == 2);
  CHECK(nested_source_probe.peak_projection_wave_size == 2);
  CHECK(nested_source_probe.projection_speculative_discarded == 1);
  CHECK(nested_source_probe.source_speculative_moves_discarded ==
        nested_source_probe.moves_enumerated - 1);
  auto const nested_source_probe_metrics =
      nested_source_probe_scheduler.metrics();
  CHECK(nested_source_probe_metrics.pending_tasks == 0);
  CHECK(nested_source_probe_metrics.tasks_submitted ==
        nested_source_probe_metrics.tasks_completed);
  CHECK(nested_source_probe_metrics.tasks_submitted ==
        nested_source_probe_metrics.tasks_joined);

  auto nested_source_scheduler = make_projection_scheduler(8);
  nested_source_options.sampled_tree_projection_scheduler =
      &nested_source_scheduler;
  std::atomic<std::size_t> nested_source_failure_hooks{0};
  nested_source_options.before_sampled_tree_projection_for_tests =
      [&](std::size_t ordinal) {
        nested_source_failure_hooks.fetch_add(1, std::memory_order_relaxed);
        if (ordinal == 0) {
          throw std::runtime_error(
              "nested source inherited-slot projection failure");
        }
      };
  auto const nested_source_before = nested_source_scheduler.metrics();
  std::string nested_source_failure;
  std::size_t nested_source_failed_gather_calls = 0;
  (void)nested_source_scheduler.for_each_indexed_range(
      8, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      [&](larch::chart_indexed_range const&, std::size_t stable_slot,
          larch::chart_scheduler_cancellation_token const&) {
        if (stable_slot != 7) return;
        auto nested_source_rng = projection_rng_state;
        try {
          (void)project_sampled_tree_moves_in_source_waves(
              prepared, nested_source_options,
              sample_options.sampled_tree_spr_radius, nested_source_rng,
              [&](std::size_t,
                  std::optional<larch::grammar_spr_candidate> const&) {
                ++nested_source_failed_gather_calls;
                return true;
              });
        } catch (std::runtime_error const& error) {
          nested_source_failure = error.what();
        }
      });
  CHECK(nested_source_failure ==
        "nested source inherited-slot projection failure");
  CHECK(nested_source_failure_hooks.load(std::memory_order_relaxed) == 1);
  CHECK(nested_source_failed_gather_calls == 0);
  auto const nested_source_after = nested_source_scheduler.metrics();
  CHECK(nested_source_after.nested_serial_fallbacks -
            nested_source_before.nested_serial_fallbacks ==
        2);
  CHECK(nested_source_after.pending_tasks == 0);
  CHECK(nested_source_after.tasks_submitted ==
        nested_source_after.tasks_completed);
  CHECK(nested_source_after.tasks_submitted ==
        nested_source_after.tasks_joined);

  nested_source_options.before_sampled_tree_projection_for_tests = {};
  std::optional<
      larch::chart_spr_detail::sampled_tree_source_wave_execution_stats>
      nested_source_recovered;
  std::size_t nested_source_recovery_gather_calls = 0;
  (void)nested_source_scheduler.for_each_indexed_range(
      8, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      [&](larch::chart_indexed_range const&, std::size_t stable_slot,
          larch::chart_scheduler_cancellation_token const&) {
        if (stable_slot != 7) return;
        auto nested_source_rng = projection_rng_state;
        nested_source_recovered = project_sampled_tree_moves_in_source_waves(
            prepared, nested_source_options,
            sample_options.sampled_tree_spr_radius, nested_source_rng,
            [&](std::size_t,
                std::optional<larch::grammar_spr_candidate> const&) {
              ++nested_source_recovery_gather_calls;
              return true;
            });
      });
  CHECK(nested_source_recovered.has_value());
  CHECK(!nested_source_recovered->cancelled);
  CHECK(nested_source_recovered->peak_projection_wave_size == 2);
  CHECK(nested_source_recovered->memory.stable_slot_count == 8);
  CHECK(nested_source_recovered->memory.active_projection_count == 2);
  CHECK(nested_source_recovery_gather_calls ==
        nested_source_recovered->moves_enumerated);
  CHECK(nested_source_recovery_gather_calls >= 2);
  auto const nested_source_recovery_metrics = nested_source_scheduler.metrics();
  CHECK(nested_source_recovery_metrics.pending_tasks == 0);
  CHECK(nested_source_recovery_metrics.tasks_submitted ==
        nested_source_recovery_metrics.tasks_completed);
  CHECK(nested_source_recovery_metrics.tasks_submitted ==
        nested_source_recovery_metrics.tasks_joined);

  // External cancellation drains the launched source wave and publishes no
  // partially projected ordinal.
  auto cancel_scheduler = make_projection_scheduler(4);
  auto cancel_options = sample_options;
  cancel_options.sampled_tree_projection_scheduler = &cancel_scheduler;
  std::atomic<bool> cancel_requested{false};
  cancel_options.sampled_tree_projection_cancel_requested = &cancel_requested;
  cancel_options.before_sampled_tree_source_enumeration_for_tests =
      [&](std::size_t source_ordinal, std::size_t) {
        if (source_ordinal == 0) {
          cancel_requested.store(true, std::memory_order_release);
        }
      };
  auto cancel_rng = projection_rng_state;
  std::size_t cancel_gather_calls = 0;
  auto cancelled = project_sampled_tree_moves_in_source_waves(
      prepared, cancel_options, sample_options.sampled_tree_spr_radius,
      cancel_rng,
      [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
        ++cancel_gather_calls;
        return true;
      });
  CHECK(cancelled.cancelled);
  CHECK(cancel_gather_calls == 0);
  CHECK(cancelled.source_waves == 1);
  CHECK(cancelled.projection_waves == 0);
  CHECK(cancel_scheduler.metrics().pending_tasks == 0);
  CHECK(cancel_scheduler.metrics().tasks_submitted ==
        cancel_scheduler.metrics().tasks_completed);
  CHECK(cancel_scheduler.metrics().tasks_submitted ==
        cancel_scheduler.metrics().tasks_joined);

  // Submission failure is one-shot, joins the accepted runner, and gathers
  // nothing from the failed source operation.
  auto submit_scheduler = make_projection_scheduler(4);
  auto submit_options = sample_options;
  submit_options.sampled_tree_projection_scheduler = &submit_scheduler;
  submit_options.force_sampled_tree_projection_submit_failure_after_for_tests =
      1;
  auto submit_before = submit_scheduler.metrics();
  auto submit_rng = projection_rng_state;
  std::size_t submit_gather_calls = 0;
  bool submit_failed = false;
  try {
    (void)project_sampled_tree_moves_in_source_waves(
        prepared, submit_options, sample_options.sampled_tree_spr_radius,
        submit_rng,
        [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          ++submit_gather_calls;
          return true;
        });
  } catch (larch::chart_scheduler_submit_error const&) {
    submit_failed = true;
  }
  CHECK(submit_failed);
  CHECK(submit_gather_calls == 0);
  auto submit_after = submit_scheduler.metrics();
  CHECK(submit_after.tasks_submitted - submit_before.tasks_submitted == 1);
  CHECK(submit_after.tasks_completed - submit_before.tasks_completed == 1);
  CHECK(submit_after.tasks_joined - submit_before.tasks_joined == 1);
  CHECK(submit_after.pending_tasks == 0);

  std::println("  PASS");
}

static void test_phase8_speculative_failures_follow_canonical_order() {
  std::println("test_phase8_speculative_failures_follow_canonical_order");
  using larch::chart_spr_detail::preassign_sampled_tree_projection_jobs;
  using larch::chart_spr_detail::project_preassigned_sampled_tree_moves;
  using larch::chart_spr_detail::project_sampled_tree_moves_in_source_waves;

  std::vector<larch::phylo_dag> source_trees;
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  source_trees.push_back(
      larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto source = larch::test::merge_tiny_trees(std::move(source_trees));
  auto grammar = larch::build_clade_grammar(source);

  larch::grammar_spr_enumeration_options base_options;
  base_options.source = larch::chart_spr_candidate_source::sampled_tree;
  base_options.sampled_tree_source_dag = &source;
  base_options.sampled_tree_spr_radius = 8;
  base_options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
  base_options.randomize_order = true;
  base_options.seed = 19;
  base_options.max_candidates = 1;
  base_options.max_candidates_is_post_dedup = true;
  std::mt19937 tree_rng(base_options.seed);
  auto tree = larch::chart_spr_detail::build_sampled_tree_from_grammar(
      grammar, base_options, 0, tree_rng);
  auto prepared =
      larch::chart_spr_detail::prepare_sampled_tree_projection(grammar, tree);
  CHECK(prepared.index().get_searchable_nodes().size() >= 4);

  auto check_scheduler_quiescent = [](larch::chart_scheduler const& scheduler) {
    auto const metrics = scheduler.metrics();
    CHECK(metrics.pending_tasks == 0);
    CHECK(metrics.tasks_submitted == metrics.tasks_completed);
    CHECK(metrics.tasks_submitted == metrics.tasks_joined);
  };

  // Pick the first ordinal outside a W1 projection subwave and a deterministic
  // order that fills the cap within that first subwave. W1 never launches the
  // later ordinal; W2/W4/W8 launch it speculatively in their wider first wave.
  std::optional<std::uint32_t> projection_order_seed;
  std::size_t w1_projection_wave_size = 0;
  std::size_t projection_job_count = 0;
  for (std::uint32_t seed = 1; seed <= 256 && !projection_order_seed; ++seed) {
    auto scheduler = make_projection_scheduler(1);
    auto options = base_options;
    options.sampled_tree_projection_scheduler = &scheduler;
    std::mt19937 rng(seed);
    auto preassignment = preassign_sampled_tree_projection_jobs(
        prepared, options, base_options.sampled_tree_spr_radius, rng);
    if (preassignment.jobs.size() <= preassignment.memory.wave_size) continue;
    bool gathered_candidate = false;
    auto stopped = project_preassigned_sampled_tree_moves(
        prepared, preassignment, options,
        [&](std::size_t,
            std::optional<larch::grammar_spr_candidate> const& candidate) {
          if (!candidate) return true;
          gathered_candidate = true;
          return false;
        });
    if (gathered_candidate && stopped.waves == 1) {
      projection_order_seed = seed;
      w1_projection_wave_size = preassignment.memory.wave_size;
      projection_job_count = preassignment.jobs.size();
    }
  }
  CHECK(projection_order_seed.has_value());
  CHECK(w1_projection_wave_size > 0);
  CHECK(projection_job_count > w1_projection_wave_size);
  auto const later_projection_ordinal = w1_projection_wave_size;

  for (auto workers :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    auto scheduler = make_projection_scheduler(workers);
    auto options = base_options;
    options.sampled_tree_projection_scheduler = &scheduler;
    std::mt19937 rng(*projection_order_seed);
    auto preassignment = preassign_sampled_tree_projection_jobs(
        prepared, options, base_options.sampled_tree_spr_radius, rng);
    CHECK(preassignment.jobs.size() == projection_job_count);
    CHECK(preassignment.memory.wave_size >= w1_projection_wave_size);
    if (workers > 1) {
      CHECK(preassignment.memory.wave_size > later_projection_ordinal);
    }

    std::atomic<std::size_t> later_failure_calls{0};
    options.before_sampled_tree_projection_for_tests =
        [&](std::size_t ordinal) {
          if (ordinal == later_projection_ordinal) {
            later_failure_calls.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("speculative later projection failure");
          }
        };
    std::size_t stopped_gather_calls = 0;
    bool cap_filled = false;
    bool unexpected_failure = false;
    try {
      auto stopped = project_preassigned_sampled_tree_moves(
          prepared, preassignment, options,
          [&](std::size_t,
              std::optional<larch::grammar_spr_candidate> const& candidate) {
            ++stopped_gather_calls;
            if (!candidate) return true;
            cap_filled = true;
            return false;
          });
      CHECK(stopped.waves == 1);
      CHECK(!stopped.cancelled);
    } catch (...) {
      unexpected_failure = true;
    }
    CHECK(!unexpected_failure);
    CHECK(cap_filled);
    CHECK(stopped_gather_calls > 0);
    CHECK(stopped_gather_calls <= later_projection_ordinal);
    CHECK(later_failure_calls.load(std::memory_order_relaxed) ==
          (workers == 1 ? 0 : 1));
    check_scheduler_quiescent(scheduler);

    // Without the earlier stop, every worker count reaches the same ordinal,
    // publishes the same canonical prefix, and rethrows the same failure.
    later_failure_calls.store(0, std::memory_order_relaxed);
    std::size_t reached_gather_calls = 0;
    std::string reached_failure;
    try {
      (void)project_preassigned_sampled_tree_moves(
          prepared, preassignment, options,
          [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
            ++reached_gather_calls;
            return true;
          });
    } catch (std::runtime_error const& error) {
      reached_failure = error.what();
    }
    CHECK(reached_failure == "speculative later projection failure");
    CHECK(reached_gather_calls == later_projection_ordinal);
    CHECK(later_failure_calls.load(std::memory_order_relaxed) == 1);
    check_scheduler_quiescent(scheduler);

    // Cancellation visible at the post-join/pre-gather boundary precedes a
    // captured later projection failure. The joined scheduler remains reusable
    // afterwards.
    std::atomic<bool> cancel_requested{false};
    options.sampled_tree_projection_cancel_requested = &cancel_requested;
    options.before_sampled_tree_projection_for_tests =
        [&](std::size_t ordinal) {
          if (ordinal == 1) {
            cancel_requested.store(true, std::memory_order_release);
            throw std::runtime_error("failure after projection cancellation");
          }
        };
    std::size_t cancelled_gather_calls = 0;
    bool cancellation_threw = false;
    larch::chart_spr_detail::sampled_tree_projection_execution_stats cancelled;
    try {
      cancelled = project_preassigned_sampled_tree_moves(
          prepared, preassignment, options,
          [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
            ++cancelled_gather_calls;
            return true;
          });
    } catch (...) {
      cancellation_threw = true;
    }
    CHECK(!cancellation_threw);
    CHECK(cancelled.cancelled);
    CHECK(cancelled_gather_calls == 0);
    check_scheduler_quiescent(scheduler);

    cancel_requested.store(false, std::memory_order_release);
    options.before_sampled_tree_projection_for_tests = {};
    std::size_t recovery_gather_calls = 0;
    auto recovered = project_preassigned_sampled_tree_moves(
        prepared, preassignment, options,
        [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          ++recovery_gather_calls;
          return true;
        });
    CHECK(!recovered.cancelled);
    CHECK(recovery_gather_calls == projection_job_count);
    check_scheduler_quiescent(scheduler);
  }

  // That cancellation boundary is a linearization point, not a retroactive
  // flag poll between canonical ordinals.  Once gather has started, publishing
  // a prefix and then returning "cancelled" would violate wave atomicity.  A
  // request raised after ordinal zero enters gather is therefore deferred; the
  // already-captured canonical failure at ordinal one wins.  The next call,
  // with cancellation visible before gather, publishes nothing.
  {
    auto scheduler = make_projection_scheduler(4);
    auto options = base_options;
    options.sampled_tree_projection_scheduler = &scheduler;
    std::mt19937 rng(*projection_order_seed);
    auto preassignment = preassign_sampled_tree_projection_jobs(
        prepared, options, base_options.sampled_tree_spr_radius, rng);
    CHECK(preassignment.memory.wave_size > 1);
    CHECK(preassignment.jobs.size() > 1);
    std::atomic<bool> cancel_requested{false};
    std::atomic<std::size_t> failure_calls{0};
    options.sampled_tree_projection_cancel_requested = &cancel_requested;
    options.before_sampled_tree_projection_for_tests =
        [&](std::size_t ordinal) {
          if (ordinal == 1) {
            failure_calls.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error(
                "failure after gather cancellation boundary");
          }
        };
    std::latch gather_entered{1};
    std::latch cancellation_written{1};
    auto cancellation_setter = std::async(std::launch::async, [&] {
      gather_entered.wait();
      cancel_requested.store(true, std::memory_order_release);
      cancellation_written.count_down();
    });
    std::size_t gather_calls = 0;
    std::string failure;
    try {
      (void)project_preassigned_sampled_tree_moves(
          prepared, preassignment, options,
          [&](std::size_t ordinal,
              std::optional<larch::grammar_spr_candidate> const&) {
            CHECK(ordinal == 0);
            gather_entered.count_down();
            cancellation_written.wait();
            ++gather_calls;
            return true;
          });
    } catch (std::runtime_error const& error) {
      failure = error.what();
    }
    cancellation_setter.get();
    CHECK(failure == "failure after gather cancellation boundary");
    CHECK(gather_calls == 1);
    CHECK(failure_calls.load(std::memory_order_relaxed) == 1);
    check_scheduler_quiescent(scheduler);

    std::size_t cancelled_gather_calls = 0;
    bool cancellation_threw = false;
    larch::chart_spr_detail::sampled_tree_projection_execution_stats cancelled;
    try {
      cancelled = project_preassigned_sampled_tree_moves(
          prepared, preassignment, options,
          [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
            ++cancelled_gather_calls;
            return true;
          });
    } catch (...) {
      cancellation_threw = true;
    }
    CHECK(!cancellation_threw);
    CHECK(cancelled.cancelled);
    CHECK(cancelled_gather_calls == 0);
    CHECK(failure_calls.load(std::memory_order_relaxed) == 1);
    check_scheduler_quiescent(scheduler);
  }

  // Select a deterministic shuffle whose first source emits a move. This
  // makes source one the precise later-lookahead slot for W2/W4/W8 instead of
  // relying on the fixture's searchable-node storage order.
  std::optional<std::uint32_t> source_order_seed;
  for (std::uint32_t seed = 1; seed <= 256 && !source_order_seed; ++seed) {
    auto scheduler = make_projection_scheduler(1);
    auto options = base_options;
    options.sampled_tree_projection_scheduler = &scheduler;
    std::mt19937 rng(seed);
    std::size_t gather_calls = 0;
    auto stopped = project_sampled_tree_moves_in_source_waves(
        prepared, options, base_options.sampled_tree_spr_radius, rng,
        [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          ++gather_calls;
          return false;
        });
    if (gather_calls == 1 && stopped.sources_enumerated == 1) {
      source_order_seed = seed;
    }
  }
  CHECK(source_order_seed.has_value());

  std::optional<std::size_t> canonical_source_prefix;
  for (auto workers :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    auto scheduler = make_projection_scheduler(workers);
    auto options = base_options;
    options.sampled_tree_projection_scheduler = &scheduler;
    std::atomic<std::size_t> source_one_failure_calls{0};
    options.before_sampled_tree_source_enumeration_for_tests =
        [&](std::size_t source_ordinal, std::size_t) {
          if (source_ordinal == 1) {
            source_one_failure_calls.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("speculative later source failure");
          }
        };

    // The cap-one gather stops in source zero. Wider source waves have already
    // joined source one's failure, but it is outside the canonical boundary.
    std::mt19937 stopped_rng(*source_order_seed);
    std::size_t stopped_gather_calls = 0;
    bool stopped_threw = false;
    try {
      (void)project_sampled_tree_moves_in_source_waves(
          prepared, options, base_options.sampled_tree_spr_radius, stopped_rng,
          [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
            ++stopped_gather_calls;
            return false;
          });
    } catch (...) {
      stopped_threw = true;
    }
    CHECK(!stopped_threw);
    CHECK(stopped_gather_calls == 1);
    CHECK(source_one_failure_calls.load(std::memory_order_relaxed) ==
          (workers == 1 ? 0 : 1));
    check_scheduler_quiescent(scheduler);

    // Continuing the gather reaches source one only after all source-zero
    // projections, independent of whether source one ran speculatively.
    source_one_failure_calls.store(0, std::memory_order_relaxed);
    std::mt19937 reached_rng(*source_order_seed);
    std::size_t reached_gather_calls = 0;
    std::string reached_failure;
    try {
      (void)project_sampled_tree_moves_in_source_waves(
          prepared, options, base_options.sampled_tree_spr_radius, reached_rng,
          [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
            ++reached_gather_calls;
            return true;
          });
    } catch (std::runtime_error const& error) {
      reached_failure = error.what();
    }
    CHECK(reached_failure == "speculative later source failure");
    CHECK(source_one_failure_calls.load(std::memory_order_relaxed) == 1);
    CHECK(reached_gather_calls > 0);
    if (!canonical_source_prefix) {
      canonical_source_prefix = reached_gather_calls;
    } else {
      CHECK(reached_gather_calls == *canonical_source_prefix);
    }
    check_scheduler_quiescent(scheduler);

    // A source-wave cancellation suppresses a later captured source failure,
    // drains cleanly, and permits a normal operation on the same scheduler.
    std::atomic<bool> source_cancel_requested{false};
    options.sampled_tree_projection_cancel_requested = &source_cancel_requested;
    source_one_failure_calls.store(0, std::memory_order_relaxed);
    options.before_sampled_tree_source_enumeration_for_tests =
        [&](std::size_t source_ordinal, std::size_t) {
          if (source_ordinal == 0) {
            source_cancel_requested.store(true, std::memory_order_release);
          } else if (source_ordinal == 1) {
            source_one_failure_calls.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("failure after source cancellation");
          }
        };
    std::mt19937 cancelled_rng(*source_order_seed);
    std::size_t cancelled_gather_calls = 0;
    bool source_cancellation_threw = false;
    larch::chart_spr_detail::sampled_tree_source_wave_execution_stats cancelled;
    try {
      cancelled = project_sampled_tree_moves_in_source_waves(
          prepared, options, base_options.sampled_tree_spr_radius,
          cancelled_rng,
          [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
            ++cancelled_gather_calls;
            return true;
          });
    } catch (...) {
      source_cancellation_threw = true;
    }
    CHECK(!source_cancellation_threw);
    CHECK(cancelled.cancelled);
    CHECK(cancelled_gather_calls == 0);
    CHECK(source_one_failure_calls.load(std::memory_order_relaxed) ==
          (workers == 1 ? 0 : 1));
    check_scheduler_quiescent(scheduler);

    source_cancel_requested.store(false, std::memory_order_release);
    options.before_sampled_tree_source_enumeration_for_tests = {};
    std::mt19937 recovery_rng(*source_order_seed);
    std::size_t recovery_gather_calls = 0;
    auto recovered = project_sampled_tree_moves_in_source_waves(
        prepared, options, base_options.sampled_tree_spr_radius, recovery_rng,
        [&](std::size_t, std::optional<larch::grammar_spr_candidate> const&) {
          ++recovery_gather_calls;
          return true;
        });
    CHECK(!recovered.cancelled);
    CHECK(recovery_gather_calls > 0);
    check_scheduler_quiescent(scheduler);
  }
  std::println("  PASS");
}

static void test_phase8_adaptive_finite_source_wave_policy() {
  std::println("test_phase8_adaptive_finite_source_wave_policy");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AA", eight_taxon_distinct_balanced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  struct run_result {
    std::vector<larch::grammar_spr_candidate> candidates;
    larch::chart_spr_candidate_generation_stats stats;
  };
  auto run = [&](std::size_t workers,
                 larch::grammar_spr_enumeration_options options) {
    auto scheduler = make_projection_scheduler(workers);
    options.sampled_tree_projection_scheduler = &scheduler;
    run_result result;
    result.candidates = collect_candidates(grammar, options, &result.stats);
    auto const metrics = scheduler.metrics();
    CHECK(metrics.pending_tasks == 0);
    CHECK(metrics.tasks_submitted == metrics.tasks_completed);
    CHECK(metrics.tasks_submitted == metrics.tasks_joined);
    return result;
  };
  auto check_same_candidates = [&](run_result const& expected,
                                   run_result const& actual) {
    CHECK(actual.candidates.size() == expected.candidates.size());
    check_legacy_generation_stats_equal(actual.stats, expected.stats);
    for (std::size_t i = 0; i < actual.candidates.size(); ++i) {
      check_candidate_payload_equal(grammar, expected.candidates[i],
                                    actual.candidates[i]);
    }
  };

  larch::grammar_spr_enumeration_options low_yield;
  low_yield.source = larch::chart_spr_candidate_source::sampled_tree;
  low_yield.sampled_tree_source_dag = &dag;
  low_yield.sampled_tree_count = 1;
  low_yield.sampled_tree_spr_radius = 16;
  low_yield.sampled_tree_score_threshold = std::numeric_limits<int>::max();
  low_yield.min_moved_clade_size = 2;
  low_yield.min_target_clade_size = 2;
  low_yield.seed = 1;

  auto const exhaustive = run(1, low_yield);
  CHECK(exhaustive.stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::exhausted);
  CHECK(exhaustive.candidates.size() >= 4);
  CHECK(exhaustive.stats.sampled_tree_projection_moves_preassigned >
        exhaustive.candidates.size());

  // Put the finite boundary one unique candidate before exhaustion. The
  // filtered stream must consume its four-source probe and activate all eight
  // admitted source slots; W1 and W8 still publish the identical prefix.
  auto late_cap_options = low_yield;
  late_cap_options.max_candidates = exhaustive.candidates.size() - 1;
  late_cap_options.max_candidates_is_post_dedup = true;
  auto const late_w1 = run(1, late_cap_options);
  auto const late_w8 = run(8, late_cap_options);
  CHECK(late_w8.stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  check_same_candidates(late_w1, late_w8);
  CHECK(late_w8.stats.sampled_tree_source_admitted_wave_width == 8);
  CHECK(late_w8.stats.sampled_tree_source_adaptive_initial_wave_width == 4);
  CHECK(late_w8.stats.sampled_tree_source_adaptive_widenings == 1);
  CHECK(late_w8.stats.sampled_tree_source_peak_wave_size == 8);
  CHECK(late_w8.stats.sampled_tree_source_full_width_waves > 0);

  // Explicit maximum widths remain hard admission/runtime limits. They expose
  // the deterministic operation-count cost of keeping a late finite stream at
  // two or four sources, while preserving the same canonical result.
  auto source_two_options = late_cap_options;
  source_two_options.sampled_tree_source_maximum_wave_size = 2;
  auto source_four_options = late_cap_options;
  source_four_options.sampled_tree_source_maximum_wave_size = 4;
  auto const source_two = run(8, source_two_options);
  auto const source_four = run(8, source_four_options);
  check_same_candidates(late_w1, source_two);
  check_same_candidates(late_w1, source_four);
  CHECK(source_two.stats.sampled_tree_source_admitted_wave_width == 2);
  CHECK(source_two.stats.sampled_tree_source_peak_wave_size == 2);
  CHECK(source_two.stats.sampled_tree_source_adaptive_initial_wave_width == 0);
  CHECK(source_two.stats.sampled_tree_source_adaptive_widenings == 0);
  CHECK(source_four.stats.sampled_tree_source_admitted_wave_width == 4);
  CHECK(source_four.stats.sampled_tree_source_peak_wave_size == 4);
  CHECK(source_four.stats.sampled_tree_source_adaptive_initial_wave_width == 0);
  CHECK(source_four.stats.sampled_tree_source_adaptive_widenings == 0);
  CHECK(source_two.stats.sampled_tree_source_waves >
        source_four.stats.sampled_tree_source_waves);
  CHECK(source_four.stats.sampled_tree_source_waves >
        late_w8.stats.sampled_tree_source_waves);

  // Both post-dedup and pre-dedup cap-one streams retain only the four-source
  // initial wave and never activate the admitted speculative tail.
  auto cap_one_post_options = low_yield;
  cap_one_post_options.min_moved_clade_size = 1;
  cap_one_post_options.min_target_clade_size = 1;
  cap_one_post_options.max_candidates = 1;
  cap_one_post_options.max_candidates_is_post_dedup = true;
  auto const cap_one_post_w1 = run(1, cap_one_post_options);
  auto const cap_one_post_w8 = run(8, cap_one_post_options);
  check_same_candidates(cap_one_post_w1, cap_one_post_w8);
  CHECK(cap_one_post_w8.candidates.size() == 1);
  CHECK(cap_one_post_w8.stats.sampled_tree_source_peak_wave_size == 4);
  CHECK(cap_one_post_w8.stats.sampled_tree_source_adaptive_widenings == 0);

  auto cap_one_pre_options = low_yield;
  cap_one_pre_options.min_moved_clade_size = 1;
  cap_one_pre_options.min_target_clade_size = 1;
  cap_one_pre_options.max_candidates = 1;
  cap_one_pre_options.max_candidates_is_post_dedup = false;
  auto const cap_one_pre_w1 = run(1, cap_one_pre_options);
  auto const cap_one_pre_w8 = run(8, cap_one_pre_options);
  check_same_candidates(cap_one_pre_w1, cap_one_pre_w8);
  CHECK(cap_one_pre_w8.stats.candidates_constructed == 1);
  CHECK(cap_one_pre_w8.stats.sampled_tree_source_peak_wave_size == 4);
  CHECK(cap_one_pre_w8.stats.sampled_tree_source_adaptive_widenings == 0);

  // A finite cap above the entire two-sample stream remains "exhausted". The
  // first sampled tree performs the sole adaptive transition; the second tree
  // starts at full width instead of paying another narrowed first wave.
  auto multiple_options = low_yield;
  multiple_options.sampled_tree_count = 2;
  multiple_options.max_candidates = (std::numeric_limits<std::size_t>::max)();
  multiple_options.max_candidates_is_post_dedup = true;
  auto const multiple = run(8, multiple_options);
  CHECK(multiple.stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::exhausted);
  CHECK(multiple.stats.sampled_tree_projection_enumeration_passes == 2);
  CHECK(multiple.stats.sampled_tree_source_adaptive_initial_wave_width == 4);
  CHECK(multiple.stats.sampled_tree_source_adaptive_widenings == 1);
  CHECK(multiple.stats.sampled_tree_source_peak_wave_size == 8);
  CHECK(multiple.stats.sampled_tree_source_full_width_waves >= 2);
  CHECK(multiple.candidates.size() < multiple_options.max_candidates);

  // Hybrid clears the sampled child's semantic post-dedup cap because the cap
  // is global. It must nevertheless retain finite adaptive scheduling, exhaust
  // sampled work, then run the grammar remainder before the unreachable cap.
  auto hybrid_options = multiple_options;
  hybrid_options.source = larch::chart_spr_candidate_source::hybrid;
  hybrid_options.sampled_tree_count = 1;
  auto const hybrid = run(8, hybrid_options);
  CHECK(hybrid.stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::exhausted);
  CHECK(hybrid.stats.sampled_tree_source_adaptive_initial_wave_width == 4);
  CHECK(hybrid.stats.sampled_tree_source_adaptive_widenings == 1);
  CHECK(hybrid.stats.sampled_tree_source_peak_wave_size == 8);
  CHECK(hybrid.stats.sampled_tree_source_speculative_moves_discarded == 0);
  CHECK(hybrid.stats.grammar_candidate_construction_waves > 0);
  CHECK(hybrid.stats.grammar_candidate_scheduler_operations > 0);
  CHECK(hybrid.stats.grammar_candidate_parallel_operations > 0);

  std::println("  PASS");
}

static void test_phase8_named_source_wave_stops_near_candidate_cap() {
  std::println("test_phase8_named_source_wave_stops_near_candidate_cap");

  auto dag = larch::load_proto_dag("data/test_5_trees/tree_0.pb.gz");
  larch::polytomy_refinement_options refinement_options;
  refinement_options.mode = larch::polytomy_mode::expand_soft_bounded;
  refinement_options.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, refinement_options);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "phase8 named sampled-generation-high fixture");
  auto grammar = std::move(refinement.grammar);

  struct run_result {
    std::vector<larch::grammar_spr_candidate> candidates;
    larch::chart_spr_candidate_generation_stats stats;
  };
  auto run = [&](std::size_t workers, std::size_t diagnostic_wave_size = 0,
                 bool include_root_moves = false) {
    auto scheduler = make_projection_scheduler(workers);
    std::latch adaptive_first_wave_started{4};
    larch::grammar_spr_enumeration_options options;
    options.source = larch::chart_spr_candidate_source::sampled_tree;
    options.sampled_tree_source_dag = &dag;
    options.sampled_tree_count = 1;
    options.sampled_tree_spr_radius = 0;
    options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
    // The wave-stopping constants below were calibrated with root-clade
    // moves excluded from the sampled-tree projection post-filters; keep
    // that posture (the default argument) so the historical move counts
    // stay comparable.  The root-move-included constants are pinned
    // separately after the historical block below.
    options.include_root_moves = include_root_moves;
    options.max_candidates = 256;
    options.max_candidates_is_post_dedup = true;
    options.seed = 1;
    options.sampled_tree_projection_scheduler = &scheduler;
    options.sampled_tree_source_finite_wave_size_for_diagnostics =
        diagnostic_wave_size;
    if (workers == 8 && diagnostic_wave_size == 0 && !include_root_moves) {
      options.before_sampled_tree_source_enumeration_for_tests =
          [&](std::size_t source_ordinal, std::size_t) {
            if (source_ordinal >= 4) return;
            adaptive_first_wave_started.count_down();
            adaptive_first_wave_started.wait();
          };
    }

    run_result result;
    result.candidates = collect_candidates(grammar, options, &result.stats);
    CHECK(result.candidates.size() == 256);
    CHECK(result.stats.stop_reason ==
          larch::chart_spr_candidate_stop_reason::candidate_cap);
    CHECK(result.stats.sampled_tree_projection_enumeration_passes == 1);
    CHECK(result.stats.sampled_tree_projection_move_enumeration_visits ==
          result.stats.sampled_tree_projection_moves_preassigned);
    CHECK(result.stats.sampled_tree_source_one_pass_move_visits ==
          result.stats.sampled_tree_projection_moves_preassigned);
    CHECK(result.stats.sampled_tree_sources_enumerated < dag.node_high_mark());
    auto const projection_jobs_per_worker =
        workers == 1 ? std::size_t{4} : std::size_t{64};
    auto const maximum_projection_wave =
        workers * projection_jobs_per_worker;
    CHECK(result.stats.sampled_tree_projection_admitted_subwave_width <=
          maximum_projection_wave);
    CHECK(result.stats.sampled_tree_projection_admitted_subwave_width >=
          result.stats.sampled_tree_projection_peak_wave_size);
    CHECK(result.stats.sampled_tree_projection_peak_wave_size <=
          result.stats.sampled_tree_projection_moves_preassigned);
    CHECK(result.stats.sampled_tree_source_speculative_moves_discarded > 0);
    auto const metrics = scheduler.metrics();
    CHECK(metrics.pending_tasks == 0);
    CHECK(metrics.tasks_submitted == metrics.tasks_completed);
    CHECK(metrics.tasks_submitted == metrics.tasks_joined);
    return result;
  };

  auto const w1 = run(1);
  auto const adaptive_w8 = run(8);
  auto const fixed_w2 = run(8, 2);
  auto const fixed_w4 = run(8, 4);
  auto const fixed_w8 = run(8, 8);

  // Root-move-included posture (the production default since root-clade
  // moves were enabled).  Numbers measured on ae551d1 by running this
  // binary with a temporary dump of the same stats fields (observed:
  // W1 sources=3 preassigned=270 subwave=4 peak=4 admitted=1 peak_src=1
  // full_waves=3 speculative_discarded=11; adaptive W8 sources=4
  // preassigned=405 subwave=512 peak=405 admitted=8 peak_src=4
  // adaptive_initial=4 widenings=0 full_waves=0 ranges=4 hwm=4
  // speculative_discarded=146 speculative_sources_discarded=1
  // projection waves=1 scheduler_ops=2 parallel_ops=2 ranges=32+405
  // worker_tasks=8+8).  Unlike the root-move-excluded posture, the serial
  // and 8-worker source waves stop after different source prefixes (root
  // moves add candidates per source), so the legacy stats-equality oracle
  // does not apply; the candidate payloads themselves remain identical.
  {
    auto const w1_root = run(1, 0, true);
    auto const adaptive_w8_root = run(8, 0, true);
    CHECK(w1_root.stats.sampled_tree_sources_enumerated == 3);
    CHECK(w1_root.stats.sampled_tree_projection_moves_preassigned == 270);
    CHECK(w1_root.stats.sampled_tree_projection_admitted_subwave_width == 4);
    CHECK(w1_root.stats.sampled_tree_projection_peak_wave_size == 4);
    CHECK(w1_root.stats.sampled_tree_source_admitted_wave_width == 1);
    CHECK(w1_root.stats.sampled_tree_source_peak_wave_size == 1);
    CHECK(w1_root.stats.sampled_tree_source_full_width_waves == 3);
    CHECK(w1_root.stats.sampled_tree_source_speculative_moves_discarded ==
          11);
    CHECK(adaptive_w8_root.stats.sampled_tree_sources_enumerated == 4);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_moves_preassigned ==
          405);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_admitted_subwave_width ==
          8 * 64);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_peak_wave_size ==
          405);
    CHECK(adaptive_w8_root.stats.sampled_tree_source_admitted_wave_width ==
          8);
    CHECK(adaptive_w8_root.stats.sampled_tree_source_peak_wave_size == 4);
    CHECK(adaptive_w8_root.stats.sampled_tree_source_adaptive_initial_wave_width ==
          4);
    CHECK(adaptive_w8_root.stats.sampled_tree_source_adaptive_widenings == 0);
    CHECK(adaptive_w8_root.stats.sampled_tree_source_full_width_waves == 0);
    CHECK(adaptive_w8_root.stats.sampled_tree_source_enumeration_ranges >= 4);
    // High-water concurrency is a scheduling property, not a semantics
    // property: on a loaded machine fewer pool workers may be simultaneously
    // resident in the adaptive wave, so only require actual engagement
    // (structural parallelism is proven by enumeration_ranges >= 4 above).
    CHECK(adaptive_w8_root.stats
              .sampled_tree_source_enumeration_active_worker_high_water >= 1);
    CHECK(adaptive_w8_root.stats
              .sampled_tree_source_speculative_moves_discarded == 146);
    CHECK(adaptive_w8_root.stats
              .sampled_tree_source_speculative_sources_discarded == 1);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_waves == 1);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_scheduler_operations ==
          2);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_parallel_operations ==
          2);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_ranges ==
          32 + 405);
    CHECK(adaptive_w8_root.stats.sampled_tree_projection_worker_tasks ==
          8 + 8);
    CHECK(adaptive_w8_root.candidates.size() == w1_root.candidates.size());
    for (std::size_t i = 0; i < w1_root.candidates.size(); ++i) {
      check_candidate_payload_equal(grammar, w1_root.candidates[i],
                                    adaptive_w8_root.candidates[i]);
    }
  }
  auto check_same_canonical_result = [&](run_result const& actual) {
    CHECK(actual.candidates.size() == w1.candidates.size());
    check_legacy_generation_stats_equal(actual.stats, w1.stats);
    for (std::size_t i = 0; i < actual.candidates.size(); ++i) {
      check_candidate_payload_equal(grammar, w1.candidates[i],
                                    actual.candidates[i]);
    }
  };
  check_same_canonical_result(adaptive_w8);
  check_same_canonical_result(fixed_w2);
  check_same_canonical_result(fixed_w4);
  check_same_canonical_result(fixed_w8);

  CHECK(w1.stats.sampled_tree_source_admitted_wave_width == 1);
  CHECK(w1.stats.sampled_tree_source_peak_wave_size == 1);
  CHECK(w1.stats.sampled_tree_projection_admitted_subwave_width == 4);
  CHECK(w1.stats.sampled_tree_projection_peak_wave_size == 4);
  for (auto const* result :
       {&adaptive_w8, &fixed_w2, &fixed_w4, &fixed_w8}) {
    CHECK(result->stats.sampled_tree_source_admitted_wave_width == 8);
    CHECK(result->stats.sampled_tree_projection_admitted_subwave_width ==
          8 * 64);
    CHECK(result->stats.sampled_tree_projection_parallel_operations > 0);
    CHECK(result->stats.sampled_tree_source_enumeration_parallel_operations >
          0);
  }

  // Default W8 uses four sources immediately. It therefore has useful source
  // parallelism on this four-source stopping prefix without admitting only
  // half the worker width or launching the old eight-source speculative tail.
  CHECK(adaptive_w8.stats.sampled_tree_source_peak_wave_size == 4);
  CHECK(adaptive_w8.stats.sampled_tree_source_adaptive_initial_wave_width == 4);
  CHECK(adaptive_w8.stats.sampled_tree_source_adaptive_widenings == 0);
  CHECK(adaptive_w8.stats.sampled_tree_source_full_width_waves == 0);
  CHECK(adaptive_w8.stats.sampled_tree_source_enumeration_ranges >= 4);
  CHECK(adaptive_w8.stats
            .sampled_tree_source_enumeration_active_worker_high_water >= 4);

  CHECK(fixed_w2.stats.sampled_tree_source_peak_wave_size == 2);
  CHECK(fixed_w4.stats.sampled_tree_source_peak_wave_size == 4);
  CHECK(fixed_w8.stats.sampled_tree_source_peak_wave_size == 8);
  CHECK(fixed_w2.stats.sampled_tree_source_adaptive_initial_wave_width == 0);
  CHECK(fixed_w4.stats.sampled_tree_source_adaptive_initial_wave_width == 0);
  CHECK(fixed_w8.stats.sampled_tree_source_adaptive_initial_wave_width == 0);
  CHECK(fixed_w2.stats.sampled_tree_source_adaptive_widenings == 0);
  CHECK(fixed_w4.stats.sampled_tree_source_adaptive_widenings == 0);
  CHECK(fixed_w8.stats.sampled_tree_source_adaptive_widenings == 0);

  // The pre-source-wave implementation at 51702c7 globally preassigned
  // 16,454 moves and visited them twice. The original W8 source wave then
  // admitted eight sources, visiting 812 moves and discarding 416 source
  // moves (four complete sources). The deterministic width comparison proves
  // that default adaptive work equals width 4 (and width 2), while restoring a
  // four-source active W8 wave; fixed width 8 reproduces the excessive tail.
  CHECK(w1.stats.sampled_tree_sources_enumerated == 4);
  CHECK(adaptive_w8.stats.sampled_tree_sources_enumerated == 4);
  CHECK(fixed_w2.stats.sampled_tree_sources_enumerated == 4);
  CHECK(fixed_w4.stats.sampled_tree_sources_enumerated == 4);
  CHECK(fixed_w8.stats.sampled_tree_sources_enumerated == 8);
  CHECK(w1.stats.sampled_tree_projection_moves_preassigned == 405);
  CHECK(adaptive_w8.stats.sampled_tree_projection_moves_preassigned == 405);
  CHECK(fixed_w2.stats.sampled_tree_projection_moves_preassigned == 405);
  CHECK(fixed_w4.stats.sampled_tree_projection_moves_preassigned == 405);
  CHECK(fixed_w8.stats.sampled_tree_projection_moves_preassigned == 812);
  CHECK(adaptive_w8.stats.sampled_tree_projection_peak_wave_size == 405);
  // The one 405-job adaptive subwave has two joined scheduler operations. The
  // relatively uniform projection work uses four ranges per worker (32
  // ranges); signature/filter postprocessing retains unit-grain dynamic
  // claiming because its candidate costs vary materially (405 ranges).
  CHECK(adaptive_w8.stats.sampled_tree_projection_waves == 1);
  CHECK(adaptive_w8.stats.sampled_tree_projection_scheduler_operations == 2);
  CHECK(adaptive_w8.stats.sampled_tree_projection_parallel_operations == 2);
  CHECK(adaptive_w8.stats.sampled_tree_projection_ranges == 32 + 405);
  CHECK(adaptive_w8.stats.sampled_tree_projection_worker_tasks == 8 + 8);
  CHECK(fixed_w4.stats.sampled_tree_projection_peak_wave_size == 405);
  CHECK(fixed_w8.stats.sampled_tree_projection_peak_wave_size == 512);
  CHECK(adaptive_w8.stats.sampled_tree_source_speculative_moves_discarded == 9);
  CHECK(adaptive_w8.stats.sampled_tree_source_speculative_sources_discarded ==
        0);
  CHECK(fixed_w8.stats.sampled_tree_source_speculative_sources_discarded == 4);

  std::println("  PASS");
}

static void test_phase6_randomized_and_reservoir_enumeration() {
  std::println("test_phase6_randomized_and_reservoir_enumeration");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::grammar_spr_enumeration_options randomized;
  randomized.randomize_order = true;
  randomized.seed = 7;
  randomized.max_candidates = 3;
  auto first = collect_candidate_signature_vector(grammar, randomized);
  auto second = collect_candidate_signature_vector(grammar, randomized);
  CHECK(first == second);
  CHECK(first.size() == 3);

  larch::grammar_spr_enumeration_options reservoir;
  reservoir.reservoir_sample = true;
  reservoir.seed = 11;
  reservoir.max_candidates = 2;
  larch::chart_spr_candidate_generation_stats stats;
  auto sampled = collect_candidate_signature_vector(grammar, reservoir, &stats);
  CHECK(sampled.size() == 2);
  CHECK(stats.stop_reason == larch::chart_spr_candidate_stop_reason::exhausted);
  CHECK(stats.candidates_generated_after_dedup >= sampled.size());
  CHECK(stats.candidates_constructed >= sampled.size());

  std::println("  PASS");
}

static void test_phase6_stable_taxon_dedup_across_equivalent_builds() {
  std::println("test_phase6_stable_taxon_dedup_across_equivalent_builds");

  auto dag1 = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto dag2 = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree_root_swapped());
  auto grammar1 = larch::build_clade_grammar(dag1);
  auto grammar2 = larch::build_clade_grammar(dag2);

  CHECK(collect_candidate_signature_set(grammar1, {}) ==
        collect_candidate_signature_set(grammar2, {}));

  std::println("  PASS");
}

static void test_phase6_sampled_tree_and_hybrid_sources() {
  std::println("test_phase6_sampled_tree_and_hybrid_sources");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree()));
  trees.push_back(larch::test::make_tiny_labelled_tree("A", four_taxon_cross_tree()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);

  larch::grammar_spr_enumeration_options sampled_opts;
  sampled_opts.source = larch::chart_spr_candidate_source::sampled_tree;
  sampled_opts.sampled_tree_source_dag = &dag;
  sampled_opts.sampled_tree_count = 2;
  sampled_opts.max_candidates = 16;
  // The native-move score check below validates the tree-SPR scorer on a
  // fixed first candidate; keep the historical root-clade exclusion so the
  // first projected candidate is an ordinary in-tree move.
  sampled_opts.include_root_moves = false;
  auto sampled = collect_candidates(grammar, sampled_opts);
  CHECK(!sampled.empty());
  for (auto const& candidate : sampled) {
    CHECK(candidate.source_tree_move.has_value());
    CHECK(candidate.source_tree_move->score_change.has_value());
  }

  auto single_sample_opts = sampled_opts;
  single_sample_opts.sampled_tree_count = 1;
  single_sample_opts.max_candidates = 1;
  auto single_sampled = collect_candidates(grammar, single_sample_opts);
  CHECK(single_sampled.size() == 1);
  CHECK(single_sampled.front().source_tree_move.has_value());
  auto const& sampled_move = *single_sampled.front().source_tree_move;
  CHECK(sampled_move.score_change.has_value());

  std::mt19937 sampled_tree_rng(single_sample_opts.seed);
  auto representative_tree =
      larch::chart_spr_detail::build_sampled_tree_from_grammar(
          grammar, single_sample_opts, 0, sampled_tree_rng);
  larch::tree_index before_index{representative_tree};
  auto after_tree = larch::apply_spr_move(representative_tree,
                                          sampled_move.src,
                                          sampled_move.dst);
  larch::tree_index after_index{after_tree};
  CHECK(*sampled_move.score_change ==
        after_index.compute_parsimony_score() -
            before_index.compute_parsimony_score());

  larch::grammar_spr_enumeration_options grammar_opts;
  grammar_opts.source = larch::chart_spr_candidate_source::grammar;
  auto direct_signatures = collect_candidate_signature_set(grammar, grammar_opts);
  std::set<std::string> sampled_signatures;
  for (auto const& candidate : sampled) {
    sampled_signatures.insert(
        larch::chart_spr_candidate_taxon_signature(grammar, candidate));
  }

  larch::grammar_spr_enumeration_options hybrid_opts;
  hybrid_opts.source = larch::chart_spr_candidate_source::hybrid;
  hybrid_opts.sampled_tree_source_dag = &dag;
  hybrid_opts.sampled_tree_count = 2;
  hybrid_opts.max_candidates = 64;
  auto hybrid = collect_candidates(grammar, hybrid_opts);
  CHECK(!hybrid.empty());

  std::set<std::string> hybrid_signatures;
  bool has_projected = false;
  bool has_direct_extra = false;
  for (auto const& candidate : hybrid) {
    auto signature = larch::chart_spr_candidate_taxon_signature(grammar, candidate);
    CHECK(hybrid_signatures.insert(signature).second);
    has_projected = has_projected || candidate.source_tree_move.has_value();
    if (!candidate.source_tree_move && !sampled_signatures.contains(signature)) {
      has_direct_extra = true;
    }
  }
  CHECK(has_projected);
  bool direct_has_extra = false;
  for (auto const& signature : direct_signatures) {
    if (!sampled_signatures.contains(signature)) {
      direct_has_extra = true;
      CHECK(hybrid_signatures.contains(signature));
      break;
    }
  }
  CHECK(!direct_has_extra || has_direct_extra);

  auto hybrid_pre_cap_opts = hybrid_opts;
  hybrid_pre_cap_opts.max_candidates = 1;
  hybrid_pre_cap_opts.max_candidates_is_post_dedup = false;
  larch::chart_spr_candidate_generation_stats pre_cap_stats;
  (void)collect_candidates(grammar, hybrid_pre_cap_opts, &pre_cap_stats);
  CHECK(pre_cap_stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(pre_cap_stats.candidates_constructed <= 1);

  // Root-move-included posture (the production default).  Numbers measured
  // on ae551d1 by running this binary with a temporary print of the same
  // fields (observed: size=16; first projected move src=2 dst=6 with
  // score_change=1 while apply+rescore gives 1->1, i.e. delta 0).  The
  // divergence is expected: the native sampled-tree estimate treats the
  // root-sibling attachment edge as a real reattachment, whereas applying
  // that move to the representative tree is score-neutral here; estimates
  // never gate acceptance (exact verification does), which is why the
  // parity identity above is asserted only for the root-move-excluded
  // first candidate.
  {
    auto root_opts = sampled_opts;
    root_opts.include_root_moves = true;
    auto sampled_root = collect_candidates(grammar, root_opts);
    CHECK(sampled_root.size() == 16);
    CHECK(!sampled_root.empty());
    for (auto const& candidate : sampled_root) {
      CHECK(candidate.source_tree_move.has_value());
      CHECK(candidate.source_tree_move->score_change.has_value());
    }

    auto single_root_opts = root_opts;
    single_root_opts.sampled_tree_count = 1;
    single_root_opts.max_candidates = 1;
    auto single_root = collect_candidates(grammar, single_root_opts);
    CHECK(single_root.size() == 1);
    auto const& root_move = *single_root.front().source_tree_move;
    CHECK(root_move.src == 2);
    CHECK(root_move.dst == 6);
    CHECK(*root_move.score_change == 1);
    std::mt19937 root_tree_rng(single_root_opts.seed);
    auto root_representative_tree =
        larch::chart_spr_detail::build_sampled_tree_from_grammar(
            grammar, single_root_opts, 0, root_tree_rng);
    larch::tree_index root_before_index{root_representative_tree};
    auto root_after_tree = larch::apply_spr_move(root_representative_tree,
                                                 root_move.src,
                                                 root_move.dst);
    larch::tree_index root_after_index{root_after_tree};
    CHECK(root_before_index.compute_parsimony_score() == 1);
    CHECK(root_after_index.compute_parsimony_score() == 1);
  }

  std::println("  PASS");
}

static void test_phase6_immediate_reversal_filter_reports_prune() {
  std::println("test_phase6_immediate_reversal_filter_reports_prune");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto candidates = collect_candidates(grammar, {});
  CHECK(!candidates.empty());
  auto blocked_key = larch::chart_spr_candidate_reversal_key(
      grammar, candidates.front());
  auto blocked_signature = larch::chart_spr_candidate_taxon_signature(
      grammar, candidates.front());

  larch::grammar_spr_enumeration_options options;
  options.immediate_reversal_candidate_key_to_skip = blocked_key;
  larch::chart_spr_candidate_generation_stats stats;
  auto filtered = collect_candidate_signature_vector(grammar, options, &stats);
  CHECK(std::find(filtered.begin(), filtered.end(), blocked_signature) ==
        filtered.end());
  CHECK(stats.candidates_pruned_immediate_reversal > 0);

  options.include_immediate_reversal_candidates = true;
  auto unfiltered = collect_candidate_signature_vector(grammar, options);
  CHECK(std::find(unfiltered.begin(), unfiltered.end(), blocked_signature) !=
        unfiltered.end());

  std::println("  PASS");
}

static void test_multisite_exact_vs_lower_bound_labels() {
  std::println("test_multisite_exact_vs_lower_bound_labels");

  auto dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto candidate = cross_candidate(grammar);

  auto lower = larch::score_multisite_spr_candidate_lower_bound(grammar, patterns, candidate);
  CHECK(lower.old_score == 1);
  CHECK(lower.new_score == 2);
  CHECK(!lower.exact_multisite);

  auto exact = larch::score_multisite_spr_candidate_exact(grammar, patterns, candidate);
  CHECK(exact.old_score == 1);
  CHECK(exact.new_score == 2);
  CHECK(exact.exact_multisite);

  std::println("  PASS");
}

int main() {
  test_overlay_temp_clades_and_single_site_score();
  test_phase8_binary_taxon_dedup_key_matches_legacy();
  test_phase8_grammar_binary_dedup_mode_selection();
  test_local_recompute_matches_full_rebuild();
  test_grammar_native_candidate_enumeration();
  test_projected_tree_spr_matches_apply_spr_move();
  test_bootstrap_projection_from_tree_validates_against_apply();
  test_phase8_prepared_projection_differential_all_emitted_moves();
  test_phase8_binary_direct_metadata_resolver_is_fail_closed();
  test_phase8_parallel_sampled_projection_is_deterministic();
  test_phase8_parallel_projection_cached_after_refs_preserves_fallback();
  test_phase8_parallel_postprocessing_gather_matches_w1();
  test_phase8_grammar_and_hybrid_worker_seed_matrix();
  test_phase8_parallel_grammar_stop_reservoir_and_failure();
  test_phase8_production_small_grammar_waves_run_inline();
  test_phase8_sampled_hybrid_reservoir_worker_seed_matrix();
  test_phase8_projection_budget_and_failure_atomicity();
  test_phase8_midwave_stop_preserves_legacy_counters();
  test_phase8_source_wave_admission_failure_and_cancellation();
  test_phase8_speculative_failures_follow_canonical_order();
  test_phase8_adaptive_finite_source_wave_policy();
  test_phase8_named_source_wave_stops_near_candidate_cap();
  test_phase6_randomized_and_reservoir_enumeration();
  test_phase6_stable_taxon_dedup_across_equivalent_builds();
  test_phase6_sampled_tree_and_hybrid_sources();
  test_phase6_immediate_reversal_filter_reports_prune();
  test_multisite_exact_vs_lower_bound_labels();
  std::println("chart_spr_test PASS");
  return 0;
}

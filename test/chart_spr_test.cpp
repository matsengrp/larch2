#include <larch/chart_spr.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/polytomy_refinement.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <latch>
#include <limits>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
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

static void check_candidate_payload_equal(
    larch::clade_grammar const& base, larch::grammar_spr_candidate const& lhs,
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
      options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
      options.randomize_order = true;
      options.seed = seed;
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
        options.before_sampled_tree_projection_for_tests = [&](std::size_t) {
          auto const call = hook_calls.fetch_add(1, std::memory_order_relaxed);
          if (call >= 2) return;
          first_wave_started.count_down();
          first_wave_started.wait();
        };
      }

      larch::chart_spr_candidate_generation_stats stats;
      auto candidates = collect_candidates(grammar, options, &stats);
      CHECK(!candidates.empty());
      CHECK(stats.sampled_tree_projection_moves_preassigned >= workers);
      CHECK(stats.sampled_tree_projection_move_enumeration_visits ==
            stats.sampled_tree_projection_moves_preassigned);
      CHECK(stats.sampled_tree_source_one_pass_move_visits ==
            stats.sampled_tree_projection_moves_preassigned);
      CHECK(stats.sampled_tree_projection_enumeration_passes == 1);
      CHECK(stats.sampled_tree_source_waves > 0);
      CHECK(stats.sampled_tree_sources_enumerated > 0);
      auto const expected_source_width =
          std::min(workers, stats.sampled_tree_sources_enumerated);
      CHECK(stats.sampled_tree_source_admitted_wave_width ==
            expected_source_width);
      CHECK(stats.sampled_tree_source_peak_wave_size == expected_source_width);
      CHECK(stats.sampled_tree_projection_admitted_subwave_width <=
            workers * 4);
      CHECK(stats.sampled_tree_projection_admitted_subwave_width >=
            stats.sampled_tree_projection_peak_wave_size);
      CHECK(stats.sampled_tree_projection_scheduler_operations > 0);
      CHECK(stats.sampled_tree_projection_peak_wave_size ==
            std::min(stats.sampled_tree_projection_moves_preassigned,
                     workers * 4));
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
        CHECK(stats.sampled_tree_source_enumeration_parallel_operations == 0);
        baseline = candidates;
        baseline_stats = stats;
      } else {
        CHECK(source_hook_calls.load(std::memory_order_relaxed) >= 2);
        CHECK(stats.sampled_tree_source_enumeration_parallel_operations > 0);
        CHECK(stats.sampled_tree_source_enumeration_ranges >= 2);
        CHECK(stats.sampled_tree_source_enumeration_worker_tasks >= 2);
        CHECK(stats.sampled_tree_source_enumeration_active_worker_high_water >=
              2);
        CHECK(stats.sampled_tree_projection_parallel_operations > 0);
        CHECK(stats.sampled_tree_projection_ranges >= 2);
        CHECK(stats.sampled_tree_projection_worker_tasks >= 2);
        CHECK(stats.sampled_tree_projection_active_worker_high_water >= 2);
        CHECK(candidates.size() == baseline.size());
        check_legacy_generation_stats_equal(stats, baseline_stats);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
          check_candidate_payload_equal(grammar, baseline[i], candidates[i]);
        }
      }
      auto metrics = scheduler.metrics();
      CHECK(metrics.pending_tasks == 0);
      CHECK(metrics.tasks_submitted == metrics.tasks_completed);
      CHECK(metrics.tasks_submitted == metrics.tasks_joined);
      CHECK(snapshot_projection_source(dag) == source_before);
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

        larch::chart_spr_candidate_generation_stats stats;
        auto candidates = collect_candidates(grammar, options, &stats);
        CHECK(!candidates.empty());
        if (source == larch::chart_spr_candidate_source::grammar) {
          CHECK(stats.sampled_tree_projection_moves_preassigned == 0);
          CHECK(stats.sampled_tree_projection_scheduler_operations == 0);
        } else {
          CHECK(stats.sampled_tree_projection_moves_preassigned > 0);
          CHECK(stats.sampled_tree_projection_scheduler_operations > 0);
          CHECK(stats.sampled_tree_projection_move_enumeration_visits ==
                stats.sampled_tree_projection_moves_preassigned);
          CHECK(stats.sampled_tree_projection_enumeration_passes == 1);
          CHECK(stats.sampled_tree_source_one_pass_move_visits ==
                stats.sampled_tree_projection_moves_preassigned);
          CHECK(stats.sampled_tree_source_waves > 0);
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
  auto submit_after = submit_scheduler.metrics();
  CHECK(submit_after.tasks_submitted - submit_before.tasks_submitted == 1);
  CHECK(submit_after.tasks_completed - submit_before.tasks_completed == 1);
  CHECK(submit_after.tasks_joined - submit_before.tasks_joined == 1);
  CHECK(submit_after.pending_tasks == 0);
  submit_options.force_sampled_tree_projection_submit_failure_after_for_tests
      .reset();
  auto submit_recovered =
      collect_projection(submit_preassignment, submit_options);
  check_projection_vectors(baseline, submit_recovered);

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
      {.minimum_grain = 1, .target_ranges_per_worker = 1});
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
  auto scheduler = make_projection_scheduler(1);
  larch::grammar_spr_enumeration_options options;
  options.source = larch::chart_spr_candidate_source::sampled_tree;
  options.sampled_tree_source_dag = &dag;
  options.sampled_tree_count = 1;
  options.sampled_tree_spr_radius = 0;
  options.sampled_tree_score_threshold = std::numeric_limits<int>::max();
  options.max_candidates = 256;
  options.max_candidates_is_post_dedup = true;
  options.seed = 1;
  options.sampled_tree_projection_scheduler = &scheduler;

  larch::chart_spr_candidate_generation_stats stats;
  auto candidates = collect_candidates(grammar, options, &stats);
  CHECK(candidates.size() == 256);
  CHECK(stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(stats.sampled_tree_projection_enumeration_passes == 1);
  CHECK(stats.sampled_tree_projection_move_enumeration_visits ==
        stats.sampled_tree_projection_moves_preassigned);
  CHECK(stats.sampled_tree_source_one_pass_move_visits ==
        stats.sampled_tree_projection_moves_preassigned);
  // The recorded unsealed diagnostic at 51702c7 globally preassigned 16,454
  // moves and visited them twice. W1 now finishes the current source but never
  // starts the next source after the cap, keeping work close to the 396
  // projections historically consumed to emit these 256 candidates.
  CHECK(stats.sampled_tree_projection_moves_preassigned < 4096);
  CHECK(stats.sampled_tree_sources_enumerated < dag.node_high_mark());
  CHECK(stats.sampled_tree_source_waves ==
        stats.sampled_tree_sources_enumerated);
  CHECK(stats.sampled_tree_source_admitted_wave_width == 1);
  CHECK(stats.sampled_tree_source_peak_wave_size == 1);
  CHECK(stats.sampled_tree_projection_admitted_subwave_width == 4);
  CHECK(stats.sampled_tree_projection_peak_wave_size == 4);
  CHECK(stats.sampled_tree_source_speculative_moves_discarded > 0);
  CHECK(scheduler.metrics().pending_tasks == 0);

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
  test_local_recompute_matches_full_rebuild();
  test_grammar_native_candidate_enumeration();
  test_projected_tree_spr_matches_apply_spr_move();
  test_bootstrap_projection_from_tree_validates_against_apply();
  test_phase8_prepared_projection_differential_all_emitted_moves();
  test_phase8_binary_direct_metadata_resolver_is_fail_closed();
  test_phase8_parallel_sampled_projection_is_deterministic();
  test_phase8_grammar_and_hybrid_worker_seed_matrix();
  test_phase8_projection_budget_and_failure_atomicity();
  test_phase8_midwave_stop_preserves_legacy_counters();
  test_phase8_source_wave_admission_failure_and_cancellation();
  test_phase8_named_source_wave_stops_near_candidate_cap();
  test_phase6_randomized_and_reservoir_enumeration();
  test_phase6_stable_taxon_dedup_across_equivalent_builds();
  test_phase6_sampled_tree_and_hybrid_sources();
  test_phase6_immediate_reversal_filter_reports_prune();
  test_multisite_exact_vs_lower_bound_labels();
  std::println("chart_spr_test PASS");
  return 0;
}

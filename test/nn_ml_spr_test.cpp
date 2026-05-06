// ML-based DAG scoring via subtree_weight with likelihood_score_ops.
// Builds a tree with 20-base sequences, scores it with the S5F model,
// and verifies subtree_weight returns the same total as manual edge scoring.
#include <larch/likelihood_score_ops.hpp>
#include <larch/model_variant.hpp>
#include <larch/rs_fivemer_model.hpp>
#include <larch/subtree_weight.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace larch;

// --- helpers (mirrors native_optimize_test patterns) ---

static compact_genome cg_from_sequence(std::string_view seq,
                                       std::string_view ref) {
  std::map<mutation_position, nuc_base> muts;
  for (std::size_t i = 0; i < seq.size(); ++i) {
    if (seq[i] != ref[i]) muts[i + 1] = nuc_base::from_char(seq[i]);
  }
  return compact_genome{std::move(muts)};
}

static std::size_t add_edge(phylo_dag& d, std::size_t parent_idx,
                            std::size_t child_idx,
                            std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
  return edge.index();
}

static void set_node_sequence(phylo_dag& d, std::size_t node_idx,
                              std::string_view seq) {
  auto const& ref = get_reference_sequence(d);
  auto nv = d.get_node(node_idx);
  std::visit(
      [&](auto node) {
        if constexpr (requires { node.cg(); }) {
          node.cg() = cg_from_sequence(seq, ref);
        }
      },
      nv);
}

template <typename Ops>
static typename Ops::weight_type manual_edge_sum(phylo_dag& d,
                                                 Ops const& ops) {
  typename Ops::weight_type total{0};
  for (auto ev : d.get_all_edges()) {
    std::visit([&](auto edge) { total += ops.compute_edge(d, edge.index()); },
               ev);
  }
  return total;
}

static std::string bcr_data_dir() {
  return larch::test::source_path_string("data/bcr");
}

static ml_model const& thrifty_model() {
  static ml_model model = load_ml_model(bcr_data_dir(), "ThriftyHumV0.2-45");
  return model;
}

struct two_alt_nll_model {
  double alt1_nll = 1.0;
  double alt2_nll = 1.0;

  double log_likelihood(std::string_view /*parent*/,
                        std::string_view child) const {
    if (child == "CAAA") return -alt1_nll;
    if (child == "ACAA") return -alt2_nll;
    throw std::runtime_error{"two_alt_nll_model: unexpected child sequence"};
  }
};

static phylo_dag make_two_alternative_ml_tie_dag() {
  constexpr std::string_view ref = "AAAA";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("AAAA", ref);

  auto alt1 = d.append_node<node_kind::leaf>();
  alt1.cg() = cg_from_sequence("CAAA", ref);
  alt1.sample_id() = "L";

  auto alt2 = d.append_node<node_kind::leaf>();
  alt2.cg() = cg_from_sequence("ACAA", ref);
  alt2.sample_id() = "L";

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), alt1.index(), 0);
  add_edge(d, root.index(), alt2.index(), 0);

  recompute_edge_mutations(d);
  return d;
}

// Build a 4-leaf tree with 20-base sequences.
//
//       UA
//       |
//      root  (= ref)
//      / \
//    i1    i2
//   / \   / \
//  L1  L2 L3  L4
//
// Mutations:
//   root→i1:  pos 0 A→T   (1 mutation)
//   root→i2:  pos 11 T→C  (1 mutation)
//   i1→L1:    none
//   i1→L2:    pos 3 T→C   (1 mutation)
//   i2→L3:    none
//   i2→L4:    pos 15 T→C  (1 mutation)
static phylo_dag make_test_tree() {
  constexpr std::string_view ref = "ACGTACGTACGTACGTACGT";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence("ACGTACGTACGTACGTACGT", ref);

  auto i1 = d.append_node<node_kind::inner>();
  i1.cg() = cg_from_sequence("TCGTACGTACGTACGTACGT", ref);

  auto i2 = d.append_node<node_kind::inner>();
  i2.cg() = cg_from_sequence("ACGTACGTACGCACGTACGT", ref);

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from_sequence("TCGTACGTACGTACGTACGT", ref);
  l1.sample_id() = "L1";

  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from_sequence("TCGCACGTACGTACGTACGT", ref);
  l2.sample_id() = "L2";

  auto l3 = d.append_node<node_kind::leaf>();
  l3.cg() = cg_from_sequence("ACGTACGTACGCACGTACGT", ref);
  l3.sample_id() = "L3";

  auto l4 = d.append_node<node_kind::leaf>();
  l4.cg() = cg_from_sequence("ACGTACGTACGCACGCACGT", ref);
  l4.sample_id() = "L4";

  add_edge(d, ua.index(), root.index(), 0);
  add_edge(d, root.index(), i1.index(), 0);
  add_edge(d, root.index(), i2.index(), 1);
  add_edge(d, i1.index(), l1.index(), 0);
  add_edge(d, i1.index(), l2.index(), 1);
  add_edge(d, i2.index(), l3.index(), 0);
  add_edge(d, i2.index(), l4.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

struct alternative_dag_fixture {
  phylo_dag dag;
  std::size_t root_alt1_edge;
  std::size_t root_alt2_edge;
  std::size_t alt1_l1_edge;
  std::size_t alt1_l2_edge;
  std::size_t alt2_l1_edge;
  std::size_t alt2_l2_edge;
  std::string alt1_seq;
  std::string alt2_seq;
};

// Build a tiny DAG with one clade-wise alternative:
//
//       UA
//       |
//      root
//       |
//   +---+---+  (same root clade; choose alt1 OR alt2)
//   |       |
//  alt1    alt2
//   | \     | \
//  L1 L2   L1 L2
static alternative_dag_fixture make_alternative_dag() {
  constexpr std::string_view ref = "ACGTACGTACGTACGTACGT";
  std::string alt1_seq = "TCGTACGTACGTACGTACGT";
  std::string alt2_seq = "ACGTACGTACGCACGTACGT";
  phylo_dag d;

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from_sequence(ref, ref);

  auto alt1 = d.append_node<node_kind::inner>();
  alt1.cg() = cg_from_sequence(alt1_seq, ref);

  auto alt2 = d.append_node<node_kind::inner>();
  alt2.cg() = cg_from_sequence(alt2_seq, ref);

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from_sequence("TCGCACGTACGTACGTACGT", ref);
  l1.sample_id() = "L1";

  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from_sequence("TCGTACGTACGTACGCACGT", ref);
  l2.sample_id() = "L2";

  add_edge(d, ua.index(), root.index(), 0);
  auto root_alt1_edge = add_edge(d, root.index(), alt1.index(), 0);
  auto root_alt2_edge = add_edge(d, root.index(), alt2.index(), 0);
  auto alt1_l1_edge = add_edge(d, alt1.index(), l1.index(), 0);
  auto alt1_l2_edge = add_edge(d, alt1.index(), l2.index(), 1);
  auto alt2_l1_edge = add_edge(d, alt2.index(), l1.index(), 0);
  auto alt2_l2_edge = add_edge(d, alt2.index(), l2.index(), 1);

  recompute_edge_mutations(d);
  return {.dag = std::move(d),
          .root_alt1_edge = root_alt1_edge,
          .root_alt2_edge = root_alt2_edge,
          .alt1_l1_edge = alt1_l1_edge,
          .alt1_l2_edge = alt1_l2_edge,
          .alt2_l1_edge = alt2_l1_edge,
          .alt2_l2_edge = alt2_l2_edge,
          .alt1_seq = std::move(alt1_seq),
          .alt2_seq = std::move(alt2_seq)};
}

// --- tests ---

void test_reconstruct_sequence() {
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);

  // UA node → reference.
  auto ua_idx = std::visit([](auto n) { return n.index(); }, tree.get_root());
  auto ua_seq = reconstruct_sequence(tree, ua_idx, ref);
  assert(ua_seq == ref);

  // Spot-check an inner node (i1: A→T at pos 0), found by sequence rather
  // than insertion-order index.
  auto i1_idx = larch::test::find_inner_node_by_sequence(
      tree, "TCGTACGTACGTACGTACGT");
  auto i1_seq = reconstruct_sequence(tree, i1_idx, ref);
  assert(i1_seq == "TCGTACGTACGTACGTACGT");

  // Spot-check a leaf (L4: T→C at pos 11, T→C at pos 15), also found by
  // sequence.
  auto l4_idx = larch::test::find_leaf_by_sample_id(tree, "L4");
  auto l4_seq = reconstruct_sequence(tree, l4_idx, ref);
  assert(l4_seq == "ACGTACGTACGCACGCACGT");

  std::println("  reconstruct_sequence: OK");
}

void test_edge_scoring() {
  auto model = rs_fivemer_model::load(bcr_data_dir(), "s5f");
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);

  likelihood_score_ops ops{.model = model, .reference = ref};

  // Score every edge manually and via ops.
  double total_manual = 0.0;
  for (auto ev : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto eidx = edge.index();
          double score = ops.compute_edge(tree, eidx);
          assert(std::isfinite(score));
          assert(score >= 0.0);  // neg-log-likelihood is non-negative

          // Edges with no mutations should have score 0.
          if (edge.mutations().empty()) {
            assert(score == 0.0);
          } else {
            assert(score > 0.0);
          }
          total_manual += score;
        },
        ev);
  }

  assert(total_manual > 0.0);
  std::println("  edge scoring: OK (total neg-LL = {:.6f})", total_manual);
}

void test_likelihood_score_ops_ua_edge_behavior() {
  auto model = rs_fivemer_model::load(bcr_data_dir(), "s5f");
  auto tree = make_test_tree();

  // Make UA->root a mutated edge.  Generic likelihood_score_ops should ignore
  // this artificial edge by default, matching ml_model_likelihood_score_ops and
  // compute_dag_ml_nll().
  set_node_sequence(tree, 1, "CCGTACGTACGTACGTACGT");
  recompute_edge_mutations(tree);

  auto const& ref = get_reference_sequence(tree);
  likelihood_score_ops ignore_ops{.model = model, .reference = ref};
  likelihood_score_ops score_ops{
      .model = model, .reference = ref, .ignore_ua_edge = false};

  bool found_ua_edge = false;
  std::size_t ua_edge_idx = 0;
  for (auto ev : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (is_ua(tree, get_parent_idx(tree, edge.index()))) {
            found_ua_edge = true;
            ua_edge_idx = edge.index();
          }
        },
        ev);
  }
  assert(found_ua_edge);
  assert(ignore_ops.compute_edge(tree, ua_edge_idx) == 0.0);
  auto ua_edge_score = score_ops.compute_edge(tree, ua_edge_idx);
  assert(std::isfinite(ua_edge_score));
  assert(ua_edge_score > 0.0);

  auto ua_idx = get_root_idx(tree);
  subtree_weight<likelihood_score_ops<rs_fivemer_model>> ignore_sw{tree, 42u};
  subtree_weight<likelihood_score_ops<rs_fivemer_model>> score_sw{tree, 42u};
  auto ignored_total = ignore_sw.compute_weight_below(ua_idx, ignore_ops);
  auto scored_total = score_sw.compute_weight_below(ua_idx, score_ops);
  assert(std::abs(scored_total - ignored_total - ua_edge_score) < 1e-10);

  std::println("  generic likelihood_score_ops UA edge behavior: OK "
               "(ignored={:.6f}, scored={:.6f})",
               ignored_total, scored_total);
}

void test_subtree_weight_matches_manual() {
  auto model = rs_fivemer_model::load(bcr_data_dir(), "s5f");
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);

  // Manual total: sum of all edge neg-log-likelihoods.
  likelihood_score_ops ops{.model = model, .reference = ref};
  double manual_total = 0.0;
  for (auto ev : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          manual_total += ops.compute_edge(tree, edge.index());
        },
        ev);
  }

  // subtree_weight computation.
  auto ua_idx = std::visit([](auto n) { return n.index(); }, tree.get_root());
  subtree_weight<likelihood_score_ops<rs_fivemer_model>> sw{tree, 42u};
  double sw_total = sw.compute_weight_below(ua_idx, ops);

  assert(std::abs(sw_total - manual_total) < 1e-10);
  std::println("  subtree_weight matches manual: OK ({:.6f})", sw_total);
}

void test_ml_score_vs_parsimony() {
  auto model = rs_fivemer_model::load(bcr_data_dir(), "s5f");
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);

  // Parsimony score.
  auto ua_idx = std::visit([](auto n) { return n.index(); }, tree.get_root());
  subtree_weight<parsimony_score_ops> pars_sw{tree, 42u};
  auto parsimony = pars_sw.compute_weight_below(ua_idx, parsimony_score_ops{});

  // ML score.
  likelihood_score_ops ops{.model = model, .reference = ref};
  subtree_weight<likelihood_score_ops<rs_fivemer_model>> ml_sw{tree, 42u};
  double ml_score = ml_sw.compute_weight_below(ua_idx, ops);

  // Both should be positive (for a tree with mutations).
  assert(parsimony == 4);  // 4 mutated edges
  assert(ml_score > 0.0);
  assert(std::isfinite(ml_score));

  std::println("  ML vs parsimony: OK (parsimony={}, ML neg-LL={:.6f})",
               parsimony, ml_score);
}

static void assert_tree_parent_invariants(phylo_dag& sampled) {
  assert(is_tree(sampled));

  // Verify the sampled tree is valid (each non-UA node has exactly 1 parent).
  for (auto nv : sampled.get_all_nodes()) {
    std::visit(
        [](auto node) {
          std::size_t parent_count = 0;
          for (auto ev : node.get_parents()) {
            (void)ev;
            ++parent_count;
          }
          if constexpr (requires { node.reference_sequence(); }) {
            assert(parent_count == 0);
          } else {
            assert(parent_count == 1);
          }
        },
        nv);
  }
}

static void assert_valid_sampled_tree(phylo_dag& sampled,
                                      phylo_dag& original) {
  assert(node_count(sampled) == node_count(original));
  assert(edge_count(sampled) == edge_count(original));
  assert_tree_parent_invariants(sampled);
}

static std::string selected_alternative_sequence(phylo_dag& sampled,
                                                 std::string_view ref,
                                                 std::string_view alt1,
                                                 std::string_view alt2) {
  std::string chosen_alt_seq;
  std::size_t chosen_alt_count = 0;
  for (auto nv : sampled.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr ((requires { node.cg(); }) &&
                        !(requires { node.sample_id(); })) {
            auto seq = reconstruct_sequence(sampled, node.index(), ref);
            if (seq == alt1 || seq == alt2) {
              chosen_alt_seq = std::move(seq);
              ++chosen_alt_count;
            }
          }
        },
        nv);
  }
  assert(chosen_alt_count == 1);
  return chosen_alt_seq;
}

static std::string selected_leaf_sequence(phylo_dag& sampled,
                                          std::string_view alt1,
                                          std::string_view alt2) {
  std::string chosen_seq;
  std::size_t chosen_count = 0;
  for (auto nv : sampled.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            auto seq = larch::test::node_sequence(sampled, node.index());
            if (seq == alt1 || seq == alt2) {
              chosen_seq = std::move(seq);
              ++chosen_count;
            }
          }
        },
        nv);
  }
  assert(chosen_count == 1);
  return chosen_seq;
}

static std::size_t first_mutated_non_ua_edge(phylo_dag& tree) {
  std::size_t edge_idx = 0;
  bool found = false;
  for (auto ev : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (!found && !edge.mutations().empty() &&
              !is_ua(tree, get_parent_idx(tree, edge.index()))) {
            edge_idx = edge.index();
            found = true;
          }
        },
        ev);
  }
  assert(found);
  return edge_idx;
}

void test_min_weight_sample_tree() {
  auto model = rs_fivemer_model::load(bcr_data_dir(), "s5f");
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);

  likelihood_score_ops ops{.model = model, .reference = ref};
  subtree_weight<likelihood_score_ops<rs_fivemer_model>> sw{tree, 42u};

  // For a single tree (no DAG alternatives), the sampled tree should
  // have the same structure as the original.
  auto sampled = sw.min_weight_sample_tree(ops);
  assert_valid_sampled_tree(sampled, tree);

  std::println("  min_weight_sample_tree: OK");
}

void test_likelihood_score_ops_exact_tie_selection() {
  auto dag = make_two_alternative_ml_tie_dag();
  auto const& ref = get_reference_sequence(dag);
  two_alt_nll_model model{.alt1_nll = 7.0, .alt2_nll = 7.0};
  likelihood_score_ops ops{.model = model, .reference = ref};
  subtree_weight<likelihood_score_ops<two_alt_nll_model>> sw{dag, 42u};

  auto root_idx = get_root_idx(dag);
  double score = sw.compute_weight_below(root_idx, ops);
  assert(std::abs(score - 7.0) < 1e-12);
  assert(sw.min_weight_count(root_idx, ops) == bigint{2});

  std::println("  likelihood_score_ops exact tie selection: OK");
}

void test_likelihood_score_ops_relative_tie_selection() {
  auto dag = make_two_alternative_ml_tie_dag();
  auto const& ref = get_reference_sequence(dag);
  double best = 1000000.0;
  double tol = min_weight_tie_tolerance(best);
  two_alt_nll_model model{.alt1_nll = best, .alt2_nll = best + 0.5 * tol};
  likelihood_score_ops ops{.model = model, .reference = ref};
  subtree_weight<likelihood_score_ops<two_alt_nll_model>> sw{dag, 42u};

  auto root_idx = get_root_idx(dag);
  double score = sw.compute_weight_below(root_idx, ops);
  assert(std::abs(score - best) < 1e-12);
  assert(sw.min_weight_count(root_idx, ops) == bigint{2});

  two_alt_nll_model outside_model{.alt1_nll = best,
                                  .alt2_nll = best + 2.0 * tol};
  likelihood_score_ops outside_ops{.model = outside_model, .reference = ref};
  subtree_weight<likelihood_score_ops<two_alt_nll_model>> outside_sw{dag, 42u};
  double outside_score = outside_sw.compute_weight_below(root_idx, outside_ops);
  assert(std::abs(outside_score - best) < 1e-12);
  assert(outside_sw.min_weight_count(root_idx, outside_ops) == bigint{1});

  std::println("  likelihood_score_ops relative tie selection: OK");
}

void test_likelihood_score_ops_uniform_tie_sampling_balance() {
  auto dag = make_two_alternative_ml_tie_dag();
  auto const& ref = get_reference_sequence(dag);
  two_alt_nll_model model{.alt1_nll = 7.0, .alt2_nll = 7.0};
  likelihood_score_ops ops{.model = model, .reference = ref};
  subtree_weight<likelihood_score_ops<two_alt_nll_model>> sw{dag, 8675309u};

  auto root_idx = get_root_idx(dag);
  assert(sw.min_weight_count(root_idx, ops) == bigint{2});

  int alt1_count = 0;
  int alt2_count = 0;
  constexpr int trials = 2000;
  for (int i = 0; i < trials; ++i) {
    auto sampled = sw.min_weight_uniform_sample_tree(ops);
    auto chosen = selected_leaf_sequence(sampled, "CAAA", "ACAA");
    if (chosen == "CAAA")
      ++alt1_count;
    else if (chosen == "ACAA")
      ++alt2_count;
    else
      assert(false && "unexpected ML sampled alternative");
  }

  assert(alt1_count + alt2_count == trials);
  assert(alt1_count > trials * 0.4 && alt1_count < trials * 0.6);
  assert(alt2_count > trials * 0.4 && alt2_count < trials * 0.6);

  std::println("  likelihood_score_ops uniform tie sampling: OK "
               "(alt1={}, alt2={})",
               alt1_count, alt2_count);
}

void test_ml_model_likelihood_score_ops_thrifty() {
  auto const& model = thrifty_model();
  assert(std::holds_alternative<indep_rs_cnn_model>(model));

  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  auto ua_idx = std::visit([](auto n) { return n.index(); }, tree.get_root());
  subtree_weight<ml_model_likelihood_score_ops> sw{tree, 42u};
  double score = sw.compute_weight_below(ua_idx, ops);
  double manual_total = manual_edge_sum(tree, ops);
  assert(std::isfinite(score));
  assert(score >= 0.0);
  assert(std::abs(score - manual_total) < 1e-8);

  auto sampled = sw.min_weight_sample_tree(ops);
  assert_valid_sampled_tree(sampled, tree);

  std::println("  ml_model_likelihood_score_ops thrifty: OK ({:.6f})", score);
}

void test_ml_model_likelihood_score_ops_ua_edge_behavior() {
  auto const& model = thrifty_model();
  auto tree = make_test_tree();

  // Mutate the biological root away from the UA/reference so UA->root is a
  // non-empty edge.  Node 1 is the root child in make_test_tree().
  set_node_sequence(tree, 1, "CCGTACGTACGTACGTACGT");
  recompute_edge_mutations(tree);

  auto const& ref = get_reference_sequence(tree);
  ml_model_likelihood_score_ops ignore_ops{
      .model = model, .reference = ref, .ignore_ua_edge = true};
  ml_model_likelihood_score_ops score_ops{
      .model = model, .reference = ref, .ignore_ua_edge = false};

  bool found_ua_edge = false;
  std::size_t ua_edge_idx = 0;
  for (auto ev : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          if (is_ua(tree, get_parent_idx(tree, edge.index()))) {
            found_ua_edge = true;
            ua_edge_idx = edge.index();
          }
        },
        ev);
  }
  assert(found_ua_edge);
  assert(ignore_ops.compute_edge(tree, ua_edge_idx) == 0.0);
  double ua_edge_score = score_ops.compute_edge(tree, ua_edge_idx);
  assert(std::isfinite(ua_edge_score));
  assert(ua_edge_score > 0.0);

  auto ua_idx = get_root_idx(tree);
  subtree_weight<ml_model_likelihood_score_ops> ignore_sw{tree, 42u};
  subtree_weight<ml_model_likelihood_score_ops> score_sw{tree, 42u};
  double ignored_total = ignore_sw.compute_weight_below(ua_idx, ignore_ops);
  double scored_total = score_sw.compute_weight_below(ua_idx, score_ops);

  assert(std::abs(ignored_total - manual_edge_sum(tree, ignore_ops)) < 1e-8);
  assert(std::abs(scored_total - manual_edge_sum(tree, score_ops)) < 1e-8);
  assert(std::abs(scored_total - ignored_total - ua_edge_score) < 1e-8);
  assert(std::abs(ignored_total - compute_dag_ml_nll(model, tree)) < 1e-8);
  assert(std::abs(scored_total - compute_dag_ml_nll(model, tree, false)) <
         1e-8);

  std::println("  ml_model_likelihood_score_ops UA edge behavior: OK "
               "(ignored={:.6f}, scored={:.6f})",
               ignored_total, scored_total);
}

void test_ml_model_likelihood_score_ops_cache_reuse() {
  auto const& model = thrifty_model();
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  auto mutated_edge_idx = first_mutated_non_ua_edge(tree);

  assert(ops.cached_edge_score_count() == 0);
  assert(ops.cached_sequence_count() == 0);
  double first = ops.compute_edge(tree, mutated_edge_idx);
  assert(std::isfinite(first));
  assert(first > 0.0);
  assert(ops.cached_edge_score_count() == 1);
  assert(ops.cached_sequence_count() == 2);

  double second = ops.compute_edge(tree, mutated_edge_idx);
  assert(second == first);
  assert(ops.cached_edge_score_count() == 1);
  assert(ops.cached_sequence_count() == 2);

  ops.clear_cache();
  assert(ops.cached_edge_score_count() == 0);
  assert(ops.cached_sequence_count() == 0);
  double after_clear = ops.compute_edge(tree, mutated_edge_idx);
  assert(std::abs(after_clear - first) < 1e-12);

  std::println("  ml_model_likelihood_score_ops cache reuse: OK "
               "(edge score={:.6f})",
               first);
}

void test_compute_dag_ml_nll_reuses_ops_cache() {
  auto const& model = thrifty_model();
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  double first = compute_dag_ml_nll(tree, ops);
  auto cached_edges = ops.cached_edge_score_count();
  auto cached_sequences = ops.cached_sequence_count();
  assert(std::isfinite(first));
  assert(cached_edges > 0);
  assert(cached_sequences > 0);

  double second = compute_dag_ml_nll(tree, ops);
  assert(std::abs(second - first) < 1e-12);
  assert(ops.cached_edge_score_count() == cached_edges);
  assert(ops.cached_sequence_count() == cached_sequences);

  std::println("  compute_dag_ml_nll ops-cache reuse: OK "
               "(NLL={:.6f}, cached_edges={})",
               first, cached_edges);
}

void test_ml_model_likelihood_score_ops_cache_switches_dags() {
  auto const& model = thrifty_model();
  auto tree_a = make_test_tree();
  auto tree_b = make_test_tree();
  auto const& ref = get_reference_sequence(tree_a);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  auto edge_a = first_mutated_non_ua_edge(tree_a);
  auto score_a = ops.compute_edge(tree_a, edge_a);
  assert(std::isfinite(score_a));
  assert(ops.cached_edge_score_count() == 1);
  assert(ops.cached_sequence_count() == 2);

  auto edge_b = first_mutated_non_ua_edge(tree_b);
  auto score_b = ops.compute_edge(tree_b, edge_b);
  assert(std::isfinite(score_b));
  assert(ops.cached_edge_score_count() == 1);
  assert(ops.cached_sequence_count() == 2);

  auto score_a_again = ops.compute_edge(tree_a, edge_a);
  assert(std::abs(score_a_again - score_a) < 1e-12);
  assert(ops.cached_edge_score_count() == 1);
  assert(ops.cached_sequence_count() == 2);

  std::println("  ml_model_likelihood_score_ops cache switches DAGs: OK "
               "(A={:.6f}, B={:.6f})",
               score_a, score_b);
}

void test_ml_model_likelihood_score_ops_deterministic_repeated_sampling() {
  auto const& model = thrifty_model();
  auto fixture = make_alternative_dag();
  auto& dag = fixture.dag;
  auto const& ref = get_reference_sequence(dag);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  auto sample_once = [&](std::uint32_t seed) {
    subtree_weight<ml_model_likelihood_score_ops> sw{dag, seed};
    double score = sw.compute_weight_below(get_root_idx(dag), ops);
    auto sampled = sw.min_weight_uniform_sample_tree(ops);
    auto chosen = selected_alternative_sequence(sampled, ref, fixture.alt1_seq,
                                                fixture.alt2_seq);
    return std::pair{score, chosen};
  };

  auto [score1, chosen1] = sample_once(123u);
  auto cached_edges_after_first = ops.cached_edge_score_count();
  auto cached_sequences_after_first = ops.cached_sequence_count();
  auto [score2, chosen2] = sample_once(123u);

  assert(std::abs(score1 - score2) < 1e-12);
  assert(chosen1 == chosen2);
  assert(ops.cached_edge_score_count() == cached_edges_after_first);
  assert(ops.cached_sequence_count() == cached_sequences_after_first);

  std::println("  ml_model_likelihood_score_ops repeated sampling: OK "
               "(score={:.6f}, chosen={})",
               score1, chosen1);
}

void test_ml_model_edge_min_global_scores_single_tree() {
  auto const& model = thrifty_model();
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  auto close = [](double lhs, double rhs) {
    return std::abs(lhs - rhs) <=
           1e-8 * std::max({1.0, std::abs(lhs), std::abs(rhs)});
  };

  subtree_weight<ml_model_likelihood_score_ops> sw{tree, 42u};
  double global_min = sw.compute_weight_below(get_root_idx(tree), ops);
  auto scores = sw.compute_edge_min_global_scores(ops);
  assert(std::isfinite(global_min));

  for (auto ev : tree.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          assert(std::isfinite(scores[edge.index()]));
          assert(close(scores[edge.index()], global_min));
        },
        ev);
  }

  std::println("  ml_model edge min global scores single tree: OK "
               "(global={:.6f})",
               global_min);
}

void test_ml_model_edge_min_global_scores() {
  auto const& model = thrifty_model();
  auto fixture = make_alternative_dag();
  auto& dag = fixture.dag;
  auto const& ref = get_reference_sequence(dag);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  double alt1_score = ops.compute_edge(dag, fixture.root_alt1_edge) +
                      ops.compute_edge(dag, fixture.alt1_l1_edge) +
                      ops.compute_edge(dag, fixture.alt1_l2_edge);
  double alt2_score = ops.compute_edge(dag, fixture.root_alt2_edge) +
                      ops.compute_edge(dag, fixture.alt2_l1_edge) +
                      ops.compute_edge(dag, fixture.alt2_l2_edge);
  double expected = std::min(alt1_score, alt2_score);

  auto close = [](double lhs, double rhs) {
    return std::abs(lhs - rhs) <=
           1e-8 * std::max({1.0, std::abs(lhs), std::abs(rhs)});
  };

  subtree_weight<ml_model_likelihood_score_ops> sw{dag, 42u};
  double global_min = sw.compute_weight_below(get_root_idx(dag), ops);
  auto scores = sw.compute_edge_min_global_scores(ops);
  assert(close(global_min, expected));

  for (auto ev : dag.get_all_edges()) {
    std::visit(
        [&](auto edge) {
          auto score = scores[edge.index()];
          assert(std::isfinite(score));
          assert(score + 1e-8 * std::max(1.0, std::abs(global_min)) >=
                 global_min);
        },
        ev);
  }

  assert(close(scores[fixture.root_alt1_edge], alt1_score));
  assert(close(scores[fixture.alt1_l1_edge], alt1_score));
  assert(close(scores[fixture.alt1_l2_edge], alt1_score));
  assert(close(scores[fixture.root_alt2_edge], alt2_score));
  assert(close(scores[fixture.alt2_l1_edge], alt2_score));
  assert(close(scores[fixture.alt2_l2_edge], alt2_score));

  constexpr double eps = 1e-8;
  if (alt1_score + eps < alt2_score) {
    assert(close(scores[fixture.root_alt1_edge], global_min));
    assert(scores[fixture.root_alt2_edge] - global_min > eps);
  } else if (alt2_score + eps < alt1_score) {
    assert(close(scores[fixture.root_alt2_edge], global_min));
    assert(scores[fixture.root_alt1_edge] - global_min > eps);
  }

  auto sampled = sw.min_weight_sample_tree(ops);
  auto chosen = selected_alternative_sequence(sampled, ref, fixture.alt1_seq,
                                              fixture.alt2_seq);
  if (chosen == fixture.alt1_seq)
    assert(close(scores[fixture.root_alt1_edge], global_min));
  else
    assert(close(scores[fixture.root_alt2_edge], global_min));

  std::println("  ml_model edge min global scores: OK "
               "(global={:.6f}, alt1={:.6f}, alt2={:.6f})",
               global_min, alt1_score, alt2_score);
}

void test_ml_model_likelihood_score_ops_selects_min_alternative() {
  auto const& model = thrifty_model();
  auto fixture = make_alternative_dag();
  auto& dag = fixture.dag;
  auto const& ref = get_reference_sequence(dag);
  ml_model_likelihood_score_ops ops{.model = model, .reference = ref};

  double alt1_score = ops.compute_edge(dag, fixture.root_alt1_edge) +
                      ops.compute_edge(dag, fixture.alt1_l1_edge) +
                      ops.compute_edge(dag, fixture.alt1_l2_edge);
  double alt2_score = ops.compute_edge(dag, fixture.root_alt2_edge) +
                      ops.compute_edge(dag, fixture.alt2_l1_edge) +
                      ops.compute_edge(dag, fixture.alt2_l2_edge);
  double expected = std::min(alt1_score, alt2_score);

  auto ua_idx = get_root_idx(dag);
  subtree_weight<ml_model_likelihood_score_ops> sw{dag, 42u};
  double score = sw.compute_weight_below(ua_idx, ops);
  assert(std::abs(score - expected) < 1e-8);

  auto sampled = sw.min_weight_sample_tree(ops);
  assert_tree_parent_invariants(sampled);

  std::string chosen_alt_seq;
  std::size_t chosen_alt_count = 0;
  for (auto nv : sampled.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr ((requires { node.cg(); }) &&
                        !(requires { node.sample_id(); })) {
            auto seq = reconstruct_sequence(sampled, node.index(), ref);
            if (seq == fixture.alt1_seq || seq == fixture.alt2_seq) {
              chosen_alt_seq = std::move(seq);
              ++chosen_alt_count;
            }
          }
        },
        nv);
  }
  assert(chosen_alt_count == 1);

  constexpr double eps = 1e-10;
  bool chose_best_alt1 =
      chosen_alt_seq == fixture.alt1_seq && alt1_score <= expected + eps;
  bool chose_best_alt2 =
      chosen_alt_seq == fixture.alt2_seq && alt2_score <= expected + eps;
  assert(chose_best_alt1 || chose_best_alt2);

  std::println("  ml_model_likelihood_score_ops min alternative: OK "
               "(alt1={:.6f}, alt2={:.6f})",
               alt1_score, alt2_score);
}

int main() {
  std::println("=== nn_ml_spr tests ===");
  test_reconstruct_sequence();
  test_edge_scoring();
  test_likelihood_score_ops_ua_edge_behavior();
  test_subtree_weight_matches_manual();
  test_ml_score_vs_parsimony();
  test_min_weight_sample_tree();
  test_likelihood_score_ops_exact_tie_selection();
  test_likelihood_score_ops_relative_tie_selection();
  test_likelihood_score_ops_uniform_tie_sampling_balance();
  test_ml_model_likelihood_score_ops_thrifty();
  test_ml_model_likelihood_score_ops_ua_edge_behavior();
  test_ml_model_likelihood_score_ops_cache_reuse();
  test_compute_dag_ml_nll_reuses_ops_cache();
  test_ml_model_likelihood_score_ops_cache_switches_dags();
  test_ml_model_likelihood_score_ops_deterministic_repeated_sampling();
  test_ml_model_edge_min_global_scores_single_tree();
  test_ml_model_edge_min_global_scores();
  test_ml_model_likelihood_score_ops_selects_min_alternative();
  std::println("All nn_ml_spr tests passed");
}

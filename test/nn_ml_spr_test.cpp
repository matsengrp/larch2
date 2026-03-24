// ML-based DAG scoring via subtree_weight with likelihood_score_ops.
// Builds a tree with 20-base sequences, scores it with the S5F model,
// and verifies subtree_weight returns the same total as manual edge scoring.
#include <larch/likelihood_score_ops.hpp>
#include <larch/rs_fivemer_model.hpp>
#include <larch/subtree_weight.hpp>

#include <cassert>
#include <cmath>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <variant>

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

static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx, std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
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

// --- tests ---

void test_reconstruct_sequence() {
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);

  // UA node → reference.
  auto ua_idx = std::visit([](auto n) { return n.index(); }, tree.get_root());
  auto ua_seq = reconstruct_sequence(tree, ua_idx, ref);
  assert(ua_seq == ref);

  // Spot-check an inner node (i1: A→T at pos 0).
  // i1 is at index 2 (ua=0, root=1, i1=2).
  auto i1_seq = reconstruct_sequence(tree, 2, ref);
  assert(i1_seq == "TCGTACGTACGTACGTACGT");

  // Spot-check a leaf (L4: T→C at pos 11, T→C at pos 15).
  auto l4_seq = reconstruct_sequence(tree, 7, ref);
  assert(l4_seq == "ACGTACGTACGCACGCACGT");

  std::println("  reconstruct_sequence: OK");
}

void test_edge_scoring() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
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

void test_subtree_weight_matches_manual() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
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
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
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

void test_min_weight_sample_tree() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  auto tree = make_test_tree();
  auto const& ref = get_reference_sequence(tree);

  likelihood_score_ops ops{.model = model, .reference = ref};
  subtree_weight<likelihood_score_ops<rs_fivemer_model>> sw{tree, 42u};

  // For a single tree (no DAG alternatives), the sampled tree should
  // have the same structure as the original.
  auto sampled = sw.min_weight_sample_tree(ops);
  assert(node_count(sampled) == node_count(tree));
  assert(edge_count(sampled) == edge_count(tree));

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

  std::println("  min_weight_sample_tree: OK");
}

int main() {
  std::println("=== nn_ml_spr tests ===");
  test_reconstruct_sequence();
  test_edge_scoring();
  test_subtree_weight_matches_manual();
  test_ml_score_vs_parsimony();
  test_min_weight_sample_tree();
  std::println("All nn_ml_spr tests passed");
}

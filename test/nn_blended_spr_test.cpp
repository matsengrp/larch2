// Test blended ML+parsimony scoring for SPR move evaluation.
#include <larch/compute.hpp>
#include <larch/model_variant.hpp>
#include <larch/native_optimize.hpp>
#include <larch/overlay_spr.hpp>
#include <larch/thread_pool.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>
#include <print>
#include <string>
#include <vector>

using namespace larch;

// Build a small 4-leaf tree with 20-base sequences.
//       UA
//       |
//      root  (= ref = "ACGTACGTACGTACGTACGT")
//      / \
//    i1    i2
//   / \   / \
//  L1  L2 L3  L4
static phylo_dag make_tree_from_sequences(
    std::string_view root_seq, std::string_view i1_seq,
    std::string_view i2_seq, std::string_view l1_seq,
    std::string_view l2_seq, std::string_view l3_seq,
    std::string_view l4_seq) {
  constexpr std::string_view ref = "ACGTACGTACGTACGTACGT";
  phylo_dag d;

  auto cg_from = [&](std::string_view seq) {
    std::map<mutation_position, nuc_base> muts;
    for (std::size_t i = 0; i < seq.size(); ++i)
      if (seq[i] != ref[i]) muts[i + 1] = nuc_base::from_char(seq[i]);
    return compact_genome{std::move(muts)};
  };

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto root = d.append_node<node_kind::inner>();
  root.cg() = cg_from(root_seq);

  auto i1 = d.append_node<node_kind::inner>();
  i1.cg() = cg_from(i1_seq);

  auto i2 = d.append_node<node_kind::inner>();
  i2.cg() = cg_from(i2_seq);

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from(l1_seq);
  l1.sample_id() = "L1";

  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from(l2_seq);
  l2.sample_id() = "L2";

  auto l3 = d.append_node<node_kind::leaf>();
  l3.cg() = cg_from(l3_seq);
  l3.sample_id() = "L3";

  auto l4 = d.append_node<node_kind::leaf>();
  l4.cg() = cg_from(l4_seq);
  l4.sample_id() = "L4";

  auto add_edge = [&](std::size_t pi, std::size_t ci, std::size_t clade) {
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = clade;
    auto pv = d.get_node(pi);
    std::visit([&](auto p) { edge.set_parent(p); }, pv);
    auto cv = d.get_node(ci);
    std::visit([&](auto c) { edge.set_child(c); }, cv);
  };

  add_edge(ua.index(), root.index(), 0);
  add_edge(root.index(), i1.index(), 0);
  add_edge(root.index(), i2.index(), 1);
  add_edge(i1.index(), l1.index(), 0);
  add_edge(i1.index(), l2.index(), 1);
  add_edge(i2.index(), l3.index(), 0);
  add_edge(i2.index(), l4.index(), 1);

  recompute_edge_mutations(d);
  return d;
}

static phylo_dag make_test_tree() {
  return make_tree_from_sequences("ACGTACGTACGTACGTACGT",
                                  "TCGTACGTACGTACGTACGT",
                                  "ACGTACGTACGCACGTACGT",
                                  "TCGTACGTACGTACGTACGT",
                                  "TCGCACGTACGTACGTACGT",
                                  "ACGTACGTACGCACGTACGT",
                                  "ACGTACGTACGCACGCACGT");
}

static phylo_dag make_ua_edge_test_tree() {
  // Every non-UA node carries the A1T mutation, so scoring vs ignoring the
  // artificial UA edge changes the full-tree NLL.
  return make_tree_from_sequences("TCGTACGTACGTACGTACGT",
                                  "TCGTACGTACGTACGTACGA",
                                  "TCGTACGTACGCACGTACGT",
                                  "TCGTACGTACGTACGTACGA",
                                  "TCGCACGTACGTACGTACGA",
                                  "TCGTACGTACGCACGTACGT",
                                  "TCGTACGTACGCACGCACGT");
}

static bool close(double lhs, double rhs) {
  return std::abs(lhs - rhs) <=
         1e-8 * std::max({1.0, std::abs(lhs), std::abs(rhs)});
}

static std::vector<profitable_move> collect_moves(phylo_dag& tree,
                                                  int threshold = -1) {
  auto& pool = thread_pool::get_default();
  tree_index idx{tree, pool};
  move_enumerator enumerator{idx, threshold};

  std::vector<profitable_move> moves;
  for (auto src : idx.get_searchable_nodes()) {
    enumerator.find_moves_for_source(
        src, 10, [&](profitable_move const& mv) { moves.push_back(mv); });
  }
  return moves;
}

void test_load_ml_model_s5f() {
  auto model = load_ml_model("data/bcr", "s5f");
  assert(std::holds_alternative<rs_fivemer_model>(model));
  std::println("  load_ml_model s5f: OK");
}

void test_load_ml_model_cnn() {
  auto model = load_ml_model("data/bcr", "ThriftyHumV0.2-45");
  assert(std::holds_alternative<indep_rs_cnn_model>(model));
  std::println("  load_ml_model cnn: OK");
}

void test_ml_log_likelihood_via_variant() {
  auto model = load_ml_model("data/bcr", "s5f");
  double ll = ml_log_likelihood(model, "ACGTACGT", "ACGTACGT");
  assert(ll == 0.0);
  double ll2 = ml_log_likelihood(model, "ACGTACGT", "CCGTACGT");
  assert(std::isfinite(ll2) && ll2 < 0.0);
  std::println("  ml_log_likelihood via variant: OK");
}

void test_delta_ml_score() {
  auto model = load_ml_model("data/bcr", "s5f");
  auto tree = make_test_tree();

  auto moves = collect_moves(tree, 0);
  assert(!moves.empty() &&
         "expected at least one non-worsening SPR move in fixture");

  auto& mv = moves[0];
  spr_move sm{.src = mv.src,
              .dst = mv.dst,
              .lca = mv.lca,
              .score_change = mv.score_change};
  auto frag = apply_spr_as_fragment(tree, sm);

  double old_nll = compute_dag_ml_nll(model, tree);
  double frag_nll = compute_dag_ml_nll(model, frag);
  assert(std::isfinite(old_nll));
  assert(std::isfinite(frag_nll));
  assert(frag_nll >= 0.0);

  double expected_delta = old_nll - frag_nll;
  double delta = compute_delta_ml_score(model, tree, frag, sm);
  double cached_delta = compute_delta_ml_score(model, old_nll, frag);
  assert(std::isfinite(delta));
  assert(close(delta, expected_delta));
  assert(close(cached_delta, expected_delta));

  std::println("  delta_ml_score: OK (delta={:.6f}, old={:.6f}, new={:.6f})",
               delta, old_nll, frag_nll);
}

void test_delta_ml_score_ua_edge_modes() {
  auto model = load_ml_model("data/bcr", "s5f");
  auto tree = make_ua_edge_test_tree();

  auto moves = collect_moves(tree, 0);
  assert(!moves.empty() &&
         "expected at least one non-worsening SPR move in UA-edge fixture");

  auto& mv = moves[0];
  spr_move sm{.src = mv.src,
              .dst = mv.dst,
              .lca = mv.lca,
              .score_change = mv.score_change};
  auto frag = apply_spr_as_fragment(tree, sm);

  double old_ignore = compute_dag_ml_nll(model, tree, true);
  double new_ignore = compute_dag_ml_nll(model, frag, true);
  double old_scored = compute_dag_ml_nll(model, tree, false);
  double new_scored = compute_dag_ml_nll(model, frag, false);

  assert(std::isfinite(old_ignore));
  assert(std::isfinite(new_ignore));
  assert(std::isfinite(old_scored));
  assert(std::isfinite(new_scored));
  assert(old_scored > old_ignore);
  assert(new_scored > new_ignore);

  double delta_ignore = compute_delta_ml_score(model, tree, frag, sm, true);
  double delta_scored = compute_delta_ml_score(model, tree, frag, sm, false);
  double cached_delta_ignore = compute_delta_ml_score(model, old_ignore, frag, true);
  double cached_delta_scored = compute_delta_ml_score(model, old_scored, frag, false);
  assert(close(delta_ignore, old_ignore - new_ignore));
  assert(close(delta_scored, old_scored - new_scored));
  assert(close(cached_delta_ignore, old_ignore - new_ignore));
  assert(close(cached_delta_scored, old_scored - new_scored));

  std::println("  delta_ml_score UA modes: OK (ignore={:.6f}, scored={:.6f})",
               delta_ignore, delta_scored);
}

void test_ml_scoring_config_adjust_score() {
  auto model = load_ml_model("data/bcr", "s5f");
  auto tree = make_test_tree();

  auto moves = collect_moves(tree, 0);
  assert(!moves.empty() &&
         "expected at least one non-worsening SPR move in fixture");

  auto& mv = moves[0];
  spr_move sm{.src = mv.src,
              .dst = mv.dst,
              .lca = mv.lca,
              .score_change = mv.score_change};
  auto frag = apply_spr_as_fragment(tree, sm);

  // With coeff=0: adjust_score returns base_score unchanged.
  ml_scoring_config cfg_off{.model = nullptr, .coeff = 0.0};
  double base = static_cast<double>(mv.score_change);
  assert(cfg_off.adjust_score(base, tree, frag, sm) == base);

  // With coeff>0: adjust_score applies the full-tree NLL delta.  The cached
  // old-NLL overload is the one used by larch2 move rescoring.
  ml_scoring_config cfg_on{.model = &model, .coeff = 1.0};
  double old_nll = compute_dag_ml_nll(model, tree);
  double adjusted = cfg_on.adjust_score(base, tree, frag, sm);
  double adjusted_cached = cfg_on.adjust_score(base, old_nll, frag);
  assert(std::isfinite(adjusted));
  assert(close(adjusted, adjusted_cached));

  std::println("  ml_scoring_config adjust_score: OK (base={:.4f} adj={:.4f})",
               base, adjusted);
}

void test_blended_scoring_zero_ml_matches_parsimony() {
  auto model = load_ml_model("data/bcr", "s5f");
  auto tree = make_test_tree();

  auto moves = collect_moves(tree, 0);
  assert(!moves.empty() &&
         "expected at least one non-worsening SPR move in fixture");

  // With coeff=0, adjust_score is identity → matches pure parsimony.
  ml_scoring_config cfg{.model = &model, .coeff = 0.0};
  for (auto& mv : moves) {
    spr_move sm{.src = mv.src,
                .dst = mv.dst,
                .lca = mv.lca,
                .score_change = mv.score_change};
    auto frag = apply_spr_as_fragment(tree, sm);
    double base = static_cast<double>(mv.score_change);
    assert(cfg.adjust_score(base, tree, frag, sm) == base);
  }
  std::println("  blended zero-ml matches parsimony: OK");
}

void test_rescored_move_strict_improvement_policy() {
  assert(is_strictly_improving_rescored_move(-1));
  assert(!is_strictly_improving_rescored_move(0));
  assert(!is_strictly_improving_rescored_move(1));

  assert(is_strictly_improving_rescored_move(-1e-6));
  assert(!is_strictly_improving_rescored_move(0.0));
  assert(!is_strictly_improving_rescored_move(
      -rescored_move_score_tolerance / 2.0));
  assert(!is_strictly_improving_rescored_move(
      rescored_move_score_tolerance));

  std::println("  rescored move strict-improvement policy: OK");
}

void test_adjust_rate_bias_via_variant() {
  auto model = load_ml_model("data/bcr", "s5f");
  auto const parent = std::string{"ACGTACGTACGTACGTACGT"};
  auto rates_before = std::get<rs_fivemer_model>(model).forward(parent).rates;

  ml_adjust_rate_bias(model, std::log(2.0));

  auto const& adjusted = std::get<rs_fivemer_model>(model);
  assert(std::abs(adjusted.rate_bias_log() - std::log(2.0)) < 1e-12);
  auto rates_after = adjusted.forward(parent).rates;
  assert(rates_after.size() == rates_before.size());
  for (std::size_t i = 0; i < rates_before.size(); ++i) {
    double expected = static_cast<double>(rates_before[i]) * 2.0;
    assert(std::abs(static_cast<double>(rates_after[i]) - expected) <
           1e-4 * expected);
  }

  std::println("  adjust_rate_bias via variant: OK");
}

int main() {
  std::println("=== nn_blended_spr tests ===");
  test_load_ml_model_s5f();
  test_load_ml_model_cnn();
  test_ml_log_likelihood_via_variant();
  test_delta_ml_score();
  test_delta_ml_score_ua_edge_modes();
  test_ml_scoring_config_adjust_score();
  test_blended_scoring_zero_ml_matches_parsimony();
  test_rescored_move_strict_improvement_policy();
  test_adjust_rate_bias_via_variant();
  std::println("All nn_blended_spr tests passed");
}

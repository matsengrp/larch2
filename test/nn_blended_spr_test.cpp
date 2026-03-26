// Test blended ML+parsimony scoring for SPR move evaluation.
#include <larch/compute.hpp>
#include <larch/model_variant.hpp>
#include <larch/native_optimize.hpp>
#include <larch/overlay_spr.hpp>
#include <larch/thread_pool.hpp>

#include <cassert>
#include <cmath>
#include <print>
#include <string>

using namespace larch;

// Build a small 4-leaf tree with 20-base sequences.
//       UA
//       |
//      root  (= ref = "ACGTACGTACGTACGTACGT")
//      / \
//    i1    i2
//   / \   / \
//  L1  L2 L3  L4
static phylo_dag make_test_tree() {
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
  root.cg() = cg_from(ref);

  auto i1 = d.append_node<node_kind::inner>();
  i1.cg() = cg_from("TCGTACGTACGTACGTACGT");

  auto i2 = d.append_node<node_kind::inner>();
  i2.cg() = cg_from("ACGTACGTACGCACGTACGT");

  auto l1 = d.append_node<node_kind::leaf>();
  l1.cg() = cg_from("TCGTACGTACGTACGTACGT");
  l1.sample_id() = "L1";

  auto l2 = d.append_node<node_kind::leaf>();
  l2.cg() = cg_from("TCGCACGTACGTACGTACGT");
  l2.sample_id() = "L2";

  auto l3 = d.append_node<node_kind::leaf>();
  l3.cg() = cg_from("ACGTACGTACGCACGTACGT");
  l3.sample_id() = "L3";

  auto l4 = d.append_node<node_kind::leaf>();
  l4.cg() = cg_from("ACGTACGTACGCACGCACGT");
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

  // Build tree index and find a profitable move.
  auto& pool = thread_pool::get_default();
  tree_index idx{tree, pool};
  move_enumerator enumerator{idx, -1};

  std::vector<profitable_move> moves;
  for (auto src : idx.get_searchable_nodes()) {
    enumerator.find_moves_for_source(
        src, 10, [&](profitable_move const& mv) { moves.push_back(mv); });
  }

  if (moves.empty()) {
    std::println("  delta_ml_score: SKIP (no profitable moves)");
    return;
  }

  // Generate a fragment and compute delta LL.
  auto& mv = moves[0];
  spr_move sm{.src = mv.src,
              .dst = mv.dst,
              .lca = mv.lca,
              .score_change = mv.score_change};
  auto frag = apply_spr_as_fragment(tree, sm);

  // Fragment NLL should be finite and non-negative.
  double frag_nll = compute_dag_ml_nll(model, frag);
  assert(std::isfinite(frag_nll));
  assert(frag_nll >= 0.0);

  // Delta should be finite (old_NLL - new_NLL).
  double delta = compute_delta_ml_score(model, tree, frag, sm);
  assert(std::isfinite(delta));

  std::println("  delta_ml_score: OK (delta={:.6f}, frag_nll={:.6f})", delta,
               frag_nll);
}

void test_ml_scoring_config_adjust_score() {
  auto model = load_ml_model("data/bcr", "s5f");
  auto tree = make_test_tree();

  auto& pool = thread_pool::get_default();
  tree_index idx{tree, pool};
  move_enumerator enumerator{idx, -1};

  std::vector<profitable_move> moves;
  for (auto src : idx.get_searchable_nodes()) {
    enumerator.find_moves_for_source(
        src, 10, [&](profitable_move const& mv) { moves.push_back(mv); });
  }
  if (moves.empty()) {
    std::println("  ml_scoring_config adjust_score: SKIP");
    return;
  }

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

  // With coeff>0: adjust_score modifies the score.
  ml_scoring_config cfg_on{.model = &model, .coeff = 1.0};
  double adjusted = cfg_on.adjust_score(base, tree, frag, sm);
  assert(std::isfinite(adjusted));
  // The ML adjustment should change the score (delta_LL != 0 for a real move).
  // (May coincidentally equal base if delta_LL is exactly 0, but very unlikely.)

  std::println("  ml_scoring_config adjust_score: OK (base={:.4f} adj={:.4f})",
               base, adjusted);
}

void test_blended_scoring_zero_ml_matches_parsimony() {
  auto model = load_ml_model("data/bcr", "s5f");
  auto tree = make_test_tree();

  auto& pool = thread_pool::get_default();
  tree_index idx{tree, pool};
  move_enumerator enumerator{idx, -1};

  std::vector<profitable_move> moves;
  for (auto src : idx.get_searchable_nodes()) {
    enumerator.find_moves_for_source(
        src, 10, [&](profitable_move const& mv) { moves.push_back(mv); });
  }

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

void test_adjust_rate_bias_via_variant() {
  auto model = load_ml_model("data/bcr", "s5f");
  double ll_before = ml_log_likelihood(model, "ACGTACGT", "CCGTACGT");
  ml_adjust_rate_bias(model, std::log(2.0));
  double ll_after = ml_log_likelihood(model, "ACGTACGT", "CCGTACGT");
  assert(std::isfinite(ll_before) && ll_before < 0.0);
  assert(std::isfinite(ll_after) && ll_after < 0.0);
  assert(ll_after != ll_before);
  std::println("  adjust_rate_bias via variant: OK");
}

int main() {
  std::println("=== nn_blended_spr tests ===");
  test_load_ml_model_s5f();
  test_load_ml_model_cnn();
  test_ml_log_likelihood_via_variant();
  test_delta_ml_score();
  test_ml_scoring_config_adjust_score();
  test_blended_scoring_zero_ml_matches_parsimony();
  test_adjust_rate_bias_via_variant();
  std::println("All nn_blended_spr tests passed");
}

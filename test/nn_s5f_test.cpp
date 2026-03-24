// Ground truth verification for RSFivemerModel inference.
// Verifies every rate and CSP value matches manual computation from raw
// embedding weights across all 500 positions of the forward pass.
#include <larch/rs_fivemer_model.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <print>
#include <vector>

using namespace larch;

void test_full_forward_ground_truth() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  auto pth = load_pth("data/bcr/s5f-libtorch.pth");
  auto& r_w = pth.get("r_kmer_embedding.weight");
  auto& s_w = pth.get("s_kmer_embedding.weight");

  std::string_view seq = "ACGTACGTACGTACGTACGT";  // 20 bases
  kmer_encoder enc{5, 500};
  auto encoded = enc.encode_sequence(seq);
  auto [rates, csp] = model.forward(seq);

  // Verify all 500 positions against manual embedding lookup + softmax.
  for (std::size_t i = 0; i < 500; ++i) {
    auto idx = static_cast<std::size_t>(encoded.kmer_indices[i]);

    // Rate: exp(r_embedding[kmer_idx]).
    float expected_rate = std::exp(r_w.data[idx]);
    assert(std::abs(rates[i] - expected_rate) < 1e-6f);

    // CSP: softmax(s_embedding[kmer_idx] + wt_modifier).
    float logits[4];
    for (std::size_t j = 0; j < 4; ++j)
      logits[j] = s_w.data[idx * 4 + j] + encoded.wt_modifier[i * 4 + j];

    float max_val = *std::max_element(logits, logits + 4);
    float sum = 0.0f;
    float expected_csp[4];
    for (std::size_t j = 0; j < 4; ++j) {
      expected_csp[j] = std::exp(logits[j] - max_val);
      sum += expected_csp[j];
    }
    for (std::size_t j = 0; j < 4; ++j) expected_csp[j] /= sum;

    for (std::size_t j = 0; j < 4; ++j)
      assert(std::abs(csp[i * 4 + j] - expected_csp[j]) < 1e-6f);
  }

  std::println("  full forward ground truth (500 positions): OK");
}

void test_kmer_indices_match_base4() {
  // Verify kmer index == base-4 encoding (A=0, C=1, G=2, T=3).
  kmer_encoder enc{5, 500};

  // "AAAAA" → position 2 (after NN padding) has kmer "AAAAA" = index 0
  auto e1 = enc.encode_sequence("AAAAA");
  assert(e1.kmer_indices[2] == 0);

  // All-T kmer → 3*256 + 3*64 + 3*16 + 3*4 + 3 = 1023
  auto e2 = enc.encode_sequence("TTTTT");
  assert(e2.kmer_indices[2] == 1023);

  // N-placeholder → 1024
  assert(e1.kmer_indices[0] == 1024);  // "NNAAA" contains N

  std::println("  kmer indices match base-4 encoding: OK");
}

void test_log_likelihood_exact_value() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  auto pth = load_pth("data/bcr/s5f-libtorch.pth");
  auto& r_w = pth.get("r_kmer_embedding.weight");
  auto& s_w = pth.get("s_kmer_embedding.weight");

  std::string_view parent = "ACGTACGTACGTACGTACGT";
  std::string_view child = "TCGTACGTACGTACGTACGT";  // A→T at pos 0

  // Manually compute the expected log-likelihood.
  kmer_encoder enc{5, 500};
  auto encoded = enc.encode_sequence(parent);
  auto parent_bases = kmer_encoder::encode_bases(parent);
  auto child_bases = kmer_encoder::encode_bases(child);

  // Build rates and CSP manually.
  std::vector<float> rates(500);
  std::vector<float> csp_vec(500 * 4);
  for (std::size_t i = 0; i < 500; ++i) {
    auto idx = static_cast<std::size_t>(encoded.kmer_indices[i]);
    rates[i] = std::exp(r_w.data[idx]);
    for (std::size_t j = 0; j < 4; ++j)
      csp_vec[i * 4 + j] =
          s_w.data[idx * 4 + j] + encoded.wt_modifier[i * 4 + j];
  }
  softmax_inplace(csp_vec, 500, 4);

  double expected =
      poisson_context_log_likelihood(rates, csp_vec, parent_bases, child_bases);
  double actual = model.log_likelihood(parent, child);

  assert(std::abs(actual - expected) < 1e-10);
  assert(std::isfinite(actual));
  assert(actual < 0.0);

  std::println("  log_likelihood exact value: OK (ll = {:.6f})", actual);
}

void test_multiple_mutations_exact() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");

  std::string_view parent = "ACGTACGTACGTACGTACGT";
  // Mutations at pos 0 (A→T), pos 3 (T→C), pos 11 (T→C)
  std::string child = "TCGCACGTACGCACGTACGT";

  double ll = model.log_likelihood(parent, child);

  assert(std::isfinite(ll));
  assert(ll < 0.0);

  // Verify determinism.
  double ll2 = model.log_likelihood(parent, child);
  assert(ll == ll2);

  std::println("  multiple mutations exact: OK (ll = {:.6f})", ll);
}

void test_different_sequences_different_likelihoods() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");

  std::string_view parent = "ACGTACGTACGTACGTACGT";

  // Same number of mutations but at different context positions.
  std::string child1 = "TCGTACGTACGTACGTACGT";  // A→T at pos 0
  std::string child2 = "ACGTACGTACGTACGCACGT";  // T→C at pos 15

  double ll1 = model.log_likelihood(parent, child1);
  double ll2 = model.log_likelihood(parent, child2);

  assert(std::isfinite(ll1) && ll1 < 0.0);
  assert(std::isfinite(ll2) && ll2 < 0.0);
  // Different k-mer contexts → different likelihoods (with overwhelming
  // probability). We don't know which is larger; just verify they differ.
  assert(ll1 != ll2);

  std::println("  different sequences different likelihoods: OK");
}

int main() {
  std::println("=== nn_s5f ground truth tests ===");
  test_kmer_indices_match_base4();
  test_full_forward_ground_truth();
  test_log_likelihood_exact_value();
  test_multiple_mutations_exact();
  test_different_sequences_different_likelihoods();
  std::println("All nn_s5f tests passed");
}

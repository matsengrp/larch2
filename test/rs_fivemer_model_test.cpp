#include <larch/rs_fivemer_model.hpp>

#include <cassert>
#include <cmath>
#include <print>

using namespace larch;

void test_load_s5f() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  assert(model.kmer_length() == 5);
  assert(model.site_count() == 500);
  assert(model.kmer_count() == 1025);  // 4^5 + 1
  std::println("  load s5f: OK");
}

void test_wrong_model_class_throws() {
  bool threw = false;
  try {
    rs_fivemer_model::load("data/bcr", "ThriftyHumV0.2-45");
  } catch (std::runtime_error const&) {
    threw = true;
  }
  assert(threw);
  std::println("  wrong model class throws: OK");
}

void test_forward_rates() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");

  // "AAAAA": position 2 maps to kmer "AAAAA" (index 0),
  // all other positions have N-containing kmers (index 1024).
  auto [rates, csp] = model.forward("AAAAA");

  assert(rates.size() == 500);
  assert(csp.size() == 2000);

  for (float r : rates) {
    assert(std::isfinite(r));
    assert(r > 0.0f);
  }

  // Positions 0,1,3,4 all map to N-placeholder — same rate.
  assert(std::abs(rates[0] - rates[1]) < 1e-10f);
  assert(std::abs(rates[0] - rates[3]) < 1e-10f);
  assert(std::abs(rates[0] - rates[4]) < 1e-10f);
  // Positions beyond the sequence also map to N-placeholder.
  assert(std::abs(rates[0] - rates[100]) < 1e-10f);

  std::println("  forward rates: OK");
}

void test_forward_rates_match_weights() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  auto pth = load_pth("data/bcr/s5f-libtorch.pth");
  auto& r = pth.get("r_kmer_embedding.weight");

  auto [rates, csp] = model.forward("AAAAA");

  // Position 2: kmer "AAAAA" = index 0.
  float expected_rate_0 = std::exp(r.data[0]);
  assert(std::abs(rates[2] - expected_rate_0) < 1e-6f);

  // Position 0: N-placeholder = index 1024.
  float expected_rate_n = std::exp(r.data[1024]);
  assert(std::abs(rates[0] - expected_rate_n) < 1e-6f);

  std::println("  forward rates match weights: OK");
}

void test_forward_csp_sums_to_one() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  auto [rates, csp] = model.forward("ACGTACGT");

  for (std::size_t i = 0; i < model.site_count(); ++i) {
    float sum = 0.0f;
    for (std::size_t j = 0; j < 4; ++j) {
      float v = csp[i * 4 + j];
      assert(v >= 0.0f && v <= 1.0f);
      sum += v;
    }
    assert(std::abs(sum - 1.0f) < 1e-5f);
  }

  std::println("  forward csp sums to one: OK");
}

void test_forward_parent_base_masked() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  auto [rates, csp] = model.forward("ACGT");

  float epsilon = 1e-6f;
  assert(csp[0 * 4 + 0] < epsilon);  // A masked at position 0
  assert(csp[1 * 4 + 1] < epsilon);  // C masked at position 1
  assert(csp[2 * 4 + 2] < epsilon);  // G masked at position 2
  assert(csp[3 * 4 + 3] < epsilon);  // T masked at position 3

  std::println("  forward parent base masked: OK");
}

void test_forward_csp_matches_manual() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  auto pth = load_pth("data/bcr/s5f-libtorch.pth");
  auto& s = pth.get("s_kmer_embedding.weight");

  // "AAAAA": position 2 has kmer "AAAAA" (index 0), parent base A (index 0).
  auto [rates, csp] = model.forward("AAAAA");

  // Manually compute softmax of s_weights[0*4..0*4+3] + wt_modifier.
  // wt_modifier: [-1e9, 0, 0, 0] at position 2 (parent A is masked).
  float logits[4];
  for (int j = 0; j < 4; ++j) logits[j] = s.data[0 * 4 + j];
  logits[0] += -1e9f;  // mask parent base

  float max_val = *std::max_element(logits, logits + 4);
  float sum = 0.0f;
  float expected[4];
  for (int j = 0; j < 4; ++j) {
    expected[j] = std::exp(logits[j] - max_val);
    sum += expected[j];
  }
  for (int j = 0; j < 4; ++j) expected[j] /= sum;

  for (int j = 0; j < 4; ++j)
    assert(std::abs(csp[2 * 4 + j] - expected[j]) < 1e-6f);

  std::println("  forward csp matches manual: OK");
}

void test_log_likelihood_no_mutations() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  double ll = model.log_likelihood("ACGTACGT", "ACGTACGT");
  assert(ll == 0.0);
  std::println("  log_likelihood no mutations: OK");
}

void test_log_likelihood_with_mutations() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  double ll = model.log_likelihood("AAAAAAA", "AACAAAA");
  assert(std::isfinite(ll));
  assert(ll < 0.0);
  std::println("  log_likelihood with mutations: OK");
}

void test_log_likelihood_matches_manual() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");

  std::string_view parent = "ACGT";
  std::string_view child = "CCGT";  // A->C at position 0

  auto [rates, csp] = model.forward(parent);
  auto parent_bases = kmer_encoder::encode_bases(parent);
  auto child_bases = kmer_encoder::encode_bases(child);

  double expected =
      poisson_context_log_likelihood(rates, csp, parent_bases, child_bases);
  double actual = model.log_likelihood(parent, child);

  assert(std::abs(actual - expected) < 1e-10);
  std::println("  log_likelihood matches manual: OK");
}

void test_log_likelihood_more_mutations_more_negative() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");

  std::string parent(25, 'A');
  std::string child1 = parent;
  child1[2] = 'C';  // 1 mutation
  std::string child2 = child1;
  child2[5] = 'C';  // 2 mutations

  double ll1 = model.log_likelihood(parent, child1);
  double ll2 = model.log_likelihood(parent, child2);

  assert(std::isfinite(ll1) && ll1 < 0.0);
  assert(std::isfinite(ll2) && ll2 < 0.0);
  assert(ll2 < ll1);

  std::println("  more mutations more negative: OK");
}

void test_different_contexts_different_rates() {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");

  // Two sequences that differ in k-mer context around a central position.
  std::string seq1 = "AAAAACAAAA";  // center kmer at pos 4: "AACAA"
  std::string seq2 = "GGGGGCGGGG";  // center kmer at pos 4: "GGCGG"

  auto [rates1, csp1] = model.forward(seq1);
  auto [rates2, csp2] = model.forward(seq2);

  assert(rates1[4] > 0.0f && std::isfinite(rates1[4]));
  assert(rates2[4] > 0.0f && std::isfinite(rates2[4]));

  std::println("  different contexts different rates: OK");
}

int main() {
  std::println("=== rs_fivemer_model tests ===");
  test_load_s5f();
  test_wrong_model_class_throws();
  test_forward_rates();
  test_forward_rates_match_weights();
  test_forward_csp_sums_to_one();
  test_forward_parent_base_masked();
  test_forward_csp_matches_manual();
  test_log_likelihood_no_mutations();
  test_log_likelihood_with_mutations();
  test_log_likelihood_matches_manual();
  test_log_likelihood_more_mutations_more_negative();
  test_different_contexts_different_rates();
  std::println("All rs_fivemer_model tests passed");
}

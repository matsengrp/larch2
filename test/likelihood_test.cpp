// Tests for poisson_context_log_likelihood
// Ported from larch-rm-optimize test_netam_likelihood.cpp
#include <larch/likelihood.hpp>

#include <cassert>
#include <cmath>
#include <print>
#include <vector>

using namespace larch;

// Helper: manually compute log-likelihood for verification.
double manual_log_likelihood(std::span<const float> rates,
                             std::span<const float> csp,  // [N*4]
                             std::span<const int64_t> parent,
                             std::span<const int64_t> child) {
  std::vector<std::size_t> mut_indices;
  for (std::size_t i = 0; i < parent.size(); ++i) {
    if (parent[i] != child[i]) mut_indices.push_back(i);
  }

  auto n = static_cast<int64_t>(mut_indices.size());
  if (n == 0) return 0.0;

  double sum_rates = 0.0;
  for (std::size_t i = 0; i < parent.size(); ++i)
    sum_rates += static_cast<double>(rates[i]);

  double t_hat = static_cast<double>(n) / sum_rates;

  double log_lambda_sum = 0.0;
  for (auto idx : mut_indices) {
    double rate = static_cast<double>(rates[idx]);
    auto child_base = static_cast<std::size_t>(child[idx]);
    double sub_prob = static_cast<double>(csp[idx * 4 + child_base]);
    log_lambda_sum += std::log(rate * sub_prob);
  }

  return log_lambda_sum + std::log(t_hat) * static_cast<double>(n) -
         static_cast<double>(n);
}

void test_no_mutations_returns_zero() {
  std::vector<float> rates{1, 1, 1, 1, 1};
  std::vector<float> csp(5 * 4, 0.25f);
  std::vector<int64_t> parent{0, 1, 2, 3, 0};
  std::vector<int64_t> child{0, 1, 2, 3, 0};  // identical

  double result = poisson_context_log_likelihood(rates, csp, parent, child);
  assert(result == 0.0);
}

void test_single_mutation() {
  std::vector<float> rates{1.0f, 1.0f, 2.0f, 1.0f, 1.0f};
  std::vector<float> csp(5 * 4, 0.25f);

  std::vector<int64_t> parent{0, 1, 2, 3, 0};  // A C G T A
  std::vector<int64_t> child{0, 1, 3, 3, 0};   // A C T T A (G->T at pos 2)

  double result = poisson_context_log_likelihood(rates, csp, parent, child);

  // n=1, sum_rates=6, t_hat=1/6
  // rate=2.0, csp[2][3]=0.25
  // log_lik = log(2.0*0.25) + log(1/6) - 1
  double expected = std::log(0.5) + std::log(1.0 / 6.0) - 1.0;
  assert(std::abs(result - expected) < 1e-5);
}

void test_multiple_mutations() {
  std::vector<float> rates{1.0f, 2.0f, 3.0f, 4.0f};
  std::vector<float> csp = {
      0.1f,  0.2f,  0.3f,  0.4f,   // pos 0
      0.4f,  0.3f,  0.2f,  0.1f,   // pos 1
      0.25f, 0.25f, 0.25f, 0.25f,  // pos 2
      0.1f,  0.1f,  0.1f,  0.7f,   // pos 3
  };
  std::vector<int64_t> parent{0, 1, 2, 3};  // A C G T
  std::vector<int64_t> child{1, 1, 0, 3};   // C C A T (mutations at 0,2)

  double result = poisson_context_log_likelihood(rates, csp, parent, child);
  double expected = manual_log_likelihood(rates, csp, parent, child);
  assert(std::abs(result - expected) < 1e-5);
}

void test_all_positions_mutated() {
  std::vector<float> rates{1.0f, 1.0f, 1.0f};
  std::vector<float> csp = {
      0.0f, 0.5f, 0.3f, 0.2f,  // parent A
      0.3f, 0.0f, 0.4f, 0.3f,  // parent C
      0.2f, 0.3f, 0.0f, 0.5f,  // parent G
  };
  std::vector<int64_t> parent{0, 1, 2};  // A C G
  std::vector<int64_t> child{1, 2, 3};   // C G T

  double result = poisson_context_log_likelihood(rates, csp, parent, child);
  double expected = manual_log_likelihood(rates, csp, parent, child);
  assert(std::abs(result - expected) < 1e-5);
}

void test_single_position_sequence() {
  std::vector<float> rates{2.0f};
  std::vector<float> csp{0.0f, 0.5f, 0.3f, 0.2f};
  std::vector<int64_t> parent{0};  // A
  std::vector<int64_t> child{1};   // C

  double result = poisson_context_log_likelihood(rates, csp, parent, child);

  // n=1, sum_rates=2, t_hat=0.5
  // log_lik = log(2.0*0.5) + log(0.5) - 1 = 0 + log(0.5) - 1
  double expected = std::log(1.0) + std::log(0.5) - 1.0;
  assert(std::abs(result - expected) < 1e-5);
}

void test_high_csp_gives_higher_likelihood() {
  std::vector<float> rates{1.0f, 1.0f, 1.0f};
  std::vector<int64_t> parent{0, 1, 2};
  std::vector<int64_t> child{1, 1, 2};  // mutation at pos 0: A->C

  // Low CSP for A->C
  std::vector<float> csp_low(3 * 4, 0.25f);
  csp_low[0 * 4 + 1] = 0.1f;

  // High CSP for A->C
  std::vector<float> csp_high(3 * 4, 0.25f);
  csp_high[0 * 4 + 1] = 0.9f;

  double ll_low = poisson_context_log_likelihood(rates, csp_low, parent, child);
  double ll_high =
      poisson_context_log_likelihood(rates, csp_high, parent, child);

  assert(ll_high > ll_low);
}

void test_high_rate_gives_higher_likelihood() {
  std::vector<float> csp(3 * 4, 0.25f);
  std::vector<int64_t> parent{0, 1, 2};
  std::vector<int64_t> child{1, 1, 2};  // mutation at pos 0

  std::vector<float> rates_low{0.1f, 1.0f, 1.0f};
  std::vector<float> rates_high{10.0f, 1.0f, 1.0f};

  double ll_low = poisson_context_log_likelihood(rates_low, csp, parent, child);
  double ll_high =
      poisson_context_log_likelihood(rates_high, csp, parent, child);

  assert(ll_high > ll_low);
}

void test_symmetry_of_mutation_count() {
  std::vector<float> rates{1.0f, 1.0f, 1.0f, 1.0f};
  std::vector<float> csp(4 * 4, 0.25f);
  std::vector<int64_t> parent{0, 1, 2, 3};

  std::vector<int64_t> child1{1, 1, 2, 3};  // mutation at 0
  std::vector<int64_t> child2{0, 2, 2, 3};  // mutation at 1

  double r1 = poisson_context_log_likelihood(rates, csp, parent, child1);
  double r2 = poisson_context_log_likelihood(rates, csp, parent, child2);

  // Uniform rates + CSPs -> same LL regardless of which position mutates.
  assert(std::abs(r1 - r2) < 1e-5);
}

void test_longer_sequence() {
  const int len = 100;
  std::vector<float> rates(len);
  std::vector<float> csp(len * 4);
  for (int i = 0; i < len; ++i) {
    rates[static_cast<std::size_t>(i)] =
        0.5f + static_cast<float>(i % 10) * 0.1f;
    float sum = 0.0f;
    for (int j = 0; j < 4; ++j) {
      float v = 0.1f + static_cast<float>((i + j) % 5) * 0.1f;
      csp[static_cast<std::size_t>(i * 4 + j)] = v;
      sum += v;
    }
    for (int j = 0; j < 4; ++j) csp[static_cast<std::size_t>(i * 4 + j)] /= sum;
  }

  std::vector<int64_t> parent(len);
  std::vector<int64_t> child(len);
  for (int i = 0; i < len; ++i) {
    parent[static_cast<std::size_t>(i)] = i % 4;
    child[static_cast<std::size_t>(i)] = i % 4;
  }
  // Introduce mutations.
  child[10] = (parent[10] + 1) % 4;
  child[50] = (parent[50] + 2) % 4;
  child[90] = (parent[90] + 3) % 4;

  double result = poisson_context_log_likelihood(rates, csp, parent, child);
  assert(std::isfinite(result));
}

void test_formula_components() {
  std::vector<float> rates{1.0f, 2.0f, 3.0f, 4.0f};
  std::vector<float> csp(4 * 4, 0.25f);

  std::vector<int64_t> parent{0, 1, 2, 3};
  std::vector<int64_t> child{1, 0, 2, 3};  // 2 mutations at pos 0,1

  double result = poisson_context_log_likelihood(rates, csp, parent, child);

  // n=2, sum_rates=10, t_hat=0.2
  // lambda_0 = 1.0*0.25 = 0.25, lambda_1 = 2.0*0.25 = 0.5
  // log_lik = log(0.25) + log(0.5) + 2*log(0.2) - 2
  double expected = std::log(0.25) + std::log(0.5) + 2.0 * std::log(0.2) - 2.0;
  assert(std::abs(result - expected) < 1e-5);
}

// ---- softmax tests ----

void test_softmax_basic() {
  std::vector<float> logits = {1.0f, 2.0f, 3.0f, 4.0f};
  softmax_inplace(logits, 1, 4);

  float sum = 0.0f;
  for (float v : logits) {
    assert(v > 0.0f && v < 1.0f);
    sum += v;
  }
  assert(std::abs(sum - 1.0f) < 1e-5f);

  // Values should be monotonically increasing.
  assert(logits[0] < logits[1]);
  assert(logits[1] < logits[2]);
  assert(logits[2] < logits[3]);
}

void test_softmax_multiple_rows() {
  std::vector<float> logits = {0, 0, 0, 0, 1, 2, 3, 4};
  softmax_inplace(logits, 2, 4);

  // First row: uniform -> each 0.25
  for (int i = 0; i < 4; ++i)
    assert(std::abs(logits[static_cast<std::size_t>(i)] - 0.25f) < 1e-5f);

  // Second row: sum to 1
  float sum = 0;
  for (int i = 4; i < 8; ++i) sum += logits[static_cast<std::size_t>(i)];
  assert(std::abs(sum - 1.0f) < 1e-5f);
}

int main() {
  std::println("=== likelihood tests ===");
  test_no_mutations_returns_zero();
  test_single_mutation();
  test_multiple_mutations();
  test_all_positions_mutated();
  test_single_position_sequence();
  test_high_csp_gives_higher_likelihood();
  test_high_rate_gives_higher_likelihood();
  test_symmetry_of_mutation_count();
  test_longer_sequence();
  test_formula_components();

  std::println("=== softmax tests ===");
  test_softmax_basic();
  test_softmax_multiple_rows();

  std::println("All likelihood tests passed");
}

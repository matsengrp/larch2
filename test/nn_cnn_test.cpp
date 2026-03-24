#include <larch/indep_rs_cnn_model.hpp>

#include <cassert>
#include <cmath>
#include <print>

using namespace larch;

void test_load_cnn() {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  assert(model.kmer_length() == 3);
  assert(model.site_count() == 500);
  assert(model.kmer_count() == 65);  // 4^3 + 1
  assert(model.embedding_dim() == 7);
  assert(model.filter_count() == 16);
  assert(model.kernel_size() == 9);
  std::println("  load cnn: OK");
}

void test_wrong_model_class_throws() {
  bool threw = false;
  try {
    indep_rs_cnn_model::load("data/bcr", "s5f");
  } catch (std::runtime_error const&) {
    threw = true;
  }
  assert(threw);
  std::println("  wrong model class throws: OK");
}

void test_tensor_shapes() {
  auto pth = load_pth("data/bcr/ThriftyHumV0.2-45-libtorch.pth");
  assert(pth.tensors.size() == 15);

  auto check = [&](std::string_view name, std::vector<int64_t> expected) {
    auto& t = pth.get(name);
    assert(t.shape == expected);
  };

  check("kmer_embedding.weight", {65, 7});
  check("conv.weight", {16, 7, 9});
  check("conv.bias", {16});
  check("linear.weight", {1, 16});
  check("linear.bias", {1});

  check("r_kmer_embedding.weight", {65, 7});
  check("r_conv.weight", {16, 7, 9});
  check("r_conv.bias", {16});
  check("r_linear.weight", {1, 16});
  check("r_linear.bias", {1});

  check("s_kmer_embedding.weight", {65, 7});
  check("s_conv.weight", {16, 7, 9});
  check("s_conv.bias", {16});
  check("s_linear.weight", {4, 16});
  check("s_linear.bias", {4});

  std::println("  tensor shapes: OK");
}

void test_forward_output_shapes() {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  auto [rates, csp] = model.forward("ACGTACGT");
  assert(rates.size() == 500);
  assert(csp.size() == 2000);
  std::println("  forward output shapes: OK");
}

void test_forward_rates_positive_finite() {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  auto [rates, csp] = model.forward("ACGTACGTACGTACGTACGT");
  for (float r : rates) {
    assert(std::isfinite(r));
    assert(r > 0.0f);
  }
  std::println("  forward rates positive finite: OK");
}

void test_forward_csp_sums_to_one() {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
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
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  auto [rates, csp] = model.forward("ACGT");

  float epsilon = 1e-6f;
  assert(csp[0 * 4 + 0] < epsilon);  // A masked at position 0
  assert(csp[1 * 4 + 1] < epsilon);  // C masked at position 1
  assert(csp[2 * 4 + 2] < epsilon);  // G masked at position 2
  assert(csp[3 * 4 + 3] < epsilon);  // T masked at position 3
  std::println("  forward parent base masked: OK");
}

void test_forward_deterministic() {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  std::string_view seq = "ACGTACGTACGTACGT";

  auto [rates1, csp1] = model.forward(seq);
  auto [rates2, csp2] = model.forward(seq);

  assert(rates1 == rates2);
  assert(csp1 == csp2);
  std::println("  forward deterministic: OK");
}

void test_log_likelihood_no_mutations() {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  double ll = model.log_likelihood("ACGTACGT", "ACGTACGT");
  assert(ll == 0.0);
  std::println("  log_likelihood no mutations: OK");
}

void test_log_likelihood_with_mutations() {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  double ll = model.log_likelihood("AAAAAAA", "AACAAAA");
  assert(std::isfinite(ll));
  assert(ll < 0.0);
  std::println("  log_likelihood with mutations: OK");
}

int main() {
  std::println("=== nn_cnn_model tests ===");
  test_load_cnn();
  test_wrong_model_class_throws();
  test_tensor_shapes();
  test_forward_output_shapes();
  test_forward_rates_positive_finite();
  test_forward_csp_sums_to_one();
  test_forward_parent_base_masked();
  test_forward_deterministic();
  test_log_likelihood_no_mutations();
  test_log_likelihood_with_mutations();
  std::println("All nn_cnn_model tests passed");
}

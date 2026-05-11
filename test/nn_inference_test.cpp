// GPU vs CPU comparison tests for nn_inference.
#include <larch/indep_rs_cnn_model.hpp>
#include <larch/nn_inference.hpp>
#include <larch/rs_fivemer_model.hpp>
#include <larch/vulkan_compute.hpp>

#include <cassert>
#include <cmath>
#include <exception>
#include <memory>
#include <print>
#include <stdexcept>
#include <utility>

using namespace larch;

static constexpr float ATOL = 1e-4f;  // absolute tolerance for float comparison

static void check_close(float a, float b, float tol, const char* label,
                        std::size_t i) {
  float diff = std::abs(a - b);
  if (diff > tol) {
    std::println("  MISMATCH {}: index {} gpu={} cpu={} diff={}", label, i, a,
                 b, diff);
    assert(false);
  }
}

template <typename Fn>
static void expect_logic_error(Fn&& fn) {
  bool threw = false;
  try {
    std::forward<Fn>(fn)();
  } catch (std::logic_error const&) {
    threw = true;
  }
  assert(threw);
}

// --- Fivemer tests ---

void test_fivemer_gpu_vs_cpu(vk_context& ctx) {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  nn_inference gpu{ctx, model};

  std::string_view seq = "ACGTACGTACGTACGTACGT";
  auto cpu_result = model.forward(seq);
  auto gpu_result = gpu.forward(seq);

  assert(cpu_result.rates.size() == gpu_result.rates.size());
  assert(cpu_result.csp.size() == gpu_result.csp.size());

  for (std::size_t i = 0; i < cpu_result.rates.size(); ++i)
    check_close(gpu_result.rates[i], cpu_result.rates[i], ATOL, "fivemer rate",
                i);

  for (std::size_t i = 0; i < cpu_result.csp.size(); ++i)
    check_close(gpu_result.csp[i], cpu_result.csp[i], ATOL, "fivemer csp", i);

  std::println("  fivemer GPU vs CPU: OK");
}

void test_fivemer_log_likelihood_gpu_vs_cpu(vk_context& ctx) {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  nn_inference gpu{ctx, model};

  std::string_view parent = "ACGTACGTACGTACGTACGT";
  std::string_view child = "TCGTACGTACGTACGTACGT";  // A→T at pos 0

  double cpu_ll = model.log_likelihood(parent, child);
  double gpu_ll = gpu.log_likelihood(parent, child);

  assert(std::isfinite(cpu_ll) && cpu_ll < 0.0);
  assert(std::isfinite(gpu_ll) && gpu_ll < 0.0);
  assert(std::abs(cpu_ll - gpu_ll) < 1e-3);

  std::println(
      "  fivemer log_likelihood GPU vs CPU: OK (cpu={:.6f} gpu={:.6f})", cpu_ll,
      gpu_ll);
}

void test_fivemer_gpu_deterministic(vk_context& ctx) {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  nn_inference gpu{ctx, model};

  std::string_view seq = "ACGTACGTACGTACGTACGT";
  auto r1 = gpu.forward(seq);
  auto r2 = gpu.forward(seq);

  assert(r1.rates == r2.rates);
  assert(r1.csp == r2.csp);
  std::println("  fivemer GPU deterministic: OK");
}

void test_fivemer_gpu_output_shapes(vk_context& ctx) {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  nn_inference gpu{ctx, model};

  auto result = gpu.forward("ACGT");
  assert(result.rates.size() == 500);
  assert(result.csp.size() == 2000);
  assert(gpu.site_count() == 500);
  std::println("  fivemer GPU output shapes: OK");
}

void test_nn_inference_moved_from_throws(vk_context& ctx) {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  nn_inference gpu{ctx, model};
  nn_inference moved{std::move(gpu)};
  assert(moved.site_count() == model.site_count());

  expect_logic_error([&] { (void)gpu.site_count(); });
  expect_logic_error([&] { (void)gpu.forward("ACGT"); });
  expect_logic_error([&] { (void)gpu.log_likelihood("ACGT", "TCGT"); });
  std::println("  moved-from nn_inference throws: OK");
}

void test_fivemer_gpu_rates_positive(vk_context& ctx) {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  nn_inference gpu{ctx, model};

  auto result = gpu.forward("ACGTACGTACGTACGTACGT");
  for (float r : result.rates) {
    assert(std::isfinite(r));
    assert(r > 0.0f);
  }
  std::println("  fivemer GPU rates positive: OK");
}

void test_fivemer_gpu_csp_sums_to_one(vk_context& ctx) {
  auto model = rs_fivemer_model::load("data/bcr", "s5f");
  nn_inference gpu{ctx, model};

  auto result = gpu.forward("ACGTACGT");
  for (std::size_t i = 0; i < gpu.site_count(); ++i) {
    float sum = 0.0f;
    for (std::size_t j = 0; j < 4; ++j) {
      float v = result.csp[i * 4 + j];
      assert(v >= 0.0f && v <= 1.0f);
      sum += v;
    }
    assert(std::abs(sum - 1.0f) < 1e-5f);
  }
  std::println("  fivemer GPU csp sums to one: OK");
}

// --- CNN tests ---

void test_cnn_gpu_vs_cpu(vk_context& ctx) {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  nn_inference gpu{ctx, model};

  std::string_view seq = "ACGTACGTACGTACGTACGT";
  auto cpu_result = model.forward(seq);
  auto gpu_result = gpu.forward(seq);

  assert(cpu_result.rates.size() == gpu_result.rates.size());
  assert(cpu_result.csp.size() == gpu_result.csp.size());

  for (std::size_t i = 0; i < cpu_result.rates.size(); ++i)
    check_close(gpu_result.rates[i], cpu_result.rates[i], ATOL, "cnn rate", i);

  for (std::size_t i = 0; i < cpu_result.csp.size(); ++i)
    check_close(gpu_result.csp[i], cpu_result.csp[i], ATOL, "cnn csp", i);

  std::println("  cnn GPU vs CPU: OK");
}

void test_cnn_log_likelihood_gpu_vs_cpu(vk_context& ctx) {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  nn_inference gpu{ctx, model};

  std::string_view parent = "AAAAAAAAAAAAAAAAAAAAAAAAA";
  std::string_view child = "AACAAAAAAAAAAAAAAAAAAAAAA";

  double cpu_ll = model.log_likelihood(parent, child);
  double gpu_ll = gpu.log_likelihood(parent, child);

  assert(std::isfinite(cpu_ll) && cpu_ll < 0.0);
  assert(std::isfinite(gpu_ll) && gpu_ll < 0.0);
  assert(std::abs(cpu_ll - gpu_ll) < 1e-3);

  std::println("  cnn log_likelihood GPU vs CPU: OK (cpu={:.6f} gpu={:.6f})",
               cpu_ll, gpu_ll);
}

void test_cnn_gpu_deterministic(vk_context& ctx) {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  nn_inference gpu{ctx, model};

  std::string_view seq = "ACGTACGTACGTACGT";
  auto r1 = gpu.forward(seq);
  auto r2 = gpu.forward(seq);

  assert(r1.rates == r2.rates);
  assert(r1.csp == r2.csp);
  std::println("  cnn GPU deterministic: OK");
}

void test_cnn_gpu_output_shapes(vk_context& ctx) {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  nn_inference gpu{ctx, model};

  auto result = gpu.forward("ACGT");
  assert(result.rates.size() == 500);
  assert(result.csp.size() == 2000);
  assert(gpu.site_count() == 500);
  std::println("  cnn GPU output shapes: OK");
}

void test_cnn_gpu_rates_positive(vk_context& ctx) {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  nn_inference gpu{ctx, model};

  auto result = gpu.forward("ACGTACGTACGTACGTACGT");
  for (float r : result.rates) {
    assert(std::isfinite(r));
    assert(r > 0.0f);
  }
  std::println("  cnn GPU rates positive: OK");
}

void test_cnn_gpu_csp_sums_to_one(vk_context& ctx) {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  nn_inference gpu{ctx, model};

  auto result = gpu.forward("ACGTACGT");
  for (std::size_t i = 0; i < gpu.site_count(); ++i) {
    float sum = 0.0f;
    for (std::size_t j = 0; j < 4; ++j) {
      float v = result.csp[i * 4 + j];
      assert(v >= 0.0f && v <= 1.0f);
      sum += v;
    }
    assert(std::abs(sum - 1.0f) < 1e-5f);
  }
  std::println("  cnn GPU csp sums to one: OK");
}

void test_cnn_gpu_parent_base_masked(vk_context& ctx) {
  auto model = indep_rs_cnn_model::load("data/bcr", "ThriftyHumV0.2-45");
  nn_inference gpu{ctx, model};

  auto result = gpu.forward("ACGT");
  float epsilon = 1e-6f;
  assert(result.csp[0 * 4 + 0] < epsilon);  // A masked at position 0
  assert(result.csp[1 * 4 + 1] < epsilon);  // C masked at position 1
  assert(result.csp[2 * 4 + 2] < epsilon);  // G masked at position 2
  assert(result.csp[3 * 4 + 3] < epsilon);  // T masked at position 3
  std::println("  cnn GPU parent base masked: OK");
}

int main() {
  std::println("=== nn_inference tests (GPU) ===");

  std::unique_ptr<vk_context> ctx;
  try {
    ctx = std::make_unique<vk_context>();
  } catch (std::exception const& e) {
    std::println("SKIP: Vulkan context unavailable: {}", e.what());
    return 77;
  }

  std::println("--- Fivemer ---");
  test_fivemer_gpu_output_shapes(*ctx);
  test_fivemer_gpu_rates_positive(*ctx);
  test_fivemer_gpu_csp_sums_to_one(*ctx);
  test_fivemer_gpu_vs_cpu(*ctx);
  test_fivemer_log_likelihood_gpu_vs_cpu(*ctx);
  test_fivemer_gpu_deterministic(*ctx);
  test_nn_inference_moved_from_throws(*ctx);

  std::println("--- CNN ---");
  test_cnn_gpu_output_shapes(*ctx);
  test_cnn_gpu_rates_positive(*ctx);
  test_cnn_gpu_csp_sums_to_one(*ctx);
  test_cnn_gpu_parent_base_masked(*ctx);
  test_cnn_gpu_vs_cpu(*ctx);
  test_cnn_log_likelihood_gpu_vs_cpu(*ctx);
  test_cnn_gpu_deterministic(*ctx);

  std::println("All nn_inference tests passed");
}

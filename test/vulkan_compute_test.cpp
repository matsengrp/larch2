// Smoke test for Vulkan compute infrastructure.
#include <larch/vulkan_compute.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <print>
#include <span>
#include <vector>

// Embedded SPIR-V bytecode — generated at build time.
#include <larch/double_it_spv.hpp>

using namespace larch;

void test_context_creation() {
  vk_context ctx;
  auto name = ctx.device_name();
  assert(!name.empty());
  std::println("  device: {}", name);
}

void test_buffer_upload_download() {
  vk_context ctx;

  std::vector<float> data{1.0f, 2.0f, 3.0f, 4.0f};
  auto buf = vk_buffer::create(ctx, data.size() * sizeof(float),
                               vk_buffer::usage::storage_read_write);
  assert(buf.size_bytes() == 16);

  buf.upload_typed(std::span{data});

  std::vector<float> out(4);
  buf.download_typed(std::span{out});

  for (int i = 0; i < 4; ++i)
    assert(out[static_cast<std::size_t>(i)] ==
           data[static_cast<std::size_t>(i)]);
  std::println("  buffer round-trip: OK");
}

void test_double_it_shader() {
  vk_context ctx;

  constexpr std::size_t N = 256;
  std::vector<float> input(N);
  for (std::size_t i = 0; i < N; ++i) input[i] = static_cast<float>(i);

  auto in_buf =
      vk_buffer::create(ctx, N * sizeof(float), vk_buffer::usage::storage_read);
  auto out_buf = vk_buffer::create(ctx, N * sizeof(float),
                                   vk_buffer::usage::storage_write);

  in_buf.upload_typed(std::span{input});

  // Create pipeline with push constants (uint count).
  auto pipeline =
      vk_pipeline::create(ctx, shaders::double_it, 2, sizeof(uint32_t));

  uint32_t count = static_cast<uint32_t>(N);
  auto pc =
      std::span{reinterpret_cast<const std::byte*>(&count), sizeof(count)};

  uint32_t workgroups = (count + 63) / 64;
  pipeline.dispatch({&in_buf, &out_buf}, pc, {workgroups, 1, 1});

  std::vector<float> output(N);
  out_buf.download_typed(std::span{output});

  for (std::size_t i = 0; i < N; ++i) {
    float expected = static_cast<float>(i) * 2.0f;
    assert(std::abs(output[i] - expected) < 1e-6f);
  }
  std::println("  double_it shader (N={}): OK", N);
}

void test_large_dispatch() {
  vk_context ctx;

  constexpr std::size_t N = 10000;
  std::vector<float> input(N);
  for (std::size_t i = 0; i < N; ++i) input[i] = static_cast<float>(i) * 0.1f;

  auto in_buf =
      vk_buffer::create(ctx, N * sizeof(float), vk_buffer::usage::storage_read);
  auto out_buf = vk_buffer::create(ctx, N * sizeof(float),
                                   vk_buffer::usage::storage_write);

  in_buf.upload_typed(std::span{input});

  auto pipeline =
      vk_pipeline::create(ctx, shaders::double_it, 2, sizeof(uint32_t));

  uint32_t count = static_cast<uint32_t>(N);
  auto pc =
      std::span{reinterpret_cast<const std::byte*>(&count), sizeof(count)};

  pipeline.dispatch({&in_buf, &out_buf}, pc, {(count + 63) / 64, 1, 1});

  std::vector<float> output(N);
  out_buf.download_typed(std::span{output});

  for (std::size_t i = 0; i < N; ++i) {
    float expected = input[i] * 2.0f;
    assert(std::abs(output[i] - expected) < 1e-4f);
  }
  std::println("  large dispatch (N={}): OK", N);
}

int main() {
  std::println("=== Vulkan compute tests ===");
  test_context_creation();
  test_buffer_upload_download();
  test_double_it_shader();
  test_large_dispatch();
  std::println("All vulkan_compute tests passed");
}

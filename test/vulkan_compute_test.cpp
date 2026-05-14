// Smoke test for Vulkan compute infrastructure.
#include <larch/vulkan_compute.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <print>
#include <span>
#include <stdexcept>
#include <vector>

// Embedded SPIR-V bytecode — generated at build time.
#include <larch/test_double_it_spv.hpp>

using namespace larch;

void test_context_creation(vk_context& ctx) {
  auto name = ctx.device_name();
  assert(!name.empty());
  std::println("  device: {}", name);
}

void test_buffer_upload_download(vk_context& ctx) {
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

void test_buffer_usage_access_checks(vk_context& ctx) {
  std::vector<float> data{42.0f};
  std::vector<float> out(1);

  auto read_buf = vk_buffer::create(ctx, data.size() * sizeof(float),
                                    vk_buffer::usage::storage_read);
  read_buf.upload_typed(std::span{data});

  bool download_rejected = false;
  try {
    read_buf.download_typed(std::span{out});
  } catch (std::logic_error const&) {
    download_rejected = true;
  }
  assert(download_rejected);

  auto write_buf = vk_buffer::create(ctx, data.size() * sizeof(float),
                                     vk_buffer::usage::storage_write);
  bool upload_rejected = false;
  try {
    write_buf.upload_typed(std::span{data});
  } catch (std::logic_error const&) {
    upload_rejected = true;
  }
  assert(upload_rejected);

  std::println("  buffer usage access checks: OK");
}

void test_double_it_shader(vk_context& ctx) {
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
      vk_pipeline::create(ctx, shaders::test_double_it, 2, sizeof(uint32_t));

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
  std::println("  test_double_it shader (N={}): OK", N);
}

void test_large_dispatch(vk_context& ctx) {
  constexpr std::size_t N = 10000;
  std::vector<float> input(N);
  for (std::size_t i = 0; i < N; ++i) input[i] = static_cast<float>(i) * 0.1f;

  auto in_buf =
      vk_buffer::create(ctx, N * sizeof(float), vk_buffer::usage::storage_read);
  auto out_buf = vk_buffer::create(ctx, N * sizeof(float),
                                   vk_buffer::usage::storage_write);

  in_buf.upload_typed(std::span{input});

  auto pipeline =
      vk_pipeline::create(ctx, shaders::test_double_it, 2, sizeof(uint32_t));

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

  std::unique_ptr<vk_context> ctx;
  try {
    ctx = std::make_unique<vk_context>();
  } catch (std::exception const& e) {
    std::println("SKIP: Vulkan context unavailable: {}", e.what());
    return 77;
  }

  test_context_creation(*ctx);
  test_buffer_upload_download(*ctx);
  test_buffer_usage_access_checks(*ctx);
  test_double_it_shader(*ctx);
  test_large_dispatch(*ctx);
  std::println("All vulkan_compute tests passed");
}

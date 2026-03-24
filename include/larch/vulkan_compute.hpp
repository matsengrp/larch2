#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace larch {

// Vulkan compute context — owns instance, device, queue, command pool.
// Move-only. Does NOT include <vulkan/vulkan.h> (pimpl).
class vk_context {
 public:
  vk_context();
  ~vk_context();
  vk_context(vk_context&&) noexcept;
  vk_context& operator=(vk_context&&) noexcept;
  vk_context(vk_context const&) = delete;
  vk_context& operator=(vk_context const&) = delete;

  std::string device_name() const;

  struct impl;
  impl& get_impl();
  impl const& get_impl() const;

 private:
  std::unique_ptr<impl> impl_;
};

// GPU buffer — wraps VkBuffer + VkDeviceMemory. Move-only.
class vk_buffer {
 public:
  enum class usage { storage_read, storage_write, storage_read_write };

  static vk_buffer create(vk_context& ctx, std::size_t size_bytes, usage u);

  void upload(std::span<const std::byte> data);
  void download(std::span<std::byte> out) const;

  // Typed helpers.
  template <typename T>
  void upload_typed(std::span<T> data) {
    upload({reinterpret_cast<const std::byte*>(data.data()),
            data.size() * sizeof(T)});
  }
  template <typename T>
  void download_typed(std::span<T> out) const {
    download(
        {reinterpret_cast<std::byte*>(out.data()), out.size() * sizeof(T)});
  }

  std::size_t size_bytes() const;

  ~vk_buffer();
  vk_buffer(vk_buffer&&) noexcept;
  vk_buffer& operator=(vk_buffer&&) noexcept;
  vk_buffer(vk_buffer const&) = delete;
  vk_buffer& operator=(vk_buffer const&) = delete;

  struct impl;
  impl& get_impl();

 private:
  std::unique_ptr<impl> impl_;
  explicit vk_buffer(std::unique_ptr<impl>);
};

// Compute pipeline — wraps VkPipeline + descriptor management.
class vk_pipeline {
 public:
  // Create a pipeline from SPIR-V bytecode.
  // num_storage_buffers: how many storage buffer bindings (binding 0..N-1).
  // push_constant_size: size of push constants in bytes (0 if none).
  static vk_pipeline create(vk_context& ctx,
                            std::span<const std::uint32_t> spirv,
                            std::uint32_t num_storage_buffers,
                            std::size_t push_constant_size = 0);

  // Bind buffers, set push constants, dispatch workgroups, and wait.
  void dispatch(std::vector<vk_buffer*> buffers,
                std::span<const std::byte> push_constants,
                std::array<std::uint32_t, 3> workgroup_count);

  // Convenience: dispatch with no push constants.
  void dispatch(std::vector<vk_buffer*> buffers,
                std::array<std::uint32_t, 3> workgroup_count) {
    dispatch(std::move(buffers), {}, workgroup_count);
  }

  ~vk_pipeline();
  vk_pipeline(vk_pipeline&&) noexcept;
  vk_pipeline& operator=(vk_pipeline&&) noexcept;
  vk_pipeline(vk_pipeline const&) = delete;
  vk_pipeline& operator=(vk_pipeline const&) = delete;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
  explicit vk_pipeline(std::unique_ptr<impl>);
};

}  // namespace larch

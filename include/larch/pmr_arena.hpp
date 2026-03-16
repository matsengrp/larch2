#pragma once

#include <cstddef>
#include <memory_resource>

namespace larch {

template <std::size_t StackSize = 8192>
struct scoped_arena {
  alignas(std::max_align_t) std::byte buf_[StackSize];
  std::pmr::monotonic_buffer_resource resource_{buf_, StackSize};

  std::pmr::memory_resource* get() { return &resource_; }
};

}  // namespace larch

#include "chart_spr_allocation_observer.hpp"

#include <cstddef>
#include <new>

namespace allocation_test = larch::test::chart_spr_allocation;

// Keep new-expression pointers visible to code in a separate translation
// unit.  A same-TU noinline sink is still transparent enough for GCC to apply
// the standard's permitted replaceable-allocation elision, which makes an
// overload-routing self-test vacuous.  This opaque sink forces the object
// address to escape the compiling translation unit without allocating.
extern "C" void chart_spr_allocation_test_escape_pointer(
    void* pointer) noexcept {
  static void* volatile escaped_pointer = nullptr;
  escaped_pointer = pointer;
}

// Strong test-executable replacements for the complete replaceable allocation
// family.  Keep these definitions in one translation unit: including the
// observer header alone never changes a binary's allocator behavior.

void* operator new(std::size_t size) {
  return allocation_test::detail::allocate_or_throw(
      size, allocation_test::allocation_kind::scalar_throwing,
      allocation_test::detail::allocation_backend::ordinary);
}

void* operator new[](std::size_t size) {
  return allocation_test::detail::allocate_or_throw(
      size, allocation_test::allocation_kind::array_throwing,
      allocation_test::detail::allocation_backend::ordinary);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_test::detail::allocate_or_throw(
      size, allocation_test::allocation_kind::scalar_aligned_throwing,
      allocation_test::detail::allocation_backend::aligned,
      static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocation_test::detail::allocate_or_throw(
      size, allocation_test::allocation_kind::array_aligned_throwing,
      allocation_test::detail::allocation_backend::aligned,
      static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::nothrow_t const&) noexcept {
  try {
    return allocation_test::detail::allocate_or_throw(
        size, allocation_test::allocation_kind::scalar_nothrow,
        allocation_test::detail::allocation_backend::ordinary);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::nothrow_t const&) noexcept {
  try {
    return allocation_test::detail::allocate_or_throw(
        size, allocation_test::allocation_kind::array_nothrow,
        allocation_test::detail::allocation_backend::ordinary);
  } catch (...) {
    return nullptr;
  }
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   std::nothrow_t const&) noexcept {
  try {
    return allocation_test::detail::allocate_or_throw(
        size, allocation_test::allocation_kind::scalar_aligned_nothrow,
        allocation_test::detail::allocation_backend::aligned,
        static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     std::nothrow_t const&) noexcept {
  try {
    return allocation_test::detail::allocate_or_throw(
        size, allocation_test::allocation_kind::array_aligned_nothrow,
        allocation_test::detail::allocation_backend::aligned,
        static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* memory) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete[](void* memory) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete(void* memory, std::nothrow_t const&) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete[](void* memory, std::nothrow_t const&) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete(void* memory, std::align_val_t,
                     std::nothrow_t const&) noexcept {
  allocation_test::detail::deallocate(memory);
}

void operator delete[](void* memory, std::align_val_t,
                       std::nothrow_t const&) noexcept {
  allocation_test::detail::deallocate(memory);
}

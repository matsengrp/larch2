#pragma once

// Test-only allocation observation for the WRIC Phase-2A scoring seam.
//
// This support is deliberately independent of product headers.  An executable
// that links chart_spr_allocation_overrides.cpp strongly replaces the global
// allocation/deallocation functions and can place a
// scoped_allocation_observation around exactly the operation being measured.
// Scope entry, accounting, and scope restoration perform no allocations.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

namespace larch::test::chart_spr_allocation {

enum class allocation_kind : std::uint8_t {
  scalar_throwing,
  array_throwing,
  scalar_nothrow,
  array_nothrow,
  scalar_aligned_throwing,
  array_aligned_throwing,
  scalar_aligned_nothrow,
  array_aligned_nothrow,
  count,
};

inline constexpr std::size_t allocation_kind_count =
    static_cast<std::size_t>(allocation_kind::count);

struct allocation_counters {
  // One call per invoked global allocation overload.  Handler retries do not
  // add calls or requested bytes; they add backend_attempts to the original
  // call.  Successes/failures are mutually exclusive terminal call outcomes.
  std::uint64_t calls = 0;
  std::uint64_t successes = 0;
  std::uint64_t failures = 0;
  std::uint64_t requested_bytes = 0;
  std::uint64_t backend_attempts = 0;

  bool operator==(allocation_counters const&) const = default;
};

struct allocation_statistics : allocation_counters {
  std::array<allocation_counters, allocation_kind_count> by_kind{};

  [[nodiscard]] allocation_counters const& for_kind(
      allocation_kind kind) const noexcept {
    return by_kind[static_cast<std::size_t>(kind)];
  }

  bool operator==(allocation_statistics const&) const = default;
};

struct allocation_observer {
  allocation_statistics statistics;

  void reset() noexcept { statistics = {}; }
};

namespace detail {

inline thread_local allocation_observer* active_observer = nullptr;

// Deterministic test injection at the backend boundary.  Each injected failure
// consumes one backend attempt before malloc/aligned_alloc is called.  This is
// thread-local for the same reason observation is thread-local: a failure test
// must not perturb unrelated allocator traffic on another worker.
inline thread_local std::size_t forced_backend_failures_remaining = 0;

enum class allocation_backend : std::uint8_t {
  ordinary,
  aligned,
};

class allocation_call_accounting {
 public:
  allocation_call_accounting(std::size_t requested_bytes,
                             allocation_kind kind) noexcept
      : observer_(active_observer), kind_(kind) {
    if (observer_ == nullptr) return;
    ++observer_->statistics.calls;
    observer_->statistics.requested_bytes += requested_bytes;
    auto& per_kind = observer_->statistics.by_kind[kind_index()];
    ++per_kind.calls;
    per_kind.requested_bytes += requested_bytes;
  }

  void record_backend_attempt() noexcept {
    if (observer_ == nullptr) return;
    ++observer_->statistics.backend_attempts;
    ++observer_->statistics.by_kind[kind_index()].backend_attempts;
  }

  void record_success() noexcept {
    if (observer_ == nullptr) return;
    ++observer_->statistics.successes;
    ++observer_->statistics.by_kind[kind_index()].successes;
  }

  void record_failure() noexcept {
    if (observer_ == nullptr) return;
    ++observer_->statistics.failures;
    ++observer_->statistics.by_kind[kind_index()].failures;
  }

 private:
  [[nodiscard]] std::size_t kind_index() const noexcept {
    return static_cast<std::size_t>(kind_);
  }

  allocation_observer* observer_ = nullptr;
  allocation_kind kind_ = allocation_kind::scalar_throwing;
};

inline bool is_power_of_two(std::size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

inline void* ordinary_backend_allocate(std::size_t size) noexcept {
  // The allocation functions must return a non-null suitably aligned pointer
  // for a successful zero-size request, so do not delegate malloc(0)'s
  // implementation-defined behavior directly.
  return std::malloc(size == 0 ? std::size_t{1} : size);
}

inline void* aligned_backend_allocate(std::size_t size,
                                      std::size_t alignment) noexcept {
  if (!is_power_of_two(alignment)) return nullptr;
  // aligned_alloc requires an implementation-supported fundamental minimum,
  // while an explicit aligned-new call may validly request 1/2/4.  Raising a
  // smaller power of two still satisfies the requested alignment.
  if (alignment < alignof(void*)) alignment = alignof(void*);

  auto effective_size = size == 0 ? std::size_t{1} : size;
  if (effective_size >
      std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
    return nullptr;
  }
  auto rounded_size =
      ((effective_size + alignment - 1) / alignment) * alignment;
  return std::aligned_alloc(alignment, rounded_size);
}

inline void* attempt_backend_allocate(allocation_backend backend,
                                      std::size_t size,
                                      std::size_t alignment) noexcept {
  if (forced_backend_failures_remaining != 0) {
    --forced_backend_failures_remaining;
    return nullptr;
  }
  if (backend == allocation_backend::ordinary) {
    return ordinary_backend_allocate(size);
  }
  return aligned_backend_allocate(size, alignment);
}

// Common throwing engine for all eight allocation overloads.  Nothrow global
// overloads call this engine and catch every exception, which preserves the
// standard new_handler retry behavior while returning nullptr on final failure.
inline void* allocate_or_throw(std::size_t size, allocation_kind kind,
                               allocation_backend backend,
                               std::size_t alignment = 0) {
  allocation_call_accounting accounting{size, kind};
  try {
    for (;;) {
      accounting.record_backend_attempt();
      if (auto* memory = attempt_backend_allocate(backend, size, alignment)) {
        accounting.record_success();
        return memory;
      }

      auto handler = std::get_new_handler();
      if (handler == nullptr) throw std::bad_alloc{};
      handler();
    }
  } catch (...) {
    accounting.record_failure();
    throw;
  }
}

inline void deallocate(void* memory) noexcept { std::free(memory); }

}  // namespace detail

class scoped_allocation_observation {
 public:
  explicit scoped_allocation_observation(allocation_observer& observer) noexcept
      : previous_(detail::active_observer) {
    detail::active_observer = &observer;
  }

  scoped_allocation_observation(scoped_allocation_observation const&) = delete;
  scoped_allocation_observation& operator=(
      scoped_allocation_observation const&) = delete;

  ~scoped_allocation_observation() noexcept {
    detail::active_observer = previous_;
  }

 private:
  allocation_observer* previous_ = nullptr;
};

[[nodiscard]] inline allocation_observer* current_observer() noexcept {
  return detail::active_observer;
}

// Test-only deterministic backend-failure scope.  Nested scopes restore the
// exact remaining count they replaced, just as observation scopes restore the
// exact observer pointer they replaced.
class scoped_backend_failures {
 public:
  explicit scoped_backend_failures(std::size_t failures) noexcept
      : previous_(detail::forced_backend_failures_remaining) {
    detail::forced_backend_failures_remaining = failures;
  }

  scoped_backend_failures(scoped_backend_failures const&) = delete;
  scoped_backend_failures& operator=(scoped_backend_failures const&) = delete;

  ~scoped_backend_failures() noexcept {
    detail::forced_backend_failures_remaining = previous_;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return detail::forced_backend_failures_remaining;
  }

 private:
  std::size_t previous_ = 0;
};

[[nodiscard]] inline std::size_t forced_backend_failures_remaining() noexcept {
  return detail::forced_backend_failures_remaining;
}

static_assert(std::is_nothrow_constructible_v<scoped_allocation_observation,
                                              allocation_observer&>);
static_assert(std::is_nothrow_destructible_v<scoped_allocation_observation>);
static_assert(
    std::is_nothrow_constructible_v<scoped_backend_failures, std::size_t>);
static_assert(std::is_nothrow_destructible_v<scoped_backend_failures>);

}  // namespace larch::test::chart_spr_allocation

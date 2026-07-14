// Phase-2A test-only allocator instrumentation self-test.
//
// This intentionally does not call or measure the current public chart-SPR
// scorer: the eventual allocation gate belongs around the frozen
// score_candidates_locally_into scoring seam only.  These tests establish that
// the observer used by that future seam is complete and trustworthy.

#include "chart_spr_allocation_observer.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>

namespace allocation_test = larch::test::chart_spr_allocation;

[[noreturn]] static void test_fail(char const* expression, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expression);
}

#define CHECK(expression)                                          \
  do {                                                             \
    if (!(expression)) test_fail(#expression, __FILE__, __LINE__); \
  } while (false)

static void check_statistics(allocation_test::allocation_observer const& obs,
                             std::uint64_t calls, std::uint64_t successes,
                             std::uint64_t failures,
                             std::uint64_t requested_bytes,
                             std::uint64_t backend_attempts) {
  CHECK(obs.statistics.calls == calls);
  CHECK(obs.statistics.successes == successes);
  CHECK(obs.statistics.failures == failures);
  CHECK(obs.statistics.requested_bytes == requested_bytes);
  CHECK(obs.statistics.backend_attempts == backend_attempts);
  CHECK(obs.statistics.calls ==
        obs.statistics.successes + obs.statistics.failures);

  allocation_test::allocation_counters kind_total;
  for (auto const& counters : obs.statistics.by_kind) {
    kind_total.calls += counters.calls;
    kind_total.successes += counters.successes;
    kind_total.failures += counters.failures;
    kind_total.requested_bytes += counters.requested_bytes;
    kind_total.backend_attempts += counters.backend_attempts;
  }
  CHECK(kind_total.calls == obs.statistics.calls);
  CHECK(kind_total.successes == obs.statistics.successes);
  CHECK(kind_total.failures == obs.statistics.failures);
  CHECK(kind_total.requested_bytes == obs.statistics.requested_bytes);
  CHECK(kind_total.backend_attempts == obs.statistics.backend_attempts);
}

static void check_kind(allocation_test::allocation_observer const& observer,
                       allocation_test::allocation_kind kind,
                       std::uint64_t calls, std::uint64_t successes,
                       std::uint64_t failures, std::uint64_t requested_bytes,
                       std::uint64_t backend_attempts) {
  auto const& counters = observer.statistics.for_kind(kind);
  CHECK(counters.calls == calls);
  CHECK(counters.successes == successes);
  CHECK(counters.failures == failures);
  CHECK(counters.requested_bytes == requested_bytes);
  CHECK(counters.backend_attempts == backend_attempts);
  CHECK(counters.calls == counters.successes + counters.failures);
}

static void check_alignment(void* memory, std::size_t alignment) {
  CHECK(memory != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(memory) % alignment == 0);
}

static void test_complete_replaceable_overload_matrix() {
  constexpr auto alignment = std::align_val_t{64};
  constexpr std::size_t alignment_value = 64;

  allocation_test::allocation_observer observer;
  {
    allocation_test::scoped_allocation_observation scope{observer};

    auto* zero = ::operator new(0);
    CHECK(zero != nullptr);
    ::operator delete(zero);

    auto* ordinary = ::operator new(1);
    CHECK(ordinary != nullptr);
    ::operator delete(ordinary);

    auto* array = ::operator new[](2);
    CHECK(array != nullptr);
    ::operator delete[](array);

    auto* sized = ::operator new(3);
    CHECK(sized != nullptr);
    ::operator delete(sized, std::size_t{3});

    auto* sized_array = ::operator new[](4);
    CHECK(sized_array != nullptr);
    ::operator delete[](sized_array, std::size_t{4});

    auto* nothrow = ::operator new(5, std::nothrow);
    CHECK(nothrow != nullptr);
    ::operator delete(nothrow, std::nothrow);

    auto* nothrow_array = ::operator new[](6, std::nothrow);
    CHECK(nothrow_array != nullptr);
    ::operator delete[](nothrow_array, std::nothrow);

    auto* aligned = ::operator new(7, alignment);
    check_alignment(aligned, alignment_value);
    ::operator delete(aligned, alignment);

    auto* aligned_array = ::operator new[](8, alignment);
    check_alignment(aligned_array, alignment_value);
    ::operator delete[](aligned_array, alignment);

    auto* sized_aligned = ::operator new(9, alignment);
    check_alignment(sized_aligned, alignment_value);
    ::operator delete(sized_aligned, std::size_t{9}, alignment);

    auto* sized_aligned_array = ::operator new[](10, alignment);
    check_alignment(sized_aligned_array, alignment_value);
    ::operator delete[](sized_aligned_array, std::size_t{10}, alignment);

    auto* aligned_nothrow = ::operator new(11, alignment, std::nothrow);
    check_alignment(aligned_nothrow, alignment_value);
    ::operator delete(aligned_nothrow, alignment, std::nothrow);

    auto* aligned_nothrow_array = ::operator new[](12, alignment, std::nothrow);
    check_alignment(aligned_nothrow_array, alignment_value);
    ::operator delete[](aligned_nothrow_array, alignment, std::nothrow);

    // Valid small power-of-two aligned-new requests must be raised to the C
    // backend's minimum rather than spuriously failing.
    constexpr std::array<std::size_t, 3> small_alignments{1, 2, 4};
    for (std::size_t i = 0; i < small_alignments.size(); ++i) {
      auto requested_alignment = std::align_val_t{small_alignments[i]};
      auto* small_aligned = ::operator new(13 + i, requested_alignment);
      check_alignment(small_aligned, alignof(void*));
      ::operator delete(small_aligned, requested_alignment);
    }
  }

  // Sixteen allocation calls: zero, sizes 1..12, and sizes 13..15 for backend
  // minimum-alignment normalization.  Every matching delete family is
  // explicitly linked and invoked above.
  check_statistics(observer, 16, 16, 0, 120, 16);
  check_kind(observer, allocation_test::allocation_kind::scalar_throwing, 3, 3,
             0, 4, 3);
  check_kind(observer, allocation_test::allocation_kind::array_throwing, 2, 2,
             0, 6, 2);
  check_kind(observer, allocation_test::allocation_kind::scalar_nothrow, 1, 1,
             0, 5, 1);
  check_kind(observer, allocation_test::allocation_kind::array_nothrow, 1, 1, 0,
             6, 1);
  check_kind(observer,
             allocation_test::allocation_kind::scalar_aligned_throwing, 5, 5, 0,
             58, 5);
  check_kind(observer, allocation_test::allocation_kind::array_aligned_throwing,
             2, 2, 0, 18, 2);
  check_kind(observer, allocation_test::allocation_kind::scalar_aligned_nothrow,
             1, 1, 0, 11, 1);
  check_kind(observer, allocation_test::allocation_kind::array_aligned_nothrow,
             1, 1, 0, 12, 1);
}

struct regular_payload {
  std::array<std::uint64_t, 4> words{};
};

struct alignas(128) over_aligned_payload {
  std::array<std::byte, 129> bytes{};
};

extern "C" void chart_spr_allocation_test_escape_pointer(void*) noexcept;

static void test_new_expressions_and_over_alignment() {
  allocation_test::allocation_observer observer;
  {
    allocation_test::scoped_allocation_observation scope{observer};

    auto* regular = new regular_payload;
    chart_spr_allocation_test_escape_pointer(regular);
    delete regular;

    auto* regular_array = new regular_payload[3];
    chart_spr_allocation_test_escape_pointer(regular_array);
    delete[] regular_array;

    auto* aligned = new over_aligned_payload;
    check_alignment(aligned, alignof(over_aligned_payload));
    chart_spr_allocation_test_escape_pointer(aligned);
    delete aligned;

    auto* aligned_array = new over_aligned_payload[2];
    check_alignment(aligned_array, alignof(over_aligned_payload));
    chart_spr_allocation_test_escape_pointer(aligned_array);
    delete[] aligned_array;
  }

  CHECK(observer.statistics.calls == 4);
  CHECK(observer.statistics.successes == 4);
  CHECK(observer.statistics.failures == 0);
  CHECK(observer.statistics.backend_attempts == 4);
  CHECK(observer.statistics.requested_bytes >=
        sizeof(regular_payload) * 4 + sizeof(over_aligned_payload) * 3);
  CHECK(observer.statistics
            .for_kind(allocation_test::allocation_kind::scalar_throwing)
            .calls == 1);
  CHECK(observer.statistics
            .for_kind(allocation_test::allocation_kind::array_throwing)
            .calls == 1);
  CHECK(observer.statistics
            .for_kind(allocation_test::allocation_kind::scalar_aligned_throwing)
            .calls == 1);
  CHECK(observer.statistics
            .for_kind(allocation_test::allocation_kind::array_aligned_throwing)
            .calls == 1);
}

static void test_scoped_unscoped_nested_and_output_exclusion() {
  CHECK(allocation_test::current_observer() == nullptr);

  // Explicit unscoped traffic must not attach itself to the next observer.
  auto* before_scope = ::operator new(13);
  ::operator delete(before_scope);

  allocation_test::allocation_observer outer;
  allocation_test::allocation_observer inner;
  allocation_test::allocation_observer exception_scope;
  {
    allocation_test::scoped_allocation_observation outer_scope{outer};
    CHECK(allocation_test::current_observer() == &outer);

    // Constructing and destroying an empty nested scope is itself allocation
    // free, and restores the exact pointer it replaced.
    {
      allocation_test::scoped_allocation_observation empty_scope{inner};
      CHECK(allocation_test::current_observer() == &inner);
    }
    CHECK(allocation_test::current_observer() == &outer);
    check_statistics(outer, 0, 0, 0, 0, 0);
    check_statistics(inner, 0, 0, 0, 0, 0);

    auto* outer_first = ::operator new(17);
    ::operator delete(outer_first);

    {
      allocation_test::scoped_allocation_observation inner_scope{inner};
      auto* inner_only = ::operator new[](23);
      ::operator delete[](inner_only);

      try {
        allocation_test::scoped_allocation_observation unwinding_scope{
            exception_scope};
        CHECK(allocation_test::current_observer() == &exception_scope);
        throw 7;
      } catch (int value) {
        CHECK(value == 7);
      }
      CHECK(allocation_test::current_observer() == &inner);
    }
    CHECK(allocation_test::current_observer() == &outer);

    auto* outer_second = ::operator new(31);
    ::operator delete(outer_second);
  }
  CHECK(allocation_test::current_observer() == nullptr);

  check_statistics(outer, 2, 2, 0, 48, 2);
  check_statistics(inner, 1, 1, 0, 23, 1);
  check_statistics(exception_scope, 0, 0, 0, 0, 0);
  check_kind(outer, allocation_test::allocation_kind::scalar_throwing, 2, 2, 0,
             48, 2);
  check_kind(inner, allocation_test::allocation_kind::array_throwing, 1, 1, 0,
             23, 1);

  // A future benchmark may allocate its report/output after the measured
  // scoring seam.  This explicit output allocation proves that traffic after
  // scope exit is ignored rather than contaminating the frozen region.
  auto const frozen = outer.statistics;
  auto* output_buffer = ::operator new[](4096);
  CHECK(output_buffer != nullptr);
  ::operator delete[](output_buffer);
  CHECK(outer.statistics == frozen);
}

static std::uint64_t new_handler_calls = 0;

static void returning_new_handler() { ++new_handler_calls; }

struct new_handler_marker {};

static void throwing_new_handler() {
  ++new_handler_calls;
  throw new_handler_marker{};
}

class scoped_new_handler {
 public:
  explicit scoped_new_handler(std::new_handler handler) noexcept
      : previous_(std::set_new_handler(handler)) {}

  scoped_new_handler(scoped_new_handler const&) = delete;
  scoped_new_handler& operator=(scoped_new_handler const&) = delete;

  ~scoped_new_handler() noexcept { std::set_new_handler(previous_); }

 private:
  std::new_handler previous_ = nullptr;
};

static void test_forced_failure_and_new_handler_semantics() {
  CHECK(allocation_test::forced_backend_failures_remaining() == 0);

  // A returning handler is called once per failed backend attempt, and the
  // original allocation call succeeds on the third attempt.
  {
    allocation_test::allocation_observer observer;
    new_handler_calls = 0;
    scoped_new_handler handler_scope{returning_new_handler};
    allocation_test::scoped_backend_failures failures{2};
    {
      allocation_test::scoped_allocation_observation observation{observer};
      auto* memory = ::operator new(41);
      CHECK(memory != nullptr);
      CHECK(failures.remaining() == 0);
      ::operator delete(memory);
    }
    CHECK(new_handler_calls == 2);
    check_statistics(observer, 1, 1, 0, 41, 3);
    check_kind(observer, allocation_test::allocation_kind::scalar_throwing, 1,
               1, 0, 41, 3);
  }
  CHECK(allocation_test::forced_backend_failures_remaining() == 0);

  // Nothrow aligned allocation uses the same retry loop and preserves
  // alignment on the eventual successful backend call.
  {
    allocation_test::allocation_observer observer;
    new_handler_calls = 0;
    scoped_new_handler handler_scope{returning_new_handler};
    allocation_test::scoped_backend_failures failures{2};
    {
      allocation_test::scoped_allocation_observation observation{observer};
      auto* memory = ::operator new[](43, std::align_val_t{64}, std::nothrow);
      check_alignment(memory, 64);
      ::operator delete[](memory, std::align_val_t{64}, std::nothrow);
    }
    CHECK(new_handler_calls == 2);
    check_statistics(observer, 1, 1, 0, 43, 3);
    check_kind(observer,
               allocation_test::allocation_kind::array_aligned_nothrow, 1, 1, 0,
               43, 3);
  }

  // With no handler, a throwing form reports one final failure and throws
  // bad_alloc after the failed backend attempt.
  {
    allocation_test::allocation_observer observer;
    scoped_new_handler handler_scope{nullptr};
    allocation_test::scoped_backend_failures failures{1};
    bool caught = false;
    {
      allocation_test::scoped_allocation_observation observation{observer};
      try {
        (void)::operator new(47);
      } catch (std::bad_alloc const&) {
        caught = true;
      }
    }
    CHECK(caught);
    check_statistics(observer, 1, 0, 1, 47, 1);
    check_kind(observer, allocation_test::allocation_kind::scalar_throwing, 1,
               0, 1, 47, 1);
  }

  // Exceptions from a handler propagate through throwing new and are mapped
  // to nullptr by a nothrow form; both are one failed allocation call.
  {
    allocation_test::allocation_observer observer;
    new_handler_calls = 0;
    scoped_new_handler handler_scope{throwing_new_handler};
    allocation_test::scoped_backend_failures failures{1};
    bool caught = false;
    {
      allocation_test::scoped_allocation_observation observation{observer};
      try {
        (void)::operator new[](53);
      } catch (new_handler_marker const&) {
        caught = true;
      }
    }
    CHECK(caught);
    CHECK(new_handler_calls == 1);
    check_statistics(observer, 1, 0, 1, 53, 1);
    check_kind(observer, allocation_test::allocation_kind::array_throwing, 1, 0,
               1, 53, 1);
  }

  {
    allocation_test::allocation_observer observer;
    new_handler_calls = 0;
    scoped_new_handler handler_scope{throwing_new_handler};
    allocation_test::scoped_backend_failures failures{1};
    void* memory = reinterpret_cast<void*>(std::uintptr_t{1});
    {
      allocation_test::scoped_allocation_observation observation{observer};
      memory = ::operator new(59, std::nothrow);
    }
    CHECK(memory == nullptr);
    CHECK(new_handler_calls == 1);
    check_statistics(observer, 1, 0, 1, 59, 1);
    check_kind(observer, allocation_test::allocation_kind::scalar_nothrow, 1, 0,
               1, 59, 1);
  }

  // Failure-injection scopes also restore the exact outer remaining count.
  {
    allocation_test::scoped_backend_failures outer{5};
    CHECK(outer.remaining() == 5);
    {
      allocation_test::scoped_backend_failures inner{2};
      CHECK(inner.remaining() == 2);
    }
    CHECK(outer.remaining() == 5);
  }
  CHECK(allocation_test::forced_backend_failures_remaining() == 0);
}

static void test_thread_local_isolation() {
  std::atomic<bool> worker_ready = false;
  std::atomic<bool> worker_go = false;
  std::atomic<bool> worker_done = false;
  allocation_test::allocation_observer worker_observer;
  bool worker_restored = false;

  // Thread creation happens before the main-thread scope, so implementation
  // allocations made by std::thread are intentionally outside measurement.
  std::thread worker{[&] {
    worker_ready.store(true, std::memory_order_release);
    while (!worker_go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    {
      allocation_test::scoped_allocation_observation observation{
          worker_observer};
      auto* memory = ::operator new(61);
      ::operator delete(memory);
    }
    worker_restored = allocation_test::current_observer() == nullptr;
    worker_done.store(true, std::memory_order_release);
  }};
  while (!worker_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  allocation_test::allocation_observer main_observer;
  {
    allocation_test::scoped_allocation_observation observation{main_observer};
    worker_go.store(true, std::memory_order_release);
    while (!worker_done.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    // The worker's observed allocation must not appear in this thread's
    // observer.  Atomic synchronization and yielding are allocation-free.
    check_statistics(main_observer, 0, 0, 0, 0, 0);
    auto* memory = ::operator new(67);
    ::operator delete(memory);
  }
  worker.join();

  CHECK(worker_restored);
  check_statistics(worker_observer, 1, 1, 0, 61, 1);
  check_statistics(main_observer, 1, 1, 0, 67, 1);
  check_kind(worker_observer, allocation_test::allocation_kind::scalar_throwing,
             1, 1, 0, 61, 1);
  check_kind(main_observer, allocation_test::allocation_kind::scalar_throwing,
             1, 1, 0, 67, 1);
}

int main() {
  test_complete_replaceable_overload_matrix();
  test_new_expressions_and_over_alignment();
  test_scoped_unscoped_nested_and_output_exclusion();
  test_forced_failure_and_new_handler_semantics();
  test_thread_local_isolation();

  std::println(
      "All chart-SPR test-only allocation observer self-tests passed!");
  return 0;
}

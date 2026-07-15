// Phase-2A test-only allocator instrumentation and dense local-score gate.
//
// The observer self-tests establish complete replaceable-new coverage.  The
// final test then places that observer around only the caller-owned
// score_candidates_locally_into scoring seam on the frozen small dense
// workload.  Fixture/state/candidate construction, workspace warmup, owning
// compatibility results, and semantic comparisons deliberately stay outside
// the measured region.

#include "chart_spr_allocation_observer.hpp"

#include <larch/chart_spr_search.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/sha256.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <print>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

namespace {

constexpr std::string_view k_dense_fixture_path =
    "data/test_5_trees/tree_0.pb.gz";
constexpr std::string_view k_dense_fixture_sha256 =
    "e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6";
constexpr std::string_view k_dense_candidate_signature_sha256 =
    "6fcd6f69962abb67e91a3236c5043787c86c33bbd4c135fa67c37b53a6e37168";
constexpr std::string_view k_dense_local_score_tuple_sha256 =
    "86f0f046744ba8dfe2d6f33668f9c47a3e0c56646bb58fce9ada71a5bb18ead7";
constexpr std::size_t k_frozen_candidate_count = 64;
constexpr std::size_t k_scored_candidate_count = 1000;
constexpr std::size_t k_active_pattern_count = 113;
constexpr std::size_t k_observed_repetitions = 3;
constexpr std::size_t k_chart_memory_budget_bytes = 12884901888ULL;

// Frozen-binary semantic provenance lives beside the Phase-2 DHAT reference:
//   binary 7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c
//   fixture e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6
//   sidecar b8d55c73220a7025b8d977ac4bb083f1e4e76dee5bf007e8754b1f79d42b8442
//   candidate section
//                3bb4a595b15d42e4e326fab5f54ca0befb93e3c998535182f30afb1876766dff
// Two independent frozen executions produced the same sidecar and output DAG.
// The compact rows below are the ordered candidate/candidate_lower_bound pairs
// extracted from that sidecar.  Every omitted field is frozen once immediately
// below and is asserted for every row; no value comes from the scorer under
// test.
struct frozen_local_score_tuple {
  std::size_t affected_clade_count;
  std::int64_t delta;
  std::uint64_t new_score;
};

constexpr bool k_frozen_local_score_valid = true;
constexpr std::string_view k_frozen_local_score_invalid_reason = "";
constexpr std::uint64_t k_frozen_local_score_old_score = 174;
constexpr auto k_frozen_local_score_kind =
    larch::chart_spr_score_kind::composite_lower_bound;
constexpr auto k_frozen_local_score_convention =
    larch::chart_spr_score_convention::full_with_invariants;
constexpr std::uint64_t k_frozen_local_score_invariant_offset = 0;
constexpr bool k_frozen_local_score_exact_multisite = false;

constexpr std::array<frozen_local_score_tuple, k_frozen_candidate_count>
    k_frozen_local_score_tuples = {{
        {5, 0, 174},   {8, 6, 180},   {11, 9, 183},  {11, 9, 183},
        {12, 8, 182},  {12, 8, 182},  {15, 10, 184}, {15, 10, 184},
        {14, 8, 182},  {13, 8, 182},  {15, 9, 183},  {16, 9, 183},
        {17, 8, 182},  {20, 8, 182},  {20, 8, 182},  {20, 8, 182},
        {20, 8, 182},  {18, 9, 183},  {14, 8, 182},  {14, 8, 182},
        {15, 9, 183},  {7, 7, 181},   {16, 11, 185}, {19, 12, 186},
        {19, 13, 187}, {18, 12, 186}, {17, 11, 185}, {10, 11, 185},
        {10, 11, 185}, {9, 7, 181},   {13, 8, 182},  {14, 8, 182},
        {9, 9, 183},   {14, 8, 182},  {12, 9, 183},  {13, 10, 184},
        {13, 10, 184}, {12, 9, 183},  {15, 11, 185}, {15, 11, 185},
        {15, 11, 185}, {15, 11, 185}, {15, 11, 185}, {9, 9, 183},
        {15, 11, 185}, {16, 10, 184}, {17, 10, 184}, {17, 10, 184},
        {16, 12, 186}, {16, 12, 186}, {12, 12, 186}, {14, 10, 184},
        {14, 10, 184}, {14, 10, 184}, {8, 7, 181},   {18, 12, 186},
        {18, 11, 185}, {16, 11, 185}, {17, 11, 185}, {14, 10, 184},
        {14, 10, 184}, {14, 10, 184}, {14, 10, 184}, {15, 10, 184},
    }};

static std::string raw_file_sha256(std::string_view path) {
  std::ifstream input{std::string{path}, std::ios::binary};
  CHECK(input.is_open());

  larch::sha256 digest;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    auto count = input.gcount();
    if (count > 0) {
      digest.update(
          std::string_view{buffer.data(), static_cast<std::size_t>(count)});
    }
  }
  CHECK(input.eof());
  return digest.hex_digest();
}

static std::string candidate_signature_sha256(
    larch::clade_grammar const& grammar,
    std::span<larch::grammar_spr_candidate const> candidates) {
  larch::sha256 digest;
  for (auto const& candidate : candidates) {
    auto signature =
        larch::chart_spr_candidate_sample_signature(grammar, candidate);
    digest.update(signature);
    digest.update("\n");
  }
  return digest.hex_digest();
}

struct dense_scoring_expectations {
  std::size_t affected_clade_visits = 0;
  std::size_t row_visits = 0;
  std::size_t candidate_partition_validations = 0;
  std::size_t production_descriptors = 0;
  std::size_t fast_path_productions = 0;
  std::size_t reachable_clades = 0;
  std::size_t reachable_productions = 0;
  std::size_t reachable_temp_clades = 0;
  std::size_t reachable_temp_productions = 0;
  std::size_t full_grammar_like_reachability_passes = 0;
};

// Literal workload totals prevent the allocation/counter oracle from calling
// the same overlay builder under test to manufacture its expectations.  The
// affected and reachability totals come from the sealed 7ddb1fca report and
// its frozen 64-candidate tuple projection; the 1,000-candidate values repeat
// those 64 ordered candidates fifteen times plus the first forty.  Descriptor
// totals were frozen from the pre-in-place value-builder checkpoint before
// this `_into` gate was observed.  The candidate and fixture hashes above make
// every literal workload-specific and fail closed if enumeration changes.
constexpr dense_scoring_expectations k_frozen_dense_scoring_expectations{
    .affected_clade_visits = 14255,
    .row_visits = 1610815,
    .candidate_partition_validations = 12239,
    .production_descriptors = 13255,
    .fast_path_productions = 1497815,
    .reachable_clades = 139000,
    .reachable_productions = 69000,
    .reachable_temp_clades = 11239,
    .reachable_temp_productions = 12239,
    .full_grammar_like_reachability_passes = 1000,
};

static_assert(k_frozen_dense_scoring_expectations.row_visits ==
              k_frozen_dense_scoring_expectations.affected_clade_visits *
                  k_active_pattern_count);
static_assert(k_frozen_dense_scoring_expectations.fast_path_productions ==
              k_frozen_dense_scoring_expectations.production_descriptors *
                  k_active_pattern_count);

}  // namespace

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

template <typename Score>
static void check_score_against_frozen(Score const& actual,
                                       std::size_t frozen_index) {
  CHECK(frozen_index < k_frozen_local_score_tuples.size());
  auto const& frozen = k_frozen_local_score_tuples[frozen_index];
  CHECK(actual.valid == k_frozen_local_score_valid);
  CHECK(actual.invalid_reason == k_frozen_local_score_invalid_reason);
  CHECK(actual.affected_clade_count == frozen.affected_clade_count);
  CHECK(actual.lower_bound.kind == k_frozen_local_score_kind);
  CHECK(actual.lower_bound.convention == k_frozen_local_score_convention);
  CHECK(actual.lower_bound.value.delta == frozen.delta);
  CHECK(actual.lower_bound.value.old_score == k_frozen_local_score_old_score);
  CHECK(actual.lower_bound.value.new_score == frozen.new_score);
  CHECK(actual.lower_bound.invariant_offset_applied ==
        k_frozen_local_score_invariant_offset);
  CHECK(actual.lower_bound.value.exact_multisite ==
        k_frozen_local_score_exact_multisite);
  CHECK(std::isfinite(actual.local_score_ms));
  CHECK(actual.local_score_ms >= 0.0);
}

// Canonical bytes, deliberately excluding the non-semantic timing field:
//   larch.chart_spr.local_score_tuple.v1\n
// followed by one ordered, tab-separated row per score:
//   index, valid, lowercase-hex(invalid_reason), affected count, kind,
//   convention, delta, old, new, invariant offset, exact-multisite, \n.
// This is the test-owned projection of the frozen canonical sidecar, so both
// scorer entry points are checked against an oracle produced by the sealed
// binary rather than against one another.
template <typename Score>
static std::string local_score_tuple_sha256(std::span<Score const> scores) {
  CHECK(scores.size() == k_frozen_candidate_count);

  larch::sha256 digest;
  digest.update("larch.chart_spr.local_score_tuple.v1\n");
  auto append_decimal = [&](auto value) {
    auto text = std::to_string(value);
    digest.update(text);
  };
  auto append_tab = [&] { digest.update("\t"); };
  constexpr std::string_view hex = "0123456789abcdef";

  for (std::size_t index = 0; index < scores.size(); ++index) {
    auto const& score = scores[index];
    append_decimal(index);
    append_tab();
    digest.update(score.valid ? "1" : "0");
    append_tab();
    for (char raw : score.invalid_reason) {
      auto byte = static_cast<unsigned char>(raw);
      std::array<char, 2> encoded = {hex[(byte >> 4U) & 0x0fU],
                                     hex[byte & 0x0fU]};
      digest.update(std::string_view{encoded.data(), encoded.size()});
    }
    append_tab();
    append_decimal(score.affected_clade_count);
    append_tab();
    digest.update(larch::chart_spr_score_kind_name(score.lower_bound.kind));
    append_tab();
    digest.update(
        larch::chart_spr_score_convention_name(score.lower_bound.convention));
    append_tab();
    append_decimal(score.lower_bound.value.delta);
    append_tab();
    append_decimal(score.lower_bound.value.old_score);
    append_tab();
    append_decimal(score.lower_bound.value.new_score);
    append_tab();
    append_decimal(score.lower_bound.invariant_offset_applied);
    append_tab();
    digest.update(score.lower_bound.value.exact_multisite ? "1" : "0");
    digest.update("\n");
  }
  return digest.hex_digest();
}

// Compatibility-wrapper equivalence is intentionally only a secondary check;
// the frozen tuples above are the primary semantic oracle.
static void check_local_result_equivalent(
    larch::chart_spr_local_score_result const& actual,
    larch::chart_spr_candidate_score const& owning_secondary) {
  CHECK(actual.valid == owning_secondary.valid);
  CHECK(actual.invalid_reason == owning_secondary.invalid_reason);
  CHECK(actual.affected_clade_count == owning_secondary.affected_clade_count);
  CHECK(actual.lower_bound.kind == owning_secondary.lower_bound.kind);
  CHECK(actual.lower_bound.convention ==
        owning_secondary.lower_bound.convention);
  CHECK(actual.lower_bound.invariant_offset_applied ==
        owning_secondary.lower_bound.invariant_offset_applied);
  CHECK(actual.lower_bound.value.delta ==
        owning_secondary.lower_bound.value.delta);
  CHECK(actual.lower_bound.value.old_score ==
        owning_secondary.lower_bound.value.old_score);
  CHECK(actual.lower_bound.value.new_score ==
        owning_secondary.lower_bound.value.new_score);
  CHECK(actual.lower_bound.value.exact_multisite ==
        owning_secondary.lower_bound.value.exact_multisite);
}

static void check_all_local_results(
    std::span<larch::chart_spr_local_score_result const> actual,
    std::span<larch::chart_spr_candidate_score const> owning_secondary) {
  CHECK(actual.size() == k_scored_candidate_count);
  CHECK(owning_secondary.size() == k_frozen_candidate_count);
  for (std::size_t i = 0; i < actual.size(); ++i) {
    auto frozen_index = i % k_frozen_candidate_count;
    check_score_against_frozen(actual[i], frozen_index);
    check_local_result_equivalent(actual[i], owning_secondary[frozen_index]);
  }
  CHECK(local_score_tuple_sha256(actual.first(k_frozen_candidate_count)) ==
        k_dense_local_score_tuple_sha256);
}

static void poison_local_results(
    std::span<larch::chart_spr_local_score_result> results) {
  for (auto& result : results) {
    result.lower_bound.value.delta = (std::numeric_limits<std::int64_t>::max)();
    result.lower_bound.value.old_score =
        (std::numeric_limits<std::uint64_t>::max)();
    result.lower_bound.value.new_score =
        (std::numeric_limits<std::uint64_t>::max)();
    result.lower_bound.value.exact_multisite = true;
    result.lower_bound.kind = larch::chart_spr_score_kind::grammar_exact;
    result.lower_bound.convention =
        larch::chart_spr_score_convention::active_only;
    result.lower_bound.invariant_offset_applied =
        (std::numeric_limits<std::uint64_t>::max)();
    result.affected_clade_count = (std::numeric_limits<std::size_t>::max)();
    result.local_score_ms = -1.0;
    result.valid = false;
    result.invalid_reason.clear();
  }
}

static void check_dense_scoring_counter_delta(
    larch::chart_spr_search_counters const& before,
    larch::chart_spr_search_counters const& after,
    dense_scoring_expectations const& expected,
    std::size_t expected_row_scratch_growths) {
#define CHECK_COUNTER_DELTA(field, value)         \
  do {                                            \
    CHECK(after.field >= before.field);           \
    CHECK(after.field - before.field == (value)); \
  } while (false)

  CHECK_COUNTER_DELTA(candidate_batches_scored, 1);
  CHECK_COUNTER_DELTA(local_candidate_scores, k_scored_candidate_count);
  CHECK_COUNTER_DELTA(local_rows_recomputed, expected.row_visits);
  CHECK_COUNTER_DELTA(local_unit_fitch_fast_path_productions_scored,
                      expected.fast_path_productions);
  CHECK_COUNTER_DELTA(local_leaf_state_view_uses,
                      k_scored_candidate_count * k_active_pattern_count);
  CHECK_COUNTER_DELTA(local_leaf_state_owned_copies, 0);
  CHECK_COUNTER_DELTA(local_row_scratch_capacity_growths,
                      expected_row_scratch_growths);

  // Reusable storage must not turn into reusable semantics: every candidate
  // still compiles and validates its candidate-local descriptor, and every
  // candidate x pattern visit still consumes that descriptor.
  CHECK_COUNTER_DELTA(chart_execution_plan_builds, 0);
  CHECK_COUNTER_DELTA(chart_execution_plan_cache_hits,
                      k_scored_candidate_count);
  CHECK_COUNTER_DELTA(candidate_execution_plan_builds,
                      k_scored_candidate_count);
  CHECK_COUNTER_DELTA(candidate_execution_plan_cache_hits,
                      k_scored_candidate_count * k_active_pattern_count);
  CHECK_COUNTER_DELTA(candidate_partition_validations,
                      expected.candidate_partition_validations);
  CHECK_COUNTER_DELTA(clade_order_sorts, k_scored_candidate_count);
  CHECK_COUNTER_DELTA(production_descriptors_compiled,
                      expected.production_descriptors);
  CHECK_COUNTER_DELTA(plan_mismatch_rejections, 0);

  CHECK_COUNTER_DELTA(overlay_reachability_validations,
                      k_scored_candidate_count);
  CHECK_COUNTER_DELTA(reachable_clades_traversed, expected.reachable_clades);
  CHECK_COUNTER_DELTA(reachable_productions_traversed,
                      expected.reachable_productions);
  CHECK_COUNTER_DELTA(reachable_temp_clades_traversed,
                      expected.reachable_temp_clades);
  CHECK_COUNTER_DELTA(reachable_temp_productions_traversed,
                      expected.reachable_temp_productions);
  CHECK_COUNTER_DELTA(reachability_full_grammar_like_passes,
                      expected.full_grammar_like_reachability_passes);

  // These counters make the zero-allocation result non-vacuous: it must be the
  // dense local recurrence, not a hidden full validation, materializer,
  // diagnostic oracle, pattern-batch path, or parallel dispatch.
  CHECK_COUNTER_DELTA(full_grammar_validations, 0);
  CHECK_COUNTER_DELTA(production_index_validations, 0);
  CHECK_COUNTER_DELTA(production_partition_validations, 0);
  CHECK_COUNTER_DELTA(dynamic_overlay_payload_partition_validations, 0);
  CHECK_COUNTER_DELTA(candidate_pattern_full_grammar_validations, 0);
  CHECK_COUNTER_DELTA(candidate_pattern_partition_validations, 0);
  CHECK_COUNTER_DELTA(candidate_pattern_clade_order_sorts, 0);
  CHECK_COUNTER_DELTA(full_overlay_materializations, 0);
  CHECK_COUNTER_DELTA(overlay_materializations_for_oracle, 0);
  CHECK_COUNTER_DELTA(overlay_materializations_for_local_scoring_bridge, 0);
  CHECK_COUNTER_DELTA(overlay_materializations_for_exact_verification, 0);
  CHECK_COUNTER_DELTA(overlay_materializations_for_accept_materialization, 0);
  CHECK_COUNTER_DELTA(overlay_materializations_for_final_compaction, 0);
  CHECK_COUNTER_DELTA(full_composite_rebuilds, 0);
  CHECK_COUNTER_DELTA(pattern_batch_cache_builds, 0);
  CHECK_COUNTER_DELTA(multifurcation_productions_scored, 0);
  CHECK_COUNTER_DELTA(local_score_parallel_batches, 0);
  CHECK_COUNTER_DELTA(local_score_worker_tasks, 0);
  CHECK_COUNTER_DELTA(exact_verifications, 0);

#undef CHECK_COUNTER_DELTA
}

static void check_warmed_high_water_storage_is_allocation_free(
    larch::chart_spr_search_state const& state,
    std::span<larch::grammar_spr_candidate const> frozen_candidates,
    larch::checked_chart_execution_plan_ref const& checked) {
  CHECK(!frozen_candidates.empty());
  auto plain = frozen_candidates.front();
  CHECK(!plain.added_clades.empty());
  CHECK(!plain.added_productions.empty());

  // Give every nested family a non-vacuous high-water shape. Grammar-local
  // scoring does not consume witness provenance, but both the resident delta
  // and acceptance candidate copy must preserve it without reallocating.
  auto rich = plain;
  std::size_t witness_children = 0;
  std::size_t witness_edges = 0;
  for (std::size_t production_index = 0;
       production_index < rich.added_productions.size(); ++production_index) {
    auto& production = rich.added_productions[production_index];
    CHECK(!production.children.empty());
    production.witnesses.clear();
    for (std::size_t witness_index = 0; witness_index < 2; ++witness_index) {
      larch::production_witness witness;
      witness.parent_node = 1000 + 10 * production_index + witness_index;
      for (std::size_t child_index = 0;
           child_index < production.children.size(); ++child_index) {
        larch::production_child_witness child;
        child.child = production.children[child_index].id;
        child.edge_alternatives = {
            10000 + 100 * production_index + 10 * witness_index + child_index,
            20000 + 100 * production_index + 10 * witness_index + child_index,
        };
        witness_edges += child.edge_alternatives.size();
        witness.children.push_back(std::move(child));
        ++witness_children;
      }
      production.witnesses.push_back(std::move(witness));
    }
  }
  CHECK(witness_children > 0);
  CHECK(witness_edges > 0);
  rich.source_tree_move = larch::spr_move{.src = 1, .dst = 2, .lca = 3};
  rich.source_before_topology_productions = rich.removed_productions;
  rich.source_after_topology_productions.emplace();
  for (std::size_t i = 0; i < rich.added_productions.size(); ++i) {
    rich.source_after_topology_productions->push_back(
        larch::temp_production_ref(static_cast<larch::production_id>(i)));
  }
  CHECK(rich.source_before_topology_productions.has_value());
  CHECK(rich.source_after_topology_productions.has_value());

  auto shallow = rich;
  for (auto& production : shallow.added_productions) {
    for (auto& witness : production.witnesses) witness.children.clear();
  }
  larch::grammar_spr_candidate identity;

  larch::spr_overlay_delta delta;
  larch::spr_overlay_delta_build_scratch build_scratch;
  auto build = [&](larch::grammar_spr_candidate const& candidate) {
    larch::build_spr_overlay_delta_from_resident_plan_into(
        state.grammar, checked, candidate, delta, build_scratch);
    CHECK(build_scratch.operation_boundary_clean());
  };
  auto build_cycle = [&] {
    build(rich);
    build(plain);
    build(rich);
    build(shallow);
    build(rich);
    build(identity);
    build(rich);
  };
  build_cycle();
  allocation_test::allocation_observer build_observer;
  {
    allocation_test::scoped_allocation_observation observation{build_observer};
    build_cycle();
  }
  CHECK(build_observer.statistics == allocation_test::allocation_statistics{});
  check_statistics(build_observer, 0, 0, 0, 0, 0);
  CHECK(delta.temp_productions.size() == rich.added_productions.size());
  CHECK(delta.temp_productions.front().witnesses.size() == 2);
  CHECK(!delta.temp_productions.front().witnesses.front().children.empty());

  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      acceptance_workspace;
  acceptance_workspace.reserve_batch(1);
  auto copy = [&](larch::grammar_spr_candidate const& candidate) {
    acceptance_workspace.begin_iteration();
    acceptance_workspace.append_candidate(candidate);
    CHECK(acceptance_workspace.candidates().size() == 1);
    acceptance_workspace.finish_batch();
  };
  auto copy_cycle = [&] {
    copy(rich);
    copy(plain);
    copy(rich);
    copy(shallow);
    copy(rich);
    copy(identity);
    copy(rich);
  };
  copy_cycle();
  allocation_test::allocation_observer copy_observer;
  {
    allocation_test::scoped_allocation_observation observation{copy_observer};
    copy_cycle();
  }
  CHECK(copy_observer.statistics == allocation_test::allocation_statistics{});
  check_statistics(copy_observer, 0, 0, 0, 0, 0);
  auto const& copied = acceptance_workspace.candidate_slots.front();
  CHECK(copied.added_productions.size() == rich.added_productions.size());
  CHECK(copied.added_productions.front().witnesses.size() == 2);
  CHECK(copied.source_tree_move.has_value());
  CHECK(copied.source_tree_move->src == rich.source_tree_move->src);
  CHECK(copied.source_tree_move->dst == rich.source_tree_move->dst);
  CHECK(copied.source_tree_move->lca == rich.source_tree_move->lca);
  CHECK(copied.source_tree_move->score_change ==
        rich.source_tree_move->score_change);
  CHECK(copied.source_before_topology_productions ==
        rich.source_before_topology_productions);
  CHECK(copied.source_after_topology_productions ==
        rich.source_after_topology_productions);
}

static void test_warmed_dense_local_scoring_is_allocation_free() {
  std::println("test_warmed_dense_local_scoring_is_allocation_free");

  CHECK(raw_file_sha256(k_dense_fixture_path) == k_dense_fixture_sha256);
  auto dag = larch::load_proto_dag(k_dense_fixture_path);
  larch::recompute_compact_genomes(dag);
  larch::set_sample_ids_from_cg(dag);

  larch::polytomy_refinement_options refinement_options;
  refinement_options.mode = larch::polytomy_mode::expand_soft_bounded;
  refinement_options.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, refinement_options);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "chart SPR allocation gate");

  larch::chart_spr_search_options search_options;
  search_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  search_options.cache.memory_budget_bytes = k_chart_memory_budget_bytes;
  search_options.cache.use_lazy_multisite_chart = false;
  auto state = larch::build_chart_spr_search_state(
      dag, std::move(refinement.grammar), search_options);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::all_active_patterns);
  CHECK(state.grammar.clades.size() == 139);
  CHECK(state.grammar.productions.size() == 69);
  CHECK(state.active_patterns.patterns.patterns.size() ==
        k_active_pattern_count);
  CHECK(state.pattern_charts.size() == k_active_pattern_count);
  CHECK(state.effective_pattern_batch_size == k_active_pattern_count);
  CHECK(state.skipped_invariant_site_count == 536);
  CHECK(state.composite_lower_bound_with_invariants == 174);

  // All grammar/plan/pattern inputs are constructed before observation.  A
  // finite auto-policy budget of one byte must fail closed in the resolver's
  // preflight, before copying even one pilot pattern or constructing any lazy
  // chart/grouping payload.  The global replaceable-new observer makes that
  // ordering non-tautological: the measured resolver region allocates zero
  // bytes and reports zero pilot builds.
  larch::chart_cache_options preflight_cache;
  preflight_cache.lazy_policy = larch::chart_spr_lazy_policy::automatic;
  preflight_cache.memory_budget_bytes = 1;
  larch::chart_spr_lazy_policy_diagnostics preflight_rejected;
  allocation_test::allocation_observer preflight_observer;
  {
    allocation_test::scoped_allocation_observation observation{
        preflight_observer};
    preflight_rejected = larch::resolve_chart_spr_lazy_policy(
        state.execution_plan, state.active_patterns, state.chart_opts,
        preflight_cache, state.estimated_full_pattern_cache_bytes);
  }
  CHECK(preflight_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(preflight_observer, 0, 0, 0, 0, 0);
  CHECK(preflight_rejected.resolved == larch::chart_spr_lazy_policy::off);
  CHECK(preflight_rejected.reason ==
        larch::chart_spr_lazy_policy_reason::full_lazy_budget_exceeded);
  CHECK(preflight_rejected.pilot_pattern_count == 0);
  CHECK(preflight_rejected.pilot_inside_chart_builds == 0);
  CHECK(preflight_rejected.pilot_outside_chart_builds == 0);
  CHECK(preflight_rejected.pilot_exact_builds == 0);
  CHECK(preflight_rejected.pilot_scheduler_submissions == 0);

  larch::grammar_spr_enumeration_options enumeration;
  enumeration.max_candidates = k_frozen_candidate_count;
  enumeration.max_candidates_is_post_dedup = true;
  std::vector<larch::grammar_spr_candidate> frozen_candidates;
  frozen_candidates.reserve(k_frozen_candidate_count);
  auto generation = larch::for_each_grammar_spr_candidate(
      state.grammar, enumeration,
      [&](larch::grammar_spr_candidate const& candidate) {
        frozen_candidates.push_back(candidate);
        return true;
      });
  CHECK(generation.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(generation.candidates_generated_after_dedup ==
        k_frozen_candidate_count);
  CHECK(frozen_candidates.size() == k_frozen_candidate_count);
  CHECK(candidate_signature_sha256(state.grammar, frozen_candidates) ==
        k_dense_candidate_signature_sha256);

  auto const expected = k_frozen_dense_scoring_expectations;
  std::size_t compatibility_checks = 0;
  std::size_t fingerprint_scans = 0;
  std::size_t legacy_index_validations = 0;
  std::size_t dynamic_partition_validations = 0;
  larch::chart_execution_plan_detail::compatibility_work_observer
      compatibility_observer{&compatibility_checks, &fingerprint_scans,
                             &legacy_index_validations,
                             &dynamic_partition_validations};
  allocation_test::allocation_observer fingerprint_observer;
  auto checked = [&] {
    allocation_test::scoped_allocation_observation allocation_scope{
        fingerprint_observer};
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility_scope{&compatibility_observer};
    return larch::check_chart_execution_plan(state.grammar,
                                             state.execution_plan);
  }();
  CHECK(fingerprint_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(fingerprint_observer, 0, 0, 0, 0, 0);
  CHECK(compatibility_checks == 1);
  CHECK(fingerprint_scans == 1);
  CHECK(legacy_index_validations == 0);
  CHECK(dynamic_partition_validations == 0);

  auto long_id_grammar = state.grammar;
  for (std::size_t taxon = 0;
       taxon < long_id_grammar.taxa.id_to_sample_id.size(); ++taxon) {
    long_id_grammar.taxa.id_to_sample_id[taxon] =
        std::string(5000, static_cast<char>('a' + taxon % 26)) + "-" +
        std::to_string(taxon);
  }
  long_id_grammar.taxa.sample_id_to_id.clear();
  long_id_grammar.taxa.sample_id_to_id.reserve(
      long_id_grammar.taxa.id_to_sample_id.size());
  for (std::size_t taxon = 0;
       taxon < long_id_grammar.taxa.id_to_sample_id.size(); ++taxon) {
    long_id_grammar.taxa.sample_id_to_id.emplace(
        long_id_grammar.taxa.id_to_sample_id[taxon],
        static_cast<larch::taxon_id>(taxon));
  }
  auto long_id_plan = larch::build_chart_execution_plan(long_id_grammar);
  allocation_test::allocation_observer long_id_fingerprint_observer;
  {
    allocation_test::scoped_allocation_observation allocation_scope{
        long_id_fingerprint_observer};
    (void)larch::check_chart_execution_plan(long_id_grammar, long_id_plan);
  }
  CHECK(long_id_fingerprint_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(long_id_fingerprint_observer, 0, 0, 0, 0, 0);
  check_warmed_high_water_storage_is_allocation_free(state, frozen_candidates,
                                                     checked);
  auto owning_scores =
      larch::score_candidates_locally(state, frozen_candidates, {}, 1, checked);
  CHECK(owning_scores.size() == k_frozen_candidate_count);
  auto owning_score_span =
      std::span<larch::chart_spr_candidate_score const>{owning_scores};
  for (std::size_t i = 0; i < owning_scores.size(); ++i) {
    check_score_against_frozen(owning_scores[i], i);
  }
  CHECK(local_score_tuple_sha256(owning_score_span) ==
        k_dense_local_score_tuple_sha256);

  std::vector<larch::grammar_spr_candidate> candidates;
  candidates.reserve(k_scored_candidate_count);
  for (std::size_t i = 0; i < k_scored_candidate_count; ++i) {
    candidates.push_back(frozen_candidates[i % frozen_candidates.size()]);
  }
  CHECK(candidates.size() == k_scored_candidate_count);

  std::vector<larch::chart_spr_local_score_result> results(
      k_scored_candidate_count);
  larch::chart_spr_local_score_workspace workspace;
  larch::local_spr_score_options local_options;
  auto candidate_span =
      std::span<larch::grammar_spr_candidate const>{candidates};
  auto result_span = std::span<larch::chart_spr_local_score_result>{results};

  // One complete, unobserved pass reaches the maximum capacity required by
  // every candidate shape in the frozen cyclic order.  Its full counter and
  // semantic contract is checked before allocations become the gate.
  CHECK(workspace.operation_boundary_clean());
  auto warm_before = state.counters;
  larch::score_candidates_locally_into(state, candidate_span, result_span,
                                       workspace, local_options, 1, checked);
  auto warm_after = state.counters;
  CHECK(workspace.operation_boundary_clean());
  auto warm_row_scratch_growths =
      warm_after.local_row_scratch_capacity_growths -
      warm_before.local_row_scratch_capacity_growths;
  CHECK(warm_row_scratch_growths > 0);
  CHECK(warm_row_scratch_growths <= k_scored_candidate_count);
  check_dense_scoring_counter_delta(warm_before, warm_after, expected,
                                    warm_row_scratch_growths);
  check_all_local_results(results, owning_score_span);

  allocation_test::allocation_observer observer;
  for (std::size_t repetition = 0; repetition < k_observed_repetitions;
       ++repetition) {
    poison_local_results(results);
    CHECK(workspace.operation_boundary_clean());
    observer.reset();
    auto before = state.counters;
    {
      allocation_test::scoped_allocation_observation observation{observer};
      larch::score_candidates_locally_into(state, candidate_span, result_span,
                                           workspace, local_options, 1,
                                           checked);
    }
    auto after = state.counters;
    CHECK(workspace.operation_boundary_clean());

    // This is deliberately stronger than the Phase-2 historical 80% gate:
    // the reusable steady-state seam must make no dynamic allocation call of
    // any replaceable kind.  The separately sealed frozen-binary DHAT result
    // supplies the Phase-0 denominator for the historical comparison.
    CHECK(observer.statistics == allocation_test::allocation_statistics{});
    check_statistics(observer, 0, 0, 0, 0, 0);
    check_dense_scoring_counter_delta(before, after, expected, 0);
    check_all_local_results(results, owning_score_span);
  }

  std::println("  PASS");
}

static void test_published_state_estimators_are_allocation_free() {
  std::println("test_published_state_estimators_are_allocation_free");

  larch::chart_spr_search_state all_active;
  all_active.cache_strategy =
      larch::chart_spr_cache_strategy::all_active_patterns;
  all_active.pattern_charts.reserve(3);
  all_active.resident_pattern_cache_bytes =
      larch::estimate_chart_spr_pattern_cache_bytes(all_active);

  larch::chart_spr_search_state lazy;
  lazy.cache_strategy = larch::chart_spr_cache_strategy::lazy_multisite_chart;
  lazy.lazy_chart.emplace();
  lazy.lazy_chart->inside_rows_by_clade.reserve(5);
  lazy.exact_trim_active_only.emplace();
  lazy.exact_trim_active_only->frontier_sizes_by_clade.reserve(7);
  lazy.resident_pattern_cache_bytes =
      larch::estimate_chart_spr_pattern_cache_bytes(lazy);

  larch::chart_spr_search_state pattern_batch;
  pattern_batch.cache_strategy =
      larch::chart_spr_cache_strategy::pattern_batches;
  pattern_batch.resident_pattern_cache_bytes = 4096;

  std::size_t checksum = 0;
  allocation_test::allocation_observer observer;
  {
    allocation_test::scoped_allocation_observation observation{observer};
    for (auto const* state : {&all_active, &lazy, &pattern_batch}) {
      checksum += larch::estimate_chart_spr_state_core_resident_bytes(*state);
      checksum +=
          larch::estimate_chart_spr_selected_cache_dynamic_resident_bytes(
              *state);
      checksum +=
          larch::estimate_chart_spr_published_state_resident_bytes(*state);
    }
    checksum += larch::estimate_chart_spr_trim_dynamic_resident_bytes(
        *lazy.exact_trim_active_only);
    checksum += larch::estimate_chart_spr_trim_resident_bytes(
        *lazy.exact_trim_active_only);
  }
  CHECK(checksum > 0);
  CHECK(observer.statistics == allocation_test::allocation_statistics{});
  check_statistics(observer, 0, 0, 0, 0, 0);

  // Production callbacks capture at most one raw pointer and pass the frozen
  // SBO guard. Exercise the same target shape under the allocation observer,
  // then publish its zero-byte persistent-target contract.
  larch::chart_spr_search_state callback_state;
  std::size_t callback_sentinel = 0;
  allocation_test::allocation_observer callback_observer;
  {
    allocation_test::scoped_allocation_observation observation{
        callback_observer};
    auto* sentinel_ptr = &callback_sentinel;
    callback_state.exact_setup_provider =
        [sentinel_ptr](larch::chart_spr_search_state const&,
                       larch::checked_chart_execution_plan_ref const&) {
          (void)sentinel_ptr;
          return larch::multisite_exact_setup{};
        };
    larch::declare_chart_spr_state_callback_target_resident_bytes(
        callback_state, 0);
    checksum +=
        larch::estimate_chart_spr_state_core_resident_bytes(callback_state);
  }
  CHECK(callback_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(callback_observer, 0, 0, 0, 0, 0);

  std::println("  PASS");
}

static void test_prepared_lazy_local_core_is_allocation_free() {
  std::println("test_prepared_lazy_local_core_is_allocation_free");
  auto dag = larch::load_proto_dag(k_dense_fixture_path);
  larch::recompute_compact_genomes(dag);
  larch::set_sample_ids_from_cg(dag);
  larch::polytomy_refinement_options refinement_options;
  refinement_options.mode = larch::polytomy_mode::expand_soft_bounded;
  refinement_options.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, refinement_options);

  larch::chart_spr_search_options search_options;
  search_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  search_options.cache.memory_budget_bytes = k_chart_memory_budget_bytes;
  search_options.cache.use_lazy_multisite_chart = true;
  auto state = larch::build_chart_spr_search_state(
      dag, std::move(refinement.grammar), search_options);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);

  std::vector<larch::grammar_spr_candidate> signature_candidates;
  larch::grammar_spr_enumeration_options enumeration;
  enumeration.max_candidates = 16;
  enumeration.max_candidates_is_post_dedup = true;
  (void)larch::for_each_grammar_spr_candidate(
      state.grammar, enumeration,
      [&](larch::grammar_spr_candidate const& value) {
        signature_candidates.push_back(value);
        return true;
      });
  CHECK(signature_candidates.size() == 16);
  auto const& candidate = signature_candidates.front();

  allocation_test::allocation_observer signature_observer;
  std::vector<std::size_t> signature_resident(signature_candidates.size());
  {
    allocation_test::scoped_allocation_observation observation{
        signature_observer};
    for (std::size_t index = 0; index < signature_candidates.size(); ++index) {
      signature_resident[index] = larch::chart_spr_search_detail::
          estimate_grammar_spr_enumeration_signature_live_bytes(
              state.grammar, signature_candidates[index]);
    }
  }
  CHECK(signature_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(signature_observer, 0, 0, 0, 0, 0);
  auto const signature_node_bytes =
      sizeof(std::set<std::string>::value_type) + 4 * sizeof(void*);
  for (std::size_t index = 0; index < signature_candidates.size(); ++index) {
    auto actual = larch::chart_spr_candidate_taxon_signature(
        state.grammar, signature_candidates[index]);
    CHECK(signature_resident[index] >=
          signature_node_bytes + actual.capacity() + 1);
    CHECK(signature_resident[index] ==
          signature_node_bytes +
              std::max(actual.size(), std::string{}.capacity()) + 1);
  }

  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  larch::chart_spr_search_detail::prepared_local_candidate_score prepared;
  larch::chart_spr_local_score_scratch scratch;
  larch::local_spr_score_options local_options;
  larch::chart_spr_search_detail::prepare_local_candidate_score_into(
      state, candidate, local_options, nullptr, checked, prepared);
  CHECK(prepared.valid_for_accumulation);
  (void)larch::chart_spr_search_detail::
      prepare_lazy_local_score_scratch_for_candidate(state, prepared.delta(),
                                                     scratch, nullptr);

  allocation_test::allocation_observer observer;
  {
    allocation_test::scoped_allocation_observation observation{observer};
    larch::chart_spr_search_detail::
        accumulate_prepared_local_candidate_lazy_prepared(
            state, prepared, local_options, nullptr, scratch, checked);
  }
  CHECK(observer.statistics == allocation_test::allocation_statistics{});
  check_statistics(observer, 0, 0, 0, 0, 0);
  CHECK(scratch.lazy_grouping_status.succeeded());
  CHECK(prepared.valid_for_accumulation);
  larch::chart_spr_search_detail::finalize_lazy_local_grouping_status(
      state, prepared, scratch);
  auto result =
      larch::chart_spr_search_detail::finish_prepared_local_candidate_score(
          state, prepared);
  CHECK(result.valid);
  scratch.clear_borrows();
  prepared.release_operation_borrows();
  CHECK(scratch.operation_boundary_clean());

  // The explicit-W1 unlimited seam retains the historical warmed
  // zero-allocation contract even for the lazy chart. A finite W1 operation
  // then uses coordinator admission; an impossible preflight must also throw
  // without allocating or reaching scheduled work.
  state.cache_opts.memory_budget_bytes = 0;
  std::array<larch::grammar_spr_candidate, 1> candidates{candidate};
  std::array<larch::chart_spr_local_score_result, 1> results;
  larch::chart_spr_local_score_workspace workspace;
  larch::local_spr_score_options unlimited_options;
  larch::score_candidates_locally_into(state, candidates, results, workspace,
                                       unlimited_options, 1, checked);
  CHECK(workspace.operation_boundary_clean());

  allocation_test::allocation_observer warmed_w1_observer;
  {
    allocation_test::scoped_allocation_observation observation{
        warmed_w1_observer};
    larch::score_candidates_locally_into(state, candidates, results, workspace,
                                         unlimited_options, 1, checked);
  }
  CHECK(warmed_w1_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(warmed_w1_observer, 0, 0, 0, 0, 0);
  CHECK(workspace.operation_boundary_clean());

  larch::local_spr_score_options impossible_options;
  impossible_options.admission_memory_budget_bytes = 1;
  CHECK(state.cache_opts.memory_budget_bytes == 0);
  CHECK(impossible_options.admission_memory_budget_bytes == 1);
  CHECK(larch::chart_spr_search_detail::
            effective_lazy_local_admission_budget_bytes(state,
                                                         impossible_options) ==
        1);
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(state) > 1);
  allocation_test::allocation_observer preflight_observer;
  bool preflight_failed = false;
  {
    allocation_test::scoped_allocation_observation observation{
        preflight_observer};
    try {
      larch::score_candidates_locally_into(state, candidates, results,
                                           workspace, impossible_options, 1,
                                           checked);
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_budget_error const&) {
      preflight_failed = true;
    }
  }
  CHECK(preflight_failed);
  CHECK(preflight_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(preflight_observer, 0, 0, 0, 0, 0);
  CHECK(workspace.operation_boundary_clean());

  // Every compatibility wrapper whose outer scheduler/result/promotion or
  // fingerprint ownership is not part of the finite envelope fails before
  // allocating. The checked caller-owned W1 seam above and the production
  // seam below are the deliberately supported families.
  larch::chart_scheduler api_scheduler{
      larch::chart_scheduler_options{.requested_workers = 2}};
  std::vector<larch::grammar_spr_candidate> owning_candidates{candidate};
  auto expect_finite_api_rejection = [&](auto expected_api, auto&& call) {
    allocation_test::allocation_observer api_observer;
    bool rejected = false;
    {
      allocation_test::scoped_allocation_observation observation{api_observer};
      try {
        call();
      } catch (larch::chart_spr_search_detail::
                   chart_spr_lazy_local_finite_api_error const& e) {
        rejected = true;
        CHECK(e.api() == expected_api);
      }
    }
    CHECK(rejected);
    CHECK(api_observer.statistics == allocation_test::allocation_statistics{});
    check_statistics(api_observer, 0, 0, 0, 0, 0);
  };
  using finite_api =
      larch::chart_spr_search_detail::chart_spr_lazy_local_finite_api;
  expect_finite_api_rejection(finite_api::unchecked_scheduler_into, [&] {
    larch::score_candidates_locally_into(state, candidates, results, workspace,
                                         impossible_options, api_scheduler);
  });
  expect_finite_api_rejection(finite_api::unchecked_worker_count_into, [&] {
    larch::score_candidates_locally_into(state, candidates, results, workspace,
                                         impossible_options, 1);
  });
  expect_finite_api_rejection(finite_api::checked_worker_count_parallel_into,
                              [&] {
                                larch::score_candidates_locally_into(
                                    state, candidates, results, workspace,
                                    impossible_options, 2, checked);
                              });
  expect_finite_api_rejection(finite_api::checked_owning, [&] {
    (void)larch::score_candidates_locally(
        state, owning_candidates, impossible_options, 1, checked);
  });
  expect_finite_api_rejection(finite_api::unchecked_owning, [&] {
    (void)larch::score_candidates_locally(state, owning_candidates,
                                          impossible_options, 1);
  });
  expect_finite_api_rejection(finite_api::unchecked_singleton, [&] {
    (void)larch::score_candidate_locally(state, candidate,
                                         impossible_options);
  });
  api_scheduler.shutdown();

  // A finite W1 multiwave call retains the first task's descriptor/grouping
  // capacity until its later waves finish. Replaying the identical candidate
  // seven more times therefore adds at most seven independently bounded
  // output-diagnostic reserves, not coordinator/task allocations; the public
  // return releases the task payload.
  std::array<larch::grammar_spr_candidate, 8> repeated_candidates;
  repeated_candidates.fill(candidate);
  std::array<larch::chart_spr_local_score_result, 8> repeated_results;
  larch::local_spr_score_options generous_options;
  generous_options.admission_memory_budget_bytes = k_chart_memory_budget_bytes;
  larch::chart_spr_local_score_workspace singleton_finite_workspace;
  allocation_test::allocation_observer singleton_finite_observer;
  {
    allocation_test::scoped_allocation_observation observation{
        singleton_finite_observer};
    larch::score_candidates_locally_into(
        state,
        std::span<larch::grammar_spr_candidate const>{repeated_candidates}
            .first(1),
        std::span<larch::chart_spr_local_score_result>{repeated_results}.first(
            1),
        singleton_finite_workspace, generous_options, 1, checked);
  }
  larch::chart_spr_local_score_workspace multiwave_finite_workspace;
  allocation_test::allocation_observer multiwave_finite_observer;
  auto const reused_before = state.counters.lazy_local_reused_prepared_tasks;
  {
    allocation_test::scoped_allocation_observation observation{
        multiwave_finite_observer};
    larch::score_candidates_locally_into(
        state,
        std::span<larch::grammar_spr_candidate const>{repeated_candidates},
        std::span<larch::chart_spr_local_score_result>{repeated_results},
        multiwave_finite_workspace, generous_options, 1, checked);
  }
  CHECK(multiwave_finite_observer.statistics.calls >=
        singleton_finite_observer.statistics.calls);
  auto const extra_calls = multiwave_finite_observer.statistics.calls -
                           singleton_finite_observer.statistics.calls;
  auto const extra_bytes =
      multiwave_finite_observer.statistics.requested_bytes -
      singleton_finite_observer.statistics.requested_bytes;
  CHECK(extra_calls <= repeated_results.size() - 1);
  CHECK(extra_bytes ==
        extra_calls * larch::chart_spr_search_detail::
                          lazy_local_invalid_reason_owned_capacity_bound());
  CHECK(state.counters.lazy_local_reused_prepared_tasks - reused_before == 7);
  CHECK(multiwave_finite_workspace.operation_boundary_clean());

  // The production finite-grammar boundary must reject an impossible global
  // envelope before rank/batch/enumerator or cold local-slot allocation.
  larch::chart_scheduler production_scheduler{
      larch::chart_scheduler_options{.requested_workers = 4}};
  larch::chart_spr_search_options production_options;
  production_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  production_options.cache.use_lazy_multisite_chart = true;
  production_options.cache.memory_budget_bytes = 1;
  production_options.cache.candidate_batch_size = 4;
  production_options.enumeration.max_candidates = 4;
  production_options.enumeration.max_candidates_is_post_dedup = true;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      production_workspace;
  allocation_test::allocation_observer production_preflight_observer;
  bool production_preflight_failed = false;
  {
    allocation_test::scoped_allocation_observation observation{
        production_preflight_observer};
    try {
      (void)larch::run_chart_spr_acceptance_iteration(
          state, std::move(production_options), 0, production_workspace,
          production_scheduler);
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_budget_error const&) {
      production_preflight_failed = true;
    }
  }
  CHECK(production_preflight_failed);
  CHECK(production_preflight_observer.statistics ==
        allocation_test::allocation_statistics{});
  check_statistics(production_preflight_observer, 0, 0, 0, 0, 0);
  CHECK(production_workspace.candidate_slots.capacity() == 0);
  CHECK(production_workspace.candidate_copy_scratch.capacity() == 0);
  CHECK(production_workspace.local_results.capacity() == 0);
  CHECK(production_workspace.local_score.operation_boundary_clean());
  production_scheduler.shutdown();
  std::println("  PASS");
}

int main() {
  test_complete_replaceable_overload_matrix();
  test_new_expressions_and_over_alignment();
  test_scoped_unscoped_nested_and_output_exclusion();
  test_forced_failure_and_new_handler_semantics();
  test_thread_local_isolation();
  test_published_state_estimators_are_allocation_free();
  test_prepared_lazy_local_core_is_allocation_free();
  test_warmed_dense_local_scoring_is_allocation_free();

  std::println(
      "All chart-SPR allocation observer and dense scoring tests passed!");
  return 0;
}

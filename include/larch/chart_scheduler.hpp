#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace larch {

namespace chart_scheduler_detail {

// A synchronous, non-owning callable view.  Unlike std::function it cannot
// allocate, which keeps the persistent scheduler's serial hot path allocation
// free after construction.
template <typename Signature>
class function_ref;

template <typename Result, typename... Args>
class function_ref<Result(Args...)> {
 public:
  function_ref() noexcept = default;

  template <typename Callable>
    requires(!std::is_same_v<std::remove_cvref_t<Callable>, function_ref>)
  function_ref(Callable& callable) noexcept
      : object_(std::addressof(callable)),
        invoke_([](void* object, Args... args) -> Result {
          return std::invoke(*static_cast<Callable*>(object),
                             std::forward<Args>(args)...);
        }) {}

  Result operator()(Args... args) const {
    return invoke_(object_, std::forward<Args>(args)...);
  }

  explicit operator bool() const noexcept { return invoke_ != nullptr; }

 private:
  void* object_ = nullptr;
  Result (*invoke_)(void*, Args...) = nullptr;
};

}  // namespace chart_scheduler_detail

// Worker resolution is deliberately separate from scheduler construction.  It
// makes the platform fallbacks observable and lets tests exercise every policy
// without depending on the topology of the machine running the test.
struct chart_worker_topology_snapshot {
  // Counts are restricted to CPUs in the process' current affinity mask.
  std::optional<std::size_t> affinity_logical_cpu_count;
  // Present only when every affinity CPU has a readable package/core identity.
  std::optional<std::size_t> affinity_physical_core_count;
  std::size_t hardware_thread_count = 0;
};

enum class chart_worker_resolution_policy {
  explicit_count,
  affinity_physical_cores,
  affinity_logical_cpus,
  hardware_concurrency,
  serial_fallback,
};

struct chart_worker_resolution {
  std::size_t requested_workers = 0;  // zero means automatic
  std::size_t resolved_workers = 1;
  chart_worker_resolution_policy policy =
      chart_worker_resolution_policy::serial_fallback;
  chart_worker_topology_snapshot topology;
};

[[nodiscard]] chart_worker_topology_snapshot
detect_chart_worker_topology() noexcept;

[[nodiscard]] chart_worker_resolution resolve_chart_worker_count(
    std::size_t requested_workers,
    chart_worker_topology_snapshot const& topology);

[[nodiscard]] chart_worker_resolution resolve_chart_worker_count(
    std::size_t requested_workers = 0);

[[nodiscard]] std::string_view chart_worker_resolution_policy_name(
    chart_worker_resolution_policy policy) noexcept;

struct chart_scheduler_options {
  std::size_t requested_workers = 0;

  // A default of 64 makes a small 64-item batch a serial range.  Later chart
  // phases can override this per call after supplying a meaningful work axis.
  std::size_t default_minimum_grain = 64;
  // The adaptive grain bounds the number of ranges independently of input
  // length, avoiding an unbounded future/task fan-out.
  std::size_t default_target_ranges_per_worker = 4;
};

struct chart_indexed_range_options {
  // Zero inherits the scheduler default.
  std::size_t minimum_grain = 0;
  std::size_t target_ranges_per_worker = 0;
  bool force_serial = false;
};

struct chart_indexed_range_plan {
  std::size_t item_count = 0;
  std::size_t effective_grain = 1;
  std::size_t range_count = 0;
  std::size_t worker_task_limit = 0;
  bool force_serial = false;
};

struct chart_indexed_range {
  // IDs are allocated monotonically by one scheduler and never depend on which
  // worker happens to claim the range.
  std::uint64_t task_id = 0;
  std::size_t range_index = 0;
  std::size_t begin = 0;
  std::size_t end = 0;
};

class chart_scheduler_cancellation_token {
 public:
  [[nodiscard]] bool stop_requested() const noexcept {
    return cancelled_ != nullptr && cancelled_->load(std::memory_order_acquire);
  }

  void request_cancel() const noexcept {
    if (cancelled_ != nullptr) {
      cancelled_->store(true, std::memory_order_release);
    }
  }

 private:
  explicit chart_scheduler_cancellation_token(
      std::atomic<bool>& cancelled) noexcept
      : cancelled_(&cancelled) {}

  std::atomic<bool>* cancelled_ = nullptr;

  friend class chart_scheduler;
};

enum class chart_scheduler_serial_reason {
  none,
  empty,
  forced,
  one_resolved_worker,
  one_range,
  nested_same_scheduler,
};

[[nodiscard]] std::string_view chart_scheduler_serial_reason_name(
    chart_scheduler_serial_reason reason) noexcept;

struct chart_scheduler_run_summary {
  std::uint64_t operation_id = 0;
  std::uint64_t first_task_id = 0;
  std::size_t item_count = 0;
  std::size_t range_count = 0;
  std::size_t ranges_completed = 0;
  std::size_t ranges_cancelled = 0;
  std::size_t effective_grain = 1;
  std::size_t worker_tasks_submitted = 0;
  std::size_t active_workers = 0;
  chart_scheduler_serial_reason serial_reason =
      chart_scheduler_serial_reason::none;
  bool cancelled = false;
  // True once run_indexed_ranges selected the owning-pool path.  A failed
  // submission can enter that path yet submit zero runners, so task count
  // alone cannot reconcile the operation with parallel_operations.
  bool parallel_branch_entered = false;
  // Set only on the optional summary published while an operation unwinds.
  // Successful returned summaries keep this false.
  bool failed = false;

  [[nodiscard]] bool used_parallel_workers() const noexcept {
    return parallel_branch_entered || worker_tasks_submitted > 0;
  }
};

struct chart_scheduler_metrics {
  std::size_t requested_workers = 0;
  std::size_t resolved_workers = 1;
  chart_worker_resolution_policy worker_policy =
      chart_worker_resolution_policy::serial_fallback;

  std::uint64_t operations = 0;
  std::uint64_t parallel_operations = 0;
  std::uint64_t serial_fallbacks = 0;
  std::uint64_t nested_serial_fallbacks = 0;
  std::uint64_t rejected_concurrent_operations = 0;

  std::uint64_t ranges_created = 0;
  std::uint64_t ranges_completed = 0;
  std::uint64_t ranges_cancelled = 0;
  std::uint64_t tasks_submitted = 0;
  std::uint64_t tasks_completed = 0;
  std::uint64_t tasks_joined = 0;

  std::uint64_t queue_wait_nanoseconds = 0;
  std::uint64_t queue_wait_nanoseconds_max = 0;
  std::uint64_t queue_wait_samples = 0;
  std::size_t active_worker_high_water = 0;
  std::size_t last_active_workers = 0;
  std::size_t minimum_effective_grain = 0;
  std::size_t maximum_effective_grain = 0;
  std::size_t last_effective_grain = 0;

  std::uint64_t pool_lifetimes = 0;
  std::uint64_t pool_lifetimes_stopped = 0;
  std::size_t live_pool_threads = 0;
  std::uint64_t pending_tasks = 0;
  std::uint64_t pending_tasks_at_shutdown = 0;
  bool shutdown = false;
};

class chart_scheduler_submit_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

namespace chart_scheduler_test_detail {
using before_submit_hook = std::function<void(std::size_t)>;
using after_submissions_hook =
    std::function<void(std::size_t, std::size_t, bool)>;
struct access;
}  // namespace chart_scheduler_test_detail

// One chart_scheduler is intended to live for one chart search.  Its backing
// thread_pool is constructed lazily on the first genuinely parallel operation
// and is retained until shutdown/destruction.
class chart_scheduler {
 public:
  explicit chart_scheduler(chart_scheduler_options options = {});
  chart_scheduler(chart_scheduler_options options,
                  chart_worker_topology_snapshot topology);
  ~chart_scheduler() noexcept;

  chart_scheduler(chart_scheduler const&) = delete;
  chart_scheduler& operator=(chart_scheduler const&) = delete;
  chart_scheduler(chart_scheduler&&) = delete;
  chart_scheduler& operator=(chart_scheduler&&) = delete;

  [[nodiscard]] chart_worker_resolution const& worker_resolution()
      const noexcept;
  [[nodiscard]] chart_indexed_range_plan plan_indexed_ranges(
      std::size_t item_count,
      chart_indexed_range_options options = {}) const noexcept;

  // Work receives (range, stable_slot_id, cancellation).  A dynamic worker
  // keeps one slot while claiming multiple ranges, and nested same-scheduler
  // fallback inherits the outer slot.  Phase-2 scratch can therefore be bound
  // to the slot without two concurrent runners sharing it.  Work may run
  // concurrently for disjoint ranges and must otherwise synchronize its own
  // shared state.  An unrelated concurrent top-level call is rejected rather
  // than racing one shared pool/search workspace.
  template <typename Work>
  chart_scheduler_run_summary for_each_indexed_range(
      std::size_t item_count, chart_indexed_range_options options,
      Work&& work,
      chart_scheduler_run_summary* failed_run_summary = nullptr) {
    auto plan = plan_indexed_ranges(item_count, options);
    auto work_object = std::forward<Work>(work);
    auto erased_callable =
        [&work_object](chart_indexed_range const& range, std::size_t slot_id,
                       chart_scheduler_cancellation_token const& token) {
          std::invoke(work_object, range, slot_id, token);
        };
    range_work erased{erased_callable};
    return run_indexed_ranges(plan, erased, {}, failed_run_summary);
  }

  template <typename Work>
  chart_scheduler_run_summary for_each_indexed_range(std::size_t item_count,
                                                     Work&& work) {
    return for_each_indexed_range(item_count, {}, std::forward<Work>(work));
  }

  // Each range creates one task-local value.  Reduction happens on the caller
  // in increasing range/task-ID order after every submitted worker task has
  // joined, so completion order cannot affect counters or results. Cooperative
  // cancellation suppresses reduction entirely and returns the unchanged
  // initial accumulator; partial work is never published.
  template <typename Accumulator, typename Work, typename Reduce>
  auto map_reduce_indexed_ranges(std::size_t item_count,
                                 chart_indexed_range_options options,
                                 Accumulator initial, Work&& work,
                                 Reduce&& reduce)
      -> std::pair<Accumulator, chart_scheduler_run_summary> {
    using local_type = std::remove_cvref_t<
        std::invoke_result_t<Work&, chart_indexed_range const&, std::size_t,
                             chart_scheduler_cancellation_token const&>>;
    static_assert(!std::is_void_v<local_type>,
                  "map_reduce work must return a task-local value");

    auto plan = plan_indexed_ranges(item_count, options);
    std::vector<std::optional<local_type>> locals(plan.range_count);
    auto work_object = std::forward<Work>(work);
    auto reduce_object = std::forward<Reduce>(reduce);

    auto erased_callable =
        [&work_object, &locals](
            chart_indexed_range const& range, std::size_t slot_id,
            chart_scheduler_cancellation_token const& token) {
          locals[range.range_index].emplace(
              std::invoke(work_object, range, slot_id, token));
        };
    range_work erased{erased_callable};
    auto finish_callable = [this, &initial, &locals, &reduce_object,
                            plan](chart_scheduler_run_summary const& summary) {
      for (std::size_t index = 0; index < locals.size(); ++index) {
        if (!locals[index].has_value()) continue;
        auto range = indexed_range_at(plan, summary.first_task_id, index);
        std::invoke(reduce_object, initial, std::move(*locals[index]), range);
      }
    };
    deterministic_finish finish{finish_callable};

    auto summary = run_indexed_ranges(plan, erased, finish);
    return {std::move(initial), summary};
  }

  template <typename Accumulator, typename Work, typename Reduce>
  auto map_reduce_indexed_ranges(std::size_t item_count, Accumulator initial,
                                 Work&& work, Reduce&& reduce)
      -> std::pair<Accumulator, chart_scheduler_run_summary> {
    return map_reduce_indexed_ranges(item_count, {}, std::move(initial),
                                     std::forward<Work>(work),
                                     std::forward<Reduce>(reduce));
  }

  [[nodiscard]] chart_scheduler_metrics metrics() const noexcept;

  // shutdown() is terminal and idempotent.  It rejects an active operation;
  // after successful shutdown, pending work and live pool threads are zero.
  void shutdown();

  [[nodiscard]] static std::size_t global_live_pool_threads() noexcept;

 private:
  using range_work = chart_scheduler_detail::function_ref<void(
      chart_indexed_range const&, std::size_t,
      chart_scheduler_cancellation_token const&)>;
  using deterministic_finish = chart_scheduler_detail::function_ref<void(
      chart_scheduler_run_summary const&)>;

  struct implementation;
  std::unique_ptr<implementation> impl_;

  chart_scheduler_run_summary run_indexed_ranges(
      chart_indexed_range_plan const& plan, range_work work,
      deterministic_finish finish,
      chart_scheduler_run_summary* failed_run_summary = nullptr);
  [[nodiscard]] static chart_indexed_range indexed_range_at(
      chart_indexed_range_plan const& plan, std::uint64_t first_task_id,
      std::size_t index) noexcept;
  void fail_submission_after_for_tests(std::size_t successful_submissions);
  void set_submission_hooks_for_tests(
      chart_scheduler_test_detail::before_submit_hook before,
      chart_scheduler_test_detail::after_submissions_hook after);

  friend struct chart_scheduler_test_detail::access;
};

namespace chart_scheduler_test_detail {

struct access {
  static void fail_submission_after(chart_scheduler& scheduler,
                                    std::size_t successful_submissions) {
    scheduler.fail_submission_after_for_tests(successful_submissions);
  }

  static void set_submission_hooks(chart_scheduler& scheduler,
                                   before_submit_hook before,
                                   after_submissions_hook after) {
    scheduler.set_submission_hooks_for_tests(std::move(before),
                                             std::move(after));
  }

  static void clear_submission_hooks(chart_scheduler& scheduler) {
    scheduler.set_submission_hooks_for_tests({}, {});
  }
};

}  // namespace chart_scheduler_test_detail

}  // namespace larch

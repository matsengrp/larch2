#include <larch/chart_scheduler.hpp>

#include <larch/thread_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <exception>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

namespace larch {
namespace {

constexpr auto no_submit_failure = std::numeric_limits<std::size_t>::max();

std::atomic<std::size_t> global_live_chart_pool_threads{0};

struct scheduler_thread_context {
  chart_scheduler* scheduler = nullptr;
  std::size_t slot_id = 0;
};

thread_local scheduler_thread_context current_scheduler_context;

class scheduler_thread_context_scope {
 public:
  scheduler_thread_context_scope(chart_scheduler& scheduler,
                                 std::size_t slot_id) noexcept
      : previous_(current_scheduler_context) {
    current_scheduler_context = {&scheduler, slot_id};
  }

  ~scheduler_thread_context_scope() { current_scheduler_context = previous_; }

 private:
  scheduler_thread_context previous_;
};

class atomic_flag_scope {
 public:
  atomic_flag_scope(std::atomic<bool>& flag, bool owns) noexcept
      : flag_(&flag), owns_(owns) {}

  ~atomic_flag_scope() {
    if (owns_) flag_->store(false, std::memory_order_release);
  }

 private:
  std::atomic<bool>* flag_;
  bool owns_;
};

template <typename T>
void atomic_update_max(std::atomic<T>& target, T value) noexcept {
  auto observed = target.load(std::memory_order_relaxed);
  while (observed < value && !target.compare_exchange_weak(
                                 observed, value, std::memory_order_relaxed,
                                 std::memory_order_relaxed)) {
  }
}

template <typename T>
void atomic_update_min(std::atomic<T>& target, T value) noexcept {
  auto observed = target.load(std::memory_order_relaxed);
  while (value < observed && !target.compare_exchange_weak(
                                 observed, value, std::memory_order_relaxed,
                                 std::memory_order_relaxed)) {
  }
}

std::size_t ceil_div(std::size_t numerator, std::size_t denominator) noexcept {
  if (numerator == 0) return 0;
  return numerator / denominator + (numerator % denominator != 0);
}

std::size_t saturating_multiply(std::size_t lhs, std::size_t rhs) noexcept {
  if (lhs == 0 || rhs == 0) return 0;
  if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
    return std::numeric_limits<std::size_t>::max();
  }
  return lhs * rhs;
}

std::optional<long long> read_topology_integer(std::string const& path) {
  std::ifstream input{path};
  long long value = 0;
  if (!(input >> value)) return std::nullopt;
  return value;
}

std::uint64_t allocate_monotonic_ids(std::atomic<std::uint64_t>& next,
                                     std::size_t count,
                                     std::string_view label) {
  if (count > std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error(std::string{label} + " ID space exhausted");
  }
  auto const increment = static_cast<std::uint64_t>(count);
  auto observed = next.load(std::memory_order_relaxed);
  for (;;) {
    if (observed > std::numeric_limits<std::uint64_t>::max() - increment) {
      throw std::overflow_error(std::string{label} + " ID space exhausted");
    }
    if (next.compare_exchange_weak(observed, observed + increment,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return observed;
    }
  }
}

}  // namespace

chart_worker_topology_snapshot detect_chart_worker_topology() noexcept {
  chart_worker_topology_snapshot result;
  result.hardware_thread_count = std::thread::hardware_concurrency();

#ifdef __linux__
  try {
    std::vector<int> cpus;
    auto configured = ::sysconf(_SC_NPROCESSORS_CONF);
    std::size_t cpu_capacity =
        configured > 0 ? static_cast<std::size_t>(configured) : 128;
    cpu_capacity = std::max<std::size_t>(cpu_capacity, 128);

    // Linux returns EINVAL when cpusetsize is too small for the kernel mask.
    // Grow dynamically so sparse/high-numbered affinity masks do not silently
    // fall back to hardware_concurrency merely because CPU_SETSIZE was small.
    for (;;) {
      auto const set_bytes = CPU_ALLOC_SIZE(cpu_capacity);
      auto* affinity = CPU_ALLOC(cpu_capacity);
      if (affinity == nullptr) return result;
      struct cpu_set_owner {
        cpu_set_t* value;
        ~cpu_set_owner() { CPU_FREE(value); }
      } owner{affinity};
      CPU_ZERO_S(set_bytes, affinity);

      if (::sched_getaffinity(0, set_bytes, affinity) == 0) {
        auto const count = CPU_COUNT_S(set_bytes, affinity);
        if (count > 0) cpus.reserve(static_cast<std::size_t>(count));
        for (std::size_t cpu = 0; cpu < cpu_capacity; ++cpu) {
          if (CPU_ISSET_S(cpu, set_bytes, affinity)) {
            cpus.push_back(static_cast<int>(cpu));
          }
        }
        break;
      }
      if (errno != EINVAL ||
          cpu_capacity > std::numeric_limits<std::size_t>::max() / 2) {
        return result;
      }
      cpu_capacity *= 2;
    }
    if (cpus.empty()) return result;
    result.affinity_logical_cpu_count = cpus.size();

    std::set<std::pair<long long, long long>> physical_cores;
    for (auto cpu : cpus) {
      auto const prefix =
          "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
      auto package = read_topology_integer(prefix + "physical_package_id");
      auto core = read_topology_integer(prefix + "core_id");
      if (!package.has_value() || !core.has_value()) {
        return result;
      }
      physical_cores.emplace(*package, *core);
    }
    if (!physical_cores.empty()) {
      result.affinity_physical_core_count = physical_cores.size();
    }
  } catch (...) {
    // The affinity count, when already observed, remains a better fallback than
    // hardware_concurrency.  Topology discovery itself is diagnostic and must
    // never prevent scheduler construction.
  }
#endif

  return result;
}

chart_worker_resolution resolve_chart_worker_count(
    std::size_t requested_workers,
    chart_worker_topology_snapshot const& topology) {
  chart_worker_resolution result;
  result.requested_workers = requested_workers;
  result.topology = topology;

  if (requested_workers != 0) {
    result.resolved_workers = requested_workers;
    result.policy = chart_worker_resolution_policy::explicit_count;
    return result;
  }

  if (topology.affinity_physical_core_count.value_or(0) != 0) {
    if (topology.affinity_logical_cpu_count.has_value() &&
        *topology.affinity_physical_core_count >
            *topology.affinity_logical_cpu_count) {
      throw std::invalid_argument(
          "chart scheduler: physical affinity cores exceed logical CPUs");
    }
    result.resolved_workers = *topology.affinity_physical_core_count;
    result.policy = chart_worker_resolution_policy::affinity_physical_cores;
    return result;
  }

  if (topology.affinity_logical_cpu_count.value_or(0) != 0) {
    result.resolved_workers = *topology.affinity_logical_cpu_count;
    result.policy = chart_worker_resolution_policy::affinity_logical_cpus;
    return result;
  }

  if (topology.hardware_thread_count != 0) {
    result.resolved_workers = topology.hardware_thread_count;
    result.policy = chart_worker_resolution_policy::hardware_concurrency;
    return result;
  }

  result.resolved_workers = 1;
  result.policy = chart_worker_resolution_policy::serial_fallback;
  return result;
}

chart_worker_resolution resolve_chart_worker_count(
    std::size_t requested_workers) {
  return resolve_chart_worker_count(requested_workers,
                                    detect_chart_worker_topology());
}

std::string_view chart_worker_resolution_policy_name(
    chart_worker_resolution_policy policy) noexcept {
  switch (policy) {
    case chart_worker_resolution_policy::explicit_count:
      return "explicit";
    case chart_worker_resolution_policy::affinity_physical_cores:
      return "affinity_physical_cores";
    case chart_worker_resolution_policy::affinity_logical_cpus:
      return "affinity_logical_cpus";
    case chart_worker_resolution_policy::hardware_concurrency:
      return "hardware_concurrency";
    case chart_worker_resolution_policy::serial_fallback:
      return "serial_fallback";
  }
  return "unknown";
}

std::string_view chart_scheduler_serial_reason_name(
    chart_scheduler_serial_reason reason) noexcept {
  switch (reason) {
    case chart_scheduler_serial_reason::none:
      return "none";
    case chart_scheduler_serial_reason::empty:
      return "empty";
    case chart_scheduler_serial_reason::forced:
      return "forced";
    case chart_scheduler_serial_reason::one_resolved_worker:
      return "one_resolved_worker";
    case chart_scheduler_serial_reason::one_range:
      return "one_range";
    case chart_scheduler_serial_reason::nested_same_scheduler:
      return "nested_same_scheduler";
  }
  return "unknown";
}

struct chart_scheduler::implementation {
  implementation(chart_scheduler_options requested_options,
                 chart_worker_topology_snapshot topology)
      : options(requested_options),
        resolution(
            resolve_chart_worker_count(options.requested_workers, topology)) {
    options.default_minimum_grain =
        std::max<std::size_t>(1, options.default_minimum_grain);
    options.default_target_ranges_per_worker =
        std::max<std::size_t>(1, options.default_target_ranges_per_worker);
  }

  chart_scheduler_options options;
  chart_worker_resolution resolution;
  std::optional<thread_pool> pool;
  std::atomic<std::size_t> live_pool_threads{0};

  std::atomic<bool> top_level_active{false};
  std::atomic<bool> is_shutdown{false};
  std::atomic<std::uint64_t> next_operation_id{1};
  std::atomic<std::uint64_t> next_task_id{1};

  std::atomic<std::uint64_t> operations{0};
  std::atomic<std::uint64_t> parallel_operations{0};
  std::atomic<std::uint64_t> serial_fallbacks{0};
  std::atomic<std::uint64_t> nested_serial_fallbacks{0};
  std::atomic<std::uint64_t> rejected_concurrent_operations{0};
  std::atomic<std::uint64_t> ranges_created{0};
  std::atomic<std::uint64_t> ranges_completed{0};
  std::atomic<std::uint64_t> ranges_cancelled{0};
  std::atomic<std::uint64_t> tasks_submitted{0};
  std::atomic<std::uint64_t> tasks_completed{0};
  std::atomic<std::uint64_t> tasks_joined{0};
  std::atomic<std::uint64_t> queue_wait_nanoseconds{0};
  std::atomic<std::uint64_t> queue_wait_nanoseconds_max{0};
  std::atomic<std::uint64_t> queue_wait_samples{0};
  std::atomic<std::size_t> active_worker_high_water{0};
  std::atomic<std::size_t> last_active_workers{0};
  std::atomic<std::size_t> minimum_effective_grain{
      std::numeric_limits<std::size_t>::max()};
  std::atomic<std::size_t> maximum_effective_grain{0};
  std::atomic<std::size_t> last_effective_grain{0};
  std::atomic<std::uint64_t> pool_lifetimes{0};
  std::atomic<std::uint64_t> pool_lifetimes_stopped{0};
  std::atomic<std::uint64_t> pending_tasks_at_shutdown{0};

  std::atomic<std::size_t> submit_failure_after{no_submit_failure};
  chart_scheduler_test_detail::before_submit_hook before_submit;
  chart_scheduler_test_detail::after_submissions_hook after_submissions;
};

std::size_t estimate_chart_scheduler_implementation_resident_bytes() {
  // One make_unique allocation owns the implementation. Two pointer widths
  // cover the frozen allocator's allocation header/alignment allowance.
  return sizeof(chart_scheduler) + sizeof(chart_scheduler::implementation) +
         2 * sizeof(void*);
}

std::size_t estimate_chart_scheduler_pool_owning_heap_bytes(
    std::size_t resolved_workers) {
  if (resolved_workers <= 1) return 0;

  auto checked_multiply = [](std::size_t lhs, std::size_t rhs,
                             std::string_view context) {
    if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
      throw std::overflow_error(std::string{context} + " byte overflow");
    }
    return lhs * rhs;
  };
  auto checked_add = [](std::size_t lhs, std::size_t rhs,
                        std::string_view context) {
    if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
      throw std::overflow_error(std::string{context} + " byte overflow");
    }
    return lhs + rhs;
  };

  // std::vector::reserve owns one jthread array. Every libstdc++ jthread also
  // owns stop-state and std::thread invocation-state allocations; eight
  // pointer widths per worker are a frozen conservative envelope for those
  // control blocks and their allocator metadata.
  auto total = checked_multiply(resolved_workers,
                                sizeof(std::jthread) + 8 * sizeof(void*),
                                "chart scheduler pool worker ownership");
  // The frozen deque uses an initial pointer map and a 512-byte element block.
  // Four pointer widths cover the two allocator allocation headers/alignment.
  constexpr auto deque_map_pointer_count = std::size_t{8};
  auto const queue_storage =
      512 + (deque_map_pointer_count + 4) * sizeof(void*);
  total =
      checked_add(total, queue_storage, "chart scheduler pool queue ownership");
  // The worker array itself is a third allocation.
  return checked_add(total, 2 * sizeof(void*),
                     "chart scheduler pool owning heap");
}

chart_scheduler::chart_scheduler(chart_scheduler_options options)
    : chart_scheduler(options, options.requested_workers == 0
                                   ? detect_chart_worker_topology()
                                   : chart_worker_topology_snapshot{}) {}

chart_scheduler::chart_scheduler(chart_scheduler_options options,
                                 chart_worker_topology_snapshot topology)
    : impl_(std::make_unique<implementation>(options, std::move(topology))) {}

chart_scheduler::~chart_scheduler() noexcept {
  if (!impl_) return;
  if (impl_->pool.has_value()) {
    impl_->pool.reset();
    auto const live =
        impl_->live_pool_threads.exchange(0, std::memory_order_relaxed);
    global_live_chart_pool_threads.fetch_sub(live, std::memory_order_relaxed);
    impl_->pool_lifetimes_stopped.fetch_add(1, std::memory_order_relaxed);
  }
  impl_->is_shutdown.store(true, std::memory_order_release);
}

chart_worker_resolution const& chart_scheduler::worker_resolution()
    const noexcept {
  return impl_->resolution;
}

chart_indexed_range_plan chart_scheduler::plan_indexed_ranges(
    std::size_t item_count,
    chart_indexed_range_options options) const noexcept {
  chart_indexed_range_plan plan;
  plan.item_count = item_count;
  plan.force_serial = options.force_serial;

  auto const minimum_grain = options.minimum_grain == 0
                                 ? impl_->options.default_minimum_grain
                                 : options.minimum_grain;
  auto const ranges_per_worker =
      options.target_ranges_per_worker == 0
          ? impl_->options.default_target_ranges_per_worker
          : options.target_ranges_per_worker;
  auto const range_budget = std::max<std::size_t>(
      1, saturating_multiply(impl_->resolution.resolved_workers,
                             std::max<std::size_t>(1, ranges_per_worker)));
  auto const adaptive_grain =
      std::max<std::size_t>(1, ceil_div(item_count, range_budget));
  plan.effective_grain = std::max<std::size_t>(
      std::max<std::size_t>(1, minimum_grain), adaptive_grain);
  plan.range_count = ceil_div(item_count, plan.effective_grain);
  plan.worker_task_limit =
      std::min(impl_->resolution.resolved_workers, plan.range_count);
  return plan;
}

chart_indexed_range chart_scheduler::indexed_range_at(
    chart_indexed_range_plan const& plan, std::uint64_t first_task_id,
    std::size_t index) noexcept {
  auto const begin = index * plan.effective_grain;
  auto const remaining = plan.item_count - begin;
  auto const end = remaining <= plan.effective_grain
                       ? plan.item_count
                       : begin + plan.effective_grain;
  return chart_indexed_range{
      .task_id = first_task_id + static_cast<std::uint64_t>(index),
      .range_index = index,
      .begin = begin,
      .end = end,
  };
}

std::size_t estimate_chart_scheduler_operation_peak_bytes(
    chart_indexed_range_plan const& plan) {
  if (plan.force_serial || plan.range_count <= 1 ||
      plan.worker_task_limit <= 1) {
    return 0;
  }
  auto checked_multiply = [](std::size_t lhs, std::size_t rhs,
                             std::string_view context) {
    if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
      throw std::overflow_error(std::string{context} + " byte overflow");
    }
    return lhs * rhs;
  };
  auto checked_add = [](std::size_t lhs, std::size_t rhs,
                        std::string_view context) {
    if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
      throw std::overflow_error(std::string{context} + " byte overflow");
    }
    return lhs + rhs;
  };
  std::size_t total = sizeof(std::vector<std::exception_ptr>) +
                      sizeof(std::vector<std::future<void>>);
  total =
      checked_add(total,
                  checked_multiply(plan.range_count, sizeof(std::exception_ptr),
                                   "chart scheduler exception slots"),
                  "chart scheduler operation");
  total = checked_add(
      total,
      checked_multiply(plan.worker_task_limit, sizeof(std::future<void>),
                       "chart scheduler future slots"),
      "chart scheduler operation");
  auto const runner_bytes = sizeof(std::packaged_task<void()>) +
                            sizeof(std::move_only_function<void()>) +
                            sizeof(chart_indexed_range) + 32 * sizeof(void*) +
                            1024;
  return checked_add(total,
                     checked_multiply(plan.worker_task_limit, runner_bytes,
                                      "chart scheduler runner storage"),
                     "chart scheduler operation");
}

chart_scheduler_run_summary chart_scheduler::run_indexed_ranges(
    chart_indexed_range_plan const& plan, range_work work,
    deterministic_finish finish,
    chart_scheduler_run_summary* failed_run_summary) {
  if (failed_run_summary != nullptr) *failed_run_summary = {};
  auto const nested = current_scheduler_context.scheduler == this;
  bool owns_top_level = false;
  if (!nested) {
    if (impl_->is_shutdown.load(std::memory_order_acquire)) {
      throw std::logic_error("chart scheduler: operation after shutdown");
    }
    bool expected = false;
    if (!impl_->top_level_active.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      impl_->rejected_concurrent_operations.fetch_add(
          1, std::memory_order_relaxed);
      throw std::logic_error(
          "chart scheduler: unrelated concurrent top-level operation");
    }
    owns_top_level = true;
  }
  atomic_flag_scope active_scope{impl_->top_level_active, owns_top_level};

  if (impl_->is_shutdown.load(std::memory_order_acquire)) {
    throw std::logic_error("chart scheduler: operation after shutdown");
  }

  chart_scheduler_run_summary summary;
  summary.item_count = plan.item_count;
  summary.range_count = plan.range_count;
  summary.effective_grain = plan.effective_grain;
  bool operation_accounted = false;
  struct failed_run_publisher {
    chart_scheduler_run_summary& summary;
    chart_scheduler_run_summary* destination;
    bool& operation_accounted;
    int uncaught_on_entry = std::uncaught_exceptions();

    ~failed_run_publisher() noexcept {
      if (destination == nullptr || !operation_accounted ||
          std::uncaught_exceptions() <= uncaught_on_entry) {
        return;
      }
      summary.failed = true;
      if (summary.ranges_completed + summary.ranges_cancelled <
          summary.range_count) {
        summary.ranges_cancelled =
            summary.range_count - summary.ranges_completed;
      }
      summary.cancelled = summary.ranges_cancelled != 0;
      *destination = summary;
    }
  } publish_failed_run{summary, failed_run_summary, operation_accounted};
  summary.operation_id = allocate_monotonic_ids(impl_->next_operation_id, 1,
                                                "chart scheduler operation");
  if (plan.range_count != 0) {
    summary.first_task_id = allocate_monotonic_ids(
        impl_->next_task_id, plan.range_count, "chart scheduler task");
  }

  impl_->operations.fetch_add(1, std::memory_order_relaxed);
  impl_->ranges_created.fetch_add(plan.range_count, std::memory_order_relaxed);
  operation_accounted = true;
  struct range_accounting_guard {
    std::atomic<std::uint64_t>* cancelled = nullptr;
    std::size_t range_count = 0;
    bool accounted = false;

    ~range_accounting_guard() {
      if (!accounted) {
        cancelled->fetch_add(range_count, std::memory_order_relaxed);
      }
    }
  } range_accounting{&impl_->ranges_cancelled, plan.range_count};
  impl_->last_effective_grain.store(plan.effective_grain,
                                    std::memory_order_relaxed);
  if (plan.range_count != 0) {
    atomic_update_min(impl_->minimum_effective_grain, plan.effective_grain);
    atomic_update_max(impl_->maximum_effective_grain, plan.effective_grain);
  }

  if (plan.range_count == 0) {
    summary.serial_reason = chart_scheduler_serial_reason::empty;
    impl_->serial_fallbacks.fetch_add(1, std::memory_order_relaxed);
    impl_->last_active_workers.store(0, std::memory_order_relaxed);
    range_accounting.accounted = true;
    if (finish && !summary.cancelled) finish(summary);
    return summary;
  }

  auto serial_reason = chart_scheduler_serial_reason::none;
  if (nested) {
    serial_reason = chart_scheduler_serial_reason::nested_same_scheduler;
  } else if (plan.force_serial) {
    serial_reason = chart_scheduler_serial_reason::forced;
  } else if (impl_->resolution.resolved_workers <= 1) {
    serial_reason = chart_scheduler_serial_reason::one_resolved_worker;
  } else if (plan.range_count <= 1) {
    serial_reason = chart_scheduler_serial_reason::one_range;
  }

  if (serial_reason != chart_scheduler_serial_reason::none) {
    // Decide and execute the serial path before constructing any owning
    // vectors.  Persistent W1 and one-range chart batches therefore need no
    // scheduler allocation after construction.
    std::atomic<bool> cancelled{false};
    chart_scheduler_cancellation_token token{cancelled};
    std::exception_ptr range_exception;
    std::size_t completed_ranges = 0;

    summary.serial_reason = serial_reason;
    summary.active_workers = 1;
    impl_->serial_fallbacks.fetch_add(1, std::memory_order_relaxed);
    if (nested) {
      impl_->nested_serial_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    atomic_update_max(impl_->active_worker_high_water, std::size_t{1});
    auto const slot_id = nested ? current_scheduler_context.slot_id : 0;
    scheduler_thread_context_scope context{*this, slot_id};
    for (std::size_t index = 0; index < plan.range_count; ++index) {
      if (cancelled.load(std::memory_order_acquire)) break;
      auto const range = indexed_range_at(plan, summary.first_task_id, index);
      try {
        work(range, slot_id, token);
        ++completed_ranges;
      } catch (...) {
        range_exception = std::current_exception();
        cancelled.store(true, std::memory_order_release);
      }
    }
    summary.ranges_completed = completed_ranges;
    summary.ranges_cancelled = plan.range_count - completed_ranges;
    summary.cancelled = cancelled.load(std::memory_order_acquire);
    impl_->ranges_completed.fetch_add(summary.ranges_completed,
                                      std::memory_order_relaxed);
    impl_->ranges_cancelled.fetch_add(summary.ranges_cancelled,
                                      std::memory_order_relaxed);
    impl_->last_active_workers.store(1, std::memory_order_relaxed);
    range_accounting.accounted = true;
    if (range_exception) std::rethrow_exception(range_exception);
    if (finish && !summary.cancelled) finish(summary);
    return summary;
  }

  impl_->parallel_operations.fetch_add(1, std::memory_order_relaxed);
  summary.parallel_branch_entered = true;
  if (!impl_->pool.has_value()) {
    impl_->pool.emplace(impl_->resolution.resolved_workers);
    impl_->live_pool_threads.store(impl_->resolution.resolved_workers,
                                   std::memory_order_relaxed);
    global_live_chart_pool_threads.fetch_add(impl_->resolution.resolved_workers,
                                             std::memory_order_relaxed);
    impl_->pool_lifetimes.fetch_add(1, std::memory_order_relaxed);
  }

  std::atomic<bool> cancelled{false};
  chart_scheduler_cancellation_token token{cancelled};
  std::vector<std::exception_ptr> range_exceptions(plan.range_count);
  std::atomic<std::size_t> completed_ranges{0};
  auto execute_range = [&](std::size_t index, std::size_t slot_id) {
    auto const range = indexed_range_at(plan, summary.first_task_id, index);
    try {
      work(range, slot_id, token);
      completed_ranges.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      range_exceptions[index] = std::current_exception();
      cancelled.store(true, std::memory_order_release);
    }
  };

  std::exception_ptr submission_failure;
  std::exception_ptr worker_task_failure;
  auto const worker_task_count = plan.worker_task_limit;
  std::vector<std::future<void>> futures;
  futures.reserve(worker_task_count);
  // Every successfully submitted runner owns its slot-indexed first range.
  // This guarantees that coordinator-side after-submissions hooks can observe
  // and release every accepted runner even when a later submit fails and sets
  // cancellation before those runners begin.  Only subsequent ranges are
  // dynamically claimed.
  std::atomic<std::size_t> next_range{worker_task_count};
  std::atomic<std::size_t> active_workers{0};
  std::atomic<std::size_t> active_high_water{0};

  for (std::size_t slot_id = 0; slot_id < worker_task_count; ++slot_id) {
    try {
      if (impl_->before_submit) impl_->before_submit(slot_id);
      auto const fail_after =
          impl_->submit_failure_after.load(std::memory_order_acquire);
      if (fail_after == slot_id) {
        impl_->submit_failure_after.store(no_submit_failure,
                                          std::memory_order_release);
        throw chart_scheduler_submit_error(
            "chart scheduler: injected submit failure");
      }

      auto const submitted_at = std::chrono::steady_clock::now();
      futures.push_back(impl_->pool->submit([&, slot_id, submitted_at] {
        struct task_completion_scope {
          implementation& impl;
          ~task_completion_scope() {
            impl.tasks_completed.fetch_add(1, std::memory_order_relaxed);
          }
        } completion{*impl_};

        auto const started_at = std::chrono::steady_clock::now();
        auto const wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              started_at - submitted_at)
                              .count();
        auto const nonnegative_wait =
            wait > 0 ? static_cast<std::uint64_t>(wait) : 0;
        impl_->queue_wait_samples.fetch_add(1, std::memory_order_relaxed);
        impl_->queue_wait_nanoseconds.fetch_add(nonnegative_wait,
                                                std::memory_order_relaxed);
        atomic_update_max(impl_->queue_wait_nanoseconds_max, nonnegative_wait);

        auto const active =
            active_workers.fetch_add(1, std::memory_order_acq_rel) + 1;
        atomic_update_max(active_high_water, active);
        atomic_update_max(impl_->active_worker_high_water, active);
        struct active_worker_scope {
          std::atomic<std::size_t>& active;
          ~active_worker_scope() {
            active.fetch_sub(1, std::memory_order_acq_rel);
          }
        } active_scope{active_workers};
        scheduler_thread_context_scope context{*this, slot_id};

        execute_range(slot_id, slot_id);
        while (!cancelled.load(std::memory_order_acquire)) {
          auto const index = next_range.fetch_add(1, std::memory_order_relaxed);
          if (index >= plan.range_count) break;
          // A claimed range always runs.  Cancellation is consulted only
          // before claiming the next range, keeping lowest-range exception
          // selection independent of a post-claim scheduling race.
          execute_range(index, slot_id);
        }
      }));
      impl_->tasks_submitted.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      submission_failure = std::current_exception();
      cancelled.store(true, std::memory_order_release);
      break;
    }
  }

  summary.worker_tasks_submitted = futures.size();
  if (impl_->after_submissions) {
    try {
      impl_->after_submissions(futures.size(), worker_task_count,
                               submission_failure != nullptr);
    } catch (...) {
      if (!submission_failure) submission_failure = std::current_exception();
      cancelled.store(true, std::memory_order_release);
    }
  }

  for (auto& future : futures) {
    try {
      future.get();
    } catch (...) {
      if (!worker_task_failure) {
        worker_task_failure = std::current_exception();
      }
    }
    impl_->tasks_joined.fetch_add(1, std::memory_order_relaxed);
  }
  summary.active_workers = active_high_water.load(std::memory_order_relaxed);
  summary.ranges_completed = completed_ranges.load(std::memory_order_relaxed);
  summary.ranges_cancelled = plan.range_count - summary.ranges_completed;
  summary.cancelled = cancelled.load(std::memory_order_acquire);
  impl_->ranges_completed.fetch_add(summary.ranges_completed,
                                    std::memory_order_relaxed);
  impl_->ranges_cancelled.fetch_add(summary.ranges_cancelled,
                                    std::memory_order_relaxed);
  impl_->last_active_workers.store(summary.active_workers,
                                   std::memory_order_relaxed);
  range_accounting.accounted = true;

  if (submission_failure) std::rethrow_exception(submission_failure);
  for (auto const& exception : range_exceptions) {
    if (exception) std::rethrow_exception(exception);
  }
  if (worker_task_failure) std::rethrow_exception(worker_task_failure);

  if (finish && !summary.cancelled) finish(summary);
  return summary;
}

chart_scheduler_metrics chart_scheduler::metrics() const noexcept {
  chart_scheduler_metrics result;
  result.requested_workers = impl_->resolution.requested_workers;
  result.resolved_workers = impl_->resolution.resolved_workers;
  result.worker_policy = impl_->resolution.policy;
  result.operations = impl_->operations.load(std::memory_order_relaxed);
  result.parallel_operations =
      impl_->parallel_operations.load(std::memory_order_relaxed);
  result.serial_fallbacks =
      impl_->serial_fallbacks.load(std::memory_order_relaxed);
  result.nested_serial_fallbacks =
      impl_->nested_serial_fallbacks.load(std::memory_order_relaxed);
  result.rejected_concurrent_operations =
      impl_->rejected_concurrent_operations.load(std::memory_order_relaxed);
  result.ranges_created = impl_->ranges_created.load(std::memory_order_relaxed);
  result.ranges_completed =
      impl_->ranges_completed.load(std::memory_order_relaxed);
  result.ranges_cancelled =
      impl_->ranges_cancelled.load(std::memory_order_relaxed);
  result.tasks_submitted =
      impl_->tasks_submitted.load(std::memory_order_relaxed);
  result.tasks_completed =
      impl_->tasks_completed.load(std::memory_order_relaxed);
  result.tasks_joined = impl_->tasks_joined.load(std::memory_order_relaxed);
  result.queue_wait_nanoseconds =
      impl_->queue_wait_nanoseconds.load(std::memory_order_relaxed);
  result.queue_wait_nanoseconds_max =
      impl_->queue_wait_nanoseconds_max.load(std::memory_order_relaxed);
  result.queue_wait_samples =
      impl_->queue_wait_samples.load(std::memory_order_relaxed);
  result.active_worker_high_water =
      impl_->active_worker_high_water.load(std::memory_order_relaxed);
  result.last_active_workers =
      impl_->last_active_workers.load(std::memory_order_relaxed);
  auto const minimum_grain =
      impl_->minimum_effective_grain.load(std::memory_order_relaxed);
  result.minimum_effective_grain =
      minimum_grain == std::numeric_limits<std::size_t>::max() ? 0
                                                               : minimum_grain;
  result.maximum_effective_grain =
      impl_->maximum_effective_grain.load(std::memory_order_relaxed);
  result.last_effective_grain =
      impl_->last_effective_grain.load(std::memory_order_relaxed);
  result.pool_lifetimes = impl_->pool_lifetimes.load(std::memory_order_relaxed);
  result.pool_lifetimes_stopped =
      impl_->pool_lifetimes_stopped.load(std::memory_order_relaxed);
  result.live_pool_threads =
      impl_->live_pool_threads.load(std::memory_order_relaxed);
  auto const submitted = result.tasks_submitted;
  auto const joined = result.tasks_joined;
  result.pending_tasks = submitted >= joined ? submitted - joined : 0;
  result.pending_tasks_at_shutdown =
      impl_->pending_tasks_at_shutdown.load(std::memory_order_relaxed);
  result.shutdown = impl_->is_shutdown.load(std::memory_order_acquire);
  return result;
}

void chart_scheduler::shutdown() {
  if (current_scheduler_context.scheduler == this) {
    throw std::logic_error(
        "chart scheduler: shutdown from an active scheduler callback");
  }

  bool expected = false;
  if (!impl_->top_level_active.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    throw std::logic_error("chart scheduler: shutdown during active operation");
  }
  atomic_flag_scope active_scope{impl_->top_level_active, true};
  if (impl_->is_shutdown.load(std::memory_order_acquire)) return;

  auto const submitted = impl_->tasks_submitted.load(std::memory_order_relaxed);
  auto const joined = impl_->tasks_joined.load(std::memory_order_relaxed);
  auto const pending = submitted >= joined ? submitted - joined : 0;
  impl_->pending_tasks_at_shutdown.store(pending, std::memory_order_relaxed);
  if (pending != 0) {
    throw std::logic_error("chart scheduler: pending tasks at shutdown");
  }

  if (impl_->pool.has_value()) {
    impl_->pool.reset();
    auto const live =
        impl_->live_pool_threads.exchange(0, std::memory_order_relaxed);
    global_live_chart_pool_threads.fetch_sub(live, std::memory_order_relaxed);
    impl_->pool_lifetimes_stopped.fetch_add(1, std::memory_order_relaxed);
  }
  impl_->is_shutdown.store(true, std::memory_order_release);
}

std::size_t chart_scheduler::global_live_pool_threads() noexcept {
  return global_live_chart_pool_threads.load(std::memory_order_relaxed);
}

void chart_scheduler::fail_submission_after_for_tests(
    std::size_t successful_submissions) {
  if (impl_->is_shutdown.load(std::memory_order_acquire)) {
    throw std::logic_error("chart scheduler: test hook after shutdown");
  }
  bool expected = false;
  if (!impl_->top_level_active.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    throw std::logic_error(
        "chart scheduler: test hook during active operation");
  }
  atomic_flag_scope active_scope{impl_->top_level_active, true};
  if (impl_->is_shutdown.load(std::memory_order_acquire)) {
    throw std::logic_error("chart scheduler: test hook after shutdown");
  }
  impl_->submit_failure_after.store(successful_submissions,
                                    std::memory_order_release);
}

void chart_scheduler::set_submission_hooks_for_tests(
    chart_scheduler_test_detail::before_submit_hook before,
    chart_scheduler_test_detail::after_submissions_hook after) {
  if (impl_->is_shutdown.load(std::memory_order_acquire)) {
    throw std::logic_error("chart scheduler: test hook after shutdown");
  }
  bool expected = false;
  if (!impl_->top_level_active.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    throw std::logic_error(
        "chart scheduler: test hook during active operation");
  }
  atomic_flag_scope active_scope{impl_->top_level_active, true};
  if (impl_->is_shutdown.load(std::memory_order_acquire)) {
    throw std::logic_error("chart scheduler: test hook after shutdown");
  }
  impl_->before_submit = std::move(before);
  impl_->after_submissions = std::move(after);
}

}  // namespace larch

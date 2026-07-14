#include <larch/chart_scheduler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <print>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

[[noreturn]] static void test_fail(char const* expression, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expression);
}

#define CHECK(expression)                                          \
  do {                                                             \
    if (!(expression)) test_fail(#expression, __FILE__, __LINE__); \
  } while (false)

static larch::chart_worker_topology_snapshot synthetic_topology(
    std::optional<std::size_t> logical, std::optional<std::size_t> physical,
    std::size_t hardware) {
  return larch::chart_worker_topology_snapshot{
      .affinity_logical_cpu_count = logical,
      .affinity_physical_core_count = physical,
      .hardware_thread_count = hardware,
  };
}

static larch::chart_scheduler make_scheduler(
    std::size_t workers, std::size_t minimum_grain = 64,
    std::size_t ranges_per_worker = 4) {
  return larch::chart_scheduler{
      larch::chart_scheduler_options{
          .requested_workers = workers,
          .default_minimum_grain = minimum_grain,
          .default_target_ranges_per_worker = ranges_per_worker,
      },
      synthetic_topology(2, 1, 2)};
}

static void test_worker_resolution_policies() {
  std::println("test_worker_resolution_policies");

  auto topology = synthetic_topology(8, 4, 64);
  for (auto requested : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                         std::size_t{8}, std::size_t{16}}) {
    auto resolved = larch::resolve_chart_worker_count(requested, topology);
    CHECK(resolved.requested_workers == requested);
    CHECK(resolved.resolved_workers == requested);
    CHECK(resolved.policy ==
          larch::chart_worker_resolution_policy::explicit_count);
  }

  auto physical = larch::resolve_chart_worker_count(0, topology);
  CHECK(physical.resolved_workers == 4);
  CHECK(physical.policy ==
        larch::chart_worker_resolution_policy::affinity_physical_cores);
  CHECK(larch::chart_worker_resolution_policy_name(physical.policy) ==
        "affinity_physical_cores");

  auto logical = larch::resolve_chart_worker_count(
      0, synthetic_topology(6, std::nullopt, 32));
  CHECK(logical.resolved_workers == 6);
  CHECK(logical.policy ==
        larch::chart_worker_resolution_policy::affinity_logical_cpus);

  auto hardware = larch::resolve_chart_worker_count(
      0, synthetic_topology(std::nullopt, std::nullopt, 12));
  CHECK(hardware.resolved_workers == 12);
  CHECK(hardware.policy ==
        larch::chart_worker_resolution_policy::hardware_concurrency);

  auto serial = larch::resolve_chart_worker_count(
      0, synthetic_topology(std::nullopt, std::nullopt, 0));
  CHECK(serial.resolved_workers == 1);
  CHECK(serial.policy ==
        larch::chart_worker_resolution_policy::serial_fallback);

  bool inconsistent_threw = false;
  try {
    (void)larch::resolve_chart_worker_count(0, synthetic_topology(2, 3, 8));
  } catch (std::invalid_argument const&) {
    inconsistent_threw = true;
  }
  CHECK(inconsistent_threw);

  auto detected = larch::detect_chart_worker_topology();
  if (detected.affinity_physical_core_count.has_value()) {
    CHECK(detected.affinity_logical_cpu_count.has_value());
    CHECK(*detected.affinity_physical_core_count > 0);
    CHECK(*detected.affinity_physical_core_count <=
          *detected.affinity_logical_cpu_count);
  }
  auto detected_resolution = larch::resolve_chart_worker_count(0, detected);
  CHECK(detected_resolution.resolved_workers >= 1);

  std::println("  PASS");
}

static void test_adaptive_planning_and_lazy_serial_edges() {
  std::println("test_adaptive_planning_and_lazy_serial_edges");

  auto scheduler = make_scheduler(8);
  CHECK(scheduler.worker_resolution().resolved_workers == 8);
  CHECK(scheduler.metrics().pool_lifetimes == 0);
  CHECK(scheduler.metrics().live_pool_threads == 0);

  std::atomic<std::size_t> visits{0};
  auto empty = scheduler.for_each_indexed_range(
      0, [&](auto const&, std::size_t, auto const&) {
        visits.fetch_add(1, std::memory_order_relaxed);
      });
  CHECK(empty.range_count == 0);
  CHECK(empty.serial_reason == larch::chart_scheduler_serial_reason::empty);

  auto one = scheduler.for_each_indexed_range(
      1, [&](auto const& range, std::size_t slot, auto const&) {
        CHECK(range.begin == 0);
        CHECK(range.end == 1);
        CHECK(slot == 0);
        visits.fetch_add(1, std::memory_order_relaxed);
      });
  CHECK(one.range_count == 1);
  CHECK(one.serial_reason == larch::chart_scheduler_serial_reason::one_range);

  auto small = scheduler.for_each_indexed_range(
      64, [&](auto const& range, std::size_t slot, auto const&) {
        CHECK(range.begin == 0);
        CHECK(range.end == 64);
        CHECK(slot == 0);
        visits.fetch_add(range.end - range.begin, std::memory_order_relaxed);
      });
  CHECK(small.effective_grain == 64);
  CHECK(small.range_count == 1);
  CHECK(small.worker_tasks_submitted == 0);
  CHECK(visits.load(std::memory_order_relaxed) == 65);
  CHECK(scheduler.metrics().pool_lifetimes == 0);
  CHECK(scheduler.metrics().live_pool_threads == 0);

  auto adaptive = scheduler.plan_indexed_ranges(
      1000, {.minimum_grain = 1, .target_ranges_per_worker = 4});
  CHECK(adaptive.effective_grain == 32);
  CHECK(adaptive.range_count == 32);
  CHECK(adaptive.worker_task_limit == 8);

  std::println("  PASS");
}

struct deterministic_accumulator {
  std::uint64_t sum = 0;
  std::vector<std::size_t> reduction_order;
  std::vector<std::uint64_t> task_ids;
};

static void test_serial_reduction_and_monotonic_task_ids() {
  std::println("test_serial_reduction_and_monotonic_task_ids");

  auto scheduler = make_scheduler(1, 1, 4);
  auto work = [](larch::chart_indexed_range const& range, std::size_t slot,
                 larch::chart_scheduler_cancellation_token const&) {
    CHECK(slot == 0);
    std::uint64_t sum = 0;
    for (std::size_t i = range.begin; i < range.end; ++i) sum += i;
    return sum;
  };
  auto reduce = [](deterministic_accumulator& aggregate, std::uint64_t local,
                   larch::chart_indexed_range const& range) {
    CHECK(range.range_index == aggregate.reduction_order.size());
    if (!aggregate.task_ids.empty()) {
      CHECK(aggregate.task_ids.back() < range.task_id);
    }
    aggregate.sum += local;
    aggregate.reduction_order.push_back(range.range_index);
    aggregate.task_ids.push_back(range.task_id);
  };

  auto [first, first_summary] = scheduler.map_reduce_indexed_ranges(
      257, deterministic_accumulator{}, work, reduce);
  CHECK(first.sum == (std::uint64_t{256} * 257) / 2);
  CHECK(first.reduction_order.size() == first_summary.range_count);
  CHECK(first_summary.serial_reason ==
        larch::chart_scheduler_serial_reason::one_resolved_worker);
  CHECK(first_summary.worker_tasks_submitted == 0);
  CHECK(first_summary.ranges_completed == first_summary.range_count);
  CHECK(scheduler.metrics().pool_lifetimes == 0);

  auto [second, second_summary] = scheduler.map_reduce_indexed_ranges(
      17, deterministic_accumulator{}, work, reduce);
  CHECK(!first.task_ids.empty());
  CHECK(!second.task_ids.empty());
  CHECK(first.task_ids.back() < second.task_ids.front());
  CHECK(first_summary.operation_id < second_summary.operation_id);

  std::println("  PASS");
}

static void test_parallel_reduction_is_stable_with_move_only_locals() {
  std::println("test_parallel_reduction_is_stable_with_move_only_locals");

  auto scheduler = make_scheduler(4, 1, 1);
  std::barrier all_started{4};
  std::array<std::atomic<bool>, 3> release_previous{};
  auto [result, summary] = scheduler.map_reduce_indexed_ranges(
      4, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      deterministic_accumulator{},
      [&](auto const& range, std::size_t, auto const&) {
        all_started.arrive_and_wait();
        if (range.range_index == 3) {
          release_previous[2].store(true, std::memory_order_release);
        } else {
          while (!release_previous[range.range_index].load(
              std::memory_order_acquire)) {
            std::this_thread::yield();
          }
        }
        if (range.range_index > 0) {
          release_previous[range.range_index - 1].store(
              true, std::memory_order_release);
        }
        return std::make_unique<std::size_t>(range.range_index);
      },
      [](deterministic_accumulator& aggregate,
         std::unique_ptr<std::size_t> local,
         larch::chart_indexed_range const& range) {
        CHECK(local != nullptr);
        CHECK(*local == range.range_index);
        CHECK(range.range_index == aggregate.reduction_order.size());
        aggregate.reduction_order.push_back(*local);
        aggregate.task_ids.push_back(range.task_id);
      });
  CHECK(summary.active_workers == 4);
  CHECK(summary.ranges_completed == 4);
  CHECK(result.reduction_order == std::vector<std::size_t>({0, 1, 2, 3}));
  CHECK(std::is_sorted(result.task_ids.begin(), result.task_ids.end()));

  std::println("  PASS");
}

static void test_dynamic_nondivisible_ranges_cover_each_item_once() {
  std::println("test_dynamic_nondivisible_ranges_cover_each_item_once");

  auto scheduler = make_scheduler(4, 1, 16);
  std::vector<std::atomic<unsigned>> visits(257);
  for (auto& visit : visits) visit.store(0, std::memory_order_relaxed);
  auto summary = scheduler.for_each_indexed_range(
      visits.size(), {.minimum_grain = 1, .target_ranges_per_worker = 16},
      [&](auto const& range, std::size_t, auto const&) {
        CHECK(range.begin < range.end);
        CHECK(range.end <= visits.size());
        for (auto index = range.begin; index < range.end; ++index) {
          visits[index].fetch_add(1, std::memory_order_relaxed);
        }
      });
  CHECK(summary.range_count > summary.worker_tasks_submitted);
  CHECK(summary.ranges_completed == summary.range_count);
  for (auto const& visit : visits) {
    CHECK(visit.load(std::memory_order_relaxed) == 1);
  }

  std::println("  PASS");
}

static void test_persistent_pool_parallelism_and_stable_slots() {
  std::println("test_persistent_pool_parallelism_and_stable_slots");

  auto const live_before = larch::chart_scheduler::global_live_pool_threads();
  {
    auto scheduler = make_scheduler(4, 1, 1);
    std::uint64_t last_task_id = 0;
    for (std::size_t iteration = 0; iteration < 3; ++iteration) {
      std::barrier rendezvous{4};
      std::mutex mutex;
      std::set<std::thread::id> threads;
      std::set<std::size_t> slots;
      std::array<std::uint64_t, 4> task_ids{};
      auto summary = scheduler.for_each_indexed_range(
          4, {.minimum_grain = 1, .target_ranges_per_worker = 1},
          [&](auto const& range, std::size_t slot, auto const&) {
            {
              std::lock_guard lock{mutex};
              threads.insert(std::this_thread::get_id());
              slots.insert(slot);
            }
            task_ids[range.range_index] = range.task_id;
            rendezvous.arrive_and_wait();
          });
      CHECK(summary.worker_tasks_submitted == 4);
      CHECK(summary.active_workers == 4);
      CHECK(summary.ranges_completed == 4);
      CHECK(threads.size() == 4);
      CHECK(slots == std::set<std::size_t>({0, 1, 2, 3}));
      CHECK(task_ids.front() > last_task_id);
      CHECK(std::is_sorted(task_ids.begin(), task_ids.end()));
      last_task_id = task_ids.back();
      CHECK(scheduler.metrics().pool_lifetimes == 1);
    }

    auto metrics = scheduler.metrics();
    CHECK(metrics.live_pool_threads == 4);
    CHECK(metrics.tasks_submitted == 12);
    CHECK(metrics.tasks_completed == 12);
    CHECK(metrics.tasks_joined == 12);
    CHECK(metrics.pending_tasks == 0);
    CHECK(metrics.queue_wait_samples == 12);
    CHECK(metrics.queue_wait_nanoseconds >= metrics.queue_wait_nanoseconds_max);
    CHECK(metrics.active_worker_high_water == 4);
    CHECK(larch::chart_scheduler::global_live_pool_threads() ==
          live_before + 4);

    scheduler.shutdown();
    auto stopped = scheduler.metrics();
    CHECK(stopped.shutdown);
    CHECK(stopped.live_pool_threads == 0);
    CHECK(stopped.pool_lifetimes == 1);
    CHECK(stopped.pool_lifetimes_stopped == 1);
    CHECK(stopped.pending_tasks == 0);
    CHECK(stopped.pending_tasks_at_shutdown == 0);
    CHECK(larch::chart_scheduler::global_live_pool_threads() == live_before);
    scheduler.shutdown();

    bool after_shutdown_threw = false;
    try {
      (void)scheduler.for_each_indexed_range(
          1, [](auto const&, std::size_t, auto const&) {});
    } catch (std::logic_error const&) {
      after_shutdown_threw = true;
    }
    CHECK(after_shutdown_threw);
  }
  CHECK(larch::chart_scheduler::global_live_pool_threads() == live_before);

  // The bounded group never submits empty workers.
  {
    auto scheduler = make_scheduler(8, 1, 1);
    std::barrier rendezvous{3};
    auto summary = scheduler.for_each_indexed_range(
        3, {.minimum_grain = 1, .target_ranges_per_worker = 1},
        [&](auto const&, std::size_t, auto const&) {
          rendezvous.arrive_and_wait();
        });
    CHECK(summary.worker_tasks_submitted == 3);
    CHECK(summary.active_workers == 3);
  }

  std::println("  PASS");
}

static void test_deterministic_exception_selection_and_recovery() {
  std::println("test_deterministic_exception_selection_and_recovery");

  auto scheduler = make_scheduler(4, 1, 1);
  auto before = scheduler.metrics();
  std::barrier claimed{4};
  bool threw = false;
  try {
    (void)scheduler.for_each_indexed_range(
        4, {.minimum_grain = 1, .target_ranges_per_worker = 1},
        [&](auto const& range, std::size_t, auto const&) {
          claimed.arrive_and_wait();
          if (range.range_index == 3) {
            throw std::runtime_error("range 3 failure");
          }
          if (range.range_index == 1) {
            throw std::runtime_error("range 1 failure");
          }
        });
  } catch (std::runtime_error const& error) {
    threw = true;
    CHECK(std::string{error.what()} == "range 1 failure");
  }
  CHECK(threw);
  auto after = scheduler.metrics();
  CHECK(after.tasks_submitted - before.tasks_submitted == 4);
  CHECK(after.tasks_completed - before.tasks_completed == 4);
  CHECK(after.tasks_joined - before.tasks_joined == 4);
  CHECK(after.pending_tasks == 0);

  std::atomic<std::size_t> recovered{0};
  auto recovery = scheduler.for_each_indexed_range(
      8, {.minimum_grain = 1, .target_ranges_per_worker = 2},
      [&](auto const& range, std::size_t, auto const&) {
        recovered.fetch_add(range.end - range.begin, std::memory_order_relaxed);
      });
  CHECK(!recovery.cancelled);
  CHECK(recovered.load(std::memory_order_relaxed) == 8);
  CHECK(scheduler.metrics().pending_tasks == 0);

  std::println("  PASS");
}

static void test_cancellation_claim_contract_and_recovery() {
  std::println("test_cancellation_claim_contract_and_recovery");

  auto scheduler = make_scheduler(4, 1, 16);
  std::latch all_claimed{4};
  std::latch cancellation_published{1};
  std::array<std::atomic<std::size_t>, 4> first_range_visits{};
  auto cancelled = scheduler.for_each_indexed_range(
      64, {.minimum_grain = 1, .target_ranges_per_worker = 16},
      [&](auto const& range, std::size_t, auto const& token) {
        if (range.range_index < first_range_visits.size()) {
          first_range_visits[range.range_index].fetch_add(
              1, std::memory_order_relaxed);
        }
        all_claimed.count_down();
        all_claimed.wait();
        if (range.range_index == 0) {
          token.request_cancel();
          cancellation_published.count_down();
        } else {
          cancellation_published.wait();
        }
      });
  CHECK(cancelled.cancelled);
  CHECK(cancelled.ranges_completed == 4);
  CHECK(cancelled.ranges_cancelled == 60);
  for (auto const& visits : first_range_visits) {
    CHECK(visits.load(std::memory_order_relaxed) == 1);
  }
  CHECK(scheduler.metrics().pending_tasks == 0);

  std::atomic<std::size_t> visits{0};
  auto recovery = scheduler.for_each_indexed_range(
      64, {.minimum_grain = 1, .target_ranges_per_worker = 16},
      [&](auto const&, std::size_t, auto const&) {
        visits.fetch_add(1, std::memory_order_relaxed);
      });
  CHECK(!recovery.cancelled);
  CHECK(recovery.ranges_completed == 64);
  CHECK(visits.load(std::memory_order_relaxed) == 64);

  std::println("  PASS");
}

static void test_map_reduce_cancellation_never_publishes_partial_state() {
  std::println("test_map_reduce_cancellation_never_publishes_partial_state");

  {
    auto scheduler = make_scheduler(1, 1, 16);
    std::size_t reduce_calls = 0;
    auto [value, summary] = scheduler.map_reduce_indexed_ranges(
        16, {.minimum_grain = 1, .target_ranges_per_worker = 16},
        std::uint64_t{41},
        [](auto const& range, std::size_t, auto const& token) {
          if (range.range_index == 0) token.request_cancel();
          return static_cast<std::uint64_t>(range.range_index + 1);
        },
        [&](std::uint64_t& aggregate, std::uint64_t local, auto const&) {
          ++reduce_calls;
          aggregate += local;
        });
    CHECK(summary.cancelled);
    CHECK(summary.ranges_completed == 1);
    CHECK(summary.ranges_cancelled == 15);
    CHECK(value == 41);
    CHECK(reduce_calls == 0);
  }

  {
    auto scheduler = make_scheduler(4, 1, 16);
    std::latch all_claimed{4};
    std::latch cancellation_published{1};
    std::size_t reduce_calls = 0;
    auto [value, summary] = scheduler.map_reduce_indexed_ranges(
        16, {.minimum_grain = 1, .target_ranges_per_worker = 16},
        std::uint64_t{73},
        [&](auto const& range, std::size_t, auto const& token) {
          all_claimed.count_down();
          all_claimed.wait();
          if (range.range_index == 0) {
            token.request_cancel();
            cancellation_published.count_down();
          } else {
            cancellation_published.wait();
          }
          return static_cast<std::uint64_t>(range.range_index + 1);
        },
        [&](std::uint64_t& aggregate, std::uint64_t local, auto const&) {
          ++reduce_calls;
          aggregate += local;
        });
    CHECK(summary.cancelled);
    CHECK(summary.ranges_completed == 4);
    CHECK(summary.ranges_cancelled == 12);
    CHECK(value == 73);
    CHECK(reduce_calls == 0);
    CHECK(scheduler.metrics().pending_tasks == 0);
  }

  std::println("  PASS");
}

static void test_nested_same_scheduler_inherits_unique_slot() {
  std::println("test_nested_same_scheduler_inherits_unique_slot");

  auto scheduler = make_scheduler(4, 1, 1);
  std::barrier outer_claimed{4};
  std::array<std::atomic<std::size_t>, 4> nested_visits{};
  std::array<larch::chart_scheduler_serial_reason, 4> nested_reasons{};
  auto outer = scheduler.for_each_indexed_range(
      4, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      [&](auto const& outer_range, std::size_t outer_slot, auto const&) {
        outer_claimed.arrive_and_wait();
        auto nested = scheduler.for_each_indexed_range(
            3, {.minimum_grain = 1, .target_ranges_per_worker = 1},
            [&](auto const&, std::size_t nested_slot, auto const&) {
              CHECK(nested_slot == outer_slot);
              nested_visits[outer_slot].fetch_add(1, std::memory_order_relaxed);
            });
        nested_reasons[outer_range.range_index] = nested.serial_reason;
        CHECK(nested.worker_tasks_submitted == 0);
      });
  CHECK(outer.active_workers == 4);
  for (std::size_t slot = 0; slot < nested_visits.size(); ++slot) {
    CHECK(nested_visits[slot].load(std::memory_order_relaxed) == 3);
    CHECK(nested_reasons[slot] ==
          larch::chart_scheduler_serial_reason::nested_same_scheduler);
  }
  CHECK(scheduler.metrics().nested_serial_fallbacks == 4);
  CHECK(scheduler.metrics().pending_tasks == 0);

  std::println("  PASS");
}

static void test_unrelated_concurrent_top_level_rejected() {
  std::println("test_unrelated_concurrent_top_level_rejected");

  auto scheduler = make_scheduler(2, 1, 1);
  std::latch entered{1};
  std::latch release{1};
  std::exception_ptr background_failure;
  std::jthread background{[&] {
    try {
      (void)scheduler.for_each_indexed_range(
          1, [&](auto const&, std::size_t, auto const&) {
            entered.count_down();
            release.wait();
          });
    } catch (...) {
      background_failure = std::current_exception();
    }
  }};
  entered.wait();

  bool concurrent_threw = false;
  try {
    (void)scheduler.for_each_indexed_range(
        1, [](auto const&, std::size_t, auto const&) {});
  } catch (std::logic_error const& error) {
    concurrent_threw = true;
    CHECK(std::string{error.what()}.find("concurrent top-level") !=
          std::string::npos);
  }
  CHECK(concurrent_threw);

  bool shutdown_threw = false;
  try {
    scheduler.shutdown();
  } catch (std::logic_error const&) {
    shutdown_threw = true;
  }
  CHECK(shutdown_threw);
  release.count_down();
  background.join();
  if (background_failure) std::rethrow_exception(background_failure);
  CHECK(scheduler.metrics().rejected_concurrent_operations == 1);

  std::println("  PASS");
}

static void test_submission_hooks_and_partial_submit_failure_join() {
  std::println("test_submission_hooks_and_partial_submit_failure_join");

  auto scheduler = make_scheduler(4, 1, 1);

  // Successful submission: release callback-blocked workers after the complete
  // submission phase and before the coordinator starts joining.
  {
    std::latch release{1};
    std::atomic<std::size_t> after_calls{0};
    larch::chart_scheduler_test_detail::access::set_submission_hooks(
        scheduler, {},
        [&](std::size_t submitted, std::size_t intended, bool failed) {
          CHECK(submitted == 4);
          CHECK(intended == 4);
          CHECK(!failed);
          after_calls.fetch_add(1, std::memory_order_relaxed);
          release.count_down();
        });
    auto summary = scheduler.for_each_indexed_range(
        4, {.minimum_grain = 1, .target_ranges_per_worker = 1},
        [&](auto const&, std::size_t, auto const&) { release.wait(); });
    CHECK(summary.ranges_completed == 4);
    CHECK(after_calls.load(std::memory_order_relaxed) == 1);
    larch::chart_scheduler_test_detail::access::clear_submission_hooks(
        scheduler);
  }

  // Partial failure after one accepted task: cancellation is already published
  // before that worker necessarily starts.  The runner must still execute its
  // stable first range, allowing the after-submissions hook to wait for and
  // release it before internal join.
  auto before = scheduler.metrics();
  std::latch accepted_entered{1};
  std::latch release_accepted{1};
  std::atomic<std::size_t> after_calls{0};
  larch::chart_scheduler_test_detail::access::set_submission_hooks(
      scheduler, {},
      [&](std::size_t submitted, std::size_t intended, bool failed) {
        CHECK(submitted == 1);
        CHECK(intended == 4);
        CHECK(failed);
        accepted_entered.wait();
        after_calls.fetch_add(1, std::memory_order_relaxed);
        release_accepted.count_down();
      });
  larch::chart_scheduler_test_detail::access::fail_submission_after(scheduler,
                                                                    1);

  bool submit_threw = false;
  try {
    (void)scheduler.for_each_indexed_range(
        4, {.minimum_grain = 1, .target_ranges_per_worker = 1},
        [&](auto const&, std::size_t, auto const&) {
          accepted_entered.count_down();
          release_accepted.wait();
        });
  } catch (larch::chart_scheduler_submit_error const&) {
    submit_threw = true;
  }
  CHECK(submit_threw);
  CHECK(after_calls.load(std::memory_order_relaxed) == 1);
  auto after = scheduler.metrics();
  CHECK(after.tasks_submitted - before.tasks_submitted == 1);
  CHECK(after.tasks_completed - before.tasks_completed == 1);
  CHECK(after.tasks_joined - before.tasks_joined == 1);
  CHECK(after.pending_tasks == 0);
  CHECK(after.queue_wait_samples - before.queue_wait_samples == 1);

  larch::chart_scheduler_test_detail::access::clear_submission_hooks(scheduler);
  std::atomic<std::size_t> recovered{0};
  auto recovery = scheduler.for_each_indexed_range(
      4, {.minimum_grain = 1, .target_ranges_per_worker = 1},
      [&](auto const&, std::size_t, auto const&) {
        recovered.fetch_add(1, std::memory_order_relaxed);
      });
  CHECK(recovery.ranges_completed == 4);
  CHECK(recovered.load(std::memory_order_relaxed) == 4);
  CHECK(scheduler.metrics().pool_lifetimes == 1);
  CHECK(scheduler.metrics().pending_tasks == 0);

  std::println("  PASS");
}

int main() {
  test_worker_resolution_policies();
  test_adaptive_planning_and_lazy_serial_edges();
  test_serial_reduction_and_monotonic_task_ids();
  test_parallel_reduction_is_stable_with_move_only_locals();
  test_dynamic_nondivisible_ranges_cover_each_item_once();
  test_persistent_pool_parallelism_and_stable_slots();
  test_deterministic_exception_selection_and_recovery();
  test_cancellation_claim_contract_and_recovery();
  test_map_reduce_cancellation_never_publishes_partial_state();
  test_nested_same_scheduler_inherits_unique_slot();
  test_unrelated_concurrent_top_level_rejected();
  test_submission_hooks_and_partial_submit_failure_join();

  CHECK(larch::chart_scheduler::global_live_pool_threads() == 0);
  std::println("All chart scheduler tests passed!");
  return 0;
}

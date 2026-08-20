#pragma once

#include <larch/task.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace larch {

namespace detail {

// Slot of the pool-worker thread running the current task.  Pool workers
// store their creation index here once per thread; threads that are not a
// worker of the pool whose task they run (e.g. a caller helping out via
// thread_pool::try_run_one) temporarily install workers_.size().  The
// sentinel means "no pool context yet".
constexpr std::size_t no_pool_thread_slot =
    static_cast<std::size_t>(-1);

inline std::size_t& pool_thread_slot() {
  thread_local std::size_t slot = no_pool_thread_slot;
  return slot;
}

// RAII guard installing a pool thread slot for the duration of a task run
// on a thread that is not one of the pool's own workers.
struct scoped_pool_thread_slot {
  std::size_t saved;
  scoped_pool_thread_slot(std::size_t slot) {
    saved = pool_thread_slot();
    pool_thread_slot() = slot;
  }
  ~scoped_pool_thread_slot() { pool_thread_slot() = saved; }
};

}  // namespace detail

class thread_pool {
  std::queue<std::move_only_function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable_any cv_;
  std::vector<std::jthread> workers_;

  thread_pool(thread_pool const&) = delete;
  thread_pool& operator=(thread_pool const&) = delete;
  thread_pool(thread_pool&&) = delete;
  thread_pool& operator=(thread_pool&&) = delete;

 public:
  explicit thread_pool(std::size_t n = std::thread::hardware_concurrency()) {
    n = std::max(std::size_t{1}, n);
    workers_.reserve(n);
    for (std::size_t i{0}; i < n; ++i) {
      workers_.emplace_back([this, i](std::stop_token st) {
        detail::pool_thread_slot() = i;
        for (;;) {
          std::move_only_function<void()> task;
          {
            std::unique_lock lock{mutex_};
            if (!cv_.wait(lock, st, [&] { return !tasks_.empty(); })) return;
            task = std::move(tasks_.front());
            tasks_.pop();
          }
          task();
        }
      });
    }
  }

  ~thread_pool() = default;

  // Number of worker threads in the pool.
  std::size_t size() const { return workers_.size(); }

  // Stable slot index of the thread executing the current pool task.
  // A pool worker reports its creation index in [0, size()); a task that
  // runs on a non-worker thread (a caller helping via try_run_one) reports
  // size().  The value is constant for the duration of a task, and two
  // concurrently running tasks never share a slot, so per-slot scratch
  // state keyed by this index is race-free.  Only meaningful while
  // executing a task of this pool.
  std::size_t worker_slot() const {
    auto const slot = detail::pool_thread_slot();
    return slot == detail::no_pool_thread_slot ? workers_.size() : slot;
  }

  // Enqueue a callable, return a future for its result.
  template <typename Callable>
  auto submit(Callable&& f) -> std::future<std::invoke_result_t<Callable>> {
    using R = std::invoke_result_t<Callable>;
    std::packaged_task<R()> inner{std::forward<Callable>(f)};
    auto future{inner.get_future()};
    {
      std::lock_guard lock{mutex_};
      tasks_.emplace([t = std::move(inner)]() mutable { t(); });
    }
    cv_.notify_one();
    return future;
  }

  // Fire-and-forget: enqueue a move-only callable.
  void schedule(std::move_only_function<void()> f) {
    {
      std::lock_guard lock{mutex_};
      tasks_.push(std::move(f));
    }
    cv_.notify_one();
  }

  // Schedule a coroutine handle for resumption on the pool.
  void schedule(std::coroutine_handle<> h) {
    schedule([h] { h.resume(); });
  }

  // Try to steal and execute one task. Returns false if queue was empty.
  bool try_run_one() {
    std::move_only_function<void()> task;
    {
      std::unique_lock lock{mutex_, std::try_to_lock};
      if (!lock.owns_lock() || tasks_.empty()) return false;
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    detail::scoped_pool_thread_slot slot{workers_.size()};
    task();
    return true;
  }

  static thread_pool& get_default() {
    static thread_pool pool;
    return pool;
  }
};

// --- async_latch::count_down (needs thread_pool to be complete) ---

inline void async_latch::count_down() {
  if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    pool_->schedule(waiter_);
  }
}

// --- sync_wait ---

namespace detail {

// A continuation coroutine that signals completion when done.
// Uses suspend_always at final_suspend so the frame stays alive
// until sync_wait extracts the result.
template <typename T>
struct sync_wait_task {
  struct promise_type {
    std::optional<T> result;
    std::exception_ptr exception;
    std::atomic<bool>* done_flag{nullptr};

    sync_wait_task get_return_object() {
      return sync_wait_task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    auto initial_suspend() noexcept { return std::suspend_always{}; }
    auto final_suspend() noexcept {
      struct notifier {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          h.promise().done_flag->store(true, std::memory_order_release);
        }
        void await_resume() noexcept {}
      };
      return notifier{};
    }
    void return_value(T v) { result.emplace(std::move(v)); }
    void unhandled_exception() { exception = std::current_exception(); }
  };

  std::coroutine_handle<promise_type> handle;

  ~sync_wait_task() {
    if (handle) handle.destroy();
  }
  sync_wait_task(sync_wait_task&& o) noexcept
      : handle{std::exchange(o.handle, nullptr)} {}

 private:
  explicit sync_wait_task(std::coroutine_handle<promise_type> h) : handle{h} {}
};

template <>
struct sync_wait_task<void> {
  struct promise_type {
    std::exception_ptr exception;
    std::atomic<bool>* done_flag{nullptr};

    sync_wait_task get_return_object() {
      return sync_wait_task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    auto initial_suspend() noexcept { return std::suspend_always{}; }
    auto final_suspend() noexcept {
      struct notifier {
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          h.promise().done_flag->store(true, std::memory_order_release);
        }
        void await_resume() noexcept {}
      };
      return notifier{};
    }
    void return_void() {}
    void unhandled_exception() { exception = std::current_exception(); }
  };

  std::coroutine_handle<promise_type> handle;

  ~sync_wait_task() {
    if (handle) handle.destroy();
  }
  sync_wait_task(sync_wait_task&& o) noexcept
      : handle{std::exchange(o.handle, nullptr)} {}

 private:
  explicit sync_wait_task(std::coroutine_handle<promise_type> h) : handle{h} {}
};

template <typename T>
sync_wait_task<T> make_sync_wait_task(task<T>&& t) {
  co_return co_await std::move(t);
}

inline sync_wait_task<void> make_sync_wait_task(task<void>&& t) {
  co_await std::move(t);
}

}  // namespace detail

template <typename T>
T sync_wait(thread_pool& pool, task<T>&& t) {
  std::atomic<bool> done{false};
  auto sw = detail::make_sync_wait_task(std::move(t));
  sw.handle.promise().done_flag = &done;
  sw.handle.resume();

  while (!done.load(std::memory_order_acquire)) {
    if (!pool.try_run_one()) std::this_thread::yield();
  }

  auto& p = sw.handle.promise();
  if (p.exception) std::rethrow_exception(p.exception);
  return std::move(*p.result);
}

template <>
inline void sync_wait<void>(thread_pool& pool, task<void>&& t) {
  std::atomic<bool> done{false};
  auto sw = detail::make_sync_wait_task(std::move(t));
  sw.handle.promise().done_flag = &done;
  sw.handle.resume();

  while (!done.load(std::memory_order_acquire)) {
    if (!pool.try_run_one()) std::this_thread::yield();
  }

  auto& p = sw.handle.promise();
  if (p.exception) std::rethrow_exception(p.exception);
}

// --- async_for_each ---

// Regular callable overload
template <std::ranges::forward_range Range, typename Callable>
  requires(!CoroutineInvocable<Callable, std::ranges::range_reference_t<Range>>)
task<void> async_for_each(thread_pool& pool, Range&& range, Callable f) {
  auto count = std::ranges::distance(range);
  if (count == 0) co_return;

  async_latch latch{count, pool};
  std::exception_ptr captured;
  std::mutex ex_mutex;

  for (auto&& elem : range) {
    auto* ptr = std::addressof(elem);
    pool.schedule([&f, ptr, &latch, &captured, &ex_mutex] {
      try {
        f(*ptr);
      } catch (...) {
        std::lock_guard lock{ex_mutex};
        if (!captured) captured = std::current_exception();
      }
      latch.count_down();
    });
  }

  co_await latch;

  if (captured) std::rethrow_exception(captured);
}

// Coroutine callable overload
template <std::ranges::forward_range Range, typename Callable>
  requires CoroutineInvocable<Callable, std::ranges::range_reference_t<Range>>
task<void> async_for_each(thread_pool& pool, Range&& range, Callable f) {
  auto count = std::ranges::distance(range);
  if (count == 0) co_return;

  async_latch latch{count, pool};
  std::exception_ptr captured;
  std::mutex ex_mutex;

  for (auto&& elem : range) {
    auto* ptr = std::addressof(elem);
    pool.schedule([&f, ptr, &latch, &captured, &ex_mutex] {
      [](auto& f, auto* ptr, async_latch& latch, std::exception_ptr& captured,
         std::mutex& ex_mutex) -> detached_task {
        try {
          co_await f(*ptr);
        } catch (...) {
          std::lock_guard lock{ex_mutex};
          if (!captured) captured = std::current_exception();
        }
        latch.count_down();
      }(f, ptr, latch, captured, ex_mutex);
    });
  }

  co_await latch;

  if (captured) std::rethrow_exception(captured);
}

// --- parallel_for_each (backward-compatible) ---

template <std::ranges::forward_range Range, typename Callable>
void parallel_for_each(thread_pool& pool, Range&& range, Callable&& f) {
  sync_wait<void>(pool, async_for_each(pool, std::forward<Range>(range),
                                       std::forward<Callable>(f)));
}

template <std::ranges::forward_range Range, typename Callable>
void parallel_for_each(Range&& range, Callable&& f) {
  parallel_for_each(thread_pool::get_default(), std::forward<Range>(range),
                    std::forward<Callable>(f));
}

// --- parallel_for_each_block (chunked, worker-slot aware) ---

// Partitions [0, count) into contiguous blocks of at most block_size
// indices and runs one pool task per block, invoking f(begin, end).
// Compared to parallel_for_each this amortizes per-task scheduling
// overhead over many elements and lets callers keep pool-affine scratch
// alive for a whole block (key it by pool.worker_slot()).
template <typename Callable>
void parallel_for_each_block(thread_pool& pool, std::size_t count,
                             std::size_t block_size, Callable&& f) {
  if (count == 0) return;
  if (block_size == 0) block_size = 1;
  std::vector<std::pair<std::size_t, std::size_t>> blocks;
  blocks.reserve((count + block_size - 1) / block_size);
  for (std::size_t begin = 0; begin < count; begin += block_size) {
    blocks.emplace_back(begin, std::min(begin + block_size, count));
  }
  parallel_for_each(
      pool, blocks,
      [&f](std::pair<std::size_t, std::size_t> const& block) {
        f(block.first, block.second);
      });
}

}  // namespace larch

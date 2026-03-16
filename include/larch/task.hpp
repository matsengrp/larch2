#pragma once

#include <atomic>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace larch {

// Forward declarations
class thread_pool;

// --- Concepts ---

template <typename T>
concept Awaitable = requires(T t, std::coroutine_handle<> h) {
  { t.await_ready() } -> std::convertible_to<bool>;
  t.await_suspend(h);
  t.await_resume();
};

template <typename F, typename Arg>
concept CoroutineInvocable = requires(F f, Arg a) {
  { std::invoke(f, a) } -> Awaitable;
};

// --- task<T> --- lazy coroutine with symmetric transfer ---

template <typename T = void>
class task;

namespace detail {

struct task_promise_base {
  std::coroutine_handle<> continuation_{std::noop_coroutine()};
  std::exception_ptr exception_;

  auto initial_suspend() noexcept { return std::suspend_always{}; }

  struct final_awaiter {
    bool await_ready() noexcept { return false; }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<Promise> h) noexcept {
      return h.promise().continuation_;
    }

    void await_resume() noexcept {}
  };

  auto final_suspend() noexcept { return final_awaiter{}; }

  void unhandled_exception() { exception_ = std::current_exception(); }
};

template <typename T>
struct task_promise : task_promise_base {
  T result_;

  task<T> get_return_object();

  void return_value(T value) { result_ = std::move(value); }

  T& result() {
    if (exception_) std::rethrow_exception(exception_);
    return result_;
  }
};

template <>
struct task_promise<void> : task_promise_base {
  task<void> get_return_object();

  void return_void() {}

  void result() {
    if (exception_) std::rethrow_exception(exception_);
  }
};

}  // namespace detail

template <typename T>
class task {
 public:
  using promise_type = detail::task_promise<T>;

  task() noexcept : handle_{nullptr} {}
  explicit task(std::coroutine_handle<promise_type> h) noexcept : handle_{h} {}

  task(task&& other) noexcept
      : handle_{std::exchange(other.handle_, nullptr)} {}
  task& operator=(task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  task(task const&) = delete;
  task& operator=(task const&) = delete;

  ~task() {
    if (handle_) handle_.destroy();
  }

  // Awaitable interface
  bool await_ready() const noexcept { return false; }

  std::coroutine_handle<> await_suspend(
      std::coroutine_handle<> caller) noexcept {
    handle_.promise().continuation_ = caller;
    return handle_;  // symmetric transfer: start this task
  }

  decltype(auto) await_resume() { return handle_.promise().result(); }

  std::coroutine_handle<promise_type> handle() const noexcept {
    return handle_;
  }

 private:
  std::coroutine_handle<promise_type> handle_;
};

namespace detail {

template <typename T>
task<T> task_promise<T>::get_return_object() {
  return task<T>{std::coroutine_handle<task_promise<T>>::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() {
  return task<void>{
      std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}

}  // namespace detail

// --- async_latch --- awaitable countdown ---

class async_latch {
  std::atomic<std::ptrdiff_t> count_;
  std::coroutine_handle<> waiter_{nullptr};
  thread_pool* pool_;

 public:
  async_latch(std::ptrdiff_t n, thread_pool& pool)
      : count_{n + 1}, pool_{&pool} {}

  void count_down();  // defined after thread_pool is complete

  // Awaitable interface
  bool await_ready() const noexcept {
    return count_.load(std::memory_order_acquire) <= 0;
  }

  bool await_suspend(std::coroutine_handle<> caller) noexcept {
    waiter_ = caller;
    // The awaiter's decrement
    if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // All tasks already done — don't suspend
      return false;
    }
    return true;  // suspend; last count_down() will resume us
  }

  void await_resume() noexcept {}
};

// --- detached_task --- fire-and-forget coroutine ---

struct detached_task {
  struct promise_type {
    detached_task get_return_object() { return {}; }
    auto initial_suspend() noexcept { return std::suspend_never{}; }
    auto final_suspend() noexcept { return std::suspend_never{}; }
    void return_void() {}
    void unhandled_exception() { std::terminate(); }
  };
};

// --- sync_wait --- bridge from coroutine to synchronous code ---

template <typename T>
T sync_wait(thread_pool& pool, task<T>&& t);

}  // namespace larch

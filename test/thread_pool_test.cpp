#include <larch/thread_pool.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <print>
#include <stdexcept>
#include <vector>

int main() {
  // 1. Submit a task returning a value, check via future.get()
  {
    larch::thread_pool pool{4};
    auto future{pool.submit([] { return 42; })};
    assert(future.get() == 42);
  }

  // 2. Submit 100 tasks incrementing an atomic, verify count
  {
    larch::thread_pool pool{4};
    std::atomic<int> count{0};
    std::vector<std::future<void>> futures;
    for (int i{0}; i < 100; ++i) {
      futures.push_back(pool.submit([&count] { count.fetch_add(1); }));
    }
    for (auto& f : futures) {
      f.get();
    }
    assert(count.load() == 100);
  }

  // 3. parallel_for_each with pool — mutate vector elements
  {
    larch::thread_pool pool{4};
    std::vector<int> v{1, 2, 3, 4, 5};
    larch::parallel_for_each(pool, v, [](int& x) { x *= 2; });
    assert(v[0] == 2);
    assert(v[1] == 4);
    assert(v[2] == 6);
    assert(v[3] == 8);
    assert(v[4] == 10);
  }

  // 4. parallel_for_each convenience overload (uses default pool)
  {
    std::vector<int> v{10, 20, 30};
    larch::parallel_for_each(v, [](int& x) { x += 1; });
    assert(v[0] == 11);
    assert(v[1] == 21);
    assert(v[2] == 31);
  }

  // 5. Empty range — no-op
  {
    larch::thread_pool pool{2};
    std::vector<int> v;
    larch::parallel_for_each(pool, v, [](int&) { assert(false); });
  }

  // 6. Single-thread pool — correctness check
  {
    larch::thread_pool pool{1};
    std::atomic<int> sum{0};
    std::vector<std::future<void>> futures;
    for (int i{1}; i <= 10; ++i) {
      futures.push_back(pool.submit([&sum, i] { sum.fetch_add(i); }));
    }
    for (auto& f : futures) {
      f.get();
    }
    assert(sum.load() == 55);
  }

  // 7. Recursive parallel_for_each — 2 workers, must not deadlock
  {
    larch::thread_pool pool{2};
    std::vector<int> outer(4, 0);
    larch::parallel_for_each(pool, outer, [&pool](int& x) {
      std::vector<int> inner(4, 1);
      larch::parallel_for_each(pool, inner, [](int& y) { y += 1; });
      int sum{0};
      for (auto v : inner) sum += v;
      x = sum;
    });
    for (auto v : outer) {
      assert(v == 8);
    }
  }

  // 8. Deep nesting — 3 levels with 2 workers
  {
    larch::thread_pool pool{2};
    std::atomic<int> count{0};
    std::vector<int> level0(3);
    larch::parallel_for_each(pool, level0, [&](int&) {
      std::vector<int> level1(3);
      larch::parallel_for_each(pool, level1, [&](int&) {
        std::vector<int> level2(3);
        larch::parallel_for_each(pool, level2,
                                 [&](int&) { count.fetch_add(1); });
      });
    });
    assert(count.load() == 27);
  }

  // 9. Exception propagation through parallel_for_each
  {
    larch::thread_pool pool{4};
    std::vector<int> v{1, 2, 3, 4, 5};
    bool caught{false};
    try {
      larch::parallel_for_each(pool, v, [](int& x) {
        if (x == 3) throw std::runtime_error{"test error"};
      });
    } catch (std::runtime_error const& e) {
      caught = true;
    }
    assert(caught);
  }

  // 10. Coroutine callable — async_for_each with co_await nesting
  {
    larch::thread_pool pool{2};
    std::vector<int> outer(4, 0);
    auto t = larch::async_for_each(
        pool, outer, [&pool](int& x) -> larch::task<void> {
          std::vector<int> inner(4, 1);
          co_await larch::async_for_each(pool, inner, [](int& y) { y += 1; });
          int sum{0};
          for (auto v : inner) sum += v;
          x = sum;
        });
    larch::sync_wait<void>(pool, std::move(t));
    for (auto v : outer) {
      assert(v == 8);
    }
  }

  // 11. sync_wait with value
  {
    larch::thread_pool pool{2};
    // Keep coroutine lambda alive — its captures are referenced
    // by the coroutine frame through `this`.
    auto make_task = [&pool]() -> larch::task<int> {
      std::vector<int> v{10, 20, 30};
      std::atomic<int> sum{0};
      co_await larch::async_for_each(pool, v,
                                     [&sum](int& x) { sum.fetch_add(x); });
      co_return sum.load();
    };
    auto t = make_task();
    int result = larch::sync_wait(pool, std::move(t));
    assert(result == 60);
  }

  // 12. Empty range with async_for_each
  {
    larch::thread_pool pool{2};
    std::vector<int> v;
    auto t = larch::async_for_each(pool, v, [](int&) -> larch::task<void> {
      assert(false);
      co_return;
    });
    larch::sync_wait<void>(pool, std::move(t));
  }

  std::println("All thread_pool tests passed");
  return 0;
}

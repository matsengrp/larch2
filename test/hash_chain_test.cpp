#include <larch/hash_chain.hpp>

#include <cassert>
#include <cstddef>
#include <print>
#include <string>

int main() {
  using set_t = larch::hash_chain<int>;
  using map_t = larch::hash_chain<std::string, int>;
  constexpr auto no_idx = larch::no_idx;

  // 1. Default-constructed hash_chain is empty
  {
    set_t s;
    assert(s.empty());
    assert(s.size() == 0);
    assert(s.bucket_count() == 8);
  }

  // 2. Set: insert and contains
  {
    set_t s;
    auto [idx, inserted] = s.insert(42);
    assert(inserted);
    assert(idx != no_idx);
    assert(s.contains(42));
    assert(s.size() == 1);
    assert(s[idx] == 42);
  }

  // 3. Set: duplicate insert returns false
  {
    set_t s;
    auto [idx1, ins1] = s.insert(10);
    auto [idx2, ins2] = s.insert(10);
    assert(ins1);
    assert(!ins2);
    assert(idx1 == idx2);
    assert(s.size() == 1);
  }

  // 4. Set: find returns index or no_idx
  {
    set_t s;
    assert(s.find(1) == no_idx);
    s.insert(1);
    auto idx = s.find(1);
    assert(idx != no_idx);
    assert(s[idx] == 1);
    assert(s.find(2) == no_idx);
  }

  // 5. Set: count
  {
    set_t s;
    assert(s.count(5) == 0);
    s.insert(5);
    assert(s.count(5) == 1);
  }

  // 6. Set: erase by key
  {
    set_t s;
    s.insert(1);
    s.insert(2);
    s.insert(3);
    assert(s.erase(2));
    assert(!s.contains(2));
    assert(s.contains(1));
    assert(s.contains(3));
    assert(s.size() == 2);
    assert(!s.erase(99));
  }

  // 7. Set: erase by index
  {
    set_t s;
    auto [idx, _] = s.insert(42);
    s.erase(idx);
    assert(!s.contains(42));
    assert(s.empty());
  }

  // 8. Set: emplace
  {
    set_t s;
    auto [idx1, ins1] = s.emplace(7);
    assert(ins1);
    assert(s[idx1] == 7);
    auto [idx2, ins2] = s.emplace(7);
    assert(!ins2);
    assert(idx1 == idx2);
  }

  // 9. Map: insert and find
  {
    map_t m;
    auto [idx, inserted] = m.insert({"hello", 1});
    assert(inserted);
    assert(m.size() == 1);
    assert(m[idx].first == "hello");
    assert(m[idx].second == 1);

    auto found = m.find("hello");
    assert(found == idx);
  }

  // 10. Map: operator[](key) insert-or-default
  {
    map_t m;
    m["x"] = 10;
    assert(m.size() == 1);
    assert(m["x"] == 10);
    m["x"] = 20;
    assert(m.size() == 1);
    assert(m["x"] == 20);
  }

  // 11. Map: at() and at() const
  {
    map_t m;
    m["key"] = 42;
    assert(m.at("key") == 42);

    bool threw = false;
    try {
      m.at("missing");
    } catch (std::out_of_range const&) {
      threw = true;
    }
    assert(threw);

    auto const& cm = m;
    assert(cm.at("key") == 42);
  }

  // 12. Iteration visits all elements
  {
    set_t s;
    s.insert(1);
    s.insert(2);
    s.insert(3);

    int sum = 0;
    int count = 0;
    for (auto& v : s) {
      sum += v;
      ++count;
    }
    assert(count == 3);
    assert(sum == 6);
  }

  // 13. Const iteration
  {
    set_t s;
    s.insert(10);
    s.insert(20);

    auto const& cs = s;
    int sum = 0;
    for (auto const& v : cs) sum += v;
    assert(sum == 30);
  }

  // 14. Clear empties the container
  {
    set_t s;
    s.insert(1);
    s.insert(2);
    s.insert(3);
    s.clear();
    assert(s.empty());
    assert(s.size() == 0);
    assert(!s.contains(1));
  }

  // 15. Rehash preserves all elements
  {
    set_t s;
    for (int i = 0; i < 20; ++i) s.insert(i);
    assert(s.size() == 20);

    s.rehash(64);
    assert(s.bucket_count() == 64);
    assert(s.size() == 20);
    for (int i = 0; i < 20; ++i) assert(s.contains(i));
  }

  // 16. Automatic rehash on load factor exceeded
  {
    set_t s{4};
    s.max_load_factor(1.0f);
    for (int i = 0; i < 10; ++i) s.insert(i);
    assert(s.size() == 10);
    assert(s.bucket_count() > 4);
    for (int i = 0; i < 10; ++i) assert(s.contains(i));
  }

  // 17. Reserve pre-allocates buckets
  {
    set_t s;
    s.reserve(100);
    assert(s.bucket_count() >= 100);
    assert(s.empty());
  }

  // 18. Move construction
  {
    set_t s;
    s.insert(1);
    s.insert(2);

    set_t s2{std::move(s)};
    assert(s.empty());
    assert(s.bucket_count() == 0);
    assert(s2.size() == 2);
    assert(s2.contains(1));
    assert(s2.contains(2));
  }

  // 19. Move assignment
  {
    set_t s;
    s.insert(10);
    s.insert(20);

    set_t s2;
    s2.insert(99);
    s2 = std::move(s);
    assert(s2.size() == 2);
    assert(s2.contains(10));
    assert(s2.contains(20));
    assert(!s2.contains(99));
  }

  // 20. Stress: many insertions and lookups
  {
    set_t s;
    for (int i = 0; i < 1000; ++i) s.insert(i);
    assert(s.size() == 1000);
    for (int i = 0; i < 1000; ++i) assert(s.contains(i));
    assert(!s.contains(1000));
  }

  // 21. Erase and reinsert: chain holes are reused
  {
    set_t s;
    auto [idx1, _1] = s.insert(1);
    auto [idx2, _2] = s.insert(2);
    auto [idx3, _3] = s.insert(3);

    s.erase(1);
    s.erase(2);
    assert(s.size() == 1);

    auto [idx4, _4] = s.insert(4);
    auto [idx5, _5] = s.insert(5);
    // Reused slots should be at previously freed indices
    assert(idx4 < idx3 || idx5 < idx3);
  }

  // 22. Map: erase by key
  {
    map_t m;
    m["a"] = 1;
    m["b"] = 2;
    m["c"] = 3;
    assert(m.erase("b"));
    assert(!m.contains("b"));
    assert(m.contains("a"));
    assert(m.contains("c"));
    assert(m.size() == 2);
  }

  // 23. Load factor
  {
    set_t s{10};
    assert(s.load_factor() == 0.0f);
    s.insert(1);
    s.insert(2);
    s.insert(3);
    s.insert(4);
    s.insert(5);
    float lf = s.load_factor();
    assert(lf > 0.0f && lf <= 1.0f);
  }

  // 24. Insert after clear reuses the container
  {
    set_t s;
    s.insert(1);
    s.insert(2);
    s.clear();
    s.insert(3);
    assert(s.size() == 1);
    assert(s.contains(3));
    assert(!s.contains(1));
  }

  // 25. Map: emplace with pair construction
  {
    map_t m;
    auto [idx, ins] = m.emplace("key", 42);
    assert(ins);
    assert(m[idx].first == "key");
    assert(m[idx].second == 42);
  }

  // 26. Map: insert_or_assign inserts new key
  {
    map_t m;
    auto [idx, inserted] = m.insert_or_assign("key", 10);
    assert(inserted);
    assert(m[idx].first == "key");
    assert(m[idx].second == 10);
    assert(m.size() == 1);
  }

  // 27. Map: insert_or_assign overwrites existing key
  {
    map_t m;
    m.insert_or_assign("key", 10);
    auto [idx, inserted] = m.insert_or_assign("key", 20);
    assert(!inserted);
    assert(m[idx].second == 20);
    assert(m.size() == 1);
  }

  // 28. Map: insert_or_assign with rvalue key
  {
    map_t m;
    std::string k = "hello";
    auto [idx, inserted] = m.insert_or_assign(std::move(k), 5);
    assert(inserted);
    assert(m[idx].first == "hello");
    assert(m[idx].second == 5);
  }

  // 29. Map: insert_or_assign multiple keys
  {
    map_t m;
    m.insert_or_assign("a", 1);
    m.insert_or_assign("b", 2);
    m.insert_or_assign("a", 99);
    assert(m.size() == 2);
    assert(m.at("a") == 99);
    assert(m.at("b") == 2);
  }

  std::println("All hash_chain tests passed");
  return 0;
}

#include <larch/chain.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <print>
#include <stdexcept>
#include <vector>

struct widget {
  std::size_t id, value, extra, tag;
};

int main() {
  using chain_t = larch::chain<widget>;
  using section_t = chain_t::contiguous_section;
  constexpr auto no_idx = larch::no_idx;

  // 1. Default-constructed chain is empty
  {
    chain_t c;
    assert(c.empty());
    assert(c.size() == 0);
  }

  // 2. emplace and operator[] work correctly
  {
    chain_t c;
    auto idx = c.emplace(1, 2, 3);
    assert(idx == 0);
    assert(c.size() == 1);
    assert(!c.empty());
    assert(c[0].id == 1);
    assert(c[0].value == 2);
    assert(c[0].extra == 3);
  }

  // 3. remove decrements size(), subsequent emplace reuses the hole
  {
    chain_t c;
    auto idx0 = c.emplace(1, 0, 0);
    auto idx1 = c.emplace(2, 0, 0);
    assert(c.size() == 2);
    c.remove(idx0);
    assert(c.size() == 1);
    auto idx2 = c.emplace(3, 0, 0);
    assert(idx2 == idx0);
    assert(c[idx2].id == 3);
    assert(c[idx1].id == 2);
  }

  // 4. Hole splitting: remove range of 5, insert 2, then insert 3
  {
    chain_t c;
    for (std::size_t i = 0; i < 8; ++i) c.emplace(i, 0, 0);
    c.remove(1, 5);
    assert(c.size() == 3);

    std::vector<widget> v2 = {{10, 0, 0}, {11, 0, 0}};
    auto sec2 = c.move_range(std::move(v2));
    assert(sec2.start() == 1);

    std::vector<widget> v3 = {{20, 0, 0}, {21, 0, 0}, {22, 0, 0}};
    auto sec3 = c.move_range(std::move(v3));
    assert(sec3.start() == 3);

    assert(c.size() == 8);
    assert(c[1].id == 10);
    assert(c[2].id == 11);
    assert(c[3].id == 20);
    assert(c[4].id == 21);
    assert(c[5].id == 22);
  }

  // 5. move_range from a std::vector
  {
    chain_t c;
    std::vector<widget> v = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    auto sec = c.move_range(std::move(v));
    assert(sec.start() == 0);
    assert(c.size() == 3);
    assert(c[0].id == 1);
    assert(c[1].id == 4);
    assert(c[2].id == 7);
  }

  // 6. copy_range from a std::vector, verify source unchanged
  {
    chain_t c;
    std::vector<widget> v = {{1, 2, 3}, {4, 5, 6}};
    auto sec = c.copy_range(v);
    assert(sec.start() == 0);
    assert(c.size() == 2);
    assert(c[0].id == 1);
    assert(c[1].id == 4);
    assert(v.size() == 2);
    assert(v[0].id == 1);
    assert(v[1].id == 4);
  }

  // 7. Growth: insert enough elements (>8) to trigger reallocation
  {
    chain_t c;
    for (std::size_t i = 0; i < 20; ++i) c.emplace(i, i * 10, i * 100);
    assert(c.size() == 20);
    for (std::size_t i = 0; i < 20; ++i) {
      assert(c[i].id == i);
      assert(c[i].value == i * 10);
      assert(c[i].extra == i * 100);
    }
  }

  // 8. Growth with holes: insert, remove some, insert more to trigger growth
  {
    chain_t c;
    for (std::size_t i = 0; i < 8; ++i) c.emplace(i, 0, 0);
    c.remove(2);
    c.remove(5);
    assert(c.size() == 6);

    for (std::size_t i = 100; i < 110; ++i) c.emplace(i, 0, 0);
    assert(c.size() == 16);

    // Original untouched elements survive reallocation.
    assert(c[0].id == 0);
    assert(c[1].id == 1);
    assert(c[3].id == 3);
    assert(c[4].id == 4);
    assert(c[6].id == 6);
    assert(c[7].id == 7);
  }

  // 9. remove with count > 1
  {
    chain_t c;
    for (std::size_t i = 0; i < 5; ++i) c.emplace(i, 0, 0);
    c.remove(1, 3);
    assert(c.size() == 2);
    assert(c[0].id == 0);
    assert(c[4].id == 4);
  }

  // 10. Bounds check: access idx >= high_mark_ throws out_of_range
  {
    chain_t c;
    c.emplace(1, 2, 3);
    bool threw = false;
    try {
      (void)c[1];
    } catch (std::out_of_range const&) {
      threw = true;
    }
    assert(threw);
  }

  // 11. Empty range returns empty section
  {
    chain_t c;
    std::vector<widget> ev;
    auto s1 = c.move_range(std::move(ev));
    assert(s1.empty());
    std::vector<widget> ev2;
    auto s2 = c.copy_range(ev2);
    assert(s2.empty());
  }

  // 12. Move semantics: move-construct chain
  {
    chain_t c;
    c.emplace(1, 2, 3);
    c.emplace(4, 5, 6);

    chain_t c2(std::move(c));
    assert(c.empty());
    assert(c.size() == 0);
    assert(c2.size() == 2);
    assert(c2[0].id == 1);
    assert(c2[1].id == 4);
  }

  // 13. Freelist ordering: create holes of different sizes, verify best-fit
  {
    chain_t c;
    for (std::size_t i = 0; i < 10; ++i) c.emplace(i, 0, 0);

    c.remove(1, 1);  // hole size 1 at idx 1
    c.remove(4, 3);  // hole size 3 at idx 4
    c.remove(8, 2);  // hole size 2 at idx 8
    assert(c.size() == 4);

    // Best-fit for size 2: smallest hole >= 2 is the size-2 hole at idx 8.
    std::vector<widget> v2 = {{100, 0, 0}, {101, 0, 0}};
    auto sec2 = c.move_range(std::move(v2));
    assert(sec2.start() == 8);

    // Best-fit for size 1: hole at idx 1 (size 1).
    auto idx = c.emplace(200, 0, 0);
    assert(idx == 1);

    // Remaining hole is size 3 at idx 4.
    std::vector<widget> v3 = {{300, 0, 0}, {301, 0, 0}, {302, 0, 0}};
    auto sec3 = c.move_range(std::move(v3));
    assert(sec3.start() == 4);
  }

  // 14. Hole merging: adjacent removes merge into one hole
  {
    chain_t c;
    for (std::size_t i = 0; i < 10; ++i) c.emplace(i, 0, 0);

    c.remove(2, 3);  // hole size 3 at idx 2
    c.remove(5, 2);  // merges with above → hole size 5 at idx 2
    assert(c.size() == 5);

    std::vector<widget> v = {
        {10, 0, 0}, {11, 0, 0}, {12, 0, 0}, {13, 0, 0}, {14, 0, 0}};
    auto sec = c.move_range(std::move(v));
    assert(sec.start() == 2);
    assert(c.size() == 10);
  }

  // 15. Position list integrity: merge_adjacent finds neighbours via position
  // list
  {
    chain_t c;
    for (std::size_t i = 0; i < 20; ++i) c.emplace(i, 0, 0);

    // Create non-adjacent holes.
    c.remove(3);
    c.remove(7);
    c.remove(11);
    c.remove(15);
    assert(c.size() == 16);

    // Remove 12 → merges with hole at 11 → size-2 hole at 11.
    c.remove(12);
    assert(c.size() == 15);

    // The merged hole (size 2 at 11) should satisfy a size-2 request.
    std::vector<widget> v2 = {{200, 0, 0}, {201, 0, 0}};
    auto sec = c.move_range(std::move(v2));
    assert(sec.start() == 11);
    assert(c[11].id == 200);
    assert(c[12].id == 201);

    // Remove 14 → merges with hole at 15 → size-2 hole at 14.
    c.remove(14);
    std::vector<widget> v3 = {{300, 0, 0}, {301, 0, 0}};
    auto sec2 = c.move_range(std::move(v3));
    assert(sec2.start() == 14);
    assert(c[14].id == 300);
    assert(c[15].id == 301);
  }

  // 16. Section range-for: iterate over all elements via section
  {
    chain_t c;
    std::vector<widget> v = {{10, 0, 0}, {20, 0, 0}, {30, 0, 0}, {40, 0, 0}};
    auto sec = c.move_range(std::move(v));
    assert(sec.size() == 4);
    assert(!sec.empty());

    std::vector<std::size_t> ids;
    for (auto& w : sec) ids.push_back(w.id);

    assert(ids.size() == 4);
    assert(ids[0] == 10);
    assert(ids[1] == 20);
    assert(ids[2] == 30);
    assert(ids[3] == 40);
  }

  // 17. Chain-level iteration: skips holes, visits all live elements
  {
    chain_t c;
    std::vector<widget> v = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0},
                             {4, 0, 0}, {5, 0, 0}, {6, 0, 0}};
    c.move_range(std::move(v));

    // Remove elements at indices 1 and 4.
    c.remove(1);
    c.remove(4);

    std::vector<std::size_t> ids;
    for (auto& w : c) ids.push_back(w.id);

    assert(ids.size() == 4);
    assert(ids[0] == 1);
    assert(ids[1] == 3);
    assert(ids[2] == 4);  // id=4 was at index 3
    assert(ids[3] == 6);
  }

  // 18. Chain-level iteration: holes at boundaries
  {
    chain_t c;
    for (std::size_t i = 0; i < 10; ++i) c.emplace(i, 0, 0);

    // Remove a multi-slot range in the middle
    c.remove(3, 3);

    std::vector<std::size_t> ids;
    for (auto& w : c) ids.push_back(w.id);

    assert(ids.size() == 7);
    assert(ids[0] == 0);
    assert(ids[1] == 1);
    assert(ids[2] == 2);
    assert(ids[3] == 6);
    assert(ids[4] == 7);
    assert(ids[5] == 8);
    assert(ids[6] == 9);
  }

  // 19. Empty section: begin() == end(), range-for body never executes
  {
    section_t sec;
    assert(sec.empty());
    assert(sec.size() == 0);

    int count = 0;
    for (auto& w : sec) {
      (void)w;
      ++count;
    }
    assert(count == 0);

    // Also test empty section from empty range.
    chain_t c;
    std::vector<widget> ev;
    auto s = c.move_range(std::move(ev));
    assert(s.empty());

    count = 0;
    for (auto& w : s) {
      (void)w;
      ++count;
    }
    assert(count == 0);
  }

  // 20. Const section iteration
  {
    chain_t c;
    std::vector<widget> v = {{10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
    auto sec = c.move_range(std::move(v));

    const auto& csec = sec;
    std::vector<std::size_t> ids;
    for (auto const& w : csec) ids.push_back(w.id);

    assert(ids.size() == 3);
    assert(ids[0] == 10);
    assert(ids[1] == 20);
    assert(ids[2] == 30);
  }

  // 21. Section operator[]: random access by index
  {
    chain_t c;
    std::vector<widget> v = {{10, 0, 0}, {20, 0, 0}, {30, 0, 0}, {40, 0, 0}};
    auto sec = c.move_range(std::move(v));

    assert(sec[0].id == 10);
    assert(sec[1].id == 20);
    assert(sec[2].id == 30);
    assert(sec[3].id == 40);

    // Bounds check
    bool threw = false;
    try {
      (void)sec[4];
    } catch (std::out_of_range const&) {
      threw = true;
    }
    assert(threw);

    // Const access
    const auto& csec = sec;
    assert(csec[0].id == 10);
  }

  // 22. Section emplace_back: empty section
  {
    chain_t c;
    section_t sec(c, 0, 0);
    sec = sec.emplace_back(widget{42, 0, 0});
    assert(sec.size() == 1);
    assert(sec[0].id == 42);
    assert(c.size() == 1);
  }

  // 23. Section emplace_back: at high_mark fast path
  {
    chain_t c;
    std::vector<widget> v = {{1, 0, 0}, {2, 0, 0}};
    auto sec = c.move_range(std::move(v));
    // Section is at the end of the chain (high_mark)
    assert(sec.start() + sec.size() == c.high_mark());

    sec = sec.emplace_back(widget{3, 0, 0});
    assert(sec.size() == 3);
    assert(sec[0].id == 1);
    assert(sec[1].id == 2);
    assert(sec[2].id == 3);
    assert(sec.start() == 0);  // didn't move
    assert(c.size() == 3);
  }

  // 24. Section emplace_back: adjacent hole fast path
  {
    chain_t c;
    for (std::size_t i = 0; i < 5; ++i) c.emplace(i, 0, 0);
    // Remove index 3 to create a hole right after a section at [0,3)
    c.remove(3);

    section_t sec(c, 0, 3);
    sec = sec.emplace_back(widget{99, 0, 0});
    assert(sec.size() == 4);
    assert(sec[3].id == 99);
    assert(sec.start() == 0);  // didn't move
    assert(c.size() == 5);     // reused the hole
  }

  // 25. Section emplace_back: general reallocation
  {
    chain_t c;
    // Fill: [A, B, X, C, D] where X is not part of our section
    c.emplace(1, 0, 0);   // idx 0
    c.emplace(2, 0, 0);   // idx 1
    c.emplace(99, 0, 0);  // idx 2 (not in section)
    c.emplace(3, 0, 0);   // idx 3
    c.emplace(4, 0, 0);   // idx 4

    // Section covers [0,2) — slot 2 is a live element (not a hole)
    section_t sec(c, 0, 2);
    sec = sec.emplace_back(widget{5, 0, 0});
    assert(sec.size() == 3);
    assert(sec[0].id == 1);
    assert(sec[1].id == 2);
    assert(sec[2].id == 5);
  }

  // 26. Section erase: from end
  {
    chain_t c;
    std::vector<widget> v = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}};
    auto sec = c.move_range(std::move(v));

    sec = sec.erase(3);
    assert(sec.size() == 3);
    assert(sec[0].id == 1);
    assert(sec[1].id == 2);
    assert(sec[2].id == 3);
    assert(c.size() == 3);
  }

  // 27. Section erase: from start
  {
    chain_t c;
    std::vector<widget> v = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}};
    auto sec = c.move_range(std::move(v));

    sec = sec.erase(0);
    assert(sec.size() == 3);
    assert(sec.start() == 1);
    assert(sec[0].id == 2);
    assert(sec[1].id == 3);
    assert(sec[2].id == 4);
    assert(c.size() == 3);
  }

  // 28. Section erase: from middle
  {
    chain_t c;
    std::vector<widget> v = {
        {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}, {5, 0, 0}};
    auto sec = c.move_range(std::move(v));

    sec = sec.erase(2);  // erase id=3
    assert(sec.size() == 4);
    assert(sec[0].id == 1);
    assert(sec[1].id == 2);
    assert(sec[2].id == 4);  // shifted left
    assert(sec[3].id == 5);  // shifted left
    assert(c.size() == 4);
  }

  // 29. Section erase: all elements
  {
    chain_t c;
    std::vector<widget> v = {{1, 0, 0}, {2, 0, 0}, {3, 0, 0}};
    auto sec = c.move_range(std::move(v));

    sec = sec.erase(0, 3);
    assert(sec.size() == 0);
    assert(sec.empty());
    assert(c.size() == 0);
  }

  // 30. Section move_range: insert at beginning
  {
    chain_t c;
    std::vector<widget> v = {{3, 0, 0}, {4, 0, 0}};
    auto sec = c.move_range(std::move(v));

    std::vector<widget> ins = {{1, 0, 0}, {2, 0, 0}};
    sec = sec.move_range(0, std::move(ins));
    assert(sec.size() == 4);
    assert(sec[0].id == 1);
    assert(sec[1].id == 2);
    assert(sec[2].id == 3);
    assert(sec[3].id == 4);
  }

  // 31. Section move_range: insert at middle
  {
    chain_t c;
    std::vector<widget> v = {{1, 0, 0}, {4, 0, 0}};
    auto sec = c.move_range(std::move(v));

    std::vector<widget> ins = {{2, 0, 0}, {3, 0, 0}};
    sec = sec.move_range(1, std::move(ins));
    assert(sec.size() == 4);
    assert(sec[0].id == 1);
    assert(sec[1].id == 2);
    assert(sec[2].id == 3);
    assert(sec[3].id == 4);
  }

  // 32. Section move_range: insert at end (append)
  {
    chain_t c;
    std::vector<widget> v = {{1, 0, 0}, {2, 0, 0}};
    auto sec = c.move_range(std::move(v));

    std::vector<widget> ins = {{3, 0, 0}, {4, 0, 0}};
    sec = sec.move_range(sec.size(), std::move(ins));
    assert(sec.size() == 4);
    assert(sec[0].id == 1);
    assert(sec[1].id == 2);
    assert(sec[2].id == 3);
    assert(sec[3].id == 4);
  }

  // 33. std::sort on a section
  {
    chain_t c;
    std::vector<widget> v = {
        {50, 0, 0}, {10, 0, 0}, {40, 0, 0}, {20, 0, 0}, {30, 0, 0}};
    auto sec = c.move_range(std::move(v));

    std::sort(sec.begin(), sec.end(),
              [](const widget& a, const widget& b) { return a.id < b.id; });

    assert(sec[0].id == 10);
    assert(sec[1].id == 20);
    assert(sec[2].id == 30);
    assert(sec[3].id == 40);
    assert(sec[4].id == 50);
  }

  // 34. Verify std::contiguous_iterator concept
  {
    static_assert(std::contiguous_iterator<section_t::iterator>);
    static_assert(std::contiguous_iterator<section_t::const_iterator>);
  }

  // 35. Chain-level iteration: empty chain
  {
    chain_t c;
    int count = 0;
    for (auto& w : c) {
      (void)w;
      ++count;
    }
    assert(count == 0);
  }

  // 36. Chain-level const iteration
  {
    chain_t c;
    c.emplace(1, 0, 0);
    c.emplace(2, 0, 0);
    c.remove(0);

    const auto& cc = c;
    std::vector<std::size_t> ids;
    for (auto const& w : cc) ids.push_back(w.id);

    assert(ids.size() == 1);
    assert(ids[0] == 2);
  }

  // 37. Reference stability across block growth
  {
    chain_t c;
    for (std::size_t i = 0; i < 8; ++i) c.emplace(i, 0, 0);

    widget* ptr0 = &c[0];
    widget* ptr7 = &c[7];

    // Triggers new block allocation (block 0 cap=8 is full)
    c.emplace(8, 0, 0);

    // Pointers to old elements are still valid
    assert(ptr0->id == 0);
    assert(ptr7->id == 7);
    assert(&c[0] == ptr0);
    assert(&c[7] == ptr7);
  }

  // 38. Multi-block iteration with holes across blocks
  {
    chain_t c;
    for (std::size_t i = 0; i < 20; ++i) c.emplace(i, 0, 0);

    // Remove some from each block
    c.remove(3);   // block 0
    c.remove(10);  // block 1

    std::vector<std::size_t> ids;
    for (auto& w : c) ids.push_back(w.id);

    assert(ids.size() == 18);
    assert(std::find(ids.begin(), ids.end(), 3) == ids.end());
    assert(std::find(ids.begin(), ids.end(), 10) == ids.end());
  }

  // 39. Leftover-as-hole reuse
  {
    chain_t c;
    for (std::size_t i = 0; i < 6; ++i) c.emplace(i, 0, 0);
    // high_mark_ = 6, block 0 cap = 8, 2 slots remaining

    // Request 5 contiguous slots — doesn't fit in remaining 2
    std::vector<widget> v = {
        {10, 0, 0}, {11, 0, 0}, {12, 0, 0}, {13, 0, 0}, {14, 0, 0}};
    auto sec = c.move_range(std::move(v));
    // Leftover 2 slots became a hole, elements placed in block 1
    assert(sec.start() == 8);

    // The leftover hole should be reusable
    std::vector<widget> v2 = {{20, 0, 0}, {21, 0, 0}};
    auto sec2 = c.move_range(std::move(v2));
    assert(sec2.start() == 6);  // reused the leftover hole
    assert(c[6].id == 20);
    assert(c[7].id == 21);
  }

  // 40. No cross-block hole merge
  {
    chain_t c;
    // Fill block 0 (cap 8) completely
    for (std::size_t i = 0; i < 8; ++i) c.emplace(i, 0, 0);
    // Fill into block 1
    for (std::size_t i = 8; i < 16; ++i) c.emplace(i, 0, 0);

    // Remove last element of block 0 and first element of block 1
    c.remove(7);  // hole at 7 (block 0)
    c.remove(8);  // hole at 8 (block 1) — should NOT merge with hole at 7

    // If they merged, we'd have a size-2 hole spanning blocks.
    // Verify they're separate by inserting a 2-element range — it should NOT
    // fit either hole.
    std::vector<widget> v = {{100, 0, 0}, {101, 0, 0}};
    auto sec = c.move_range(std::move(v));
    assert(sec.start() == 16);  // new allocation at high_mark

    // But individual emplaces should reuse each size-1 hole
    auto idx1 = c.emplace(200, 0, 0);
    auto idx2 = c.emplace(201, 0, 0);
    assert((idx1 == 7 || idx1 == 8) && (idx2 == 7 || idx2 == 8) &&
           idx1 != idx2);
  }

  std::println("All chain tests passed");
  return 0;
}

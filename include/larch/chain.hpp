#pragma once

#include <larch/common.hpp>

#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace larch {

template <typename T>
class chain {
 public:
  using index_type = std::size_t;

 private:
  struct hole {
    std::size_t size;
    index_type next_smaller;
    index_type next_bigger;
    index_type next_in_order;  // position-sorted link (ascending by index)
  };

  static constexpr bool needs_padding_ = sizeof(T) < sizeof(hole);

  struct alignas(alignof(T) > alignof(hole) ? alignof(T) : alignof(hole))
      padded_slot {
    std::byte storage[sizeof(hole)];
  };

  using slot_type = std::conditional_t<needs_padding_, padded_slot, T>;

  struct block_info {
    slot_type* data;
    index_type base_index;
    std::size_t capacity;
  };

  std::vector<block_info> blocks_;
  std::size_t high_mark_ = 0;
  std::size_t live_count_ = 0;
  index_type smallest_hole_ = no_idx;
  index_type biggest_hole_ = no_idx;
  index_type first_hole_ = no_idx;

  // --- block helpers -------------------------------------------------------

  slot_type* data_ptr(index_type idx) {
    for (auto& b : blocks_) {
      if (idx < b.base_index + b.capacity) return b.data + (idx - b.base_index);
    }
    __builtin_unreachable();
  }

  const slot_type* data_ptr(index_type idx) const {
    for (auto const& b : blocks_) {
      if (idx < b.base_index + b.capacity) return b.data + (idx - b.base_index);
    }
    __builtin_unreachable();
  }

  bool in_same_block(index_type a, index_type b) const {
    for (auto const& bl : blocks_) {
      index_type end = bl.base_index + bl.capacity;
      if (a < end) return b >= bl.base_index && b < end;
    }
    return false;
  }

  bool has_room_at_end(std::size_t count) const {
    if (blocks_.empty()) return false;
    auto const& last = blocks_.back();
    return last.base_index + last.capacity - high_mark_ >= count;
  }

  // --- element accessors ---------------------------------------------------

  T& elem(index_type idx) {
    if constexpr (needs_padding_)
      return *std::launder(reinterpret_cast<T*>(data_ptr(idx)));
    else
      return *data_ptr(idx);
  }

  T const& elem(index_type idx) const {
    if constexpr (needs_padding_)
      return *std::launder(reinterpret_cast<T const*>(data_ptr(idx)));
    else
      return *data_ptr(idx);
  }

  static T* slot_ptr(slot_type* base, index_type idx) {
    if constexpr (needs_padding_)
      return reinterpret_cast<T*>(&base[idx]);
    else
      return &base[idx];
  }

  T* slot_ptr(index_type idx) {
    if constexpr (needs_padding_)
      return reinterpret_cast<T*>(data_ptr(idx));
    else
      return data_ptr(idx);
  }

  // --- hole access ---------------------------------------------------------

  hole* hole_at(index_type idx) {
    return std::launder(reinterpret_cast<hole*>(data_ptr(idx)));
  }

  const hole* hole_at(index_type idx) const {
    return std::launder(reinterpret_cast<const hole*>(data_ptr(idx)));
  }

  // --- position-list helpers -----------------------------------------------

  void insert_into_position_list(index_type idx) {
    hole* h = hole_at(idx);

    if (first_hole_ == no_idx || idx < first_hole_) {
      h->next_in_order = first_hole_;
      first_hole_ = idx;
      return;
    }

    index_type cur = first_hole_;
    while (hole_at(cur)->next_in_order != no_idx &&
           hole_at(cur)->next_in_order < idx)
      cur = hole_at(cur)->next_in_order;

    h->next_in_order = hole_at(cur)->next_in_order;
    hole_at(cur)->next_in_order = idx;
  }

  void unlink_from_position_list(index_type idx) {
    if (first_hole_ == idx) {
      first_hole_ = hole_at(idx)->next_in_order;
      return;
    }

    index_type cur = first_hole_;
    while (cur != no_idx && hole_at(cur)->next_in_order != idx)
      cur = hole_at(cur)->next_in_order;

    if (cur != no_idx)
      hole_at(cur)->next_in_order = hole_at(idx)->next_in_order;
  }

  // --- freelist operations -------------------------------------------------

  void insert_into_freelist(index_type idx) {
    hole* h = hole_at(idx);
    h->next_in_order = no_idx;
    std::size_t sz = h->size;

    if (smallest_hole_ == no_idx) {
      h->next_smaller = no_idx;
      h->next_bigger = no_idx;
      smallest_hole_ = idx;
      biggest_hole_ = idx;
      insert_into_position_list(idx);
      return;
    }

    // Walk ascending to find sorted-by-size insertion point.
    index_type cur = smallest_hole_;
    index_type prev = no_idx;
    while (cur != no_idx && hole_at(cur)->size < sz) {
      prev = cur;
      cur = hole_at(cur)->next_bigger;
    }

    h->next_smaller = prev;
    h->next_bigger = cur;

    if (prev != no_idx)
      hole_at(prev)->next_bigger = idx;
    else
      smallest_hole_ = idx;

    if (cur != no_idx)
      hole_at(cur)->next_smaller = idx;
    else
      biggest_hole_ = idx;

    insert_into_position_list(idx);
  }

  void unlink_from_freelist(index_type idx) {
    hole* h = hole_at(idx);
    index_type smaller = h->next_smaller;
    index_type bigger = h->next_bigger;

    if (smaller != no_idx)
      hole_at(smaller)->next_bigger = bigger;
    else
      smallest_hole_ = bigger;

    if (bigger != no_idx)
      hole_at(bigger)->next_smaller = smaller;
    else
      biggest_hole_ = smaller;

    unlink_from_position_list(idx);
  }

  index_type find_hole(std::size_t needed) {
    if (smallest_hole_ == no_idx) return no_idx;

    std::size_t small_sz = hole_at(smallest_hole_)->size;
    std::size_t big_sz = hole_at(biggest_hole_)->size;

    if (big_sz < needed) return no_idx;
    if (small_sz >= needed) return smallest_hole_;

    // small_sz < needed <= big_sz -- pick traversal direction
    if (needed - small_sz <= big_sz - needed) {
      // From small end: first with size >= needed
      index_type cur = smallest_hole_;
      while (cur != no_idx) {
        if (hole_at(cur)->size >= needed) return cur;
        cur = hole_at(cur)->next_bigger;
      }
      return no_idx;
    } else {
      // From big end: last one still >= needed
      index_type cur = biggest_hole_;
      index_type best = biggest_hole_;
      while (cur != no_idx && hole_at(cur)->size >= needed) {
        best = cur;
        cur = hole_at(cur)->next_smaller;
      }
      return best;
    }
  }

  void split_hole(index_type hole_idx, std::size_t used) {
    std::size_t old_size = hole_at(hole_idx)->size;
    unlink_from_freelist(hole_idx);

    if (old_size > used) {
      index_type new_idx = hole_idx + used;
      auto* nh = new (data_ptr(new_idx)) hole;
      nh->size = old_size - used;
      nh->next_smaller = no_idx;
      nh->next_bigger = no_idx;
      nh->next_in_order = no_idx;
      insert_into_freelist(new_idx);
    }
  }

  void merge_adjacent(index_type idx, std::size_t size) {
    index_type merged_idx = idx;
    std::size_t merged_size = size;

    index_type before = no_idx;
    index_type after = no_idx;

    // Walk position list to find neighbours.
    index_type cur = first_hole_;
    while (cur != no_idx) {
      hole* h = hole_at(cur);
      if (cur + h->size == idx) {
        if (in_same_block(cur, idx)) before = cur;
      } else if (cur == idx + size) {
        if (in_same_block(idx + size - 1, cur)) after = cur;
        break;
      }
      if (cur > idx + size) break;
      cur = h->next_in_order;
    }

    if (before != no_idx) {
      merged_idx = before;
      merged_size += hole_at(before)->size;
      unlink_from_freelist(before);
    }

    if (after != no_idx) {
      merged_size += hole_at(after)->size;
      unlink_from_freelist(after);
    }

    auto* nh = new (data_ptr(merged_idx)) hole;
    nh->size = merged_size;
    nh->next_smaller = no_idx;
    nh->next_bigger = no_idx;
    nh->next_in_order = no_idx;
    insert_into_freelist(merged_idx);
  }

  // --- allocation helpers --------------------------------------------------

  static slot_type* allocate(std::size_t count) {
    if constexpr (alignof(slot_type) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      return static_cast<slot_type*>(::operator new(
          count * sizeof(slot_type), std::align_val_t{alignof(slot_type)}));
    } else {
      return static_cast<slot_type*>(::operator new(count * sizeof(slot_type)));
    }
  }

  static void deallocate(slot_type* ptr) {
    if constexpr (alignof(slot_type) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      ::operator delete(ptr, std::align_val_t{alignof(slot_type)});
    } else {
      ::operator delete(ptr);
    }
  }

  // Collect (index, size) pairs — already in index order via position list.
  std::vector<std::pair<index_type, std::size_t>> collect_holes() const {
    std::vector<std::pair<index_type, std::size_t>> holes;
    index_type cur = first_hole_;
    while (cur != no_idx) {
      const hole* h = hole_at(cur);
      holes.emplace_back(cur, h->size);
      cur = h->next_in_order;
    }
    return holes;
  }

  void ensure_capacity(std::size_t needed) {
    if (blocks_.empty()) {
      std::size_t cap = 8;
      if (cap < needed) cap = needed;
      slot_type* data = allocate(cap);
      blocks_.push_back({data, 0, cap});
      return;
    }

    auto& last = blocks_.back();
    std::size_t block_end = last.base_index + last.capacity;

    if (block_end - high_mark_ >= needed) return;

    std::size_t remaining = block_end - high_mark_;
    if (remaining > 0) {
      merge_adjacent(high_mark_, remaining);
      high_mark_ = block_end;
    }

    std::size_t new_cap = last.capacity * 2;
    if (new_cap < needed) new_cap = needed;
    slot_type* data = allocate(new_cap);
    blocks_.push_back({data, block_end, new_cap});
  }

  // Destroy all live T objects (skipping holes).
  void destroy_live() {
    if (blocks_.empty() || high_mark_ == 0) return;

    auto holes = collect_holes();
    std::size_t hole_cursor = 0;
    std::size_t i = 0;
    while (i < high_mark_) {
      if (hole_cursor < holes.size() && holes[hole_cursor].first == i) {
        i += holes[hole_cursor].second;
        ++hole_cursor;
      } else {
        std::destroy_at(&elem(i));
        ++i;
      }
    }
  }

  // --- hole query ----------------------------------------------------------

  bool is_hole_at(index_type idx) const {
    index_type cur = first_hole_;
    while (cur != no_idx) {
      if (cur == idx) return true;
      if (cur > idx) return false;
      cur = hole_at(cur)->next_in_order;
    }
    return false;
  }

 public:
  // --- chain-level hole-skipping iterator ----------------------------------

  struct sentinel_t {};

  template <bool IsConst>
  class basic_iterator {
    friend chain;

    using chain_ptr = std::conditional_t<IsConst, const chain*, chain*>;
    using ref_type = std::conditional_t<IsConst, T const&, T&>;
    using ptr_type = std::conditional_t<IsConst, T const*, T*>;

    chain_ptr parent_ = nullptr;
    index_type pos_ = 0;
    index_type end_ = 0;
    index_type next_hole_ = no_idx;
    std::size_t hole_size_ = 0;

    void advance_next_hole() {
      if (next_hole_ == no_idx) return;
      index_type next = parent_->hole_at(next_hole_)->next_in_order;
      if (next != no_idx) {
        next_hole_ = next;
        hole_size_ = parent_->hole_at(next)->size;
      } else {
        next_hole_ = no_idx;
        hole_size_ = 0;
      }
    }

    void skip_holes() {
      while (next_hole_ != no_idx && pos_ < end_) {
        if (pos_ < next_hole_) return;
        pos_ = next_hole_ + hole_size_;
        advance_next_hole();
      }
    }

    basic_iterator(chain_ptr parent)
        : parent_(parent), pos_(0), end_(parent ? parent->high_mark_ : 0) {
      if (!parent || end_ == 0) {
        pos_ = end_;
        return;
      }

      next_hole_ = parent_->first_hole_;
      if (next_hole_ != no_idx) hole_size_ = parent_->hole_at(next_hole_)->size;
      skip_holes();
    }

   public:
    using value_type = T;
    using reference = ref_type;
    using pointer = ptr_type;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

    basic_iterator() = default;

    ref_type operator*() const { return parent_->elem(pos_); }
    ptr_type operator->() const { return &parent_->elem(pos_); }

    basic_iterator& operator++() {
      ++pos_;
      skip_holes();
      return *this;
    }

    basic_iterator operator++(int) {
      auto tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const basic_iterator& other) const {
      return pos_ == other.pos_;
    }
    bool operator==(sentinel_t) const { return pos_ >= end_; }

    index_type index() const { return pos_; }
  };

  using iterator = basic_iterator<false>;
  using const_iterator = basic_iterator<true>;

  iterator begin() { return iterator(this); }
  sentinel_t end() { return {}; }
  const_iterator begin() const { return const_iterator(this); }
  sentinel_t end() const { return {}; }

  // --- padded_section_iterator (for contiguous_section when needs_padding_) -

  template <bool IsConst>
  class padded_section_iterator {
    using byte_ptr = std::conditional_t<IsConst, std::byte const*, std::byte*>;
    using ref_type = std::conditional_t<IsConst, T const&, T&>;
    using ptr_type = std::conditional_t<IsConst, T const*, T*>;

    byte_ptr ptr_ = nullptr;

   public:
    using value_type = T;
    using reference = ref_type;
    using pointer = ptr_type;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::random_access_iterator_tag;
    using iterator_category = std::random_access_iterator_tag;

    padded_section_iterator() = default;
    explicit padded_section_iterator(byte_ptr p) : ptr_(p) {}

    ref_type operator*() const {
      return *std::launder(reinterpret_cast<ptr_type>(ptr_));
    }
    ptr_type operator->() const {
      return std::launder(reinterpret_cast<ptr_type>(ptr_));
    }

    padded_section_iterator& operator++() {
      ptr_ += sizeof(slot_type);
      return *this;
    }
    padded_section_iterator operator++(int) {
      auto t = *this;
      ++(*this);
      return t;
    }
    padded_section_iterator& operator--() {
      ptr_ -= sizeof(slot_type);
      return *this;
    }
    padded_section_iterator operator--(int) {
      auto t = *this;
      --(*this);
      return t;
    }

    padded_section_iterator& operator+=(difference_type n) {
      ptr_ += n * static_cast<difference_type>(sizeof(slot_type));
      return *this;
    }
    padded_section_iterator& operator-=(difference_type n) {
      ptr_ -= n * static_cast<difference_type>(sizeof(slot_type));
      return *this;
    }

    friend padded_section_iterator operator+(padded_section_iterator it,
                                             difference_type n) {
      it += n;
      return it;
    }
    friend padded_section_iterator operator+(difference_type n,
                                             padded_section_iterator it) {
      it += n;
      return it;
    }
    friend padded_section_iterator operator-(padded_section_iterator it,
                                             difference_type n) {
      it -= n;
      return it;
    }
    friend difference_type operator-(padded_section_iterator a,
                                     padded_section_iterator b) {
      return (a.ptr_ - b.ptr_) /
             static_cast<difference_type>(sizeof(slot_type));
    }

    ref_type operator[](difference_type n) const { return *(*this + n); }

    bool operator==(padded_section_iterator other) const {
      return ptr_ == other.ptr_;
    }
    auto operator<=>(padded_section_iterator other) const {
      return ptr_ <=> other.ptr_;
    }
  };

  // --- contiguous_section (purely contiguous random-access subview) --------

  class contiguous_section {
    chain* chain_ = nullptr;
    index_type start_index_ = 0;
    std::size_t count_ = 0;

   public:
    using iterator =
        std::conditional_t<needs_padding_, padded_section_iterator<false>, T*>;
    using const_iterator =
        std::conditional_t<needs_padding_, padded_section_iterator<true>,
                           T const*>;

    contiguous_section() = default;
    contiguous_section(chain& c, index_type start, std::size_t count)
        : chain_(&c), start_index_(start), count_(count) {}

    index_type start() const { return start_index_; }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    T& operator[](std::size_t i) {
      if (i >= count_)
        throw std::out_of_range("contiguous_section: index out of range");
      return chain_->elem(start_index_ + i);
    }

    T const& operator[](std::size_t i) const {
      if (i >= count_)
        throw std::out_of_range("contiguous_section: index out of range");
      return chain_->elem(start_index_ + i);
    }

    iterator begin() {
      if (!count_) return iterator{};
      if constexpr (needs_padding_)
        return iterator{
            reinterpret_cast<std::byte*>(chain_->data_ptr(start_index_))};
      else
        return chain_->data_ptr(start_index_);
    }
    iterator end() {
      if (!count_) return iterator{};
      if constexpr (needs_padding_)
        return iterator{reinterpret_cast<std::byte*>(
            chain_->data_ptr(start_index_) + count_)};
      else
        return chain_->data_ptr(start_index_) + count_;
    }
    const_iterator begin() const {
      if (!count_) return const_iterator{};
      if constexpr (needs_padding_)
        return const_iterator{
            reinterpret_cast<std::byte const*>(chain_->data_ptr(start_index_))};
      else
        return chain_->data_ptr(start_index_);
    }
    const_iterator end() const {
      if (!count_) return const_iterator{};
      if constexpr (needs_padding_)
        return const_iterator{reinterpret_cast<std::byte const*>(
            chain_->data_ptr(start_index_) + count_)};
      else
        return chain_->data_ptr(start_index_) + count_;
    }

    // --- modification (return updated section by value) ------------------

    [[nodiscard]] contiguous_section emplace_back(auto&&... args) {
      if (count_ == 0) {
        start_index_ = chain_->emplace(std::forward<decltype(args)>(args)...);
        count_ = 1;
        return *this;
      }
      if (start_index_ + count_ == chain_->high_mark_ &&
          chain_->has_room_at_end(1)) {
        std::construct_at(chain_->slot_ptr(chain_->high_mark_),
                          std::forward<decltype(args)>(args)...);
        ++chain_->high_mark_;
        ++chain_->live_count_;
        ++count_;
        return *this;
      }
      if (chain_->is_hole_at(start_index_ + count_) &&
          chain_->in_same_block(start_index_, start_index_ + count_)) {
        chain_->split_hole(start_index_ + count_, 1);
        std::construct_at(chain_->slot_ptr(start_index_ + count_),
                          std::forward<decltype(args)>(args)...);
        ++chain_->live_count_;
        ++count_;
        return *this;
      }
      std::vector<T> tmp;
      tmp.reserve(count_ + 1);
      for (std::size_t i = 0; i < count_; ++i)
        tmp.push_back(std::move(chain_->elem(start_index_ + i)));
      tmp.emplace_back(std::forward<decltype(args)>(args)...);
      chain_->remove(start_index_, count_);
      auto new_sec = chain_->move_range(std::move(tmp));
      start_index_ = new_sec.start();
      count_ = new_sec.size();
      return *this;
    }

    [[nodiscard]] contiguous_section erase(std::size_t pos,
                                           std::size_t count = 1) {
      if (count == 0) return *this;
      if (pos + count == count_) {
        chain_->remove(start_index_ + pos, count);
        count_ -= count;
        return *this;
      }
      if (pos == 0) {
        chain_->remove(start_index_, count);
        start_index_ += count;
        count_ -= count;
        return *this;
      }
      for (std::size_t i = pos; i + count < count_; ++i)
        chain_->elem(start_index_ + i) =
            std::move(chain_->elem(start_index_ + i + count));
      chain_->remove(start_index_ + count_ - count, count);
      count_ -= count;
      return *this;
    }

    template <std::ranges::sized_range Range>
    [[nodiscard]] contiguous_section move_range(std::size_t pos, Range&& r) {
      std::size_t rcount = std::ranges::size(r);
      if (rcount == 0) return *this;
      if (count_ == 0) {
        auto new_sec = chain_->move_range(std::forward<Range>(r));
        start_index_ = new_sec.start();
        count_ = new_sec.size();
        return *this;
      }
      if (pos == count_ && start_index_ + count_ == chain_->high_mark_ &&
          chain_->has_room_at_end(rcount)) {
        slot_type* base = chain_->data_ptr(chain_->high_mark_);
        std::size_t i = 0;
        for (auto&& elem : r) {
          std::construct_at(slot_ptr(base, i), std::move(elem));
          ++i;
        }
        chain_->high_mark_ += rcount;
        chain_->live_count_ += rcount;
        count_ += rcount;
        return *this;
      }
      std::vector<T> tmp;
      tmp.reserve(count_ + rcount);
      for (std::size_t i = 0; i < pos; ++i)
        tmp.push_back(std::move(chain_->elem(start_index_ + i)));
      for (auto&& elem : r) tmp.push_back(std::move(elem));
      for (std::size_t i = pos; i < count_; ++i)
        tmp.push_back(std::move(chain_->elem(start_index_ + i)));
      chain_->remove(start_index_, count_);
      auto new_sec = chain_->move_range(std::move(tmp));
      start_index_ = new_sec.start();
      count_ = new_sec.size();
      return *this;
    }
  };

  // --- constructors / assignment -------------------------------------------

  chain() = default;

  ~chain() {
    destroy_live();
    for (auto& b : blocks_) deallocate(b.data);
  }

  chain(chain const&) = delete;
  chain& operator=(chain const&) = delete;

  chain(chain&& other) noexcept
      : blocks_(std::move(other.blocks_)),
        high_mark_(other.high_mark_),
        live_count_(other.live_count_),
        smallest_hole_(other.smallest_hole_),
        biggest_hole_(other.biggest_hole_),
        first_hole_(other.first_hole_) {
    other.high_mark_ = 0;
    other.live_count_ = 0;
    other.smallest_hole_ = no_idx;
    other.biggest_hole_ = no_idx;
    other.first_hole_ = no_idx;
  }

  chain& operator=(chain&& other) noexcept {
    if (this != &other) {
      destroy_live();
      for (auto& b : blocks_) deallocate(b.data);

      blocks_ = std::move(other.blocks_);
      high_mark_ = other.high_mark_;
      live_count_ = other.live_count_;
      smallest_hole_ = other.smallest_hole_;
      biggest_hole_ = other.biggest_hole_;
      first_hole_ = other.first_hole_;

      other.high_mark_ = 0;
      other.live_count_ = 0;
      other.smallest_hole_ = no_idx;
      other.biggest_hole_ = no_idx;
      other.first_hole_ = no_idx;
    }
    return *this;
  }

  // -- element insertion ----------------------------------------------------

  index_type emplace(auto&&... args) {
    index_type idx;
    index_type h = find_hole(1);
    if (h != no_idx) {
      split_hole(h, 1);
      idx = h;
    } else {
      ensure_capacity(1);
      idx = high_mark_++;
    }
    std::construct_at(slot_ptr(idx), std::forward<decltype(args)>(args)...);
    ++live_count_;
    return idx;
  }

  template <std::ranges::sized_range Range>
  contiguous_section move_range(Range&& r) {
    std::size_t count = std::ranges::size(r);
    if (count == 0) return {};

    index_type start;
    index_type h = find_hole(count);
    if (h != no_idx) {
      split_hole(h, count);
      start = h;
    } else {
      ensure_capacity(count);
      start = high_mark_;
      high_mark_ += count;
    }

    slot_type* base = data_ptr(start);
    std::size_t i = 0;
    for (auto&& elem : r) {
      std::construct_at(slot_ptr(base, i), std::move(elem));
      ++i;
    }
    live_count_ += count;
    return {*this, start, count};
  }

  template <std::ranges::sized_range Range>
  contiguous_section copy_range(Range const& r) {
    std::size_t count = std::ranges::size(r);
    if (count == 0) return {};

    index_type start;
    index_type h = find_hole(count);
    if (h != no_idx) {
      split_hole(h, count);
      start = h;
    } else {
      ensure_capacity(count);
      start = high_mark_;
      high_mark_ += count;
    }

    slot_type* base = data_ptr(start);
    std::size_t i = 0;
    for (auto const& elem : r) {
      std::construct_at(slot_ptr(base, i), elem);
      ++i;
    }
    live_count_ += count;
    return {*this, start, count};
  }

  // -- element removal ------------------------------------------------------

  void remove(index_type begin, std::size_t count = 1) {
    for (std::size_t i = 0; i < count; ++i) std::destroy_at(&elem(begin + i));
    live_count_ -= count;
    merge_adjacent(begin, count);
  }

  // -- observers ------------------------------------------------------------

  bool empty() const { return live_count_ == 0; }
  std::size_t size() const { return live_count_; }
  std::size_t high_mark() const { return high_mark_; }

  T& operator[](index_type idx) {
    if (idx >= high_mark_) throw std::out_of_range("chain: index out of range");
    return elem(idx);
  }

  T const& operator[](index_type idx) const {
    if (idx >= high_mark_) throw std::out_of_range("chain: index out of range");
    return elem(idx);
  }
};

}  // namespace larch

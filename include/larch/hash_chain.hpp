#pragma once

#include <larch/chain.hpp>
#include <larch/common.hpp>

#include <cmath>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace larch {

template <typename T>
struct node {
  T value;
  std::size_t next = no_idx;
};

template <typename K, typename V = K, typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class hash_chain {
 public:
  static constexpr bool is_set = std::is_same_v<K, V>;

  using key_type = K;
  using mapped_type = V;
  using stored_type = std::conditional_t<is_set, K, std::pair<K const, V>>;
  using node_type = node<stored_type>;
  using index_type = std::size_t;
  using hasher = Hash;
  using key_equal = KeyEqual;

 private:
  chain<node_type> chain_;
  std::size_t* buckets_ = nullptr;
  std::size_t bucket_count_ = 0;
  float max_load_factor_ = 1.0f;

  // --- helpers -------------------------------------------------------------

  static K const& extract_key(stored_type const& v) {
    if constexpr (is_set)
      return v;
    else
      return v.first;
  }

  std::size_t bucket_for(K const& key) const {
    return Hash{}(key) % bucket_count_;
  }

  static std::size_t* allocate_buckets(std::size_t count) {
    auto* p =
        static_cast<std::size_t*>(::operator new(count * sizeof(std::size_t)));
    std::fill(p, p + count, no_idx);
    return p;
  }

  static void deallocate_buckets(std::size_t* p) { ::operator delete(p); }

  void maybe_rehash() {
    if (bucket_count_ == 0 ||
        static_cast<float>(chain_.size()) / static_cast<float>(bucket_count_) >
            max_load_factor_)
      rehash(bucket_count_ == 0 ? 8 : bucket_count_ * 2);
  }

 public:
  // --- iterator ------------------------------------------------------------

  template <bool IsConst>
  class basic_iterator {
    friend hash_chain;

    using inner_iter =
        std::conditional_t<IsConst, typename chain<node_type>::const_iterator,
                           typename chain<node_type>::iterator>;
    using ref_type =
        std::conditional_t<IsConst, stored_type const&, stored_type&>;
    using ptr_type =
        std::conditional_t<IsConst, stored_type const*, stored_type*>;

    inner_iter it_;

   public:
    using value_type = stored_type;
    using reference = ref_type;
    using pointer = ptr_type;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

    basic_iterator() = default;
    explicit basic_iterator(inner_iter it) : it_{it} {}

    ref_type operator*() const { return (*it_).value; }
    ptr_type operator->() const { return &(*it_).value; }

    basic_iterator& operator++() {
      ++it_;
      return *this;
    }
    basic_iterator operator++(int) {
      auto tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(basic_iterator const& other) const {
      return it_ == other.it_;
    }
    bool operator==(typename chain<node_type>::sentinel_t s) const {
      return it_ == s;
    }
  };

  using iterator = basic_iterator<false>;
  using const_iterator = basic_iterator<true>;
  using sentinel_type = typename chain<node_type>::sentinel_t;

  // --- construction / destruction ------------------------------------------

  hash_chain() : hash_chain{8} {}

  explicit hash_chain(std::size_t initial_bucket_count)
      : buckets_{allocate_buckets(initial_bucket_count)},
        bucket_count_{initial_bucket_count} {}

  ~hash_chain() {
    if (buckets_) deallocate_buckets(buckets_);
  }

  hash_chain(hash_chain const&) = delete;
  hash_chain& operator=(hash_chain const&) = delete;

  hash_chain(hash_chain&& other) noexcept
      : chain_{std::move(other.chain_)},
        buckets_{other.buckets_},
        bucket_count_{other.bucket_count_},
        max_load_factor_{other.max_load_factor_} {
    other.buckets_ = nullptr;
    other.bucket_count_ = 0;
  }

  hash_chain& operator=(hash_chain&& other) noexcept {
    if (this != &other) {
      if (buckets_) deallocate_buckets(buckets_);
      chain_ = std::move(other.chain_);
      buckets_ = other.buckets_;
      bucket_count_ = other.bucket_count_;
      max_load_factor_ = other.max_load_factor_;
      other.buckets_ = nullptr;
      other.bucket_count_ = 0;
    }
    return *this;
  }

  // --- iteration -----------------------------------------------------------

  iterator begin() { return iterator{chain_.begin()}; }
  sentinel_type end() { return {}; }
  const_iterator begin() const { return const_iterator{chain_.begin()}; }
  sentinel_type end() const { return {}; }

  // --- capacity ------------------------------------------------------------

  bool empty() const { return chain_.empty(); }
  std::size_t size() const { return chain_.size(); }

  // --- lookup --------------------------------------------------------------

  index_type find(K const& key) const {
    if (bucket_count_ == 0) return no_idx;
    auto bucket = bucket_for(key);
    auto cur = buckets_[bucket];
    while (cur != no_idx) {
      if (KeyEqual{}(extract_key(chain_[cur].value), key)) return cur;
      cur = chain_[cur].next;
    }
    return no_idx;
  }

  bool contains(K const& key) const { return find(key) != no_idx; }

  std::size_t count(K const& key) const { return contains(key) ? 1 : 0; }

  // --- access by index -----------------------------------------------------

  stored_type& operator[](index_type idx) { return chain_[idx].value; }

  stored_type const& operator[](index_type idx) const {
    return chain_[idx].value;
  }

  // --- map-only access -----------------------------------------------------

  V& operator[](K const& key)
    requires(!is_set)
  {
    auto [idx, inserted] = insert(stored_type{key, V{}});
    return chain_[idx].value.second;
  }

  V& operator[](K&& key)
    requires(!is_set)
  {
    auto [idx, inserted] = insert(stored_type{std::move(key), V{}});
    return chain_[idx].value.second;
  }

  V& at(K const& key)
    requires(!is_set)
  {
    auto idx = find(key);
    if (idx == no_idx) throw std::out_of_range{"hash_chain::at: key not found"};
    return chain_[idx].value.second;
  }

  V const& at(K const& key) const
    requires(!is_set)
  {
    auto idx = find(key);
    if (idx == no_idx) throw std::out_of_range{"hash_chain::at: key not found"};
    return chain_[idx].value.second;
  }

  // --- insertion -----------------------------------------------------------

  std::pair<index_type, bool> insert(stored_type const& value) {
    auto const& key = extract_key(value);

    if (bucket_count_ > 0) {
      auto bucket = bucket_for(key);
      auto cur = buckets_[bucket];
      while (cur != no_idx) {
        if (KeyEqual{}(extract_key(chain_[cur].value), key))
          return {cur, false};
        cur = chain_[cur].next;
      }
    }

    maybe_rehash();
    auto bucket = bucket_for(key);
    auto idx =
        chain_.emplace(node_type{.value = value, .next = buckets_[bucket]});
    buckets_[bucket] = idx;
    return {idx, true};
  }

  std::pair<index_type, bool> insert(stored_type&& value) {
    auto const& key = extract_key(value);

    if (bucket_count_ > 0) {
      auto bucket = bucket_for(key);
      auto cur = buckets_[bucket];
      while (cur != no_idx) {
        if (KeyEqual{}(extract_key(chain_[cur].value), key))
          return {cur, false};
        cur = chain_[cur].next;
      }
    }

    maybe_rehash();
    auto bucket = bucket_for(key);
    auto idx = chain_.emplace(
        node_type{.value = std::move(value), .next = buckets_[bucket]});
    buckets_[bucket] = idx;
    return {idx, true};
  }

  template <typename M>
  std::pair<index_type, bool> insert_or_assign(K const& key, M&& obj)
    requires(!is_set)
  {
    if (bucket_count_ > 0) {
      auto bucket = bucket_for(key);
      auto cur = buckets_[bucket];
      while (cur != no_idx) {
        if (KeyEqual{}(extract_key(chain_[cur].value), key)) {
          chain_[cur].value.second = std::forward<M>(obj);
          return {cur, false};
        }
        cur = chain_[cur].next;
      }
    }

    maybe_rehash();
    auto bucket = bucket_for(key);
    auto idx = chain_.emplace(
        node_type{.value = stored_type{key, std::forward<M>(obj)},
                  .next = buckets_[bucket]});
    buckets_[bucket] = idx;
    return {idx, true};
  }

  template <typename M>
  std::pair<index_type, bool> insert_or_assign(K&& key, M&& obj)
    requires(!is_set)
  {
    if (bucket_count_ > 0) {
      auto bucket = bucket_for(key);
      auto cur = buckets_[bucket];
      while (cur != no_idx) {
        if (KeyEqual{}(extract_key(chain_[cur].value), key)) {
          chain_[cur].value.second = std::forward<M>(obj);
          return {cur, false};
        }
        cur = chain_[cur].next;
      }
    }

    maybe_rehash();
    auto bucket = bucket_for(key);
    auto idx = chain_.emplace(
        node_type{.value = stored_type{std::move(key), std::forward<M>(obj)},
                  .next = buckets_[bucket]});
    buckets_[bucket] = idx;
    return {idx, true};
  }

  template <typename... Args>
  std::pair<index_type, bool> emplace(Args&&... args) {
    maybe_rehash();
    auto idx = chain_.emplace(node_type{
        .value = stored_type{std::forward<Args>(args)...}, .next = no_idx});
    auto const& key = extract_key(chain_[idx].value);
    auto bucket = bucket_for(key);

    auto cur = buckets_[bucket];
    while (cur != no_idx) {
      if (KeyEqual{}(extract_key(chain_[cur].value), key)) {
        chain_.remove(idx);
        return {cur, false};
      }
      cur = chain_[cur].next;
    }

    chain_[idx].next = buckets_[bucket];
    buckets_[bucket] = idx;
    return {idx, true};
  }

  // --- erasure -------------------------------------------------------------

  bool erase(K const& key) {
    if (bucket_count_ == 0) return false;
    auto bucket = bucket_for(key);
    auto cur = buckets_[bucket];
    std::size_t prev = no_idx;

    while (cur != no_idx) {
      if (KeyEqual{}(extract_key(chain_[cur].value), key)) {
        if (prev != no_idx)
          chain_[prev].next = chain_[cur].next;
        else
          buckets_[bucket] = chain_[cur].next;
        chain_.remove(cur);
        return true;
      }
      prev = cur;
      cur = chain_[cur].next;
    }
    return false;
  }

  void erase(index_type idx) {
    auto const& key = extract_key(chain_[idx].value);
    auto bucket = bucket_for(key);
    auto cur = buckets_[bucket];
    std::size_t prev = no_idx;

    while (cur != no_idx) {
      if (cur == idx) {
        if (prev != no_idx)
          chain_[prev].next = chain_[cur].next;
        else
          buckets_[bucket] = chain_[cur].next;
        chain_.remove(cur);
        return;
      }
      prev = cur;
      cur = chain_[cur].next;
    }
  }

  void clear() {
    std::fill(buckets_, buckets_ + bucket_count_, no_idx);
    chain_ = chain<node_type>{};
  }

  // --- bucket interface ----------------------------------------------------

  std::size_t bucket_count() const { return bucket_count_; }

  float load_factor() const {
    return bucket_count_ == 0
               ? 0.0f
               : static_cast<float>(size()) / static_cast<float>(bucket_count_);
  }

  float max_load_factor() const { return max_load_factor_; }
  void max_load_factor(float ml) { max_load_factor_ = ml; }

  void rehash(std::size_t new_bucket_count) {
    if (new_bucket_count < 1) new_bucket_count = 1;
    auto* new_buckets = allocate_buckets(new_bucket_count);

    for (auto it = chain_.begin(); it != chain_.end(); ++it) {
      auto const& key = extract_key((*it).value);
      auto bucket = Hash{}(key) % new_bucket_count;
      (*it).next = new_buckets[bucket];
      new_buckets[bucket] = it.index();
    }

    if (buckets_) deallocate_buckets(buckets_);
    buckets_ = new_buckets;
    bucket_count_ = new_bucket_count;
  }

  void reserve(std::size_t count) {
    auto needed = static_cast<std::size_t>(
        std::ceil(static_cast<float>(count) / max_load_factor_));
    if (needed > bucket_count_) rehash(needed);
  }
};

}  // namespace larch

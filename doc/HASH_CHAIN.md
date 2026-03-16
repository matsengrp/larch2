# `larch::hash_chain<K, V, Hash, KeyEqual>` — Chain-Backed Hash Table

A separate-chaining hash map/set that stores all nodes in a `chain<node<T>>`.
Bucket linked lists are threaded through `next` indices inside each node, so the
entire data structure lives in flat array storage with hole reuse and best-fit
freelist allocation from the underlying chain.

## Why

Standard hash containers (`std::unordered_map`, `std::unordered_set`) scatter
nodes across individual heap allocations, degrading cache locality. By packing
all nodes into a `chain`, lookups traverse contiguous memory. Erased slots
become chain holes that are automatically reclaimed by subsequent insertions —
no tombstones, no lazy deletion bitmaps.

## Set vs map mode

The class template doubles as both a set and a map:

```cpp
hash_chain<int>                         // set of int
hash_chain<std::string, int>            // map from string to int
hash_chain<int, int, MyHash, MyEqual>   // map with custom hash/equality
```

When `V == K`, the container operates in set mode (`stored_type = K`). When
`V != K`, it operates in map mode (`stored_type = std::pair<K const, V>`).
Detection is compile-time: `static constexpr bool is_set = std::is_same_v<K, V>`.

## Public interface

```
namespace larch {
template <typename T> struct node;
template <typename K, typename V = K,
          typename Hash = std::hash<K>,
          typename KeyEqual = std::equal_to<K>>
class hash_chain;
}
```

### Type aliases

| Alias | Definition |
|-------|------------|
| `key_type` | `K` |
| `mapped_type` | `V` |
| `stored_type` | `K` (set) or `std::pair<K const, V>` (map) |
| `node_type` | `node<stored_type>` |
| `index_type` | `std::size_t` |
| `hasher` | `Hash` |
| `key_equal` | `KeyEqual` |

### Members

| Member | Signature | Notes |
|--------|-----------|-------|
| default ctor | `hash_chain()` | Allocates 8 buckets |
| explicit ctor | `hash_chain(size_t bucket_count)` | Custom initial bucket count |
| dtor | `~hash_chain()` | Frees bucket array; chain dtor handles nodes |
| move ctor/assign | `hash_chain(hash_chain&&) noexcept` | Transfers all state, resets source |
| copy | deleted | |
| `find` | `index_type find(K const&) const` | Returns chain index or `no_idx` |
| `contains` | `bool contains(K const&) const` | |
| `count` | `size_t count(K const&) const` | Always 0 or 1 (unique keys) |
| `operator[]` | `stored_type&` / `stored_type const&` | Access by chain index |
| `operator[]` | `V& operator[](K const&)` | Map-only: insert-or-default (requires `!is_set`) |
| `operator[]` | `V& operator[](K&&)` | Map-only: move variant |
| `at` | `V&` / `V const&` | Map-only: throws `out_of_range` if missing |
| `insert` | `pair<index_type, bool> insert(stored_type const&)` | Returns `{index, true}` on insertion, `{existing, false}` on duplicate |
| `insert` | `pair<index_type, bool> insert(stored_type&&)` | Move variant |
| `insert_or_assign` | `pair<index_type, bool> insert_or_assign(K const&, M&&)` | Map-only: inserts or overwrites mapped value (requires `!is_set`) |
| `insert_or_assign` | `pair<index_type, bool> insert_or_assign(K&&, M&&)` | Map-only: move-key variant |
| `emplace` | `pair<index_type, bool> emplace(Args&&...)` | Constructs in-place; removes if duplicate found |
| `erase` | `bool erase(K const&)` | Erase by key, returns true if found |
| `erase` | `void erase(index_type)` | Erase by chain index |
| `clear` | `void clear()` | Resets all buckets to `no_idx`, replaces chain |
| `empty` | `bool empty() const` | |
| `size` | `size_t size() const` | |
| `bucket_count` | `size_t bucket_count() const` | |
| `load_factor` | `float load_factor() const` | `size() / bucket_count()` |
| `max_load_factor` | `float max_load_factor() const` | Default 1.0 |
| `max_load_factor` | `void max_load_factor(float)` | Set threshold |
| `rehash` | `void rehash(size_t)` | Rebuild bucket array |
| `reserve` | `void reserve(size_t)` | Pre-allocate buckets for expected element count |
| `begin` / `end` | `iterator` / `sentinel_type` | Forward iteration over all stored values |

## Node struct

```cpp
template <typename T>
struct node {
    T value;
    std::size_t next = no_idx;  // next node in same bucket
};
```

Each node wraps a stored value and a `next` index linking to the next node in
the same hash bucket. `no_idx` terminates the chain.

## Internal storage

```
buckets_ (raw size_t array)             chain_ (flat node storage)
┌────────┬────────┬────────┬────────┐   ┌────────┬────────┬─ hole ─┬────────┐
│ no_idx │   0    │   2    │ no_idx │   │ node_0 │ node_1 │  hole  │ node_3 │
└────────┴───┬────┴───┬────┴────────┘   └───▲────┴────────┴────────┴───▲────┘
             │        │                     │                          │
             │        └─── bucket 2 head ───┘                          │
             └──────────── bucket 1 head ──────────────────────────────┘
                          (node_3.next = 0)
```

- **`chain<node_type> chain_`** — all nodes in flat storage with hole reuse
- **`std::size_t* buckets_`** — raw array of head indices, each `no_idx`
  initially, allocated with `::operator new` / `::operator delete`
- **`std::size_t bucket_count_`** — number of buckets (default 8)
- **`float max_load_factor_`** — rehash threshold (default 1.0)

The bucket array is a raw `std::size_t*` rather than a `chain<std::size_t>`
because `sizeof(std::size_t) < sizeof(chain::hole)` would trigger padding
(32 bytes per slot instead of 8), and the bucket array never has holes so
chain's freelist adds no value.

## Key extraction

```cpp
static K const& extract_key(stored_type const& v) {
    if constexpr (is_set) return v;
    else return v.first;
}
```

A single compile-time branch handles both modes. All lookup and insertion paths
use `extract_key` to obtain the key from the stored value.

## Insert flow

```
insert(value)
   │
   ├─ extract key, compute bucket
   ├─ walk bucket chain for duplicate ──► found: return {existing, false}
   │
   ├─ maybe_rehash()  ──► if load_factor > max, rehash to 2x buckets
   ├─ recompute bucket (may have changed after rehash)
   │
   ├─ chain_.emplace(node{value, buckets_[bucket]})
   │     └─ chain finds hole or appends, returns index
   │
   ├─ buckets_[bucket] = new_index
   └─ return {new_index, true}
```

`insert_or_assign` (map-only) follows the same lookup-first pattern as `insert`,
but on a duplicate it assigns the new mapped value to the existing node's
`second` field and returns `{existing, false}`. On a miss it inserts normally
and returns `{new_index, true}`.

The `emplace` variant differs: it constructs the node first (to extract the key
from the constructed value), checks for duplicates after, and removes the node
if a duplicate is found.

## Erase flow

```
erase(key)
   │
   ├─ compute bucket
   ├─ walk bucket chain, tracking previous
   │     └─ not found: return false
   │
   ├─ unlink from bucket chain
   │     ├─ prev exists: prev.next = cur.next
   │     └─ head: buckets_[bucket] = cur.next
   │
   ├─ chain_.remove(idx)
   │     └─ slot becomes a reusable hole (merged with adjacent holes)
   │
   └─ return true
```

Erase-by-index follows the same bucket walk but matches on index rather than
key equality.

## Rehashing

```cpp
void rehash(std::size_t new_bucket_count) {
    // allocate new bucket array (all no_idx)
    // iterate all live nodes via chain iterator
    //   recompute bucket, prepend to new bucket list
    // free old bucket array
}
```

Rehashing walks all live nodes using the chain's hole-skipping iterator. Each
node's `next` is rewritten to point to the current head of its new bucket, and
the bucket head is updated. The chain iterator's `index()` method provides each
node's chain index for the bucket assignment.

Automatic rehashing triggers in `maybe_rehash()` whenever `load_factor() >
max_load_factor_`, doubling the bucket count (or initializing to 8 if zero).

## Iteration

The hash_chain iterator wraps the chain's `basic_iterator<node_type>` but
dereferences to `stored_type&` (the node's `value` field) instead of the raw
node. It follows chain's sentinel pattern:

```cpp
for (auto& value : my_hash_chain) { ... }    // value is stored_type&
for (auto& [k, v] : my_hash_map) { ... }     // structured bindings for map mode
```

Iteration order is chain storage order (insertion order modulo hole reuse), not
hash order.

## Complexity

| Operation | Time |
|-----------|------|
| `find` / `contains` / `count` | O(B) average — B = bucket chain length |
| `insert` / `emplace` | O(B + H) amortized — bucket walk + possible rehash |
| `erase` | O(B + H) — bucket walk + chain remove with hole merge |
| `operator[](index)` | O(1) |
| `operator[](key)` (map) | O(B + H) amortized — delegates to insert |
| `at` | O(B) |
| `rehash` | O(N + B_new) — N nodes relinked, B_new buckets initialized |
| `clear` | O(N + B) — chain destruction + bucket reset |
| iteration (full) | O(M) — M = chain high_mark, holes skipped via freelist |
| `size` / `empty` | O(1) |

Where B = average bucket chain length (N / bucket_count under uniform hashing),
H = number of chain holes, N = number of elements, M = chain high_mark.

## Constraints and caveats

### No index stability guarantee across rehash

Chain indices remain stable across rehash (the chain itself is not reallocated).
However, `emplace` may trigger `maybe_rehash` before the duplicate check,
meaning an index returned by a prior `find` could still be valid but the bucket
it lives in may have changed. Always use the returned index from insert/emplace
rather than caching find results across mutations.

### `emplace` constructs then checks

Unlike `insert`, `emplace` constructs the node in the chain first, then checks
for duplicates. If a duplicate exists, the newly constructed node is immediately
removed. This avoids requiring the key to be extractable before construction but
wastes a construct/destroy cycle on duplicate emplace calls.

### Non-copyable

The container is move-only, inheriting this constraint from `chain`.

### Unique keys only

`count()` returns 0 or 1. There is no multi-key variant.

### Iteration order

Iteration follows chain storage order, which reflects insertion order modulo
hole reuse. It is not hash-ordered or sorted.

## Files

| File | Role |
|------|------|
| `include/larch/hash_chain.hpp` | Header-only template: `node<T>` + `hash_chain` |
| `include/larch/chain.hpp` | Underlying storage (`chain<node<T>>`) |
| `test/hash_chain_test.cpp` | 29 assert-based tests (set, map, rehash, erase, move, insert_or_assign, stress) |

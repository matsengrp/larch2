# `larch::chain<T>` — Hole-Reusing Flat Container

A dynamically growing `T[]` with index-based access, a `contiguous_section`
subview for random-access iteration and modification, and chain-level
hole-skipping iteration. Removed elements leave "holes" in the backing array,
tracked by two freelists — size-sorted (for best-fit allocation) and
position-sorted (for adjacency merging and hole-skipping iteration) — embedded
directly in the vacated memory. New insertions consult the freelist to reuse
holes before appending.

## Why

Standard containers don't offer stable index identity with hole reuse.
`std::vector` compacts on erase (invalidating indices). `std::deque` doesn't
reuse interior gaps. Slot maps add a layer of indirection. `chain` gives raw
array performance with O(1) indexed access while recycling freed slots.

## Public interface

```
namespace larch {
template <typename T> class chain;
}
```

| Member | Signature | Notes |
|--------|-----------|-------|
| `index_type` | `std::size_t` | |
| default ctor | `chain() = default` | No allocation |
| dtor | `~chain()` | Destroys live Ts, frees all blocks |
| move ctor/assign | `chain(chain&&) noexcept` | Transfers all state, resets source |
| copy | deleted | |
| `emplace` | `index_type emplace(auto&&... args)` | Returns index of constructed T |
| `move_range` | `contiguous_section move_range(Range&& r)` | Move-constructs from sized range, returns `contiguous_section` |
| `copy_range` | `contiguous_section copy_range(Range const& r)` | Copy-constructs from sized range, returns `contiguous_section` |
| `remove` | `void remove(index_type begin, size_t count = 1)` | Destroys Ts, inserts hole into freelist |
| `empty` | `bool empty() const` | `live_count_ == 0` |
| `size` | `size_t size() const` | Number of live T objects |
| `high_mark` | `size_t high_mark() const` | One past the last used or holed index |
| `operator[]` | `T&` / `T const&` | Throws `out_of_range` if `idx >= high_mark_` |
| `begin` / `end` | `iterator` / `sentinel_t` | Chain-level hole-skipping forward iteration |

The sentinel index `larch::no_idx` (`numeric_limits<size_t>::max()`) is defined in
`common.hpp` at namespace scope, along with `larch::no_idx_v` (`cw<no_idx>`).

Range methods accept any `std::ranges::sized_range` and return an empty
`contiguous_section` for empty ranges. `move_range` and `copy_range` place
elements contiguously; the returned `contiguous_section` covers that span.

## Memory layout

Storage is organized as a sequence of independently allocated blocks. Each
block is a contiguous array of slots. Blocks are never moved or reallocated
after creation — elements are stable in memory once constructed.

```
blocks_[0]                       blocks_[1]
┌──────┬──────┬ hole ┬──────┐    ┌──────┬──────┬──────┬//////////////┐
│  T0  │  T1  │  H0  │  T3  │    │  T4  │  T5  │  T6  │  uninit...  │
└──────┴──────┴──────┴──────┘    └──────┴──────┴──────┴//////////////┘
0      1      2      3      4    4      5      6      7            20
                          base=0, cap=4          base=4, cap=16
                                                        ▲
                                                   high_mark_
```

- `blocks_` — a `std::vector<block_info>` where each entry holds a pointer to
  the block's data, its base index, and its capacity. Allocated with
  `::operator new` (with over-alignment support when
  `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`)
- `high_mark_` — one past the last used or holed index; slots after
  `high_mark_` in the last block are raw uninitialized memory
- `live_count_` — number of live T objects (`size()` returns this)
- Ts are constructed via placement `new` (`std::construct_at`) and destroyed
  via `std::destroy_at`
- Flat indices are contiguous across blocks: block 0 covers
  `[0, cap0)`, block 1 covers `[cap0, cap0 + cap1)`, etc.
- `data_ptr(idx)` resolves a flat index to a `slot_type*` by walking the
  block list — O(B) where B = number of blocks (logarithmic in N due to
  capacity doubling)

## Transparent padding

`chain` internally pads small `T` types so that every slot can hold a `hole`
struct (32 bytes on 64-bit). This removes the old `static_assert(sizeof(T) >=
sizeof(hole))` restriction — `chain` now accepts any `T` size.

- **`needs_padding_`** — compile-time constant: `sizeof(T) < sizeof(hole)`.
  When `false`, no padding machinery is used and `slot_type` is just `T`.
- **`padded_slot`** — an aligned byte array of `sizeof(hole)` bytes, with
  alignment `max(alignof(T), alignof(hole))`. Used as the storage type when
  padding is needed.
- **`slot_type`** — `std::conditional_t<needs_padding_, padded_slot, T>`. Each
  block's data buffer is an array of `slot_type`, so every slot is at least
  large enough to hold a `hole`.
- **`elem()` accessors** — when `needs_padding_` is true, access uses
  `std::launder(reinterpret_cast<T*>(data_ptr(idx)))` to recover the `T` from
  the `padded_slot` storage. When padding is not needed, `elem()` returns
  `*data_ptr(idx)` directly.

The trade-off: small types waste `sizeof(hole) - sizeof(T)` bytes per slot.
For example, a `chain<std::size_t>` uses 32-byte slots for an 8-byte element,
wasting 24 bytes per slot.

## Hole struct and the freelists

When a range of slots is freed, a `hole` struct is written via placement new
into the first slot of that range:

```cpp
struct hole {
    std::size_t size;           // number of contiguous freed slots
    index_type  next_smaller;   // link toward smallest hole  (size-sorted)
    index_type  next_bigger;    // link toward biggest hole   (size-sorted)
    index_type  next_in_order;  // link to next hole by index (position-sorted)
};
```

On 64-bit systems the hole is 32 bytes (four `size_t` fields). When
`sizeof(T) < sizeof(hole)`, the transparent padding mechanism (see above)
ensures each slot is still large enough to hold a hole. Remaining slots in a
multi-slot hole are don't-care memory.

The hole is accessed via `std::launder(reinterpret_cast<hole*>(data_ptr(idx)))`.

### Size-sorted freelist

All holes form a doubly-linked list sorted ascending by `size`. Two head
pointers provide entry from either end:

```
smallest_hole_ ──► H_a ◄──► H_b ◄──► H_c ◄── biggest_hole_
                  size=1    size=3    size=7
               next_bigger►  next_bigger►
               ◄next_smaller ◄next_smaller
```

This list is used by `find_hole` for best-fit allocation.

### Position-sorted freelist

A separate singly-linked list threads the same hole nodes in ascending index
order via `next_in_order`, headed by `first_hole_`:

```
first_hole_ ──► H_x ──► H_y ──► H_z ──► ∅
               idx=2    idx=6    idx=11
            next_in_order► next_in_order►
```

This list is used by `merge_adjacent` (to find positional neighbours in a
single early-exiting walk) and by chain-level iterators (to skip holes while
iterating).

Links use indices (not pointers), so they remain valid across block
allocations without fixup.

## Freelist operations

All freelist operations are private.

### `insert_into_freelist(idx)` — O(H)

Walks the size-sorted list from the smallest end to find the insertion point
(first hole with `size >= new_hole.size`), then splices the new hole in.
Updates `smallest_hole_` or `biggest_hole_` if the new hole lands at either
extreme. Also inserts into the position-sorted list via
`insert_into_position_list`.

### `unlink_from_freelist(idx)` — O(H)

Unlinks from the size-sorted list in O(1) (standard doubly-linked unlink), then
walks the position-sorted singly-linked list to find and patch around `idx`.
The O(H) cost comes from the position-list walk.

### `find_hole(needed)` — O(num_holes)

Finds the smallest hole with `size >= needed` (best-fit). Three fast-path
checks before traversal:

1. Freelist empty → `no_idx`
2. Biggest hole too small → `no_idx`
3. Smallest hole already fits → return it immediately

Otherwise, compares `needed` to the sizes at both heads and starts traversal
from whichever end is closer:

- **From small end**: walk `next_bigger`, return the first hole with
  `size >= needed`
- **From big end**: walk `next_smaller`, return the last hole that still has
  `size >= needed`

Both directions converge on the same result — the smallest fitting hole.

### `split_hole(hole_idx, used)` — O(num_holes)

Unlinks the hole from the freelist. If the hole is larger than `used`, writes a
new (smaller) hole at `hole_idx + used` with the leftover size and inserts it
into the freelist. The O(num_holes) cost comes from `insert_into_freelist`.

### `merge_adjacent(idx, size)` — O(H)

Called by `remove()` after destroying the T objects. Walks the **position-sorted
list** (not the size-sorted list) looking for:

- A hole ending at `idx` (hole `h` where `h + h.size == idx`) → merge before
- A hole starting at `idx + size` → merge after

Both merges are guarded by a **block boundary check** (`in_same_block`):
adjacent holes that span different blocks are kept separate, since their
underlying memory is not contiguous. The walk exits early once past
`idx + size`, since the position list is ascending. If either or both
neighbours are found and in the same block, unlinks them, expands the
`(idx, size)` region to cover the merged range, then writes one combined hole
and inserts it. This prevents fragmentation from repeated remove/insert cycles
on adjacent ranges.

## Growth and block allocation

- **Initial block**: 8 slots (or `needed` if larger)
- **Growth factor**: 2x (or `needed` if larger)

When `ensure_capacity(needed)` is called:

1. If no blocks exist, allocate the first block
2. If the last block has room (`block_end - high_mark_ >= needed`), return
3. If leftover space remains in the current block (`block_end - high_mark_`):
   turn the leftover into a hole via `merge_adjacent` (which also merges with
   any adjacent hole ending at `high_mark_`), then advance `high_mark_` to the
   block's end
4. Allocate a new block with capacity `max(prev_capacity * 2, needed)`, base
   index = previous block's end

**No elements are ever moved after construction.** Old blocks remain in place,
so pointers and references to existing elements stay valid across growth.
Leftover space from step 3 is recycled as a hole and available for future
`find_hole` reuse.

Destruction (`destroy_live()`) uses `collect_holes()` + linear-scan to skip
holes when destroying live T objects. The destructor then deallocates all
blocks.

## Insertion flow

`emplace`, `move_range`, and `copy_range` all follow the same pattern:

```
find_hole(count)
   │
   ├─ found ──► split_hole(hole_idx, count) ──► construct into hole_idx
   │
   └─ no_idx ──► ensure_capacity(count) ──► construct at high_mark_
                                              high_mark_ += count
```

For range insertions, elements are placed contiguously; the returned
`contiguous_section` covers that span (the start of either the reused hole or
the freshly appended region).

## Chain-level iteration

`chain<T>` models `std::ranges::range` via `begin()` / `end()`. The iterator
is a forward iterator paired with a sentinel:

```cpp
chain::iterator       begin();
chain::sentinel_t     end();
chain::const_iterator begin() const;
chain::sentinel_t     end()   const;
```

The iterator walks `[0, high_mark_)` while following the position-sorted
freelist to skip holes. It maintains a cursor into the freelist (`next_hole_`,
`hole_size_`) and advances past each hole as `pos_` reaches it.

Use chain-level iteration for full scans over all live elements:

```cpp
for (auto& elem : my_chain) { ... }
```

This replaces the old pattern of constructing a section over
`[0, high_mark())` — the chain-level iterator handles holes directly.

## `contiguous_section` — random-access contiguous subview

`contiguous_section` is a non-owning, random-access view into a contiguous
span of chain slots. It is returned by `move_range` and `copy_range`, and can
also be constructed directly from a `chain&`, start index, and slot count.

**Invariant**: a `contiguous_section` must cover a hole-free span within a
single block. Sections returned by `move_range` / `copy_range` satisfy this
automatically.

### Members

```cpp
class contiguous_section {
    chain*      chain_;        // parent chain (nullptr for empty sections)
    index_type  start_index_;  // first slot in the span
    std::size_t count_;        // number of live elements in the span
};
```

### Public interface

| Member | Signature | Notes |
|--------|-----------|-------|
| default ctor | `contiguous_section()` | Empty section (`chain_ = nullptr, count_ = 0`) |
| `start` | `index_type start() const` | Start index of the span |
| `size` | `size_t size() const` | Number of elements |
| `empty` | `bool empty() const` | `count_ == 0` |
| `operator[]` | `T&` / `T const&` | Bounds-checked random access (throws `out_of_range`) |
| `begin` / `end` | iterator / const_iterator | `T*` when unpadded; `padded_section_iterator` when padded (see below) |
| `emplace_back` | `[[nodiscard]] contiguous_section emplace_back(auto&&...)` | Append one element |
| `erase` | `[[nodiscard]] contiguous_section erase(size_t pos, size_t count = 1)` | Remove element(s) at position |
| `move_range` | `[[nodiscard]] contiguous_section move_range(size_t pos, Range&&)` | Insert range at position |

### Iterators

When `needs_padding_` is false (i.e. `sizeof(T) >= sizeof(hole)`),
`contiguous_section::iterator` is `T*` and `contiguous_section::const_iterator`
is `T const*`. Both satisfy `std::contiguous_iterator`.

When `needs_padding_` is true, section iterators are `padded_section_iterator`
— a random-access iterator that strides by `sizeof(slot_type)` (rather than
`sizeof(T)`) to step over the padding bytes between elements. It dereferences
via `std::launder(reinterpret_cast<T*>(...))`. The section still satisfies
random-access iteration but is no longer contiguous in the
`std::contiguous_iterator` sense for padded types.

In both cases, `std::sort` and other algorithms work directly:

```cpp
auto sec = my_chain.move_range(my_vector);
std::sort(sec.begin(), sec.end(), comparator);
```

### Modification methods

All modification methods are `[[nodiscard]]` and return the updated section by
value. The returned section must be captured — the old section/iterators/pointers
are invalidated.

```cpp
auto sec = chain.move_range(initial_data);
sec = sec.emplace_back(new_element);   // must capture return
sec = sec.erase(2);                     // must capture return
sec = sec.move_range(1, more_data);     // must capture return
```

#### `emplace_back`

Appends one element. Four fast paths, tried in order:

1. **Empty section** (`count_ == 0`): delegates to `chain.emplace`
2. **At high_mark with room** (`start + count == high_mark` and last block has
   space): constructs in place, extends high_mark. O(1).
3. **Adjacent hole in same block** (hole starts at `start + count`, same
   block): splits the hole, constructs in place. O(H).
4. **General**: moves all elements + new to vector, removes old section,
   re-inserts via `chain.move_range`. O(N + H).

#### `erase`

Removes element(s) at a position within the section. Always in-place — no
reallocation needed.

- **From end** (`pos + count == size`): just removes the tail. O(1) + O(H).
- **From start** (`pos == 0`): removes the head, advances `start_index_`. O(1) + O(H).
- **From middle**: shifts trailing elements left, removes the moved-from tail.
  O(N) + O(H).

#### `move_range` (insert)

Inserts a range of elements at a position within the section.

- **Append at high_mark with room** (`pos == count`, at high_mark, and last
  block has space): constructs in place. O(R).
- **General**: rebuilds via vector, removes old section, re-inserts.
  O(N + R + H).

## Removal flow

```
remove(begin, count)
   │
   ├─ destroy_at each T in [begin, begin+count)
   ├─ live_count_ -= count
   └─ merge_adjacent(begin, count)
        │
        ├─ walk position list for neighbors
        ├─ block boundary check (skip cross-block merges)
        ├─ unlink any adjacent same-block holes
        └─ write combined hole, insert into freelist
```

## Complexity

| Operation | Time |
|-----------|------|
| `emplace` | O(H + B) amortized — find_hole + possible split + block lookup |
| `move_range` / `copy_range` | O(H + N + B) — find_hole + N constructions + block lookup |
| `remove` | O(H + N + B) — N destructions + merge scan + block lookup |
| `operator[]` | O(B) — block lookup |
| `size` / `empty` | O(1) |
| `ensure_capacity` | O(1) amortized — no element movement, just block allocation |
| chain-level iteration | O(M * B) — M = high_mark_, skips holes via freelist |
| section iteration | O(N) — N = section size, no block lookup (raw pointer) |
| section `emplace_back` | O(1) / O(N + H) worst case |
| section `erase` | O(N) + O(H) |
| section `move_range` | O(N + R + H) |

Where H = number of holes, N = number of elements in the section, R = number
of elements in the inserted range, M = high_mark_, B = number of blocks
(logarithmic in M due to capacity doubling).

## Constraints and caveats

### Small element overhead

There is no minimum `sizeof(T)` restriction. When `sizeof(T) < sizeof(hole)`,
chain transparently pads each slot to `sizeof(hole)` (see Transparent padding
above). The trade-off is that small types waste `sizeof(hole) - sizeof(T)`
bytes per slot.

### No hole detection on `operator[]`

`operator[]` checks `idx < high_mark_` but does not verify the slot contains a
live T. Accessing a hole through `operator[]` is undefined behavior — the
caller must track which indices are valid. This is a deliberate trade-off for
O(B) access without an auxiliary bitmap.

### Non-copyable

The container is move-only. Copy construction and copy assignment are deleted.

### Section modification invalidation

Calling `emplace_back`, `erase`, or `move_range` on a `contiguous_section`
invalidates the old section value and any iterators or pointers obtained from
it. Always use the returned section. The `[[nodiscard]]` attribute enforces
this at the call site.

### Sections must not contain holes or span blocks

`contiguous_section` assumes a hole-free span within a single block.
Constructing a section over a range that contains holes or spans block
boundaries and then using `operator[]` or pointer iteration will access
invalid memory — undefined behavior. Sections returned by `move_range` and
`copy_range` are always hole-free and within a single block.

### Block boundary (holes don't cross blocks)

Holes are always contained within a single block. `merge_adjacent` checks
`in_same_block` before merging with neighbouring holes and skips the merge if
the neighbour is in a different block. This means two adjacent holes at a
block boundary remain as separate entries in the freelist.

### Freelist overhead under fragmentation

All freelist-touching operations are O(num_holes). A workload that creates many
small non-adjacent holes will degrade insertion and removal performance
linearly. The merge-on-remove strategy mitigates this for adjacent removals,
but scattered single-element removes will accumulate holes.

## Files

| File | Role |
|------|------|
| `include/ogi/chain.hpp` | Header-only template implementation |
| `test/chain_test.cpp` | 40 assert-based tests |

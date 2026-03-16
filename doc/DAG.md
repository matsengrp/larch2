# `larch::dag<N, E>` — Directed Acyclic Graph Container

A typed directed acyclic graph parameterized on two user-provided enums: `N` for
node kinds and `E` for edge kinds. Users specialize `annotation<V>` and
`interface<V>` templates per enumerator to attach data and behavior to graph
elements. Lightweight view handles (`node_view`, `edge_view`) act as the primary
API surface — they inherit the user's interface and proxy all mutation and
traversal back to the owning `dag`.

All storage is backed by `larch::chain`, so nodes and edges have stable indices,
removed elements leave reusable holes, and iteration skips over gaps
automatically.

## Why

Graph libraries typically force a choice between type-safety and heterogeneity.
A `std::vector<Node>` gives you one node type; variant-based approaches lose
type information at every access. `dag` offers both: compile-time access returns
a concrete `node_view<dag, my_node::leaf>` with the full typed interface, while
runtime access returns a `std::variant` over all view types. The user never
touches topology structs or indices directly — views handle everything.

The enum-driven design also means adding a new node or edge kind is a two-step
process: add an enumerator, specialize the templates. No class hierarchies, no
visitor boilerplate, no registration macros.

## Setup: enums, annotations, interfaces

### Step 1: Define enums

```cpp
enum class my_node { root, branch, leaf };
enum class my_edge { structural, reference };
```

The enumerators are extracted at compile time via `enumerators_list<E>` (which
uses C++26 reflection under the hood — `enumerators_of(^^E)`). Every enumerator
in both enums must have a corresponding `annotation` and `interface`
specialization.

### Step 2: Specialize `annotation<V>` for each enumerator

Annotations hold per-element data. Each specialization is an ordinary struct:

```cpp
template <> struct larch::annotation<my_node::root>       { std::string name; };
template <> struct larch::annotation<my_node::branch>     { int priority = 0; };
template <> struct larch::annotation<my_node::leaf>       { double value = 0.0; };
template <> struct larch::annotation<my_edge::structural> { int weight = 1; };
template <> struct larch::annotation<my_edge::reference>  { std::string label; };
```

Annotations can be any size — `chain` handles padding internally when needed.

### Step 3: Specialize `interface<V>` for each enumerator

Interfaces define the methods available on views. They use explicit object
parameters (deducing `this`) to reach the view's `get_annotation()`:

```cpp
template <> struct larch::interface<my_node::root> {
    auto& name(this auto& self) { return self.get_annotation().name; }
};
template <> struct larch::interface<my_node::branch> {
    auto& priority(this auto& self) { return self.get_annotation().priority; }
};
template <> struct larch::interface<my_node::leaf> {
    auto& value(this auto& self) { return self.get_annotation().value; }
};
template <> struct larch::interface<my_edge::structural> {
    auto& weight(this auto& self) { return self.get_annotation().weight; }
};
template <> struct larch::interface<my_edge::reference> {
    auto& label(this auto& self) { return self.get_annotation().label; }
};
```

The `self` parameter resolves to the concrete `node_view<D, V>` or
`edge_view<D, V>`, so `self.get_annotation()` returns the correctly typed
`annotation<V>&`. This is the mechanism that lets interface methods access
per-kind data without casts or virtual dispatch.

### Step 4: Instantiate the dag

```cpp
using dag_t = larch::dag<my_node, my_edge>;
dag_t d;
```

## Public interface

### `dag<N, E>`

| Member | Signature | Notes |
|--------|-----------|-------|
| `node_enum` | `N` | |
| `edge_enum` | `E` | |
| `index_type` | `std::size_t` | |
| `node_variant_type` | `std::variant<node_view<dag, V>...>` | One alternative per node enumerator |
| `edge_variant_type` | `std::variant<edge_view<dag, V>...>` | One alternative per edge enumerator |
| `append_node<V>()` | `node_view<dag, V>` | Compile-time node creation |
| `append_edge<V>()` | `edge_view<dag, V>` | Compile-time edge creation |
| `append_node(N)` | `node_variant_type` | Runtime node creation |
| `append_edge(E)` | `edge_variant_type` | Runtime edge creation |
| `set_root(node)` | `void` | Accepts any `node_view` |
| `get_root()` | `node_variant_type` | Runtime-typed root access |
| `get_root_as<V>()` | `node_view<dag, V>` | Compile-time root access |
| `get_all_nodes()` | range of `node_variant_type` | Iterates all live nodes |
| `get_all_edges()` | range of `edge_variant_type` | Iterates all live edges |

### `node_view<D, V>`

A lightweight handle (reference + index, 16 bytes) that inherits
`interface<V>`. Not copyable in the usual sense — it holds a reference to the
dag, so it is valid only while the dag is alive and the node has not been
removed.

| Method | Signature | Notes |
|--------|-----------|-------|
| `append_child<EV>()` | `edge_view<D, EV>` | Creates edge, sets edge's parent to this node, adds to children list |
| `append_parent<EV>()` | `edge_view<D, EV>` | Creates edge, sets edge's child to this node, adds to parents list |
| `get_children()` | range of `edge_variant_type` | Outgoing edges |
| `get_parents()` | range of `edge_variant_type` | Incoming edges |
| `remove()` | `void` | Cascading removal — removes all connected edges first |
| `get_annotation()` | `annotation<V>&` | Direct annotation access |
| `index()` | `std::size_t` | Topology index (stable until removed) |

Interface methods (from `interface<V>`) are also available directly on the view.

### `edge_view<D, V>`

Same handle pattern as `node_view`. Inherits `interface<V>`.

| Method | Signature | Notes |
|--------|-----------|-------|
| `set_child(node)` | `void` | Accepts any `node_view`; updates neighbor lists |
| `set_parent(node)` | `void` | Accepts any `node_view`; updates neighbor lists |
| `get_child()` | `node_variant_type` | Runtime-typed child access |
| `get_parent()` | `node_variant_type` | Runtime-typed parent access |
| `get_child_as<NV>()` | `node_view<D, NV>` | Compile-time child access |
| `get_parent_as<NV>()` | `node_view<D, NV>` | Compile-time parent access |
| `remove()` | `void` | Removes edge, updates both endpoint neighbor lists |
| `get_annotation()` | `annotation<V>&` | Direct annotation access |
| `index()` | `std::size_t` | Topology index (stable until removed) |

`set_child` and `set_parent` handle re-assignment: if the edge already has a
child/parent, the old node's neighbor list is updated before the new one is
linked.

## Usage examples

### Building a tree

```cpp
dag_t d;
auto root = d.append_node<my_node::root>();
root.name() = "top";
d.set_root(root);

auto branch = d.append_node<my_node::branch>();
branch.priority() = 5;

auto leaf = d.append_node<my_node::leaf>();
leaf.value() = 3.14;

// Connect root -> branch -> leaf
auto e1 = d.append_edge<my_edge::structural>();
e1.set_parent(root);
e1.set_child(branch);
e1.weight() = 10;

auto e2 = d.append_edge<my_edge::structural>();
e2.set_parent(branch);
e2.set_child(leaf);
```

### Shorthand via `append_child`

```cpp
auto edge = root.append_child<my_edge::structural>();
edge.set_child(branch);
```

`append_child` creates the edge, sets its parent to `root`, and adds it to
`root`'s children list in one call. The child endpoint is set separately because
the edge kind and child kind are independent.

### Iteration

```cpp
// All nodes
for (auto nv : d.get_all_nodes()) {
    std::visit([](auto& node) {
        // node is a concrete node_view<dag_t, some_kind>
    }, nv);
}

// Children of a specific node
for (auto ev : root.get_children()) {
    std::visit([](auto& edge) {
        // edge is a concrete edge_view<dag_t, some_kind>
    }, ev);
}
```

### Runtime (variant) creation

```cpp
auto node_var = d.append_node(my_node::leaf);  // returns node_variant_type
auto edge_var = d.append_edge(my_edge::reference);  // returns edge_variant_type
```

Useful when the kind is determined at runtime (e.g., deserialization).

### Removal

```cpp
edge.remove();   // disconnects from both endpoints
node.remove();   // removes node + all connected edges
```

`node_view::remove()` is cascading: it collects all edge indices from the
node's parent and children neighbor lists, removes each edge (updating the
other endpoint's neighbor list), then removes the node itself.

## Internal storage

### Topology structs

The graph's structure is stored as flat topology records in `chain` containers:

```cpp
struct node_topology<N> {       // ~56 bytes
    N kind_;                    // enum value
    std::size_t self_index_;    // own index in node_topologies_
    std::size_t parents_start_; // start index in neighbors_ chain
    std::size_t parents_count_; // number of parent edges
    std::size_t children_start_;// start index in neighbors_ chain
    std::size_t children_count_;// number of child edges
    std::size_t annotation_index_; // index into per-kind annotation chain
};

struct edge_topology<E> {       // ~40 bytes
    E kind_;
    std::size_t self_index_;
    std::size_t parent_;        // index in node_topologies_
    std::size_t child_;         // index in node_topologies_
    std::size_t annotation_index_;
};
```

`self_index_` is patched after emplacement (the index isn't known until `chain`
returns it). It exists so that `get_all_nodes()` can transform topology records
into variant views without needing the chain iterator to expose its position.

### Neighbor lists

Edge adjacency for each node is stored in a shared `chain<std::size_t>` where
each element is an index into `edge_topologies_`. `chain` handles padding the
small `std::size_t` type internally (see `chain`'s transparent padding).

Each node's parents and children are contiguous runs within this chain, tracked
by `(start, count)` pairs in the node topology. When a neighbor is added or
removed, the entire run is collected, modified, removed from the chain, and
re-inserted as a new contiguous block. This keeps neighbor iteration
cache-friendly at the cost of O(degree) work per mutation.

### Annotation storage

Annotations are stored in a tuple of chains, one per enum value:

```
node_annotations_: tuple<
    chain<annotation<my_node::root>>,
    chain<annotation<my_node::branch>>,
    chain<annotation<my_node::leaf>>
>

edge_annotations_: tuple<
    chain<annotation<my_edge::structural>>,
    chain<annotation<my_edge::reference>>
>
```

Each node/edge topology record stores an `annotation_index_` into the chain for
its kind. The per-kind chain is selected at compile time via `std::get<I>` where
`I = index_in_list<V, enumerators>`. When an annotation type is smaller than
`chain`'s hole size, `chain` transparently pads the slots internally.

### Data member layout

```cpp
template <Enum N, Enum E>
class dag {
    index_type root_ = no_idx;
    chain<std::size_t> neighbors_;                // shared neighbor storage
    chain<node_topology<N>> node_topologies_;     // one entry per node
    chain<edge_topology<E>> edge_topologies_;     // one entry per edge
    annotation_tuple_from<node_enumerators> node_annotations_; // per-kind chains
    annotation_tuple_from<edge_enumerators> edge_annotations_; // per-kind chains
};
```

## Metaprogramming machinery (namespace `detail`)

These are internal helpers that bridge the gap between runtime enum values and
compile-time template parameters.

### `enum_dispatch<nttp_list<Vs...>>::call(val, f)`

Runtime-to-compile-time dispatch. Given a runtime enum value `val`, calls
`f(cw<Vi>{})` for the matching enumerator `Vi`, where `cw<Vi>` is a
`constant_wrapper` carrying the value as a compile-time constant. Uses a fold
expression with short-circuit `||` to find the match.

The return type is `std::common_type_t` of all possible `f(cw<Vs>{})...`
invocations. Result is held in a `std::optional` to avoid requiring
default-constructibility of the return type (necessary because view types hold
references and cannot be default-constructed).

This is the core mechanism behind runtime `append_node(N kind)`,
`make_node_variant`, and annotation removal in `remove()`.

### `node_variant_from<D, nttp_list<Vs...>>`

Maps an `nttp_list` of node enumerators to
`std::variant<node_view<D, Vs>...>`. Similarly `edge_variant_from` for edges.

### `annotation_tuple_from<nttp_list<Vs...>>`

Maps an `nttp_list` to `std::tuple<chain<annotation<Vs>>...>` — one chain per
enumerator.

### `index_in_list<V, nttp_list<Vs...>>`

Compile-time index of enumerator `V` in the list. Used to `std::get<I>` into
the annotation tuple.

## How views work

`node_view` and `edge_view` are defined as complete class types (required by
`std::variant`), but their method bodies that access `dag` internals are
defined out-of-line after the `dag` class body. This two-phase approach
resolves the circular dependency: views need `dag` internals, but `dag` needs
complete view types for its variant typedefs.

Each view inherits from `interface<V>`, giving it the user's custom methods.
The `get_annotation()` method on the view is what makes the interface's
deducing-this pattern work:

```
interface<my_node::root>::name(this auto& self)
    └─ self.get_annotation()           // calls node_view::get_annotation()
        └─ dag_.node_annotations_      // tuple of per-kind chains
            └─ std::get<I>(...)        // select chain for this kind
                └─ [annotation_index_] // index into chain → annotation<V>&
```

## Removal mechanics

### `edge_view::remove()`

1. Remove this edge from the parent node's children neighbor list
2. Remove this edge from the child node's parents neighbor list
3. Remove the edge's annotation from its per-kind annotation chain
4. Remove the edge topology from `edge_topologies_`

### `node_view::remove()` (cascading)

1. Collect all edge indices from this node's parents and children neighbor lists
   into a vector (must snapshot first — the lists will be mutated)
2. For each collected edge:
   - Update the *other* endpoint's neighbor list (remove this edge from it)
   - Remove the edge's annotation
   - Remove the edge topology
3. Remove this node's neighbor entries from the shared neighbor chain
4. Remove the node's annotation
5. Remove the node topology

The snapshot-then-iterate pattern avoids iterator invalidation during the
cascading removal.

## Complexity

| Operation | Time |
|-----------|------|
| `append_node<V>()` | O(H) — chain emplace for annotation + topology |
| `append_edge<V>()` | O(H) — chain emplace for annotation + topology |
| `append_node(N)` / `append_edge(E)` | O(H + K) — dispatch over K enumerators + emplace |
| `set_child` / `set_parent` | O(D + H) — rebuild neighbor run (D = node degree) |
| `get_children` / `get_parents` | O(1) to construct the view; O(D) to iterate |
| `get_all_nodes` / `get_all_edges` | O(M) — M = high_mark of topology chain |
| `node_view::remove()` | O(D * D' + H) — D edges, each touching neighbor lists of degree D' |
| `edge_view::remove()` | O(D + H) — update two neighbor lists |
| `get_annotation()` | O(1) — two indexed lookups |
| `make_node_variant` / `make_edge_variant` | O(K) — dispatch over K enumerators |

Where H = number of holes in the relevant chain, D = node degree, K = number of
enumerators in the enum, M = high water mark of the topology chain.

## Constraints and caveats

### Specialization requirement

Every enumerator in both `N` and `E` must have both an `annotation<V>` and
`interface<V>` specialization defined before `dag` is instantiated. Missing
specializations will produce template errors at instantiation.

### View validity

Views hold a reference to the `dag` and a topology index. A view becomes invalid
if:
- The dag is destroyed or moved from
- The element is removed (its index becomes a hole in the chain)

Using an invalid view is undefined behavior. Views do not track liveness.

### Neighbor list rebuild cost

Every `set_child`, `set_parent`, `append_child`, `append_parent`, and neighbor
removal collects the entire neighbor run into a vector, modifies it, removes the
old run from the chain, and re-inserts the new run. This is O(degree) per
mutation. For nodes with very high degree, this becomes expensive.

### No cycle detection

Despite the name "DAG", the container does not enforce acyclicity. It is the
user's responsibility to avoid creating cycles. The container is a directed
graph; the "acyclic" part is a usage contract, not a runtime invariant.

### Single root

`set_root` / `get_root` track a single root index. The container does not
enforce that only one root exists or that the root is actually a graph root
(zero in-degree). It is a convenience slot for the user to mark a distinguished
node.

## Dependencies

`dag.hpp` builds on:

| Dependency | Role |
|------------|------|
| `common.hpp` | `empty_type`, `Enum` concept, `cw<V>`, `nttp_list`, `enumerators_list<E>`, `no_idx`, `no_idx_v` |
| `chain.hpp` | All storage (`chain<T>`, `chain<T>::contiguous_section`) |
| C++26 reflection | `enumerators_of(^^E)` via `enumerators_list` in `common.hpp` |

## Files

| File | Role |
|------|------|
| `include/larch/dag.hpp` | Header-only template implementation |
| `include/larch/common.hpp` | Reflection-based enum utilities, `no_idx`/`no_idx_v` |
| `include/larch/chain.hpp` | Backing storage container |
| `test/dag_test.cpp` | 12 assert-based tests |

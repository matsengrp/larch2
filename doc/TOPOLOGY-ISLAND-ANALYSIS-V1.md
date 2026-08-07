# Topology-Island Analysis Artifact, Version 1

## 1. Purpose and claim boundary

This document is the normative byte contract for
`topology-island-analysis-v1`. It records exact connected components,
batch-at-equal-score persistent `H_0`, deterministic representatives,
pairwise distances, and typed optimum-component minimax barriers for one
certified edge-index view.

The artifact does not itself establish exhaustive adjacency. Exact and
right-censored results require both:

1. a complete `topology-rspr-edge-index-v1` adjacency certificate; and
2. an independent direct checker covering every unordered vertex pair.

Without matching authority, the same graph calculations are
`observed_graph` only and all exact/censored barrier conclusions are
`unresolved`.

The canonical TSV, UTF-8, percent-encoding, lowercase SHA-256, canonical
integer, null (`-`), boolean, safe-path, regular-file, staging, no-replace, and
all-or-nothing rules from `TOPOLOGY-LANDSCAPE-V2.md` apply.

## 2. Required members

Exactly these regular files occur:

```text
manifest.tsv
scope.tsv
provenance.tsv
files.tsv
component_summary.tsv
component_membership.tsv
h0_events.tsv
optimum_component_barriers.tsv
barrier_paths.tsv
component_representatives.tsv
component_pair_distances.tsv
checker_results.tsv
SHA256SUMS
```

`files.tsv` lists exactly ten payload members other than manifest and outer
checksum:

```text
scope.tsv                         identity
provenance.tsv                    identity
component_summary.tsv            result
component_membership.tsv         result
h0_events.tsv                     result
optimum_component_barriers.tsv   result
barrier_paths.tsv                 proof
component_representatives.tsv     result
component_pair_distances.tsv      result
checker_results.tsv               certificate
```

Its header is:

```text
filename<TAB>role<TAB>data_rows<TAB>sha256
```

`SHA256SUMS` is headerless and lists every other member once as
`sha256<TAB>filename` in byte-sorted filename order.

## 3. Scope and authority

`scope.tsv` is a sorted key/value file containing exactly:

```text
adjacency_certificate_sha256
adjacency_semantics_hash
analysis_policy_hash
authority_class
edge_bearing_bundle_output_data_sha256
edge_table_sha256
external_frontier_mode
independent_checker_certificate_sha256
landscape_semantics_hash
maximum_score_delta
move_relation
q1_optimum_partition
q2_barrier_summary
score_baseline
simple_edge_completeness
source_gate_certificate_sha256
source_tree_table_sha256
tree_count
```

`authority_class` is `exact_induced_view` or `observed_graph`. The initial exact
relation is `rooted_common_prune_reduction_hard_rspr_v1`.
`external_frontier_mode` is initially `not_measured`; later schema-compatible
values are `counted` or `retained` only when a bound external payload exists.
`simple_edge_completeness` is `complete` or `not_complete`.

For an exact artifact, every source, adjacency, bundle/checkpoint, edge, and
checker hash is present and reconciles; completeness is `complete`.
`edge_bearing_bundle_output_data_sha256` may be null only for the Gate-C raw
checkpoint, in which case the adjacency certificate binds the immutable source
selection directly.

Q1 is exactly one of:

```text
single_connected_optimum_plateau
multiple_complete_scope_islands
unresolved
```

Q2 is exactly one of:

```text
not_applicable_single_component
all_finite_by_delta1
all_finite_by_delta2
some_right_censored_above_delta2
unresolved
```

Q1 and Q2 are orthogonal. Multiple optimum components may coexist with finite
or censored pairwise barriers.

The analysis-scope ID is:

```text
SHA256("topology-island-analysis.scope.v1\n" || exact scope.tsv bytes)
```

## 4. Provenance, result identity, and manifest

`provenance.tsv` contains exactly:

```text
command
executable_sha256
producer_commit
producer_dirty
producer_repository
producer_source_state_sha256
toolchain
```

Its ID is:

```text
SHA256("topology-island-analysis.provenance.v1\n" || exact provenance.tsv bytes)
```

The analysis-data hash is:

```text
SHA256("topology-island-analysis.data.v1\n" ||
       SORTED(filename || TAB || sha256 || LF))
```

over `scope.tsv` and all eight result/proof/certificate tables other than
`provenance.tsv`.

`manifest.tsv` contains exactly:

```text
analysis_data_sha256
analysis_provenance_id
analysis_scope_id
files_row_count
files_sha256
schema_name
schema_version
```

Fixed values are `schema_name=topology-island-analysis-v1`,
`schema_version=1`, and `files_row_count=10`.

## 5. Component summary

`component_summary.tsv` has:

```text
score_delta<TAB>absolute_score<TAB>vertex_count<TAB>edge_count<TAB>beta0<TAB>component_size_vector<TAB>isolated_vertex_count<TAB>optimum_containing_component_count<TAB>orphan_component_count<TAB>partition_sha256
```

There is one row for every integer delta from zero through
`maximum_score_delta`, in numeric order. A level contains all vertices and
edges with filtration at most that delta. Component sizes sort decreasingly,
then numerically, as a comma-separated list and sum to `vertex_count`.
`beta0` is its length. An orphan component contains no delta-0 vertex.

`partition_sha256` is:

```text
SHA256("topology-island-analysis.partition.v1\n" ||
       SORTED(topology_sha256 || TAB || component_id || LF))
```

over that level's membership rows.

## 6. Component membership

`component_membership.tsv` has:

```text
score_delta<TAB>topology_sha256<TAB>absolute_score<TAB>topology_score_delta<TAB>component_id<TAB>delta0_ancestor_component_ids
```

Rows sort numerically by level, then by topology hash. Only active vertices
occur. `component_id` is the lexicographically least topology hash in the
component. The final field is `-` for an orphan or a comma-separated sorted set
of canonical delta-0 component IDs inherited by the component.

## 7. Batch persistent H0

`h0_events.tsv` has:

```text
score_delta<TAB>absolute_score<TAB>births<TAB>merges<TAB>surviving_components<TAB>active_vertex_count<TAB>active_edge_count<TAB>partition_sha256<TAB>delta0_ancestor_partition_sha256
```

There is one row per level. All vertices at a score are activated, then all
edges whose maximum endpoint delta is that score are processed as one batch.
The row records aggregate births/merges only; no biological interpretation is
attached to arbitrary elder-rule pairings among tied births. Counts agree with
the static component summary.

## 8. Typed optimum-component barriers

`optimum_component_barriers.tsv` has:

```text
component_low<TAB>component_high<TAB>status<TAB>exact_barrier_delta<TAB>exact_barrier_absolute_score<TAB>strict_lower_bound_delta<TAB>ambient_upper_bound_delta<TAB>path_id<TAB>separation_partition_sha256<TAB>methods_agree
```

Rows cover every unordered pair of delta-0 component IDs including diagonals,
sorted by `(component_low,component_high)`. Status is exactly:

```text
same_component_zero
finite_score_barrier
barrier_right_censored
ambient_barrier_upper_bound
unresolved
infinite_in_scope
```

Rules are:

- diagonal `same_component_zero`: exact delta `0`, exact absolute baseline,
  all bound/path/separation fields null;
- `finite_score_barrier`: distinct components, exact delta `1..maximum`, exact
  absolute score `baseline+delta`, mandatory `path_id`, other bounds null;
- `barrier_right_censored`: exact values/path null, strict lower bound equal to
  the maximum complete delta, mandatory separation-partition hash, and no
  ambient conclusion;
- `ambient_barrier_upper_bound`: mandatory upper bound and path, no exact or
  strict-lower value;
- `unresolved`: all numeric/path/separation fields null; and
- `infinite_in_scope`: permitted only with a separately certified complete
  entire finite-universe graph, which the initial delta-2 WRIC scope lacks.

No numeric infinity sentinel is legal. `methods_agree=true` is required for
every non-unresolved exact/censored result.

## 9. Path witnesses

`barrier_paths.tsv` has:

```text
path_id<TAB>component_low<TAB>component_high<TAB>topology_sequence<TAB>absolute_score_sequence<TAB>score_delta_sequence<TAB>edge_key_sequence<TAB>edge_proof_sequence<TAB>maximum_score_delta<TAB>path_length<TAB>selection_policy
```

Sequences are comma-separated and length-consistent. Topology and proof hashes
cannot contain commas. Every consecutive topology pair is the referenced edge;
every edge proof reconstructs both directions; endpoints belong to the named
delta-0 components; maximum delta and edge-count path length recompute.

The deterministic initial policy is
`bottleneck_dijkstra_lexicographic_topology_tie_v1`. The path ID is:

```text
SHA256("topology-island-analysis.barrier-path.v1\n" ||
       component_low || LF || component_high || LF || topology_sequence || LF ||
       absolute_score_sequence || LF || score_delta_sequence || LF ||
       edge_key_sequence || LF || edge_proof_sequence || LF ||
       selection_policy || LF)
```

A chosen path is a deterministic witness, not a unique or causal connector.

## 10. Representatives and distances

`component_representatives.tsv` has:

```text
score_delta<TAB>component_id<TAB>component_size<TAB>medoid_topology_sha256<TAB>distance_sum<TAB>tie_count<TAB>selection_policy
```

The medoid minimizes total unweighted shortest-path distance within its active
component; topology hash breaks ties. Policy is
`unweighted_component_medoid_min_hash_tie_v1`.

`component_pair_distances.tsv` has:

```text
score_delta<TAB>component_low<TAB>component_high<TAB>medoid_low<TAB>medoid_high<TAB>rooted_rf_distance<TAB>graph_distance_status<TAB>graph_distance
```

The rooted RF distance is the size of the symmetric difference of nontrivial
rooted clades, excluding singleton clades and the full taxon set. Graph status
is `finite`, `disconnected`, or `unavailable`; only `finite` has a numeric
distance. No infinity sentinel is allowed.

## 11. Independent checker results

`checker_results.tsv` has:

```text
check_name<TAB>implementation_id<TAB>source_state_sha256<TAB>executable_sha256<TAB>status<TAB>output_sha256<TAB>detail
```

Rows sort by check name. Required exact checks are:

```text
static_traversal_components
batch_union_find_h0
component_indicator_incidence_rank
bottleneck_dijkstra
kruskal_msf_barriers
path_revalidation
```

Every status is `passed`; implementation IDs identify structurally separate
paths rather than aliases of one result. Output hashes bind their canonical
reports. Additional checks are allowed only in a later schema version because
this version has an exact member/row-name contract.

## 12. Validation and negative controls

A conforming validator checks exact members and checksums; all identities;
authority linkage; graph/table counts; canonical component IDs; memberships,
partitions, and batch counts; Q1/Q2 consistency; barrier nullability and
scope; every path; medoids and distances; checker names/statuses/hashes; and
no-replace publication.

Mandatory negatives include a generic-valid edge omission without matching
adjacency authority, noncanonical component IDs, inconsistent partitions,
within-tie persistence order dependence, invalid beta0, finite barrier without
a path, censored barrier with a path, mixed grammar/ambient bounds, numeric
infinity, invalid edge/proof sequence, wrong path maximum, non-medoid
representative, wrong RF distance, duplicated checker name, and false Q1/Q2
summary.

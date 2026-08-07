# Rooted-rSPR Edge-Index Artifact, Version 1

## 1. Purpose and authority

This document is the normative byte contract for
`topology-rspr-edge-index-v1`. The artifact certifies the complete induced
simple relation among an exact selected set of canonical rooted trees under
one frozen rooted-SPR relation. It is derived from, and never mutates, a
validated `topology-landscape-neutral-v2` parent.

The artifact separates three claims:

```text
complete simple endpoint relation
representative common-prune proof per endpoint pair
complete move-action witness census
```

Only the first is complete here. `edge_proofs.tsv` retains one deterministic
presence proof per edge; it is not a complete action-witness table. External
neighbors and full-grammar membership are outside this schema.

The canonical TSV, UTF-8, percent-encoding, lowercase SHA-256, canonical
integer, null (`-`), boolean, safe-path, regular-file, all-or-nothing loading,
staging, and no-replace rules from `TOPOLOGY-LANDSCAPE-V2.md` apply.

## 2. Exact relation

The initial relation is
`rooted_common_prune_reduction_hard_rspr_v1`. Trees are rooted, have unique
leaf labels, have no unary internal node, treat multifurcations as hard, and
exclude any synthetic universal ancestor from topology identity.

For each non-root node `u` of a canonical tree `T`, detachment removes the
subtree at `u`, retains the parent if at least two children remain, suppresses
the parent if exactly one remains, suppresses a unary biological root, and
canonicalizes the remainder. Its exact signature is:

```text
(canonical moved-subtree bytes, canonical reduced-remainder bytes)
```

Two distinct selected trees are adjacent exactly when they share a signature.
A suppressed target parent reconstructs by edge attachment, a suppressed
target biological root by root-stem attachment, and a retained target parent
by internal-vertex attachment. Both directions are reconstructed during proof
validation.

Fingerprint equality is never topology equality. Fingerprints may select a
comparison bucket, but both canonical signature coordinates are compared byte
for byte. A hash collision can cause extra comparisons but cannot create or
remove an edge.

## 3. Required members

Exactly these regular files occur:

```text
manifest.tsv
semantics.tsv
provenance.tsv
files.tsv
vertices.tsv
cut_census.tsv
topology_edges.tsv
edge_proofs.tsv
ADJACENCY-CERTIFICATE.tsv
SHA256SUMS
```

`SHA256SUMS` is headerless and lists every other member once as
`sha256<TAB>filename` in byte-sorted filename order.
`files.tsv` lists exactly the seven payload files other than manifest and outer
checksum, with roles:

```text
ADJACENCY-CERTIFICATE.tsv    certificate
cut_census.tsv              claim
edge_proofs.tsv             proof
provenance.tsv              identity
semantics.tsv               identity
topology_edges.tsv          table
vertices.tsv                table
```

Its header is:

```text
filename<TAB>role<TAB>data_rows<TAB>sha256
```

## 4. Identities

Every key/value file has header `key<TAB>value`, contains exactly its declared
keys, and sorts by encoded key.

`semantics.tsv` has exactly:

```text
analysis_policy_hash
canonical_topology_encoding
equality_mode
landscape_semantics_hash
move_family
move_relation
move_scope
move_symmetry
pair_coverage_method
pair_decision_coverage
parent_semantic_subset_sha256
parent_tree_table_sha256
polytomy_policy
root_policy
score_max
score_min
signature_algorithm
source_archive_sha256
source_gate_certificate_sha256
tree_count
unordered_pair_denominator
```

Fixed initial tokens are:

```text
canonical_topology_encoding=rooted-labelled-length-grammar-v1
equality_mode=full_canonical_signature_coordinates_v1
move_family=rooted_subtree_prune_regraft
move_relation=rooted_common_prune_reduction_hard_rspr_v1
move_scope=hard_rspr_induced_selected_view_v1
move_symmetry=intrinsic_bidirectional
pair_coverage_method=complete_signature_equivalence_partition
pair_decision_coverage=complete
polytomy_policy=hard_multifurcation
root_policy=rooted_no_synthetic_ua
signature_algorithm=canonical_moved_and_reduced_remainder_v1
```

`score_min <= score_max`; `tree_count` is positive; and
`unordered_pair_denominator = tree_count*(tree_count-1)/2`, with checked
unsigned arithmetic. Parent and source hashes are lowercase SHA-256.

The adjacency-semantics hash is:

```text
SHA256("topology-rspr-edge-index.semantics.v1\n" || exact semantics.tsv bytes)
```

`provenance.tsv` has exactly:

```text
command
executable_sha256
producer_commit
producer_dirty
producer_repository
producer_source_state_sha256
storage_mode
toolchain
worker_count
```

`storage_mode` is `memory` or `external_merge`, chosen before computation.
`worker_count` is a positive integer. The provenance ID is:

```text
SHA256("topology-rspr-edge-index.provenance.v1\n" || exact provenance.tsv bytes)
```

The semantic-subset hash is:

```text
SHA256("topology-rspr-edge-index.semantic-subset.v1\n" ||
       SORTED(filename || TAB || sha256 || LF))
```

over exactly `semantics.tsv`, `vertices.tsv`, `cut_census.tsv`,
`topology_edges.tsv`, and `edge_proofs.tsv`.

## 5. Manifest

`manifest.tsv` has exactly:

```text
adjacency_semantics_hash
artifact_provenance_id
files_row_count
files_sha256
schema_name
schema_version
semantic_subset_sha256
```

Required fixed values are `schema_name=topology-rspr-edge-index-v1`,
`schema_version=1`, and `files_row_count=7`. Every linked identity recomputes.

## 6. Selected vertices

`vertices.tsv` has:

```text
topology_sha256<TAB>grammar_ordinal<TAB>absolute_score<TAB>score_delta
```

Rows sort by topology SHA-256. Hashes and ordinals are unique. Every row equals
the corresponding parent `trees.tsv` row. Scores lie in the inclusive
`[score_min,score_max]` interval, delta equals
`absolute_score-score_min`, and the rows are exactly the parent score-view
projection declared by this artifact. Row count equals `tree_count`.

## 7. Cut census

A canonical cut locator is the slash-separated sequence of zero-based child
indices from the root in canonical child order, for example `0`, `1/0`, or
`2/3/1`. Leading zeroes, empty segments, and the empty root path are forbidden.

`cut_census.tsv` has:

```text
topology_sha256<TAB>expected_nonroot_node_count<TAB>observed_unique_cut_locator_count<TAB>cut_locator_set_sha256<TAB>exact_cut_signature_ledger_sha256
```

Rows sort by topology SHA-256 and have exactly the vertex set. Both counts equal
the number of non-root nodes derived from the immutable canonical parent tree.
The validator derives the complete canonical path set and rejects any missing
or duplicate locator.

For byte string `x`, define `frame(x)=decimal_byte_length || ":" || x`.
The path-set digest is:

```text
SHA256("topology-rspr-edge-index.cut-locators.v1\n" ||
       frame(path_1) || ... || frame(path_n))
```

for paths sorted by unsigned byte order. The exact signature-ledger digest is:

```text
SHA256("topology-rspr-edge-index.cut-signatures.v1\n" ||
       frame(path_1) || frame(moved_1) || frame(remainder_1) || ...)
```

in the same path order. Canonical moved and remainder bytes are recomputed from
the parent tree; digests alone never replace this reconstruction.

## 8. Simple edges

`topology_edges.tsv` has:

```text
edge_key<TAB>topology_low<TAB>topology_high<TAB>move_scope<TAB>endpoint_low_score<TAB>endpoint_high_score<TAB>endpoint_low_delta<TAB>endpoint_high_delta<TAB>filtration<TAB>proof_id
```

Rows sort by `edge_key`; endpoint pairs and edge keys are unique;
`topology_low < topology_high`; endpoints exist in `vertices.tsv`; score and
delta fields agree; and `filtration` is the maximum endpoint delta.

The scope-neutral edge key is:

```text
SHA256("topology-rspr-edge-index.simple-edge.v1\n" ||
       move_relation || LF || topology_low || LF || topology_high || LF ||
       move_scope || LF)
```

The neutral-v2 renderer derives the view-scoped v2 edge ID separately from the
analysis-policy hash. Evidence completeness is not part of `move_scope` or edge
identity.

## 9. Representative edge proofs

`edge_proofs.tsv` has:

```text
proof_id<TAB>edge_key<TAB>topology_low<TAB>topology_high<TAB>low_cut_path<TAB>high_cut_path<TAB>moved_support<TAB>low_source_parent_fate<TAB>high_source_parent_fate<TAB>low_to_high_attachment<TAB>high_to_low_attachment<TAB>moved_topology_sha256<TAB>reduced_remainder_sha256
```

There is exactly one row per edge and every edge references it. When multiple
signatures prove an edge, choose the lexicographically least tuple
`(low_cut_path,high_cut_path)` after exact byte resolution.

Parent fate is `retained`, `suppressed`, or `root_suppressed`. The attachment
used to reconstruct one endpoint is inferred from the cut fate in that target:

```text
retained         -> internal_vertex
suppressed       -> edge
root_suppressed  -> root_stem
```

The validator follows both locators, detaches both moved subtrees, compares
moved and remainder canonical bytes exactly, checks support and both digests,
and reconstructs each endpoint from the common remainder. `moved_support` is
the sorted, length-framed leaf-label set of the moved tree, percent-encoded as a
TSV field.

The proof ID is:

```text
SHA256("topology-rspr-edge-index.edge-proof.v1\n" ||
       edge_key || LF || low_cut_path || LF || high_cut_path || LF)
```

These rows prove edge presence and reversibility. They do not enumerate all
parallel or directed move actions.

## 10. Adjacency certificate

`ADJACENCY-CERTIFICATE.tsv` has exactly:

```text
accepted_edge_count
adjacency_semantics_hash
adjacency_status
candidate_pair_comparisons
cut_record_count
edge_proof_count
edge_proofs_sha256
exact_signature_comparisons
fingerprint_collision_bucket_count
independent_accepted_edge_count
independent_checker_executable_sha256
independent_checker_source_state_sha256
independent_direct_pair_predicate_evaluations
independent_edge_set_sha256
largest_signature_class_size
pair_decision_coverage
semantic_subset_sha256
topology_edges_sha256
tree_count
unordered_pair_denominator
vertices_sha256
```

`adjacency_status=complete` requires all fields. Counts are canonical unsigned
integers. `cut_record_count` equals the cut-census sum; proof and accepted-edge
counts equal their tables; the independent direct-pair count equals the pair
denominator; independent edge count/hash equal the primary edge table; and
`pair_decision_coverage=complete` agrees with semantics. Runtime/RSS remain
provenance-side operational evidence and do not enter the semantic subset.

## 11. Validation order and completeness authority

A conforming validator performs:

1. exact member, regular-file, safe-path, canonical-byte, and checksum checks;
2. exact table headers, key sets, encodings, ordering, and file-ledger checks;
3. manifest, semantics, provenance, and semantic-subset identity checks;
4. immutable parent bundle and source archive/certificate binding;
5. exact selected vertex projection and pair denominator checks;
6. complete canonical cut-path derivation and both cut-ledger hashes;
7. endpoint, edge-key, score, filtration, and uniqueness checks;
8. full-byte proof replay and bidirectional endpoint reconstruction;
9. certificate counter/table/hash reconciliation; and
10. independent checker count and edge-set equality.

Generic neutral-v2 `simple_edge=complete` validation checks only internal row
consistency. An exact or right-censored topology claim additionally requires a
matching complete adjacency certificate under this contract. A self-consistent
edge omission may pass generic neutral validation but fails this authority
layer.

## 12. Determinism and negative tests

W1/W8 semantic parity covers exact bytes of `semantics.tsv`, `vertices.tsv`,
`cut_census.tsv`, `topology_edges.tsv`, and `edge_proofs.tsv`, plus their
semantic-subset hash. Provenance, command, worker count, manifest, certificate,
file ledger, and outer checksum may differ. Worker parity is a metamorphic
determinism check, not an independent algorithm or biological replicate.

Mandatory negative tests include missing/duplicated cut locators, omitted or
extra edges, self/reversed/duplicate endpoints, wrong score/filtration, invalid
proof paths, unequal moved/remainder bytes, wrong parent fate/attachment,
unreconstructed endpoints, false pair/cut/checker counts, source drift,
digest-only equality, forced fingerprint collisions, heuristic/capped
coverage, and replacement of an existing publication.

# Larch Topology Score-Band Raw Handoff, Version 1

## 1. Purpose and authority

This document defines the exact larch-owned evidence handoff used to render a
neutral topology-landscape v2 bundle. It is a producer certificate, not a
replacement for the neutral contract. It preserves grammar-specific score and
materialization evidence so the renderer performs a total mechanical
projection without rescoring or inventing completeness.

The schema name is `larch-topology-score-band-raw-v1`; the version is `1`.

## 2. Canonical byte rules and members

The LF-only TSV, canonical percent-encoding, null token, canonical integer,
lowercase SHA-256, safe direct-regular-file, exact member, staging, and
no-replace rules are those of the neutral topology-landscape contract.

Exactly these files occur:

```text
manifest.tsv
semantics.tsv
provenance.tsv
files.tsv
score_census.tsv
selected_topologies.tsv
SHA256SUMS
```

`SHA256SUMS` has no header, sorts by filename, and binds every other member
exactly once.

## 3. Manifest and identities

`manifest.tsv` has header `key<TAB>value` and exactly:

```text
files_row_count
files_sha256
provenance_id
schema_name
schema_version
semantic_data_sha256
semantics_hash
```

The manifest requires the schema name/version above.

`semantics.tsv` has header `key<TAB>value` and exactly:

```text
alignment_sha256
alphabet
ambiguity_policy
canonical_topology_encoding
digest_algorithm
edge_weight
endpoint_filtration
face_policy
grammar_construction
grammar_digest
grammar_semantic_digest
grammar_topology_count
hodge_metric
input_content_sha256
move_family
move_relation
move_symmetry
null_family
numeric_policy
parsimony_model
polytomy_policy
reference_sha256
root_policy
score_baseline
score_view
site_pattern_digest
taxon_labels_sha256
taxon_order
universe
ua_scoring
```

Its identity is:

```text
SHA256("larch.topology-score-band.semantics.v1\n" || exact_file_bytes)
```

Its fixed tokens and digest meanings are exactly the corresponding neutral-v2
landscape and analysis values. TI-2 additionally fixes `edge_weight=unit`,
`hodge_metric=not_applicable`, `null_family=not_applicable`, and
`polytomy_policy=hard_multifurcation`. `score_view` uses
`absolute_score_interval_inclusive_v1:<minimum>:<maximum>`.
`score_baseline`, both score-view endpoints, every census score, and every
selected/oracle score are at most `INT64_MAX`. The score-view projection is
nonempty.
Every other field specified as a canonical nonnegative integer fits
`UINT64_MAX`.

`provenance.tsv` has exactly:

```text
command
derivation_ref
producer_commit
producer_dirty
producer_repository
provider_input_sha256
provider_legacy_dag_semantic_sha256
reference_input_sha256
seed_tree_input_sha256
toolchain
worker_count
```

Its identity is:

```text
SHA256("larch.topology-score-band.provenance.v1\n" || exact_file_bytes)
```

The three input hashes bind raw compressed/artifact bytes. The legacy provider
digest remains in the `larch.dag.semantic.ndjson` namespace and is not a
grammar digest. Dirty and worker-count grammars follow the neutral contract.

The semantic-data identity is:

```text
SHA256("larch.topology-score-band.semantic-data.v1\n" ||
       SORTED(filename || TAB || sha256 || LF))
```

over exactly `semantics.tsv`, `score_census.tsv`, and
`selected_topologies.tsv`. It excludes provenance.

## 4. File ledger

`files.tsv` has:

```text
filename<TAB>role<TAB>row_count<TAB>sha256
```

It contains exactly:

```text
provenance.tsv              identity
score_census.tsv            claim
selected_topologies.tsv     table
semantics.tsv               identity
```

Rows sort by encoded filename. Row counts exclude headers.
`manifest.files_row_count=4`, and `files_sha256` binds exact ledger bytes.

## 5. Complete ordinal/score ledger

`score_census.tsv` is byte-compatible with neutral v2:

```text
grammar_ordinal<TAB>absolute_score
```

Rows use numeric ordinal order. Both cells are required canonical nonnegative
integers, and scores are at most `INT64_MAX`. Ordinals are exactly the contiguous range
`0 .. grammar_topology_count-1`. The row count equals
`grammar_topology_count`. The declared score view is projected from this
ledger; a human stdout histogram is not evidence input.

## 6. Selected topology evidence

`selected_topologies.tsv` has exactly this header:

```text
grammar_ordinal<TAB>absolute_score<TAB>incremental_fitch_score<TAB>incremental_fitch_status<TAB>selected_grammar_sankoff_score<TAB>selected_grammar_sankoff_status<TAB>selected_grammar_sankoff_oracle<TAB>selected_production_keys<TAB>artifact_local_replay_ids<TAB>legacy_selection_sha256<TAB>canonical_bytes<TAB>topology_sha256<TAB>leaf_count<TAB>root_arity_summary<TAB>materialized_fitch_score<TAB>materialized_fitch_status<TAB>reload_fitch_score<TAB>reload_fitch_status<TAB>tree_native_sankoff_score<TAB>tree_native_sankoff_status<TAB>tree_native_sankoff_oracle<TAB>materialized_valid<TAB>reload_valid<TAB>production_origin<TAB>legacy_dag_semantic_sha256<TAB>legacy_dag_clades_sha256<TAB>legacy_dag_productions_sha256
```

Rows use numeric ordinal order. They are exactly the census rows inside
`score_view`, with no missing or extra ordinal.

Canonical bytes use `rooted-labelled-length-grammar-v1`; topology SHA-256 uses
the stable neutral-v1 topology domain. Canonical bytes and topology hashes are
unique, and every tree has the declared common taxon set.

All five score observations are required, have status `verified`, and equal
`absolute_score`. The selected-grammar and tree-native Sankoff oracle names
are required, unequal nonempty method tokens; neither may be the reserved
`not_applicable` token because every TI-2 raw status is verified. Their meanings are:

1. incremental generalized Fitch on the selected grammar topology;
2. stateless Sankoff on the selected grammar topology;
3. generalized Fitch after arbitrary-arity tree materialization;
4. generalized Fitch after protobuf serialization/reload and fresh assignment;
5. direct stateless Sankoff over parsed canonical topology child adjacency.

Both validation flags are `true`. Production origin is `known`, `unknown`,
or `not_applicable`. Production keys and replay IDs are non-null.

All four audit digests are required lowercase SHA-256 values for TI-2. The
legacy selection digest remains in `larch.direct-kary-topology.v1`; the three
DAG digests remain in their declared larch canonical-DAG namespaces. None is
neutral topology identity.

## 7. Mandatory raw validation

A conforming raw validator checks, in order:

1. exact member/type/path and canonical bytes;
2. outer checksum, file-ledger, row-count, and payload hashes;
3. exact manifest/semantics/provenance key sets and linked identities;
4. fixed tokens, hash, integer, score-view, and provenance grammar;
5. streaming census contiguity/count/nonnegative scores;
6. exact selected ordinal/score set equality with the census view;
7. all five score/status equalities;
8. canonical topology parse/hash/leaf/root/taxon uniqueness;
9. selected production, validation, origin, and audit-digest fields; and
10. semantic-data identity.

Validation is all-or-nothing. The validator may keep the census in a compact
typed score vector and must not allocate one generic string map per census row.

Required mutations include outer and table hashes, wrong schema/domain, missing
or substituted census ordinal, census-selected score mismatch, interval change,
duplicate canonical topology, bad topology hash/taxon set, absent oracle
coverage, false validation flag, malformed audit digest, and noncanonical
numeric ordering.

## 8. Mechanical neutral-v2 projection

The renderer independently revalidates raw bytes and maps:

- raw semantics to neutral landscape semantics plus analysis policy;
- raw provider identity to neutral artifact provenance, with
  `input_artifact_sha256=provider_input_sha256` and `derivation_ref` binding
  raw `semantic_data_sha256`, raw `provenance_id`, and exact renderer-script
  SHA-256;
- the census ledger byte-for-byte to neutral `score_census.tsv`;
- selected identity and five score observations to neutral `trees.tsv`;
- grammar coordinates, production keys, provider hash, origin, and four audit
  digests to neutral grammar provenance;
- empty edge and witness tables with applicable-but-unmeasured completeness;
- no search trace with not-applicable trace completeness; and
- complete census, selected ordinal/topology, and five score-verification
  dimensions with census-derived denominators.

Complete `score_census` and `grammar_ordinal` claims use the frozen method token
`larch_validated_full_ordinal_score_ledger_v1`. Complete
`canonical_topology` and the five selected score-verification claims use
`larch_selected_row_materialize_reload_sankoff_validation_v1`. Neither token
is caller-selectable. The first records full raw-v1 ledger/coverage validation;
the second records selected-row canonicalization, materialization, reload, and
the required Fitch/Sankoff score observations. The five observations are not a
claim of five independent algorithms; only the canonical-tree Sankoff path is
separately implemented from the grammar scorer and Fitch implementation.

The renderer computes neutral identities and ledgers from rendered bytes. It
does not copy raw hashes into a neutral identity field unless the mapping above
requires it. Both independent neutral validators must accept the same staging
directory before no-replace publication and must re-open the destination.
Publication first attempts an atomic directory `RENAME_NOREPLACE`. Only when
the filesystem reports that primitive unsupported may it reserve the output
with an exclusive directory creation and copy, whose transient partial member
set remains invalid to concurrent readers.

This projection is total in the following precise sense. The 30 raw semantic
fields partition into the 24 landscape fields and six analysis-policy fields;
the renderer adds only the recomputed landscape cross-link. Every selected-row
field either appears in `trees.tsv` or `grammar_topology_provenance.tsv`, or is
one of the two `*_valid` guards that must be `true` before any row is emitted.
Neutral producer command/commit/dirty/repository/toolchain fields describe the
renderer, not the upstream larch producer; renderer worker count is one. The
command is a normalized, internally constructed descriptor binding every
transformation/content argument by value or SHA-256, not caller-authored free
text. Renderer repository, commit, dirty state, and toolchain metadata are
bound by the adjacent producer fields. All raw producer fields, including
command, seed, reference-input,
legacy-DAG, and derivation fields, remain bound through the raw `provenance_id`
named in neutral `derivation_ref`; they are not silently discarded. The raw
handoff (or a content-addressed certificate containing its provenance bytes)
must remain retrievable wherever the neutral artifact is retained. Renderer-generated
empty adjacency/search tables carry only the explicitly unknown or
not-applicable claims listed above.

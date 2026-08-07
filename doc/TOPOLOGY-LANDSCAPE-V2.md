# Neutral Topology-Landscape Bundle, Version 2

## 1. Status and relation to version 1

This document is the normative byte contract for
`topology-landscape-neutral-v2`. It imports the canonical TSV encoding,
canonical rooted-labelled topology grammar, topology SHA-256 identity,
taxon-set SHA-256 identity, safe-path rules, four linked identity model,
all-or-nothing loading, and no-replace publication rules from
`TOPOLOGY-LANDSCAPE-V1.md`, except where this document explicitly replaces a
rule.

Version 1 remains frozen. A conforming implementation dispatches on the exact
member set and manifest schema, accepts both versions, and never interprets a
v1 directory using v2 rules. The topology and taxon-set identities deliberately
retain their v1 domains so one canonical tree has the same identity in both
bundle versions.

Version 2 adds a complete grammar-ordinal/score ledger and scopes selected-view
completeness to the analysis policy. This permits an exact finite score band to
be exported without redefining the full grammar universe as that band.

## 2. Required members

Exactly these regular files occur:

```text
manifest.tsv
landscape_semantics.tsv
analysis_policy.tsv
search_run.tsv
artifact_provenance.tsv
files.tsv
completeness.tsv
score_census.tsv
trees.tsv
grammar_topology_provenance.tsv
topology_edges.tsv
move_witnesses.tsv
SHA256SUMS
```

The v1 directory, symlink, path, canonical byte, percent-encoding, null,
boolean, integer, SHA-256, and outer-ledger rules apply. Data rows sort by their
full encoded bytes except where a table below declares numeric ordinal order.

## 3. Stable topology identities

Canonical topology bytes use `rooted-labelled-length-grammar-v1`. Their digest
remains:

```text
SHA256("topology-landscape.rooted-labelled-topology.v1\n" || canonical_bytes)
```

The common taxon-label-set digest remains:

```text
SHA256("topology-landscape.taxon-label-set.v1\n" ||
       len(l1) || ":" || l1 || ... || len(ln) || ":" || ln)
```

Synthetic universal-ancestor nodes, node IDs, ancestral states, branch lengths,
child order, grammar coordinates, and score data remain outside topology
identity.

## 4. Linked identities

Every key/value file has header `key<TAB>value`, contains exactly the listed
keys, and sorts by encoded key.

### 4.1 Landscape semantics

`landscape_semantics.tsv` has exactly:

```text
alignment_sha256
alphabet
ambiguity_policy
canonical_topology_encoding
digest_algorithm
endpoint_filtration
grammar_construction
grammar_digest
grammar_semantic_digest
grammar_topology_count
input_content_sha256
move_family
move_relation
move_symmetry
parsimony_model
polytomy_policy
reference_sha256
root_policy
score_baseline
site_pattern_digest
taxon_labels_sha256
taxon_order
universe
ua_scoring
```

Its identity is:

```text
SHA256("topology-landscape.landscape-semantics.v2\n" || exact_file_bytes)
```

Fixed parser tokens are `digest_algorithm=sha256`,
`canonical_topology_encoding=rooted-labelled-length-grammar-v1`,
`endpoint_filtration=maximum_endpoint_score_delta`,
`taxon_order=unsigned_utf8_byte_order`, and
`root_policy=rooted_no_synthetic_ua`. `score_baseline` is a canonical
nonnegative integer no greater than `INT64_MAX`; `grammar_topology_count` is a
positive canonical nonnegative integer. All fields ending in `_sha256` and both grammar digest fields are
lowercase SHA-256 values.
Every field specified as a canonical nonnegative integer fits `UINT64_MAX`;
the narrower score bounds above and below still apply.

The identities have these non-overlapping meanings:

- `input_content_sha256` is the specified producer's domain-separated,
  length-framed composite of canonical taxon, alignment, site-pattern, and
  consumed-reference digests, never raw protobuf bytes;
- `alignment_sha256` binds normalized taxon-indexed observations and original
  site/position mapping;
- `site_pattern_digest` binds the exact ordered scorer input, including version,
  taxon indexing, pattern order, state masks, weights, reference-state counts,
  and invariant constant offsets;
- `reference_sha256` binds parsed decompressed reference sequence bytes;
- `grammar_digest` binds the normalized ordered enumerator input and therefore
  the ordinal map; and
- `grammar_semantic_digest` binds the versioned canonical root key and
  count/length-framed multiset of canonical clade/production keys, retaining
  duplicate multiplicity but ignoring artifact-local numeric order.

The exchange validator verifies hash grammar and linkage, not a producer's
domain-specific construction of those opaque content digests. The producing
raw certificate and scientific replay establish those constructions.

For the `larch-topology-score-band-raw-v1` producer, those constructions are
normative and no longer opaque. Every structured preimage starts with the
listed ASCII domain line and then records fields as
`key<TAB>decimal-byte-length:value<LF>`; integer values are canonical unsigned
decimal strings. Repeated records retain their stated order.

- `grammar_digest` hashes
  `larch.topology-score-band.ordered-grammar.v1\n`, followed by taxon count and
  registry labels in numeric ID order, root-clade ID, clades in vector order
  with taxa in stored order, productions in vector order with parent and child
  IDs in stored order, and every parent clade's production IDs after numeric
  sorting. Counts precede every repeated sequence.
- `grammar_semantic_digest` hashes
  `larch.topology-score-band.semantic-grammar.v1\n`, the construction-policy
  token, canonical root-clade sample key, then separately counted, byte-sorted
  canonical clade-key and production-key multisets. Duplicate keys are retained.
- `site_pattern_digest` hashes
  `larch.topology-score-band.site-patterns.v1\n`, taxon count and labels in
  scorer-column order, patterns in vector order, each singleton state as the
  decimal mask `1 << state`, weight, four A/C/G/T reference-state counts, and
  the three invariant/skipped-invariant score offsets. Pattern IDs and all
  sequence counts are explicit.
- `alignment_sha256` hashes
  `larch.topology-score-band.normalized-alignment.v1\n`, ordered taxon labels,
  total site count, then each zero-based site offset in order with its one-based
  original position, pattern ID, and taxon-column states. Raw-v1 export requires
  complete, bijective position-to-pattern coverage and does not admit skipped
  invariant positions.
- `reference_sha256` hashes only the exact uppercase A/C/G/T sequence obtained
  after decompressing and parsing the single reference record; FASTA headers,
  whitespace, and compressed bytes are excluded.
- `taxon_labels_sha256` retains its v1 construction:
  `topology-landscape.taxon-label-set.v1\n` followed by byte-sorted,
  length-prefixed labels with no separator. The raw producer additionally
  requires its taxon registry and inverse map to be in that same strict order.
- `input_content_sha256` hashes
  `topology-landscape.input-content.v2\n` followed by framed
  `taxon_labels_sha256`, `alignment_sha256`, `site_pattern_digest`, and
  `reference_sha256` records in that order.

The producer derives these values from the live grammar, scorer input, and
parsed reference, compares them with the replay expectations, hashes the exact
provider/reference/seed artifact bytes, reloads the provider protobuf, and
requires its canonical legacy DAG semantic digest to equal the live provider's
digest. `parsimony_model`, `ambiguity_policy`, and `ua_scoring` are likewise
derived fixed tokens rather than caller-authored labels.

### 4.2 Analysis policy

`analysis_policy.tsv` has the v1 key set:

```text
edge_weight
face_policy
hodge_metric
landscape_semantics_hash
null_family
numeric_policy
score_view
```

Its identity is:

```text
SHA256("topology-landscape.analysis-policy.v2\n" || exact_file_bytes)
```

`numeric_policy=exact_integer`. Version 2 score views initially have exactly:

```text
absolute_score_interval_inclusive_v1:<minimum>:<maximum>
```

Both endpoints are canonical nonnegative integers with `minimum <= maximum`.
Both are at most `INT64_MAX`.
The linked landscape hash must recompute.

### 4.3 Search run and artifact provenance

`search_run.tsv` and `artifact_provenance.tsv` retain the v1 key sets. Their
identities use:

```text
SHA256("topology-landscape.search-run.v2\n" || exact_file_bytes)
SHA256("topology-landscape.artifact-provenance.v2\n" || exact_file_bytes)
```

`artifact_provenance.input_artifact_sha256` is the raw provider artifact for a
grammar export. Other raw derivation inputs remain named by the derivation
certificate referenced by `derivation_ref`.

`artifact_provenance.output_data_sha256` is:

```text
SHA256("topology-landscape.output-data.v2\n" ||
       SORTED(filename || TAB || sha256 || LF))
```

over exactly `completeness.tsv`, `score_census.tsv`, `trees.tsv`,
`grammar_topology_provenance.tsv`, `topology_edges.tsv`, and
`move_witnesses.tsv`.

## 5. Manifest and file ledger

`manifest.tsv` retains the v1 key set. It requires
`schema_name=topology-landscape-neutral-v2` and `schema_version=2`. Linked IDs
must recompute with the v2 domains.

`files.tsv` has the v1 header and exactly ten rows. Fixed roles are:

```text
analysis_policy.tsv                 identity
artifact_provenance.tsv             identity
completeness.tsv                    claim
grammar_topology_provenance.tsv     table
landscape_semantics.tsv             identity
move_witnesses.tsv                  table
score_census.tsv                    claim
search_run.tsv                      identity
topology_edges.tsv                  table
trees.tsv                           table
```

Row counts exclude headers. `files_sha256` binds exact `files.tsv` bytes.
`SHA256SUMS` covers every member except itself.

The optional semantic-parity checksum used by TI-2 is:

```text
SHA256("topology-landscape.semantic-subset.v2\n" ||
       SORTED(filename || TAB || sha256 || LF))
```

over landscape semantics, analysis policy, search run, completeness, score
census, trees, grammar provenance, topology edges, and move witnesses. It is a
result certificate field, not a manifest identity.

## 6. Complete ordinal/score census

`score_census.tsv` has:

```text
grammar_ordinal<TAB>absolute_score
```

Rows are ordered by numeric ordinal, not encoded lexical order. Both fields are
required canonical nonnegative integers, and scores are at most `INT64_MAX`.
Ordinals occur exactly once and are
the contiguous range `0 .. grammar_topology_count-1`. The row count equals
`grammar_topology_count`. The table may be streamed into a compact typed score
vector; it need not be retained as generic decoded string maps.

The complete table determines the full score histogram. For a score view
`[a,b]`, the selected ordinal/score set is exactly the census rows satisfying
`a <= absolute_score <= b`.
The interval projection is required to contain at least one census row.

## 7. Selected trees

`trees.tsv` has exactly:

```text
topology_sha256<TAB>scope_ref<TAB>canonical_bytes<TAB>replay_encoding<TAB>replay_bytes<TAB>split_digest<TAB>leaf_count<TAB>root_arity_summary<TAB>absolute_score<TAB>score_delta<TAB>score_baseline_ref<TAB>incremental_fitch_score<TAB>incremental_fitch_status<TAB>selected_grammar_sankoff_score<TAB>selected_grammar_sankoff_status<TAB>selected_grammar_sankoff_oracle<TAB>fitch_score<TAB>fitch_status<TAB>reload_score<TAB>reload_status<TAB>sankoff_score<TAB>sankoff_status<TAB>sankoff_oracle<TAB>grammar_ordinal<TAB>local_production_witness_count<TAB>exact_compatible_source_history_count<TAB>original_occurrence_count
```

The v1 topology/replay/leaf/root/score/delta/uniqueness rules apply. Tree and
score-baseline scopes equal the landscape hash. `grammar_ordinal` is required
whenever selected ordinal completeness is complete.

All five score/status pairs use the v1 tokens `verified`, `not_checked`,
`mismatch`, and `not_applicable`. A verified score is present and equals
`absolute_score`; a mismatch is present and unequal; the other states require a
null score. Both Sankoff oracle-name fields are required and equal
`not_applicable` only with a not-applicable status. Unless both observations
are not applicable, their oracle names are distinct nonempty method tokens;
grammar-native and parsed-tree-native verification cannot be labeled as the
same method. The five observations mean:

1. incremental grammar generalized Fitch;
2. stateless selected-grammar Sankoff;
3. generalized Fitch after tree materialization;
4. generalized Fitch after serialization/reload; and
5. stateless Sankoff over parsed canonical tree bytes.

## 8. Grammar provenance

`grammar_topology_provenance.tsv` has exactly:

```text
grammar_digest<TAB>grammar_ordinal<TAB>topology_sha256<TAB>absolute_score<TAB>selected_production_keys<TAB>artifact_local_replay_ids<TAB>source_artifact_sha256<TAB>production_origin<TAB>legacy_selection_sha256<TAB>materialized_dag_semantic_sha256<TAB>materialized_dag_clades_sha256<TAB>materialized_dag_productions_sha256
```

Rows use numeric ordinal order. The v1 ordinal/topology/score bijection and
origin rules apply. The first digest equals landscape `grammar_digest` and
`source_artifact_sha256` equals provenance `input_artifact_sha256`. The four
audit digests are nullable for a producer to which they do not apply; when
present they are lowercase SHA-256 values and never topology identity.

## 9. Edges and move witnesses

The v1 table headers and semantic checks apply, except that populated rows use
`analysis_policy_hash` as `scope_ref`. Simple-edge and move-witness IDs use:

```text
SHA256("topology-landscape.simple-edge.v2\n" ||
       scope_ref || LF || topology_low || LF || topology_high || LF ||
       move_scope || LF)

SHA256("topology-landscape.move-witness.v2\n" ||
       scope_ref || LF || source_topology || LF || destination_topology || LF ||
       move_family || LF || move_version || LF || prune_support || LF ||
       regraft_support || LF || restrictions || LF)
```

Witness `score_reference` remains the landscape hash. Empty tables do not imply
that no adjacency exists.

## 10. Field-scoped completeness

`completeness.tsv` retains the v1 header and state grammar. Exactly these names
occur:

```text
canonical_topology
face
grammar_membership_index
grammar_ordinal
incremental_fitch_verification
materialized_fitch_verification
move_witness
reload_fitch_verification
score_census
selected_grammar_sankoff_verification
simple_edge
trace_event
tree_native_sankoff_verification
```

Scopes are:

- `score_census`: landscape semantics hash;
- `trace_event`: search run ID; and
- every other dimension: analysis policy hash.

`score_census=complete` requires denominator kind
`exact_grammar_ordinal_count`, denominator and numerator equal landscape
`grammar_topology_count`, a non-null method, and witness `score_census.tsv`.

Complete `canonical_topology` and `grammar_ordinal` claims require denominator
kind `exact_score_view_ordinal_count`, denominator and numerator equal the
census-derived selected count, non-null method, and respectively witnesses
`trees.tsv` and `grammar_topology_provenance.tsv`. Both selected tables must
equal the census interval projection as ordinal/score sets and must be in
bijection through topology SHA-256.

Each complete score-verification claim uses denominator kind
`exact_score_view_ordinal_count`, denominator equal the selected count,
numerator equal the number of `verified` statuses in its corresponding tree
column, a non-null method, and witness `trees.tsv`.

Complete simple-edge and move-witness claims retain `exact_row_count` and their
v1 payload witnesses, but use analysis scope. Face and grammar-membership index
cannot be complete until a later version supplies their payload certificates.
Unknown means unmeasured, not zero. Not-applicable retains the v1 null and zero
rules.

Because v2 defines no face, grammar-membership-index, or trace payload table,
their unknown/not-applicable claims have `observed_numerator=0`. A nonzero
observation requires a later schema with a bound payload certificate.

## 11. Mandatory validation order

A conforming validator performs:

1. exact member, type, safe-path, and canonical-byte checks;
2. outer checksum, file-ledger, row-count, and payload-digest checks;
3. exact headers, key sets, encodings, and ordering checks;
4. recomputation and linkage of v2 identities;
5. streaming score-census contiguity, count, and score checks;
6. topology parse/hash/replay/taxon/score/oracle checks;
7. grammar provenance and selected ordinal/score/topology bijection checks;
8. exact selected-set and per-score histogram equality with the census view;
9. edge and witness v2 scope/identity checks;
10. completeness state, scope, denominator, witness, local-count, and oracle
    coverage checks; and
11. v2 output-data digest verification.

Loading is all-or-nothing. Generic validation proves internal certificate
consistency. Caller-pinned landscape/census identities detect replacement by a
different internally valid result. Exhaustive producer replay and method review
establish scientific exactness.

Required coherent negative tests include a missing/substituted census ordinal,
census score mutation, selected score mutation, interval mutation without row
change, selected interval bucket/set mismatch, false view denominator, wrong view scope,
missing oracle coverage, grammar/audit digest mutation, v1/v2 domain confusion,
and every applicable v1 integrity/topology/edge/witness mutation.

## 12. Consumer and producer obligations

The producer validates staging before no-replace publication. Cross-repository
publication requires both independent neutral validators to accept the same
staging bytes and to re-open the published destination. The renderer validates
the raw handoff independently and mechanically maps every required field; it
does not score, infer completeness, or discard audit evidence.

Consumers preserve completeness without upgrade, distinguish the full grammar
from the selected view, retain grammar/audit identities separately from topology
identity, and never interpret unknown empty move tables as isolation.

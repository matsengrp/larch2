# Topology Landscape Exchange Contract, version 1

Status: frozen for TI-0 implementation. This is the byte-level contract shared
by `larch2` producers and `tric-explore` consumers. A conforming validator
proves canonical syntax, integrity, linked identity, and internal consistency;
it does not prove that a biological search or adjacency enumeration was
exhaustive.

## 1. Scope and semantic firewall

The bundle describes a finite scored set of canonical rooted, leaf-labelled
topologies and, optionally, two distinct edge carriers:

- `K_simple` contains at most one unordered edge for each adjacent topology
  pair. Connectivity, persistent H0, barriers, and graph spectra use this
  carrier.
- `K_move` contains directed move witnesses. Several witnesses may project to
  one `K_simple` edge through `pi_edge_id`. Witness multiplicity never changes
  the simple graph.

Grammar ordinals, canonical topologies, production/history witnesses, and
search visits are separate entity identities. The manifest also links four
bundle-level identities: landscape semantics, analysis policy, search run, and
artifact provenance. No count may be transferred between these layers without
an explicit table row or linkage.

Version 1 is uncompressed and directory based. It deliberately does not define
faces, cubes, search-event payloads, Newick parsing, or a general rSPR oracle.
Later schema versions may add them without changing accepted version-1 bytes.

## 2. Required bundle members

Exactly these regular files occur in a version-1 directory:

```text
manifest.tsv
landscape_semantics.tsv
analysis_policy.tsv
search_run.tsv
artifact_provenance.tsv
files.tsv
completeness.tsv
trees.tsv
grammar_topology_provenance.tsv
topology_edges.tsv
move_witnesses.tsv
SHA256SUMS
```

Subdirectories, symlinks, absolute paths, `.` or `..` path components, and
unlisted extra files are rejected. All names in tables are the fixed basenames
above. `SHA256SUMS` covers every member except itself.

## 3. Canonical TSV byte rules

Every file uses bytes, not locale-dependent text behavior.

1. Lines end with LF (`0a`), never CRLF, and every file has exactly one final
   LF. Empty files are forbidden.
2. Columns are separated by one TAB. Headers and column counts are exact.
   Blank lines, comments, BOMs, unknown columns, and trailing TABs are rejected.
3. Rows after a header are in increasing unsigned-byte lexicographic order of
   their full encoded line and are unique. Two-column key/value files are
   sorted by their encoded key. The only exception is
   `grammar_topology_provenance.tsv`, whose primary order is numeric
   `grammar_ordinal` and whose tie-breaker is the full encoded line.
4. Values are canonical percent encodings. Bytes `21` through `7e` may occur
   raw except `%` (`25`). Every other byte, and `%`, TAB, CR, and LF, is encoded
   as `%HH` with uppercase hexadecimal. Escaping a byte that is permitted raw,
   lowercase hex escapes, malformed UTF-8 in fields declared textual, and
   decode-then-reencode inequality are rejected.
5. The single raw token `-` means null. A literal one-byte hyphen is `%2D`;
   this whole-cell token is the sole exception to the rule against escaping a
   byte that could otherwise occur raw. Hyphens in longer raw values remain
   unescaped. Required fields cannot be null.
6. Booleans are `true` or `false`. Unsigned integers are decimal with no sign
   or leading zero except `0`. Signed integers use an optional leading `-` and
   otherwise the same rule. SHA-256 values are exactly 64 lowercase hex digits.
7. Hash inputs are the exact decoded or encoded byte strings specified below;
   no platform newline, Unicode, path, or floating-point normalization occurs.

Version 1 uses integer parsimony scores only. Rational or floating weights
require a later schema.

## 4. Canonical rooted labelled topology identity

A leaf with raw UTF-8 label bytes `x` is encoded as:

```text
L<decimal byte count>:<x>
```

An internal node with child byte strings `c1 ... cn` is encoded as:

```text
I<n>[<len(c1)>:<c1>...<len(cn)>:<cn>]
```

Constraints:

- `n >= 2`; unary nodes are invalid.
- Child encodings are sorted in increasing unsigned-byte lexicographic order.
- Leaf labels are unique within a tree and are compared as exact UTF-8 bytes;
  version 1 performs no Unicode normalization.
- Internal labels, branch lengths, node IDs, scores, grammar IDs, and synthetic
  universal-ancestor nodes are excluded.
- The root is the outermost internal node. A leaf-only topology is not allowed
  in version 1.

Each topology digest is:

```text
SHA256("topology-landscape.rooted-labelled-topology.v1\n" || canonical_bytes)
```

`canonical_bytes` and `replay_bytes` are percent-encoded in `trees.tsv`.
Version-1 `replay_encoding` is `canonical-bytes-v1`, and replay bytes must be
byte-identical to canonical bytes. This makes replay independent of Newick
parser conventions. `split_digest` is a separately domain-defined witness and
may be null; it is never the topology identity.

Every topology in one bundle has the same leaf-label set. Let `l1 ... ln` be
the distinct raw UTF-8 label byte strings in unsigned-byte lexicographic order.
The declared taxon-set digest is:

```text
SHA256("topology-landscape.taxon-label-set.v1\n" ||
       len(l1) || ":" || l1 || ... || len(ln) || ":" || ln)
```

Lengths are canonical decimal byte counts. This digest must equal
`taxon_labels_sha256`, and every parsed tree must reproduce the same set.

## 5. Linked bundle identities

The following four files have header `key<TAB>value`, contain every listed key
exactly once, contain no other key, and are sorted by key.

### 5.1 `landscape_semantics.tsv`

```text
alignment_sha256
alphabet
ambiguity_policy
canonical_topology_encoding
digest_algorithm
endpoint_filtration
grammar_construction
grammar_digest
input_content_sha256
move_family
move_relation
move_symmetry
parsimony_model
polytomy_policy
reference_sha256
root_policy
score_baseline
taxon_labels_sha256
taxon_order
universe
ua_scoring
```

Its ID is:

```text
SHA256("topology-landscape.landscape-semantics.v1\n" || exact_file_bytes)
```

These values determine what trees, scores, and moves mean. A change creates a
new landscape identity. A producer based on the present search-oriented larch
SPR implementation must disclose its restrictions; it must not claim exact
adjacency. TI-1 freezes two exact-small calibration relations:

- `rooted_common_prune_reduction_binary_rspr_v1`; and
- `rooted_common_prune_reduction_hard_rspr_v1`.

For both, prune any proper rooted subtree and suppress its parent exactly when
the parent becomes unary, replacing a unary biological root by its survivor.
Binary regraft subdivides any remainder edge, including the virtual stem above
the biological root. The hard relation additionally permits attaching the
subtree directly to any biological internal vertex. This is the symmetric
common-prune/remainder relation: the hard edge and vertex attachment modes are
constructive inverses, not a post-hoc symmetric closure. Exact artifacts use
`move_symmetry=intrinsic_bidirectional` and retain every directed witness.

The virtual stem is an operational move location, not a canonical topology
edge and not part of `root_policy`; exact-small artifacts still use
`root_policy=rooted_no_synthetic_ua`. Their synthetic reference-aware Sankoff
calibration uses
`ua_scoring=fixed_reference_root_boundary_unit_cost`. That score boundary is
independent of the virtual move stem and must not be inferred for another
landscape.

The v1 parser implements the fixed tokens `digest_algorithm=sha256`,
`canonical_topology_encoding=rooted-labelled-length-grammar-v1`,
`endpoint_filtration=maximum_endpoint_score_delta`,
`taxon_order=unsigned_utf8_byte_order`, and
`root_policy=rooted_no_synthetic_ua`. Other values require a later schema.

### 5.2 `analysis_policy.tsv`

```text
edge_weight
face_policy
hodge_metric
landscape_semantics_hash
null_family
numeric_policy
score_view
```

Its ID is:

```text
SHA256("topology-landscape.analysis-policy.v1\n" || exact_file_bytes)
```

The linked landscape hash must equal the manifest landscape hash. Analysis
choices never silently alter landscape semantics.
Version 1 requires `numeric_policy=exact_integer` and
`score_view=absolute_and_delta`.

### 5.3 `search_run.tsv`

```text
budget
initial_state
landscape_semantics_hash
mode
seed
trace_schema
```

Its ID is:

```text
SHA256("topology-landscape.search-run.v1\n" || exact_file_bytes)
```

Use explicit `not_applicable` values for a bundle without a search trace. Its
linked landscape hash must still match.

### 5.4 `artifact_provenance.tsv`

```text
command
derivation_ref
input_artifact_sha256
output_data_sha256
producer_commit
producer_dirty
producer_repository
toolchain
worker_count
```

`output_data_sha256` breaks hash recursion. It is:

```text
SHA256("topology-landscape.output-data.v1\n" ||
       SORTED(filename || TAB || sha256 || LF))
```

over these five data tables only: `completeness.tsv`, `trees.tsv`,
`grammar_topology_provenance.tsv`, `topology_edges.tsv`, and
`move_witnesses.tsv`. The provenance ID is:

```text
SHA256("topology-landscape.artifact-provenance.v1\n" || exact_file_bytes)
```

Dirty state, unavailable source artifacts, and synthetic fixtures must be
reported literally, never replaced by an apparently stronger provenance.

## 6. Manifest and file ledger

`manifest.tsv` has header `key<TAB>value` and exactly these keys:

```text
analysis_policy_hash
artifact_provenance_id
experiment_id
files_row_count
files_sha256
landscape_semantics_hash
schema_name
schema_version
search_run_id
```

`schema_name` is `topology-landscape-neutral-v1`; `schema_version` is `1`.
`files_sha256` is SHA-256 of exact `files.tsv` bytes and `files_row_count` is
its number of data rows. The other IDs equal the recomputed values above.
`experiment_id` is a human-chosen opaque scope label and carries no evidence.

`files.tsv` has:

```text
filename<TAB>role<TAB>row_count<TAB>sha256
```

It lists, once each, the four identity files and five data tables. Roles are
`identity`, `claim`, or `table`. `row_count` excludes the header. The digest is
SHA-256 of the exact file. Manifest, `files.tsv`, and `SHA256SUMS` are excluded
to avoid recursion. Rows and declared roles are fixed by this specification.

`SHA256SUMS` has no header. Each line is:

```text
sha256<TAB>filename
```

Rows are sorted by filename. It includes every regular bundle member except
itself. A validator must recompute it before trusting any parsed claim.

## 7. Field-scoped completeness

`completeness.tsv` has:

```text
name<TAB>state<TAB>scope_ref<TAB>denominator_kind<TAB>denominator_value<TAB>observed_numerator<TAB>method_id<TAB>witness_ref<TAB>reason
```

Exactly these names occur once:

```text
canonical_topology
face
grammar_membership_index
grammar_ordinal
move_witness
simple_edge
trace_event
```

States are `complete`, `partial`, `unknown`, and `not_applicable`.

- `complete` requires a non-null exact denominator kind/value, an observed
  numerator equal to that denominator, and non-null method and witness.
- `partial` preserves incompleteness even if the current numerator happens to
  equal a stated denominator. It requires a reason.
- `unknown` is not zero and requires a reason. Its denominator may be null.
- `not_applicable` requires null denominator and method/witness, numerator `0`,
  and a reason.

`scope_ref` names the exact semantic or artifact scope. Counts must agree with
the referenced local table where version 1 defines a direct count:
canonical topologies with `trees.tsv`, grammar ordinals with grammar rows,
simple edges with `topology_edges.tsv`, and move witnesses with
`move_witnesses.tsv`. A validator may reject a self-contradictory `complete`
claim. It may not establish scientific completeness merely by counting rows.

Version 1 fixes claim scopes and direct-count certificates. Every claim except
`trace_event` uses the landscape-semantics hash; `trace_event` uses the search
run ID. A `complete` claim for `canonical_topology`, `grammar_ordinal`,
`simple_edge`, or `move_witness` uses denominator kind `exact_row_count` and
respectively witnesses `trees.tsv`, `grammar_topology_provenance.tsv`,
`topology_edges.tsv`, or `move_witnesses.tsv`. Other dimensions cannot be
`complete` in version 1 because no corresponding payload table exists.

## 8. Data tables

### 8.1 `trees.tsv`

```text
topology_sha256<TAB>scope_ref<TAB>canonical_bytes<TAB>replay_encoding<TAB>replay_bytes<TAB>split_digest<TAB>leaf_count<TAB>root_arity_summary<TAB>absolute_score<TAB>score_delta<TAB>score_baseline_ref<TAB>fitch_score<TAB>fitch_status<TAB>reload_score<TAB>reload_status<TAB>sankoff_score<TAB>sankoff_status<TAB>sankoff_oracle<TAB>grammar_ordinal<TAB>local_production_witness_count<TAB>exact_compatible_source_history_count<TAB>original_occurrence_count
```

Rows sort by `topology_sha256`. Canonical bytes and hashes are unique.
`scope_ref` and `score_baseline_ref` equal the landscape hash. Score delta is
exactly `absolute_score - score_baseline`. Oracle status tokens are
`verified`, `not_checked`, `mismatch`, or `not_applicable`; null scores must
agree with status. `verified` requires equality with `absolute_score`;
`mismatch` requires a present unequal score; the other two states require a
null score. The three multiplicity counts remain independent nullable
fields. `grammar_ordinal` is nullable only when grammar membership is unknown
or not applicable.

### 8.2 `grammar_topology_provenance.tsv`

```text
grammar_digest<TAB>grammar_ordinal<TAB>topology_sha256<TAB>absolute_score<TAB>selected_production_keys<TAB>artifact_local_replay_ids<TAB>source_artifact_sha256<TAB>production_origin
```

Rows sort by ordinal numerically, then by their full encoded line. Each ordinal
occurs once and maps to one existing topology; each topology has at most one
ordinal in version 1. Grammar digest and scores agree with landscape/tree
records. Pipe-separated production keys are an opaque witness and are never a
canonical topology digest. `production_origin` is explicit (`known`,
`unknown`, or `not_applicable`).

### 8.3 `topology_edges.tsv`

```text
edge_id<TAB>scope_ref<TAB>topology_low<TAB>topology_high<TAB>move_scope<TAB>endpoint_low_score<TAB>endpoint_high_score<TAB>endpoint_low_delta<TAB>endpoint_high_delta<TAB>filtration
```

Endpoints exist in `trees.tsv`, are distinct, and satisfy
`topology_low < topology_high`. Scores/deltas agree with tree rows and
`filtration = max(endpoint_low_delta, endpoint_high_delta)`. There is one row
per unordered pair in a scope. The edge ID is:

```text
SHA256("topology-landscape.simple-edge.v1\n" ||
       scope_ref || LF || topology_low || LF || topology_high || LF ||
       move_scope || LF)
```

### 8.4 `move_witnesses.tsv`

```text
witness_id<TAB>scope_ref<TAB>source_topology<TAB>destination_topology<TAB>pi_edge_id<TAB>move_family<TAB>move_version<TAB>prune_support<TAB>regraft_support<TAB>restrictions<TAB>topology_valid<TAB>leaf_set_valid<TAB>root_valid<TAB>reverse_status<TAB>no_self_loop<TAB>band_membership<TAB>recomputed_score<TAB>score_reference<TAB>grammar_membership<TAB>grammar_membership_witness
```

Source and destination are distinct existing trees. `pi_edge_id` exists and
has exactly those unordered endpoints. Validation booleans are literal producer
claims. Witness rows never create simple edges implicitly. The witness ID is:

```text
SHA256("topology-landscape.move-witness.v1\n" ||
       scope_ref || LF || source_topology || LF || destination_topology || LF ||
       move_family || LF || move_version || LF || prune_support || LF ||
       regraft_support || LF || restrictions || LF)
```

Parallel and reverse witnesses are distinct. `reverse_status` is one of
`certified`, `not_checked`, `not_applicable`, or `failed`.
The five validation flags are literal booleans. `recomputed_score` equals the
destination topology's absolute score and `score_reference` is the landscape
hash. `grammar_membership` is `verified`, `mismatch`, `not_checked`, or
`not_applicable`; the first two require a membership witness and the latter two
require it to be null.

## 9. Mandatory validator behavior

A conforming version-1 validator performs, in order:

1. directory/type/name/path and canonical-byte checks;
2. `SHA256SUMS`, `files.tsv` hash, payload hashes, and row counts;
3. exact headers, field encodings, value grammars, and row order;
4. recomputation and linkage of all four bundle identities;
5. canonical topology parse, child-order check, leaf uniqueness/count, replay
   equality, and topology digest recomputation;
6. grammar ordinal uniqueness and tree/provenance score consistency;
7. simple-edge endpoint, ordering, ID, score, filtration, and uniqueness checks;
8. witness ID, endpoint, and quotient projection checks;
9. completeness state grammar, local count dependencies, and absence semantics.

Diagnostics identify a stable code, file, line when applicable, and detail.
Loading is all-or-nothing: no partially validated bundle is returned.

Required negative tests include corrupt outer/table hashes, unsupported schema,
noncanonical percent escapes/newlines/order, unsafe paths and symlinks, duplicate
topology bytes/hash, digest mismatch, conflicting grammar ordinal, score/delta
mismatch, missing edge endpoint, self-loop, reversed/duplicate simple edge, bad
edge or witness ID, witness projection mismatch, identity cross-link mismatch,
and internally false completeness. Some fixtures must update outer hashes after
a semantic mutation so tests reach semantic validation rather than stopping at
the integrity layer.

## 10. Producer and consumer obligations

The producer writes to a uniquely named staging directory and validates the
staged bundle. It must never replace an existing destination. Where the host
offers an atomic no-replace directory rename, the producer uses it. On a
filesystem that rejects that primitive, it may atomically reserve the new
destination with an exclusive directory creation and copy the already
validated members into it; a reader observing that brief incomplete state must
reject it through the normal member-set/checksum rules. The producer validates
the fallback destination before reporting success and removes it on failure.
A canonical rewrite of a valid bundle must be byte-identical. Real-data export
must retain the producing commit, dirty state, full command, toolchain, inputs,
and source artifact hashes.

The consumer treats all completeness states and oracle statuses exactly as
declared. Import must never upgrade `partial` or `unknown`, equate an absent
table with zero, use production-selection hashes as topology IDs, collapse
move witnesses before preserving their witness table, or call a
grammar-relative universe globally exhaustive.

Cross-repository TI-0 acceptance requires byte-identical copies of one golden
bundle, a frozen aggregate ledger, independent validation in each repository,
and rejection of coherent semantic mutations in each implementation. This
synthetic fixture tests the contract only; no WRIC or phylogenetic conclusion
follows from it.

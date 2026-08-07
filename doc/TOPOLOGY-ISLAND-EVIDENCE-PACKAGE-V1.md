# Topology-Island Evidence Package, Version 1

## 1. Purpose

This document is the normative packaging contract for the durable TI-3 WRIC
adjacency and topology-island handoff. It binds immutable TI-2 sources,
adjacency artifacts, independent pair checks, enriched neutral-v2 graph
bundles, exact analysis artifacts, implementation sources, commands, resource
records, and independent reviews in one content-addressed archive.

Packaging does not strengthen scientific completeness. Every archived object
retains its own scope, authority, and unknown dimensions.

## 2. Archive and content address

The archive is a deterministic GNU tar stream compressed by single-threaded
xz level 6. Tar entries are byte-sorted, have modification time zero, numeric
owner/group zero, empty owner/group names, and contain only regular files or
directories. Absolute paths, `..`, duplicate normalized paths, links, devices,
sockets, FIFOs, sparse files, and trailing unindexed regular files are
forbidden.

The archive filename is:

```text
<sha256-of-exact-archive-bytes>.tar.xz
```

Publication is staging plus no-replace. An existing archive or locator is
never overwritten, even when bytes appear equivalent.

## 3. Required layout

The archive root contains exactly these control files and allowed directory
prefixes:

```text
PACKAGE-CERTIFICATE.tsv
PACKAGE-FILES.tsv
packager-source.sh
contracts/
source/
adjacency/
checker/
neutral/
analysis/
review/
```

Required content classes are:

- `contracts/`: exact edge-index, analysis, and package normative documents;
- `source/`: TI-2 Gate-D/Gate-E certificates, both durable source archives,
  the content-addressed provider and provider certificate;
- `adjacency/`: final W1/W8 delta-0, delta-1, and delta-2 edge-index artifacts,
  semantic parity records, commands, stdout/stderr, source-state ledgers, and
  runtime/RSS records;
- `checker/`: direct all-pair checker certificates and canonical results for
  all three views;
- `neutral/`: enriched delta-1 and delta-2 neutral-v2 graph bundles, render
  certificates, and both-repository validator reports;
- `analysis/`: validated delta-0, delta-1, and final delta-2 analysis artifacts;
  and
- `review/`: compact gate certificates and independent review verdict ledgers.

Every regular file, including the three root control files except
`PACKAGE-FILES.tsv` itself as described below, is covered by the package
certificate or inventory. A package may add files within an allowed prefix
only when they are indexed and assigned a registered role. A new top-level
prefix requires a new schema version.

## 4. Package inventory

`PACKAGE-FILES.tsv` has:

```text
relative_path<TAB>role<TAB>bytes<TAB>sha256
```

It lists every payload regular file except `PACKAGE-CERTIFICATE.tsv`,
`PACKAGE-FILES.tsv`, and `packager-source.sh`, in unsigned byte-sorted path
order. Paths use `/`, are relative, normalized, safe, and unique. `bytes` is a
canonical unsigned integer. Roles are exactly:

```text
analysis
certificate
checker
contract
graph_bundle
log
provider
review
source_archive
source_ledger
```

All recorded sizes and hashes are checked after extraction. The package-files
hash is the SHA-256 of exact `PACKAGE-FILES.tsv` bytes.

## 5. Embedded package certificate

`PACKAGE-CERTIFICATE.tsv` is a sorted key/value file with exactly:

```text
adjacency_delta0_semantic_subset_sha256
adjacency_delta1_semantic_subset_sha256
adjacency_delta2_semantic_subset_sha256
analysis_delta2_data_sha256
archive_format
delta0_edge_count
delta1_edge_count
delta2_edge_count
gate_d_archive_sha256
gate_d_certificate_sha256
gate_e_archive_sha256
gate_e_certificate_sha256
landscape_semantics_hash
neutral_delta1_output_data_sha256
neutral_delta2_output_data_sha256
package_files_sha256
packager_source_sha256
payload_member_count
payload_uncompressed_bytes
provider_sha256
q1_optimum_partition
q2_barrier_summary
schema_name
schema_version
tree_count_delta0
tree_count_delta1
tree_count_delta2
```

Fixed values are:

```text
schema_name=topology-island-evidence-package-v1
schema_version=1
archive_format=gnu_tar_xz6_single_thread_mtime0_owner0_group0_v1
tree_count_delta0=12
tree_count_delta1=176
tree_count_delta2=1076
```

The initial source identities are Gate-D archive `00cbb231...`, Gate-D
certificate `ed762697...`, Gate-E archive `b60dab26...`, Gate-E certificate
`292e6fd1...`, provider `ae9c0c1b...`, and landscape semantics `7dfa1a6d...`;
the exact full hashes are checked against the corresponding archived bytes.

Payload member count and uncompressed bytes equal the inventory. The
packager-source hash equals exact `packager-source.sh` bytes. Every adjacency,
neutral, analysis, outcome, tree-count, and edge-count field reconciles with
the archived certificates and tables.

The embedded certificate deliberately does not contain the archive hash,
which would be circular.

## 6. External locator certificate

Beside the archive, a same-prefix locator file is published:

```text
<archive-sha256>.certificate.tsv
```

It is a sorted key/value file with exactly:

```text
archive_bytes
archive_filename
archive_sha256
package_certificate_sha256
package_files_sha256
packager_source_sha256
schema_name
```

`schema_name=topology-island-evidence-package-locator-v1`. The archive hash
equals both its filename prefix and exact bytes. All three internal hashes are
obtained by extracting the named control members.

## 7. Required extraction validation

Before publication and again from the final content-addressed archive:

1. verify archive hash/name, exact xz stream, safe unique tar paths, metadata,
   and allowed top-level layout;
2. extract into a fresh directory without replacing an existing path;
3. verify package inventory row count, aggregate bytes, every member size/hash,
   and absence of unindexed payloads;
4. verify embedded and locator certificates and archived packager source;
5. revalidate both TI-2 source archives/certificates and the provider;
6. revalidate all six W1/W8 adjacency artifacts and three parity/checker
   records under `TOPOLOGY-RSPR-EDGE-INDEX-V1`;
7. validate enriched neutral bundles with both repository validators;
8. validate all analysis artifacts under
   `TOPOLOGY-ISLAND-ANALYSIS-V1`;
9. reconcile nested delta-0/delta-1 endpoint/proof projections with delta 2;
10. recompute final Q1/Q2 fields, paths, and every cross-link; and
11. compare archived contract/source hashes with the reviewed source state.

No build-directory path or transient timestamp is evidence. Runtime and RSS
records are operational metadata with their measurement methods; worker parity
is determinism evidence, not biological replication.

## 8. Failure and supersession

Path unsafety, hash/size drift, missing source bytes, unindexed files, invalid
inner artifacts, changed semantic rows, broken cross-links, or failed
post-extraction validation blocks durable publication. The packager never
drops a failing member, rewrites an expected hash, weakens a completeness
state, or replaces an existing archive.

A later correction publishes a new content address and a supersession record;
the old archive remains immutable. Resource-limited or right-censored
scientific outcomes are valid package contents when labeled by their inner
contracts.

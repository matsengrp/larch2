# WRIC chart parallelization: results and evidence ledger

## Status

This ledger records evidence for
`doc/WRIC-CHART-PARALLELIZATION-PLAN.md`. It is intentionally incomplete:
Phase 0 is now completely captured, audited, and sealed. The Phase-7/8/9
supplemental manifests have also been captured, audited, and sealed, while
their performance, RSS, cross-phase, and final acceptance decisions remain
open. The sealed base has 13 captures: 267 ordinary canonical rows across 60
repeat stages, 27 approved timeouts, and two real-fixture W1/W8
expected-infeasible refusal observations.
The pending and final audits and strict smokes passed. The workload-manifest
SHA-256 is
`32ae82a93cb72a28afaa510391eac83f80f13638d46e1513881d2b484c9dc3ae`,
and the final artifact-ledger SHA-256 is
`a33436d78ec6c840427b343615c6b9a7b555a87f989000865873e9c5adeb06bb`.
No Phase-1+ timing, scaling, memory, parity, policy, or default result is
accepted merely because this prerequisite is complete.

The current immutable evidence product is Q,
`a9db72e60f153a95362db544107373817a58a258` (`Stabilize grammar worker
overlap test`). Historical product P
`3b0e442e213a7913c6e007c76715d7d856bce0bd` remains the source of the retained
six-repetition diagnostics, but a clean full-suite run exposed its
timing-sensitive `chart_spr_test` high-water assertion. Q replaces that test
timing dependency with a deterministic latch and has the sealed clean normal
result: 176 unique tests, 174 pass, zero fail, and exactly the two established
skips. Q's full serial ASAN suite also passes with the same inventory and a
clean ASAN/LSan scan. Q's exact targeted TSan matrix also passes 55/55 with a
clean race/lifetime/thread scan. Timed recaptures and final acceptance remain
open.

On 2026-07-14 the user authorized a deadline-driven scheduling exception: this
state may be committed as the Phase-0 measurement checkpoint and later-phase
implementation may begin before the quiet-host timing campaign. The
checkpoint is `7ca527b8906d018124756182274335cbff936d72`, with exact subject
`Baseline measure` and sole parent
`408434ecfd096af484ecbbd3deeb67511151cd76`; it remains an ancestor of the
current implementation and documentation checkpoints. The authorization did
not itself accept a timing baseline; the subsequently completed capture,
finalization, audit, and seal do. The frozen executable/workload identities
remain binding, and every timing-dependent later-phase or Phase-10 gate must
still be run and decided independently against that sealed base.

Raw artifacts belong under `build/wric-chart-parallelization/` and are not
committed. Phase-0 artifacts use the non-overwriting directory
`build/wric-chart-parallelization/baseline-408434e/`.

| Phase | Evidence status | Result |
|---|---|---|
| 0. Repair and freeze measurement | complete (audited and sealed) | 13 capture statuses close 267 ordinary canonical rows across 60 repeat stages, 27 approved timeouts, and two real-fixture exact-refusal observations; pending/final audits and strict smokes pass; workload and final-ledger SHA-256 values are `32ae82a93cb72a28afaa510391eac83f80f13638d46e1513881d2b484c9dc3ae` and `a33436d78ec6c840427b343615c6b9a7b555a87f989000865873e9c5adeb06bb` |
| 1. Compile an immutable chart plan | implementation complete; optimized-candidate acceptance pending | checkpoint `208ce23` passes its functional gates. Q/C4's complete evidence and gate-by-gate diagnosis passed Phase-1 canonical and serial no-regression comparisons; the changed candidate must be frozen and recaptured |
| 2. Remove allocations and duplicate work | implementation complete; optimized-candidate acceptance pending | checkpoint `0c4623b` passes its allocation/correctness gates, and Q/C4 passed the Phase-2 timing/RSS diagnosis. Candidate-bound correctness, timing, RSS, and sanitizer closure remain pending |
| 3. Add one persistent adaptive scheduler | implementation complete; optimized-candidate acceptance pending | checkpoint `7d294d6` passes the scheduler gates, and Q/C4 passed the small-case diagnosis. The immutable candidate's clean suite, sanitizers, and recapture remain pending |
| 4. Parallelize patterns and local scoring | implementation complete; optimized-candidate acceptance pending | checkpoint `cbf92b6` passes the functional gates; Q/C4's deep Phase-4 validator and contention investigation passed scaling, Phase-3 comparison, RSS, swap, and high-water gates. Candidate-bound recapture remains pending |
| 5. Parallelize a single exact B&B | implementation checkpoint complete; optimized-candidate acceptance pending | `bb29300` plus `3a10e9c` pass historical functional gates, and Q/C4 passed the exact-span, timeout, and RSS diagnosis. Fresh final-product evidence remains pending |
| 6. Parallelize exact top-K candidates | implementation complete; optimized-candidate acceptance pending | checkpoint `870c298` passes its in-tree matrix; Q/C4 covered current-product Top-K 1/4/16 semantics, repeatability, scaling, admission, and RSS without a diagnosed failure. The changed candidate must repeat the authoritative campaign and evaluator |
| 7. Make lazy charts scalable and adaptive | optimized candidate implemented; authoritative acceptance pending | Q `a9db72e60f153a95362db544107373817a58a258` has sealed normal/ASAN/TSan closure and a complete C4 campaign, but C7 measured forced-lazy W8/W1 `0.682706744372248`, above `2/3`. The retained fingerprint-fusion diagnostic is `0.630021916938`; immutable-candidate tests, sanitizers, recapture, and evaluator remain pending. Manifest hashes remain `82250bb26d5394d3c2c616e4ccbf606e80eca074e560fd8e3361274655e56718` and `7b934fa0ad8279893d03321724935dccf8df99fa840899652dccdc63c8c37bff` |
| 8. Parallelize and pipeline candidate generation | sealed retry1 generation gate passes; candidate end-to-end acceptance pending | attempt 0 remains a sealed failure at `1.287784679`; retry1 on R `07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c` passes at `0.403067171046`. C4 `be5e4a025c357d22fb1a6b41f544e35f2d8a5891` completed and audited all 20 Q components/430 rows, and the diagnosis found no Phase-8 failure. Candidate same-workload recapture and strict evaluation remain pending |
| 9. Parallelize accepted-state cache updates | implementation checkpoint complete; optimized-candidate acceptance pending | functional, manifest, and counter gates pass through `4ef6126`; Q/C4's deep Phase-9 validation and timing/RSS diagnosis passed before C7 failed later performance gates. Candidate-bound validation, sanitizers, timing, and RSS remain pending |
| 10. Integrate, tune defaults, and prove parity | implementation in progress; authoritative candidate closure pending | Q has sealed normal/ASAN/TSan closure, and C4 completed/audited 20 components and 430 rows. C7's sole pre-default invocation failed closed on four diagnosed gaps: forced lazy, physical grammar scaling, primary grammar scaling, and sampled-tree fixed-topology stress. Dirty candidate diagnostics pass all four limits; immutable-candidate closure, a fresh campaign/evaluator, conditional default promotion, and post-default closure remain pending |

## Phase 0 — immutable provenance

### Revision identity

| Field | Frozen value |
|---|---|
| Branch | `wric` |
| Research-tip commit | `408434ecfd096af484ecbbd3deeb67511151cd76` |
| Research-tip subject | `Add chart row fluidity.` |
| Research-tip commit date | `2026-07-07T23:35:38+03:00` |
| Pre-WRIC merge base | `fee366e71439d7b8c8eda588cc72cde8534f102f` |
| Merge-base subject | `Bcr vulkan sampling (#41)` |
| Baseline capture date | `2026-07-13` |
| Baseline directory | `build/wric-chart-parallelization/baseline-408434e/` |

The native executable was copied before Phase-0 reporting changes rebuilt the
working tree. The initial baseline audit found the research-tip commit checked
out and only the new implementation-plan document untracked. Subsequent dirty
status is implementation state and must be recorded independently in every
benchmark artifact.

### Frozen native executable

| Field | Frozen value |
|---|---|
| Source at capture | `build/bin/larch2` |
| Frozen path | `build/wric-chart-parallelization/baseline-408434e/bin/larch2` |
| SHA-256 | `ee160aa4fdecce5660f1bf0737289fc32fd33b1965e660f0ec92072805a4de9b` |
| Size | 54,958,240 bytes |
| Version output | `larch2 2.2.0 (408434e)` |
| Format | x86-64 ELF, dynamically linked musl, debug info present, not stripped |
| Source/frozen byte comparison | `cmp` exit 0 |

The frozen path is the only native executable permitted in final parity runs.
It must be verified by SHA-256 before each use.

### Phase-0 semantic-oracle freeze audit

Reporting and canonical instrumentation was completed and tested before any
product performance optimization. The resulting executable was then copied
byte-for-byte and made read-only:

| Field | Frozen value |
|---|---|
| Source at capture | `build/bin/dagutil` |
| Frozen path | `build/wric-chart-parallelization/baseline-408434e/bin/dagutil` |
| SHA-256 | `1e71334d5e634af9c07a1dd2e0c571ae3b71c9b6eacbdc4376d4ed95cecb6b12` |
| Size | 84,454,896 bytes |
| Version output | `dagutil 2.2.0 (408434e)` |
| Source/frozen byte comparison | `cmp` exit 0 |
| Frozen mode | `-r-xr-xr-x` |

The focused Phase-0 CTest set passed 5/5: process accounting, chart-SPR
observability, canonical CLI output, the SHA/canonical-report unit test, and
the synthetic harness regression. An independent two-run explicit-one-worker
audit used `test/wric_binary_four.{fa,nwk,ref}` with two candidates and two
exact verifications. Both compact search reports, both full NDJSON sidecars,
and both external canonical DAG reports were byte-identical. The common search
semantic SHA-256 was
`53021d1e888fa530a6750ca957c4feaebb5bfbecf664eedc0b557a5264d1d93f`;
the canonical DAG semantic SHA-256 embedded in the report was
`b01f93479c68073b419ef81a9a6878509347bfeab6a58f040fad7e3f35f967fa`.
Audit artifacts are under
`build/wric-chart-parallelization/oracle-audit-408434e/`.

This first freeze is superseded and must not be used as the final manifest
oracle. An independent post-freeze audit found that canonical capture forced
root provenance in score-only exact mode and allowed report-construction
exceptions to enter candidate rejection control flow. It also demonstrated
that in-process compact capture materially perturbs authoritative timing and
RSS. Phase 0 corrected the first two oracle bugs and moved canonical capture
to an unmeasured companion process. The hash above remains in this append-only
ledger as the superseded first-freeze identity, preserved at
`bin/dagutil.provisional-1e71334d`.

The corrected oracle leaves every algorithmic trim option identical between
capture-off and capture-on. Exact tied provenance is produced by a report-only
companion outside semantic catches and is accepted only after exact equality
of optimum, full keep mask, mask kind/exactness, every frontier size, active
pattern count, and invariant offset. Score-only capture remains
scalar/frontier-only. A forced report-construction failure now aborts rather
than invalidating a candidate. The final frozen identity is:

| Field | Corrected frozen value |
|---|---|
| Frozen path | `build/wric-chart-parallelization/baseline-408434e/bin/dagutil` |
| SHA-256 | `86ad9c3ec3ad3e25291672b65946120835dbd843717ee34b9c7f13d203663772` |
| Size | 84,477,936 bytes |
| Version output | `dagutil 2.2.0 (408434e)` |
| Source/frozen byte comparison | `cmp` exit 0 |
| Frozen mode | `-r-xr-xr-x` |
| Focused verification | 4/4 CTests pass, including score-only and forced-failure regressions |

A fresh two-run W1 audit of the corrected frozen executable is byte-identical
for compact search JSON, full NDJSON, and external canonical DAG JSON. Its
full/search semantic SHA-256 is
`a85e87a58a096c2a2457cb80454280c05a71551bbbb70ceaf4b13aeb097adb8a`;
the external DAG semantic identity remains
`b01f93479c68073b419ef81a9a6878509347bfeab6a58f040fad7e3f35f967fa`.
Artifacts are under
`build/wric-chart-parallelization/oracle-audit-corrected-86ad9c3e/`.

That corrected freeze is also superseded as the final workload-manifest
oracle, and its bytes are preserved read-only as
`bin/dagutil.provisional-86ad9c3e`.  Before any performance optimization, the
strict manifest review found that the product report did not repeat every
workload-defining search control.  The report was completed with the candidate
cap, seed, topology selector, sampling/path/clade bounds, polytomy caps,
lazy/cache/batch/memory controls, validation controls, and exact-search
controls needed for direct manifest-to-product validation.  The same rebuild
also includes a correctness-only loader repair: the 256-KiB gzip read buffer
now lives on the heap instead of overflowing the small worker-thread stack
when several inputs are loaded.  The previously failing multi-input ML sample,
edge-ML, and edge-weight tests all pass after that repair.  Neither change
alters chart search semantics, budgets, or the native executable.

Those report-complete bytes are preserved read-only as
`bin/dagutil.provisional-50fe01b2`. A final observability audit then made the
construction/materialization timers exception-safe and completed the six
Phase-0 timing and exact-concurrency fields consumed by the strict harness.
It found no scheduler, recurrence, pruning, candidate-budget, exactness, or
native-loop optimization. The final pre-optimization semantic-oracle identity
is:

| Field | Final frozen value |
|---|---|
| Source at capture | `build/bin/dagutil` |
| Frozen path | `build/wric-chart-parallelization/baseline-408434e/bin/dagutil` |
| SHA-256 | `7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c` |
| Size | 84,641,744 bytes |
| Version output | `dagutil 2.2.0 (408434e)` |
| Source/frozen byte comparison | `cmp` exit 0 |
| Frozen mode | `-r-xr-xr-x` |
| Focused verification | independent oracle audit 3/3; complete focused Phase-0 gate 9/9 |

A final two-run explicit-W1 audit again produced byte-identical compact search
JSON, full semantic NDJSON, external canonical DAG JSON, and output protobuf.
Its search/full semantic SHA-256 remains
`a85e87a58a096c2a2457cb80454280c05a71551bbbb70ceaf4b13aeb097adb8a`;
the external DAG semantic identity remains
`b01f93479c68073b419ef81a9a6878509347bfeab6a58f040fad7e3f35f967fa`.
Artifacts are under
`build/wric-chart-parallelization/oracle-audit-final-7ddb1fca/`.

### Native sample--SPR--merge non-change proof

The pre-WRIC merge-base fallback is not required. At the two pinned commits,
the complete `run_native` function in `tools/larch2.cpp` is byte-identical:

| Revision | Extracted lines | SHA-256 |
|---|---:|---|
| `fee366e71439d7b8c8eda588cc72cde8534f102f` | 1836--2278 | `06e03bc1c1cab8e7edee5eaa20ad739e6a46eb3811fc20d2e7b22a543e705343` |
| `408434ecfd096af484ecbbd3deeb67511151cd76` | 2047--2489 | `06e03bc1c1cab8e7edee5eaa20ad739e6a46eb3811fc20d2e7b22a543e705343` |

An exact `diff -u` of those extracted functions produced no output and exited
zero. The native-path headers `include/larch/native_optimize.hpp`,
`include/larch/spr_pipeline.hpp`, `include/larch/sample_method.hpp`, and
`include/larch/merge.hpp` also have no diff between the pinned commits. The
larger `tools/larch2.cpp` diff adds chart-B&B trim/reporting integration after
optimization; it does not alter `run_native`.

The reproducible commands and captured output are stored in
`baseline-408434e/provenance-commands.sh` and
`baseline-408434e/native-loop-proof.txt`.

## Phase 0 — build and machine metadata

Metadata was captured on `2026-07-13T15:09:59+03:00` in timezone
`Europe/Sofia`.

### Build

| Field | Value |
|---|---|
| Project version | 2.2.0 |
| CMake build type | `RelWithDebInfo` |
| C++ compiler | `/home/ogi-agent/install/gcc-trunk/bin/g++-trunk` |
| C compiler | `/home/ogi-agent/install/gcc-trunk/bin/gcc-trunk` |
| Compiler version | GCC 17.0.0 20260530 (experimental) |
| C++ flags | `-O2 -g -DNDEBUG -std=c++26 -freflection` |
| CMake | 4.3.3 |
| Generator | Unix Makefiles |
| Make | GNU Make 4.4.1 |
| CMake cache SHA-256 at native capture audit | `69dd8bef9d8b65818c6db4d9eb5ec3d023520dbc6ef72348857e26b703523ff4` |
| `larch2` flags file SHA-256 at native capture audit | `c44251a3390855a58f2005a4c94cf92d1a07575432f113f94e7c52babfdf0166` |

The cache and flags hashes describe the existing build used for the frozen
native executable; later rebuilds may legitimately change those working-tree
files. The frozen executable hash, not either build-file hash, is the runtime
identity gate.

### Machine

| Field | Value |
|---|---|
| Host CPU | AMD Ryzen 7 7730U with Radeon Graphics |
| Topology | 1 socket, 8 physical cores, 2 threads/core, 16 logical CPUs |
| Online CPUs | `0-15` |
| Physical-core affinity list | `0,2,4,6,8,10,12,14` |
| NUMA | 1 node; CPUs `0-15` |
| L1 data / instruction | 256 KiB / 256 KiB total, 8 instances each |
| L2 | 4 MiB total, 8 instances |
| L3 | 16 MiB, 1 instance |
| Installed RAM | 65,156,911,104 bytes (about 60.7 GiB) |
| Kernel | Linux 6.18.18-gentoo-gentoo, x86-64, musl userland |
| Page size | 4096 bytes |
| Clock ticks | 100 per second |

Timed acceptance runs must additionally record the live affinity, frequency
policy, load/interference, memory availability, exact environment, and working
tree dirty state in their own raw artifacts. The metadata above is not a
substitute for per-run capture.

## Phase 0 — fixture checksum inputs

Checksums for all planned base fixtures were initially captured in
`baseline-408434e/unsealed-sha256-inputs.txt`. That historical input list is
now covered by the sealed workload and artifact manifests; it is not itself
the authoritative manifest.

| Fixture | SHA-256 |
|---|---|
| `data/test_5_trees/tree_0.pb.gz` | `e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6` |
| `data/testcase/full_dag.pb.gz` | `19b429c8156e06fd3959650cd86398c0d6f499cbcf259f008a76159bfc12c9df` |
| `data/seedtree/seedtree.pb.gz` | `2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153` |
| `data/seedtree/refseq.txt.gz` | `088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c` |
| `data/20D_from_fasta/1final-tree-1.nh1.pb.gz` | `a65f300916f158ea4c8de8bc49f5a94379905a4c3b7fba337ce6773d65ecfce4` |
| `data/20D_from_fasta/refseq.txt` | `82c11885688b9a67b72ec4d2dd5913571a1723039ca501dc63bafb2cb5ea13eb` |
| `test/wric_four_taxon_misplaced.fa` | `8ad9f7e5749303e9c4c02cd8c8a51ed1294690effe1356d4f04fd86632765202` |
| `test/wric_four_taxon_misplaced.nwk` | `080cae5e92f87baa40ad621a181d64e9377d6ca5aa6f76feae9cca005e6de342` |
| `test/wric_four_taxon_misplaced.ref` | `06f961b802bc46ee168555f066d28f4f0e9afdf3f88174c1ee6f9de004fc30a0` |
| `test/wric_two_polytomy.fa` | `a52c06edd7cf75172220081bd5e9e856d2413f623d1681726edf865e299a6278` |
| `test/wric_two_polytomy.nwk` | `e41d284185c20c48166894306f63553b588976c4c6aa49509b7a5a1d00da1ceb` |
| `test/wric_two_polytomy.ref` | `06f961b802bc46ee168555f066d28f4f0e9afdf3f88174c1ee6f9de004fc30a0` |

At the Phase-0 seal, Phase-7/8/9 fixtures did not yet exist in the base
manifest and therefore had no Phase-0 performance result. They were
subsequently added through append-only frozen-oracle characterization and the
five sealed supplemental manifests whose hashes are recorded in the current
status table; no base row was replaced.

### 20D bounded-real preflight classification

A read-only diagnostic used the final frozen oracle on the exact 20D
preflight contract: one iteration, seed 1, grammar exact, one candidate and
one exact verification, 12 GiB chart memory budget, physical-core affinity,
and explicit W1. Four repetitions all ended before report emission with
ordinary exit status 1, signal/core/timeout flags zero, no output artifact,
and byte-identical stderr SHA-256
`84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35`.
The exact stderr is 159 bytes. Standard output is also byte-identical: 37
bytes with SHA-256
`b0174b43d6ddf95ec64e301a7820b4fb19c74c38d9da0873a76787715cfeab36`.
The runs took about 1.17--1.18 seconds and sampled about 47 MiB RSS. An
explicit-W8 confirmation produced the same stdout/refusal bytes and no output. The
final stderr line identifies the pre-existing bounded-refinement arity limit;
the throw is present at research tip `408434e` and predates this branch.

The input and refseq SHA-256 values are respectively
`a65f300916f158ea4c8de8bc49f5a94379905a4c3b7fba337ce6773d65ecfce4`
and
`82c11885688b9a67b72ec4d2dd5913571a1723039ca501dc63bafb2cb5ea13eb`.
The frozen initial score is 11155 and its canonical input semantic identity is
`b4e8c4ea029f2222a789026ebc56f096e37bebeb3fef9474ed333aa4c157ad2f`.
The canonical command digests are
`ac03537d4db4cc20a843154aae5ed578927d5d1852809c9a8d84fdc4b27dda2f`
for W1 and
`27f498987087c3ef91f39db04ae56107bca4adb0abd0c32aeb5abfa3d9c3faf3`
for W8.

This diagnostic selected the plan's exact `expected_infeasible` branch. The
final Phase-0 bootstrap subsequently repeated W1 and W8 through the frozen
timeout/RSS wrapper and approved both as
`high_arity_refinement_refusal`. Each sealed observation has ordinary exit
status 1; zero signal, core, timeout, and RSS-limit flags; no output artifact;
and the approved stderr SHA-256
`84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35`.
Their capture-status SHA-256 is
`470a630c48f7d05493f8fc3a21ddb2345bc83958e53554f5f8e713765ccd7f70`.
They are sealed refusal observations, not successful wall-time measurements.

## Phase 0 — measurement ledger

### Process-wrapper lifecycle and calibration work

The schema-v2 process wrapper now enforces the frozen descendant-tree RSS cap
at a 10-ms cadence, subreaps and drains the complete tree, identity-checks
reparented and `setsid` descendants, restores a waitable `SIGCHLD`
disposition, and records timeout and RSS observations under the frozen
timeout-first outcome precedence.  The focused lifecycle suite passed once
and then passed 10 consecutive `until-fail` repetitions (43.42 seconds total).
Those repetitions include deterministic zombie cascades, rapid forks,
ignored inherited `SIGCHLD`, timeout/RSS overlap, process-group escape, and
200 late-fork/`setsid`-at-leader-exit cases.  The exact schema remains 41
fields.

The first interference-screened saturated calibration is retained as a failed
diagnostic at
`build/wric-chart-parallelization/wrapper-overhead-schema2-20260713T1847+0300-f2f6dd1b/`.
It used 15 alternating direct/wrapped measured pairs on physical CPUs
`0,2,4,6,8,10,12,14`.  Its median paired wrapped/direct ratio was
`1.028192179`, and its ratio of medians was `1.041456838`; both exceed the
strict `1.02` gate.  Two attempted longer confirmations were rejected when
the live guard observed unrelated test/build interference, so neither is
claimed as evidence.

The steady-state monitor was subsequently optimized without changing the
sampling interval or lifecycle contract: one sample now performs one complete
tree traversal and about two identity reads for the common one-process case,
instead of two traversals and about eight reads.  The resulting runner binary
SHA-256 is
`3dbf846e6aa05b007160d9b8d149a82f8e26c4e8099a4e4fc39fdbc63ab02104`.
The adversarial 10-repetition lifecycle result above is against these exact
bytes. At that historical checkpoint no passing calibration had yet been
recorded; the sealed completion update below supersedes that then-current
status without altering the diagnostic record.

Two canonical-v3 attempts against these exact bytes were rejected without
creating a JSON artifact. The directories
`wrapper-calibration-v3-20260713T2130+0300-3dbf846e/` and
`wrapper-calibration-v3-20260713T2135+0300-3dbf846e/` remain empty evidence of
the fail-closed behavior. The first attempt rejected a wrapped arm at
`6.529225729094745...` CPU/wall; after an isolated wrapped-arm reproduction
reached `7.859542808287`, a fully paused retry rejected a later direct arm at
`5.918823060149...`. Both are below the immutable `7.2` eight-core saturation
floor, indicating intermittent host competition. Neither attempt is an
overhead result and the gate has not been relaxed.

#### Passing calibration and sealed capture update (completed 2026-07-22)

The failed diagnostics above remain part of the append-only record, but their
then-current statement that no passing calibration existed is now superseded.
The canonical calibration JSON in the detached Phase-0 worktree is
`build/wric-chart-parallelization/phase0-calibration-408434e/wrapper-calibration.json`;
the bootstrap consumed a byte-identical copy at
`baseline-408434e/bootstrap-phase0/wrapper-calibration.json`. Its SHA-256 is
`b254001b2327e0b2e8ef2c1d6c32e5b0f5e315bdeeadffa9249a8765228f39d0`
and its decision is `PASS`. The median paired wrapped/direct ratio is
`1.015775988195` and the ratio of medians is `1.008541338978`, both below the
unchanged `1.020000000000` limit. The live guard recorded zero forbidden
process matches; its maximum unselected-SMT activity was 65,518 ppm against
the frozen 200,000-ppm threshold.

All 12 ordinary captures have completed under
`baseline-408434e/bootstrap-phase0/captures/`; their status records are stored
at `baseline-408434e/bootstrap-phase0/capture-*.status.json`:

| Capture ID | Canonical rows | Repeat stages | Timeouts | Status SHA-256 |
|---|---:|---:|---:|---|
| `small-dense64-physical` | 25 | 5 | 0 | `841ec89429720bef9b5ef7d3f6f4a20e77393d1870a6b62e1a34e2c865ede4f8` |
| `small-exact1-physical` | 20 | 4 | 0 | `94132bd12bfb736f1d4248ab3fc9c199c49db217ae046e3418b492126aa74b38` |
| `small-primary32k4-physical` | 36 | 12 | 0 | `d1edbe2b79abbd64213f16ab30d41f0906f1f31db268a3da8d096453a8bc7ae6` |
| `small-stress128k16-physical` | 39 | 13 | 0 | `ae4e51995372a1228ca3d2c7f5f004babc91a47cf2824868d59b263361f263ff` |
| `small-auto-unpinned` | 15 | 3 | 0 | `5f7c6a2cd34f2ae658cbbb15ce14074e4b66a906000eb1170eb88f2e451632d4` |
| `medium-dense64-physical` | 20 | 4 | 0 | `433e236692c884cf278c67c83f51aec22e4157cd80ae54c40a0bd52333907af3` |
| `medium-cache1-physical` | 20 | 4 | 0 | `da5f4c77f516657160ad684eec3238ba4f63531a740da8fcea75345e5e9d6dcd` |
| `medium-exact1-physical` | 4 | 0 | 4 | `9bc0b7fc4385c94a867b19323e905fb0159eb83b7b28e38b2c59593e11385243` |
| `medium-primary32k4-physical` | 33 | 5 | 8 | `99f82d4dc4db02ede225bc2381c7366659cf0152ff76afeeb358a756f7525fcf` |
| `medium-stress128k16-physical` | 23 | 5 | 8 | `e2956ed5fa183d1f734be6c75bcd73205b62ce55e5296fc11b9d55727e3d8c2b` |
| `medium-lazy64-physical` | 20 | 4 | 0 | `56d80b07d6204889b2eb81f1154415cc6cdfa4552b7117503211a20a3a074061` |
| `medium-primary32k4-smt` | 12 | 1 | 7 | `c2db6d0f982ce80e32e290ac5c66c1740ef7e18fc0f5ad1960b92159f8701e5b` |
| **Ordinary total** | **267** | **60** | **27** | — |

`medium-dense64-physical` bound W1/2/4/8 to four canonical paired rows
each and covered 2451.982 seconds between its live pre/post snapshots. Its
preflight quiet-monitor SHA-256 is
`b334e99c5619261026d799bc7546bc6d3aa54172526f4a6867027499104d5e0b`;
no capture-run guard transcript was persisted, so only the preflight monitor
survives and no claim here depends on an absent transcript. The corresponding
`medium-cache1-physical` interval was 2324.777 seconds, with the same four
W1/2/4/8 repeat stages. Its preflight quiet-monitor SHA-256 is
`960c6144fa0de3b554178975b61c590210d0361ece48c49a1b9873ad4f8d90a3`
and its persisted no-interference guard log SHA-256 is
`da758986264aa05e7d57b1171b3d474daafe6ab99aeade673306953fae6c0925`.
These durations describe capture intervals, not accepted wall-performance
results.

The 27 timeouts are explicit record-only characterization observations
authorized by four immutable timeout-approval records. They do not make a
strict later-phase acceptance run timeout-tolerant. Four earlier starts of
`medium-dense64-physical` were interrupted by
unrelated external builds and were moved out of the baseline to
`phase0-failed-external-interference-20260716T233345Z/`,
`phase0-failed-external-interference-20260717T010228Z/`, and
`phase0-failed-external-interference-20260717T011149Z/`, plus
`phase0-failed-external-interference-20260717T031134Z/`. The fourth retry
passed its five-second guard at 102,766 ppm and then ran cleanly for more than
14 minutes before the recurring proof-assist build restarted during its second
repeat block. All four are contaminated partial attempts and count as neither
captures nor timing evidence. A subsequent `medium-exact1-physical` launch at
`2026-07-17T10:18:59+03:00` was stopped immediately by its persisted guard
when the same external build restarted in the launch race. The helper created
no artifact for that rejected attempt; the later accepted exact-one capture is
the status row in the table above. The rejected guard log has SHA-256
`07b7b9ff848c40ed575a3f490581c14ccb7eb4f6d617b4ad5ba2f7560fab79e5`.

The thirteenth capture is the real 20D exact-one preflight. It records two
non-timing observations, W1 and W8, both approved as
`high_arity_refinement_refusal` with the exact exit/no-output contract and
stderr hash documented above. Thus the complete Phase-0 capture consists of
267 ordinary canonical rows plus two real-refusal observations, across 13
capture statuses and 60 repeat stages.

The pending and final ordinary strict smokes and the pending and final real
strict smokes all exited zero and bind manifest SHA-256
`32ae82a93cb72a28afaa510391eac83f80f13638d46e1513881d2b484c9dc3ae`.
Pending audit, seal promotion, and final audit passed. The pending and final
workload manifests are byte-identical at that hash; the seal transaction names
`workloads.tsv` and `phase0-artifacts.tsv` as the final manifests. The final
artifact ledger has SHA-256
`a33436d78ec6c840427b343615c6b9a7b555a87f989000865873e9c5adeb06bb`.
Phase 0 is complete as a sealed measurement base. No later-phase performance,
RSS, scaling, parity, supplemental-manifest, policy, or default gate is closed
by that fact alone.

### Final sealed-base independent audit

An independent read-only audit reran the frozen helper's final audit and
returned `strict final-manifest/artifact audit passed`. A complete before/after
snapshot of path, type, mode, size, nanosecond mtime, and content SHA-256 was
byte-identical at
`c97844e8efe65735b320051634e7a3cd41b62b23e636a70810229f6c2dad3432`,
covering 7,448 files and 332 directories; the audit modified nothing.

The independent closure check verified 89 unique ordered workload rows, all
13 planned capture statuses, the 267 ordinary rows and two real refusal
observations, all 60 repeat stages, and exact coverage of the 27 approved
timeouts by four approval records. It also verified every entry in the
7,451-row final artifact ledger. The pending-to-final ledger delta is exactly
67 expected additions, with no removed URI and no changed common hash. The
final manifest and ledger hashes match the sealed values above, and the
required manifests, detached seals, transaction, command contract, helper,
and executable roles have their exact read-only/executable modes.

### Strict bootstrap and source-binding audit

The final pre-capture bootstrap/harness state passed an independent audit on
stable source bytes.  The audit covered all seven strict areas: exact timed
process-metrics binding, fixture-wide initial-score unanimity, exact canonical
DAG binding, compact/full semantic-sidecar binding, RSS-cap enforcement,
non-overwriting transaction ownership/resume behavior, and detached artifact
ledger closure.  It also ran a real small characterization capture and a real
two-trial repeat capture with the frozen oracle/runner, then rejected nine
independent source-splice mutations.  Valid-timeout replay and representative
command, process, unavailable-sentinel, and nonunanimous-score forgeries were
also rejected.

The independent bootstrap self-test reconstructed exactly 13 captures and 89
rows.  The bootstrap shell test, complete benchmark-harness shell test, Python
compile, Bash syntax check, and `git diff --check` all passed.  No long capture,
calibration, finalization, or seal was performed by the audit.

After the source-binding audit, the final focused Phase-0 CTest gate passed
9/9 tests serially in 65.64 seconds on the required GCC-trunk C++26
RelWithDebInfo build.  It covered process lifecycle accounting, calibration
workload/controller validation, product observability, canonical CLI/report
semantics, smoke and adversarial harness behavior, and bootstrap closure.

The subsequent complete target refresh passed (`cmake --build build
--parallel 2`, 1309.40 seconds wall).  The full RelWithDebInfo suite then
passed all 155 configured tests with zero failures in 216.81 seconds: 153
tests passed and the two established diagnostic tests, `merge_consistency_test`
and `rotaA_diagnostic_test`, were skipped as expected.  `git diff --check`
remained clean.  The completed `LastTest.log` SHA-256 was
`43ec997396bb12fce3bbf7c8c0bea0afefb452ca10e2ce1e3604e3f297aa5ed0`.
`CMakeLists.txt` had SHA-256
`f8f2c89d276a9363d59a8d5c633a3f995f4f8791ac1f2258b6da87585942f7cc`
for this gate, and the frozen native, oracle, and runner hashes/modes were
unchanged afterward.

| Audited source | SHA-256 |
|---|---|
| `tools/wric_phase0_manifest_bootstrap.py` | `21d6bed6fa1bb895eed0241f9e041c48dbaea177fe61b88d4f812e5f69de0439` |
| `tools/wric_spr_search_benchmark.sh` | `9cdcd0c0eaa9cfed37ceed3a34c8cc557637988bc0834a17465a752d8560ba32` |
| `test/wric_spr_search_benchmark_harness_test.sh` | `3c632bd275c279ec59ef435e9970553b5bcebceb3de916b709306d9e6289b70e` |
| `test/wric_phase0_manifest_bootstrap_test.sh` | `f0ca1c5f46544a5337c7f4bccf8e01d20c345762cf59b0beedfdbb3e9c4f8570` |

#### Post-audit validation strengthening

A subsequent requirement-by-requirement audit found that two implemented
contracts lacked direct adversarial coverage: supplemental-manifest validation
and the harness's frozen native/oracle identity checks.  The harness regression
now executes a real selected supplement and proves both renamed rows reach the
raw trial ledger.  Exact-reason negative cases cover an invalid supplement
schema version, wrong parent, stale body seal, corrupted detached seal, base-row
override, cross-supplement duplicate, changed inherited native/oracle roles,
and tampered supplement preamble assets.  Separate exact-reason cases cover a
bad frozen-native digest, a passed native executable with different bytes, and
a bad frozen-oracle digest.  All mutations use private temporary fake assets;
two independent static reviews traced them to the intended validation branches.

The strengthened harness test SHA-256 is
`50c31447eedc20418ebab2adabc2e749a0cfa88cc8c1f6f963600c6d84667c9e`.
Its standalone run passed in 58.21 seconds.  The final focused gate then passed
9/9 tests in 69.63 seconds.  Read-only focused artifacts are under
`build/wric-chart-parallelization/phase0-focused-ctest-20260713T2325+0300-50c31447/`;
the focused `LastTest.log` SHA-256 is
`a7b5bd5a66eb35872f3684474c23f669b2f3c7e57c8e511e33a45a30d6067d21`.

The authoritative final full suite on the same source passed all 155
registrations with zero failures in 137.99 seconds: 153 passed and the two
pre-existing external-data diagnostics, `merge_consistency_test` and
`rotaA_diagnostic_test`, returned their established CTest skip code because
their private canonical repro data is not installed on this host.  The
inventory is exactly 147 pre-existing plus eight new Phase-0 registrations;
none is disabled or removed.  Read-only full-run output, JUnit, inventory, and
`LastTest.log` are under
`build/wric-chart-parallelization/phase0-full-ctest-20260713T2327+0300-50c31447/`.
Their SHA-256 values are respectively
`5b062a971d979fddfb63a1f7839249075d81331b174923a0684c89a19a0f98da`,
`f98861880acefdce65bce7f1f55d7fa9014f7aebc8d63249a6ae334246b289d7`,
`d7aa0157f150be7b3f787826a98a98ddf4696041ef1dd39ce5e4b03fe6643e3a`,
and
`4b9adb9165156b5cfae34b64a559a245c729dcba30a890fc87a5fa98d8001a2a`.
This supersedes the earlier full-run log evidence, whose mutable CTest
`LastTest.log` was not retained before a later inventory command replaced it.
The frozen native, oracle, and runner hashes and modes remained unchanged.

### Raw artifact contract

The final Phase-0 artifact set is sealed:

| Artifact | Current state |
|---|---|
| `bin/larch2` | frozen at SHA-256 `ee160aa4fdecce5660f1bf0737289fc32fd33b1965e660f0ec92072805a4de9b`; ledger-covered |
| `bin/dagutil` | final oracle frozen at SHA-256 `7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c`; ledger-covered |
| `workloads.tsv` | sealed at SHA-256 `32ae82a93cb72a28afaa510391eac83f80f13638d46e1513881d2b484c9dc3ae` |
| `commands.phase0.sh` | prepared command contract; ledger-covered |
| raw/aggregate TSVs, reports, and canonical sidecars | captured beneath the 12 ordinary capture directories; ledger-covered |
| real preflight evidence and approval | W1/W8 exact refusal statuses and approval; ledger-covered |
| `phase0-artifacts.tsv` | final ledger sealed at SHA-256 `a33436d78ec6c840427b343615c6b9a7b555a87f989000865873e9c5adeb06bb` |

No existing artifact in this directory may be overwritten once the final
Phase-0 SHA-256 manifest is sealed.

### Named microbenchmarks

An ordinary timeout below is an explicit manifest-approved characterization
row, never a blank or imputed result. Later supplements remain separate.

| Benchmark | Frozen mode/budget from plan | Workers required | Phase-0 result |
|---|---|---|---|
| `dense-local-small-64` | `grammar_lower_bound`, 64/0 | matrix | captured in `small-dense64-physical` |
| `dense-local-medium-64` | `grammar_lower_bound`, 64/0 | matrix | captured in `medium-dense64-physical` |
| `cache-medium` | `grammar_lower_bound`, 1/0 | matrix | captured in `medium-cache1-physical` |
| `exact-small-one` | `grammar_exact`, 1/1 | matrix | captured in `small-exact1-physical` |
| `exact-medium-one` | `grammar_exact`, 1/1, 600 s | 1 and 8 required | four approved timeout rows captured in `medium-exact1-physical` |
| `exact-medium-topk4` | `grammar_exact`, 32/4 | matrix | captured in physical-core and SMT primary captures, with approved timeout rows |
| `lazy-compression-medium` | `grammar_lower_bound`, 64/0, forced lazy | matrix | captured in `medium-lazy64-physical` |
| `sampled-generation-high` | `sampled_tree_fixed`, at least 256/1 | later supplement | sealed in `phase8-generation.tsv` at SHA-256 `773545d093904c20eaa80343615fa77089989735e61d34947530b07ef008cf17` |
| `local-commit-three` | `grammar_exact`, 32/4, 3 iterations, local commit | later supplement | characterization and production manifests sealed at SHA-256 `8dc08f393bccb4c8a609501f90cb3c949f1b44600ef967fa8038edc7fd399ed1` and `2f332c5d459b8f47901df015427cba3d4ead7daedac56e1fd9873882b5b65b39` |

### Unsealed diagnostic characterization

The following single runs are diagnostic only. They were made with the
provisional first-freeze oracle while the final harness/manifest was still
under repair, so they do not replace the sealed Phase-0 rows or establish a
performance claim. They are retained because they independently identify the
optimization target and give exact commands/artifacts under
`build/wric-chart-parallelization/characterization-408434e/`.

| Workload | Workers | Wall s | User s | System s | Max RSS KiB | Key phase |
|---|---:|---:|---:|---:|---:|---|
| small dense lower-bound 64/0 | 1 | 0.101712 | 0.085880 | 0.010735 | 8,264 | local 47.636 ms |
| small dense lower-bound 64/0 | 8 | 0.172674 | 0.481042 | 0.332485 | 8,188 | local 122.556 ms |
| medium dense lower-bound 64/0 | 1 | 2.397251 | 2.178220 | 0.223835 | 48,556 | cache 449.628 ms; local 1,695.140 ms |
| medium dense lower-bound 64/0 | 8 | 4.082843 | 9.255927 | 7.409546 | 48,880 | cache 467.759 ms; local 3,345.173 ms |
| small grammar-exact 1/1 | 1 | 0.959422 | 0.927305 | 0.026589 | 6,400 | exact init 463.592 ms; verifier 468.251 ms |
| small grammar-exact 1/1 | 8 | 0.936369 | 0.901586 | 0.036063 | 6,468 | exact init 455.259 ms; verifier 461.681 ms |
| medium grammar-exact 1/1 | 1 | timeout at 600.010257 | 596.257898 | 3.696261 | 108,808 | no completed search report |

All completed one-versus-eight pairs had byte-identical compact search and
canonical output-DAG reports, and every sampled process reported zero swap.
The medium exact timeout is explicit process-runner output, not a missing or
imputed row. Its in-process canonical capture is known to add reporting
overhead, so the final corrected capture uses a capture-off timed process and
a separate correctness companion.

### Primary and stress baseline availability

| Contract | Native rows | Chart rows | Canonical parity | Wall result | RSS result |
|---|---|---|---|---|---|
| medium primary, native 50 vs chart 32/4 | captured | captured, including approved timeout observations | sealed for completed ordinary trials | optimized comparison pending | optimized comparison pending |
| medium stress, native 50 vs chart 128/16 | captured | captured, including approved timeout observations | sealed for completed ordinary trials | optimized comparison pending | optimized comparison pending |
| bounded real-scale confirmation | not applicable to refusal branch | W1/W8 `expected_infeasible` refusal observations sealed | exact refusal identity sealed | no successful timing row | no successful timing row |

The primary acceptance summary will report all five paired trials, alternating
execution order, each paired chart/native ratio, both medians, and the
worker-scaling ratio. Medians alone cannot hide a paired-ratio failure.

### Canonical oracle checks

| Required check | Result |
|---|---|
| Two explicit one-worker runs byte-identical | pass on independent small exact audit |
| Compact digest covers full untimed sidecar | pass; compact semantic SHA equals full-sidecar byte SHA |
| Ordered candidates/signatures/scores captured | pass in canonical schema/unit and CLI tests |
| Exact keep mask and tied provenance captured canonically | pass in canonical schema/unit and CLI tests |
| Accepted sequence and per-iteration objectives captured | pass in canonical schema/unit and CLI tests |
| Chain identity and final validated parsimony captured | pass in canonical search plus external-DAG schemas |
| Final canonical topology digest captured | pass in canonical search plus external-DAG schemas |

### Phase-0 acceptance checklist

- [x] Exit-141 harness regression repaired and covered by a repeated-key test.
- [x] Smoke test emits exactly the required rows and exits zero.
- [x] Acceptance mode rejects missing manifest, missing rows, timeout, child
  failure, validation failure, workload mismatch, and missing CPU/RSS fields.
- [x] Record-only mode accepts exactly and only manifest-declared timeout rows.
- [x] Unified worker and memory controls, compatibility conflict, and default
  behavior are tested.
- [x] Requested/resolved workers and all Phase-0 timing/process metrics are
  present in report and TSV output.
- [x] Canonical semantic output is deterministic and the corrected oracle's
  observational-safety/exception audit passes.
- [x] Workload and supplemental manifest validation tests pass.
- [x] The frozen process-wrapper calibration passes both 2% overhead gates and
  its canonical JSON is bound by SHA-256.
- [x] The four required small physical-core captures contain 120 canonical rows
  across 34 repeat stages with zero timeouts, and the separate automatic/
  unpinned small capture adds 15 rows across three stages with zero timeouts.
- [x] The dense64 and cache1 physical-core medium captures contain 40 canonical
  rows across eight W1/2/4/8 repeat stages with zero timeouts.
- [x] Native and exact-chart medium 32/4 and 128/16 rows are captured, with
  every manifest-approved timeout recorded explicitly.
- [x] Every remaining medium and real-preflight row is captured without
  external interference.
- [x] Instrumented `dagutil` is frozen; exact native/oracle mismatch rejection
  is covered by the harness regression and runner mismatch rejection by the
  bootstrap regression.
- [x] The pending/final manifest and harness audits re-verify the frozen
  executable hashes, and all four strict ordinary/real smokes exit zero.
- [x] `commands.phase0.sh`, reports, TSVs, sidecars, fixtures, and executables
  are covered by final artifact ledger SHA-256
  `a33436d78ec6c840427b343615c6b9a7b555a87f989000865873e9c5adeb06bb`.
- [x] All pre-existing and new Phase-0 tests pass (155/155, with two expected
  diagnostic skips and zero failures).
- [x] Frozen Phase-0 capture inputs and artifacts contain no product
  performance optimization; deadline-overlap Phase-1+ working-tree changes are
  segregated and excluded from every Phase-0 executable role.

**Phase-0 decision:** pass. The base workload manifest is sealed at SHA-256
`32ae82a93cb72a28afaa510391eac83f80f13638d46e1513881d2b484c9dc3ae`.
This decision establishes the immutable comparison base only; it is not a
decision on any later-phase performance gate.

## Phase 1 — immutable chart execution plan

### Implementation checkpoint

| Field | Value |
|---|---|
| Code revision | `208ce23f0c005d3702d114f535fe21564b3b79b6` (`Compile immutable chart execution plan`) |
| Baseline parent | `7ca527b8906d018124756182274335cbff936d72` (`Baseline measure`) |
| Build directory/type | `build/`; `RelWithDebInfo` |
| Compiler | `/home/ogi-agent/install/gcc-trunk/bin/g++-trunk`; `g++-trunk (GCC) 17.0.0 20260530 (experimental)` |
| Optimization flags | `-O2 -g -DNDEBUG` |
| Functional-run dirty state | product, test, and tool tree exactly at `208ce23`; only this plan/results documentation was unstaged |
| Raw functional evidence | `build/wric-chart-parallelization/phase1-functional-20260714-208ce23/` |

The checkpoint adds an immutable generation/fingerprint-bound chart execution
plan with canonical inside/outside orders, dependency levels, flat production
and child descriptors, binary metadata, a checked generic-arity path, and the
shared four-state transition table. Dense, lazy, exact-trim, local-score, and
inside/outside cache paths consume the plan. Search states own the resident
plan; fresh exact, transient, accepted-tip, and final-compaction grammars are
published with a fresh generation and their plan as one checked result.

Candidate preparation publishes a private, base-pointer-free, const execution
descriptor. Its raw and compiled arrays cannot diverge after publication, and
four simultaneous readers were tested with independent row scratch. Standalone
public APIs retain their full checked boundary. Inside an immutable acceptance
epoch an opaque grammar/plan capability is minted once and then propagated
through candidate generation, local scoring, exact verification, cache
construction, transient extension, and commit preparation.

Validation accounting distinguishes resident fingerprint scans, legacy index
checks, output-plan partition checks, candidate-plan checks, dynamic overlay
payload checks, and the candidate-by-pattern recurrence region. Dynamic
payload validation includes unreachable temporary productions and is preserved
on exceptional materialization exits. The planned materializer callback
overloads are constrained, so literal-null stats arguments select the intended
non-callback API.

### Mechanical counter and semantic evidence

The focused tests establish all of the following without timing inference:

- one full compatibility/fingerprint scan per resident acceptance iteration,
  independent of candidate batch size and exact top-K work;
- zero full grammar validations, production-partition validations, and
  clade-order sorts in candidate-by-pattern scoring;
- one candidate execution-plan build per prepared candidate and reuse across
  its patterns;
- fresh grammar generations and exactly one output plan for planned
  candidate/chain materialization;
- stale same-generation grammar mutation, wrong generation, wrong fingerprint,
  moved-from plan, and wrong resident identity rejection before recurrence
  work;
- validation of every dynamic temporary production, including invalid
  unreachable payload, with success and failure counts retained separately;
- dense/lazy, binary/multifurcating, exact optimum, keep-mask, fluidity, tied
  provenance, cache, chain, and canonical-report equivalence on the existing
  oracle fixtures.

### Reproducible functional commands

```sh
cmake --build build --target \
  parsimony_chart_test chart_parallel_test chart_trim_test chart_spr_test \
  chart_spr_search_test overlay_chain_test chart_two_chart_oracle_test \
  multifurcation_chart_oracle_test inside_chart_cache_test \
  outside_chart_cache_test chart_spr_semantic_report_test dagutil --parallel 4

ctest --test-dir build \
  -R '^(parsimony_chart_test|chart_parallel_test|chart_trim_test|chart_spr_test|chart_spr_search_test|overlay_chain_test|chart_two_chart_oracle_test|multifurcation_chart_oracle_test|inside_chart_cache_test|outside_chart_cache_test|chart_spr_semantic_report_test)$' \
  --output-on-failure

cmake --build build --parallel 4
ctest --test-dir build --output-on-failure --parallel 4
```

The final targeted rerun passed 11/11 tests with zero failures in 45.70
seconds. Its copied `LastTest.log` SHA-256 is
`200b719bc488ce390f88bf12cda546ff1dcefe2ac3cdab56bfe2e32c00bfe20c`.

The final full RelWithDebInfo run passed all 156 registered tests with zero
failures in 114.90 seconds. The two pre-existing optional external-data
diagnostics, `merge_consistency_test` and `rotaA_diagnostic_test`, returned
their established skip status. The copied full `LastTest.log` SHA-256 is
`71f1168c4165fa2bc1e51459f786a37169f6f920eb0f71eeea377eeb0883bfa6`.

### Phase-1 exit decision

| Exit criterion | Decision |
|---|---|
| Zero candidate-by-pattern full validation and clade sorting | pass, mechanically asserted |
| Targeted and full RelWithDebInfo CTest | pass |
| Canonical semantic equality to sealed Phase 0 | sealed base available; comparison run pending; in-tree independent semantic oracles pass |
| Medium one-worker local-score target and <=5% chart-phase regressions | sealed base available; timing run pending |

Phase 1 is therefore **implementation complete; performance acceptance
pending**. No timing, scaling, RSS, or final parity claim is made from the
deadline-overlap worktree; the sealed comparison base is now available for
the outstanding runs.

## Phase 2 — first allocation/reuse checkpoint

Checkpoint `a7b810b` (`Reuse immutable inputs across chart scoring`) records a
bounded, functionally verified first wave. It is not the Phase-2 exit point.
The checkpoint:

- borrows immutable leaf-state spans in dense candidate-by-pattern scoring and
  retains serial row-scratch capacity across candidates and pattern batches;
- finalizes one owning exact setup from cold or resident inside charts, reuses
  it across both exact frontier passes, retains no inside chart or borrowed
  pattern pointer, and deduplicates initial-upper-bound topologies;
- builds cold outside rows directly from a compatible resident inside cache,
  with zero inside recurrence builds on that path; and
- stamps exact setups and inside caches with generation plus full structural
  fingerprint, rejecting stale same-address/same-shape inputs before row work.

Deterministic counters cover leaf-view use, scratch-capacity growth, exact
setup/inside/outside/topology/frontier work, and cold outside-cache reuse. The
new local-kernel fields are mirrored into the search summary and CLI counter
reports. The separate scoped allocation observer and the production search
wiring needed for the strict 80% allocation gate remain Phase-2 work.

The focused RelWithDebInfo build compiled the five affected test binaries plus
`dagutil` and `wric_counter_baseline_compile_check`. A combined rerun passed all
14 focused semantic, cache-identity, exact-setup, canonical-report, CLI-report,
and counter-compile registrations in 41.10 seconds. The copied log is
`build/wric-chart-parallelization/phase2-wave1-20260714-a7b810b/focused-and-report-contract.LastTest.log`;
its SHA-256 is
`c7d55635eaed195268df7cd7d83ec5a08f2bea5e83910c5bc85279322b97422f`.

The Phase-2 allocation-reduction, serial-speed, exact-small-speed, RSS, ASAN,
full-CTest, and Phase-0-relative timing criteria are still open. No timing from
this busy-host functional run is acceptance evidence.

### Phase 2 — exact/outside reuse and outside-stack checkpoint

Checkpoint `0232cb2` (`Reuse exact charts and remove outside scratch`) records
the second bounded Phase-2 wave. The worktree was clean immediately after the
commit. This is not the Phase-2 exit point.

The checkpoint:

- consumes all-active resident inside charts when finalizing the current
  state's exact setup, caches the resulting trim across repeated exact gates,
  and keeps explicit cold and lazy accounting for representations that cannot
  use that dense setup;
- builds the local-commit outside cache from the just-built inside cache, with
  a checked `0 built / P reused / P outside built` contract;
- avoids copying and advancing persistent inside/outside caches for production
  transient verification, while retaining that work behind the two-chart
  diagnostic oracle and reporting it separately;
- dispatches binary outside recurrences through fixed stack arrays and reuses
  one checked vector scratch bundle per generic-arity production; and
- adds an executable-local allocation observer covering all eight replaceable
  C++ allocation forms and twelve corresponding deletion forms. The observer
  is self-tested only at this checkpoint; it does not yet satisfy the scoped
  scorer allocation gate.

Exact-setup, leaf-copy, outside-cache, upper-bound-deduplication, frontier,
binary/generic outside-recurrence, and diagnostic-cache counters are asserted
in the focused tests and exposed through the search summary, `dagutil`, and the
counter-baseline compile surface. The recurrence tests include a saturated-add
witness plus deterministic grammar/compiled-plan equivalence across every
parent state, unreachable and near-saturated parent costs, varied child rows,
and a multifurcating generic path.

The coordinated RelWithDebInfo build used:

```text
cmake --build build --target chart_parallel_test chart_spr_search_test \
  chart_trim_test inside_chart_cache_test outside_chart_cache_test \
  chart_spr_allocation_test dagutil wric_counter_baseline_compile_check \
  --parallel 4
cmake --build build --target parsimony_chart_test \
  chart_spr_semantic_report_test --parallel 4
```

The initial focused run found one observer self-test defect: GCC legally
elided same-translation-unit new/delete expressions. The pointer sink was
moved across a translation-unit boundary, the observer test passed in
isolation, and the complete focused command was rerun. The final 29-test
semantic, cache, CLI/report, observer, and architectural-guard set passed
29/29 in 18.97 seconds. Its copied log is
`build/wric-chart-parallelization/phase2-wave2-20260714-0232cb2/focused-semantic-and-report.LastTest.log`;
the SHA-256 is
`7d0d8b6da0a8840e5ba70720891c7f0f454f6fd56aec21c5c8d78e878088d511`.

A strict post-checkpoint audit keeps Phase 2 open. The returning scorer still
owns output vectors and creates row scratch per public call; default
one-candidate acceptance batches therefore need a caller-owned `_into` seam
and search-lifetime workspace before the allocation region can be measured.
The profiled local unit-Fitch recurrence still needs its all-state
specialization. Two literal duplicate-inside-build paths also remain:
pattern-batch exact initialization first computes discarded composite charts,
and all-active local-commit setup rebuilds inside-cache rows after resident
pattern charts were created. A non-vacuous upper-bound topology-deduplication
witness, the 80% allocation comparison, timing/RSS gates, targeted ASAN, full
CTest, and deferred Phase-0-relative acceptance remain pending. No timing from
this busy-host checkpoint is acceptance evidence.

### Phase 2 — recurrence-ownership and unit-Fitch checkpoint

Checkpoint `a6d3c02` (`Reuse resident charts across local exact setup`) records
the third bounded Phase-2 wave. The worktree was clean immediately after the
commit. This is not the Phase-2 exit point.

The checkpoint closes the remaining profile-supported duplicate-recurrence
work:

- a production local-row kernel uses the all-parent-state unit-Fitch
  recurrence directly, retaining generic provider order and saturated-cost
  behavior and reporting every fast-path production;
- all-active local-commit initialization projects the already-resident state
  charts into its persistent inside cache, while the lazy representation
  projects its resident class rows, so neither path repeats an unchanged dense
  recurrence;
- pattern-batch local-commit initialization defers its initial composite until
  the persistent inside cache has built the one authoritative chart per active
  pattern, then exposes those rows through an owning exact-setup provider;
- conservative pattern-batch exact initialization lets its one cold exact
  setup own the initial composite rather than first scanning and discarding an
  equivalent set of batches;
- initial-state, persistent-inside-cache, and exact-setup chart ownership have
  disjoint build/reuse counters. Before candidate verification begins, local
  orchestration asserts that the three recurrence owners sum to exactly the
  active-pattern count; and
- resident-cache compatibility includes the full active-pattern identity,
  distinct frozen destination support, lifetime/mutation rejection, and a
  fail-closed deferred-bootstrap boundary. Outside-cache construction reports
  only the recurrence it actually executes rather than inheriting historical
  inside work.

The earlier dense Callgrind profile under
`build/wric-chart-parallelization/phase2-profile-a7b810b/` attributed 50.20%
of sampled local-scoring work to the recurrence and 87.5% to
candidate-by-pattern accumulation. That evidence motivated the all-state
unit-Fitch specialization. Equality-key replacement was not applied to this
dense kernel: the same profile did not identify a material dense/local
equality-key hotspot; lazy structural keys remain Phase 7 work and exact
frontier/provenance keys remain Phase 5 work. The scoped allocation profile is
repeated after the caller-owned workspace lands, and this decision must be
revisited if a key path becomes material there.

The exact-setup upper-bound deduplication test is non-vacuous: it generates
three starting topologies, proves that only two are unique, and checks the
same optimum/keep result for cold and resident setup with and without the UA
edge convention. Exhaustive, boundary, randomized, and provider-order tests
cover the unit-Fitch helper. Binary and multifurcating resident-cache tests
cover destination ownership, source destruction, same-shape mutation, stale
generation/fingerprint, and active-pattern mismatch.

The coordinated RelWithDebInfo build used:

```text
cmake --build build --parallel 4 --target parsimony_chart_test \
  chart_trim_test inside_chart_cache_test outside_chart_cache_test \
  chart_spr_search_test chart_spr_phase10_test dagutil \
  wric_counter_baseline_compile_check
```

After a token-preserving cleanup of accidental whole-file formatter churn,
`git clang-format --diff HEAD` and `git diff --check` were clean. The final
focused command covered the recurrence, trim, inside/outside cache, search,
Phase-10 counter surface, semantic report, and CLI local-commit report:

```text
ctest --test-dir build --output-on-failure \
  -R '^(parsimony_chart_test|chart_trim_test|inside_chart_cache_test|outside_chart_cache_test|chart_spr_search_test|chart_spr_phase10_test|chart_parallel_test|chart_spr_semantic_report_test|dagutil_chart_spr_search_local_commit_report)$'
```

It passed 9/9 in 37.17 seconds. The copied log is
`build/wric-chart-parallelization/phase2-wave3-20260714-a6d3c02/focused-wave3.LastTest.log`;
its SHA-256 is
`b2c632f66de667768fb39d40e03dc95d87c0f4916ddc51c535d135de1c8da6df`.

The complete RelWithDebInfo suite then passed all 157 registered tests in
124.49 seconds with zero failures. The two established optional external-data
diagnostics, `merge_consistency_test` and `rotaA_diagnostic_test`, retained
their skip status. The copied full `LastTest.log` SHA-256 is
`8763573fd2c52eeac5e393a78f5359a5f0ab65ddeb2db6002e8429bebc7440d1`.

A strict post-checkpoint audit keeps Phase 2 open. The public scorer still
returns an owning vector, candidate preparation still constructs fresh
descriptors, and default one-candidate acceptance batches do not retain a
search-lifetime workspace. The real 1,000-score allocation gate, integrated
all-active/lazy one-build relationships, local-provider negative identity
tests, a post-workspace full CTest, targeted ASAN, and every Phase-0-relative
allocation/timing/RSS/canonical gate remain pending. No busy-host timing is
acceptance evidence.

### Phase 2 — caller-owned in-place scoring checkpoint

Checkpoint `0c4623b` (`Reuse chart SPR scoring storage in place`) closes the
Phase-2 implementation work. The worktree was clean immediately after the
commit. It adds a caller-owned `score_candidates_locally_into` boundary and a
search-lifetime acceptance workspace, then retains high-water storage for
candidate payloads, overlay deltas, compiled candidate rows, reachability and
affected queues, result rows, and per-worker recurrence scratch. Resident
all-active and lazy scoring keep one full descriptor per effective worker,
not per candidate; pattern-batch scoring keeps one prepared descriptor per
candidate because those descriptors must survive all pattern batches. This
keeps the reuse optimization bounded by the selected parallel width on the
resident-cache paths.

The in-place delta builder is fail-closed. A failed rebuild invalidates every
row-map validity surface before the object can be consumed again. Candidate
copy reuse preserves nested clade, production, witness, child-witness, and
optional topology-provenance capacity across rich--plain--shallow--identity
cycles. Production partition validation, index reset, reachability, affected
ordering, and candidate-row compilation reuse caller-owned buffers. Rank-3
and Option-C callers use the same in-place delta scratch rather than creating
an owning intermediate.

The owning compatibility API remains available, but ownership promotion is
outside the allocation-sensitive `_into` region. Its elapsed time is added
back to both aggregate and per-candidate timing, preserving the old timing
scope. Parallel submission accounting advances only after a successful
non-empty submission. Submission failure and worker failure both join every
accepted future before unwinding; in particular, pattern-batch cache entries
cannot die while a worker still borrows them. Deterministic test barriers make
those exceptional paths non-racy and prove clean recovery. Boundary tests also
cover the checked multisite arithmetic and its labelled `string_view`
diagnostics.

#### Same-profiler allocation gate

The frozen reference is under
`build/wric-chart-parallelization/phase2-allocation-reference-7ddb1fca-diagnostic/`.
`tools/wric_dhat_allocation_reference.py` validates the exact command, binary,
fixture, product counters, DHAT schema, scorer frames, allocation-owner lines,
and exclusions. The immutable identities are:

| Item | SHA-256 |
|---|---|
| Frozen allocation-reference executable | `7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c` |
| Frozen fixture | `e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6` |
| Frozen semantic sidecar | `b8d55c73220a7025b8d977ac4bb083f1e4e76dee5bf007e8754b1f79d42b8442` |
| Frozen ordered candidate signature | `6fcd6f69962abb67e91a3236c5043787c86c33bbd4c135fa67c37b53a6e37168` |
| Frozen local-score tuple digest | `86f0f046744ba8dfe2d6f33668f9c47a3e0c56646bb58fce9ada71a5bb18ead7` |

Three frozen Valgrind 3.27.0 DHAT runs were exactly identical by allocation
owner and count. The scorer stack made 578,849 allocation calls for 64
candidates. The frozen region contract excludes 64 output-vector calls, 1,696
prepared-candidate ownership copies, and 1,696 returned-candidate ownership
copies. The included reference is therefore 575,393 calls, or
8,990,515.625 calls per 1,000 candidates.
The frozen parser's deterministic report is copied beside the optimized
profile as `frozen-reference.json`; its SHA-256 is
`d54bc1fc4879c1c97321dd582cbfc2773c2b0343a672d093bc7706a1074c0522`.

The optimized same-command profile is under
`build/wric-chart-parallelization/phase2-wave4-20260714-0c4623b/current-dhat-exact/`.
Its copied executable reports `dagutil 2.2.0 (0c4623b)` and has SHA-256
`e729f5c59d98298694460487839abac13f9e36639bb272fa798a29bd61804998`.
The DHAT file SHA-256 is
`7c12a62dfff020fd4399aa41abb8b9b169331ff735b3f0a4a67ef38c435a1854`.
`tools/wric_dhat_scoring_region.py` independently validates that file's v2
schema, hashes, exact command, product counters, complete scorer symbol and
source lines, unique scorer-frame occurrence, expected allocation totals, and
widened-integer threshold. The analyzer hard-pins the frozen report, frozen
binary, frozen/current fixture identity, three-run consensus, and required 80%
threshold; its denominator is not a caller-supplied number. Its deterministic
`current-region.json` has SHA-256
`887029bc8dd82d028886424882494075fcd2a6c2f7a3f09ba901cadc2461b05d`.
The optimized process made 67,780 allocation calls in total, of which exactly
338 calls at 41 allocation points had a
`score_candidates_locally_into` frame. The `_into` boundary structurally owns
neither output results nor promoted candidates, so all 338 are included cold
high-water growth. That is 5,281.25 calls per 1,000 candidates and a
99.941257540% same-profiler reduction. The analyzer's non-normalized,
widened-integer 80% test is
`338 * 64 * 100 <= 575393 * 64 * 20`, or
`2,163,200 <= 736,503,040`. Even treating every
allocation in the optimized process as scorer work gives an 88.220225133%
conservative reduction. The optimized protobuf SHA-256 is
`e51855d97a671a6d1af85f4e32c742939ab2a28e641a1cb74eca08ee5265fe1b`,
byte-identical to all three frozen-reference outputs.

The executable-local observer adds a stronger steady-state regression gate.
After one complete unobserved high-water pass, each of three observed
1,000-candidate passes makes exactly zero calls through any replaceable C++
allocation form. Each pass still executes the frozen non-vacuity workload:
1,610,815 local row visits, 1,497,815 unit-Fitch production visits, 12,239
candidate partition validations, 13,255 production descriptors, 139,000 base
clade and 69,000 base production reachability visits, 11,239 temporary-clade
and 12,239 temporary-production visits, and 1,000 full-grammar-like
reachability passes. Separate observed rich-shape delta-build and acceptance
candidate-copy cycles are also allocation-free. All scores match the 64-row
frozen tuple oracle and its digest; wrapper equivalence is only a secondary
check, not the oracle.

#### Correctness and sanitizer evidence

The post-commit focused command ran `chart_trim_test`,
`chart_spr_search_test`, and `chart_spr_allocation_test`. It passed 3/3 in
5.50 seconds. Its copied `LastTest.log` SHA-256 is
`acdf80a415cd15656240f12a95f9671772c624f4b3afd75570c67b43a92fd6e2`.
The complete RelWithDebInfo suite then passed all 157 registered tests in
206.79 seconds. The established optional `merge_consistency_test` and
`rotaA_diagnostic_test` skips were unchanged. The full log SHA-256 is
`f4af3a6333b5906cc8597d0c2126d1ea2338c4ff1d731f1bfdaffa89e5455312`.
The analyzer's deterministic in-tree adversarial self-test is registered as
`wric_dhat_scoring_region_test`. It covers the valid contract and rejects
schema, command, counter, every input hash, frozen-report hash, duplicate,
foreign-source, truncated-frame, exact-symbol, source-line, three expected
total, malformed-consensus, and failed-threshold mutations.
A post-analyzer parallel rerun passed all 158 registered tests in 86.60
seconds, with only the same two established optional skips. Its
`full-evidence.LastTest.log` SHA-256 is
`60947017f40f03b4eb34c8901ba680b863a93308a26bea923ff0e65f150af499`.

The targeted ASAN build used the required GCC trunk, C++26,
`RelWithDebInfo`, `ENABLE_ASAN=ON`, and `ENABLE_TSAN=OFF`. The host loader's
default GCC-15 `libasan.so.8` lacked a GCC-trunk sanitizer symbol; the preserved
minimal loader failure has SHA-256
`7afbdb213492ee32e8a60fe248a2136a8387accc171ad2bab0e43624b6a9b95d`.
Rerunning with
`LD_LIBRARY_PATH=/home/ogi-agent/install/gcc-trunk/lib64` selected the matching
GCC-trunk runtime. `chart_trim_test`, `chart_spr_test`,
`chart_spr_search_test`, `rank3_rewrite_test`, and
`chart_spr_allocation_test` then passed 5/5 in 40.36 seconds with
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1`; that log's SHA-256 is
`b03c281e3e540c2b14de205954e4c700bc059723bc184a90f1c8354462972d8d`.
`option_c_test` and `option_c_chain_commit_test` passed 2/2 in 0.08 seconds
under the same runtime and options; that log's SHA-256 is
`435186c4410732f3e3acc051f179fb7ab80821836b59da57c8f0159f7c05b3ed`.
There were no address or leak diagnostics.

| Phase-2 exit criterion | Decision |
|---|---|
| At least 80% fewer scoped allocations per 1,000 candidates | pass: 99.941257540% same-profiler reduction; warmed result is zero |
| One unchanged active-pattern inside build per committed state | pass: disjoint build/reuse counters and one-owner assertions cover all-active, lazy, and pattern-batch modes |
| Medium one-worker local scoring at least 30% faster than Phase 0 and no more than 5% slower than Phase 1 | sealed base available; timing run pending |
| Small one-candidate exact at least 25% faster than Phase 1 | sealed base available; timing run pending |
| Peak RSS no more than 25% above every reference | sealed base available; paired RSS run pending; per-worker resident descriptor ownership is mechanically bounded |
| Canonical output | in-tree frozen tuple/digest and byte-identical profiled output pass; sealed-base comparison run pending |
| Full CTest and targeted ASAN | pass |

Phase 2 is therefore **implementation complete; performance acceptance
pending**. No busy-host wall time or RSS observation in this section is
acceptance evidence; the sealed comparison base is now available for the
outstanding runs.

## Phase 3 — persistent adaptive chart scheduler

Checkpoint `7d294d6` (`Add persistent adaptive chart scheduler`) closes the
Phase-3 implementation work. It adds one lazily started scheduler whose
lifetime spans the whole chart search, threads it through every local-scoring
acceptance batch, and shuts it down synchronously before publishing its final
metrics. Explicit requests `1,2,4,8,16` are exact. Automatic resolution uses
the process affinity's physical-core count when every CPU topology record is
available, then affinity logical CPUs, `hardware_concurrency()`, and finally a
serial fallback. The selected policy and topology inputs are reported.

The indexed-range primitive uses monotonic operation/task IDs, bounded dynamic
ranges, stable scratch slots, adaptive grain, and fixed-order coordinator
reduction. Every accepted runner completes its stable first range; cancellation
stops later claims, joins all launched runners, and suppresses partial
map-reduce publication. Exceptions are selected by lowest stable range. Nested
use of the same scheduler executes serially in the inherited scratch slot,
while unrelated concurrent top-level use is rejected. Pool start/stop, live
threads, pending work, queue waits, range/task accounting, active-worker high
water, grain, and serial-fallback reasons are all observable. Direct explicit
W1 scoring retains the Phase-2 allocation-free seam and does not construct or
type-erase a scheduler.

The standalone scheduler suite covers explicit and automatic resolution;
empty, one-item, and fewer-items-than-workers calls; exact-once non-divisible
ranges; stable move-only reduction despite out-of-order completion; repeated
operations on one lazy pool; deterministic exception selection; cooperative
cancellation and recovery; no partial reduction; nested fallback; concurrent
top-level rejection; partial submission failure; shutdown; and zero pending or
live threads. Fifty consecutive standalone repetitions passed, as did a
warning-clean GCC-trunk standalone build. Integration tests prove one pool
lifetime over multiple search iterations, byte-identical W1/W8 semantics,
actual parallel operations above the threshold, canonical CLI identity for
requests `1,2,4,8,16,0`, and exact scheduler accounting in the emitted report.

The complete RelWithDebInfo suite passed all 159 registered tests in 84.28
seconds, with only the established optional external-data skips
`merge_consistency_test` and `rotaA_diagnostic_test`. The preserved log is
`build/wric-chart-parallelization/phase3-7d294d6/full-relwithdebinfo.LastTest.log`;
its SHA-256 is
`a1cf5f9157c073930bb2b66464654436a33b3a94064a03e58d0e393d695baa2e`.

The GCC-trunk TSan build initially selected an incompatible system runtime;
pinning `/home/ogi-agent/install/gcc-trunk/lib64` exposed a separate musl 1.2.6
runtime abort before threaded tests could execute. Its assertion and source
line exactly matched LLVM compiler-rt change
`b917156f9bdf0b7f9bb88e056da32409f5d71630` (`[TSan] Fix determining static
TLS blocks`,
https://chromium.googlesource.com/external/github.com/llvm/llvm-project/compiler-rt/+/b917156f9bdf0b7f9bb88e056da32409f5d71630).
Applying that runtime-only inclusive-boundary fix exposed one
additional executable layout: debugger inspection showed a 16-byte gap between
an align-64 libtsan TLS block and the align-8 executable TLS block. The private
validation runtime therefore uses the upstream inclusive comparison and the
larger adjacent alignment when identifying contiguous static TLS blocks. This
changes neither larch2 code nor compiler instrumentation. The external GCC
source tree was restored clean immediately after copying the runtime.

The exact runtime patch, source identity, loader order, and diagnosis are under
`build/wric-chart-parallelization/phase3-7d294d6/patched-tsan-runtime/`.
`libtsan.so.2.0.0` has SHA-256
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.
With that runtime first in `LD_LIBRARY_PATH`, the serial targeted TSan set
passed 52/52 in 296.65 seconds. It includes the scheduler, thread pool, local
scorer/search, exact trim, persistent inside/outside caches, fixed-topology and
multifurcation oracles, overlay chain, CLI search/trim paths, and the long cache
tests. The copied `targeted-tsan.LastTest.log` SHA-256 is
`a181bb59a227edadaa75b025137b360f63a7cd2d6a6f81530fc94413ff8ee242`.
There were no TSan race reports.

| Phase-3 exit criterion | Decision |
|---|---|
| One pool lifetime across multiple iterations | pass |
| Explicit W1 matches the Phase-2 canonical oracle | pass; direct and CLI worker matrices are byte-identical |
| Empty/small/repeated/exception/cancellation coverage | pass |
| All submitted work joined; zero pending/live work at shutdown | pass |
| Small 64-candidate W8 no more than 5% slower than W1 | sealed base available; timing run pending |
| Non-vacuous parallel execution above threshold | pass |
| Targeted TSan and full CTest | pass: 52/52 and 159/159 |

Phase 3 is therefore **implementation complete; performance acceptance
pending**. No busy-host timing observation is accepted as evidence for the
outstanding small-case wall-time gate; the sealed comparison base is now
available for that run.

## Phase 4 — pattern construction and local scoring

Checkpoint `cbf92b6` (`Parallelize chart pattern and local scoring`) closes
the Phase-4 implementation work under the deadline-overlap rule. It routes
initial-chart construction, active-pattern exact setup, resident inside and
outside cache construction, fixed-topology direct scoring, coarse candidate
scoring, and narrow candidate-by-pattern tiles through the one search-lifetime
scheduler. Pattern work writes pre-sized indexed slots; candidate work uses
stable result slots, affected-set-weighted ordering, bounded dynamic ranges,
and coordinator-ordered integer reduction. One/few-candidate batches tile the
pattern axis, while pattern-batch mode fuses one bounded cold-cache build with
scoring instead of retaining or rebuilding an unbounded cache.

Worker failures are captured per stable range, all launched work is joined,
and the lowest stable failure is rethrown only after the operation boundary is
clean. Scheduler-infrastructure failures retain their distinct taxonomy and
the same scheduler is exercised successfully after caught worker and
submission failures. Per-axis operation, item, range, task, grain, parallel
operation, and active-worker-high-water counters reconcile exactly with the
global scheduler report; shutdown reports zero pending tasks and zero live
pool threads.

The local-commit admission check now covers persistent inside/outside storage,
capacity rather than size for allocator-owned vectors, lazy class maps and
weights, the old and conservatively projected new lazy state, pattern-cache
entry objects as well as rows, selected-topology cache admission, and the
maximum frozen-plus-temporary dense clade surface. Overflow and inconsistency
fail before mutation with the hard local-commit taxonomy. Selected-topology
state is bounded to one candidate. A regression fixture also exercises a
previously unreachable frozen clade becoming reachable after append/rebase;
newly reachable clades are unioned into the affected set and recomputed in
bottom-up/top-down dependency order before comparison with a cold oracle.

The new strict postprocessor, `tools/wric_phase4_acceptance.py`, consumes raw
trials rather than summary rows. It rejects incomplete or overlapping worker
matrices, identity/hash mismatches, malformed or duplicated report fields,
nonzero swap, unresolved scheduler work, missing parallel high-water evidence,
and disagreement between global and semantic-axis accounting. It enforces the
local and construction W8/W1 limits of 0.50, both W8/W4 limits of 1.10, the
Phase-3 W1 limit of 1.05, system/user CPU profiling threshold of 0.25, and both
the relative and global RSS limits. Deferring the Phase-3 comparison must be
explicit. Its deterministic adversarial suite passes 26/26, including exact
threshold boundaries and just-over-threshold failures for every timing ratio.

### Correctness and sanitizer evidence

After formatting only the lines introduced in this checkpoint, the focused
command

```text
ctest --test-dir build --output-on-failure -j 4 \
  -R '^(chart_parallel_test|chart_trim_test|chart_spr_search_test|inside_chart_cache_test|outside_chart_cache_test|wric_phase4_acceptance_test)$'
```

passed 6/6 in 19.33 seconds. The finalized complete RelWithDebInfo command
`ctest --test-dir build --output-on-failure -j 8` passed all 160 registered
tests in 95.19 seconds. The established optional `merge_consistency_test` and
`rotaA_diagnostic_test` skips were unchanged. The preserved log is
`build/wric-chart-parallelization/phase4-cbf92b6/full-relwithdebinfo.LastTest.log`;
its SHA-256 is
`3241e83706e73ec87800e31fa3588b19f9543fc4ce1c592fb4c29708256efe59`.

The targeted GCC-trunk TSan build exercised `chart_parallel_test`,
`chart_spr_search_test`, `inside_chart_cache_test`, and
`outside_chart_cache_test` separately with

```text
LD_LIBRARY_PATH="$PWD/build/wric-chart-parallelization/phase4-tsan-runtime:/home/ogi-agent/install/gcc-trunk/lib64" \
TSAN_OPTIONS=halt_on_error=1 \
./build-tsan-phase3-7d294d6/<test>
```

All four exited zero with no TSan diagnostic, including deterministic real
worker overlap, local-commit cache barriers, failure joining and recovery, and
the long inside/outside cache chains. The build-local runtime link selects the
preserved Phase-3 validation runtime whose SHA-256 remains
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`;
the default system runtime was again rejected because it lacks the required
GCC-trunk symbol. The subsequent checkpoint changes were formatter-only C++
whitespace plus the Python acceptance-gate correction; the normal binaries
were rebuilt from the final checkpoint tree.

A non-acceptance W1/W4 CLI smoke under
`build/phase4-report-smoke.ax9Nz9/` produced byte-identical canonical JSON
(SHA-256
`0506f22bd4190c647af96c6ea90cc1f9e91045a6c4de61f6e1b8b41b3c59c4c7`).
The W4 report resolved four workers, reached an active-worker high water of
four, recorded two genuinely parallel operations, and reconciled 30 ranges
and eight submitted/completed/joined tasks across the initial-chart and
candidate-pattern axes. Pending and live counts were zero at shutdown. These
busy-host observations prove path activation only and are not performance
acceptance evidence.

| Phase-4 exit criterion | Decision |
|---|---|
| W1/W2/W4/W8 canonical matrix for seeds 1, 7, and 19 across dense, fixed-topology, and grammar-exact modes | sealed base available; capture run pending; in-tree serial/parallel oracle, W1/W4 pattern-axis, W1/W8 multi-accept, and CLI identity tests pass |
| Medium 64-candidate local W8/W1 at most 0.50 and Phase-4 W1/Phase-3 W1 at most 1.05 | sealed base available; timing run pending |
| Medium construction W8/W1 at most 0.50 | sealed base available; timing run pending |
| Local and construction W8/W4 at most 1.10; CPU contention investigated above 0.25 | sealed base available; timing run pending; strict postprocessor and boundary tests pass |
| W8 peak RSS within the global final bound | sealed base available; paired RSS run pending; fail-closed admission and postprocessor gates pass |
| Full CTest and targeted TSan | pass |

Phase 4 is therefore **implementation complete; performance acceptance
pending**. No timing or RSS value in this section satisfies an outstanding
performance gate; the sealed comparison base is now available for those runs.

## Phase 5 profile-guided checkpoint (diagnostic, acceptance pending Phase 0)

This checkpoint was measured from the dirty Phase-5 tree after the dependency
wavefront and proven-single-topology setup path were added, but before the
measured-zero heavy-product implementation was removed. It is diagnostic
evidence for the profile decision, not final Phase-5 or Phase-0 acceptance.
The frozen binaries, workload, candidate/exact limits, validation, and
`Baseline measure` checkpoint `7ca527b8906d018124756182274335cbff936d72`
were not changed.

The initial frozen-medium diagnostic at
`build/wric-chart-parallelization/phase5-medium-exact1-w8/` completed the
one-candidate exact search in 0.900777 seconds at W8, versus 118.227425
seconds for its paired frozen native sample--explore--merge run. Its exact
frontier performed 1,192 product combinations over 138 dependency levels:
the ordinary clade wavefront took 32.740 ms at W8 versus 96.815 ms in the W1
semantic companion (2.96x), while the heavy-product axis performed exactly
zero operations, ranges, tasks, combinations, and milliseconds. Heavy
splitting therefore represented 0% of the post-wavefront profile. Under the
Phase-5 conditional-action rule, it must be omitted rather than retained as
ineffective complexity.

Callgrind output at
`build/wric-chart-parallelization/phase5-callgrind-w1.out` (SHA-256
`5ea2f5cca0566efda1fba991e52183dee06c071440b5978e565c1f7736ed4dd5`)
then identified deterministic per-pattern traceback and recursive selected-
topology scoring as the dominant exact-setup work. The medium structure has
597 taxa, 1,193 clades, and 596 productions. Every reachable internal clade
has exactly one production, but each setup had generated 1,107 topology
candidates and deduplicated them to one before rescoring that sole feasible
topology across all 1,106 active patterns. The replacement proves uniqueness
by a root-reachable structural traversal; only in that exact case the
per-pattern chart lower-bound sum is itself feasible, so it is also the upper
bound. General multi-topology trace, deduplication, and scoring remain the
oracle path.

Three alternating W1/W8 runs used the unchanged seedtree one-candidate,
top-K-one exact command, taskset `0,2,4,6,8,10,12,14`, the frozen process
metrics runner, validation, and a 12 GiB chart-memory option. Raw stems are
`build/wric-chart-parallelization/phase5-unique-fast-w{1,8}-*`; every process
exited zero, timed out zero times, and sampled zero swap. The median results
were:

| Metric | W1 median | W8 median | W1/W8 |
|---|---:|---:|---:|
| exact initialization | 143.601 ms | 34.071 ms | 4.22x |
| exact verification | 179.173 ms | 41.995 ms | 4.27x |
| exact B&B frontier | 83.170 ms | 27.795 ms | 2.99x |
| whole-process wall | 0.575261 s | 0.272354 s | 2.11x |
| user CPU | 0.482216 s | 0.533513 s | -- |
| system CPU | 0.090317 s | 0.122608 s | -- |
| maximum RSS | 86,708 KiB | 86,628 KiB | -- |

The W8 median system/user ratio is 22.98%, below the plan's mandatory 25%
contention-investigation threshold and far below the pre-change W8 ratios.
Each exact setup now reports one scheduler operation and one structurally
established feasible topology; across current and candidate trims the search
report therefore records two setup operations and two topology candidates.
Focused correctness, full-suite, sanitizer, final same-revision measurement,
and RSS gates remain pending on the post-omission tree.

## Phase 5 post-omission checkpoint (functional pass; timing diagnostic)

Checkpoint `bb29300` (`Parallelize exact chart B&B wavefronts`) is the
post-omission product implementation. Commit `3a10e9c` fixes the benchmark
reader exposed by per-level diagnostics: `extract_value` now selects its first
match in one `sed` process, rather than allowing a multi-match producer to
receive SIGPIPE through `head` under `pipefail`. The product binary is
unchanged by that follow-up. Frozen Phase-0 executables, workloads, validation,
candidate/exact budgets, and the `Baseline measure` checkpoint remain
unchanged.

The exact path now uses dependency-level clade wavefronts, stable per-clade
slots, coordinator-ordered folding, delayed mask/provenance materialization,
and deterministic equality merging. Exact setup consumes the existing
pattern-parallel builder. A root-reachable structural proof selects the
single-topology setup fast path only when every reachable internal clade has
exactly one valid binary production. Primary exact trims retain canonical root
provenance directly, so semantic capture no longer launches a companion B&B.
Arithmetic overflow, scheduler failures, allocation failures, provenance
capture failures, and invariant failures remain hard errors through cold,
transient, and accepted-state rebuild boundaries.

The focused post-omission command passed 11/11. It covers the W1/W8 matrix for
every dominance mode, bound pruning on/off, and UA scoring on/off; dependency
publication, real overlap, partial submission, full join, stable failure, and
scheduler reuse; unique-topology and deterministic traceback oracles;
cold/transient overflow propagation; canonical capture; dense/lazy standalone
contracts; and exact-counter/report reconciliation. Its log is
`build/wric-chart-parallelization/phase5-3a10e9c/focused.ctest.log`, SHA-256
`074b100cc78e5ca76a467ddaf0ac9cdd8bb950e40e7b01cce166fe4a0e64619b`.

The complete RelWithDebInfo rerun passed all 161 registered tests in 69.43
seconds, with only the established optional `merge_consistency_test` and
`rotaA_diagnostic_test` skips. The log SHA-256 is
`6b1f6963fbd2ae7f8c64c8055fb4979235842e1e7ef56e5f194e191b174318c0`.
The first full run found the benchmark-reader SIGPIPE above; its failure was
reproduced in isolation, corrected without changing the product, and the full
suite was rerun from the beginning.

The GCC-trunk targeted TSan build ran `chart_scheduler_test`,
`chart_parallel_test`, `chart_trim_test`, and `chart_spr_search_test` serially
under `TSAN_OPTIONS=halt_on_error=1`. All 4/4 passed with no race report. The
preserved runtime SHA-256 remains
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`;
the test-log SHA-256 is
`7a88ec8d79833ab3d8dbc538845e4faed08e4fe54faa82eef2ef7e269c204038`.

An untimed explicit-W1/explicit-W8 semantic run produced byte-identical digest
JSON, full NDJSON, output protobuf validation, and external canonical-DAG JSON.
The common semantic SHA-256 is
`efa7b4541180d7b0242392045834c72e650b5fcfba00671f5a0c95ae0a209257`;
the exact-evidence SHA-256 is
`a5772604932e235ed71fc40b5dfb6798518f77ffc0b4078edd4daee975194fc6`;
the final-topology SHA-256 is
`0b7ac7731066e2c6f6150207a4ce763257638f6be438b43bc45b9e4cbf7ad5ec`;
and the external canonical-DAG file SHA-256 is
`38659ef32cd697484fedc7c05993abb23dd0a608449bb164cbc943cc97e3e9d9`.
This comparison includes exact keep masks, frontier sizes, optimal-root
provenance classes, retained topology identity, candidate records, and the
accepted/final topology. All six timed outputs independently validate to the
same external canonical DAG.

Three alternating W1/W8 measurements used the frozen process runner, physical
CPU list `0,2,4,6,8,10,12,14`, seedtree, validation, one grammar candidate,
top-K one, exact acceptance, and the 12 GiB chart-memory option. Before/after
hash manifests are byte-identical. Every process exited zero, respected the
600-second W1 and 180-second W8 limits, sampled zero swap, remained below the
global RSS limit, and reported 1,192 logical products over 138 level waves,
2,386 clade visits, and 2,386 frontier entries. W8 reached eight active workers
on both retained exact axes; all submitted/completed/joined and
created/completed counts reconcile, with no cancellation or pending/live work.
Removed heavy/product/scratch report fields are absent.

| Metric | W1 median/max | W8 median/max | W1/W8 or W8/W1 |
|---|---:|---:|---:|
| exact initialization median | 150.474 ms | 43.637 ms | 3.45x |
| exact B&B median | 91.280 ms | 32.307 ms | 2.83x |
| exact verification median | 189.792 ms | 50.077 ms | 3.79x |
| whole-process wall median | 0.615352 s | 0.316089 s | 1.95x |
| peak sampled RSS max | 85,276 KiB | 83,660 KiB | 0.981x W8/W1 |

The W8 median system/user ratio is 24.84%, below the 25% investigation
threshold despite one unrelated busy host core. Raw evidence and the computed
summary are under
`build/wric-chart-parallelization/phase5-3a10e9c/`; the summary SHA-256 is
`b34119b0bc26b393759b9bbd5031a5bfc18eba54b97bad4efdd0079a1cc7f34c`.
These timing/RSS observations are diagnostic only until Phase 0 is calibrated,
captured, finalized, audited, and sealed.

Phase 5 therefore has a **functionally passing implementation checkpoint**,
but is not accepted. At this checkpoint Phase 6 still had to close the global
bounded-memory invariant, and Phase 0 still had to supply sealed timeout,
scaling, and RSS evidence. Phase 6 has since implemented that invariant and
the Phase-0 base is now sealed; the final timing and RSS decisions remain
pending.

## Phase 6 implementation checkpoint (unsealed)

Phase 6 is **implementation complete; performance acceptance pending** at
`870c298ff1c0c21901bdf79d341bf97d121f389c` (tree
`e98984f4f587546e5ef19ba08e2f1b1a4e8ce6b1`). The immutable measurement
checkpoint remains `7ca527b8906d018124756182274335cbff936d72`
(`Baseline measure`). No frozen Phase-0 executable, workload, runner, oracle,
or search contract was replaced.

The implementation publishes one immutable old exact trim, admits retained
candidates in deterministic stable-rank waves, gives each task private
verifier counters/scratch/results, forbids nested-pool execution, and merges
failures, counters, canonical evidence, and winner selection serially after
each join. Its unified finite budget uses the tighter nonzero state/iteration
value and charges resident charts and exact trim, coordinator/live-input
storage, selector and estimator work, task-local exact setup/frontier or
fixed-topology scratch, retained results across waves, and actual-capacity
backstops. Unsafe or overflowing estimates and custom finite-budget callbacks
without matching memory estimators fail closed.

The standalone `chart_spr_search_test` exercises Top-K `{1,4,16}` by workers
`{1,2,4,8}` across dense cold/unambiguous two-pass conservative, forced-lazy
transient exact-local, and pattern-batch fixed exact-local paths. It compares
non-vacuous per-candidate exact scores/evidence, exceptions, masks, fluidity
provenance, winner, canonical digest, and full canonical sidecar to the W1
oracle; it also repeats W8 and compares both canonical byte representations.
Stable-rank failure selection, partial-submission joining/quiescence,
candidate-versus-inner-axis choice, finite-budget Top-K-16 wave splitting,
pre-verifier rejection, custom-callback serialization, and checked admission
arithmetic have dedicated passing cases. The standalone log has SHA-256
`f785c49a6afb8095f699f3fb3248a9c7cd11763d9e676b6c65ff0ab2023c7313`.

The strict benchmark postprocessor now emits
`phase6_exact_verification_speedup.tsv`, `phase6_admission_evidence.tsv`, and
`phase6_rss_comparisons.tsv`; binds rows to workload, contract, search, and
output-semantic hashes; uses exact fixed-point median arithmetic; requires a
non-vacuous W1/W8 pair and Top-K-16 cardinality; and fails closed for timeout,
fallback, nonfinite, incomplete, or boundary-failing input. Its adversarial
shell regression and the Phase-0 bootstrap regression pass. This proves the
evidence contract, not the still-unrecorded performance, RSS, or real-workload
admission result.

### Reproducible functional evidence

Raw artifacts are uncommitted under
`build/wric-chart-parallelization/phase6-870c298/`. The normal build uses
RelWithDebInfo `-O2 -g -DNDEBUG`, GCC trunk
`g++-trunk (GCC) 17.0.0 20260530 (experimental)`, and C++26 reflection. The
TSan build uses the same configuration plus `-fsanitize=thread`; its runtime
is the previously documented TLS-boundary-patched `libtsan.so.2.0.0`, SHA-256
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.

| Check | Exact command/result | Artifact SHA-256 |
|---|---|---|
| Focused RelWithDebInfo | `ctest --test-dir build --output-on-failure --parallel 4 -R '^(chart_scheduler_test|chart_parallel_test|chart_trim_test|chart_spr_search_test)$'`; 4/4 passed in 6.83 s | `475624e63b57748585e8f6f5f7c312bc3189bcbb47121470cc6bc7964bdab07a` |
| Full RelWithDebInfo | `ctest --test-dir build --output-on-failure --parallel 8`; zero failures out of 161 in 176.96 s (159 passed and the two established optional tests skipped) | `b03850b288cd5e9237e38767bb3b1c5235f5190dcdf0cd31b8c7970be678e42b` |
| Phase-0/bootstrap and adversarial benchmark harness | registered shell tests; both passed | `2a6f9fceba0a38cbdfa36474e8d03084d2211d7b044984bdbae38f9e8c57f84e` |
| Targeted TSan | `LD_LIBRARY_PATH=.../patched-tsan-runtime:.../gcc-trunk/lib64 TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan-phase3-7d294d6 --output-on-failure --parallel 1 -R '^(chart_scheduler_test|chart_parallel_test|chart_trim_test|chart_spr_search_test)$'`; 4/4 passed in 32.33 s without a TSan report | `0f17116d44b07e4963a60483899621289da21d025a91de6d80cf1e050cf2ae7f` |

The frozen inputs still hash as follows: runner
`3dbf846e6aa05b007160d9b8d149a82f8e26c4e8099a4e4fc39fdbc63ab02104`,
oracle
`7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10c6622581f21a5a38c`,
seedtree
`2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153`,
and reference sequence
`088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c`.

### Phase-6 exit decisions

| Phase-6 exit criterion | Decision |
|---|---|
| Top-K `1,4,16` exact semantics and stable exceptions at W1/W2/W4/W8 | pass in the independent in-tree matrix; sealed frozen-workload capture run remains pending |
| Medium Top-K-4 W8 `exact_verification_ms` at least 2.0x faster than same-revision W1 | sealed base available; timing run pending |
| Real Top-K-16 stays within configured concurrent-memory admission bound | sealed real-workload contract available; bounded confirmation pending; synthetic/fixture admission, splitting, rejection, and estimator-backstop contracts pass |
| W8 peak RSS no more than 2.0x W1 | sealed base available; paired RSS run pending |
| Repeated W8 results byte-identical | pass in-tree for digest and full canonical sidecar; frozen-workload repetition remains pending |
| Exact-search TSan and full CTest | pass at the code checkpoint; exact hashes are retained in the artifact manifest |

Quiet-host Phase-0 calibration, capture, finalization, audit, and detached seal
have since completed. No same-revision diagnostic, unit fixture, or
postprocessor regression is substituted for the still-pending Phase-6
performance gates.

## Phase 7 packed-grouping checkpoint (unsealed; implementation in progress)

The first Phase-7 product checkpoint is `c08602b` (`Use packed keys for lazy
selected topology grouping`). The immutable measurement checkpoint remains
`7ca527b8906d018124756182274335cbff936d72` (`Baseline measure`), and no frozen
Phase-0 executable, workload, runner, oracle, candidate budget, or exactness
contract changed.

The checkpoint replaces every production
`std::map<std::vector<std::size_t>, ...>` lazy grouping site with checked,
pattern-major packed keys and deterministic grouping. This covers plan and
grammar inside construction, grammar and plan outside contexts, lazy
candidate-local overlay scoring, and the fixed-topology selected-tree scorer.
The primitive preserves first-occurrence class IDs and representatives,
input-order CSR members, and an explicit lexicographic class traversal where
the removed ordered map made traversal observable. Checked narrowing,
overflow, reusable-capacity accounting, empty input, collision, multifurcation,
partial-failure, and independent ordered-map oracle cases are exercised
directly. Test-only ordered maps remain as semantic oracles; production code
has no remaining vector-key map grouping.

The associated commits are `cb35587` (primitive), `b6193cd` (prepared
admission/status hardening), `18d51da` (candidate-local overlay grouping),
`533d045` (plan inside grouping), `eb1c27e` (grammar inside grouping),
`f1ff0bb` (outside grouping), and `c08602b` (fixed-topology grouping). Commit
`50e6e50` adds the two deterministic Phase-7 fixtures and their frozen-oracle
regenerator. `tools/regen_wric_lazy_fixtures.py --dagutil
build/wric-chart-parallelization/baseline-408434e/bin/dagutil --check` passed;
the high-compression PB/ref SHA-256 values are
`e103be6cd1df36e5a002ae9dd9ffb53110b35c840eed1d8fa867475ad3109874` and
`86f9d532555a1cf709ea3a9a3efe7c72aa9cd6c6a01bd7aa2614636a846a9128`;
the dense-favoring values are
`e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6` and
`b16c732ac5692f8644afc19f0c454422c381e9d88eff5dacb40ac96a603f8404`.
These hashes remain pinned by the committed regenerator and were subsequently
bound into the sealed `phase7-lazy.tsv` and `phase7-lazy-completion.tsv`
manifests recorded in the current status table.

At `c08602b`, the GCC-trunk RelWithDebInfo command
`ctest --test-dir build --output-on-failure -R
'^(chart_parallel_test|chart_spr_search_test|multifurcation_chart_oracle_test|lazy_key_grouping_test)$'`
passed 4/4 tests in 7.80 seconds. The combined suite covers the direct packed
contracts, dense/lazy and materialized per-pattern oracles, multifurcations,
local SPR scoring, and fixed-topology selected-row behavior. `git diff --check`
and changed-line `git clang-format --diff` are clean.

This is not a Phase-7 exit decision. At that packed-grouping checkpoint,
dependency-level lazy inside/outside wavefronts, bounded transient scratch
admission, allocation-free admitted candidate kernels, within-clade staging
where required by scaling, explicit `off|on|auto` policy/reporting, complete
dense/lazy canonical equivalence, ASAN/TSAN/full-CTest, and every sealed
performance/RSS gate remained pending. The next subsection records the first
of those later implementation units without changing the exit decision.

### Dependency-wavefront checkpoint

Commit `a8abc49` (`Schedule lazy chart dependency wavefronts`) replaces the
trusted-plan lazy inside and outside serial traversals used by chart-SPR state
construction with dependency-level operations on the one persistent chart
scheduler. Inside tasks publish disjoint clades from immutable child levels;
outside root initialization remains serial and non-root tasks publish disjoint
clades in top-down levels. Sparse child-map dependency consumption and all
global recurrence/collision counter folds remain coordinator-only after the
complete level joins. State construction releases retained inside grouping
scratch before preparing outside scratch and releases outside scratch after
the build.

The focused semantic matrix covers workers `1,2,4,8`, a repeated W8 run,
at-least-four-worker simultaneous progress, reference-edge outside scoring,
multifurcations, a root-only grammar, stable dual inside and outside failures,
partial scheduler submission followed by same-scheduler retry, and a shared
internal child whose sparse maps remain readable through both same-level
parents and are reclaimed only at the level barrier. Lazy inside/outside clade
axes are included in scheduler reconciliation and the dagutil report.

On the clean worktree at that historical checkpoint, the GCC-trunk
RelWithDebInfo command
`ctest --test-dir build --output-on-failure -R
'^(chart_parallel_test|chart_spr_search_test|multifurcation_chart_oracle_test|lazy_key_grouping_test)$'`
passed 4/4 tests in 7.71 seconds; `dagutil` compiled, `git diff --check` was
clean, and changed-line clang-format reported no edits. This is an unsealed
functional checkpoint, not a timing or Phase-7 exit claim. Finite state-build
transient admission, allocation-free candidate-local admission, within-clade
staging/profile evidence, automatic policy, sanitizer gates, full CTest, RSS,
and all deferred Phase-0-dependent measurements remain pending.

### Phase-7 implementation checkpoint

Phase 7 is **implementation complete; performance acceptance pending** at
`38e9a281396e5263647ba68724414848841525d7` (`Select lazy chart policy
automatically`). The immutable measurement checkpoint remains
`7ca527b8906d018124756182274335cbff936d72` (`Baseline measure`) and is an
ancestor of the implementation tip. The historical counter-baseline document
is byte-identical at both revisions, with SHA-256
`cdb3fd9f936cb335f8f93eafd5abbc4d31822bfe657f4f3fd343c85c1203a82e`.
No baseline executable, runner, oracle, candidate/exact budget, validation, or
search-exactness contract was replaced.

After the packed grouping and dependency wavefront above, commits `1607fd9`,
`be02001`, `3ff306d`, and `a9e74bb` remove successful lazy-pattern validation
allocations and close state-build, candidate-scoring, and unified exact-memory
admission. The builders pre-admit bounded coordinator, task-slot, scheduler,
and old/new publication overlap, reduce concurrency under finite pressure,
and retain measured-capacity backstops. Rejected E-1 boundaries occur before
the corresponding scheduler submission; E succeeds. Temporal phases use a
maximum rather than summing non-overlapping pilot, chart-build, local-score,
and exact work.

Commit `38e9a28` adds deterministic `off|on|auto` selection. The bounded,
midpoint-stratified pilot constructs at most 32 inside-only patterns and uses
integer structural-class, retained-row, estimated-byte, and packed-key-work
comparisons; it does not use wall time or submit scheduler work. Accepted-state
rebuilds carry a private frozen decision, skip repiloting, recompute admission
for the current selected representation, and charge the complete preceding
published state while the replacement is built. Explicit `off` and `on`
retain their historical behavior. Reports expose the requested and resolved
policy, reason, pilot counts, frozen reuse, and scheduler axes. `dagutil`
accepts `auto` only for chart-SPR state/search modes; standalone consumers and
`larch2` reject it explicitly rather than silently choosing a representation.

The conditional additional within-clade staging action is omitted. The raw
decision record is
`build/wric-chart-parallelization/phase7-within-clade-profile/README.md`,
SHA-256
`1a941352c33a82720e6484c4e2c878acefd6d17ae5172da6e165719e9fe6c559`.
After the allocation-free validation change, successful validation fell from
3,161,872,940 to 25,288,668 Callgrind instructions, a 99.200% reduction. On
the high-compression fixture the scheduled combined lazy stage measured
190.041839 ms at W1 and 46.321395 ms at W8 (4.103x). Even deleting all work in
underfilled W8 levels could save only 4.456%, below the plan's 10% threshold;
the named small stage is only 2.979360 ms at W1 and already exposes parallel
overhead in its inside half. A second nested pattern/representative staging
axis is therefore not justified. These measurements support only that
conditional omission and are not sealed Phase-7 performance acceptance.

The worktree at the `38e9a28` checkpoint rebuilt `chart_spr_search_test`,
`chart_spr_allocation_test`, `chart_spr_phase10_test`, `chart_parallel_test`,
`dagutil`, `larch2`, and the counter compile check. The build log SHA-256 is
`7521135fbfa6a5467afa63de43419baccdfd64c6ec509d5751f4c54323a8a4b5`.
It then ran those four C++ tests, the two `dagutil` auto-policy tests, the
chart-SPR auto CLI test, and the `larch2` rejection test. All 8/8 passed in
11.38 seconds. The raw CTest log is
`build/wric-chart-parallelization/phase7-38e9a28-main/focused.ctest.log`,
SHA-256
`4a7b9aeeba4ae6c3b1bd57e42f320efa099de39061dd004e333199635508a682`.
A separate clean-tip review also passed the benchmark-harness regression and
found no branch-specific accounting, scheduler-axis, formatting, or CLI
blocker. Changed-line clang-format and `git diff --check` are clean.

This was not the Phase-7 exit decision. The later production manifests are now
sealed: `phase7-lazy.tsv` has SHA-256
`82250bb26d5394d3c2c616e4ccbf606e80eca074e560fd8e3361274655e56718`,
and `phase7-lazy-completion.tsv` has SHA-256
`7b934fa0ad8279893d03321724935dccf8df99fa840899652dccdc63c8c37bff`.
Quiet-host forced-off/forced-on/auto medians, the required high-fixture and
small-fixture scaling/overhead comparisons, paired RSS, sparse-reclamation
TSan remain pending. The full normal, serial ASAN, and exact 55-test TSan
gates later passed on immutable product R as recorded under Phase 8. Product P
was a later implementation change, but its clean full-suite attempt exposed a
timing-sensitive high-water assertion. Product Q fixes that test
deterministically and has the sealed clean normal closure recorded below; Q
full serial ASAN and exact targeted TSan have also passed. Under the authorized deadline-overlap rule,
implementation proceeded to Phase 8 without substituting diagnostics for the
remaining gates.

### Historical immutable product P dependency-ready checkpoint (diagnostic only)

The historical implementation checkpoint is immutable commit
`3b0e442e213a7913c6e007c76715d7d856bce0bd` (`Parallelize WRIC chart search
pipeline`), called product P below. Its diagnostic `dagutil` and
`wric-process-metrics` SHA-256 values are respectively
`c13704f26a811efdfbf5c65b4a609c07a658d4d7db6ba99f5fb20af084717f79`
and
`3dbf846e6aa05b007160d9b8d149a82f8e26c4e8099a4e4fc39fdbc63ab02104`.
The retained artifacts are:

- `build/wric-chart-parallelization/diag-dependency-ready-r6/`; and
- `build/wric-chart-parallelization/diag-auto-policy-r6/`.

Product P adds a finite dependency-ready route for full-retention lazy charts
over strict binary trees at W2 or greater when no observing hook is installed.
The inside phase uses atomic remaining-child counts and the outside phase uses
the unique strict-tree production dependency; each has a fixed-capacity ready
queue and occupies one scheduler operation for the complete phase. Outputs
and statistics have stable disjoint owners, worker-output capacity is
certified and charged, and a joined deterministic fold chooses the earliest
canonical worker failure. Generic DAGs, sparse mode, W1, observer-enabled
execution, and certificate or finite-budget misses retain the earlier
wavefront/serial fallback. Submission failure, worker failure, cleanup, and
same-scheduler retry are covered directly.

The same checkpoint caps candidate-local lazy work at three quarters of the
resolved workers once there are at least 1,024 active patterns. At W8 that is
six concurrent tasks; W1, W2--W7, smaller streams, and already-small batches
are unchanged. The shared planner drives both runtime waves and the finite
iteration envelope. The route and cap are exposed by these additive report
counters:

- `lazy_chart_{inside,outside}_dependency_ready_executions`;
- `lazy_chart_{inside,outside}_dependency_ready_jobs`;
- `lazy_chart_{inside,outside}_dependency_ready_scheduler_operations`;
- `lazy_chart_{inside,outside}_dependency_ready_capacity_resident_bytes_max`;
- `lazy_local_requested_concurrency_max`;
- `lazy_local_effective_concurrency_max`; and
- `lazy_local_bandwidth_capped_batches`.

The focused RelWithDebInfo command covered
`chart_parallel_test`, `chart_spr_search_test`,
`chart_spr_pipeline_test`, `chart_spr_allocation_test`,
`lazy_key_grouping_test`, `chart_spr_test`, `merge_test`,
`dagutil_chart_spr_canonical`, and
`wric_counter_baseline_temporal_fields`; all 9/9 passed. A prior in-tree
full-suite run appeared to account for all 176 tests, but the clean detached P
run failed `chart_spr_test` because
`grammar_candidate_active_worker_high_water >= 2` depended on incidental
scheduler overlap. The isolated test could pass, which confirms the assertion
was timing-sensitive rather than establishing a clean normal closure. P is
therefore superseded as the evidence product by Q below.

#### Dependency-ready six-repetition matrix

All rows used the high-compression fixture at physical-core affinity
`0,2,4,6,8,10,12,14`, 64 scored candidates, no exact verification, and the
same bounded one-iteration workload. Medians are the mean of the middle two
of six repetitions.

| Forced policy/workers | Wall median (s) | Report total (ms) | Cache build (ms) | Initial chart (ms) | Generation (ms) | Local score (ms) |
|---|---:|---:|---:|---:|---:|---:|
| off / W1 | 0.233316443500 | 186.518500 | 127.218500 | 90.628000 | 4.991500 | 39.336000 |
| off / W8 | 0.122693050000 | 82.311000 | 54.034500 | 17.209500 | 8.698500 | 10.493500 |
| on / W1 | 0.202869868500 | 157.075500 | 85.873000 | 48.766000 | 4.876500 | 50.492000 |
| on / W8 | 0.131911820500 | 87.868500 | 55.538000 | 18.794000 | 4.239500 | 14.570500 |

The exact diagnostic arithmetic is:

- forced lazy W8/W1 =
  `0.131911820500 / 0.202869868500 = 0.650228747499`, which is below
  `2/3`;
- forced dense W8/W1 =
  `0.122693050000 / 0.233316443500 = 0.525865421911`; and
- forced lazy/dense at W8 =
  `0.131911820500 / 0.122693050000 = 1.075136859830`, which is below
  `1.10`.

Every forced-lazy W8 repetition reports exactly one inside and one outside
dependency-ready execution, 1,023 inside jobs and 511 outside jobs, one
scheduler operation for each phase, and capacity maxima of 8,400 and 2,232
bytes respectively. Each also reports requested/effective lazy-local
concurrency `8/6` and two bandwidth-capped batches. All 24 compact canonical
sidecars are byte-identical with SHA-256
`ae62f9153f5a0f8af0646db23e08088790c727f4a7990cbe1a6a5e71d97b84c6`;
all reports embed canonical semantic SHA-256
`0f50e129d006c960de0f1825072f062ac04350d78ba77af62fbcab772ec2edef`.

#### Automatic-policy six-repetition matrix

The second rotated matrix used W8 and the same candidate/search controls. The
forced high-compression medians come from the immediately preceding
same-product matrix.

| Fixture/policy | Resolved policy | Wall median (s) |
|---|---|---:|
| dense-favoring / off | off | 0.040738142500 |
| dense-favoring / on | on | 0.040660933500 |
| dense-favoring / auto | off | 0.040736641000 |
| high-compression / off | off | 0.122693050000 |
| high-compression / on | on | 0.131911820500 |
| high-compression / auto | on | 0.131609652500 |

Dense auto is `1.001861922329` times its fastest forced representation and
the two dense forced medians differ by only `1.001898849666`, so choosing
`off` is permitted by the within-10% rule. High auto is `1.072674063445`
times its fastest forced representation and the high forced medians differ by
only `1.075136859830`, so choosing `on` is likewise permitted. High auto is
`0.997709318249` times the forced-on median. Thus both auto overhead
diagnostics are below `1.10`, and the two named fixtures exercise both
resolved branches.

All 18 dense compact sidecars have SHA-256
`24cc66ce9fa739d4066d75651d38eb4e5fdec4b12c02c48263802b22f3f904c9`
and embedded semantic SHA-256
`b8d55c73220a7025b8d977ac4bb083f1e4e76dee5bf007e8754b1f79d42b8442`.
All six high-auto sidecars have SHA-256
`ae62f9153f5a0f8af0646db23e08088790c727f4a7990cbe1a6a5e71d97b84c6`
and embedded semantic SHA-256
`0f50e129d006c960de0f1825072f062ac04350d78ba77af62fbcab772ec2edef`.
The before/after input hashes match: the high-compression and dense-favoring
fixture SHA-256 values remain
`e103be6cd1df36e5a002ae9dd9ffb53110b35c840eed1d8fa867475ad3109874`
and
`e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6`.

Both matrices are explicitly **non-accepting diagnostics**. They are not
published through the sealed harness/evaluator, do not decide the
small-fixture overhead or RSS gates, and cannot close Phase 7 or Phase 10.
The required sealed Q Phase-7 recaptures, their cross-phase decision, Q sanitizer
reruns, and all remaining final acceptance gates are pending.

### Immutable product Q sealed clean normal closure (2026-07-23)

Current evidence product Q is
`a9db72e60f153a95362db544107373817a58a258` (`Stabilize grammar worker
overlap test`), tree
`904f35c58daa1c331f2c43f3aba373f8c8858d4e`, with P as its sole parent.
Q uses a deterministic latch to hold grammar candidates until multiple
workers are active before releasing them, removing the suite-load dependency
from the overlap assertion without changing the P chart implementation or its
historical diagnostics.

The clean detached locked worktree is
`/home/ogi-agent/matsen/larch2-wric-product-a9db72e`; its evidence root is
`/home/ogi-agent/matsen/larch2-wric-evidence/product-a9db72e-normal-full`.
The literal command

```sh
ctest --test-dir build --output-on-failure --parallel 8
```

registered 176 unique tests and completed with exit 0: 174 passed, zero
failed, and exactly `123 - merge_consistency_test` and
`130 - rotaA_diagnostic_test` skipped. `chart_spr_test` passed in 3.11
seconds; total suite wall time was 192.64 seconds. The worktree was clean
before and after the run.

| Q normal evidence artifact | SHA-256 |
|---|---|
| `full.ctest.log` | `15fb82e99b72ecadeb0b2bc28df28d01e4c36fea46cefcd6688b9067be34d76c` |
| `full.LastTest.log` | `ace8762f67f1d407ccc461203e2ac75e086dd53db5c51487a02da5852c5fb0c8` |
| `posttest-provenance.txt` | `80d80c2f1405cb1474cfc95e21365fb18182bc35c9b9ac29b3dc5173b8c3fc52` |
| `artifacts.sha256` | `d6b36894d951aa89e3ba983b271b2129c1c83f09817184dc67d7761f8b1cf04c` |
| `build/CMakeCache.txt` | `a494d9671f8ee27077b0ca401b560fbbc8959e3bcfea30ccc5188602ab3e7b67` |
| `build/bin/dagutil` | `8d135043b6b9fe19d5f971a590d9f52278befa87fd9c6b861c284d69bd4fff54` |
| `build/bin/larch2` | `6399773f3ef043f500b0950e6cb976eaa373f71494d43dcf8047f611ffba3da4` |
| `build/bin/wric-process-metrics` | `637a3974a0b7879dc60f2eebef672923cac4c0ffdabb177466ac41ae9b7275f3` |
| `build/chart_spr_test` | `5ea28533cc598fdc7e6c68355c96e8ef209c96a3e1204907610715e9441fa87a` |
| `build/wric_counter_baseline_compile_check` | `0f2f66b64eb810ee2ce189354ff4e7a06840c5d50901a33e8aec55ede640e01b` |

This seals Q's normal result only.

### Immutable product Q sealed full-ASAN closure

The same clean detached Q root was configured and built with the pinned
GCC-trunk toolchain, `ENABLE_ASAN=ON`, `ENABLE_TSAN=OFF`, and
`RelWithDebInfo`. The excluded counter-baseline target was built explicitly,
then the exact 176-test inventory ran serially with
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1` and the pinned GCC-trunk runtime
library path. It completed in 728.20 seconds: 174 passed, zero failed, and
exactly `123 - merge_consistency_test` and `130 - rotaA_diagnostic_test`
skipped. `LastTest.log` contains all 176 records. The explicit scan found zero
AddressSanitizer, LeakSanitizer, or detected-leak signatures, and Q remained
clean and exact throughout.

The sealed evidence root is
`/home/ogi-agent/matsen/larch2-wric-evidence/product-a9db72e-asan-full`.

| Q ASAN evidence artifact | SHA-256 |
|---|---|
| `build.log` | `c010dafcd0b14850cc0ab70d69240e106a4469bf7333b734d3e5bb1fcb8d63c1` |
| `configure.log` | `a9a3f1899ae385548ee3c0f98b68935af3c4674cb71964949c54039c2aa29b68` |
| `excluded-target-build.log` | `98daf11118f8128e53ee5063f6a949bd0ad5b774026e2ce043b9cce3c49e8079` |
| `full-inventory.log` | `11a5692e1080b2ad7e1f4bbb59a05a3a2f83cb4e609e5fff30ca19c1bf89a15f` |
| `full.ctest.log` | `5abb6db6ea594359cd10eb66f6bdab1949684009e27093e83addaaa468981d1f` |
| `full.LastTest.log` | `8cbc3f3d364d5c2d1d72d9dcca9f8aaeaad0937f36fcc7ab1cdd2ef2addc4fa7` |
| `diagnostic-scan.log` | `00adc251e5163809e3fef0b04edb2b6d71c8637fe69999ebefcb74aa3b958c1a` |
| `postflight.log` | `b5cf0ac93a3f3316775a9698f757de560f159fe1b98e9bc0704390fc9bc2875b` |
| `SHA256SUMS` | `ac339eee73459489962f844f437b0e7e11c78051c972d1d3d153e93a913683fd` |

This closes Q's ASAN gate. The targeted TSan closure is recorded immediately
below. At this checkpoint every timed Q recapture was pending; C4 later
completed and audited all 20 components. This sanitizer result alone was not
Phase-7, Phase-10, default, or final acceptance.

### Immutable product Q sealed targeted-TSan closure

The same clean Q root was configured and built with `ENABLE_ASAN=OFF`,
`ENABLE_TSAN=ON`, and the pinned GCC-trunk toolchain. The full inventory is
176 and the exact required regex selects 55 tests. Those 55 ran serially with
`TSAN_OPTIONS=halt_on_error=1`, `LD_PRELOAD` unset, and the pinned patched
runtime first in `LD_LIBRARY_PATH`; all 55 passed, with zero skips or failures,
in 281.86 seconds. Eleven representative binaries all resolve `libtsan.so.2`
to patched bytes with SHA-256
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.
The strict scan found zero ThreadSanitizer, data-race, lock-order,
use-after-publication, thread-leak, summary, warning, or fatal signatures. Q
remained detached, clean, and exact throughout.

The sealed evidence root is
`/home/ogi-agent/matsen/larch2-wric-evidence/product-a9db72e-tsan-focused`.

| Q TSan evidence artifact | SHA-256 |
|---|---|
| `build.log` | `63f2cd6c3ac4e7d1016380dac684d733a7c50975ce7e4858bbdd31dd6e8fdf1c` |
| `configure.log` | `e772eff4a532cf1b4525da5ca51e595cf2dfe823c83537ef18ecc66ac79dfd47` |
| `full-inventory.log` | `6848e57f60b888da9e00275b35ab9a6e16d6ab6ec2d926ee6d616ce375104726` |
| `targeted-inventory.log` | `a31ed742b506e2c39393bc835fad8e2e3115bd99e629acb92e9395f9218987b7` |
| `focused.ctest.log` | `a3cbef485a81da355096734757c0607d0102f65bcafd9ed25cb31ee9b95811a2` |
| `focused.LastTest.log` | `67a1e94c0344eb1a5e427d6c1f7bb9b9f8a528d7e68cccf1b03e9f0fdc46fb91` |
| `runtime-provenance.log` | `9a79f46db76cd3322f26bb9d414841e3a3eed2b6a2aa1c1ec471e50544228ff3` |
| `diagnostic-scan.log` | `6615793e3480df9ae24df6ec7813afd53ce5b44f0586513ceacdae761dc0db29` |
| `postflight.log` | `33592677448e7d786f79eab502254a29cab4fba8c85658ece3e53bacb2d96741` |
| `SHA256SUMS` | `5b90a3f931853a03aec20bdaf2b44ff71c7f2d1afddc54c42a51a3f7c7f4eef1` |

This closes Q's normal, ASAN, and targeted TSan gates. C4 later completed all
timed Q recaptures, and C7 failed the pre-default decision on the four gaps
recorded below. Any later product revision reopens the affected
product-bound gates.

## Historical Phase 8 implementation checkpoint (unsealed)

Phase 8 has a **functionally passing implementation checkpoint; final
performance acceptance pending** at
`6c8d0c7651c2aa2e5c396d0f57c2e4e18c322310` (`Make finite pipeline
admission test scheduler-invariant`, tree
`8773b0a742f0a7ddca7909f8a9bf616dcbb1a9e7`). The immutable measurement
checkpoint remains `7ca527b8906d018124756182274335cbff936d72`
(`Baseline measure`). The historical counter-baseline document remains
byte-identical, SHA-256
`cdb3fd9f936cb335f8f93eafd5abbc4d31822bfe657f4f3fd343c85c1203a82e`.
No frozen executable, oracle, runner, workload, candidate/exact budget,
validation, or exactness contract was replaced.

Commits `6671235`, `ea8d492`, and `e48cadd` prepare sampled trees once,
partition projection into stable indexed slots on the one search-lifetime
scheduler, and add a bounded two-buffer producer/consumer pipeline. The
pipeline snapshots the grammar, execution plan, cache, and pattern epochs;
generation and scoring share an explicit scheduler-handoff barrier; every
early accept, stale snapshot, failure, and cancellation drains the producer
and invalidates unscored buffers before commit.

Commits `648f74f`, `5c5b849`, `3d00358`, `60e04c7`, and `913f1cf` replace the
global all-move sampled preassignment with bounded stable-source waves, direct
tree-delta projection, reusable per-slot dominant storage, capacity-rounded
signature accounting, and allocation-free prepared metadata. The coordinator
alone advances the legacy RNG stream and performs canonical filter, signature,
deduplication, cap/reservoir, callback, counter, and final-order decisions.
Workers may construct speculative results, but completion order is never
semantic; a stop restores the exact canonical RNG/counter boundary and discards
the tail after joining it. Warmed direct projection of all 416 named-fixture
moves now reports zero allocation calls and zero requested bytes.

Commits `15459e3`, `3110d1e`, and `45573cb` close the finite-memory and grammar
halves. The unified finite iteration envelope independently admits sampled
source, sampled projection, and grammar-construction widths, charges retained
source/dedup/output ownership once, treats mutually exclusive scheduler
operations as temporal alternatives, and retains actual-capacity backstops.
Grammar enumeration remains the exact canonical event/RNG stream; bounded work
descriptors own source/destination paths and the post-event RNG/counter
snapshot, candidate construction runs in parallel, and serial gather restores
the last committed boundary on stop or failure. Exact E succeeds, E-1 rejects
before buffer reserve/submission, and deliberately underestimated realized
source/projection/grammar capacities fail before semantic work or scheduler
handoff.

Commit `6c8d0c7` separates two independent contracts found during the TSan
gate. A tight finite envelope may admit one grammar construction per wave; the
scheduler handoff must then serialize the next construction behind scoring,
so overlap is not an admission invariant. The finite-boundary test no longer
uses a timing sleep or demands overlap in that case. The dedicated full-slot
cancellation test still requires and observes non-vacuous producer/consumer
overlap, drain, stale-work discard, and scheduler recovery.

### Functional evidence

The worktree at that implementation checkpoint rebuilt `larch`,
`chart_spr_test`,
`chart_spr_pipeline_test`, `chart_spr_search_test`, and
`chart_spr_allocation_test` with the required GCC-trunk C++26 RelWithDebInfo
toolchain. The registered focused command

```text
ctest --test-dir build --output-on-failure --parallel 4 -R '^(chart_spr_test|chart_spr_pipeline_test|chart_spr_search_test|chart_spr_allocation_test)$'
```

passed 4/4 in 8.32 seconds. Its raw log is
`build/wric-chart-parallelization/phase8-45573cb-main/focused.ctest.log`,
SHA-256
`1239b09fbb91325c6608b66f3a7a891248e5e2a468a0c5822a41a27b29a86d71`.
The matrix compares workers `1,2,4,8` and seeds `1,7,19` for grammar,
sampled-tree, and hybrid sources; checks candidate identity/order/source,
random traversal and reservoir selection, exact counters, callbacks, and
repeated W8 output; exercises binary, multifurcating, multiparent, and sampled
projection differentials; and requires real parallel operations/high-water.
Dedicated pipeline tests cover overlap, stale state, early acceptance,
cancellation, scoring/generation error precedence, quiescent recovery, and the
three-dimensional finite boundary.

At `6c8d0c7`, the complete RelWithDebInfo command

```text
ctest --test-dir build --output-on-failure --parallel 8
```

passed 167/167 in 185.54 seconds. `merge_consistency_test` and
`rotaA_diagnostic_test` retained their pre-existing documented skips. The raw
successful log is
`build/wric-chart-parallelization/phase8-6c8d0c7/full-rerun.ctest.log`,
SHA-256
`12b1d0acbb74e1ec30386b771ef8660a6480e87e9e84c50a94e0e7fa2aa3c6e8`.
The immediately preceding full attempt passed every chart test but observed a
loaded-host timeout-startup race in `wric_process_metrics_test`
(`timeout_kill_sent=0` rather than `1`); that test then passed alone in 4.33
seconds and in the complete successful rerun. The failed-attempt log is
retained, SHA-256
`996ccc5923baf87c3da4f16ce69d3037db1a0bc750f69b09adacfb759d80d5d8`,
and is not counted as gate evidence.

The targeted GCC-trunk TSan command used serial CTest execution for
`chart_parallel_test`, `chart_scheduler_test`, `chart_spr_test`,
`chart_spr_search_test`, and `chart_spr_pipeline_test`. All 5/5 passed in 43.13
seconds with `TSAN_OPTIONS=halt_on_error=1` and no race report. The raw log is
`build/wric-chart-parallelization/phase8-45573cb-main/tsan.ctest.log`,
SHA-256
`730dcbee30b2543bec51de054b52f0192dc2f758423870ba53e8f8a116fcfd14`.
The required patched runtime is
`build/wric-chart-parallelization/phase3-7d294d6/patched-tsan-runtime/libtsan.so.2.0.0`,
SHA-256
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.

### Collected generation diagnostics

`data/test_5_trees/tree_0.pb.gz` qualifies for the named
`sampled-generation-high` row: the frozen command emits exactly 256 post-dedup
candidates and the pre-source-wave W1 product timer at `51702c7` measured
702.396 ms, above the plan's 100 ms minimum. The fixture SHA-256 is
`e8dcd803ba2cd82ed594dbe66433934a62b3711ea7ddb0d349de35ef86030dd6`.
The pre-optimization profile, exact command, executable hashes, split counters,
and recommendation are preserved under
`build/wric-chart-parallelization/phase8-generation-profile/`.

After bounded source waves and reusable direct projection, three unsealed
trials produced the following diagnostic medians:

| Metric | W1 | W8 | Comparison |
|---|---:|---:|---:|
| candidate generation | 344.876 ms | 646.266 ms | 0.534x W8 speedup |
| whole-process wall | 0.464027 s | 0.775479 s | 0.598x W8 speedup |
| maximum sampled RSS | 17,316 KiB | 17,516 KiB | 1.012x W8/W1 |

W1 generation is 2.04x faster than the earlier 702.396 ms product result, but
the noisy-host W8 result misses the required same-revision 2.0x gate. All six
runs have byte-identical canonical-file SHA-256
`7bd15c26b2b529fcdb9f8095b24f6b7437c0beeaba6dc8a64ac5abf5b3c2d594`.
The copied raw outputs are under
`build/wric-chart-parallelization/phase8-45573cb-main/source-waves-unsealed/`
and `direct-projection-unsealed/`.

These measurements were deliberately not an exit decision: unrelated host
compilation was active, Phase 0 was not yet calibrated/sealed, and the Phase-8
supplemental manifest had not yet been created. Phase 0 and the supplement were
subsequently sealed, and sealed generation attempt 0 failed as recorded below.
This historical checkpoint does not reinterpret that miss or substitute for
the later sealed evidence.

### Stable-slot, reservoir, and finite-envelope hardening (current through `94a6323`)

The later committed hardening chain extends the historical `6c8d0c7`
checkpoint without changing its canonical source/RNG contract:

- `0c0fbb9` (`Preserve canonical speculative failure ordering`) retains the
  earliest canonical accepted failure after every speculative task has joined.
- `b66e115` (`Adapt finite sampled source waves`) adds bounded, adaptive
  sampled-source waves under the unified temporal-memory envelope.
- `6e8167d` (`Harden nested sampled projection waves`) sizes direct-projection
  workspace vectors by stable scheduler slots while charging dynamic storage
  only for the active wave, quarantines a failed workspace for the rest of its
  joined wave/subwave (including nested same-scheduler fallback), and removes
  a stale retained-candidate charge from the post-generation evidence phase.
- `e3eec44` (`Cover reservoir and width-two admission contracts`) adds the
  sampled/hybrid reservoir matrix and direct/outer width-two admission
  coverage.
- `94a6323` (`Strengthen reservoir and admission oracles`) independently
  replays Algorithm R and the final shuffle over the exhaustive canonical child
  stream for workers `1,2,4,8` and seeds `1,7,19`. It also replaces an
  unreachable duplicated-provenance outer fixture with an unmodified
  production state: exact budget `E` admits source width two, `E-1` executes at
  source width one with identical canonical old-state evidence, and the direct
  child boundary still rejects its own `E-1` before source, projection,
  workspace, or scheduler side effects.

The implementation-plan evidence status was synchronized at `d8adbb5`
(`Record Phase 8 hardening evidence`); that commit changes documentation only.
The immutable measurement checkpoint remains
`7ca527b8906d018124756182274335cbff936d72`, with exact subject
`Baseline measure` and sole parent
`408434ecfd096af484ecbbd3deeb67511151cd76`. Git ancestry verification confirms
that it is an ancestor of both `94a6323` and `d8adbb5`; no baseline checkpoint
or frozen executable role was replaced.

On the required GCC-trunk C++26 RelWithDebInfo build, the focused binaries
`build/chart_spr_test` and `build/chart_spr_pipeline_test` both report
`PASS` at this source checkpoint. Their raw logs are respectively
`build/wric-chart-parallelization/phase8-hardening-current/chart_spr_test.log`
(SHA-256
`06cdf0627834ea91e59b77541b26bcda26430f0e45db4f23496455f146e57776`)
and
`build/wric-chart-parallelization/phase8-hardening-current/chart_spr_pipeline_test.log`
(SHA-256
`676d55384d07c861e4d8a89ecb5e847863c249c5490770601e3e284102d06009`).

This focused evidence was not a Phase-8 exit decision. The Phase-8 supplement
was subsequently sealed at SHA-256
`773545d093904c20eaa80343615fa77089989735e61d34947530b07ef008cf17`,
and sealed attempt 0 subsequently failed the 2x generation gate. The additive
optimized retry, Phase-7 end-to-end comparison, and remaining RSS decision
remain pending.

### Latest complete pre-cleanup correctness and sanitizer closure (`51d2a3c`)

Commit `51d2a3c2c19c0e71c94ff23013a1a58cadf3b9ed` corrects the
capture contract by pinning historical run label `phase8-generation` to full
immutable Phase-8 checkpoint
`94a63238d25a8e3262428419d53f8f0986e8879b`, replacing stale pre-hardening
revision `a21ab7a`. The matching regression test requires the exact full hash,
accepts it, and rejects mismatches. No production C++ byte changes between
`94a6323` and `51d2a3c`; intervening commits are documentation plus this
capture-tool/test correction. The focused RelWithDebInfo capture-contract
CTest passes 1/1 (26 Python cases) in 24.41 seconds. Its stdout and LastTest
logs are under `build/wric-chart-parallelization/anchor-51d2a3c/`, with
SHA-256 values
`4d94eeac36eb26eeb3d5a2b8e4dd7bd01de35dfee27922c7e4da8ebe2e25b98e`
and
`694acf350bbe799bf3bae5cd4e0114151f201e9ddca6843e5e4ff3b0c77108d9`.

The coherent GCC-trunk C++26 RelWithDebInfo build and full CTest at production
source checkpoint `94a6323` plus documentation checkpoint `1923bbc` has zero
failures across 176 registered tests in 204.81 seconds: 174 pass and the
established code-77 `merge_consistency_test` and `rotaA_diagnostic_test` skip.
The build, CTest stdout, and preserved LastTest logs are under
`build/wric-chart-parallelization/final-94a6323/`; their SHA-256 values are,
respectively,
`7b4eb62054c0ff7c70d0602082b09140b0b881ca1799dd09762e004f31606b12`,
`3715f2355f8337e79c815d235a0c7f8fe8d7ff8793350816b45bb0e41a79c50b`,
and
`c161cde4f2c2bceebe29a6204fa366b680f75ba15144922ad0a1f689594f20de`.

After the capture-anchor change, the complete RelWithDebInfo CTest was rerun
at exact HEAD `51d2a3c`. It has zero failures across 176 registered tests in
190.31 seconds: 174 pass and the same two established code-77 tests skip. The
command/environment transcript, CTest stdout, and preserved LastTest hashes
are
`aa6ea26328b81f8232cf837a7895d9af97857ce04c5451caa60642a2c26875bc`,
`7c694b4dc8384ef747d809f65d3f831784a7121a5cec2b8103759cbe15aa8d01`,
and
`096c26d250ea1faf6d1325140a739e56c504a94c9a3db57a927da72685b69f2a`;
the files are under
`build/wric-chart-parallelization/post-anchor-51d2a3c/`.

At `51d2a3c`, the complete serial ASAN CTest also has zero failures across 176
registered tests in 894.83 seconds: 174 pass, the same two established tests
skip, and there is no Address- or LeakSanitizer diagnostic. Loader provenance
for `chart_spr_test`,
`chart_spr_pipeline_test`, `dagutil`, and `larch2` resolves `libasan`,
`libstdc++`, and `libgcc_s` under GCC trunk; the exact runtime
`libasan.so.8.0.0` has SHA-256
`182be2e1985a5c9a3a3c6a52f56f92af375f34a69e950d50b940b5baff395575`.
The ASAN command/environment transcript, build, runtime-provenance, CTest
stdout, and preserved LastTest hashes are
`a71411820a788b307fb11f102db34bf3476d1e4f6fb3032821810085afa643d2`,
`32f9a7d83a1477159b35f4ca612ae0214219c6292ed25a9b856cea34d737c7b1`,
`9838d4b9ed86bb0602f50912e19a7450e98b625d63e998922201231adef385a1`,
`823542b4db4f4ac3a5cc33596ab3cf278dc40501b5ce9124f188e5d84d052163`,
and
`92fa789d422191b6bad0493a74197864a0efa119f9b047240a1081dc8e568411`;
the files are under `final-94a6323/asan/`.

The fresh `51d2a3c` TSan build registers 176 total tests and the exact final
plan regex selects 55. Patched runtime `libtsan.so.2.0.0` has SHA-256
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`;
representative thread-pool, pipeline, `dagutil`, and `larch2` binaries all
resolve to that patched runtime before GCC-trunk `lib64`. The serial targeted
matrix passes 55/55 in 293.61 seconds with no TSan warning, fatal, summary,
race, lock-order, publication, or thread-leak report. A separately preserved
first pass also completed 55/55. The accepted command/environment transcript,
build, runtime-provenance, inventory, CTest stdout, and LastTest hashes are
`efd174a660ef6be903848fad7792590f3874c4e0e6cef2cfbb269636a4fbb142`,
`03025ede3343a51d61612b3732e04b9968fcd0e5f7360773bc20c5ceaf1b3758`,
`bbe248f9183252113bba931f7ad28ce97c085c6352835010f6fb84e172743939`,
`b49508c42ca10a2bc94ed54bf826edb57abc0540867fda3676c31ae8db72da86`,
`604037bdb09eda2c97f92a5032b5229f2445c6179151900b6a78a247d0f919cf`,
and
`61e9e99b7b820b727814c2e4c91004a45d9c17a68385077a020c96733d68155f`;
the files are under `build/wric-chart-parallelization/tsan-51d2a3c/`.

These results close only the latest complete pre-cleanup normal and sanitizer
checkpoint. They close no Phase-0, supplement, performance, RSS,
automatic-policy, or default-change gate. The production-state cleanup at
`4ef6126` invalidated this evidence and the complete post-cleanup sequence was
rerun as recorded next.

### Post-cleanup correctness and sanitizer closure (`4ef6126`)

Commit `4ef6126607c8da5f25d54928ea2fd6d152b89e29`, tree
`833842e7b8416ff8a22b5ee471609e31e3b2f252`, was clean before and after
every accepted gate. The immutable `Baseline measure` commit remains its
ancestor with exact sole parent `408434e`. All builds use the required
GCC-trunk C++26 compiler; the compiler SHA-256 is
`776a974a2559ed75f973d8f9ec4de789763c897a34950b4651875c9c0d57a404`.

The focused cleanup/matrix gate used:

```bash
cmake --build build --parallel 8 --target \
  chart_parallel_test chart_scheduler_test chart_trim_test \
  chart_spr_search_test chart_spr_pipeline_test chart_spr_phase10_test

ctest --test-dir build --output-on-failure --no-tests=error \
  -R '^(chart_parallel_test|chart_scheduler_test|chart_trim_test|chart_spr_search_test|chart_spr_pipeline_test|chart_spr_phase10_test|no_direct_chart_spr_multisite_helpers|no_direct_chart_spr_root_score_detail|no_chart_bnb_superset_topology_exact|no_overlay_chain_compaction_prefix_materialize|no_keep_production_without_exact_check)$'
```

It passes 11/11 in 12.24 seconds. The build, CTest, and preserved LastTest
SHA-256 values are
`c8f24d78135dbfff221f2a650e7b461e1487892ec02f9792c67267141a42c0e5`,
`b250e6d184bbb41f792c7c9138da1bb894f6b23481b204a3a4475f95dbcd45ad`,
and
`5b906f0c9196370a8afa9b855b980893dbdfae1413778c0363bf2c34d933b7b6`.
The evidence is under
`build/wric-chart-parallelization/phase10-matrix-4ef6126/`; its artifact
manifest SHA-256 is
`451bb931332232256e41f8fed4aeb314fc13b945a2e2c249618dacb5a37d529a`.

The complete normal gate used:

```bash
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --parallel 8 --no-tests=error
```

All 176 registered tests complete with zero failures in 213.57 seconds: 174
pass, while the established code-77 `merge_consistency_test` and
`rotaA_diagnostic_test` skip for their unchanged reasons. The command,
build, inventory, CTest, and preserved LastTest SHA-256 values are
`9f8f57ee05f07396046e8089a86ede2434e879d6bd43018d010b47e3a63e02d2`,
`3265ecd47afd5c4c526c9569308c6b5f5c9841a2ed7395557908be6d3a1776f8`,
`f214ccee352c3689affdc794e2591ca40a9a86e913158eaec6c1e4e8b8550e76`,
`b5341dd922a557b86948af1358a76b7a83e317961d040c406643fc03a115d758`,
and
`8b9a4b3005e12a37729a9ee972a55a1ea8b4413ada3c9ac98496d7c61c7942bb`.
The evidence is under
`build/wric-chart-parallelization/post-phase10-4ef6126/`; its artifact
manifest SHA-256 is
`0ad04ec468d1aa8d5e317e7240a4adeb5701ce8552bc7a16b7a908b0acd42bea`.

The complete ASAN gate used the plan's RelWithDebInfo configuration and:

```bash
cmake --build build-asan --parallel 8
LD_LIBRARY_PATH=/home/ogi-agent/install/gcc-trunk/lib64 \
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 \
  ctest --test-dir build-asan --output-on-failure --parallel 1 \
    --no-tests=error
```

All 176 registered tests again complete with zero failures in 873.06 seconds:
174 pass and the same two established code-77 tests skip. The diagnostic scan
finds no AddressSanitizer or LeakSanitizer report. Every representative binary
resolves GCC-trunk `libasan`, `libstdc++`, and `libgcc_s`; exact
`libasan.so.8.0.0` SHA-256 remains
`182be2e1985a5c9a3a3c6a52f56f92af375f34a69e950d50b940b5baff395575`.
The command, build, inventory, runtime, CTest, and preserved LastTest SHA-256
values are
`7705a40146f0585c17c029c0f7207cea2bf96b16f8371dbea35aad01fa64cce1`,
`3ba59f17196c5a2cde1b8b9b88ee4a94596a7e1b2d54f60ef9e847a4be100ac7`,
`071877401ab17e7b1ca44e8357618bee22185e83cc79db0c638baa90c1f4aec4`,
`8fbb0bdf75ebc966bf8bc0b61faf82bdbd8692f8c701e290ac1c298de81405a6`,
`819d8a5d6df2073f085ed9b8744aadad29287baa14b456030e0b69ce73102882`,
and
`7a9228239cf8ad6c05c5a689e026ae201b84115de7121e5e26de44e90d339415`.
The evidence is under `build/wric-chart-parallelization/asan-4ef6126/`; its
artifact manifest SHA-256 is
`cea275ecb93bd2f20073795adec2659a1e93fa9bc3f7585f7d1c4787af28b2a3`.

The exact TSan regex in the plan and command transcript selects 55 of 176
registered tests. With
`LD_LIBRARY_PATH` preferring the validated patched runtime and
`TSAN_OPTIONS=halt_on_error=1`, the serial targeted matrix passes 55/55 in
582.49 seconds with no TSan, race, lock-order, publication, or thread-leak
diagnostic. Patched `libtsan.so.2.0.0` SHA-256 remains
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.
The command, build, selected inventory, runtime, CTest, and preserved LastTest
SHA-256 values are
`8dd99420d1375afbb152f7638ad055873a516828ec068946788d81869536c7a7`,
`9d7b68049312ccfa6a15d37d675f1b7b916891a2ab4318d7dde2e8f00f3f8c52`,
`b49508c42ca10a2bc94ed54bf826edb57abc0540867fda3676c31ae8db72da86`,
`80783b00e5ff6e32e93950cc2ce889b4b176b4ce0ad6ab6ef6cd478e3820aabb`,
`51a29a1c46a0e928998f0dccb2d2b12ef15e5ed30a7d67e4b9d95d5f3f520b24`,
and
`8c56f23c7163fc51441fe6cef20fa7a303d7700c81e9e043d08690833ec7054e`.
The evidence is under `build/wric-chart-parallelization/tsan-4ef6126/`; its
`SHA256SUMS` digest is
`de70d1d6cf21ac25a0bbf8ee84c7f7a4a7e843967bb0d19cf4454bcc70ead612`.

The sanitizer rebuilds overlapped each other and the unrelated proof-assist
campaign; the normal and sanitizer tests also ran while unrelated host work
was present. Their elapsed times are operational diagnostics only. These
results closed the post-cleanup functional and sanitizer gates at `4ef6126`,
but no Phase-0, supplement, performance, scaling, RSS, automatic-policy, or
default gate. The later Phase-8 production changes postdate and reopened the
complete normal, ASAN, and targeted TSan gates. All three have since rerun and
passed on the exact bytes now committed as immutable product revision R, as
recorded below. P and then Q postdate R; Q has sealed clean normal, full serial
ASAN, and exact targeted TSan closure.

### Sealed Phase-8 generation attempt 0 (failed 2026-07-22)

The first production capture is a valid sealed failed attempt, not a pending
or discardable diagnostic. Historical run label `phase8-generation` is bound
to full product revision
`94a63238d25a8e3262428419d53f8f0986e8879b`; its immutable capture-tool
revision is `b6ae1a968c0a2374d8180e5682ae53775377c31e`. The product and capture-tool
roots are respectively
`/home/ogi-agent/matsen/larch2-wric-evidence/phase8-generation` and
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-b6ae1a9`; both are
detached at their exact revisions. The historical capture wrapper's hard-pinned
SHA-256 is
`e374ed726e026ab973ffa9ff385b13cd8c0eea8041d32b861df61fb4fa6c652e`.
The independently audited Phase-8 supplemental manifest is sealed at SHA-256
`773545d093904c20eaa80343615fa77089989735e61d34947530b07ef008cf17`.
Its frozen-oracle W1 characterization records `1044.731 ms` of candidate
generation and 256 post-dedup candidates, qualifying the named fixture against
the plan's minimum 100 ms and 256 candidates.

The five-repetition W1/W8 capture is preserved at
`/home/ogi-agent/matsen/larch2-wric-evidence/captures/phase8-generation/phase8-generation__workers-1-8__reps-5`.
Its `raw_trials.tsv` SHA-256 is
`e3eb3b3d9e77fc4006aa2c701102f39f84244971fae4f794bf66e5ca72cf4a56`.
Its independently audited `wric-evidence-run-ledger.tsv` SHA-256, also
recorded by its detached seal, is
`d22f42f31cf4e37bc92abc53e70f938d163f5fc422af68677c0f7dd7244841e1`.
A fresh read-only audit at `2026-07-23T00:18+03:00`, executed under the
physical-core affinity through the frozen b6 capture wrapper and repository
root, returned `status=audited`, `member_count=166`, and the same ledger
SHA-256. It changed no capture bytes.

| Attempt-0 candidate-generation metric | W1 | W8 | Decision |
|---|---:|---:|---|
| five-trial values (ms) | 49.089, 48.520, 48.628, 48.783, 49.819 | 61.907, 65.186, 61.478, 62.822, 64.901 | complete |
| median (ms) | 48.783 | 62.822 | W8/W1 = `1.287784679` |
| required W8/W1 | - | - | `<= 0.50`: **fail** |

All ten measured rows exited normally with `status=ok` and
`validation_status=ok`. W1 and W8 have identical search-semantic and
output-semantic SHA-256 values, validated objective 174, 256 generated and 256
scored candidates, and one exact verification. Both enumerate four sampled
sources and preassign/visit 405 moves, with the same nine discarded
source-speculative moves; the differing direct-projection counts are permitted
joined speculative tails rather than different canonical work. Thus the
supported semantic and canonical-work gates pass.

The maximum sampled RSS is 18,296 KiB at W1 and 18,516 KiB at W8, a W8/W1
ratio of approximately `1.012`; every measured row reports zero sampled swap,
no RSS-limit trigger, and no timeout. The supported paired-ratio and configured
RSS-limit checks therefore pass. The capture does not contain the separate
Phase-7 end-to-end comparison and makes no claim to close it. Whole-process W8
wall time being slightly lower than W1 also cannot substitute for the explicit
candidate-generation metric required by the exit criterion.

**Attempt-0 decision:** fail Phase 8. The `1.287784679` generation ratio is
above `0.50`, so capture execution stopped before the remaining campaign
components. This capture, its supplement, ledger, hashes, label, and product
revision remain permanent evidence and must not be removed, overwritten,
relabelled, or resealed to describe another executable.

Optimization evidence proceeds monotonically. The successor was required to
use the fresh `phase8-generation-retry1` label, pin new immutable product and
capture-tool identities, write a distinct capture directory and ledger, and
publish new external hash anchors. The sealed retry1 recorded below satisfies
those requirements as an additive successor while retaining attempt 0; it
does not alter this failed result or inherit its speed decision. Any further
attempt must increment the suffix and remain additive.

### Immutable optimized product revision R (locked 2026-07-23)

The accounting-complete optimized product is committed at exact revision
`07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c` (R). A clean detached worktree for
all retry1 product execution is locked at
`/home/ogi-agent/matsen/larch2-wric-evidence/phase8-generation-retry1-product`.
That root is detached at exact R with no worktree changes. Its product-local
`build/bin/dagutil` has SHA-256
`f3ccb220b698e0bd6bf0b2951e73d5204aa289c35aa18b5678169668cee12161`.

Revision R contains the optimized chart-SPR product and its tests and
documentation, but deliberately contains no additive retry capture or
later evaluator tooling. Those tooling changes are descendant work and
therefore cannot change the identity of the executable under measurement.

### Accounting-complete immutable-R diagnostic (non-accepting 2026-07-22)

The cleaned post-attempt-0 product retains the bounded source-wave and stable
ordinal contracts while deepening only the W>1 product projection wave to at
most 64 ordinals per worker; W1 retains its historical width four. Projection
uses four target ranges per worker. Once projection joins, a second unit-grain
scheduler operation performs postconstruction filters and constructs each
canonical dedup key before the serial gather alone publishes filter counters,
chooses the first observable failure, advances RNG/caps, invokes callbacks,
and fixes final candidate order.

The W>1 classic-locale path uses a fixed-width, count-delimited binary taxon
key with the exact equality relation of the historical textual key. Public and
cross-run signatures and W1 behavior retain the historical representation.
Standalone custom-locale generation falls back to that representation; finite
W>1 sampled admission rejects a custom global locale before work rather than
mixing an encoding outside its bound. The W>1 direct builder also records
after-certificate production references in candidate-owned storage during its
first lookup pass rather than repeating those lookups. The unified finite-
memory envelope charges retained per-slot key capacity, active-worker
postprocessing scratch, the postprocessing scheduler operation, and the binary
dedup-set node; runtime capacity backstops and exact pre-work budget rejection
remain in force.

No abandoned alternative is carried into the prospective product revision.
The slower unit-grain projection-range experiment was reverted, the
superseded fast textual signature family was removed after the binary key
replaced its only product use, and the unintegrated prepared-destination API
and tests were removed.

The quiet-host development command used one warmup and five measured W1/W8
pairs with full canonical correctness. Its build-local output is
`build/wric-chart-parallelization/phase8-accounting-complete-clean-diagnostic/`.

| Immutable-R candidate-generation metric | W1 | W8 | Diagnostic result |
|---|---:|---:|---|
| five-trial values (ms) | 48.927, 48.828, 48.617, 48.498, 50.252 | 19.292, 18.819, 19.997, 19.576, 19.707 | complete |
| median (ms) | 48.828 | 19.576 | W8/W1 = `0.400917506349` |
| implied speedup | - | - | `2.494278709x` |
| maximum process RSS (KiB) | 17,564 | 17,628 | W8/W1 = `1.003644` |

All ten measured rows exited normally with `status=ok` and
`validation_status=ok`, generated and scored 256 candidates, performed one
exact verification, and reported zero sampled swap. Every row has search-
semantic SHA-256
`be6e4a8819b89a419840395ba30bb63e323ecb36c6a57aba96555037bd0dcbf8`
and output-semantic SHA-256
`281ca02744aca9054a9ca328804a7ec4caca1402a82f237cf747d78a08caedb6`.
The measured executable SHA-256 is
`dd64567e43404c7bd86b80d2bd9319bb50d13b46974a40cf748274dd33d6fa5f`;
the `raw_trials.tsv` SHA-256 is
`4bfd1339ae5021a82c473dd247687cbaf4b12b2b534e8dcffc3c604ed12985cc`.

**Diagnostic decision:** this establishes performance headroom for immutable
product R, but it is not Phase-8 acceptance and is not attempt 1. At the time,
the run lacked the exact outer retry label, independent capture-tool identity,
metadata, audited ledger and seal, and external immutable hash anchors. The
later sealed retry1 below supplies those artifacts and supersedes attempt 0
for the current generation-speed decision without removing or rewriting the
failed historical evidence. Neither the diagnostic nor retry1 supplies the
separate Phase-7 end-to-end comparison. Full RelWithDebInfo CTest, serial ASAN
with leak detection, and the exact serial patched-runtime TSan matrix passed
on exact R as recorded below.

### Immutable-R full RelWithDebInfo closure (2026-07-22)

The immutable R tree built and passed the complete normal suite with
the exact commands

```sh
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --parallel 8
```

CTest registered 176 tests: 174 passed, zero failed, and exactly
`123 - merge_consistency_test` and `130 - rotaA_diagnostic_test` skipped for
their unchanged reasons. Total CTest time was 197.38 seconds.

This closes the full RelWithDebInfo gate for exact immutable product R. It does
not close Phase-8 performance/retry acceptance or any sealed evidence gate.
Any later product change reopens the normal gate.

### Immutable-R serial ASAN closure (2026-07-22)

The immutable R tree used the plan's exact ASAN configure, build, and
serial test commands:

```sh
cmake -S . -B build-asan \
  -DGCC_TOOLCHAIN=/home/ogi-agent/install/gcc-trunk \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_ASAN=ON \
  -DENABLE_TSAN=OFF
cmake --build build-asan --parallel 8
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 \
  ctest --test-dir build-asan --output-on-failure --parallel 1
```

CTest registered 176 tests: 174 passed, zero failed, and exactly
`123 - merge_consistency_test` and `130 - rotaA_diagnostic_test` skipped for
their unchanged reasons. Total CTest time was 842.54 seconds. A grep of
`build-asan/Testing/Temporary/LastTest.log` found no `AddressSanitizer`,
`LeakSanitizer`, or detected-leak diagnostic.

This closes the full serial ASAN gate for exact immutable product R. The
generation retry is sealed below; any later product change, including Q,
reopens current-product normal and ASAN gates.

### Immutable-R targeted TSan closure (2026-07-22)

The exact final regex selected the following 55-test inventory:

```sh
TSAN_REGEX='^(thread_pool_test|chart_parallel_test|chart_scheduler_test|parsimony_chart_test|chart_trim_test|chart_bnb_trim_apply_test|chart_spr_test|chart_spr_search_test|chart_spr_pipeline_test|chart_two_chart_oracle_test|multifurcation_chart_oracle_test|overlay_chain_test|inside_chart_cache_test|outside_chart_cache_test|chart_spr_phase10_test|dagutil_chart_spr_.*|dagutil_wric_phase9_chart_bnb_.*|dagutil_wric_lazy_chart_.*|dagutil_wric_lazy_polytomy_fixed_topology_search|larch2_chart_bnb_.*|wric_bnb_trim_benchmark_smoke)$'
ctest --test-dir build-tsan -N -R "$TSAN_REGEX"
```

The full numbered selection is preserved at
`build/wric-chart-parallelization/phase8-tsan-pre-r/inventory.log`, SHA-256
`b49508c42ca10a2bc94ed54bf826edb57abc0540867fda3676c31ae8db72da86`.
It contains exactly `Total Tests: 55`.

The serial run used the validated patched runtime through search-path
selection, not preloading:

```sh
LD_LIBRARY_PATH="$PWD/build/wric-chart-parallelization/phase3-7d294d6/patched-tsan-runtime:/home/ogi-agent/install/gcc-trunk/lib64" \
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-tsan --output-on-failure --parallel 1 \
    -R "$TSAN_REGEX"
```

`LD_PRELOAD` was not used. Representative `ldd` output preserved as
`build/wric-chart-parallelization/phase8-tsan-pre-r/ldd.log` resolves
`libtsan.so.2` to the Phase-3 patched runtime, then `libstdc++.so.6` and
`libgcc_s.so.1` to GCC-trunk `lib64`. Its SHA-256 is
`92616f6be163505716926f945ea73b5b3c3ec6b14d2a454c01f3b62fd41a4ba6`.
The resolved `libtsan.so.2.0.0` SHA-256 is
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.

All 55 selected tests passed serially with zero failures in 288.11 seconds.
The CTest output contains exactly 55 `Passed` records, and the preserved
LastTest log contains exactly 55 `<end of output>` records. Scanning both logs
finds no TSan warning, fatal, summary, race, lock-order, publication, or
thread-leak diagnostic. The artifacts and SHA-256 values are:

| Immutable-R TSan artifact (historical `pre-r` directory name) | SHA-256 |
|---|---|
| `build/wric-chart-parallelization/phase8-tsan-pre-r/targeted.ctest.log` | `09fc0239311c114810fbd7f6623452d6df3de90b9ecbd94cfbdde03860b7a25f` |
| `build/wric-chart-parallelization/phase8-tsan-pre-r/targeted.LastTest.log` | `18d2bc1c7552db146d4ed98d039cb4135f41204fa4fab8df3b04601de1ae5628` |

This closes the exact targeted TSan gate for immutable product R. The additive
retry1 is sealed below and passes its generation-speed gate. Q is a later
product and therefore requires fresh current-product normal, ASAN, and TSan
closure.

### Sealed optimized generation retry1 (passing 2026-07-23)

The additive `phase8-generation-retry1` observation is sealed on immutable
product R `07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c` and immutable capture tool
`0013fcc7231c32bce0fb5ccb11eaae62dacea4c5`. Their locked detached roots are
`/home/ogi-agent/matsen/larch2-wric-evidence/phase8-generation-retry1-product`
and
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-0013fcc`.
The distinct capture is
`/home/ogi-agent/matsen/larch2-wric-evidence/captures/phase8-generation-retry1/phase8-generation-retry1__workers-1-8__reps-5`.

| Retry1 metric | W1 | W8 | Decision |
|---|---:|---:|---|
| five candidate-generation values (ms) | 48.815, 49.128, 48.608, 48.905, 49.428 | 19.506, 20.004, 19.712, 20.723, 18.938 | complete |
| candidate-generation median (ms) | 48.905 | 19.712 | W8/W1 = `0.403067171046` |
| median wall time (s) | 0.151872 | 0.131355 | W8/W1 is approximately `0.865` |
| required generation ratio | - | - | `<= 0.50`: **pass** |

All measured rows completed with successful validation and the same
search/output semantic SHA-256 values as the R diagnostic. The immutable raw
TSV SHA-256 is
`0e5eff54263cb1d1c3da15ac8d21ec9d456993992b4a60189ae2105f1238e36a`;
the independently audited ledger and detached seal bind SHA-256
`5a307b51f1ba662b1484e1919f03a8b18f17af934522953e06c039ef90501fdd`.

**Retry1 decision:** pass the Phase-8 candidate-generation speed gate. Attempt
0 remains immutable failed evidence; retry1 is its additive current
generation observation. This does not by itself close the separate Phase-7
same-workload non-regression, all Q recaptures, pre-default/final evaluation,
default promotion, or final acceptance.

### Immutable Q-aware capture-tool C checkpoint (superseded before capture)

The historical capture/evaluation tool is immutable C,
`03ff9f12d0b479321ea1c887a3112fed1d34ef71` (`Pin Q acceptance capture
retries`), tree
`05d70b80cd05fd33d2fad4a42586c29066039191`, in locked detached root
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-03ff9f1`.

| Tool C artifact | SHA-256 |
|---|---|
| `tools/wric_benchmark_capture.py` | `786b8a17874c00f89693b6d8d399c447d6e5a79a144ab9a304be33dcb4133ce1` |
| `tools/wric_evidence_run_ledger.py` | `def3ce4b6868b16c050599b918240ba600e204dcf884caea36bffe52769cd29f` |
| `tools/wric_cross_phase_acceptance.py` | `4503d67e3804b83aab04da5b8787b4eb846819ec89745a1e5aecc4fbb43b72c1` |

The evaluator output is schema v5. It retains attempt 0 and retry1 with their
own historical product/tool roots and hard-pinned anchors, while all current
recapture labels are pinned to Q. Its explicit `pre-default` mode reports
`completion_eligible=false` and defers exactly `final-default-auto`; only
`final` requires that capture and can be completion-eligible. The loader
verifies canonical metadata, routed pre/post repository state, tracked wrapper
and ledger bytes, complete ledger closure and detached seals. It rejects
symlink/hard-link aliases, unsafe paths, and reused capture identities, then
rescans the closure to fail closed against TOCTOU changes.

The two focused registered capture/evaluator tests pass 2/2 on exact C. No Q
timed capture used C. A subsequent final dry-run audit found that C selected
its own sibling Phase-9 acceptance evaluator while passing Q as the product
root; the product evaluator correctly rejected that mixed route. C therefore
remains immutable failed tooling evidence and is superseded only by C2.

### Immutable Q-aware capture-tool C2 checkpoint (superseded before capture)

Historical capture/evaluation tool C2 is immutable commit
`af4f2011d8d462cceaab7efe1d443eaa4c207fc8` (`Bind Phase 9 acceptance to
product root`), tree
`3233c90da3d3f408e5bde32db57fa926b83dd9bd`, with C as its sole parent. Its
clean locked detached root is
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-af4f201`.

| Tool C2 artifact | SHA-256 |
|---|---|
| `tools/wric_benchmark_capture.py` | `786b8a17874c00f89693b6d8d399c447d6e5a79a144ab9a304be33dcb4133ce1` |
| `tools/wric_evidence_run_ledger.py` | `def3ce4b6868b16c050599b918240ba600e204dcf884caea36bffe52769cd29f` |
| `tools/wric_cross_phase_acceptance.py` | `84d1fdfec0dcec611c1128d2d7348a8c179d543ac3483e2985264683eaa861d0` |

Schema v6 requires the exact Q-local Phase-9 evaluator and binds its working
bytes and tracked HEAD blob to external SHA-256
`12eef19f3cad3dcd26363d6dfc951642cdd3b0647ddda98b6f6affb185314568`.
It checks clean Q repository and tool provenance before and after delegated
evaluation, uses a fixed interpreter environment that still permits the
authenticated sibling manifest helper but excludes ambient configuration and
ignored bytecode, rejects any stderr, validates the exact schema-v2 Phase-9
payload, and records it separately from product-tool provenance.

The direct product-root/isolation regression passes. Under the unchanged CTest
timeouts, `wric_cross_phase_acceptance_test` passed in 29.37 seconds and
`wric_benchmark_capture_test` passed in 38.86 seconds; both completed 2/2 in
38.87 seconds. This closes the current tooling checkpoint, not any performance
result. No timed Q capture used C2. A final capture-path audit found that C2's
Phase-9 seal/audit subprocesses could load a valid ignored sibling bootstrap
`.pyc` through the product tool's shebang. Because Git-clean and tracked-blob
checks do not authenticate ignored bytecode, C2 is superseded before capture.

### Historical immutable Q-aware capture-tool C3 checkpoint

Historical capture/evaluation tool C3 is immutable commit
`82aaec5813121230b82f45050b01e4bbd27a4b24` (`Isolate Phase 9 capture
postprocessor`), tree
`e0e25e1e58e2c5d7dc08fa04de079aa49c3dffda`, with C2 as its sole parent. Its
clean locked detached root is
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-82aaec5`.

| Tool C3 artifact | SHA-256 |
|---|---|
| `tools/wric_benchmark_capture.py` | `6ddfa017d03a035718a7a046c68bae76cfbe5ffb2b2578048165b76cb41730ec` |
| `tools/wric_evidence_run_ledger.py` | `def3ce4b6868b16c050599b918240ba600e204dcf884caea36bffe52769cd29f` |
| `tools/wric_cross_phase_acceptance.py` | `84d1fdfec0dcec611c1128d2d7348a8c179d543ac3483e2985264683eaa861d0` |

C3 runs both Phase-9 postprocessor commands under fixed
`-E -s -S -B -X pycache_prefix=/dev/null` interpreter isolation, retaining
the authenticated script directory for the tracked sibling source while
excluding ambient configuration, site hooks, and ignored cache reads/writes.
The regression installs an unchecked-hash poisoned sibling `.pyc`, proves an
unisolated interpreter reads it, and then proves capture plus two repeat audits
use tracked source without deleting or rewriting the poison. The Phase-9 class
passes 3/3; the full registered capture-controller entry passes in 42.99
seconds. C2's registered cross-phase test had already passed with the same
evaluator bytes. C3 was the sole permitted tool at this checkpoint; it was
superseded before measurement by C4.

The campaign driver is
`/home/ogi-agent/matsen/larch2-wric-evidence/run-q-c3-capture-campaign.sh`,
mode `0755`, nlink one, and SHA-256
`71e94f3103249be23a358c258053ca5531af955556032c912ebc5f7aee7c6557`.
`bash -n` and an independent read-only contract audit pass. The audit confirms
all 20 tuples exactly match C3, their expected row counts sum to 430, P/S/U
affinity semantics are correct, Phase-9 outer/inner ledgers remain distinct,
all destinations are new, and default-auto is intentionally absent. This C3
driver was not executed; C4's successor driver later completed and audited
the 20-component/430-row Q campaign.

This closed the historical C3 tooling checkpoint, not any performance result.
Q's targeted TSan and timed C4 campaign subsequently completed. C7's
pre-default invocation failed the four gaps recorded below; candidate-bound
pre-default/final evaluation and final acceptance remain pending.

## Phase 9 accepted-state update checkpoint (supplements sealed; performance pending)

Phase 9 has a **functionally passing implementation checkpoint; acceptance
pending the accepted-update timing decision and paired RSS** at `19e26db`
(`Clear aborted accept transaction evidence`). The immutable measurement
checkpoint remains `7ca527b8906d018124756182274335cbff936d72`, whose exact
subject is `Baseline measure`; it is an ancestor of this checkpoint and has
not been replaced.

Commits `45f2ebc`, `ef8f537`, `253c86d`, and `3eedb07` stage affected inside
and outside rows by pattern on the one search-lifetime scheduler, admit the
complete transaction before allocation or submission, use the exhaustively
checked tight outside dependency set, and move unaffected lazy state instead
of copying it. Commit publication remains single-writer and occurs only after
both cache barriers join. Commits `ff70a71` and `e62efb6` reuse a compatible
accepted exact frontier as the next old state and expose persistent inside-row
views to local scoring. The report distinguishes attempted selection from a
committed transaction; `19e26db` makes every accepted-but-uncommitted path
publish zero recomputed rows and an empty transaction signature while retaining
the canonical selected-candidate evidence.

The committed nontrivial fixture and reference have these immutable identities:

| Artifact | SHA-256 |
|---|---|
| `test/wric_chart_three_accepts.pb.gz` | `1df318b1ab7082acc1d14c00c8e4a3b243e5fa52b2207af854fe345305f740d9` |
| `test/wric_chart_three_accepts.ref` | `2ecea31dd7eb5ab2a77d35c30ada571514f84e462579c239cc32e639fae0ad05` |

It has 2,592 active patterns and produces three real local commits under the
frozen seed-1 32-candidate/top-K-4 contract. The current W1 report is
`build/wric-chart-parallelization/phase9-iteration-evidence/seed1-w1-12g.report.txt`,
SHA-256
`110d6e1ef1e529ed8ff3a2b7d650fba53cbbe622050fb370a284e80a6af16da5`.
Its objective chain is `15384 -> 13608 -> 11898 -> 10314`; accepted inside /
outside row counts are `12960 / 51840`, `12960 / 51840`, and
`10368 / 54432`. It reports 248,832 persistent-row-view pattern visits, three
accepted exact-trim reuses, zero reuse rejections, zero per-accept sidecar
rebuilds, and three committed transaction signatures. The frozen-oracle and
current output DAGs are byte-identical, SHA-256
`4c58feef9e4c3985acb935d29dd2ea6d8167e939caf5db291b9af48debd1b4c6`.

The score domains are intentionally separate. `15384 -> 10314` is the
active-pattern chart objective without UA-edge scoring. External canonical DAG
validation includes the UA convention and reports `11538`; the supplemental
manifest therefore records `expected_initial_score=16648`,
`expected_final_score=10314`, and `expected_validated_parsimony=11538` rather
than cross-comparing unlike scores.

The frozen W1 diagnostic report
`build/wric-chart-parallelization/diagnostics/phase9-2592-frozen/s1-w1.report.txt`
has SHA-256
`bb11a83a067faf9cb82737422bbb48ea98e78def6127796660c296e782aadcaa`.
It measured 179.878 ms of accepted update in 915602.498 ms total, or
0.019646%. This one row is diagnostic only. The required twelve-row
characterization and production manifests have since been sealed, but no
accepted-update speed exemption/decision or RSS claim is made here.

Commits `2f7f8f8` through `0f7b336` add and harden the supplemental acceptance
pipeline. It validates every captured report/output/identity hash and score
domain, uses output-scoped locking, durable parent creation and directory
syncs, Linux no-replace publication, inode-validated journals, private-bundle
audit before live publication, and ownership-safe recovery at every injected
crash boundary. Regression tests cover symlink/hardlink/forged journals,
foreign-target races, cross-control-path collisions, partial and complete
invalid journals, and post-rename recovery. The canonical
`build/wric-chart-parallelization/supplements/phase9-local-commit.characterization.tsv`
and `phase9-local-commit.tsv` bundles are now sealed at SHA-256
`8dc08f393bccb4c8a609501f90cb3c949f1b44600ef967fa8038edc7fd399ed1`
and
`2f332c5d459b8f47901df015427cba3d4ead7daedac56e1fd9873882b5b65b39`,
respectively. Their sealing does not itself decide the speed exemption or RSS
gate.

### Interim functional evidence

At `19e26db`, a coherent RelWithDebInfo rebuild followed by

```text
ctest --test-dir build --output-on-failure --no-tests=error \
  -R '^(chart_spr_pipeline_test|chart_spr_search_test|chart_spr_phase10_test)$'
```

passed 3/3 in 9.37 seconds (`8.69`, `0.22`, and `0.45` seconds). The search
matrix includes the non-vacuous local tombstone abort, conservative and local
post-materialization rejection evidence, W1/W8 multiparent transient-oracle
parity, three committed moves, exact masks, chain identity, and canonical full
sidecar parity. Earlier at `b648c42`, the targeted ASAN command for the
canonical CLI/report, bootstrap/acceptance, search, and persistent cache tests
passed 9/9 in 269.87 seconds without diagnostics. That sanitizer run predates
the latest report/matrix changes and is interim evidence only. The historical
post-cleanup complete normal/ASAN and exact targeted TSan gates are recorded
under the `4ef6126` closure section above; they include both persistent cache
tests and passed without diagnostics. The expanded Phase-9 production matrix
at that checkpoint covers multiparent and non-vacuous local multi-accept paths
at W1/2/4/8 with exact canonical parity and non-vacuous scheduler evidence.
The Phase-8 production changes now pinned as immutable R reopened all three
global gates. The full normal, serial ASAN, and exact targeted TSan gates have
since rerun and passed on exact R. P and then Q postdate R; Q has sealed clean
normal, full serial ASAN, and exact targeted TSan closure.

Phase 9 remains open for the accepted-update exemption or 1.5x speed decision,
and paired RSS. A later conditional default change would require the affected
test and sanitizer gates to be rerun at the default-change descendant.

## Phase-7/9 evidence-production tooling checkpoint

Commits `0f099ca9adc7d43a601e2a9f52dd710de18e2136` (`Produce sealed
Phase 9 characterization`),
`3359fbf359576c48cc0de88ded0319b549f2c310` (`Complete Phase 7
workload matrix`), and `c5156a84c67acce2e40d2a3c3115d89738f40cd4`
(`Register Phase 7 completion bootstrap test`) are **evidence-production and
tooling checkpoints only**. Those commits did not themselves contain a
production supplemental manifest or timing/RSS acceptance evidence. The
production Phase-0 base and all five Phase-7/8/9 supplement manifests were
subsequently sealed; every later-phase performance gate remains open.

### Phase-9 characterization producer

Commit `0f099ca` adds a `characterize` command to
`tools/wric_phase9_manifest_bootstrap.py`. It runs the sealed Phase-0 oracle
through the explicitly hash-bound process-metrics runner for the exact
`seed={1,7,19} x workers={1,2,4,8}` matrix. Each search and output validation
uses the 1,800-second timeout, 16-GiB RSS limit, and 12-GiB chart-memory
contract. The persistent capture binds the sealed base hash, fixture hash,
oracle hash, affinity, runner hash, canonical argv and trial digests, reports,
full canonical streams, output digests, receipts, restart status, and exact row
closure.

The producer publishes a schema-v3 characterization TSV, detached seal, and a
hash-addressed immutable asset directory whose closure name is the SHA-256 of
its canonical asset ledger. Publication uses the existing journaled
assets--manifest--seal order, no-replace renames, private audit, and restart
recovery. The later `build` command independently reruns all twelve
frozen-oracle rows and requires them to match this source characterization
before it can publish `phase9-local-commit.tsv`; producing the source TSV alone
does not satisfy the Phase-9 supplement or acceptance gates.

### Phase-7 completion producer

Commit `3359fbf` adds
`tools/wric_phase7_completion_manifest_bootstrap.py`. Its immutable output
basename and manifest ID are `phase7-lazy-completion.tsv` and
`phase7-lazy-completion`. It produces only the eight rows that are not already
owned by the sealed Phase-0 matrix:

- group `phase7-lazy-medium-auto`: seedtree `lazy=auto` at W1/2/4/8;
- group `phase7-lazy-small-on`: exact
  `data/test_5_trees/tree_0.pb.gz` `lazy=on` at W1/2/4/8.

The medium rows are not guessed from historical wall times. At every worker
count, the producer runs the explicitly hash-bound current working chart under
the 600-second runner, reads the reported resolved auto policy, and requires
its canonical search/output semantics to equal the matching sealed Phase-0
forced branch. It re-derives the manifest semantics from the exact Phase-0
`p0-medium-dense64-*` or `p0-medium-lazy64-*` row. The small rows are the only
new frozen-oracle rows; each is compared with its exact sealed
`p0-small-dense64-*` peer. The builder verifies all twelve source row IDs and
resolution contracts, rejects resolver or row-ID collisions, binds the sealed
base, runner, affinity, fixtures, working-chart hash, canonical argv/trial
digests and complete assets, and uses the same crash-durable no-replace
publication and production-harness sentinel. Commit `c5156a8` registers its
focused integration as `wric_phase7_completion_manifest_bootstrap_test` with a
120-second CTest timeout.

### Functional tooling tests

At `c5156a8`, the exact focused command was:

```bash
ctest --test-dir build --output-on-failure --no-tests=error \
  -R '^(wric_phase9_manifest_bootstrap_test|wric_phase78_manifest_bootstrap_test|wric_phase7_completion_manifest_bootstrap_test)$'
```

All 3/3 tests passed. The individual CTest times were 79.14 seconds for the
Phase-9 bootstrap, 12.34 seconds for the Phase-7/8 bootstrap, and 10.60 seconds
for the Phase-7 completion bootstrap. At that checkpoint, the ephemeral
`build/Testing/Temporary/LastTest.log` had SHA-256
`6e5d29ad8de5862829e493e602ad552c460dc8dec3071d81f78f0bf326c85d89`;
later CTest runs are expected to overwrite this temporary log, so it is not a
durable acceptance artifact.
The completion test covers the exact eight-row/group closure, seventeen
600-second-runner receipts, wrong-binary rejection before execution, canonical
and status tampering, resolver collision, foreign-target preservation, and
roll-forward after an injected manifest-publication crash. These elapsed test
times measure synthetic regression tests, not chart-SPR performance.

### Future production invocations

Run these only after Phase 0 has been prepared, captured, approved, finalized,
audited, and sealed. `SEALED_REPO_ROOT` must remain the persistent absolute
repository root recorded by Phase 0; do not move it or substitute the optimized
worktree. Replace every angle-bracketed value with the independently recorded
sealed value before running the hash checks.

```bash
set -euo pipefail

SEALED_REPO_ROOT='<persistent absolute Phase-0 repository root>'
BASE='<absolute path to sealed Phase-0 workloads.tsv>'
BASE_SHA='<sealed Phase-0 workloads.tsv SHA-256>'
RUNNER='<absolute path to the Phase-0-frozen wric-process-metrics>'
RUNNER_SHA='<Phase-0-frozen process-metrics SHA-256>'
AFFINITY='0,2,4,6,8,10,12,14'
SUPPLEMENTS='<absolute persistent supplements directory>'
HARNESS="$PWD/tools/wric_spr_search_benchmark.sh"
WORKING_CHART="$PWD/build/bin/dagutil"
WORKING_CHART_SHA='<qualified current working-chart SHA-256>'
PHASE9_FIXTURE="$PWD/test/wric_chart_three_accepts.pb.gz"
PHASE9_FIXTURE_SHA='1df318b1ab7082acc1d14c00c8e4a3b243e5fa52b2207af854fe345305f740d9'

test "$(sha256sum "$BASE" | awk '{print $1}')" = "$BASE_SHA"
test "$(sha256sum "$RUNNER" | awk '{print $1}')" = "$RUNNER_SHA"
test "$(sha256sum "$WORKING_CHART" | awk '{print $1}')" = "$WORKING_CHART_SHA"
test "$(sha256sum "$PHASE9_FIXTURE" | awk '{print $1}')" = "$PHASE9_FIXTURE_SHA"
(cd "$(dirname "$BASE")" && sha256sum --check --strict "$(basename "$BASE").sha256")
```

Produce the sealed Phase-9 source characterization, then independently build
and audit the Phase-9 supplement:

```bash
tools/wric_phase9_manifest_bootstrap.py characterize \
  --base-manifest "$BASE" \
  --expected-parent-sha256 "$BASE_SHA" \
  --fixture "$PHASE9_FIXTURE" \
  --expected-fixture-sha256 "$PHASE9_FIXTURE_SHA" \
  --affinity-cpus "$AFFINITY" \
  --capture-dir "$SUPPLEMENTS/phase9-characterization.capture" \
  --output "$SUPPLEMENTS/phase9-local-commit.characterization.tsv" \
  --repo-root "$SEALED_REPO_ROOT" \
  --process-metrics "$RUNNER" \
  --expected-process-metrics-sha256 "$RUNNER_SHA"

tools/wric_phase9_manifest_bootstrap.py build \
  --base-manifest "$BASE" \
  --expected-parent-sha256 "$BASE_SHA" \
  --characterization "$SUPPLEMENTS/phase9-local-commit.characterization.tsv" \
  --fixture "$PHASE9_FIXTURE" \
  --expected-fixture-sha256 "$PHASE9_FIXTURE_SHA" \
  --capture-dir "$SUPPLEMENTS/phase9-local-commit.capture" \
  --output "$SUPPLEMENTS/phase9-local-commit.tsv" \
  --repo-root "$SEALED_REPO_ROOT" \
  --benchmark-harness "$HARNESS" \
  --process-metrics "$RUNNER"

tools/wric_phase9_manifest_bootstrap.py audit \
  --base-manifest "$BASE" \
  --expected-parent-sha256 "$BASE_SHA" \
  --supplement "$SUPPLEMENTS/phase9-local-commit.tsv" \
  --repo-root "$SEALED_REPO_ROOT" \
  --benchmark-harness "$HARNESS" \
  --process-metrics "$RUNNER"
```

Produce and audit the separate Phase-7 completion supplement:

```bash
tools/wric_phase7_completion_manifest_bootstrap.py build \
  --base-manifest "$BASE" \
  --expected-parent-sha256 "$BASE_SHA" \
  --repo-root "$SEALED_REPO_ROOT" \
  --benchmark-harness "$HARNESS" \
  --process-metrics "$RUNNER" \
  --expected-process-metrics-sha256 "$RUNNER_SHA" \
  --working-chart "$WORKING_CHART" \
  --expected-working-chart-sha256 "$WORKING_CHART_SHA" \
  --capture-dir "$SUPPLEMENTS/phase7-lazy-completion.capture" \
  --output "$SUPPLEMENTS/phase7-lazy-completion.tsv" \
  --affinity-cpus "$AFFINITY"

tools/wric_phase7_completion_manifest_bootstrap.py audit \
  --base-manifest "$BASE" \
  --expected-parent-sha256 "$BASE_SHA" \
  --repo-root "$SEALED_REPO_ROOT" \
  --benchmark-harness "$HARNESS" \
  --process-metrics "$RUNNER" \
  --expected-process-metrics-sha256 "$RUNNER_SHA" \
  --working-chart "$WORKING_CHART" \
  --expected-working-chart-sha256 "$WORKING_CHART_SHA" \
  --supplement "$SUPPLEMENTS/phase7-lazy-completion.tsv"
```

Successful completion of these commands would produce auditable input evidence
for later benchmark runs. It would not by itself pass Phase 0, the Phase-7 or
Phase-9 timing/RSS gates, full RelWithDebInfo CTest, ASAN, TSan, or final parity.

## Historical benchmark-capture preparation (setup only)

To shorten the post-Phase-0 critical path without creating premature timing
evidence, clean detached historical product roots and capture-compatible
RelWithDebInfo builds were prepared under
`/home/ogi-agent/matsen/larch2-wric-evidence` (abbreviated `$E` below). No
benchmark capture, supplement, timing run, or Phase-0 operation was performed.

The clean detached capture-tool root is `$E/capture-tool-b6ae1a9` at revision
`b6ae1a968c0a2374d8180e5682ae53775377c31e`, tree
`6feae616b0eb8e6c6c0ac0887cdc3ff341e550f3`. Its raw tracked-root audit
observed 1,081 files and 107,252,099 bytes with digest
`ca13ad1d8b80a7e9601879d23fe4e26e8531f50eb38c1f926066f52984869c8e`.
The capture wrapper, compatibility auditor, and evidence ledger SHA-256 values
are, respectively,
`e374ed726e026ab973ffa9ff385b13cd8c0eea8041d32b861df61fb4fa6c652e`,
`22f526a08bc20825ed66ca679a3d4121e5f0c5d83f181f6d27fe42397c3e3075`,
and
`def3ce4b6868b16c050599b918240ba600e204dcf884caea36bffe52769cd29f`.

| Run label(s) / root | Product revision | `dagutil` SHA-256 | Harness SHA-256 | Metadata SHA-256 |
|---|---|---|---|---|
| `phase1` / `$E/phase1` | `208ce23f0c005d3702d114f535fe21564b3b79b6` | `4f39d997ecc48c2896778e0d31e88cfa6744c1eaa48914e76e8e4f4cbd2b056b` | `2fa15aeb1c34de8b018b3ba079ca4fb2a117baf53bc59feb2dabd67d0161d795` | `737812a139fd47e237f1b0673ef7996b344f84ec880a53eb4bc9aee8b7ddffc0` |
| `phase2` / `$E/phase2` | `0c4623ba1793395ae8f5c3df2a2524a27d89bc80` | `6827c53e3838715a308d1cc039986033651489aece73de864bbbe410856c02e4` | `2fa15aeb1c34de8b018b3ba079ca4fb2a117baf53bc59feb2dabd67d0161d795` | `a0b65b91d0182eea8b91929f2542b26761857a8232320e4d82c220a94e3ef686` |
| `phase3` / `$E/phase3` | `7d294d68eaadc8c55b92be4f5589278c8a2f78f2` | `73042280f79250c12fde8f408eacfa9229a9a82037d29197b62bb1d97cb8461c` | `2fa15aeb1c34de8b018b3ba079ca4fb2a117baf53bc59feb2dabd67d0161d795` | `c0fbad76a0c44b317f0a9a5218ed89ea9352c0dc65385a4f64597c0347dbc50f` |
| `phase4` / `$E/phase4` | `cbf92b62284b2a93e506f59187ac94a5336b0be3` | `1c5995c1b367264ec0d099d2590924da83dc9ba460a55951e4f96d31d2415b07` | `2fa15aeb1c34de8b018b3ba079ca4fb2a117baf53bc59feb2dabd67d0161d795` | `c4bf4f13fc7952c0eaddaaa087b5652c19f53de12ef976848f812fdb43fc6de2` |
| `phase5` / `$E/phase5` | `3a10e9cc45050f7a6f846f9f1adb8d5f4157f9a5` | `e6d2c83abb97c119a7799706cdc9dc1a50c9910eacedc8d0226da1b318147dc3` | `2fa15aeb1c34de8b018b3ba079ca4fb2a117baf53bc59feb2dabd67d0161d795` | `73d8dea53f65a6ba9d675f08909e78aedb29c485fab92a55dad642ee2bb0a64a` |
| `phase6` / `$E/phase6` | `870c298ff1c0c21901bdf79d341bf97d121f389c` | `ee69fde20f1daba3c23928bb62e3d2249d511c80d07d05cf97d21605185d0da2` | `17d178cb27e3f500fdbe5b95ada89282ce537630907b4796a7f6e00945165068` | `af5d27f2c472ae14eabb6369436789b737586fb31640bd22dd84d686451a484d` |
| `phase7-high`, `phase7-small`, `phase7-auto`, `phase8-end-to-end` / `$E/phase7` | `38e9a281396e5263647ba68724414848841525d7` | `447c24d9f18291fdcc07d8d209d11ff6a44b0f905dd20bd035370cc04d672901` | `ed089af213f7f6773a252908dc3111a4d86bf01c85f12ad9d3a846ec92b3770f` | `6163b746551a058dd6cd8faf8f0ca2cfd35c3b6710e75635a51885b1610b07a1` |
| `phase8-generation` / `$E/phase8-generation` | `94a63238d25a8e3262428419d53f8f0986e8879b` | `55065540af091ee28e555601101505fabf36d6b901c40171b2fbc53cc85c7933` | tracked `ed089af213f7f6773a252908dc3111a4d86bf01c85f12ad9d3a846ec92b3770f` | none (`-`) |

Every listed product root was clean and detached at inventory time and its
capture-compatible build passed cache/recipe/flag/link validation. Each
generated Phase-1--7 compatibility pair re-audited with status `ok` against
the capture-tool root; `phase8-generation` uses the tracked harness byte
directly and therefore has no generated compatibility metadata. The common
compatibility transformation-spec SHA-256 is
`c53e4f57b685dfba70b0cc781cda00e9ff4ca5b3c41329cde752931fe84fe697`.
The compatibility harnesses are singly linked mode `0555` and their metadata
mode `0444`, but the roots and builds remain owner-writable preparation state,
not sealed evidence. The capture wrapper must revalidate and bind all live
provenance at capture time; any mutation requires a fresh inventory.

The Phase-7 completion supplement must use exact `$E/phase7` as its fixture
source at revision `38e9a28`, because its lazy fixtures are absent from the
frozen Phase-0 root. The old diagnostic Phase-8 roots at `6c8d0c7` and
`a21ab7a` must not be used for `phase8-generation`; the selected product is the
`94a6323` root above. Phase 0 and the five later-phase manifests have since
been sealed with the hashes recorded in the status table. Timing, RSS,
scaling, parity, and all later-phase acceptance decisions remain open.

## Phase 10 integration checkpoint (in progress)

Commit `e3ef48e` exposes the generation-phase, evidence-phase, and retained
ranked-candidate-exact-evidence byte maxima needed to audit the finite lazy
iteration envelope over time. The historical frozen counter table changed by
exactly three additive zero-valued rows and no deletion; a static regression
test prevents recapturing or otherwise rewriting that baseline. Real finite
lazy tests require nonzero values, summary/counter equality, the envelope as
the maximum of mutually exclusive temporal phases, and exact `E` success /
`E-1` pre-work rejection.

Commit `37b2f18` covers exact hybrid search for seeds `1,7,19` and workers
`1,2,4,8`, including randomized/reservoir source selection, 12 scored
candidates, four grammar-exact evidence records, a real `2 -> 1` commit,
scheduler reconciliation, and W1-identical canonical digest/full sidecar per
seed. Commit `80210de` adds dynamically resolved explicit-auto equivalence,
W16-versus-W1 semantics, the three-accept conservative W1/2/4/8 matrix, and
fixed-topology multifurcation W1/W8 parity with an arity-three witness. Commit
`19e26db` adds the corresponding abort-transaction hygiene and multiparent
oracle coverage.

Commit `4ef6126607c8da5f25d54928ea2fd6d152b89e29` completes the
cleanup review and required in-tree semantic matrix. The review removed the
obsolete, zero-only
`chart_spr_search_state::selected_topology_cache_admitted_bytes` diagnostic;
selected caches are task-local and no behavior or admission decision depended
on it. This public state-surface removal is not claimed to preserve source or
ABI compatibility. No superseded per-batch pool, duplicate production chart
builder, unused scratch copy, or temporary compatibility execution path
remained. The persistent search-lifetime scheduler, bounded source-wave
producer, supported public source-compatibility wrappers, and cold/oracle
fallbacks remain active intentional contracts.

The completed in-tree matrix adds W1/2/4/8 production-path coverage for
automatic dense construction, multiparent transient verification,
non-vacuous three-accept local commits, and fixed-topology multifurcation. The
scheduled multisite frontier covers `score_ua_edge={false,true}` by all four
dominance modes by bound pruning `{false,true}` by W1/2/4/8. Every parallel
count matches W1, including canonical digest/full sidecar where applicable,
and proves exact worker binding, scheduler dispatch/tasks, pool lifetime, and
shutdown. Existing deterministic overlap cases in `chart_trim_test`,
`chart_spr_search_test`, and `chart_scheduler_test` supply the required active
worker high-water witnesses without flaky timing assertions. The exact
focused 11/11 gate and historical full normal/ASAN/TSan evidence are recorded
in the `4ef6126` post-cleanup closure section above.

These commits are correctness and observability checkpoints, not Phase-10
acceptance. They close only the cleanup review and in-tree functional
mode/worker matrix at that checkpoint. Phase 0 and the later-phase supplement
manifests are sealed. Q later obtained clean normal, full serial ASAN, and
exact targeted TSan closure, and C4 captured and audited the worker-policy,
RSS, primary, scaling, unpinned-auto, stress, and bounded-real matrices. C7's
sole pre-default invocation nevertheless failed closed; the diagnosis below
identified exactly four performance gaps. The dirty optimized candidate
diagnostics pass those four limits, but no candidate-bound acceptance result
exists yet.

At this checkpoint the product default stayed serial because explicit auto had
not yet passed every prerequisite; no default-worker performance claim had
been made. The later conditional promotion still requires every affected
normal and sanitizer gate to pass again at its descendant.

## Post-Q pre-default gap closure (diagnostic; authoritative recapture pending)

Let `$E` denote
`/home/ogi-agent/matsen/larch2-wric-evidence` and `$D` denote
`$E/diagnostics`.

### Product-Q/C4 capture and C7 decision

Product Q remains
`a9db72e60f153a95362db544107373817a58a258`. Capture tool C4 is
`be5e4a025c357d22fb1a6b41f544e35f2d8a5891`, tree
`222a373a3e8c12ff7c72107645fc7eed0c70b6bb`. Its retry-2 campaign completed
and individually audited all 20 required components, totaling 430 measured
rows. The component paths, row counts, and outer ledger SHA-256 values are
recorded in `$E/q-c4-retry2-full-campaign.log`; the captures and their audit
receipts are under `$E/captures/`.

The separately sealed Phase-4 contention investigation is under
`$E/phase4-contention-profile-a9db72e/`. It classified the system/user CPU
trigger as short-run fixed overhead plus parallel contention, not a scaling
blocker, and concluded
`no_rollback_required_for_bound_capture`. The sealed acceptance timings remain
authoritative: dense local scoring fell from `29.445 ms` at W1 to `8.031 ms`
at W8 (3.67x), construction fell from `58.846 ms` to `10.071 ms` (5.84x),
and Phase-4 W1 was `0.9642` of Phase-3 W1. W8 RSS was below W1 in both
matrices. The diagnostic/profile timings were not substituted into
acceptance.

Analysis tool C7 is
`6e5fccfd8553b641f18c668025a806b2dc30826b`, tree
`6512c8123202fbac64f05dcff4a48e72b25ff2de`. Its schema-v6 cross-phase
evaluator SHA-256 is
`638fc37d789babd701ef3f86e2bb0e9052a4f5b24ce23097dace749d6351848a`.
The C7 pre-default controller was invoked exactly once against the complete
Q/C4 campaign. It failed closed on the first failing gate and preserved its
no-clobber staging files:

- result partial:
  `$E/captures/q-c4-c7-pre-default-evaluation.result.json.partial`, zero
  bytes, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`;
- stderr partial:
  `$E/captures/q-c4-c7-pre-default-evaluation.stderr.partial`, SHA-256
  `0d01536b496685a4d9c8855c07eb820f1473b69b338450aedf79ecde79491057`.

The preserved stderr reports only the first failure because the evaluator is
fail-fast. A gate-by-gate diagnosis of the same sealed rows found exactly four
performance gaps:

| Gate | Q W1 median (s) | Q W8 median (s) | W8/W1 | Limit | Result |
|---|---:|---:|---:|---:|---|
| Phase-7 high-compression forced lazy | 0.192696926 | 0.131555491 | `0.682706744372248` | `2/3` | fail |
| Final physical scaling, grammar exact | 1.321053506 | 0.680584192 | `0.515182911902434` | `0.50` | fail |
| Final primary, grammar exact | 1.321595265 | 0.684191774 | `0.517701441673976` | `0.50` | fail |
| Final stress, sampled-tree fixed topology | 23.718979292 | 42.840240608 | `1.806158691763320` | `1.00` | fail |

All other pre-default gates passed that diagnosis. The partials must remain
untouched; they are not a reusable evaluator slot.

### Optimization iterations after the C7 diagnosis

The following measurements are exploratory diagnostics from a dirty working
tree based on C7. They preserve executable and canonical hashes but are not
substitutes for an immutable candidate capture, full tests, sanitizers, or the
strict evaluator.

The retained implementation combines:

- binary two-child recurrence fast paths, while preserving the generic checked
  multifurcation path;
- caller-owned reusable restricted-topology memo and overlay scratch;
- exact-frontier liveness accounting using immutable-plan child occurrences,
  with nonroot frontiers released only after their final consumer and only
  after a scheduled dependency level joins;
- a three-candidate cold exact outer burst;
- a two-candidate bandwidth cap for the built-in dense fixed-topology verifier
  at at least 1,024 active patterns, with smaller workloads and callback
  verifiers bypassing the cap;
- one-time candidate lazy-context flattening, stable wide-key LSD radix
  grouping, zero-byte pass elision, and an automatic W8 batch of 30;
- finite-memory accounting for reusable fixed-topology scratch and lazy-context
  vector capacity;
- checked grammar/execution-plan identity before dense, lazy, or fixed-topology
  dispatch;
- W>1 fusion of compact-genome source-fingerprint collection into site-pattern
  construction, while retaining the historical W1 path and the public
  three-argument `build_site_patterns` API.

#### Primary grammar exact

A three-candidate cold burst first brought the primary grammar ratio to the
threshold but left insufficient margin and exceeded the RSS ratio:

| Diagnostic | W1 median wall (s) | W8 median wall (s) | W8/W1 | Median `max_rss_kb` W1/W8 | RSS ratio |
|---|---:|---:|---:|---:|---:|
| `primary-grammar-burst3-draft{1,2,3}-w{1,8}` | 1.170888945 | 0.585510088 | `0.500056039046` | 85,292 / 176,176 | `2.065563007082` |
| `primary-grammar-reclaim3-r{1,2,3}-w{1,8}` | 1.140849370 | 0.564544153 | `0.494845479031` | 77,252 / 144,492 | `1.870398177393` |

The second row includes consumed-frontier reclamation and passes both the
`<= 0.50` wall ratio and `<= 2.0` RSS ratio diagnostically. Its executable
SHA-256 is
`658567411a0f889d38069a26d1e47c61abbd0dc3f7e9128ce23bbdba53c6d076`.
All six W1/W8 canonical files have SHA-256
`9de2fa7254d688e8fb55e4b94650352e70f4521e8e77e00e182f390aba7f79d8`.

#### Sampled-tree fixed-topology stress

Scratch reuse alone exposed memory-bandwidth saturation: the single W1 and W8
diagnostics were `15.157949032 s` and `22.384412405 s`, respectively. A
two-candidate verifier cap reduced the W8 observation to `15.136242571 s`.
The cap was retained, but the accompanying experimental latch/rendezvous was
removed: review found an unsafe stack-lifetime race, and the latch itself
provided no performance benefit.

The subsequent three-pair campaign is under
`$D/stress-fixed-reclaim-futures-r{1,2,3}-w{1,8}`:

| Metric | W1 median | W8 median | W8/W1 |
|---|---:|---:|---:|
| wall (s) | 15.479752045 | 14.937815446 | `0.964990615003` |

This diagnostically passes the workload’s `<= 1.00` non-regression gate. The
executable SHA-256 is
`658567411a0f889d38069a26d1e47c61abbd0dc3f7e9128ce23bbdba53c6d076`,
and every canonical file has SHA-256
`e8959afb7acf5d67e92ed60efd60577e5d5bcd53c6496a464c561344a909269c`.

#### Phase-7 high-compression lazy width

All complete campaigns use the same high-compression fixture SHA-256
`e103be6cd1df36e5a002ae9dd9ffb53110b35c840eed1d8fa867475ad3109874`
and produced the same canonical SHA-256
`ae62f9153f5a0f8af0646db23e08088790c727f4a7990cbe1a6a5e71d97b84c6`.

| Iteration and artifact directory | Paired repetitions | W1 median (s) | W8 median (s) | W8/W1 | Disposition |
|---|---:|---:|---:|---:|---|
| `phase7-width-cap6-q-c7-r2` | 9 | 0.192382710 | 0.131268303 | `0.682329004514` | cap tuning rejected |
| `phase7-width-cap8-q-c7` | 9 | 0.192283396 | 0.131416506 | `0.683452179095` | cap tuning rejected |
| `phase7-width-flattened-q-c7-draft1` | 9 | 0.192424824 | 0.131503249 | `0.683400645854` | flattening retained for structural reuse; insufficient alone |
| `phase7-width-latch-flat-cap2-draft2` | 9 | 0.192106768 | 0.131569737 | `0.684878197524` | latch rejected and reverted |
| `phase7-width-radix-batch30-futures-draft3` | 10 | 0.192461142 | 0.131503947 | `0.683275312790` | radix/batching retained; still fails `2/3` |
| `phase7-width-tuple-fixed-broad-draft1` | 5 | 0.192282628 | 0.131456665 | `0.683663762906` | fixed-width tuple specialization rejected and fully reverted |
| `phase7-width-fingerprint-fusion-draft1` | 7 | 0.192369022 | 0.121196700 | `0.630021916938` | retained; diagnostic pass |

The retained fingerprint-fusion campaign uses executable SHA-256
`650e2709e98756d44049468b5de2c8fd5c1a45df9abaeeb710c058525aeca268`.
Its recorded median `max_rss_kb` values are 70,488 at W1 and 73,112 at W8,
ratio `1.037226194530`. Recorded component medians are:

| Width | `total_ms` | `cache_build_ms` | `initial_chart_construction_ms` | `local_scoring_ms` |
|---|---:|---:|---:|---:|
| W1 | 148.755 | 84.240 | 47.660 | 44.891 |
| W8 | 80.922 | 50.803 | 18.267 | 13.396 |

Relative to the preceding radix/batch campaign, fusion leaves W1 essentially
unchanged (`0.999521357927` ratio) and reduces W8 wall to
`0.921620246121` of its prior median. It avoids a second reachable-DAG and
compact-genome walk by collecting exact per-taxon compact-genome hashes during
the existing parallel site-pattern traversal and performing only canonical
fingerprint finalization afterward.

Tests added with this candidate cover reusable-scratch recovery after failure,
frontier reclamation with shared children and scheduled levels, provenance
retention, fixed-topology cap and bypass behavior, the three-candidate cold
burst, stale-plan rejection, finite `E`/`E-1` allocation accounting,
wide-radix semantic/allocation oracles, duplicate-leaf fingerprint parity, and
transactional optional metadata publication.

#### Normalized pre-commit confirmation

After removing formatter-only churn, repository-style `clang-format`
normalization was byte-identical before and after for every cleaned source
file.  The normalized working binary has SHA-256
`4aba2df83629419ea11227742f34b13befc61fb7bf004610a57d7c2341ed21bd`.
The focused five-test set (`site_patterns_test`, `lazy_key_grouping_test`,
`chart_trim_test`, `chart_spr_search_test`, and
`chart_spr_allocation_test`) passed 5/5; its log is
`$E/candidate-focused/ctest-final.log`.

Fresh normalized diagnostics retained margin on all four previously failing
gates:

| Diagnostic | W1 median (s) | W8 median (s) | W8/W1 | Result |
|---|---:|---:|---:|---|
| Phase-7 high forced lazy, 7 pairs | 0.192034901 | 0.121367114 | `0.632005501958` | pass (`<= 2/3`) |
| Primary grammar exact, 3 pairs | 1.170644780 | 0.575482835 | `0.491594756011` | pass (`<= 0.50`) |
| Stress sampled-tree fixed, 3 pairs | 15.717782112 | 15.418867137 | `0.980982369340` | pass (`<= 1.00`) |

The primary grammar median RSS values were 77,444 KiB at W1 and 144,484 KiB
at W8, ratio `1.865657765611`, below the `2.0` limit.  Raw diagnostic roots are
`$D/phase7-width-phase7-width-normalized-final`,
`$D/primary-grammar-normalized-final-r{1,2,3}-w{1,8}`, and
`$D/stress-fixed-normalized-final-r{1,2,3}-w{1,8}`.  These remain diagnostic
until the revision is committed and the strict candidate-bound campaign is
captured and evaluated.

### Remaining authoritative gates

None of the diagnostics above changes a phase status by itself. The following
records remain to be appended after they actually complete:

| Gate | Pending authoritative record |
|---|---|
| Immutable optimized candidate | revision, tree, clean root, executable hashes, and committed implementation/test scope |
| Correctness | focused tests and full 176-test RelWithDebInfo result |
| Sanitizers | full serial ASAN/LSan result and exact 55-test targeted TSan result, with clean scans |
| Candidate timing campaign | fresh candidate-bound 20-component capture, row count, audit receipts, ledgers, and hashes |
| Pre-default decision | a new no-clobber strict evaluator result proving every gate passes |
| Conditional default promotion | descendant commit changing omitted workers only after the pre-default pass |
| Post-default closure | default/explicit-auto capture, final evaluator, repeated normal/ASAN/TSan gates, and final artifact identities |

This diagnostic section records the pre-promotion state: its serial-default
statement applies only until an immutable candidate passes the strict
pre-default evaluator. The authoritative promotion and post-default closure
are recorded separately below rather than rewriting this history.

## Later-phase evidence template

Before the Phase-0 seal, every later-phase measurement is labelled
`diagnostic; acceptance pending Phase 0` and cannot satisfy an exit criterion.

For each Phase 1--10 result, append an immutable section containing:

1. revision and dirty status;
2. exact command/environment and raw-artifact path;
3. executable, command, manifest, and fixture hashes;
4. report/work counters proving that the intended path ran;
5. one-worker canonical comparison and parallel high-water proof;
6. wall/user/system/RSS rows and phase-specific acceptance arithmetic;
7. normal, focused, ASAN, and TSAN test evidence as required; and
8. an explicit pass/fail decision for every exit criterion.

Do not replace prior-phase numbers. If an artifact or conclusion is corrected,
append the correction with its reason and new hash.

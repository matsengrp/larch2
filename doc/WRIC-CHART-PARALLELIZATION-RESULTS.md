# WRIC chart parallelization: results and evidence ledger

## Status

This ledger records evidence for
`doc/WRIC-CHART-PARALLELIZATION-PLAN.md`. It is intentionally incomplete:
the final Phase-0 semantic oracle and capture contract are frozen and the
strict workload-manifest harness repair has passed focused and complete
functional verification, while a passing wrapper calibration and the baseline
trials remain outstanding. The plan's deadline-overlap rule authorizes
Phase-1+ implementation and functional evidence now, but Phase 0 remains open.
No Phase-1+ timing, scaling, memory, or parity result is acceptance evidence
until Phase 0 is calibrated, captured, finalized, audited, and sealed.

On 2026-07-14 the user authorized a deadline-driven scheduling exception: this
state may be committed as the Phase-0 measurement checkpoint and later-phase
implementation may begin before the quiet-host timing campaign. The
checkpoint is measurement infrastructure, not an accepted timing baseline.
Phase 0 stays open, the frozen executable/workload identities remain binding,
and calibration, capture, finalization, detached sealing, and final audit are
still mandatory before any timing-dependent phase exit or Phase-10 acceptance.

Raw artifacts belong under `build/wric-chart-parallelization/` and are not
committed. Phase-0 artifacts use the non-overwriting directory
`build/wric-chart-parallelization/baseline-408434e/`.

| Phase | Evidence status | Result |
|---|---|---|
| 0. Repair and freeze measurement | in progress (timing deferred) | native/oracle/runner bytes frozen and functional gates independently audited; passing calibration, capture, finalization, and seal pending |
| 1. Compile an immutable chart plan | implementation complete; acceptance pending Phase 0 | code checkpoint `208ce23`; focused and full RelWithDebInfo correctness/counter gates pass; canonical baseline and timing gates remain pending |
| 2. Remove allocations and duplicate work | implementation complete; acceptance pending Phase 0 | code checkpoint `0c4623b`; same-profiler allocation reduction, warmed-zero allocation, frozen semantic/counter, focused/full RelWithDebInfo, and targeted ASAN gates pass; serial timing, exact-small timing, RSS, and sealed canonical comparison remain pending |
| 3. Add one persistent adaptive scheduler | implementation complete; acceptance pending Phase 0 | code checkpoint `7d294d6`; scheduler contract, canonical worker matrix, full RelWithDebInfo, and targeted TSan gates pass; small-case timing remains pending |
| 4. Parallelize patterns and local scoring | implementation complete; acceptance pending Phase 0 | code checkpoint `cbf92b6` and evidence checkpoint `1be6ffe`; full RelWithDebInfo and targeted TSan gates pass; sealed scaling/RSS gates pending |
| 5. Parallelize a single exact B&B | implementation checkpoint complete; acceptance pending Phase 0/6 | `bb29300` plus harness fix `3a10e9c` pass post-omission semantics, full CTest, and targeted TSAN; diagnostic exact-phase scaling exceeds 2x; unified frontier/candidate admission and sealed timing/RSS remain pending |
| 6. Parallelize exact top-K candidates | implementation complete; acceptance pending Phase 0 | code checkpoint `870c298`; in-tree Top-K/worker semantics, stable failures, deterministic scheduling, functional bounded admission, full RelWithDebInfo CTest, and targeted TSan pass; sealed medium scaling, real Top-K-16 admission, frozen-workload repeatability, and RSS remain pending |
| 7--10 | pending | may follow in plan order under the same acceptance gate |

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

Checksums for all currently available planned fixtures were captured in
`baseline-408434e/unsealed-sha256-inputs.txt`. That file is an input to the
future workload manifest; it is not itself the sealed workload manifest.

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

Phase-7/8/9 fixtures do not yet exist and therefore have no checksum or
historical performance result. They must be added through append-only
supplemental manifests after frozen-oracle characterization.

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

This diagnostic selects the plan's exact `expected_infeasible` branch; it is
not itself the sealed manifest evidence. The final Phase-0 bootstrap must
repeat W1 and W8 through the frozen timeout/RSS wrapper, require explicit
post-capture approval of the exact reason tuple, and include all command,
process, stderr, no-output, fixture, binary, and approval evidence in the
detached artifact ledger.

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
bytes.  A passing calibration has not yet been recorded; Phase 0 remains open
until a canonical calibration JSON tied to this runner passes both 2% gates
and is included in the prepared and final detached artifact closures.

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

The following final artifact names are reserved but not yet sealed:

| Artifact | Current state |
|---|---|
| `bin/larch2` | frozen and hashed |
| `bin/dagutil` | corrected semantic oracle frozen and hashed |
| `workloads.tsv` | schema/reconstruction verified; pending capture completion and seal |
| `commands.sh` | generator verified; pending one-time prepared bootstrap artifact |
| raw per-trial TSV | pending |
| aggregate TSV/report | pending |
| full canonical sidecars | pending |
| compact timed semantic digests | pending |
| final SHA-256 manifest | pending; seal only after all preceding artifacts exist |

No existing artifact in this directory may be overwritten once the final
Phase-0 SHA-256 manifest is sealed.

### Named microbenchmarks

`pending` means no run has been claimed. A timeout will be recorded only as an
explicit manifest-declared result row, never as a blank cell.

| Benchmark | Frozen mode/budget from plan | Workers required | Phase-0 result |
|---|---|---|---|
| `dense-local-small-64` | `grammar_lower_bound`, 64/0 | matrix | pending |
| `dense-local-medium-64` | `grammar_lower_bound`, 64/0 | matrix | pending |
| `cache-medium` | `grammar_lower_bound`, 1/0 | matrix | pending |
| `exact-small-one` | `grammar_exact`, 1/1 | matrix | pending |
| `exact-medium-one` | `grammar_exact`, 1/1, 600 s | 1 and 8 required | pending |
| `exact-medium-topk4` | `grammar_exact`, 32/4 | matrix | pending |
| `lazy-compression-medium` | `grammar_lower_bound`, 64/0, forced lazy | matrix | pending |
| `sampled-generation-high` | `sampled_tree_fixed`, at least 256/1 | later supplement | fixture pending |
| `local-commit-three` | `grammar_exact`, 32/4, 3 iterations, local commit | later supplement | fixture pending |

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

### Primary and stress comparisons

| Contract | Native rows | Chart rows | Canonical parity | Wall result | RSS result |
|---|---|---|---|---|---|
| medium primary, native 50 vs chart 32/4 | pending | pending | pending | pending | pending |
| medium stress, native 50 vs chart 128/16 | pending | pending | pending | pending | pending |
| bounded real-scale confirmation | pending preflight | pending preflight | pending | pending | pending |

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
- [ ] Native and exact-chart medium 32/4 and 128/16 rows are captured.
- [x] Instrumented `dagutil` is frozen; exact native/oracle mismatch rejection
  is covered by the harness regression and runner mismatch rejection by the
  bootstrap regression.
- [ ] The final sealed manifest/harness audit re-verifies both executable hashes.
- [ ] `commands.sh`, reports, TSVs, sidecars, fixtures, and executables are
  covered by the final SHA-256 manifest.
- [x] All pre-existing and new Phase-0 tests pass (155/155, with two expected
  diagnostic skips and zero failures).
- [x] Frozen Phase-0 capture inputs and artifacts contain no product
  performance optimization; deadline-overlap Phase-1+ working-tree changes are
  segregated and excluded from every Phase-0 executable role.

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
| Canonical semantic equality to sealed Phase 0 | pending Phase-0 capture/seal; in-tree independent semantic oracles pass |
| Medium one-worker local-score target and <=5% chart-phase regressions | pending Phase-0 capture/seal |

Phase 1 is therefore **implementation complete; acceptance pending Phase 0**.
No timing, scaling, RSS, or final parity claim is made from the deadline-overlap
worktree.

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
| Medium one-worker local scoring at least 30% faster than Phase 0 and no more than 5% slower than Phase 1 | pending sealed Phase-0 capture |
| Small one-candidate exact at least 25% faster than Phase 1 | pending sealed Phase-0 capture |
| Peak RSS no more than 25% above every reference | pending sealed Phase-0 capture; per-worker resident descriptor ownership is mechanically bounded |
| Canonical output | in-tree frozen tuple/digest and byte-identical profiled output pass; final sealed Phase-0 comparison pending |
| Full CTest and targeted ASAN | pass |

Phase 2 is therefore **implementation complete; acceptance pending Phase 0**
under the deadline-overlap rule. No busy-host wall time or RSS observation in
this section is acceptance evidence.

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
| Small 64-candidate W8 no more than 5% slower than W1 | pending sealed Phase-0 capture |
| Non-vacuous parallel execution above threshold | pass |
| Targeted TSan and full CTest | pass: 52/52 and 159/159 |

Phase 3 is therefore **implementation complete; acceptance pending Phase 0**
under the deadline-overlap rule. No busy-host timing observation is accepted
as evidence for the outstanding small-case wall-time gate.

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
| W1/W2/W4/W8 canonical matrix for seeds 1, 7, and 19 across dense, fixed-topology, and grammar-exact modes | pending sealed Phase-0 capture; in-tree serial/parallel oracle, W1/W4 pattern-axis, W1/W8 multi-accept, and CLI identity tests pass |
| Medium 64-candidate local W8/W1 at most 0.50 and Phase-4 W1/Phase-3 W1 at most 1.05 | pending sealed Phase-0 capture |
| Medium construction W8/W1 at most 0.50 | pending sealed Phase-0 capture |
| Local and construction W8/W4 at most 1.10; CPU contention investigated above 0.25 | pending sealed Phase-0 capture; strict postprocessor and boundary tests pass |
| W8 peak RSS within the global final bound | pending sealed Phase-0 capture; fail-closed admission and postprocessor gates pass |
| Full CTest and targeted TSan | pass |

Phase 4 is therefore **implementation complete; acceptance pending Phase 0**.
No timing or RSS value in this section satisfies an outstanding performance
gate.

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
but is not accepted. Phase 6 must first close the global bounded-memory
invariant by releasing score-pass frontiers before exact-mask recovery and by
admitting candidate/frontier scratch under the unified budget. Phase 0 must
then supply sealed timeout, scaling, and RSS evidence. Under the authorized
deadline-overlap rule, implementation proceeds to Phase 6 now.

## Phase 6 implementation checkpoint (unsealed)

Phase 6 is **implementation complete; acceptance pending Phase 0** at
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
| Top-K `1,4,16` exact semantics and stable exceptions at W1/W2/W4/W8 | pass in the independent in-tree matrix; final frozen-workload capture remains a Phase-0-dependent final gate |
| Medium Top-K-4 W8 `exact_verification_ms` at least 2.0x faster than same-revision W1 | pending sealed Phase-0 workload capture |
| Real Top-K-16 stays within configured concurrent-memory admission bound | pending sealed real-workload run; synthetic/fixture admission, splitting, rejection, and estimator-backstop contracts pass |
| W8 peak RSS no more than 2.0x W1 | pending paired sealed Phase-0 run |
| Repeated W8 results byte-identical | pass in-tree for digest and full canonical sidecar; frozen-workload repetition remains pending |
| Exact-search TSan and full CTest | pass at the code checkpoint; exact hashes are retained in the artifact manifest |

Quiet-host Phase-0 calibration, capture, finalization, audit, and detached seal
remain deferred under the authorized deadline-overlap rule. No same-revision
diagnostic, unit fixture, or postprocessor regression is substituted for those
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
These hashes remain pinned by the committed regenerator and will be copied
into the immutable supplemental manifest only during the later quiet-host
Phase-0 seal.

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

On the clean current main worktree, the GCC-trunk RelWithDebInfo command
`ctest --test-dir build --output-on-failure -R
'^(chart_parallel_test|chart_spr_search_test|multifurcation_chart_oracle_test|lazy_key_grouping_test)$'`
passed 4/4 tests in 7.71 seconds; `dagutil` compiled, `git diff --check` was
clean, and changed-line clang-format reported no edits. This is an unsealed
functional checkpoint, not a timing or Phase-7 exit claim. Finite state-build
transient admission, allocation-free candidate-local admission, within-clade
staging/profile evidence, automatic policy, sanitizer gates, full CTest, RSS,
and all deferred Phase-0-dependent measurements remain pending.

### Phase-7 implementation checkpoint

Phase 7 is **implementation complete; acceptance pending Phase 0** at
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

The current main worktree rebuilt `chart_spr_search_test`,
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

This is not the Phase-7 exit decision. Quiet-host forced-off/forced-on/auto
medians on both named fixtures, the `phase7-lazy.tsv` supplemental manifest,
the required high-fixture and small-fixture scaling/overhead comparisons,
paired RSS, sparse-reclamation TSan, full RelWithDebInfo CTest, and final
sanitizer gates remain pending. Under the authorized deadline-overlap rule,
implementation proceeds to Phase 8 without substituting the diagnostics above
for those gates.

## Phase 8 implementation checkpoint (unsealed)

Phase 8 has a **functionally passing implementation checkpoint; acceptance
pending Phase 0 and the remaining performance gates** at
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

The current main worktree rebuilt `larch`, `chart_spr_test`,
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

These measurements are deliberately not an exit decision: unrelated host
compilation was active, Phase 0 is not calibrated/sealed, and the Phase-8
supplemental manifest has not been created. The quiet-host run must either pass
the 2.0x gate or trigger another profile-supported optimization cycle; it must
also establish Phase-7 end-to-end non-regression and paired RSS. This checkpoint
does not reinterpret the observed miss or substitute it for sealed evidence.

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

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
| 2. Remove allocations and duplicate work | implementation in progress | exact-setup reuse and allocation-free local-kernel slices started after Phase-1 checkpoint |
| 3--10 | pending | may follow in plan order under the same acceptance gate |

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

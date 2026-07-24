# WRIC chart parallelization: implementation plan

## Status and execution contract

This is the execution plan for bringing the new WRIC chart-SPR search path to
wall-clock parity with the pre-WRIC sample--SPR--merge loop while preserving
the exact algorithm described in
`~/matsen/higher-rank-larch2-tex/main.tex`.

Research snapshot:

- branch: `wric`;
- research tip: `408434e` (`Add chart row fluidity`);
- build: existing `build/`, `RelWithDebInfo`, `-O2 -g -DNDEBUG`;
- required compiler prefix: `/home/ogi-agent/install/gcc-trunk`;
- host: AMD Ryzen 7 7730U, 8 physical cores / 16 SMT threads, 60 GiB RAM;
- physical-core CPU list on this host: `0,2,4,6,8,10,12,14`.

The `/goal` executor must treat this file as a checklist and keep the phase
status table current. A phase is complete only after its exit criteria pass
and its evidence is recorded in
`doc/WRIC-CHART-PARALLELIZATION-RESULTS.md`. Raw benchmark output belongs under
`build/wric-chart-parallelization/` and must not be committed.

Do not mark the goal complete merely because code has been written or because
one microbenchmark improved. Completion requires every final acceptance gate
in this document.

| Phase | Status | Required evidence |
|---|---|---|
| 0. Repair and freeze measurement | complete (audited and sealed) | Thirteen captures are closed: 267 ordinary canonical rows across 60 repeat stages, 27 manifest-approved timeouts, and two real-fixture W1/W8 `high_arity_refinement_refusal` observations. Pending/final audits and strict smokes passed, and the final workload and artifact-ledger SHA-256 values are `32ae82a93cb72a28afaa510391eac83f80f13638d46e1513881d2b484c9dc3ae` and `a33436d78ec6c840427b343615c6b9a7b555a87f989000865873e9c5adeb06bb` |
| 1. Compile an immutable chart plan | implementation complete; optimized-candidate acceptance pending | Functional/counter gates pass at `208ce23`. The complete Q/C4 evidence and subsequent gate-by-gate diagnosis found the Phase-1 canonical and serial no-regression gates passing, but the current optimized candidate must be frozen and recaptured before this phase can close on the final product |
| 2. Remove allocations and duplicate work | implementation complete; optimized-candidate acceptance pending | `0c4623b` passes the allocation/build/canonical/full-CTest/targeted-ASAN gates. Q/C4 passed the Phase-2 serial timing and RSS diagnosis; the changed candidate reopens the candidate-bound correctness, timing, RSS, and sanitizer gates |
| 3. Add one persistent adaptive scheduler | implementation complete; optimized-candidate acceptance pending | Safe one-pool orchestration, full CTest, and targeted TSan pass at `7d294d6`, and the Q/C4 small-case timing diagnosis passed. The final immutable candidate still requires its clean suite, sanitizer, and timing evidence |
| 4. Parallelize patterns and local scoring | implementation complete; optimized-candidate acceptance pending | `cbf92b6` passes the functional gates. The sealed Q/C4 deep Phase-4 validator and contention investigation passed scaling, Phase-3 serial comparison, RSS, swap, and parallel-high-water gates; candidate-bound recapture and validation remain required |
| 5. Parallelize a single exact B&B | implementation checkpoint complete; optimized-candidate acceptance pending | `bb29300` plus harness fix `3a10e9c` pass semantics and sanitizer checkpoints, and Q/C4 passed the exact-span, timeout, and RSS diagnosis. The final candidate's fresh full evidence remains pending |
| 6. Parallelize exact top-K candidates | implementation complete; optimized-candidate acceptance pending | `870c298` passes the in-tree functional matrix. Q/C4 covered the current-product Top-K 1/4/16 worker semantics, repeatability, scaling, admission, and RSS diagnosis; the changed candidate must repeat the authoritative campaign and evaluator |
| 7. Make lazy charts scalable and adaptive | optimized candidate implemented; authoritative acceptance pending | Immutable Q `a9db72e60f153a95362db544107373817a58a258` has sealed normal/ASAN/TSan closure and a complete C4 campaign, but C7 found forced high-compression lazy W8/W1 `0.682706744372248`, above `2/3`. The retained fingerprint-fusion candidate diagnostic is `0.630021916938`, but it cannot close the gate before an immutable candidate, full tests/sanitizers, a fresh campaign, and a passing evaluator. The production manifest hashes remain `82250bb26d5394d3c2c616e4ccbf606e80eca074e560fd8e3361274655e56718` and `7b934fa0ad8279893d03321724935dccf8df99fa840899652dccdc63c8c37bff` |
| 8. Parallelize and pipeline candidate generation | sealed retry1 generation gate passes; candidate end-to-end acceptance pending | Attempt 0 remains a sealed failure at W8/W1 `1.287784679`; additive retry1 on R `07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c` passes at `0.403067171046`. Q-aware tool C4 `be5e4a025c357d22fb1a6b41f544e35f2d8a5891` completed and audited the 20-component/430-row Q campaign, and the diagnosis found no Phase-8 failure. The changed candidate still requires its same-workload recapture and strict evaluator decision |
| 9. Parallelize accepted-state cache updates | implementation checkpoint complete; optimized-candidate acceptance pending | Persistent cache transactions, exact-state reuse, the three-accept worker matrix, durable manifests, and counter contracts pass through `4ef6126`. Q/C4 deep Phase-9 validation and its timing/RSS diagnosis passed before C7 failed later gates; candidate-bound validation, sanitizers, timing, and RSS remain pending |
| 10. Integrate, tune defaults, and prove parity | implementation in progress; authoritative candidate closure pending | Q has sealed normal/ASAN/TSan closure, and C4 completed and audited all 20 components/430 rows. C7's single pre-default invocation failed closed; diagnosis identified exactly four gaps: forced lazy, physical grammar scaling, primary grammar scaling, and sampled-tree fixed-topology stress. Dirty candidate diagnostics now pass all four limits, but the immutable candidate, full tests/sanitizers, fresh campaign, passing pre-default evaluator, conditional default promotion, and post-default closure all remain pending |

### Deadline-overlap scheduling exception

The audited Phase-0 native/oracle/runner identities,
harness/bootstrap/calibration inputs, workload generator, fixtures, and named
workload contract are the immutable pre-optimization boundary. To meet the
implementation deadline, Phase-1+ product work may begin before the quiet-host
calibration and baseline capture. After a phase's implementation and
non-performance correctness/counter gates pass, the executor may continue to
the next phase in plan order even while Phase-0-dependent performance gates
remain pending.

This overlap, authorized on 2026-07-14, did not waive any gate. While capture
was outstanding, later phases could be marked only as implementation
checkpoints and frozen Phase-0 inputs could not be replaced by optimized
working-tree binaries. That deferred work is now complete: Phase 0 was
prepared, captured, approved, finalized, audited, and sealed from the frozen
executables. Later phases still require their own canonical, timing, scaling,
RSS, supplemental-manifest, and final acceptance gates; completion of Phase 0
does not make any of those gates pass.

## Objective

The non-negotiable performance objective is:

> On the medium seedtree fixture, the median end-to-end wall time of exact
> `grammar_exact` chart-SPR search must be no greater than the median wall time
> of the unchanged sample--SPR--merge baseline under the frozen workload and
> equal CPU affinity, while producing the same exact chart result as the
> one-worker oracle.

The parity workload freezes the benchmark harness's current default chart
budget at 32 scored candidates and top-K 4 exact verifications, versus 50
native moves, for one iteration and seed 1. These numbers may not change after
Phase 0. The documented 128/16 comparison remains a required stress and
real-scale confirmation, but it is not allowed to redefine the primary parity
contract by making the chart perform four times the frozen candidate work.

`grammar_exact` is the primary parity claim because it exercises the new WRIC
grammar, chart recurrence, multi-site B&B, candidate verification, and commit
path. `sampled_tree_fixed` and `hybrid_exact` remain mandatory correctness and
non-regression workloads. They may only be advertised as reaching parity if
they independently pass the same wall-time gate.

The current research measurements explain the ordering of this plan:

- medium dense local scoring takes only about 1.5 seconds for 64 candidates;
- a medium one-candidate grammar-exact run did not produce a search report
  before a 180-second timeout;
- one local native medium baseline run took about 110 seconds;
- current local-score threading makes performance worse: for 4,096 small
  candidates, one worker took 4.54 seconds wall / 0.28 seconds system, while
  eight workers took 12.53 seconds wall / 31.3 seconds system;
- dense profiles show repeated validation, vector construction, key
  comparisons, and allocation in the candidate x pattern loop;
- lazy mode recomputes far fewer rows on the medium fixture but loses the
  saving to structural-key and tree-map overhead.

Therefore exact construction/verification, repeated work, and allocator-heavy
kernels have priority over adding more threads to the current local scorer.

## Non-goals and prohibited shortcuts

The following do not satisfy this plan:

- replacing exact multi-site acceptance with the composite lower bound;
- reducing iterations, candidates, exact top-K, validation, refinement shapes,
  active patterns, or candidate sources to obtain a faster number;
- changing exact scores, exact keep masks, tie provenance, topology identity,
  candidate order, or accepted-move order;
- reducing either method's frozen method-specific budget or CPU affinity for a
  comparison. Native moves and chart candidates/exact verifications are
  different units and are not compared numerically to each other;
- using ASAN, TSAN, Debug, smoke, or the small fixture for a performance claim;
- parallelizing independent per-site optima and calling their sum a coupled
  multi-site optimum;
- schedule-dependent RNG, reservoir selection, tie-breaking, or exception
  selection;
- concurrent move commits or scoring across a committed-state generation
  boundary;
- nested worker pools or unbounded concurrent B&B instances;
- GPU, distributed-memory, or approximate-search work;
- changing the native sample--SPR--merge algorithm to make the baseline easier
  to beat.

Tasks at the nucleotide-state level are explicitly out of scope. The alphabet
has four states, so state and ordinary production tasks are too small; use
fixed arrays, SIMD, or batching inside a coarse task instead.

## Correctness and concurrency invariants

Every phase must preserve these invariants:

1. **One-worker oracle.** Explicit one-worker execution remains available and
   is the semantic oracle for all parallel executions.
2. **Immutable snapshot.** Candidate tasks read one fully published grammar,
   chart/cache generation, exact baseline, pattern set, and option set.
3. **Single-writer commit.** All candidate work joins before canonical
   selection. Exactly one accepted move is committed. No worker observes a
   partially updated state.
4. **Inside/outside barrier.** Accepted inside-cache updates finish before any
   outside-cache update reads them; publication occurs only after both phases.
5. **Stable task identity.** Results are written to slots identified by stable
   pattern, candidate, clade, or frontier-product indices, never completion
   order.
6. **Canonical reductions.** Integer sums/minima use a fixed order where
   overflow checking or provenance depends on order. Tied witnesses are sorted
   or selected by canonical production/state/candidate identity.
7. **Exact provenance.** An exact keep mask continues to mean the union of all
   productions appearing in an optimal coupled topology. Parallel equality
   merging may not under- or over-keep provenance.
8. **Deterministic RNG.** Random draws are indexed/preassigned by the frozen
   canonical source position/identity and reproduce the Phase-0 sequential draw
   stream exactly. They never depend on worker ID, queue order, or completion
   time; identity hashing alone is not presumed equivalent.
9. **Bounded memory.** Concurrent exact work is limited by both the worker
   budget and a conservative per-task memory estimate. Memory pressure must
   reduce concurrency rather than spill or fail unpredictably.
10. **No hidden serial fallback in scaling tests.** Correctness tests must also
    assert that enough work was submitted to and completed by more than one
    worker when parallel work is expected.
11. **Exceptions and cancellation.** A deterministic first failure is
    rethrown after all launched tasks are joined; cancellation cannot publish
    partial results or leave work running.
12. **Generic chart safety.** Binary fast paths must retain the existing
    checked behavior for multifurcations and unsupported arities.

## Files and ownership map

Expected files are listed to orient implementation, not to require artificial
changes to every file.

| Area | Primary files | Intended ownership |
|---|---|---|
| Chart plan and dense recurrence | `include/larch/parsimony_chart.hpp`, `include/larch/site_patterns.hpp` | Immutable plan; output owned by pattern |
| Exact B&B | `include/larch/chart_trim.hpp` | Pattern tasks, clade-level wavefronts, stable frontier merges |
| Search orchestration/local scoring | `include/larch/chart_spr_search.hpp` | One search scheduler; candidate x pattern adaptive tasks |
| Search implementation/commit | `src/chart_spr_search.cpp` | Immutable readers, single writer, pattern-sharded cache phases |
| Lazy charts | `include/larch/lazy_chart.hpp` | Increasing/decreasing clade wavefronts and stable class grouping |
| Candidate generation | `include/larch/chart_spr.hpp` | Stable source-local candidate buffers and serial canonical gather |
| Persistent caches | `include/larch/inside_chart_cache.hpp`, `include/larch/outside_chart_cache.hpp` | Pattern shards with inside/outside barrier |
| Scheduler/pool | `include/larch/thread_pool.hpp` and, if useful, a chart scheduler header | One lifetime per search; no nested pools |
| CLI/report | `tools/dagutil.cpp` | Unified worker/memory controls and phase diagnostics |
| Benchmarks | `tools/wric_spr_search_benchmark.sh`, `doc/WRIC-SPR-SEARCH-BENCHMARK.md` | Repeatable raw trials and strict aggregation |
| Tests | chart, trim, cache, search, and new `test/chart_parallel_test.cpp` | 1-vs-N semantic and concurrency coverage |

## Benchmark and evidence protocol

### Fixtures

Each fixture has a specific role:

| Fixture | Role |
|---|---|
| `test/wric_four_taxon_misplaced.{fa,nwk,ref}` | Non-vacuous accepted move and commit publication |
| `test/wric_two_polytomy.{fa,nwk,ref}` | Multifurcation, lazy grouping, and fixed-topology behavior |
| `data/test_5_trees/tree_0.pb.gz` | Fast smoke, determinism, and local-score scaling; not parity |
| `data/testcase/full_dag.pb.gz` | Multiparent/multi-tree DAG and cache reuse |
| `data/seedtree/seedtree.pb.gz` plus `refseq.txt.gz` | Primary medium performance gate |
| `data/20D_from_fasta/1final-tree-1.nh1.pb.gz` plus `refseq.txt` | Real-scale confirmation when the contracted workload is feasible |

The small native loop has an artificial roughly 0.8-second progress-polling
floor, so it must never be used to claim end-to-end parity.

### Evidence recorded after every phase

For each benchmark artifact, record:

- git revision and dirty status;
- exact command and environment;
- compiler path/version, build type and flags;
- CPU model/topology, affinity, worker request and resolved worker count;
- fixture hashes, active patterns, grammar clades/productions, candidate source,
  candidate count, exact-verification count, seed, and exactness labels;
- wall, user, and system CPU time;
- peak RSS, resident chart bytes, configured memory budget, and peak concurrent
  exact verifiers;
- candidate-generation, chart-plan, initial chart, cache, local scoring, exact
  setup, exact candidate, accepted update, materialization, and total time;
- scheduler task/grain/axis/utilization counters;
- final exact objective, externally validated parsimony, candidate signatures,
  accepted sequence, exact masks/provenance digest, chain identity, and final
  canonical topology digest.

This box does not have GNU `/usr/bin/time`, `perf`, or `hyperfine`. Collect
user/system time and peak RSS with an in-repository measurement wrapper that
forks/execs and obtains child usage with `wait4`/`getrusage` (with the Linux RSS
unit documented), or an equally reproducible mechanism that can measure the
frozen native binary without rebuilding it. The wrapper must subreap and join
the full command descendant tree, fail closed on monitoring errors, and use
the sampled aggregate descendant RSS for caps; leader-only `ru_maxrss` is only
a diagnostic. It must reset inherited `SIGCHLD=SIG_IGN`/`SA_NOCLDWAIT` while
it waits, synchronously reap verified zombie-only adoption chains before
timeout classification, follow identity-checked `setsid` descendants, and
enforce an empty tree plus `ECHILD` on every return path. A cap crossing
receives immediate SIGKILL after the first 10-ms sample (with possible
one-interval overshoot), while only timeout handling has a TERM grace period.
Do not silently omit these fields.

Every parallel speedup is computed between one-worker and N-worker runs of the
same revision, executable, fixture, options, and canonical workload. A separate
same-worker comparison against the prior phase detects serial regression;
never count a serial optimization from an older revision as parallel speedup.

### Canonical comparison

Add a machine-readable canonical result that excludes timing and scheduler
accounting but includes every semantic output needed for 1-vs-N comparison:

- ordered stable candidate signatures and local/exact scores;
- exact keep mask and canonical tied provenance;
- selected and accepted candidate sequence;
- exactness, acceptance, source, commit, and verification labels;
- state objective after every iteration;
- chain identity report;
- final validated parsimony and canonical production/taxon-set topology keys.

Timed runs emit only the compact canonical digests needed for comparison; they
must not serialize every full candidate mask or provenance witness in the timed
region. A separate untimed correctness run emits the complete canonical
sidecar and proves that each digest covers its full data. Parallel runs must
match the one-worker canonical result byte-for-byte. If serialization currently
includes unstable addresses or unordered-map order, canonicalize the
serialization rather than weakening the comparison.

### Required parallel test matrix

Create and register `chart_parallel_test`. Across the phases,
the matrix must compare workers `1,2,4,8` (and `0,16` where scheduling itself
is under test) for:

- dense, forced-lazy, and forced pattern-batch chart construction;
- local candidate scores, ordered candidate results, and workload counters;
- exact top-K verification, including tied optima and tied provenance;
- single-candidate B&B wavefronts, including wide same-level clade work;
- exact optimum, exact keep mask, frontier sizes, and canonical witnesses;
- fixed-topology-exact and grammar-exact acceptance;
- grammar, sampled-tree, and hybrid candidate sources;
- conservative and local-commit searches with multiple real acceptances;
- empty work, one task, more workers than work, cancellation, and exceptions;
- randomized traversal and reservoir sampling for seeds `1,7,19`;
- memory-admission batching and adaptive dense/lazy selection;
- binary, multifurcating, multiparent, UA/reference-state, and two-pass
  exact-mask cases.

The comparison may ignore only timing and scheduler-accounting fields. It must
also assert the active-worker high-water mark in cases intended to exercise
parallel execution, so a silently serial implementation cannot pass.

## Phase 0 — Repair and freeze measurement

### Goal

Create and freeze a trustworthy benchmark contract and correctness oracle
before changing product performance code; complete its quiet-host measurement
and seal before accepting any performance result.

### Current evidence (sealed 2026-07-22)

The immutable measurement checkpoint remains
`7ca527b8906d018124756182274335cbff936d72`, with exact subject
`Baseline measure` and sole parent
`408434ecfd096af484ecbbd3deeb67511151cd76`. It remains an ancestor of the
implementation tip; its detached runner, oracle, workload definitions, and
binaries are the only admissible Phase-0 inputs.

The frozen wrapper calibration now passes. Its canonical JSON is
`build/wric-chart-parallelization/phase0-calibration-408434e/wrapper-calibration.json`
in the detached Phase-0 worktree and has SHA-256
`b254001b2327e0b2e8ef2c1d6c32e5b0f5e315bdeeadffa9249a8765228f39d0`.
The median paired wrapper/direct ratio is `1.015775988195` and the ratio of
medians is `1.008541338978`, both below the frozen `1.02` limit; the live guard
recorded zero forbidden-process matches.

All 13 planned captures are complete. The 12 ordinary capture records contain
267 canonical rows across 60 repeat stages and classify 27 timeouts approved
by the immutable manifest. The real 20D preflight separately records exact W1
and W8 ordinary-exit-status-1 refusals with reason
`high_arity_refinement_refusal`; both have zero signal/core/timeout/RSS-limit
flags, produce no output artifact, and match approved stderr SHA-256
`84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35`.
These are two expected-infeasible observations, not successful timing rows.

Pending and final audits, both ordinary and real strict smoke runs, and the
seal completed successfully. The pending and final workload manifests are
byte-identical. The sealed `workloads.tsv` SHA-256 is
`32ae82a93cb72a28afaa510391eac83f80f13638d46e1513881d2b484c9dc3ae`;
the final `phase0-artifacts.tsv` ledger SHA-256 is
`a33436d78ec6c840427b343615c6b9a7b555a87f989000865873e9c5adeb06bb`.
Phase 0 is therefore complete as the immutable measurement base. This closes
no later-phase timing, scaling, RSS, parity, supplemental-manifest, automatic
policy, or default-selection gate.

### Actions

1. Fix the deterministic exit-141 failure in
   `tools/wric_spr_search_benchmark.sh`. `set -o pipefail` currently combines
   with `sed ... | head -n 1`; repeated report keys cause `sed` to receive
   `SIGPIPE`. Use a single-process extractor and add a repeated-key regression.
2. Add harness controls for:
   - `--warmups`;
   - `--repetitions`;
   - `--workers-list` and a single `--chart-workers` budget;
   - harness `--chart-memory-budget`, forwarded exactly to product option
     `dagutil --chart-spr-memory-budget` and reported as a unified chart/exact
     budget contract; Phase 6 adds exact-task admission enforcement;
   - timeout with an explicit `timeout` result row;
   - record-only `--allow-expected-timeout ROW_ID` for Phase-0 characterization
     and strict fail-on-timeout behavior for every acceptance run. `ROW_ID`
     resolves through the immutable workload manifest to one exact
     `(fixture, method, workers, candidate/exact budget, timeout)` tuple; the
     harness requires exactly the declared number of timeout rows and rejects
     every additional timeout;
   - optional CPU-affinity metadata;
   - alternating paired baseline/chart execution order;
   - `--local-accept-updates`, forwarded exactly to
     `dagutil --chart-spr-local-accept-updates`;
   - raw per-trial TSV and median/maximum aggregation;
   - strict expected-work, validation, per-method/worker
     `--require-wall-ratio METHOD@WORKERS=RATIO`, and RSS gates;
   - a strict `--require-worker-policy default=automatic_default` Phase-10
     gate pairing omitted/default with explicit auto for policy, resolved
     workers, semantics, and per-trial/median 10% wall-time equivalence;
   - `--workload-manifest PATH`, required in acceptance mode, which supplies or
     validates every workload-defining option and frozen input hash;
   - repeatable `--supplemental-workload-manifest PATH` and
     `--run-manifest-group NAME`. A supplement records its parent base-manifest
     SHA-256, may add new content-addressed fixtures/rows but never replace a
     base row ID, and is itself immutable/SHA-256-manifested.

   Here `METHOD` is the stable TSV method identifier, for example
   `chart_spr_grammar_exact`, not the shorter mode token.
3. Add `candidate_generation_ms`, exact-initialization time, per-exact-candidate
   time, process CPU/RSS metrics, and requested/resolved worker counts to the
   report and TSV. The process wrapper owns and reaps the complete command
   descendant tree, measures aggregate RSS without scanning unrelated system
   processes, and never returns with a live or zombie descendant. An observed
   RSS-cap violation takes the immediate-kill path; timeout and RSS observations
   are recorded separately under a deterministic precedence rule. Existing
   counters and column meanings must remain stable.
4. Define harness `--chart-workers N|auto` as forwarding to new product option
   `dagutil --chart-spr-workers N`, with `auto` mapping to numeric zero.
   When this option is present, the harness must not emit the legacy product
   option. Preserve harness `--local-workers` and product
   `--chart-spr-local-score-workers` as compatibility aliases that map to the
   unified budget only when `--chart-workers` is absent. Supplying both harness
   options is the labelled conflict. The harness records whether either option
   was explicitly supplied; when neither is supplied it emits neither product
   flag, rather than injecting the legacy default of one, so product-default
   testing is real. Before Phase 3, worker matrices describe only the existing
   local-score phase; the unified budget governs new phases as they land.
5. Add the canonical semantic comparison described above.
6. Capture pre-optimization worker matrices on small and medium, both for the
   frozen 32/4 parity contract and the 128/16 stress contract. Record expected
   timeouts as results in record-only mode, not missing rows. Acceptance mode
   must still reject every timeout.
7. Create `doc/WRIC-CHART-PARALLELIZATION-RESULTS.md` with the baseline command,
   machine/build metadata, raw-artifact location, and summary table.
8. Put raw baselines in a non-overwriting directory named with the git revision
   and capture a SHA-256 manifest for commands, binaries, fixtures, reports,
   and TSVs. Later phases must never rewrite the Phase-0 directory.
9. Before rebuilding anything, copy research-tip `build/bin/larch2` into the
   untracked baseline directory. After Phase-0 reporting/canonical-oracle
   instrumentation is complete but before any product optimization, copy that
   instrumented `build/bin/dagutil` alongside it. Record both SHA-256/version
   values. Every final native comparison uses the exact frozen `larch2`; every
   later-created fixture is first run through the frozen Phase-0 `dagutil` to
   establish its semantic/RNG/candidate oracle before it is used for a gate.
   This remains true even if shared headers later rebuild the working-tree
   tools. Record the diff against pre-WRIC merge base
   `fee366e71439d7b8c8eda588cc72cde8534f102f` proving the native
   sample--SPR--merge algorithm is unchanged. If that proof fails, build and
   freeze the merge-base binary in a separate git worktree instead.
10. Freeze exact commands and report fields for named phase microbenchmarks:
    small/medium dense local-64, medium cache construction, small and medium
    grammar-exact-one, medium grammar-exact-top-K-4, lazy compression, sampled
    candidate generation, and accepted local-commit update. Later phases may
    not change source, cache/lazy policy, verification, dominance, commit mode,
    seed, timeout, or work budget under the same benchmark name. Use a
    600-second comparison timeout for both one- and eight-worker medium
    grammar-exact-one; Phase 5 separately requires the eight-worker row under
    180 seconds.
11. Add an explicit whitelist mechanism for a frozen
    `(fixture, mode, reason)` labelled `expected_infeasible`. It is allowed only
    for the pre-existing high-arity/refinement refusal; timeout, crash, missing
    output, or a changed refusal reason remain failures. The runtime contract is
    exact: ordinary exit status 1, signal/core/timeout/RSS-limit flags all zero,
    no output artifact, and byte-identical stderr matching the approved SHA-256.
    Exit status 2 or any other positive status is not interchangeable with the
    whitelisted refusal.
12. For non-primary real fixtures only, freeze the exact-one 600-second/6-GiB
    preflight used by the bounded real-scale contract. A `scale_limit` result
    must name the exact fixture/mode/resource exceeded and the largest feasible
    prefix; it is never an expected-timeout waiver and is forbidden for
    seedtree.
13. Seal later Phase-7/8/9 fixtures in append-only supplemental manifests after
    running them through frozen Phase-0 `dagutil`. Record the parent base hash,
    fixture hashes, canonical output, options, and counters. Never edit
    `baseline-408434e/workloads.tsv` to add them.

This supplemental-manifest action is complete: the two Phase-7 manifests, the
Phase-8 generation manifest, and the Phase-9 characterization and production
manifests are sealed at the hashes in the status table. Their performance and
cross-phase acceptance gates remain separate and open.

### Named Phase-0 workload contract

Unless a row says otherwise, use one iteration, seed 1, validation on,
`expand-bounded`, one refinement shape, dense/lazy-off, transient verification,
conservative commit, the current exact dominance mode, and 12 GiB unified
memory budget. The generated Phase-0 `commands.phase0.sh` is the final
authority and is SHA-256-manifested.

| Name | Fixture | Mode | Candidate/exact budget | Purpose |
|---|---|---|---:|---|
| `dense-local-small-64` | `data/test_5_trees/tree_0.pb.gz` | `grammar_lower_bound` | 64 / 0 | Grain/overhead and allocations |
| `dense-local-medium-64` | seedtree + refseq | `grammar_lower_bound` | 64 / 0 | Dense local scaling |
| `cache-medium` | seedtree + refseq | `grammar_lower_bound` | 1 / 0 | Initial plan/chart/cache phase |
| `exact-small-one` | small DAG | `grammar_exact` | 1 / 1 | Exact setup plus one verifier |
| `exact-medium-one` | seedtree + refseq | `grammar_exact` | 1 / 1 | Single-B&B span, 600 s timeout |
| `exact-medium-topk4` | seedtree + refseq | `grammar_exact` | 32 / 4 | Candidate/B&B hierarchy |
| `lazy-compression-medium` | seedtree + refseq, lazy forced on | `grammar_lower_bound` | 64 / 0 | Key/class/row metrics |
| `sampled-generation-high` | Phase-8 frozen high-candidate fixture | `sampled_tree_fixed` | at least 256 / 1 | Generation/projection only phase |
| `local-commit-three` | committed three-accept fixture | `grammar_exact`, local updates | 32 / 4, 3 iterations | Incremental cache/commit |

For every row, freeze candidate source, cache/pattern-batch policy,
verification/commit/dominance labels, worker count, timeout, and all report
fields before product optimization begins. If a named fixture is created in a
later phase, add its immutable input/hash and baseline row before using its
speed gate; never compare it to an invented historical number.

### Exit criteria

- `wric_spr_search_benchmark_smoke` emits exactly its required baseline and
  chart rows and exits zero; no exit 141 remains.
- In acceptance mode, missing rows, timeout, nonzero/unwhitelisted child
  status, validation failure, workload mismatch, or unavailable timing/RSS
  fields make the harness exit nonzero. Record-only baseline mode may exit zero
  only with exactly the manifest-declared expected timeout rows and may not
  hide any other/additional timeout or failure.
- Two repeated explicit one-worker runs have byte-identical canonical results.
- Baseline artifacts contain native and all exact chart medium rows for both
  32/4 and 128/16, including an explicitly expected timeout where applicable,
  plus the frozen native/oracle executables and manifest.
- The harness verifies frozen `larch2` and semantic-oracle `dagutil` SHA-256
  values before use and fails on mismatch.
- With RSS enforcement enabled, delayed/background descendants cannot escape
  the cap or timeout, all descendants are gone before wrapper return, and the
  median wall perturbation of the wrapper on a pinned saturated calibration is
  at most 2% relative to direct execution. The exact process-metrics schema and
  timeout/RSS precedence pass adversarial tests, including inherited ignored
  `SIGCHLD`, deterministic zombie-order/cascade cases, rapid forks, and live
  descendants that leave the original process group with `setsid`.
- Acceptance mode refuses to run without the frozen workload manifest and
  fails if resolved lazy/dense, pattern-batch/cache, verification, commit,
  dominance, polytomy/refinement, source, seed, budget, or input-hash fields
  differ from its named row.
- A supplemental manifest is accepted only when its recorded parent hash
  matches the sealed base manifest, its own hash verifies, and none of its row
  IDs override an existing base/supplement row.
- All 147 pre-existing tests plus new harness tests pass.
- No Phase-1+ product optimization is included in or used by the frozen
  Phase-0 capture inputs or artifacts; deadline-overlap working-tree changes
  remain segregated.

## Phase 1 — Compile an immutable chart execution plan

### Goal

Remove repeated validation, ordering, and descriptor construction from every
pattern and candidate without changing the recurrence.

### Actions

1. Build a reusable immutable plan when the grammar/search state is created.
   It should contain:
   - a canonical bottom-up clade order;
   - increasing-size/topological dependency levels;
   - reverse levels where outside computation needs them;
   - validated parent/child production descriptors;
   - compact binary descriptors and a checked generic-arity path;
   - plan-local dense production/clade IDs and any reusable four-state
     transition data.
2. Validate the grammar and production partition contract exactly once at the
   checked boundary. Retain debug assertions/fingerprints so a plan cannot be
   reused with another grammar generation.
3. Make dense charts, exact active-pattern construction, local scoring, and
   cache construction consume the plan instead of sorting and revalidating.
4. Compile each candidate overlay/delta into a checked immutable execution
   descriptor once during candidate preparation. Every pattern scorer consumes
   that descriptor rather than rebuilding child vectors or revalidating the
   candidate production. Plan-local IDs are never used as cross-generation
   identity; taxon/sample-set keys remain the stable identity contract.
5. Where the substitution cost model is shared, profile caching each child's
   four-state transform once. Keep it only if it improves the contracted
   workload and remains correct for every supported edge-cost convention.
6. Add counters for plan/delta builds, full grammar validations, production
   validations, clade-order sorts, and plan cache hits.

### Tests

- Dense and lazy chart equivalence for all existing fixtures.
- Binary and multifurcating grammar behavior.
- Exact optimum, exact keep mask, fluidity, and tied provenance equivalence.
- Deliberately stale/mismatched plan rejection.
- Invalid grammar/partition inputs still fail at construction with useful
  diagnostics.

### Exit criteria

- Candidate x pattern scoring performs zero full production-partition
  validations and zero clade-order sorts, proven by counters.
- The one-worker medium dense local-scoring median is no more than 5% slower
  than Phase 0; a 10% improvement is the recorded target, while Phase 2 owns
  the mandatory cumulative 30% serial improvement gate.
- No small or medium one-worker chart phase regresses by more than 5%.
- Canonical semantic results match Phase 0 exactly.
- Targeted and full RelWithDebInfo CTest pass.

If the 10% target is missed, profile twice and redesign the hot representation.
The immutable checked plan may remain as a required parallel-safety primitive
only if the zero-validation/sort and no-regression gates pass; Phase 2 must
still reach the cumulative 30% improvement over Phase 0.

## Phase 2 — Remove allocation and duplicate chart work

### Goal

Make independent tasks large enough and sufficiently allocator-free to scale.

### Actions

#### 2A — Allocation-free dense/local kernel

1. Replace per-production child vectors, copied leaf-state vectors, temporary
   row vectors, and binary prefix/suffix vectors with spans, fixed arrays, or
   reusable small buffers. Preserve a checked generic-arity path.
2. Define an owner-neutral scratch-context/arena API that works in the serial
   oracle. No persisted result may refer into scratch, and scratch resets at a
   deterministic operation boundary. Phase 3 binds one context to each safe
   scheduler worker/task.
3. Replace allocation-heavy equality keys in the dense/local contracted kernel
   with packed keys, reserved tables, or stable sort-and-unique where profiling
   supports it. Lazy structural keys remain Phase 7 work; frontier/provenance
   representation remains Phase 5 work.
4. Add test-only allocation counting around the scoring region itself.

#### 2B — Exact input and chart reuse

5. Reuse resident inside charts when building exact active-pattern and
   composite information. Do not build the same unchanged pattern inside chart
   two or three times during exact initialization.
6. Build cold outside data from the already-built inside cache rather than
   rebuilding inside charts.
7. Remove transient-verifier scratch-cache copies that the subsequent B&B does
   not consume. Prefer read-only base data plus copy-on-write affected overlays.
8. Deduplicate initial-upper-bound topologies before cross-pattern scoring.
9. Add build/allocation/reuse counters sufficient to make each claim
   mechanically testable.

### Exit criteria

- Profiled dynamic allocation calls per 1,000 small dense candidates fall by
  at least 80% from Phase 0. Count only the frozen candidate-scoring region;
  exclude process startup, grammar/state initialization, reporting,
  materialization, and output-owned result storage. Record the exact profiler
  or test allocator.
- Every unchanged active-pattern inside chart is built at most once per
  committed state; counters demonstrate that exact setup does not rebuild it.
- One-worker medium local scoring is at least 30% faster than Phase 0 and no
  more than 5% slower than Phase 1.
- Small one-candidate grammar-exact wall time improves at least 25% relative to
  Phase 1.
- Peak RSS does not increase more than 25% on any reference workload.
- Canonical output, full CTest, and targeted ASAN tests pass.

Any representation that slows a mandatory reference workload by more than 5%
or violates the RSS gate must be removed or made workload-adaptive.

## Phase 3 — Add one persistent adaptive chart scheduler

### Goal

Establish safe orchestration before enabling concurrent chart algorithms.

### Actions

1. Create exactly one scheduler/pool lifetime per chart search. Reuse or extend
   `thread_pool`; do not create a pool in every candidate/pattern batch.
2. Honor explicit worker counts `1,2,4,8,16` plus `0/auto`. Report requested,
   resolved, and actually active workers. Automatic resolution must inspect the
   current process affinity and physical-core/SMT topology where available;
   `hardware_concurrency()` alone is not affinity-aware. Document the portable
   fallback.
3. Implement bounded task-group and dynamic indexed-range primitives with:
   - monotonic/stable task IDs;
   - adaptive minimum grain;
   - task-local counters and deterministic reduction;
   - deterministic exception selection and join-on-cancel.
4. Bind one Phase-2 scratch context to each safe worker/task ownership unit.
5. Prohibit a pool worker from blocking for child tasks queued to the same
   fixed pool. Later phases select one parallel axis before a batch, unless a
   separately tested cooperative work-first/help-while-waiting task group is
   implemented.
6. Report tasks submitted/completed/joined, pending tasks at shutdown, grain,
   queue waits, active-worker high-water mark, serial fallbacks, and live pool
   threads. Phase-specific axis/work-estimate policies land in Phases 4--6 and
   final automatic selection lands in Phase 10.

### Exit criteria

- A test proves one pool lifetime per search, including multiple iterations.
- Explicit one-worker scheduling matches the Phase 2 canonical oracle.
- Empty work, one item, fewer items than workers, exceptions, cancellation, and
  repeated searches are covered.
- Tests/counters prove every submitted task is joined, cancellation leaves
  zero pending work, and no pool thread survives the scheduler lifetime.
- Requesting eight workers on the small 64-candidate case is no more than 5%
  slower than explicit one worker because the scheduler chooses a serial grain.
- A parallel test proves that multiple workers actually execute tasks when the
  workload exceeds the threshold.
- Targeted TSAN and full CTest pass.

This phase establishes orchestration; it does not claim scaling by itself.

## Phase 4 — Parallelize pattern construction and local scoring

### Goal

Use the safest, widest chart axis: independent effective-site patterns, with
candidate x pattern tiling when either dimension alone is narrow.

### Actions

1. Parallelize the outer pattern loops for initial dense inside charts, exact
   active-pattern information, composite data, and cold inside/outside caches.
   Write to pre-sized pattern-indexed slots.
2. Fuse inside/outside preparation where it avoids a duplicate inside build.
3. Implement adaptive candidate x pattern/context tiling for local scoring:
   - coarse candidate tasks when there are many balanced candidates;
   - pattern/context tiles for one or few candidates;
   - dynamic weighted candidates when affected sets vary;
   - fixed-order weighted integer reduction.
4. Keep the four-state recurrence inside a task. Add vectorization or pattern
   batching only when its isolated benchmark improves.
5. Do not clade-parallelize the current dense outside scatter. Pattern
   ownership is race-free; any future within-pattern outside parallelism must
   use gather-by-child or thread-local rows followed by min reduction.
6. Shard fixed-topology direct-oracle work by pattern. Any persistent structural
   cache must be worker-local/sharded or synchronize entry creation without
   completion-order-dependent output.

### Exit criteria

- Worker counts `1,2,4,8` give byte-identical canonical output for seeds
  `1,7,19`, dense local scoring, fixed-topology exact, and grammar exact.
- Medium 64-candidate dense local scoring at eight workers is at least 2.0x
  faster than the same-revision Phase-4 one-worker result. The Phase-4
  one-worker result is no more than 5% slower than Phase 3.
- Medium initial chart/cache construction at eight workers is at least 2.0x
  faster than its same-revision one-worker result.
- Four-to-eight-worker wall time may not regress by more than 10% on a phase
  with enough tasks. Record user/system CPU; a system/user ratio above 25% is a
  mandatory profiling and rollback investigation, not by itself a correctness
  failure when wall/RSS gates pass.
- Eight-worker peak RSS remains within the global final RSS bound.
- Full CTest and targeted TSAN pass.

If allocator/system-time contention still prevents positive scaling, return to
Phase 2. Do not mask it by adding more candidate tasks.

## Phase 5 — Parallelize one exact B&B

### Goal

Reduce the dominant single-call exact span, including initial exact-state
construction and top-K-one workloads.

### Actions

1. Consume the Phase-4 pattern-parallel active-pattern inside/outside builder;
   do not introduce a second implementation in B&B.
2. Process frontier clades in increasing dependency-level wavefronts. Every
   task owns one clade result and starts only after all child levels publish.
3. Conditional only if one clade dominates runtime, partition its production x
   left-frontier x right-frontier Cartesian product into stable ranges, using
   worker-local maps and a canonical merge without nested waits.
   **Measured decision:** the frozen medium W1/W8 profile attributed zero of
   1,192 combinations and 0.000 ms to this path (0% of post-wavefront work).
   The conditional implementation and its scheduler/report surface were
   therefore removed under the omission rule below.
4. Preserve a fixed pruning upper bound within a parallel frontier wave where
   schedule-dependent incumbent discovery would otherwise change work or
   diagnostics.
5. Store frontier pattern-cost vectors and masks compactly and delay full
   provenance copying until a candidate survives the relevant prune/dedup
   stage. Keep equality/provenance merging deterministic; do not use atomic-min
   or first-completer provenance.
6. Profile dominance pruning. If it remains material, implement a parallel
   mark/compact pass or a better exact skyline/index algorithm; retain the old
   oracle until exact-mask equivalence is proven.
7. Vectorize/batch arithmetic across pattern components where profitable.
8. Cache immutable active/composite inputs across two-pass exact-mask recovery.
9. Expose per-level frontier sizes, logical product work, pruning, and time in
   diagnostics. Heavy-clade-only fields are omitted with conditional Action 3.

### Exit criteria

- For every existing dominance mode, one- and eight-worker exact optimum,
  exactness labels, keep masks, frontier sizes, recovery counters, and canonical
  retained provenance/topologies are identical.
- A test asserts that no clade task starts before all child levels publish.
- Both same-revision one- and eight-worker medium one-candidate diagnostics
  complete within the Phase-0 frozen 600-second measurement timeout; the
  eight-worker diagnostic additionally completes within 180 seconds.
- Its eight-worker exact initialization/B&B phase is at least 2.0x faster than
  its same-revision one-worker phase.
- Phase 5 does not exceed the global RSS bound and is TSAN-clean.
- Full CTest passes.

If heavy-clade splitting accounts for less than 10% of the post-wavefront
profile, document and omit it instead of adding ineffective complexity.

## Phase 6 — Parallelize exact top-K candidates

### Implementation status (2026-07-15)

The Phase-6 implementation is complete at
`870c298ff1c0c21901bdf79d341bf97d121f389c`. Stable exact-candidate waves,
task-local verification state, coordinator-ordered aggregation, and the
unified hard memory-admission path pass the in-tree Top-K `{1,4,16}` by worker
`{1,2,4,8}` semantic matrix, stable-failure and partial-submission tests,
finite-budget admission tests, repeated-W8 identity test, full RelWithDebInfo
CTest, and targeted TSan. Reproducible unsealed evidence is recorded under
`build/wric-chart-parallelization/phase6-870c298/` and summarized in the
results ledger.

The immutable measurement checkpoint remains
`7ca527b8906d018124756182274335cbff936d72` (`Baseline measure`). Its frozen
runner, oracle, workloads, and binaries are unchanged. Its wrapper calibration,
all 13 captures, approvals, finalization, pending/final audits, and seal have
now completed, so the sealed base is available. Phase-6 performance acceptance
nevertheless remains pending. In particular, the later-phase
medium Top-K-4 scaling gate, real Top-K-16 admission gate, paired W1/W8 RSS
gate, and frozen-workload repeated-output capture cannot be inferred from the
passing in-tree functional contracts or benchmark-postprocessor regressions;
they must be run against the sealed base.

### Goal

Exploit independent retained candidates after single-call exact work is under
control.

### Actions

1. Build and publish the old exact trim once before launching candidate tasks.
2. Give each verifier task private counters, scratch, transient overlays, and
   result storage while sharing immutable baseline data.
3. Estimate per-candidate B&B memory and admit tasks only when both worker and
   memory budgets allow. Report queued-for-memory time and peak concurrency.
4. Write results by stable lower-bound rank/candidate signature, then perform
   exact winner selection and commit serially.
5. Choose the parallel axis before launching each exact batch: with enough
   candidates, run candidate-parallel and keep each B&B serial; with one/few
   candidates, run Phase-5 clade/pattern-parallel B&B and keep the candidate
   loop serial. A pool worker may not synchronously wait for B&B children
   queued to the same pool. A cooperative work-first alternative is allowed
   only after a dedicated no-deadlock/help-while-waiting test. Never create a
   nested pool.
6. Make fixed-topology persistent caches safe through sharding/task-local
   caches and deterministic merge, or synchronized construction with disjoint
   fills.

### Exit criteria

- Top-K `1,4,16` produce identical per-candidate scores, exceptions, masks,
  provenance, winning candidate, and canonical report at `1,2,4,8` workers.
- Eight-worker medium top-K-4 `exact_verification_ms` is at least 2.0x faster
  than the same-revision one-worker result.
- Top-K-16 never exceeds the configured concurrent-memory admission bound.
- Eight-worker peak RSS is no more than 2.0x the corresponding one-worker RSS.
- Repeated eight-worker results are byte-identical.
- Exact-search TSAN cases and full CTest pass.

## Phase 7 — Make lazy charts scalable and adaptive

### Goal

Retain lazy-chart wins where structural compression exists while avoiding the
medium fixture's nearly unique context-key failure mode.

### Actions

1. Replace recursive retain-all inside construction with increasing clade-level
   wavefronts. Tasks at one level read smaller immutable child slots and own
   distinct output clades.
2. Build lazy outside rows in decreasing-level wavefronts, keeping all
   productions for one clade within one task or min-reducing task-local rows.
3. Compute context keys in pattern blocks, then stable-sort/deduplicate and
   build distinct representatives concurrently. These are two orchestrator
   stages separated by a join: a clade worker may not launch and wait for
   pattern-key or representative-row subtasks in the same fixed pool.
4. Replace `std::map<vector<...>,...>` and repeated structural strings with
   packed/interned keys plus deterministic grouping.
5. For sparse mode, defer map destruction to a level barrier or add safe
   reference counting. Never free a child map while a peer can read it.
6. Avoid copying whole lazy charts and rebuilding reference-state outside
   charts solely for diagnostics.
7. Add `auto` policy based on a bounded pilot of structural-class ratio,
   row-merge ratio, estimated bytes, and key-build cost. Explicit `on` and
   `off` retain their meanings.
8. Add and commit deterministic
   `test/wric_lazy_high_compression.pb.gz` plus
   `test/wric_lazy_high_compression.ref`, with at least 64 active patterns,
   structural-class ratio at most 0.25, lazy/dense retained-row ratio at most
   0.50, and enough repeated work for the one-worker measured lazy phase to
   exceed 50 ms. Also add
   `test/wric_lazy_dense_favoring.pb.gz` plus `.ref`, with at least 64 active
   patterns and forced dense at least 10% faster than forced lazy. Freeze both
   through frozen Phase-0 `dagutil` into immutable supplemental manifest
   `build/wric-chart-parallelization/supplements/phase7-lazy.tsv` before using
   their gates. Use
   `data/test_5_trees/tree_0.pb.gz` as the named small-overhead fixture.

### Exit criteria

- Forced lazy matches forced dense exact scores, masks, fluidity provenance,
  accepted sequence, and final canonical topology.
- When forced medians differ by more than 10%, auto chooses the faster safe
  representation; when they differ by at most 10%, either choice is allowed.
  The two named frozen fixtures must exercise both dense and lazy branches. Do
  not require a particular seedtree decision if optimization changes which
  forced representation is genuinely faster.
- Auto median wall time is no more than 1.10x the faster forced representation
  on the medium and high-compression fixtures.
- Eight-worker forced lazy is at least 1.5x faster than same-revision one worker
  on the high-compression fixture and has no more than 10% overhead on the
  named small fixture.
- Sparse reclamation stress tests and TSAN pass.
- Full CTest passes.

Do not make `auto` the default until all these gates pass.

### Historical immutable product P checkpoint (diagnostic; superseded for evidence)

Immutable product P was
`3b0e442e213a7913c6e007c76715d7d856bce0bd` (`Parallelize WRIC chart search
pipeline`). For finite, full-retention lazy charts over strict binary trees
with more than one worker and no observing hook, P replaces one scheduler
operation per dependency level with one dependency-ready scheduler operation
for the complete inside phase and one for the complete outside phase. A
fixed-capacity ready queue, atomic child readiness, stable output ownership,
certified worker-output accounting, joined deterministic failure selection,
and cleanup/retry coverage preserve the serial semantics. W1, generic DAG,
sparse, observer-enabled, and uncertified/budget-miss cases retain the
existing path.

P also caps lazy candidate-local concurrency at three quarters of resolved
workers when the active-pattern stream has at least 1,024 patterns. Thus W8
requests eight tasks but admits six; smaller streams, W1, W2--W7, and batches
already no larger than the cap are unchanged. The same planner controls the
runtime waves and finite-memory envelope. Reports expose:

- `lazy_chart_{inside,outside}_dependency_ready_executions`;
- `lazy_chart_{inside,outside}_dependency_ready_jobs`;
- `lazy_chart_{inside,outside}_dependency_ready_scheduler_operations`;
- `lazy_chart_{inside,outside}_dependency_ready_capacity_resident_bytes_max`;
- `lazy_local_requested_concurrency_max`;
- `lazy_local_effective_concurrency_max`; and
- `lazy_local_bandwidth_capped_batches`.

The exact focused RelWithDebInfo matrix passed 9/9. A prior in-tree full-suite
run appeared to account for all 176 tests, but the clean detached P rerun
exposed that `chart_spr_test` could fail its
`grammar_candidate_active_worker_high_water >= 2` assertion when scheduler
timing did not overlap the workers. P is therefore not the current full-suite
evidence product. The retained six-repetition
`diag-dependency-ready-r6` high-compression matrix has forced-lazy median wall
times `0.202869868500` seconds at W1 and `0.131911820500` seconds at W8,
W8/W1 `0.650228747499`; forced dense at W8 is `0.122693050000` seconds, so
lazy/dense is `1.075136859830`. All 24 canonical sidecars have SHA-256
`ae62f9153f5a0f8af0646db23e08088790c727f4a7990cbe1a6a5e71d97b84c6`.

The separate `diag-auto-policy-r6` matrix resolves the dense-favoring fixture
to `off` and the high-compression fixture to `on`. Its dense auto median is
`0.040736641000` seconds, `1.001861922329` times the fastest forced dense
median; its high auto median is `0.131609652500` seconds,
`1.072674063445` times the fastest forced high-compression median. The dense
and high canonical sidecar SHA-256 values are respectively
`24cc66ce9fa739d4066d75651d38eb4e5fdec4b12c02c48263802b22f3f904c9`
and
`ae62f9153f5a0f8af0646db23e08088790c727f4a7990cbe1a6a5e71d97b84c6`.

These matrices are historical same-product working diagnostics only. They did
not replace the later sealed Q recaptures, small-fixture/RSS decisions, or
Q's subsequently completed targeted TSan gate, and they do not make `auto`
the default. The complete Q/C4 campaign and C7 decision are recorded in the
results ledger; candidate-bound Phase-7 and final acceptance remain pending.

### Immutable product Q normal-suite checkpoint

The current evidence product is immutable Q,
`a9db72e60f153a95362db544107373817a58a258` (`Stabilize grammar worker
overlap test`), tree
`904f35c58daa1c331f2c43f3aba373f8c8858d4e`, with P as its sole parent.
Q replaces the timing-sensitive overlap observation with a deterministic
latch-based test; it does not discard the P implementation or diagnostic
tables.

The locked detached Q root passed the literal normal command with an inventory
of 176 unique tests: 174 passed, zero failed, and exactly
`merge_consistency_test` and `rotaA_diagnostic_test` skipped. The sealed
evidence root is
`/home/ogi-agent/matsen/larch2-wric-evidence/product-a9db72e-normal-full`;
`full.ctest.log`, `full.LastTest.log`, `posttest-provenance.txt`, and
`artifacts.sha256` have SHA-256 values
`15fb82e99b72ecadeb0b2bc28df28d01e4c36fea46cefcd6688b9067be34d76c`,
`ace8762f67f1d407ccc461203e2ac75e086dd53db5c51487a02da5852c5fb0c8`,
`80d80c2f1405cb1474cfc95e21365fb18182bc35c9b9ac29b3dc5173b8c3fc52`,
and
`d6b36894d951aa89e3ba983b271b2129c1c83f09817184dc67d7761f8b1cf04c`.
This closes Q's normal gate only.

### Immutable product Q full-ASAN checkpoint

The same clean Q revision passed the complete serial ASAN suite with
`ASAN_OPTIONS=halt_on_error=1:detect_leaks=1` and the pinned GCC-trunk runtime
library path. The inventory is exactly 176: 174 passed, zero failed, and only
`123 - merge_consistency_test` and `130 - rotaA_diagnostic_test` skipped for
their unchanged reasons. All 176 records appear in `LastTest.log`; the suite
took 728.20 seconds. The explicit AddressSanitizer, LeakSanitizer, and detected
leak scan contains zero matches, and Q was clean and exact before and after.

Evidence is sealed at
`/home/ogi-agent/matsen/larch2-wric-evidence/product-a9db72e-asan-full`.
`full.ctest.log`, `full.LastTest.log`, `diagnostic-scan.log`, and
`postflight.log` have SHA-256 values
`5abb6db6ea594359cd10eb66f6bdab1949684009e27093e83addaaa468981d1f`,
`8cbc3f3d364d5c2d1d72d9dcca9f8aaeaad0937f36fcc7ab1cdd2ef2addc4fa7`,
`00adc251e5163809e3fef0b04edb2b6d71c8637fe69999ebefcb74aa3b958c1a`,
and
`b5cf0ac93a3f3316775a9698f757de560f159fe1b98e9bc0704390fc9bc2875b`.
The complete `SHA256SUMS` file has SHA-256
`ac339eee73459489962f844f437b0e7e11c78051c972d1d3d153e93a913683fd`.
This closes Q's ASAN gate; the targeted TSan closure is recorded immediately
below. At this checkpoint the timed recaptures were pending; C4 subsequently
completed and audited all 20 Q components.

### Immutable product Q targeted-TSan checkpoint

The same clean Q revision exposes a full inventory of 176 and an exact
55-test selection under the required regex. The selected suite ran serially
with `TSAN_OPTIONS=halt_on_error=1`, `LD_PRELOAD` unset, and the pinned patched
runtime first in `LD_LIBRARY_PATH`; all 55 passed with zero skips or failures
in 281.86 seconds. Eleven representative binaries all resolve
`libtsan.so.2` to the patched bytes with SHA-256
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.
The strict ThreadSanitizer, data-race, lock-order, use-after-publication,
thread-leak, summary, warning, and fatal scan contains zero matches.

Evidence is sealed at
`/home/ogi-agent/matsen/larch2-wric-evidence/product-a9db72e-tsan-focused`.
`targeted-inventory.log`, `focused.ctest.log`, `focused.LastTest.log`,
`runtime-provenance.log`, and `diagnostic-scan.log` have SHA-256 values
`a31ed742b506e2c39393bc835fad8e2e3115bd99e629acb92e9395f9218987b7`,
`a3cbef485a81da355096734757c0607d0102f65bcafd9ed25cb31ee9b95811a2`,
`67a1e94c0344eb1a5e427d6c1f7bb9b9f8a528d7e68cccf1b03e9f0fdc46fb91`,
`9a79f46db76cd3322f26bb9d414841e3a3eed2b6a2aa1c1ec471e50544228ff3`,
and
`6615793e3480df9ae24df6ec7813afd53ce5b44f0586513ceacdae761dc0db29`.
The complete `SHA256SUMS` file has SHA-256
`5b90a3f931853a03aec20bdaf2b44ff71c7f2d1afddc54c42a51a3f7c7f4eef1`.
This closes Q's normal/ASAN/TSan gates. The later C4 campaign completed all Q
timed recaptures, and C7 failed its pre-default decision on the four gaps
recorded in the results ledger. Any later product revision reopens the
affected product-bound gates.

## Phase 8 — Parallelize and pipeline candidate generation

### Implementation status (2026-07-17)

The committed hardening chain through `e3eec44` extends the earlier `6c8d0c7`
checkpoint without changing the canonical source/RNG contract. `0c0fbb9`
preserves ordinal failure selection after speculative work joins; `b66e115`
adds finite and adaptive sampled-source waves under the unified temporal
memory envelope; and `6e8167d` sizes direct-projection workspace vectors by
stable scheduler slots while charging dynamic storage only for the active
wave. The same checkpoint quarantines a failed direct workspace for the rest
of its joined wave/subwave, including nested same-scheduler serial fallback,
and removes the stale retained-candidate charge from the post-generation
evidence phase. `e3eec44` adds sampled/hybrid reservoir matrices and the first
direct/outer width-two admission tests. `94a6323` hardens those tests: the
reservoir matrix now replays Algorithm R and its final shuffle independently
over the exhaustive canonical child stream for every required seed; the outer
unified-memory test uses the unmodified production state to prove a realizable
adaptive boundary where exact budget `E` admits source width two while `E-1`
executes at source width one; and the direct child test explicitly pins width
two and rejects `E-1` before source/workspace/scheduler side effects. The outer
test compares every canonical old-state evidence field across both widths and
does not synthesize duplicate provenance or claim presemantic rejection at an
unreachable state. The focused RelWithDebInfo `chart_spr_test` and
`chart_spr_pipeline_test` binaries both pass at this checkpoint.

The targeted TSan result at `6c8d0c7` is historical evidence only. An earlier
coherent pre-cleanup production-code full RelWithDebInfo suite has zero
failures across 176 registered tests at `1923bbc` (174 pass and the two
established tests skip); the only
later code/test change, `51d2a3c`, pins `phase8-generation` to immutable
checkpoint `94a6323`, and its focused 26-case capture-contract test passes.
At `51d2a3c`, the complete serial ASAN suite has zero failures across 176
registered tests (174 pass and the same two established code-77 tests skip),
and the exact final TSan regex passes 55/55 with the pinned patched runtime and
no diagnostic. The post-anchor full normal suite also has zero failures across
176 registered tests at `51d2a3c` (174 pass and the same two tests skip).
Those results close only the pre-cleanup correctness and sanitizer checkpoint.
The last committed post-cleanup full RelWithDebInfo, ASAN, and targeted TSan
gates passed at `4ef6126` as recorded under Phase 10 below. The later Phase-8
production changes reopened all three gates; the full RelWithDebInfo, serial
ASAN, and exact targeted TSan gates have since rerun and passed on the exact
bytes now committed as immutable product revision R. The supplemental fixture
seal and first quiet-host generation attempt have also been completed as
recorded below. That attempt failed the generation-speed gate. The additive
optimized retry described below is now sealed and passes that gate; the Q
recaptures, end-to-end comparison, and all remaining final gates stay open.

### Sealed generation attempt 0 (failed 2026-07-22)

The original historical run label `phase8-generation` remains bound to full
product revision `94a63238d25a8e3262428419d53f8f0986e8879b`. Its immutable
capture-tool revision is
`b6ae1a968c0a2374d8180e5682ae53775377c31e`, routed through detached root
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-b6ae1a9`; its exact
capture-wrapper SHA-256 is
`e374ed726e026ab973ffa9ff385b13cd8c0eea8041d32b861df61fb4fa6c652e`.
The attempt-0 product root is the detached worktree
`/home/ogi-agent/matsen/larch2-wric-evidence/phase8-generation`. Its frozen
supplement is sealed at SHA-256
`773545d093904c20eaa80343615fa77089989735e61d34947530b07ef008cf17`.
The frozen-oracle W1 characterization reports `1044.731 ms` of candidate
generation and 256 post-dedup candidates, so the named fixture satisfies the
required at-least-100-ms and at-least-256-candidate qualification.

The quiet-host five-repetition capture is preserved at
`/home/ogi-agent/matsen/larch2-wric-evidence/captures/phase8-generation/phase8-generation__workers-1-8__reps-5`.
Its `raw_trials.tsv` SHA-256 is
`e3eb3b3d9e77fc4006aa2c701102f39f84244971fae4f794bf66e5ca72cf4a56`,
and its independently audited `wric-evidence-run-ledger.tsv` SHA-256 is
`d22f42f31cf4e37bc92abc53e70f938d163f5fc422af68677c0f7dd7244841e1`.
At `2026-07-23T00:18+03:00`, a fresh read-only audit routed through the frozen
b6 wrapper/root under the physical-core affinity returned `status=audited`
with all 166 ledger members and the same ledger hash; no capture bytes changed.
The W1 and W8 candidate-generation medians are respectively `48.783 ms` and
`62.822 ms`: W8/W1 is `1.287784679`, which decisively fails the required
`<= 0.50` gate.

The failure is not semantic or a workload/RSS escape. All measured W1/W8 rows
completed with successful validation and matching search/output semantics;
each generated and scored 256 candidates and performed one exact
verification, while both enumerated four sources and 405 moves with the same
nine discarded source-speculative moves. Their maximum sampled RSS values are
18,296 KiB and 18,516 KiB (`1.012` W8/W1), with no RSS-limit or swap event.
Those supported gates pass, but they cannot compensate for the failed speed
gate or close the still-uncaptured Phase-7 end-to-end comparison.

Attempt 0 is immutable failure evidence. It must not be deleted, overwritten,
relabelled, resealed around a different executable, or silently reused for a
later product revision. Its optimized successor was required to use a new
monotonically named run/capture label, pinned immutable product and
capture-tool identities, a distinct capture directory and ledger, and new
external hash anchors. The sealed `phase8-generation-retry1` successor below
satisfies those provenance requirements without replacing this failed
observation; any later retry must remain additive and increment the suffix.

### Immutable optimized product revision R (locked 2026-07-23)

The accounting-complete optimized product is committed at exact revision
`07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c` (R). Its clean detached,
locked measurement root is
`/home/ogi-agent/matsen/larch2-wric-evidence/phase8-generation-retry1-product`.
The product-local `build/bin/dagutil` has SHA-256
`f3ccb220b698e0bd6bf0b2951e73d5204aa289c35aa18b5678169668cee12161`.
Revision R intentionally contains no retry capture or evaluator tooling. The
later tooling and sealed capture are descendants with independent identities,
so the measured product identity did not drift with the evidence machinery.

### Accounting-complete immutable-R diagnostic (non-accepting 2026-07-22)

The post-attempt-0 optimization keeps the W1 projection path unchanged while
allowing the bounded W>1 product path to retain up to 64 projection ordinals
per worker. Projection uses four target ranges per worker; after it joins, a
second unit-grain scheduler operation performs postconstruction filtering and
canonical dedup-key construction before the serial gather publishes counters,
errors, RNG/cap decisions, callbacks, and final order. The W>1 classic-locale
dedup key is a count-delimited binary encoding with the same equality relation
as the historical textual key. W1 and public/report signatures retain the
historical representation; standalone custom-locale generation falls back to
that representation, while finite W>1 sampled admission rejects a custom
global locale before work rather than mixing an unaccounted encoding.
Candidate-owned after-certificate references avoid a second direct-projection
lookup pass.

The finite-memory envelope now charges each retained signature, active-worker
postprocessing scratch, its scheduler operation, and the binary dedup-set
node. Runtime actual-capacity backstops and exact-budget rejection remain
authoritative. The unit-grain projection experiment was reverted, and the
superseded fast textual serializer and unused prepared-destination API were
removed rather than retained as dormant alternatives.

On the sealed Phase-8 fixture, one warmup plus five W1/W8 measured pairs in
`build/wric-chart-parallelization/phase8-accounting-complete-clean-diagnostic/`
produced:

| Immutable-R candidate-generation metric | W1 | W8 | Diagnostic result |
|---|---:|---:|---|
| five-trial values (ms) | 48.927, 48.828, 48.617, 48.498, 50.252 | 19.292, 18.819, 19.997, 19.576, 19.707 | complete |
| median (ms) | 48.828 | 19.576 | W8/W1 = `0.400917506349` |
| implied speedup | - | - | `2.494278709x` |

All ten measured rows have successful execution and validation, identical
search/output semantics, 256 generated and 256 scored candidates, and one
exact verification. Maximum process RSS is 17,564 KiB at W1 and 17,628 KiB at
W8; every row reports zero sampled swap. The exact diagnostic executable and
`raw_trials.tsv` SHA-256 values are respectively
`dd64567e43404c7bd86b80d2bd9319bb50d13b46974a40cf748274dd33d6fa5f`
and
`4bfd1339ae5021a82c473dd247687cbaf4b12b2b534e8dcffc3c604ed12985cc`.

This diagnostic is deliberately not attempt 1 and does not change the sealed
attempt-0 failure. At capture time it lacked the exact outer retry label,
independent capture-tool identity, metadata, audited ledger and seal, and
external hash anchors. The later sealed retry1 below supplies those artifacts.
Neither run closes the separate Phase-7 end-to-end comparison. On the exact
product bytes committed as R, the commands

```sh
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --parallel 8
```

completed the full RelWithDebInfo gate: 176 tests were registered, 174 passed,
zero failed, and exactly `123 - merge_consistency_test` and
`130 - rotaA_diagnostic_test` skipped for their unchanged reasons. CTest took
197.38 seconds. This closes the immutable-R full normal gate; any later product
change reopens it. The serial ASAN gate then used the plan's exact commands:

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

The serial ASAN CTest registered 176 tests: 174 passed, zero failed, and
exactly `123 - merge_consistency_test` and `130 - rotaA_diagnostic_test`
skipped for their unchanged reasons. It took 842.54 seconds. A grep of
`build-asan/Testing/Temporary/LastTest.log` found no `AddressSanitizer`,
`LeakSanitizer`, or detected-leak diagnostic. This closes the immutable-R ASAN
gate; any later product change reopens both gates for that later product.

The immutable-R targeted TSan gate used the exact 55-test regex in the `### TSAN`
section below. Its selected inventory is preserved at
`build/wric-chart-parallelization/phase8-tsan-pre-r/inventory.log`, SHA-256
`b49508c42ca10a2bc94ed54bf826edb57abc0540867fda3676c31ae8db72da86`.
`LD_LIBRARY_PATH` selected the validated patched runtime first and GCC-trunk
`lib64` second, without `LD_PRELOAD`; representative `ldd` output confirms
that selection. Patched `libtsan.so.2.0.0` has SHA-256
`58725dae226e91ea96bebbdf54f84820638404a691528570ec1dab595ed08842`.
With serial CTest and `TSAN_OPTIONS=halt_on_error=1`, all 55 selected tests
passed with zero failures in 288.11 seconds. The CTest output contains exactly
55 `Passed` records, the preserved LastTest log contains exactly 55
`<end of output>` records, and the diagnostic scan has no match. The CTest and
LastTest log SHA-256 values are
`09fc0239311c114810fbd7f6623452d6df3de90b9ecbd94cfbdde03860b7a25f`
and
`18d2bc1c7552db146d4ed98d039cb4135f41204fa4fab8df3b04601de1ae5628`.

This closes the immutable-R TSan gate. The additive retry1 is now sealed and
passes its generation-speed gate, while Q is a later product and therefore
requires fresh normal/sanitizer and timed evidence wherever the plan requires
current-product closure.

### Sealed optimized generation retry1 (passing 2026-07-23)

The additive `phase8-generation-retry1` capture is sealed on immutable product
R `07309523cf3a3aaa9e5095f4d4b1d0f98ac4557c` and immutable capture tool
`0013fcc7231c32bce0fb5ccb11eaae62dacea4c5`. Its distinct capture directory is
`/home/ogi-agent/matsen/larch2-wric-evidence/captures/phase8-generation-retry1/phase8-generation-retry1__workers-1-8__reps-5`.
The raw TSV and audited ledger SHA-256 values are
`0e5eff54263cb1d1c3da15ac8d21ec9d456993992b4a60189ae2105f1238e36a`
and
`5a307b51f1ba662b1484e1919f03a8b18f17af934522953e06c039ef90501fdd`.

The five-trial candidate-generation medians are `48.905 ms` at W1 and
`19.712 ms` at W8, so W8/W1 is `0.403067171046` and passes the required
`<= 0.50` gate. Median end-to-end wall times are `0.151872 s` at W1 and
`0.131355 s` at W8. This changes the Phase-8 generation-speed result from
pending to passing while preserving attempt 0 as immutable failure evidence.
It does not establish the Phase-7 same-workload non-regression decision, Q
recaptures, default promotion, or final acceptance.

### Immutable Q-aware capture-tool C (superseded before capture)

The historical capture/evaluation tool checkpoint C is immutable commit
`03ff9f12d0b479321ea1c887a3112fed1d34ef71` (`Pin Q acceptance capture
retries`), tree
`05d70b80cd05fd33d2fad4a42586c29066039191`, in locked detached root
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-03ff9f1`.
Its capture wrapper, generic ledger tool, and cross-phase evaluator SHA-256
values are respectively
`786b8a17874c00f89693b6d8d399c447d6e5a79a144ab9a304be33dcb4133ce1`,
`def3ce4b6868b16c050599b918240ba600e204dcf884caea36bffe52769cd29f`,
and
`4503d67e3804b83aab04da5b8787b4eb846819ec89745a1e5aecc4fbb43b72c1`.
The two focused registered tooling tests pass 2/2.

The evaluator is schema v5. It retains attempt 0 and retry1 with their own
historical product/tool roots and immutable anchors, while current recapture
labels are pinned to Q. `pre-default` still emits
`completion_eligible=false` and defers `final-default-auto`; only `final`
requires that label and can be completion-eligible. The loader verifies
metadata, ledger closure and detached seals, rejects aliased or reused
provenance, and repeats closure checks to fail closed against TOCTOU changes.
C was not used for a timed capture. A final dry-run audit found that its
Phase-9 delegation selected C's sibling evaluator while claiming Q as the
working product root, so the product evaluator correctly rejected the route.
That fail-closed defect is preserved here and corrected only by additive C2.

### Immutable Q-aware capture-tool C2 (superseded before capture)

Historical capture/evaluation checkpoint C2 is immutable commit
`af4f2011d8d462cceaab7efe1d443eaa4c207fc8` (`Bind Phase 9 acceptance to
product root`), tree
`3233c90da3d3f408e5bde32db57fa926b83dd9bd`, with C as its sole parent, in
locked detached root
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-af4f201`.
Its capture wrapper and generic ledger bytes are unchanged at SHA-256
`786b8a17874c00f89693b6d8d399c447d6e5a79a144ab9a304be33dcb4133ce1`
and
`def3ce4b6868b16c050599b918240ba600e204dcf884caea36bffe52769cd29f`;
the schema-v6 cross-phase evaluator SHA-256 is
`84d1fdfec0dcec611c1128d2d7348a8c179d543ac3483e2985264683eaa861d0`.

C2 requires the exact Q-local `tools/wric_phase9_acceptance.py`, whose
working bytes and tracked HEAD blob must both match external SHA-256
`12eef19f3cad3dcd26363d6dfc951642cdd3b0647ddda98b6f6affb185314568`.
It authenticates clean Q repository/tool provenance before and after a
delegated run, invokes the source with a fixed isolated environment and no
ambient or ignored bytecode path, rejects any stderr, verifies the exact
Phase-9 schema-v2 result shape, and wraps that result with separate product
tool provenance. The targeted regression and both unchanged-timeout focused
CTest entries pass; the latter completed 2/2 in 38.87 seconds. No timed Q
capture used C2. A final capture-path audit then found that its Phase-9 seal
and audit subprocesses still used the Q tool's shebang, which could consume an
ignored but valid sibling bootstrap bytecode cache invisible to Git-clean and
tracked-blob checks. C2 is therefore superseded before capture by C3.

### Historical immutable Q-aware capture-tool C3

Historical capture/evaluation checkpoint C3 is immutable commit
`82aaec5813121230b82f45050b01e4bbd27a4b24` (`Isolate Phase 9 capture
postprocessor`), tree
`e0e25e1e58e2c5d7dc08fa04de079aa49c3dffda`, with C2 as its sole parent, in
clean detached root
`/home/ogi-agent/matsen/larch2-wric-evidence/capture-tool-82aaec5`.
Its capture wrapper, generic ledger tool, and schema-v6 cross-phase evaluator
SHA-256 values are respectively
`6ddfa017d03a035718a7a046c68bae76cfbe5ffb2b2578048165b76cb41730ec`,
`def3ce4b6868b16c050599b918240ba600e204dcf884caea36bffe52769cd29f`,
and
`84d1fdfec0dcec611c1128d2d7348a8c179d543ac3483e2985264683eaa861d0`.

C3 invokes both capture-time Phase-9 postprocessor modes with fixed
`-E -s -S -B -X pycache_prefix=/dev/null` interpreter isolation. This retains
the authenticated script directory required for the tracked sibling source
while excluding ambient Python configuration, site hooks, and ignored cache
reads/writes. An adversarial regression installs an unchecked-hash poisoned
sibling `.pyc`, proves an unisolated interpreter consumes it, then proves
capture and two repeat audits use tracked source while leaving the poison
byte-identical. The Phase-9-focused class passes 3/3, the complete registered
capture-controller test passes in 42.99 seconds, and the prior C2 cross-phase
suite remains passing because its evaluator bytes are unchanged. C3 was the
only permitted tool at that checkpoint; it was superseded before measurement
by the C4 tool recorded in the results ledger.

The independently audited execution driver is
`/home/ogi-agent/matsen/larch2-wric-evidence/run-q-c3-capture-campaign.sh`,
SHA-256
`71e94f3103249be23a358c258053ca5531af955556032c912ebc5f7aee7c6557`.
It pins every C3/Q/base/supplement/binary anchor, enforces the physical/SMT/
unpinned affinity classes, uses exclusive non-overwriting capture and receipt
paths, immediately re-audits every component, and validates exactly 430 rows
across 20 components. Its static and independent audits passed; no capture was
executed with this historical C3 driver. C4's successor driver later completed
and audited the 20-component/430-row Q campaign.

### Goal

Remove the serial source-enumeration/projection phase, especially for
sampled-tree fixed-topology mode.

### Actions

1. Partition grammar/source enumeration and sampled-tree projection by stable
   source-node or sampled-tree identity.
2. Produce task-local candidate buffers. Perform canonical deduplication,
   caps/reservoir selection, and final ordering after the join.
3. Preserve the Phase-0 sequential RNG stream exactly: either preassign draws
   in canonical serial source order before parallel work or prove an indexed
   mapping reproduces every Phase-0 draw/selection. Merely hashing seed plus
   identity is not equivalent and is insufficient.
4. Add bounded double buffering so generation of the next candidate batch may
   overlap current scoring. Stop, drain, and invalidate the next buffer before
   any accepted-state commit.
5. Report generation work, dedup/cap work, pipeline overlap, stalls,
   cancellation, and discarded stale-buffer work.
6. Freeze a high-candidate sampled-tree benchmark using
   `data/test_5_trees/tree_0.pb.gz` if it produces at least 256 post-dedup
   candidates and at least 100 ms of one-worker generation work. Otherwise add
   a deterministic committed fixture with those minimum counters; do not use
   an unnamed/ad hoc input for the speed gate. Seal its frozen-oracle row in
   immutable supplemental manifest
   `build/wric-chart-parallelization/supplements/phase8-generation.tsv`.

### Exit criteria

- Candidate identity multiset, capped order, source labels, and RNG-selected
  subset are identical for workers `1,2,4,8` and seeds `1,7,19` for grammar,
  sampled-tree, and hybrid sources.
- On a frozen high-candidate sampled-tree fixture, eight-worker generation is
  at least 2.0x faster than same-revision one worker.
- Pipelining never scores or publishes a candidate from the wrong state
  generation; forced early-acceptance/cancellation tests prove it.
- End-to-end sampled-tree-fixed mode does not regress from Phase 7.
- RSS, TSAN, and full CTest gates pass.

## Phase 9 — Parallelize accepted-state cache updates

### Goal

Make non-vacuous multi-iteration search scale without weakening the
single-writer transaction.

### Actions

1. Build one immutable transaction plan containing the chain-tip index,
   affected inside/outside clades, compiled overlay child references, and
   materialization work. Share it across update phases.
2. Shard inside-cache recomputation by pattern, join, then shard outside-cache
   recomputation by pattern and join. Publish epochs/state only afterward.
3. Reuse the accepted candidate's exact trim/frontier result as the next old
   state where valid instead of discarding and rebuilding it.
4. Parallelize pattern projection/tip refresh or expose immutable row views to
   avoid copying every `[pattern][clade]` row.
5. Validate the tight outside affected-set policy by move class against the
   existing all-reachable oracle. Adopt it only where exhaustive differential
   tests prove equality.
6. Materialize the appended chain concurrently with cache computation only if
   it remains at least 10% of the same-revision local-commit profile, both read
   the same immutable transaction snapshot, and both join before publish.
7. Keep move selection, overlay append, generation increment, and final
   publication serial.

### Exit criteria

- `test/wric_four_taxon_misplaced` accepts a real move, so commit counters and
  barriers are non-vacuously exercised.
- Add and commit `test/wric_chart_three_accepts.pb.gz` plus
  `test/wric_chart_three_accepts.ref`. Under seed 1 and the frozen 32/4,
  three-iteration contract it must accept at least three real moves, have at
  least 64 active patterns, recompute at least 32 affected rows per accept, and
  spend at least 100 ms cumulatively in the frozen-oracle one-worker accepted
  update. Multi-iteration correctness tests also use seeds `7,19` at workers
  `1,2,4,8`; if those seeds cannot each produce three accepts, add separate
  frozen correctness fixtures without weakening the seed-1 performance case.
  Seal all of these frozen-oracle rows in immutable supplemental manifest
  `build/wric-chart-parallelization/supplements/phase9-local-commit.tsv`.
- Accepted sequence, state scores, exact masks, chain identity, epochs, and
  final canonical topology are byte-identical.
- In local-commit incremental mode, no row outside the dependency-derived
  affected clade set is recomputed and no unaffected cached row is copied or
  rebuilt. Conservative mode need only preserve its existing rebuild/counter
  contract. No exact old-state result is rebuilt when the accepted verified
  result is reusable.
- Eight-worker accepted-update time is at least 1.5x faster than same-revision
  one worker on a fixture where that phase is at least 10% of total one-worker
  wall time. The speed exemption applies only if every frozen representative
  local-commit workload, including the named nontrivial fixture, measures below
  10%; otherwise the gate is mandatory. Document any exemption and retain the
  simplest correct path.
- TSAN, ASAN targeted cache tests, Phase-10 counter contracts, and full CTest
  pass.

## Phase 10 — Integrate, tune defaults, and prove parity

### Current implementation checkpoint (2026-07-17)

Commit `4ef6126607c8da5f25d54928ea2fd6d152b89e29` completes the
cleanup review and required in-tree semantic matrix. The review removed the
obsolete, zero-only
`chart_spr_search_state::selected_topology_cache_admitted_bytes` diagnostic;
selected caches are task-local and no behavior or admission decision depended
on it. This public state-surface removal is not claimed to preserve source or
ABI compatibility. No superseded per-batch pool, duplicate production chart
builder, unused scratch copy, or temporary compatibility execution path
remained. The search-lifetime scheduler, bounded source-wave producer,
supported public wrappers, and cold/oracle fallbacks remain intentionally.

The expanded matrix covers automatic dense construction, multiparent
transient verification, non-vacuous three-accept local commit, and
multifurcating fixed-topology commit at W1/2/4/8. The scheduled multisite
frontier covers `score_ua_edge={false,true}` by every dominance mode by
pruning `{false,true}` by W1/2/4/8. Canonical digest/full-sidecar parity, exact
worker binding, dispatch/task accounting, pool lifecycle, and shutdown are
checked. Deterministic overlap witnesses in `chart_trim_test`,
`chart_spr_search_test`, and `chart_scheduler_test` satisfy the active-worker
high-water requirement without flaky timing assertions.

At clean `4ef6126`, the focused RelWithDebInfo gate passes 11/11, the complete
RelWithDebInfo suite has zero failures across 176 registered tests (174 pass
and the two established code-77 tests skip), the complete serial ASAN suite
has the same result with no diagnostic, and the exact targeted TSan matrix
passes 55/55 with no diagnostic. These are correctness and sanitizer results,
not Phase-0, timing, scaling, RSS, supplement, automatic-policy, or default
evidence. Their exact commands and hashes are recorded in the results ledger.
The Phase-8 production changes that are now immutable R postdate and reopened
the complete normal, ASAN, and targeted TSan gates. All three have since rerun
and passed on exact R, including the exact 55-test TSan inventory. P and then
Q postdate R; Q's sealed clean normal, full serial ASAN, and exact targeted
TSan gates pass. C4 later completed the timed Q campaign; C7 failed the four
recorded pre-default performance gates, and the optimized candidate's
authoritative acceptance remains pending.

### Actions

1. Remove superseded per-batch pools, duplicate chart builders, unused scratch
   copies, and temporary compatibility paths while preserving explicit
   one-worker execution.
2. Exercise cold/transient verification, conservative/local commit, dense,
   lazy/auto, fixed topology, grammar exact, hybrid, dominance modes, UA-edge
   conventions, multiparent DAGs, and multifurcations.
3. Choose the automatic worker/grain/memory policy from the frozen worker
   matrix. `auto` must report its decision and be within 10% of the fastest
   safe explicit worker count under the same affinity on the primary fixture.
4. First pass every correctness/performance gate using explicit `auto`. Then
   change the product default from serial to automatic, keep an explicit
   one-worker oracle and compatibility CLI, and rerun the omitted-worker-option
   command plus full CTest. Do not use the new default to establish the policy
   that justified changing it.
5. Update chart-SPR and benchmark documentation, report fields, and
   `doc/WRIC-CHART-PARALLELIZATION-RESULTS.md` with exact reproduction commands.
6. Run the full acceptance sequence below without unrelated host work.

### Exit criteria

Phase 10 becomes complete only after all strict final performance commands,
the omitted/default-worker command, full RelWithDebInfo/ASAN/TSAN gates, frozen
manifest/hash checks, results documentation, and every completion-checklist
item below pass. Explicit-auto success alone does not complete the phase.

## Test commands

### Normal build and complete suite

```bash
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure --parallel 8
```

All existing and new tests must pass. Skips are allowed only where an existing
test deliberately returns code 77 and the reason is unchanged/documented.

### Fast chart-focused development suite

```bash
ctest --test-dir build --output-on-failure --parallel 8 \
  -R '^(dagutil_chart_spr_.*|dagutil_wric_phase9_chart_bnb_.*|dagutil_wric_lazy_chart_.*|dagutil_wric_lazy_polytomy_fixed_topology_search|larch2_chart_bnb_.*|wric_phase10_sanity_table_smoke|wric_spr_search_benchmark_smoke|wric_bnb_trim_benchmark_smoke|thread_pool_test|parsimony_chart_test|site_patterns_test|lazy_key_grouping_test|chart_trim_test|chart_bnb_trim_apply_test|chart_spr_test|chart_spr_search_test|chart_spr_allocation_test|chart_spr_pipeline_test|chart_parallel_test|chart_scheduler_test|chart_two_chart_oracle_test|multifurcation_chart_oracle_test|overlay_chain_test|inside_chart_cache_test|outside_chart_cache_test|chart_spr_phase10_test|no_direct_chart_spr_multisite_helpers|no_direct_chart_spr_root_score_detail|no_chart_bnb_superset_topology_exact|no_overlay_chain_compaction_prefix_materialize|no_keep_production_without_exact_check)$'
```

Also run the dedicated target explicitly so a broad regex cannot hide a
missing test:

```bash
cmake --build build --target chart_parallel_test chart_scheduler_test --parallel 8
ctest --test-dir build --output-on-failure --no-tests=error \
  -R '^(chart_parallel_test|chart_scheduler_test)$'
```

### ASAN

```bash
cmake -S . -B build-asan \
  -DGCC_TOOLCHAIN=/home/ogi-agent/install/gcc-trunk \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_ASAN=ON \
  -DENABLE_TSAN=OFF
cmake --build build-asan --parallel 8
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 \
  ctest --test-dir build-asan --output-on-failure --parallel 1
```

The complete suite must pass without ASAN or leak diagnostics.

### TSAN

```bash
cmake -S . -B build-tsan \
  -DGCC_TOOLCHAIN=/home/ogi-agent/install/gcc-trunk \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_ASAN=OFF \
  -DENABLE_TSAN=ON
cmake --build build-tsan --parallel 8
TSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-tsan --output-on-failure --parallel 1 \
  -R '^(thread_pool_test|chart_parallel_test|chart_scheduler_test|parsimony_chart_test|chart_trim_test|chart_bnb_trim_apply_test|chart_spr_test|chart_spr_search_test|chart_spr_pipeline_test|chart_two_chart_oracle_test|multifurcation_chart_oracle_test|overlay_chain_test|inside_chart_cache_test|outside_chart_cache_test|chart_spr_phase10_test|dagutil_chart_spr_.*|dagutil_wric_phase9_chart_bnb_.*|dagutil_wric_lazy_chart_.*|dagutil_wric_lazy_polytomy_fixed_topology_search|larch2_chart_bnb_.*|wric_bnb_trim_benchmark_smoke)$'
```

CTest is deliberately serial here so the relevant concurrency is inside the
tested code. There must be no race, lock-order, use-after-publication, or
thread-leak report. If this toolchain cannot execute a sanitizer runtime, do
not waive the gate: record a minimal diagnostic and report the goal blocked.

## Strict final performance acceptance

The Phase-0 prerequisite for this section is satisfied: a passing wrapper
calibration was frozen by `prepare`, every required capture and approval
completed, and `finalize`, pending `audit`, `seal`, and final `audit` all
succeeded. Earlier development runs remain diagnostic only. Each result below
still has to be produced with the sealed base and pass its own canonical,
timing, scaling, RSS, supplemental-manifest, and policy criteria before it can
satisfy a phase exit criterion.

The Phase-0 harness extensions make the following commands executable. They
must not be replaced with informal `time` invocations.

### Worker-scaling and automatic-policy gate

Run the grammar-exact worker matrix before the paired parity gate:

```bash
taskset -c 0,2,4,6,8,10,12,14 \
  tools/wric_spr_search_benchmark.sh \
    --dagutil "$PWD/build/bin/dagutil" \
    --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
    --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
    --out-dir "$PWD/build/wric-chart-parallelization/final-scaling" \
    --run-manifest-group 'p0-primary-physical' \
    --workers-list '1,2,4,8' \
    --warmups 1 \
    --repetitions 3
```

The harness must keep the workload and canonical result fixed across the
matrix. Use these rows for the one-vs-eight physical-core speedup, RSS, and
phase-scaling gates. Measure SMT and automatic policy separately on all 16
logical CPUs:

```bash
taskset -c 0-15 \
  tools/wric_spr_search_benchmark.sh \
    --dagutil "$PWD/build/bin/dagutil" \
    --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
    --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
    --out-dir "$PWD/build/wric-chart-parallelization/final-scaling-smt" \
    --run-manifest-group 'p0-primary-smt' \
    --workers-list '1,2,4,8,16,auto' \
    --warmups 1 \
    --repetitions 3
```

Choose best/auto comparisons only among rows with the same affinity.

Run the named small overhead guard separately; compare chart rows and phase
times, not the native small-fixture polling floor:

```bash
tools/wric_spr_search_benchmark.sh \
  --dagutil "$PWD/build/bin/dagutil" \
  --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
  --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
  --out-dir "$PWD/build/wric-chart-parallelization/final-small-auto" \
  --run-manifest-group 'p0-small-auto' \
  --workers-list '1,auto' \
  --warmups 1 \
  --repetitions 5
```

### Primary medium gate

```bash
taskset -c 0,2,4,6,8,10,12,14 \
  tools/wric_spr_search_benchmark.sh \
    --dagutil "$PWD/build/bin/dagutil" \
    --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
    --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
    --out-dir "$PWD/build/wric-chart-parallelization/final-medium" \
    --run-manifest-group 'p0-primary-physical' \
    --workers-list '1,8' \
    --warmups 1 \
    --repetitions 5 \
    --require-wall-ratio chart_spr_grammar_exact@8=1.0
```

All of the following must hold:

1. Every measured process exits zero and validates its output.
2. Every exact chart run scores 32 candidates and exact-verifies 4, unless
   the canonical candidate stream proves genuine exhaustion and the same
   exhaustion occurs in both its same-revision one-worker oracle and the
   frozen Phase-0 canonical stream.
3. Candidate source, acceptance, objective, verification, commit, mask,
   output-exactness, lazy/dense, pattern-batch/cache, dominance,
   polytomy/refinement, seed, and budget labels match the frozen manifest row.
4. A committed move never worsens its exact chart objective or externally
   validated final parsimony.
5. The `grammar_exact` median wall time is no greater than the paired native
   sample--SPR--merge median. This is the non-negotiable parity gate.
6. At least three of five paired grammar-exact trials individually meet or beat
   their paired native trial.
7. Every paired grammar-exact/native wall-time ratio is at most 1.15; an
   unrelated native outlier may not hide a chart outlier.
8. `sampled_tree_fixed` and `hybrid_exact` match their one-worker canonical
   semantics and do not regress from their Phase-0 same-workload median. Any
   mode claiming parity must separately have median ratio at most 1.00. If a
   Phase-0 mode has no finite median because every frozen trial timed out,
   completing the identical workload is the non-regression evidence; do not
   manufacture a timeout-derived median.
9. Eight-worker grammar-exact median wall time is at most 0.50x its explicit
   one-worker median on the same contracted workload.
10. Initial chart construction, local scoring, and exact verification each
    achieve at least 1.5x speedup when that phase is at least 10% of the final
    same-revision one-worker wall time. Phase 0 is the serial-regression
    comparator, not the denominator for parallel speedup. A smaller phase may
    be exempted only with recorded profile evidence.
11. Eight-worker peak RSS is no greater than 2.0x one-worker peak RSS and no
    greater than the smaller of 16 GiB or 25% of detected physical RAM.
12. Harness `--chart-memory-budget` must map exactly to product
    `--chart-spr-memory-budget` and cover resident charts plus admitted exact
    scratch/overlays. Resident plus admitted estimates remain within it. The
    chart process reports peak `VmSwap == 0` and no OOM; host swap before/after
    is diagnostic and becomes a hard failure only on a documented otherwise
    idle host.
13. Automatic mode, run separately, is within 10% of the best safe explicit
    worker count measured under the same affinity and produces identical
    canonical output.
14. The named small automatic workload is no more than 10% slower than its
    explicit one-worker median.

Repeat the primary grammar-exact gate without affinity using explicit automatic
mode (`auto` maps to product worker count zero):

```bash
tools/wric_spr_search_benchmark.sh \
  --dagutil "$PWD/build/bin/dagutil" \
  --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
  --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
  --out-dir "$PWD/build/wric-chart-parallelization/final-medium-auto-unpinned" \
  --run-manifest-group 'p0-primary-smt' \
  --workers-list 'auto' \
  --warmups 1 \
  --repetitions 5 \
  --require-wall-ratio chart_spr_grammar_exact@auto=1.0
```

Its grammar-exact median must be no slower than the unpinned frozen-native
median; CPU affinity may not be the only reason parity is claimed. On this
host the frozen unpinned affinity equals `0-15`, so the row is shared with
`p0-primary-smt`. If Phase-0 preparation observes a distinct unpinned affinity,
use its generated `p0-primary-unpinned-auto` group instead.

After explicit auto passes and Phase 10 changes the default, run the omitted
default and explicit-auto rows together so the harness can pair them exactly:

```bash
tools/wric_spr_search_benchmark.sh \
  --dagutil "$PWD/build/bin/dagutil" \
  --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
  --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
  --out-dir "$PWD/build/wric-chart-parallelization/final-medium-default-unpinned" \
  --run-manifest-group 'p0-primary-smt' \
  --workers-list 'default,auto' \
  --warmups 1 \
  --repetitions 5 \
  --require-wall-ratio chart_spr_grammar_exact@default=1.0 \
  --require-worker-policy default=automatic_default
```

The harness must prove that the selected `default` row emitted neither unified
nor legacy worker option, while the paired `auto` row emitted explicit numeric
zero. The product report must label them `automatic_default` and `automatic`,
respectively; their canonical output and resolved workers must match exactly,
and default must be no more than 10% slower than explicit auto per trial and by
median. Faster default execution is acceptable. Rerun full CTest after this
command. If unpinned affinity differed at Phase 0, use the generated group that
contains its paired default/auto rows.

### Non-vacuous local-commit gate

The primary parity command intentionally preserves the existing conservative
benchmark mode. Exercise Phase 9 separately using the committed deterministic
three-accept fixture required by that phase:

```bash
taskset -c 0,2,4,6,8,10,12,14 \
  tools/wric_spr_search_benchmark.sh \
    --dagutil "$PWD/build/bin/dagutil" \
    --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
    --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
    --supplemental-workload-manifest "$PWD/build/wric-chart-parallelization/supplements/phase9-local-commit.tsv" \
    --out-dir "$PWD/build/wric-chart-parallelization/final-local-commit" \
    --run-manifest-group 'phase9-local-commit' \
    --workers-list '1,8' \
    --warmups 1 \
    --repetitions 3
```

Every measured chart trial must report at least three actual accepted moves,
the local-commit counter contract, byte-identical one-vs-eight semantics, and
the Phase-9 accepted-update speed/RSS gates. A fixture that merely attempts or
rejects three moves is vacuous and does not pass.

### Seedtree 128/16 stress comparison

```bash
taskset -c 0,2,4,6,8,10,12,14 \
  tools/wric_spr_search_benchmark.sh \
    --dagutil "$PWD/build/bin/dagutil" \
    --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
    --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
    --out-dir "$PWD/build/wric-chart-parallelization/final-stress-seedtree" \
    --run-manifest-group 'p0-stress-physical' \
    --workers-list '1,8' \
    --warmups 1 \
    --repetitions 3
```

This 128/16 run confirms multi-iteration seedtree and memory stress; it does
not replace the frozen 32/4 parity gate. Require semantic correctness, bounded
memory, and no timeout. For rows with at least 100 ms of measured
one-worker chart work, require median W8/W1 at most 1.00; for smaller rows,
require at most 1.10. Report the native wall ratio without claiming parity
unless it is at most 1.00. Missing output, reduced work, or fallback is not
acceptance.

### Bounded real-scale confirmation

```bash
taskset -c 0,2,4,6,8,10,12,14 \
  tools/wric_spr_search_benchmark.sh \
    --dagutil "$PWD/build/bin/dagutil" \
    --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
    --workload-manifest "$PWD/build/wric-chart-parallelization/baseline-408434e/workloads.tsv" \
    --out-dir "$PWD/build/wric-chart-parallelization/final-real-bounded" \
    --run-manifest-group 'real-bounded' \
    --workers-list '1,8' \
    --warmups 1 \
    --repetitions 3
```

Before including a non-primary real row in the exact 32/4 gate, Phase 0 runs a
frozen exact-one preflight with a 600-second timeout and 6 GiB RSS cap. A row
that passes must complete 32/4 with the same semantic and scaling thresholds as
the stress run. The immutable `real-bounded` manifest group contains each
fixture/mode/input and its own full 32/4 or largest-feasible-prefix budget; the
command supplies no conflicting global candidate/exact override. A
pre-existing high-arity refusal may be labelled
`expected_infeasible` only through its exact frozen W1/W8 whitelist tuple,
including exact input/refseq, affinity, work/resource contract, canonical argv,
full stderr, exit-one/no-signal/no-core/no-timeout/no-RSS/no-monitor state, and
no output. A 20D row
whose exact-one preflight exceeds the resource cap is labelled `scale_limit`
and must run the largest frozen exact prefix that fits plus full lower-bound,
structural, and output validation; it is reported but does not extend the
seedtree parity objective. `scale_limit` is forbidden for seedtree and cannot
hide a crash, changed refusal, semantic mismatch, missing bounded row, or a
regression relative to its frozen feasible prefix.

For the frozen 20D fixture, Phase-0 characterization reached the exact allowed
high-arity refusal before scheduling or report emission. The sealed
`real-bounded` group therefore contains the contracted explicit-W1 preflight
and an explicit-W8 confirmation of the same pre-scheduler refusal. Both rows
must independently satisfy the exact exit/reason/no-output contract above;
all report-derived, semantic-output, and not-reached counter fields are literal
`-`. These two cheap confirmations replace the conditional success or
resource-prefix branches for this fixture; they do not create a timing or
scaling claim.

## Stop, rollback, and escalation rules

Stop the current phase immediately and fix it before continuing if any worker
count changes an exact score, exactness label, keep mask, tied provenance,
candidate order/signature, accepted move, chain identity, or canonical output.

Remove, redesign, or keep opt-in any optimization that:

- misses its individual action speed target after two profile-guided revisions;
- regresses a mandatory reference workload by more than 5%;
- violates an RSS or memory-budget gate;
- introduces a sanitizer finding;
- requires nested pools, schedule-dependent ties/RNG, or partial publication;
- has less than 10% post-change profile weight and adds substantial complexity.

Do not compensate for a failed phase by weakening a later gate. Record the
failed experiment and profile evidence in the results document. A missed
performance gate is not by itself a blocker: continue with the next in-scope
optimization supported by the current profile. Keeping a failed change opt-in
does not complete the phase or waive its mandatory exit criteria.

If all profile-backed alternatives have been exhausted and final parity still
fails, leave the goal incomplete. Produce a blocking report containing:

- an Amdahl breakdown from measured phase times;
- current flame/call profiles and allocation counts;
- worker utilization, user/system time, memory bandwidth/cache evidence where
  available, and peak RSS;
- the remaining serial critical path;
- the smallest additional exact algorithm or representation change likely to
  close the gap.

Request user direction rather than changing exactness or benchmark work only
after the same measured algorithmic/external impasse has recurred and no
remaining in-scope profile-supported work can make meaningful progress.

## Completion checklist

The `/goal` is complete only when all boxes can truthfully be checked:

Immutable product Q's sealed 174-pass/two-skip normal and ASAN inventories and
55/55 targeted TSan result remain valid historical evidence. They do not
satisfy the completion gates below after the optimized product changes.

- [ ] Every Phase 0--10 exit criterion passes. Only actions explicitly labelled
      conditional may be omitted under their stated profile threshold; omitting
      such an action never waives the phase's exit criteria.
- [ ] The immutable optimized candidate is committed, clean, locked, and
      recorded with its revision, tree, executable hashes, and exact
      implementation/test scope.
- [ ] The immutable optimized candidate's canonical semantics match at every
      worker count assigned to each case by the required matrix; scheduler
      cases additionally cover `0/auto` and 16.
- [ ] The immutable optimized candidate's full RelWithDebInfo build and
      176-test CTest inventory pass with exactly the two established skips.
- [ ] The immutable optimized candidate's full serial ASAN/LSan suite passes
      with the exact inventory and a clean diagnostic scan.
- [ ] The immutable optimized candidate's exact 55-test targeted TSan suite
      passes under the pinned patched runtime with a clean strict scan.
- [ ] A fresh candidate-bound 20-component campaign completes all expected
      rows, audits, ledgers, and hashes without reusing Q/C4 result slots.
- [ ] A new no-clobber strict pre-default evaluator result proves every
      explicit-auto correctness, timing, RSS, policy, stress, and real-scale
      gate passes.
- [ ] Primary medium grammar-exact wall parity passes exactly as specified.
- [ ] Eight-worker grammar exact is at least 2.0x faster than one worker.
- [ ] RSS, chart-memory, and concurrent-verifier bounds pass.
- [ ] Unpinned explicit-auto mode reaches native parity before any default
      change.
- [ ] Only after the strict pre-default pass, a descendant commit promotes
      omitted workers to automatic while retaining explicit one-worker
      compatibility.
- [ ] Omitted/default automatic mode matches explicit auto and reaches native
      parity in a fresh post-promotion capture and final evaluator result.
- [ ] The post-default descendant repeats and passes the required full normal,
      ASAN/LSan, and targeted TSan closures.
- [ ] Small workloads do not suffer material parallel overhead.
- [ ] Seedtree 128/16 stress completes; bounded real-scale confirmation either
      completes or records only the exact allowed `expected_infeasible` or
      resource-preflight `scale_limit` contract above.
- [ ] Documentation and final raw-artifact paths reproduce every claim.
- [ ] No native baseline algorithm, search budget, validation, or exactness was
      weakened.

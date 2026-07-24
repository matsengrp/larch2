# WRIC chart-SPR search benchmark

The Phase-0 harness compares the frozen sample--explore--merge optimizer with
the working DAG-native chart-SPR search path. A committed CI-scale result table
lives in [`WRIC-SPR-SEARCH-BENCHMARK-RESULTS.md`](WRIC-SPR-SEARCH-BENCHMARK-RESULTS.md).
For API/cost-model details, see [`WRIC-SPR-SEARCH.md`](WRIC-SPR-SEARCH.md).

## Smoke run

After building `larch2` and `dagutil`:

```bash
tools/wric_spr_search_benchmark.sh --smoke
```

The smoke run uses `data/test_5_trees/tree_0.pb.gz`, one larch2 native iteration, and one lower-bound chart-SPR candidate. It exits nonzero if any required smoke row is missing, has `status != ok`, or has `validation_status != ok`. It writes:

- `summary.md` - compact benchmark table;
- `summary.tsv` - machine-readable metrics;
- `paired_ratios.tsv` - exact raw-trial joins and ratios for requested wall
  gates (header-only when no wall gate is requested);
- `raw_trials.tsv` - one row per measured trial, including requested/resolved
  workers, execution order, wait4 CPU time, Linux KiB maximum RSS, sampled
  RSS/swap, timeout outcome, exact-phase timing, input hashes, and the compact
  canonical semantic digest;
- `phase6_admission_evidence.tsv` - in manifest mode, one chart row per raw
  trial with its top-K/worker identity, configured budget, sampled RSS, and the
  seven exact-candidate admission fields used by the Phase-6 bounded-memory
  gate (otherwise header-only);
- `phase6_rss_comparisons.tsv` - complete comparable W1/W8 exact-row pairs,
  their maximum sampled RSS values, and the W8/W1 ratio used by the Phase-6
  RSS gate (header-only when no such pair is selected);
- `phase6_exact_verification_speedup.tsv` - hash-bound fixed-point W1/W8
  Top-K-4 exact-verification samples, exact median arithmetic, and the strict
  Phase-6 2x speedup comparison (header-only unless the named medium workload
  is selected);
- `commands.sh` - exact commands used;
- `logs/` - per-method stdout/stderr reports;
- `curves/` - score-over-time TSV files (`iteration`, `elapsed_s`, `reported_objective`, `external_validated_parsimony_min`); elapsed time is method wall-clock apportioned across emitted iteration reports when the underlying tool does not emit per-iteration timestamps. For sample--explore--merge, larch2's iteration summary reports the sampled-tree objective rather than the merged output DAG minimum, so the baseline curve endpoint uses the externally validated final DAG parsimony when validation succeeds;
- `outputs/` - final DAGs for validation/scoring.

## Full comparison command

```bash
tools/wric_spr_search_benchmark.sh \
  --iterations 3 \
  --seed 1 \
  --max-moves 50 \
  --max-candidates 128 \
  --top-k-exact 16 \
  --include-data-fixtures \
  --include-heuristic
```

## Repeated worker matrices

Use one unified chart budget for new benchmark work. `auto` is forwarded as
numeric zero:

```bash
tools/wric_spr_search_benchmark.sh \
  --dagutil "$PWD/build/bin/dagutil" \
  --larch2 "$PWD/build/wric-chart-parallelization/baseline-408434e/bin/larch2" \
  --process-metrics "$PWD/build/bin/wric-process-metrics" \
  --tree "$PWD/data/seedtree/seedtree.pb.gz:$PWD/data/seedtree/refseq.txt.gz" \
  --workers-list 1,2,4,8 \
  --warmups 1 \
  --repetitions 5 \
  --timeout-seconds 600 \
  --chart-memory-budget 12884901888
```

The native and chart blocks alternate order between paired trials. The
summary reports medians and maxima; raw trial rows remain the authoritative
evidence. `--chart-workers`, `--workers-list`, and compatibility
`--local-workers` are mutually exclusive. If none is provided, no product
worker option is emitted. `--local-workers` forwards the legacy
`--chart-spr-local-score-workers`; the other two controls forward the unified
`--chart-spr-workers` option.

`--chart-lazy-policy off|on|auto` freezes and forwards
`--wric-lazy-chart`. Automatic selection is a chart-SPR search/state policy;
standalone composite, chart-B&B, benchmark, and fluidity consumers require an
explicit `off` or `on`. Reports and manifests preserve the requested `auto`
value, while `lazy_policy_resolved` and `cache_strategy` record the one
deterministically frozen representation branch.
Manifest-group execution obtains the fixture, work budgets, timeout, seed,
cache/batch policy, polytomy caps, lazy mode, verification mode, and commit
mode from every selected row. Conflicting global work/search options are
rejected. A worker list is only a closed selector for worker rows; every
selected row must execute exactly once per recorded repetition.

Timed chart runs are capture-off. All timed native/chart children finish
before any generated-output scoring or semantic work. The harness then
validates every measured output with the frozen Phase-0
`dagutil --canonical-dag-result` and runs one unaggregated compact semantic
companion per row/worker contract. The companion must reproduce the measured
output digest. `--full-canonical-correctness` additionally queues an explicit
`--chart-spr-workers 1` full-NDJSON companion; its byte SHA-256 must equal both
compact semantic digests. Thus an omitted timed worker remains genuinely
omitted, while the full correctness oracle is always explicit W1.

Every process, including validation, is run through
`wric-process-metrics`. Schema v2 passes the selected manifest
`rss_limit_bytes` explicitly to the initial scorer, timed child, deferred
validator, compact companion, companion validator, and optional full
companion. The Linux wrapper is a child subreaper: it walks only the command
descendant tree, samples aggregate RSS/swap every 10 ms, and reaps through
`ECHILD`. It establishes a waitable `SIGCHLD` disposition in the wrapper while
restoring the inherited disposition in the command child. Already-dead adopted
zombie chains are synchronously reaped and are not leaks; a non-zombie process
observed after leader collection is killed, joined, and reported as
`descendant_leak`/125. Identity-checked cleanup follows `setsid` descendants
outside the original process group. Unreadable live process records fail closed
as `monitor_error`/125, and wait/lifecycle-invariant failures take priority as
`wait_error`/125. Every outcome converges on real-procfs cleanup and proves an
empty tree plus `ECHILD` before return.

`max_rss_kb` remains the wait4 diagnostic. `peak_sampled_rss_kb` is the
aggregate value used by manifest and RSS gates; both use KiB (1024-byte
units). RSS enforcement is reactive: the first sampled aggregate above the
byte cap gets immediate SIGKILL with no TERM grace, so allocation can overshoot
by up to one sampling interval. Timeouts retain a 250-ms TERM grace and status
124. When timeout and RSS overlap, timeout has precedence while
`rss_limit_observed` and trigger bytes still record the crossing. RSS
termination is status 123. These outcomes are explicit raw rows, never missing
rows.

## Phase-6 exact-candidate evidence

Every successful chart raw row carries these seven admission fields. Manifest
runs also project them into `phase6_admission_evidence.tsv`:

- `exact_candidate_admission_batches`: completed deterministic admission
  waves;
- `exact_candidate_parallel_batches`: waves that ran two or more candidates
  on the outer exact-candidate scheduler axis;
- `exact_candidate_inner_parallel_batches`: singleton waves that used the
  Phase-5 inner exact axis;
- `exact_candidate_memory_limited_batches`: waves shortened by the finite
  budget, including a singleton forced to remain serial because its inner
  estimate did not fit;
- `exact_candidate_peak_admitted_bytes`: the largest admitted wave's task
  scratch plus retained-result charge;
- `exact_candidate_peak_projected_resident_bytes`: the largest projection of
  shared resident exact state, prior-wave retained results, and the admitted
  wave; and
- `exact_candidate_queued_for_memory_ms`: completed-wave time charged only
  when later candidates were deferred by memory admission, not ordinary
  scheduler queue time.

The harness requires the six count/byte fields to be unsigned integers and the
queue timer to be a finite nonnegative decimal. For every successful chart
trial it enforces

```text
parallel_batches <= admission_batches
inner_parallel_batches <= admission_batches
memory_limited_batches <= admission_batches
parallel_batches + inner_parallel_batches <= admission_batches
peak_admitted_bytes <= peak_projected_resident_bytes
finite budget => peak_projected_resident_bytes <= configured budget
```

Zero admission batches require all six remaining admission values to be zero;
positive batches require both peak-byte fields to be positive. Any positive
exact-verification count requires a positive admission-batch count, and zero
memory-limited batches require zero memory-queue time. A selected successful
top-K-16 manifest row must produce exactly one evidence row per trial with
positive exact, budget, batch, admitted-byte, and projected-byte values;
missing or duplicate cardinality fails the gate.

`phase6_rss_comparisons.tsv` joins successful exact W1 and W8 rows only when
their comparison identity and sealed workload-contract identity match. It
uses the maximum `peak_sampled_rss_kb` over the repetitions for each worker
count and requires W8/W1 to be at most 2.0. If a selected manifest group
contains both W1 and W8 exact contracts, every expected comparison must be a
complete one-to-one manifest pair and must appear in the output; missing,
duplicate, contract-drifted, or vacuous comparisons fail closed.
The pair must also have identical `search_semantic_sha256` and
`output_semantic_sha256`; both hashes are retained in the RSS evidence row.

For the named `exact-medium-topk4` grammar-exact workload,
`phase6_exact_verification_speedup.tsv` requires exactly one W1 and one W8
manifest endpoint and every configured recorded trial at each endpoint. It
converts the three-decimal millisecond fields to integer thousandths, computes
the median without floating-point comparison, and requires the W8 median to be
at most half the W1 median. The W8 rows must also report at least one outer
candidate-parallel batch, a concurrent-verifier high-water mark of at least
two, and an exact-candidate scheduler high-water mark of at least two; W1 must
remain serial on that axis. Each arithmetic row records the workload and
contract hashes, the identical W1/W8 search and output semantic hashes, and its
own SHA-256 digest. Missing or duplicate trials, timeout/fallback outcomes,
inconsistent exact timing counts, nonfinite values, inactive parallel
execution, semantic drift, or contract drift fail the gate.

## Strict gates and immutable manifests

`--require-wall-ratio METHOD@WORKERS=RATIO` performs an exact raw join on
fixture and trial index, emits every ratio to `paired_ratios.tsv`, and compares
the chart/native medians. Duplicate, missing, failed, differently ordered, or
unvalidated members fail the join. For a parity ratio at most 1.00, at least
`ceil(N/2)` chart trials must individually beat native and every individual
ratio must be at most 1.15. `--require-max-rss-kb` applies an additional
aggregate RSS cap. Either gate requires `--workload-manifest`.
Every non-waived child failure, validation failure, workload mismatch,
canonical mismatch, missing gate row, and RSS violation makes the harness
exit nonzero.

Manifests use the exact wide Phase-0 TSV schema defined in
`tools/wric_spr_search_benchmark.sh`. Every field is present and nonempty;
literal `-` is the only inapplicable sentinel and is matched exactly, never as
a wildcard. Booleans are lowercase `true` or `false`. Closed values include:

- `binary_role`: `frozen_native` or `working_chart`;
- `input_kind`: `dag_pb` or `tree_pb_refseq`;
- `worker_option`: `none`, `chart_spr_workers`, or
  `chart_spr_local_score_workers`;
- native `requested_workers` and `expected_resolved_workers`: `-`;
- an omitted chart worker uses `requested_workers=default`; explicit workers
  are base-10 integers and auto is requested zero;
- explicit workers have an exact positive resolved count. Auto/default may use
  the closed `policy` sentinel, but the observed count must still be positive;
- `expected_worker_policy` is `-` for native, `explicit` or `automatic` for
  the unified option, and `legacy_explicit` or `legacy_automatic` for its
  compatibility alias. Only an omitted/default row uses `policy`: its runtime
  report must be one of the closed transition labels historical
  `default_serial` or promoted `automatic_default`. Manifest numeric zero is
  reported in raw/summary output as the stable user-facing worker label `auto`.

The Phase-10 transition is a separate strict gate:

```text
--require-worker-policy default=automatic_default
```

It requires exactly one selected explicit-auto companion for every selected
default row and vice versa, with the same sealed workload contract. Every raw
trial must have successful validation, default policy `automatic_default`,
explicit-auto policy `automatic`, identical positive resolved worker counts,
and identical search/output semantics. Default wall time must be at most 1.10
times explicit-auto wall time in every trial and for the medians. Missing or
duplicate companions fail before timed execution. Select both policies with
`--workers-list default,auto`; the default command emits no product worker
flag, while the auto command forwards numeric zero.

The required preamble fields are `schema`, `schema_version`, `kind`,
`manifest_id`, `parent_sha256`, `repo_revision`, `merge_base`, frozen native
and semantic-oracle binary URI/SHA pairs, and the frozen commands URI/SHA.
A detached `PATH.sha256` seal must contain exactly:

```text
<64 lowercase hex><two ASCII spaces><basename><newline>
```

The harness verifies the exact sealed bytes and filename. A supplement must
have `kind=supplement`, name the exact base-manifest byte hash as its parent,
retain the same schema and frozen binary hashes, and may not override a row ID
from the base or another supplement. Acceptance output directories are
non-overwriting.

All assets use only `repo://RELATIVE/PATH` or
`manifest://RELATIVE/PATH`. Absolute/bare paths, empty or non-normal paths,
`.`/`..` traversal, and symlink resolution outside the selected root are
rejected; the target must be a regular file. Preamble keys occur exactly once
in order, and schema/version, revision hashes, row width, enums, conditional
sentinels, and content hashes are closed and exact.

A frozen harness copy may be relocated outside the source tree only with an
absolute `WRIC_REPO_ROOT`. The harness canonicalizes and requires that
directory, then resolves defaults and every `repo://` asset against it; a
relative or nonexistent override is rejected.

Canonical argv is path-independent and hashes these exact bytes:

```text
wric-canonical-argv-v1
argc=<N>
<byte-length>:<argument>
...
```

Executable and path arguments use role/content placeholders such as
`@binary:working_chart`, `@primary:<sha256>`, and `@output`. The combined trial
digest is SHA-256 of:

```text
wric-trial-semantic-v1
method=<stable method>
search_semantic_sha256=<compact companion digest or ->
output_semantic_sha256=<frozen external DAG digest>
canonical_argv_sha256=<canonical argv digest>
```

Manifest mode compares all four digests independently.

`--allow-expected-timeout ROW_ID` is record-only and requires that exact row
to declare `expected_outcome=timeout` and the exact expected trial count in
the sealed manifest. Waivers reject warmups, acceptance gates, duplicates,
unselected rows, and a repetition/count mismatch. They cannot waive any other
timeout or failure. Optimized code may later complete a frozen timeout row; it
must then validate and pass same-revision cross-worker semantics because no
timeout-derived oracle exists.

`expected_infeasible` is limited to the two frozen real-20D grammar-exact
preflight tuples: exact input/refseq hashes, physical affinity, W1 or W8,
candidate/top-K one, iteration/seed one, bounded-shape one, 600 seconds, 6 GiB
aggregate sampled RSS, exact canonical argv digest, and full-stderr SHA
`84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35`.
Runtime acceptance requires ordinary child exit exactly 1, no
signal/core/timeout/RSS/monitor state, exact stderr, and no output. Unreached
report/result/semantic asset fields are literal `-`; only the separately
reached initial score remains concrete. The same validator applies to base
manifests and supplements. `scale_limit` is a successful
bounded grammar-exact prefix with an exact timeout/RSS resource and largest
candidate/top-K tuple. It is forbidden for seedtree and requires a successful
full lower-bound companion for the same selected fixture/worker. Neither is a
general failure waiver.

This is a benchmark command, not a CI command. Exact modes on medium/real fixtures can be much slower in ASAN builds, and high-arity real fixtures require a binary chart-compatible refinement.

Default chart-SPR modes are:

1. `sampled_tree_fixed` - sampled-tree projected candidates with `fixed_topology_exact` acceptance using the sampled tree's explicit before/after topology certificate;
2. `grammar_exact` - grammar-native candidates with `exact_multisite` acceptance;
3. `hybrid_exact` - sampled-tree plus grammar-native candidates with `exact_multisite` acceptance.

`--include-heuristic` also adds the opt-in `grammar_lower_bound` mode, labelled `composite_lower_bound_heuristic`.

## Extra fixtures

```bash
tools/wric_spr_search_benchmark.sh \
  --dag path/to/input.dag.pb.gz \
  --tree path/to/tree.pb.gz:path/to/refseq.txt.gz
```

or via environment:

```bash
WRIC_SPR_SEARCH_EXTRA_DAG_PBS="a.pb.gz b.pb.gz" \
WRIC_SPR_SEARCH_EXTRA_TREE_PBS="tree.pb.gz:refseq.txt.gz" \
tools/wric_spr_search_benchmark.sh
```

## Reported metrics

The TSV keeps externally validated parsimony separate from method-specific objectives: `initial_validated_parsimony_min`, `final_validated_parsimony_min`, `best_validated_parsimony_min`, and `best_reported_objective`. For sample--explore--merge, the reported objective endpoint is the externally validated output-DAG parsimony, because larch2's native per-iteration summary reports the sampled-tree objective rather than the merged DAG minimum. For `fixed_topology_exact`, the reported objective is the selected sampled/grammar topology score; for `grammar_lower_bound`, it is the composite lower-bound heuristic, not a parsimony claim. The TSV also includes wall-clock time, score-over-time curves, candidate source, acceptance/objective mode, generated/scored/exact-verified candidates, path-expansion and prune counters, reachability traversal counters, reason-coded overlay materializations, full search-state rebuild counts, cache/local/exact/materialization time splits, affected-clade distribution, grammar/pattern/cache size, final DAG size, and validation status.

The `full_search_state_rebuilds` column is reported separately from exact-verification and oracle overlay materializations so rejected candidates can be checked for zero full sidecar rebuilds.

`initial_chart_construction_ms` is the actual initial dense (all-active or
pattern-batched) or lazy inside+outside chart construction span.  Unlike the
broader `cache_build_ms`, it excludes pattern extraction, grammar validation,
cache sizing, and exact-trim initialization.

`materialization_ms` is exactly the sum of three disjoint spans:
`materialization_exact_verification_ms` measures actual dense overlay/chain
materialization performed inside exact verifiers (including the transient
verifier's current chain fold and any enabled verifier oracle),
`materialization_accepted_update_ms` measures conservative accepted-DAG
materialization as the whole `materialize_chart_spr_accepted_candidate` call
(Option A/B tree/DAG materialization, merge, validation, and grammar audit), or
only the local-commit tip's `materialize_overlay_chain` call in local-commit
mode, and
`materialization_final_compaction_ms` measures only final compaction's single
dense chain materialization.  It excludes diagnostic materialization performed
only by the local-scoring oracle, and the final-compaction bucket excludes
witness derivation, DAG merging, grammar rebuilding, and final exact
validation.  When an exact-verification or conservative accepted-update
materializer throws and the search converts that exception into a reportable
invalid candidate or rejected accept, the elapsed call span remains charged to
its reason bucket; the corresponding success-only materialization counter is
not incremented.  The report's `peak_concurrent_exact_verifiers` is an observed
atomic entry/exit high-water mark: it is zero when no verifier executes and one
for the currently serial verifier loop; it is not inferred from the requested
worker count.

## Phase-0 manifest bootstrap

The strict workload manifest cannot be invented before the semantic captures
exist.  `tools/wric_phase0_manifest_bootstrap.py` closes that cycle without an
unsealed acceptance mode and without modifying the harness's validator.  It
first captures ordinary raw output, then derives the exact manifest-mode
canonical argv/trial digests, writes `workloads.pending.tsv`, and deliberately
asks the harness to reject a nonexistent sentinel group.  Reaching that exact
post-validation rejection proves that the harness accepted the manifest seal,
preamble, schema, assets, hashes, and rows without starting a benchmark.

Preparation is write-exclusive and launches no benchmark. It runs from the
persistent Phase-0 restoration worktree: Git HEAD is the frozen research tip
`408434e`, with the exact `7ca527b` instrumentation diff present as an
unstaged overlay. The absolute worktree and baseline paths are part of the
prepared trust root and must not later be moved or replaced by symlinks.

After a quiet-host wrapper calibration succeeds, prepare the matrix with its
calibration and affinity inputs explicitly bound:

```bash
taskset -c 0-15 python3 tools/wric_wrapper_calibration.py run \
  --output "$PWD/build/wric-wrapper-calibration/wrapper-calibration.json"

python3 tools/wric_phase0_manifest_bootstrap.py prepare \
  --baseline-dir "$PWD/build/wric-chart-parallelization/baseline-408434e" \
  --expected-oracle-sha256 7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c \
  --wrapper-calibration \
    "$PWD/build/wric-wrapper-calibration/wrapper-calibration.json" \
  --physical-affinity 0,2,4,6,8,10,12,14 \
  --smt-affinity 0-15 \
  --unpinned-affinity 0-15
```

The verified oracle digest is intentionally mandatory rather than defaulted,
and the helper also hard-binds that exact settled value: the CLI cannot bless a
self-consistent replacement. A stale provisional binary must not silently seed
the matrix.
Preparation also copies the exact benchmark harness and process wrapper into
the baseline `bin/` directory after byte/hash comparison. Metadata, commands,
strict executions, and artifact ledgers reference only those frozen copies;
later Phase-1+ edits to the live repository scripts cannot invalidate or alter
the sealed Phase-0 procedure.

Preparation finishes by writing `bootstrap-phase0/prepared-contract.tsv` and
an exact detached seal before any capture is permitted. That non-circular root
covers canonical metadata, the capture/row/timeout plans, generated commands,
the frozen helper, harness, process wrapper, larch2 and dagutil, the regenerated
20D refusal proof, all five fixture files, and the four pre-existing root
provenance files. Every later command verifies this root before reading
metadata, enforces the fixed repository/revision/baseline and lexical frozen
role paths, rebuilds the matrix from the recorded affinities, byte-compares all
plans and commands, regenerates the refusal proof from the frozen git object,
and rehashes the fixed fixtures. File permissions are defense in depth; hashes
and independent derivations are the acceptance boundary.

On this host the base contains 89 unique rows from 13 capture commands. The
observed unpinned affinity equals the recorded SMT affinity, so the resolver
reuses those rows; a host with a distinct inherited affinity adds a separate
unpinned capture rather than aliasing it. The
matrix includes:

- small and seedtree dense local-64 at physical workers 1, 2, 4, and 8;
- seedtree cache-one, small/seedtree exact-one, and forced-lazy local-64;
- the primary seedtree 32/4 contract for sampled-tree-fixed, grammar-exact,
  and hybrid-exact at physical workers 1, 2, 4, and 8;
- the three-iteration seedtree 128/16 stress contract for all three exact
  sources at physical workers 1, 2, 4, and 8;
- grammar-exact primary rows on all SMT CPUs at 1, 2, 4, 8, 16, and explicit
  auto, plus a true omitted/default-worker row;
- the small 64-candidate explicit-one/auto overhead guard; and
- separate unpinned medium auto/default rows only if the observed unpinned
  affinity differs from the recorded SMT affinity; and
- the real-20D exact-one preflight at explicit workers 1 and 8.  Those two
  rows freeze the independently repeated, pre-existing high-arity refusal;
  they are not timeouts or scale-limit rows.

Successful auto/default rows use `expected_resolved_workers=policy`; explicit auto freezes
`expected_worker_policy=automatic`, while omitted/default freezes the controlled
`expected_worker_policy=policy` transition sentinel. Both still require a
positive observed worker count. A timeout or expected-infeasible refusal has
no completed product report, so both manifest worker-observation fields are
the literal unavailable sentinel `-`; a later successful execution is checked
against its immutable worker invocation instead of an invented historical
resolution. Resolver-equivalent rows are deduplicated: in
particular there is one native row for each unique fixture, affinity,
iterations, move budget, and timeout contract.  The bootstrap self-test and
final audit both reject any duplicate key that would make a non-group
acceptance command ambiguous.

Capture one short group as follows:

```bash
python3 \
  build/wric-chart-parallelization/baseline-408434e/bootstrap-phase0/bootstrap-helper.py \
  capture --capture-id small-dense64-physical
```

Every medium command has a second guard and is non-overwriting:

```bash
python3 \
  build/wric-chart-parallelization/baseline-408434e/bootstrap-phase0/bootstrap-helper.py \
  capture --capture-id medium-primary32k4-physical \
  --confirm-long-medium
```

`commands.phase0.sh` records every exact invocation and refuses to run the full
long matrix unless `WRIC_CONFIRM_LONG_PHASE0_CAPTURES=YES` is present.  A
timed-out capture may make the ordinary harness exit one; the bootstrap accepts
that only when the row was statically timeout-eligible. Eligibility is not an
approval: after inspecting the capture, the operator must separately run
`approve-timeouts` with every and only the row IDs that actually timed out.
The generated command script never supplies those approvals. Finalization
hash-binds the approval to the raw TSV and completion record, independently
proves that every planned row is present and every other row validated, checks
successful cross-worker semantics and explicit-W1 full sidecars, and refuses
an approval for a capture with no timeout. It never converts a crash,
validation error, missing row, semantic mismatch, or companion failure into an
expected timeout.

For a timeout row, the strict schema still requires closed refinement/cache
fields even though the timed process wrote no report. Finalization derives
those three values only from unanimously agreeing successful reports with the
same input bytes, input kind, polytomy contract, lazy/cache/batch/memory
controls, UA policy, and no-VCF policy. Method, candidate budget, worker count,
and affinity are intentionally excluded from that compatibility key; any
disagreement aborts. `timeout-field-evidence.tsv` records the deterministic
source report and hashes for every timeout row. No value is defaulted or
invented.

The 20D preflight is separately guarded and chart-only. It invokes the frozen
oracle directly through the wait4 process wrapper with both the 600-second
timeout and an enforced 6-GiB aggregate process-group RSS cap; the algorithmic
chart memory budget remains the frozen 12 GiB. It first freezes the initial
score/semantic digest, then records the exact W1/W8 argv, stdout, stderr,
metrics, and no-output proof. It launches only with explicit confirmation:

```bash
python3 \
  build/wric-chart-parallelization/baseline-408434e/bootstrap-phase0/bootstrap-helper.py \
  capture --capture-id real20d-exact1-preflight-physical \
  --confirm-real-preflight
```

The observed branch may be admitted only by a second, exclusive action naming
both exact rows and the independently repeated stderr digest:

```bash
python3 \
  build/wric-chart-parallelization/baseline-408434e/bootstrap-phase0/bootstrap-helper.py \
  approve-real-outcome \
  --expected-infeasible p0-real20d-preflight-grammar-exact-w1 \
  --expected-infeasible p0-real20d-preflight-grammar-exact-w8 \
  --expected-reason-sha256 84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35 \
  --confirm-real-outcome
```

Approval requires ordinary exit 1, signal/timeout/core/RSS-limit flags zero,
aggregate peak RSS within 6 GiB, no output DAG, the exact 37-byte stdout
(`b0174b43...`) and 159-byte stderr (`84d2f5de...`) for both W1 and W8, the
refusal suffix, the frozen W1/W8 canonical argv hashes, and a content-hashed
git proof that the refusal predates the Phase-0 revision. Because construction stops
before a chart report exists, all non-reached structural, result, counter,
semantic, sidecar, and report fields are literal `-`; the helper does not
invent structural evidence. A timeout/RSS branch would instead require the
documented feasible-prefix ladder and successful lower-bound companions, but
that conditional branch is not inserted into this observed Phase-0 matrix.

After every planned capture finishes, derive and audit the pending base using
the frozen helper copy:

```bash
python3 \
  build/wric-chart-parallelization/baseline-408434e/bootstrap-phase0/bootstrap-helper.py \
  finalize
python3 \
  build/wric-chart-parallelization/baseline-408434e/bootstrap-phase0/bootstrap-helper.py \
  audit
```

Before writing its artifact ledger, finalization runs two real strict
manifest-mode checks: a paired small native/lower-bound W1 execution and the
two 20D expected-infeasible rows. Both are retained and hash-bound. It then
writes `phase0-artifacts.pending.tsv` plus an exact detached seal, covering the
commands, provenance proofs, binaries, fixtures, plans, approvals, reports,
outputs, smoke runs, and raw TSVs. The ledger intentionally excludes itself
and its detached seal; the detached seal is its non-circular root of trust.
Every pending or final audit also reconstructs all manifest and timeout-field
evidence bytes from the frozen 13-capture/89-row plan and validates every raw
trial, report, approval, and sidecar source; matching ledger hashes alone are
not sufficient. Final promotion is an explicit, separate, byte-preserving
action:

```bash
python3 \
  build/wric-chart-parallelization/baseline-408434e/bootstrap-phase0/bootstrap-helper.py \
  seal --confirm-seal-base
```

Promotion strictly validates and executes against the final `workloads.tsv`
path again, then creates mandatory `phase0-artifacts.tsv` and its detached
seal. The final ledger includes the final workload manifest, its seal, the
pending ledger/seal, both final strict executions, and every other closure
member, while again excluding only itself and its own seal. `audit` requires
and checks the appropriate complete ledger; artifact checking is never
optional. Promotion records an immutable seal transaction and is resumable:
already complete artifacts are accepted only when their bytes validate
exactly, while an interrupted smoke bundle with no admissible completion
record is cleaned and rerun at its reserved path. No valid artifact is
overwritten.

Prefer the manifest groups for final commands so the frozen 600-second timeout
and all search controls come from the row rather than being accidentally
restated.  For example, the primary physical gate selects
`--run-manifest-group p0-primary-physical --workers-list 1,8`; SMT policy uses
`p0-primary-smt`; small auto overhead uses `p0-small-auto`; and stress uses
`p0-stress-physical`.  A true omitted/default medium run uses the same SMT (or
distinct-unpinned) primary group with `--workers-list default`; row selection
uses that literal sentinel but the timed product command emits neither the
unified nor legacy worker option.  The native row remains in the selected
group, so wall-ratio joins are executable rather than merely structurally
resolvable.

Phase-7 lazy fixtures, Phase-8 high-generation fixtures, and Phase-9
three-accept local-commit fixtures are not retroactively inserted into this
base.  Each is frozen through the Phase-0 oracle in its documented append-only
supplement.  Commands using one of those fixtures must pass both
`--workload-manifest workloads.tsv` and its
`--supplemental-workload-manifest`, then select the supplement's named group.

# WRIC chart empirical evidence and measurement scope

## Purpose

This note makes the evidence boundary at repository revision `ad5a746`
explicit. It records two previously session-local results, explains which
chart modes can be compared on the frozen 597-leaf grammar, and specifies the
instrumentation still needed for a move-rediscovery or amortized-throughput
claim. It adds no timing claim.

Run the tracked-data checks with:

```bash
python3 tools/verify_wric_chart_empirical_evidence.py
```

This mode pins the exact bytes and schemas of every evidence table it consumes,
then checks their counts, identities, regret arithmetic, and factorization
relations independently.

On the original collection machine, where the ignored trace and two temporary
DAGs are still present, also run:

```bash
python3 tools/verify_wric_chart_empirical_evidence.py --local-artifacts
```

The latter command hash-checks the local inputs, re-runs `dagutil --parsimony`
on both DAGs, compares both complete histograms with the tracked table, checks
the ignored four-site trace, and validates and re-scores all 12 retained census
minima.

## Durable empirical additions

The four patterns used to explain the 1614 lower bound have a tracked weight
and provenance manifest in
[`WRIC-CHART-1614-PATTERN-PROVENANCE.tsv`](WRIC-CHART-1614-PATTERN-PROVENANCE.tsv).
Patterns 304, 312, 1002, and 1023 each have weight one. The manifest also
separates captured occurrence IDs from byte-distinct provider classes and
records one sufficient production package for each captured provider route;
these packages are not claims of universal necessity across every minimizing
direct parse. Its source is the complete
four-site trace with SHA-256
`0074b0c9d231c8da91e42f42b293f0729918f3844dd56e81dad1280086b52c14`;
the provider aliases are independently normalized in
[`WRIC-CHART-1614-FOUR-SITE-PROVIDERS.tsv`](WRIC-CHART-1614-FOUR-SITE-PROVIDERS.tsv).
The independent 1,110-row pattern score capture has SHA-256
`0485fe11d58f79d0f7a94017d3ce39ca77bcc3337a72654d2c83e6db3c7d5991`;
its weights sum to 29,903, its zero-score rows carry the 28,665 invariant-site
weight, and its four selected rows agree with the manifest.

The chart-fronted additive DAG and corrected native DAG have the same complete
23-bin labelled-history score histogram, not merely the same extrema. The
tracked histogram is
[`WRIC-CHART-RECOMBINATION-SCORE-HISTOGRAM.tsv`](WRIC-CHART-RECOMBINATION-SCORE-HISTOGRAM.tsv).
Each column sums to 45,084 histories, ranges from score 1620 (count 4) to 1642
(count 2), and agrees bin for bin. The source DAGs are pinned by the hashes
already recorded in
[`WRIC-CHART-RECOMBINATION-CASE-STUDY.md`](WRIC-CHART-RECOMBINATION-CASE-STUDY.md):

| DAG | SHA-256 |
|---|---|
| `/tmp/chart-additive-medium-final.pb.gz` | `ae9c0c1b746439e3544eb2e8cef877c404e019d493d173a922717b1772a2b25b` |
| `/tmp/case-study-current-native-n1.pb.gz` | `7b67a125dec0fa930ce92a96220b56655b63f1018c75babd395360a2400820b5` |

Their independently produced `dagutil --parsimony` stdout files are
byte-identical and have SHA-256
`d3958c9a717632d7fc74ccce73c46c69fae1f9656322a15783a6b4cdbd02e26a`.
This strengthens only the corrected chart-fronted-versus-corrected-native
semantic parity result. It is not a comparison with the historical pre-fix
1627 output.

## Mode and artifact taxonomy

| Mode/result | Grammar being scored | Scalar meaning | Production/output meaning |
|---|---|---|---|
| Composite chart score 1614 | Direct, unrefined k-ary grammar | Sum of 1,110 independently minimized, multiplicity-weighted pattern contributions; a lower bound | Score-only; it names no common topology and has no coupled mask |
| Direct k-ary census 1616 | Same direct, unrefined k-ary grammar | Exact minimum over all 575,168 complete parses | Twelve minimum topology classes were materialized and independently validated |
| Coupled B&B, `score-only` dominance | Binary or explicitly refined grammar | Exact coupled scalar optimum for that selected grammar | `keep_mask_kind=score_only_not_exact`; the mask must not be applied and no topology is implied by it |
| Coupled B&B, exact-mask modes | Binary or explicitly refined grammar | Exact coupled scalar optimum for that selected grammar | Exact production union, coupled annotation, or topology materialization depending on the application mode |
| Additive chart-SPR union | Direct arbitrary-arity input/output DAG | Independently scored output-DAG minimum, 1620 in the frozen run | A materialized 45,084-history union; not a trim and not an exhaustive grammar result |
| Corrected native sample--SPR--merge | Compact-genome-labelled history DAG | Independently scored output-DAG minimum, 1620 in the frozen run | The same labelled-history histogram and collapsed semantic representation as the chart-fronted union |

“Score-only” therefore has two unrelated uses that must remain qualified:
the 1614 composite is independent-per-pattern lower-bound scoring, whereas
coupled-B&B score-only dominance still computes an exact coupled scalar but
does not preserve an exact production mask. “Materialized” is likewise not an
exactness label by itself: the additive union is a materialized valid DAG that
retains suboptimal histories, while census minimum materialization is a
validated witness for an exhaustively established optimum.

## Direct census and coupled frontier are not a like-for-like 1614 comparison

The direct 1614/1616 grammar contains 133 non-binary productions and has
maximum arity 10. The current coupled B&B frontier is binary-gated. Running it
on that grammar with `--wric-polytomy-mode allow` fails before frontier
construction with the labelled diagnostic:

```text
multi-site exact setup: WI6 arity gate: the chart supports multifurcations;
this consumer's B&B frontier does not ... use --wric-polytomy-mode
expand-exact or expand-bounded before B&B trim
```

Using either expansion mode changes the grammar and its topology space.
Consequently there is currently no honest “frontier versus census on the same
direct 575,168-parse grammar” wall-clock or work-count ratio. A binary fixture
can exercise both implementations, but it cannot establish the price of
exactness for the k-ary 1614 case. Likewise,
`wric_bnb_trim_benchmark.sh`'s `current_trim` versus
`chart_bnb_score_only` rows are not per-pattern-lower-bound versus coupled
exact scoring and must not be presented as that comparison.

What is known without incompatible timing is:

- relaxing common-topology coupling improves 1616 to the unattainable bound
  1614, so the objective price of exact coupling is two steps on this grammar;
- exhaustive direct scoring covers 575,168 parses, of which 12 attain 1616;
- the full direct score histogram and every optimum witness reconcile at
  575,168; and
- the coupled frontier's time, memory, and frontier counts apply only to the
  selected binary/refined grammar named by its report.

These are structural and correctness observations, not a runtime multiple.

## Move rediscovery is still unmeasured

No checked-in native-search report records a label-free canonical SPR key for
each score evaluation. The chart counter `candidates_pruned_duplicate` counts
duplicates within its own candidate generator, but it neither identifies
distinct ancestral labelings nor supplies a native denominator. It therefore
cannot support the claim that the native loop independently rediscovers a move
once per labelling.

The benchmark harness does apply `--iterations` to both methods. It does not
apply one common move-budget concept: `--iterations` is shared,
`--max-moves` is native-only and becomes larch2's retained moves per radius,
and `--max-candidates` plus `--top-k-exact` are chart-only post-dedup and exact
verification caps. The existing one-move tables set these unlike numerical
values to one, but those values cap different stages and do not make the work
universes symmetric. They are not equal-work comparisons.

A publication-usable rediscovery measurement needs one event per actual move
score evaluation from both paths with at least:

- run, iteration, radius, method, and evaluation ordinal;
- sampled topology digest and ancestral-labelling digest;
- a label-free canonical move key made from descendant-taxon sets for the
  moved clade, old context, and destination context;
- predicted score/delta and whether the evaluation survived each cap; and
- the output score and accepted/merged status, kept separate from evaluation.

For each method, group events by canonical move key and report total
evaluations, distinct keys, distinct labellings per key, keys observed under
more than one labelling, and `sum(max(0, evaluations_per_key - 1))`. A direct
chart/native comparison should additionally report only the intersection of
canonical keys reached by both searches. Without those events, neither a
single rediscovery ratio nor a “one chart evaluation” denominator is known.

## Amortized benchmark still needed

The chart reports already separate initial chart construction,
candidate-generation/local-scoring, exact verification, materialization, and
final compaction. They also count locally scored and exactly verified
candidates and already report `local_candidates_per_second` for chart rows.
Native reports do not expose an equivalent structural-candidate or
rediscovery count and timing span for actual move-score evaluations.
Therefore the existing chart throughput field is not currently a symmetric
cross-method metric.

A future amortization study should sweep enough work to show the construction
cost being spread over many evaluations, but should freeze and report each
method's distinct cap semantics rather than equate `max-moves` with
`max-candidates`. At minimum it should report:

1. initial construction/setup time separately from steady candidate work;
2. total and distinct canonical move keys actually score-evaluated;
3. local-score and exact-verification counts and time spans separately;
4. accepted moves/compound batches and independently validated output score;
5. worker, memory, iteration, radius, sampling, and cap policies; and
6. raw repeated trials from the benchmark build.

Until that instrumentation exists, the committed Phase-8 tables remain
CI-scale harness demonstrations and cannot substantiate an amortized advantage.

## Reproduction details

Reproduce the full labelled-history histogram for either retained DAG with:

```bash
build/bin/dagutil \
  --dag-pb DAG.pb.gz \
  --force-no-vcf --validate --parsimony
```

Reproduce the direct-frontier arity gate with:

```bash
build/bin/dagutil \
  --dag-pb /tmp/chart-additive-medium-final.pb.gz \
  --refseq data/seedtree/refseq.txt.gz \
  --force-no-vcf --validate \
  --wric-polytomy-mode allow \
  --chart-score-ua-edge --chart-bnb-trim
```

The supported direct census command remains in
[`WRIC-CHART-1614-EXACT-CENSUS.md`](WRIC-CHART-1614-EXACT-CENSUS.md).

## Claims rejected by the current evidence

- The 1614 composite is not a topology score or a materialized result.
- Coupled-B&B score-only mode is not the independent per-pattern chart sum.
- A binary/refined coupled-frontier run is not a same-grammar comparison with
  the direct k-ary census.
- Setting two unlike budget flags to one does not create equal search work.
- The current counters do not measure move rediscovery across ancestral
  labelings.
- The CI/ASan Phase-8 rows are not publication-grade amortized timings.
- Corrected chart/native histogram parity does not show an advantage over the
  corrected native merger; it shows the same output semantics on this run.

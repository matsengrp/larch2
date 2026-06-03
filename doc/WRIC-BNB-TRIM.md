# WRIC chart-B&B trim

This note documents the production WRIC multi-site chart branch-and-bound (B&B)
trim path.  It is meant to make the exactness labels in `dagutil`, `larch2`, and
benchmark TSVs mechanical rather than interpretive.

## Scores and lower bounds

The chart code reports two different quantities that must not be conflated:

- **`composite_lower_bound`** (`composite_lower_bound_kind: LOWER_BOUND`) is the
  sum of independent per-pattern chart optima.  It is useful for pruning and
  speed diagnostics, but it is not a coupled multi-site topology score and not an
  optimization-quality column.
- **`optimum` / `bnb_optimum` / `exact_bnb_optimum`** is the coupled multi-site
  B&B optimum over one shared topology.  This is the exact trim objective for
  the selected refined grammar and `score_ua_edge` convention.

Benchmark tables therefore keep lower-bound diagnostics in explicitly named
`*_lower_bound*_heuristic` columns and keep exact B&B/output-DAG parsimony in
separate columns.

## Dominance modes

`multisite_dominance_mode` labels how frontier dominance was used:

- `off`: no dominance pruning.  This is the simplest exact score and exact-mask
  path.
- `score-only`: componentwise dominance is used only to compute the scalar
  optimum.  The resulting `keep_production` mask is not exact.
- `strict-mask-safe`: discards an entry only when it is strictly worse on every
  outside-completable component, so it cannot appear in a global optimum.  This
  remains exact for masks.
- `two-pass-exact-mask`: first computes the scalar optimum with score-only
  dominance, then reruns an exact mask recovery pass under that optimum.
- `provenance-preserving`: reserved for a future compact mode that preserves the
  correlations needed for exact masks while pruning.

Why the distinction matters: if frontier entry `A` dominates `B`, then `B` is
unneeded for the scalar optimum.  However, `B` can still tie `A` under a
particular outside context; blindly discarding `B` can under-keep productions in
an optimal topology, while merging `B`'s provenance into `A` can over-keep
suboptimal productions.

## Exact mask recovery

A B&B result may expose a production mask with:

- `keep_mask_kind: exact_optimal_production_union` and
  `keep_production_exact: true`: production `p` is kept iff some globally
  optimal topology uses `p`.
- `keep_mask_kind: score_only_not_exact` and `keep_production_exact: false`: the
  scalar optimum is exact, but the mask must not be applied or reported as an
  exact production union.

Any consumer that applies or summarizes `keep_production` must first assert or
report `keep_production_exact`.  `dagutil --chart-bnb-score-only` is diagnostic
only and cannot be combined with `--chart-bnb-apply-trim`.

## Output exactness labels

The output artifact has its own exactness label, separate from the scalar score
and exact production mask:

- **`production_mask_superset`**: applies the exact optimal-production union to
  source DAG witnesses.  This preserves every optimal topology and removes
  productions not in the exact mask, but an ordinary recombining DAG can still
  contain non-optimal recombinations of individually optimal productions.
  Consequently `topology_exact` is false.
- **`annotated_optimal_trim` / `coupled_frontier_exact`**: emits a packed coupled
  frontier/provenance annotation instead of an ordinary DAG.  The annotation
  preserves B&B coupling constraints and is exact for the represented frontier.
- **`grammar_topology_exact`**: materializes all optimal grammar topologies in an
  identity-preserving tree-set DAG.  This is exact for collapsed/refined grammar
  topologies, not necessarily every source-history witness variant.
- **`source_history_topology_exact`**: stronger future label for preserving every
  source-history witness alternative with the required correlations.

A topology-exact ordinary DAG must be identity-preserving, or validated on a
small fixture by enumeration/re-scoring to prove that merging did not introduce
recombined extra topologies.

## Validation reporting

B&B output reports both the exact B&B objective and an output-DAG validation
summary:

- `bnb_optimum`: exact B&B objective for the selected grammar.
- `validated_output_parsimony_min`: minimum parsimony observed/validated on the
  output artifact.
- `validated_output_parsimony_min_exact`: whether that minimum is exact for the
  validation scope.
- `validation_oracle` and `validation_strength`: how independent and complete
  the validation was.
- `output_contains_only_optimal_topologies`: `true`, `false`, or `unknown` for
  the output topology set.

Small fixtures use brute-force topology enumeration plus Fitch re-scoring when
possible.  Larger DAGs may use rebuilt-grammar B&B or structural validation; the
weaker oracle is reported explicitly.

## Deferred scaling work

Lazy chart construction and effective-site reduction are not required for the
current output-producing B&B trim path.  They are future scaling levers that
reduce chart/pattern work before or around frontier construction; they are
orthogonal to dominance pruning.

## CLI examples

Score-only exact objective with a non-exact mask:

```bash
dagutil --dag-pb data/test_5_trees/tree_0.pb.gz \
  --force-no-vcf \
  --wric-polytomy-mode expand-bounded --wric-polytomy-max-shapes 1 \
  --chart-bnb-trim \
  --chart-bnb-dominance score-only --chart-bnb-score-only
```

Production-mask DAG output for a binary/witness-safe input:

```bash
dagutil --fasta test/wric_binary_four.fa \
  --newick test/wric_binary_four.nwk \
  --refseq test/wric_binary_four.ref \
  --force-no-vcf \
  --wric-polytomy-mode reject \
  --chart-bnb-trim --chart-bnb-apply-trim \
  --chart-bnb-trim-application production-mask \
  -o build/wric_binary_four.production-mask.pb.gz
```

Grammar-topology-exact materialization:

```bash
dagutil --dag-pb data/test_5_trees/tree_0.pb.gz \
  --force-no-vcf \
  --wric-polytomy-mode expand-bounded --wric-polytomy-max-shapes 1 \
  --chart-bnb-trim --chart-bnb-apply-trim \
  --chart-bnb-trim-application optimal-topology-materialize \
  --chart-bnb-max-exact-topologies 100 \
  -o build/chart-bnb.optimal-topologies.pb.gz
```

Coupled frontier annotation instead of a DAG:

```bash
dagutil --dag-pb data/test_5_trees/tree_0.pb.gz \
  --force-no-vcf \
  --wric-polytomy-mode expand-bounded --wric-polytomy-max-shapes 1 \
  --chart-bnb-trim --chart-bnb-apply-trim \
  --chart-bnb-trim-application annotated-optimal-trim \
  --chart-bnb-report-json build/chart-bnb.annotation.json
```

`larch2` can use the same output path through chart-B&B trim mode:

```bash
larch2 --dag-pb data/test_5_trees/tree_0.pb.gz \
  --trim-mode chart-bnb \
  --wric-polytomy-mode expand-bounded --wric-polytomy-max-shapes 1 \
  --chart-bnb-trim-application optimal-topology-materialize \
  -o build/larch2.chart-bnb.pb.gz
```

## Benchmark commands

B&B trim smoke benchmark:

```bash
tools/wric_bnb_trim_benchmark.sh --smoke
```

Longer B&B trim comparison:

```bash
tools/wric_bnb_trim_benchmark.sh \
  --include-data-fixtures \
  --modes "current_trim chart_bnb_score_only chart_bnb_mask chart_bnb_optimal_topology_materialize" \
  --score-only-dominance score-only \
  --mask-dominance two-pass-exact-mask \
  --materialize-dominance off
```

Chart-SPR benchmark lower-bound modes remain heuristic and are labelled as such:

```bash
tools/wric_spr_search_benchmark.sh --include-heuristic
```

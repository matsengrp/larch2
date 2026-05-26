# WRIC chart-SPR search benchmark

Phase 8 adds a reproducible harness for comparing the current sample--explore--merge optimizer with the DAG-native chart-SPR search path.  A committed CI-scale result table lives in [`WRIC-SPR-SEARCH-BENCHMARK-RESULTS.md`](WRIC-SPR-SEARCH-BENCHMARK-RESULTS.md).  For API/cost-model details, see [`WRIC-SPR-SEARCH.md`](WRIC-SPR-SEARCH.md).

## Smoke run

After building `larch2` and `dagutil`:

```bash
tools/wric_spr_search_benchmark.sh --smoke
```

The smoke run uses `data/test_5_trees/tree_0.pb.gz`, one larch2 native iteration, and one lower-bound chart-SPR candidate. It exits nonzero if any required smoke row is missing, has `status != ok`, or has `validation_status != ok`. It writes:

- `summary.md` - compact benchmark table;
- `summary.tsv` - machine-readable metrics;
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

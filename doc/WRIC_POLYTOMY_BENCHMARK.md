# WRIC polytomy benchmark notes

Adds a reproducible Phase-9 benchmark entry point for WRIC grammar/chart
metrics and soft-polytomy binary refinement expansion:

```bash
cmake --build build/wric-asan --target dagutil
./tools/wric_polytomy_benchmark.sh
```

Set `DAGUTIL=/path/to/dagutil` to use a different build.  The script runs the
real `data/test_5_trees/tree_0.pb.gz` fixture, any optional local DAG fixtures
listed in `WRIC_POLYTOMY_EXTRA_DAG_PBS`, optional tree protobuf fixtures listed
as `tree.pb.gz:refseq` pairs in `WRIC_POLYTOMY_EXTRA_TREE_PBS`, the committed
two-polytomy FASTA/Newick fixture, and generated synthetic stars (`4 6 8` by
default; set `WRIC_POLYTOMY_SYNTHETIC_ARITIES`).

Use `--wric-benchmark` for the general Phase-9 CLI entry point; it is a
polytomy-aware alias of the older `--wric-polytomy-benchmark` flag.  To include
larger committed tree-protobuf fixtures from `data/seedtree` and one
representative `data/20D_from_fasta` tree, run for example:

```bash
WRIC_POLYTOMY_INCLUDE_DATA_FIXTURES=1 \
WRIC_POLYTOMY_BENCHMARK_SHAPE_CAPS=1 \
./tools/wric_polytomy_benchmark.sh
```

Keeping the shape cap at `1` is recommended for routine large-fixture smoke
runs; use `1,4,16` for more thorough bounded-refinement timing.

## Representative local results

Timings vary by machine/build; the stable values to compare are arities, grammar
sizes, exact/truncated status, and counts.

### `data/test_5_trees/tree_0.pb.gz`

Default caps (`max_exact_arity=6`, per-polytomy clades/prods `256/1024`) report:

- source k-ary productions: `14`
- arity histogram: `3:9, 4:1, 6:2, 8:1, 9:1`
- theoretical exact upper bounds: `896` synthetic clades, `13036` binary productions
- full soft-refinement count estimate: saturated `uint64_t`
- exact benchmark: attempted, then refused quickly by the arity cap
- bounded caps `1,4,16`: deterministic, labelled truncated when caps omit seed shapes

Opting in to the observed maximum arity succeeds on this fixture:

```bash
./build/wric-asan/bin/dagutil \
  --dag-pb data/test_5_trees/tree_0.pb.gz --force-no-vcf \
  --wric-polytomy-benchmark --wric-polytomy-benchmark-shape-caps 1 \
  --wric-polytomy-max-exact-arity 9 \
  --wric-polytomy-max-clades 600 \
  --wric-polytomy-max-productions 10000
```

Representative ASan run: exact expansion succeeded in about `125 ms`, producing
`1003` refined clades and `13059` refined productions (`896` synthetic clades,
`13036` synthetic productions).  Single-site charting without trace was about
`29 ms`; with trace was about `68 ms`.

### Synthetic stars

The compact exact closure grows as follows before any reuse:

| arity | synthetic clade upper bound | binary production upper bound | rooted binary refinements |
|---:|---:|---:|---:|
| 3 | 3 | 6 | 3 |
| 4 | 10 | 25 | 15 |
| 5 | 25 | 90 | 105 |
| 6 | 56 | 301 | 945 |
| 7 | 119 | 966 | 10395 |
| 8 | 246 | 3025 | 135135 |
| 9 | 501 | 9330 | 2027025 |

## Default recommendation

Keep the default exact arity cap at `6`.

Rationale: arity `6` remains well within the default compact-closure caps and
has `945` rooted binary refinements.  Arity `7` still fits the compact production
cap but jumps to `10395` rooted refinements, and real fixtures already contain
arity `8/9` events where exact expansion should be an explicit opt-in.  The CLI
therefore warns with theoretical upper bounds, but the benchmark still attempts
the reuse-aware exact expander and reports the actual success/failure.

Bounded benchmarking defaults to candidate seed-shape caps `1,4,16`; those caps
are reproducible diagnostics, not claims of full soft-polytomy exactness unless
`exact_for_soft_polytomies: true` is printed.

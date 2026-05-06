# larch2

A phylogenetic DAG (directed acyclic graph) library and optimizer. larch2
merges multiple phylogenetic trees into a DAG and iteratively improves
parsimony scores using SPR (subtree prune and regraft) moves.

## Building

larch2 requires a C++ compiler with C++26 and `-freflection` support (GCC
trunk), plus CMake 3.25+, zlib, and pthreads.

```sh
cmake -B build -DGCC_TOOLCHAIN=/path/to/gcc-install
cmake --build build
```

`GCC_TOOLCHAIN` should point to the GCC installation prefix (the directory
containing `bin/gcc`, `bin/g++`, etc.). CMake will auto-discover the compiler
and supporting tools (ar, ranlib) from that prefix.

Optional CMake options:

| Option | Description |
|--------|-------------|
| `-DCMAKE_BUILD_TYPE=Release` | Enable optimizations and LTO |
| `-DENABLE_ASAN=ON` | Enable AddressSanitizer |
| `-DENABLE_TSAN=ON` | Enable ThreadSanitizer |

### Running tests

```sh
ctest --test-dir build
```

## Usage

```
larch2 [options] -o <output.pb.gz>
```

### Input formats (one required)

| Option | Description |
|--------|-------------|
| `--dag-pb <path>` | Protobuf DAG (`.pb` or `.pb.gz`) |
| `--tree-pb <path>` | Parsimony protobuf tree (requires `--refseq`) |
| `--fasta <path>` | Leaf sequences in FASTA format (requires `--newick` and `--refseq`) |

| Option | Description |
|--------|-------------|
| `--newick <path>` | Tree topology (Newick string file, used with `--fasta`) |
| `--refseq <path>` | Reference sequence file (required with `--tree-pb` or `--fasta`) |
| `--vcf <path>` | VCF file with ambiguous leaf sequences (optional) |

### Output

| Option | Description |
|--------|-------------|
| `-o, --output <path>` | Output DAG in protobuf format (required) |

### Optimization

| Option | Default | Description |
|--------|---------|-------------|
| `-n, --iterations <N>` | 10 | Number of optimization iterations |
| `--patience <P>` | off | Stop after P iterations without active sampling-objective improvement |
| `--drift <N>` | off | With parsimony sampling, try N drift iterations when patience triggers |
| `--optimizer <name>` | native | `native` (SPR enumeration) or `random` |
| `--max-moves <N>` | 50 | Max moves per iteration (native optimizer) |
| `--seed <N>` | random | Random seed for reproducibility |
| `--sample-per-radius` | off | Re-sample tree between radius doublings |

### Sampling

| Option | Default | Description |
|--------|---------|-------------|
| `--sample-method <M>` | parsimony | `parsimony`, `random`, `rf-minsum`, `rf-maxsum`, `ml`/`thrifty`, or `edge-weight` |
| `--sample-uniformly` | off | Weight sampling proportional to subtree tree-counts |
| `--ignore-root-edge-mutations` | off | Ignore UA-to-root edge mutations in parsimony scoring |
| `--ignore-ua-edge-ml` | on | Default ML behavior: ignore the UA-to-root edge |
| `--score-ua-edge-ml` | off | Opt in to scoring the UA-to-root edge during active ML scoring |
| `--model-dir <path>` | | Model directory for `ml`/`thrifty` sampling or ML move scoring; must be paired with `--model-name` |
| `--model-name <name>` | | Model name, e.g. `ThriftyHumV0.2-45`; must be paired with `--model-dir` |

Sampling methods:

| Method | Meaning |
|--------|---------|
| `parsimony` | Sample a minimum mutation-count tree from the DAG |
| `random` | Sample any compatible tree from the DAG |
| `rf-minsum` / `rf-maxsum` | Sample by RF-distance criterion during larch2 optimization |
| `ml` / `thrifty` | Sample a minimum ML/NN negative-log-likelihood tree; requires `--model-dir` and `--model-name` |
| `edge-weight` | Sample a minimum sum of stored protobuf/in-memory `edge_weight` values |

`--trim` and `--diverse-sample` currently support only the parsimony sampling
objective; non-parsimony `--sample-method` values are rejected with those modes.

### Move strategy

| Option | Default | Description |
|--------|---------|-------------|
| `--callback-option <O>` | best-moves | `best-moves` or `all-moves` |
| `--move-coeff-pscore <N>` | 1 | Parsimony score coefficient for move scoring |
| `--move-coeff-nodes <N>` | 0 | New-node penalty coefficient for move scoring |
| `--move-coeff-ml <F>` | 0.0 | ML log-likelihood coefficient for SPR move rescoring; requires model args when positive |
| `--move-score-threshold <N>` | -1 (or 0 with node penalty) | Max parsimony score for enumerated moves |

### Subtree optimization

| Option | Default | Description |
|--------|---------|-------------|
| `--switch-subtrees <N>` | off | After N iterations, optimize subtrees instead of the whole tree |
| `--min-subtree-clade-size <N>` | 100 | Minimum leaves in selected subtree |
| `--max-subtree-clade-size <N>` | 1000 | Maximum leaves in selected subtree |

### Diverse tree extraction

| Option | Default | Description |
|--------|---------|-------------|
| `--diverse-sample <K>` | off | Extract K maximally diverse parsimony-optimal trees; requires parsimony sampling |
| `--diverse-pool <N>` | max(10K, 100) | Override the candidate pool size |
| `--diverse-newick <path>` | | Write selected trees as Newick strings (one per line) |

### Other

| Option | Description |
|--------|-------------|
| `--trim` | Trim output to a single minimum-parsimony tree; rejects non-parsimony `--sample-method` |
| `--log-metrics` | Print extended per-iteration metrics to stderr |
| `--validate` | Validate DAG invariants at key pipeline points |

### Examples

Optimize a protobuf DAG for 20 iterations:

```sh
larch2 --dag-pb input.pb.gz -o output.pb.gz -n 20
```

Build from FASTA + Newick and optimize:

```sh
larch2 --fasta seqs.fasta --newick tree.nwk --refseq ref.txt -o output.pb.gz
```

Optimize a parsimony protobuf tree with RF-distance sampling:

```sh
larch2 --tree-pb tree.pb.gz --refseq ref.txt -o output.pb.gz \
    --sample-method rf-minsum -n 50
```

Sample optimization trees by Thrifty/ML NLL:

```sh
larch2 --dag-pb input.pb.gz -o output.pb.gz \
    --sample-method ml \
    --model-dir data/bcr --model-name ThriftyHumV0.2-45
```

Use Thrifty/ML for both DAG tree sampling and SPR move rescoring:

```sh
larch2 --dag-pb input.pb.gz -o output.pb.gz \
    --sample-method ml \
    --move-coeff-ml 1.0 \
    --model-dir data/bcr --model-name ThriftyHumV0.2-45
```

Note: `--sample-method ml` controls which tree is sampled from the DAG. With
the native optimizer, providing `--model-dir` and `--model-name` without
`--sample-method ml` defaults move scoring to ML-only (`--move-coeff-ml 1.0`,
and parsimony coefficient 0 unless explicitly set). With `--sample-method ml`,
ML move scoring is still disabled unless `--move-coeff-ml` is set. The
`--ignore-ua-edge-ml` / `--score-ua-edge-ml` setting applies to all active
larch2 ML scoring paths: ML sampling, ML move scoring, and ML metrics; larch2
warns if either flag is supplied when no ML scoring path is active. ML sampling
progress is reported as `parsimony P, ML NLL X`; edge-weight sampling is
reported as `parsimony P, edge_weight W`.

Note: `--sample-method edge-weight` is intended for DAGs where every edge has a
meaningful stored protobuf `edge_weight`. Mixing scored and unscored DAGs, or
continuing optimization after edge-weight sampling, can introduce default-zero
new/unknown edges that later look artificially cheap.

Convergence reporting includes both sampled-tree parsimony and the active
sampling objective. `--patience` tracks the active sampling objective (`ML NLL`
for `ml`/`thrifty`, `edge_weight` for edge-weight sampling, RF score for RF
sampling, otherwise parsimony). `--drift` is still parsimony-specific and is
rejected with non-parsimony `--sample-method` values.

Extract 5 diverse optimal trees as Newick:

```sh
larch2 --dag-pb input.pb.gz -o output.pb.gz \
    --diverse-sample 5 --diverse-newick trees.nwk
```

## dagutil

`dagutil` is a companion utility for merging, pruning, and inspecting
phylogenetic DAGs and trees. It accepts multiple inputs, merges them into a
single DAG, and can report statistics, trim, or sample from the result.

```
dagutil [options]
```

### Input (repeatable, at least one required)

| Option | Description |
|--------|-------------|
| `--dag-pb <path>` | Protobuf DAG (`.pb` or `.pb.gz`) |
| `--tree-pb <path>` | Parsimony protobuf tree (requires `--refseq`) |
| `--fasta <path>` | Leaf sequences in FASTA format (requires `--newick` and `--refseq`) |
| `--newick <path>` | Tree topology (paired with `--fasta`) |
| `--refseq <path>` | Reference sequence file |
| `--vcf <path>` | VCF file (required unless `--force-no-vcf`) |
| `--force-no-vcf` | Skip VCF requirement |

Each `--dag-pb`, `--tree-pb`, or `--fasta`/`--newick` pair can appear multiple
times. All loaded inputs are merged into a single DAG.

### Output

| Option | Description |
|--------|-------------|
| `-o, --output <path>` | Output DAG in protobuf format (optional -- omit to skip output) |

### Pruning and sampling

| Option | Description |
|--------|-------------|
| `-t, --trim` | Trim to best parsimony score |
| `--rf <path>` | Trim to minimize RF distance to this DAG file |
| `-s, --sample` | Sample a single tree from the DAG |
| `--sample-method <M>` | Sampling criterion: `random` (default), `parsimony`, `ml`/`thrifty`, or `edge-weight` |
| `--sample-uniformly` | Weight sampling proportional to subtree tree-counts |
| `--model-dir <path>` | Model directory for `ml`/`thrifty` sampling or `--edge-ml` |
| `--model-name <name>` | Model name, e.g. `ThriftyHumV0.2-45` |
| `--ignore-ua-edge-ml` | Ignore UA-to-root edge during ML scoring (default) |
| `--score-ua-edge-ml` | Score UA-to-root edge during ML scoring |
| `--seed <N>` | Random seed for sampling |

`--sample-method` is used for `--sample` tree extraction. With `--trim --rf
--sample`, the RF criterion comes from `--rf`, so `--sample-method` should be
omitted. The `--ignore-ua-edge-ml` / `--score-ua-edge-ml` setting also applies
to `--edge-ml`. `--edge-parsimony` and `--edge-ml` write penalties to an output
DAG and cannot be combined with `--trim` or `--sample`; run sampling/trimming as
a second command.

### Analysis

| Option | Description |
|--------|-------------|
| `--dag-info` | Print all DAG statistics (tree count, parsimony, RF) |
| `--parsimony` | Print parsimony score distribution |
| `--sum-rf-distance` | Print sum RF distance distribution |
| `--edge-parsimony` | Store per-edge global parsimony penalties in protobuf `edge_weight` (cannot combine with `--trim`/`--sample`) |
| `--edge-ml` | Store per-edge global ML-NLL penalties in protobuf `edge_weight` (requires model args; `--edge-thrifty` alias also accepted; cannot combine with `--trim`/`--sample`) |
| `--validate` | Validate DAG invariants |

Per-edge penalty outputs use the stored protobuf `edge_weight` field. For each
edge `e`, dagutil writes:

```text
penalty[e] = min_score(any tree containing e) - global_min_score
```

So `edge_weight == 0` means the edge appears in at least one globally optimal
tree under that criterion (within numerical tolerance for ML). `--edge-ml` uses
ML/NN negative log likelihood and is affected by `--ignore-ua-edge-ml` /
`--score-ua-edge-ml`.

### Examples

Inspect a single DAG:

```sh
dagutil --dag-pb input.pb.gz --force-no-vcf --dag-info
```

Merge two DAGs and save the result:

```sh
dagutil --dag-pb a.pb.gz --dag-pb b.pb.gz --force-no-vcf -o merged.pb.gz
```

Trim to minimum parsimony and sample a single tree:

```sh
dagutil --dag-pb input.pb.gz --force-no-vcf -o best.pb -t -s --seed 42
```

Trim to minimize RF distance to a reference DAG:

```sh
dagutil --dag-pb input.pb.gz --force-no-vcf -o closest.pb -t --rf ref.pb.gz
```

Extract a minimum Thrifty/ML-NLL tree:

```sh
dagutil --dag-pb input.pb.gz --force-no-vcf --sample \
    --sample-method ml --model-dir data/bcr --model-name ThriftyHumV0.2-45 \
    -o sampled-thrifty.pb.gz
```

Compute per-edge Thrifty/ML penalties and then extract a tree minimizing the
stored penalties:

```sh
dagutil --dag-pb input.pb.gz --force-no-vcf --edge-ml \
    --model-dir data/bcr --model-name ThriftyHumV0.2-45 \
    -o ml-edge-penalties.pb.gz

dagutil --dag-pb ml-edge-penalties.pb.gz --force-no-vcf --sample \
    --sample-method edge-weight -o sampled-edge-weight.pb.gz
```

## License

See [LICENSE](LICENSE).

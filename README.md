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
| `--optimizer <name>` | native | `native` (SPR enumeration) or `random` |
| `--max-moves <N>` | 50 | Max moves per iteration (native optimizer) |
| `--seed <N>` | random | Random seed for reproducibility |
| `--sample-per-radius` | off | Re-sample tree between radius doublings |

### Sampling

| Option | Default | Description |
|--------|---------|-------------|
| `--sample-method <M>` | parsimony | `parsimony`, `random`, `rf-minsum`, or `rf-maxsum` |
| `--sample-uniformly` | off | Weight sampling proportional to subtree tree-counts |
| `--ignore-root-edge-mutations` | off | Ignore UA-to-root edge mutations in parsimony scoring |

### Move strategy

| Option | Default | Description |
|--------|---------|-------------|
| `--callback-option <O>` | best-moves | `best-moves` or `all-moves` |
| `--move-coeff-pscore <N>` | 1 | Parsimony score coefficient for move scoring |
| `--move-coeff-nodes <N>` | 0 | New-node penalty coefficient for move scoring |
| `--move-score-threshold <N>` | -1 (or 0 with node penalty) | Max parsimony score for enumerated moves |

### Subtree optimization

| Option | Default | Description |
|--------|---------|-------------|
| `--switch-subtrees <N>` | off | After N iterations, optimize subtrees instead of the whole tree |
| `--min-subtree-clade-size <N>` | 100 | Minimum leaves in selected subtree |
| `--max-subtree-clade-size <N>` | 1000 | Maximum leaves in selected subtree |

### Other

| Option | Description |
|--------|-------------|
| `--trim` | Trim output to a single minimum-parsimony tree |
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
| `--seed <N>` | Random seed for sampling |

### Analysis

| Option | Description |
|--------|-------------|
| `--dag-info` | Print all DAG statistics (tree count, parsimony, RF) |
| `--parsimony` | Print parsimony score distribution |
| `--sum-rf-distance` | Print sum RF distance distribution |
| `--validate` | Validate DAG invariants |

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

## License

See [LICENSE](LICENSE).

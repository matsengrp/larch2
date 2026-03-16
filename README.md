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

## License

See [LICENSE](LICENSE).

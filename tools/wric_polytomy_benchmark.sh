#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dagutil=${DAGUTIL:-"$repo_root/build/wric-asan/bin/dagutil"}
shape_caps=${WRIC_POLYTOMY_BENCHMARK_SHAPE_CAPS:-"1,4,16"}
synthetic_arities=${WRIC_POLYTOMY_SYNTHETIC_ARITIES:-"4 6 8"}
extra_dag_pbs=${WRIC_POLYTOMY_EXTRA_DAG_PBS:-""}
extra_tree_pbs=${WRIC_POLYTOMY_EXTRA_TREE_PBS:-""}
include_data_fixtures=${WRIC_POLYTOMY_INCLUDE_DATA_FIXTURES:-0}

if [[ ! -x "$dagutil" ]]; then
  echo "error: dagutil binary not found/executable: $dagutil" >&2
  echo "Set DAGUTIL=/path/to/dagutil or build the default wric-asan target." >&2
  exit 1
fi

run_benchmark() {
  local label=$1
  shift
  printf '\n### %s\n' "$label"
  "$dagutil" "$@"
}

run_benchmark \
  "data/test_5_trees/tree_0.pb.gz" \
  --dag-pb "$repo_root/data/test_5_trees/tree_0.pb.gz" \
  --force-no-vcf \
  --wric-benchmark \
  --wric-polytomy-benchmark-shape-caps "$shape_caps"

for dag_pb in $extra_dag_pbs; do
  run_benchmark \
    "extra DAG ${dag_pb}" \
    --dag-pb "$dag_pb" \
    --force-no-vcf \
    --wric-benchmark \
    --wric-polytomy-benchmark-shape-caps "$shape_caps"
done

for tree_spec in $extra_tree_pbs; do
  if [[ "$tree_spec" != *:* ]]; then
    echo "error: WRIC_POLYTOMY_EXTRA_TREE_PBS entries must be tree.pb.gz:refseq" >&2
    exit 1
  fi
  tree_pb=${tree_spec%%:*}
  refseq=${tree_spec#*:}
  run_benchmark \
    "extra tree ${tree_pb}" \
    --tree-pb "$tree_pb" \
    --refseq "$refseq" \
    --force-no-vcf \
    --wric-benchmark \
    --wric-polytomy-benchmark-shape-caps "$shape_caps"
done

if [[ "$include_data_fixtures" != 0 ]]; then
  seedtree="$repo_root/data/seedtree/seedtree.pb.gz"
  seedref="$repo_root/data/seedtree/refseq.txt.gz"
  if [[ -f "$seedtree" && -f "$seedref" ]]; then
    run_benchmark \
      "data/seedtree/seedtree.pb.gz" \
      --tree-pb "$seedtree" \
      --refseq "$seedref" \
      --force-no-vcf \
      --wric-benchmark \
      --wric-polytomy-benchmark-shape-caps "$shape_caps"
  fi

  tree20d="$repo_root/data/20D_from_fasta/1final-tree-1.nh1.pb.gz"
  ref20d="$repo_root/data/20D_from_fasta/refseq.txt"
  if [[ -f "$tree20d" && -f "$ref20d" ]]; then
    run_benchmark \
      "data/20D_from_fasta/1final-tree-1.nh1.pb.gz" \
      --tree-pb "$tree20d" \
      --refseq "$ref20d" \
      --force-no-vcf \
      --wric-benchmark \
      --wric-polytomy-benchmark-shape-caps "$shape_caps"
  fi
fi

run_benchmark \
  "test/wric_two_polytomy.fa + .nwk" \
  --fasta "$repo_root/test/wric_two_polytomy.fa" \
  --newick "$repo_root/test/wric_two_polytomy.nwk" \
  --refseq "$repo_root/test/wric_two_polytomy.ref" \
  --force-no-vcf \
  --wric-benchmark \
  --wric-polytomy-benchmark-shape-caps "$shape_caps"

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
printf 'A\n' >"$tmpdir/ref.fa"

make_star_fixture() {
  local n=$1
  local fasta=$2
  local newick=$3
  : >"$fasta"
  : >"$newick"
  printf '(' >>"$newick"
  for ((i = 1; i <= n; ++i)); do
    local label="T$i"
    local state
    case $(( (i - 1) % 4 )) in
      0) state=A ;;
      1) state=C ;;
      2) state=G ;;
      *) state=T ;;
    esac
    printf '>%s\n%s\n' "$label" "$state" >>"$fasta"
    if (( i > 1 )); then printf ',' >>"$newick"; fi
    printf '%s' "$label" >>"$newick"
  done
  printf ');\n' >>"$newick"
}

for arity in $synthetic_arities; do
  fasta="$tmpdir/star_${arity}.fa"
  newick="$tmpdir/star_${arity}.nwk"
  make_star_fixture "$arity" "$fasta" "$newick"
  run_benchmark \
    "synthetic ${arity}-star" \
    --fasta "$fasta" \
    --newick "$newick" \
    --refseq "$tmpdir/ref.fa" \
    --force-no-vcf \
    --wric-benchmark \
    --wric-polytomy-benchmark-shape-caps "$shape_caps"
done

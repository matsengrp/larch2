#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dagutil=${DAGUTIL:-"$repo_root/build/wric-asan/bin/dagutil"}
shape_caps=${WRIC_POLYTOMY_BENCHMARK_SHAPE_CAPS:-"1,4,16"}
synthetic_arities=${WRIC_POLYTOMY_SYNTHETIC_ARITIES:-"4 6 8"}
extra_dag_pbs=${WRIC_POLYTOMY_EXTRA_DAG_PBS:-""}

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
  --wric-polytomy-benchmark \
  --wric-polytomy-benchmark-shape-caps "$shape_caps"

for dag_pb in $extra_dag_pbs; do
  run_benchmark \
    "extra DAG ${dag_pb}" \
    --dag-pb "$dag_pb" \
    --force-no-vcf \
    --wric-polytomy-benchmark \
    --wric-polytomy-benchmark-shape-caps "$shape_caps"
done

run_benchmark \
  "test/wric_two_polytomy.fa + .nwk" \
  --fasta "$repo_root/test/wric_two_polytomy.fa" \
  --newick "$repo_root/test/wric_two_polytomy.nwk" \
  --refseq "$repo_root/test/wric_two_polytomy.ref" \
  --force-no-vcf \
  --wric-polytomy-benchmark \
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
    --wric-polytomy-benchmark \
    --wric-polytomy-benchmark-shape-caps "$shape_caps"
done

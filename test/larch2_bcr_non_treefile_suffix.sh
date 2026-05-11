#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <larch2-bcr executable>" >&2
  exit 2
fi

larch2_bcr=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/trees"
cat >"$tmp/reference.txt" <<'EOF'
AAAA
EOF
cat >"$tmp/sequences.fa" <<'EOF'
>A
AAAA
>B
AACA
EOF
cat >"$tmp/trees/example-rerooted.nwk" <<'EOF'
(A,B);
EOF

set +e
"$larch2_bcr" \
  --fasta "$tmp/sequences.fa" \
  --trees "$tmp/trees" \
  --reference "$tmp/reference.txt" \
  --tree-suffix "-rerooted.nwk" \
  -o "$tmp/out.pb.gz" \
  >"$tmp/stdout" \
  2>"$tmp/stderr"
status=$?
set -e

cat "$tmp/stdout"
cat "$tmp/stderr" >&2

if [[ $status -ne 0 ]]; then
  exit $status
fi

if [[ ! -s "$tmp/out.pb.gz" ]]; then
  echo "expected non-empty output DAG at $tmp/out.pb.gz" >&2
  exit 1
fi

if ! grep -q "Found 1 tree files" "$tmp/stderr"; then
  echo "expected larch2-bcr to find the .nwk tree" >&2
  exit 1
fi

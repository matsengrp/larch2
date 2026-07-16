#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <counter-baseline-doc> <counter-baseline-generator>" >&2
  exit 2
fi

doc=$1
generator=$2
for path in "$doc" "$generator"; do
  if [[ ! -f $path ]]; then
    echo "missing counter-baseline input: $path" >&2
    exit 2
  fi
done

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

begin='<!-- wric-counter-baseline: snapshot begin -->'
end='<!-- wric-counter-baseline: snapshot end -->'
awk -v begin="$begin" -v end="$end" '
  $0 == begin { inside = 1; begin_count += 1; next }
  $0 == end { inside = 0; end_count += 1; next }
  inside { print }
  END {
    if (begin_count != 1 || end_count != 1 || inside) exit 1
  }
' "$doc" >"$tmp" || {
  echo "counter-baseline snapshot markers are missing, duplicate, or unbalanced" >&2
  exit 1
}

keys=(
  lazy_local_iteration_generation_phase_bytes_max
  lazy_local_iteration_evidence_phase_bytes_max
  lazy_local_ranked_candidate_exact_evidence_bytes_max
)
for key in "${keys[@]}"; do
  snapshot_count=$(grep -Ec "^  ${key}: +0$" "$tmp" || true)
  if [[ $snapshot_count -ne 1 ]]; then
    echo "expected exactly one zero-valued '$key' in frozen snapshot; found $snapshot_count" >&2
    exit 1
  fi

  spelling_count=$(grep -Fc "\"$key\"" "$generator" || true)
  member_count=$(grep -Fc "c.$key" "$generator" || true)
  if [[ $spelling_count -ne 1 || $member_count -ne 1 ]]; then
    echo "generator contract for '$key' is not one spelling/one member read" >&2
    exit 1
  fi
done

echo "wric_counter_baseline_temporal_fields: PASS"

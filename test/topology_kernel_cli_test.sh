#!/usr/bin/env bash
set -euo pipefail

kernel=$1
work=$(mktemp -d "${TMPDIR:-/tmp}/larch-topology-kernel.XXXXXX")
trap 'rm -rf "$work"' EXIT

"$kernel" generate s0_binary_6 "$work/s0"
"$kernel" generate s0m_hard_4 "$work/s0m"

check_summary() {
  local file=$1 key=$2 expected=$3
  local actual
  actual=$(awk -F '\t' -v key="$key" '$1 == key {print $2}' "$file")
  test "$actual" = "$expected"
}

check_summary "$work/s0/SUMMARY.tsv" tree_count 945
check_summary "$work/s0/SUMMARY.tsv" edge_count 22320
check_summary "$work/s0/SUMMARY.tsv" witness_count 59760
check_summary "$work/s0/SUMMARY.tsv" move_relation rooted_common_prune_reduction_binary_rspr_v1
check_summary "$work/s0/SUMMARY.tsv" move_symmetry intrinsic_bidirectional
check_summary "$work/s0/SUMMARY.tsv" root_policy rooted_no_synthetic_ua
check_summary "$work/s0/SUMMARY.tsv" ua_scoring fixed_reference_root_boundary_unit_cost
check_summary "$work/s0/SUMMARY.tsv" trees_sha256 09b6589e747fb20b980b024d4fc01ba4ede36b283f13a011e22cd0a7db62b587
check_summary "$work/s0/SUMMARY.tsv" edges_sha256 292f37ba9e89985d791347f1d2675c2600fc04c8b3675fa7c050e67822d3155f
check_summary "$work/s0/SUMMARY.tsv" witnesses_sha256 b641c35e168e740448b779a11285b5537d8322ad7b97ae6e3481ad1c6a56f00e

check_summary "$work/s0m/SUMMARY.tsv" tree_count 26
check_summary "$work/s0m/SUMMARY.tsv" edge_count 202
check_summary "$work/s0m/SUMMARY.tsv" witness_count 656
check_summary "$work/s0m/SUMMARY.tsv" move_relation rooted_common_prune_reduction_hard_rspr_v1
check_summary "$work/s0m/SUMMARY.tsv" move_symmetry intrinsic_bidirectional
check_summary "$work/s0m/SUMMARY.tsv" root_policy rooted_no_synthetic_ua
check_summary "$work/s0m/SUMMARY.tsv" ua_scoring fixed_reference_root_boundary_unit_cost
check_summary "$work/s0m/SUMMARY.tsv" trees_sha256 8bfc936763454484e2b3bd7773d22c996e502f28ae83ffd1432ea381cc793e7d
check_summary "$work/s0m/SUMMARY.tsv" edges_sha256 115209a46f5edd59659d614013a9f6189631e79d4eadf53fe33678e09b58b265
check_summary "$work/s0m/SUMMARY.tsv" witnesses_sha256 720059acde440070e7b534957b93b7a6c8e0cc95c7ece8d59b4df34222f243c2

if "$kernel" generate s0_binary_6 "$work/s0" >/dev/null 2>&1; then
  echo "topology-kernel replaced an existing artifact" >&2
  exit 1
fi

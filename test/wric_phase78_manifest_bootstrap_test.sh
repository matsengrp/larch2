#!/usr/bin/env bash
set -euo pipefail

[[ $# == 1 ]] || { echo "usage: $0 PROCESS_METRICS" >&2; exit 2; }
repo_root=$(git rev-parse --show-toplevel)
export PYTHONDONTWRITEBYTECODE=1
export WRIC_PHASE78_TEST_PROCESS_METRICS=$1
exec python3 "$repo_root/test/wric_phase78_manifest_bootstrap_test.py"

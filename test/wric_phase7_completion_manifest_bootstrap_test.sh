#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
runner=${1:?usage: $0 WRIC_PROCESS_METRICS}
export WRIC_PHASE7_COMPLETION_TEST_PROCESS_METRICS=$runner
exec python3 "$repo_root/test/wric_phase7_completion_manifest_bootstrap_test.py"

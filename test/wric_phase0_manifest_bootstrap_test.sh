#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tool="$repo_root/tools/wric_phase0_manifest_bootstrap.py"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

python3 "$tool" self-test >"$tmp/self-test.out"
grep -q '^PASS: .* unique rows;' "$tmp/self-test.out"
python3 "$repo_root/tools/wric_wrapper_calibration.py" self-test \
  >"$tmp/calibration-self-test.out"
grep -qx 'PASS: wrapper calibration controller arithmetic/schema/lifecycle self-test' \
  "$tmp/calibration-self-test.out"

# Prepare has no calibration bypass: argparse must reject an invocation that
# binds an oracle but omits the canonical wrapper-calibration artifact.
if python3 "$tool" prepare \
    --expected-oracle-sha256 \
    7ddb1fca7b15d1057912d6775b5e5fb32218390f13b3a10f6622581f21a5a38c \
    >"$tmp/missing-calibration.out" 2>&1; then
  echo "prepare accepted a missing --wrapper-calibration" >&2
  exit 1
fi
grep -q -- '--wrapper-calibration' "$tmp/missing-calibration.out"

# The bootstrap, frozen harness source, and process wrapper must name one exact
# closed schema-v2 key set. This catches one-sided additions or stale parsers.
python3 - "$tool" "$repo_root/tools/wric_spr_search_benchmark.sh" \
  "$repo_root/tools/wric_process_metrics.cpp" <<'PY'
import ast
from pathlib import Path
import re
import sys

helper_path, harness_path, runner_path = map(Path, sys.argv[1:])
helper = helper_path.read_text(encoding="utf-8")
module = ast.parse(helper)
helper_fields = None
admission_fields = None
for statement in module.body:
    if not isinstance(statement, ast.Assign):
        continue
    if any(
        isinstance(target, ast.Name)
        and target.id == "PROCESS_METRICS_V2_FIELDS"
        for target in statement.targets
    ):
        helper_fields = set(ast.literal_eval(statement.value))
    if any(
        isinstance(target, ast.Name)
        and target.id == "EXACT_CANDIDATE_ADMISSION_FIELDS"
        for target in statement.targets
    ):
        admission_fields = set(ast.literal_eval(statement.value))
assert helper_fields is not None
assert admission_fields is not None and len(admission_fields) == 7
harness = harness_path.read_text(encoding="utf-8")
match = re.search(
    r'required="(schema_version outcome exit_code[^\"]+)"', harness
)
assert match is not None
harness_fields = set(match.group(1).split())
runner = runner_path.read_text(encoding="utf-8")
start = runner.index('add_integer("schema_version"')
end = runner.index("bool metrics_written", start)
runner_fields = set(
    re.findall(r'add_(?:integer|metric)\s*\(\s*"([^"]+)"', runner[start:end])
)
assert len(helper_fields) == 41
assert helper_fields == harness_fields == runner_fields
trial_start = harness.index("trial_columns=(")
trial_end = harness.index("\n(", trial_start)
harness_trial_fields = set(
    re.findall(r"[A-Za-z_][A-Za-z0-9_]*", harness[trial_start:trial_end])
)
assert admission_fields <= harness_trial_fields
assert '",".join(EXACT_CANDIDATE_ADMISSION_FIELDS)' in helper
PY

python3 "$tool" matrix --unpinned-affinity 0-15 >"$tmp/matrix.tsv"
awk -F '\t' '
  NR==1 {if ($0!="kind\tid\taffinity\tfixture\tcontract") exit 1; next}
  $1=="capture" {captures++; capture[$2]=$0}
  $1=="row" {
    rows++
    id=$2
    if (seen[id]++) exit 1
    if ($5 ~ /@auto\//) any_auto++
    if ($5 ~ /chart_spr_grammar_exact@auto\//) exact_auto++
    if ($5 ~ /chart_spr_grammar_exact@default\//) defaults++
    if (id=="p0-real20d-preflight-grammar-exact-w1") real_w1++
    if (id=="p0-real20d-preflight-grammar-exact-w8") real_w8++
    if ($5 ~ /\/p0-small-primary-physical\/target=3$/) small32++
    if ($5 ~ /\/p0-small-stress-physical\/target=3$/) small128++
    if ($5 ~ /\/p0-primary-physical\/target=5$/) primary5++
    if ($5 ~ /\/p0-stress-physical\/target=3$/) stress3++
  }
  END {
    if (captures!=13 || rows!=89) exit 1
    if (!("small-primary32k4-physical" in capture)) exit 1
    if (!("small-stress128k16-physical" in capture)) exit 1
    if (!("medium-primary32k4-physical" in capture)) exit 1
    if (!("medium-stress128k16-physical" in capture)) exit 1
    if (!("medium-primary32k4-smt" in capture)) exit 1
    if (!("small-auto-unpinned" in capture)) exit 1
    if (!("real20d-exact1-preflight-physical" in capture)) exit 1
    if (any_auto<2 || exact_auto!=1 || defaults!=1) exit 1
    if (real_w1!=1 || real_w8!=1) exit 1
    if (small32!=12 || small128!=13) exit 1
    if (primary5!=13 || stress3!=13) exit 1
  }
' "$tmp/matrix.tsv"

grep -q $'^capture\tsmall-primary32k4-physical\t[^\t]*\tsmall\ti1/m50/c32/k4/' \
  "$tmp/matrix.tsv"
grep -q $'^capture\tsmall-stress128k16-physical\t[^\t]*\tsmall\ti3/m50/c128/k16/' \
  "$tmp/matrix.tsv"
grep -q 'repeat=characterize1_then_warmup1_success_only' "$tmp/matrix.tsv"
"$repo_root/tools/wric_spr_search_benchmark.sh" --help \
  >"$tmp/harness-help.txt" 2>&1
grep -q -- '--native-only' "$tmp/harness-help.txt"

# A distinct unpinned cpuset must add auto/default rows without making the
# non-group resolver ambiguous; the Python self-test checks the full key and
# this surface assertion ensures the conditional plan is visible.
python3 "$tool" matrix --unpinned-affinity 2-15 >"$tmp/distinct.tsv"
grep -q $'^capture\tmedium-primary32k4-unpinned\t2-15\tmedium\t' "$tmp/distinct.tsv"
grep -q 'p0-primary-unpinned' "$tmp/distinct.tsv"

echo "PASS: Phase-0 bootstrap matrix, canonical wrapper calibration, real refusal rows, auto/default coverage, and unique resolver keys"

#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
helper=$repo_root/tools/wric_phase9_manifest_bootstrap.py
tmp=$repo_root/build/wric-phase9-manifest-bootstrap-test.$$
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/base" "$tmp/characterization/evidence" "$tmp/out-a" "$tmp/out-b"

sha() { sha256sum "$1" | awk '{print $1}'; }
seal() { printf '%s  %s\n' "$(sha "$1")" "$(basename "$1")" >"$1.sha256"; }
expect_fail() {
  local label=$1; shift
  if "$@" >"$tmp/$label.stdout" 2>"$tmp/$label.stderr"; then
    echo "expected failure: $label" >&2
    exit 1
  fi
}

printf '#!/usr/bin/env bash\nexit 0\n' >"$tmp/base/larch2"
printf '#!/usr/bin/env bash\nexit 0\n' >"$tmp/base/dagutil"
chmod 555 "$tmp/base/larch2" "$tmp/base/dagutil"
printf 'base characterization commands\n' >"$tmp/base/commands.sh"
printf 'synthetic phase9 fixture\n' >"$tmp/fixture.pb.gz"
fixture_sha=$(sha "$tmp/fixture.pb.gz")
oracle_sha=$(sha "$tmp/base/dagutil")

manifest_header=$(python3 - "$repo_root" <<'PY'
import importlib.util
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / "tools/wric_phase9_manifest_bootstrap.py"
spec = importlib.util.spec_from_file_location("phase9_bootstrap", path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
print("\t".join(module.MANIFEST_HEADER))
PY
)

python3 - "$manifest_header" >"$tmp/base/base-row.tsv" <<'PY'
import sys
header = sys.argv[1].split("\t")
row = {field: "-" for field in header}
row["row_id"] = "synthetic-base-row"
print("\t".join(row[field] for field in header))
PY

base=$tmp/base/workloads.tsv
{
  printf '%s\n' \
    '# schema=wric_chart_parallelization_workloads' \
    '# schema_version=1' \
    '# kind=base' \
    '# manifest_id=synthetic-phase0' \
    '# parent_sha256=-' \
    '# repo_revision=0000000000000000000000000000000000000000' \
    '# merge_base=1111111111111111111111111111111111111111' \
    '# frozen_larch2_uri=manifest://larch2' \
    "# frozen_larch2_sha256=$(sha "$tmp/base/larch2")" \
    '# frozen_oracle_dagutil_uri=manifest://dagutil' \
    "# frozen_oracle_dagutil_sha256=$oracle_sha" \
    '# commands_uri=manifest://commands.sh' \
    "# commands_sha256=$(sha "$tmp/base/commands.sh")"
  printf '%s\n' "$manifest_header"
  cat "$tmp/base/base-row.tsv"
} >"$base"
seal "$base"
parent_sha=$(sha "$base")

make_report() {
  local seed=$1 workers=$2 path=$3
  cat >"$path" <<EOF
chart_spr_search:
  acceptance: exact_multisite
  objective: grammar_exact
  candidate_selection: lower_bound_top_k
  candidate_source: grammar
  candidate_cap_semantics: post-dedup
  topology_selector: none
  requested_max_iterations: 3
  configured_max_candidates: 32
  top_k_exact_verify: 4
  randomize_order: false
  reservoir_sample: false
  include_immediate_reversals: false
  sampled_tree_count: 1
  sampled_tree_radius: 0
  sampled_tree_score_threshold: 2147483647
  max_upward_path_expansions: 0
  max_path_pairs: 0
  min_moved_clade_size: 1
  max_moved_clade_size: 0
  min_target_clade_size: 1
  max_target_clade_size: 0
  max_affected_clades: 0
  polytomy_mode: expand-bounded
  polytomy_max_exact_arity: 6
  polytomy_max_shapes: 1
  polytomy_max_productions: 1024
  polytomy_max_clades: 256
  lazy_policy: off
  max_cached_patterns: 0
  configured_pattern_batch_size: 0
  configured_candidate_batch_size: 0
  memory_budget_bytes: 12884901888
  commit_mode: overlay_delta
  verification_mode: transient
  local_accept_updates: true
  dominance_mode: off
  bound_pruning: true
  require_exact_keep_mask: true
  max_frontier_entries: 0
  score_ua_edge: false
  validate: true
  force_no_vcf: true
  seed: $seed
  chart_workers_requested: $workers
  chart_workers_resolved: $workers
  chart_worker_policy: explicit
  refinement_exactness: EXACT
  cache_strategy: all_active_patterns
  effective_pattern_batch_size: 64
  final_compaction_exactness_kind: exact_optimal_production_union
  chain_per_accept_exactness_label: exact_multisite
  active_patterns: 64
  initial_grammar_clades: 23
  initial_grammar_productions: 11
  candidates_generated: 96
  candidates_scored: 96
  exact_verifications: 12
  iterations: 3
  accepted_moves: 3
  initial_score: 100
  final_score: 70
  inside_rows_recomputed_on_commit: 64
  outside_rows_recomputed_on_commit: 64
  accepted_rebuild_ms: 101.250
  iteration_reports:
    - iteration: 0
      candidate_generation:
        stop_reason: candidate_cap
    - iteration: 1
      candidate_generation:
        stop_reason: candidate_cap
    - iteration: 2
      candidate_generation:
        stop_reason: candidate_cap
EOF
}

characterization=$tmp/characterization/frozen.tsv
{
  printf '%s\n' \
    '# schema=wric_phase9_frozen_characterization' \
    '# schema_version=1' \
    "# primary_sha256=$fixture_sha" \
    "# frozen_oracle_sha256=$oracle_sha" \
    '# affinity_cpus=0' \
    '# timeout_seconds=600' \
    '# rss_limit_bytes=17179869184' \
    '# memory_budget_bytes=12884901888'
  python3 "$helper" print-characterization-template | tail -n 1
  for seed in 1 7 19; do
    for workers in 1 2 4 8; do
      stem=s${seed}-w${workers}
      report=evidence/$stem.report.txt
      sidecar=evidence/$stem.canonical.ndjson
      canonical=evidence/$stem.canonical.json
      output=evidence/$stem.output-canonical.json
      make_report "$seed" "$workers" "$tmp/characterization/$report"
      cat >"$tmp/characterization/$sidecar" <<EOF
{"record":"metadata"}
{"record":"contract","acceptance":"exact_multisite","objective":"grammar_exact","candidate_selection":"lower_bound_top_k","candidate_source":"grammar","topology_selection":"none","commit_mode":"overlay_delta","accepted_state_update":"overlay_chain_local_commit","verification_mode":"transient","max_iterations":3,"max_candidates":32,"top_k_exact":4,"seed":$seed,"polytomy_max_shapes":1,"refinement_exactness":"EXACT"}
{"record":"exact_evidence","keep_mask_kind":"exact_optimal_production_union"}
EOF
      search_sha=$(sha "$tmp/characterization/$sidecar")
      printf '{"schema":"larch.chart_spr.semantic_digest","schema_version":1,"digest_algorithm":"sha256","semantic_sha256":"%s"}\n' \
        "$search_sha" >"$tmp/characterization/$canonical"
      output_semantic=$(printf 'output-seed-%s\n' "$seed" | sha256sum | awk '{print $1}')
      printf '{"schema":"larch.dag.semantic_digest","schema_version":1,"digest_algorithm":"sha256","semantic_sha256":"%s","parsimony_min":70}\n' \
        "$output_semantic" >"$tmp/characterization/$output"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$seed" "$workers" "$report" "$(sha "$tmp/characterization/$report")" \
        "$sidecar" "$search_sha" "$canonical" "$(sha "$tmp/characterization/$canonical")" \
        "$output" "$(sha "$tmp/characterization/$output")"
    done
  done
} >"$characterization"
seal "$characterization"

build_supplement() {
  local output=$1; shift
  python3 "$helper" build \
    --repo-root "$repo_root" \
    --base-manifest "$base" \
    --expected-parent-sha256 "$parent_sha" \
    --characterization "$characterization" \
    --fixture "$tmp/fixture.pb.gz" \
    --expected-fixture-sha256 "$fixture_sha" \
    --output "$output" "$@"
}

supplement_a=$tmp/out-a/phase9-local-commit.tsv
supplement_b=$tmp/out-b/phase9-local-commit.tsv
build_supplement "$supplement_a"
build_supplement "$supplement_b"
cmp "$supplement_a" "$supplement_b"
cmp "$supplement_a.sha256" "$supplement_b.sha256"
diff -ru "$tmp/out-a/phase9-local-commit.assets" "$tmp/out-b/phase9-local-commit.assets"

python3 "$helper" audit --repo-root "$repo_root" --base-manifest "$base" \
  --expected-parent-sha256 "$parent_sha" --supplement "$supplement_a"
"$tmp/out-a/phase9-local-commit.assets/commands.sh" --verify-only >/dev/null
bash -n "$tmp/out-a/phase9-local-commit.assets/commands.sh"

python3 - "$supplement_a" <<'PY'
import csv
import hashlib
import pathlib
import sys

lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
data = [line for line in lines if not line.startswith("#")]
rows = list(csv.DictReader(data, dialect="excel-tab"))
assert len(rows) == 12
assert {(int(row["seed"]), int(row["requested_workers"])) for row in rows} == {
    (seed, workers) for seed in (1, 7, 19) for workers in (1, 2, 4, 8)
}
assert len({row["workload_name"] for row in rows}) == 3
for seed in (1, 7, 19):
    assert len({row["workload_name"] for row in rows if int(row["seed"]) == seed}) == 1
assert all(row["canonical_argv_sha256"] != row["oracle_trial_semantic_sha256"] for row in rows)

# Reproduce one digest independently of the helper.  Token order is part of
# the acceptance contract, not merely an implementation detail of the builder.
row = next(row for row in rows if row["seed"] == "1" and row["requested_workers"] == "1")
argv = [
    "@binary:working_chart", "--dag-pb", f"@primary:{row['primary_sha256']}",
    "--validate", "--force-no-vcf", "--wric-polytomy-mode", "expand-bounded",
    "--wric-polytomy-max-exact-arity", "6", "--wric-polytomy-max-shapes", "1",
    "--wric-polytomy-max-productions", "1024", "--wric-polytomy-max-clades", "256",
    "--wric-lazy-chart", "off", "--chart-spr-search", "--chart-spr-max-iterations", "3",
    "--chart-spr-max-candidates", "32", "--chart-spr-top-k-exact", "4",
    "--chart-spr-candidate-selection", "lower_bound_top_k",
    "--chart-spr-candidate-source", "grammar", "--chart-spr-acceptance", "exact_multisite",
    "--chart-spr-sampled-tree-count", "1", "--chart-spr-sampled-tree-radius", "0",
    "--chart-spr-max-upward-path-expansions", "0", "--chart-spr-max-path-pairs", "0",
    "--chart-spr-min-moved-clade-size", "1", "--chart-spr-max-moved-clade-size", "0",
    "--chart-spr-min-target-clade-size", "1", "--chart-spr-max-target-clade-size", "0",
    "--chart-spr-max-cached-patterns", "0", "--chart-spr-pattern-batch-size", "0",
    "--chart-spr-candidate-batch-size", "0", "--chart-spr-memory-budget", "12884901888",
    "--chart-spr-commit-mode", "overlay_delta", "--chart-spr-verification-mode", "transient",
    "--chart-bnb-dominance", "off", "--chart-spr-local-accept-updates", "--seed", "1",
    "--chart-spr-workers", "1", "-o", "@output",
]
payload = bytearray(b"wric-canonical-argv-v1\n" + f"argc={len(argv)}\n".encode())
for token in argv:
    encoded = token.encode()
    payload.extend(str(len(encoded)).encode() + b":" + encoded + b"\n")
argv_sha = hashlib.sha256(payload).hexdigest()
assert argv_sha == row["canonical_argv_sha256"]
trial = (
    "wric-trial-semantic-v1\n"
    f"method={row['method']}\n"
    f"search_semantic_sha256={row['oracle_search_semantic_sha256']}\n"
    f"output_semantic_sha256={row['oracle_output_semantic_sha256']}\n"
    f"canonical_argv_sha256={argv_sha}\n"
).encode()
assert hashlib.sha256(trial).hexdigest() == row["oracle_trial_semantic_sha256"]
PY

expect_fail wrong-parent python3 "$helper" build --repo-root "$repo_root" \
  --base-manifest "$base" --expected-parent-sha256 "$(printf wrong | sha256sum | awk '{print $1}')" \
  --characterization "$characterization" --fixture "$tmp/fixture.pb.gz" \
  --expected-fixture-sha256 "$fixture_sha" --output "$tmp/wrong-parent.tsv"
[[ ! -e "$tmp/wrong-parent.tsv" && ! -e "$tmp/wrong-parent.assets" ]]

bad_base=$tmp/base/schema-bad.tsv
sed 's/# schema_version=1/# schema_version=2/' "$base" >"$bad_base"
seal "$bad_base"
expect_fail schema-mismatch python3 "$helper" build --repo-root "$repo_root" \
  --base-manifest "$bad_base" --expected-parent-sha256 "$(sha "$bad_base")" \
  --characterization "$characterization" --fixture "$tmp/fixture.pb.gz" \
  --expected-fixture-sha256 "$fixture_sha" --output "$tmp/schema-bad.tsv"

bad_characterization=$tmp/characterization/hash-bad.tsv
cp "$characterization" "$bad_characterization"
python3 - "$bad_characterization" <<'PY'
import pathlib
import sys
path = pathlib.Path(sys.argv[1])
lines = path.read_text().splitlines()
fields = lines[8].split("\t")
row = lines[9].split("\t")
row[fields.index("product_report_sha256")] = "0" * 64
lines[9] = "\t".join(row)
path.write_text("\n".join(lines) + "\n")
PY
seal "$bad_characterization"
expect_fail evidence-hash python3 "$helper" build --repo-root "$repo_root" \
  --base-manifest "$base" --expected-parent-sha256 "$parent_sha" \
  --characterization "$bad_characterization" --fixture "$tmp/fixture.pb.gz" \
  --expected-fixture-sha256 "$fixture_sha" --output "$tmp/evidence-bad.tsv"

missing_characterization=$tmp/characterization/missing-row.tsv
head -n -1 "$characterization" >"$missing_characterization"
seal "$missing_characterization"
expect_fail matrix-cardinality python3 "$helper" build --repo-root "$repo_root" \
  --base-manifest "$base" --expected-parent-sha256 "$parent_sha" \
  --characterization "$missing_characterization" --fixture "$tmp/fixture.pb.gz" \
  --expected-fixture-sha256 "$fixture_sha" --output "$tmp/matrix-bad.tsv"

tampered=$tmp/out-b/phase9-local-commit.tsv
chmod u+w "$tampered" "$tampered.sha256"
python3 - "$tampered" <<'PY'
import pathlib
import sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = next(word for word in text.split("\t") if len(word) == 64 and set(word) <= set("0123456789abcdef"))
path.write_text(text.replace(old, "f" * 64, 1))
PY
seal "$tampered"
expect_fail tampered-supplement python3 "$helper" audit --repo-root "$repo_root" \
  --base-manifest "$base" --expected-parent-sha256 "$parent_sha" --supplement "$tampered"

echo "wric_phase9_manifest_bootstrap_test: PASS"

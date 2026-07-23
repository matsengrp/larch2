#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -x $1 ]]; then
  echo "usage: $0 <dagutil>" >&2
  exit 2
fi
dagutil=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

help=$($dagutil --help 2>&1 || true)
grep -q -- '--chart-spr-canonical-result <PATH>' <<<"$help"
grep -q -- '--chart-spr-canonical-sidecar <PATH>' <<<"$help"
grep -q -- '--canonical-dag-result <PATH>' <<<"$help"

search_controls=(
  --force-no-vcf
  --wric-polytomy-mode reject
  --chart-spr-search
  --chart-spr-max-candidates 2
  --chart-spr-top-k-exact 2
  --chart-spr-max-iterations 1
)
common=(
  --fasta test/wric_binary_four.fa
  --newick test/wric_binary_four.nwk
  --refseq test/wric_binary_four.ref
  "${search_controls[@]}"
)

run() {
  local workers=$1
  "$dagutil" "${common[@]}" \
    --chart-spr-workers "$workers" \
    --chart-spr-canonical-result "$tmp/search-$workers.json" \
    --chart-spr-canonical-sidecar "$tmp/search-$workers.ndjson" \
    --canonical-dag-result "$tmp/dag-$workers.json" \
    -o "$tmp/output-$workers.pb.gz" \
    >"$tmp/run-$workers.out" 2>"$tmp/run-$workers.err"
}

workers=(1 2 4 8 16 0)
for worker_count in "${workers[@]}"; do
  run "$worker_count"
done

# A proper single tree can use the lightweight physical normalization without
# changing any output bytes. Repeating the same input forces the general merge
# path and supplies the complete parity oracle.
grep -q '^single_input_merge_fast_path: true$' "$tmp/run-1.out"
"$dagutil" "${common[@]}" \
  --fasta test/wric_binary_four.fa \
  --newick test/wric_binary_four.nwk \
  --chart-spr-workers 1 \
  --chart-spr-canonical-result "$tmp/search-general.json" \
  --chart-spr-canonical-sidecar "$tmp/search-general.ndjson" \
  --canonical-dag-result "$tmp/dag-general.json" \
  -o "$tmp/output-general.pb.gz" \
  >"$tmp/run-general.out" 2>"$tmp/run-general.err"
grep -q '^single_input_merge_fast_path: false$' "$tmp/run-general.out"
cmp "$tmp/search-1.json" "$tmp/search-general.json"
cmp "$tmp/search-1.ndjson" "$tmp/search-general.ndjson"
cmp "$tmp/dag-1.json" "$tmp/dag-general.json"
cmp <(gzip -dc "$tmp/output-1.pb.gz") \
    <(gzip -dc "$tmp/output-general.pb.gz")

# Re-load a real DAG protobuf through the worker-aware compact-genome path.
# Its W1 and W4 searches must preserve the same full semantics and serialized
# output, and both must remain eligible for the one-input normalization.
for worker_count in 1 4; do
  "$dagutil" --dag-pb "$tmp/output-1.pb.gz" "${search_controls[@]}" \
    --chart-spr-workers "$worker_count" \
    --chart-spr-canonical-result "$tmp/pb-search-$worker_count.json" \
    --chart-spr-canonical-sidecar "$tmp/pb-search-$worker_count.ndjson" \
    --canonical-dag-result "$tmp/pb-dag-$worker_count.json" \
    -o "$tmp/pb-output-$worker_count.pb.gz" \
    >"$tmp/pb-run-$worker_count.out" \
    2>"$tmp/pb-run-$worker_count.err"
  grep -q '^single_input_merge_fast_path: true$' \
    "$tmp/pb-run-$worker_count.out"
done
cmp "$tmp/pb-search-1.json" "$tmp/pb-search-4.json"
cmp "$tmp/pb-search-1.ndjson" "$tmp/pb-search-4.ndjson"
cmp "$tmp/pb-dag-1.json" "$tmp/pb-dag-4.json"
cmp <(gzip -dc "$tmp/pb-output-1.pb.gz") \
    <(gzip -dc "$tmp/pb-output-4.pb.gz")

# Worker count, batching, timings, and memory/counter diagnostics are excluded
# from the semantic bytes.  The search and external-output oracles must match
# exactly across worker counts.
for worker_count in "${workers[@]:1}"; do
  cmp "$tmp/search-1.json" "$tmp/search-$worker_count.json"
  cmp "$tmp/search-1.ndjson" "$tmp/search-$worker_count.ndjson"
  cmp "$tmp/dag-1.json" "$tmp/dag-$worker_count.json"
done

grep -q '"schema":"larch.chart_spr.semantic_digest"' "$tmp/search-1.json"
grep -q '"schema":"larch.dag.semantic_digest"' "$tmp/dag-1.json"
grep -q '"parsimony_min":[0-9]' "$tmp/dag-1.json"
grep -q '"record":"candidate"' "$tmp/search-1.ndjson"
grep -q '"evidence_kind":"grammar_exact_frontier_provenance_companion"' \
  "$tmp/search-1.ndjson"
grep -q '"record":"exact_root_provenance_class"' \
  "$tmp/search-1.ndjson"

expected=$(sed -n \
  's/.*"semantic_sha256":"\([0-9a-f]\{64\}\)".*/\1/p' \
  "$tmp/search-1.json")
actual=$(sha256sum "$tmp/search-1.ndjson" | awk '{print $1}')
[[ $expected == "$actual" ]]

# A three-commit CLI run makes the per-iteration accepted-update evidence and
# pre-commit candidate-signature lifetime non-vacuous.  The tiny three-pattern
# source mirrors the committed Phase-9 performance fixture's topology while
# keeping this semantic/report contract test fast.
three_common=(
  --fasta test/wric_chart_three_accepts_tiny.fa
  --newick test/wric_chart_three_accepts_tiny.nwk
  --refseq test/wric_chart_three_accepts_tiny.ref
  --force-no-vcf
  --validate
  --wric-polytomy-mode reject
  --chart-spr-search
  --chart-spr-local-accept-updates
  --chart-spr-acceptance exact-multisite
  --chart-spr-candidate-selection lower-bound-top-k
  --chart-spr-max-candidates 32
  --chart-spr-top-k-exact 4
  --chart-spr-max-iterations 3
)

run_three() {
  local seed=$1
  local workers=$2
  "$dagutil" "${three_common[@]}" \
    --seed "$seed" \
    --chart-spr-workers "$workers" \
    --chart-spr-canonical-result "$tmp/three-$seed-$workers.json" \
    --chart-spr-canonical-sidecar "$tmp/three-$seed-$workers.ndjson" \
    >"$tmp/three-$seed-$workers.out" \
    2>"$tmp/three-$seed-$workers.err"
}

for seed in 1 7 19; do
  for worker_count in 1 2 4 8; do
    run_three "$seed" "$worker_count"
    [[ $(grep -Ec '^      accepted_candidate_signature: .+' \
          "$tmp/three-$seed-$worker_count.out") == 3 ]]
    [[ $(grep -Ec '^      accepted_inside_rows_recomputed: [1-9][0-9]*$' \
          "$tmp/three-$seed-$worker_count.out") == 3 ]]
    [[ $(grep -Ec '^      accepted_outside_rows_recomputed: [1-9][0-9]*$' \
          "$tmp/three-$seed-$worker_count.out") == 3 ]]
    [[ $(grep -c '"record":"iteration_outcome".*"accepted_move_committed":true.*"selected_signature":".' \
          "$tmp/three-$seed-$worker_count.ndjson") == 3 ]]

    iteration_inside=$(awk '/^      accepted_inside_rows_recomputed: / {sum += $2} END {print sum + 0}' \
      "$tmp/three-$seed-$worker_count.out")
    total_inside=$(awk '/^  inside_rows_recomputed_on_commit: / {print $2}' \
      "$tmp/three-$seed-$worker_count.out")
    [[ $iteration_inside == "$total_inside" ]]
    iteration_outside=$(awk '/^      accepted_outside_rows_recomputed: / {sum += $2} END {print sum + 0}' \
      "$tmp/three-$seed-$worker_count.out")
    total_outside=$(awk '/^  outside_rows_recomputed_on_commit: / {print $2}' \
      "$tmp/three-$seed-$worker_count.out")
    [[ $iteration_outside == "$total_outside" ]]

    grep '^      accepted_candidate_signature: ' \
      "$tmp/three-$seed-$worker_count.out" \
      >"$tmp/three-$seed-$worker_count.signatures"
    grep -E '^      accepted_(inside|outside)_rows_recomputed: ' \
      "$tmp/three-$seed-$worker_count.out" \
      >"$tmp/three-$seed-$worker_count.rows"
  done

  for worker_count in 2 4 8; do
    cmp "$tmp/three-$seed-1.json" "$tmp/three-$seed-$worker_count.json"
    cmp "$tmp/three-$seed-1.ndjson" "$tmp/three-$seed-$worker_count.ndjson"
    cmp "$tmp/three-$seed-1.signatures" \
      "$tmp/three-$seed-$worker_count.signatures"
    cmp "$tmp/three-$seed-1.rows" "$tmp/three-$seed-$worker_count.rows"
  done
done

# `--chart-bnb-score-only` still computes an exact scalar optimum, but
# intentionally has no exact keep mask or tied-production provenance.  Search
# capture must neither force that unavailable evidence nor change the search
# outcome/output DAG.
"$dagutil" "${common[@]}" \
  --chart-bnb-score-only \
  --canonical-dag-result "$tmp/score-only-off-dag.json" \
  -o "$tmp/score-only-off.pb.gz" \
  >"$tmp/score-only-off.out" 2>"$tmp/score-only-off.err"
"$dagutil" "${common[@]}" \
  --chart-bnb-score-only \
  --chart-spr-canonical-result "$tmp/score-only-on.json" \
  --chart-spr-canonical-sidecar "$tmp/score-only-on.ndjson" \
  --canonical-dag-result "$tmp/score-only-on-dag.json" \
  -o "$tmp/score-only-on.pb.gz" \
  >"$tmp/score-only-on.out" 2>"$tmp/score-only-on.err"
cmp "$tmp/score-only-off-dag.json" "$tmp/score-only-on-dag.json"
for key in acceptance objective candidates_generated candidates_scored \
           exact_verifications iterations accepted_moves initial_score \
           final_score; do
  off=$(awk -v prefix="  $key: " 'index($0,prefix)==1 {print; n++} END {if(n!=1) exit 1}' \
    "$tmp/score-only-off.out")
  on=$(awk -v prefix="  $key: " 'index($0,prefix)==1 {print; n++} END {if(n!=1) exit 1}' \
    "$tmp/score-only-on.out")
  [[ $off == "$on" ]]
done
grep -q '"evidence_kind":"grammar_exact_score_only_frontier_statistics"' \
  "$tmp/score-only-on.ndjson"
grep -q '"keep_mask_kind":"score_only_not_exact"' \
  "$tmp/score-only-on.ndjson"
grep -q '"keep_production_exact":false' "$tmp/score-only-on.ndjson"
if grep -q '"record":"exact_root_provenance_class"' \
    "$tmp/score-only-on.ndjson"; then
  echo "score-only canonical sidecar unexpectedly contains tied provenance" >&2
  exit 1
fi

# A sidecar without its compact digest is rejected rather than producing an
# unanchored semantic artifact.
if "$dagutil" "${common[@]}" \
    --chart-spr-canonical-sidecar "$tmp/unanchored.ndjson" \
    >"$tmp/unanchored.out" 2>"$tmp/unanchored.err"; then
  echo "canonical sidecar unexpectedly accepted without compact result" >&2
  exit 1
fi
grep -q -- '--chart-spr-canonical-sidecar requires' "$tmp/unanchored.err"

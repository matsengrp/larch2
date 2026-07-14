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

common=(
  --fasta test/wric_binary_four.fa
  --newick test/wric_binary_four.nwk
  --refseq test/wric_binary_four.ref
  --force-no-vcf
  --wric-polytomy-mode reject
  --chart-spr-search
  --chart-spr-max-candidates 2
  --chart-spr-top-k-exact 2
  --chart-spr-max-iterations 1
)

run() {
  local workers=$1
  "$dagutil" "${common[@]}" \
    --chart-spr-workers "$workers" \
    --chart-spr-canonical-result "$tmp/search-$workers.json" \
    --chart-spr-canonical-sidecar "$tmp/search-$workers.ndjson" \
    --canonical-dag-result "$tmp/dag-$workers.json" \
    >"$tmp/run-$workers.out" 2>"$tmp/run-$workers.err"
}

run 1
run 2

# Worker count, batching, timings, and memory/counter diagnostics are excluded
# from the semantic bytes.  The search and external-output oracles must match
# exactly across worker counts.
cmp "$tmp/search-1.json" "$tmp/search-2.json"
cmp "$tmp/search-1.ndjson" "$tmp/search-2.ndjson"
cmp "$tmp/dag-1.json" "$tmp/dag-2.json"

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

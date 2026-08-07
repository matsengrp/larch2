#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 || ! -x $1 || ! -f $2 || ! -f $3 ||
      ($5 != 1 && $5 != 8) ]]; then
  echo "usage: $0 <dagutil> <provider> <manifest> <output-directory> <1|8>" >&2
  exit 2
fi

dagutil=$1
provider=$2
manifest=$3
destination=$4
workers=$5
staging=${destination}.staging
reference=data/seedtree/refseq.txt.gz
seed=data/seedtree/seedtree.pb.gz

fail() {
  echo "TI-2 W${workers} census replay: $*" >&2
  exit 1
}

sha() {
  cmake -E sha256sum "$1" | awk '{print $1}'
}

manifest_value() {
  local key=$1
  awk -F '\t' -v key="$key" '$1 == key { print $2; found = 1 }
    END { if (!found) exit 1 }' "$manifest"
}

[[ ! -e $destination ]] || fail "destination already exists"
[[ ! -e $staging ]] || fail "stale staging directory exists"
mkdir -p "$staging"

ledger=$staging/ordinal-scores.tsv
minimum_prefix=$staging/minimum
stdout=$staging/census.stdout
stderr=$staging/census.stderr

"$dagutil" \
  --dag-pb "$provider" \
  --refseq "$reference" \
  --force-no-vcf --validate \
  --wric-polytomy-mode allow \
  --chart-score-ua-edge \
  --chart-kary-census \
  --chart-kary-census-workers "$workers" \
  --chart-kary-census-sankoff-stride 10000 \
  --chart-kary-census-output-prefix "$minimum_prefix" \
  --chart-kary-census-replay-manifest "$manifest" \
  --chart-kary-census-seed-input "$seed" \
  --chart-kary-census-ledger-output "$ledger" \
  >"$stdout" 2>"$stderr"

for expected in \
  "  workers: $workers" \
  "  grammar_audit_topology_count: 575168" \
  "  enumerator_topology_count: 575168" \
  "  scored_topology_count: 575168" \
  "  counts_reconcile: true" \
  "  sankoff_verification_stride: 10000" \
  "  sankoff_verified_topology_count: 58" \
  "  optimum: 1616" \
  "  optimal_topology_count: 12" \
  "  distinct_minimum_class_count: 12"; do
  grep -Fxq "$expected" "$stdout" || fail "missing report field: $expected"
done

[[ $(wc -l <"$ledger") -eq 575169 ]] ||
  fail "ordinal/score ledger row count mismatch"
minimum_ledger=$minimum_prefix.minimum-witnesses.tsv
[[ -f $minimum_ledger && $(wc -l <"$minimum_ledger") -eq 13 ]] ||
  fail "minimum witness ledger row count mismatch"
mapfile -t witnesses < <(find "$staging" -maxdepth 1 -type f \
  -name 'minimum.class-*.pb.gz' -print | sort)
[[ ${#witnesses[@]} -eq 12 ]] || fail "minimum witness artifact count mismatch"

manifest_sha=$(sha "$manifest")
ledger_sha=$(sha "$ledger")
minimum_ledger_sha=$(sha "$minimum_ledger")
cat >"$staging/census-semantics.tsv" <<EOF
key	value
schema_name	larch-topology-islands-ti2-census-semantics-v1
replay_manifest_sha256	$manifest_sha
provider_input_sha256	$(manifest_value provider_input_sha256)
provider_legacy_dag_semantic_sha256	$(manifest_value provider_legacy_dag_semantic_sha256)
provider_stored_history_parsimony_min	$(manifest_value provider_stored_history_parsimony_min)
grammar_digest	$(manifest_value grammar_digest)
grammar_semantic_digest	$(manifest_value grammar_semantic_digest)
grammar_topology_count	575168
alignment_sha256	$(manifest_value alignment_sha256)
site_pattern_digest	$(manifest_value site_pattern_digest)
input_content_sha256	$(manifest_value input_content_sha256)
reference_sha256	$(manifest_value reference_sha256)
taxon_labels_sha256	$(manifest_value taxon_labels_sha256)
parsimony_model	$(manifest_value parsimony_model)
ua_scoring	$(manifest_value ua_scoring)
score_baseline	1616
sankoff_verification_stride	10000
sankoff_verified_topology_count	58
ordinal_score_ledger_sha256	$ledger_sha
minimum_witness_ledger_sha256	$minimum_ledger_sha
EOF

source_state=$staging/producer-source-state.tsv
echo $'path\tsha256' >"$source_state"
for path in \
  CMakeLists.txt \
  include/larch/grammar_topology_census.hpp \
  include/larch/grammar_topology_band.hpp \
  include/larch/grammar_topology_band_export.hpp \
  include/larch/grammar_topology_replay.hpp \
  include/larch/tree_pattern_sankoff.hpp \
  src/grammar_topology_band.cpp \
  src/grammar_topology_band_export.cpp \
  src/grammar_topology_replay.cpp \
  src/tree_pattern_sankoff.cpp \
  tools/dagutil.cpp \
  test/topology_islands_ti2_provider_replay.sh \
  test/topology_islands_ti2_census_replay.sh \
  doc/WRIC-TOPOLOGY-SCORE-BAND-REPLAY-V1.tsv; do
  [[ -f $path ]] || fail "source-state member is absent: $path"
  printf '%s\t%s\n' "$path" "$(sha "$path")" >>"$source_state"
done

commit=$(git rev-parse HEAD)
dirty=false
if [[ -n $(git status --porcelain=v1 --untracked-files=all) ]]; then
  dirty=true
fi
cat >"$staging/run-provenance.tsv" <<EOF
key	value
schema_name	larch-topology-islands-ti2-census-run-v1
worker_count	$workers
invocation_schema	test/topology_islands_ti2_census_replay.sh:v1
producer_commit	$commit
producer_dirty	$dirty
producer_dagutil_sha256	$(sha "$dagutil")
producer_source_state_sha256	$(sha "$source_state")
producer_toolchain	gcc-trunk-17-experimental
census_stdout_sha256	$(sha "$stdout")
census_stderr_sha256	$(sha "$stderr")
EOF

files=$staging/files.tsv
echo $'path\tsha256' >"$files"
while IFS= read -r path; do
  relative=${path#"$staging/"}
  printf '%s\t%s\n' "$relative" "$(sha "$path")" >>"$files"
done < <(find "$staging" -maxdepth 1 -type f \
  ! -name files.tsv ! -name SHA256SUMS -print | sort)
awk -F '\t' 'NR > 1 { print $2 "  " $1 }' "$files" >"$staging/SHA256SUMS"

mv "$staging" "$destination"
echo "TI-2 W${workers} census replay: PASS"

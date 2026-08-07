#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 9 || ! -x $1 || ! -x $2 || ! -f $3 || ! -f $4 ||
      ($6 != 1 && $6 != 8) || ($7 != 1 && $7 != 2) || ! -x $8 ||
      ! -f $9 ]]; then
  echo "usage: $0 <dagutil> <larch-validator> <provider> <manifest> <output-directory> <1|8> <1|2> <compiler> <publisher>" >&2
  exit 2
fi

dagutil=$1
validator=$2
provider=$3
manifest=$4
destination=$5
workers=$6
delta=$7
compiler=$8
publisher=$9
staging=${destination}.staging
reference=data/seedtree/refseq.txt.gz
seed=data/seedtree/seedtree.pb.gz
expected_ledger=7fc216738daf2d9008bdb89b94adc6fbe947f5d8fb92957f5aeeaef0dec75986
expected_minima=4dd773e3a5a132c2e45aed4bce05d05003039164728d08c300d888e260728562
expected_recovery_wrapper=b4fb0643e7af1a9fd29710fe6a89666d6d64b6127e80fb2781122d144be9eb37

if [[ $delta == 1 ]]; then
  expected_selected=176
  expected_maximum=1617
else
  expected_selected=1076
  expected_maximum=1618
fi

fail() {
  echo "TI-2 delta-${delta} W${workers} raw export: $*" >&2
  exit 1
}

sha() {
  cmake -E sha256sum "$1" | awk '{print $1}'
}

kv() {
  local file=$1
  local key=$2
  awk -F '\t' -v key="$key" '$1 == key { print $2; found = 1 }
    END { if (!found) exit 1 }' "$file"
}

source_value() {
  local file=$1
  local kind=$2
  local name=$3
  awk -F '\t' -v kind="$kind" -v name="$name" \
    '$1 == kind && $2 == name { print $3; found = 1 }
     END { if (!found) exit 1 }' "$file"
}

[[ ! -e $destination && ! -L $destination ]] ||
  fail "destination already exists"

dirty=false
if [[ -n $(git status --porcelain=v1 --untracked-files=all) ]]; then
  dirty=true
fi
head_commit=$(git rev-parse HEAD)
compiled_commit=$(git rev-parse --short HEAD)
compiler_version=$(printf '%s\n' '__VERSION__' | \
  "$compiler" -E -P -x c++ - | sed -e 's/^"//' -e 's/"$//')
[[ -n $compiler_version ]] || fail "could not derive compiler __VERSION__"
compiler_version_cell=${compiler_version//%/%25}
compiler_version_cell=${compiler_version_cell// /%20}
source_state=$staging/producer-source-state.tsv
ledger=$staging/ordinal-scores.tsv
minimum_prefix=$staging/minimum
raw=$staging/raw
stdout=$staging/export.stdout
stderr=$staging/export.stderr
producer_wrapper=$staging/producer-wrapper-source.sh
finalizer_wrapper=$staging/finalizer-wrapper-source.sh
execution_mode=fresh_same_pass
recovery_reason=not_applicable

if [[ -e $staging || -L $staging ]]; then
  [[ -d $staging && ! -L $staging && -f $source_state && -f $ledger &&
     -f $minimum_prefix.minimum-witnesses.tsv && -d $raw &&
     -f $stdout && -f $stderr && -f $producer_wrapper ]] ||
    fail "stale staging path is not a resumable completed raw export"
  execution_mode=validated_resume_after_toolchain_cell_encoding_mismatch
  recovery_reason=raw_tsv_percent_encoded_toolchain_compared_to_decoded_version
  [[ $workers == 1 && $delta == 1 ]]
  [[ $(source_value "$source_state" file \
       test/topology_islands_ti2_band_export.sh) == \
     "$expected_recovery_wrapper" ]] ||
    fail "staging producer wrapper is not the reviewed recovery version"
  [[ $(source_value "$source_state" identity git_commit) == "$head_commit" ]]
  [[ $(source_value "$source_state" identity git_dirty) == "$dirty" ]]
  [[ $(source_value "$source_state" identity dagutil_sha256) == \
     "$(sha "$dagutil")" ]]
  [[ $(source_value "$source_state" identity compiled_commit_expected) == \
     "$compiled_commit" ]]
  [[ $(source_value "$source_state" identity compiler_path) == \
     "$(realpath "$compiler")" ]]
  [[ $(source_value "$source_state" identity compiler_version) == \
     "$compiler_version" ]]
  while IFS=$'\t' read -r kind path expected; do
    [[ $kind == file ]] || continue
    candidate=$path
    if [[ $path == test/topology_islands_ti2_band_export.sh ]]; then
      candidate=$producer_wrapper
    fi
    [[ -f $candidate && $(sha "$candidate") == "$expected" ]] ||
      fail "resumed source-state member differs: $path"
  done <"$source_state"
else
  mkdir -p "$staging"
  cat >"$source_state" <<EOF
kind	name	value
identity	git_commit	$head_commit
identity	git_dirty	$dirty
identity	dagutil_sha256	$(sha "$dagutil")
identity	compiled_commit_expected	$compiled_commit
identity	compiler_path	$(realpath "$compiler")
identity	compiler_version	$compiler_version
EOF
  for path in \
    CMakeLists.txt \
    include/larch/exhaustive_rooted_rspr.hpp \
    include/larch/grammar_topology_census.hpp \
    include/larch/grammar_topology_band.hpp \
    include/larch/grammar_topology_band_export.hpp \
    include/larch/grammar_topology_replay.hpp \
    include/larch/phylo_topology_adapter.hpp \
    include/larch/rooted_topology.hpp \
    include/larch/topology_landscape_bundle.hpp \
    include/larch/tree_pattern_sankoff.hpp \
    src/exhaustive_rooted_rspr.cpp \
    src/grammar_topology_band.cpp \
    src/grammar_topology_band_export.cpp \
    src/grammar_topology_replay.cpp \
    src/phylo_topology_adapter.cpp \
    src/rooted_topology.cpp \
    src/topology_landscape_bundle.cpp \
    src/tree_pattern_sankoff.cpp \
    tools/dagutil.cpp \
    test/publish_directory_no_replace.py \
    test/topology_islands_ti2_band_export.sh \
    doc/WRIC-TOPOLOGY-SCORE-BAND-REPLAY-V1.tsv \
    doc/WRIC-TOPOLOGY-ISLANDS-TI2-GATE-C.tsv; do
    [[ -f $path ]] || fail "source-state member is absent: $path"
    printf 'file\t%s\t%s\n' "$path" "$(sha "$path")" >>"$source_state"
  done
  cmake -E copy "$0" "$producer_wrapper"

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
    --chart-kary-census-export-max-delta "$delta" \
    --chart-kary-census-export-directory "$raw" \
    --chart-kary-census-producer-dirty-attestation "$dirty" \
    --chart-kary-census-producer-source-state "$source_state" \
    >"$stdout" 2>"$stderr"
fi

source_state_sha=$(sha "$source_state")
producer_wrapper_sha=$(source_value "$source_state" file \
  test/topology_islands_ti2_band_export.sh)

"$validator" validate-score-band-raw "$raw"
[[ $(sha "$ledger") == "$expected_ledger" ]] ||
  fail "complete ordinal/score ledger differs from Gate C"
minimum_ledger=$minimum_prefix.minimum-witnesses.tsv
[[ $(sha "$minimum_ledger") == "$expected_minima" ]] ||
  fail "minimum witness ledger differs from Gate C"
minimum_artifact_count=0
while IFS=$'\t' read -r class ordinal exact_score topology_sha \
    dag_semantic dag_clades dag_productions artifact artifact_sha; do
  [[ $class == class ]] && continue
  [[ $artifact == minimum.class-*.pb.gz && $artifact != */* &&
     -f $staging/$artifact && $(sha "$staging/$artifact") == "$artifact_sha" ]] ||
    fail "minimum witness artifact is absent or differs: $artifact"
  minimum_artifact_count=$((minimum_artifact_count + 1))
done <"$minimum_ledger"
[[ $minimum_artifact_count == 12 ]] ||
  fail "minimum witness artifact count differs from Gate C"
cmp <(tail -n +2 "$ledger") <(tail -n +2 "$raw/score_census.tsv") ||
  fail "raw score census differs from accepted complete ledger"
[[ $(wc -l <"$raw/selected_topologies.tsv") -eq $((expected_selected + 1)) ]] ||
  fail "selected topology row count mismatch"
expected_view=absolute_score_interval_inclusive_v1:1616:$expected_maximum
[[ $(kv "$raw/semantics.tsv" score_view) == "$expected_view" ]] ||
  fail "raw score view mismatch"
[[ $(kv "$raw/provenance.tsv" producer_dirty) == "$dirty" ]] ||
  fail "raw producer dirty-state mismatch"
[[ $(kv "$raw/provenance.tsv" producer_commit) == "$compiled_commit" ]] ||
  fail "raw compiled commit differs from source-state commit"
[[ $(kv "$raw/provenance.tsv" toolchain) == "$compiler_version_cell" ]] ||
  fail "raw compiled toolchain differs from source-state compiler"
[[ $(kv "$raw/provenance.tsv" worker_count) == "$workers" ]] ||
  fail "raw worker-count mismatch"
derivation=$(kv "$raw/provenance.tsv" derivation_ref)
[[ $derivation == *"producer-source-state-sha256:$source_state_sha"* ]] ||
  fail "raw provenance does not bind source state"

[[ ! -e $finalizer_wrapper && ! -L $finalizer_wrapper ]] ||
  fail "finalizer wrapper archive already exists"
cmake -E copy "$0" "$finalizer_wrapper"
finalizer_wrapper_sha=$(sha "$finalizer_wrapper")
[[ $finalizer_wrapper_sha == "$(sha "$0")" ]] ||
  fail "finalizer wrapper archive differs from invoked wrapper"

cat >"$staging/EXPORT-CERTIFICATE.tsv" <<EOF
key	value
schema_name	larch-topology-islands-ti2-band-export-v1
execution_mode	$execution_mode
recovery_reason	$recovery_reason
score_delta	$delta
worker_count	$workers
selected_topology_count	$expected_selected
ordinal_score_ledger_sha256	$expected_ledger
minimum_witness_ledger_sha256	$expected_minima
producer_source_state_sha256	$source_state_sha
producer_wrapper_source_sha256	$producer_wrapper_sha
finalizer_wrapper_source_sha256	$finalizer_wrapper_sha
raw_semantic_data_sha256	$(kv "$raw/manifest.tsv" semantic_data_sha256)
raw_semantics_hash	$(kv "$raw/manifest.tsv" semantics_hash)
raw_provenance_id	$(kv "$raw/manifest.tsv" provenance_id)
raw_sha256sums_sha256	$(sha "$raw/SHA256SUMS")
raw_validation	passed
EOF

files=$staging/RUN-FILES.tsv
echo $'path\tsha256' >"$files"
while IFS= read -r path; do
  relative=${path#"$staging/"}
  printf '%s\t%s\n' "$relative" "$(sha "$path")" >>"$files"
done < <(find "$staging" -type f ! -name RUN-FILES.tsv \
  ! -name RUN-SHA256SUMS -print | sort)
awk -F '\t' 'NR > 1 { print $2 "  " $1 }' "$files" \
  >"$staging/RUN-SHA256SUMS"

python3.14 "$publisher" "$staging" "$destination"
echo "TI-2 delta-${delta} W${workers} raw export: PASS"

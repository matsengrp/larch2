#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 || ! -d $1/raw || ! -d $2/raw ]]; then
  echo "usage: $0 <w1-export-root> <w8-export-root> <certificate>" >&2
  exit 2
fi

w1=$1
w8=$2
certificate=$3

for member in ordinal-scores.tsv minimum.minimum-witnesses.tsv \
              raw/semantics.tsv \
              raw/score_census.tsv raw/selected_topologies.tsv; do
  cmp "$w1/$member" "$w8/$member"
done

value() {
  awk -F '\t' -v key="$2" '$1 == key { print $2; found = 1 }
    END { if (!found) exit 1 }' "$1"
}

source_without_wrapper() {
  awk -F '\t' '$1 != "file" || $2 != "test/topology_islands_ti2_band_export.sh"' \
    "$1"
}

w1_source=$(cmake -E sha256sum "$w1/producer-source-state.tsv" | awk '{print $1}')
w8_source=$(cmake -E sha256sum "$w8/producer-source-state.tsv" | awk '{print $1}')
w1_wrapper=$(awk -F '\t' '$1 == "file" && \
  $2 == "test/topology_islands_ti2_band_export.sh" {print $3}' \
  "$w1/producer-source-state.tsv")
w8_wrapper=$(awk -F '\t' '$1 == "file" && \
  $2 == "test/topology_islands_ti2_band_export.sh" {print $3}' \
  "$w8/producer-source-state.tsv")
w1_mode=$(value "$w1/EXPORT-CERTIFICATE.tsv" execution_mode)
w8_mode=$(value "$w8/EXPORT-CERTIFICATE.tsv" execution_mode)
w1_recovery=$(value "$w1/EXPORT-CERTIFICATE.tsv" recovery_reason)
w8_recovery=$(value "$w8/EXPORT-CERTIFICATE.tsv" recovery_reason)
score_delta=$(value "$w1/EXPORT-CERTIFICATE.tsv" score_delta)
[[ $(value "$w8/EXPORT-CERTIFICATE.tsv" score_delta) == "$score_delta" &&
   ($score_delta == 1 || $score_delta == 2) ]]
source_equal=false
source_equal_except_recovery=false
if [[ $w1_mode == fresh_same_pass && $w8_mode == fresh_same_pass ]]; then
  cmp "$w1/producer-source-state.tsv" "$w8/producer-source-state.tsv"
  [[ $w1_wrapper == "$w8_wrapper" && $w1_recovery == not_applicable &&
     $w8_recovery == not_applicable ]]
  source_equal=true
elif [[ $score_delta == 1 &&
        $w1_mode == validated_resume_after_toolchain_cell_encoding_mismatch &&
        $w8_mode == fresh_same_pass ]]; then
  cmp <(source_without_wrapper "$w1/producer-source-state.tsv") \
      <(source_without_wrapper "$w8/producer-source-state.tsv")
  [[ $w1_wrapper != "$w8_wrapper" &&
     $w1_recovery == raw_tsv_percent_encoded_toolchain_compared_to_decoded_version &&
     $w8_recovery == not_applicable ]]
  source_equal_except_recovery=true
else
  echo "unsupported W1/W8 execution-mode pairing" >&2
  exit 1
fi
[[ $(value "$w1/EXPORT-CERTIFICATE.tsv" producer_wrapper_source_sha256) == \
   "$w1_wrapper" ]]
[[ $(value "$w8/EXPORT-CERTIFICATE.tsv" producer_wrapper_source_sha256) == \
   "$w8_wrapper" ]]
w1_finalizer=$(value "$w1/EXPORT-CERTIFICATE.tsv" \
  finalizer_wrapper_source_sha256)
w8_finalizer=$(value "$w8/EXPORT-CERTIFICATE.tsv" \
  finalizer_wrapper_source_sha256)
[[ $(cmake -E sha256sum "$w1/producer-wrapper-source.sh" | awk '{print $1}') == \
   "$w1_wrapper" ]]
[[ $(cmake -E sha256sum "$w8/producer-wrapper-source.sh" | awk '{print $1}') == \
   "$w8_wrapper" ]]
[[ $(cmake -E sha256sum "$w1/finalizer-wrapper-source.sh" | awk '{print $1}') == \
   "$w1_finalizer" ]]
[[ $(cmake -E sha256sum "$w8/finalizer-wrapper-source.sh" | awk '{print $1}') == \
   "$w8_finalizer" ]]
if [[ $source_equal == true ]]; then
  [[ $w1_finalizer == "$w1_wrapper" && $w8_finalizer == "$w8_wrapper" ]]
else
  [[ $w1_finalizer == "$w8_wrapper" && $w8_finalizer == "$w8_wrapper" ]]
fi

w1_semantic=$(value "$w1/raw/manifest.tsv" semantic_data_sha256)
w8_semantic=$(value "$w8/raw/manifest.tsv" semantic_data_sha256)
[[ $w1_semantic == "$w8_semantic" ]]
[[ $(value "$w1/raw/provenance.tsv" worker_count) == 1 ]]
[[ $(value "$w8/raw/provenance.tsv" worker_count) == 8 ]]
w1_provenance=$(value "$w1/raw/manifest.tsv" provenance_id)
w8_provenance=$(value "$w8/raw/manifest.tsv" provenance_id)
[[ $w1_provenance != "$w8_provenance" ]]

staging=${certificate}.staging
[[ ! -e $certificate && ! -e $staging ]]
mkdir -p "$(dirname "$certificate")"
cat >"$staging" <<EOF
key	value
schema_name	larch-topology-islands-ti2-raw-parity-v1
score_delta	$score_delta
w1_w8_ordinal_score_ledger_equal	true
ordinal_score_ledger_sha256	$(cmake -E sha256sum \
  "$w1/ordinal-scores.tsv" | awk '{print $1}')
w1_w8_minimum_witness_ledger_equal	true
minimum_witness_ledger_sha256	$(cmake -E sha256sum \
  "$w1/minimum.minimum-witnesses.tsv" | awk '{print $1}')
w1_w8_producer_source_state_equal	$source_equal
w1_w8_producer_source_state_equal_except_certified_wrapper_fix	$source_equal_except_recovery
w1_producer_source_state_sha256	$w1_source
w8_producer_source_state_sha256	$w8_source
w1_producer_wrapper_source_sha256	$w1_wrapper
w8_producer_wrapper_source_sha256	$w8_wrapper
w1_finalizer_wrapper_source_sha256	$w1_finalizer
w8_finalizer_wrapper_source_sha256	$w8_finalizer
w1_execution_mode	$w1_mode
w8_execution_mode	$w8_mode
w1_w8_raw_semantic_members_equal	true
raw_semantic_data_sha256	$w1_semantic
w1_raw_provenance_id	$w1_provenance
w8_raw_provenance_id	$w8_provenance
raw_provenance_distinct	true
EOF
ln "$staging" "$certificate"
rm "$staging"
echo "TI-2 delta-${score_delta} raw W1/W8 parity: PASS"

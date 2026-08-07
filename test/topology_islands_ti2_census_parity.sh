#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 || ! -d $1 || ! -d $2 ]]; then
  echo "usage: $0 <w1-directory> <w8-directory> <certificate>" >&2
  exit 2
fi

w1=$1
w8=$2
certificate=$3

for member in ordinal-scores.tsv minimum.minimum-witnesses.tsv \
              census-semantics.tsv producer-source-state.tsv; do
  cmp "$w1/$member" "$w8/$member"
done

for key in producer_commit producer_dirty producer_dagutil_sha256 \
           producer_source_state_sha256 producer_toolchain; do
  w1_value=$(awk -F '\t' -v key="$key" '$1 == key { print $2 }' \
    "$w1/run-provenance.tsv")
  w8_value=$(awk -F '\t' -v key="$key" '$1 == key { print $2 }' \
    "$w8/run-provenance.tsv")
  [[ -n $w1_value && $w1_value == "$w8_value" ]]
done

ledger_sha=$(cmake -E sha256sum "$w1/ordinal-scores.tsv" | awk '{print $1}')
minimum_sha=$(cmake -E sha256sum "$w1/minimum.minimum-witnesses.tsv" |
  awk '{print $1}')
semantics_sha=$(cmake -E sha256sum "$w1/census-semantics.tsv" |
  awk '{print $1}')
staging=${certificate}.staging
[[ ! -e $certificate && ! -e $staging ]]
mkdir -p "$(dirname "$certificate")"
cat >"$staging" <<EOF
key	value
schema_name	larch-topology-islands-ti2-census-parity-v1
w1_w8_ordinal_score_ledger_equal	true
ordinal_score_ledger_sha256	$ledger_sha
w1_w8_minimum_witness_ledger_equal	true
minimum_witness_ledger_sha256	$minimum_sha
w1_w8_census_semantics_equal	true
census_semantics_sha256	$semantics_sha
w1_w8_producer_source_state_equal	true
producer_source_state_sha256	$(cmake -E sha256sum \
  "$w1/producer-source-state.tsv" | awk '{print $1}')
EOF
ln "$staging" "$certificate"
rm "$staging"
echo "TI-2 W1/W8 census parity: PASS"

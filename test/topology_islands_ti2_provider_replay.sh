#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 || ! -x $1 || ! -f $3 ]]; then
  echo "usage: $0 <dagutil> <provider-directory> <replay-manifest>" >&2
  exit 2
fi

dagutil=$1
directory=$2
manifest=$3
seed=data/seedtree/seedtree.pb.gz
reference=data/seedtree/refseq.txt.gz
provider=$directory/provider.pb.gz
expected_seed=2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153
expected_reference=088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c
expected_provider=ae9c0c1b746439e3544eb2e8cef877c404e019d493d173a922717b1772a2b25b
expected_semantic=b39fbf5d36614de8bc0a931cd21144f7c6c78208e9207d2487da0ee1020a72cd
expected_manifest=f92f59c403cf5d2bed7fd4f95a320de167065a7905e47993821938ce27485572

fail() {
  echo "TI-2 provider replay: $*" >&2
  exit 1
}

sha() {
  cmake -E sha256sum "$1" | awk '{print $1}'
}

[[ $(sha "$seed") == "$expected_seed" ]] || fail "seed input hash drift"
[[ $(sha "$reference") == "$expected_reference" ]] ||
  fail "reference input hash drift"
[[ $(sha "$manifest") == "$expected_manifest" ]] ||
  fail "replay manifest hash drift"
mkdir -p "$directory"

disposition=verified_existing

if [[ -e $provider ]]; then
  [[ -f $provider ]] || fail "provider destination is not a regular file"
  [[ $(sha "$provider") == "$expected_provider" ]] ||
    fail "existing provider has the wrong hash"
else
  disposition=regenerated_this_build
  staging=$directory/.provider.pb.gz.staging
  [[ ! -e $staging ]] || fail "stale provider staging file exists"
  "$dagutil" \
    --tree-pb "$seed" \
    --refseq "$reference" \
    --force-no-vcf --validate \
    --wric-polytomy-mode allow \
    --chart-score-ua-edge \
    --chart-spr-search \
    --chart-spr-additive-batch-union \
    --chart-spr-acceptance fixed-topology-exact \
    --chart-spr-candidate-source sampled-tree \
    --chart-spr-additive-batch-max-moves 50 \
    --chart-spr-sampled-tree-score-threshold -1 \
    --chart-spr-max-iterations 1 \
    --chart-spr-workers 8 \
    --seed 1 \
    -o "$staging" \
    >"$directory/provider-search.stdout" \
    2>"$directory/provider-search.stderr"
  [[ -s $staging ]] || fail "provider search produced no artifact"
  [[ $(sha "$staging") == "$expected_provider" ]] ||
    fail "regenerated provider raw hash drift"
  ln "$staging" "$provider" || fail "no-replace provider publication failed"
  rm "$staging"
fi
if [[ -s $directory/provider-search.stdout &&
      -s $directory/provider-search.stderr ]]; then
  disposition=regenerated_this_build
fi

canonical=$directory/provider-canonical.json
stdout=$directory/provider-validation.stdout
stderr=$directory/provider-validation.stderr
rm -f "$canonical" "$stdout" "$stderr"
"$dagutil" \
  --dag-pb "$provider" \
  --force-no-vcf --validate \
  --canonical-dag-result "$canonical" \
  >"$stdout" 2>"$stderr"
[[ -s $canonical ]] || fail "canonical provider report is absent"
grep -Fq "\"semantic_sha256\":\"$expected_semantic\"" "$canonical" ||
  fail "provider semantic digest drift"
grep -Fq '"parsimony_min":1620' "$canonical" ||
  fail "provider stored-history minimum drift"

identity=$directory/identity.tsv
identity_stdout=$directory/provider-identity.stdout
identity_stderr=$directory/provider-identity.stderr
write_identity() {
  local output=$1
  local stdout_path=$2
  local stderr_path=$3
  "$dagutil" \
    --dag-pb "$provider" \
    --refseq "$reference" \
    --force-no-vcf --validate \
    --wric-polytomy-mode allow \
    --chart-score-ua-edge \
    --chart-kary-census-seed-input "$seed" \
    --chart-kary-census-grammar-construction \
      larch_collapsed_clade_direct_kary_allow_v1 \
    --chart-kary-census-universe \
      wric_597_taxon_full_direct_collapsed_grammar_v1 \
    --chart-kary-census-identity-report "$output" \
    >"$stdout_path" 2>"$stderr_path"
}
if [[ ! -e $identity ]]; then
  write_identity "$identity" "$identity_stdout" "$identity_stderr"
else
  current_identity=$directory/.identity.current.tsv
  current_stdout=$directory/.identity.current.stdout
  current_stderr=$directory/.identity.current.stderr
  [[ ! -e $current_identity && ! -e $current_stdout &&
     ! -e $current_stderr ]] || fail "stale current-identity staging files"
  write_identity "$current_identity" "$current_stdout" "$current_stderr"
  cmp -s "$current_identity" "$identity" ||
    fail "current validator normalized identity differs from certificate input"
  rm "$current_identity" "$current_stdout" "$current_stderr"
fi
[[ -s $identity ]] || fail "normalized identity report is absent"
awk -F '\t' '
  FNR == NR {
    if (FNR > 1) manifest[$1] = $2
    next
  }
  FNR == 1 {
    if ($1 != "key" || $2 != "value" || NF != 2) exit 1
    next
  }
  NF != 2 || !($1 in manifest) || manifest[$1] != $2 { exit 1 }
  { ++seen }
  END { if (seen != 18) exit 1 }
' "$manifest" "$identity" || fail "normalized identity/manifest mismatch"

commit=$(git rev-parse HEAD)
dirty=false
if [[ -n $(git status --porcelain=v1 --untracked-files=all) ]]; then
  dirty=true
fi
dagutil_sha=$(sha "$dagutil")
identity_sha=$(sha "$identity")
canonical_sha=$(sha "$canonical")
search_stdout_sha=$(sha "$directory/provider-search.stdout")
search_stderr_sha=$(sha "$directory/provider-search.stderr")
certificate=$directory/PROVIDER-CERTIFICATE.tsv
certificate_staging=$directory/.PROVIDER-CERTIFICATE.tsv.staging
if [[ -e $certificate ]]; then
  certificate_value() {
    awk -F '\t' -v key="$1" '$1 == key { print $2; found = 1 }
      END { if (!found) exit 1 }' "$certificate"
  }
  [[ $(certificate_value provider_input_sha256) == "$expected_provider" ]] ||
    fail "existing certificate provider identity differs"
  [[ $(certificate_value replay_manifest_sha256) == "$expected_manifest" ]] ||
    fail "existing certificate replay manifest differs"
  certified_identity=$(certificate_value normalized_identity_report_sha256)
  [[ $certified_identity == "$identity_sha" ]] ||
    fail "existing certificate normalized identity differs"
  certified_canonical=$(certificate_value canonical_provider_report_sha256)
  [[ $certified_canonical == "$canonical_sha" ]] ||
    fail "existing certificate canonical report differs"
  awk -F '\t' '
    FNR == NR {
      if (FNR > 1) certificate[$1] = $2
      next
    }
    FNR == 1 { next }
    !($1 in certificate) || certificate[$1] != $2 { exit 1 }
  ' "$certificate" "$identity" ||
    fail "existing certificate normalized fields differ"
  echo "TI-2 provider replay: PASS (verified existing certificate)"
  exit 0
fi
[[ ! -e $certificate_staging ]] || fail "stale certificate staging file"
{
cat <<EOF
key	value
schema_name	larch-topology-islands-ti2-provider-v1
artifact_availability	replayable_only
retrieval_witness	test/topology_islands_ti2_provider_replay.sh+tracked_seed_and_reference
provider_disposition	$disposition
replay_manifest_sha256	$expected_manifest
normalized_identity_report_sha256	$identity_sha
canonical_provider_report_sha256	$canonical_sha
provider_search_stdout_sha256	$search_stdout_sha
provider_search_stderr_sha256	$search_stderr_sha
validation_commit	$commit
validation_dirty	$dirty
validator_dagutil_sha256	$dagutil_sha
generation_dagutil_sha256	not_recorded
producer_toolchain	gcc-trunk-17-experimental
provider_recipe	deterministic_medium_chart_spr_seed1_v1
EOF
tail -n +2 "$identity"
} >"$certificate_staging"
ln "$certificate_staging" "$certificate" ||
  fail "no-replace certificate publication failed"
rm "$certificate_staging"

echo "TI-2 provider replay: PASS"

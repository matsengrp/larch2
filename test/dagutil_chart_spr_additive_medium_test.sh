#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -x $1 ]]; then
  echo "usage: $0 <dagutil>" >&2
  exit 2
fi

dagutil=$1
input=data/seedtree/seedtree.pb.gz
reference=data/seedtree/refseq.txt.gz
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# This is a frozen quality/usability fixture, not a timing benchmark. Refuse to
# silently bless a changed dataset under the historical 1642 -> <=1627
# contract.
expected_input_sha256=2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153
expected_reference_sha256=088f7d8ebcf6277f1a971961ccaa9e797bd6e5269656e14bc782ba7fb4ec742c
actual_input_sha256=$(cmake -E sha256sum "$input" | awk '{print $1}')
actual_reference_sha256=$(cmake -E sha256sum "$reference" | awk '{print $1}')
[[ $actual_input_sha256 == "$expected_input_sha256" ]]
[[ $actual_reference_sha256 == "$expected_reference_sha256" ]]

fail() {
  echo "additive chart medium regression: $*" >&2
  exit 1
}

# Return a scalar that occurs exactly once at the requested indentation. Exact
# indentation prevents an iteration field from satisfying a summary check (or
# vice versa).
scalar() {
  local file=$1
  local spaces=$2
  local key=$3
  awk -v wanted="$(printf '%*s' "$spaces" '')$key: " '
    index($0, wanted) == 1 {
      count += 1
      value = substr($0, length(wanted) + 1)
    }
    END {
      if (count != 1) exit 1
      print value
    }
  ' "$file"
}

parsimony_min() {
  local file=$1
  awk '
    match($0, /^parsimony_min: score:[0-9]+, count:[0-9]+$/) {
      count += 1
      value = substr($0, RSTART, RLENGTH)
      sub(/^parsimony_min: score:/, "", value)
      sub(/, count:.*$/, "", value)
    }
    END {
      if (count != 1) exit 1
      print value
    }
  ' "$file"
}

require_positive_integer() {
  local label=$1
  local value=$2
  [[ $value =~ ^[1-9][0-9]*$ ]] ||
    fail "$label must be a positive integer, got '$value'"
}

# Establish the independently scored input objective from the frozen protobuf,
# rather than trusting a value emitted by the chart search itself.
"$dagutil" \
  --tree-pb "$input" \
  --refseq "$reference" \
  --force-no-vcf \
  --validate \
  --parsimony \
  >"$tmp/input-score.out" 2>"$tmp/input-score.err"
input_score=$(parsimony_min "$tmp/input-score.out") ||
  fail "could not read the independent input parsimony score"
[[ $input_score == 1642 ]] ||
  fail "frozen input parsimony must be 1642, got $input_score"

# One deterministic, direct-kary transaction matches the native loop's
# per-radius move cap and predicted-delta threshold. Worker parallelism changes
# neither stable move gathering nor any quality gate; wall-clock speed is
# deliberately not an acceptance condition.
"$dagutil" \
  --tree-pb "$input" \
  --refseq "$reference" \
  --force-no-vcf \
  --validate \
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
  -o "$tmp/output.pb.gz" \
  >"$tmp/search.out" 2>"$tmp/search.err"
[[ -s "$tmp/output.pb.gz" ]] ||
  fail "search did not write a non-empty output DAG"

[[ $(scalar "$tmp/search.out" 2 polytomy_mode) == allow ]] ||
  fail "search did not run in direct allow mode"
search_mode=$(scalar "$tmp/search.out" 2 search_mode) ||
  fail "missing search-mode label"
[[ $search_mode == sampled_tree_spr_additive_fragment_union ]] ||
  fail "search did not run the additive fragment-union path"
[[ $(scalar "$tmp/search.out" 2 additive_batch_fragment_materialization) == \
  projected_chart_candidate_exact_after_topology_certificate ]] ||
  fail "search did not bind fragments to projected chart certificates"
[[ $(scalar "$tmp/search.out" 2 score_ua_edge) == true ]] ||
  fail "search did not use the external-score UA-edge convention"

initial_arity=$(scalar "$tmp/search.out" 2 grammar_max_arity) ||
  fail "missing initial grammar arity"
require_positive_integer grammar_max_arity "$initial_arity"
(( initial_arity > 2 )) ||
  fail "frozen input was replaced by a binary surrogate (arity=$initial_arity)"

reported_initial=$(scalar "$tmp/search.out" 2 initial_score) ||
  fail "missing reported initial score"
reported_final=$(scalar "$tmp/search.out" 2 final_score) ||
  fail "missing reported final score"
accepted_moves=$(scalar "$tmp/search.out" 2 accepted_moves) ||
  fail "missing accepted-move count"
[[ $reported_initial == "$input_score" ]] ||
  fail "chart initial score $reported_initial disagrees with external $input_score"
[[ $accepted_moves == 1 ]] ||
  fail "expected one committed additive transaction, got $accepted_moves"
[[ $reported_final =~ ^[0-9]+$ ]] ||
  fail "reported final score is not an integer: '$reported_final'"
(( reported_final < reported_initial )) ||
  fail "accepted transaction did not reduce the DAG score"
(( reported_final <= 1627 )) ||
  fail "final score $reported_final is not comparable to the native 1627 result"

[[ $(scalar "$tmp/search.out" 6 additive_batch_union) == true ]] ||
  fail "iteration was not labelled as an additive batch"
[[ $(scalar "$tmp/search.out" 6 accepted_move_committed) == true ]] ||
  fail "iteration did not commit an accepted update"
[[ $(scalar "$tmp/search.out" 6 batch_exact_witness_score_parity) == true ]] ||
  fail "exact k-ary chart witness did not pass external-score parity"

sampled_chart=$(scalar "$tmp/search.out" 6 batch_sampled_tree_exact_chart_score) ||
  fail "missing sampled-tree exact chart score"
sampled_external=$(scalar "$tmp/search.out" 6 batch_sampled_tree_external_score) ||
  fail "missing sampled-tree external score"
tentative_external=$(scalar "$tmp/search.out" 6 \
  batch_tentative_union_external_score) ||
  fail "missing tentative-union external score"
[[ $sampled_chart == "$sampled_external" ]] ||
  fail "sampled chart/external scores differ: $sampled_chart != $sampled_external"
[[ $sampled_external == "$reported_initial" ]] ||
  fail "sampled baseline $sampled_external differs from initial $reported_initial"
(( tentative_external < reported_initial )) ||
  fail "tentative union $tentative_external did not improve initial $reported_initial"

projected=$(scalar "$tmp/search.out" 6 batch_moves_projected) ||
  fail "missing projected-move count"
kary_projected=$(scalar "$tmp/search.out" 6 batch_multifurcating_moves_projected) ||
  fail "missing projected k-ary move count"
max_source_arity=$(scalar "$tmp/search.out" 6 batch_max_source_parent_arity) ||
  fail "missing maximum source-parent arity"
fragments=$(scalar "$tmp/search.out" 6 batch_fragments_materialized) ||
  fail "missing fragment count"
certificate_fragments=$(scalar "$tmp/search.out" 6 \
  batch_candidate_certificates_materialized) ||
  fail "missing candidate-certificate materialization count"
output_arity=$(scalar "$tmp/search.out" 6 batch_output_grammar_max_arity) ||
  fail "missing output grammar arity"
witness_kary=$(scalar "$tmp/search.out" 6 \
  batch_exact_witness_multifurcation_productions) ||
  fail "missing exact-witness k-ary production count"
for item in \
  "batch_moves_projected:$projected" \
  "batch_multifurcating_moves_projected:$kary_projected" \
  "batch_max_source_parent_arity:$max_source_arity" \
  "batch_fragments_materialized:$fragments" \
  "batch_candidate_certificates_materialized:$certificate_fragments" \
  "batch_output_grammar_max_arity:$output_arity" \
  "batch_exact_witness_multifurcation_productions:$witness_kary"; do
  require_positive_integer "${item%%:*}" "${item#*:}"
done
(( max_source_arity > 2 )) ||
  fail "no projected move came from a multifurcating parent"
(( output_arity > 2 )) ||
  fail "committed output lost arbitrary-arity productions"
[[ $fragments == "$projected" ]] ||
  fail "projected move/complete fragment mismatch: $projected != $fragments"
[[ $certificate_fragments == "$projected" ]] ||
  fail "projected move/certified fragment mismatch: $projected != $certificate_fragments"

witness_chart=$(scalar "$tmp/search.out" 6 batch_exact_witness_chart_score) ||
  fail "missing exact chart witness score"
witness_external=$(scalar "$tmp/search.out" 6 batch_exact_witness_external_score) ||
  fail "missing exact external witness score"
batch_output=$(scalar "$tmp/search.out" 6 batch_output_external_score) ||
  fail "missing batch output score"
[[ $witness_chart == "$witness_external" ]] ||
  fail "chart/external witness scores differ: $witness_chart != $witness_external"
(( witness_external < sampled_external )) ||
  fail "witness $witness_external did not improve sampled baseline $sampled_external"
(( batch_output <= witness_external )) ||
  fail "batch output $batch_output is worse than witness $witness_external"
[[ $batch_output == "$reported_final" ]] ||
  fail "batch and summary output scores differ: $batch_output != $reported_final"

# Re-open the serialized artifact through a separate dagutil invocation. This
# is the acceptance oracle for loadability, DAG validity, direct k-ary
# preservation, and the independently computed final parsimony objective.
"$dagutil" \
  --dag-pb "$tmp/output.pb.gz" \
  --force-no-vcf \
  --validate \
  --parsimony \
  --wric-polytomy-mode allow \
  --wric-polytomy-report \
  >"$tmp/output-score.out" 2>"$tmp/output-score.err"
external_final=$(parsimony_min "$tmp/output-score.out") ||
  fail "could not read independently rescored output parsimony"
[[ $external_final == "$reported_final" ]] ||
  fail "serialized output score $external_final disagrees with report $reported_final"
(( external_final <= 1627 )) ||
  fail "serialized output score $external_final exceeds 1627"
reloaded_arity=$(scalar "$tmp/output-score.out" 2 grammar_max_arity) ||
  fail "missing reloaded grammar arity"
(( reloaded_arity > 2 )) ||
  fail "reloaded output is not arbitrary-arity (arity=$reloaded_arity)"

echo "additive chart medium regression: PASS (1642 -> $external_final)"

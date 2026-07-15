#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
harness=$repo_root/tools/wric_spr_search_benchmark.sh
runner=${1:-$repo_root/build/bin/wric-process-metrics}
[[ -x "$runner" ]] || { echo "missing process runner: $runner" >&2; exit 1; }

tmp=$(mktemp -d)
if [[ ${KEEP_TMP:-0} == 1 ]]; then trap 'echo "kept test directory: $tmp"' EXIT
else trap 'rm -rf "$tmp"' EXIT; fi
printf 'fixture\n' >"$tmp/input.pb.gz"

cat >"$tmp/dagutil" <<'FAKE_DAGUTIL'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} == --help ]]; then
  printf '%s\n' '  --chart-spr-workers N' \
    '  --chart-spr-canonical-result PATH' \
    '  --chart-spr-canonical-sidecar PATH' \
    '  --canonical-dag-result PATH'
  exit 0
fi
output= input= canonical= sidecar= canonical_dag= workers=1 worker_requested=1 candidates=1 exact=0 chart=0 lazy=off
worker_policy=default_serial
if [[ ${FAKE_DEFAULT_AUTOMATIC:-0} == 1 ]]; then workers=4; worker_requested=0; worker_policy=automatic_default; fi
acceptance=lower_bound_heuristic source=grammar selection=lower_bound_top_k topology=none
iterations=1 seed=1 memory=0 max_cached=0 pattern_batch=0 candidate_batch=0
commit=overlay_delta verification=transient local_accept=false dominance=off
bound_pruning=true require_exact_mask=true max_frontier=0 score_ua=false
polytomy_mode=expand-bounded exact_arity=6 shapes=1 max_productions=1024 max_clades=256
while (($#)); do
  case "$1" in
    --dag-pb|--tree-pb) input=$2; shift 2 ;;
    -o) output=$2; shift 2 ;;
    --chart-spr-search) chart=1; shift ;;
    --chart-spr-max-candidates) candidates=$2; shift 2 ;;
    --chart-spr-top-k-exact) exact=$2; shift 2 ;;
    --chart-spr-workers)
      workers=$2; worker_requested=$2
      if [[ $workers == 0 ]]; then workers=4; worker_policy=automatic
      else worker_policy=explicit; fi
      shift 2 ;;
    --chart-spr-local-score-workers)
      workers=$2; worker_requested=$2
      if [[ $workers == 0 ]]; then workers=4; worker_policy=legacy_automatic
      else worker_policy=legacy_explicit; fi
      shift 2 ;;
    --chart-spr-canonical-result) canonical=$2; shift 2 ;;
    --chart-spr-canonical-sidecar) sidecar=$2; shift 2 ;;
    --canonical-dag-result) canonical_dag=$2; shift 2 ;;
    --wric-lazy-chart) lazy=$2; shift 2 ;;
    --chart-spr-acceptance) acceptance=$2; shift 2 ;;
    --chart-spr-candidate-source) source=$2; shift 2 ;;
    --chart-spr-candidate-selection) selection=$2; shift 2 ;;
    --chart-spr-topology-selector) topology=$2; shift 2 ;;
    --wric-polytomy-mode) polytomy_mode=$2; shift 2 ;;
    --wric-polytomy-max-exact-arity) exact_arity=$2; shift 2 ;;
    --wric-polytomy-max-shapes) shapes=$2; shift 2 ;;
    --wric-polytomy-max-productions) max_productions=$2; shift 2 ;;
    --wric-polytomy-max-clades) max_clades=$2; shift 2 ;;
    --chart-spr-max-iterations) iterations=$2; shift 2 ;;
    --chart-spr-memory-budget) memory=$2; shift 2 ;;
    --chart-spr-max-cached-patterns) max_cached=$2; shift 2 ;;
    --chart-spr-pattern-batch-size) pattern_batch=$2; shift 2 ;;
    --chart-spr-candidate-batch-size) candidate_batch=$2; shift 2 ;;
    --chart-spr-commit-mode) commit=$2; shift 2 ;;
    --chart-spr-verification-mode) verification=$2; shift 2 ;;
    --chart-bnb-dominance) dominance=$2; shift 2 ;;
    --chart-bnb-max-frontier)
      [[ $2 != 0 ]] || { echo 'zero frontier cap must be represented by option omission' >&2; exit 64; }
      max_frontier=$2; shift 2 ;;
    --seed) seed=$2; shift 2 ;;
    --chart-spr-local-accept-updates) local_accept=true; shift ;;
    --chart-bnb-no-bound-pruning) bound_pruning=false; shift ;;
    --chart-bnb-score-only) require_exact_mask=false; shift ;;
    --chart-score-ua-edge) score_ua=true; shift ;;
    --refseq|--chart-spr-sampled-tree-count|--chart-spr-sampled-tree-radius|--chart-spr-max-upward-path-expansions|--chart-spr-max-path-pairs|--chart-spr-min-moved-clade-size|--chart-spr-max-moved-clade-size|--chart-spr-min-target-clade-size|--chart-spr-max-target-clade-size) shift 2 ;;
    *) shift ;;
  esac
done
parsimony=7
[[ $(basename "$input") == 1final-tree-1.nh1.pb.gz ]] && parsimony=11155
if ((chart==0)); then
  dag_payload='{"record":"canonical-dag"}'
  dag_semantic=$(printf '%s\n' "$dag_payload" | sha256sum | awk '{print $1}')
  if [[ -n "$canonical_dag" ]]; then
    printf '{"schema":"larch.dag.semantic_digest","schema_version":1,"digest_algorithm":"sha256","semantic_sha256":"%s","clades_sha256":"%s","productions_sha256":"%s","clade_count":3,"production_count":2,"parsimony_min":%s}\n' \
      "$dag_semantic" "$dag_semantic" "$dag_semantic" "$parsimony" >"$canonical_dag"
  fi
  printf '%s\n' 'nodes: 3' 'nodes: 999' 'edges: 2' \
    "parsimony_min: score:$parsimony, count:1" 'parsimony_min: score:999, count:1'
  exit 0
fi
if ((candidates==99)); then sleep 5; fi
if [[ $(basename "$input") == 1final-tree-1.nh1.pb.gz ]]; then
  printf '%s\n' 'Loading 1 input(s)...' 'Loading done.' \
    'validate_dag [after merge]: OK (5436 nodes, 5435 edges)' \
    'error: polytomy refinement: bounded expansion requires arity <= 63' >&2
  case ${FAKE_REFUSAL_MODE:-exact} in
    exact) exit 1 ;;
    exit2) exit 2 ;;
    signal) kill -TERM "$$" ;;
    core) kill -ABRT "$$" ;;
    output) printf 'forbidden output\n' >"$output"; exit 1 ;;
    dangling) ln -s "$output.missing-target" "$output"; exit 1 ;;
    sleep) sleep 5 ;;
    success) : ;;
    *) exit 3 ;;
  esac
fi
if [[ ${FAKE_DEFAULT_AUTOMATIC:-0} == 1 ]]; then
  if [[ $worker_policy == automatic_default ]]; then
    if [[ ${FAKE_SLOW_DEFAULT:-0} == 1 ]]; then sleep 0.08; else sleep 0.005; fi
    [[ ${FAKE_DEFAULT_RESOLVED_MISMATCH:-0} == 0 ]] || workers=3
  elif [[ $worker_policy == automatic ]]; then
    sleep 0.03
  fi
fi
if [[ ( ${FAKE_PHASE6_RSS_PAD:-0} == 1 && $exact == 16 ) ||
      ( ${FAKE_PHASE6_TIMING:-0} == 1 && $exact == 4 ) ]]; then
  # The process-metrics sampler observes the whole process group. A common
  # retained pad makes the W1/W8 RSS ratio deterministic instead of letting a
  # few pages of shell-startup noise dominate this tiny fake workload.
  python3 -c 'import time; pad = bytearray(32 * 1024 * 1024); time.sleep(0.05)'
fi
if [[ ${FAKE_PHASE6_TIMING:-0} == 1 && $exact == 4 &&
      ( ( $worker_requested == 8 && ${FAKE_PHASE6_SPEED_MODE:-ok} == timeout_w8 ) ||
        ( $worker_requested == 1 && ${FAKE_PHASE6_SPEED_MODE:-ok} == timeout_w1 ) ) ]]; then
  sleep 5
fi
if [[ ${FAKE_PHASE6_K16_TIMEOUT:-0} == 1 && $exact == 16 &&
      $worker_requested == 8 ]]; then
  sleep 5
fi
[[ -z "$output" ]] || cp "$input" "$output"
semantic_payload='{"record":"contract","topology_selection":"none","refinement_exactness":"BOUNDED_REFINED_GRAMMAR","keep_mask_contract":"exact_required"}'
if [[ ${FAKE_PHASE6_W1_SEMANTIC_MISMATCH:-0} == 1 &&
      ( $exact == 4 || $exact == 16 ) &&
      $worker_requested == 1 ]]; then
  semantic_payload='{"record":"contract","topology_selection":"none","refinement_exactness":"BOUNDED_REFINED_GRAMMAR","keep_mask_contract":"exact_required","phase6_w1_mismatch":true}'
fi
if [[ -n "$sidecar" && ${FAKE_FULL_MISMATCH:-0} == 1 ]]; then
  semantic_payload='{"record":"contract","topology_selection":"none","refinement_exactness":"BOUNDED_REFINED_GRAMMAR","keep_mask_contract":"exact_required","mismatch":true}'
fi
semantic=$(printf '%s\n' "$semantic_payload" | sha256sum | awk '{print $1}')
if [[ -n "$sidecar" ]]; then printf '%s\n' "$semantic_payload" >"$sidecar"; fi
if [[ -n "$canonical" ]]; then
  printf '{"schema":"chart_spr_canonical_result","schema_version":1,"digest_algorithm":"sha256","payload_encoding":"ndjson","semantic_sha256":"%s","contract_sha256":"%s","candidates_sha256":"%s","exact_sha256":"%s","acceptance_sha256":"%s","chain_sha256":"%s","final_topology_sha256":"%s","record_count":1,"candidate_count":%s,"exact_candidate_count":%s,"iteration_count":1}\n' \
    "$semantic" "$semantic" "$semantic" "$semantic" "$semantic" "$semantic" "$semantic" "$candidates" "$exact" >"$canonical"
fi
cache_strategy=all_active_patterns
[[ "$lazy" == on ]] && cache_strategy=lazy_multisite_chart
case "$acceptance" in
  exact|exact_multisite) acceptance=exact_multisite; objective=grammar_exact ;;
  fixed-topology|fixed_topology_exact) acceptance=fixed_topology_exact; objective=fixed_topology_exact ;;
  lower-bound|lower_bound_heuristic) acceptance=lower_bound_heuristic; objective=composite_lower_bound_heuristic ;;
  *) objective=unknown ;;
esac
if [[ ${FAKE_WRONG_SEMANTICS:-0} == 1 ]]; then
  acceptance=exact_multisite
  objective=grammar_exact
fi
if [[ ${FAKE_WRONG_COMMIT:-0} == 1 ]]; then commit=materialize_rebuild; fi
if [[ ${FAKE_WRONG_WORKER_POLICY:-0} == 1 ]]; then worker_policy=automatic; fi
fake_trial=0
if [[ $output =~ _trial([0-9]+)_ ]]; then fake_trial=${BASH_REMATCH[1]}; fi
peak_exact=0
(( exact == 0 )) || peak_exact=1
reported_exact=$exact
initial_chart_line='  initial_chart_construction_ms: 0.5'
case ${FAKE_OBSERVABILITY_MODE:-ok} in
  ok) ;;
  duplicate_top) ;;
  nested_only)
    initial_chart_line='    initial_chart_construction_ms: 0.5' ;;
  nested_splice)
    printf '%s\n' '    initial_chart_construction_ms: 999.0' ;;
  malformed)
    initial_chart_line='  initial_chart_construction_ms: nan' ;;
  impossible_peak)
    reported_exact=1
    peak_exact=2 ;;
  *) exit 65 ;;
esac
admission_batches=0
parallel_batches=0
inner_batches=0
memory_limited_batches=0
peak_admitted=0
peak_projected=0
queued_for_memory_ms=0.0
timing_count=0
exact_axis_high_water=0
exact_verification_ms=0.0
exact_timing_min=0.0
exact_timing_mean=0.0
exact_timing_max=0.0
total_ms=10.0
if (( reported_exact > 0 )); then
  admission_batches=1
  inner_batches=1
  peak_admitted=1024
  peak_projected=2048
  timing_count=$reported_exact
  exact_axis_high_water=1
fi
case ${FAKE_ADMISSION_MODE:-ok} in
  ok) ;;
  exact_without_batches)
    admission_batches=0; inner_batches=0
    peak_admitted=0; peak_projected=0 ;;
  batches_without_bytes)
    peak_admitted=0; peak_projected=0 ;;
  queue_without_limit)
    queued_for_memory_ms=1.0 ;;
  k16_w8_missing)
    if [[ $exact == 16 && $worker_requested == 8 ]]; then
      reported_exact=0; peak_exact=0; timing_count=0
      admission_batches=0; parallel_batches=0; inner_batches=0
      memory_limited_batches=0; peak_admitted=0; peak_projected=0
      queued_for_memory_ms=0.0
    fi ;;
  *) exit 66 ;;
esac
if [[ ${FAKE_PHASE6_TIMING:-0} == 1 && $reported_exact == 4 ]]; then
  exact_verification_ms=100.000
  exact_timing_min=20.000
  exact_timing_mean=20.000
  exact_timing_max=20.000
  total_ms=110.000
  if [[ ${FAKE_PHASE6_SPEED_MODE:-ok} == boundary_pass ||
        ${FAKE_PHASE6_SPEED_MODE:-ok} == boundary_over ]]; then
    if [[ $fake_trial == 1 ]]; then
      exact_verification_ms=99.999
    elif [[ $fake_trial == 2 ]]; then
      exact_verification_ms=100.001
    fi
  fi
  if [[ $worker_requested == 8 ]]; then
    exact_verification_ms=40.000
    total_ms=50.000
    parallel_batches=1
    inner_batches=0
    peak_exact=4
    exact_axis_high_water=4
    case ${FAKE_PHASE6_SPEED_MODE:-ok} in
      ok|timeout_w8|timeout_w1) ;;
      boundary_pass)
        total_ms=60.000
        if [[ $fake_trial == 1 ]]; then
          exact_verification_ms=49.999
        elif [[ $fake_trial == 2 ]]; then
          exact_verification_ms=50.001
        fi ;;
      boundary_over)
        total_ms=60.000
        if [[ $fake_trial == 1 ]]; then
          exact_verification_ms=50.000
        elif [[ $fake_trial == 2 ]]; then
          exact_verification_ms=50.002
        fi ;;
      slow_w8)
        exact_verification_ms=60.000; total_ms=70.000 ;;
      serial_fallback)
        parallel_batches=0; inner_batches=1
        peak_exact=1; exact_axis_high_water=1 ;;
      low_peak)
        peak_exact=1 ;;
      low_axis)
        exact_axis_high_water=1 ;;
      nonfinite)
        exact_verification_ms=nan ;;
      inconsistent_count)
        timing_count=3 ;;
      inconsistent_range)
        exact_timing_min=30.000
        exact_timing_mean=20.000
        exact_timing_max=25.000 ;;
      *) exit 67 ;;
    esac
  fi
fi
cat <<REPORT
chart_spr_search:
  polytomy_mode: $polytomy_mode
  acceptance: $acceptance
  candidate_cap_semantics: post-dedup
  candidate_selection: $selection
  candidate_source: $source
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
  configured_max_candidates: $candidates
  top_k_exact_verify: $exact
  seed: $seed
  topology_selector: $topology
  topology_selection: $topology
  objective: $objective
  commit_mode: $commit
  verification_mode: $verification
  local_accept_updates: $local_accept
  chain_per_accept_exactness_label: none_conservative_materialize_rebuild
  score_ua_edge: $score_ua
  dominance_mode: $dominance
  bound_pruning: $bound_pruning
  require_exact_keep_mask: $require_exact_mask
  max_frontier_entries: $max_frontier
  keep_mask_contract: exact_required
  refinement_exactness: BOUNDED_REFINED_GRAMMAR
  polytomy_max_exact_arity: $exact_arity
  polytomy_max_shapes: $shapes
  polytomy_max_productions: $max_productions
  polytomy_max_clades: $max_clades
  lazy_policy: $lazy
  max_cached_patterns: $max_cached
  configured_pattern_batch_size: $pattern_batch
  configured_candidate_batch_size: $candidate_batch
  memory_budget_bytes: $memory
  validate: true
  force_no_vcf: true
  chart_workers_requested: $worker_requested
  chart_workers_resolved: $workers
  chart_worker_policy: $worker_policy
  local_score_workers: $workers
  cache_strategy: $cache_strategy
  active_patterns: 2
  initial_grammar_clades: 3
  initial_grammar_productions: 2
  effective_pattern_batch_size: 2
  requested_max_iterations: $iterations
  iterations: 1
  accepted_moves: 0
  initial_score: 7
  final_score: 7
  candidates_generated: $candidates
  candidates_scored: $candidates
  exact_verifications: $reported_exact
  candidate_accepts_attempted: 0
  post_materialization_rejections: 0
  initial_search_state_rebuilds: 1
  sidecar_rebuilds_after_accept: 0
  full_search_state_rebuilds: 1
  final_compaction_rebuilds: 0
  overlay_materializations_for_exact_verification: 0
  overlay_materializations_for_accept_materialization: 0
  overlay_materializations_for_final_compaction: 0
  final_compaction_exactness_kind: none
  cache_build_ms: 1.0
$initial_chart_line
  candidate_generation_ms: 2.0
  exact_initialization_ms: 3.0
  local_scoring_ms: 4.0
  exact_verification_ms: $exact_verification_ms
  accepted_rebuild_ms: 0.0
  final_compaction_ms: 0.0
  post_materialization_check_ms: 0.0
  materialization_ms: 0.0
  materialization_exact_verification_ms: 0.0
  materialization_accepted_update_ms: 0.0
  materialization_final_compaction_ms: 0.0
  peak_concurrent_exact_verifiers: $peak_exact
  chart_axis_exact_candidate_active_worker_high_water: $exact_axis_high_water
  exact_candidate_admission_batches: $admission_batches
  exact_candidate_parallel_batches: $parallel_batches
  exact_candidate_inner_parallel_batches: $inner_batches
  exact_candidate_memory_limited_batches: $memory_limited_batches
  exact_candidate_peak_admitted_bytes: $peak_admitted
  exact_candidate_peak_projected_resident_bytes: $peak_projected
  exact_candidate_queued_for_memory_ms: $queued_for_memory_ms
  local_candidates_per_second: 0.0
  chart_cache_resident_bytes: 0
  final_grammar_clades: 3
  final_grammar_productions: 2
  total_ms: $total_ms
  exact_candidate_timing_count: $timing_count
  exact_candidate_verification_ms_min: $exact_timing_min
  exact_candidate_verification_ms_mean: $exact_timing_mean
  exact_candidate_verification_ms_max: $exact_timing_max
  final_dag:
    leaves: 2
    nodes: 3
    edges: 2
  affected_clade_count_distribution:
    mean: 0.000
    p50: 0
    p95: 0
    max: 0
  iteration_reports:
    - iteration: 0
      candidate_generation:
        stop_reason: candidate_cap
  counters:
    upward_path_iterator_steps: 0
    path_pairs_considered: 0
    candidates_pruned_before_construction: 0
    candidates_pruned_after_construction: 0
    candidates_generated_after_dedup: $candidates
    candidate_cap_cutoffs: 1
    path_budget_cutoffs: 0
    overlay_materializations_for_oracle: 0
    full_overlay_materializations: 0
    reachable_clades_traversed: 0
    reachable_productions_traversed: 0
    reachability_full_grammar_like_passes: 0
    exact_verifications: $reported_exact
    accepted_moves: 0
    candidate_accepts_attempted: 0
    post_materialization_rejections: 0
    sidecar_rebuilds_after_accept: 0
    overlay_materializations_for_exact_verification: 0
    overlay_materializations_for_accept_materialization: 0
    overlay_materializations_for_final_compaction: 0
  candidate_generation:
    candidates_scored: 999
    stop_reason: candidate_cap
REPORT
if [[ ${FAKE_OBSERVABILITY_MODE:-ok} == duplicate_top ]]; then
  printf '%s\n' '  materialization_ms: 0.0'
fi
FAKE_DAGUTIL

cat >"$tmp/larch2" <<'FAKE_LARCH2'
#!/usr/bin/env bash
set -euo pipefail
input= output=
while (($#)); do
  case "$1" in
    --dag-pb|--tree-pb) input=$2; shift 2 ;;
    -o) output=$2; shift 2 ;;
    --refseq|-n|--max-moves|--seed) shift 2 ;;
    *) shift ;;
  esac
done
sleep 0.03
cp "$input" "$output"
printf '%s\n' 'iter 1: parsimony=7' >&2
FAKE_LARCH2
chmod +x "$tmp/dagutil" "$tmp/larch2"

capture_rss_limit=1073741824
common_base=(--dagutil "$tmp/dagutil" --larch2 "$tmp/larch2"
  --process-metrics "$runner" --dag "$tmp/input.pb.gz" --iterations 1
  --max-moves 1 --max-candidates 1 --top-k-exact 0
  --modes grammar_lower_bound)
common=("${common_base[@]}" --capture-rss-limit-bytes "$capture_rss_limit")

# A nested repeated name formerly triggered sed|head SIGPIPE/exit 141.  The
# strict chart-summary extractor ignores it and reads the unique top-level key.
"$harness" "${common[@]}" --out-dir "$tmp/smoke" >/dev/null
[[ $(awk 'END{print NR-1}' "$tmp/smoke/summary.tsv") == 2 ]]
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["method"]~/grammar/{print $h["candidates_scored"]}' "$tmp/smoke/raw_trials.tsv") == 1 ]]
awk -F '\t' '
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  $h["method"]~/grammar/ {
    found=1
    exit !($h["initial_chart_construction_ms"]=="0.5" &&
      $h["materialization_ms"]=="0.0" &&
      $h["materialization_exact_verification_ms"]=="0.0" &&
      $h["materialization_accepted_update_ms"]=="0.0" &&
      $h["materialization_final_compaction_ms"]=="0.0" &&
      $h["peak_concurrent_exact_verifiers"]=="0" &&
      $h["exact_candidate_admission_batches"]=="0" &&
      $h["exact_candidate_peak_admitted_bytes"]=="0" &&
      $h["exact_candidate_peak_projected_resident_bytes"]=="0")
  }
  END {if(!found)exit 1}' "$tmp/smoke/raw_trials.tsv"
! grep -Eq -- '--chart-spr-(workers|local-score-workers)' "$tmp/smoke/commands.sh"

# Warmups are absent from raw rows, measured trials alternate order, and the
# compatibility worker option never emits the unified option.
"$harness" "${common[@]}" --out-dir "$tmp/repeated" --local-workers 1 \
  --warmups 1 --repetitions 2 >/dev/null
[[ $(awk 'END{print NR-1}' "$tmp/repeated/raw_trials.tsv") == 4 ]]
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}{seen[$h["execution_order"]]=1}END{print length(seen)}' "$tmp/repeated/raw_trials.tsv") == 2 ]]
grep -q -- '--chart-spr-local-score-workers 1' "$tmp/repeated/commands.sh"
! grep -q -- '--chart-spr-workers' "$tmp/repeated/commands.sh"

# Unified matrices forward numeric worker budgets, and full provenance is a
# separate unaggregated command rather than part of the timed command.
"$harness" "${common[@]}" --out-dir "$tmp/matrix" --workers-list 1,2 >/dev/null
grep -q -- '--chart-spr-workers 1' "$tmp/matrix/commands.sh"
grep -q -- '--chart-spr-workers 2' "$tmp/matrix/commands.sh"
! grep -q -- '--chart-spr-local-score-workers' "$tmp/matrix/commands.sh"
"$harness" "${common[@]}" --out-dir "$tmp/full" --full-canonical-correctness >/dev/null
[[ $(find "$tmp/full/logs" -name '*_full_canonical.ndjson' -type f | wc -l) == 1 ]]
[[ $(grep -c -- '--chart-spr-canonical-sidecar' "$tmp/full/commands.sh") == 1 ]]
grep -q -- '--chart-spr-workers 1' "$tmp/full/commands.sh"
# Timed commands precede all generated-output validation and semantic work,
# and compact capture is absent from the timed chart argv.
timed_chart_line=$(grep -n 'workers=default trial1' "$tmp/full/commands.sh" | cut -d: -f1)
first_deferred_line=$(awk '/deferred output validation/{print NR;exit}' "$tmp/full/commands.sh")
(( timed_chart_line < first_deferred_line ))
timed_chart_command=$(sed -n "$((timed_chart_line+1))p" "$tmp/full/commands.sh")
[[ "$timed_chart_command" != *chart-spr-canonical-result* ]]
# Every initial, timed, deferred-validation, compact-companion, and full-oracle
# child in a non-manifest capture inherits the one frozen capture cap.
metrics_count=0
while IFS= read -r -d '' metrics_file; do
  awk -F= -v cap="$capture_rss_limit" '
    {value=substr($0,length($1)+2); values[$1]=value}
    END {
      exit !(values["outcome"]=="exited" && values["runner_exit_code"]=="0" &&
        values["rss_limit_enabled"]=="1" && values["rss_limit_bytes"]==cap &&
        values["rss_limit_observed"]=="0" && values["rss_limit_exceeded"]=="0" &&
        values["rss_limit_trigger_bytes"]=="0" &&
        values["rss_limit_term_sent"]=="0" && values["rss_limit_kill_sent"]=="0")
    }' "$metrics_file"
  ((metrics_count += 1))
done < <(find "$tmp/full" -type f -name '*.process_metrics' -print0)
(( metrics_count >= 6 ))
awk -F '\t' -v cap="$capture_rss_limit" '
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  {
    if($h["runner_outcome"]!="exited" || $h["runner_exit_code"]!="0" ||
       $h["rss_limit_enabled"]!="1" || $h["rss_limit_observed"]!="0" ||
       $h["rss_limit_exceeded"]!="0" || $h["rss_limit_trigger_bytes"]!="0" ||
       $h["rss_limit_term_sent"]!="0" || $h["rss_limit_kill_sent"]!="0" ||
       $h["process_rss_limit_bytes"]!=cap || $h["manifest_rss_limit_bytes"]!=cap)
      exit 1
  }' "$tmp/full/raw_trials.tsv"

"$harness" "${common[@]}" --out-dir "$tmp/lazy" --chart-lazy-policy on >/dev/null
grep -q -- '--wric-lazy-chart on' "$tmp/lazy/commands.sh"
"$harness" "${common[@]}" --out-dir "$tmp/lazy-auto" \
  --chart-lazy-policy auto >/dev/null
grep -q -- '--wric-lazy-chart auto' "$tmp/lazy-auto/commands.sh"

set +e
"$harness" "${common[@]}" --out-dir "$tmp/conflict" --chart-workers 2 \
  --local-workers 1 >/dev/null 2>&1
conflict_status=$?
set -e
[[ $conflict_status -ne 0 ]]

# Strict manifest helpers. Canonical argv bytes are deliberately reproduced
# here so a path or option-order change is a tested contract change.
header=$(DAGUTIL="$tmp/dagutil" LARCH2="$tmp/larch2" WRIC_PROCESS_METRICS="$runner" \
  "$harness" --print-workload-manifest-header)
mkdir "$tmp/frozen-harness"
cp "$harness" "$tmp/frozen-harness/wric_spr_search_benchmark.sh"
relocated_header=$(WRIC_REPO_ROOT="$repo_root" DAGUTIL="$tmp/dagutil" \
  LARCH2="$tmp/larch2" WRIC_PROCESS_METRICS="$runner" \
  "$tmp/frozen-harness/wric_spr_search_benchmark.sh" \
  --print-workload-manifest-header)
[[ "$relocated_header" == "$header" ]]
set +e
WRIC_REPO_ROOT=relative "$tmp/frozen-harness/wric_spr_search_benchmark.sh" \
  --print-workload-manifest-header >/dev/null 2>&1
bad_repo_root_status=$?
set -e
(( bad_repo_root_status != 0 ))
input_sha=$(sha256sum "$tmp/input.pb.gz" | awk '{print $1}')
dagutil_sha=$(sha256sum "$tmp/dagutil" | awk '{print $1}')
larch2_sha=$(sha256sum "$tmp/larch2" | awk '{print $1}')
printf 'frozen commands\n' >"$tmp/frozen-commands.sh"
commands_sha=$(sha256sum "$tmp/frozen-commands.sh" | awk '{print $1}')
affinity=$(taskset -pc $$ | awk -F': ' '{print $2}')
dag_payload='{"record":"canonical-dag"}'
output_sha=$(printf '%s\n' "$dag_payload" | sha256sum | awk '{print $1}')
semantic_payload='{"record":"contract","topology_selection":"none","refinement_exactness":"BOUNDED_REFINED_GRAMMAR","keep_mask_contract":"exact_required"}'
search_sha=$(printf '%s\n' "$semantic_payload" | sha256sum | awk '{print $1}')
printf '%s\n' "$semantic_payload" >"$tmp/oracle.sidecar.ndjson"
printf '{"schema":"chart_spr_canonical_result","schema_version":1,"digest_algorithm":"sha256","semantic_sha256":"%s"}\n' "$search_sha" >"$tmp/oracle.report.json"
sidecar_sha=$(sha256sum "$tmp/oracle.sidecar.ndjson" | awk '{print $1}')
report_sha=$(sha256sum "$tmp/oracle.report.json" | awk '{print $1}')

canonical_argv_sha() {
  { printf 'wric-canonical-argv-v1\n'; printf 'argc=%s\n' "$#"
    local arg; for arg in "$@"; do printf '%s:%s\n' "${#arg}" "$arg"; done
  } | sha256sum | awk '{print $1}'
}
trial_sha() {
  { printf 'wric-trial-semantic-v1\nmethod=%s\nsearch_semantic_sha256=%s\noutput_semantic_sha256=%s\ncanonical_argv_sha256=%s\n' \
      "$1" "$2" "$3" "$4"; } | sha256sum | awk '{print $1}'
}
chart_argv_sha() {
  local candidates=$1 lazy=$2 acceptance=$3 worker_option=${4:-none} requested=${5:-default}
  local top_k=${6:-0} memory_budget=${7:-0}
  local argv=(@binary:working_chart --dag-pb "@primary:$input_sha"
    --validate --force-no-vcf --wric-polytomy-mode expand-bounded
    --wric-polytomy-max-exact-arity 6 --wric-polytomy-max-shapes 1
    --wric-polytomy-max-productions 1024 --wric-polytomy-max-clades 256
    --wric-lazy-chart "$lazy" --chart-spr-search --chart-spr-max-iterations 1
    --chart-spr-max-candidates "$candidates" --chart-spr-top-k-exact "$top_k"
    --chart-spr-candidate-selection lower_bound_top_k --chart-spr-candidate-source grammar
    --chart-spr-acceptance "$acceptance" --chart-spr-sampled-tree-count 1
    --chart-spr-sampled-tree-radius 0 --chart-spr-max-upward-path-expansions 0
    --chart-spr-max-path-pairs 0 --chart-spr-min-moved-clade-size 1
    --chart-spr-max-moved-clade-size 0 --chart-spr-min-target-clade-size 1
    --chart-spr-max-target-clade-size 0 --chart-spr-max-cached-patterns 0
    --chart-spr-pattern-batch-size 0 --chart-spr-candidate-batch-size 0
    --chart-spr-memory-budget "$memory_budget" --chart-spr-commit-mode overlay_delta
    --chart-spr-verification-mode transient --chart-bnb-dominance off --seed 1)
  case "$worker_option" in
    none) ;;
    chart_spr_workers) argv+=(--chart-spr-workers "$requested") ;;
    chart_spr_local_score_workers) argv+=(--chart-spr-local-score-workers "$requested") ;;
    *) return 2 ;;
  esac
  argv+=(-o @output)
  canonical_argv_sha "${argv[@]}"
}
native_argv=$(canonical_argv_sha @binary:frozen_native --dag-pb "@primary:$input_sha" \
  -o @output -n 1 --max-moves 1 --seed 1 --validate)
native_trial=$(trial_sha sample_explore_merge - "$output_sha" "$native_argv")

seal_manifest() {
  local path=$1 sha; sha=$(sha256sum "$path" | awk '{print $1}')
  printf '%s  %s\n' "$sha" "$(basename "$path")" >"$path.sha256"
}
write_supplement() {
  local base=$1 path=$2 manifest_id=$3 group=$4 native_id=$5 chart_id=$6
  local parent_sha; parent_sha=$(sha256sum "$base" | awk '{print $1}')
  awk -F '\t' -v OFS='\t' -v manifest_id="$manifest_id" \
    -v parent_sha="$parent_sha" -v group="$group" \
    -v native_id="$native_id" -v chart_id="$chart_id" '
    /^# kind=/ {print "# kind=supplement"; next}
    /^# manifest_id=/ {print "# manifest_id=" manifest_id; next}
    /^# parent_sha256=/ {print "# parent_sha256=" parent_sha; next}
    /^#/ {print; next}
    !header {for(i=1;i<=NF;i++)h[$i]=i;header=1;print;next}
    {
      $h["run_group"]=group
      if($h["row_id"]=="native-ok")$h["row_id"]=native_id
      else if($h["row_id"]=="chart-row")$h["row_id"]=chart_id
      print
    }' "$base" >"$path"
  seal_manifest "$path"
}
write_manifest() {
  local path=$1 group=$2 outcome=$3 candidates=$4 lazy=$5 method=$6 acceptance=$7 objective=$8
  local chart_argv=$9 chart_search=${10} chart_output=${11} chart_trial=${12}
  {
    printf '%s\n' '# schema=wric_chart_parallelization_workloads' '# schema_version=1' \
      '# kind=base' '# manifest_id=harness-test' '# parent_sha256=-' \
      '# repo_revision=0000000000000000000000000000000000000000' \
      '# merge_base=1111111111111111111111111111111111111111' \
      '# frozen_larch2_uri=manifest://larch2' "# frozen_larch2_sha256=$larch2_sha" \
      '# frozen_oracle_dagutil_uri=manifest://dagutil' "# frozen_oracle_dagutil_sha256=$dagutil_sha" \
      '# commands_uri=manifest://frozen-commands.sh' "# commands_sha256=$commands_sha"
    printf '%s\n' "$header"
    awk -v header="$header" -v input="$input_sha" -v affinity="$affinity" \
      -v group="$group" -v outcome="$outcome" -v candidates="$candidates" -v lazy="$lazy" \
      -v method="$method" -v acceptance="$acceptance" -v objective="$objective" \
      -v native_argv="$native_argv" -v native_trial="$native_trial" -v output="$output_sha" \
      -v chart_argv="$chart_argv" -v search="$chart_search" -v chart_output="$chart_output" \
      -v chart_trial="$chart_trial" -v sidecar="$sidecar_sha" -v report="$report_sha" '
      function emit(kind, i,n,col,line,success) {
        n=split(header,col,"\t"); for(i=1;i<=n;i++)v[col[i]]="-"
        success=(outcome=="ok" || outcome=="scale_limit")
        v["row_id"]=(kind=="native"?"native-ok":"chart-row"); v["run_group"]=group
        v["workload_name"]="paired"; v["fixture_id"]="fake"; v["input_kind"]="dag_pb"
        v["primary_uri"]="manifest://input.pb.gz"; v["primary_sha256"]=input
        v["binary_role"]=(kind=="native"?"frozen_native":"working_chart")
        v["method"]=(kind=="native"?"sample_explore_merge":method); v["worker_option"]="none"
        v["requested_workers"]=(kind=="native"?"-":"default")
        v["expected_resolved_workers"]=(kind=="native"||outcome=="timeout"?"-":"1")
        v["expected_worker_policy"]=(kind=="native"||outcome=="timeout"?"-":"policy")
        v["affinity_cpus"]=affinity; v["timeout_seconds"]="1"; v["rss_limit_bytes"]="1073741824"
        v["expected_outcome"]=(kind=="native"?"ok":outcome); v["expected_timeout_trials"]=(kind=="native"?"0":(outcome=="timeout"?"1":"0"))
        v["iterations"]="1"; v["seed"]="1"; v["native_max_moves"]=(kind=="native"?"1":"-")
        v["chart_max_candidates"]=(kind=="native"?"-":candidates); v["chart_top_k_exact"]=(kind=="native"?"-":"0")
        v["validate"]="true"; v["expected_initial_score"]="7"
        v["expected_final_score"]=(kind=="native"||success?"7":"-"); v["expected_validated_parsimony"]=(kind=="native"||success?"7":"-")
        v["oracle_search_semantic_sha256"]=(kind=="native"?"-":(success?search:"-"))
        v["oracle_output_semantic_sha256"]=(kind=="native"?output:(success?chart_output:"-"))
        v["oracle_trial_semantic_sha256"]=(kind=="native"?native_trial:(success?chart_trial:"-"))
        v["canonical_argv_sha256"]=(kind=="native"?native_argv:chart_argv)
        if(kind!="native") {
          v["candidate_cap_semantics"]="post-dedup"; v["acceptance"]=acceptance; v["objective"]=objective
          v["candidate_selection"]="lower_bound_top_k"; v["candidate_source"]="grammar"; v["topology_selector"]="none"
          v["randomize_order"]="false"; v["reservoir_sample"]="false"; v["include_immediate_reversals"]="false"
          v["sampled_tree_count"]="1"; v["sampled_tree_radius"]="0"; v["sampled_tree_score_threshold"]="2147483647"
          v["max_upward_path_expansions"]="0"; v["max_path_pairs"]="0"; v["min_moved_clade_size"]="1"; v["max_moved_clade_size"]="0"
          v["min_target_clade_size"]="1"; v["max_target_clade_size"]="0"; v["max_affected_clades"]="0"
          v["polytomy_mode"]="expand-bounded"; v["polytomy_max_exact_arity"]="6"; v["polytomy_max_shapes"]="1"
          v["polytomy_max_productions"]="1024"; v["polytomy_max_clades"]="256"; v["lazy_policy"]=lazy
          v["max_cached_patterns"]="0"; v["pattern_batch_size"]="0"; v["candidate_batch_size"]="0"; v["memory_budget_bytes"]="0"
          v["commit_mode"]="overlay_delta"; v["verification_mode"]="transient"; v["local_accept_updates"]="false"
          v["dominance_mode"]="off"; v["bound_pruning"]="true"; v["require_exact_keep_mask"]="true"; v["max_frontier_entries"]="0"
          v["score_ua_edge"]="false"; v["force_no_vcf"]="true"
          v["expected_refinement_exactness"]="BOUNDED_REFINED_GRAMMAR"
          v["expected_cache_strategy"]=(lazy=="on"?"lazy_multisite_chart":"all_active_patterns")
          v["expected_effective_pattern_batch_size"]="2"
          if(success) {
            v["canonical_sidecar_uri"]="manifest://oracle.sidecar.ndjson"; v["canonical_sidecar_sha256"]=sidecar
            v["oracle_report_uri"]="manifest://oracle.report.json"; v["oracle_report_sha256"]=report
            v["expected_final_compaction_exactness"]="none"
            v["expected_chain_exactness"]="none_conservative_materialize_rebuild"; v["expected_active_patterns"]="2"
            v["expected_initial_clades"]="3"; v["expected_initial_productions"]="2"; v["expected_candidates_generated"]=candidates
            v["expected_candidates_scored"]=candidates; v["expected_exact_verifications"]="0"; v["expected_stop_reason"]="candidate_cap"
            v["expected_iterations"]="1"; v["expected_accepted_moves"]="0"
          }
        }
        line=v[col[1]]; for(i=2;i<=n;i++)line=line"\t"v[col[i]]; print line; delete v
      }
      BEGIN{emit("native");emit("chart")}'
  } >"$path"
  seal_manifest "$path"
}
mutate_field() {
  local src=$1 dst=$2 row=$3 field=$4 value=$5
  awk -F '\t' -v OFS='\t' -v id="$row" -v field="$field" -v value="$value" '
    /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
    $x["row_id"]==id{$x[field]=value}{print}' "$src" >"$dst"
  seal_manifest "$dst"
}
make_timeout_labeled_row() {
  local src=$1 dst=$2 row=$3
  awk -F '\t' -v OFS='\t' -v id="$row" '
    /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
    $x["row_id"]==id {
      $x["expected_outcome"]="timeout"
      $x["expected_timeout_trials"]="1"
      $x["expected_resolved_workers"]="-"
      $x["expected_worker_policy"]="-"
      split("oracle_search_semantic_sha256 oracle_output_semantic_sha256 oracle_trial_semantic_sha256 canonical_sidecar_uri canonical_sidecar_sha256 oracle_report_uri oracle_report_sha256", fields, " ")
      for(i in fields)$x[fields[i]]="-"
    }
    {print}' "$src" >"$dst"
  seal_manifest "$dst"
}
dash_fields() {
  local src=$1 dst=$2 row=$3; shift 3
  local fields; fields=$(IFS=,; echo "$*")
  awk -F '\t' -v OFS='\t' -v id="$row" -v fields="$fields" '
    BEGIN{n=split(fields,wanted,",");for(i=1;i<=n;i++)dash[wanted[i]]=1}
    /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
    $x["row_id"]==id{for(field in dash)$x[field]="-"}{print}' "$src" >"$dst"
  seal_manifest "$dst"
}
make_real_infeasible_manifest() {
  local src=$1 dst=$2
  awk -F '\t' -v OFS='\t' '
    /^#/{print;next}
    !h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
    $x["row_id"]=="chart-row" {
      $x["run_group"]="infeasible-test"
      $x["input_kind"]="tree_pb_refseq"
      $x["primary_uri"]="repo://data/20D_from_fasta/1final-tree-1.nh1.pb.gz"
      $x["primary_sha256"]="a65f300916f158ea4c8de8bc49f5a94379905a4c3b7fba337ce6773d65ecfce4"
      $x["refseq_uri"]="repo://data/20D_from_fasta/refseq.txt"
      $x["refseq_sha256"]="82c11885688b9a67b72ec4d2dd5913571a1723039ca501dc63bafb2cb5ea13eb"
      $x["method"]="chart_spr_grammar_exact"
      $x["worker_option"]="chart_spr_workers"; $x["requested_workers"]="1"
      $x["expected_resolved_workers"]="-"; $x["expected_worker_policy"]="-"
      $x["affinity_cpus"]="0,2,4,6,8,10,12,14"
      $x["timeout_seconds"]="600"; $x["rss_limit_bytes"]="6442450944"
      $x["expected_outcome"]="expected_infeasible"; $x["expected_timeout_trials"]="0"
      $x["expected_reason_code"]="high_arity_refinement_refusal"
      $x["expected_reason_sha256"]="84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35"
      $x["iterations"]="1"; $x["seed"]="1"
      $x["chart_max_candidates"]="1"; $x["chart_top_k_exact"]="1"
      $x["acceptance"]="exact_multisite"; $x["objective"]="grammar_exact"
      $x["candidate_source"]="grammar"; $x["polytomy_mode"]="expand-bounded"
      $x["polytomy_max_shapes"]="1"; $x["memory_budget_bytes"]="12884901888"
      $x["expected_initial_score"]="11155"
      $x["canonical_argv_sha256"]="ac03537d4db4cc20a843154aae5ed578927d5d1852809c9a8d84fdc4b27dda2f"
      split("expected_refinement_exactness expected_cache_strategy expected_effective_pattern_batch_size expected_keep_mask_kind expected_final_compaction_exactness expected_chain_exactness expected_active_patterns expected_initial_clades expected_initial_productions expected_candidates_generated expected_candidates_scored expected_exact_verifications expected_stop_reason expected_iterations expected_accepted_moves expected_final_score expected_validated_parsimony oracle_search_semantic_sha256 oracle_output_semantic_sha256 oracle_trial_semantic_sha256 canonical_sidecar_uri canonical_sidecar_sha256 oracle_report_uri oracle_report_sha256 scale_resource scale_limit scale_largest_candidates scale_largest_top_k",dash," ")
      for(i in dash)$x[dash[i]]="-"
      print
    }' "$src" >"$dst"
  seal_manifest "$dst"
}
expect_fail() {
  set +e; "$@" >/dev/null 2>&1; local status=$?; set -e
  (( status != 0 )) || { echo "expected failure: $*" >&2; exit 1; }
}
expect_fail_reason() {
  local label=$1 reason=$2; shift 2
  set +e
  "$@" >"$tmp/$label.stdout" 2>"$tmp/$label.stderr"
  local status=$?
  set -e
  (( status != 0 )) || {
    echo "expected labeled failure '$label': $*" >&2
    exit 1
  }
  grep -Fq -- "$reason" "$tmp/$label.stderr" || {
    echo "missing failure reason for '$label': $reason" >&2
    sed 's/^/  /' "$tmp/$label.stderr" >&2
    exit 1
  }
}

# Phase-0 observability fields are accepted only from one exact two-space
# summary key.  Missing top-level/duplicate/malformed values fail, a same-named
# nested value cannot splice the row, and the observed concurrency peak cannot
# exceed the actual exact-verification count.
expect_fail env FAKE_OBSERVABILITY_MODE=duplicate_top "$harness" "${common[@]}" \
  --out-dir "$tmp/observability-duplicate"
expect_fail env FAKE_OBSERVABILITY_MODE=nested_only "$harness" "${common[@]}" \
  --out-dir "$tmp/observability-nested-only"
expect_fail env FAKE_OBSERVABILITY_MODE=malformed "$harness" "${common[@]}" \
  --out-dir "$tmp/observability-malformed"
expect_fail env FAKE_OBSERVABILITY_MODE=impossible_peak "$harness" "${common[@]}" \
  --out-dir "$tmp/observability-impossible-peak"
env FAKE_OBSERVABILITY_MODE=nested_splice "$harness" "${common[@]}" \
  --out-dir "$tmp/observability-nested-splice" >/dev/null
awk -F '\t' '
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  $h["method"]~/grammar/ {
    found=1
    if($h["initial_chart_construction_ms"]!="0.5") exit 1
  }
  END {if(!found)exit 1}' "$tmp/observability-nested-splice/raw_trials.tsv"

# Successful chart rows obey the same exact-admission cross-field invariants
# as the bootstrap finalizer.  These cases used to pass the harness and fail
# only later when the sealed artifacts were finalized.
admission_common=(--dagutil "$tmp/dagutil" --larch2 "$tmp/larch2"
  --process-metrics "$runner" --dag "$tmp/input.pb.gz" --iterations 1
  --max-moves 1 --max-candidates 1 --top-k-exact 1 --modes grammar_exact)
for admission_mode in exact_without_batches batches_without_bytes queue_without_limit; do
  expect_fail_reason "admission-$admission_mode" \
    'invalid Phase-0 chart instrumentation' \
    env FAKE_ADMISSION_MODE="$admission_mode" "$harness" \
    "${admission_common[@]}" --out-dir "$tmp/admission-$admission_mode"
done

# Global totals are insufficient: every iteration item must own exactly one
# candidate-generation mapping and exactly one stop reason within that map.
stop_parser_definition=$(awk '
  /^extract_unanimous_iteration_stop_reason\(\) \{/ {inside=1}
  inside {print}
  inside && /^}$/ {exit}
' "$harness")
[[ -n "$stop_parser_definition" ]]
eval "$stop_parser_definition"
cat >"$tmp/redistributed-stop-report.out" <<'REDISTRIBUTED_STOP_REPORT'
  iterations: 2
  iteration_reports:
    - iteration: 0
      candidate_generation:
        stop_reason: candidate_cap
      candidate_generation:
        stop_reason: candidate_cap
    - iteration: 1
  counters:
    local_score_worker_tasks: 1
REDISTRIBUTED_STOP_REPORT
set +e
extract_unanimous_iteration_stop_reason "$tmp/redistributed-stop-report.out" \
  >"$tmp/redistributed-stop-scope.out" 2>"$tmp/redistributed-stop-scope.err"
redistributed_stop_status=$?
set -e
(( redistributed_stop_status != 0 ))
grep -q 'iteration stop-reason contract is ambiguous' \
  "$tmp/redistributed-stop-scope.err"

cat >"$tmp/preamble-stop-report.out" <<'PREAMBLE_STOP_REPORT'
  iterations: 2
  iteration_reports:
      candidate_generation:
        stop_reason: candidate_cap
    - iteration: 0
      candidate_generation:
        stop_reason: candidate_cap
    - iteration: 1
      candidate_generation:
        stop_reason: candidate_cap
  counters:
    local_score_worker_tasks: 1
PREAMBLE_STOP_REPORT
set +e
extract_unanimous_iteration_stop_reason "$tmp/preamble-stop-report.out" \
  >"$tmp/preamble-stop-scope.out" 2>"$tmp/preamble-stop-scope.err"
preamble_stop_status=$?
set -e
(( preamble_stop_status != 0 ))
grep -q 'iteration stop-reason contract is ambiguous' \
  "$tmp/preamble-stop-scope.err"

for bad_cap in 0 - 01 nope; do
  expect_fail "$harness" "${common_base[@]}" --out-dir "$tmp/bad-cap-$bad_cap" \
    --capture-rss-limit-bytes "$bad_cap"
done
expect_fail "$harness" "${common[@]}" --out-dir "$tmp/duplicate-cap" \
  --capture-rss-limit-bytes "$capture_rss_limit"

# A schema-valid-looking RSS observation must not be accepted under an exited
# success or timeout label: neither state may hide a cap event.
cat >"$tmp/rss-observation-runner" <<'RSS_OBSERVATION_RUNNER'
#!/usr/bin/env bash
set +e
metrics=$(mktemp)
"$REAL_WRIC_RUNNER" "$@" >"$metrics"
status=$?
if grep -q "^outcome=$MUTATE_OUTCOME$" "$metrics"; then
  sed -e 's/^rss_limit_observed=.*/rss_limit_observed=1/' \
    -e "s/^rss_limit_trigger_bytes=.*/rss_limit_trigger_bytes=$MUTATE_TRIGGER_BYTES/" \
    -e "s/^peak_sampled_rss_kb=.*/peak_sampled_rss_kb=$MUTATE_PEAK_RSS_KB/" \
    "$metrics"
else
  cat "$metrics"
fi
rm -f "$metrics"
exit "$status"
RSS_OBSERVATION_RUNNER
chmod +x "$tmp/rss-observation-runner"
expect_fail env REAL_WRIC_RUNNER="$runner" MUTATE_OUTCOME=exited \
  MUTATE_TRIGGER_BYTES=$((capture_rss_limit + 1)) \
  MUTATE_PEAK_RSS_KB=$((capture_rss_limit / 1024 + 1)) \
  "$harness" "${common[@]}" --process-metrics "$tmp/rss-observation-runner" \
  --out-dir "$tmp/observed-success"

# Metrics are a typed schema, not merely six nonempty strings.  Corrupting one
# required numeric value must fail before it can be coerced to zero by an AWK
# aggregate or acceptance gate.
cat >"$tmp/corrupt-runner" <<'CORRUPT_RUNNER'
#!/usr/bin/env bash
set +e
metrics=$(mktemp)
"$REAL_WRIC_RUNNER" "$@" >"$metrics"
status=$?
sed 's/^wall_seconds=.*/wall_seconds=NA/' "$metrics"
rm -f "$metrics"
exit "$status"
CORRUPT_RUNNER
chmod +x "$tmp/corrupt-runner"
expect_fail env REAL_WRIC_RUNNER="$runner" "$harness" "${common[@]}" \
  --process-metrics "$tmp/corrupt-runner" --out-dir "$tmp/corrupt-metrics"

timeout_argv=$(chart_argv_sha 99 off lower_bound_heuristic)
timeout_manifest="$tmp/timeout.tsv"
write_manifest "$timeout_manifest" timeout-test timeout 99 off \
  chart_spr_grammar_lower_bound_heuristic lower_bound_heuristic composite_lower_bound_heuristic \
  "$timeout_argv" - - -
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/timeout" --workload-manifest "$timeout_manifest" \
  --run-manifest-group timeout-test --allow-expected-timeout chart-row >/dev/null
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]=="chart-row"{print $h["status"]}' "$tmp/timeout/raw_trials.tsv") == timeout ]]
awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
  $h["row_id"]=="chart-row"{exit !($h["resolved_workers"]=="NA" &&
    $h["worker_policy"]=="timeout_unobserved")}' "$tmp/timeout/raw_trials.tsv"
mutate_field "$timeout_manifest" "$tmp/timeout-invented-worker-1.tsv" chart-row \
  expected_resolved_workers 1
mutate_field "$tmp/timeout-invented-worker-1.tsv" \
  "$tmp/timeout-invented-worker.tsv" chart-row expected_worker_policy explicit
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/timeout-invented-worker" \
  --workload-manifest "$tmp/timeout-invented-worker.tsv" \
  --run-manifest-group timeout-test --allow-expected-timeout chart-row
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/manifest-capture-cap" \
  --workload-manifest "$timeout_manifest" --run-manifest-group timeout-test \
  --capture-rss-limit-bytes "$capture_rss_limit"
set +e
env REAL_WRIC_RUNNER="$runner" MUTATE_OUTCOME=timeout \
  MUTATE_TRIGGER_BYTES=$((capture_rss_limit + 1)) \
  MUTATE_PEAK_RSS_KB=$((capture_rss_limit / 1024 + 1)) \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$tmp/rss-observation-runner" --out-dir "$tmp/observed-timeout" \
  --workload-manifest "$timeout_manifest" --run-manifest-group timeout-test \
  --allow-expected-timeout chart-row \
  >"$tmp/observed-timeout.stdout" 2>"$tmp/observed-timeout.stderr"
observed_timeout_status=$?
set -e
(( observed_timeout_status != 0 ))
! grep -q 'invalid process metrics' "$tmp/observed-timeout.stderr"
grep -q 'gate failure: chart-row aggregate RSS .* exceeds 1073741824 bytes' \
  "$tmp/observed-timeout.stderr"
awk -F '\t' '
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  $h["method"]=="sample_explore_merge" {
    native_initial=$h["initial_validated_parsimony_min"]
  }
  $h["row_id"]=="chart-row" {
    found=1
    exit !($h["status"]=="timeout" && $h["runner_outcome"]=="timeout" &&
      $h["runner_exit_code"]=="124" && $h["timed_out"]=="1" &&
      $h["rss_limit_enabled"]=="1" && $h["rss_limit_observed"]=="1" &&
      $h["rss_limit_exceeded"]=="0" && $h["rss_limit_term_sent"]=="0" &&
      $h["rss_limit_kill_sent"]=="0" &&
      $h["rss_limit_trigger_bytes"]>$h["process_rss_limit_bytes"] &&
      $h["initial_validated_parsimony_min"]~/^[0-9]+$/ &&
      $h["initial_validated_parsimony_min"]==native_initial &&
      $h["iterations"]=="1" && $h["seed"]=="1" &&
      $h["acceptance"]=="lower_bound_heuristic" &&
      $h["objective"]=="composite_lower_bound_heuristic" &&
      $h["candidate_selection"]=="lower_bound_top_k" &&
      $h["candidate_source"]=="grammar" &&
      $h["peak_sampled_swap_kb"]~/^[0-9]+$/ &&
      (($h["exit_code"]=="-1" && $h["term_signal"]~/^[1-9][0-9]*$/) ||
       ($h["exit_code"]~/^[0-9]+$/ && $h["term_signal"]=="0")))
  }
  END {if(!found)exit 1}' "$tmp/observed-timeout/raw_trials.tsv"

# When a frozen timeout later succeeds it has no historical semantic digest.
# The harness must automatically run a deferred explicit-W1 full oracle and
# reject any disagreement, even if the selected group has only that one chart
# row.
timeout_success_argv=$(chart_argv_sha 2 off lower_bound_heuristic)
timeout_success_manifest="$tmp/timeout-success.tsv"
write_manifest "$timeout_success_manifest" timeout-success-test timeout 2 off \
  chart_spr_grammar_lower_bound_heuristic lower_bound_heuristic composite_lower_bound_heuristic \
  "$timeout_success_argv" - - -
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/timeout-success" --workload-manifest "$timeout_success_manifest" \
  --run-manifest-group timeout-success-test >/dev/null
[[ $(find "$tmp/timeout-success/logs" -name '*_full_canonical.ndjson' -type f | wc -l) == 1 ]]
grep -q -- '--chart-spr-workers 1' "$tmp/timeout-success/commands.sh"
expect_fail env FAKE_FULL_MISMATCH=1 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/timeout-success-mismatch" \
  --workload-manifest "$timeout_success_manifest" --run-manifest-group timeout-success-test

# Timeout waivers are exact record-only contracts.
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/timeout-warmup" --workload-manifest "$timeout_manifest" --run-manifest-group timeout-test \
  --warmups 1 --allow-expected-timeout chart-row
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/timeout-duplicate" --workload-manifest "$timeout_manifest" --run-manifest-group timeout-test \
  --allow-expected-timeout chart-row --allow-expected-timeout chart-row
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/timeout-gate" --workload-manifest "$timeout_manifest" --run-manifest-group timeout-test \
  --allow-expected-timeout chart-row --require-max-rss-kb chart_spr_grammar_lower_bound_heuristic@default=999999
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/timeout-count" --workload-manifest "$timeout_manifest" --run-manifest-group timeout-test \
  --repetitions 2 --allow-expected-timeout chart-row
mutate_field "$timeout_manifest" "$tmp/timeout-mixed-scale.tsv" chart-row scale_limit 1
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/timeout-mixed-scale" --workload-manifest "$tmp/timeout-mixed-scale.tsv" --run-manifest-group timeout-test \
  --allow-expected-timeout chart-row

# URI confinement, exact preamble schema, and strict non-overwrite.
cp "$timeout_manifest" "$tmp/traversal.tsv"; sed -i 's#manifest://input.pb.gz#manifest://../input.pb.gz#g' "$tmp/traversal.tsv"; seal_manifest "$tmp/traversal.tsv"
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/traversal-out" --workload-manifest "$tmp/traversal.tsv" --run-manifest-group timeout-test
ln -s /etc/passwd "$tmp/escape"
cp "$timeout_manifest" "$tmp/symlink.tsv"; sed -i 's#manifest://input.pb.gz#manifest://escape#g' "$tmp/symlink.tsv"; seal_manifest "$tmp/symlink.tsv"
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/symlink-out" --workload-manifest "$tmp/symlink.tsv" --run-manifest-group timeout-test
cp "$timeout_manifest" "$tmp/schema.tsv"; sed -i 's/# schema_version=1/# schema_version=2/' "$tmp/schema.tsv"; seal_manifest "$tmp/schema.tsv"
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/schema-out" --workload-manifest "$tmp/schema.tsv" --run-manifest-group timeout-test
mkdir "$tmp/existing"; printf 'do not overwrite\n' >"$tmp/existing/sentinel"
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/existing" --workload-manifest "$timeout_manifest" --run-manifest-group timeout-test
[[ $(<"$tmp/existing/sentinel") == 'do not overwrite' ]]

# Closed expected-infeasible whitelist: exact frozen 20D W1 tuple, child exit
# one, exact stderr bytes, no signal/core/RSS/monitor state, and no output.
infeasible_manifest="$tmp/infeasible.tsv"
write_manifest "$tmp/infeasible-base.tsv" infeasible-test timeout 1 off \
  chart_spr_grammar_lower_bound_heuristic lower_bound_heuristic composite_lower_bound_heuristic \
  "$(chart_argv_sha 1 off lower_bound_heuristic)" - - -
make_real_infeasible_manifest "$tmp/infeasible-base.tsv" "$infeasible_manifest"
taskset -c 0,2,4,6,8,10,12,14 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/infeasible" --workload-manifest "$infeasible_manifest" --run-manifest-group infeasible-test >/dev/null
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]=="chart-row"{print $h["status"]}' "$tmp/infeasible/raw_trials.tsv") == expected_infeasible ]]
awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
  $h["row_id"]=="chart-row"{exit !($h["runner_outcome"]=="exited" &&
    $h["runner_exit_code"]=="1" && $h["exit_code"]=="1" &&
    $h["term_signal"]=="0" && $h["core_dumped"]=="0" &&
    $h["timed_out"]=="0" && $h["monitor_error"]=="0" &&
    $h["rss_limit_observed"]=="0" && $h["rss_limit_exceeded"]=="0" &&
    $h["resolved_workers"]=="NA" && $h["worker_policy"]=="unobserved")}' \
  "$tmp/infeasible/raw_trials.tsv"
# The second independently frozen worker invocation has the same refusal
# contract but a distinct canonical argv digest; neither may borrow the
# other's worker observation.
awk -F '\t' -v OFS='\t' '
  /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  $x["row_id"]=="chart-row" {
    $x["requested_workers"]="8"
    $x["canonical_argv_sha256"]="27f498987087c3ef91f39db04ae56107bca4adb0abd0c32aeb5abfa3d9c3faf3"
  }
  {print}' "$infeasible_manifest" >"$tmp/infeasible-w8.tsv"
seal_manifest "$tmp/infeasible-w8.tsv"
taskset -c 0,2,4,6,8,10,12,14 "$harness" --dagutil "$tmp/dagutil" \
  --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/infeasible-w8" --workload-manifest "$tmp/infeasible-w8.tsv" \
  --run-manifest-group infeasible-test >/dev/null
mutate_field "$infeasible_manifest" "$tmp/bad-reason.tsv" chart-row expected_reason_sha256 0000000000000000000000000000000000000000000000000000000000000000
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/bad-reason" --workload-manifest "$tmp/bad-reason.tsv" --run-manifest-group infeasible-test
for refusal_mode in exit2 signal core output dangling success; do
  expect_fail taskset -c 0,2,4,6,8,10,12,14 env FAKE_REFUSAL_MODE="$refusal_mode" "$harness" \
    --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
    --out-dir "$tmp/infeasible-$refusal_mode" --workload-manifest "$infeasible_manifest" \
    --run-manifest-group infeasible-test
done
mutate_field "$infeasible_manifest" "$tmp/infeasible-runtime-field.tsv" chart-row \
  expected_refinement_exactness BOUNDED_REFINED_GRAMMAR
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/infeasible-runtime-field" \
  --workload-manifest "$tmp/infeasible-runtime-field.tsv" --run-manifest-group infeasible-test

# Successful heterogeneous row-driven work, canonical output/trial binding,
# and exact per-trial wall pairing.
success_argv=$(chart_argv_sha 2 on lower_bound_heuristic)
success_trial=$(trial_sha chart_spr_grammar_lower_bound_heuristic "$search_sha" "$output_sha" "$success_argv")
success_manifest="$tmp/success.tsv"
write_manifest "$success_manifest" success-test ok 2 on \
  chart_spr_grammar_lower_bound_heuristic lower_bound_heuristic composite_lower_bound_heuristic \
  "$success_argv" "$search_sha" "$output_sha" "$success_trial"

# A sealed row may request the deterministic automatic representation policy;
# manifest validation and canonical argv binding must preserve the literal
# `auto` request instead of rejecting or rewriting it.
auto_manifest_argv=$(chart_argv_sha 2 auto lower_bound_heuristic)
auto_manifest_trial=$(trial_sha chart_spr_grammar_lower_bound_heuristic \
  "$search_sha" "$output_sha" "$auto_manifest_argv")
auto_manifest="$tmp/lazy-auto-manifest.tsv"
write_manifest "$auto_manifest" lazy-auto-manifest-test ok 2 auto \
  chart_spr_grammar_lower_bound_heuristic lower_bound_heuristic composite_lower_bound_heuristic \
  "$auto_manifest_argv" "$search_sha" "$output_sha" "$auto_manifest_trial"
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/lazy-auto-manifest" \
  --workload-manifest "$auto_manifest" \
  --run-manifest-group lazy-auto-manifest-test >/dev/null
grep -q -- '--wric-lazy-chart auto' \
  "$tmp/lazy-auto-manifest/commands.sh"

# Phase-6 evidence is cardinality-checked against the selected manifest.  A
# valid K16 W1/W8 pair produces one RSS comparison while retaining one
# admission row per selected row and recorded trial.
phase6_budget=1048576
phase6_w1_argv=$(chart_argv_sha 16 off exact_multisite chart_spr_workers 1 16 "$phase6_budget")
phase6_w8_argv=$(chart_argv_sha 16 off exact_multisite chart_spr_workers 8 16 "$phase6_budget")
phase6_w1_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$phase6_w1_argv")
phase6_w8_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$phase6_w8_argv")
phase6_manifest="$tmp/phase6.tsv"
awk -F '\t' -v OFS='\t' -v w1_argv="$phase6_w1_argv" \
  -v w8_argv="$phase6_w8_argv" -v w1_trial="$phase6_w1_trial" \
  -v w8_trial="$phase6_w8_trial" -v budget="$phase6_budget" '
  /^#/ {print; next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  {
    $x["run_group"]="phase6-test"
    $x["workload_name"]="phase6-paired"
    if($x["row_id"]=="chart-row") {
      $x["row_id"]="phase6-w1"
      $x["method"]="chart_spr_grammar_exact"
      $x["worker_option"]="chart_spr_workers"
      $x["requested_workers"]="1"
      $x["expected_resolved_workers"]="1"
      $x["expected_worker_policy"]="explicit"
      $x["chart_max_candidates"]="16"
      $x["chart_top_k_exact"]="16"
      $x["acceptance"]="exact_multisite"
      $x["objective"]="grammar_exact"
      $x["lazy_policy"]="off"
      $x["memory_budget_bytes"]=budget
      $x["expected_cache_strategy"]="all_active_patterns"
      $x["expected_candidates_generated"]="16"
      $x["expected_candidates_scored"]="16"
      $x["expected_exact_verifications"]="16"
      $x["canonical_argv_sha256"]=w1_argv
      $x["oracle_trial_semantic_sha256"]=w1_trial
      print
      $x["row_id"]="phase6-w8"
      $x["requested_workers"]="8"
      $x["expected_resolved_workers"]="8"
      $x["canonical_argv_sha256"]=w8_argv
      $x["oracle_trial_semantic_sha256"]=w8_trial
      print
      next
    }
    print
  }' "$success_manifest" >"$phase6_manifest"
seal_manifest "$phase6_manifest"

env FAKE_PHASE6_RSS_PAD=1 "$harness" \
  --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/phase6-positive" \
  --workload-manifest "$phase6_manifest" --run-manifest-group phase6-test \
  --workers-list 1,8 --repetitions 2 >/dev/null
[[ $(awk 'END{print NR-1}' "$tmp/phase6-positive/phase6_admission_evidence.tsv") == 4 ]]
[[ $(awk 'END{print NR-1}' "$tmp/phase6-positive/phase6_rss_comparisons.tsv") == 1 ]]
awk -F '\t' '
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  {
    rows++
    if($h["chart_top_k_exact"]!="16" ||
       $h["exact_candidate_admission_batches"]!="1" ||
       $h["exact_candidate_peak_admitted_bytes"]!="1024" ||
       $h["exact_candidate_peak_projected_resident_bytes"]!="2048") exit 1
  }
  END {if(rows!=4)exit 1}' "$tmp/phase6-positive/phase6_admission_evidence.tsv"

# A frozen timeout label is provenance, not a permanent waiver. Without an
# explicit record-only waiver, an optimized current K16 row must now complete
# and contributes to both the expected admission cardinality and RSS pair.
make_timeout_labeled_row "$phase6_manifest" \
  "$tmp/phase6-k16-frozen-timeout.tsv" phase6-w8
env FAKE_PHASE6_RSS_PAD=1 "$harness" \
  --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/phase6-k16-frozen-timeout" \
  --workload-manifest "$tmp/phase6-k16-frozen-timeout.tsv" \
  --run-manifest-group phase6-test --workers-list 1,8 >/dev/null
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-k16-frozen-timeout/phase6_admission_evidence.tsv") == 2 ]]
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-k16-frozen-timeout/phase6_rss_comparisons.tsv") == 1 ]]
make_timeout_labeled_row "$phase6_manifest" \
  "$tmp/phase6-k16-w1-timeout.tsv" phase6-w1
expect_fail_reason phase6-k16-w1-semantic-drift \
  'Phase-6 W1/W8 search/output semantic drift' \
  env FAKE_PHASE6_RSS_PAD=1 FAKE_PHASE6_W1_SEMANTIC_MISMATCH=1 \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-k16-w1-timeout.tsv" \
  --run-manifest-group phase6-test --workers-list 1,8 \
  --out-dir "$tmp/phase6-k16-w1-semantic-drift"
env FAKE_PHASE6_RSS_PAD=1 FAKE_PHASE6_K16_TIMEOUT=1 "$harness" \
  --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/phase6-k16-record-only" \
  --workload-manifest "$tmp/phase6-k16-frozen-timeout.tsv" \
  --run-manifest-group phase6-test --workers-list 8 \
  --allow-expected-timeout phase6-w8 >/dev/null
[[ $(awk -F '\t' '
  NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
  $h["row_id"]=="phase6-w8"{print $h["status"]}
' "$tmp/phase6-k16-record-only/raw_trials.tsv") == timeout ]]
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-k16-record-only/phase6_rss_comparisons.tsv") == 0 ]]

# A selected successful K16 row that loses its successful report must make the
# K16 evidence cardinality fail, even though the remaining row is valid.
expect_fail_reason phase6-k16-cardinality 'invalid K16 evidence cardinality' \
  env FAKE_PHASE6_RSS_PAD=1 FAKE_ADMISSION_MODE=k16_w8_missing "$harness" \
  --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/phase6-k16-cardinality" \
  --workload-manifest "$phase6_manifest" --run-manifest-group phase6-test \
  --workers-list 1,8

# A sealed W8 label drift used to split the comparison keys and leave a
# header-only RSS artifact that passed.  Both successful workers now have to
# form the exact one-to-one pair declared by the selected group.
mutate_field "$phase6_manifest" "$tmp/phase6-unpaired.tsv" phase6-w8 \
  workload_name phase6-unpaired
expect_fail_reason phase6-rss-unpaired \
  'selected Phase-6 RSS comparison' \
  env FAKE_PHASE6_RSS_PAD=1 "$harness" \
  --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/phase6-rss-unpaired" \
  --workload-manifest "$tmp/phase6-unpaired.tsv" \
  --run-manifest-group phase6-test --workers-list 1,8

# The named medium TopK4 pair has a stricter Phase-6 gate than the general
# exact-row RSS evidence above. Its acceptance arithmetic is fixed-point and
# hash-bound, and W8 must prove that candidate-parallel execution was observed.
phase6_speed_w1_argv=$(chart_argv_sha 32 off exact_multisite chart_spr_workers 1 4 "$phase6_budget")
phase6_speed_w8_argv=$(chart_argv_sha 32 off exact_multisite chart_spr_workers 8 4 "$phase6_budget")
phase6_speed_w1_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$phase6_speed_w1_argv")
phase6_speed_w8_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$phase6_speed_w8_argv")
phase6_speed_manifest="$tmp/phase6-speed.tsv"
awk -F '\t' -v OFS='\t' -v w1_argv="$phase6_speed_w1_argv" \
  -v w8_argv="$phase6_speed_w8_argv" -v w1_trial="$phase6_speed_w1_trial" \
  -v w8_trial="$phase6_speed_w8_trial" -v budget="$phase6_budget" '
  /^#/ {print; next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  {
    $x["run_group"]="phase6-speed-test"
    $x["workload_name"]="exact-medium-topk4"
    if($x["row_id"]=="chart-row") {
      $x["row_id"]="phase6-speed-w1"
      $x["method"]="chart_spr_grammar_exact"
      $x["worker_option"]="chart_spr_workers"
      $x["requested_workers"]="1"
      $x["expected_resolved_workers"]="1"
      $x["expected_worker_policy"]="explicit"
      $x["chart_max_candidates"]="32"
      $x["chart_top_k_exact"]="4"
      $x["acceptance"]="exact_multisite"
      $x["objective"]="grammar_exact"
      $x["lazy_policy"]="off"
      $x["memory_budget_bytes"]=budget
      $x["expected_cache_strategy"]="all_active_patterns"
      $x["expected_candidates_generated"]="32"
      $x["expected_candidates_scored"]="32"
      $x["expected_exact_verifications"]="4"
      $x["canonical_argv_sha256"]=w1_argv
      $x["oracle_trial_semantic_sha256"]=w1_trial
      print
      $x["row_id"]="phase6-speed-w8"
      $x["requested_workers"]="8"
      $x["expected_resolved_workers"]="8"
      $x["canonical_argv_sha256"]=w8_argv
      $x["oracle_trial_semantic_sha256"]=w8_trial
      print
      next
    }
    print
  }' "$success_manifest" >"$phase6_speed_manifest"
seal_manifest "$phase6_speed_manifest"

phase6_speed_common=(--dagutil "$tmp/dagutil" --larch2 "$tmp/larch2"
  --process-metrics "$runner" --workload-manifest "$phase6_speed_manifest"
  --run-manifest-group phase6-speed-test --workers-list 1,8)
env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE=boundary_pass \
  "$harness" "${phase6_speed_common[@]}" \
  --out-dir "$tmp/phase6-speed-positive" --repetitions 2 >/dev/null
phase6_speed_artifact="$tmp/phase6-speed-positive/phase6_exact_verification_speedup.tsv"
[[ $(awk 'END{print NR-1}' "$phase6_speed_artifact") == 1 ]]
awk -F '\t' '
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  {
    if($h["arithmetic_sha256"]!~/^[0-9a-f]{64}$/ ||
       $h["comparison_sha256"]!~/^[0-9a-f]{64}$/ ||
       $h["contract_sha256"]!~/^[0-9a-f]{64}$/ ||
       $h["search_semantic_sha256"]!~/^[0-9a-f]{64}$/ ||
       $h["output_semantic_sha256"]!~/^[0-9a-f]{64}$/ ||
       $h["fixture"]!="exact-medium-topk4" ||
       $h["method"]!="chart_spr_grammar_exact" ||
       $h["chart_top_k_exact"]!="4" ||
       $h["trial_count_per_worker"]!="2" ||
       $h["w1_trial_millims_by_index"]!="1:99999,2:100001" ||
       $h["w8_trial_millims_by_index"]!="1:49999,2:50001" ||
       $h["w1_median_twice_millims"]!="200000" ||
       $h["w8_median_twice_millims"]!="100000" ||
       $h["gate_lhs_twice_millims"]!="200000" ||
       $h["gate_rhs_twice_millims"]!="200000" ||
       $h["w1_median_ms"]!="100.0000" ||
       $h["w8_median_ms"]!="50.0000" ||
       $h["w1_over_w8"]!="2.000000000") exit 1
    rows++
  }
  END {if(rows!=1)exit 1}' "$phase6_speed_artifact"
phase6_speed_body=$(tail -n +2 "$phase6_speed_artifact" | cut -f2-)
phase6_speed_recorded_sha=$(tail -n +2 "$phase6_speed_artifact" | cut -f1)
[[ $(printf '%s\n' "$phase6_speed_body" | sha256sum | awk '{print $1}') == \
   "$phase6_speed_recorded_sha" ]]
awk -F '\t' '
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  $h["chart_top_k_exact"]==4 && $h["requested_workers"]==8 {
    rows++
    if($h["exact_candidate_parallel_batches"]<1 ||
       $h["peak_concurrent_exact_verifiers"]<2 ||
       $h["chart_axis_exact_candidate_active_worker_high_water"]<2) exit 1
  }
  END {if(rows!=2)exit 1}' \
  "$tmp/phase6-speed-positive/phase6_admission_evidence.tsv"

# Frozen Phase-0 timeouts are valid inputs to a later acceptance run. Without
# the explicit record-only waiver, the current row must succeed and is used in
# the speed arithmetic. With the waiver, the historical timeout remains a
# permitted characterization row and does not activate this acceptance gate.
make_timeout_labeled_row "$phase6_speed_manifest" \
  "$tmp/phase6-speed-frozen-timeout.tsv" phase6-speed-w8
env FAKE_PHASE6_TIMING=1 "$harness" \
  --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-speed-frozen-timeout.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 1,8 \
  --out-dir "$tmp/phase6-speed-frozen-timeout-success" >/dev/null
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-speed-frozen-timeout-success/phase6_exact_verification_speedup.tsv") == 1 ]]
env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE=timeout_w8 \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-speed-frozen-timeout.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 8 \
  --allow-expected-timeout phase6-speed-w8 \
  --out-dir "$tmp/phase6-speed-record-only-timeout" >/dev/null
[[ $(awk -F '\t' '
  NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
  $h["row_id"]=="phase6-speed-w8"{print $h["status"]}
' "$tmp/phase6-speed-record-only-timeout/raw_trials.tsv") == timeout ]]
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-speed-record-only-timeout/phase6_exact_verification_speedup.tsv") == 0 ]]

# A record-only exact key must be removed as a whole. In a mixed group the
# other complete key still gates and produces exactly one RSS/speed row; its
# W8 presence must not make the waived key's remaining W1 endpoint look
# incomplete.
cp "$phase6_speed_manifest" "$tmp/phase6-mixed-record-only.tsv"
awk -F '\t' -v OFS='\t' '
  /^#/ {next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;next}
  $x["row_id"]=="phase6-w1" || $x["row_id"]=="phase6-w8" {
    $x["run_group"]="phase6-speed-test"
    print
  }
' "$tmp/phase6-k16-frozen-timeout.tsv" \
  >>"$tmp/phase6-mixed-record-only.tsv"
seal_manifest "$tmp/phase6-mixed-record-only.tsv"
env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_RSS_PAD=1 FAKE_PHASE6_K16_TIMEOUT=1 \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-mixed-record-only.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 1,8 \
  --allow-expected-timeout phase6-w8 \
  --out-dir "$tmp/phase6-mixed-record-only" >/dev/null
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-mixed-record-only/phase6_rss_comparisons.tsv") == 1 ]]
[[ $(awk -F '\t' '
  NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
  {print $h["chart_top_k_exact"]}
' "$tmp/phase6-mixed-record-only/phase6_rss_comparisons.tsv") == 4 ]]
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-mixed-record-only/phase6_exact_verification_speedup.tsv") == 1 ]]

# Complementary waivers can leave global W1 and W8 expected-row totals while
# every comparison key is record-only. That is a valid header-only capture,
# not evidence that the two endpoints belong to one comparison.
make_timeout_labeled_row "$phase6_speed_manifest" \
  "$tmp/phase6-speed-w1-timeout.tsv" phase6-speed-w1
expect_fail_reason phase6-speed-w1-semantic-drift \
  'Phase-6 exact-speed search/output semantic drift' \
  env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_W1_SEMANTIC_MISMATCH=1 \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-speed-w1-timeout.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 1,8 \
  --out-dir "$tmp/phase6-speed-w1-semantic-drift"
cp "$tmp/phase6-speed-w1-timeout.tsv" \
  "$tmp/phase6-all-record-only.tsv"
awk -F '\t' -v OFS='\t' '
  /^#/ {next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;next}
  $x["row_id"]=="phase6-w1" || $x["row_id"]=="phase6-w8" {
    $x["run_group"]="phase6-speed-test"
    print
  }
' "$tmp/phase6-k16-frozen-timeout.tsv" \
  >>"$tmp/phase6-all-record-only.tsv"
seal_manifest "$tmp/phase6-all-record-only.tsv"
env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE=timeout_w1 \
  FAKE_PHASE6_RSS_PAD=1 FAKE_PHASE6_K16_TIMEOUT=1 \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-all-record-only.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 1,8 \
  --allow-expected-timeout phase6-speed-w1 \
  --allow-expected-timeout phase6-w8 \
  --out-dir "$tmp/phase6-all-record-only" >/dev/null
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-all-record-only/phase6_rss_comparisons.tsv") == 0 ]]
[[ $(awk 'END{print NR-1}' \
  "$tmp/phase6-all-record-only/phase6_exact_verification_speedup.tsv") == 0 ]]

# With two repetitions the medians are the average of the two middle fixed-
# point samples. The exact 2x boundary above passes; moving the W8 median only
# one thousandth of a millisecond above that boundary must fail.
expect_fail_reason phase6-speed-boundary-over \
  'W8 exact_verification_ms median is not at least 2.0x faster' \
  env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE=boundary_over \
  "$harness" "${phase6_speed_common[@]}" \
  --out-dir "$tmp/phase6-speed-boundary-over" --repetitions 2

expect_fail_reason phase6-speed-slow \
  'W8 exact_verification_ms median is not at least 2.0x faster' \
  env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE=slow_w8 \
  "$harness" "${phase6_speed_common[@]}" \
  --out-dir "$tmp/phase6-speed-slow"
for speed_mode in serial_fallback low_peak low_axis; do
  expect_fail_reason "phase6-speed-$speed_mode" \
    'W8 candidate-parallel path not activated' \
    env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE="$speed_mode" \
    "$harness" "${phase6_speed_common[@]}" \
    --out-dir "$tmp/phase6-speed-$speed_mode"
done
expect_fail_reason phase6-speed-nonfinite 'invalid Phase-6 exact_verification_ms' \
  env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE=nonfinite \
  "$harness" "${phase6_speed_common[@]}" \
  --out-dir "$tmp/phase6-speed-nonfinite"
for speed_mode in inconsistent_count inconsistent_range; do
  expect_fail_reason "phase6-speed-$speed_mode" \
    'inconsistent Phase-6 exact-candidate timing evidence' \
    env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE="$speed_mode" \
    "$harness" "${phase6_speed_common[@]}" \
    --out-dir "$tmp/phase6-speed-$speed_mode"
done
expect_fail_reason phase6-speed-timeout \
  'exact-speed row is not a successful non-timeout execution' \
  env FAKE_PHASE6_TIMING=1 FAKE_PHASE6_SPEED_MODE=timeout_w8 \
  "$harness" "${phase6_speed_common[@]}" \
  --out-dir "$tmp/phase6-speed-timeout"

awk -F '\t' -v OFS='\t' '
  /^#/ {print; next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  $x["row_id"]!="phase6-speed-w8" {print}
' "$phase6_speed_manifest" >"$tmp/phase6-speed-missing.tsv"
seal_manifest "$tmp/phase6-speed-missing.tsv"
expect_fail_reason phase6-speed-missing 'exact-speed comparison' \
  env FAKE_PHASE6_TIMING=1 "$harness" --dagutil "$tmp/dagutil" \
  --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-speed-missing.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 1,8 \
  --out-dir "$tmp/phase6-speed-missing"

awk -F '\t' -v OFS='\t' '
  /^#/ {print; next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  {
    print
    if($x["row_id"]=="phase6-speed-w8") {
      $x["row_id"]="phase6-speed-w8-duplicate"
      print
    }
  }
' "$phase6_speed_manifest" >"$tmp/phase6-speed-duplicate.tsv"
seal_manifest "$tmp/phase6-speed-duplicate.tsv"
expect_fail_reason phase6-speed-duplicate 'W1/W8 manifest cardinality 1/2' \
  env FAKE_PHASE6_TIMING=1 "$harness" --dagutil "$tmp/dagutil" \
  --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-speed-duplicate.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 1,8 \
  --out-dir "$tmp/phase6-speed-duplicate"

# Selecting only a named grammar-exact W2 row must activate the gate and fail
# for absent endpoints instead of producing a header-only arithmetic artifact.
phase6_speed_w2_argv=$(chart_argv_sha 32 off exact_multisite chart_spr_workers 2 4 "$phase6_budget")
phase6_speed_w2_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$phase6_speed_w2_argv")
awk -F '\t' -v OFS='\t' -v argv="$phase6_speed_w2_argv" \
  -v trial="$phase6_speed_w2_trial" '
  /^#/ {print; next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  $x["row_id"]=="phase6-speed-w8" {next}
  {
    if($x["row_id"]=="phase6-speed-w1") {
      $x["row_id"]="phase6-speed-w2"
      $x["requested_workers"]="2"
      $x["expected_resolved_workers"]="2"
      $x["canonical_argv_sha256"]=argv
      $x["oracle_trial_semantic_sha256"]=trial
    }
    print
  }
' "$phase6_speed_manifest" >"$tmp/phase6-speed-w2-only.tsv"
seal_manifest "$tmp/phase6-speed-w2-only.tsv"
expect_fail_reason phase6-speed-w2-only 'W1/W8 manifest cardinality 0/0' \
  env FAKE_PHASE6_TIMING=1 "$harness" --dagutil "$tmp/dagutil" \
  --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-speed-w2-only.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 2 \
  --out-dir "$tmp/phase6-speed-w2-only"

# The workload name closes the grammar-exact shape: changing TopK must fail
# activation rather than silently disabling the Phase-6 performance gate.
phase6_speed_k5_w1_argv=$(chart_argv_sha 32 off exact_multisite chart_spr_workers 1 5 "$phase6_budget")
phase6_speed_k5_w8_argv=$(chart_argv_sha 32 off exact_multisite chart_spr_workers 8 5 "$phase6_budget")
phase6_speed_k5_w1_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$phase6_speed_k5_w1_argv")
phase6_speed_k5_w8_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$phase6_speed_k5_w8_argv")
awk -F '\t' -v OFS='\t' -v w1_argv="$phase6_speed_k5_w1_argv" \
  -v w8_argv="$phase6_speed_k5_w8_argv" -v w1_trial="$phase6_speed_k5_w1_trial" \
  -v w8_trial="$phase6_speed_k5_w8_trial" '
  /^#/ {print; next}
  !h {for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  {
    if($x["row_id"]=="phase6-speed-w1" || $x["row_id"]=="phase6-speed-w8") {
      $x["chart_top_k_exact"]="5"
      $x["expected_exact_verifications"]="5"
      if($x["row_id"]=="phase6-speed-w1") {
        $x["canonical_argv_sha256"]=w1_argv
        $x["oracle_trial_semantic_sha256"]=w1_trial
      } else {
        $x["canonical_argv_sha256"]=w8_argv
        $x["oracle_trial_semantic_sha256"]=w8_trial
      }
    }
    print
  }
' "$phase6_speed_manifest" >"$tmp/phase6-speed-topk-drift.tsv"
seal_manifest "$tmp/phase6-speed-topk-drift.tsv"
expect_fail_reason phase6-speed-topk-drift \
  'exact-medium-topk4 grammar row has TopK drift' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --workload-manifest "$tmp/phase6-speed-topk-drift.tsv" \
  --run-manifest-group phase6-speed-test --workers-list 1,8 \
  --out-dir "$tmp/phase6-speed-topk-drift"

# Supplements are append-only, base-bound manifests with their own exact
# detached seal.  Exercise acceptance plus every identity/override rejection
# before any benchmark work can blur a provenance failure.
valid_supplement="$tmp/valid-supplement.tsv"
write_supplement "$success_manifest" "$valid_supplement" supplement-valid \
  supplement-test supplement-native supplement-chart
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/supplement-valid" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$valid_supplement" \
  --run-manifest-group supplement-test >/dev/null
[[ $(awk -F '\t' '
  NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
  $h["row_id"]=="supplement-native"||$h["row_id"]=="supplement-chart"{seen[$h["row_id"]]=1}
  END{print length(seen)}' "$tmp/supplement-valid/raw_trials.tsv") == 2 ]]

wrong_parent_supplement="$tmp/wrong-parent-supplement.tsv"
cp "$valid_supplement" "$wrong_parent_supplement"
sed -i 's/^# parent_sha256=.*/# parent_sha256=0000000000000000000000000000000000000000000000000000000000000000/' \
  "$wrong_parent_supplement"
seal_manifest "$wrong_parent_supplement"
expect_fail_reason supplement-wrong-parent \
  'supplement parent hash does not match base' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/supplement-wrong-parent-out" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$wrong_parent_supplement" \
  --run-manifest-group supplement-test

bad_schema_supplement="$tmp/bad-schema-supplement.tsv"
cp "$valid_supplement" "$bad_schema_supplement"
sed -i 's/^# schema_version=1$/# schema_version=2/' "$bad_schema_supplement"
seal_manifest "$bad_schema_supplement"
expect_fail_reason supplement-bad-schema \
  'unsupported manifest schema_version' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/supplement-bad-schema-out" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$bad_schema_supplement" \
  --run-manifest-group supplement-test

for role_key in frozen_larch2_sha256 frozen_oracle_dagutil_sha256; do
  bad_role_supplement="$tmp/bad-role-${role_key}.tsv"
  cp "$valid_supplement" "$bad_role_supplement"
  sed -i \
    "s/^# ${role_key}=.*/# ${role_key}=0000000000000000000000000000000000000000000000000000000000000000/" \
    "$bad_role_supplement"
  seal_manifest "$bad_role_supplement"
  expect_fail_reason "supplement-bad-role-${role_key}" \
    'supplement changes the frozen binary roles' \
    "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
    --process-metrics "$runner" \
    --out-dir "$tmp/supplement-bad-role-${role_key}-out" \
    --workload-manifest "$success_manifest" \
    --supplemental-workload-manifest "$bad_role_supplement" \
    --run-manifest-group supplement-test
done

# Keep the supplement's base-bound digest unchanged while redirecting its
# native asset URI to a tampered executable copy.  This reaches the distinct
# supplement preamble-asset check without modifying the real frozen asset.
cp "$tmp/larch2" "$tmp/tampered-supplement-larch2"
printf '\n# supplement asset identity mutation\n' \
  >>"$tmp/tampered-supplement-larch2"
bad_asset_supplement="$tmp/bad-asset-supplement.tsv"
cp "$valid_supplement" "$bad_asset_supplement"
sed -i \
  's|^# frozen_larch2_uri=manifest://larch2$|# frozen_larch2_uri=manifest://tampered-supplement-larch2|' \
  "$bad_asset_supplement"
seal_manifest "$bad_asset_supplement"
expect_fail_reason supplement-bad-preamble-asset \
  'supplement preamble asset hash mismatch' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" \
  --out-dir "$tmp/supplement-bad-preamble-asset-out" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$bad_asset_supplement" \
  --run-manifest-group supplement-test

# Corrupting either the sealed bytes or the detached digest must fail at the
# exact GNU-sha256sum seal check.  Neither mutation is allowed to reach rows.
body_corrupt_supplement="$tmp/body-corrupt-supplement.tsv"
write_supplement "$success_manifest" "$body_corrupt_supplement" \
  supplement-body-corrupt supplement-body-test body-native body-chart
sed -i 's/^# manifest_id=.*/# manifest_id=supplement-body-mutated/' \
  "$body_corrupt_supplement"
expect_fail_reason supplement-body-corrupt \
  'manifest seal is not exact GNU sha256sum format' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/supplement-body-corrupt-out" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$body_corrupt_supplement" \
  --run-manifest-group supplement-body-test

seal_corrupt_supplement="$tmp/seal-corrupt-supplement.tsv"
write_supplement "$success_manifest" "$seal_corrupt_supplement" \
  supplement-seal-corrupt supplement-seal-test seal-native seal-chart
printf '%064d  %s\n' 0 "$(basename "$seal_corrupt_supplement")" \
  >"$seal_corrupt_supplement.sha256"
expect_fail_reason supplement-seal-corrupt \
  'manifest seal is not exact GNU sha256sum format' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/supplement-seal-corrupt-out" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$seal_corrupt_supplement" \
  --run-manifest-group supplement-seal-test

override_supplement="$tmp/override-supplement.tsv"
write_supplement "$success_manifest" "$override_supplement" \
  supplement-override supplement-override-test native-ok override-chart
expect_fail_reason supplement-base-override 'manifest row ID overridden: native-ok' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/supplement-base-override-out" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$override_supplement" \
  --run-manifest-group supplement-override-test

duplicate_supplement_a="$tmp/duplicate-supplement-a.tsv"
duplicate_supplement_b="$tmp/duplicate-supplement-b.tsv"
write_supplement "$success_manifest" "$duplicate_supplement_a" \
  supplement-duplicate-a supplement-duplicate-a-test duplicate-a-native duplicate-shared-chart
write_supplement "$success_manifest" "$duplicate_supplement_b" \
  supplement-duplicate-b supplement-duplicate-b-test duplicate-b-native duplicate-shared-chart
expect_fail_reason supplement-cross-duplicate \
  'manifest row ID overridden: duplicate-shared-chart' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/supplement-cross-duplicate-out" \
  --workload-manifest "$success_manifest" \
  --supplemental-workload-manifest "$duplicate_supplement_a" \
  --supplemental-workload-manifest "$duplicate_supplement_b" \
  --run-manifest-group supplement-duplicate-a-test

# Frozen binary hashes bind both the manifest assets and the explicitly passed
# native executable.  Mutate only copies/expected digests; the source fakes
# stand in for the real read-only Phase-0 binaries and remain untouched.
bad_frozen_larch2_manifest="$tmp/bad-frozen-larch2.tsv"
cp "$success_manifest" "$bad_frozen_larch2_manifest"
sed -i 's/^# frozen_larch2_sha256=.*/# frozen_larch2_sha256=0000000000000000000000000000000000000000000000000000000000000000/' \
  "$bad_frozen_larch2_manifest"
seal_manifest "$bad_frozen_larch2_manifest"
expect_fail_reason frozen-larch2-hash \
  'frozen native larch2 is missing or has the wrong hash' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/frozen-larch2-hash-out" \
  --workload-manifest "$bad_frozen_larch2_manifest" \
  --run-manifest-group success-test

cp "$tmp/larch2" "$tmp/mutated-larch2"
printf '\n# identity mutation\n' >>"$tmp/mutated-larch2"
expect_fail_reason passed-larch2-hash \
  '--larch2 is not the manifest-frozen native binary' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/mutated-larch2" \
  --process-metrics "$runner" --out-dir "$tmp/passed-larch2-hash-out" \
  --workload-manifest "$success_manifest" --run-manifest-group success-test

bad_frozen_oracle_manifest="$tmp/bad-frozen-oracle.tsv"
cp "$success_manifest" "$bad_frozen_oracle_manifest"
sed -i 's/^# frozen_oracle_dagutil_sha256=.*/# frozen_oracle_dagutil_sha256=0000000000000000000000000000000000000000000000000000000000000000/' \
  "$bad_frozen_oracle_manifest"
seal_manifest "$bad_frozen_oracle_manifest"
expect_fail_reason frozen-oracle-hash \
  'frozen semantic-oracle dagutil is missing or has the wrong hash' \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/frozen-oracle-hash-out" \
  --workload-manifest "$bad_frozen_oracle_manifest" \
  --run-manifest-group success-test

mutate_field "$success_manifest" "$tmp/success-dash-worker-1.tsv" chart-row \
  expected_resolved_workers -
mutate_field "$tmp/success-dash-worker-1.tsv" "$tmp/success-dash-worker.tsv" \
  chart-row expected_worker_policy -
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/success-dash-worker" \
  --workload-manifest "$tmp/success-dash-worker.tsv" \
  --run-manifest-group success-test
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/success" --workload-manifest "$success_manifest" --run-manifest-group success-test \
  --repetitions 3 --require-wall-ratio chart_spr_grammar_lower_bound_heuristic@default=1.0 >/dev/null
[[ $(awk 'END{print NR-1}' "$tmp/success/paired_ratios.tsv") == 3 ]]
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]=="chart-row"{print $h["candidates_scored"]}' "$tmp/success/raw_trials.tsv" | sort -u) == 2 ]]
grep -q -- '--wric-lazy-chart on' "$tmp/success/commands.sh"
! grep -q -- '--chart-bnb-max-frontier 0' "$tmp/success/commands.sh"

# Worker invocation policy is a sealed conditional contract.  Omitted/default
# uses the transition sentinel, while unified/legacy explicit and auto rows
# must carry their exact closed policy labels.
mutate_field "$success_manifest" "$tmp/bad-default-policy-schema.tsv" chart-row expected_worker_policy automatic
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/bad-default-policy-schema" \
  --workload-manifest "$tmp/bad-default-policy-schema.tsv" --run-manifest-group success-test
expect_fail env FAKE_WRONG_WORKER_POLICY=1 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/wrong-default-policy-report" \
  --workload-manifest "$success_manifest" --run-manifest-group success-test
explicit_argv=$(chart_argv_sha 2 on lower_bound_heuristic chart_spr_workers 2)
explicit_trial=$(trial_sha chart_spr_grammar_lower_bound_heuristic "$search_sha" "$output_sha" "$explicit_argv")
awk -F '\t' -v OFS='\t' -v argv="$explicit_argv" -v trial="$explicit_trial" '
  /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  $x["row_id"]=="chart-row" {
    $x["worker_option"]="chart_spr_workers"; $x["requested_workers"]="2"
    $x["expected_resolved_workers"]="2"; $x["expected_worker_policy"]="explicit"
    $x["canonical_argv_sha256"]=argv; $x["oracle_trial_semantic_sha256"]=trial
  }
  {print}' "$success_manifest" >"$tmp/explicit-policy.tsv"
seal_manifest "$tmp/explicit-policy.tsv"
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/explicit-policy" --workload-manifest "$tmp/explicit-policy.tsv" \
  --run-manifest-group success-test >/dev/null
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]=="chart-row"{print $h["worker_policy"]}' "$tmp/explicit-policy/raw_trials.tsv") == explicit ]]

# Phase 10 promotes omitted/default from default_serial to automatic_default.
# Its strict gate joins every default trial to exactly one explicit-auto trial,
# compares resolved workers and semantics, and applies per-trial plus median
# 1.10 wall limits.  Raw auto labels remain stable even though argv uses 0.
policy_auto_argv=$(chart_argv_sha 2 on lower_bound_heuristic chart_spr_workers 0)
policy_auto_trial=$(trial_sha chart_spr_grammar_lower_bound_heuristic "$search_sha" "$output_sha" "$policy_auto_argv")
awk -F '\t' -v OFS='\t' -v auto_argv="$policy_auto_argv" -v auto_trial="$policy_auto_trial" '
  /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  $x["row_id"]=="chart-row" {
    $x["expected_resolved_workers"]="policy"; print
    $x["row_id"]="chart-auto"; $x["worker_option"]="chart_spr_workers"
    $x["requested_workers"]="0"; $x["expected_worker_policy"]="automatic"
    $x["canonical_argv_sha256"]=auto_argv; $x["oracle_trial_semantic_sha256"]=auto_trial
    print; next
  }
  {print}' "$success_manifest" >"$tmp/worker-policy.tsv"
seal_manifest "$tmp/worker-policy.tsv"
env FAKE_DEFAULT_AUTOMATIC=1 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/worker-policy" \
  --workload-manifest "$tmp/worker-policy.tsv" --run-manifest-group success-test \
  --workers-list default,auto --repetitions 3 \
  --require-worker-policy default=automatic_default >/dev/null
[[ $(awk 'END{print NR-1}' "$tmp/worker-policy/worker_policy_comparisons.tsv") == 3 ]]
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]=="chart-auto"{print $h["requested_workers"]}' "$tmp/worker-policy/raw_trials.tsv" | sort -u) == auto ]]
grep -q -- '--chart-spr-workers 0' "$tmp/worker-policy/commands.sh"

# Missing and duplicate companions fail before benchmarking.  Runtime label,
# resolution, and timing divergence each fail the strict comparison gate.
expect_fail env FAKE_DEFAULT_AUTOMATIC=1 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/worker-policy-missing" \
  --workload-manifest "$tmp/worker-policy.tsv" --run-manifest-group success-test \
  --workers-list default --require-worker-policy default=automatic_default
awk -F '\t' -v OFS='\t' '
  /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  {print; if($x["row_id"]=="chart-auto"){$x["row_id"]="chart-auto-duplicate";print}}' \
  "$tmp/worker-policy.tsv" >"$tmp/worker-policy-duplicate.tsv"
seal_manifest "$tmp/worker-policy-duplicate.tsv"
expect_fail env FAKE_DEFAULT_AUTOMATIC=1 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/worker-policy-duplicate" \
  --workload-manifest "$tmp/worker-policy-duplicate.tsv" --run-manifest-group success-test \
  --workers-list default,auto --require-worker-policy default=automatic_default
expect_fail env FAKE_DEFAULT_AUTOMATIC=1 FAKE_DEFAULT_RESOLVED_MISMATCH=1 \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/worker-policy-resolved" --workload-manifest "$tmp/worker-policy.tsv" \
  --run-manifest-group success-test --workers-list default,auto \
  --require-worker-policy default=automatic_default
expect_fail env FAKE_DEFAULT_AUTOMATIC=1 FAKE_SLOW_DEFAULT=1 \
  "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/worker-policy-slow" --workload-manifest "$tmp/worker-policy.tsv" \
  --run-manifest-group success-test --workers-list default,auto \
  --require-worker-policy default=automatic_default

# The strict wall gate joins only rows that share the sealed workload label.
# A chart/native label mismatch must fail rather than silently comparing
# unrelated or nonexistent trial pairs.
mutate_field "$success_manifest" "$tmp/mismatched-pair-label.tsv" chart-row workload_name unpaired
set +e
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/mismatched-pair-label" \
  --workload-manifest "$tmp/mismatched-pair-label.tsv" --run-manifest-group success-test \
  --require-wall-ratio chart_spr_grammar_lower_bound_heuristic@default=1.0 \
  >"$tmp/mismatched-pair-label.out" 2>"$tmp/mismatched-pair-label.err"
mismatched_pair_status=$?
set -e
(( mismatched_pair_status != 0 ))
grep -q 'gate failure: incomplete/non-exact paired join' "$tmp/mismatched-pair-label.err"

# A method label is a closed semantic contract, not a cosmetic manifest name.
# Reject both an internally inconsistent sealed row and a product report that
# contradicts an otherwise-consistent row.
mutate_field "$success_manifest" "$tmp/mislabeled-method.tsv" chart-row method chart_spr_grammar_exact
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/mislabeled-method" --workload-manifest "$tmp/mislabeled-method.tsv" --run-manifest-group success-test
expect_fail env FAKE_WRONG_SEMANTICS=1 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/wrong-report-semantics" \
  --workload-manifest "$success_manifest" --run-manifest-group success-test
expect_fail env FAKE_WRONG_COMMIT=1 "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" \
  --process-metrics "$runner" --out-dir "$tmp/wrong-report-commit" \
  --workload-manifest "$success_manifest" --run-manifest-group success-test

mutate_field "$success_manifest" "$tmp/bad-output.tsv" chart-row oracle_output_semantic_sha256 0000000000000000000000000000000000000000000000000000000000000000
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/bad-output" --workload-manifest "$tmp/bad-output.tsv" --run-manifest-group success-test
mutate_field "$success_manifest" "$tmp/bad-trial.tsv" chart-row oracle_trial_semantic_sha256 0000000000000000000000000000000000000000000000000000000000000000
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/bad-trial" --workload-manifest "$tmp/bad-trial.tsv" --run-manifest-group success-test
mutate_field "$success_manifest" "$tmp/bad-argv.tsv" chart-row canonical_argv_sha256 0000000000000000000000000000000000000000000000000000000000000000
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/bad-argv" --workload-manifest "$tmp/bad-argv.tsv" --run-manifest-group success-test

# A valid scale_limit is a successful grammar-exact feasible prefix and is
# closed over its resource plus a selected full lower-bound companion.
scale_exact_argv=$(chart_argv_sha 2 off exact_multisite)
scale_exact_trial=$(trial_sha chart_spr_grammar_exact "$search_sha" "$output_sha" "$scale_exact_argv")
scale_lower_argv=$(chart_argv_sha 2 off lower_bound_heuristic)
scale_lower_trial=$(trial_sha chart_spr_grammar_lower_bound_heuristic "$search_sha" "$output_sha" "$scale_lower_argv")
awk -F '\t' -v OFS='\t' -v exact_argv="$scale_exact_argv" -v exact_trial="$scale_exact_trial" \
  -v lower_argv="$scale_lower_argv" -v lower_trial="$scale_lower_trial" '
  /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  $x["row_id"]=="native-ok" {$x["run_group"]="not-scale"; print; next}
  $x["row_id"]=="chart-row" {
    $x["row_id"]="scale-exact"; $x["run_group"]="scale-test"; $x["workload_name"]="scale-prefix"; $x["fixture_id"]="real_fake"
    $x["method"]="chart_spr_grammar_exact"; $x["acceptance"]="exact_multisite"; $x["objective"]="grammar_exact"
    $x["lazy_policy"]="off"; $x["expected_cache_strategy"]="all_active_patterns"
    $x["expected_outcome"]="scale_limit"; $x["scale_resource"]="timeout_seconds"; $x["scale_limit"]="1"
    $x["scale_largest_candidates"]="2"; $x["scale_largest_top_k"]="0"
    $x["canonical_argv_sha256"]=exact_argv; $x["oracle_trial_semantic_sha256"]=exact_trial
    print
    $x["row_id"]="scale-lower"; $x["workload_name"]="scale-lower"; $x["method"]="chart_spr_grammar_lower_bound_heuristic"
    $x["acceptance"]="lower_bound_heuristic"; $x["objective"]="composite_lower_bound_heuristic"; $x["expected_outcome"]="ok"
    $x["scale_resource"]="-"; $x["scale_limit"]="-"; $x["scale_largest_candidates"]="-"; $x["scale_largest_top_k"]="-"
    $x["canonical_argv_sha256"]=lower_argv; $x["oracle_trial_semantic_sha256"]=lower_trial
    print; next
  }
  {print}' "$success_manifest" >"$tmp/scale.tsv"
seal_manifest "$tmp/scale.tsv"
"$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" \
  --out-dir "$tmp/scale" --workload-manifest "$tmp/scale.tsv" --run-manifest-group scale-test >/dev/null
[[ $(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]=="scale-exact"{print $h["status"]}' "$tmp/scale/raw_trials.tsv") == scale_limit ]]
mutate_field "$tmp/scale.tsv" "$tmp/scale-seedtree.tsv" scale-exact fixture_id seedtree
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/scale-seedtree" --workload-manifest "$tmp/scale-seedtree.tsv" --run-manifest-group scale-test
mutate_field "$tmp/scale.tsv" "$tmp/scale-resource.tsv" scale-exact scale_resource gpu_time
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/scale-resource" --workload-manifest "$tmp/scale-resource.tsv" --run-manifest-group scale-test
mutate_field "$tmp/scale.tsv" "$tmp/scale-missing.tsv" scale-lower run_group not-scale
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/scale-missing" --workload-manifest "$tmp/scale-missing.tsv" --run-manifest-group scale-test
mutate_field "$tmp/scale.tsv" "$tmp/scale-zero-companion.tsv" scale-lower chart_max_candidates 0
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/scale-zero-companion" --workload-manifest "$tmp/scale-zero-companion.tsv" --run-manifest-group scale-test

# Labels are not fixture identity: a different input with the same fixture_id
# cannot close the companion contract.
printf 'different fixture\n' >"$tmp/input2.pb.gz"
input2_sha=$(sha256sum "$tmp/input2.pb.gz" | awk '{print $1}')
mutate_field "$tmp/scale.tsv" "$tmp/scale-input-1.tsv" scale-lower primary_uri manifest://input2.pb.gz
mutate_field "$tmp/scale-input-1.tsv" "$tmp/scale-wrong-input.tsv" scale-lower primary_sha256 "$input2_sha"
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/scale-wrong-input" --workload-manifest "$tmp/scale-wrong-input.tsv" --run-manifest-group scale-test

# Renaming the frozen seedtree bytes must not evade its absolute scale-limit
# prohibition.
cp "$repo_root/data/seedtree/seedtree.pb.gz" "$tmp/renamed.pb.gz"
seedtree_sha=$(sha256sum "$tmp/renamed.pb.gz" | awk '{print $1}')
mutate_field "$tmp/scale.tsv" "$tmp/scale-seedbytes-1.tsv" scale-exact primary_uri manifest://renamed.pb.gz
mutate_field "$tmp/scale-seedbytes-1.tsv" "$tmp/scale-seedbytes-2.tsv" scale-exact primary_sha256 "$seedtree_sha"
mutate_field "$tmp/scale-seedbytes-2.tsv" "$tmp/scale-seedbytes-3.tsv" scale-lower primary_uri manifest://renamed.pb.gz
mutate_field "$tmp/scale-seedbytes-3.tsv" "$tmp/scale-seedbytes.tsv" scale-lower primary_sha256 "$seedtree_sha"
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/scale-seedbytes" --workload-manifest "$tmp/scale-seedbytes.tsv" --run-manifest-group scale-test

# A duplicate logical native row cannot satisfy the exact paired join.
awk -F '\t' -v OFS='\t' '
  /^#/{print;next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;print;next}
  {print; if($x["row_id"]=="native-ok"){$x["row_id"]="native-duplicate";print}}' \
  "$success_manifest" >"$tmp/duplicate-pair.tsv"
seal_manifest "$tmp/duplicate-pair.tsv"
expect_fail "$harness" --dagutil "$tmp/dagutil" --larch2 "$tmp/larch2" --process-metrics "$runner" --out-dir "$tmp/duplicate-pair" --workload-manifest "$tmp/duplicate-pair.tsv" --run-manifest-group success-test --require-wall-ratio chart_spr_grammar_lower_bound_heuristic@default=1.0

echo "wric_spr_search_benchmark_harness_test: PASS"

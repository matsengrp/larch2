#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dagutil=${DAGUTIL:-"$repo_root/build/wric-asan/bin/dagutil"}
larch2=${LARCH2:-"$repo_root/build/wric-asan/bin/larch2"}
out_dir=${WRIC_SPR_SEARCH_BENCHMARK_OUT:-"$repo_root/build/wric-spr-search-benchmark"}
iterations=${WRIC_SPR_SEARCH_BENCHMARK_ITERATIONS:-1}
seed=${WRIC_SPR_SEARCH_BENCHMARK_SEED:-1}
max_moves=${WRIC_SPR_SEARCH_BENCHMARK_MAX_MOVES:-50}
max_candidates=${WRIC_SPR_SEARCH_BENCHMARK_MAX_CANDIDATES:-32}
top_k_exact=${WRIC_SPR_SEARCH_BENCHMARK_TOP_K_EXACT:-4}
polytomy_mode=${WRIC_SPR_SEARCH_BENCHMARK_POLYTOMY_MODE:-expand-bounded}
polytomy_shapes=${WRIC_SPR_SEARCH_BENCHMARK_POLYTOMY_SHAPES:-1}
local_workers=${WRIC_SPR_SEARCH_BENCHMARK_LOCAL_WORKERS:-1}
include_heuristic=${WRIC_SPR_SEARCH_BENCHMARK_INCLUDE_HEURISTIC:-0}
include_data_fixtures=${WRIC_SPR_SEARCH_INCLUDE_DATA_FIXTURES:-0}
extra_dag_pbs=${WRIC_SPR_SEARCH_EXTRA_DAG_PBS:-""}
extra_tree_pbs=${WRIC_SPR_SEARCH_EXTRA_TREE_PBS:-""}
mode_list=${WRIC_SPR_SEARCH_BENCHMARK_MODES:-"sampled_tree_fixed grammar_exact hybrid_exact"}
smoke=0

usage() {
  cat <<'USAGE'
wric_spr_search_benchmark.sh -- compare sample--explore--merge to DAG-native chart-SPR

Options:
  --dagutil PATH          dagutil binary (default: DAGUTIL env or build/wric-asan/bin/dagutil)
  --larch2 PATH           larch2 binary (default: LARCH2 env or build/wric-asan/bin/larch2)
  --out-dir DIR           output/report directory
  --iterations N          iteration budget for both methods (default: 1)
  --seed N                RNG seed for both methods (default: 1)
  --max-moves N           larch2 native max moves per iteration (default: 50)
  --max-candidates N      chart-SPR candidate cap per run (default: 32)
  --top-k-exact N         chart-SPR lower-bound top-K exact verification (default: 4)
  --local-workers N       chart-SPR local scoring workers (default: 1)
  --polytomy-mode MODE    dagutil WRIC polytomy mode (default: expand-bounded)
  --polytomy-shapes N     bounded refinement seed-shape cap (default: 1)
  --modes LIST            chart modes: sampled_tree_fixed grammar_exact hybrid_exact grammar_lower_bound
  --include-heuristic     include grammar_lower_bound even if not in --modes
  --include-data-fixtures include repo seedtree/20D fixtures when present
  --dag PATH              add DAG fixture
  --tree PATH:REFSEQ      add tree fixture with refseq
  --smoke                 quick CI smoke: small fixture, one lower-bound chart mode
  -h, --help              show this help

Environment mirrors the defaults above with WRIC_SPR_SEARCH_BENCHMARK_* names.
Extra fixtures can also be supplied via WRIC_SPR_SEARCH_EXTRA_DAG_PBS and
WRIC_SPR_SEARCH_EXTRA_TREE_PBS (space-separated PATH:REFSEQ entries).
USAGE
}

fixture_specs=()
user_fixtures=0

add_dag_fixture() {
  local label=$1
  local path=$2
  fixture_specs+=("$label|dag|$path|")
}

add_tree_fixture() {
  local label=$1
  local path=$2
  local refseq=$3
  fixture_specs+=("$label|tree|$path|$refseq")
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dagutil) dagutil=$2; shift 2 ;;
    --larch2) larch2=$2; shift 2 ;;
    --out-dir) out_dir=$2; shift 2 ;;
    --iterations) iterations=$2; shift 2 ;;
    --seed) seed=$2; shift 2 ;;
    --max-moves) max_moves=$2; shift 2 ;;
    --max-candidates) max_candidates=$2; shift 2 ;;
    --top-k-exact) top_k_exact=$2; shift 2 ;;
    --local-workers) local_workers=$2; shift 2 ;;
    --polytomy-mode) polytomy_mode=$2; shift 2 ;;
    --polytomy-shapes) polytomy_shapes=$2; shift 2 ;;
    --modes) mode_list=$2; shift 2 ;;
    --include-heuristic) include_heuristic=1; shift ;;
    --include-data-fixtures) include_data_fixtures=1; shift ;;
    --dag)
      user_fixtures=1
      path=$2
      label=$(basename "$path")
      label=${label//[^A-Za-z0-9_.-]/_}
      add_dag_fixture "$label" "$path"
      shift 2
      ;;
    --tree)
      user_fixtures=1
      spec=$2
      if [[ "$spec" != *:* ]]; then
        echo "error: --tree entries must be PATH:REFSEQ" >&2
        exit 1
      fi
      path=${spec%%:*}
      ref=${spec#*:}
      label=$(basename "$path")
      label=${label//[^A-Za-z0-9_.-]/_}
      add_tree_fixture "$label" "$path" "$ref"
      shift 2
      ;;
    --smoke)
      smoke=1
      iterations=1
      max_moves=1
      max_candidates=1
      top_k_exact=0
      mode_list="grammar_lower_bound"
      include_heuristic=0
      include_data_fixtures=0
      shift
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ ! -x "$dagutil" ]]; then
  echo "error: dagutil binary not found/executable: $dagutil" >&2
  exit 1
fi
if [[ ! -x "$larch2" ]]; then
  echo "error: larch2 binary not found/executable: $larch2" >&2
  exit 1
fi

if [[ $user_fixtures -eq 0 ]]; then
  add_dag_fixture "small_test_5_tree0" "$repo_root/data/test_5_trees/tree_0.pb.gz"
fi

if [[ "$include_data_fixtures" != 0 ]]; then
  seedtree="$repo_root/data/seedtree/seedtree.pb.gz"
  seedref="$repo_root/data/seedtree/refseq.txt.gz"
  if [[ -f "$seedtree" && -f "$seedref" ]]; then
    add_tree_fixture "medium_seedtree" "$seedtree" "$seedref"
  fi
  tree20d="$repo_root/data/20D_from_fasta/1final-tree-1.nh1.pb.gz"
  ref20d="$repo_root/data/20D_from_fasta/refseq.txt"
  if [[ -f "$tree20d" && -f "$ref20d" ]]; then
    add_tree_fixture "real_20D_one_tree" "$tree20d" "$ref20d"
  fi
fi

for dag_pb in $extra_dag_pbs; do
  label=$(basename "$dag_pb")
  label=${label//[^A-Za-z0-9_.-]/_}
  add_dag_fixture "extra_${label}" "$dag_pb"
done
for tree_spec in $extra_tree_pbs; do
  if [[ "$tree_spec" != *:* ]]; then
    echo "error: WRIC_SPR_SEARCH_EXTRA_TREE_PBS entries must be PATH:REFSEQ" >&2
    exit 1
  fi
  tree_pb=${tree_spec%%:*}
  refseq=${tree_spec#*:}
  label=$(basename "$tree_pb")
  label=${label//[^A-Za-z0-9_.-]/_}
  add_tree_fixture "extra_${label}" "$tree_pb" "$refseq"
done

if [[ "$include_heuristic" != 0 && " $mode_list " != *" grammar_lower_bound "* ]]; then
  mode_list="$mode_list grammar_lower_bound"
fi

mkdir -p "$out_dir" "$out_dir/logs" "$out_dir/outputs" "$out_dir/curves"
summary_tsv="$out_dir/summary.tsv"
summary_md="$out_dir/summary.md"
commands_log="$out_dir/commands.sh"
: >"$commands_log"

extract_value() {
  local file=$1
  local key=$2
  sed -n "s/^[[:space:]]*${key}:[[:space:]]*//p" "$file" | head -n 1
}

extract_counter_value() {
  local file=$1
  local key=$2
  awk -v key="$key" '
    /^[[:space:]]*counters:[[:space:]]*$/ { in_counters=1; next }
    in_counters && $0 ~ "^[[:space:]]*" key ":[[:space:]]*" {
      sub("^[[:space:]]*" key ":[[:space:]]*", ""); print; exit
    }
  ' "$file"
}

extract_parsimony_min() {
  local file=$1
  sed -n 's/.*parsimony_min: score:\([0-9][0-9]*\).*/\1/p' "$file" | head -n 1
}

run_capture() {
  local label=$1
  local stdout=$2
  local stderr=$3
  shift 3
  printf '# %s\n' "$label" >>"$commands_log"
  printf '%q ' "$@" >>"$commands_log"
  printf '\n\n' >>"$commands_log"
  local start_ns stop_ns status
  start_ns=$(date +%s%N)
  set +e
  "$@" >"$stdout" 2>"$stderr"
  status=$?
  set -e
  stop_ns=$(date +%s%N)
  RUN_STATUS=$status
  RUN_WALL_S=$(awk -v start="$start_ns" -v stop="$stop_ns" \
    'BEGIN { printf "%.6f", (stop - start) / 1000000000.0 }')
}

fixture_input_args() {
  local kind=$1
  local input=$2
  local refseq=$3
  if [[ "$kind" == "dag" ]]; then
    printf '%s\0%s\0' --dag-pb "$input"
  else
    printf '%s\0%s\0%s\0%s\0' --tree-pb "$input" --refseq "$refseq"
  fi
}

read_null_args() {
  local -n dst=$1
  dst=()
  while IFS= read -r -d '' item; do
    dst+=("$item")
  done
}

score_input_or_output() {
  local kind=$1
  local input=$2
  local refseq=$3
  local label=$4
  local out=$5
  local err=$6
  local args=()
  if [[ "$kind" == "dag" ]]; then
    args=(--dag-pb "$input")
  else
    args=(--tree-pb "$input" --refseq "$refseq")
  fi
  run_capture "$label" "$out" "$err" \
    "$dagutil" "${args[@]}" --force-no-vcf --validate --dag-info
  SCORE_STATUS=$RUN_STATUS
  SCORE_WALL_S=$RUN_WALL_S
}

min_score_or_na() {
  local lhs=${1:-NA}
  local rhs=${2:-NA}
  awk -v lhs="$lhs" -v rhs="$rhs" '
    function numeric(x) { return x ~ /^[0-9]+([.][0-9]+)?$/ }
    BEGIN {
      if (numeric(lhs) && numeric(rhs)) print (lhs + 0 <= rhs + 0 ? lhs : rhs);
      else if (numeric(lhs)) print lhs;
      else if (numeric(rhs)) print rhs;
      else print "NA";
    }
  '
}

write_baseline_curve() {
  local stderr=$1
  local initial_score=$2
  local curve=$3
  local wall_s=$4
  local final_validated=${5:-NA}
  printf 'iteration\telapsed_s\treported_objective\texternal_validated_parsimony_min\n0\t0.000000\t%s\t%s\n' \
    "${initial_score:-NA}" "${initial_score:-NA}" >"$curve"

  # larch2's native summary reports the sampled tree's objective for each
  # iteration, not the final merged DAG's minimum parsimony.  The benchmark is
  # comparing output DAG quality, so use the externally validated final output
  # score for the baseline curve endpoint when validation succeeded.
  if [[ "$final_validated" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    printf '%s\t%.6f\t%s\t%s\n' \
      "$iterations" "$wall_s" "$final_validated" "$final_validated" \
      >>"$curve"
    return
  fi

  awk -v wall_s="$wall_s" '
    /iter [0-9]+: parsimony=/ {
      iter = $2;
      sub(":", "", iter);
      score = $3;
      sub("parsimony=", "", score);
      rows[++n] = iter "\t" score;
      max_iter = iter;
    }
    END {
      for (i = 1; i <= n; ++i) {
        split(rows[i], parts, "\t");
        iter = parts[1] + 0;
        elapsed = (max_iter > 0) ? wall_s * iter / max_iter : wall_s;
        printf "%d\t%.6f\t%s\tNA\n", iter, elapsed, parts[2];
      }
    }
  ' "$stderr" >>"$curve"
}

write_chart_curve() {
  local report=$1
  local initial_score=$2
  local curve=$3
  local wall_s=$4
  local final_validated=${5:-NA}
  printf 'iteration\telapsed_s\treported_objective\texternal_validated_parsimony_min\n0\t0.000000\t%s\t%s\n' \
    "${initial_score:-NA}" "${initial_score:-NA}" >"$curve"
  awk -v wall_s="$wall_s" -v final_validated="$final_validated" '
    /^[[:space:]]*- iteration:/ { iter = $3 + 1 }
    /^[[:space:]]*state_score_after:/ && iter != "" {
      iters[++n] = iter;
      scores[n] = $2;
      if (iter > max_iter) max_iter = iter;
    }
    END {
      for (i = 1; i <= n; ++i) {
        elapsed = (max_iter > 0) ? wall_s * iters[i] / max_iter : wall_s;
        external = (i == n) ? final_validated : "NA";
        printf "%d\t%.6f\t%s\t%s\n", iters[i], elapsed, scores[i], external;
      }
    }
  ' "$report" >>"$curve"
}

printf 'benchmark_scope\tfixture\tmethod\tstatus\tvalidation_status\tinitial_validated_parsimony_min\tfinal_validated_parsimony_min\tbest_reported_objective\tbest_validated_parsimony_min\twall_clock_s\titerations\tseed\tacceptance\tcandidate_selection\tcandidate_source\tobjective\tcandidates_generated\tcandidates_scored\texact_verifications\taccepted_moves\tcandidate_accepts_attempted\tpost_materialization_rejections\tcommitted_attempt_ratio\tcache_build_ms\tlocal_scoring_ms\texact_verification_ms\taccepted_rebuild_ms\tpost_materialization_check_ms\ttotal_ms\tlocal_ms_per_candidate\tlocal_candidates_per_second\taffected_mean\taffected_p50\taffected_p95\taffected_max\tupward_path_iterator_steps\tpath_pairs_considered\tcandidates_pruned_before_construction\tcandidates_pruned_after_construction\tcandidates_generated_after_dedup\tcandidate_cap_cutoffs\tpath_budget_cutoffs\tfull_search_state_rebuilds\tinitial_search_state_rebuilds\tsidecar_rebuilds_after_accept\toverlay_materializations_for_exact_verification\toverlay_materializations_for_accept_materialization\toverlay_materializations_for_oracle\tfull_overlay_materializations\treachable_clades_traversed\treachable_productions_traversed\treachability_full_grammar_like_passes\tgrammar_clades\tgrammar_productions\tactive_patterns\tchart_cache_resident_bytes\tfinal_dag_nodes\tfinal_dag_edges\tcurve_path\treport_path\n' >"$summary_tsv"

append_row() {
  printf '%s' "CHART_SPR_PHASE8_SEARCH_COMPARISON" >>"$summary_tsv"
  local value
  for value in "$@"; do
    printf '\t%s' "$value" >>"$summary_tsv"
  done
  printf '\n' >>"$summary_tsv"
}

for spec in "${fixture_specs[@]}"; do
  IFS='|' read -r fixture kind input refseq <<<"$spec"
  fixture_safe=${fixture//[^A-Za-z0-9_.-]/_}

  initial_out="$out_dir/logs/${fixture_safe}_initial_score.out"
  initial_err="$out_dir/logs/${fixture_safe}_initial_score.err"
  score_input_or_output "$kind" "$input" "$refseq" \
    "$fixture initial score" "$initial_out" "$initial_err"
  initial_status=$SCORE_STATUS
  initial_score=$(extract_parsimony_min "$initial_out")
  if [[ $initial_status -ne 0 ]]; then
    echo "warning: initial scoring failed for $fixture; see $initial_err" >&2
    initial_score="NA"
  fi

  # Baseline: current larch2 sample--explore--merge/native path.
  baseline_out_pb="$out_dir/outputs/${fixture_safe}_sample_explore_merge.pb.gz"
  baseline_stdout="$out_dir/logs/${fixture_safe}_sample_explore_merge.out"
  baseline_stderr="$out_dir/logs/${fixture_safe}_sample_explore_merge.err"
  baseline_args=()
  if [[ "$kind" == "dag" ]]; then
    baseline_args=(--dag-pb "$input")
  else
    baseline_args=(--tree-pb "$input" --refseq "$refseq")
  fi
  run_capture "$fixture sample--explore--merge" "$baseline_stdout" "$baseline_stderr" \
    "$larch2" "${baseline_args[@]}" -o "$baseline_out_pb" \
    -n "$iterations" --max-moves "$max_moves" --seed "$seed" --validate
  baseline_status=$RUN_STATUS
  baseline_wall_s=$RUN_WALL_S
  baseline_score_out="$out_dir/logs/${fixture_safe}_sample_explore_merge_score.out"
  baseline_score_err="$out_dir/logs/${fixture_safe}_sample_explore_merge_score.err"
  validation_status="failed"
  final_score="NA"
  final_nodes="NA"
  final_edges="NA"
  if [[ $baseline_status -eq 0 && -f "$baseline_out_pb" ]]; then
    score_input_or_output dag "$baseline_out_pb" "" \
      "$fixture sample--explore--merge validation score" \
      "$baseline_score_out" "$baseline_score_err"
    if [[ $SCORE_STATUS -eq 0 ]]; then
      validation_status="ok"
      final_score=$(extract_parsimony_min "$baseline_score_out")
      final_nodes=$(extract_value "$baseline_score_out" nodes)
      final_edges=$(extract_value "$baseline_score_out" edges)
    fi
  fi
  if [[ $baseline_status -ne 0 ]]; then
    method_status="failed"
  else
    method_status="ok"
  fi
  baseline_curve="$out_dir/curves/${fixture_safe}_sample_explore_merge.tsv"
  write_baseline_curve "$baseline_stderr" "$initial_score" "$baseline_curve" \
    "$baseline_wall_s" "$final_score"
  best_reported_objective=$final_score
  if [[ -s "$baseline_curve" ]]; then
    best_reported_objective=$(awk 'NR>1 && $3 != "NA" { if (best == "" || $3 < best) best=$3 } END { if (best == "") print "NA"; else print best }' "$baseline_curve")
  fi
  best_validated_score=$(min_score_or_na "$initial_score" "$final_score")
  append_row "$fixture" "sample_explore_merge" "$method_status" "$validation_status" \
    "${initial_score:-NA}" "${final_score:-NA}" \
    "${best_reported_objective:-NA}" "${best_validated_score:-NA}" \
    "$baseline_wall_s" \
    "$iterations" "$seed" "sample_explore_merge" "native_best_moves" "sampled_tree" \
    "parsimony_sampling" "NA" "NA" "NA" "NA" "NA" "NA" "NA" \
    "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" \
    "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" \
    "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" "NA" \
    "${final_nodes:-NA}" "${final_edges:-NA}" "$baseline_curve" "$baseline_stderr"

  for mode in $mode_list; do
    case "$mode" in
      sampled_tree_fixed)
        method="chart_spr_sampled_tree_fixed_topology"
        source="sampled-tree"
        acceptance="fixed-topology"
        objective="fixed_topology_exact"
        ;;
      grammar_exact)
        method="chart_spr_grammar_exact"
        source="grammar"
        acceptance="exact"
        objective="grammar_exact"
        ;;
      hybrid_exact)
        method="chart_spr_hybrid_exact"
        source="hybrid"
        acceptance="exact"
        objective="grammar_exact"
        ;;
      grammar_lower_bound)
        method="chart_spr_grammar_lower_bound_heuristic"
        source="grammar"
        acceptance="lower-bound"
        objective="composite_lower_bound_heuristic"
        ;;
      *)
        echo "warning: unknown chart-SPR benchmark mode '$mode' (skipping)" >&2
        continue
        ;;
    esac

    out_pb="$out_dir/outputs/${fixture_safe}_${method}.pb.gz"
    report="$out_dir/logs/${fixture_safe}_${method}.out"
    err="$out_dir/logs/${fixture_safe}_${method}.err"
    chart_args=()
    if [[ "$kind" == "dag" ]]; then
      chart_args=(--dag-pb "$input")
    else
      chart_args=(--tree-pb "$input" --refseq "$refseq")
    fi
    run_capture "$fixture $method" "$report" "$err" \
      "$dagutil" "${chart_args[@]}" --force-no-vcf --validate \
      --wric-polytomy-mode "$polytomy_mode" \
      --wric-polytomy-max-shapes "$polytomy_shapes" \
      --chart-spr-search \
      --chart-spr-max-iterations "$iterations" \
      --chart-spr-max-candidates "$max_candidates" \
      --chart-spr-top-k-exact "$top_k_exact" \
      --chart-spr-candidate-selection lower-bound-top-k \
      --chart-spr-candidate-source "$source" \
      --chart-spr-acceptance "$acceptance" \
      --chart-spr-local-score-workers "$local_workers" \
      --seed "$seed" \
      -o "$out_pb"
    chart_status=$RUN_STATUS
    chart_wall_s=$RUN_WALL_S

    score_out="$out_dir/logs/${fixture_safe}_${method}_score.out"
    score_err="$out_dir/logs/${fixture_safe}_${method}_score.err"
    validation_status="failed"
    final_score="NA"
    final_nodes="NA"
    final_edges="NA"
    if [[ $chart_status -eq 0 && -f "$out_pb" ]]; then
      score_input_or_output dag "$out_pb" "" \
        "$fixture $method validation score" "$score_out" "$score_err"
      if [[ $SCORE_STATUS -eq 0 ]]; then
        validation_status="ok"
        final_score=$(extract_parsimony_min "$score_out")
        final_nodes=$(extract_value "$score_out" nodes)
        final_edges=$(extract_value "$score_out" edges)
      fi
    fi
    if [[ $chart_status -ne 0 ]]; then
      method_status="failed"
    else
      method_status="ok"
    fi

    curve="$out_dir/curves/${fixture_safe}_${method}.tsv"
    write_chart_curve "$report" "$initial_score" "$curve" \
      "$chart_wall_s" "$final_score"
    best_reported_objective=$final_score
    if [[ -s "$curve" ]]; then
      best_reported_objective=$(awk 'NR>1 && $3 != "NA" { if (best == "" || $3 < best) best=$3 } END { if (best == "") print "NA"; else print best }' "$curve")
    fi
    best_validated_score=$(min_score_or_na "$initial_score" "$final_score")

    accepted_moves=$(extract_value "$report" accepted_moves)
    candidate_attempts=$(extract_value "$report" candidate_accepts_attempted)
    post_rejects=$(extract_value "$report" post_materialization_rejections)
    committed_attempt_ratio="NA"
    if [[ -n "${candidate_attempts:-}" && "$candidate_attempts" != 0 ]]; then
      committed_attempt_ratio=$(awk -v a="${accepted_moves:-0}" -v b="$candidate_attempts" 'BEGIN { printf "%.6f", a / b }')
    fi
    candidates_scored=$(extract_value "$report" candidates_scored)
    local_ms=$(extract_value "$report" local_scoring_ms)
    local_ms_per_candidate="NA"
    if [[ -n "${candidates_scored:-}" && "$candidates_scored" != 0 && -n "${local_ms:-}" ]]; then
      local_ms_per_candidate=$(awk -v ms="$local_ms" -v n="$candidates_scored" 'BEGIN { printf "%.6f", ms / n }')
    fi

    append_row "$fixture" "$method" "$method_status" "$validation_status" \
      "${initial_score:-NA}" "${final_score:-NA}" \
      "${best_reported_objective:-NA}" "${best_validated_score:-NA}" \
      "$chart_wall_s" \
      "$(extract_value "$report" iterations)" "$seed" \
      "$(extract_value "$report" acceptance)" \
      "$(extract_value "$report" candidate_selection)" \
      "$(extract_value "$report" candidate_source)" \
      "$objective" \
      "$(extract_value "$report" candidates_generated)" \
      "${candidates_scored:-NA}" \
      "$(extract_value "$report" exact_verifications)" \
      "${accepted_moves:-NA}" \
      "${candidate_attempts:-NA}" \
      "${post_rejects:-NA}" \
      "$committed_attempt_ratio" \
      "$(extract_value "$report" cache_build_ms)" \
      "${local_ms:-NA}" \
      "$(extract_value "$report" exact_verification_ms)" \
      "$(extract_value "$report" accepted_rebuild_ms)" \
      "$(extract_value "$report" post_materialization_check_ms)" \
      "$(extract_value "$report" total_ms)" \
      "$local_ms_per_candidate" \
      "$(extract_value "$report" local_candidates_per_second)" \
      "$(extract_value "$report" mean)" \
      "$(extract_value "$report" p50)" \
      "$(extract_value "$report" p95)" \
      "$(extract_value "$report" max)" \
      "$(extract_counter_value "$report" upward_path_iterator_steps)" \
      "$(extract_counter_value "$report" path_pairs_considered)" \
      "$(extract_counter_value "$report" candidates_pruned_before_construction)" \
      "$(extract_counter_value "$report" candidates_pruned_after_construction)" \
      "$(extract_counter_value "$report" candidates_generated_after_dedup)" \
      "$(extract_counter_value "$report" candidate_cap_cutoffs)" \
      "$(extract_counter_value "$report" path_budget_cutoffs)" \
      "$(extract_value "$report" full_search_state_rebuilds)" \
      "$(extract_value "$report" initial_search_state_rebuilds)" \
      "$(extract_value "$report" sidecar_rebuilds_after_accept)" \
      "$(extract_value "$report" overlay_materializations_for_exact_verification)" \
      "$(extract_value "$report" overlay_materializations_for_accept_materialization)" \
      "$(extract_counter_value "$report" overlay_materializations_for_oracle)" \
      "$(extract_counter_value "$report" full_overlay_materializations)" \
      "$(extract_counter_value "$report" reachable_clades_traversed)" \
      "$(extract_counter_value "$report" reachable_productions_traversed)" \
      "$(extract_counter_value "$report" reachability_full_grammar_like_passes)" \
      "$(extract_value "$report" final_grammar_clades)" \
      "$(extract_value "$report" final_grammar_productions)" \
      "$(extract_value "$report" active_patterns)" \
      "$(extract_value "$report" chart_cache_resident_bytes)" \
      "${final_nodes:-NA}" "${final_edges:-NA}" "$curve" "$report"
  done
done

{
  echo "# WRIC chart-SPR search benchmark"
  echo
  echo "benchmark_scope: CHART_SPR_PHASE8_SEARCH_COMPARISON"
  echo
  echo "Configuration: iterations=$iterations, seed=$seed, max_moves=$max_moves, max_candidates=$max_candidates, top_k_exact=$top_k_exact, polytomy_mode=$polytomy_mode, polytomy_shapes=$polytomy_shapes, local_workers=$local_workers."
  echo
  echo "| fixture | method | status | validation | initial_parsimony | final_validated_parsimony | best_reported_objective | best_validated_parsimony | wall_s | acceptance | candidate_source | candidates_scored | exact_verifications | accepted/attempted | total_ms | report |"
  echo "|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|---:|---:|---:|---:|---|"
  awk -F '\t' 'NR == 1 { next } {
    attempted = $21; accepted = $20;
    aa = (accepted == "NA" || attempted == "NA") ? "NA" : accepted "/" attempted;
    printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | `%s` |\n", $2, $3, $4, $5, $6, $7, $8, $9, $10, $13, $15, $18, $19, aa, $29, $60;
  }' "$summary_tsv"
  echo
  echo "Full TSV: $summary_tsv"
  echo "Commands: $commands_log"
  echo "Curves: $out_dir/curves"
} >"$summary_md"

cat "$summary_md"

if [[ "$smoke" != 0 ]]; then
  smoke_check=$(awk -F '\t' '
    NR == 1 { next }
    {
      ++rows;
      fixtures[$2] = 1;
      seen[$2 "\034" $3] = 1;
      if ($4 != "ok" || $5 != "ok") {
        bad = bad sprintf("%s/%s status=%s validation=%s\n", $2, $3, $4, $5);
      }
    }
    END {
      for (fixture in fixtures) {
        if (!(fixture "\034" "sample_explore_merge" in seen)) {
          bad = bad sprintf("%s/sample_explore_merge missing\n", fixture);
        }
        if (!(fixture "\034" "chart_spr_grammar_lower_bound_heuristic" in seen)) {
          bad = bad sprintf("%s/chart_spr_grammar_lower_bound_heuristic missing\n", fixture);
        }
      }
      if (rows == 0) bad = bad "no benchmark rows emitted\n";
      if (bad != "") {
        printf "%s", bad;
        exit 1;
      }
    }
  ' "$summary_tsv") || {
    echo "smoke benchmark failed required-row validation:" >&2
    printf '%s' "$smoke_check" >&2
    exit 1
  }
fi

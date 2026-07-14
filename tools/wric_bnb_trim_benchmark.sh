#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dagutil=${DAGUTIL:-"$repo_root/build/wric-asan/bin/dagutil"}
larch2=${LARCH2:-"$repo_root/build/wric-asan/bin/larch2"}
out_dir=${WRIC_BNB_TRIM_BENCHMARK_OUT:-"$repo_root/build/wric-bnb-trim-benchmark"}
mode_list=${WRIC_BNB_TRIM_BENCHMARK_MODES:-"current_trim chart_bnb_score_only chart_bnb_mask chart_bnb_optimal_topology_materialize"}
iterations=${WRIC_BNB_TRIM_BENCHMARK_ITERATIONS:-0}
max_moves=${WRIC_BNB_TRIM_BENCHMARK_MAX_MOVES:-50}
seed=${WRIC_BNB_TRIM_BENCHMARK_SEED:-1}
polytomy_mode=${WRIC_BNB_TRIM_BENCHMARK_POLYTOMY_MODE:-expand-bounded}
polytomy_shapes=${WRIC_BNB_TRIM_BENCHMARK_POLYTOMY_SHAPES:-1}
max_frontier=${WRIC_BNB_TRIM_BENCHMARK_MAX_FRONTIER:-1000}
max_exact_topologies=${WRIC_BNB_TRIM_BENCHMARK_MAX_EXACT_TOPOLOGIES:-100}
score_only_dominance=${WRIC_BNB_TRIM_BENCHMARK_SCORE_ONLY_DOMINANCE:-score-only}
mask_dominance=${WRIC_BNB_TRIM_BENCHMARK_MASK_DOMINANCE:-off}
materialize_dominance=${WRIC_BNB_TRIM_BENCHMARK_MATERIALIZE_DOMINANCE:-off}
score_ua_edge=${WRIC_BNB_TRIM_BENCHMARK_SCORE_UA_EDGE:-0}
include_data_fixtures=${WRIC_BNB_TRIM_INCLUDE_DATA_FIXTURES:-0}
extra_dag_pbs=${WRIC_BNB_TRIM_EXTRA_DAG_PBS:-""}
extra_tree_pbs=${WRIC_BNB_TRIM_EXTRA_TREE_PBS:-""}
extra_fasta_newicks=${WRIC_BNB_TRIM_EXTRA_FASTA_NEWICKS:-""}
strict=${WRIC_BNB_TRIM_BENCHMARK_STRICT:-0}
smoke=0

usage() {
  cat <<'USAGE'
wric_bnb_trim_benchmark.sh -- benchmark exact WRIC chart-B&B trimming and DAG output

Options:
  --dagutil PATH          dagutil binary (default: DAGUTIL env or build/wric-asan/bin/dagutil)
  --larch2 PATH           larch2 binary (default: LARCH2 env or build/wric-asan/bin/larch2)
  --out-dir DIR           output/report directory
  --modes LIST            modes: current_trim chart_bnb_score_only chart_bnb_mask
                          chart_bnb_optimal_topology_materialize
  --iterations N          larch2 current-trim sampling iterations (default: 0)
  --max-moves N           larch2 max moves per iteration (default: 50)
  --seed N                larch2 RNG seed (default: 1)
  --polytomy-mode MODE    dagutil WRIC polytomy mode (default: expand-bounded)
  --polytomy-shapes N     bounded refinement seed-shape cap (default: 1)
  --max-frontier N        chart-B&B frontier cap; 0 omits cap (default: 1000)
  --max-exact-topologies N
                          topology materialization cap; 0 = unlimited (default: 100)
  --score-only-dominance M
                          dominance for chart_bnb_score_only (default: score-only)
  --mask-dominance M      dominance for chart_bnb_mask (default: off; use
                          two-pass-exact-mask to benchmark exact-mask recovery)
  --materialize-dominance M
                          dominance for optimal-topology materialization (default: off)
  --score-ua-edge         include the UA/reference edge in dagutil chart-B&B runs
  --include-data-fixtures include repo seedtree/20D fixtures when present
  --dag PATH              add DAG protobuf fixture
  --tree PATH:REFSEQ      add tree protobuf fixture with reference sequence
  --fasta-newick FASTA:NEWICK:REFSEQ
                          add FASTA+Newick fixture
  --strict                exit nonzero if any selected method fails/unsupported
  --smoke                 quick CI smoke on a generated binary four-taxon fixture
  -h, --help              show this help

Environment mirrors the defaults above with WRIC_BNB_TRIM_BENCHMARK_* names.
Extra fixtures can also be supplied via WRIC_BNB_TRIM_EXTRA_DAG_PBS,
WRIC_BNB_TRIM_EXTRA_TREE_PBS (space-separated PATH:REFSEQ entries), and
WRIC_BNB_TRIM_EXTRA_FASTA_NEWICKS (space-separated FASTA:NEWICK:REFSEQ entries).

Notes:
  The TSV intentionally names the per-pattern chart value
  composite_lower_bound_heuristic. It is a lower-bound/speed diagnostic, not an
  optimization-quality objective. The exact coupled objective is exact_bnb_optimum.
  chart_bnb_mask is direct source-edge production-mask application; fixtures
  that require synthetic polytomy-refinement provenance are reported as
  unsupported for that mode rather than mislabeled topology-exact.
USAGE
}

fixture_specs=()
user_fixtures=0
tmpdirs=()
strict_fail=0
strict_messages=()

cleanup() {
  for dir in "${tmpdirs[@]}"; do
    rm -rf "$dir"
  done
}
trap cleanup EXIT

sanitize_label() {
  local label=$1
  label=${label//[^A-Za-z0-9_.-]/_}
  printf '%s' "$label"
}

add_dag_fixture() {
  local label=$1
  local path=$2
  fixture_specs+=("$label|dag|$path||")
}

add_tree_fixture() {
  local label=$1
  local path=$2
  local refseq=$3
  fixture_specs+=("$label|tree|$path||$refseq")
}

add_fasta_newick_fixture() {
  local label=$1
  local fasta=$2
  local newick=$3
  local refseq=$4
  fixture_specs+=("$label|fasta_newick|$fasta|$newick|$refseq")
}

add_smoke_fixture() {
  local tmpdir
  tmpdir=$(mktemp -d)
  tmpdirs+=("$tmpdir")
  cat >"$tmpdir/ref.fa" <<'EOF'
ACGT
EOF
  cat >"$tmpdir/tiny.fa" <<'EOF'
>A
ACGT
>B
AGGT
>C
TCGT
>D
ACGA
EOF
  cat >"$tmpdir/tiny.nwk" <<'EOF'
((A,B),(C,D));
EOF
  add_fasta_newick_fixture "tiny_binary_four_taxon" \
    "$tmpdir/tiny.fa" "$tmpdir/tiny.nwk" "$tmpdir/ref.fa"
}

parse_colon3() {
  local spec=$1
  local what=$2
  if [[ "$spec" != *:*:* ]]; then
    echo "error: $what entries must be A:B:C" >&2
    exit 1
  fi
  local first=${spec%%:*}
  local rest=${spec#*:}
  local second=${rest%%:*}
  local third=${rest#*:}
  if [[ -z "$first" || -z "$second" || -z "$third" ]]; then
    echo "error: $what entries must not contain empty fields" >&2
    exit 1
  fi
  PARSE_COLON3_FIRST=$first
  PARSE_COLON3_SECOND=$second
  PARSE_COLON3_THIRD=$third
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dagutil) dagutil=$2; shift 2 ;;
    --larch2) larch2=$2; shift 2 ;;
    --out-dir) out_dir=$2; shift 2 ;;
    --modes) mode_list=$2; shift 2 ;;
    --iterations) iterations=$2; shift 2 ;;
    --max-moves) max_moves=$2; shift 2 ;;
    --seed) seed=$2; shift 2 ;;
    --polytomy-mode) polytomy_mode=$2; shift 2 ;;
    --polytomy-shapes) polytomy_shapes=$2; shift 2 ;;
    --max-frontier) max_frontier=$2; shift 2 ;;
    --max-exact-topologies) max_exact_topologies=$2; shift 2 ;;
    --score-only-dominance) score_only_dominance=$2; shift 2 ;;
    --mask-dominance) mask_dominance=$2; shift 2 ;;
    --materialize-dominance) materialize_dominance=$2; shift 2 ;;
    --score-ua-edge) score_ua_edge=1; shift ;;
    --include-data-fixtures) include_data_fixtures=1; shift ;;
    --strict) strict=1; shift ;;
    --dag)
      user_fixtures=1
      path=$2
      label=$(sanitize_label "$(basename "$path")")
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
      label=$(sanitize_label "$(basename "$path")")
      add_tree_fixture "$label" "$path" "$ref"
      shift 2
      ;;
    --fasta-newick)
      user_fixtures=1
      parse_colon3 "$2" "--fasta-newick"
      label=$(sanitize_label "$(basename "$PARSE_COLON3_SECOND")")
      add_fasta_newick_fixture "$label" \
        "$PARSE_COLON3_FIRST" "$PARSE_COLON3_SECOND" \
        "$PARSE_COLON3_THIRD"
      shift 2
      ;;
    --smoke)
      smoke=1
      shift
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ $smoke -ne 0 ]]; then
  fixture_specs=()
  user_fixtures=1
  mode_list="current_trim chart_bnb_score_only chart_bnb_mask chart_bnb_optimal_topology_materialize"
  iterations=0
  max_frontier=1000
  max_exact_topologies=100
  polytomy_mode=reject
  polytomy_shapes=1
  score_only_dominance=score-only
  mask_dominance=off
  materialize_dominance=off
  strict=1
  add_smoke_fixture
fi

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
  label=$(sanitize_label "extra_$(basename "$dag_pb")")
  add_dag_fixture "$label" "$dag_pb"
done
for tree_spec in $extra_tree_pbs; do
  if [[ "$tree_spec" != *:* ]]; then
    echo "error: WRIC_BNB_TRIM_EXTRA_TREE_PBS entries must be PATH:REFSEQ" >&2
    exit 1
  fi
  tree_pb=${tree_spec%%:*}
  refseq=${tree_spec#*:}
  label=$(sanitize_label "extra_$(basename "$tree_pb")")
  add_tree_fixture "$label" "$tree_pb" "$refseq"
done
for fn_spec in $extra_fasta_newicks; do
  parse_colon3 "$fn_spec" "WRIC_BNB_TRIM_EXTRA_FASTA_NEWICKS"
  label=$(sanitize_label "extra_$(basename "$PARSE_COLON3_SECOND")")
  add_fasta_newick_fixture "$label" \
    "$PARSE_COLON3_FIRST" "$PARSE_COLON3_SECOND" \
    "$PARSE_COLON3_THIRD"
done

mkdir -p "$out_dir" "$out_dir/logs" "$out_dir/outputs" "$out_dir/reports"
summary_tsv="$out_dir/summary.tsv"
summary_md="$out_dir/summary.md"
commands_log="$out_dir/commands.sh"
: >"$commands_log"

tsv_header=(
  benchmark_scope fixture method status validation_status unsupported_reason
  input_kind input_path input_nodes input_edges input_tree_count_estimate
  input_validated_parsimony_min grammar_clades grammar_productions
  polytomy_mode refinement_exactness exact_patterns active_patterns
  skipped_invariant_sites invariant_constant_offset kept_productions
  keep_mask_kind keep_production_exact composite_lower_bound_kind
  composite_lower_bound_heuristic greedy_initial_upper_bound exact_bnb_optimum
  validated_output_parsimony_min validated_output_parsimony_min_exact
  output_dag_nodes output_dag_edges output_tree_count_estimate
  output_exactness_mode output_contains_only_optimal_topologies
  identity_preserving_tree_set production_mask_superset grammar_topology_exact
  source_history_topology_exact coupled_frontier_exact frontier_size_sum
  frontier_size_mean frontier_size_p50 frontier_size_p95 frontier_size_max
  frontier_size_histogram equality_deduplicated bound_pruned dominance_mode
  dominance_candidates_considered dominance_pruned_score_pass
  dominance_pruned_mask_pass dominance_pruned exact_mask_recovery_passes
  grammar_build_ms pattern_build_ms chart_outside_ms score_pass_ms mask_pass_ms
  bnb_trim_ms dag_apply_materialize_ms validation_wall_ms wall_clock_s
  frontier_cost_vector_memory_estimate_bytes topology_cap_truncated
  materialized_topologies masked_productions_reappeared validation_oracle
  validation_strength report_path output_path json_report_path
)

write_tsv_header() {
  local IFS=$'\t'
  printf '%s\n' "${tsv_header[*]}" >"$summary_tsv"
}

append_assoc_row() {
  local -n row_ref=$1
  local first=1
  local key value
  for key in "${tsv_header[@]}"; do
    value=${row_ref[$key]:-NA}
    value=${value//$'\t'/ }
    value=${value//$'\n'/ }
    if [[ $first -eq 1 ]]; then
      printf '%s' "$value" >>"$summary_tsv"
      first=0
    else
      printf '\t%s' "$value" >>"$summary_tsv"
    fi
  done
  printf '\n' >>"$summary_tsv"

  if [[ "$strict" != 0 && "${row_ref[status]:-NA}" != "ok" ]]; then
    strict_fail=1
    strict_messages+=("${row_ref[fixture]:-NA}/${row_ref[method]:-NA}: status=${row_ref[status]:-NA} reason=${row_ref[unsupported_reason]:-NA}")
  fi
}

write_tsv_header

extract_value() {
  local file=$1
  local key=$2
  # Per-level diagnostics legitimately repeat aggregate field names.  Select
  # the first report value in one process: under `set -o pipefail`, piping a
  # multi-match sed into `head` makes sed receive SIGPIPE and aborts the
  # benchmark with status 141.
  sed -n "/^[[:space:]]*${key}:[[:space:]]*/ { s/^[[:space:]]*${key}:[[:space:]]*//; p; q; }" "$file"
}

extract_parsimony_min() {
  local file=$1
  sed -n '/parsimony_min: score:[0-9]/ { s/.*parsimony_min: score:\([0-9][0-9]*\).*/\1/; p; q; }' "$file"
}

extract_frontier_histogram() {
  local file=$1
  awk '
    /^[[:space:]]*frontier_size_histogram:[[:space:]]*$/ { in_hist = 1; next }
    in_hist && /^[[:space:]]*[0-9]+:[[:space:]]*[0-9]+[[:space:]]*$/ {
      line = $0;
      sub(/^[[:space:]]*/, "", line);
      sub(/[[:space:]]*$/, "", line);
      gsub(/[[:space:]]*:[[:space:]]*/, ":", line);
      if (out != "") out = out ",";
      out = out line;
    }
    END { print out }
  ' "$file"
}

hist_stat_from_string() {
  local hist=$1
  local stat=$2
  awk -v hist="$hist" -v stat="$stat" '
    function ceil(x) { return (x == int(x)) ? x : int(x) + 1 }
    BEGIN {
      if (hist == "") { print "NA"; exit }
      n = split(hist, pairs, ",");
      for (i = 1; i <= n; ++i) {
        split(pairs[i], kv, ":");
        size = kv[1] + 0;
        count = kv[2] + 0;
        sizes[i] = size;
        counts[i] = count;
        clades += count;
        sum += size * count;
        if (size > max) max = size;
      }
      if (clades == 0) { print "NA"; exit }
      if (stat == "clades") { print clades; exit }
      if (stat == "sum") { print sum; exit }
      if (stat == "mean") { printf "%.6f\n", sum / clades; exit }
      if (stat == "max") { print max; exit }
      p50_target = ceil(0.50 * clades);
      p95_target = ceil(0.95 * clades);
      cumulative = 0;
      p50 = "NA";
      p95 = "NA";
      for (i = 1; i <= n; ++i) {
        cumulative += counts[i];
        if (p50 == "NA" && cumulative >= p50_target) p50 = sizes[i];
        if (p95 == "NA" && cumulative >= p95_target) p95 = sizes[i];
      }
      if (stat == "p50") print p50;
      else if (stat == "p95") print p95;
      else print "NA";
    }
  '
}

is_uint() {
  [[ ${1:-} =~ ^[0-9]+$ ]]
}

estimate_frontier_memory_bytes() {
  local frontier_sum=${1:-NA}
  local active_patterns=${2:-NA}
  if is_uint "$frontier_sum" && is_uint "$active_patterns"; then
    awk -v f="$frontier_sum" -v p="$active_patterns" \
      'BEGIN { printf "%.0f", f * p * 4 * 8 }'
  else
    printf 'NA'
  fi
}

normalise_refinement_exactness() {
  local value=${1:-}
  case "$value" in
    EXACT|FULL_SOFT_POLYTOMY_SPACE) printf 'EXACT' ;;
    BOUNDED_REFINED_GRAMMAR) printf 'BOUNDED_REFINED_GRAMMAR' ;;
    "") printf 'NA' ;;
    *) printf '%s' "$value" ;;
  esac
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
  RUN_WALL_MS=$(awk -v s="$RUN_WALL_S" 'BEGIN { printf "%.3f", s * 1000.0 }')
}

fixture_args() {
  local -n dst=$1
  local kind=$2
  local input=$3
  local aux=$4
  local refseq=$5
  dst=()
  case "$kind" in
    dag) dst=(--dag-pb "$input") ;;
    tree) dst=(--tree-pb "$input" --refseq "$refseq") ;;
    fasta_newick) dst=(--fasta "$input" --newick "$aux" --refseq "$refseq") ;;
    *) echo "error: unknown fixture kind: $kind" >&2; exit 1 ;;
  esac
}

fixture_display_path() {
  local kind=$1
  local input=$2
  local aux=$3
  local refseq=$4
  case "$kind" in
    dag) printf '%s' "$input" ;;
    tree) printf '%s:%s' "$input" "$refseq" ;;
    fasta_newick) printf '%s:%s:%s' "$input" "$aux" "$refseq" ;;
    *) printf '%s' "$input" ;;
  esac
}

chart_common_args() {
  local -n dst=$1
  dst=(--force-no-vcf --wric-polytomy-mode "$polytomy_mode" \
       --wric-polytomy-max-shapes "$polytomy_shapes")
  if [[ "$score_ua_edge" != 0 ]]; then
    dst+=(--chart-score-ua-edge)
  fi
}

append_frontier_cap_args() {
  local -n dst=$1
  if [[ "$max_frontier" != 0 ]]; then
    dst+=(--chart-bnb-max-frontier "$max_frontier")
  fi
}

append_topology_cap_args() {
  local -n dst=$1
  dst+=(--chart-bnb-max-exact-topologies "$max_exact_topologies")
}

validate_output_dag() {
  local path=$1
  local label=$2
  local out=$3
  local err=$4
  VALIDATION_STATUS=failed
  VALIDATION_WALL_MS=NA
  VALIDATED_SCORE=NA
  VALIDATED_NODES=NA
  VALIDATED_EDGES=NA
  VALIDATED_TREE_COUNT=NA
  if [[ ! -f "$path" ]]; then
    return
  fi
  run_capture "$label validation" "$out" "$err" \
    "$dagutil" --dag-pb "$path" --force-no-vcf --validate --dag-info
  VALIDATION_WALL_MS=$RUN_WALL_MS
  if [[ $RUN_STATUS -eq 0 ]]; then
    VALIDATION_STATUS=ok
    VALIDATED_SCORE=$(extract_parsimony_min "$out")
    VALIDATED_NODES=$(extract_value "$out" nodes)
    VALIDATED_EDGES=$(extract_value "$out" edges)
    VALIDATED_TREE_COUNT=$(extract_value "$out" tree_count)
  fi
}

init_common_row() {
  local -n row_ref=$1
  local fixture=$2
  local kind=$3
  local display=$4
  local info_out=$5
  local pattern_out=$6
  row_ref=()
  row_ref[benchmark_scope]=WRIC_BNB_TRIM_PHASE7
  row_ref[fixture]=$fixture
  row_ref[input_kind]=$kind
  row_ref[input_path]=$display
  row_ref[input_nodes]=${INPUT_NODES:-NA}
  row_ref[input_edges]=${INPUT_EDGES:-NA}
  row_ref[input_tree_count_estimate]=${INPUT_TREE_COUNT:-NA}
  row_ref[input_validated_parsimony_min]=${INPUT_SCORE:-NA}
  row_ref[polytomy_mode]=$polytomy_mode
  row_ref[exact_patterns]=$(extract_value "$pattern_out" exact_patterns)
  row_ref[skipped_invariant_sites]=$(extract_value "$pattern_out" skipped_invariant_sites)
  row_ref[refinement_exactness]=$(normalise_refinement_exactness "$(extract_value "$pattern_out" score_scope)")
  row_ref[validation_status]=not_applicable
  row_ref[status]=not_run
  row_ref[unsupported_reason]=NA
  row_ref[composite_lower_bound_kind]=LOWER_BOUND
}

fill_bnb_report_fields() {
  local -n row_ref=$1
  local report=$2
  local hist frontier_sum active_patterns
  hist=$(extract_frontier_histogram "$report")
  frontier_sum=$(hist_stat_from_string "$hist" sum)
  active_patterns=$(extract_value "$report" active_patterns)

  row_ref[grammar_clades]=$(hist_stat_from_string "$hist" clades)
  row_ref[grammar_productions]=$(extract_value "$report" total_productions)
  local exactness
  exactness=$(extract_value "$report" refinement_exactness)
  if [[ -z "$exactness" ]]; then
    exactness=$(extract_value "$report" score_kind)
  fi
  row_ref[refinement_exactness]=$(normalise_refinement_exactness "$exactness")
  row_ref[exact_patterns]=$(extract_value "$report" exact_patterns)
  row_ref[active_patterns]=$active_patterns
  row_ref[invariant_constant_offset]=$(extract_value "$report" invariant_constant_offset)
  row_ref[kept_productions]=$(extract_value "$report" kept_productions)
  row_ref[keep_mask_kind]=$(extract_value "$report" keep_mask_kind)
  row_ref[keep_production_exact]=$(extract_value "$report" keep_production_exact)
  row_ref[composite_lower_bound_kind]=$(extract_value "$report" composite_lower_bound_kind)
  row_ref[composite_lower_bound_heuristic]=$(extract_value "$report" composite_lower_bound)
  row_ref[greedy_initial_upper_bound]=$(extract_value "$report" initial_upper_bound)
  row_ref[exact_bnb_optimum]=$(extract_value "$report" optimum)
  row_ref[equality_deduplicated]=$(extract_value "$report" equality_deduplicated)
  row_ref[bound_pruned]=$(extract_value "$report" bound_pruned)
  row_ref[dominance_mode]=$(extract_value "$report" dominance_mode)
  row_ref[dominance_candidates_considered]=$(extract_value "$report" dominance_candidates_considered)
  row_ref[dominance_pruned_score_pass]=$(extract_value "$report" dominance_pruned_score_pass)
  row_ref[dominance_pruned_mask_pass]=$(extract_value "$report" dominance_pruned_mask_pass)
  row_ref[dominance_pruned]=$(extract_value "$report" dominance_pruned)
  row_ref[exact_mask_recovery_passes]=$(extract_value "$report" exact_mask_recovery_passes)
  row_ref[grammar_build_ms]=$(extract_value "$report" grammar_build_ms)
  row_ref[pattern_build_ms]=$(extract_value "$report" pattern_build_ms)
  row_ref[bnb_trim_ms]=$(extract_value "$report" bnb_trim_ms)
  if [[ "${row_ref[exact_mask_recovery_passes]:-0}" == "0" ]]; then
    row_ref[score_pass_ms]=${row_ref[bnb_trim_ms]:-NA}
    row_ref[mask_pass_ms]=0
  else
    row_ref[score_pass_ms]=NA
    row_ref[mask_pass_ms]=NA
  fi
  row_ref[dag_apply_materialize_ms]=$(extract_value "$report" apply_trim_ms)
  row_ref[frontier_size_sum]=$frontier_sum
  row_ref[frontier_size_mean]=$(hist_stat_from_string "$hist" mean)
  row_ref[frontier_size_p50]=$(hist_stat_from_string "$hist" p50)
  row_ref[frontier_size_p95]=$(hist_stat_from_string "$hist" p95)
  row_ref[frontier_size_max]=$(hist_stat_from_string "$hist" max)
  row_ref[frontier_size_histogram]=${hist:-NA}
  row_ref[frontier_cost_vector_memory_estimate_bytes]=$(estimate_frontier_memory_bytes "$frontier_sum" "$active_patterns")
}

fill_bnb_apply_fields() {
  local -n row_ref=$1
  local report=$2
  row_ref[validated_output_parsimony_min]=$(extract_value "$report" validated_output_parsimony_min)
  row_ref[validated_output_parsimony_min_exact]=$(extract_value "$report" validated_output_parsimony_min_exact)
  row_ref[output_contains_only_optimal_topologies]=$(extract_value "$report" output_contains_only_optimal_topologies)
  row_ref[identity_preserving_tree_set]=$(extract_value "$report" identity_preserving_tree_set)
  row_ref[production_mask_superset]=$(extract_value "$report" production_mask_superset)
  row_ref[grammar_topology_exact]=$(extract_value "$report" grammar_topology_exact)
  row_ref[source_history_topology_exact]=$(extract_value "$report" source_history_topology_exact)
  row_ref[coupled_frontier_exact]=$(extract_value "$report" coupled_frontier_exact)
  row_ref[topology_cap_truncated]=$(extract_value "$report" topology_cap_truncated)
  row_ref[materialized_topologies]=$(extract_value "$report" materialized_topologies)
  row_ref[masked_productions_reappeared]=$(extract_value "$report" masked_productions_reappeared)
  row_ref[validation_oracle]=$(extract_value "$report" validation_oracle)
  row_ref[validation_strength]=$(extract_value "$report" validation_strength)

  if [[ "${row_ref[source_history_topology_exact]:-false}" == "true" ]]; then
    row_ref[output_exactness_mode]=source_history_topology_exact
  elif [[ "${row_ref[grammar_topology_exact]:-false}" == "true" ]]; then
    row_ref[output_exactness_mode]=grammar_topology_exact
  elif [[ "${row_ref[coupled_frontier_exact]:-false}" == "true" ]]; then
    row_ref[output_exactness_mode]=coupled_frontier_exact
  elif [[ "${row_ref[production_mask_superset]:-false}" == "true" ]]; then
    row_ref[output_exactness_mode]=production_mask_superset
  else
    row_ref[output_exactness_mode]=unknown
  fi
}

record_validation_in_row() {
  local -n row_ref=$1
  row_ref[validation_wall_ms]=$VALIDATION_WALL_MS
  row_ref[output_dag_nodes]=$VALIDATED_NODES
  row_ref[output_dag_edges]=$VALIDATED_EDGES
  row_ref[output_tree_count_estimate]=$VALIDATED_TREE_COUNT
  if [[ "${row_ref[validated_output_parsimony_min]:-NA}" == "NA" || -z "${row_ref[validated_output_parsimony_min]:-}" ]]; then
    row_ref[validated_output_parsimony_min]=$VALIDATED_SCORE
  fi
  if [[ "$VALIDATION_STATUS" == ok ]]; then
    row_ref[validation_status]=ok
    if [[ "${row_ref[validated_output_parsimony_min_exact]:-NA}" == "NA" ]]; then
      row_ref[validated_output_parsimony_min_exact]=true
    fi
  else
    row_ref[validation_status]=failed
  fi
}

mark_mask_unsupported_if_witness_failure() {
  local -n row_ref=$1
  local report=$2
  local err=$3
  if grep -Eqi 'production-mask|witness|synthetic polytomy' "$report" "$err"; then
    row_ref[status]=unsupported
    if grep -Eqi 'synthetic polytomy-refinement provenance|synthetic polytomy' "$report" "$err"; then
      row_ref[unsupported_reason]=synthetic_polytomy_refinement_provenance
    elif grep -Eqi 'witness' "$report" "$err"; then
      row_ref[unsupported_reason]=production_mask_apply_not_witness_safe
    else
      row_ref[unsupported_reason]=production_mask_apply_failed
    fi
    row_ref[validation_status]=not_applicable
  fi
}

run_current_trim() {
  local fixture=$1 kind=$2 input=$3 aux=$4 refseq=$5 display=$6 fixture_safe=$7 info_out=$8 pattern_out=$9
  local out_pb="$out_dir/outputs/${fixture_safe}_current_trim.pb.gz"
  local stdout="$out_dir/logs/${fixture_safe}_current_trim.out"
  local stderr="$out_dir/logs/${fixture_safe}_current_trim.err"
  local score_out="$out_dir/logs/${fixture_safe}_current_trim_score.out"
  local score_err="$out_dir/logs/${fixture_safe}_current_trim_score.err"
  local args=()
  fixture_args args "$kind" "$input" "$aux" "$refseq"

  declare -A row
  init_common_row row "$fixture" "$kind" "$display" "$info_out" "$pattern_out"
  row[method]=current_trim
  row[output_exactness_mode]=current_trim_baseline
  row[report_path]=$stderr
  row[output_path]=$out_pb

  run_capture "$fixture current_trim" "$stdout" "$stderr" \
    "$larch2" "${args[@]}" -n "$iterations" --max-moves "$max_moves" \
    --seed "$seed" --trim --validate -o "$out_pb"
  row[wall_clock_s]=$RUN_WALL_S
  if [[ $RUN_STATUS -eq 0 && -f "$out_pb" ]]; then
    row[status]=ok
    validate_output_dag "$out_pb" "$fixture current_trim" "$score_out" "$score_err"
    record_validation_in_row row
    if [[ "${row[validation_status]:-failed}" != "ok" ]]; then
      row[status]=failed
    fi
    row[validation_oracle]=dagutil_dag_info_parsimony_summary
    row[validation_strength]=external_dag_validation_summary
  else
    row[status]=failed
    row[validation_status]=failed
  fi
  append_assoc_row row
}

run_chart_bnb_score_only() {
  local fixture=$1 kind=$2 input=$3 aux=$4 refseq=$5 display=$6 fixture_safe=$7 info_out=$8 pattern_out=$9
  local report="$out_dir/logs/${fixture_safe}_chart_bnb_score_only.out"
  local err="$out_dir/logs/${fixture_safe}_chart_bnb_score_only.err"
  local args=() common=()
  fixture_args args "$kind" "$input" "$aux" "$refseq"
  chart_common_args common
  append_frontier_cap_args common

  declare -A row
  init_common_row row "$fixture" "$kind" "$display" "$info_out" "$pattern_out"
  row[method]=chart_bnb_score_only
  row[output_exactness_mode]=score_only_no_output
  row[report_path]=$report
  row[validation_status]=not_applicable

  run_capture "$fixture chart_bnb_score_only" "$report" "$err" \
    "$dagutil" "${args[@]}" "${common[@]}" --chart-bnb-trim \
    --chart-bnb-dominance "$score_only_dominance" --chart-bnb-score-only
  row[wall_clock_s]=$RUN_WALL_S
  fill_bnb_report_fields row "$report"
  if [[ $RUN_STATUS -eq 0 ]]; then
    row[status]=ok
  else
    row[status]=failed
  fi
  append_assoc_row row
}

run_chart_bnb_apply_mode() {
  local mode=$1 fixture=$2 kind=$3 input=$4 aux=$5 refseq=$6 display=$7 fixture_safe=$8 info_out=$9 pattern_out=${10}
  local app_cli app_label dominance out_pb report err json score_out score_err
  case "$mode" in
    chart_bnb_mask)
      app_cli=production-mask
      app_label=chart_bnb_mask
      dominance=$mask_dominance
      ;;
    chart_bnb_optimal_topology_materialize)
      app_cli=optimal-topology-materialize
      app_label=chart_bnb_optimal_topology_materialize
      dominance=$materialize_dominance
      ;;
    *) echo "error: internal unknown apply mode: $mode" >&2; exit 1 ;;
  esac
  out_pb="$out_dir/outputs/${fixture_safe}_${app_label}.pb.gz"
  report="$out_dir/logs/${fixture_safe}_${app_label}.out"
  err="$out_dir/logs/${fixture_safe}_${app_label}.err"
  json="$out_dir/reports/${fixture_safe}_${app_label}.json"
  score_out="$out_dir/logs/${fixture_safe}_${app_label}_score.out"
  score_err="$out_dir/logs/${fixture_safe}_${app_label}_score.err"

  local args=() common=()
  fixture_args args "$kind" "$input" "$aux" "$refseq"
  chart_common_args common
  append_frontier_cap_args common
  append_topology_cap_args common

  declare -A row
  init_common_row row "$fixture" "$kind" "$display" "$info_out" "$pattern_out"
  row[method]=$app_label
  row[report_path]=$report
  row[output_path]=$out_pb
  row[json_report_path]=$json

  run_capture "$fixture $app_label" "$report" "$err" \
    "$dagutil" "${args[@]}" "${common[@]}" --chart-bnb-trim \
    --chart-bnb-dominance "$dominance" --chart-bnb-apply-trim \
    --chart-bnb-trim-application "$app_cli" \
    --chart-bnb-report-json "$json" -o "$out_pb"
  row[wall_clock_s]=$RUN_WALL_S
  fill_bnb_report_fields row "$report"

  if [[ $RUN_STATUS -eq 0 && -f "$out_pb" ]]; then
    row[status]=ok
    fill_bnb_apply_fields row "$report"
    validate_output_dag "$out_pb" "$fixture $app_label" "$score_out" "$score_err"
    record_validation_in_row row
    if [[ "${row[validation_status]:-failed}" != "ok" ]]; then
      row[status]=failed
    fi
    if is_uint "${row[validated_output_parsimony_min]:-NA}" && is_uint "$VALIDATED_SCORE" && \
       [[ "${row[validated_output_parsimony_min]}" != "$VALIDATED_SCORE" ]]; then
      row[validation_status]=mismatch
      row[status]=failed
      row[unsupported_reason]=apply_validation_mismatch
    fi
  else
    row[status]=failed
    row[validation_status]=failed
    fill_bnb_apply_fields row "$report"
    if [[ "$mode" == "chart_bnb_mask" ]]; then
      mark_mask_unsupported_if_witness_failure row "$report" "$err"
    fi
  fi
  append_assoc_row row
}

for spec in "${fixture_specs[@]}"; do
  IFS='|' read -r fixture kind input aux refseq <<<"$spec"
  fixture_safe=$(sanitize_label "$fixture")
  display=$(fixture_display_path "$kind" "$input" "$aux" "$refseq")

  input_info_out="$out_dir/logs/${fixture_safe}_input_info.out"
  input_info_err="$out_dir/logs/${fixture_safe}_input_info.err"
  pattern_out="$out_dir/logs/${fixture_safe}_pattern_info.out"
  pattern_err="$out_dir/logs/${fixture_safe}_pattern_info.err"

  fixture_args input_args "$kind" "$input" "$aux" "$refseq"
  run_capture "$fixture input dag-info" "$input_info_out" "$input_info_err" \
    "$dagutil" "${input_args[@]}" --force-no-vcf --validate --dag-info
  if [[ $RUN_STATUS -eq 0 ]]; then
    INPUT_NODES=$(extract_value "$input_info_out" nodes)
    INPUT_EDGES=$(extract_value "$input_info_out" edges)
    INPUT_TREE_COUNT=$(extract_value "$input_info_out" tree_count)
    INPUT_SCORE=$(extract_parsimony_min "$input_info_out")
  else
    INPUT_NODES=NA
    INPUT_EDGES=NA
    INPUT_TREE_COUNT=NA
    INPUT_SCORE=NA
  fi

  chart_common_args pattern_common
  run_capture "$fixture chart pattern-info" "$pattern_out" "$pattern_err" \
    "$dagutil" "${input_args[@]}" "${pattern_common[@]}" --chart-pattern-info
  if [[ $RUN_STATUS -ne 0 ]]; then
    : >"$pattern_out"
  fi

  for mode in $mode_list; do
    case "$mode" in
      current_trim)
        run_current_trim "$fixture" "$kind" "$input" "$aux" "$refseq" \
          "$display" "$fixture_safe" "$input_info_out" "$pattern_out"
        ;;
      chart_bnb_score_only)
        run_chart_bnb_score_only "$fixture" "$kind" "$input" "$aux" \
          "$refseq" "$display" "$fixture_safe" "$input_info_out" "$pattern_out"
        ;;
      chart_bnb_mask)
        run_chart_bnb_apply_mode chart_bnb_mask "$fixture" "$kind" "$input" \
          "$aux" "$refseq" "$display" "$fixture_safe" "$input_info_out" \
          "$pattern_out"
        ;;
      chart_bnb_optimal_topology_materialize)
        run_chart_bnb_apply_mode chart_bnb_optimal_topology_materialize \
          "$fixture" "$kind" "$input" "$aux" "$refseq" "$display" \
          "$fixture_safe" "$input_info_out" "$pattern_out"
        ;;
      *)
        echo "warning: unknown WRIC B&B trim benchmark mode '$mode' (skipping)" >&2
        ;;
    esac
  done
done

{
  echo "# WRIC chart-B&B trim benchmark"
  echo
  echo "benchmark_scope: WRIC_BNB_TRIM_PHASE7"
  echo
  echo "Configuration: modes=$mode_list, iterations=$iterations, seed=$seed, polytomy_mode=$polytomy_mode, polytomy_shapes=$polytomy_shapes, max_frontier=$max_frontier, max_exact_topologies=$max_exact_topologies, score_ua_edge=$score_ua_edge."
  echo
  echo "The composite lower-bound column is named \`composite_lower_bound_heuristic\` and is not used as an exact optimization-quality objective. Compare exact runs using \`exact_bnb_optimum\` and output DAG validation columns."
  echo
  echo "| fixture | method | status | validation | exact B&B optimum | output parsimony min | output exactness | dominance | frontier max | wall_s | validation strength | report |"
  echo "|---|---|---:|---:|---:|---:|---|---|---:|---:|---|---|"
  awk -F '\t' '
    NR == 1 { for (i = 1; i <= NF; ++i) h[$i] = i; next }
    {
      printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | `%s` |\n", \
        $h["fixture"], $h["method"], $h["status"], $h["validation_status"], \
        $h["exact_bnb_optimum"], $h["validated_output_parsimony_min"], \
        $h["output_exactness_mode"], $h["dominance_mode"], \
        $h["frontier_size_max"], $h["wall_clock_s"], \
        $h["validation_strength"], $h["report_path"];
    }
  ' "$summary_tsv"
  echo
  echo "Full TSV: $summary_tsv"
  echo "Commands: $commands_log"
  echo "Outputs: $out_dir/outputs"
  echo "JSON reports: $out_dir/reports"
} >"$summary_md"

cat "$summary_md"

if [[ "$smoke" != 0 ]]; then
  smoke_check=$(awk -F '\t' '
    NR == 1 { for (i = 1; i <= NF; ++i) h[$i] = i; next }
    {
      key = $h["fixture"] "\034" $h["method"];
      seen[key] = 1;
      if ($h["status"] != "ok") {
        bad = bad sprintf("%s/%s status=%s reason=%s\n", \
          $h["fixture"], $h["method"], $h["status"], $h["unsupported_reason"]);
      }
      if ($h["method"] == "chart_bnb_mask" || \
          $h["method"] == "chart_bnb_optimal_topology_materialize") {
        if ($h["validation_status"] != "ok") {
          bad = bad sprintf("%s/%s validation=%s\n", \
            $h["fixture"], $h["method"], $h["validation_status"]);
        }
        if ($h["exact_bnb_optimum"] != $h["validated_output_parsimony_min"]) {
          bad = bad sprintf("%s/%s optimum=%s output=%s\n", \
            $h["fixture"], $h["method"], $h["exact_bnb_optimum"], \
            $h["validated_output_parsimony_min"]);
        }
      }
      if ($h["method"] == "chart_bnb_score_only" && \
          $h["output_exactness_mode"] != "score_only_no_output") {
        bad = bad "score-only row has wrong output exactness\n";
      }
      if ($h["method"] == "chart_bnb_mask" && \
          $h["output_exactness_mode"] != "production_mask_superset") {
        bad = bad "mask row has wrong exactness label\n";
      }
      if ($h["method"] == "chart_bnb_optimal_topology_materialize" && \
          $h["output_exactness_mode"] != "grammar_topology_exact") {
        bad = bad "materialize row has wrong exactness label\n";
      }
    }
    END {
      fixture = "tiny_binary_four_taxon";
      split("current_trim chart_bnb_score_only chart_bnb_mask chart_bnb_optimal_topology_materialize", methods, " ");
      for (i in methods) {
        key = fixture "\034" methods[i];
        if (!(key in seen)) bad = bad sprintf("missing smoke row %s/%s\n", fixture, methods[i]);
      }
      if (bad != "") { printf "%s", bad; exit 1 }
    }
  ' "$summary_tsv") || {
    echo "smoke benchmark failed required-row validation:" >&2
    printf '%s' "$smoke_check" >&2
    exit 1
  }
fi

if [[ $strict_fail -ne 0 ]]; then
  echo "strict benchmark mode saw failed/unsupported rows:" >&2
  printf '  %s\n' "${strict_messages[@]}" >&2
  exit 1
fi

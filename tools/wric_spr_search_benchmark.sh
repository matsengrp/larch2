#!/usr/bin/env bash
set -euo pipefail

# Phase-0 measurement harness for native sample--SPR--merge versus chart-SPR.
# Timed processes always run through wric-process-metrics so timeout, CPU, and
# RSS values come from wait4/getrusage rather than shell polling.

if [[ -n ${WRIC_REPO_ROOT:-} ]]; then
  [[ "$WRIC_REPO_ROOT" == /* ]] || {
    echo "error: WRIC_REPO_ROOT must be an absolute directory" >&2
    exit 2
  }
  repo_root=$(realpath -e -- "$WRIC_REPO_ROOT") || {
    echo "error: WRIC_REPO_ROOT does not exist: $WRIC_REPO_ROOT" >&2
    exit 2
  }
  [[ -d "$repo_root" ]] || {
    echo "error: WRIC_REPO_ROOT is not a directory: $WRIC_REPO_ROOT" >&2
    exit 2
  }
else
  repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fi
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
local_workers=${WRIC_SPR_SEARCH_BENCHMARK_LOCAL_WORKERS:-}
chart_workers=${WRIC_SPR_SEARCH_BENCHMARK_CHART_WORKERS:-}
workers_list=${WRIC_SPR_SEARCH_BENCHMARK_WORKERS_LIST:-}
warmups=${WRIC_SPR_SEARCH_BENCHMARK_WARMUPS:-0}
repetitions=${WRIC_SPR_SEARCH_BENCHMARK_REPETITIONS:-1}
timeout_seconds=${WRIC_SPR_SEARCH_BENCHMARK_TIMEOUT_SECONDS:-0}
capture_rss_limit_bytes=-
capture_rss_limit_explicit=0
chart_memory_budget=${WRIC_SPR_SEARCH_BENCHMARK_CHART_MEMORY_BUDGET:-}
local_accept_updates=${WRIC_SPR_SEARCH_BENCHMARK_LOCAL_ACCEPT_UPDATES:-0}
chart_lazy_policy=${WRIC_SPR_SEARCH_BENCHMARK_LAZY_POLICY:-off}
process_metrics=${WRIC_PROCESS_METRICS:-"$(dirname "$dagutil")/wric-process-metrics"}
workload_manifest=${WRIC_SPR_SEARCH_BENCHMARK_WORKLOAD_MANIFEST:-}
run_manifest_group=${WRIC_SPR_SEARCH_BENCHMARK_RUN_MANIFEST_GROUP:-}
include_heuristic=${WRIC_SPR_SEARCH_BENCHMARK_INCLUDE_HEURISTIC:-0}
include_data_fixtures=${WRIC_SPR_SEARCH_INCLUDE_DATA_FIXTURES:-0}
extra_dag_pbs=${WRIC_SPR_SEARCH_EXTRA_DAG_PBS:-""}
extra_tree_pbs=${WRIC_SPR_SEARCH_EXTRA_TREE_PBS:-""}
mode_list=${WRIC_SPR_SEARCH_BENCHMARK_MODES:-"sampled_tree_fixed grammar_exact hybrid_exact"}
smoke=0
emit_full_canonical=0
print_manifest_header=0
native_only=0

iterations_explicit=0
seed_explicit=0
max_moves_explicit=0
max_candidates_explicit=0
top_k_exact_explicit=0
polytomy_mode_explicit=0
polytomy_shapes_explicit=0
memory_budget_explicit=0
local_accept_explicit=0
chart_lazy_policy_explicit=0
modes_explicit=0
fixture_option_explicit=0
[[ -v WRIC_SPR_SEARCH_BENCHMARK_LAZY_POLICY ]] && chart_lazy_policy_explicit=1

local_workers_explicit=0
chart_workers_explicit=0
workers_list_explicit=0
[[ -v WRIC_SPR_SEARCH_BENCHMARK_LOCAL_WORKERS ]] && local_workers_explicit=1
[[ -v WRIC_SPR_SEARCH_BENCHMARK_CHART_WORKERS ]] && chart_workers_explicit=1
[[ -v WRIC_SPR_SEARCH_BENCHMARK_WORKERS_LIST ]] && workers_list_explicit=1
expected_timeout_ids=()
supplemental_manifests=()
wall_ratio_requirements=()
rss_requirements=()
worker_policy_requirements=()
fixture_specs=()
user_fixtures=0

usage() {
  cat <<'USAGE'
wric_spr_search_benchmark.sh -- compare sample--explore--merge to chart-SPR

Core options:
  --dagutil PATH          dagutil binary
  --larch2 PATH           larch2 binary
  --process-metrics PATH  wric-process-metrics wait4 wrapper
  --out-dir DIR           output/report directory
  --iterations N          iteration budget for both methods (default: 1)
  --seed N                RNG seed for both methods (default: 1)
  --max-moves N           native maximum moves per iteration (default: 50)
  --max-candidates N      chart candidate cap (default: 32)
  --top-k-exact N         exact-verification budget (default: 4)
  --modes LIST            sampled_tree_fixed grammar_exact hybrid_exact grammar_lower_bound
  --polytomy-mode MODE    WRIC polytomy mode (default: expand-bounded)
  --polytomy-shapes N     bounded refinement seed-shape cap (default: 1)

Scheduling and measurement:
  --chart-workers N|auto  unified chart-worker budget; auto forwards numeric 0
  --workers-list LIST     comma-separated matrix, for example 1,2,4,8,auto
  --local-workers N       compatibility alias for the legacy local-score flag
  --warmups N             unrecorded paired warmups (default: 0)
  --repetitions N         recorded paired trials (default: 1)
  --timeout-seconds N     per-process timeout; 0 disables (default: 0)
  --capture-rss-limit-bytes B
                          non-manifest per-process RSS cap; positive bytes
  --chart-memory-budget B forward exactly to --chart-spr-memory-budget
  --local-accept-updates  forward exactly to --chart-spr-local-accept-updates
  --chart-lazy-policy M   forward off|on|auto to --wric-lazy-chart (default: off)
  --full-canonical-correctness
                          run one separate untimed full-sidecar oracle per mode
  --native-only           run only the native timed row (non-manifest capture)
  --print-workload-manifest-header
                          print the exact Phase-0 TSV header and exit

Strict gates:
  --workload-manifest P   immutable base workload manifest
  --supplemental-workload-manifest P
                          append-only manifest supplement (repeatable)
  --run-manifest-group N  select a named workload group
  --allow-expected-timeout ROW_ID
                          record-only manifest timeout waiver (repeatable)
  --require-wall-ratio METHOD@WORKERS=RATIO
                          require chart median <= native median * RATIO
  --require-max-rss-kb METHOD@WORKERS=KB
                          cap aggregate sampled descendant RSS
  --require-worker-policy default=automatic_default
                          require omitted/default to match explicit auto policy,
                          workers, semantics, and wall time within 10%

Fixtures and convenience:
  --include-heuristic     append grammar_lower_bound mode
  --include-data-fixtures include seedtree and 20D fixtures when present
  --dag PATH              add DAG fixture
  --tree PATH:REFSEQ      add tree fixture
  --smoke                 one native and one lower-bound chart row
  -h, --help              show this help

Worker options are mutually exclusive. If none is supplied, the harness emits
no product worker flag, so the run tests the product's real default.
USAGE
}

add_dag_fixture() { fixture_specs+=("$1|dag|$2|"); }
add_tree_fixture() { fixture_specs+=("$1|tree|$2|$3"); }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dagutil) dagutil=$2; shift 2 ;;
    --larch2) larch2=$2; shift 2 ;;
    --process-metrics) process_metrics=$2; shift 2 ;;
    --out-dir) out_dir=$2; shift 2 ;;
    --iterations) iterations=$2; iterations_explicit=1; shift 2 ;;
    --seed) seed=$2; seed_explicit=1; shift 2 ;;
    --max-moves) max_moves=$2; max_moves_explicit=1; shift 2 ;;
    --max-candidates) max_candidates=$2; max_candidates_explicit=1; shift 2 ;;
    --top-k-exact) top_k_exact=$2; top_k_exact_explicit=1; shift 2 ;;
    --local-workers) local_workers=$2; local_workers_explicit=1; shift 2 ;;
    --chart-workers) chart_workers=$2; chart_workers_explicit=1; shift 2 ;;
    --workers-list) workers_list=$2; workers_list_explicit=1; shift 2 ;;
    --warmups) warmups=$2; shift 2 ;;
    --repetitions) repetitions=$2; shift 2 ;;
    --timeout|--timeout-seconds) timeout_seconds=$2; shift 2 ;;
    --capture-rss-limit-bytes)
      (( capture_rss_limit_explicit == 0 )) || \
        { echo "error: --capture-rss-limit-bytes may be supplied only once" >&2; exit 1; }
      capture_rss_limit_bytes=$2; capture_rss_limit_explicit=1; shift 2 ;;
    --chart-memory-budget) chart_memory_budget=$2; memory_budget_explicit=1; shift 2 ;;
    --local-accept-updates) local_accept_updates=1; local_accept_explicit=1; shift ;;
    --chart-lazy-policy) chart_lazy_policy=$2; chart_lazy_policy_explicit=1; shift 2 ;;
    --full-canonical-correctness) emit_full_canonical=1; shift ;;
    --native-only) native_only=1; shift ;;
    --print-workload-manifest-header) print_manifest_header=1; shift ;;
    --workload-manifest) workload_manifest=$2; shift 2 ;;
    --supplemental-workload-manifest) supplemental_manifests+=("$2"); shift 2 ;;
    --run-manifest-group) run_manifest_group=$2; shift 2 ;;
    --allow-expected-timeout) expected_timeout_ids+=("$2"); shift 2 ;;
    --require-wall-ratio) wall_ratio_requirements+=("$2"); shift 2 ;;
    --require-max-rss-kb) rss_requirements+=("$2"); shift 2 ;;
    --require-worker-policy) worker_policy_requirements+=("$2"); shift 2 ;;
    --polytomy-mode) polytomy_mode=$2; polytomy_mode_explicit=1; shift 2 ;;
    --polytomy-shapes) polytomy_shapes=$2; polytomy_shapes_explicit=1; shift 2 ;;
    --modes) mode_list=$2; modes_explicit=1; shift 2 ;;
    --include-heuristic) include_heuristic=1; shift ;;
    --include-data-fixtures) include_data_fixtures=1; shift ;;
    --dag)
      user_fixtures=1; fixture_option_explicit=1; path=$2; label=$(basename "$path")
      label=${label//[^A-Za-z0-9_.-]/_}; add_dag_fixture "$label" "$path"; shift 2
      ;;
    --tree)
      user_fixtures=1; fixture_option_explicit=1; spec=$2
      [[ "$spec" == *:* ]] || { echo "error: --tree requires PATH:REFSEQ" >&2; exit 1; }
      path=${spec%%:*}; ref=${spec#*:}; label=$(basename "$path")
      label=${label//[^A-Za-z0-9_.-]/_}; add_tree_fixture "$label" "$path" "$ref"; shift 2
      ;;
    --smoke)
      smoke=1; iterations=1; max_moves=1; max_candidates=1; top_k_exact=0
      mode_list=grammar_lower_bound; include_heuristic=0; include_data_fixtures=0
      warmups=0; repetitions=1; shift
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "error: unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

fail() { echo "error: $*" >&2; exit 1; }
for pair in iterations:$iterations seed:$seed max-moves:$max_moves \
  max-candidates:$max_candidates top-k-exact:$top_k_exact \
  polytomy-shapes:$polytomy_shapes warmups:$warmups repetitions:$repetitions \
  timeout-seconds:$timeout_seconds; do
  [[ ${pair#*:} =~ ^[0-9]+$ ]] || fail "--${pair%%:*} requires a non-negative integer"
done
(( iterations > 0 && repetitions > 0 )) || fail "iterations and repetitions must be positive"
[[ -z "$chart_memory_budget" || "$chart_memory_budget" =~ ^[0-9]+$ ]] || \
  fail "--chart-memory-budget requires a non-negative integer"
(( capture_rss_limit_explicit == 0 )) || \
  [[ "$capture_rss_limit_bytes" =~ ^[1-9][0-9]*$ ]] || \
  fail "--capture-rss-limit-bytes requires a positive integer"
(( capture_rss_limit_explicit == 0 )) || [[ -z "$workload_manifest" ]] || \
  fail "--capture-rss-limit-bytes cannot be combined with --workload-manifest"
[[ "$chart_lazy_policy" == off || "$chart_lazy_policy" == on ||
   "$chart_lazy_policy" == auto ]] || \
  fail "--chart-lazy-policy requires off, on, or auto"

worker_option_count=$((local_workers_explicit + chart_workers_explicit + workers_list_explicit))
(( worker_option_count <= 1 )) || \
  fail "--local-workers, --chart-workers, and --workers-list are mutually exclusive"
chart_worker_specs=()
if (( workers_list_explicit )); then
  IFS=',' read -r -a chart_worker_specs <<<"$workers_list"
elif (( chart_workers_explicit )); then
  chart_worker_specs=("$chart_workers")
elif (( local_workers_explicit )); then
  chart_worker_specs=("$local_workers")
else
  chart_worker_specs=(default)
fi
(( ${#chart_worker_specs[@]} > 0 )) || fail "worker list is empty"
declare -A seen_workers=()
for worker in "${chart_worker_specs[@]}"; do
  [[ "$worker" == auto || "$worker" == default || "$worker" =~ ^[0-9]+$ ]] || \
    fail "invalid worker value '$worker'"
  [[ -z ${seen_workers[$worker]:-} ]] || fail "duplicate worker value '$worker'"
  seen_workers[$worker]=1
done
[[ -x "$dagutil" ]] || fail "dagutil binary not found/executable: $dagutil"
[[ -x "$larch2" ]] || fail "larch2 binary not found/executable: $larch2"
[[ -x "$process_metrics" ]] || fail "process measurement helper not found/executable: $process_metrics"
AFFINITY_CPUS=$(taskset -pc $$ 2>/dev/null | awk -F': ' '{print $2; exit}')
AFFINITY_CPUS=${AFFINITY_CPUS:-unknown}
if (( ${#expected_timeout_ids[@]} > 0 )); then
  [[ -n "$workload_manifest" ]] || fail "expected timeouts require --workload-manifest"
  [[ -n "$run_manifest_group" ]] || (( timeout_seconds > 0 )) || \
    fail "expected timeouts require a nonzero timeout"
  (( warmups == 0 )) || fail "expected-timeout characterization forbids warmups"
  (( ${#wall_ratio_requirements[@]} == 0 && ${#rss_requirements[@]} == 0 &&
     ${#worker_policy_requirements[@]} == 0 )) || \
    fail "expected-timeout characterization cannot be combined with acceptance gates"
  (( emit_full_canonical == 0 )) || \
    fail "expected-timeout characterization cannot request full canonical correctness"
  declare -A seen_timeout_waiver=()
  for row_id in "${expected_timeout_ids[@]}"; do
    [[ -z ${seen_timeout_waiver[$row_id]:-} ]] || \
      fail "duplicate expected-timeout waiver: $row_id"
    seen_timeout_waiver[$row_id]=1
  done
fi
if (( ${#wall_ratio_requirements[@]} > 0 || ${#rss_requirements[@]} > 0 ||
      ${#worker_policy_requirements[@]} > 0 )) && \
   [[ -z "$workload_manifest" ]]; then
  fail "acceptance gates require --workload-manifest"
fi
declare -A seen_worker_policy_requirement=()
for requirement in "${worker_policy_requirements[@]}"; do
  [[ "$requirement" == default=automatic_default ]] || \
    fail "unsupported worker-policy gate '$requirement' (expected default=automatic_default)"
  [[ -z ${seen_worker_policy_requirement[$requirement]:-} ]] || \
    fail "duplicate worker-policy gate: $requirement"
  seen_worker_policy_requirement[$requirement]=1
done
if (( ${#supplemental_manifests[@]} > 0 )) && [[ -z "$workload_manifest" ]]; then
  fail "supplemental manifests require --workload-manifest"
fi
[[ -z "$run_manifest_group" || -n "$workload_manifest" ]] || \
  fail "--run-manifest-group requires --workload-manifest"
(( native_only == 0 )) || [[ -z "$workload_manifest" ]] || \
  fail "--native-only is restricted to non-manifest capture"
if [[ -n "$run_manifest_group" ]]; then
  (( fixture_option_explicit == 0 && include_data_fixtures == 0 )) || \
    fail "manifest-group mode obtains fixtures only from the manifest"
  (( iterations_explicit == 0 && seed_explicit == 0 && max_moves_explicit == 0 &&
     max_candidates_explicit == 0 && top_k_exact_explicit == 0 &&
     polytomy_mode_explicit == 0 && polytomy_shapes_explicit == 0 &&
     memory_budget_explicit == 0 && local_accept_explicit == 0 &&
     chart_lazy_policy_explicit == 0 &&
     modes_explicit == 0 )) || \
    fail "manifest-group mode obtains work/search options from each selected row"
  [[ -z "$extra_dag_pbs" && -z "$extra_tree_pbs" ]] || \
    fail "manifest-group mode forbids environment fixture additions"
fi

if (( user_fixtures == 0 )) && [[ -z "$run_manifest_group" ]]; then
  add_dag_fixture small_test_5_tree0 "$repo_root/data/test_5_trees/tree_0.pb.gz"
fi
if [[ "$include_data_fixtures" != 0 ]]; then
  [[ ! -f "$repo_root/data/seedtree/seedtree.pb.gz" ]] || \
    add_tree_fixture medium_seedtree "$repo_root/data/seedtree/seedtree.pb.gz" \
      "$repo_root/data/seedtree/refseq.txt.gz"
  [[ ! -f "$repo_root/data/20D_from_fasta/1final-tree-1.nh1.pb.gz" ]] || \
    add_tree_fixture real_20D_one_tree \
      "$repo_root/data/20D_from_fasta/1final-tree-1.nh1.pb.gz" \
      "$repo_root/data/20D_from_fasta/refseq.txt"
fi
for path in $extra_dag_pbs; do
  label=$(basename "$path"); add_dag_fixture "extra_${label//[^A-Za-z0-9_.-]/_}" "$path"
done
for spec in $extra_tree_pbs; do
  [[ "$spec" == *:* ]] || fail "extra tree entries require PATH:REFSEQ"
  path=${spec%%:*}; ref=${spec#*:}; label=$(basename "$path")
  add_tree_fixture "extra_${label//[^A-Za-z0-9_.-]/_}" "$path" "$ref"
done
if [[ "$include_heuristic" != 0 && " $mode_list " != *" grammar_lower_bound "* ]]; then
  mode_list="$mode_list grammar_lower_bound"
fi

# Immutable workload format. The detached seal is exactly the GNU sha256sum
# text form '<hex><two spaces><basename>\n'; a supplement names the exact hash
# of its base manifest bytes in the parent_sha256 preamble field.
manifest_header=$'row_id\trun_group\tworkload_name\tfixture_id\tmethod\tinput_kind\tprimary_uri\tprimary_sha256\tsecondary_uri\tsecondary_sha256\trefseq_uri\trefseq_sha256\tbinary_role\tworker_option\trequested_workers\texpected_resolved_workers\texpected_worker_policy\taffinity_cpus\ttimeout_seconds\trss_limit_bytes\texpected_outcome\texpected_timeout_trials\texpected_reason_code\texpected_reason_sha256\tscale_resource\tscale_limit\tscale_largest_candidates\tscale_largest_top_k\titerations\tseed\tnative_max_moves\tchart_max_candidates\tchart_top_k_exact\tcandidate_cap_semantics\tacceptance\tobjective\tcandidate_selection\tcandidate_source\ttopology_selector\trandomize_order\treservoir_sample\tinclude_immediate_reversals\tsampled_tree_count\tsampled_tree_radius\tsampled_tree_score_threshold\tmax_upward_path_expansions\tmax_path_pairs\tmin_moved_clade_size\tmax_moved_clade_size\tmin_target_clade_size\tmax_target_clade_size\tmax_affected_clades\tpolytomy_mode\tpolytomy_max_exact_arity\tpolytomy_max_shapes\tpolytomy_max_productions\tpolytomy_max_clades\tlazy_policy\tmax_cached_patterns\tpattern_batch_size\tcandidate_batch_size\tmemory_budget_bytes\tcommit_mode\tverification_mode\tlocal_accept_updates\tdominance_mode\tbound_pruning\trequire_exact_keep_mask\tmax_frontier_entries\tscore_ua_edge\tvalidate\tforce_no_vcf\texpected_refinement_exactness\texpected_cache_strategy\texpected_effective_pattern_batch_size\texpected_keep_mask_kind\texpected_final_compaction_exactness\texpected_chain_exactness\texpected_active_patterns\texpected_initial_clades\texpected_initial_productions\texpected_candidates_generated\texpected_candidates_scored\texpected_exact_verifications\texpected_stop_reason\texpected_iterations\texpected_accepted_moves\texpected_initial_score\texpected_final_score\texpected_validated_parsimony\toracle_search_semantic_sha256\toracle_output_semantic_sha256\toracle_trial_semantic_sha256\tcanonical_sidecar_uri\tcanonical_sidecar_sha256\toracle_report_uri\toracle_report_sha256\tcanonical_argv_sha256'
if (( print_manifest_header )); then printf '%s\n' "$manifest_header"; exit 0; fi
manifest_files=()
base_manifest_sha=NA
# The primary seedtree is never eligible for a real-scale `scale_limit`
# waiver.  Match its frozen bytes, not a user-controlled fixture label or URI.
phase0_seedtree_sha256=2a1059432188123629169118a3cf72ec4ad377f3c8479794990e10bb7da38153
preamble_keys=(schema schema_version kind manifest_id parent_sha256 repo_revision
  merge_base frozen_larch2_uri frozen_larch2_sha256 frozen_oracle_dagutil_uri
  frozen_oracle_dagutil_sha256 commands_uri commands_sha256)
preamble_value() {
  awk -v key="$2" 'index($0,"# " key "=")==1 {
    print substr($0,length(key)+4); exit
  }' "$1"
}
manifest_uri_path() {
  local manifest=$1 uri=$2 scheme rel root candidate resolved
  case "$uri" in
    repo://*) scheme=repo; rel=${uri#repo://}; root=$repo_root ;;
    manifest://*) scheme=manifest; rel=${uri#manifest://}; root=$(dirname "$manifest") ;;
    *) fail "manifest URI must use repo:// or manifest://: $uri" ;;
  esac
  [[ -n "$rel" && "$rel" != /* && "$rel" != */ && "$rel" != *//* &&
     "$rel" != *\\* && "$rel" != *$'\t'* && "$rel" != *$'\n'* &&
     ! "$rel" =~ (^|/)(\.|\.\.)($|/) ]] || \
    fail "manifest URI is not a confined normalized relative path: $uri"
  root=$(realpath -e -- "$root") || fail "manifest URI root does not exist: $uri"
  candidate=$root/$rel
  resolved=$(realpath -e -- "$candidate") || fail "manifest URI target does not exist: $uri"
  [[ "$resolved" == "$root"/* ]] || fail "manifest URI escapes its $scheme root: $uri"
  [[ -f "$resolved" ]] || fail "manifest URI target is not a regular file: $uri"
  printf '%s' "$resolved"
}
validate_manifest_rows() {
  local file=$1
  awk -F '\t' -v seedtree_sha="$phase0_seedtree_sha256" '
    function bad(message) {print "error: manifest row " id ": " message > "/dev/stderr"; failed=1}
    function ishash(x) {return x ~ /^[0-9a-f]{64}$/}
    function isuint(x) {return x ~ /^(0|[1-9][0-9]*)$/}
    function ispos(x) {return x ~ /^[1-9][0-9]*$/}
    function isbool(x) {return x=="true" || x=="false"}
    function isp(x) {return x=="-"}
    function pair(uri,sha,label) {
      if ((uri=="-") != (sha=="-")) bad(label " URI/hash must both be - or both be present")
      if (uri!="-" && !ishash(sha)) bad(label " hash is not lowercase SHA-256")
    }
    /^#/ {next}
    !header {for(i=1;i<=NF;i++) h[$i]=i; header=1; next}
    {
      id=$(h["row_id"])
      if (id !~ /^[A-Za-z0-9][A-Za-z0-9_.-]*$/) bad("invalid row_id")
      if ($(h["run_group"]) !~ /^[A-Za-z0-9][A-Za-z0-9_.-]*$/) bad("invalid run_group")
      if ($(h["workload_name"]) !~ /^[A-Za-z0-9][A-Za-z0-9_.-]*$/) bad("invalid workload_name")
      if ($(h["fixture_id"]) !~ /^[A-Za-z0-9][A-Za-z0-9_.-]*$/) bad("invalid fixture_id")
      kind=$(h["input_kind"]); if (kind!="dag_pb" && kind!="tree_pb_refseq") bad("invalid input_kind")
      pair($(h["primary_uri"]),$(h["primary_sha256"]),"primary")
      pair($(h["secondary_uri"]),$(h["secondary_sha256"]),"secondary")
      pair($(h["refseq_uri"]),$(h["refseq_sha256"]),"refseq")
      pair($(h["canonical_sidecar_uri"]),$(h["canonical_sidecar_sha256"]),"canonical sidecar")
      pair($(h["oracle_report_uri"]),$(h["oracle_report_sha256"]),"oracle report")
      if ($(h["primary_uri"])=="-") bad("primary input is required")
      if (kind=="dag_pb" && ($(h["secondary_uri"])!="-" || $(h["refseq_uri"])!="-")) bad("dag_pb forbids secondary/refseq assets")
      if (kind=="tree_pb_refseq" && ($(h["secondary_uri"])!="-" || $(h["refseq_uri"])=="-")) bad("tree_pb_refseq requires only primary+refseq assets")
      role=$(h["binary_role"]); method=$(h["method"]); option=$(h["worker_option"])
      outcome=$(h["expected_outcome"])
      if (method=="sample_explore_merge") {
        if (role!="frozen_native") bad("native method requires frozen_native")
        if (option!="none" || $(h["requested_workers"])!="-" || $(h["expected_resolved_workers"])!="-" || $(h["expected_worker_policy"])!="-") bad("native worker fields are not canonical")
        if (!ispos($(h["native_max_moves"])) || $(h["chart_max_candidates"])!="-" || $(h["chart_top_k_exact"])!="-") bad("native work budget is not canonical")
        if ($(h["candidate_cap_semantics"])!="-") bad("native candidate_cap_semantics must be -")
      } else {
        if (method!="chart_spr_sampled_tree_fixed_topology" && method!="chart_spr_grammar_exact" && method!="chart_spr_hybrid_exact" && method!="chart_spr_grammar_lower_bound_heuristic") bad("unknown chart method")
        if (role!="working_chart") bad("chart method requires working_chart")
        if (option!="none" && option!="chart_spr_workers" && option!="chart_spr_local_score_workers") bad("invalid chart worker_option")
        if (option=="none" && $(h["requested_workers"])!="default") bad("omitted chart worker must be literal default")
        if (option!="none" && !isuint($(h["requested_workers"]))) bad("explicit chart worker must be an integer (auto is 0)")
        requested=$(h["requested_workers"]); worker_policy=$(h["expected_worker_policy"])
        resolved=$(h["expected_resolved_workers"])
        unobserved=(outcome=="timeout" || outcome=="expected_infeasible")
        if (unobserved) {
          if (resolved!="-" || worker_policy!="-")
            bad("unobserved chart outcome must not claim resolved workers or policy")
        } else {
          if (resolved=="policy") {
            if (option!="none" && requested!="0") bad("policy resolved-worker sentinel is only for auto/default")
          } else if (!ispos(resolved)) bad("successful chart expected_resolved_workers must be positive or policy")
          if (option=="none") expected_policy="policy"
          else if (option=="chart_spr_workers") expected_policy=(requested=="0" ? "automatic" : "explicit")
          else expected_policy=(requested=="0" ? "legacy_automatic" : "legacy_explicit")
          if (worker_policy!=expected_policy) bad("expected_worker_policy is inconsistent with worker_option/requested_workers")
        }
        if ($(h["native_max_moves"])!="-" || !isuint($(h["chart_max_candidates"])) || !isuint($(h["chart_top_k_exact"]))) bad("chart work budget is not canonical")
        if ($(h["candidate_cap_semantics"])!="post-dedup") bad("chart candidate cap must be post-dedup")
        lazy=$(h["lazy_policy"]); if(lazy!="off" && lazy!="on" && lazy!="auto") bad("lazy_policy must be off, on, or auto")
        if ($(h["polytomy_mode"])!="reject" && $(h["polytomy_mode"])!="expand-exact" && $(h["polytomy_mode"])!="expand-bounded") bad("invalid polytomy_mode")
        if ($(h["candidate_selection"])!="lower_bound_top_k" && $(h["candidate_selection"])!="exhaustive_exact" && $(h["candidate_selection"])!="lower_bound_first_improvement" && $(h["candidate_selection"])!="sampled_or_randomized") bad("invalid candidate_selection")
        if ($(h["candidate_source"])!="grammar" && $(h["candidate_source"])!="sampled_tree" && $(h["candidate_source"])!="hybrid") bad("invalid candidate_source")
        if ($(h["topology_selector"])!="none" && $(h["topology_selector"])!="first_reachable_overlay_topology") bad("invalid topology_selector")
        if (outcome=="expected_infeasible") {
          split("expected_refinement_exactness expected_cache_strategy expected_effective_pattern_batch_size expected_keep_mask_kind expected_final_compaction_exactness expected_chain_exactness expected_active_patterns expected_initial_clades expected_initial_productions expected_candidates_generated expected_candidates_scored expected_exact_verifications expected_stop_reason expected_iterations expected_accepted_moves expected_final_score expected_validated_parsimony", unreached, " ")
          for(i in unreached) if($(h[unreached[i]])!="-") bad("expected_infeasible claims unreached field " unreached[i])
          if(!isuint($(h["expected_initial_score"]))) bad("expected_infeasible requires its separately reached initial score")
          if($(h["canonical_sidecar_uri"])!="-" || $(h["canonical_sidecar_sha256"])!="-" ||
             $(h["oracle_report_uri"])!="-" || $(h["oracle_report_sha256"])!="-")
            bad("expected_infeasible must not claim report/canonical assets")
        } else {
          refinement=$(h["expected_refinement_exactness"])
          if (refinement!="EXACT" && refinement!="BOUNDED_REFINED_GRAMMAR") bad("chart row requires a closed expected_refinement_exactness")
          cache=$(h["expected_cache_strategy"])
          if (cache!="all_active_patterns" && cache!="pattern_batches" && cache!="lazy_multisite_chart") bad("chart row requires a closed expected_cache_strategy")
          if (!ispos($(h["expected_effective_pattern_batch_size"]))) bad("chart row requires a positive expected_effective_pattern_batch_size")
        }
        acceptance=$(h["acceptance"]); objective=$(h["objective"])
        source=$(h["candidate_source"]); topology=$(h["topology_selector"])
        if (method=="chart_spr_sampled_tree_fixed_topology") {
          if (acceptance!="fixed_topology_exact" || objective!="fixed_topology_exact" ||
              source!="sampled_tree" || topology!="first_reachable_overlay_topology")
            bad("sampled-tree fixed-topology method has inconsistent semantic labels")
        } else if (method=="chart_spr_grammar_exact") {
          if (acceptance!="exact_multisite" || objective!="grammar_exact" ||
              source!="grammar" || topology!="none")
            bad("grammar-exact method has inconsistent semantic labels")
        } else if (method=="chart_spr_hybrid_exact") {
          if (acceptance!="exact_multisite" || objective!="grammar_exact" ||
              source!="hybrid" || topology!="none")
            bad("hybrid-exact method has inconsistent semantic labels")
        } else if (method=="chart_spr_grammar_lower_bound_heuristic") {
          if (acceptance!="lower_bound_heuristic" || objective!="composite_lower_bound_heuristic" ||
              source!="grammar" || topology!="none")
            bad("grammar lower-bound method has inconsistent semantic labels")
        }
      }
      if (!ispos($(h["iterations"])) || !isuint($(h["seed"])) || !isuint($(h["timeout_seconds"])) || !ispos($(h["rss_limit_bytes"]))) bad("invalid iteration/seed/resource integer")
      if ($(h["affinity_cpus"]) !~ /^[0-9]+([,-][0-9]+)*$/) bad("invalid affinity_cpus")
      if ($(h["validate"])!="true") bad("validation must be true")
      timeouts=$(h["expected_timeout_trials"])
      if (!isuint(timeouts)) bad("expected_timeout_trials is not an integer")
      if (outcome=="ok") {
        if (timeouts!="0" || $(h["expected_reason_code"])!="-" || $(h["expected_reason_sha256"])!="-" || $(h["scale_resource"])!="-" || $(h["scale_limit"])!="-" || $(h["scale_largest_candidates"])!="-" || $(h["scale_largest_top_k"])!="-") bad("ok outcome has waiver fields")
      } else if (outcome=="timeout") {
        if (!ispos(timeouts) || !ispos($(h["timeout_seconds"])) || $(h["expected_reason_code"])!="-" || $(h["expected_reason_sha256"])!="-" || $(h["scale_resource"])!="-" || $(h["scale_limit"])!="-" || $(h["scale_largest_candidates"])!="-" || $(h["scale_largest_top_k"])!="-") bad("timeout outcome has invalid waiver fields")
      } else if (outcome=="expected_infeasible") {
        if (method=="sample_explore_merge" || timeouts!="0" || $(h["expected_reason_code"])!="high_arity_refinement_refusal" || !ishash($(h["expected_reason_sha256"])) || $(h["scale_resource"])!="-" || $(h["scale_limit"])!="-" || $(h["scale_largest_candidates"])!="-" || $(h["scale_largest_top_k"])!="-") bad("expected_infeasible is not the closed high-arity refusal contract")
        refusal_worker=$(h["requested_workers"])
        refusal_argv=$(h["canonical_argv_sha256"])
        expected_refusal_argv=(refusal_worker=="1" ?
          "ac03537d4db4cc20a843154aae5ed578927d5d1852809c9a8d84fdc4b27dda2f" :
          (refusal_worker=="8" ?
          "27f498987087c3ef91f39db04ae56107bca4adb0abd0c32aeb5abfa3d9c3faf3" : "-"))
        if(kind!="tree_pb_refseq" ||
           $(h["primary_sha256"])!="a65f300916f158ea4c8de8bc49f5a94379905a4c3b7fba337ce6773d65ecfce4" ||
           $(h["refseq_sha256"])!="82c11885688b9a67b72ec4d2dd5913571a1723039ca501dc63bafb2cb5ea13eb" ||
           method!="chart_spr_grammar_exact" || option!="chart_spr_workers" ||
           expected_refusal_argv=="-" || $(h["expected_resolved_workers"])!="-" ||
           $(h["expected_worker_policy"])!="-" ||
           $(h["affinity_cpus"])!="0,2,4,6,8,10,12,14" ||
           $(h["timeout_seconds"])!="600" || $(h["rss_limit_bytes"])!="6442450944" ||
           $(h["iterations"])!="1" || $(h["seed"])!="1" ||
           $(h["chart_max_candidates"])!="1" || $(h["chart_top_k_exact"])!="1" ||
           $(h["candidate_source"])!="grammar" || $(h["acceptance"])!="exact_multisite" ||
           $(h["objective"])!="grammar_exact" ||
           $(h["polytomy_mode"])!="expand-bounded" || $(h["polytomy_max_shapes"])!="1" ||
           $(h["expected_initial_score"])!="11155" ||
           $(h["expected_reason_sha256"])!="84d2f5dec0140f55f8bd3fcde7d89b2cbf95e7c4ae6437daccbfe4b9fde15e35" ||
           refusal_argv!=expected_refusal_argv)
          bad("expected_infeasible is not an exact frozen real-20D W1/W8 tuple")
      } else if (outcome=="scale_limit") {
        resource=$(h["scale_resource"])
        if (method!="chart_spr_grammar_exact" || timeouts!="0" || (resource!="timeout_seconds" && resource!="rss_limit_bytes") || !ispos($(h["scale_limit"])) || !ispos($(h["scale_largest_candidates"])) || !isuint($(h["scale_largest_top_k"])) || $(h["chart_max_candidates"])!=$(h["scale_largest_candidates"]) || $(h["chart_top_k_exact"])!=$(h["scale_largest_top_k"]) || $(h["expected_reason_code"])!="-" || $(h["expected_reason_sha256"])!="-") bad("invalid scale_limit contract")
        lower=tolower($(h["fixture_id"]) " " $(h["workload_name"]) " " $(h["primary_uri"]))
        if (index(lower,"seedtree") || $(h["primary_sha256"])==seedtree_sha) bad("scale_limit is forbidden for seedtree")
        if ($(h["scale_largest_top_k"])+0 > $(h["scale_largest_candidates"])+0) bad("scale prefix top-K exceeds candidate prefix")
        if (resource=="timeout_seconds" && $(h["scale_limit"])!=$(h["timeout_seconds"])) bad("timeout scale_limit must equal timeout_seconds")
        if (resource=="rss_limit_bytes" && $(h["scale_limit"])!=$(h["rss_limit_bytes"])) bad("RSS scale_limit must equal rss_limit_bytes")
      } else bad("invalid expected_outcome")
      argv=$(h["canonical_argv_sha256"]); if (!ishash(argv)) bad("canonical_argv_sha256 is required")
      success=(outcome=="ok" || outcome=="scale_limit")
      search=$(h["oracle_search_semantic_sha256"]); output=$(h["oracle_output_semantic_sha256"]); trial=$(h["oracle_trial_semantic_sha256"])
      if (success) {
        if (method=="sample_explore_merge") {if(search!="-") bad("native search digest must be -")} else if(!ishash(search)) bad("chart search digest is required")
        if (!ishash(output) || !ishash(trial)) bad("successful row requires output/trial digests")
        if (method!="sample_explore_merge" && ($(h["canonical_sidecar_uri"])=="-" || $(h["oracle_report_uri"])=="-")) bad("successful chart row requires frozen sidecar/report assets")
        if (method!="sample_explore_merge" && $(h["canonical_sidecar_sha256"])!=search) bad("search digest must equal frozen full-sidecar SHA-256")
      } else if (search!="-" || output!="-" || trial!="-") bad("non-success outcome must not claim semantic output")
      if (method!="sample_explore_merge") {
        split("randomize_order reservoir_sample include_immediate_reversals local_accept_updates bound_pruning require_exact_keep_mask score_ua_edge force_no_vcf",b," ")
        for(i in b) if(!isbool($(h[b[i]]))) bad(b[i] " must be lowercase boolean")
        split("chart_max_candidates chart_top_k_exact sampled_tree_count sampled_tree_radius sampled_tree_score_threshold max_upward_path_expansions max_path_pairs min_moved_clade_size max_moved_clade_size min_target_clade_size max_target_clade_size max_affected_clades polytomy_max_exact_arity polytomy_max_shapes polytomy_max_productions polytomy_max_clades max_cached_patterns pattern_batch_size candidate_batch_size memory_budget_bytes max_frontier_entries",n," ")
        for(i in n) if(!isuint($(h[n[i]]))) bad(n[i] " must be an integer")
        if (!ispos($(h["min_moved_clade_size"])) || !ispos($(h["min_target_clade_size"]))) bad("minimum clade sizes must be positive")
        if ($(h["score_ua_edge"])=="true" && $(h["force_no_vcf"])!="true") bad("UA-edge scoring requires the frozen no-VCF contract")
      }
    }
    END {exit failed}
  ' "$file" || fail "manifest row validation failed: $file"
}
verify_manifest() {
  local file=$1 sidecar=${1}.sha256 actual expected_line last_byte actual_header
  local expected_columns key value i line
  [[ -f "$file" ]] || fail "workload manifest not found: $file"
  [[ -f "$sidecar" ]] || fail "manifest checksum sidecar not found: $sidecar"
  actual=$(sha256sum "$file" | awk '{print $1}')
  expected_line="$actual  $(basename "$file")"
  [[ $(awk 'END{print NR}' "$sidecar") == 1 && $(<"$sidecar") == "$expected_line" ]] || \
    fail "manifest seal is not exact GNU sha256sum format: $sidecar"
  last_byte=$(tail -c 1 "$sidecar" | od -An -tuC | awk '{print $1}')
  [[ "$last_byte" == 10 ]] || fail "manifest seal must end in exactly one newline: $sidecar"
  for ((i=0; i<${#preamble_keys[@]}; ++i)); do
    key=${preamble_keys[$i]}
    line=$(sed -n "$((i+1))p" "$file")
    [[ "$line" == "# $key="* && -n ${line#*=} ]] || \
      fail "manifest preamble line $((i+1)) must define exactly '$key': $file"
  done
  actual_header=$(sed -n "$((${#preamble_keys[@]}+1))p" "$file")
  [[ "$actual_header" == "$manifest_header" ]] || fail "manifest TSV header mismatch: $file"
  tail -n +"$((${#preamble_keys[@]}+2))" "$file" | awk '/^#/ {exit 1}' || \
    fail "comments/extra preamble fields after the TSV header are forbidden: $file"
  [[ $(preamble_value "$file" schema) == wric_chart_parallelization_workloads ]] || \
    fail "unsupported manifest schema: $file"
  [[ $(preamble_value "$file" schema_version) == 1 ]] || \
    fail "unsupported manifest schema_version: $file"
  [[ $(preamble_value "$file" kind) == base || $(preamble_value "$file" kind) == supplement ]] || \
    fail "manifest kind must be base or supplement: $file"
  [[ $(preamble_value "$file" manifest_id) =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || \
    fail "invalid manifest_id: $file"
  for key in repo_revision merge_base; do
    [[ $(preamble_value "$file" "$key") =~ ^[0-9a-f]{40,64}$ ]] || \
      fail "manifest $key must be a full lowercase revision hash: $file"
  done
  for key in frozen_larch2_sha256 frozen_oracle_dagutil_sha256 commands_sha256; do
    [[ $(preamble_value "$file" "$key") =~ ^[0-9a-f]{64}$ ]] || \
      fail "manifest $key is not lowercase SHA-256: $file"
  done
  expected_columns=$(awk -F '\t' '{print NF; exit}' <<<"$manifest_header")
  awk -F '\t' -v expected="$expected_columns" '
    /^#/ {next} !header {header=1;next}
    NF!=expected {exit 1}
    {for(i=1;i<=NF;i++)if($i=="")exit 1; rows++}
    END{if(rows==0)exit 1}' "$file" || \
    fail "manifest rows must have the exact width and no empty defining fields: $file"
  validate_manifest_rows "$file"
  printf '%s' "$actual"
}
verify_manifest_assets() {
  local manifest=$1 row_id label uri sha path actual
  while IFS=$'\t' read -r row_id label uri sha; do
    if [[ "$uri" == - || "$sha" == - ]]; then
      [[ "$uri" == - && "$sha" == - ]] || \
        fail "$row_id has a partial $label URI/hash pair"
      continue
    fi
    [[ "$sha" =~ ^[0-9a-f]{64}$ ]] || fail "$row_id has an invalid $label SHA-256"
    path=$(manifest_uri_path "$manifest" "$uri")
    [[ -f "$path" ]] || fail "$row_id $label asset is missing: $path"
    actual=$(sha256sum "$path" | awk '{print $1}')
    [[ "$actual" == "$sha" ]] || fail "$row_id $label asset hash mismatch: $path"
  done < <(awk -F '\t' '
    /^#/ {next} !header {for(i=1;i<=NF;i++)h[$i]=i;header=1;next}
    {print $h["row_id"],"primary",$h["primary_uri"],$h["primary_sha256"]
     print $h["row_id"],"secondary",$h["secondary_uri"],$h["secondary_sha256"]
     print $h["row_id"],"refseq",$h["refseq_uri"],$h["refseq_sha256"]
     print $h["row_id"],"canonical_sidecar",$h["canonical_sidecar_uri"],$h["canonical_sidecar_sha256"]
     print $h["row_id"],"oracle_report",$h["oracle_report_uri"],$h["oracle_report_sha256"]}' \
    OFS='\t' "$manifest")
  while IFS=$'\t' read -r row_id method outcome sidecar_uri report_uri search_sha expected_refinement expected_keep topology; do
    [[ "$method" != sample_explore_merge && ( "$outcome" == ok || "$outcome" == scale_limit ) ]] || continue
    local sidecar report actual_value expected_topology
    sidecar=$(manifest_uri_path "$manifest" "$sidecar_uri")
    report=$(manifest_uri_path "$manifest" "$report_uri")
    [[ $(sha256sum "$sidecar" | awk '{print $1}') == "$search_sha" ]] || \
      fail "$row_id frozen sidecar does not equal oracle search digest"
    [[ $(awk 'match($0,/"semantic_sha256"[[:space:]]*:[[:space:]]*"[0-9a-f]+"/){v=substr($0,RSTART,RLENGTH);sub(/^[^:]*:[[:space:]]*"/,"",v);sub(/"$/,"",v);print v;exit}' "$report") == "$search_sha" ]] || \
      fail "$row_id compact oracle report does not name the frozen search digest"
    if [[ "$expected_refinement" != - ]]; then
      actual_value=$(awk 'index($0,"\"record\":\"contract\"")&&match($0,/"refinement_exactness":"[^"]+"/){v=substr($0,RSTART,RLENGTH);sub(/^.*:"/,"",v);sub(/"$/,"",v);print v;exit}' "$sidecar")
      [[ "$actual_value" == "$expected_refinement" ]] || \
        fail "$row_id frozen refinement_exactness differs from its manifest field"
    fi
    if [[ "$expected_keep" != - ]]; then
      actual_value=$(awk -v want="$expected_keep" 'index($0,"\"keep_mask_kind\":\"" want "\""){found=1;exit}END{if(found)print want}' "$sidecar")
      [[ "$actual_value" == "$expected_keep" ]] || \
        fail "$row_id frozen sidecar lacks expected keep-mask evidence"
    fi
    expected_topology=none
    [[ "$topology" == none ]] || expected_topology="deterministic_selector:$topology"
    actual_value=$(awk 'index($0,"\"record\":\"contract\"")&&match($0,/"topology_selection":"[^"]+"/){v=substr($0,RSTART,RLENGTH);sub(/^.*:"/,"",v);sub(/"$/,"",v);print v;exit}' "$sidecar")
    [[ "$actual_value" == "$expected_topology" ]] || \
      fail "$row_id frozen topology-selection contract differs from its manifest field"
  done < <(awk -F '\t' '
    /^#/ {next} !header {for(i=1;i<=NF;i++)h[$i]=i;header=1;next}
    {print $h["row_id"],$h["method"],$h["expected_outcome"],
      $h["canonical_sidecar_uri"],$h["oracle_report_uri"],
      $h["oracle_search_semantic_sha256"],$h["expected_refinement_exactness"],
      $h["expected_keep_mask_kind"],$h["topology_selector"]}' OFS='\t' "$manifest")
}
if [[ -n "$workload_manifest" ]]; then
  [[ ! -e "$out_dir" && ! -L "$out_dir" ]] || \
    fail "manifest-mode output directory already exists (strict non-overwrite): $out_dir"
  base_manifest_sha=$(verify_manifest "$workload_manifest")
  verify_manifest_assets "$workload_manifest"
  [[ $(preamble_value "$workload_manifest" kind) == base ]] || \
    fail "base workload manifest must declare kind=base"
  BASE_SCHEMA=$(preamble_value "$workload_manifest" schema)
  BASE_SCHEMA_VERSION=$(preamble_value "$workload_manifest" schema_version)
  BASE_FROZEN_LARCH2_SHA=$(preamble_value "$workload_manifest" frozen_larch2_sha256)
  BASE_FROZEN_ORACLE_SHA=$(preamble_value "$workload_manifest" frozen_oracle_dagutil_sha256)
  frozen_larch2_path=$(manifest_uri_path "$workload_manifest" "$(preamble_value "$workload_manifest" frozen_larch2_uri)")
  frozen_oracle_path=$(manifest_uri_path "$workload_manifest" "$(preamble_value "$workload_manifest" frozen_oracle_dagutil_uri)")
  frozen_commands_path=$(manifest_uri_path "$workload_manifest" "$(preamble_value "$workload_manifest" commands_uri)")
  [[ -f "$frozen_larch2_path" && $(sha256sum "$frozen_larch2_path" | awk '{print $1}') == "$BASE_FROZEN_LARCH2_SHA" ]] || \
    fail "frozen native larch2 is missing or has the wrong hash"
  [[ $(sha256sum "$larch2" | awk '{print $1}') == "$BASE_FROZEN_LARCH2_SHA" ]] || \
    fail "--larch2 is not the manifest-frozen native binary"
  [[ -f "$frozen_oracle_path" && $(sha256sum "$frozen_oracle_path" | awk '{print $1}') == "$BASE_FROZEN_ORACLE_SHA" ]] || \
    fail "frozen semantic-oracle dagutil is missing or has the wrong hash"
  "$frozen_oracle_path" --help 2>&1 | \
    awk '/--canonical-dag-result[[:space:]]/{found=1} END{exit !found}' || \
    fail "frozen semantic-oracle dagutil lacks --canonical-dag-result"
  [[ -f "$frozen_commands_path" && $(sha256sum "$frozen_commands_path" | awk '{print $1}') == "$(preamble_value "$workload_manifest" commands_sha256)" ]] || \
    fail "frozen commands artifact is missing or has the wrong hash"
  manifest_files+=("$workload_manifest")
  declare -A manifest_row_ids=()
  [[ $(preamble_value "$workload_manifest" parent_sha256) == - ]] || \
    fail "base workload manifest must declare parent_sha256=-"
  for supplement in "${supplemental_manifests[@]}"; do
    supplement_sha=$(verify_manifest "$supplement")
    verify_manifest_assets "$supplement"
    [[ $(preamble_value "$supplement" kind) == supplement ]] || \
      fail "supplement must declare kind=supplement: $supplement"
    [[ $(preamble_value "$supplement" schema) == "$BASE_SCHEMA" && \
       $(preamble_value "$supplement" schema_version) == "$BASE_SCHEMA_VERSION" ]] || \
      fail "supplement schema differs from base: $supplement"
    [[ $(preamble_value "$supplement" frozen_larch2_sha256) == "$BASE_FROZEN_LARCH2_SHA" && \
       $(preamble_value "$supplement" frozen_oracle_dagutil_sha256) == "$BASE_FROZEN_ORACLE_SHA" ]] || \
      fail "supplement changes the frozen binary roles: $supplement"
    supplement_larch2=$(manifest_uri_path "$supplement" "$(preamble_value "$supplement" frozen_larch2_uri)")
    supplement_oracle=$(manifest_uri_path "$supplement" "$(preamble_value "$supplement" frozen_oracle_dagutil_uri)")
    supplement_commands=$(manifest_uri_path "$supplement" "$(preamble_value "$supplement" commands_uri)")
    [[ $(sha256sum "$supplement_larch2" | awk '{print $1}') == "$BASE_FROZEN_LARCH2_SHA" &&
       $(sha256sum "$supplement_oracle" | awk '{print $1}') == "$BASE_FROZEN_ORACLE_SHA" &&
       $(sha256sum "$supplement_commands" | awk '{print $1}') == "$(preamble_value "$supplement" commands_sha256)" ]] || \
      fail "supplement preamble asset hash mismatch: $supplement"
    parent_sha=$(preamble_value "$supplement" parent_sha256)
    [[ "$parent_sha" == "$base_manifest_sha" ]] || \
      fail "supplement parent hash does not match base: $supplement"
    manifest_files+=("$supplement")
  done
  for manifest in "${manifest_files[@]}"; do
    while IFS= read -r row_id; do
      [[ -z ${manifest_row_ids[$row_id]:-} ]] || fail "manifest row ID overridden: $row_id"
      manifest_row_ids[$row_id]=1
    done < <(awk -F '\t' '/^#/ {next} !header {for(i=1;i<=NF;i++) h[$i]=i; header=1; next} {print $h["row_id"]}' "$manifest")
  done
  if [[ -n "$run_manifest_group" ]]; then
    selected_group_rows=$(for manifest in "${manifest_files[@]}"; do
      awk -F '\t' -v group="$run_manifest_group" '/^#/{next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;next}$x["run_group"]==group{n++}END{print n+0}' "$manifest"
    done | awk '{n+=$1}END{print n+0}')
    (( selected_group_rows > 0 )) || fail "manifest group has no rows: $run_manifest_group"
  fi
fi

mkdir -p "$out_dir" "$out_dir/logs" "$out_dir/outputs" "$out_dir/curves"
raw_trials_tsv="$out_dir/raw_trials.tsv"
summary_tsv="$out_dir/summary.tsv"
summary_md="$out_dir/summary.md"
commands_log="$out_dir/commands.sh"
validation_queue="$out_dir/.deferred-validation.tsv"
: >"$commands_log"
: >"$validation_queue"

extract_value() {
  awk -v key="$2" '$0 ~ "^[[:space:]]*" key ":[[:space:]]*" {
    sub("^[[:space:]]*" key ":[[:space:]]*", ""); print; exit
  }' "$1"
}
# Chart-search summary scalars have exactly two leading spaces.  Acceptance
# evidence must never borrow a same-named value from a nested iteration/counter
# section, and duplicate top-level keys are ambiguous rather than "first wins".
extract_unique_chart_top_value() {
  awk -v key="$2" -v file="$1" '
    BEGIN { prefix = "  " key ": " }
    index($0, prefix) == 1 {
      count += 1
      value = substr($0, length(prefix) + 1)
    }
    END {
      if (count != 1) {
        printf "invalid chart report: top-level key %s occurs %d times in %s\n", key, count + 0, file > "/dev/stderr"
        exit 1
      }
      print value
    }
  ' "$1"
}
extract_chart_top_value() {
  if [[ ${ROW[status]:-} == pending_validation ]]; then
    extract_unique_chart_top_value "$@"
  else
    # Expected timeout/refusal rows have no chart report by construction.
    extract_value "$@"
  fi
}
extract_unique_chart_counter_value() {
  awk -v key="$2" -v file="$1" '
    $0 == "  counters:" {sections++; inside=1; next}
    inside && $0 ~ /^  [^ ]/ {inside=0}
    inside && index($0, "    " key ": ") == 1 {
      count++; value=substr($0,length("    " key ": ")+1)
    }
    END {
      if(sections!=1 || count!=1) {
        printf "invalid chart report: counters sections=%d, key %s occurs %d times in %s\n", sections+0,key,count+0,file > "/dev/stderr"
        exit 1
      }
      print value
    }' "$1"
}
extract_unique_chart_section_value() {
  awk -v section="$2" -v key="$3" -v file="$1" '
    $0 == "  " section ":" {sections++; inside=1; next}
    inside && $0 ~ /^  [^ ]/ {inside=0}
    inside && index($0, "    " key ": ") == 1 {
      count++; value=substr($0,length("    " key ": ")+1)
    }
    END {
      if(sections!=1 || count!=1) {
        printf "invalid chart report: section %s occurs %d times, key %s occurs %d times in %s\n",section,sections+0,key,count+0,file > "/dev/stderr"
        exit 1
      }
      print value
    }' "$1"
}
extract_counter_value() {
  if [[ ${ROW[status]:-} == pending_validation ]]; then
    extract_unique_chart_counter_value "$@"
  else
    # Timeout/refusal reports are empty by contract.
    extract_value "$@"
  fi
}
extract_unanimous_iteration_stop_reason() {
  awk -v file="$1" '
    function close_item() {
      if (have_item && (item_generation_count != 1 || item_stop_count != 1)) {
        bad_scope=1
      }
    }
    index($0,"  iterations: ")==1 {iteration_keys++; iterations=substr($0,length("  iterations: ")+1)}
    $0=="  iteration_reports:" {sections++; inside=1; next}
    inside && $0 ~ /^  [^ ]/ {close_item(); have_item=0; inside=0}
    inside && index($0,"    - iteration: ")==1 {
      close_item()
      item_count++; item=substr($0,length("    - iteration: ")+1)
      if(item !~ /^[0-9]+$/ || item+0 != item_count-1) bad_identity=1
      have_item=1; item_generation_count=0; item_stop_count=0; in_generation=0
      next
    }
    inside && $0=="      candidate_generation:" {
      if(!have_item) bad_scope=1
      item_generation_count++; generation_sections++; in_generation=1; next
    }
    inside && $0 ~ /^      [^ ]/ {in_generation=0}
    inside && index($0,"        stop_reason: ")==1 {
      if(!have_item || !in_generation) bad_scope=1
      item_stop_count++
      count++; value=substr($0,length("        stop_reason: ")+1); seen[value]=1
    }
    END {
      close_item()
      distinct=0; for(value in seen)distinct++
      if(iteration_keys!=1 || iterations!~/^[0-9]+$/ || sections!=1 ||
         item_count+0!=iterations+0 || bad_identity ||
         generation_sections+0!=iterations+0 ||
         count+0!=iterations+0 || count==0 || distinct!=1 || bad_scope) {
        printf "invalid chart report: iteration stop-reason contract is ambiguous in %s\n",file > "/dev/stderr"
        exit 1
      }
      for(value in seen) {
        if(value!="exhausted" && value!="candidate_cap" && value!="path_budget" && value!="callback_stop") {
          printf "invalid chart report: unknown iteration stop reason %s in %s\n",value,file > "/dev/stderr"
          exit 1
        }
        print value
      }
    }' "$1"
}
extract_parsimony_min() {
  awk 'match($0,/parsimony_min: score:[0-9]+/) {
    value=substr($0,RSTART,RLENGTH); sub(/^.*score:/,"",value); print value; exit
  }' "$1"
}
extract_json_string() {
  awk -v key="$2" '
    match($0, "\"" key "\"[[:space:]]*:[[:space:]]*\"[0-9A-Za-z_.-]+\"") {
      value=substr($0,RSTART,RLENGTH); sub(/^.*:[[:space:]]*"/,"",value); sub(/"$/,"",value)
      print value; exit
    }' "$1"
}
extract_json_number() {
  awk -v key="$2" '
    match($0, "\"" key "\"[[:space:]]*:[[:space:]]*[0-9]+") {
      value=substr($0,RSTART,RLENGTH); sub(/^.*:[[:space:]]*/,"",value); print value; exit
    }' "$1"
}
metric_value() {
  awk -F= -v key="$2" '$1==key {print substr($0,length(key)+2); exit}' "$1"
}

validate_process_metrics() {
  local file=$1 runner_status=$2 label=$3 expected_rss_limit=$4
  local expected_rss_bytes=0
  [[ "$expected_rss_limit" == - ]] || expected_rss_bytes=$expected_rss_limit
  awk -F= -v runner_status="$runner_status" -v expected_rss_bytes="$expected_rss_bytes" '
    function bad(message) {
      print "invalid process metrics: " message > "/dev/stderr"; failed=1
    }
    function isuint(value) {return value ~ /^(0|[1-9][0-9]*)$/}
    function isint(value) {return value ~ /^-?(0|[1-9][0-9]*)$/}
    function isdecimal(value) {return value ~ /^(0|[1-9][0-9]*)[.][0-9]+$/}
    BEGIN {
      required="schema_version outcome exit_code term_signal timed_out runner_exit_code wall_seconds user_seconds system_seconds max_rss_kb peak_sampled_rss_kb peak_sampled_swap_kb rss_kb_unit proc_status_samples proc_rss_samples proc_swap_samples proc_group_samples peak_sampled_process_count subreaper_enabled descendants_reaped post_leader_descendants descendant_cleanup_kill_sent live_descendants_at_return process_group_alive_at_return wait4_echild_at_return monitor_error monitor_error_count wait4_collected wait_errno child_error_stage child_error_errno core_dumped timeout_term_sent timeout_kill_sent rss_limit_bytes rss_limit_enabled rss_limit_observed rss_limit_exceeded rss_limit_trigger_bytes rss_limit_term_sent rss_limit_kill_sent"
      nrequired=split(required, names, " ")
      for(i=1;i<=nrequired;i++) allowed[names[i]]=1
    }
    {
      key=$1; value=substr($0,length(key)+2)
      if (!(key in allowed)) bad("unknown schema-v2 key " key)
      count[key]++; values[key]=value
    }
    END {
      for(i=1;i<=nrequired;i++) if(count[names[i]]!=1)
        bad("key " names[i] " occurs " (count[names[i]]+0) " times")
      if(values["schema_version"]!="2") bad("schema_version is not 2")
      if(values["rss_kb_unit"]!="1024_bytes") bad("rss_kb_unit is not 1024_bytes")
      if(!isint(values["exit_code"])) bad("exit_code is not an integer")
      if(!isuint(values["term_signal"])) bad("term_signal is not an unsigned integer")
      if(!isuint(values["runner_exit_code"]) || values["runner_exit_code"]+0 != runner_status+0)
        bad("runner_exit_code disagrees with process status")
      for(i=1;i<=3;i++) {
        key=(i==1?"wall_seconds":(i==2?"user_seconds":"system_seconds"))
        if(!isdecimal(values[key])) bad(key " is not a finite nonnegative decimal")
      }
      for(i=1;i<=8;i++) {
        key=(i==1?"max_rss_kb":(i==2?"peak_sampled_rss_kb":(i==3?"peak_sampled_swap_kb":(i==4?"proc_status_samples":(i==5?"proc_rss_samples":(i==6?"proc_swap_samples":(i==7?"proc_group_samples":"peak_sampled_process_count")))))))
        if(!isuint(values[key])) bad(key " is not an unsigned integer")
      }
      split("subreaper_enabled descendants_reaped post_leader_descendants descendant_cleanup_kill_sent live_descendants_at_return process_group_alive_at_return wait4_echild_at_return monitor_error monitor_error_count wait4_collected wait_errno child_error_errno core_dumped timeout_term_sent timeout_kill_sent rss_limit_bytes rss_limit_enabled rss_limit_observed rss_limit_exceeded rss_limit_trigger_bytes rss_limit_term_sent rss_limit_kill_sent", ints, " ")
      for(i=1;i<=22;i++) if(!isuint(values[ints[i]])) bad(ints[i] " is not an unsigned integer")
      split("timed_out subreaper_enabled descendant_cleanup_kill_sent process_group_alive_at_return wait4_echild_at_return monitor_error wait4_collected core_dumped timeout_term_sent timeout_kill_sent rss_limit_enabled rss_limit_observed rss_limit_exceeded rss_limit_term_sent rss_limit_kill_sent", bools, " ")
      for(i=1;i<=15;i++) if(values[bools[i]]!="0" && values[bools[i]]!="1") bad(bools[i] " is not boolean")
      outcome=values["outcome"]
      if(outcome!="exited" && outcome!="signaled" && outcome!="timeout" &&
         outcome!="rss_limit" && outcome!="exec_error" &&
         outcome!="setup_error" && outcome!="descendant_leak" &&
         outcome!="monitor_error" && outcome!="wait_error") bad("outcome is not closed")
      if(values["subreaper_enabled"]!="1" || values["live_descendants_at_return"]!="0" ||
         values["process_group_alive_at_return"]!="0" || values["wait4_echild_at_return"]!="1")
        bad("subreaper lifecycle did not close before return")
      if(outcome!="wait_error" && values["wait4_collected"]!="1")
        bad("leader was not collected")
      if((outcome=="wait_error") != (values["wait_errno"]+0>0))
        bad("wait-error outcome/errno disagree")
      if((outcome=="monitor_error") != (values["monitor_error"]=="1") ||
         ((values["monitor_error"]=="1") != (values["monitor_error_count"]+0>0)))
        bad("monitor-error outcome/count disagree")
      if((outcome=="timeout") != (values["timed_out"]=="1")) bad("timeout outcome/flag disagree")
      if(values["rss_limit_bytes"]+0 != expected_rss_bytes+0)
        bad("rss_limit_bytes disagrees with the selected row")
      expected_enabled=(expected_rss_bytes+0>0 ? "1" : "0")
      if(values["rss_limit_enabled"]!=expected_enabled)
        bad("rss_limit_enabled disagrees with the selected row")
      if(values["rss_limit_observed"]=="1") {
        if(values["rss_limit_enabled"]!="1" ||
           values["rss_limit_trigger_bytes"]+0<=values["rss_limit_bytes"]+0)
          bad("RSS observation lacks an over-limit trigger")
        if(values["rss_limit_trigger_bytes"]+0 > (values["peak_sampled_rss_kb"]+0)*1024)
          bad("RSS trigger exceeds sampled peak RSS")
      } else if(values["rss_limit_trigger_bytes"]!="0") {
        bad("unobserved RSS limit has a trigger")
      }
      if((outcome=="rss_limit") != (values["rss_limit_exceeded"]=="1"))
        bad("RSS-limit outcome/flag disagree")
      if(outcome=="timeout" && (runner_status+0!=124 || values["rss_limit_exceeded"]!="0" ||
         values["timeout_term_sent"]!="1" || values["rss_limit_term_sent"]!="0" ||
         values["rss_limit_kill_sent"]!="0")) bad("timeout has inconsistent enforcement state")
      if(outcome=="rss_limit") {
        if(runner_status+0!=123 || values["timed_out"]!="0" ||
           values["rss_limit_enabled"]!="1" || values["rss_limit_observed"]!="1" ||
           values["rss_limit_term_sent"]!="0" || values["rss_limit_kill_sent"]!="1" ||
           values["timeout_term_sent"]!="0" || values["timeout_kill_sent"]!="0")
          bad("RSS-limit outcome has inconsistent enforcement state")
      } else if(values["rss_limit_term_sent"]!="0" ||
                values["rss_limit_kill_sent"]!="0") {
        bad("non-RSS outcome has RSS enforcement state")
      }
      if(outcome=="descendant_leak" && (runner_status+0!=125 ||
         values["post_leader_descendants"]+0==0 || values["timed_out"]!="0"))
        bad("descendant-leak outcome is inconsistent")
      if(outcome=="monitor_error" && runner_status+0!=125)
        bad("monitor-error status is not 125")
      if(outcome=="wait_error" && runner_status+0!=125)
        bad("wait-error status is not 125")
      if(outcome=="exited" && (values["exit_code"]+0<0 || values["term_signal"]!="0"))
        bad("exited outcome has inconsistent status")
      if(outcome=="exited" && values["rss_limit_enabled"]=="1" &&
         (values["rss_limit_observed"]!="0" || values["rss_limit_exceeded"]!="0" ||
          values["rss_limit_trigger_bytes"]!="0" || values["rss_limit_term_sent"]!="0" ||
          values["rss_limit_kill_sent"]!="0"))
        bad("successful capped process has RSS observation or enforcement state")
      if(outcome=="signaled" && (values["exit_code"]!="-1" || values["term_signal"]+0==0))
        bad("signaled outcome has inconsistent status")
      stage=values["child_error_stage"]
      if(stage!="none" && stage!="restore_sigchld" &&
         stage!="set_process_group" && stage!="redirect_stdout" &&
         stage!="redirect_stderr" && stage!="exec") bad("child_error_stage is not closed")
      if(outcome=="exec_error" && stage!="exec") bad("exec error lacks exec stage")
      if(outcome=="setup_error" && (stage=="none" || stage=="exec")) bad("setup error stage is inconsistent")
      if(outcome!="exec_error" && outcome!="setup_error" && stage!="none")
        bad("ordinary outcome has a child-error stage")
      exit failed
    }
  ' "$file" || fail "process helper emitted an invalid contract for '$label'"
}

run_capture() {
  local label=$1 stdout=$2 stderr=$3 rss_limit=$4; shift 4
  local metrics_file=${stdout}.process_metrics status
  printf '# %s\n' "$label" >>"$commands_log"
  printf '%q ' "$@" >>"$commands_log"; printf '\n\n' >>"$commands_log"
  local runner=("$process_metrics" --timeout-seconds "$timeout_seconds"
    --stdout "$stdout" --stderr "$stderr")
  [[ "$rss_limit" == - ]] || runner+=(--rss-limit-bytes "$rss_limit")
  runner+=(--)
  set +e; "${runner[@]}" "$@" >"$metrics_file"; status=$?; set -e
  validate_process_metrics "$metrics_file" "$status" "$label" "$rss_limit"
  RUN_OUTCOME=$(metric_value "$metrics_file" outcome)
  RUN_RUNNER_STATUS=$(metric_value "$metrics_file" runner_exit_code)
  RUN_STATUS=$(metric_value "$metrics_file" exit_code); RUN_STATUS=${RUN_STATUS:-$status}
  RUN_TERM_SIGNAL=$(metric_value "$metrics_file" term_signal); RUN_TERM_SIGNAL=${RUN_TERM_SIGNAL:-0}
  RUN_TIMED_OUT=$(metric_value "$metrics_file" timed_out); RUN_TIMED_OUT=${RUN_TIMED_OUT:-0}
  RUN_WALL_S=$(metric_value "$metrics_file" wall_seconds)
  RUN_USER_S=$(metric_value "$metrics_file" user_seconds)
  RUN_SYSTEM_S=$(metric_value "$metrics_file" system_seconds)
  RUN_MAX_RSS_KB=$(metric_value "$metrics_file" max_rss_kb)
  RUN_PEAK_RSS_KB=$(metric_value "$metrics_file" peak_sampled_rss_kb)
  RUN_PEAK_SWAP_KB=$(metric_value "$metrics_file" peak_sampled_swap_kb)
  RUN_CORE_DUMPED=$(metric_value "$metrics_file" core_dumped)
  RUN_PROCESS_RSS_LIMIT_BYTES=$(metric_value "$metrics_file" rss_limit_bytes)
  RUN_RSS_LIMIT_ENABLED=$(metric_value "$metrics_file" rss_limit_enabled)
  RUN_RSS_LIMIT_OBSERVED=$(metric_value "$metrics_file" rss_limit_observed)
  RUN_RSS_LIMIT_EXCEEDED=$(metric_value "$metrics_file" rss_limit_exceeded)
  RUN_RSS_LIMIT_TRIGGER_BYTES=$(metric_value "$metrics_file" rss_limit_trigger_bytes)
  RUN_RSS_LIMIT_TERM_SENT=$(metric_value "$metrics_file" rss_limit_term_sent)
  RUN_RSS_LIMIT_KILL_SENT=$(metric_value "$metrics_file" rss_limit_kill_sent)
  RUN_MONITOR_ERROR=$(metric_value "$metrics_file" monitor_error)
}

canonical_argv_digest() {
  {
    printf 'wric-canonical-argv-v1\n'
    printf 'argc=%s\n' "$#"
    local arg
    for arg in "$@"; do
      [[ "$arg" != *$'\n'* ]] || fail "canonical argv token contains a newline"
      printf '%s:%s\n' "${#arg}" "$arg"
    done
  } | sha256sum | awk '{print $1}'
}
trial_semantic_digest() {
  local method=$1 search_sha=$2 output_sha=$3 argv_sha=$4
  {
    printf 'wric-trial-semantic-v1\n'
    printf 'method=%s\n' "$method"
    printf 'search_semantic_sha256=%s\n' "$search_sha"
    printf 'output_semantic_sha256=%s\n' "$output_sha"
    printf 'canonical_argv_sha256=%s\n' "$argv_sha"
  } | sha256sum | awk '{print $1}'
}
score_output() {
  local label=$1 pb=$2 out=$3 err=$4 rss_limit=$5
  local canonical=${out%.out}.canonical-dag.json oracle=$dagutil
  [[ -z "$workload_manifest" ]] || oracle=$frozen_oracle_path
  local canonical_args=()
  if "$oracle" --help 2>&1 | awk '/--canonical-dag-result[[:space:]]/{found=1} END{exit !found}'; then
    canonical_args=(--canonical-dag-result "$canonical")
  elif [[ -n "$workload_manifest" ]]; then
    fail "manifest mode requires frozen --canonical-dag-result support"
  fi
  run_capture "$label" "$out" "$err" "$rss_limit" "$oracle" --dag-pb "$pb" \
    --force-no-vcf --validate --dag-info "${canonical_args[@]}"
  SCORE_STATUS=$RUN_RUNNER_STATUS
  SCORE_OUTPUT_SHA=-; SCORE_OUTPUT_PARSIMONY=-; SCORE_CANONICAL_PATH=$canonical
  if (( SCORE_STATUS == 0 )) && (( ${#canonical_args[@]} )); then
    [[ -s "$canonical" ]] || fail "semantic oracle omitted canonical DAG result: $label"
    [[ $(extract_json_string "$canonical" schema) == larch.dag.semantic_digest &&
       $(extract_json_number "$canonical" schema_version) == 1 &&
       $(extract_json_string "$canonical" digest_algorithm) == sha256 ]] || \
      fail "invalid canonical DAG schema: $canonical"
    SCORE_OUTPUT_SHA=$(extract_json_string "$canonical" semantic_sha256)
    [[ "$SCORE_OUTPUT_SHA" =~ ^[0-9a-f]{64}$ ]] || \
      fail "canonical DAG result lacks semantic_sha256: $canonical"
    for digest_key in clades_sha256 productions_sha256; do
      digest_value=$(extract_json_string "$canonical" "$digest_key")
      [[ "$digest_value" =~ ^[0-9a-f]{64}$ ]] || \
        fail "canonical DAG result lacks $digest_key: $canonical"
    done
    for count_key in clade_count production_count parsimony_min; do
      count_value=$(extract_json_number "$canonical" "$count_key")
      [[ "$count_value" =~ ^[0-9]+$ ]] || \
        fail "canonical DAG result lacks $count_key: $canonical"
    done
    SCORE_OUTPUT_PARSIMONY=$(extract_json_number "$canonical" parsimony_min)
  elif (( SCORE_STATUS == 0 )); then
    SCORE_OUTPUT_PARSIMONY=$(extract_parsimony_min "$out")
    local fallback_nodes fallback_edges
    fallback_nodes=$(extract_value "$out" nodes); fallback_edges=$(extract_value "$out" edges)
    SCORE_OUTPUT_SHA=$({
      printf 'wric-nonmanifest-output-fallback-v1\n'
      printf 'parsimony_min=%s\nnodes=%s\nedges=%s\n' \
        "$SCORE_OUTPUT_PARSIMONY" "$fallback_nodes" "$fallback_edges"
    } | sha256sum | awk '{print $1}')
  fi
}

legacy_columns=(benchmark_scope fixture method status validation_status
  initial_validated_parsimony_min final_validated_parsimony_min
  best_reported_objective best_validated_parsimony_min wall_clock_s iterations seed
  acceptance candidate_selection candidate_source objective candidates_generated
  candidates_scored exact_verifications accepted_moves candidate_accepts_attempted
  post_materialization_rejections committed_attempt_ratio cache_build_ms
  local_scoring_ms exact_verification_ms accepted_rebuild_ms final_compaction_ms
  final_compaction_exactness_kind post_materialization_check_ms total_ms
  local_ms_per_candidate local_candidates_per_second affected_mean affected_p50
  affected_p95 affected_max upward_path_iterator_steps path_pairs_considered
  candidates_pruned_before_construction candidates_pruned_after_construction
  candidates_generated_after_dedup candidate_cap_cutoffs path_budget_cutoffs
  full_search_state_rebuilds final_compaction_rebuilds initial_search_state_rebuilds
  sidecar_rebuilds_after_accept overlay_materializations_for_exact_verification
  overlay_materializations_for_accept_materialization
  overlay_materializations_for_final_compaction overlay_materializations_for_oracle
  full_overlay_materializations reachable_clades_traversed
  reachable_productions_traversed reachability_full_grammar_like_passes
  grammar_clades grammar_productions active_patterns chart_cache_resident_bytes
  final_dag_nodes final_dag_edges curve_path report_path)
trial_columns=(row_id requested_workers resolved_workers worker_policy trial_index
  execution_order runner_outcome runner_exit_code exit_code term_signal core_dumped
  timed_out monitor_error rss_limit_enabled rss_limit_observed rss_limit_exceeded rss_limit_trigger_bytes
  rss_limit_term_sent rss_limit_kill_sent process_rss_limit_bytes
  user_cpu_s system_cpu_s max_rss_kb
  peak_sampled_rss_kb peak_sampled_swap_kb candidate_generation_ms
  exact_initialization_ms initial_chart_construction_ms materialization_ms
  materialization_exact_verification_ms materialization_accepted_update_ms
  materialization_final_compaction_ms peak_concurrent_exact_verifiers
  chart_axis_exact_candidate_active_worker_high_water
  exact_candidate_admission_batches exact_candidate_parallel_batches
  exact_candidate_inner_parallel_batches exact_candidate_memory_limited_batches
  exact_candidate_peak_admitted_bytes exact_candidate_peak_projected_resident_bytes
  exact_candidate_queued_for_memory_ms
  exact_candidate_timing_count
  exact_candidate_verification_ms_min exact_candidate_verification_ms_mean
  exact_candidate_verification_ms_max exact_ms_per_candidate
  configured_chart_memory_budget manifest_rss_limit_bytes input_sha256 refseq_sha256
  search_semantic_sha256 output_semantic_sha256 trial_semantic_sha256
  canonical_argv_sha256 canonical_digest)
(
  IFS=$'\t'; printf '%s\n' "${legacy_columns[*]}"$'\t'"${trial_columns[*]}"
) >"$raw_trials_tsv"
declare -A ROW=()
declare -A full_canonical_done=()
declare -A canonical_companion_search=()
declare -A canonical_companion_output=()
declare -A CANONICAL_QUEUE_COMMAND=()
declare -A CANONICAL_QUEUE_PB=()
declare -A CANONICAL_QUEUE_JSON=()
declare -A CANONICAL_QUEUE_OUT=()
declare -A CANONICAL_QUEUE_ERR=()
declare -A CANONICAL_QUEUE_FULL_COMMAND=()
declare -A CANONICAL_QUEUE_FULL_JSON=()
declare -A CANONICAL_QUEUE_FULL_SIDECAR=()
declare -A CANONICAL_QUEUE_ROW=()
declare -A CANONICAL_QUEUE_TIMEOUT=()
declare -A CANONICAL_QUEUE_RSS_LIMIT=()
canonical_queue_order=()

queue_output_validation() {
  local recorded=$1 row_id=$2 trial=$3 method=$4 pb=$5 initial=$6 report=$7 curve=$8 wall=$9
  local score_prefix=${10} row_timeout=${11} row_rss_limit=${12}
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$recorded" "$row_id" "$trial" "$method" "$pb" "$initial" "$report" \
    "$curve" "$wall" "$score_prefix" "$row_timeout" "$row_rss_limit" >>"$validation_queue"
}
reset_row() {
  ROW=()
  local col
  for col in "${legacy_columns[@]}" "${trial_columns[@]}"; do ROW[$col]=NA; done
  ROW[benchmark_scope]=CHART_SPR_PHASE0_SEARCH_COMPARISON
  ROW[trial_index]=$TRIAL_INDEX; ROW[execution_order]=$EXECUTION_ORDER
  ROW[input_sha256]=$CURRENT_INPUT_SHA256; ROW[refseq_sha256]=$CURRENT_REFSEQ_SHA256
  ROW[configured_chart_memory_budget]=${chart_memory_budget:-product-default}
}
emit_row() {
  local col first=1
  for col in "${legacy_columns[@]}" "${trial_columns[@]}"; do
    (( first )) || printf '\t' >>"$raw_trials_tsv"; first=0
    printf '%s' "${ROW[$col]:-NA}" >>"$raw_trials_tsv"
  done
  printf '\n' >>"$raw_trials_tsv"
}
set_process_row() {
  ROW[runner_outcome]=$RUN_OUTCOME; ROW[runner_exit_code]=$RUN_RUNNER_STATUS
  ROW[exit_code]=$RUN_STATUS; ROW[term_signal]=$RUN_TERM_SIGNAL
  ROW[core_dumped]=$RUN_CORE_DUMPED
  ROW[timed_out]=$RUN_TIMED_OUT; ROW[wall_clock_s]=$RUN_WALL_S
  ROW[monitor_error]=$RUN_MONITOR_ERROR
  ROW[rss_limit_enabled]=$RUN_RSS_LIMIT_ENABLED
  ROW[rss_limit_observed]=$RUN_RSS_LIMIT_OBSERVED
  ROW[rss_limit_exceeded]=$RUN_RSS_LIMIT_EXCEEDED
  ROW[rss_limit_trigger_bytes]=$RUN_RSS_LIMIT_TRIGGER_BYTES
  ROW[rss_limit_term_sent]=$RUN_RSS_LIMIT_TERM_SENT
  ROW[rss_limit_kill_sent]=$RUN_RSS_LIMIT_KILL_SENT
  ROW[process_rss_limit_bytes]=$RUN_PROCESS_RSS_LIMIT_BYTES
  ROW[user_cpu_s]=$RUN_USER_S; ROW[system_cpu_s]=$RUN_SYSTEM_S
  ROW[max_rss_kb]=$RUN_MAX_RSS_KB; ROW[peak_sampled_rss_kb]=$RUN_PEAK_RSS_KB
  ROW[peak_sampled_swap_kb]=$RUN_PEAK_SWAP_KB
}

# Resolve one exact immutable workload tuple. `NA` means method-specific not
# applicable; it is an exact value, never a wildcard.
resolve_manifest_row() {
  local method=$1 workers=$2 match_file matches=()
  if [[ -z "$workload_manifest" ]]; then
    RESOLVED_ROW_ID="${CURRENT_FIXTURE}/${method}@${workers}"
    RESOLVED_EXPECTED_STATUS=ok; RESOLVED_EXPECTED_TIMEOUTS=0
    RESOLVED_EXPECTED_CANDIDATES=-; RESOLVED_EXPECTED_EXACT=-
    RESOLVED_EXPECTED_WORKERS=-; RESOLVED_RSS_LIMIT_BYTES=$capture_rss_limit_bytes
    RESOLVED_EXPECTED_INITIAL=-; RESOLVED_EXPECTED_FINAL=-
    RESOLVED_EXPECTED_VALIDATED=-; RESOLVED_ORACLE_SEARCH_SHA=-
    RESOLVED_ORACLE_OUTPUT_SHA=-; RESOLVED_ORACLE_TRIAL_SHA=-
    RESOLVED_CANONICAL_ARGV_SHA=-; RESOLVED_EXPECTED_REASON_CODE=-
    RESOLVED_EXPECTED_REASON_SHA=-; RESOLVED_SCALE_RESOURCE=-
    RESOLVED_SCALE_LIMIT=-; RESOLVED_SCALE_CANDIDATES=-; RESOLVED_SCALE_EXACT=-
    return
  fi
  if [[ -n ${FORCED_MANIFEST_ROW_ID:-} ]]; then
    RESOLVED_ROW_ID=$FORCED_MANIFEST_ROW_ID
    local forced_method forced_requested
    forced_method=$(manifest_row_value "$RESOLVED_ROW_ID" method)
    forced_requested=$(manifest_row_value "$RESOLVED_ROW_ID" requested_workers)
    [[ "$forced_method" == "$method" ]] || \
      fail "internal manifest-group method mismatch for $RESOLVED_ROW_ID"
    if [[ "$method" == sample_explore_merge ]]; then
      [[ "$workers" == native && "$forced_requested" == - ]] || \
        fail "internal manifest-group native worker mismatch for $RESOLVED_ROW_ID"
    else
      local passed=$workers; [[ "$passed" == auto ]] && passed=0
      [[ "$passed" == "$forced_requested" ]] || \
        fail "internal manifest-group chart worker mismatch for $RESOLVED_ROW_ID"
    fi
    RESOLVED_EXPECTED_STATUS=$(manifest_row_value "$RESOLVED_ROW_ID" expected_outcome)
    RESOLVED_EXPECTED_TIMEOUTS=$(manifest_row_value "$RESOLVED_ROW_ID" expected_timeout_trials)
    RESOLVED_EXPECTED_CANDIDATES=$(manifest_row_value "$RESOLVED_ROW_ID" expected_candidates_scored)
    RESOLVED_EXPECTED_EXACT=$(manifest_row_value "$RESOLVED_ROW_ID" expected_exact_verifications)
    RESOLVED_EXPECTED_WORKERS=$(manifest_row_value "$RESOLVED_ROW_ID" expected_resolved_workers)
    RESOLVED_RSS_LIMIT_BYTES=$(manifest_row_value "$RESOLVED_ROW_ID" rss_limit_bytes)
    RESOLVED_EXPECTED_INITIAL=$(manifest_row_value "$RESOLVED_ROW_ID" expected_initial_score)
    RESOLVED_EXPECTED_FINAL=$(manifest_row_value "$RESOLVED_ROW_ID" expected_final_score)
    RESOLVED_EXPECTED_VALIDATED=$(manifest_row_value "$RESOLVED_ROW_ID" expected_validated_parsimony)
    RESOLVED_ORACLE_SEARCH_SHA=$(manifest_row_value "$RESOLVED_ROW_ID" oracle_search_semantic_sha256)
    RESOLVED_ORACLE_OUTPUT_SHA=$(manifest_row_value "$RESOLVED_ROW_ID" oracle_output_semantic_sha256)
    RESOLVED_ORACLE_TRIAL_SHA=$(manifest_row_value "$RESOLVED_ROW_ID" oracle_trial_semantic_sha256)
    RESOLVED_CANONICAL_ARGV_SHA=$(manifest_row_value "$RESOLVED_ROW_ID" canonical_argv_sha256)
    RESOLVED_EXPECTED_REASON_CODE=$(manifest_row_value "$RESOLVED_ROW_ID" expected_reason_code)
    RESOLVED_EXPECTED_REASON_SHA=$(manifest_row_value "$RESOLVED_ROW_ID" expected_reason_sha256)
    RESOLVED_SCALE_RESOURCE=$(manifest_row_value "$RESOLVED_ROW_ID" scale_resource)
    RESOLVED_SCALE_LIMIT=$(manifest_row_value "$RESOLVED_ROW_ID" scale_limit)
    RESOLVED_SCALE_CANDIDATES=$(manifest_row_value "$RESOLVED_ROW_ID" scale_largest_candidates)
    RESOLVED_SCALE_EXACT=$(manifest_row_value "$RESOLVED_ROW_ID" scale_largest_top_k)
    return
  fi
  local manifest_workers=$workers
  [[ "$manifest_workers" == native ]] && manifest_workers=-
  [[ "$manifest_workers" == auto ]] && manifest_workers=0
  local native_moves=- chart_candidates=- chart_exact=- worker_option=none
  local acceptance=- objective=- selection=- source=- topology=-
  local poly=- exact_arity=- shapes=- max_productions=- max_clades=-
  local lazy=- max_cached=- pattern_batch=- candidate_batch=- memory=-
  local commit=- verification=- local_accept=- dominance=- pruning=- keep_mask=- frontier=- score_ua=- force_no_vcf=-
  local binary_role=frozen_native manifest_kind=dag_pb manifest_ref=$CURRENT_REFSEQ_SHA256
  [[ "$CURRENT_KIND" == tree ]] && manifest_kind=tree_pb_refseq
  [[ "$manifest_ref" == NA ]] && manifest_ref=-
  if [[ "$method" == sample_explore_merge ]]; then
    native_moves=$max_moves
  else
    binary_role=working_chart
    chart_candidates=$max_candidates; chart_exact=$top_k_exact
    if [[ "$workers" == default ]]; then worker_option=none
    elif (( workers_list_explicit || chart_workers_explicit )); then worker_option=chart_spr_workers
    elif (( local_workers_explicit )); then worker_option=chart_spr_local_score_workers
    else worker_option=none; fi
    acceptance=$ACCEPTANCE; [[ "$acceptance" == exact ]] && acceptance=exact_multisite
    [[ "$acceptance" == fixed-topology ]] && acceptance=fixed_topology_exact
    [[ "$acceptance" == lower-bound ]] && acceptance=lower_bound_heuristic
    objective=$OBJECTIVE; selection=lower_bound_top_k; source=$SOURCE
    topology=none
    [[ "$method" == chart_spr_sampled_tree_fixed_topology ]] && \
      topology=first_reachable_overlay_topology
    poly=$polytomy_mode; exact_arity=6; shapes=$polytomy_shapes
    max_productions=1024; max_clades=256; lazy=$chart_lazy_policy
    max_cached=0; pattern_batch=0; candidate_batch=0; memory=${chart_memory_budget:-0}
    commit=overlay_delta; verification=transient
    (( local_accept_updates )) && local_accept=true || local_accept=false
    dominance=off; pruning=true; keep_mask=true; frontier=0; score_ua=false; force_no_vcf=true
  fi
  for match_file in "${manifest_files[@]}"; do
    while IFS=$'\t' read -r rid estatus etimeout ecandidates eexact eresolved rss_limit einitial efinal evalidated osearch ooutput otrial argv reason reason_sha scale_resource scale_limit scale_candidates scale_exact; do
      matches+=("$rid|$estatus|$etimeout|$ecandidates|$eexact|$eresolved|$rss_limit|$einitial|$efinal|$evalidated|$osearch|$ooutput|$otrial|$argv|$reason|$reason_sha|$scale_resource|$scale_limit|$scale_candidates|$scale_exact")
    done < <(awk -F '\t' \
      -v group="$run_manifest_group" \
      -v method="$method" -v workers="$manifest_workers" -v input="$CURRENT_INPUT_SHA256" \
      -v ref="$manifest_ref" -v input_kind="$manifest_kind" -v binary_role="$binary_role" \
      -v iterations="$iterations" -v seed="$seed" \
      -v native_moves="$native_moves" -v candidates="$chart_candidates" -v exact="$chart_exact" \
      -v timeout="$timeout_seconds" -v affinity="$AFFINITY_CPUS" -v worker_option="$worker_option" \
      -v acceptance="$acceptance" -v objective="$objective" -v selection="$selection" -v source="$source" \
      -v topology="$topology" -v poly="$poly" -v exact_arity="$exact_arity" -v shapes="$shapes" \
      -v max_productions="$max_productions" -v max_clades="$max_clades" -v lazy="$lazy" \
      -v max_cached="$max_cached" -v pattern_batch="$pattern_batch" -v candidate_batch="$candidate_batch" \
      -v memory="$memory" -v commit="$commit" -v verification="$verification" -v local="$local_accept" \
      -v dominance="$dominance" -v pruning="$pruning" -v keep_mask="$keep_mask" -v frontier="$frontier" \
      -v score_ua="$score_ua" -v force_no_vcf="$force_no_vcf" '
      BEGIN {OFS="\t"}
      /^#/ {next}
      !header {for(i=1;i<=NF;i++) h[$i]=i; header=1; next}
      function val(name) {return $(h[name])}
      function same(name,want) {return val(name)==want}
      (group=="" || same("run_group",group)) && same("method",method) &&
      same("requested_workers",workers) && same("worker_option",worker_option) &&
      same("binary_role",binary_role) && same("input_kind",input_kind) &&
      same("primary_sha256",input) && same("refseq_sha256",ref) &&
      same("affinity_cpus",affinity) && same("iterations",iterations) && same("seed",seed) &&
      same("native_max_moves",native_moves) && same("chart_max_candidates",candidates) &&
      same("chart_top_k_exact",exact) && same("timeout_seconds",timeout) &&
      same("candidate_cap_semantics",method=="sample_explore_merge"?"-":"post-dedup") &&
      same("acceptance",acceptance) && same("objective",objective) &&
      same("candidate_selection",selection) && same("candidate_source",source) &&
      same("topology_selector",topology) &&
      same("randomize_order",method=="sample_explore_merge"?"-":"false") &&
      same("reservoir_sample",method=="sample_explore_merge"?"-":"false") &&
      same("include_immediate_reversals",method=="sample_explore_merge"?"-":"false") &&
      same("sampled_tree_count",method=="sample_explore_merge"?"-":"1") &&
      same("sampled_tree_radius",method=="sample_explore_merge"?"-":"0") &&
      same("sampled_tree_score_threshold",method=="sample_explore_merge"?"-":"2147483647") &&
      same("max_upward_path_expansions",method=="sample_explore_merge"?"-":"0") &&
      same("max_path_pairs",method=="sample_explore_merge"?"-":"0") &&
      same("min_moved_clade_size",method=="sample_explore_merge"?"-":"1") &&
      same("max_moved_clade_size",method=="sample_explore_merge"?"-":"0") &&
      same("min_target_clade_size",method=="sample_explore_merge"?"-":"1") &&
      same("max_target_clade_size",method=="sample_explore_merge"?"-":"0") &&
      same("max_affected_clades",method=="sample_explore_merge"?"-":"0") &&
      same("polytomy_mode",poly) && same("polytomy_max_exact_arity",exact_arity) &&
      same("polytomy_max_shapes",shapes) && same("polytomy_max_productions",max_productions) &&
      same("polytomy_max_clades",max_clades) && same("lazy_policy",lazy) &&
      same("max_cached_patterns",max_cached) && same("pattern_batch_size",pattern_batch) &&
      same("candidate_batch_size",candidate_batch) && same("memory_budget_bytes",memory) &&
      same("commit_mode",commit) && same("verification_mode",verification) &&
      same("local_accept_updates",local) && same("dominance_mode",dominance) &&
      same("bound_pruning",pruning) && same("require_exact_keep_mask",keep_mask) &&
      same("max_frontier_entries",frontier) && same("score_ua_edge",score_ua) &&
      same("validate","true") && same("force_no_vcf",force_no_vcf) {
        print val("row_id"), val("expected_outcome"), val("expected_timeout_trials"),
          val("expected_candidates_scored"), val("expected_exact_verifications"),
          val("expected_resolved_workers"), val("rss_limit_bytes"),
          val("expected_initial_score"), val("expected_final_score"),
          val("expected_validated_parsimony"), val("oracle_search_semantic_sha256"),
          val("oracle_output_semantic_sha256"), val("oracle_trial_semantic_sha256"),
          val("canonical_argv_sha256"), val("expected_reason_code"),
          val("expected_reason_sha256"), val("scale_resource"), val("scale_limit"),
          val("scale_largest_candidates"), val("scale_largest_top_k")
      }' "$match_file")
  done
  (( ${#matches[@]} == 1 )) || fail "manifest resolved ${#matches[@]} rows for $method@$workers ($CURRENT_FIXTURE)"
  IFS='|' read -r RESOLVED_ROW_ID RESOLVED_EXPECTED_STATUS \
    RESOLVED_EXPECTED_TIMEOUTS RESOLVED_EXPECTED_CANDIDATES \
    RESOLVED_EXPECTED_EXACT RESOLVED_EXPECTED_WORKERS RESOLVED_RSS_LIMIT_BYTES \
    RESOLVED_EXPECTED_INITIAL RESOLVED_EXPECTED_FINAL RESOLVED_EXPECTED_VALIDATED \
    RESOLVED_ORACLE_SEARCH_SHA RESOLVED_ORACLE_OUTPUT_SHA RESOLVED_ORACLE_TRIAL_SHA \
    RESOLVED_CANONICAL_ARGV_SHA RESOLVED_EXPECTED_REASON_CODE RESOLVED_EXPECTED_REASON_SHA \
    RESOLVED_SCALE_RESOURCE RESOLVED_SCALE_LIMIT RESOLVED_SCALE_CANDIDATES \
    RESOLVED_SCALE_EXACT <<<"${matches[0]}"
}
manifest_row_value() {
  local row_id=$1 field=$2 manifest value
  [[ -n "$workload_manifest" ]] || { printf '%s' -; return; }
  for manifest in "${manifest_files[@]}"; do
    value=$(awk -F '\t' -v id="$row_id" -v field="$field" '
      /^#/ {next} !header {for(i=1;i<=NF;i++)h[$i]=i;header=1;next}
      $h["row_id"]==id {print $h[field];exit}' "$manifest")
    [[ -z "$value" ]] || { printf '%s' "$value"; return; }
  done
  fail "manifest field lookup failed: $row_id/$field"
}
manifest_file_for_row() {
  local row_id=$1 manifest
  for manifest in "${manifest_files[@]}"; do
    if awk -F '\t' -v id="$row_id" '/^#/{next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;next}$x["row_id"]==id{found=1;exit}END{exit !found}' "$manifest"; then
      printf '%s' "$manifest"; return
    fi
  done
  fail "manifest row not found: $row_id"
}
manifest_bool_flag() {
  local row_id=$1 field=$2 flag=$3 value
  value=$(manifest_row_value "$row_id" "$field")
  [[ "$value" == false ]] || MANIFEST_CHART_ARGS_NO_WORKER+=("$flag")
}

# Key for the exact structural, scheduling, and resource contract that a
# scale-limit grammar-exact row shares with its full lower-bound companion.
# Method/acceptance/objective/topology and top-K are deliberately excluded;
# the companion is lower-bound-only with top-K zero.  Input identity is based
# on bytes as well as labels so renamed fixtures cannot satisfy the closure.
scale_companion_contract_key() {
  local row_id=$1 field
  local fields=(fixture_id input_kind primary_sha256 secondary_sha256 refseq_sha256
    worker_option requested_workers expected_resolved_workers affinity_cpus
    timeout_seconds rss_limit_bytes iterations seed chart_max_candidates
    candidate_cap_semantics candidate_selection candidate_source randomize_order
    reservoir_sample include_immediate_reversals sampled_tree_count
    sampled_tree_radius sampled_tree_score_threshold max_upward_path_expansions
    max_path_pairs min_moved_clade_size max_moved_clade_size
    min_target_clade_size max_target_clade_size max_affected_clades
    polytomy_mode polytomy_max_exact_arity polytomy_max_shapes
    polytomy_max_productions polytomy_max_clades lazy_policy
    max_cached_patterns pattern_batch_size candidate_batch_size
    memory_budget_bytes commit_mode verification_mode local_accept_updates
    dominance_mode bound_pruning require_exact_keep_mask max_frontier_entries
    score_ua_edge validate force_no_vcf)
  for field in "${fields[@]}"; do
    printf '%s\037' "$(manifest_row_value "$row_id" "$field")"
  done
}
# Exact workload identity shared by an omitted/default worker row and its
# explicit-auto companion.  Only invocation-policy identity and the digests
# that necessarily cover argv are excluded; every algorithm, input, resource,
# expected-work, and frozen semantic field must otherwise agree.
default_auto_contract_key() {
  local row_id=$1 field
  local IFS=$'\t' fields=()
  read -r -a fields <<<"$manifest_header"
  for field in "${fields[@]}"; do
    case "$field" in
      row_id|worker_option|requested_workers|expected_resolved_workers|expected_worker_policy|oracle_trial_semantic_sha256|canonical_argv_sha256)
        continue ;;
    esac
    printf '%s\037' "$(manifest_row_value "$row_id" "$field")"
  done
}
# Worker-independent identity for Phase-6 admission and RSS comparisons.
# Expected results and frozen report artifacts are deliberately excluded:
# those are observations, whereas this key represents the input, algorithm,
# work, and resource contract that explicit W1 and W8 rows must share.
phase6_worker_contract_key() {
  local row_id=$1 field
  local fields=(run_group workload_name fixture_id method input_kind primary_sha256
    secondary_sha256 refseq_sha256 binary_role worker_option affinity_cpus timeout_seconds
    rss_limit_bytes iterations seed native_max_moves chart_max_candidates
    chart_top_k_exact candidate_cap_semantics acceptance objective
    candidate_selection candidate_source topology_selector randomize_order
    reservoir_sample include_immediate_reversals sampled_tree_count
    sampled_tree_radius sampled_tree_score_threshold max_upward_path_expansions
    max_path_pairs min_moved_clade_size max_moved_clade_size
    min_target_clade_size max_target_clade_size max_affected_clades
    polytomy_mode polytomy_max_exact_arity polytomy_max_shapes
    polytomy_max_productions polytomy_max_clades lazy_policy
    max_cached_patterns pattern_batch_size candidate_batch_size
    memory_budget_bytes commit_mode verification_mode local_accept_updates
    dominance_mode bound_pruning require_exact_keep_mask max_frontier_entries
    score_ua_edge validate force_no_vcf)
  for field in "${fields[@]}"; do
    printf '%s\037' "$(manifest_row_value "$row_id" "$field")"
  done
}
phase6_worker_comparison_key() {
  local row_id=$1 field
  local fields=(run_group workload_name fixture_id method input_kind primary_sha256
    secondary_sha256 refseq_sha256 worker_option chart_top_k_exact)
  for field in "${fields[@]}"; do
    printf '%s\037' "$(manifest_row_value "$row_id" "$field")"
  done
}
configure_manifest_row() {
  local row_id=$1 manifest method input_kind uri ref_uri option requested
  local acceptance topology value frontier
  FORCED_MANIFEST_ROW_ID=$row_id
  manifest=$(manifest_file_for_row "$row_id")
  method=$(manifest_row_value "$row_id" method)
  CURRENT_FIXTURE=$(manifest_row_value "$row_id" workload_name)
  CURRENT_FIXTURE_ID=$(manifest_row_value "$row_id" fixture_id)
  input_kind=$(manifest_row_value "$row_id" input_kind)
  uri=$(manifest_row_value "$row_id" primary_uri)
  CURRENT_INPUT=$(manifest_uri_path "$manifest" "$uri")
  CURRENT_INPUT_SHA256=$(manifest_row_value "$row_id" primary_sha256)
  [[ $(sha256sum "$CURRENT_INPUT" | awk '{print $1}') == "$CURRENT_INPUT_SHA256" ]] || \
    fail "selected manifest row input changed: $row_id"
  CURRENT_REFSEQ=; CURRENT_REFSEQ_SHA256=NA; CURRENT_KIND=dag
  if [[ "$input_kind" == tree_pb_refseq ]]; then
    CURRENT_KIND=tree
    ref_uri=$(manifest_row_value "$row_id" refseq_uri)
    CURRENT_REFSEQ=$(manifest_uri_path "$manifest" "$ref_uri")
    CURRENT_REFSEQ_SHA256=$(manifest_row_value "$row_id" refseq_sha256)
    [[ $(sha256sum "$CURRENT_REFSEQ" | awk '{print $1}') == "$CURRENT_REFSEQ_SHA256" ]] || \
      fail "selected manifest row refseq changed: $row_id"
  fi
  [[ $(manifest_row_value "$row_id" affinity_cpus) == "$AFFINITY_CPUS" ]] || \
    fail "selected row affinity differs from current affinity: $row_id"
  iterations=$(manifest_row_value "$row_id" iterations)
  seed=$(manifest_row_value "$row_id" seed)
  timeout_seconds=$(manifest_row_value "$row_id" timeout_seconds)
  max_moves=$(manifest_row_value "$row_id" native_max_moves)
  max_candidates=$(manifest_row_value "$row_id" chart_max_candidates)
  top_k_exact=$(manifest_row_value "$row_id" chart_top_k_exact)
  chart_memory_budget=$(manifest_row_value "$row_id" memory_budget_bytes)
  polytomy_mode=$(manifest_row_value "$row_id" polytomy_mode)
  polytomy_shapes=$(manifest_row_value "$row_id" polytomy_max_shapes)
  chart_lazy_policy=$(manifest_row_value "$row_id" lazy_policy)
  local_accept_updates=0
  [[ $(manifest_row_value "$row_id" local_accept_updates) == true ]] && local_accept_updates=1
  local_workers_explicit=0; chart_workers_explicit=0; workers_list_explicit=0
  MANIFEST_CHART_ARGS_NO_WORKER=()
  MANIFEST_TIMED_WORKER_ARGS=()
  MANIFEST_MODE=
  MANIFEST_WORKER=native
  if [[ "$method" == sample_explore_merge ]]; then
    [[ "$max_moves" != - ]] || fail "native manifest row lacks max moves: $row_id"
    return
  fi
  [[ "$max_candidates" != - && "$top_k_exact" != - ]] || \
    fail "chart manifest row lacks work budget: $row_id"
  case "$method" in
    chart_spr_sampled_tree_fixed_topology) MANIFEST_MODE=sampled_tree_fixed ;;
    chart_spr_grammar_exact) MANIFEST_MODE=grammar_exact ;;
    chart_spr_hybrid_exact) MANIFEST_MODE=hybrid_exact ;;
    chart_spr_grammar_lower_bound_heuristic) MANIFEST_MODE=grammar_lower_bound ;;
    *) fail "unsupported manifest chart method: $method" ;;
  esac
  option=$(manifest_row_value "$row_id" worker_option)
  requested=$(manifest_row_value "$row_id" requested_workers)
  MANIFEST_WORKER=$requested
  case "$option" in
    none) ;;
    chart_spr_workers)
      chart_workers_explicit=1
      MANIFEST_TIMED_WORKER_ARGS=(--chart-spr-workers "$requested") ;;
    chart_spr_local_score_workers)
      local_workers_explicit=1
      MANIFEST_TIMED_WORKER_ARGS=(--chart-spr-local-score-workers "$requested") ;;
    *) fail "unsupported manifest worker option: $row_id" ;;
  esac
  acceptance=$(manifest_row_value "$row_id" acceptance)
  topology=$(manifest_row_value "$row_id" topology_selector)
  MANIFEST_CHART_ARGS_NO_WORKER=(--validate)
  [[ $(manifest_row_value "$row_id" force_no_vcf) == false ]] || \
    MANIFEST_CHART_ARGS_NO_WORKER+=(--force-no-vcf)
  MANIFEST_CHART_ARGS_NO_WORKER+=(
    --wric-polytomy-mode "$(manifest_row_value "$row_id" polytomy_mode)"
    --wric-polytomy-max-exact-arity "$(manifest_row_value "$row_id" polytomy_max_exact_arity)"
    --wric-polytomy-max-shapes "$(manifest_row_value "$row_id" polytomy_max_shapes)"
    --wric-polytomy-max-productions "$(manifest_row_value "$row_id" polytomy_max_productions)"
    --wric-polytomy-max-clades "$(manifest_row_value "$row_id" polytomy_max_clades)"
    --wric-lazy-chart "$chart_lazy_policy"
    --chart-spr-search --chart-spr-max-iterations "$iterations"
    --chart-spr-max-candidates "$max_candidates" --chart-spr-top-k-exact "$top_k_exact"
    --chart-spr-candidate-selection "$(manifest_row_value "$row_id" candidate_selection)"
    --chart-spr-candidate-source "$(manifest_row_value "$row_id" candidate_source)"
    --chart-spr-acceptance "$acceptance")
  [[ "$topology" == none ]] || MANIFEST_CHART_ARGS_NO_WORKER+=(--chart-spr-topology-selector "$topology")
  manifest_bool_flag "$row_id" randomize_order --chart-spr-randomize-order
  manifest_bool_flag "$row_id" reservoir_sample --chart-spr-reservoir-sample
  manifest_bool_flag "$row_id" include_immediate_reversals --chart-spr-include-immediate-reversals
  MANIFEST_CHART_ARGS_NO_WORKER+=(
    --chart-spr-sampled-tree-count "$(manifest_row_value "$row_id" sampled_tree_count)"
    --chart-spr-sampled-tree-radius "$(manifest_row_value "$row_id" sampled_tree_radius)"
    --chart-spr-max-upward-path-expansions "$(manifest_row_value "$row_id" max_upward_path_expansions)"
    --chart-spr-max-path-pairs "$(manifest_row_value "$row_id" max_path_pairs)"
    --chart-spr-min-moved-clade-size "$(manifest_row_value "$row_id" min_moved_clade_size)"
    --chart-spr-max-moved-clade-size "$(manifest_row_value "$row_id" max_moved_clade_size)"
    --chart-spr-min-target-clade-size "$(manifest_row_value "$row_id" min_target_clade_size)"
    --chart-spr-max-target-clade-size "$(manifest_row_value "$row_id" max_target_clade_size)"
    --chart-spr-max-cached-patterns "$(manifest_row_value "$row_id" max_cached_patterns)"
    --chart-spr-pattern-batch-size "$(manifest_row_value "$row_id" pattern_batch_size)"
    --chart-spr-candidate-batch-size "$(manifest_row_value "$row_id" candidate_batch_size)"
    --chart-spr-memory-budget "$chart_memory_budget"
    --chart-spr-commit-mode "$(manifest_row_value "$row_id" commit_mode)"
    --chart-spr-verification-mode "$(manifest_row_value "$row_id" verification_mode)"
    --chart-bnb-dominance "$(manifest_row_value "$row_id" dominance_mode)")
  # Product CLI zero means "no frontier cap" by omitting this positive-only
  # option.  The manifest/report contract still records and verifies literal
  # zero, while positive caps remain explicit canonical argv members.
  frontier=$(manifest_row_value "$row_id" max_frontier_entries)
  [[ "$frontier" == 0 ]] || \
    MANIFEST_CHART_ARGS_NO_WORKER+=(--chart-bnb-max-frontier "$frontier")
  [[ $(manifest_row_value "$row_id" local_accept_updates) == false ]] || \
    MANIFEST_CHART_ARGS_NO_WORKER+=(--chart-spr-local-accept-updates)
  [[ $(manifest_row_value "$row_id" bound_pruning) == true ]] || \
    MANIFEST_CHART_ARGS_NO_WORKER+=(--chart-bnb-no-bound-pruning)
  [[ $(manifest_row_value "$row_id" require_exact_keep_mask) == true ]] || \
    MANIFEST_CHART_ARGS_NO_WORKER+=(--chart-bnb-score-only)
  [[ $(manifest_row_value "$row_id" score_ua_edge) == false ]] || \
    MANIFEST_CHART_ARGS_NO_WORKER+=(--chart-score-ua-edge)
  [[ $(manifest_row_value "$row_id" sampled_tree_score_threshold) == 2147483647 ]] || \
    fail "product has no CLI for nondefault sampled_tree_score_threshold: $row_id"
  [[ $(manifest_row_value "$row_id" max_affected_clades) == 0 ]] || \
    fail "product has no CLI for nondefault max_affected_clades: $row_id"
  MANIFEST_CHART_ARGS_NO_WORKER+=(--seed "$seed")
}
require_report_match() {
  local row_id=$1 manifest_field=$2 report_key=$3 report=$4
  local scope=${5:-top} expected actual
  expected=$(manifest_row_value "$row_id" "$manifest_field")
  [[ "$expected" == - ]] && return
  if [[ "$scope" == iteration_stop_reason ]]; then
    if ! actual=$(extract_unanimous_iteration_stop_reason "$report"); then
      ROW[status]=workload_mismatch
      return
    fi
  elif ! actual=$(extract_unique_chart_top_value "$report" "$report_key"); then
    ROW[status]=workload_mismatch
    return
  fi
  if [[ "$actual" != "$expected" ]]; then
    echo "workload mismatch: $row_id $report_key=${actual:-missing}, expected $expected" >&2
    ROW[status]=workload_mismatch
  fi
}
require_worker_policy_match() {
  local row_id=$1 report=$2 expected actual
  expected=$(manifest_row_value "$row_id" expected_worker_policy)
  [[ "$expected" == - ]] && return
  if ! actual=$(extract_unique_chart_top_value "$report" chart_worker_policy); then
    ROW[status]=workload_mismatch
    return
  fi
  if [[ "$expected" == policy ]]; then
    if [[ "$actual" != default_serial && "$actual" != automatic_default ]]; then
      echo "workload mismatch: $row_id chart_worker_policy=${actual:-missing}, expected closed default policy" >&2
      ROW[status]=workload_mismatch
    fi
  elif [[ "$actual" != "$expected" ]]; then
    echo "workload mismatch: $row_id chart_worker_policy=${actual:-missing}, expected $expected" >&2
    ROW[status]=workload_mismatch
  fi
}
require_worker_observation_match() {
  local row_id=$1 report=$2 expected_resolved expected_policy option requested
  local actual_policy actual_requested actual_report_resolved actual_local
  expected_resolved=$(manifest_row_value "$row_id" expected_resolved_workers)
  expected_policy=$(manifest_row_value "$row_id" expected_worker_policy)
  option=$(manifest_row_value "$row_id" worker_option)
  requested=$(manifest_row_value "$row_id" requested_workers)

  if ! actual_policy=$(extract_unique_chart_top_value "$report" chart_worker_policy) ||
     ! actual_requested=$(extract_unique_chart_top_value "$report" chart_workers_requested) ||
     ! actual_report_resolved=$(extract_unique_chart_top_value "$report" chart_workers_resolved) ||
     ! actual_local=$(extract_unique_chart_top_value "$report" local_score_workers); then
    ROW[status]=workload_mismatch
    return
  fi
  [[ "$actual_requested" =~ ^[0-9]+$ &&
     "$actual_report_resolved" =~ ^[1-9][0-9]*$ &&
     "$actual_local" =~ ^[1-9][0-9]*$ &&
     "$actual_report_resolved" == "${ROW[resolved_workers]}" &&
     "$actual_local" == "${ROW[resolved_workers]}" ]] || \
    ROW[status]=workload_mismatch
  case "$option/$requested" in
    none/default)
      if [[ "$actual_policy" == default_serial ]]; then
        [[ "$actual_requested" == 1 && "${ROW[resolved_workers]}" == 1 ]] || \
          ROW[status]=workload_mismatch
      elif [[ "$actual_policy" == automatic_default ]]; then
        [[ "$actual_requested" == 0 ]] || ROW[status]=workload_mismatch
      else
        ROW[status]=workload_mismatch
      fi ;;
    chart_spr_workers/0)
      [[ "$actual_requested" == 0 && "$actual_policy" == automatic ]] || \
        ROW[status]=workload_mismatch ;;
    chart_spr_local_score_workers/0)
      [[ "$actual_requested" == 0 && "$actual_policy" == legacy_automatic ]] || \
        ROW[status]=workload_mismatch ;;
    chart_spr_workers/*)
      [[ "$actual_requested" == "$requested" &&
         "${ROW[resolved_workers]}" == "$requested" &&
         "$actual_policy" == explicit ]] || ROW[status]=workload_mismatch ;;
    chart_spr_local_score_workers/*)
      [[ "$actual_requested" == "$requested" &&
         "${ROW[resolved_workers]}" == "$requested" &&
         "$actual_policy" == legacy_explicit ]] || ROW[status]=workload_mismatch ;;
    *) ROW[status]=workload_mismatch ;;
  esac

  if [[ "$expected_resolved" == - && "$expected_policy" == - ]]; then
    # A frozen timeout has no historical product observation.  If the same
    # workload completes on a later revision, validate its newly reached
    # worker fields from the immutable invocation contract rather than
    # comparing them with an unavailable sentinel.
    [[ "${ROW[resolved_workers]}" =~ ^[1-9][0-9]*$ ]] || \
      ROW[status]=workload_mismatch
    return
  fi

  if [[ "$expected_resolved" == policy ]]; then
    [[ "${ROW[resolved_workers]}" =~ ^[1-9][0-9]*$ ]] || ROW[status]=workload_mismatch
  else
    [[ "${ROW[resolved_workers]}" == "$expected_resolved" ]] || ROW[status]=workload_mismatch
  fi
  require_worker_policy_match "$row_id" "$report"
}

min_score() {
  awk -v a="${1:-NA}" -v b="${2:-NA}" '
    function n(x){return x~/^[0-9]+([.][0-9]+)?$/}
    BEGIN{if(n(a)&&n(b)) print(a+0<=b+0?a:b); else if(n(a))print a; else if(n(b))print b; else print "NA"}'
}
write_baseline_curve() {
  local stderr=$1 initial=$2 curve=$3 wall=$4 final=$5
  printf 'iteration\telapsed_s\treported_objective\texternal_validated_parsimony_min\n0\t0.000000\t%s\t%s\n' \
    "$initial" "$initial" >"$curve"
  if [[ "$final" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    printf '%s\t%.6f\t%s\t%s\n' "$iterations" "$wall" "$final" "$final" >>"$curve"
  else
    awk -v wall="$wall" '/iter [0-9]+: parsimony=/{i=$2;sub(":","",i);s=$3;sub("parsimony=","",s);row[++n]=i"\t"s;max=i}
      END{for(j=1;j<=n;j++){split(row[j],p,"\t");printf "%d\t%.6f\t%s\tNA\n",p[1],max?wall*p[1]/max:wall,p[2]}}' \
      "$stderr" >>"$curve"
  fi
}
write_chart_curve() {
  local report=$1 initial=$2 curve=$3 wall=$4 final=$5
  printf 'iteration\telapsed_s\treported_objective\texternal_validated_parsimony_min\n0\t0.000000\t%s\t%s\n' \
    "$initial" "$initial" >"$curve"
  awk -v wall="$wall" -v final="$final" '
    /^[[:space:]]*- iteration:/{iter=$3+1}
    /^[[:space:]]*state_score_after:/&&iter!=""{it[++n]=iter;s[n]=$2;if(iter>max)max=iter}
    END{for(i=1;i<=n;i++)printf "%d\t%.6f\t%s\t%s\n",it[i],max?wall*it[i]/max:wall,s[i],i==n?final:"NA"}' \
    "$report" >>"$curve"
}
run_baseline() {
  local fixture=$1 kind=$2 input=$3 refseq=$4 safe=$5 suffix=$6
  resolve_manifest_row sample_explore_merge native
  local pb="$out_dir/outputs/${safe}_sample_explore_merge_${suffix}.pb.gz"
  local out="$out_dir/logs/${safe}_sample_explore_merge_${suffix}.out"
  local err="$out_dir/logs/${safe}_sample_explore_merge_${suffix}.err"
  local args=(); [[ "$kind" == dag ]] && args=(--dag-pb "$input") || args=(--tree-pb "$input" --refseq "$refseq")
  local canonical_input_args=(--dag-pb "@primary:$CURRENT_INPUT_SHA256")
  [[ "$kind" == dag ]] || canonical_input_args=(--tree-pb "@primary:$CURRENT_INPUT_SHA256" --refseq "@refseq:$CURRENT_REFSEQ_SHA256")
  local canonical_command=("@binary:frozen_native" "${canonical_input_args[@]}" -o @output \
    -n "$iterations" --max-moves "$max_moves" --seed "$seed" --validate)
  local argv_sha; argv_sha=$(canonical_argv_digest "${canonical_command[@]}")
  run_capture "$fixture sample--explore--merge $suffix" "$out" "$err" \
    "$RESOLVED_RSS_LIMIT_BYTES" \
    "$larch2" "${args[@]}" -o "$pb" -n "$iterations" --max-moves "$max_moves" --seed "$seed" --validate
  reset_row; set_process_row
  ROW[row_id]=$RESOLVED_ROW_ID; ROW[fixture]=$fixture; ROW[method]=sample_explore_merge
  ROW[configured_chart_memory_budget]=NA
  ROW[manifest_rss_limit_bytes]=$RESOLVED_RSS_LIMIT_BYTES
  ROW[requested_workers]=native; ROW[resolved_workers]=native; ROW[worker_policy]=native
  ROW[iterations]=$iterations; ROW[seed]=$seed; ROW[acceptance]=sample_explore_merge
  ROW[canonical_argv_sha256]=$argv_sha
  ROW[search_semantic_sha256]=-
  ROW[candidate_selection]=native_best_moves; ROW[candidate_source]=sampled_tree
  ROW[objective]=parsimony_sampling; ROW[initial_validated_parsimony_min]=$INITIAL_SCORE
  local score=NA nodes=NA edges=NA validation=failed output_ready=0
  SCORE_OUTPUT_SHA=-; SCORE_OUTPUT_PARSIMONY=-
  if [[ "$RUN_OUTCOME" == descendant_leak || "$RUN_OUTCOME" == monitor_error ||
        "$RUN_OUTCOME" == wait_error ]]; then
    ROW[status]=runner_failure; validation=not_run
  elif [[ "$RUN_TIMED_OUT" == 1 || "$RUN_TIMED_OUT" == true ]]; then
    ROW[status]=timeout; validation=not_run
  elif [[ "$RUN_RSS_LIMIT_OBSERVED" == 1 ]]; then
    ROW[status]=rss_limit; validation=not_run
  elif [[ "$RUN_OUTCOME" != exited || "$RUN_RUNNER_STATUS" != 0 || "$RUN_STATUS" != 0 ]]; then
    ROW[status]=failed
  elif [[ -f "$pb" && ! -L "$pb" ]]; then
    validation=pending; ROW[status]=pending_validation; output_ready=1
  else ROW[status]=failed; fi
  if [[ -n "$workload_manifest" && "$RESOLVED_EXPECTED_INITIAL" != - &&
        "$INITIAL_SCORE" != "$RESOLVED_EXPECTED_INITIAL" ]]; then
    echo "workload mismatch: $RESOLVED_ROW_ID initial_score=$INITIAL_SCORE, expected $RESOLVED_EXPECTED_INITIAL" >&2
    ROW[status]=workload_mismatch; validation=not_run; output_ready=0
  fi
  ROW[validation_status]=$validation; ROW[final_validated_parsimony_min]=${score:-NA}
  ROW[best_reported_objective]=${score:-NA}; ROW[best_validated_parsimony_min]=$(min_score "$INITIAL_SCORE" "$score")
  ROW[final_dag_nodes]=${nodes:-NA}; ROW[final_dag_edges]=${edges:-NA}; ROW[report_path]=$err
  local curve="$out_dir/curves/${safe}_sample_explore_merge_${suffix}.tsv"
  write_baseline_curve "$err" "$INITIAL_SCORE" "$curve" "${ROW[wall_clock_s]}" "$score"
  ROW[curve_path]=$curve
  ROW[output_semantic_sha256]=-; ROW[trial_semantic_sha256]=-; ROW[canonical_digest]=-
  if [[ -n "$workload_manifest" && "$argv_sha" != "$RESOLVED_CANONICAL_ARGV_SHA" ]]; then
    echo "canonical argv mismatch: $RESOLVED_ROW_ID" >&2; ROW[status]=workload_mismatch
  fi
  if (( output_ready )); then
    queue_output_validation "$RECORD_TRIAL" "$RESOLVED_ROW_ID" "$TRIAL_INDEX" \
      sample_explore_merge "$pb" "$INITIAL_SCORE" "$err" "$curve" \
      "${ROW[wall_clock_s]}" "$out_dir/logs/${safe}_sample_explore_merge_score_${suffix}" \
      "$timeout_seconds" "$RESOLVED_RSS_LIMIT_BYTES"
  fi
  if (( RECORD_TRIAL )); then emit_row
  elif [[ ${ROW[status]} != pending_validation || $validation != pending ]]; then
    WARMUP_FAILURES=$((WARMUP_FAILURES+1))
  fi
}

mode_fields() {
  case "$1" in
    sampled_tree_fixed) METHOD=chart_spr_sampled_tree_fixed_topology; SOURCE=sampled_tree; ACCEPTANCE=fixed-topology; RAW_ACCEPTANCE=fixed_topology_exact; OBJECTIVE=fixed_topology_exact ;;
    grammar_exact) METHOD=chart_spr_grammar_exact; SOURCE=grammar; ACCEPTANCE=exact; RAW_ACCEPTANCE=exact_multisite; OBJECTIVE=grammar_exact ;;
    hybrid_exact) METHOD=chart_spr_hybrid_exact; SOURCE=hybrid; ACCEPTANCE=exact; RAW_ACCEPTANCE=exact_multisite; OBJECTIVE=grammar_exact ;;
    grammar_lower_bound) METHOD=chart_spr_grammar_lower_bound_heuristic; SOURCE=grammar; ACCEPTANCE=lower-bound; RAW_ACCEPTANCE=lower_bound_heuristic; OBJECTIVE=composite_lower_bound_heuristic ;;
    *) fail "unknown chart mode '$1'" ;;
  esac
}

run_chart() {
  local fixture=$1 kind=$2 input=$3 refseq=$4 safe=$5 suffix=$6 mode=$7 worker=$8
  mode_fields "$mode"; resolve_manifest_row "$METHOD" "$worker"
  local requested_worker_label=$worker
  [[ "$requested_worker_label" == 0 ]] && requested_worker_label=auto
  local wsafe=${requested_worker_label//[^A-Za-z0-9_.-]/_}
  local pb="$out_dir/outputs/${safe}_${METHOD}_${suffix}_w${wsafe}.pb.gz"
  local out="$out_dir/logs/${safe}_${METHOD}_${suffix}_w${wsafe}.out"
  local err="$out_dir/logs/${safe}_${METHOD}_${suffix}_w${wsafe}.err"
  local input_args=(); [[ "$kind" == dag ]] && input_args=(--dag-pb "$input") || input_args=(--tree-pb "$input" --refseq "$refseq")
  local worker_args=()
  if [[ "$worker" == default ]]; then
    worker_args=()
  elif (( workers_list_explicit || chart_workers_explicit )); then
    forwarded=$worker; [[ "$forwarded" == auto ]] && forwarded=0
    worker_args=(--chart-spr-workers "$forwarded")
  elif (( local_workers_explicit )); then
    forwarded=$worker; [[ "$forwarded" == auto ]] && forwarded=0
    worker_args=(--chart-spr-local-score-workers "$forwarded")
  fi
  local memory_args=(); [[ -z "$chart_memory_budget" ]] || memory_args=(--chart-spr-memory-budget "$chart_memory_budget")
  local commit_args=(); (( local_accept_updates == 0 )) || commit_args=(--chart-spr-local-accept-updates)
  local canonical_json="$out_dir/logs/${safe}_${METHOD}_${suffix}_w${wsafe}.canonical.json"
  local canonical_sidecar="$out_dir/logs/${safe}_${METHOD}_${suffix}_w${wsafe}.canonical.sidecar"
  local canonical_args=()
  if "$dagutil" --help 2>&1 | awk '/chart-spr-canonical-result[[:space:]]/{found=1} END{exit !found}'; then
    canonical_args=(--chart-spr-canonical-result "$canonical_json")
  elif [[ -n "$workload_manifest" ]]; then
    fail "manifest mode requires working --chart-spr-canonical-result support"
  fi
  local lazy_args=(); (( chart_lazy_policy_explicit )) && lazy_args=(--wric-lazy-chart "$chart_lazy_policy")
  [[ -z "$run_manifest_group" ]] || lazy_args=(--wric-lazy-chart "$chart_lazy_policy")
  local chart_command=()
  if [[ -n "$run_manifest_group" ]]; then
    chart_command=("$dagutil" "${input_args[@]}" "${MANIFEST_CHART_ARGS_NO_WORKER[@]}"
      "${MANIFEST_TIMED_WORKER_ARGS[@]}" -o "$pb")
  else
    chart_command=("$dagutil" "${input_args[@]}" --force-no-vcf --validate \
      --wric-polytomy-mode "$polytomy_mode" --wric-polytomy-max-shapes "$polytomy_shapes" \
      "${lazy_args[@]}" \
      --chart-spr-search --chart-spr-max-iterations "$iterations" \
      --chart-spr-max-candidates "$max_candidates" --chart-spr-top-k-exact "$top_k_exact" \
      --chart-spr-candidate-selection lower-bound-top-k --chart-spr-candidate-source "$SOURCE" \
      --chart-spr-acceptance "$ACCEPTANCE" "${worker_args[@]}" "${memory_args[@]}" \
      "${commit_args[@]}" --seed "$seed" -o "$pb")
  fi
  local canonical_command=("${chart_command[@]}")
  canonical_command[0]=@binary:working_chart
  local arg_index
  for ((arg_index=1; arg_index<${#canonical_command[@]}; ++arg_index)); do
    case "${canonical_command[$arg_index]}" in
      "$input") canonical_command[$arg_index]="@primary:$CURRENT_INPUT_SHA256" ;;
      "$refseq") canonical_command[$arg_index]="@refseq:$CURRENT_REFSEQ_SHA256" ;;
      "$canonical_json") canonical_command[$arg_index]=@search-canonical-result ;;
      "$pb") canonical_command[$arg_index]=@output ;;
    esac
  done
  local argv_sha; argv_sha=$(canonical_argv_digest "${canonical_command[@]}")
  run_capture "$fixture $METHOD workers=$worker $suffix" "$out" "$err" \
    "$RESOLVED_RSS_LIMIT_BYTES" \
    "${chart_command[@]}"
  reset_row; set_process_row
  ROW[row_id]=$RESOLVED_ROW_ID; ROW[fixture]=$fixture; ROW[method]=$METHOD
  ROW[manifest_rss_limit_bytes]=$RESOLVED_RSS_LIMIT_BYTES
  ROW[requested_workers]=$requested_worker_label; ROW[resolved_workers]=$(extract_value "$out" chart_workers_resolved)
  [[ -n ${ROW[resolved_workers]} ]] || ROW[resolved_workers]=$(extract_value "$out" local_score_workers)
  [[ -n ${ROW[resolved_workers]} ]] || ROW[resolved_workers]=NA
  ROW[worker_policy]=$(extract_value "$out" chart_worker_policy)
  [[ -n ${ROW[worker_policy]} ]] || ROW[worker_policy]=unobserved
  ROW[iterations]=$iterations; ROW[seed]=$seed
  ROW[acceptance]=$RAW_ACCEPTANCE; ROW[objective]=$OBJECTIVE
  ROW[candidate_selection]=lower_bound_top_k; ROW[candidate_source]=$SOURCE
  ROW[initial_validated_parsimony_min]=$INITIAL_SCORE
  ROW[canonical_argv_sha256]=$argv_sha
  local score=NA nodes=NA edges=NA validation=failed output_ready=0
  local search_status=$RUN_STATUS search_timed_out=$RUN_TIMED_OUT
  local refusal_sha; refusal_sha=$(sha256sum "$err" | awk '{print $1}')
  SCORE_OUTPUT_SHA=-; SCORE_OUTPUT_PARSIMONY=-
  if [[ "$RUN_OUTCOME" == descendant_leak || "$RUN_OUTCOME" == monitor_error ||
        "$RUN_OUTCOME" == wait_error ]]; then ROW[status]=runner_failure; validation=not_run
  elif [[ "$RUN_TIMED_OUT" == 1 || "$RUN_TIMED_OUT" == true ]]; then ROW[status]=timeout; validation=not_run
  elif [[ "$RUN_RSS_LIMIT_OBSERVED" == 1 ]]; then ROW[status]=rss_limit; validation=not_run
  elif [[ "$RUN_OUTCOME" != exited || "$RUN_RUNNER_STATUS" != "$RUN_STATUS" || "$RUN_STATUS" != 0 ]]; then
    if [[ -n "$workload_manifest" && "$RESOLVED_EXPECTED_STATUS" == expected_infeasible &&
          "$RESOLVED_EXPECTED_REASON_CODE" == high_arity_refinement_refusal &&
          "$RUN_OUTCOME" == exited && "$RUN_RUNNER_STATUS" == 1 &&
          "$RUN_STATUS" == 1 && "$RUN_TERM_SIGNAL" == 0 &&
          "$RUN_CORE_DUMPED" == 0 && "$RUN_TIMED_OUT" == 0 &&
          "$RUN_MONITOR_ERROR" == 0 && "$RUN_RSS_LIMIT_OBSERVED" == 0 &&
          "$RUN_RSS_LIMIT_EXCEEDED" == 0 &&
          "$refusal_sha" == "$RESOLVED_EXPECTED_REASON_SHA" &&
          ! -e "$pb" && ! -L "$pb" ]]; then
      ROW[status]=expected_infeasible; validation=not_applicable
    else
      ROW[status]=failed
    fi
  elif [[ -f "$pb" && ! -L "$pb" ]]; then
    validation=pending; ROW[status]=pending_validation; output_ready=1
  else ROW[status]=failed; fi
  # A killed timeout process did not finish a product report.  In particular,
  # the requested worker token is not an observed resolved worker count.  Keep
  # the request as forensic command evidence, but label both product-derived
  # values explicitly unavailable rather than falling back to the request.
  if [[ ${ROW[status]} == timeout ]]; then
    ROW[resolved_workers]=NA
    ROW[worker_policy]=timeout_unobserved
  fi
  if [[ -n "$workload_manifest" && "$RESOLVED_EXPECTED_INITIAL" != - &&
        "$INITIAL_SCORE" != "$RESOLVED_EXPECTED_INITIAL" ]]; then
    echo "workload mismatch: $RESOLVED_ROW_ID initial_score=$INITIAL_SCORE, expected $RESOLVED_EXPECTED_INITIAL" >&2
    ROW[status]=workload_mismatch; validation=not_run; output_ready=0
  fi
  ROW[validation_status]=$validation; ROW[final_validated_parsimony_min]=${score:-NA}
  ROW[best_validated_parsimony_min]=$(min_score "$INITIAL_SCORE" "$score")
  ROW[final_dag_nodes]=${nodes:-NA}; ROW[final_dag_edges]=${edges:-NA}; ROW[report_path]=$out
  local curve="$out_dir/curves/${safe}_${METHOD}_${suffix}_w${wsafe}.tsv"
  write_chart_curve "$out" "$INITIAL_SCORE" "$curve" "${ROW[wall_clock_s]}" "$score"
  ROW[curve_path]=$curve
  if [[ ${ROW[status]} == pending_validation ]]; then
    ROW[resolved_workers]=$(extract_unique_chart_top_value "$out" chart_workers_resolved)
    ROW[worker_policy]=$(extract_unique_chart_top_value "$out" chart_worker_policy)
  fi
  local key col
  for key in candidates_generated candidates_scored exact_verifications accepted_moves candidate_accepts_attempted \
    post_materialization_rejections cache_build_ms local_scoring_ms exact_verification_ms \
    accepted_rebuild_ms final_compaction_ms final_compaction_exactness_kind \
    post_materialization_check_ms total_ms local_candidates_per_second active_patterns \
    chart_cache_resident_bytes; do ROW[$key]=$(extract_chart_top_value "$out" "$key"); ROW[$key]=${ROW[$key]:-NA}; done
  ROW[best_reported_objective]=$(extract_chart_top_value "$out" final_score)
  ROW[best_reported_objective]=${ROW[best_reported_objective]:-NA}
  [[ ${ROW[candidate_accepts_attempted]} =~ ^[1-9][0-9]*$ ]] && \
    ROW[committed_attempt_ratio]=$(awk -v a="${ROW[accepted_moves]:-0}" -v b="${ROW[candidate_accepts_attempted]}" 'BEGIN{printf "%.6f",a/b}')
  [[ ${ROW[candidates_scored]} =~ ^[1-9][0-9]*$ && ${ROW[local_scoring_ms]} =~ ^[0-9]+([.][0-9]+)?$ ]] && \
    ROW[local_ms_per_candidate]=$(awk -v a="${ROW[local_scoring_ms]}" -v b="${ROW[candidates_scored]}" 'BEGIN{printf "%.6f",a/b}')
  for key in mean p50 p95 max; do
    col=affected_$key
    if [[ ${ROW[status]} == pending_validation ]]; then
      ROW[$col]=$(extract_unique_chart_section_value "$out" affected_clade_count_distribution "$key")
    else
      ROW[$col]=$(extract_value "$out" "$key")
    fi
    ROW[$col]=${ROW[$col]:-NA}
  done
  for key in upward_path_iterator_steps path_pairs_considered candidates_pruned_before_construction \
    candidates_pruned_after_construction candidates_generated_after_dedup candidate_cap_cutoffs \
    path_budget_cutoffs overlay_materializations_for_oracle full_overlay_materializations \
    reachable_clades_traversed reachable_productions_traversed reachability_full_grammar_like_passes; do
    ROW[$key]=$(extract_counter_value "$out" "$key"); ROW[$key]=${ROW[$key]:-NA}
  done
  for key in full_search_state_rebuilds final_compaction_rebuilds initial_search_state_rebuilds \
    sidecar_rebuilds_after_accept overlay_materializations_for_exact_verification \
    overlay_materializations_for_accept_materialization overlay_materializations_for_final_compaction; do
    ROW[$key]=$(extract_chart_top_value "$out" "$key"); ROW[$key]=${ROW[$key]:-NA}
  done
  ROW[grammar_clades]=$(extract_chart_top_value "$out" final_grammar_clades); ROW[grammar_clades]=${ROW[grammar_clades]:-NA}
  ROW[grammar_productions]=$(extract_chart_top_value "$out" final_grammar_productions); ROW[grammar_productions]=${ROW[grammar_productions]:-NA}
  for key in candidate_generation_ms exact_initialization_ms \
    initial_chart_construction_ms materialization_ms \
    materialization_exact_verification_ms materialization_accepted_update_ms \
    materialization_final_compaction_ms peak_concurrent_exact_verifiers \
    chart_axis_exact_candidate_active_worker_high_water \
    exact_candidate_admission_batches exact_candidate_parallel_batches \
    exact_candidate_inner_parallel_batches exact_candidate_memory_limited_batches \
    exact_candidate_peak_admitted_bytes exact_candidate_peak_projected_resident_bytes \
    exact_candidate_queued_for_memory_ms \
    exact_candidate_timing_count \
    exact_candidate_verification_ms_min exact_candidate_verification_ms_mean \
    exact_candidate_verification_ms_max; do ROW[$key]=$(extract_chart_top_value "$out" "$key"); ROW[$key]=${ROW[$key]:-NA}; done
  ROW[exact_ms_per_candidate]=${ROW[exact_candidate_verification_ms_mean]}
  if [[ ${ROW[status]} == pending_validation ]]; then
    instrumentation_ok=1
    for key in cache_build_ms total_ms initial_chart_construction_ms materialization_ms \
      materialization_exact_verification_ms materialization_accepted_update_ms \
      materialization_final_compaction_ms; do
      [[ ${ROW[$key]} =~ ^[0-9]+([.][0-9]+)?$ ]] || instrumentation_ok=0
    done
    [[ ${ROW[peak_concurrent_exact_verifiers]} =~ ^[0-9]+$ ]] || \
      instrumentation_ok=0
    [[ ${ROW[chart_axis_exact_candidate_active_worker_high_water]} =~ ^[0-9]+$ ]] || \
      instrumentation_ok=0
    for key in exact_candidate_admission_batches exact_candidate_parallel_batches \
      exact_candidate_inner_parallel_batches exact_candidate_memory_limited_batches \
      exact_candidate_peak_admitted_bytes exact_candidate_peak_projected_resident_bytes; do
      [[ ${ROW[$key]} =~ ^[0-9]+$ ]] || instrumentation_ok=0
    done
    [[ ${ROW[exact_candidate_queued_for_memory_ms]} =~ ^[0-9]+([.][0-9]+)?$ ]] || \
      instrumentation_ok=0
    if (( instrumentation_ok )); then
      awk -v initial="${ROW[initial_chart_construction_ms]}" \
          -v cache="${ROW[cache_build_ms]}" \
          -v total="${ROW[materialization_ms]}" \
          -v exact="${ROW[materialization_exact_verification_ms]}" \
          -v accepted="${ROW[materialization_accepted_update_ms]}" \
          -v final="${ROW[materialization_final_compaction_ms]}" \
          -v run_total="${ROW[total_ms]}" \
          'BEGIN {
             d = total - (exact + accepted + final); if (d < 0) d = -d
             exit !(initial <= cache + 0.0021 && total <= run_total + 0.0021 &&
                    d <= 0.0021)
           }' || instrumentation_ok=0
      if [[ ${ROW[exact_verifications]} =~ ^[0-9]+$ ]]; then
        awk -v exact="${ROW[exact_verifications]}" \
            -v peak="${ROW[peak_concurrent_exact_verifiers]}" \
            'BEGIN { exit !(exact == 0 ? peak == 0 : (peak >= 1 && peak <= exact)) }' || \
          instrumentation_ok=0
      else
        instrumentation_ok=0
      fi
      awk -v batches="${ROW[exact_candidate_admission_batches]}" \
          -v parallel="${ROW[exact_candidate_parallel_batches]}" \
          -v inner="${ROW[exact_candidate_inner_parallel_batches]}" \
          -v limited="${ROW[exact_candidate_memory_limited_batches]}" \
          -v admitted="${ROW[exact_candidate_peak_admitted_bytes]}" \
          -v projected="${ROW[exact_candidate_peak_projected_resident_bytes]}" \
          -v queued="${ROW[exact_candidate_queued_for_memory_ms]}" \
          -v exact="${ROW[exact_verifications]}" \
          'BEGIN {
             exit !(parallel <= batches && inner <= batches && limited <= batches &&
                    parallel + inner <= batches && admitted <= projected &&
                    (batches != 0 || (parallel == 0 && inner == 0 && limited == 0 &&
                                      admitted == 0 && projected == 0 && queued == 0)) &&
                    (batches == 0 || (admitted > 0 && projected > 0)) &&
                    (exact == 0 || batches > 0) &&
                    (limited != 0 || queued == 0))
           }' || instrumentation_ok=0
      if [[ ${ROW[configured_chart_memory_budget]} =~ ^[0-9]+$ &&
            ${ROW[configured_chart_memory_budget]} != 0 ]]; then
        if ! awk -v projected="${ROW[exact_candidate_peak_projected_resident_bytes]}" \
            -v budget="${ROW[configured_chart_memory_budget]}" \
            'BEGIN { exit !(projected <= budget) }'; then
          echo "chart admission projection exceeds configured budget: $RESOLVED_ROW_ID" \
            "projected=${ROW[exact_candidate_peak_projected_resident_bytes]}" \
            "budget=${ROW[configured_chart_memory_budget]}" >&2
          instrumentation_ok=0
        fi
      fi
    fi
    if (( ! instrumentation_ok )); then
      echo "invalid Phase-0 chart instrumentation: $RESOLVED_ROW_ID" >&2
      ROW[status]=workload_mismatch
    fi
  fi
  if [[ ${ROW[status]} == pending_validation && "$RESOLVED_EXPECTED_CANDIDATES" != - && \
        "${ROW[candidates_scored]}" != "$RESOLVED_EXPECTED_CANDIDATES" ]]; then
    echo "workload mismatch: $RESOLVED_ROW_ID candidates_scored=${ROW[candidates_scored]}, expected $RESOLVED_EXPECTED_CANDIDATES" >&2
    ROW[status]=workload_mismatch
  fi
  if [[ ${ROW[status]} == pending_validation && "$RESOLVED_EXPECTED_EXACT" != - && \
        "${ROW[exact_verifications]}" != "$RESOLVED_EXPECTED_EXACT" ]]; then
    echo "workload mismatch: $RESOLVED_ROW_ID exact_verifications=${ROW[exact_verifications]}, expected $RESOLVED_EXPECTED_EXACT" >&2
    ROW[status]=workload_mismatch
  fi
  if [[ -n "$workload_manifest" && ${ROW[status]} == pending_validation ]]; then
    if [[ "$RESOLVED_EXPECTED_STATUS" == expected_infeasible ]]; then
      # A refusal row has no success oracle or reached report contract.  It is
      # an exact expected failure, not a historical-timeout compatibility row.
      ROW[status]=workload_mismatch
      output_ready=0
    fi
  fi
  if [[ -n "$workload_manifest" && ${ROW[status]} == pending_validation ]]; then
    require_worker_observation_match "$RESOLVED_ROW_ID" "$out"
    [[ "$RESOLVED_EXPECTED_INITIAL" == - || "$INITIAL_SCORE" == "$RESOLVED_EXPECTED_INITIAL" ]] || ROW[status]=workload_mismatch
    [[ "$RESOLVED_EXPECTED_FINAL" == - || "$(extract_unique_chart_top_value "$out" final_score)" == "$RESOLVED_EXPECTED_FINAL" ]] || ROW[status]=workload_mismatch
    require_report_match "$RESOLVED_ROW_ID" acceptance acceptance "$out"
    require_report_match "$RESOLVED_ROW_ID" objective objective "$out"
    require_report_match "$RESOLVED_ROW_ID" candidate_selection candidate_selection "$out"
    require_report_match "$RESOLVED_ROW_ID" candidate_source candidate_source "$out"
    require_report_match "$RESOLVED_ROW_ID" candidate_cap_semantics candidate_cap_semantics "$out"
    require_report_match "$RESOLVED_ROW_ID" topology_selector topology_selector "$out"
    require_report_match "$RESOLVED_ROW_ID" iterations requested_max_iterations "$out"
    require_report_match "$RESOLVED_ROW_ID" seed seed "$out"
    require_report_match "$RESOLVED_ROW_ID" chart_max_candidates configured_max_candidates "$out"
    require_report_match "$RESOLVED_ROW_ID" chart_top_k_exact top_k_exact_verify "$out"
    require_report_match "$RESOLVED_ROW_ID" randomize_order randomize_order "$out"
    require_report_match "$RESOLVED_ROW_ID" reservoir_sample reservoir_sample "$out"
    require_report_match "$RESOLVED_ROW_ID" include_immediate_reversals include_immediate_reversals "$out"
    require_report_match "$RESOLVED_ROW_ID" sampled_tree_count sampled_tree_count "$out"
    require_report_match "$RESOLVED_ROW_ID" sampled_tree_radius sampled_tree_radius "$out"
    require_report_match "$RESOLVED_ROW_ID" sampled_tree_score_threshold sampled_tree_score_threshold "$out"
    require_report_match "$RESOLVED_ROW_ID" max_upward_path_expansions max_upward_path_expansions "$out"
    require_report_match "$RESOLVED_ROW_ID" max_path_pairs max_path_pairs "$out"
    require_report_match "$RESOLVED_ROW_ID" min_moved_clade_size min_moved_clade_size "$out"
    require_report_match "$RESOLVED_ROW_ID" max_moved_clade_size max_moved_clade_size "$out"
    require_report_match "$RESOLVED_ROW_ID" min_target_clade_size min_target_clade_size "$out"
    require_report_match "$RESOLVED_ROW_ID" max_target_clade_size max_target_clade_size "$out"
    require_report_match "$RESOLVED_ROW_ID" max_affected_clades max_affected_clades "$out"
    require_report_match "$RESOLVED_ROW_ID" polytomy_mode polytomy_mode "$out"
    require_report_match "$RESOLVED_ROW_ID" polytomy_max_exact_arity polytomy_max_exact_arity "$out"
    require_report_match "$RESOLVED_ROW_ID" polytomy_max_shapes polytomy_max_shapes "$out"
    require_report_match "$RESOLVED_ROW_ID" polytomy_max_productions polytomy_max_productions "$out"
    require_report_match "$RESOLVED_ROW_ID" polytomy_max_clades polytomy_max_clades "$out"
    require_report_match "$RESOLVED_ROW_ID" lazy_policy lazy_policy "$out"
    require_report_match "$RESOLVED_ROW_ID" max_cached_patterns max_cached_patterns "$out"
    require_report_match "$RESOLVED_ROW_ID" pattern_batch_size configured_pattern_batch_size "$out"
    require_report_match "$RESOLVED_ROW_ID" candidate_batch_size configured_candidate_batch_size "$out"
    require_report_match "$RESOLVED_ROW_ID" memory_budget_bytes memory_budget_bytes "$out"
    require_report_match "$RESOLVED_ROW_ID" commit_mode commit_mode "$out"
    require_report_match "$RESOLVED_ROW_ID" verification_mode verification_mode "$out"
    require_report_match "$RESOLVED_ROW_ID" local_accept_updates local_accept_updates "$out"
    require_report_match "$RESOLVED_ROW_ID" dominance_mode dominance_mode "$out"
    require_report_match "$RESOLVED_ROW_ID" bound_pruning bound_pruning "$out"
    require_report_match "$RESOLVED_ROW_ID" require_exact_keep_mask require_exact_keep_mask "$out"
    require_report_match "$RESOLVED_ROW_ID" max_frontier_entries max_frontier_entries "$out"
    require_report_match "$RESOLVED_ROW_ID" score_ua_edge score_ua_edge "$out"
    require_report_match "$RESOLVED_ROW_ID" validate validate "$out"
    require_report_match "$RESOLVED_ROW_ID" force_no_vcf force_no_vcf "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_refinement_exactness refinement_exactness "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_cache_strategy cache_strategy "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_effective_pattern_batch_size effective_pattern_batch_size "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_final_compaction_exactness final_compaction_exactness_kind "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_chain_exactness chain_per_accept_exactness_label "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_active_patterns active_patterns "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_initial_clades initial_grammar_clades "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_initial_productions initial_grammar_productions "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_candidates_generated candidates_generated "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_stop_reason stop_reason "$out" iteration_stop_reason
    require_report_match "$RESOLVED_ROW_ID" expected_iterations iterations "$out"
    require_report_match "$RESOLVED_ROW_ID" expected_accepted_moves accepted_moves "$out"
  fi
  ROW[search_semantic_sha256]=-; ROW[output_semantic_sha256]=-
  ROW[trial_semantic_sha256]=-; ROW[canonical_digest]=-
  if [[ -n "$workload_manifest" && "$argv_sha" != "$RESOLVED_CANONICAL_ARGV_SHA" ]]; then
    echo "canonical argv mismatch: $RESOLVED_ROW_ID" >&2; ROW[status]=workload_mismatch
  fi
  if (( output_ready )); then
    queue_output_validation "$RECORD_TRIAL" "$RESOLVED_ROW_ID" "$TRIAL_INDEX" \
      "$METHOD" "$pb" "$INITIAL_SCORE" "$out" "$curve" "${ROW[wall_clock_s]}" \
      "$out_dir/logs/${safe}_${METHOD}_score_${suffix}_w${wsafe}" "$timeout_seconds" \
      "$RESOLVED_RSS_LIMIT_BYTES"
    local companion_key=$RESOLVED_ROW_ID
    if [[ -z ${CANONICAL_QUEUE_COMMAND[$companion_key]:-} ]]; then
      local companion_safe=${RESOLVED_ROW_ID//[^A-Za-z0-9_.-]/_}
      local companion_pb="$out_dir/outputs/${safe}_${companion_safe}_canonical_companion.pb.gz"
      local companion_json="$out_dir/logs/${safe}_${companion_safe}_canonical_companion.json"
      local companion_out="$out_dir/logs/${safe}_${companion_safe}_canonical_companion.out"
      local companion_err="$out_dir/logs/${safe}_${companion_safe}_canonical_companion.err"
      local companion_command=() companion_arg
      for companion_arg in "${chart_command[@]}"; do
        if [[ "$companion_arg" == -o ]]; then
          companion_command+=(--chart-spr-canonical-result "$companion_json" -o)
        elif [[ "$companion_arg" == "$pb" ]]; then
          companion_command+=("$companion_pb")
        else
          companion_command+=("$companion_arg")
        fi
      done
      CANONICAL_QUEUE_COMMAND[$companion_key]=$(declare -p companion_command)
      CANONICAL_QUEUE_PB[$companion_key]=$companion_pb
      CANONICAL_QUEUE_JSON[$companion_key]=$companion_json
      CANONICAL_QUEUE_OUT[$companion_key]=$companion_out
      CANONICAL_QUEUE_ERR[$companion_key]=$companion_err
      CANONICAL_QUEUE_ROW[$companion_key]="$fixture|$METHOD|$worker"
      CANONICAL_QUEUE_TIMEOUT[$companion_key]=$timeout_seconds
      CANONICAL_QUEUE_RSS_LIMIT[$companion_key]=$RESOLVED_RSS_LIMIT_BYTES
      canonical_queue_order+=("$companion_key")
      # A row that timed out in the frozen Phase-0 oracle has no historical
      # semantic digest.  If a later revision completes it, automatically run
      # the identical workload as an explicit one-worker full oracle after all
      # timed children.  This makes the required same-revision 1-vs-N check
      # non-vacuous even when the caller selected only the formerly-timed-out
      # parallel row.
      if (( emit_full_canonical )) || \
         [[ -n "$workload_manifest" && "$RESOLVED_EXPECTED_STATUS" == timeout ]]; then
        "$dagutil" --help 2>&1 | awk '/chart-spr-canonical-sidecar/{found=1} END{exit !found}' || \
          fail "full canonical correctness requires --chart-spr-canonical-sidecar"
        local full_json="$out_dir/logs/${safe}_${companion_safe}_full_canonical.json"
        local full_sidecar="$out_dir/logs/${safe}_${companion_safe}_full_canonical.ndjson"
        local full_pb="$out_dir/outputs/${safe}_${companion_safe}_full_canonical.pb.gz"
        local full_command=()
        if [[ -n "$run_manifest_group" ]]; then
          full_command=("$dagutil" "${input_args[@]}" "${MANIFEST_CHART_ARGS_NO_WORKER[@]}"
            --chart-spr-workers 1 --chart-spr-canonical-result "$full_json"
            --chart-spr-canonical-sidecar "$full_sidecar" -o "$full_pb")
        else
          full_command=("$dagutil" "${input_args[@]}" --force-no-vcf --validate
            --wric-polytomy-mode "$polytomy_mode" --wric-polytomy-max-shapes "$polytomy_shapes"
            "${lazy_args[@]}" --chart-spr-search --chart-spr-max-iterations "$iterations"
            --chart-spr-max-candidates "$max_candidates" --chart-spr-top-k-exact "$top_k_exact"
            --chart-spr-candidate-selection lower-bound-top-k --chart-spr-candidate-source "$SOURCE"
            --chart-spr-acceptance "$ACCEPTANCE" --chart-spr-workers 1 "${memory_args[@]}"
            "${commit_args[@]}" --chart-spr-canonical-result "$full_json"
            --chart-spr-canonical-sidecar "$full_sidecar" --seed "$seed" -o "$full_pb")
        fi
        CANONICAL_QUEUE_FULL_COMMAND[$companion_key]=$(declare -p full_command)
        CANONICAL_QUEUE_FULL_JSON[$companion_key]=$full_json
        CANONICAL_QUEUE_FULL_SIDECAR[$companion_key]=$full_sidecar
      fi
    fi
  fi
  if (( RECORD_TRIAL )); then emit_row
  elif [[ ${ROW[status]} != pending_validation && ${ROW[status]} != expected_infeasible ]] || \
       [[ $validation != pending && $validation != not_applicable ]]; then
    WARMUP_FAILURES=$((WARMUP_FAILURES+1))
  fi
}

score_current_initial() {
  local safe=$1 input=$2 refseq=$3 kind=$4 rss_limit=$5
  local cache_key="${CURRENT_INPUT_SHA256}/${CURRENT_REFSEQ_SHA256}/${timeout_seconds}/${rss_limit}"
  if [[ -n ${INITIAL_SCORE_CACHE[$cache_key]:-} ]]; then
    INITIAL_SCORE=${INITIAL_SCORE_CACHE[$cache_key]}; return
  fi
  local initial_out="$out_dir/logs/${safe}_initial_score.out"
  local initial_err="$out_dir/logs/${safe}_initial_score.err"
  local initial_canonical="$out_dir/logs/${safe}_initial_score.canonical-dag.json"
  local initial_args=(); [[ "$kind" == dag ]] && initial_args=(--dag-pb "$input") || \
    initial_args=(--tree-pb "$input" --refseq "$refseq")
  local scorer=$dagutil canonical_args=()
  if [[ -n "$workload_manifest" ]]; then
    scorer=$frozen_oracle_path
    canonical_args=(--canonical-dag-result "$initial_canonical")
  fi
  run_capture "$CURRENT_FIXTURE initial score" "$initial_out" "$initial_err" \
    "$rss_limit" \
    "$scorer" "${initial_args[@]}" --force-no-vcf --validate --dag-info \
    "${canonical_args[@]}"
  (( RUN_RUNNER_STATUS == 0 )) || fail "initial scoring failed for $CURRENT_FIXTURE"
  INITIAL_SCORE=$(extract_parsimony_min "$initial_out")
  [[ -n "$INITIAL_SCORE" ]] || fail "initial score missing for $CURRENT_FIXTURE"
  if [[ -n "$workload_manifest" ]]; then
    [[ $(extract_json_string "$initial_canonical" schema) == larch.dag.semantic_digest &&
       $(extract_json_number "$initial_canonical" parsimony_min) == "$INITIAL_SCORE" ]] || \
      fail "frozen initial semantic oracle result is invalid: $CURRENT_FIXTURE"
  fi
  INITIAL_SCORE_CACHE[$cache_key]=$INITIAL_SCORE
}

WARMUP_FAILURES=0
declare -A INITIAL_SCORE_CACHE=()
if [[ -n "$run_manifest_group" ]]; then
  selected_manifest_rows=()
  for manifest in "${manifest_files[@]}"; do
    while IFS= read -r row_id; do
      [[ -n "$row_id" ]] || continue
      method=$(manifest_row_value "$row_id" method)
      if [[ "$method" != sample_explore_merge ]] && (( worker_option_count > 0 )); then
        requested=$(manifest_row_value "$row_id" requested_workers)
        selected=0
        for worker in "${chart_worker_specs[@]}"; do
          normalized=$worker; [[ "$normalized" == auto ]] && normalized=0
          [[ "$normalized" == "$requested" ]] && selected=1
        done
        (( selected )) || continue
      fi
      selected_manifest_rows+=("$row_id")
    done < <(awk -F '\t' -v group="$run_manifest_group" '
      /^#/{next}!h{for(i=1;i<=NF;i++)x[$i]=i;h=1;next}
      $x["run_group"]==group{print $x["row_id"]}' "$manifest")
  done
  (( ${#selected_manifest_rows[@]} > 0 )) || \
    fail "manifest group/worker selection has no rows: $run_manifest_group"
  declare -A selected_row_set=()
  for row_id in "${selected_manifest_rows[@]}"; do selected_row_set[$row_id]=1; done
  for row_id in "${selected_manifest_rows[@]}"; do
    [[ $(manifest_row_value "$row_id" expected_outcome) == scale_limit ]] || continue
    fixture_id=$(manifest_row_value "$row_id" fixture_id)
    scale_key=$(scale_companion_contract_key "$row_id")
    scale_candidates=$(manifest_row_value "$row_id" scale_largest_candidates)
    companion=0
    for candidate_id in "${selected_manifest_rows[@]}"; do
      [[ $(manifest_row_value "$candidate_id" fixture_id) == "$fixture_id" &&
         $(manifest_row_value "$candidate_id" method) == chart_spr_grammar_lower_bound_heuristic &&
         $(manifest_row_value "$candidate_id" expected_outcome) == ok &&
         $(manifest_row_value "$candidate_id" chart_max_candidates) == "$scale_candidates" &&
         $(manifest_row_value "$candidate_id" chart_top_k_exact) == 0 &&
         $(scale_companion_contract_key "$candidate_id") == "$scale_key" ]] && companion=1
    done
    (( companion )) || fail "scale_limit row lacks selected full lower-bound companion: $row_id"
  done
  if (( ${#worker_policy_requirements[@]} > 0 )); then
    default_policy_rows=(); explicit_auto_rows=()
    for row_id in "${selected_manifest_rows[@]}"; do
      [[ $(manifest_row_value "$row_id" method) != sample_explore_merge ]] || continue
      option=$(manifest_row_value "$row_id" worker_option)
      requested=$(manifest_row_value "$row_id" requested_workers)
      if [[ "$option" == none && "$requested" == default ]]; then
        default_policy_rows+=("$row_id")
      elif [[ "$option" == chart_spr_workers && "$requested" == 0 ]]; then
        explicit_auto_rows+=("$row_id")
      fi
    done
    (( ${#default_policy_rows[@]} > 0 && ${#explicit_auto_rows[@]} > 0 )) || \
      fail "worker-policy gate requires selected omitted/default and explicit-auto chart rows"
    for row_id in "${default_policy_rows[@]}"; do
      policy_key=$(default_auto_contract_key "$row_id"); companion=0
      for candidate_id in "${explicit_auto_rows[@]}"; do
        [[ $(default_auto_contract_key "$candidate_id") == "$policy_key" ]] && \
          companion=$((companion+1))
      done
      (( companion == 1 )) || \
        fail "default worker-policy row requires exactly one selected explicit-auto companion: $row_id (found $companion)"
    done
    for row_id in "${explicit_auto_rows[@]}"; do
      policy_key=$(default_auto_contract_key "$row_id"); companion=0
      for candidate_id in "${default_policy_rows[@]}"; do
        [[ $(default_auto_contract_key "$candidate_id") == "$policy_key" ]] && \
          companion=$((companion+1))
      done
      (( companion == 1 )) || \
        fail "explicit-auto worker-policy row requires exactly one selected default companion: $row_id (found $companion)"
    done
  fi
  for row_id in "${expected_timeout_ids[@]}"; do
    [[ -n ${selected_row_set[$row_id]:-} ]] || \
      fail "expected-timeout waiver row is not selected by the manifest group: $row_id"
    [[ $(manifest_row_value "$row_id" expected_timeout_trials) == "$repetitions" ]] || \
      fail "expected-timeout count must equal repetitions for $row_id"
    (( $(manifest_row_value "$row_id" timeout_seconds) > 0 )) || \
      fail "expected-timeout row has zero timeout: $row_id"
  done
  native_rows=(); chart_rows=()
  for row_id in "${selected_manifest_rows[@]}"; do
    if [[ $(manifest_row_value "$row_id" method) == sample_explore_merge ]]; then
      native_rows+=("$row_id")
    else
      chart_rows+=("$row_id")
    fi
  done
  # Prepare every distinct starting score before the first timed child so a
  # new fixture cannot inject oracle work between paired blocks.
  for row_id in "${selected_manifest_rows[@]}"; do
    configure_manifest_row "$row_id"
    safe=${CURRENT_FIXTURE//[^A-Za-z0-9_.-]/_}
    row_safe=${row_id//[^A-Za-z0-9_.-]/_}
    row_rss_limit=$(manifest_row_value "$row_id" rss_limit_bytes)
    score_current_initial "${safe}_${row_safe}" "$CURRENT_INPUT" "$CURRENT_REFSEQ" \
      "$CURRENT_KIND" "$row_rss_limit"
  done
  total=$((warmups+repetitions))
  for ((sequence=1; sequence<=total; ++sequence)); do
    if (( sequence<=warmups )); then RECORD_TRIAL=0; TRIAL_INDEX=$sequence; trial_suffix=warmup$TRIAL_INDEX
    else RECORD_TRIAL=1; TRIAL_INDEX=$((sequence-warmups)); trial_suffix=trial$TRIAL_INDEX; fi
    ordered_rows=(); sequence_chart_rows=("${chart_rows[@]}")
    if (( ${#worker_policy_requirements[@]} > 0 && sequence%2==0 )); then
      sequence_chart_rows=()
      for ((row_index=${#chart_rows[@]}-1; row_index>=0; --row_index)); do
        sequence_chart_rows+=("${chart_rows[$row_index]}")
      done
    fi
    if (( sequence%2==1 )); then
      EXECUTION_ORDER=baseline-first; ordered_rows=("${native_rows[@]}" "${sequence_chart_rows[@]}")
    else
      EXECUTION_ORDER=chart-first; ordered_rows=("${sequence_chart_rows[@]}" "${native_rows[@]}")
    fi
    for row_id in "${ordered_rows[@]}"; do
      configure_manifest_row "$row_id"
      safe=${CURRENT_FIXTURE//[^A-Za-z0-9_.-]/_}
      row_safe=${row_id//[^A-Za-z0-9_.-]/_}
      row_rss_limit=$(manifest_row_value "$row_id" rss_limit_bytes)
      score_current_initial "${safe}_${row_safe}" "$CURRENT_INPUT" "$CURRENT_REFSEQ" \
        "$CURRENT_KIND" "$row_rss_limit"
      suffix="${trial_suffix}_${row_safe}"
      if [[ $(manifest_row_value "$row_id" method) == sample_explore_merge ]]; then
        run_baseline "$CURRENT_FIXTURE" "$CURRENT_KIND" "$CURRENT_INPUT" "$CURRENT_REFSEQ" "$safe" "$suffix"
      else
        run_chart "$CURRENT_FIXTURE" "$CURRENT_KIND" "$CURRENT_INPUT" "$CURRENT_REFSEQ" \
          "$safe" "$suffix" "$MANIFEST_MODE" "$MANIFEST_WORKER"
      fi
    done
  done
  FORCED_MANIFEST_ROW_ID=
else
for spec in "${fixture_specs[@]}"; do
  IFS='|' read -r CURRENT_FIXTURE kind input refseq <<<"$spec"
  CURRENT_KIND=$kind
  safe=${CURRENT_FIXTURE//[^A-Za-z0-9_.-]/_}
  [[ -f "$input" ]] || fail "fixture not found: $input"
  CURRENT_INPUT_SHA256=$(sha256sum "$input" | awk '{print $1}')
  CURRENT_REFSEQ_SHA256=NA
  [[ -z "$refseq" ]] || { [[ -f "$refseq" ]] || fail "refseq not found: $refseq"; CURRENT_REFSEQ_SHA256=$(sha256sum "$refseq" | awk '{print $1}'); }
  if [[ -z "$workload_manifest" ]]; then
    score_current_initial "$safe" "$input" "$refseq" "$kind" \
      "$capture_rss_limit_bytes"
  else
    # Resolve and cap every distinct initial-score invocation before timing.
    # The cache key includes timeout and RSS, so rows with an identical
    # resource contract share work while rows with different caps cannot.
    resolve_manifest_row sample_explore_merge native
    row_safe=${RESOLVED_ROW_ID//[^A-Za-z0-9_.-]/_}
    score_current_initial "${safe}_${row_safe}" "$input" "$refseq" "$kind" \
      "$RESOLVED_RSS_LIMIT_BYTES"
    if (( native_only == 0 )); then
      for mode in $mode_list; do
        mode_fields "$mode"
        for worker in "${chart_worker_specs[@]}"; do
          resolve_manifest_row "$METHOD" "$worker"
          row_safe=${RESOLVED_ROW_ID//[^A-Za-z0-9_.-]/_}
          score_current_initial "${safe}_${row_safe}" "$input" "$refseq" "$kind" \
            "$RESOLVED_RSS_LIMIT_BYTES"
        done
      done
    fi
  fi
  total=$((warmups+repetitions))
  for ((sequence=1;sequence<=total;++sequence)); do
    if (( sequence<=warmups )); then RECORD_TRIAL=0; TRIAL_INDEX=$sequence; suffix=warmup$TRIAL_INDEX
    else RECORD_TRIAL=1; TRIAL_INDEX=$((sequence-warmups)); suffix=trial$TRIAL_INDEX; fi
    if (( sequence%2==1 )); then
      EXECUTION_ORDER=baseline-first
      run_baseline "$CURRENT_FIXTURE" "$kind" "$input" "$refseq" "$safe" "$suffix"
      if (( native_only == 0 )); then
        for mode in $mode_list; do for worker in "${chart_worker_specs[@]}"; do run_chart "$CURRENT_FIXTURE" "$kind" "$input" "$refseq" "$safe" "$suffix" "$mode" "$worker"; done; done
      fi
    else
      EXECUTION_ORDER=chart-first
      if (( native_only == 0 )); then
        for mode in $mode_list; do for worker in "${chart_worker_specs[@]}"; do run_chart "$CURRENT_FIXTURE" "$kind" "$input" "$refseq" "$safe" "$suffix" "$mode" "$worker"; done; done
      fi
      run_baseline "$CURRENT_FIXTURE" "$kind" "$input" "$refseq" "$safe" "$suffix"
    fi
  done
done
fi

# No validation or semantic-capture process is allowed inside the paired timed
# sequence above.  Resolve every generated output first, then run one compact
# semantic companion per exact row/worker contract, and finally bind those
# correctness results back into the raw measured rows.
declare -A DEFERRED_SCORE=() DEFERRED_NODES=() DEFERRED_EDGES=()
declare -A DEFERRED_OUTPUT_SHA=() DEFERRED_VALIDATION=()
while IFS=$'\t' read -r recorded row_id trial method pb initial report curve wall score_prefix row_timeout row_rss_limit; do
  timeout_seconds=$row_timeout
  score_output "$row_id deferred output validation trial=$trial" "$pb" \
    "${score_prefix}.out" "${score_prefix}.err" "$row_rss_limit"
  result_key="$row_id|$trial"
  if (( SCORE_STATUS == 0 )); then
    deferred_score=$(extract_parsimony_min "${score_prefix}.out")
    deferred_nodes=$(extract_value "${score_prefix}.out" nodes)
    deferred_edges=$(extract_value "${score_prefix}.out" edges)
    if [[ -z "$deferred_score" || "$SCORE_OUTPUT_PARSIMONY" == - || \
          "$deferred_score" != "$SCORE_OUTPUT_PARSIMONY" ]]; then
      deferred_ok=0
    else
      deferred_ok=1
    fi
  else
    deferred_ok=0
  fi
  if (( recorded )); then
    if (( deferred_ok )); then
      DEFERRED_VALIDATION[$result_key]=ok
      DEFERRED_SCORE[$result_key]=$deferred_score
      DEFERRED_NODES[$result_key]=${deferred_nodes:-NA}
      DEFERRED_EDGES[$result_key]=${deferred_edges:-NA}
      DEFERRED_OUTPUT_SHA[$result_key]=$SCORE_OUTPUT_SHA
    else
      DEFERRED_VALIDATION[$result_key]=failed
    fi
  elif (( deferred_ok == 0 )); then
    WARMUP_FAILURES=$((WARMUP_FAILURES+1))
  fi
done <"$validation_queue"

SEMANTIC_COMPANION_FAILURES=0
for companion_key in "${canonical_queue_order[@]}"; do
  timeout_seconds=${CANONICAL_QUEUE_TIMEOUT[$companion_key]}
  companion_rss_limit=${CANONICAL_QUEUE_RSS_LIMIT[$companion_key]}
  eval "${CANONICAL_QUEUE_COMMAND[$companion_key]}"
  IFS='|' read -r companion_fixture companion_method companion_worker \
    <<<"${CANONICAL_QUEUE_ROW[$companion_key]}"
  run_capture "$companion_fixture $companion_method deferred semantic companion workers=$companion_worker" \
    "${CANONICAL_QUEUE_OUT[$companion_key]}" "${CANONICAL_QUEUE_ERR[$companion_key]}" \
    "$companion_rss_limit" \
    "${companion_command[@]}"
  companion_json=${CANONICAL_QUEUE_JSON[$companion_key]}
  companion_pb=${CANONICAL_QUEUE_PB[$companion_key]}
  companion_ok=1
  (( RUN_RUNNER_STATUS == 0 )) && [[ -s "$companion_json" && -s "$companion_pb" ]] || companion_ok=0
  if (( companion_ok )); then
    companion_search=$(extract_json_string "$companion_json" semantic_sha256)
    [[ "$companion_search" =~ ^[0-9a-f]{64}$ &&
       $(extract_json_number "$companion_json" schema_version) == 1 &&
       $(extract_json_string "$companion_json" digest_algorithm) == sha256 ]] || companion_ok=0
    for digest_key in contract_sha256 candidates_sha256 exact_sha256 \
      acceptance_sha256 chain_sha256 final_topology_sha256; do
      digest_value=$(extract_json_string "$companion_json" "$digest_key")
      [[ "$digest_value" =~ ^[0-9a-f]{64}$ ]] || companion_ok=0
    done
    for count_key in record_count candidate_count exact_candidate_count iteration_count; do
      count_value=$(extract_json_number "$companion_json" "$count_key")
      [[ "$count_value" =~ ^[0-9]+$ ]] || companion_ok=0
    done
  fi
  if (( companion_ok )); then
    companion_score_prefix=${CANONICAL_QUEUE_OUT[$companion_key]%.out}.score
    score_output "$companion_fixture $companion_method deferred semantic output workers=$companion_worker" \
      "$companion_pb" "${companion_score_prefix}.out" "${companion_score_prefix}.err" \
      "$companion_rss_limit"
    (( SCORE_STATUS == 0 )) || companion_ok=0
  fi
  if (( companion_ok )); then
    canonical_companion_search[$companion_key]=$companion_search
    canonical_companion_output[$companion_key]=$SCORE_OUTPUT_SHA
  else
    echo "semantic companion failure: $companion_key" >&2
    SEMANTIC_COMPANION_FAILURES=$((SEMANTIC_COMPANION_FAILURES+1))
    continue
  fi
  if [[ -n ${CANONICAL_QUEUE_FULL_COMMAND[$companion_key]:-} ]]; then
    eval "${CANONICAL_QUEUE_FULL_COMMAND[$companion_key]}"
    full_out=${CANONICAL_QUEUE_FULL_JSON[$companion_key]%.json}.out
    full_err=${CANONICAL_QUEUE_FULL_JSON[$companion_key]%.json}.err
    run_capture "$companion_fixture $companion_method deferred explicit-W1 full correctness" \
      "$full_out" "$full_err" "$companion_rss_limit" "${full_command[@]}"
    full_semantic=$(extract_json_string "${CANONICAL_QUEUE_FULL_JSON[$companion_key]}" semantic_sha256)
    full_sha=$(sha256sum "${CANONICAL_QUEUE_FULL_SIDECAR[$companion_key]}" 2>/dev/null | awk '{print $1}')
    if (( RUN_RUNNER_STATUS != 0 )) || [[ "$full_semantic" != "$full_sha" ||
         "$full_semantic" != "${canonical_companion_search[$companion_key]}" ]]; then
      echo "compact/full explicit-W1 semantic mismatch: $companion_key" >&2
      SEMANTIC_COMPANION_FAILURES=$((SEMANTIC_COMPANION_FAILURES+1))
    fi
  fi
done

raw_final="$out_dir/.raw_trials.final.tsv"
IFS=$'\t' read -r -a raw_header <"$raw_trials_tsv"
declare -A raw_index=()
for ((raw_i=0; raw_i<${#raw_header[@]}; ++raw_i)); do raw_index[${raw_header[$raw_i]}]=$raw_i; done
(
  IFS=$'\t'; printf '%s\n' "${raw_header[*]}"
  tail -n +2 "$raw_trials_tsv" | while IFS=$'\t' read -r -a field; do
    row_id=${field[${raw_index[row_id]}]}
    trial=${field[${raw_index[trial_index]}]}
    status=${field[${raw_index[status]}]}
    result_key="$row_id|$trial"
    if [[ "$status" == pending_validation ]]; then
      if [[ ${DEFERRED_VALIDATION[$result_key]:-failed} != ok ]]; then
        field[${raw_index[status]}]=failed
        field[${raw_index[validation_status]}]=failed
      else
        method=${field[${raw_index[method]}]}
        initial=${field[${raw_index[initial_validated_parsimony_min]}]}
        final=${DEFERRED_SCORE[$result_key]}
        output_sha=${DEFERRED_OUTPUT_SHA[$result_key]}
        search_sha=-
        if [[ "$method" != sample_explore_merge ]]; then
          search_sha=${canonical_companion_search[$row_id]:--}
        fi
        argv_sha=${field[${raw_index[canonical_argv_sha256]}]}
        final_status=ok
        [[ "$method" == sample_explore_merge || "$search_sha" =~ ^[0-9a-f]{64}$ ]] || final_status=failed
        [[ "$method" == sample_explore_merge || ${canonical_companion_output[$row_id]:--} == "$output_sha" ]] || final_status=failed
        trial_sha=$(trial_semantic_digest "$method" "$search_sha" "$output_sha" "$argv_sha")
        if [[ -n "$workload_manifest" ]]; then
          expected_outcome=$(manifest_row_value "$row_id" expected_outcome)
          expected_initial=$(manifest_row_value "$row_id" expected_initial_score)
          expected_final=$(manifest_row_value "$row_id" expected_final_score)
          expected_validated=$(manifest_row_value "$row_id" expected_validated_parsimony)
          expected_search=$(manifest_row_value "$row_id" oracle_search_semantic_sha256)
          expected_output=$(manifest_row_value "$row_id" oracle_output_semantic_sha256)
          expected_trial=$(manifest_row_value "$row_id" oracle_trial_semantic_sha256)
          [[ "$expected_initial" == - || "$initial" == "$expected_initial" ]] || final_status=workload_mismatch
          [[ "$expected_final" == - || "$final" == "$expected_final" ]] || final_status=workload_mismatch
          [[ "$expected_validated" == - || "$final" == "$expected_validated" ]] || final_status=workload_mismatch
          [[ "$expected_search" == - || "$search_sha" == "$expected_search" ]] || final_status=workload_mismatch
          [[ "$expected_output" == - || "$output_sha" == "$expected_output" ]] || final_status=workload_mismatch
          [[ "$expected_trial" == - || "$trial_sha" == "$expected_trial" ]] || final_status=workload_mismatch
          case "$expected_outcome" in
            ok|timeout) ;;
            scale_limit) [[ "$final_status" != ok ]] || final_status=scale_limit ;;
            *) final_status=workload_mismatch ;;
          esac
        fi
        field[${raw_index[status]}]=$final_status
        field[${raw_index[validation_status]}]=ok
        field[${raw_index[final_validated_parsimony_min]}]=$final
        field[${raw_index[best_validated_parsimony_min]}]=$(min_score "$initial" "$final")
        field[${raw_index[output_semantic_sha256]}]=$output_sha
        field[${raw_index[search_semantic_sha256]}]=$search_sha
        field[${raw_index[trial_semantic_sha256]}]=$trial_sha
        field[${raw_index[canonical_digest]}]=$trial_sha
        field[${raw_index[final_dag_nodes]}]=${DEFERRED_NODES[$result_key]}
        field[${raw_index[final_dag_edges]}]=${DEFERRED_EDGES[$result_key]}
        if [[ "$method" == sample_explore_merge ]]; then
          # Native's loop summary is a sampled-tree objective, so its stable
          # TSV endpoint is the externally validated merged-DAG minimum.
          field[${raw_index[best_reported_objective]}]=$final
          write_baseline_curve "${field[${raw_index[report_path]}]}" "$initial" \
            "${field[${raw_index[curve_path]}]}" "${field[${raw_index[wall_clock_s]}]}" "$final"
        else
          write_chart_curve "${field[${raw_index[report_path]}]}" "$initial" \
            "${field[${raw_index[curve_path]}]}" "${field[${raw_index[wall_clock_s]}]}" "$final"
        fi
      fi
    fi
    (IFS=$'\t'; printf '%s\n' "${field[*]}")
  done
) >"$raw_final"
mv "$raw_final" "$raw_trials_tsv"
rm -f "$validation_queue"

# Aggregate measured rows by fixture/method/requested worker. wall_clock_s is
# the median; explicit aggregate columns retain the maximums and trial count.
awk -F '\t' -v OFS='\t' '
  NR==1 {for(i=1;i<=NF;i++){h[$i]=i; name[i]=$i} nfields=NF;
    print $0,"trial_count","wall_clock_max_s","user_cpu_median_s","system_cpu_median_s","max_rss_max_kb","peak_sampled_rss_max_kb"; next}
  {
    g=$h["fixture"] SUBSEP $h["method"] SUBSEP $h["requested_workers"]
    if(!(g in seen)){seen[g]=1; order[++ng]=g; for(i=1;i<=NF;i++) value[g,i]=$i}
    n[g]++; wall[g,n[g]]=$h["wall_clock_s"]+0; user[g,n[g]]=$h["user_cpu_s"]+0; sys[g,n[g]]=$h["system_cpu_s"]+0
    if($h["wall_clock_s"]+0>wallmax[g])wallmax[g]=$h["wall_clock_s"]+0
    if($h["max_rss_kb"]+0>rssmax[g])rssmax[g]=$h["max_rss_kb"]+0
    if($h["peak_sampled_rss_kb"]+0>peakrssmax[g])peakrssmax[g]=$h["peak_sampled_rss_kb"]+0
    if($h["status"]!="ok")value[g,h["status"]]=$h["status"]
    if($h["validation_status"]!="ok")value[g,h["validation_status"]]=$h["validation_status"]
    if(value[g,h["canonical_digest"]]!=$h["canonical_digest"])value[g,h["canonical_digest"]]="MISMATCH"
  }
  function med(kind,g,count, a,i,j,x) {
    for(i=1;i<=count;i++){if(kind=="w")a[i]=wall[g,i];else if(kind=="u")a[i]=user[g,i];else a[i]=sys[g,i]}
    for(i=2;i<=count;i++){x=a[i];j=i-1;while(j>=1&&a[j]>x){a[j+1]=a[j];j--}a[j+1]=x}
    return count%2?a[(count+1)/2]:(a[count/2]+a[count/2+1])/2
  }
  END {for(k=1;k<=ng;k++){g=order[k];value[g,h["wall_clock_s"]]=sprintf("%.6f",med("w",g,n[g]));
    line=value[g,1];for(i=2;i<=nfields;i++)line=line OFS value[g,i];
    print line,n[g],sprintf("%.6f",wallmax[g]),sprintf("%.6f",med("u",g,n[g])),sprintf("%.6f",med("s",g,n[g])),rssmax[g],peakrssmax[g]}}
' "$raw_trials_tsv" >"$summary_tsv"

gate_failures=0
(( SEMANTIC_COMPANION_FAILURES == 0 )) || \
  gate_failures=$((gate_failures+SEMANTIC_COMPANION_FAILURES))

phase6_admission_tsv="$out_dir/phase6_admission_evidence.tsv"
phase6_rss_tsv="$out_dir/phase6_rss_comparisons.tsv"
phase6_exact_speedup_tsv="$out_dir/phase6_exact_verification_speedup.tsv"
printf '%s\n' $'comparison_sha256\tcontract_sha256\trow_id\tfixture\tmethod\tchart_top_k_exact\trequested_workers\ttrial_index\tstatus\tvalidation_status\texact_verifications\tconfigured_chart_memory_budget\tpeak_sampled_rss_kb\texact_candidate_admission_batches\texact_candidate_parallel_batches\texact_candidate_inner_parallel_batches\texact_candidate_memory_limited_batches\texact_candidate_peak_admitted_bytes\texact_candidate_peak_projected_resident_bytes\texact_candidate_queued_for_memory_ms\trunner_outcome\ttimed_out\texact_verification_ms\tpeak_concurrent_exact_verifiers\tchart_axis_exact_candidate_active_worker_high_water\texact_candidate_timing_count\texact_candidate_verification_ms_min\texact_candidate_verification_ms_mean\texact_candidate_verification_ms_max\tsearch_semantic_sha256\toutput_semantic_sha256' >"$phase6_admission_tsv"
if [[ -n "$workload_manifest" ]]; then
  declare -A phase6_top_k_by_row=()
  declare -A phase6_comparison_by_row=()
  declare -A phase6_contract_by_row=()
  while IFS=$'\t' read -r row_id fixture method requested trial status validation \
    exact budget rss admission parallel inner limited admitted projected queued \
    runner_outcome timed_out exact_ms peak_concurrent exact_axis_high_water \
    timing_count timing_min timing_mean timing_max search_semantic output_semantic; do
    if [[ -z ${phase6_top_k_by_row[$row_id]+set} ]]; then
      phase6_top_k_by_row[$row_id]=$(manifest_row_value "$row_id" chart_top_k_exact)
    fi
    top_k=${phase6_top_k_by_row[$row_id]}
    comparison_sha=-
    contract_sha=-
    if [[ "$requested" == 1 || "$requested" == 8 ]]; then
      if [[ -z ${phase6_comparison_by_row[$row_id]+set} ]]; then
        phase6_comparison_by_row[$row_id]=$(phase6_worker_comparison_key "$row_id" | sha256sum | awk '{print $1}')
        phase6_contract_by_row[$row_id]=$(phase6_worker_contract_key "$row_id" | sha256sum | awk '{print $1}')
      fi
      comparison_sha=${phase6_comparison_by_row[$row_id]}
      contract_sha=${phase6_contract_by_row[$row_id]}
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$comparison_sha" "$contract_sha" "$row_id" "$fixture" "$method" \
      "$top_k" "$requested" "$trial" "$status" "$validation" "$exact" \
      "$budget" "$rss" "$admission" "$parallel" "$inner" "$limited" \
      "$admitted" "$projected" "$queued" "$runner_outcome" "$timed_out" \
      "$exact_ms" "$peak_concurrent" "$exact_axis_high_water" "$timing_count" \
      "$timing_min" "$timing_mean" "$timing_max" "$search_semantic" \
      "$output_semantic" >>"$phase6_admission_tsv"
  done < <(awk -F '\t' -v OFS='\t' '
    NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
    $h["method"]!="sample_explore_merge" {
      print $h["row_id"],$h["fixture"],$h["method"],$h["requested_workers"],
      $h["trial_index"],$h["status"],$h["validation_status"],
      $h["exact_verifications"],$h["configured_chart_memory_budget"],
      $h["peak_sampled_rss_kb"],$h["exact_candidate_admission_batches"],
      $h["exact_candidate_parallel_batches"],
      $h["exact_candidate_inner_parallel_batches"],
      $h["exact_candidate_memory_limited_batches"],
      $h["exact_candidate_peak_admitted_bytes"],
      $h["exact_candidate_peak_projected_resident_bytes"],
      $h["exact_candidate_queued_for_memory_ms"],$h["runner_outcome"],
      $h["timed_out"],$h["exact_verification_ms"],
      $h["peak_concurrent_exact_verifiers"],
      $h["chart_axis_exact_candidate_active_worker_high_water"],
      $h["exact_candidate_timing_count"],
      $h["exact_candidate_verification_ms_min"],
      $h["exact_candidate_verification_ms_mean"],
      $h["exact_candidate_verification_ms_max"],
      $h["search_semantic_sha256"],$h["output_semantic_sha256"]
    }
  ' "$raw_trials_tsv")
fi

phase6_expected_k16_rows=0
phase6_require_k16_cardinality=0
phase6_expected_k16_row_ids_csv=
phase6_expected_rss_pair_count=0
phase6_expected_rss_keys_csv=
phase6_expected_exact_speed_pair_count=0
phase6_expected_exact_speed_keys_csv=
phase6_expected_exact_speed_contracts_csv=
if [[ -n "$run_manifest_group" ]]; then
  declare -A phase6_expected_rss_key_seen=()
  declare -A phase6_expected_rss_worker_count=()
  declare -A phase6_expected_rss_contract_by_key=()
  declare -A phase6_record_only_rss_key=()
  declare -A phase6_expected_exact_speed_key_seen=()
  declare -A phase6_expected_exact_speed_worker_count=()
  declare -A phase6_expected_exact_speed_contract_by_key=()
  declare -A phase6_record_only_exact_speed_key=()
  declare -A phase6_record_only_timeout_row=()
  for row_id in "${expected_timeout_ids[@]}"; do
    phase6_record_only_timeout_row[$row_id]=1
  done
  phase6_expected_rss_w1_rows=0
  phase6_expected_rss_w8_rows=0
  phase6_expected_exact_speed_rows=0
  for row_id in "${selected_manifest_rows[@]}"; do
    method=$(manifest_row_value "$row_id" method)
    expected_outcome=$(manifest_row_value "$row_id" expected_outcome)
    row_top_k=$(manifest_row_value "$row_id" chart_top_k_exact)
    workload_name=$(manifest_row_value "$row_id" workload_name)
    if [[ "$workload_name" == exact-medium-topk4 &&
          "$method" == chart_spr_grammar_exact ]]; then
      (( phase6_expected_exact_speed_rows += 1 ))
      if [[ "$row_top_k" != 4 ]]; then
        echo "gate failure: selected Phase-6 exact-medium-topk4 grammar row has TopK drift: $row_id" >&2
        gate_failures=$((gate_failures+1))
      fi
      if [[ "$expected_outcome" != ok && "$expected_outcome" != timeout ]]; then
        echo "gate failure: selected Phase-6 exact-speed row is manifest-labelled $expected_outcome: $row_id" >&2
        gate_failures=$((gate_failures+1))
      fi
      requested=$(manifest_row_value "$row_id" requested_workers)
      comparison_sha=$(phase6_worker_comparison_key "$row_id" | sha256sum | awk '{print $1}')
      contract_sha=$(phase6_worker_contract_key "$row_id" | sha256sum | awk '{print $1}')
      phase6_expected_exact_speed_key_seen[$comparison_sha]=1
      if [[ ( "$requested" == 1 || "$requested" == 8 ) &&
            "$expected_outcome" == timeout &&
            -n ${phase6_record_only_timeout_row[$row_id]:-} ]]; then
        phase6_record_only_exact_speed_key[$comparison_sha]=1
      fi
      if [[ "$requested" == 1 || "$requested" == 8 ]]; then
        worker_key="$comparison_sha:$requested"
        worker_rows=${phase6_expected_exact_speed_worker_count[$worker_key]:-0}
        phase6_expected_exact_speed_worker_count[$worker_key]=$((worker_rows + 1))
      fi
      if [[ -n ${phase6_expected_exact_speed_contract_by_key[$comparison_sha]:-} &&
            ${phase6_expected_exact_speed_contract_by_key[$comparison_sha]} != "$contract_sha" ]]; then
        echo "gate failure: selected Phase-6 exact-speed rows have workload contract drift for $comparison_sha" >&2
        gate_failures=$((gate_failures+1))
      else
        phase6_expected_exact_speed_contract_by_key[$comparison_sha]=$contract_sha
      fi
    fi
    requested=$(manifest_row_value "$row_id" requested_workers)
    if [[ "$method" != sample_explore_merge && "$row_top_k" =~ ^[1-9][0-9]*$ &&
          ( "$requested" == 1 || "$requested" == 8 ) &&
          "$expected_outcome" == timeout &&
          -n ${phase6_record_only_timeout_row[$row_id]:-} ]]; then
      comparison_sha=$(phase6_worker_comparison_key "$row_id" | sha256sum | awk '{print $1}')
      phase6_record_only_rss_key[$comparison_sha]=1
    fi
    phase6_expect_current_success=0
    if [[ "$expected_outcome" == ok ]] || \
       [[ "$expected_outcome" == timeout &&
          -z ${phase6_record_only_timeout_row[$row_id]:-} ]]; then
      phase6_expect_current_success=1
    fi
    [[ "$method" != sample_explore_merge &&
       "$phase6_expect_current_success" == 1 ]] || continue
    if [[ "$row_top_k" == 16 ]]; then
      (( phase6_expected_k16_rows += repetitions ))
      [[ -z "$phase6_expected_k16_row_ids_csv" ]] || \
        phase6_expected_k16_row_ids_csv+=,
      phase6_expected_k16_row_ids_csv+=$row_id
    fi
    (( row_top_k > 0 )) || continue
    requested=$(manifest_row_value "$row_id" requested_workers)
    [[ "$requested" == 1 || "$requested" == 8 ]] || continue
    comparison_sha=$(phase6_worker_comparison_key "$row_id" | sha256sum | awk '{print $1}')
    contract_sha=$(phase6_worker_contract_key "$row_id" | sha256sum | awk '{print $1}')
    phase6_expected_rss_key_seen[$comparison_sha]=1
    worker_key="$comparison_sha:$requested"
    worker_rows=${phase6_expected_rss_worker_count[$worker_key]:-0}
    phase6_expected_rss_worker_count[$worker_key]=$((worker_rows + 1))
    if [[ "$requested" == 1 ]]; then
      (( phase6_expected_rss_w1_rows += 1 ))
    else
      (( phase6_expected_rss_w8_rows += 1 ))
    fi
    if [[ -n ${phase6_expected_rss_contract_by_key[$comparison_sha]:-} &&
          ${phase6_expected_rss_contract_by_key[$comparison_sha]} != "$contract_sha" ]]; then
      echo "gate failure: selected Phase-6 W1/W8 rows have workload contract drift for $comparison_sha" >&2
      gate_failures=$((gate_failures+1))
    else
      phase6_expected_rss_contract_by_key[$comparison_sha]=$contract_sha
    fi
  done
  (( phase6_expected_k16_rows == 0 )) || phase6_require_k16_cardinality=1

  # A selected group that contains successful W1 and W8 exact rows must define
  # complete one-to-one comparison pairs. Otherwise label/key drift could make
  # the RSS gate produce only its header and pass vacuously.
  if (( phase6_expected_rss_w1_rows > 0 && phase6_expected_rss_w8_rows > 0 )); then
    phase6_rss_keys_requiring_gate=0
    for comparison_sha in "${!phase6_expected_rss_key_seen[@]}"; do
      [[ -z ${phase6_record_only_rss_key[$comparison_sha]:-} ]] || continue
      (( phase6_rss_keys_requiring_gate += 1 ))
      w1_rows=${phase6_expected_rss_worker_count[$comparison_sha:1]:-0}
      w8_rows=${phase6_expected_rss_worker_count[$comparison_sha:8]:-0}
      if (( w1_rows != 1 || w8_rows != 1 )); then
        echo "gate failure: selected Phase-6 RSS comparison $comparison_sha has W1/W8 manifest cardinality $w1_rows/$w8_rows" >&2
        gate_failures=$((gate_failures+1))
        continue
      fi
      [[ -z "$phase6_expected_rss_keys_csv" ]] || phase6_expected_rss_keys_csv+=,
      phase6_expected_rss_keys_csv+=$comparison_sha
      (( phase6_expected_rss_pair_count += 1 ))
    done
    if (( phase6_rss_keys_requiring_gate > 0 &&
          phase6_expected_rss_pair_count == 0 )); then
      echo "gate failure: selected Phase-6 group has successful W1 and W8 exact rows but no RSS comparison pair" >&2
      gate_failures=$((gate_failures+1))
    fi
  fi

  # The named medium TopK4 workload is the strict Phase-6 performance gate.
  # Selecting any part of it requires one and only one manifest row at each
  # endpoint; a partial worker selection must not create a vacuous pass.
  if (( phase6_expected_exact_speed_rows > 0 )); then
    phase6_exact_speed_keys_requiring_gate=0
    for comparison_sha in "${!phase6_expected_exact_speed_key_seen[@]}"; do
      [[ -z ${phase6_record_only_exact_speed_key[$comparison_sha]:-} ]] || continue
      (( phase6_exact_speed_keys_requiring_gate += 1 ))
      w1_rows=${phase6_expected_exact_speed_worker_count[$comparison_sha:1]:-0}
      w8_rows=${phase6_expected_exact_speed_worker_count[$comparison_sha:8]:-0}
      if (( w1_rows != 1 || w8_rows != 1 )); then
        echo "gate failure: selected Phase-6 exact-speed comparison $comparison_sha has W1/W8 manifest cardinality $w1_rows/$w8_rows" >&2
        gate_failures=$((gate_failures+1))
        continue
      fi
      [[ -z "$phase6_expected_exact_speed_keys_csv" ]] || \
        phase6_expected_exact_speed_keys_csv+=,
      phase6_expected_exact_speed_keys_csv+=$comparison_sha
      [[ -z "$phase6_expected_exact_speed_contracts_csv" ]] || \
        phase6_expected_exact_speed_contracts_csv+=,
      phase6_expected_exact_speed_contracts_csv+="$comparison_sha:${phase6_expected_exact_speed_contract_by_key[$comparison_sha]}"
      (( phase6_expected_exact_speed_pair_count += 1 ))
    done
    if (( phase6_exact_speed_keys_requiring_gate > 0 &&
          phase6_expected_exact_speed_pair_count == 0 )); then
      echo "gate failure: selected Phase-6 medium TopK4 rows define no complete exact-speed pair" >&2
      gate_failures=$((gate_failures+1))
    fi
  fi
fi

if ! awk -F '\t' -v expected_rows="$phase6_expected_k16_rows" \
    -v expected_ids_csv="$phase6_expected_k16_row_ids_csv" \
    -v require_cardinality="$phase6_require_k16_cardinality" '
  BEGIN {
    n=split(expected_ids_csv,ids,",")
    for(i=1;i<=n;i++) if(ids[i]!="") expected_id[ids[i]]=1
  }
  function uint(v) {return v ~ /^[0-9]+$/}
  function positive(v) {return v ~ /^[1-9][0-9]*$/}
  function decimal(v) {return v ~ /^[0-9]+([.][0-9]+)?$/}
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  $h["chart_top_k_exact"]==16 && $h["method"]!="sample_explore_merge" &&
      $h["status"]=="ok" && $h["validation_status"]=="ok" {
    if ($h["row_id"] in expected_id) observed++
    row=$h["row_id"] " trial " $h["trial_index"]
    exact=$h["exact_verifications"]
    budget=$h["configured_chart_memory_budget"]
    batches=$h["exact_candidate_admission_batches"]
    parallel=$h["exact_candidate_parallel_batches"]
    inner=$h["exact_candidate_inner_parallel_batches"]
    limited=$h["exact_candidate_memory_limited_batches"]
    admitted=$h["exact_candidate_peak_admitted_bytes"]
    projected=$h["exact_candidate_peak_projected_resident_bytes"]
    queued=$h["exact_candidate_queued_for_memory_ms"]
    if (!positive(exact) || !positive(budget) || !positive(batches) ||
        !uint(parallel) || !uint(inner) || !uint(limited) ||
        !positive(admitted) || !positive(projected) || !decimal(queued) ||
        parallel+inner>batches || limited>batches || admitted>projected ||
        projected>budget) {
      print "invalid K16 admission evidence: " row > "/dev/stderr"
      bad=1
    }
  }
  END {
    if (require_cardinality && observed+0 != expected_rows+0) {
      print "invalid K16 evidence cardinality: expected " expected_rows \
            " successful rows, observed " (observed+0) > "/dev/stderr"
      bad=1
    }
    exit bad
  }
' "$phase6_admission_tsv"; then
  echo "gate failure: Phase-6 K16 concurrent-memory admission" >&2
  gate_failures=$((gate_failures+1))
fi

phase6_exact_speedup_body="$out_dir/.phase6_exact_verification_speedup.body.tsv"
if ! LC_ALL=C awk -F '\t' -v OFS='\t' \
    -v expected_keys_csv="$phase6_expected_exact_speed_keys_csv" \
    -v expected_contracts_csv="$phase6_expected_exact_speed_contracts_csv" \
    -v expected_pair_count="$phase6_expected_exact_speed_pair_count" \
    -v expected_trials="$repetitions" '
  function uint(v) {return v ~ /^[0-9]+$/}
  function positive(v) {return v ~ /^[1-9][0-9]*$/}
  function hash(v) {return v ~ /^[0-9a-f][0-9a-f]*$/ && length(v)==64}
  # Product timing fields are printed with three decimal places. Convert them
  # to integer thousandths so the 2x gate never depends on rounded ratios or
  # implementation-specific floating-point formatting.
  function millims(v, parts,n,frac) {
    if (v !~ /^[0-9]+([.][0-9][0-9]?[0-9]?)?$/) return -1
    n=split(v,parts,".")
    if (length(parts[1])>9) return -1
    frac=(n==1 ? "" : parts[2])
    while (length(frac)<3) frac=frac "0"
    return (parts[1]+0)*1000+(frac+0)
  }
  function median_twice(g,w,n, values,i,j,x) {
    for (i=1;i<=n;i++) values[i]=sample[g,w,i]
    for (i=2;i<=n;i++) {
      x=values[i]; j=i-1
      while (j>=1 && values[j]>x) {values[j+1]=values[j]; j--}
      values[j+1]=x
    }
    return n%2 ? 2*values[(n+1)/2] : values[n/2]+values[n/2+1]
  }
  function trial_values(g,w,n, value,i) {
    value=""
    for (i=1;i<=n;i++) {
      if (!((g SUBSEP w SUBSEP i) in trial_sample)) {
        bad=1
        return "MISSING"
      }
      if (value!="") value=value ","
      value=value i ":" sprintf("%.0f",trial_sample[g,w,i])
    }
    return value
  }
  BEGIN {
    n=split(expected_keys_csv,keys,",")
    for (i=1;i<=n;i++) if (keys[i]!="") expected[keys[i]]=1
    n=split(expected_contracts_csv,contracts,",")
    for (i=1;i<=n;i++) if (contracts[i]!="") {
      split(contracts[i],pair,":")
      expected_contract[pair[1]]=pair[2]
    }
  }
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  $h["comparison_sha256"] in expected {
    g=$h["comparison_sha256"]
    worker=$h["requested_workers"]
    if (worker!=1 && worker!=8) next
    row=$h["row_id"] " trial " $h["trial_index"]
    if ($h["contract_sha256"]!=expected_contract[g]) {
      print "Phase-6 exact-speed workload contract drift for " row > "/dev/stderr"
      bad=1
    }
    if ($h["method"]!="chart_spr_grammar_exact" ||
        $h["chart_top_k_exact"]!="4" || $h["exact_verifications"]!="4") {
      print "inconsistent Phase-6 medium TopK4 row: " row > "/dev/stderr"
      bad=1
    }
    if ($h["status"]!="ok" || $h["validation_status"]!="ok" ||
        $h["runner_outcome"]!="exited" || $h["timed_out"]!="0") {
      print "Phase-6 exact-speed row is not a successful non-timeout execution: " row > "/dev/stderr"
      bad=1
      next
    }
    row_search=$h["search_semantic_sha256"]
    row_output=$h["output_semantic_sha256"]
    if (!hash(row_search) || !hash(row_output)) {
      print "invalid Phase-6 exact-speed semantic digest for " row > "/dev/stderr"
      bad=1
      next
    }
    if (!(g in semantic_seen)) {
      semantic_seen[g]=1
      search_semantic[g]=row_search
      output_semantic[g]=row_output
    } else if (search_semantic[g]!=row_search || output_semantic[g]!=row_output) {
      print "Phase-6 exact-speed search/output semantic drift for " row > "/dev/stderr"
      semantic_drift[g]=1
      bad=1
    }
    trial=$h["trial_index"]
    if (!positive(trial) || trial>expected_trials || seen_trial[g,worker,trial]++) {
      print "duplicate or invalid Phase-6 exact-speed trial index: " row > "/dev/stderr"
      bad=1
      next
    }
    exact_ms=millims($h["exact_verification_ms"])
    if (exact_ms<=0) {
      print "invalid Phase-6 exact_verification_ms for " row > "/dev/stderr"
      bad=1
      next
    }
    timing_count=$h["exact_candidate_timing_count"]
    timing_min=millims($h["exact_candidate_verification_ms_min"])
    timing_mean=millims($h["exact_candidate_verification_ms_mean"])
    timing_max=millims($h["exact_candidate_verification_ms_max"])
    if (timing_count!="4" || timing_min<0 || timing_mean<0 || timing_max<0 ||
        timing_min>timing_mean || timing_mean>timing_max ||
        timing_max>exact_ms+1) {
      print "inconsistent Phase-6 exact-candidate timing evidence for " row > "/dev/stderr"
      bad=1
      next
    }
    parallel=$h["exact_candidate_parallel_batches"]
    peak=$h["peak_concurrent_exact_verifiers"]
    axis=$h["chart_axis_exact_candidate_active_worker_high_water"]
    if (!uint(parallel) || !uint(peak) || !uint(axis)) {
      print "invalid Phase-6 exact-candidate scheduler evidence for " row > "/dev/stderr"
      bad=1
      next
    }
    if (worker==8 && (parallel<1 || peak<2 || axis<2)) {
      print "Phase-6 W8 candidate-parallel path not activated for " row > "/dev/stderr"
      bad=1
    }
    if (worker==1 && (parallel!=0 || peak!=1 || axis>1)) {
      print "inconsistent Phase-6 W1 candidate scheduling evidence for " row > "/dev/stderr"
      bad=1
    }
    count[g,worker]++
    sample[g,worker,count[g,worker]]=exact_ms
    trial_sample[g,worker,trial]=exact_ms
    contract[g]=$h["contract_sha256"]
    fixture[g]=$h["fixture"]
    method[g]=$h["method"]
  }
  END {
    for (g in expected) {
      if (count[g,1]+0!=expected_trials || count[g,8]+0!=expected_trials) {
        print "Phase-6 exact-speed evidence cardinality mismatch for " g \
              ": expected " expected_trials "/" expected_trials \
              ", observed " (count[g,1]+0) "/" (count[g,8]+0) > "/dev/stderr"
        bad=1
        continue
      }
      w1=median_twice(g,1,expected_trials)
      w8=median_twice(g,8,expected_trials)
      lhs=2*w8
      rhs=w1
      if (lhs>rhs) {
        print "Phase-6 W8 exact_verification_ms median is not at least 2.0x faster for " \
              fixture[g] > "/dev/stderr"
        bad=1
      }
      print g,contract[g],fixture[g],method[g],search_semantic[g],
            output_semantic[g],4,expected_trials,
            trial_values(g,1,expected_trials),trial_values(g,8,expected_trials),
            sprintf("%.0f",w1),sprintf("%.0f",w8),sprintf("%.0f",lhs),
            sprintf("%.0f",rhs),sprintf("%.4f",w1/2000),
            sprintf("%.4f",w8/2000),sprintf("%.9f",w1/w8)
      observed_pairs++
    }
    if (observed_pairs+0!=expected_pair_count+0) {
      print "Phase-6 exact-speed arithmetic cardinality mismatch: expected " \
            expected_pair_count ", observed " (observed_pairs+0) > "/dev/stderr"
      bad=1
    }
    exit bad
  }
' "$phase6_admission_tsv" >"$phase6_exact_speedup_body"; then
  echo "gate failure: Phase-6 medium TopK4 exact-verification speedup" >&2
  gate_failures=$((gate_failures+1))
fi
LC_ALL=C sort -t $'\t' -k1,1 "$phase6_exact_speedup_body" \
  >"$phase6_exact_speedup_body.sorted"
mv "$phase6_exact_speedup_body.sorted" "$phase6_exact_speedup_body"
{
  printf '%s\n' $'arithmetic_sha256\tcomparison_sha256\tcontract_sha256\tfixture\tmethod\tsearch_semantic_sha256\toutput_semantic_sha256\tchart_top_k_exact\ttrial_count_per_worker\tw1_trial_millims_by_index\tw8_trial_millims_by_index\tw1_median_twice_millims\tw8_median_twice_millims\tgate_lhs_twice_millims\tgate_rhs_twice_millims\tw1_median_ms\tw8_median_ms\tw1_over_w8'
  while IFS= read -r arithmetic_row; do
    [[ -n "$arithmetic_row" ]] || continue
    arithmetic_sha=$(printf '%s\n' "$arithmetic_row" | sha256sum | awk '{print $1}')
    printf '%s\t%s\n' "$arithmetic_sha" "$arithmetic_row"
  done <"$phase6_exact_speedup_body"
} >"$phase6_exact_speedup_tsv"
rm -f "$phase6_exact_speedup_body"

if ! awk -F '\t' -v OFS='\t' \
    -v expected_keys_csv="$phase6_expected_rss_keys_csv" \
    -v expected_pair_count="$phase6_expected_rss_pair_count" '
  BEGIN {
    print "comparison_sha256","fixture","method","chart_top_k_exact",
               "search_semantic_sha256","output_semantic_sha256",
               "w1_peak_sampled_rss_max_kb","w8_peak_sampled_rss_max_kb",
               "w8_over_w1"
    n=split(expected_keys_csv, keys, ",")
    for (i=1; i<=n; ++i) if (keys[i] != "") expected[keys[i]]=1
  }
  NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
  $h["chart_top_k_exact"]+0>0 && $h["method"]!="sample_explore_merge" &&
      $h["status"]=="ok" && $h["validation_status"]=="ok" &&
      ($h["requested_workers"]==1 || $h["requested_workers"]==8) {
    g=$h["comparison_sha256"]
    if (!(g in expected)) next
    worker=$h["requested_workers"]
    rss=$h["peak_sampled_rss_kb"]
    if (rss !~ /^[1-9][0-9]*$/) {
      print "invalid Phase-6 peak RSS for " $h["row_id"] > "/dev/stderr"
      bad=1
      next
    }
    row_search=$h["search_semantic_sha256"]
    row_output=$h["output_semantic_sha256"]
    if (length(row_search)!=64 || row_search !~ /^[0-9a-f][0-9a-f]*$/ ||
        length(row_output)!=64 || row_output !~ /^[0-9a-f][0-9a-f]*$/) {
      print "invalid Phase-6 search/output semantic digest for " $h["row_id"] > "/dev/stderr"
      bad=1
      next
    }
    if (!(g in semantic_seen)) {
      semantic_seen[g]=1
      search_semantic[g]=row_search
      output_semantic[g]=row_output
    } else if (search_semantic[g]!=row_search || output_semantic[g]!=row_output) {
      semantic_drift[g]=1
    }
    if (!(g in detail)) detail[g]=$h["contract_sha256"]
    else if (detail[g]!=$h["contract_sha256"]) drift[g]=1
    seen[g,worker]=1
    if (rss+0>peak[g,worker]) peak[g,worker]=rss+0
    fixture[g]=$h["fixture"]
    method[g]=$h["method"]
    topk[g]=$h["chart_top_k_exact"]
  }
  END {
    for (g in expected) {
      if (!seen[g,1] || !seen[g,8]) {
        print "Phase-6 expected W1/W8 RSS pair is incomplete: " g > "/dev/stderr"
        bad=1
      }
    }
    for (g in fixture) {
      if (!(g in expected)) continue
      if (!seen[g,1] || !seen[g,8]) continue
      if (drift[g]) {
        print "Phase-6 W1/W8 workload contract drift for " fixture[g] "/" method[g] "/K" topk[g] > "/dev/stderr"
        bad=1
        continue
      }
      if (semantic_drift[g]) {
        print "Phase-6 W1/W8 search/output semantic drift for " fixture[g] "/" method[g] "/K" topk[g] > "/dev/stderr"
        bad=1
        continue
      }
      observed_pairs++
      ratio=peak[g,8]/peak[g,1]
      print g,fixture[g],method[g],topk[g],search_semantic[g],
            output_semantic[g],peak[g,1],peak[g,8],sprintf("%.9f",ratio)
      if (peak[g,8]>2.0*peak[g,1]) {
        print "Phase-6 W8/W1 peak RSS exceeds 2.0 for " fixture[g] "/" method[g] "/K" topk[g] > "/dev/stderr"
        bad=1
      }
    }
    if (observed_pairs+0 != expected_pair_count+0) {
      print "Phase-6 RSS evidence cardinality mismatch: expected " \
            expected_pair_count ", observed " (observed_pairs+0) > "/dev/stderr"
      bad=1
    }
    exit bad
  }
' "$phase6_admission_tsv" >"$phase6_rss_tsv"; then
  echo "gate failure: Phase-6 W8/W1 peak RSS" >&2
  gate_failures=$((gate_failures+1))
fi

if [[ -n "$run_manifest_group" ]]; then
  for row_id in "${selected_manifest_rows[@]}"; do
    actual_rows=$(awk -F '\t' -v id="$row_id" 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]==id{n++}END{print n+0}' "$raw_trials_tsv")
    [[ "$actual_rows" == "$repetitions" ]] || {
      echo "gate failure: selected manifest row $row_id executed $actual_rows/$repetitions recorded trials" >&2
      gate_failures=$((gate_failures+1))
    }
  done
  unexpected_rows=$(awk -F '\t' -v ids="$(IFS=,; echo "${selected_manifest_rows[*]}")" '
    BEGIN{n=split(ids,a,",");for(i=1;i<=n;i++)wanted[a[i]]=1}
    NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}!($h["row_id"] in wanted){bad++}END{print bad+0}' "$raw_trials_tsv")
  (( unexpected_rows == 0 )) || {
    echo "gate failure: manifest group emitted $unexpected_rows unselected rows" >&2
    gate_failures=$((gate_failures+1))
  }
fi
declare -A allowed_timeout=()
for row_id in "${expected_timeout_ids[@]}"; do
  [[ -n ${manifest_row_ids[$row_id]:-} ]] || fail "expected-timeout row is absent from manifest: $row_id"
  declared_status=$(for manifest in "${manifest_files[@]}"; do awk -F '\t' -v id="$row_id" '/^#/{next}!hd{for(i=1;i<=NF;i++)h[$i]=i;hd=1;next}$h["row_id"]==id{print $h["expected_outcome"]}' "$manifest"; done)
  [[ "$declared_status" == timeout ]] || fail "expected-timeout row is not manifest-labelled timeout: $row_id"
  [[ $(manifest_row_value "$row_id" expected_timeout_trials) == "$repetitions" ]] || \
    fail "expected-timeout count must equal repetitions for $row_id"
  allowed_timeout[$row_id]=1
done
while IFS=$'\t' read -r status validation row_id candidates exact; do
  if [[ "$status" == timeout && -n ${allowed_timeout[$row_id]:-} ]]; then continue; fi
  if [[ "$status" == expected_infeasible && "$validation" == not_applicable &&
        $(manifest_row_value "$row_id" expected_outcome) == expected_infeasible ]]; then continue; fi
  if [[ "$status" == scale_limit && "$validation" == ok &&
        $(manifest_row_value "$row_id" expected_outcome) == scale_limit ]]; then continue; fi
  if [[ "$status" != ok || "$validation" != ok ]]; then
    echo "gate failure: $row_id status=$status validation=$validation" >&2; gate_failures=$((gate_failures+1))
  fi
done < <(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}{print $h["status"],$h["validation_status"],$h["row_id"],$h["candidates_scored"],$h["exact_verifications"]}' OFS='\t' "$raw_trials_tsv")
while IFS=$'\t' read -r row_id rss_kb limit_bytes; do
  if [[ "$limit_bytes" != - ]] && ! awk -v rss="$rss_kb" -v limit="$limit_bytes" \
    'BEGIN{exit !(rss*1024<=limit)}'; then
    echo "gate failure: $row_id aggregate RSS ${rss_kb}KiB exceeds ${limit_bytes} bytes" >&2
    gate_failures=$((gate_failures+1))
  fi
done < <(awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}{print $h["row_id"],$h["peak_sampled_rss_kb"],$h["manifest_rss_limit_bytes"]}' OFS='\t' "$raw_trials_tsv")
for row_id in "${expected_timeout_ids[@]}"; do
  expected=$(for manifest in "${manifest_files[@]}"; do awk -F '\t' -v id="$row_id" '/^#/{next}!hd{for(i=1;i<=NF;i++)h[$i]=i;hd=1;next}$h["row_id"]==id{print $h["expected_timeout_trials"]}' "$manifest"; done)
  actual=$(awk -F '\t' -v id="$row_id" 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["row_id"]==id&&$h["status"]=="timeout"{n++}END{print n+0}' "$raw_trials_tsv")
  [[ "$actual" == "$expected" ]] || { echo "gate failure: $row_id expected $expected timeouts, got $actual" >&2; gate_failures=$((gate_failures+1)); }
done

worker_policy_tsv="$out_dir/worker_policy_comparisons.tsv"
printf 'requirement\tfixture\tmethod\ttrial_index\tdefault_wall_s\tauto_wall_s\tdefault_over_auto\tdefault_policy\tauto_policy\tresolved_workers\texecution_order\n' >"$worker_policy_tsv"
for requirement in "${worker_policy_requirements[@]}"; do
  required_default_policy=${requirement#*=}
  policy_pair_file="$out_dir/.worker-policy-pairs.$$.tsv"
  set +e
  awk -F '\t' -v OFS='\t' -v required="$required_default_policy" '
    NR==1 {for(i=1;i<=NF;i++)h[$i]=i; next}
    $h["method"]!="sample_explore_merge" && ($h["requested_workers"]=="default" || $h["requested_workers"]=="auto") {
      k=$h["fixture"] SUBSEP $h["method"] SUBSEP $h["trial_index"]
      wanted[k]=1
      if($h["requested_workers"]=="default") {
        dc[k]++; dw[k]=$h["wall_clock_s"]; dp[k]=$h["worker_policy"]
        dr[k]=$h["resolved_workers"]; ds[k]=$h["status"]; dv[k]=$h["validation_status"]
        dsearch[k]=$h["search_semantic_sha256"]; doutput[k]=$h["output_semantic_sha256"]
        dorder[k]=$h["execution_order"]
      } else {
        ac[k]++; aw[k]=$h["wall_clock_s"]; ap[k]=$h["worker_policy"]
        ar[k]=$h["resolved_workers"]; as[k]=$h["status"]; av[k]=$h["validation_status"]
        asearch[k]=$h["search_semantic_sha256"]; aoutput[k]=$h["output_semantic_sha256"]
        aorder[k]=$h["execution_order"]
      }
    }
    END {
      for(k in wanted) {
        split(k,p,SUBSEP)
        if(dc[k]!=1 || ac[k]!=1) {
          print "worker-policy pairing error for " p[1] "/" p[2] " trial " p[3] ": default=" dc[k] ", auto=" ac[k] > "/dev/stderr"; bad=1; continue
        }
        if(ds[k]!="ok" || dv[k]!="ok" || as[k]!="ok" || av[k]!="ok") {
          print "worker-policy pair is not successful/validated for " p[1] "/" p[2] " trial " p[3] > "/dev/stderr"; bad=1
        }
        if(dp[k]!=required || ap[k]!="automatic") {
          print "worker-policy labels differ for " p[1] "/" p[2] " trial " p[3] ": " dp[k] "/" ap[k] > "/dev/stderr"; bad=1
        }
        if(dr[k] !~ /^[1-9][0-9]*$/ || dr[k]!=ar[k]) {
          print "worker-policy resolved counts differ for " p[1] "/" p[2] " trial " p[3] ": " dr[k] "/" ar[k] > "/dev/stderr"; bad=1
        }
        if(dsearch[k] !~ /^[0-9a-f]{64}$/ || dsearch[k]!=asearch[k] ||
           doutput[k] !~ /^[0-9a-f]{64}$/ || doutput[k]!=aoutput[k]) {
          print "worker-policy semantics differ for " p[1] "/" p[2] " trial " p[3] > "/dev/stderr"; bad=1
        }
        if(dw[k] !~ /^[0-9]+([.][0-9]+)?$/ || aw[k] !~ /^[0-9]+([.][0-9]+)?$/ || aw[k]+0<=0 || dw[k]+0>1.10*(aw[k]+0)) {
          print "worker-policy per-trial wall gate failed for " p[1] "/" p[2] " trial " p[3] ": default=" dw[k] ", auto=" aw[k] > "/dev/stderr"; bad=1
        }
        if(dorder[k]!=aorder[k]) {
          print "worker-policy execution-order labels differ for " p[1] "/" p[2] " trial " p[3] > "/dev/stderr"; bad=1
        }
        print p[1],p[2],p[3],dw[k],aw[k],dw[k]/aw[k],dp[k],ap[k],dr[k],dorder[k]
        pairs++
      }
      if(pairs==0) {print "worker-policy gate matched no default/auto pairs" > "/dev/stderr"; bad=1}
      exit bad
    }' "$raw_trials_tsv" >"$policy_pair_file"
  policy_pair_status=$?
  set -e
  if (( policy_pair_status != 0 )); then
    echo "gate failure: default/explicit-auto worker-policy join for $requirement" >&2
    gate_failures=$((gate_failures+1))
    rm -f "$policy_pair_file"
    continue
  fi
  while IFS=$'\t' read -r fixture method trial default_wall auto_wall policy_ratio default_policy auto_policy resolved order; do
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%.9f\t%s\t%s\t%s\t%s\n' \
      "$requirement" "$fixture" "$method" "$trial" "$default_wall" "$auto_wall" \
      "$policy_ratio" "$default_policy" "$auto_policy" "$resolved" "$order" >>"$worker_policy_tsv"
  done <"$policy_pair_file"
  set +e
  awk -F '\t' -v reps="$repetitions" '
    {g=$1 SUBSEP $2; n[g]++; d[g,n[g]]=$4+0; a[g,n[g]]=$5+0}
    function med(kind,g,count, values,i,j,x) {
      for(i=1;i<=count;i++)values[i]=(kind=="d"?d[g,i]:a[g,i])
      for(i=2;i<=count;i++){x=values[i];j=i-1;while(j>=1&&values[j]>x){values[j+1]=values[j];j--}values[j+1]=x}
      return count%2?values[(count+1)/2]:(values[count/2]+values[count/2+1])/2
    }
    END {for(g in n) {
      if(n[g]!=reps){print "worker-policy pair count " n[g] ", expected " reps > "/dev/stderr";bad=1}
      dm=med("d",g,n[g]); am=med("a",g,n[g]);
      if(dm>1.10*am){print "worker-policy median wall gate failed: default=" dm ", auto=" am > "/dev/stderr";bad=1}
    } exit bad}' "$policy_pair_file"
  policy_median_status=$?
  set -e
  (( policy_median_status == 0 )) || {
    echo "gate failure: default worker-policy median exceeds explicit auto by more than 10%" >&2
    gate_failures=$((gate_failures+1))
  }
  rm -f "$policy_pair_file"
done

paired_ratios_tsv="$out_dir/paired_ratios.tsv"
printf 'requirement\tfixture\tmethod\trequested_workers\ttrial_index\tchart_wall_s\tnative_wall_s\tratio\texecution_order\n' >"$paired_ratios_tsv"
for requirement in "${wall_ratio_requirements[@]}"; do
  lhs=${requirement%%=*}; ratio=${requirement#*=}; method=${lhs%@*}; workers=${lhs##*@}
  [[ "$ratio" =~ ^[0-9]+([.][0-9]+)?$ ]] || fail "invalid wall ratio '$requirement'"
  pair_file="$out_dir/.pairs.$$.tsv"
  set +e
  awk -F '\t' -v OFS='\t' -v m="$method" -v w="$workers" '
    NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
    $h["method"]=="sample_explore_merge" {
      k=$h["fixture"] SUBSEP $h["trial_index"]
      nc[k]++; nw[k]=$h["wall_clock_s"]; no[k]=$h["execution_order"]
      ns[k]=$h["status"]; nv[k]=$h["validation_status"]
    }
    $h["method"]==m && $h["requested_workers"]==w {
      k=$h["fixture"] SUBSEP $h["trial_index"]
      cc[k]++; cw[k]=$h["wall_clock_s"]; co[k]=$h["execution_order"]
      cs[k]=$h["status"]; cv[k]=$h["validation_status"]; wanted[k]=1
    }
    END {
      for(k in wanted) {
        split(k,p,SUBSEP)
        if(cc[k]!=1 || nc[k]!=1 || cs[k]!="ok" || cv[k]!="ok" || ns[k]!="ok" || nv[k]!="ok" || co[k]!=no[k]) {
          print "pairing error for " p[1] " trial " p[2] ": chart=" cc[k] ", native=" nc[k] ", statuses=" cs[k] "/" ns[k] ", orders=" co[k] "/" no[k] > "/dev/stderr"; bad=1
        } else {
          print p[1],p[2],cw[k],nw[k],cw[k]/nw[k],co[k]
        }
      }
      if(length(wanted)==0) {print "no chart trials matched" > "/dev/stderr"; bad=1}
      exit bad
    }' "$raw_trials_tsv" >"$pair_file"
  pair_status=$?
  set -e
  if (( pair_status != 0 )); then
    echo "gate failure: incomplete/non-exact paired join for $requirement" >&2
    gate_failures=$((gate_failures+1)); rm -f "$pair_file"; continue
  fi
  while IFS=$'\t' read -r fixture trial chart native pair_ratio order; do
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.9f\t%s\n' \
      "$requirement" "$fixture" "$method" "$workers" "$trial" "$chart" "$native" "$pair_ratio" "$order" >>"$paired_ratios_tsv"
  done <"$pair_file"
  set +e
  awk -F '\t' -v reps="$repetitions" -v parity="$ratio" '
    {n[$1]++; if($5<=1.0)wins[$1]++; if($5>1.15)outlier[$1]=1}
    END{for(f in n) {
      need=int((n[f]+1)/2)
      if(n[f]!=reps){print f ": expected " reps " pairs, got " n[f] > "/dev/stderr";bad=1}
      if(parity<=1.0 && wins[f]<need){print f ": parity wins " wins[f] "/" n[f] ", need " need > "/dev/stderr";bad=1}
      if(parity<=1.0 && outlier[f]){print f ": a paired ratio exceeds 1.15" > "/dev/stderr";bad=1}
    } exit bad}' "$pair_file"
  pair_contract=$?
  set -e
  rm -f "$pair_file"
  if [[ "$pair_contract" != 0 ]]; then
    echo "gate failure: paired trial contract $requirement" >&2
    gate_failures=$((gate_failures+1))
  fi
  comparisons=$(awk -F '\t' -v m="$method" -v w="$workers" '
    NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
    $h["method"]=="sample_explore_merge"{native[$h["fixture"]]=$h["wall_clock_s"]}
    $h["method"]==m&&$h["requested_workers"]==w{chart[$h["fixture"]]=$h["wall_clock_s"]}
    END{for(f in chart)if(f in native)print f,chart[f],native[f]}' OFS='\t' "$summary_tsv")
  if [[ -z "$comparisons" ]]; then
    echo "gate failure: no rows matched wall ratio $requirement" >&2
    gate_failures=$((gate_failures+1))
    continue
  fi
  while IFS=$'\t' read -r fixture chart native; do
    awk -v c="$chart" -v n="$native" -v r="$ratio" 'BEGIN{exit !(c<=n*r)}' || {
      echo "gate failure: $fixture $method@$workers median $chart > native $native * $ratio" >&2; gate_failures=$((gate_failures+1)); }
  done <<<"$comparisons"
done
for requirement in "${rss_requirements[@]}"; do
  lhs=${requirement%%=*}; cap=${requirement#*=}; method=${lhs%@*}; workers=${lhs##*@}
  [[ "$cap" =~ ^[0-9]+$ ]] || fail "invalid RSS gate '$requirement'"
  awk -F '\t' -v m="$method" -v w="$workers" -v cap="$cap" '
    NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
    $h["method"]==m&&$h["requested_workers"]==w{seen=1;if($h["peak_sampled_rss_max_kb"]>cap)bad=1}
    END{exit !seen||bad}' "$summary_tsv" || { echo "gate failure: RSS $requirement" >&2; gate_failures=$((gate_failures+1)); }
done
awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}$h["canonical_digest"]=="MISMATCH"{print "gate failure: canonical mismatch for "$h["fixture"]"/"$h["method"]"@"$h["requested_workers"] > "/dev/stderr";bad=1}END{exit bad}' "$summary_tsv" || gate_failures=$((gate_failures+1))
awk -F '\t' '
  NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}
  $h["method"]!="sample_explore_merge" && $h["status"]=="ok" &&
      $h["validation_status"]=="ok" {g=$h["fixture"] SUBSEP $h["method"];
    semantic=$h["search_semantic_sha256"] SUBSEP $h["output_semantic_sha256"];
    if(!(g in digest))digest[g]=semantic;
    else if(digest[g]!=semantic){print "gate failure: 1-vs-N search/output semantic mismatch for "$h["fixture"]"/"$h["method"] > "/dev/stderr";bad=1}}
  END{exit bad}' "$summary_tsv" || gate_failures=$((gate_failures+1))
(( WARMUP_FAILURES == 0 )) || { echo "gate failure: $WARMUP_FAILURES warmup rows failed" >&2; gate_failures=$((gate_failures+1)); }

{
  echo "# WRIC chart-SPR search benchmark"; echo
  echo "benchmark_scope: CHART_SPR_PHASE0_SEARCH_COMPARISON"; echo
  echo "Configuration: iterations=$iterations, seed=$seed, max_moves=$max_moves, max_candidates=$max_candidates, top_k_exact=$top_k_exact, workers=${chart_worker_specs[*]}, warmups=$warmups, repetitions=$repetitions, timeout_seconds=$timeout_seconds, chart_memory_budget=${chart_memory_budget:-product-default}."; echo
  echo "| fixture | method | workers | status | validation | median_wall_s | max_wall_s | aggregate_peak_rss_kb | candidates | exact | canonical |"
  echo "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"
  awk -F '\t' 'NR==1{for(i=1;i<=NF;i++)h[$i]=i;next}{printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | `%s` |\n",$h["fixture"],$h["method"],$h["requested_workers"],$h["status"],$h["validation_status"],$h["wall_clock_s"],$h["wall_clock_max_s"],$h["peak_sampled_rss_max_kb"],$h["candidates_scored"],$h["exact_verifications"],$h["canonical_digest"]}' "$summary_tsv"
  echo; echo "Raw trials: $raw_trials_tsv"; echo "Aggregates: $summary_tsv"
  echo "Phase-6 admission evidence: $phase6_admission_tsv"
  echo "Phase-6 RSS comparisons: $phase6_rss_tsv"
  echo "Phase-6 exact-verification speedup: $phase6_exact_speedup_tsv"
  echo "Paired ratios: $paired_ratios_tsv"
  echo "Worker-policy comparisons: $worker_policy_tsv"; echo "Commands: $commands_log"
} >"$summary_md"
cat "$summary_md"

if (( smoke )); then
  rows=$(awk 'END{print NR-1}' "$summary_tsv")
  (( rows == 2 )) || { echo "smoke gate failure: expected exactly 2 aggregate rows, got $rows" >&2; gate_failures=$((gate_failures+1)); }
fi
(( gate_failures == 0 )) || exit 1

#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <dagutil>" >&2
  exit 2
fi

dagutil=$1
if [[ ! -x $dagutil ]]; then
  echo "dagutil is not executable: $dagutil" >&2
  exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Keep this CLI-contract test independent of the larger performance fixtures.
# The four-taxon input still generates and exact-verifies a real chart-SPR
# candidate, but completes quickly enough for the ordinary CTest suite.
common=(
  --fasta test/wric_binary_four.fa
  --newick test/wric_binary_four.nwk
  --refseq test/wric_binary_four.ref
  --validate
  --force-no-vcf
  --wric-polytomy-mode reject
  --wric-polytomy-max-exact-arity 6
  --wric-polytomy-max-shapes 1
  --wric-polytomy-max-productions 1024
  --wric-polytomy-max-clades 256
  --wric-lazy-chart off
  --chart-spr-search
  --chart-spr-max-candidates 1
  --chart-spr-top-k-exact 1
  --chart-spr-max-iterations 1
  --chart-spr-candidate-selection lower_bound_top_k
  --chart-spr-candidate-source grammar
  --chart-spr-sampled-tree-count 1
  --chart-spr-sampled-tree-radius 0
  --chart-spr-max-upward-path-expansions 0
  --chart-spr-max-path-pairs 0
  --chart-spr-min-moved-clade-size 1
  --chart-spr-max-moved-clade-size 0
  --chart-spr-min-target-clade-size 1
  --chart-spr-max-target-clade-size 0
  --chart-spr-max-cached-patterns 0
  --chart-spr-pattern-batch-size 0
  --chart-spr-candidate-batch-size 0
  --chart-spr-memory-budget 12884901888
  --chart-spr-commit-mode overlay_delta
  --chart-spr-verification-mode transient
  --chart-bnb-dominance off
  --seed 1
)

run_search() {
  local name=$1
  shift
  "$dagutil" "${common[@]}" "$@" \
    >"$tmp/$name.out" 2>"$tmp/$name.err"
}

# Read a scalar from exactly the two-space-indented chart_spr_search summary.
# Nested iteration/counter keys deliberately do not match this contract.
top_value() {
  local file=$1
  local key=$2
  awk -v wanted="  $key: " '
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

# Read a scalar from the single four-space-indented nested counters block.
# This deliberately rejects missing/duplicate fields and does not accept a
# top-level summary field as a substitute for the raw counter.
counter_value() {
  local file=$1
  local key=$2
  awk -v wanted="    $key: " '
    $0 == "  counters:" {
      in_counters = 1
      next
    }
    in_counters && /^  [^ ]/ { in_counters = 0 }
    in_counters && index($0, wanted) == 1 {
      count += 1
      value = substr($0, length(wanted) + 1)
    }
    END {
      if (count != 1) exit 1
      print value
    }
  ' "$file"
}

require_top_value() {
  local file=$1
  local key=$2
  local expected=$3
  local actual
  actual=$(top_value "$file" "$key") || {
    echo "missing or duplicate top-level '$key' in $file" >&2
    return 1
  }
  if [[ $actual != "$expected" ]]; then
    echo "$key: expected '$expected', got '$actual'" >&2
    return 1
  fi
}

require_nonnegative_decimal() {
  local file=$1
  local key=$2
  local value
  value=$(top_value "$file" "$key") || {
    echo "missing or duplicate top-level '$key' in $file" >&2
    return 1
  }
  if [[ ! $value =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "$key is not a finite nonnegative decimal: '$value'" >&2
    return 1
  fi
}

require_nonnegative_integer() {
  local file=$1
  local key=$2
  local value
  value=$(top_value "$file" "$key") || {
    echo "missing or duplicate top-level '$key' in $file" >&2
    return 1
  }
  if [[ ! $value =~ ^[0-9]+$ ]]; then
    echo "$key is not a nonnegative integer: '$value'" >&2
    return 1
  fi
}

require_scheduler_contract() {
  local file=$1
  local key
  for key in chart_workers_actually_active_high_water \
             chart_scheduler_operations \
             chart_scheduler_parallel_operations \
             chart_scheduler_ranges_created \
             chart_scheduler_ranges_completed \
             chart_scheduler_ranges_cancelled \
             chart_scheduler_tasks_submitted \
             chart_scheduler_tasks_completed \
             chart_scheduler_tasks_joined \
             chart_scheduler_pending_tasks \
             chart_scheduler_pending_tasks_at_shutdown \
             chart_scheduler_minimum_effective_grain \
             chart_scheduler_maximum_effective_grain \
             chart_scheduler_last_effective_grain \
             chart_scheduler_queue_wait_nanoseconds \
             chart_scheduler_queue_wait_nanoseconds_max \
             chart_scheduler_queue_wait_samples \
             chart_scheduler_last_active_workers \
             chart_scheduler_serial_fallbacks \
             chart_scheduler_nested_serial_fallbacks \
             chart_scheduler_rejected_concurrent_operations \
             chart_scheduler_pool_lifetimes \
             chart_scheduler_pool_lifetimes_stopped \
             chart_scheduler_live_pool_threads; do
    require_nonnegative_integer "$file" "$key"
  done

  local operations parallel serial
  local ranges completed cancelled
  local submitted tasks_completed joined wait_samples
  local wait_total wait_max
  local pools stopped
  local minimum_grain maximum_grain last_grain
  operations=$(top_value "$file" chart_scheduler_operations)
  parallel=$(top_value "$file" chart_scheduler_parallel_operations)
  serial=$(top_value "$file" chart_scheduler_serial_fallbacks)
  ranges=$(top_value "$file" chart_scheduler_ranges_created)
  completed=$(top_value "$file" chart_scheduler_ranges_completed)
  cancelled=$(top_value "$file" chart_scheduler_ranges_cancelled)
  submitted=$(top_value "$file" chart_scheduler_tasks_submitted)
  tasks_completed=$(top_value "$file" chart_scheduler_tasks_completed)
  joined=$(top_value "$file" chart_scheduler_tasks_joined)
  wait_samples=$(top_value "$file" chart_scheduler_queue_wait_samples)
  wait_total=$(top_value "$file" chart_scheduler_queue_wait_nanoseconds)
  wait_max=$(top_value "$file" chart_scheduler_queue_wait_nanoseconds_max)
  pools=$(top_value "$file" chart_scheduler_pool_lifetimes)
  stopped=$(top_value "$file" chart_scheduler_pool_lifetimes_stopped)
  minimum_grain=$(top_value "$file" chart_scheduler_minimum_effective_grain)
  maximum_grain=$(top_value "$file" chart_scheduler_maximum_effective_grain)
  last_grain=$(top_value "$file" chart_scheduler_last_effective_grain)

  if (( operations != parallel + serial )); then
    echo "scheduler operation accounting mismatch in $file" >&2
    return 1
  fi
  if (( ranges != completed + cancelled )); then
    echo "scheduler range accounting mismatch in $file" >&2
    return 1
  fi
  if (( submitted != tasks_completed || submitted != joined ||
        submitted != wait_samples )); then
    echo "scheduler task accounting mismatch in $file" >&2
    return 1
  fi
  if (( wait_total < wait_max )); then
    echo "scheduler queue-wait accounting mismatch in $file" >&2
    return 1
  fi
  if (( pools != stopped )); then
    echo "scheduler pool lifetime mismatch in $file" >&2
    return 1
  fi
  if (( ranges > 0 &&
        (minimum_grain == 0 || minimum_grain > last_grain ||
         last_grain > maximum_grain) )); then
    echo "scheduler grain accounting mismatch in $file" >&2
    return 1
  fi
  require_top_value "$file" chart_scheduler_pending_tasks 0
  require_top_value "$file" chart_scheduler_pending_tasks_at_shutdown 0
  require_top_value "$file" chart_scheduler_live_pool_threads 0
  require_top_value "$file" chart_scheduler_shutdown true
}

default_out=$tmp/default.out
run_search default
require_top_value "$default_out" chart_workers_requested 0
require_top_value "$default_out" chart_worker_policy automatic_default
default_resolved=$(top_value "$default_out" chart_workers_resolved)
if [[ ! $default_resolved =~ ^[1-9][0-9]*$ ]]; then
  echo "default chart worker count did not resolve positively: '$default_resolved'" >&2
  exit 1
fi
require_top_value "$default_out" local_score_workers "$default_resolved"
default_resolution_policy=$(top_value "$default_out" chart_worker_resolution_policy)
case "$default_resolution_policy" in
  affinity_physical_cores|affinity_logical_cpus|hardware_concurrency|serial_fallback)
    ;;
  *)
    echo "unexpected default chart worker resolution policy: '$default_resolution_policy'" >&2
    exit 1
    ;;
esac
require_scheduler_contract "$default_out"

# Explicit one-worker execution remains the serial semantic oracle and
# compatibility path after omitted workers become automatic.
serial_out=$tmp/serial.out
run_search serial --chart-spr-workers 1
require_top_value "$serial_out" chart_workers_requested 1
require_top_value "$serial_out" chart_workers_resolved 1
require_top_value "$serial_out" chart_worker_policy explicit
require_top_value "$serial_out" local_score_workers 1
require_top_value "$serial_out" chart_worker_resolution_policy explicit
require_scheduler_contract "$serial_out"

# Every workload-defining manifest field is repeated by the real product
# report.  These checks complement the synthetic manifest test: a fake CLI
# accepting an option is not evidence that dagutil preserves and reports it.
while IFS= read -r contract; do
  key=${contract%%\\t*}
  expected=${contract#*\\t}
  require_top_value "$default_out" "$key" "$expected"
done <<'REPORT_CONTRACT'
acceptance\texact_multisite
objective\tgrammar_exact
candidate_selection\tlower_bound_top_k
candidate_source\tgrammar
candidate_cap_semantics\tpost-dedup
topology_selector\tnone
requested_max_iterations\t1
seed\t1
configured_max_candidates\t1
top_k_exact_verify\t1
randomize_order\tfalse
reservoir_sample\tfalse
include_immediate_reversals\tfalse
sampled_tree_count\t1
sampled_tree_radius\t0
sampled_tree_score_threshold\t2147483647
max_upward_path_expansions\t0
max_path_pairs\t0
min_moved_clade_size\t1
max_moved_clade_size\t0
min_target_clade_size\t1
max_target_clade_size\t0
max_affected_clades\t0
polytomy_mode\treject
polytomy_max_exact_arity\t6
polytomy_max_shapes\t1
polytomy_max_productions\t1024
polytomy_max_clades\t256
lazy_policy\toff
max_cached_patterns\t0
configured_pattern_batch_size\t0
configured_candidate_batch_size\t0
memory_budget_bytes\t12884901888
commit_mode\toverlay_delta
verification_mode\ttransient
local_accept_updates\tfalse
dominance_mode\toff
bound_pruning\ttrue
require_exact_keep_mask\ttrue
max_frontier_entries\t0
score_ua_edge\tfalse
validate\ttrue
force_no_vcf\ttrue
REPORT_CONTRACT

# A real finite, forced-lazy exact search reports the temporal admission
# envelope at both public surfaces.  Each stable name occurs exactly once in
# the top-level summary and once in the nested raw-counter block, and both
# surfaces carry the same nonzero value.
finite_lazy_out=$tmp/finite_lazy.out
run_search finite_lazy --wric-lazy-chart on \
  --chart-spr-max-candidates 2 --chart-spr-top-k-exact 2 \
  --chart-spr-canonical-result "$tmp/finite_lazy.json"
temporal_keys=(
  lazy_local_iteration_generation_phase_bytes_max
  lazy_local_iteration_evidence_phase_bytes_max
  lazy_local_ranked_candidate_exact_evidence_bytes_max
)
for key in "${temporal_keys[@]}"; do
  summary_value=$(top_value "$finite_lazy_out" "$key") || {
    echo "missing or duplicate top-level '$key' in $finite_lazy_out" >&2
    exit 1
  }
  raw_value=$(counter_value "$finite_lazy_out" "$key") || {
    echo "missing or duplicate nested counter '$key' in $finite_lazy_out" >&2
    exit 1
  }
  if [[ ! $summary_value =~ ^[1-9][0-9]*$ ]]; then
    echo "top-level $key is not a positive integer: '$summary_value'" >&2
    exit 1
  fi
  if [[ $summary_value != "$raw_value" ]]; then
    echo "$key differs between summary ($summary_value) and counters ($raw_value)" >&2
    exit 1
  fi
done

finite_envelope=$(top_value "$finite_lazy_out" \
  lazy_local_iteration_envelope_bytes_max)
finite_generation=$(top_value "$finite_lazy_out" \
  lazy_local_iteration_generation_phase_bytes_max)
finite_evidence=$(top_value "$finite_lazy_out" \
  lazy_local_iteration_evidence_phase_bytes_max)
finite_ranked=$(top_value "$finite_lazy_out" \
  lazy_local_ranked_candidate_exact_evidence_bytes_max)
finite_expected=$finite_generation
if (( finite_evidence > finite_expected )); then
  finite_expected=$finite_evidence
fi
if (( finite_envelope != finite_expected )); then
  echo "finite lazy envelope $finite_envelope != max($finite_generation, $finite_evidence)" >&2
  exit 1
fi
if (( finite_evidence < finite_ranked )); then
  echo "finite lazy evidence phase $finite_evidence does not contain ranked evidence $finite_ranked" >&2
  exit 1
fi

# The unified option is the authoritative forward-looking budget.
unified_out=$tmp/unified.out
run_search unified --chart-spr-workers 2
require_top_value "$unified_out" chart_workers_requested 2
require_top_value "$unified_out" chart_workers_resolved 2
require_top_value "$unified_out" chart_worker_policy explicit
require_top_value "$unified_out" local_score_workers 2
require_top_value "$unified_out" chart_worker_resolution_policy explicit
require_scheduler_contract "$unified_out"

# The old local-score option remains a labelled compatibility alias.
legacy_out=$tmp/legacy.out
run_search legacy --chart-spr-local-score-workers 2
require_top_value "$legacy_out" chart_workers_requested 2
require_top_value "$legacy_out" chart_workers_resolved 2
require_top_value "$legacy_out" chart_worker_policy legacy_explicit
require_top_value "$legacy_out" local_score_workers 2
require_top_value "$legacy_out" chart_worker_resolution_policy explicit
require_scheduler_contract "$legacy_out"

# Numeric zero means automatic resolution, and the resolved value is both
# positive and the budget seen by the currently parallel local-score phase.
auto_out=$tmp/auto.out
run_search auto --chart-spr-workers 0
require_top_value "$auto_out" chart_workers_requested 0
require_top_value "$auto_out" chart_worker_policy automatic
auto_resolved=$(top_value "$auto_out" chart_workers_resolved)
if [[ ! $auto_resolved =~ ^[1-9][0-9]*$ ]]; then
  echo "automatic chart worker count did not resolve positively: '$auto_resolved'" >&2
  exit 1
fi
require_top_value "$auto_out" local_score_workers "$auto_resolved"
auto_resolution_policy=$(top_value "$auto_out" chart_worker_resolution_policy)
case "$auto_resolution_policy" in
  affinity_physical_cores|affinity_logical_cpus|hardware_concurrency|serial_fallback)
    ;;
  *)
    echo "unexpected automatic chart worker resolution policy: '$auto_resolution_policy'" >&2
    exit 1
    ;;
esac
require_scheduler_contract "$auto_out"

# Supplying both spellings is never silently order-dependent.
set +e
"$dagutil" "${common[@]}" \
  --chart-spr-workers 2 \
  --chart-spr-local-score-workers 2 \
  >"$tmp/conflict.out" 2>"$tmp/conflict.err"
conflict_status=$?
set -e
if (( conflict_status == 0 )); then
  echo "unified/legacy worker option conflict unexpectedly succeeded" >&2
  exit 1
fi
if ! grep -qi 'conflict' "$tmp/conflict.err" ||
   ! grep -q -- '--chart-spr-workers' "$tmp/conflict.err" ||
   ! grep -q -- '--chart-spr-local-score-workers' "$tmp/conflict.err"; then
  echo "worker-option conflict was not labelled with both option names" >&2
  cat "$tmp/conflict.err" >&2
  exit 1
fi

# Phase-0 timing fields are diagnostic, finite, nonnegative, and internally
# consistent. This run has exactly one exact candidate. Its per-candidate
# verifier values agree, while the aggregate exact phase additionally includes
# selection, estimation, admission, and post-join aggregation and therefore
# must only envelope the verifier timing.
for key in cache_build_ms initial_chart_construction_ms \
           candidate_generation_ms exact_initialization_ms \
           local_scoring_ms exact_verification_ms \
           materialization_ms materialization_exact_verification_ms \
           materialization_accepted_update_ms \
           materialization_final_compaction_ms \
           exact_candidate_verification_ms_min \
           exact_candidate_verification_ms_mean \
           exact_candidate_verification_ms_max total_ms; do
  require_nonnegative_decimal "$default_out" "$key"
done
for key in candidates_generated exact_verifications \
           exact_candidate_timing_count peak_concurrent_exact_verifiers; do
  require_nonnegative_integer "$default_out" "$key"
done

candidates_generated=$(top_value "$default_out" candidates_generated)
exact_verifications=$(top_value "$default_out" exact_verifications)
timing_count=$(top_value "$default_out" exact_candidate_timing_count)
peak_exact=$(top_value "$default_out" peak_concurrent_exact_verifiers)
if (( candidates_generated < 1 || exact_verifications < 1 )); then
  echo "small observability fixture did not exercise a real exact candidate" >&2
  exit 1
fi
if (( peak_exact < 1 )); then
  echo "real exact verification reported zero verifier concurrency" >&2
  exit 1
fi
if [[ $timing_count != "$exact_verifications" ]]; then
  echo "per-candidate timing count $timing_count != exact verifications $exact_verifications" >&2
  exit 1
fi

per_candidate_line=$(awk '
  index($0, "      exact_candidate_verification_ms: ") == 1 {
    count += 1
    value = substr($0, length("      exact_candidate_verification_ms: ") + 1)
  }
  END {
    if (count != 1) exit 1
    print value
  }
' "$default_out") || {
  echo "missing or duplicate per-iteration exact-candidate timing list" >&2
  exit 1
}

if [[ ! $per_candidate_line =~ ^\[([0-9]+([.][0-9]+)?)(,[[:space:]]*[0-9]+([.][0-9]+)?)*\]$ ]]; then
  echo "malformed exact-candidate timing list: '$per_candidate_line'" >&2
  exit 1
fi

timing_csv=${per_candidate_line:1:${#per_candidate_line}-2}
IFS=, read -r -a candidate_timings <<<"$timing_csv"
if (( ${#candidate_timings[@]} != timing_count )); then
  echo "timing list length ${#candidate_timings[@]} != reported count $timing_count" >&2
  exit 1
fi

cache_ms=$(top_value "$default_out" cache_build_ms)
initial_chart_ms=$(top_value "$default_out" initial_chart_construction_ms)
generation_ms=$(top_value "$default_out" candidate_generation_ms)
exact_init_ms=$(top_value "$default_out" exact_initialization_ms)
exact_total_ms=$(top_value "$default_out" exact_verification_ms)
exact_min_ms=$(top_value "$default_out" exact_candidate_verification_ms_min)
exact_mean_ms=$(top_value "$default_out" exact_candidate_verification_ms_mean)
exact_max_ms=$(top_value "$default_out" exact_candidate_verification_ms_max)
total_ms=$(top_value "$default_out" total_ms)
materialization_ms=$(top_value "$default_out" materialization_ms)
materialization_exact_ms=$(top_value "$default_out" materialization_exact_verification_ms)
materialization_accepted_ms=$(top_value "$default_out" materialization_accepted_update_ms)
materialization_final_ms=$(top_value "$default_out" materialization_final_compaction_ms)

awk -v cache="$cache_ms" -v initial_chart="$initial_chart_ms" \
    -v generation="$generation_ms" \
    -v initialization="$exact_init_ms" -v exact_total="$exact_total_ms" \
    -v exact_min="$exact_min_ms" -v exact_mean="$exact_mean_ms" \
    -v exact_max="$exact_max_ms" -v total="$total_ms" \
    -v materialization="$materialization_ms" \
    -v materialization_exact="$materialization_exact_ms" \
    -v materialization_accepted="$materialization_accepted_ms" \
    -v materialization_final="$materialization_final_ms" \
    -v candidate="${candidate_timings[0]}" '
  function abs(x) { return x < 0 ? -x : x }
  BEGIN {
    tolerance = 0.011
    if (initial_chart > cache + tolerance) exit 6
    if (initialization > cache + tolerance) exit 1
    if (generation > total + tolerance) exit 2
    if (exact_total > total + tolerance) exit 3
    if (exact_min > exact_mean + tolerance ||
        exact_mean > exact_max + tolerance) exit 4
    if (abs(exact_min - candidate) > tolerance ||
        abs(exact_mean - candidate) > tolerance ||
        abs(exact_max - candidate) > tolerance ||
        candidate > exact_total + tolerance) exit 5
    if (materialization > total + tolerance) exit 7
    if (abs(materialization - (materialization_exact + materialization_accepted + materialization_final)) > tolerance) exit 8
  }
' || {
  echo "Phase-0 timing aggregates are internally inconsistent" >&2
  exit 1
}

# The same real product path reports an observed peak of zero when the
# acceptance policy performs no exact verification.
no_exact_out=$tmp/no_exact.out
run_search no_exact --chart-spr-acceptance lower-bound
require_top_value "$no_exact_out" exact_verifications 0
require_top_value "$no_exact_out" peak_concurrent_exact_verifiers 0

iteration_generation_ms=$(awk '
  index($0, "      candidate_generation_ms: ") == 1 {
    count += 1
    value = substr($0, length("      candidate_generation_ms: ") + 1)
  }
  END {
    if (count != 1) exit 1
    print value
  }
' "$default_out") || {
  echo "missing or duplicate per-iteration candidate generation timing" >&2
  exit 1
}
awk -v summary="$generation_ms" -v iteration="$iteration_generation_ms" '
  function abs(x) { return x < 0 ? -x : x }
  BEGIN { exit !(abs(summary - iteration) <= 0.011) }
' || {
  echo "summary/iteration candidate-generation timings disagree" >&2
  exit 1
}

echo "dagutil_chart_spr_phase0_observability: PASS"

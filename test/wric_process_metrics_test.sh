#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <wric-process-metrics>" >&2
  exit 2
fi

runner=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

metric() {
  local file=$1
  local key=$2
  awk -F= -v wanted="$key" '
    $1 == wanted { count += 1; value = substr($0, index($0, "=") + 1) }
    END {
      if (count != 1) exit 1
      print value
    }
  ' "$file"
}

require_metric() {
  local file=$1
  local key=$2
  local expected=$3
  local actual
  actual=$(metric "$file" "$key") || {
    echo "missing or duplicate metric '$key' in $file" >&2
    return 1
  }
  if [[ $actual != "$expected" ]]; then
    echo "metric $key: expected '$expected', got '$actual'" >&2
    return 1
  fi
}

run_and_capture_status() {
  local status_file=$1
  shift
  set +e
  "$@"
  local status=$?
  set -e
  printf '%s\n' "$status" >"$status_file"
}

# A normal nonzero exit must be returned unchanged while child output remains
# separate from the machine-readable metrics stream.
run_and_capture_status "$tmp/normal.status" \
  "$runner" \
    --stdout "$tmp/normal.out" \
    --stderr "$tmp/normal.err" \
    -- /bin/sh -c 'printf child-stdout; printf child-stderr >&2; exit 7' \
    >"$tmp/normal.metrics"
[[ $(<"$tmp/normal.status") == 7 ]]
[[ $(<"$tmp/normal.out") == child-stdout ]]
[[ $(<"$tmp/normal.err") == child-stderr ]]
require_metric "$tmp/normal.metrics" schema_version 2
require_metric "$tmp/normal.metrics" outcome exited
require_metric "$tmp/normal.metrics" exit_code 7
require_metric "$tmp/normal.metrics" term_signal 0
require_metric "$tmp/normal.metrics" timed_out 0
require_metric "$tmp/normal.metrics" runner_exit_code 7
require_metric "$tmp/normal.metrics" wait4_collected 1
require_metric "$tmp/normal.metrics" rss_kb_unit 1024_bytes
require_metric "$tmp/normal.metrics" rss_limit_bytes 0
require_metric "$tmp/normal.metrics" rss_limit_enabled 0
require_metric "$tmp/normal.metrics" rss_limit_observed 0
require_metric "$tmp/normal.metrics" rss_limit_exceeded 0
require_metric "$tmp/normal.metrics" rss_limit_trigger_bytes 0
require_metric "$tmp/normal.metrics" rss_limit_term_sent 0
require_metric "$tmp/normal.metrics" rss_limit_kill_sent 0
require_metric "$tmp/normal.metrics" subreaper_enabled 1
require_metric "$tmp/normal.metrics" live_descendants_at_return 0
require_metric "$tmp/normal.metrics" process_group_alive_at_return 0
require_metric "$tmp/normal.metrics" wait4_echild_at_return 1
require_metric "$tmp/normal.metrics" monitor_error 0
require_metric "$tmp/normal.metrics" monitor_error_count 0

# Reaping order is not lifecycle evidence. Deterministically wait until the
# child is Z before the leader exits; wait4(-1) may then report the leader
# first. A fully dead adopted chain is synchronously reaped, never classified
# as a leak or allowed to cross into timeout classification.
zombie_program='
import os, time
child = os.fork()
if child == 0:
    os._exit(0)
deadline = time.monotonic() + 2
while True:
    with open(f"/proc/{child}/stat", encoding="ascii") as status:
        line = status.read()
    if line[line.rfind(") ") + 2] == "Z":
        break
    if time.monotonic() >= deadline:
        raise RuntimeError("child did not become a zombie")
    time.sleep(.001)
os._exit(0)
'
for iteration in $(seq 1 50); do
  zombie_metrics="$tmp/reaped-zombie-$iteration.metrics"
  "$runner" \
    --timeout-seconds 1 \
    --stdout "$tmp/reaped-zombie-$iteration.out" \
    --stderr "$tmp/reaped-zombie-$iteration.err" \
    -- python3 -c "$zombie_program" \
    >"$zombie_metrics"
  require_metric "$zombie_metrics" outcome exited
  require_metric "$zombie_metrics" runner_exit_code 0
  require_metric "$zombie_metrics" descendants_reaped 1
  require_metric "$zombie_metrics" post_leader_descendants 0
  require_metric "$zombie_metrics" live_descendants_at_return 0
  require_metric "$zombie_metrics" wait4_echild_at_return 1
done

# Reaping one direct zombie can expose another adopted generation. The common
# drain must follow the cascade through ECHILD without treating either already-
# dead process as a live escape.
zombie_chain_program='
import os, time
def wait_zombie(pid):
    deadline = time.monotonic() + 2
    while True:
        with open(f"/proc/{pid}/stat", encoding="ascii") as status:
            line = status.read()
        if line[line.rfind(") ") + 2] == "Z":
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(f"{pid} did not become a zombie")
        time.sleep(.001)
child = os.fork()
if child == 0:
    grandchild = os.fork()
    if grandchild == 0:
        os._exit(0)
    wait_zombie(grandchild)
    os._exit(0)
wait_zombie(child)
os._exit(0)
'
"$runner" \
  --timeout-seconds 1 \
  --stdout "$tmp/zombie-chain.out" \
  --stderr "$tmp/zombie-chain.err" \
  -- python3 -c "$zombie_chain_program" \
  >"$tmp/zombie-chain.metrics"
require_metric "$tmp/zombie-chain.metrics" outcome exited
require_metric "$tmp/zombie-chain.metrics" runner_exit_code 0
require_metric "$tmp/zombie-chain.metrics" descendants_reaped 2
require_metric "$tmp/zombie-chain.metrics" post_leader_descendants 0
require_metric "$tmp/zombie-chain.metrics" monitor_error 0
require_metric "$tmp/zombie-chain.metrics" live_descendants_at_return 0
require_metric "$tmp/zombie-chain.metrics" wait4_echild_at_return 1

# The wrapper establishes a waitable SIGCHLD disposition even if its caller
# ignored SIGCHLD/SA_NOCLDWAIT, while restoring the inherited disposition in
# the command child before exec.
python3 -c '
import os, signal, sys
signal.signal(signal.SIGCHLD, signal.SIG_IGN)
os.execv(sys.argv[1], [sys.argv[1], "--stdout", sys.argv[2], "--stderr",
                      sys.argv[3], "--", "/bin/sh", "-c", "exit 0"])
' "$runner" "$tmp/ignored-sigchld.out" "$tmp/ignored-sigchld.err" \
  >"$tmp/ignored-sigchld.metrics"
require_metric "$tmp/ignored-sigchld.metrics" outcome exited
require_metric "$tmp/ignored-sigchld.metrics" runner_exit_code 0
require_metric "$tmp/ignored-sigchld.metrics" wait4_collected 1
require_metric "$tmp/ignored-sigchld.metrics" wait4_echild_at_return 1

# Rapidly created and normally reaped grandchildren exercise procfs identity
# races without permitting false monitor/leak outcomes.
for iteration in $(seq 1 10); do
  "$runner" \
    --timeout-seconds 2 \
    --stdout "$tmp/fork-stress-$iteration.out" \
    --stderr "$tmp/fork-stress-$iteration.err" \
    -- python3 -c \
       'import subprocess; [subprocess.run(["/bin/true"], check=True) for _ in range(40)]' \
    >"$tmp/fork-stress-$iteration.metrics"
  require_metric "$tmp/fork-stress-$iteration.metrics" outcome exited
  require_metric "$tmp/fork-stress-$iteration.metrics" monitor_error 0
  require_metric "$tmp/fork-stress-$iteration.metrics" live_descendants_at_return 0
  require_metric "$tmp/fork-stress-$iteration.metrics" wait4_echild_at_return 1
done

# Reusing the sampled tree for steady-state liveness must still close the
# sample-to-wait4 fork/reparent window. Repeatedly fork a setsid descendant at
# the leader's exit boundary; once wait4 collects the leader, the runner must
# refresh the adopted tree, classify the live escape, and join it.
late_fork_program='
import os, sys, time
ready_read, ready_write = os.pipe()
child = os.fork()
if child == 0:
    os.close(ready_read)
    os.setsid()
    with open(sys.argv[1], "w", encoding="ascii") as output:
        output.write(str(os.getpid()))
    os.write(ready_write, b"1")
    os.close(ready_write)
    time.sleep(10)
    os._exit(0)
os.close(ready_write)
if os.read(ready_read, 1) != b"1":
    raise RuntimeError("late-fork child did not become ready")
os.close(ready_read)
os._exit(0)
'
for iteration in $(seq 1 20); do
  run_and_capture_status "$tmp/late-fork-$iteration.status" \
    "$runner" \
      --timeout-seconds 1 \
      --stdout "$tmp/late-fork-$iteration.out" \
      --stderr "$tmp/late-fork-$iteration.err" \
      -- python3 -c "$late_fork_program" "$tmp/late-fork-$iteration.pid" \
      >"$tmp/late-fork-$iteration.metrics"
  [[ $(<"$tmp/late-fork-$iteration.status") == 125 ]]
  require_metric "$tmp/late-fork-$iteration.metrics" outcome descendant_leak
  require_metric "$tmp/late-fork-$iteration.metrics" monitor_error 0
  require_metric "$tmp/late-fork-$iteration.metrics" live_descendants_at_return 0
  require_metric "$tmp/late-fork-$iteration.metrics" process_group_alive_at_return 0
  require_metric "$tmp/late-fork-$iteration.metrics" wait4_echild_at_return 1
  (( $(metric "$tmp/late-fork-$iteration.metrics" post_leader_descendants) >= 1 ))
  if kill -0 "$(<"$tmp/late-fork-$iteration.pid")" 2>/dev/null; then
    echo "late-fork descendant survived runner return" >&2
    exit 1
  fi
done

# A configured cap is part of the metrics contract even when it is not reached.
"$runner" \
  --rss-limit-bytes 134217728 \
  --stdout "$tmp/nonexceed.out" \
  --stderr "$tmp/nonexceed.err" \
  -- /bin/sh -c 'exit 0' \
  >"$tmp/nonexceed.metrics"
require_metric "$tmp/nonexceed.metrics" outcome exited
require_metric "$tmp/nonexceed.metrics" runner_exit_code 0
require_metric "$tmp/nonexceed.metrics" rss_limit_bytes 134217728
require_metric "$tmp/nonexceed.metrics" rss_limit_enabled 1
require_metric "$tmp/nonexceed.metrics" rss_limit_observed 0
require_metric "$tmp/nonexceed.metrics" rss_limit_exceeded 0
require_metric "$tmp/nonexceed.metrics" rss_limit_trigger_bytes 0
require_metric "$tmp/nonexceed.metrics" rss_limit_term_sent 0
require_metric "$tmp/nonexceed.metrics" rss_limit_kill_sent 0

# A child signal is labelled explicitly and returned with the conventional
# 128+signal shell status.
run_and_capture_status "$tmp/signal.status" \
  "$runner" \
    --stdout "$tmp/signal.out" \
    --stderr "$tmp/signal.err" \
    -- /bin/sh -c 'kill -TERM "$$"' \
    >"$tmp/signal.metrics"
[[ $(<"$tmp/signal.status") == 143 ]]
require_metric "$tmp/signal.metrics" outcome signaled
require_metric "$tmp/signal.metrics" exit_code -1
require_metric "$tmp/signal.metrics" term_signal 15
require_metric "$tmp/signal.metrics" timed_out 0
require_metric "$tmp/signal.metrics" runner_exit_code 143

# A timeout gets its own outcome/status even though the raw child status is a
# signal. The stubborn process exercises the TERM grace and KILL/join path.
run_and_capture_status "$tmp/timeout.status" \
  "$runner" \
    --timeout-seconds 0.05 \
    --rss-limit-bytes 1073741824 \
    --stdout "$tmp/timeout.out" \
    --stderr "$tmp/timeout.err" \
    -- /bin/sh -c 'trap "" TERM; echo "$$" >"$1"; exec /bin/sleep 10' \
       wric-timeout "$tmp/timeout.pid" \
    >"$tmp/timeout.metrics"
[[ $(<"$tmp/timeout.status") == 124 ]]
require_metric "$tmp/timeout.metrics" outcome timeout
require_metric "$tmp/timeout.metrics" exit_code -1
require_metric "$tmp/timeout.metrics" term_signal 9
require_metric "$tmp/timeout.metrics" timed_out 1
require_metric "$tmp/timeout.metrics" runner_exit_code 124
require_metric "$tmp/timeout.metrics" timeout_term_sent 1
require_metric "$tmp/timeout.metrics" timeout_kill_sent 1
require_metric "$tmp/timeout.metrics" rss_limit_bytes 1073741824
require_metric "$tmp/timeout.metrics" rss_limit_enabled 1
require_metric "$tmp/timeout.metrics" rss_limit_observed 0
require_metric "$tmp/timeout.metrics" rss_limit_exceeded 0
require_metric "$tmp/timeout.metrics" rss_limit_trigger_bytes 0
require_metric "$tmp/timeout.metrics" rss_limit_term_sent 0
require_metric "$tmp/timeout.metrics" rss_limit_kill_sent 0
if kill -0 "$(<"$tmp/timeout.pid")" 2>/dev/null; then
  echo "timed-out child process was not killed and joined" >&2
  exit 1
fi

# A timeout also kills and joins a descendant that escaped the leader process
# group with setsid and ignored TERM; cleanup is tree-based, not PGID-only.
run_and_capture_status "$tmp/timeout-setsid.status" \
  "$runner" \
    --timeout-seconds 0.05 \
    --stdout "$tmp/timeout-setsid.out" \
    --stderr "$tmp/timeout-setsid.err" \
    -- /bin/sh -c \
       'python3 -c "import os,signal,time; os.setsid(); signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(10)" & helper=$!; echo "$helper" >"$1"; wait "$helper"' \
       wric-timeout-setsid "$tmp/timeout-setsid.pid" \
    >"$tmp/timeout-setsid.metrics"
[[ $(<"$tmp/timeout-setsid.status") == 124 ]]
require_metric "$tmp/timeout-setsid.metrics" outcome timeout
require_metric "$tmp/timeout-setsid.metrics" timeout_term_sent 1
require_metric "$tmp/timeout-setsid.metrics" timeout_kill_sent 1
require_metric "$tmp/timeout-setsid.metrics" live_descendants_at_return 0
require_metric "$tmp/timeout-setsid.metrics" process_group_alive_at_return 0
require_metric "$tmp/timeout-setsid.metrics" wait4_echild_at_return 1
if kill -0 "$(<"$tmp/timeout-setsid.pid")" 2>/dev/null; then
  echo "setsid timeout descendant survived runner return" >&2
  exit 1
fi

# Decimal CPU/wall fields and integer memory fields must always be present.
for key in wall_seconds user_seconds system_seconds; do
  value=$(metric "$tmp/timeout.metrics" "$key")
  [[ $value =~ ^[0-9]+\.[0-9]+$ ]] || {
    echo "$key is not a nonnegative decimal: $value" >&2
    exit 1
  }
done
for key in max_rss_kb peak_sampled_rss_kb peak_sampled_swap_kb \
           proc_status_samples proc_rss_samples proc_swap_samples \
           proc_group_samples peak_sampled_process_count; do
  value=$(metric "$tmp/timeout.metrics" "$key")
  [[ $value =~ ^[0-9]+$ ]] || {
    echo "$key is not a nonnegative integer: $value" >&2
    exit 1
  }
done
timeout_wall=$(metric "$tmp/timeout.metrics" wall_seconds)
awk -v wall="$timeout_wall" 'BEGIN { exit !(wall >= 0.04 && wall < 2.0) }' || {
  echo "timeout wall time is outside the expected test bounds: $timeout_wall" >&2
  exit 1
}
(( $(metric "$tmp/timeout.metrics" max_rss_kb) > 0 ))
(( $(metric "$tmp/timeout.metrics" peak_sampled_rss_kb) > 0 ))
(( $(metric "$tmp/timeout.metrics" proc_status_samples) > 0 ))
(( $(metric "$tmp/timeout.metrics" proc_rss_samples) > 0 ))
(( $(metric "$tmp/timeout.metrics" proc_swap_samples) > 0 ))
(( $(metric "$tmp/timeout.metrics" proc_group_samples) > 0 ))
(( $(metric "$tmp/timeout.metrics" peak_sampled_process_count) > 0 ))

# The cap applies to aggregate process-tree RSS, not only one process. Each
# 40-MiB helper remains individually below the 64-MiB cap; their sum plus the
# shell/interpreter baseline crosses it. The timeout is only a backstop.
run_and_capture_status "$tmp/rss.status" \
  "$runner" \
    --rss-limit-bytes 67108864 \
    --timeout-seconds 5 \
    --stdout "$tmp/rss.out" \
    --stderr "$tmp/rss.err" \
    -- /bin/sh -c \
       'python3 -c "import os,time; os.setsid(); x=bytearray(40*1024*1024); time.sleep(10)" & first=$!; python3 -c "import os,time; os.setsid(); x=bytearray(40*1024*1024); time.sleep(10)" & second=$!; echo "$first $second" >"$1"; wait "$first" "$second"' \
       wric-rss "$tmp/rss.pid" \
    >"$tmp/rss.metrics"
[[ $(<"$tmp/rss.status") == 123 ]]
require_metric "$tmp/rss.metrics" outcome rss_limit
require_metric "$tmp/rss.metrics" exit_code -1
require_metric "$tmp/rss.metrics" term_signal 9
require_metric "$tmp/rss.metrics" timed_out 0
require_metric "$tmp/rss.metrics" runner_exit_code 123
require_metric "$tmp/rss.metrics" timeout_term_sent 0
require_metric "$tmp/rss.metrics" timeout_kill_sent 0
require_metric "$tmp/rss.metrics" rss_limit_bytes 67108864
require_metric "$tmp/rss.metrics" rss_limit_enabled 1
require_metric "$tmp/rss.metrics" rss_limit_observed 1
require_metric "$tmp/rss.metrics" rss_limit_exceeded 1
require_metric "$tmp/rss.metrics" rss_limit_term_sent 0
require_metric "$tmp/rss.metrics" rss_limit_kill_sent 1
rss_trigger=$(metric "$tmp/rss.metrics" rss_limit_trigger_bytes)
(( rss_trigger > 67108864 )) || {
  echo "RSS trigger does not exceed the configured cap: $rss_trigger" >&2
  exit 1
}
(( $(metric "$tmp/rss.metrics" peak_sampled_process_count) >= 3 ))
rss_wall=$(metric "$tmp/rss.metrics" wall_seconds)
awk -v wall="$rss_wall" 'BEGIN { exit !(wall < 2.0) }' || {
  echo "RSS enforcement was not prompt: wall_seconds=$rss_wall" >&2
  exit 1
}
for helper in $(<"$tmp/rss.pid"); do
  if kill -0 "$helper" 2>/dev/null; then
    echo "over-limit helper process was not killed: $helper" >&2
    exit 1
  fi
done
require_metric "$tmp/rss.metrics" live_descendants_at_return 0
require_metric "$tmp/rss.metrics" process_group_alive_at_return 0
require_metric "$tmp/rss.metrics" wait4_echild_at_return 1

# A leader that exits before its delayed allocator runs must not let that
# descendant escape to PPID 1. This is a runner-contract failure, and the
# subreaper kills and joins the descendant before returning.
run_and_capture_status "$tmp/leak.status" \
  "$runner" \
    --rss-limit-bytes 16777216 \
    --timeout-seconds 5 \
    --stdout "$tmp/leak.out" \
    --stderr "$tmp/leak.err" \
    -- /bin/sh -c \
       'python3 -c "import os,time; os.setsid(); time.sleep(.25); x=bytearray(64*1024*1024); time.sleep(10)" & helper=$!; echo "$helper" >"$1"; exit 0' \
       wric-leak "$tmp/leak.pid" \
    >"$tmp/leak.metrics"
[[ $(<"$tmp/leak.status") == 125 ]]
require_metric "$tmp/leak.metrics" outcome descendant_leak
require_metric "$tmp/leak.metrics" exit_code 0
require_metric "$tmp/leak.metrics" runner_exit_code 125
require_metric "$tmp/leak.metrics" descendant_cleanup_kill_sent 1
require_metric "$tmp/leak.metrics" live_descendants_at_return 0
require_metric "$tmp/leak.metrics" process_group_alive_at_return 0
require_metric "$tmp/leak.metrics" wait4_echild_at_return 1
(( $(metric "$tmp/leak.metrics" post_leader_descendants) >= 1 ))
if kill -0 "$(<"$tmp/leak.pid")" 2>/dev/null; then
  echo "post-leader descendant survived runner return" >&2
  exit 1
fi

# Timeout has deterministic precedence if the cap is crossed only during its
# TERM grace period, while RSS observation remains independently visible.
run_and_capture_status "$tmp/precedence.status" \
  "$runner" \
    --rss-limit-bytes 16777216 \
    --timeout-seconds 0.5 \
    --stdout "$tmp/precedence.out" \
    --stderr "$tmp/precedence.err" \
    -- python3 -c \
       'import signal,time
x = None
def allocate_after_timeout(_signal, _frame):
    global x
    x = bytearray(64*1024*1024)
signal.signal(signal.SIGTERM, allocate_after_timeout)
time.sleep(10)' \
    >"$tmp/precedence.metrics"
[[ $(<"$tmp/precedence.status") == 124 ]]
require_metric "$tmp/precedence.metrics" outcome timeout
require_metric "$tmp/precedence.metrics" timed_out 1
require_metric "$tmp/precedence.metrics" rss_limit_observed 1
require_metric "$tmp/precedence.metrics" rss_limit_exceeded 0
require_metric "$tmp/precedence.metrics" rss_limit_term_sent 0
require_metric "$tmp/precedence.metrics" rss_limit_kill_sent 0
precedence_trigger=$(metric "$tmp/precedence.metrics" rss_limit_trigger_bytes)
(( precedence_trigger > 16777216 ))
precedence_peak_kb=$(metric "$tmp/precedence.metrics" peak_sampled_rss_kb)
(( precedence_trigger <= precedence_peak_kb * 1024 ))
require_metric "$tmp/precedence.metrics" live_descendants_at_return 0
require_metric "$tmp/precedence.metrics" wait4_echild_at_return 1

# Optional metrics-file output and a disabled zero timeout are both supported.
"$runner" \
  --timeout-seconds 0 \
  --stdout "$tmp/file.out" \
  --stderr "$tmp/file.err" \
  --metrics "$tmp/file.metrics" \
  -- /bin/sh -c 'exit 0'
require_metric "$tmp/file.metrics" outcome exited
require_metric "$tmp/file.metrics" timed_out 0
require_metric "$tmp/file.metrics" rss_limit_enabled 0

# The RSS option is a strict positive uint64 and invalid input is rejected
# before the child command can run.
for invalid in 0 -1 1.5 '' 18446744073709551616; do
  rm -f "$tmp/invalid-ran"
  run_and_capture_status "$tmp/invalid.status" \
    "$runner" \
      --rss-limit-bytes "$invalid" \
      --stdout "$tmp/invalid.out" \
      --stderr "$tmp/invalid.err" \
      -- /bin/sh -c 'touch "$1"' wric-invalid "$tmp/invalid-ran" \
      >"$tmp/invalid.metrics" 2>"$tmp/invalid-runner.err"
  [[ $(<"$tmp/invalid.status") == 2 ]]
  [[ ! -e "$tmp/invalid-ran" ]]
done
run_and_capture_status "$tmp/missing-rss.status" \
  "$runner" \
    --stdout "$tmp/missing-rss.out" \
    --stderr "$tmp/missing-rss.err" \
    --rss-limit-bytes \
    >"$tmp/missing-rss.metrics" 2>"$tmp/missing-rss-runner.err"
[[ $(<"$tmp/missing-rss.status") == 2 ]]

# Metrics/output hardlink aliases are rejected before truncation or exec.
printf 'must-survive\n' >"$tmp/alias.metrics"
ln "$tmp/alias.metrics" "$tmp/alias.out"
run_and_capture_status "$tmp/alias.status" \
  "$runner" \
    --metrics "$tmp/alias.metrics" \
    --stdout "$tmp/alias.out" \
    --stderr "$tmp/alias.err" \
    -- /bin/sh -c 'touch "$1"' wric-alias "$tmp/alias-ran" \
    >"$tmp/alias-runner.out" 2>"$tmp/alias-runner.err"
[[ $(<"$tmp/alias.status") == 125 ]]
[[ $(<"$tmp/alias.metrics") == must-survive ]]
[[ ! -e "$tmp/alias-ran" ]]

printf 'symlink-must-survive\n' >"$tmp/symlink.metrics"
ln -s "$tmp/symlink.metrics" "$tmp/symlink.out"
run_and_capture_status "$tmp/symlink.status" \
  "$runner" \
    --metrics "$tmp/symlink.metrics" \
    --stdout "$tmp/symlink.out" \
    --stderr "$tmp/symlink.err" \
    -- /bin/sh -c 'touch "$1"' wric-symlink "$tmp/symlink-ran" \
    >"$tmp/symlink-runner.out" 2>"$tmp/symlink-runner.err"
[[ $(<"$tmp/symlink.status") == 125 ]]
[[ $(<"$tmp/symlink.metrics") == symlink-must-survive ]]
[[ ! -e "$tmp/symlink-ran" ]]

# A live child whose proc records cannot be monitored is killed and joined;
# measurement never silently degrades to leader-only or no-RSS sampling.
ln -s /proc "$tmp/injected-proc"
( /bin/sleep 0.05; rm "$tmp/injected-proc" ) &
proc_remover=$!
run_and_capture_status "$tmp/monitor.status" \
  "$runner" \
    --proc-root "$tmp/injected-proc" \
    --rss-limit-bytes 1073741824 \
    --timeout-seconds 5 \
    --stdout "$tmp/monitor.out" \
    --stderr "$tmp/monitor.err" \
    -- /bin/sh -c 'echo "$$" >"$1"; exec /bin/sleep 10' \
       wric-monitor "$tmp/monitor.pid" \
    >"$tmp/monitor.metrics"
wait "$proc_remover"
[[ $(<"$tmp/monitor.status") == 125 ]]
require_metric "$tmp/monitor.metrics" outcome monitor_error
require_metric "$tmp/monitor.metrics" runner_exit_code 125
require_metric "$tmp/monitor.metrics" monitor_error 1
(( $(metric "$tmp/monitor.metrics" monitor_error_count) > 0 ))
require_metric "$tmp/monitor.metrics" live_descendants_at_return 0
require_metric "$tmp/monitor.metrics" process_group_alive_at_return 0
require_metric "$tmp/monitor.metrics" wait4_echild_at_return 1
if kill -0 "$(<"$tmp/monitor.pid")" 2>/dev/null; then
  echo "unmonitorable child survived fail-closed cleanup" >&2
  exit 1
fi

# execvp failure retains 127 and is distinguishable from a command that chose
# to exit 127 itself.
run_and_capture_status "$tmp/exec.status" \
  "$runner" \
    --stdout "$tmp/exec.out" \
    --stderr "$tmp/exec.err" \
    -- wric-command-that-does-not-exist \
    >"$tmp/exec.metrics"
[[ $(<"$tmp/exec.status") == 127 ]]
require_metric "$tmp/exec.metrics" outcome exec_error
require_metric "$tmp/exec.metrics" exit_code 127
require_metric "$tmp/exec.metrics" child_error_stage exec

echo "wric_process_metrics_test: PASS"

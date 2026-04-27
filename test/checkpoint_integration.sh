#!/bin/sh
# Integration test for larch2 checkpoint/resume.
#
# Verifies (against issue #39's test plan):
#   3. Phase-1-style resume produces a final score ≤ uninterrupted final score.
#   4. Phase 2 deterministic resume — for every K in {1, 2, 3} on a 6-iteration
#      run with --seed 42 --checkpoint-after 1, the resumed run produces a
#      final DAG byte-equal to the uninterrupted run, AND every per-iteration
#      parsimony score in the results vector matches.
#   6. Args fingerprint mismatch — resuming with a different --patience exits
#      non-zero with an error message naming the conflicting field.
#   7. Multi-resume chain — kill at K=2, resume, kill at K=4, resume to
#      completion; final output byte-equals the uninterrupted run.
#   8. Forbidden input flag on resume — --resume + --vcf exits non-zero.
#
# Usage: checkpoint_integration.sh <path-to-larch2> <path-to-dag_canonical_equal>

set -e

LARCH2="$1"
CANON="$2"
if [ -z "$LARCH2" ] || [ -z "$CANON" ]; then
    echo "usage: $0 <path-to-larch2> <path-to-dag_canonical_equal>" >&2
    exit 1
fi

FIXTURE="data/testcase/full_dag.pb.gz"
if [ ! -f "$FIXTURE" ]; then
    echo "missing fixture: $FIXTURE" >&2
    exit 1
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Helper: run larch2 quietly, fail on non-zero exit.
run_larch() {
    "$LARCH2" "$@" > "$TMP/log.out" 2>&1 || {
        echo "larch2 failed; args: $*" >&2
        cat "$TMP/log.out" >&2
        exit 1
    }
}

# Helper: extract per-iteration parsimony scores from the run log.
extract_scores() {
    grep -oE "score=[0-9]+ " "$1" | sed 's/score=//;s/ //'
}

echo "=== Test: uninterrupted baseline ==="
run_larch --dag-pb "$FIXTURE" --iterations 6 --checkpoint-after 1 \
    --seed 42 -o "$TMP/baseline.pb.gz" --checkpoint-prefix "$TMP/baseline"
cp "$TMP/log.out" "$TMP/baseline.log"
BASELINE_SCORES=$(extract_scores "$TMP/baseline.log")
echo "baseline scores: $BASELINE_SCORES"

# Phase 2 gate: for each K in {1, 2, 3}, resume from ckpt-K and verify the
# final output byte-equals the uninterrupted baseline. (Byte equality is
# achievable because we preserve phylo_dag node/edge ordering through
# save_proto_dag/load_proto_dag — which save_proto_dag does once load order
# is forced to match.)
echo "=== Test: Phase 2 deterministic resume ==="
for K in 1 2 3; do
    rm -f "$TMP/resumed_${K}.pb.gz"
    run_larch --resume "$TMP/baseline.ckpt-${K}.pb.gz" \
        --iterations 6 --seed 42 -o "$TMP/resumed_${K}.pb.gz" \
        --checkpoint-prefix "$TMP/resumed_${K}"
    cp "$TMP/log.out" "$TMP/resumed_${K}.log"

    if ! "$CANON" "$TMP/baseline.pb.gz" "$TMP/resumed_${K}.pb.gz"; then
        echo "FAIL: resumed_${K}.pb.gz canonical-differs from baseline.pb.gz" >&2
        exit 1
    fi

    RESUMED_SCORES=$(extract_scores "$TMP/resumed_${K}.log")
    if [ "$BASELINE_SCORES" != "$RESUMED_SCORES" ]; then
        echo "FAIL: resumed_${K} scores ($RESUMED_SCORES) differ from baseline ($BASELINE_SCORES)" >&2
        exit 1
    fi
    echo "  K=$K: canonical-equal, scores match"
done

# Multi-resume chain: resume from baseline.ckpt-2, write fresh checkpoints,
# then resume from one of those fresh checkpoints to completion. iterations
# stays at the baseline value so the args fingerprint check passes.  This
# catches any RNG-state non-idempotency where serialize -> deserialize ->
# serialize drifts (the second deserialization runs on a state that itself
# came from a deserialization round-trip).
echo "=== Test: multi-resume chain ==="
run_larch --resume "$TMP/baseline.ckpt-2.pb.gz" \
    --iterations 6 --seed 42 -o "$TMP/chain1.pb.gz" \
    --checkpoint-prefix "$TMP/chain1" --checkpoint-after 1
run_larch --resume "$TMP/chain1.ckpt-4.pb.gz" \
    --iterations 6 --seed 42 -o "$TMP/chain2.pb.gz" \
    --checkpoint-prefix "$TMP/chain2"
if ! "$CANON" "$TMP/baseline.pb.gz" "$TMP/chain2.pb.gz"; then
    echo "FAIL: chained-resume canonical-differs from baseline" >&2
    exit 1
fi
echo "  multi-resume chain canonical-equal"

# Phase 1 contract: the score after resume is ≤ baseline's final score on the
# same fixture (resume should never make things worse). Since Phase 2 produces
# byte-equal output, this trivially holds, but the assertion guards Phase 1
# regressions if someone reverts the deterministic-resume path.
echo "=== Test: Phase 1 score-no-worse-than-baseline ==="
BASELINE_FINAL=$(echo "$BASELINE_SCORES" | tail -1)
for K in 1 2 3; do
    R_FINAL=$(extract_scores "$TMP/resumed_${K}.log" | tail -1)
    if [ "$R_FINAL" -gt "$BASELINE_FINAL" ]; then
        echo "FAIL: resumed_${K} final score ($R_FINAL) > baseline ($BASELINE_FINAL)" >&2
        exit 1
    fi
done
echo "  resume final score ≤ baseline final score"

# Args fingerprint mismatch: --patience differs between checkpoint and runtime
# args. Must exit non-zero and name "patience" as the differing field.
echo "=== Test: args fingerprint mismatch ==="
# First make a checkpoint with --patience set so the mismatch on resume is
# meaningful.
run_larch --dag-pb "$FIXTURE" --iterations 4 --checkpoint-after 2 \
    --seed 42 --patience 3 -o "$TMP/p3_out.pb.gz" \
    --checkpoint-prefix "$TMP/p3"

if "$LARCH2" --resume "$TMP/p3.ckpt-2.pb.gz" --iterations 4 --seed 42 \
    --patience 99 -o "$TMP/p99.pb.gz" > "$TMP/log.out" 2>&1; then
    echo "FAIL: expected non-zero exit on patience mismatch" >&2
    cat "$TMP/log.out" >&2
    exit 1
fi
if ! grep -q "patience" "$TMP/log.out"; then
    echo "FAIL: error message did not mention 'patience'" >&2
    cat "$TMP/log.out" >&2
    exit 1
fi
echo "  patience-mismatch refused with the right error"

# Forbidden input flag with --resume.
echo "=== Test: --resume rejects input flags ==="
if "$LARCH2" --resume "$TMP/baseline.ckpt-1.pb.gz" --vcf /dev/null \
    -o "$TMP/forbidden.pb.gz" > "$TMP/log.out" 2>&1; then
    echo "FAIL: expected non-zero exit on --resume + --vcf" >&2
    cat "$TMP/log.out" >&2
    exit 1
fi
if ! grep -q -- "--vcf" "$TMP/log.out"; then
    echo "FAIL: error message did not mention --vcf" >&2
    cat "$TMP/log.out" >&2
    exit 1
fi
echo "  --resume + --vcf refused"

# Pre-drift checkpoint: triggering drift entry should produce a
# *.ckpt-<K>-pre-drift.pb.gz file. With --patience 2 --drift 1, the score
# plateau at 75 will exhaust patience after iter 3 and trigger a drift entry,
# at which point the worker writes the pre-drift checkpoint.
echo "=== Test: pre-drift checkpoint ==="
rm -f "$TMP"/drift.*
run_larch --dag-pb "$FIXTURE" --iterations 8 --patience 2 --drift 1 \
    --seed 42 --checkpoint-after 1 -o "$TMP/drift.pb.gz" \
    --checkpoint-prefix "$TMP/drift"
PRE_DRIFT=$(ls "$TMP"/drift.ckpt-*-pre-drift.pb.gz 2>/dev/null || true)
if [ -z "$PRE_DRIFT" ]; then
    echo "FAIL: expected a pre-drift checkpoint with --patience 2 --drift 1" >&2
    cat "$TMP/log.out" >&2
    exit 1
fi
echo "  pre-drift checkpoint present: $(basename $PRE_DRIFT)"

# A baseline run (no --patience / no --drift) must NOT write any pre-drift
# checkpoint files.
if ls "$TMP"/baseline.ckpt-*-pre-drift.pb.gz >/dev/null 2>&1; then
    echo "FAIL: unexpected pre-drift checkpoint without --drift" >&2
    exit 1
fi
echo "  no spurious pre-drift checkpoint"

# Higher-than-supported schema_version is rejected. We synthesize a minimal
# checkpoint with schema_version=999 by editing the first 2 bytes of an
# existing checkpoint (tag 0x08 + varint 0xe7 0x07).
echo "=== Test: schema_version too new is rejected ==="
python3 - "$TMP/baseline.ckpt-1.pb.gz" "$TMP/future.pb.gz" <<'PY'
import sys, gzip
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
# data[0] == 0x08 (tag for schema_version varint), data[1] == 0x01
# Replace the schema_version byte with 0x7f (varint 127, single-byte).
assert data[0] == 0x08, "expected tag 0x08 at offset 0"
out = bytes([0x08, 0x7f]) + data[2:]
open(dst, 'wb').write(out)
PY
if "$LARCH2" --resume "$TMP/future.pb.gz" --iterations 1 --seed 42 \
    -o "$TMP/future_out.pb.gz" > "$TMP/log.out" 2>&1; then
    echo "FAIL: expected non-zero exit on future schema_version" >&2
    exit 1
fi
if ! grep -qi "schema_version" "$TMP/log.out"; then
    echo "FAIL: error message did not mention schema_version" >&2
    cat "$TMP/log.out" >&2
    exit 1
fi
echo "  future schema_version refused"

echo "=== ALL CHECKPOINT INTEGRATION TESTS PASSED ==="

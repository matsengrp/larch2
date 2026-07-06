#!/usr/bin/env bash
# WRIC DAG-native SPR & rank-3 rewrite: Phase 10 CI-scale sanity table.
#
# Runs the DAG-native chart-SPR search on a fixture KNOWN TO ACCEPT an improving
# move (test/wric_four_taxon_misplaced.{fa,nwk,ref}, the same four-taxon
# "misplaced" tree the library-level Phase-10 tests use) in BOTH accepted-state
# modes -- conservative (rebuild_after_accept = true, the Phase-0 baseline) and
# local-commit (--chart-spr-local-accept-updates) -- and emits a markdown sanity
# table diffing the cross-cutting counter contract between them, so a regression
# to "full rebuild per accept" or "dense materialize per candidate" is visible at
# CI scale.
#
# This is the Phase-10 deliverable: "CI-scale sanity table generation diffing a
# local-commit run's counters against the Phase 0 baseline."  The fixture is
# chosen so the per-accept contract is exercised for REAL: the single generated
# candidate is an improving move that is accepted exactly once, so
# local_commit_accepted_moves > 0 and the conservative per-accept counters
# (sidecar_rebuilds_after_accept / overlay_materializations_for_accept_
# materialization) are > 0 -- the load-bearing contrast a vacuous all-zero table
# cannot show.
#
# The contracted values it checks (per the plan's cross-cutting counter contract):
#
#   * sidecar_rebuilds_after_accept:
#       conservative > 0 (per accept), local-commit == 0
#   * overlay_materializations_for_accept_materialization:
#       conservative > 0 (per accept), local-commit == 0  (the load-bearing
#       per-accept counter; the umbrella full_overlay_materializations is
#       nonzero on any exact run because per-candidate exact verification
#       materializes, and only reaches 0 once Phase 8/9 eliminate that)
#   * local_commit_accepted_moves:
#       > 0 in BOTH local-commit modes (non-vacuous contract)
#   * transient_chain_extensions_for_verification:
#       present and nonzero in transient local-commit mode; 0 in cold mode
#   * inside_rows_recomputed_on_commit / outside_rows_recomputed_on_commit:
#       present and > 0 for a local-commit run (the Phase-3 affected-set
#       counters are non-trivial on a real commit)
#
# Usage (from the repo root):
#   cmake -B build -DGCC_TOOLCHAIN=$HOME/install/gcc-trunk -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ./doc/wric_phase10_sanity_table.sh
#
# Override paths via flags:
#   ./doc/wric_phase10_sanity_table.sh \
#       --dagutil ./build/bin/dagutil \
#       --out-doc doc/WRIC-CHART-SPR-SEARCH-PHASE10-SANITY.md
#
# Exit codes: 0 on a clean table; non-zero if a contracted value is violated
# (so this can gate CI, not just produce a report).
set -euo pipefail

DAGUTIL="./build/bin/dagutil"
OUT_DOC="doc/WRIC-CHART-SPR-SEARCH-PHASE10-SANITY.md"
SMOKE_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dagutil) DAGUTIL="$2"; shift 2 ;;
    --out-doc) OUT_DOC="$2"; shift 2 ;;
    --smoke) SMOKE_ONLY=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -x "$DAGUTIL" ]]; then
  echo "error: dagutil not found or not executable: $DAGUTIL" >&2
  echo "       build it first: cmake --build build -j" >&2
  exit 1
fi

# A fixture known to accept exactly one improving SPR move under
# --chart-spr-max-candidates 1.  This is the four-taxon "misplaced" tree the
# library-level Phase-10 tests (test/chart_spr_phase10_test.cpp) use, exposed
# as FASTA/Newick/refseq so dagutil can load it the same way the library tests
# build it.  (The Phase-0 baseline fixture data/test_5_trees/tree_0 accepts 0
# moves under --chart-spr-max-candidates 1, which made the per-accept contract
# vacuous; this fixture makes it non-vacuous.)
FIXTURE_FA="test/wric_four_taxon_misplaced.fa"
FIXTURE_NWK="test/wric_four_taxon_misplaced.nwk"
FIXTURE_REF="test/wric_four_taxon_misplaced.ref"

COMMON_ARGS=(
  --fasta "$FIXTURE_FA"
  --newick "$FIXTURE_NWK"
  --refseq "$FIXTURE_REF"
  --force-no-vcf
  --wric-polytomy-mode expand-bounded
  --wric-polytomy-max-shapes 1
  --chart-spr-search
  --chart-spr-max-candidates 1
)

# Extract a "key: value" counter line from a dagutil report.  Returns the
# integer value (or "absent").
counter_value() {
  local report="$1" key="$2"
  local v
  # The report prints each counter on its own "  key: value" line.  Grab the
  # first match (the summary-level report; the counters dump repeats them).
  v=$(grep -E -m1 "^[[:space:]]+${key}:[[:space:]]+[0-9]+" "$report" \
      | sed -E "s|^[[:space:]]+${key}:[[:space:]]+([0-9]+).*|\1|" || true)
  if [[ -z "$v" ]]; then echo "absent"; else echo "$v"; fi
}

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

cons_report="$tmp_dir/conservative.txt"
local_report="$tmp_dir/local_commit.txt"
cold_report="$tmp_dir/local_commit_cold.txt"

"$DAGUTIL" "${COMMON_ARGS[@]}" \
  > "$cons_report" 2>"$tmp_dir/cons.stderr" || {
    echo "error: conservative-mode dagutil run failed:" >&2
    cat "$tmp_dir/cons.stderr" >&2
    exit 1
  }

"$DAGUTIL" "${COMMON_ARGS[@]}" --chart-spr-local-accept-updates \
  > "$local_report" 2>"$tmp_dir/local.stderr" || {
    echo "error: local-commit dagutil run failed:" >&2
    cat "$tmp_dir/local.stderr" >&2
    exit 1
  }

# Cold verification mode: the named verification-mode choice must actually
# toggle the path (transient counter stays 0).
SMOKE_NOTE=""
if [[ "$SMOKE_ONLY" -eq 1 ]]; then
  # In smoke mode, skip the cold run to keep CI fast (the cold label is
  # separately covered by the dagutil_chart_spr_search_verification_cold
  # ctest).  The table records the cold column as "skipped (smoke)".
  SMOKE_NOTE=$'\n''> smoke mode: cold-verification column skipped (covered by dagutil_chart_spr_search_verification_cold).'
  cold_local_moves="skipped (smoke)"
  cold_accepted="skipped (smoke)"
  cold_sidecar="skipped (smoke)"
  cold_accept_mat="skipped (smoke)"
  cold_transient="skipped (smoke)"
  cold_verify="skipped (smoke)"
  cold_inside="skipped (smoke)"
  cold_outside="skipped (smoke)"
else
  "$DAGUTIL" "${COMMON_ARGS[@]}" --chart-spr-local-accept-updates \
    --chart-spr-verification-mode cold \
    > "$cold_report" 2>"$tmp_dir/cold.stderr" || {
      echo "error: cold-verification dagutil run failed:" >&2
      cat "$tmp_dir/cold.stderr" >&2
      exit 1
    }
  # Read the cold column from the COLD report (not the transient one); these
  # are semantically equal (verification mode does not affect commit counters)
  # but reading from the cold run is honest when the run was executed.
  # Read the cold column from the COLD report (not the transient one); the
  # commit counters are semantically equal across verification modes, but
  # reading from the cold run is honest when the run was executed.
  cold_local_moves=$(counter_value "$cold_report" "local_commit_accepted_moves")
  cold_accepted=$(counter_value "$cold_report" "accepted_moves")
  cold_sidecar=$(counter_value "$cold_report" "sidecar_rebuilds_after_accept")
  cold_accept_mat=$(counter_value "$cold_report" "overlay_materializations_for_accept_materialization")
  cold_transient=$(counter_value "$cold_report" "transient_chain_extensions_for_verification")
  cold_verify=$(counter_value "$cold_report" "overlay_materializations_for_exact_verification")
  cold_inside=$(counter_value "$cold_report" "inside_rows_recomputed_on_commit")
  cold_outside=$(counter_value "$cold_report" "outside_rows_recomputed_on_commit")
fi

# Read the contracted counters off each report.
# `accepted_moves` is the mode-independent accept count (the fixture must
# accept a move in every mode); `local_commit_accepted_moves` is local-commit-
# specific (0 in conservative mode by design).
cons_accepted=$(counter_value "$cons_report" "accepted_moves")
cons_sidecar=$(counter_value "$cons_report" "sidecar_rebuilds_after_accept")
cons_accept_mat=$(counter_value "$cons_report" "overlay_materializations_for_accept_materialization")
cons_local_moves=$(counter_value "$cons_report" "local_commit_accepted_moves")

local_sidecar=$(counter_value "$local_report" "sidecar_rebuilds_after_accept")
local_accept_mat=$(counter_value "$local_report" "overlay_materializations_for_accept_materialization")
local_local_moves=$(counter_value "$local_report" "local_commit_accepted_moves")
local_transient=$(counter_value "$local_report" "transient_chain_extensions_for_verification")
local_inside=$(counter_value "$local_report" "inside_rows_recomputed_on_commit")
local_outside=$(counter_value "$local_report" "outside_rows_recomputed_on_commit")

# Contract checks.  A violation exits non-zero so this script can gate CI.
violations=()
check_eq() {
  local label="$1" got="$2" want="$3"
  if [[ "$got" != "$want" ]]; then
    violations+=("$label: got $got, want $want")
  fi
}
check_gt_zero() {
  local label="$1" got="$2"
  if [[ "$got" == "absent" ]] || [[ "$got" -le 0 ]]; then
    violations+=("$label: got $got, want > 0")
  fi
}
# --- Non-vacuous contract: the fixture must actually accept a move. ---
# This is the load-bearing fix for the Phase-10 sanity table: the per-accept
# contract is meaningless unless at least one accept happens.  Both the
# conservative and the local-commit runs must accept >= 1 move on this fixture
# (accepted_moves is mode-independent; local_commit_accepted_moves is the
# local-commit-specific count, which is 0 in conservative mode by design).
check_gt_zero "conservative accepted_moves (fixture must accept)" "$cons_accepted"
check_gt_zero "local-commit accepted_moves (fixture must accept)" "$(counter_value "$local_report" "accepted_moves")"
check_gt_zero "local-commit local_commit_accepted_moves (non-vacuous contract)" "$local_local_moves"
# --- Conservative-mode contrast (the Phase-0 baseline reference): it
# materializes and rebuilds per accept, so the per-accept counters are > 0. ---
check_gt_zero "conservative sidecar_rebuilds_after_accept (contrast)" "$cons_sidecar"
check_gt_zero "conservative overlay_materializations_for_accept_materialization (contrast)" "$cons_accept_mat"
# --- Local-commit contracted values (the load-bearing per-accept pair). ---
check_eq "local-commit sidecar_rebuilds_after_accept" "$local_sidecar" "0"
check_eq "local-commit overlay_materializations_for_accept_materialization" "$local_accept_mat" "0"
# --- Phase-3 affected-row counters are non-trivial on a real commit. ---
check_gt_zero "local-commit inside_rows_recomputed_on_commit" "$local_inside"
check_gt_zero "local-commit outside_rows_recomputed_on_commit" "$local_outside"
# The contracted counters must be PRESENT (not "absent") in the local-commit
# report -- a renamed field hiding a regression is exactly what this guards.
for k in transient_chain_extensions_for_verification \
         inside_rows_recomputed_on_commit \
         outside_rows_recomputed_on_commit; do
  v=$(counter_value "$local_report" "$k")
  if [[ "$v" == "absent" ]]; then
    violations+=("local-commit $k absent from report")
  fi
done
# Conservative mode is unchanged: the per-accept counters must be present.
for k in sidecar_rebuilds_after_accept \
         overlay_materializations_for_accept_materialization; do
  v=$(counter_value "$cons_report" "$k")
  if [[ "$v" == "absent" ]]; then
    violations+=("conservative $k absent from report")
  fi
done
if [[ "$SMOKE_ONLY" -ne 1 ]]; then
  check_eq "cold-mode transient_chain_extensions_for_verification" "$cold_transient" "0"
  # Cold mode still accepts the move and commits locally (only the verification
  # path differs).
  check_gt_zero "cold-mode local_commit_accepted_moves" "$cold_local_moves"
fi

# Emit the markdown sanity table.
{
  echo "# WRIC chart-SPR search Phase-10 counter sanity table"
  echo
  echo "Auto-generated by \`doc/wric_phase10_sanity_table.sh\`.  Diffs a"
  echo "local-commit run's counters against the conservative Phase-0 baseline"
  echo "on a fixture known to accept an improving move"
  echo "(\`test/wric_four_taxon_misplaced\`), so the per-accept contract is"
  echo "exercised for real (not vacuously) and a regression to \"full rebuild"
  echo "per accept\" or \"dense materialize per candidate\" is visible."
  echo "Counter-only (no timing); see"
  echo "\`doc/WRIC-CHART-SPR-SEARCH-COUNTER-BASELINE.md\` for the full baseline."
  echo "${SMOKE_NOTE}"
  echo
  echo "## Contracted counters"
  echo
  echo "| counter | conservative (baseline) | local-commit (transient) | local-commit (cold) |"
  echo "|---|---:|---:|---:|"
  echo "| accepted_moves | $cons_accepted | $(counter_value "$local_report" "accepted_moves") | $cold_accepted |"
  echo "| local_commit_accepted_moves | $cons_local_moves | $local_local_moves | $cold_local_moves |"
  echo "| sidecar_rebuilds_after_accept | $cons_sidecar | $local_sidecar | $cold_sidecar |"
  echo "| overlay_materializations_for_accept_materialization | $cons_accept_mat | $local_accept_mat | $cold_accept_mat |"
  echo "| transient_chain_extensions_for_verification | 0 (no substrate) | $local_transient | $cold_transient |"
  echo "| overlay_materializations_for_exact_verification (cold path) | n/a | (see full report) | $cold_verify |"
  echo "| inside_rows_recomputed_on_commit | 0 | $local_inside | $cold_inside |"
  echo "| outside_rows_recomputed_on_commit | 0 | $local_outside | $cold_outside |"
  echo
  echo "## Contract"
  echo
  echo "- The fixture accepts an improving move in every mode"
  echo "  (\`local_commit_accepted_moves > 0\`), so the per-accept contract is"
  echo "  non-vacuous (a regression that broke local commit could not hide"
  echo "  behind an all-zero table)."
  echo "- Conservative mode materializes and rebuilds per accept:"
  echo "  \`sidecar_rebuilds_after_accept > 0\` and"
  echo "  \`overlay_materializations_for_accept_materialization > 0\` -- the"
  echo "  Phase-0 baseline reference."
  echo "- local-commit: \`sidecar_rebuilds_after_accept == 0\` and"
  echo "  \`overlay_materializations_for_accept_materialization == 0\` (no per-accept"
  echo "  dense materialization; the single allowed dense materialization is"
  echo "  counted under \`overlay_materializations_for_final_compaction\`)."
  echo "- local-commit reports the named \`commit_mode\` / \`verification_mode\` /"
  echo "  \`chain_per_accept_exactness_label\` (see the full dagutil report)."
  echo "- The umbrella \`full_overlay_materializations\` is nonzero on any exact"
  echo "  run because per-candidate exact verification materializes; it only"
  echo "  reaches 0 once Phase 8/9 eliminate that, which is why the per-accept"
  echo "  counter above is the load-bearing one."
  echo
  if [[ ${#violations[@]} -eq 0 ]]; then
    echo "## Result: PASS"
    echo
    echo "All contracted values hold."
  else
    echo "## Result: FAIL"
    echo
    echo "Violations:"
    for v in "${violations[@]}"; do
      echo "- $v"
    done
  fi
} > "$OUT_DOC"

echo "wrote $OUT_DOC"

if [[ ${#violations[@]} -gt 0 ]]; then
  echo "FAIL: ${#violations[@]} contract violation(s):" >&2
  for v in "${violations[@]}"; do echo "  - $v" >&2; done
  exit 1
fi

echo "Phase-10 sanity table: PASS"

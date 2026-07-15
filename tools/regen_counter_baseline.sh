#!/usr/bin/env bash
# Regenerate the fenced counter-snapshot block in
# doc/WRIC-CHART-SPR-SEARCH-COUNTER-BASELINE.md from
# tools/wric_counter_baseline.cpp, WITHOUT touching the surrounding prose.
#
# Why a splice and not a plain redirect: the generator emits only the counter
# snapshot block; the checked-in doc also has hand-written prose
# (How to regenerate / Configuration / What later phases check / Note) wrapping
# that block.  Redirecting the generator over the doc (`> doc/...md`) would
# silently delete all of that prose.  This script instead replaces ONLY the
# region delimited by the `<!-- wric-counter-baseline: snapshot begin/end -->`
# markers (the fenced block between them), so the prose is preserved and the
# regen path round-trips.
#
# Usage (from the repo root):
#   cmake -B build -DGCC_TOOLCHAIN=$HOME/install/gcc-trunk -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   # Build the generator once with the project's flags:
#   g++-trunk -std=c++26 -freflection -I include -I build/generated \
#       tools/wric_counter_baseline.cpp build/liblarch.a -lz -lpthread \
#       -static-libstdc++ -static-libgcc -o build/wric_counter_baseline
#   ./tools/regen_counter_baseline.sh
#
# Override the generator path / doc path via the environment if needed:
#   WRIC_COUNTER_BASELINE=./build/wric_counter_baseline \
#   WRIC_COUNTER_BASELINE_DOC=doc/WRIC-CHART-SPR-SEARCH-COUNTER-BASELINE.md \
#   ./tools/regen_counter_baseline.sh
#
# The run is deterministic (serial local scoring, seed=1), so two runs on the
# same commit produce byte-identical output and the splice is idempotent.
set -euo pipefail

GEN="${WRIC_COUNTER_BASELINE:-./build/wric_counter_baseline}"
DOC="${WRIC_COUNTER_BASELINE_DOC:-doc/WRIC-CHART-SPR-SEARCH-COUNTER-BASELINE.md}"
BEGIN_MARKER='<!-- wric-counter-baseline: snapshot begin -->'
END_MARKER='<!-- wric-counter-baseline: snapshot end -->'

if [[ ! -x "$GEN" ]]; then
  echo "error: generator not found or not executable: $GEN" >&2
  echo "       build it first; see the header of $0 or the doc's regen section" >&2
  exit 1
fi
if [[ ! -f "$DOC" ]]; then
  echo "error: baseline doc not found: $DOC" >&2
  exit 1
fi

# Sanity: both markers must be present and unique so the splice is unambiguous.
begin_count=$(grep -c -F -- "$BEGIN_MARKER" "$DOC" || true)
end_count=$(grep -c -F -- "$END_MARKER" "$DOC" || true)
if [[ "$begin_count" -ne 1 || "$end_count" -ne 1 ]]; then
  echo "error: expected exactly one begin and one end marker in $DOC;" >&2
  echo "       found begin=$begin_count end=$end_count" >&2
  echo "       markers: $BEGIN_MARKER / $END_MARKER" >&2
  exit 1
fi

tmp_doc="$(mktemp)"
tmp_snap="$(mktemp)"
trap 'rm -f "$tmp_doc" "$tmp_snap"' EXIT

# Capture the freshly generated snapshot block (the generator prints only the
# fenced-block content; we add the ``` fences ourselves around it below).
"$GEN" > "$tmp_snap"

# Splice: copy the doc verbatim, but between the two markers emit exactly
#   <begin marker>\n```<fenced snapshot>\n```\n<end marker>
# replacing whatever was there before.  Every line outside the markers,
# including all hand-written prose, is passed through unchanged.
awk -v snap="$tmp_snap" \
    -v begin="$BEGIN_MARKER" \
    -v end="$END_MARKER" '
  $0 == begin {
    in_block = 1
    print
    print "```"
    while ((getline line < snap) > 0) print line
    close(snap)
    print "```"
    next
  }
  $0 == end { in_block = 0; print; next }
  !in_block { print }
' "$DOC" > "$tmp_doc"

# Guard: the new doc must still contain exactly one begin/end pair (defensive —
# a botched splice must never silently drop the markers and thus a future regen).
new_begin=$(grep -c -F -- "$BEGIN_MARKER" "$tmp_doc" || true)
new_end=$(grep -c -F -- "$END_MARKER" "$tmp_doc" || true)
if [[ "$new_begin" -ne 1 || "$new_end" -ne 1 ]]; then
  echo "error: splice produced $new_begin begin / $new_end end markers (expected 1/1);" >&2
  echo "       leaving $DOC untouched" >&2
  exit 1
fi

mv "$tmp_doc" "$DOC"
echo "regenerated snapshot block in $DOC (prose preserved)"

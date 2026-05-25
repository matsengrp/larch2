#!/usr/bin/env bash
set -euo pipefail

# Legacy multisite chart-SPR helpers are diagnostic/oracle APIs.  Production
# code must not call them directly; it should route through instrumented oracle
# wrappers or future search-state APIs so counters can distinguish full rebuilds
# from local candidate scores.  The only allowed matches in production headers
# are the helper function definitions themselves.
pattern='score_multisite_spr_candidate_(lower_bound|exact)[[:space:]]*\('
status=0
while IFS= read -r file; do
  while IFS=: read -r line_no line_text; do
    [[ -n "${line_no}" ]] || continue
    if [[ "$file" == "include/larch/chart_spr.hpp" ]] && \
       [[ "$line_text" =~ ^[[:space:]]*inline[[:space:]]+spr_score_result[[:space:]]+score_multisite_spr_candidate_(lower_bound|exact)[[:space:]]*\( ]]; then
      continue
    fi
    echo "direct chart-SPR multisite helper call in $file:$line_no:" >&2
    echo "$line_text" >&2
    status=1
  done < <(grep -nE "$pattern" "$file" || true)
done < <(find include src tools -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)
exit "$status"

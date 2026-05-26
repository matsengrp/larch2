#!/usr/bin/env bash
set -euo pipefail

# Chart-SPR search code must score root rows through the public wrapper so
# score_ua_edge=true compressed patterns keep their per-reference-state counts.
# Scan production headers/tools the same way the multisite-helper lint does.
# The only permitted direct reference to the chart_multisite_detail helper is
# the single call inside chart_spr_weighted_root_score_from_row().
pattern='chart_multisite_detail::weighted_root_score_from_row'
wrapper_file='include/larch/chart_spr_search.hpp'
wrapper_name='chart_spr_weighted_root_score_from_row'

is_wrapper_occurrence() {
  local file=$1
  local line_no=$2
  [[ "$file" == "$wrapper_file" ]] || return 1
  awk -v target="$line_no" -v wrapper="$wrapper_name" '
    BEGIN { in_wrapper = 0; ok = 0 }
    $0 ~ "inline[[:space:]]+std::uint64_t[[:space:]]+" wrapper "[[:space:]]*\\(" {
      in_wrapper = 1
    }
    NR == target {
      ok = in_wrapper
      exit
    }
    in_wrapper && $0 ~ /^}/ {
      in_wrapper = 0
    }
    END { exit ok ? 0 : 1 }
  ' "$file"
}

status=0
occurrences=0
allowed_occurrences=0
while IFS= read -r file; do
  while IFS=: read -r line_no line_text; do
    [[ -n "${line_no}" ]] || continue
    occurrences=$((occurrences + 1))
    if is_wrapper_occurrence "$file" "$line_no"; then
      allowed_occurrences=$((allowed_occurrences + 1))
      continue
    fi
    echo "direct chart-SPR root-row detail scorer call in $file:$line_no:" >&2
    echo "$line_text" >&2
    status=1
  done < <(grep -nF "$pattern" "$file" || true)
done < <(find include src tools -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)

if [[ "$occurrences" -ne 1 || "$allowed_occurrences" -ne 1 ]]; then
  echo "expected exactly one direct $pattern occurrence: the wrapper call in $wrapper_name(); found $occurrences total, $allowed_occurrences allowed" >&2
  status=1
fi

exit "$status"

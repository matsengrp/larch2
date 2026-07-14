#!/usr/bin/env bash
set -euo pipefail

# Guard B&B output exactness labels.  A production-mask output is a safe DAG
# superset, not a topology-exact DAG.  Future topology_exact=true claims should
# update this guard only when backed by identity-preserving/source-history
# materialization or explicit no-recombination validation.

status=0

pattern='(^|[^[:alnum:]_])topology_exact[[:space:]]*=[[:space:]]*true'
# Match plain text reports (`topology_exact: true`), JSON output
# (`"topology_exact": true`), and C++ string literals that escape JSON quotes
# (`\"topology_exact\": true`).
report_pattern='(^|[^[:alnum:]_])(\\)?"?topology_exact(\\)?"?[[:space:]]*:[[:space:]]*true'
while IFS= read -r file; do
  while IFS=: read -r line_no line_text; do
    [[ -n "${line_no:-}" ]] || continue
    echo "topology_exact=true claim requires an identity-preserving/source-history proof in $file:$line_no:" >&2
    echo "$line_text" >&2
    status=1
  done < <(grep -nE "$pattern" "$file" || true)
  while IFS=: read -r line_no line_text; do
    [[ -n "${line_no:-}" ]] || continue
    echo "hard-coded topology_exact: true report is not allowed in $file:$line_no:" >&2
    echo "$line_text" >&2
    status=1
  done < <(grep -nE "$report_pattern" "$file" || true)
done < <(find include src tools -type f \( -name '*.hpp' -o -name '*.cpp' \) | sort)

# In the production-mask implementation body, topology_exact must never be true
# and production_mask_superset must be set true.
awk '
  /apply_production_mask_superset[[:space:]]*\(/ { in_fn = 1; depth = 0; saw_open = 0 }
  in_fn {
    if ($0 ~ /\{/) { n = gsub(/\{/, "{"); depth += n; saw_open = 1 }
    if ($0 ~ /(^|[^[:alnum:]_])topology_exact[[:space:]]*=[[:space:]]*true/) {
      printf "production-mask apply sets topology_exact=true at src/chart_bnb_trim_apply.cpp:%d\n", NR > "/dev/stderr";
      bad = 1;
    }
    if ($0 ~ /production_mask_superset[[:space:]]*=[[:space:]]*true/) saw_superset = 1;
    if ($0 ~ /}/) { n = gsub(/}/, "}"); depth -= n; if (saw_open && depth <= 0) in_fn = 0 }
  }
  END {
    if (!saw_superset) {
      print "apply_production_mask_superset() does not set production_mask_superset=true" > "/dev/stderr";
      bad = 1;
    }
    exit bad ? 1 : 0;
  }
' src/chart_bnb_trim_apply.cpp || status=1

# Benchmark guardrail: lower-bound chart-SPR must be labelled as a heuristic
# objective, with validated parsimony kept in separate columns.
spr_bench=tools/wric_spr_search_benchmark.sh
if ! grep -Eq '(objective|OBJECTIVE)="?composite_lower_bound_heuristic"?' "$spr_bench"; then
  echo "grammar_lower_bound benchmark mode is not labelled composite_lower_bound_heuristic" >&2
  status=1
fi
if ! grep -q 'best_validated_parsimony_min' "$spr_bench" || \
   ! grep -q 'final_validated_parsimony_min' "$spr_bench"; then
  echo "chart-SPR benchmark TSV must keep validated parsimony columns separate" >&2
  status=1
fi
if awk '
  /grammar_lower_bound\)/ { in_block = 1 }
  in_block && /;;/ { in_block = 0 }
  in_block && /(objective|OBJECTIVE)="?(grammar_exact|fixed_topology_exact|exact)/ { bad = 1 }
  END { exit bad ? 1 : 0 }
' "$spr_bench"; then
  :
else
  echo "grammar_lower_bound benchmark block assigns an exact-looking objective" >&2
  status=1
fi

exit "$status"

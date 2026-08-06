#!/usr/bin/env bash
set -euo pipefail

# The removed legacy helper merged a dominated topology's production and
# provenance masks into its dominator.  That is unsound for exact mask recovery
# and the helper had no callers.  Keep the named legacy seam out of the header;
# test_multisite_two_pass_dominated_suboptimal_does_not_overkeep supplies the
# semantic runtime regression for the behavior, while this script is only the
# named-seam tripwire.
header=include/larch/chart_trim.hpp
if [[ ! -r "$header" ]]; then
  echo "cannot read chart trim header: $header" >&2
  exit 1
fi

set +e
grep -nE '\bapply_dominance_pruning[[:space:]]*\(' "$header"
status=$?
set -e
case "$status" in
  0)
    echo "legacy provenance-merging dominance helper was reintroduced" >&2
    exit 1
    ;;
  1) ;;
  *)
    echo "failed to inspect chart trim header: grep exited $status" >&2
    exit 1
    ;;
esac

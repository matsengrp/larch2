#!/usr/bin/env bash
set -euo pipefail

# Phase-5 counter-contract guard: deriving prefix/intended-production witness
# topology key sets during final compaction must not perform hidden dense
# overlay materializations.  The single allowed dense materialization for local
# commit final compaction is compact_overlay_chain_to_dag()'s
# materialize_overlay_chain(chain) call, counted by the search call site as
# overlay_materializations_for_final_compaction.

file='include/larch/overlay_chain_compaction.hpp'
status=0

# No direct materialize_overlay_grammar() call is allowed anywhere in the
# compaction helper code.  Strip line comments so explanatory comments may name
# the forbidden API without failing this guard.
if ! awk '
  {
    line = $0;
    sub(/\/\/.*/, "", line);
    if (line ~ /materialize_overlay_grammar[[:space:]]*\(/) {
      printf "hidden dense overlay materialization in compaction code at %s:%d:\n%s\n", FILENAME, NR, $0 > "/dev/stderr";
      bad = 1;
    }
  }
  END { exit bad ? 1 : 0 }
' "$file"; then
  status=1
fi

# The prefix-witness derivation function itself must stay overlay/key-set
# based.  This catches the historical regression where every chain prefix was
# materialized before the final materialize_overlay_chain() call.
if ! awk '
  /^[[:space:]]*overlay_chain_prefix_witness_topology_key_sets[[:space:]]*\([^;]*\)[[:space:]]*\{/ {
    in_fn = 1;
    depth = 0;
    saw_open = 0;
    saw_overlay_key_witness = 0;
  }
  in_fn {
    line = $0;
    sub(/\/\/.*/, "", line);
    if (line ~ /\{/) { n = gsub(/\{/, "{", line); depth += n; saw_open = 1 }
    if (line ~ /materialize_overlay_grammar[[:space:]]*\(/ ||
        line ~ /materialize_overlay_chain[[:space:]]*\(/) {
      printf "prefix witness derivation materializes overlay grammar at %s:%d:\n%s\n", FILENAME, NR, $0 > "/dev/stderr";
      bad = 1;
    }
    if (line ~ /overlay_topology_key_set_containing_production_key[[:space:]]*\(/) {
      saw_overlay_key_witness = 1;
    }
    if (line ~ /}/) {
      n = gsub(/}/, "}", line);
      depth -= n;
      if (saw_open && depth <= 0) {
        in_fn = 0;
        done = 1;
      }
    }
  }
  END {
    if (!done) {
      print "could not locate overlay_chain_prefix_witness_topology_key_sets() body" > "/dev/stderr";
      bad = 1;
    }
    if (!saw_overlay_key_witness) {
      print "prefix witness derivation no longer calls the overlay/key-set witness helper" > "/dev/stderr";
      bad = 1;
    }
    exit bad ? 1 : 0;
  }
' "$file"; then
  status=1
fi

# Conversely, compact_overlay_chain_to_dag() should contain exactly one dense
# chain materialization: the advertised final compaction materialization.
if ! awk '
  /compact_overlay_chain_to_dag[[:space:]]*\(/ {
    in_fn = 1;
    depth = 0;
    saw_open = 0;
    materialize_chain_count = 0;
  }
  in_fn {
    line = $0;
    sub(/\/\/.*/, "", line);
    if (line ~ /\{/) { n = gsub(/\{/, "{", line); depth += n; saw_open = 1 }
    materialize_chain_count += gsub(/materialize_overlay_chain[[:space:]]*\([[:space:]]*chain[[:space:]]*\)/, "&", line);
    if (line ~ /}/) {
      n = gsub(/}/, "}", line);
      depth -= n;
      if (saw_open && depth <= 0) {
        in_fn = 0;
        done = 1;
      }
    }
  }
  END {
    if (!done) {
      print "could not locate compact_overlay_chain_to_dag() body" > "/dev/stderr";
      bad = 1;
    }
    if (materialize_chain_count != 1) {
      printf "expected exactly one materialize_overlay_chain(chain) call in compact_overlay_chain_to_dag(); found %d\n", materialize_chain_count > "/dev/stderr";
      bad = 1;
    }
    exit bad ? 1 : 0;
  }
' "$file"; then
  status=1
fi

exit "$status"

#!/usr/bin/env bash
set +e

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <regex> <command> [args...]" >&2
  exit 2
fi

regex=$1
shift

output=$("$@" 2>&1)
status=$?
printf '%s\n' "$output"

if [[ $status -eq 0 ]]; then
  echo "expected command to fail, but it exited 0" >&2
  exit 1
fi

printf '%s\n' "$output" | grep -Eq "$regex" || {
  echo "output did not match regex '$regex'" >&2
  exit 1
}

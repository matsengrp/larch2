#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! -f $1 ]]; then
  echo "usage: $0 <publisher>" >&2
  exit 2
fi

publisher=$1
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

mkdir "$root/staging"
printf 'first\n' >"$root/staging/member"
python3.14 "$publisher" "$root/staging" "$root/published"
[[ $(<"$root/published/member") == first ]]
if [[ -e $root/staging ]]; then
  [[ -L $root/published ]]
else
  [[ ! -L $root/published ]]
fi

mkdir "$root/race.staging" "$root/race.destination"
printf 'candidate\n' >"$root/race.staging/member"
printf 'winner\n' >"$root/race.destination/member"
if python3.14 "$publisher" "$root/race.staging" \
    "$root/race.destination" >/dev/null 2>&1; then
  echo "publisher replaced a concurrently created destination" >&2
  exit 1
fi
[[ $(<"$root/race.staging/member") == candidate ]]
[[ $(<"$root/race.destination/member") == winner ]]

echo "atomic directory no-replace publication: PASS"

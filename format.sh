#!/bin/sh
base="${1:-.}"
find "$base" -path '*/build*' -prune -o \( -name '*.hpp' -o -name '*.cpp' \) -print | xargs clang-format -i

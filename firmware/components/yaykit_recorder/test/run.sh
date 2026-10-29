#!/usr/bin/env bash
# Host tests for yaykit_recorder (no ESP-IDF needed); recordings go to a temporary directory.
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
mkdir "$OUT/rec"
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_rec" test_rec.c ../yk_rec.c
"$OUT/test_rec" "$OUT/rec"
rm -rf "$OUT"

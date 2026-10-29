#!/usr/bin/env bash
# Host tests for the pure C parts of yaykit_link (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -DYK_MEM_HOST -I../include -o "$OUT/test_link" test_link.c ../yk_line.c ../yk_mem.c
"$OUT/test_link"
# The long mode's one-phone rule (yk_link_keep.h).
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_bond" test_bond.c ../yk_link_bond.c
"$OUT/test_bond"
rm -rf "$OUT"

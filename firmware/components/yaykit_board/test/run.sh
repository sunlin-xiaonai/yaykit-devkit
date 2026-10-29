#!/usr/bin/env bash
# Host tests for the pure C parts of yaykit_board (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_board" test_board.c ../yk_board_logic.c ../rtc_regs.c ../yk_font_pack.c
"$OUT/test_board"
rm -rf "$OUT"

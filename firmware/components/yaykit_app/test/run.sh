#!/usr/bin/env bash
# Host tests for the pure parts of yaykit_app (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -I../include -I../../yaykit_key/include -I../../yaykit_key -o "$OUT/test_app" \
  test_app.c ../yk_app_logic.c ../yk_recovery_logic.c ../../yaykit_key/yk_key_sm.c
"$OUT/test_app"
rm -rf "$OUT"

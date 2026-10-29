#!/usr/bin/env bash
# Host tests for the pure C parts of yaykit_wifi (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_wifi" test_wifi.c ../yk_wifi_creds.c
"$OUT/test_wifi"
rm -rf "$OUT"

#!/usr/bin/env bash
# Host tests for the pure C parts of yaykit_voice (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_voice" test_voice.c ../yk_wav.c
"$OUT/test_voice"
rm -rf "$OUT"

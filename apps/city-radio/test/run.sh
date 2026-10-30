#!/usr/bin/env bash
# Host tests for City Radio's plain C parts (no device, no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_radio" test_radio.c ../main/stations.c ../main/pcm_conv.c
"$OUT/test_radio"
rm -rf "$OUT"

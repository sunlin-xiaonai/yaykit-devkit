#!/usr/bin/env bash
# Host tests for the pure C parts of yaykit_audio (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O2 -o "$OUT/test_audio" test_audio.c ../yk_dsp.c ../yk_fe.c ../yk_srmodels.c ../yk_onset.c -lm
"$OUT/test_audio"
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/test_ptt" test_ptt.c ../yk_ptt.c
"$OUT/test_ptt"
rm -rf "$OUT"

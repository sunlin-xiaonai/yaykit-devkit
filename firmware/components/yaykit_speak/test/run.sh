#!/usr/bin/env bash
# Host tests for the pure parts of yaykit_speak (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -I.. -o "$OUT/test_speak" test_speak.c ../yk_speak_dsp.c -lm
"$OUT/test_speak"
cc -std=c11 -Wall -Wextra -Werror -O1 -I.. -I../include -Istub -o "$OUT/test_mix" test_mix.c ../yk_mix.c ../yk_adpcm.c -lm
"$OUT/test_mix"
rm -rf "$OUT"

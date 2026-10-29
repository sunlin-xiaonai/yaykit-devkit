#!/usr/bin/env bash
# Host tests for yaykit_motion's core (no ESP-IDF needed): synthesised movements and the recordings in motion/.
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O1 -I.. -I../include -Istub -o "$OUT/test_motion" test_motion.c ../yk_motion_core.c -lm
YKM_TMP="$OUT/replay.csv" "$OUT/test_motion" motion
rm -rf "$OUT"

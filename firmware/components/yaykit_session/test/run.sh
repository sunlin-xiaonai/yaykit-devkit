#!/usr/bin/env bash
# Host tests for yaykit_session's protocol code, against the platform's own examples
# (packages/contracts/fixtures/device.json). cJSON comes from ESP-IDF (IDF_PATH, or ~/esp/esp-idf).
set -euo pipefail
cd "$(dirname "$0")"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
CJSON="$IDF/components/json/cJSON"
if [ ! -f "$CJSON/cJSON.c" ]; then
  echo "yaykit_session host tests skipped: no ESP-IDF for cJSON (set IDF_PATH)"
  exit 0
fi
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -I.. -I"$CJSON" -o "$OUT/test_session" test_session.c ../yk_proto.c \
  "$CJSON/cJSON.c" -lm
"$OUT/test_session" ../../../../../packages/contracts/fixtures/device.json
rm -rf "$OUT"

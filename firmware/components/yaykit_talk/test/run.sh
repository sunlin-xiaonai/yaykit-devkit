#!/usr/bin/env bash
# Host tests for yaykit_talk's plain C parts (the turn state machine, the settings by voice, the hello), the hello
# against the platform's own examples (packages/contracts/fixtures/device.json). cJSON comes from ESP-IDF
# (IDF_PATH, or ~/esp/esp-idf).
set -euo pipefail
cd "$(dirname "$0")"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
CJSON="$IDF/components/json/cJSON"
if [ ! -f "$CJSON/cJSON.c" ]; then
  echo "yaykit_talk host tests skipped: no ESP-IDF for cJSON (set IDF_PATH)"
  exit 0
fi
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -I../include -I.. -I../../yaykit_session -I"$CJSON" -o "$OUT/test_talk" \
  test_talk.c ../yk_talk_sm.c ../yk_talk_setting.c ../yk_talk_hello.c ../yk_talk_msg.c ../yk_talk_flush.c ../yk_talk_close.c ../yk_talk_setup.c \
  ../../yaykit_session/yk_proto.c "$CJSON/cJSON.c" -lm
"$OUT/test_talk" ../../../../../packages/contracts/fixtures/device.json
rm -rf "$OUT"

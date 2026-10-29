#!/usr/bin/env bash
# Host tests for yaykit_sig against firmware/signing/TEST-ONLY-vectors.json, built with ESP-IDF's mbedTLS and cJSON.
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${IDF_PATH:-}" ] || [ ! -d "$IDF_PATH/components/mbedtls/mbedtls/library" ]; then
  echo "yaykit_sig check skipped: set IDF_PATH to build with ESP-IDF's mbedTLS"
  exit 0
fi
MBED="$IDF_PATH/components/mbedtls/mbedtls"
CJSON="$IDF_PATH/components/json/cJSON"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# The component itself with every warning; mbedTLS and cJSON as they come.
cc -std=c11 -Wall -Wextra -Werror -O1 -I ../include -I "$MBED/include" -c ../yaykit_sig.c -o "$OUT/yaykit_sig.o"
cc -std=c11 -Wall -Wextra -Werror -O1 -I ../include -I "$CJSON" -c test_sig.c -o "$OUT/test_sig.o"
cc -std=c11 -O1 -w -I "$MBED/include" -I "$CJSON" -o "$OUT/test_sig" "$OUT/yaykit_sig.o" "$OUT/test_sig.o" "$CJSON/cJSON.c" "$MBED"/library/*.c
"$OUT/test_sig" "$(cd ../../../signing && pwd)/TEST-ONLY-vectors.json"

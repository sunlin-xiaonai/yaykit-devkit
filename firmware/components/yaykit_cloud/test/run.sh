#!/usr/bin/env bash
# Host tests for the pure C parts of yaykit_cloud (no ESP-IDF needed).
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -O1 -I ../include -o "$OUT/test_cloud" test_cloud.c ../yk_codec.c ../yk_sse.c ../yk_locale.c
"$OUT/test_cloud"
rm -rf "$OUT"

# The device API v1 readers and builders against the platform's contract fixtures, with ESP-IDF's cJSON.
if [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/components/json/cJSON/cJSON.c" ]; then
  CJSON="$IDF_PATH/components/json/cJSON"
  OUT=$(mktemp -d)
  cc -std=c11 -Wall -Wextra -Werror -O1 -I ../include -I "$CJSON" -o "$OUT/test_api" test_api.c ../yk_api.c "$CJSON/cJSON.c"
  "$OUT/test_api" "$(cd ../../../../.. && pwd)/packages/contracts/fixtures/device-api.json"
  rm -rf "$OUT"
  # /me and the kept owner: yaykit_cloud.c itself, against test/stub (Yay's check leaves the language alone)
  OUT=$(mktemp -d)
  cc -std=c11 -Wall -Wextra -Werror -O1 -I stub -I ../include -I .. -I "$CJSON" -o "$OUT/test_me" test_me.c ../yaykit_cloud.c \
    ../yk_api.c ../yk_sse.c ../yk_locale.c ../yk_codec.c "$CJSON/cJSON.c"
  "$OUT/test_me"
  rm -rf "$OUT"
else
  echo "device API check skipped: set IDF_PATH for ESP-IDF's cJSON"
fi

# Signatures: build yk_sign.c with ESP-IDF's mbedTLS and verify them with the server's verifier.
if [ -n "${IDF_PATH:-}" ] && [ -d "$IDF_PATH/components/mbedtls/mbedtls/library" ]; then
  MBED="$IDF_PATH/components/mbedtls/mbedtls"
  OUT=$(mktemp -d)
  cc -std=c11 -O1 -w -I "$MBED/include" -o "$OUT/sign_vectors" sign_vectors.c ../yk_sign.c ../yk_codec.c "$MBED"/library/*.c
  ROOT=$(cd ../../../.. && pwd)
  "$OUT/sign_vectors" | (cd "$ROOT" && pnpm exec tsx firmware/components/yaykit_cloud/test/verify_vectors.ts)
  rm -rf "$OUT"
else
  echo "signature check skipped: set IDF_PATH to build with ESP-IDF's mbedTLS"
fi

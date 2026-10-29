#!/usr/bin/env bash
# Host tests for yaykit_install's pure parts and key table, built with ESP-IDF's mbedTLS (SHA-256, ECDSA for yaykit_sig).
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${IDF_PATH:-}" ] || [ ! -d "$IDF_PATH/components/mbedtls/mbedtls/library" ]; then
  echo "yaykit_install check skipped: set IDF_PATH to build with ESP-IDF's mbedTLS"
  exit 0
fi
MBED="$IDF_PATH/components/mbedtls/mbedtls"
SIG=../../yaykit_sig
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# The component with every warning; mbedTLS as it comes.
for src in ../yk_install_logic.c ../yk_install_keys.c test_install.c; do
  cc -std=c11 -Wall -Wextra -Werror -O1 -I . -I .. -I "$SIG/include" -I "$MBED/include" -c "$src" -o "$OUT/$(basename "$src" .c).o"
done
cc -std=c11 -O1 -w -I "$SIG/include" -I "$MBED/include" -c "$SIG/yaykit_sig.c" -o "$OUT/yaykit_sig.o"
cc -std=c11 -O1 -w -I "$MBED/include" -o "$OUT/test_install" "$OUT"/*.o "$MBED"/library/*.c
"$OUT/test_install"

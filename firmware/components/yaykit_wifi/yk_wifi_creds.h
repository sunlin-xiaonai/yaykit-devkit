// Checks for `wifi.join` arguments. Pure C, host-tested (test/run.sh).
#pragma once

/**
 * NULL when `ssid` / `password` can be handed to esp_wifi, else the Yaykit Link error code: BAD_SSID (1 to 32
 * bytes) or BAD_PASSWORD (empty for an open network, 8 to 63 bytes without control characters, or 64 hex digits).
 */
const char *yk_wifi_join_error(const char *ssid, const char *password);

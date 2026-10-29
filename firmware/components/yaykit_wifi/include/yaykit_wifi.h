// Yaykit Wi-Fi: one station connection shared by every app in the yaykit-v1 layout.
//
// Credentials are set from the phone over Yaykit Link (standard prov-scan / prov-config endpoints), or from the
// desktop client and web installer over USB with the `wifi.scan` / `wifi.join` commands, and kept by esp_wifi in
// NVS, so the launcher and the apps all reuse them. The password is never logged, echoed or reported; over USB it
// travels only on the hidden argument line of `yk cmd wifi.join -` (see yaykit_link.h).
// Console: `wifi status`, `wifi scan`, `wifi forget`.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YK_WIFI_OFF,
    YK_WIFI_UNCONFIGURED,
    YK_WIFI_CONNECTING,
    YK_WIFI_CONNECTED,
    YK_WIFI_FAILED,  // retries continue in the background with a longer delay
} yk_wifi_state_t;

/** Starts the station and connects with stored credentials, if any. */
esp_err_t yk_wifi_start(void);
yk_wifi_state_t yk_wifi_state(void);
const char *yk_wifi_state_name(void);
/** Waits until the station has an IP address. */
bool yk_wifi_wait_connected(uint32_t timeout_ms);
/** Fills a yk-info "wifi" object: state; ssid / rssi / ip when connected; error (WRONG_PASSWORD or NOT_FOUND) when failed. */
void yk_wifi_describe(cJSON *wifi);
/**
 * Yaykit Link commands; ESP_ERR_NOT_FOUND for other names.
 *   wifi.scan                         -> {"networks":[{"ssid","rssi","auth":"open|password|unsupported"}]}, strongest
 *                                        first, one entry per name, hidden networks left out; blocks up to 15 s.
 *   wifi.join {"ssid","password"}     -> {"state":"connecting","ssid"}; then follow yk-info "wifi" to connected or
 *                                        failed. Errors: BAD_SSID, BAD_PASSWORD, WIFI_OFF, JOIN_FAILED.
 */
esp_err_t yk_wifi_command(const char *name, const cJSON *args, cJSON *reply, const char **code);
/** Forgets a network the phone sent but never applied (prov-config without apply): call when the phone link goes
 *  off, so the password does not stay in memory. */
void yk_wifi_drop_pending(void);
/** wifi_prov_scan_handlers_t / wifi_prov_config_handlers_t for yk_link_config_t. */
void *yk_wifi_scan_handlers(void);
void *yk_wifi_config_handlers(void);
/** Connected, but nothing gets through (the caller looked: a name that does not resolve, say): leaves the network
 *  and joins it again, which also renews the address. ESP_ERR_INVALID_STATE when not connected. `why` is logged. */
esp_err_t yk_wifi_reconnect(const char *why);
/** Registers the `wifi` console command. */
esp_err_t yk_wifi_register_console(void);

#ifdef __cplusplus
}
#endif

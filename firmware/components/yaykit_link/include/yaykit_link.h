// Yaykit Link: device facts and remote commands over USB console and BLE.
//
// USB console:  `yk info`             -> YK_INFO {json}
//               `yk cmd <name> [json]` -> YK_OK {json} | YK_ERR <code>
//               `yk cmd <name> -`      -> YK_ARGS, then the client sends the JSON on the next line, which is
//                                         neither echoed nor kept in the console history (10 s; ARGS_TIMEOUT,
//                                         TOO_LONG). Secrets go this way only: an inline "password" is refused
//                                         with SECRET_IN_LINE and the history is cleared.
//               `yk ble on|off|status` (bench testing; prints pairing data)
// BLE:          ESP-IDF protocomm over NimBLE with Security 2 (SRP6a + AES-GCM),
//               username "wifiprov", proof of possession = 8-digit pairing code.
//               Endpoints: proto-ver, prov-session, yk-info, yk-cmd, and prov-scan / prov-config
//               when the app provides Wi-Fi provisioning handlers. Replies longer than one BLE
//               attribute arrive in parts: "\x1e<length>\n" and the first bytes, then the client sends
//               {"name":"link.more"} to yk-cmd for each following part (proto-ver yk.part = part size).
#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YK_LINK_BLE_ADVERTISING,
    YK_LINK_BLE_CONNECTED,
    YK_LINK_BLE_DISCONNECTED,
    YK_LINK_BLE_STOPPED,
} yk_link_event_t;

/** Runs a command. Fill `reply` (an empty object) and return ESP_OK, or return an error and set `*code`. */
typedef esp_err_t (*yk_link_command_fn)(const char *name, const cJSON *args, cJSON *reply, const char **code, void *ctx);
/** Transport state changes. Called from the BLE or timer task; keep it short and do not block. */
typedef void (*yk_link_event_fn)(yk_link_event_t event, void *ctx);

typedef struct {
    const char *board;        // e.g. "tikpal-r1"
    const char *app_slug;     // e.g. "party-play"
    const char *app_version;  // e.g. "0.2.0"
    const char *layout;       // "factory-v0" or "yaykit-v1"
    bool wifi;                // reports Wi-Fi state when the app has Wi-Fi
    int idle_seconds;         // BLE stops after this long without a request (default 90)
    yk_link_command_fn on_command;
    yk_link_event_fn on_event;
    void *ctx;
    /** Optional standard Wi-Fi provisioning over BLE: wifi_prov_scan_handlers_t and wifi_prov_config_handlers_t
     *  (mutable; protocomm keeps per-session context in them). Both set adds prov-scan, prov-config and the
     *  "wifi_scan" capability. */
    void *wifi_scan_handlers;
    void *wifi_config_handlers;
    /** Optional: fills the yk-info "wifi" object (state, ssid, ip...). */
    void (*describe_wifi)(cJSON *wifi);
    /** Optional: fills the yk-info "account" object (cloud activation state, see yaykit_account.h). */
    void (*describe_account)(cJSON *account);
    /** Optional: adds to the yk-info "heap" object, after the numbers every firmware has (Yay: stackMinFree). */
    void (*describe_heap)(cJSON *heap);
    /** Optional: fills a top-level yk-info "recovery" object (Yay: the recovery's version, sha256, size); return false
     *  to leave it out (not read). Not in "slots": yk info's slot 0 is Yay. */
    bool (*describe_recovery)(cJSON *recovery);
    /** Optional: what this firmware can do that clients must know of, as the yk-info "capabilities" array
     *  (NULL-terminated; left out when NULL). "signed_install": it installs signed images only (yaykit_install), so
     *  a client offers no unsigned version. */
    const char *const *capabilities;
} yk_link_config_t;

esp_err_t yk_link_init(const yk_link_config_t *config);
/** Registers the `yk` console command. Call after esp_console is initialised. */
esp_err_t yk_link_register_console(void);

/** Stable per device, not reversible to the MAC: yk_ + first 12 hex of SHA-256(base MAC + "yaykit"). */
const char *yk_link_device_id(void);
/** The yk-info document. Caller frees with cJSON_free(). */
char *yk_link_info_json(void);

/** Starts advertising YK-xxxx with a fresh pairing code. Takes a moment: the SRP verifier is derived here. */
esp_err_t yk_link_ble_start(void);
esp_err_t yk_link_ble_stop(void);
/** While held, the link does not stop by itself after idle_seconds without a request: a screen showing the pairing
 *  code holds it, as the phone may take a while to get to it (Rejoin, 10-21: 36 s to type the code). Released, the
 *  idle time counts again from then. */
void yk_link_ble_hold(bool hold);
bool yk_link_ble_active(void);
bool yk_link_ble_connected(void);
/** Valid while BLE is active. */
const char *yk_link_ble_name(void);
const char *yk_link_pairing_code(void);
/** ESPProvision QR payload: {"ver":"v1","name":...,"username":"wifiprov","pop":...,"transport":"ble","security":2}. */
const char *yk_link_pairing_qr(void);

#ifdef __cplusplus
}
#endif

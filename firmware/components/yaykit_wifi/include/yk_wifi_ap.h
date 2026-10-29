// Yaykit Wi-Fi: a hotspot of the round screen's own, for an extension's phone to join (自拍屏's viewfinder,
// docs/design/selfie.md 3). Not provisioning: the shared credentials are only ever read, never written (esp_wifi
// keeps configs in RAM from the first yk_wifi_ap_start() on).
//
// Added beside yaykit_wifi.h, nothing in it changed: a firmware that never calls these runs as before. Without
// yk_wifi_start() the hotspot runs alone (soft AP only: no station scanning to pull the channel away while the
// network is down); after it, beside the station (AP + STA on the station's channel).
//
// WPA2, one client by default, 192.168.4.1 with its own DHCP server. Wi-Fi and lwIP buffers follow the firmware's
// sdkconfig (PSRAM where it says so). A firmware receiving a stream over it caps lwIP's out-of-order segments
// (CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS): see firmware/selfie/sdkconfig.defaults for why.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_WIFI_AP_IP "192.168.4.1"

typedef struct {
    const char *ssid;      // ASCII, 1..32 bytes
    const char *password;  // WPA2, 8..63 characters; never logged
    uint8_t max_clients;   // 0 = 1
    uint8_t channel;       // 0: the station's when it runs, else 6
} yk_wifi_ap_config_t;

/** A phone joined (true) or left (false); from the event task, keep it short. */
typedef void (*yk_wifi_ap_client_fn)(bool joined, void *ctx);

/** Opens the hotspot (any task with an internal-RAM stack the first time: it may initialise esp_wifi). Already open:
 *  ESP_OK and nothing changes. ESP_ERR_INVALID_ARG for a bad name or password. */
esp_err_t yk_wifi_ap_start(const yk_wifi_ap_config_t *cfg);
/** Closes it (the station, if any, stays as it was). Not open: ESP_OK. */
esp_err_t yk_wifi_ap_stop(void);
bool yk_wifi_ap_active(void);
/** Phones joined now. */
int yk_wifi_ap_clients(void);
/** The hotspot's MAC (its BSSID: Android's WifiNetworkSpecifier matches it). ESP_ERR_INVALID_STATE before the
 *  first start. */
esp_err_t yk_wifi_ap_bssid(uint8_t mac[6]);
/** One listener for joins and leaves (NULL stops). */
void yk_wifi_ap_on_client(yk_wifi_ap_client_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

// The hotspot (yk_wifi_ap.h). Grown from the selfie lab's sl_net.c (exp/selfie-lab, c527e06c): there the AP ran
// beside the station and opened in 10-53 ms without the station dropping.
#include "yk_wifi_ap.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "yaykit_wifi.h"

static const char *TAG = "yk_wifi_ap";

static SemaphoreHandle_t s_lock;
static esp_netif_t *s_netif;
static bool s_ours;     // esp_wifi was started here (no station): stopped again with the hotspot
static bool s_on;
static bool s_inited;   // esp_wifi initialised (by yaykit_wifi or here)
static volatile int s_clients;
static yk_wifi_ap_client_fn s_fn;
static void *s_ctx;

bool yk_wifi_ap_active(void) { return s_on; }
int yk_wifi_ap_clients(void) { return s_clients; }

void yk_wifi_ap_on_client(yk_wifi_ap_client_fn fn, void *ctx) {
    s_ctx = ctx;
    s_fn = fn;
}

esp_err_t yk_wifi_ap_bssid(uint8_t mac[6]) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return esp_wifi_get_mac(WIFI_IF_AP, mac);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg, (void)base, (void)data;
    bool joined;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        joined = true;
        s_clients++;
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        joined = false;
        if (s_clients > 0) s_clients--;
    } else {
        return;
    }
    ESP_LOGI(TAG, "%s (%d now)", joined ? "a phone joined" : "a phone left", s_clients);
    const yk_wifi_ap_client_fn fn = s_fn;
    if (fn) fn(joined, s_ctx);
}

static bool printable_ascii(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) return false;
    return true;
}

/** esp_wifi up for the hotspot: yaykit_wifi's when it runs (AP + STA), else started here (AP only). */
static esp_err_t ensure_wifi(bool *with_sta) {
    *with_sta = yk_wifi_state() != YK_WIFI_OFF;
    if (!s_netif) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_netif = esp_netif_create_default_wifi_ap();
        if (!s_netif) return ESP_ERR_NO_MEM;
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, on_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, on_event, NULL));
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        const esp_err_t err = esp_wifi_init(&init);
        if (err != ESP_OK) return err;
    }
    s_inited = true;
    // Never write the shared credentials' namespace (nvs.net80211): from here on esp_wifi keeps configs in RAM.
    return esp_wifi_set_storage(WIFI_STORAGE_RAM);
}

static esp_err_t start_locked(const yk_wifi_ap_config_t *cfg, size_t ssid_len, size_t pass_len) {
    bool with_sta = false;
    esp_err_t err = ensure_wifi(&with_sta);
    if (err != ESP_OK) return err;
    uint8_t channel = cfg->channel ? cfg->channel : 6;
    wifi_ap_record_t sta;
    if (with_sta && esp_wifi_sta_get_ap_info(&sta) == ESP_OK) channel = sta.primary;  // the AP follows it anyway
    if ((err = esp_wifi_set_mode(with_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP)) != ESP_OK) return err;
    wifi_config_t c = {0};
    memcpy(c.ap.ssid, cfg->ssid, ssid_len);
    c.ap.ssid_len = ssid_len;
    memcpy(c.ap.password, cfg->password, pass_len);
    c.ap.channel = channel;
    c.ap.authmode = WIFI_AUTH_WPA2_PSK;
    c.ap.max_connection = cfg->max_clients ? cfg->max_clients : 1;
    c.ap.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_AP, &c);
    memset(&c, 0, sizeof(c));  // the password
    if (err == ESP_OK) esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (err == ESP_OK && !with_sta) {
        err = esp_wifi_start();  // ESP_OK when it runs already
        s_ours = err == ESP_OK;
    }
    if (err != ESP_OK) {
        esp_wifi_set_mode(with_sta ? WIFI_MODE_STA : WIFI_MODE_NULL);
        return err;
    }
    s_clients = 0;
    s_on = true;
    ESP_LOGI(TAG, "on: %s, channel %u%s", cfg->ssid, channel, with_sta ? ", beside the station" : "");
    return ESP_OK;
}

esp_err_t yk_wifi_ap_start(const yk_wifi_ap_config_t *cfg) {
    if (!cfg || !cfg->ssid || !cfg->password) return ESP_ERR_INVALID_ARG;
    const size_t ssid_len = strlen(cfg->ssid), pass_len = strlen(cfg->password);
    if (ssid_len < 1 || ssid_len > 32 || !printable_ascii(cfg->ssid) || pass_len < 8 || pass_len > 63 ||
        !printable_ascii(cfg->password))
        return ESP_ERR_INVALID_ARG;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = s_on ? ESP_OK : start_locked(cfg, ssid_len, pass_len);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t yk_wifi_ap_stop(void) {
    if (!s_lock) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_on) {
        s_on = false;
        if (s_ours) {
            err = esp_wifi_stop();
            s_ours = false;
        } else {
            err = esp_wifi_set_mode(WIFI_MODE_STA);
        }
        s_clients = 0;
        ESP_LOGI(TAG, "off");
    }
    xSemaphoreGive(s_lock);
    return err;
}

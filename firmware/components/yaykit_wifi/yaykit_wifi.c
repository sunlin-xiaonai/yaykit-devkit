#include "yaykit_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "wifi_provisioning/wifi_config.h"
#include "wifi_provisioning/wifi_scan.h"
#include "yk_wifi_creds.h"

static const char *TAG = "yk_wifi";

#define CONNECTED_BIT BIT0
#define SCAN_DONE_BIT BIT1
#define MAX_RESULTS 16
#define QUICK_ATTEMPTS 4      // fast retries after new credentials or a dropped connection
#define MAX_BACKOFF_S 300

static EventGroupHandle_t events;
static SemaphoreHandle_t lock;
static esp_netif_t *sta;
static esp_timer_handle_t connect_timer;
static volatile yk_wifi_state_t state = YK_WIFI_OFF;
static wifi_prov_sta_fail_reason_t fail_reason;
static int attempts_left, backoff_s;
static bool scanning;
static wifi_ap_record_t results[MAX_RESULTS];
static uint16_t result_count;
static char ip[16];

static const char *const STATE_NAMES[] = {"off", "unconfigured", "connecting", "connected", "failed"};

/** Overwrites credentials in a way the compiler keeps, even for a buffer about to go out of scope. */
static void wipe(void *p, size_t len) {
    volatile uint8_t *bytes = p;
    while (len--) *bytes++ = 0;
}

static bool configured(void) {
    wifi_config_t cfg;
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0];
}

static void connect_later(int ms) {
    esp_timer_stop(connect_timer);
    esp_timer_start_once(connect_timer, (uint64_t)ms * 1000);
}

// The channel the network was last joined on (NVS yk_wifi/chan): the next join scans that channel first and stops at
// the first match, instead of listening on all 13 for the strongest (about 2.5 s at every start, an app switch
// included). A network that moved is still found: the scan goes on to the other channels.
static esp_timer_handle_t hint_timer;
static uint8_t hint_seen;

static uint8_t hint_load(void) {
    uint8_t ch = 0;
    nvs_handle_t h;
    if (nvs_open("yk_wifi", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "chan", &ch);
        nvs_close(h);
    }
    return ch >= 1 && ch <= 14 ? ch : 0;
}

/** In the timer task (its stack takes an NVS write; the event task's does not). */
static void on_hint_timer(void *arg) {
    (void)arg;
    const uint8_t ch = hint_seen;
    if (!ch || ch == hint_load()) return;
    nvs_handle_t h;
    if (nvs_open("yk_wifi", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "chan", ch) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/** Another network, or none: its channel is not known. */
static void hint_drop(void) {
    hint_seen = 0;
    nvs_handle_t h;
    if (nvs_open("yk_wifi", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, "chan") == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void hint_apply(void) {
    const uint8_t ch = hint_load();
    wifi_config_t c;
    if (!ch || esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return;
    if (c.sta.channel != ch || c.sta.scan_method != WIFI_FAST_SCAN) {
        c.sta.channel = ch;
        c.sta.scan_method = WIFI_FAST_SCAN;
        esp_wifi_set_config(WIFI_IF_STA, &c);
    }
    wipe(&c, sizeof(c));
}

static void on_connect_timer(void *arg) {
    (void)arg;
    if (scanning || state == YK_WIFI_CONNECTED || !configured()) return;
    hint_apply();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
}

static bool auth_failure(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return true;
    default:
        return false;
    }
}

static int by_signal(const void *a, const void *b) {
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

/** Keeps the strongest access point per network name; hidden networks are dropped. */
static void collect_results(void) {
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *all = n ? calloc(n, sizeof(*all)) : NULL;
    if (n && !all) n = 0;
    if (esp_wifi_scan_get_ap_records(&n, all) != ESP_OK) n = 0;
    if (n) qsort(all, n, sizeof(*all), by_signal);
    xSemaphoreTake(lock, portMAX_DELAY);
    result_count = 0;
    for (uint16_t i = 0; i < n && result_count < MAX_RESULTS; i++) {
        if (!all[i].ssid[0]) continue;
        bool seen = false;
        for (uint16_t j = 0; j < result_count && !seen; j++)
            seen = !strcmp((const char *)results[j].ssid, (const char *)all[i].ssid);
        if (!seen) results[result_count++] = all[i];
    }
    xSemaphoreGive(lock);
    free(all);
    if (!n) esp_wifi_clear_ap_list();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got = data;
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&got->ip_info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            hint_seen = ap.primary;
            esp_timer_start_once(hint_timer, 1000);
        }
        state = YK_WIFI_CONNECTED;
        backoff_s = 0;
        xEventGroupSetBits(events, CONNECTED_BIT);
        ESP_LOGI(TAG, "connected");
        return;
    }
    if (base != WIFI_EVENT) return;
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (configured()) {
            state = YK_WIFI_CONNECTING;
            attempts_left = QUICK_ATTEMPTS;
            connect_later(10);
        }
        break;
    case WIFI_EVENT_SCAN_DONE:
        collect_results();
        scanning = false;
        xEventGroupSetBits(events, SCAN_DONE_BIT);
        if (state == YK_WIFI_CONNECTING || state == YK_WIFI_FAILED) connect_later(10);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *event = data;
        xEventGroupClearBits(events, CONNECTED_BIT);
        ip[0] = 0;
        // Our own disconnect (new credentials, scan or forget) is not a failed attempt.
        if (event->reason == WIFI_REASON_ASSOC_LEAVE || scanning || !configured()) break;
        if (state == YK_WIFI_CONNECTED) {
            state = YK_WIFI_CONNECTING;
            attempts_left = QUICK_ATTEMPTS;
        }
        if (state == YK_WIFI_CONNECTING && attempts_left-- > 0) {
            connect_later(1000);
            break;
        }
        if (state != YK_WIFI_FAILED) ESP_LOGW(TAG, "could not connect (reason %d)", event->reason);
        fail_reason = auth_failure(event->reason) ? WIFI_PROV_STA_AUTH_ERROR : WIFI_PROV_STA_AP_NOT_FOUND;
        state = YK_WIFI_FAILED;
        backoff_s = backoff_s ? (backoff_s * 2 > MAX_BACKOFF_S ? MAX_BACKOFF_S : backoff_s * 2) : 15;
        connect_later(backoff_s * 1000);
        break;
    }
    default:
        break;
    }
}

esp_err_t yk_wifi_reconnect(const char *why) {
    if (state != YK_WIFI_CONNECTED || scanning) return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "connected but stuck (%s): joining again", why ? why : "?");
    state = YK_WIFI_CONNECTING;
    attempts_left = QUICK_ATTEMPTS;
    xEventGroupClearBits(events, CONNECTED_BIT);
    ip[0] = 0;
    esp_wifi_disconnect();  // our own leave: the event handler does not count it, the timer joins again
    connect_later(500);
    return ESP_OK;
}

esp_err_t yk_wifi_start(void) {
    if (state != YK_WIFI_OFF) return ESP_OK;
    events = xEventGroupCreate();
    lock = xSemaphoreCreateMutex();
    if (!events || !lock) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, "yaykit");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    const esp_timer_create_args_t timer = {.callback = on_connect_timer, .name = "yk_wifi"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &connect_timer));
    const esp_timer_create_args_t hint = {.callback = on_hint_timer, .name = "yk_wifi_hint"};
    ESP_ERROR_CHECK(esp_timer_create(&hint, &hint_timer));
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    state = configured() ? YK_WIFI_CONNECTING : YK_WIFI_UNCONFIGURED;
    // Modem sleep is required while BLE shares the radio.
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return ESP_OK;
}

yk_wifi_state_t yk_wifi_state(void) { return state; }

const char *yk_wifi_state_name(void) { return STATE_NAMES[state]; }

bool yk_wifi_wait_connected(uint32_t timeout_ms) {
    if (!events) return false;
    return xEventGroupWaitBits(events, CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & CONNECTED_BIT;
}

void yk_wifi_describe(cJSON *wifi) {
    cJSON_AddStringToObject(wifi, "state", yk_wifi_state_name());
    if (state == YK_WIFI_FAILED)
        cJSON_AddStringToObject(wifi, "error", fail_reason == WIFI_PROV_STA_AUTH_ERROR ? "WRONG_PASSWORD" : "NOT_FOUND");
    if (state != YK_WIFI_CONNECTED) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(wifi, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    }
    if (ip[0]) cJSON_AddStringToObject(wifi, "ip", ip);
}

// ---- prov-scan ----

static esp_err_t scan_start(bool blocking, bool passive, uint8_t group_channels, uint32_t period_ms,
                            wifi_prov_scan_ctx_t **ctx) {
    (void)group_channels, (void)ctx;
    if (state == YK_WIFI_OFF) return ESP_ERR_INVALID_STATE;
    if (scanning) return ESP_OK;
    // A connection attempt in progress blocks scanning; it resumes after the scan.
    scanning = true;
    if (state != YK_WIFI_CONNECTED) {
        esp_timer_stop(connect_timer);
        esp_wifi_disconnect();
    }
    xEventGroupClearBits(events, SCAN_DONE_BIT);
    wifi_scan_config_t cfg = {.show_hidden = false};
    if (passive) {
        cfg.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        cfg.scan_time.passive = period_ms ? period_ms : 360;
    } else {
        // The driver's own active times (0/0): with Bluetooth on (the phone link, which is how the App asks for the
        // list) it shares the radio with BLE and wants them ("Should use default active scan time parameter…").
        // Measured with the link on: ~3.3 s for the 13 channels.
        cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        (void)period_ms;
    }
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        scanning = false;
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return err;
    }
    if (blocking) xEventGroupWaitBits(events, SCAN_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    return ESP_OK;
}

static esp_err_t scan_status(bool *finished, uint16_t *count, wifi_prov_scan_ctx_t **ctx) {
    (void)ctx;
    *finished = !scanning;
    *count = scanning ? 0 : result_count;
    return ESP_OK;
}

static esp_err_t scan_result(uint16_t index, wifi_prov_scan_result_t *out, wifi_prov_scan_ctx_t **ctx) {
    (void)ctx;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_FAIL;
    if (index < result_count) {
        const wifi_ap_record_t *r = &results[index];
        memcpy(out->ssid, r->ssid, sizeof(out->ssid));
        memcpy(out->bssid, r->bssid, sizeof(out->bssid));
        out->channel = r->primary;
        out->rssi = r->rssi;
        out->auth = r->authmode;
        err = ESP_OK;
    }
    xSemaphoreGive(lock);
    return err;
}

static wifi_prov_scan_handlers_t scan_handlers = {
    .scan_start = scan_start, .scan_status = scan_status, .scan_result = scan_result};

// ---- prov-config ----

static wifi_config_t pending;
static bool pending_set;

static esp_err_t get_status(wifi_prov_config_get_data_t *out, wifi_prov_ctx_t **ctx) {
    (void)ctx;
    memset(out, 0, sizeof(*out));
    switch (state) {
    case YK_WIFI_CONNECTED: {
        out->wifi_state = WIFI_PROV_STA_CONNECTED;
        snprintf(out->conn_info.ip_addr, sizeof(out->conn_info.ip_addr), "%s", ip);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(out->conn_info.bssid, ap.bssid, sizeof(ap.bssid));
            memcpy(out->conn_info.ssid, ap.ssid, sizeof(ap.ssid));
            out->conn_info.channel = ap.primary;
            out->conn_info.auth_mode = ap.authmode;
        }
        break;
    }
    case YK_WIFI_FAILED:
    case YK_WIFI_UNCONFIGURED:
        out->wifi_state = WIFI_PROV_STA_DISCONNECTED;
        out->fail_reason = fail_reason;
        break;
    default:
        out->wifi_state = WIFI_PROV_STA_CONNECTING;
        out->connecting_info.attempts_remaining = attempts_left > 0 ? attempts_left : 0;
        break;
    }
    return ESP_OK;
}

static esp_err_t set_config(const wifi_prov_config_set_data_t *in, wifi_prov_ctx_t **ctx) {
    (void)ctx;
    memset(&pending, 0, sizeof(pending));
    memcpy(pending.sta.ssid, in->ssid, strnlen(in->ssid, sizeof(pending.sta.ssid)));
    strlcpy((char *)pending.sta.password, in->password, sizeof(pending.sta.password));
    pending.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    pending.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    pending_set = true;
    ESP_LOGI(TAG, "new network received");
    return ESP_OK;
}

/** Stores new credentials (esp_wifi keeps them in NVS) and connects with them; wipes `config`. */
static esp_err_t join(wifi_config_t *config) {
    esp_timer_stop(connect_timer);
    esp_wifi_disconnect();
    hint_drop();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, config);
    wipe(config, sizeof(*config));
    if (err != ESP_OK) return err;
    state = YK_WIFI_CONNECTING;
    attempts_left = QUICK_ATTEMPTS;
    backoff_s = 0;
    // Let the reply reach the phone before the radio retunes.
    connect_later(500);
    return ESP_OK;
}

static esp_err_t apply_config(wifi_prov_ctx_t **ctx) {
    (void)ctx;
    if (!pending_set || state == YK_WIFI_OFF) return ESP_ERR_INVALID_STATE;
    pending_set = false;
    const esp_err_t err = join(&pending);  // the driver keeps its own copy
    memset(&pending, 0, sizeof(pending));
    return err;
}

static wifi_prov_config_handlers_t config_handlers = {
    .get_status_handler = get_status, .set_config_handler = set_config, .apply_config_handler = apply_config};

void *yk_wifi_scan_handlers(void) { return &scan_handlers; }
void *yk_wifi_config_handlers(void) { return &config_handlers; }

// ---- Yaykit Link commands ----

/** What a client needs to offer a network: "open", "password", or "unsupported" (enterprise sign-in, DPP). */
static const char *auth_kind(wifi_auth_mode_t mode) {
    switch (mode) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_OWE:
        return "open";
    case WIFI_AUTH_WEP:
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WAPI_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
        return "password";
    default:
        return "unsupported";
    }
}

static esp_err_t scan_command(cJSON *reply, const char **code) {
    // A scan the phone started is still running: use its results.
    bool ok = true;
    if (scanning) xEventGroupWaitBits(events, SCAN_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    else ok = scan_start(true, false, 0, 0, NULL) == ESP_OK;
    if (!ok || scanning) {
        *code = "SCAN_FAILED";
        return ESP_FAIL;
    }
    cJSON *networks = cJSON_AddArrayToObject(reply, "networks");
    xSemaphoreTake(lock, portMAX_DELAY);
    for (uint16_t i = 0; i < result_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (const char *)results[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", results[i].rssi);
        cJSON_AddStringToObject(network, "auth", auth_kind(results[i].authmode));
        cJSON_AddItemToArray(networks, network);
    }
    xSemaphoreGive(lock);
    return ESP_OK;
}

static esp_err_t join_command(const cJSON *args, cJSON *reply, const char **code) {
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(args, "password");
    const char *error = yk_wifi_join_error(cJSON_IsString(ssid) ? ssid->valuestring : NULL,
                                           cJSON_IsString(password) ? password->valuestring : NULL);
    if (password && !cJSON_IsString(password)) error = "BAD_PASSWORD";
    if (error) {
        *code = error;
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid->valuestring, strlen(ssid->valuestring));
    if (password) memcpy(config.sta.password, password->valuestring, strlen(password->valuestring));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_LOGI(TAG, "new network received");
    esp_err_t err = join(&config);
    if (err != ESP_OK) {
        *code = "JOIN_FAILED";
        return err;
    }
    cJSON_AddStringToObject(reply, "state", yk_wifi_state_name());
    cJSON_AddStringToObject(reply, "ssid", ssid->valuestring);
    return ESP_OK;
}

esp_err_t yk_wifi_command(const char *name, const cJSON *args, cJSON *reply, const char **code) {
    bool scan = !strcmp(name, "wifi.scan"), join_cmd = !strcmp(name, "wifi.join");
    if (!scan && !join_cmd) return ESP_ERR_NOT_FOUND;
    if (state == YK_WIFI_OFF) {
        *code = "WIFI_OFF";
        return ESP_ERR_INVALID_STATE;
    }
    return scan ? scan_command(reply, code) : join_command(args, reply, code);
}

// ---- console ----

static int wifi_cmd(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "scan")) {
        if (scan_start(true, false, 0, 0, NULL) != ESP_OK) {
            printf("YK_WIFI_SCAN error\n");
            return 1;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        for (uint16_t i = 0; i < result_count; i++)
            printf("YK_WIFI_AP rssi=%d channel=%d auth=%d ssid=%s\n", results[i].rssi, results[i].primary,
                   results[i].authmode, (const char *)results[i].ssid);
        printf("YK_WIFI_SCAN count=%u\n", (unsigned)result_count);
        xSemaphoreGive(lock);
        return 0;
    }
    if (!strcmp(sub, "forget")) {
        esp_timer_stop(connect_timer);
        wifi_config_t empty = {0};
        esp_wifi_disconnect();
        hint_drop();
        esp_wifi_set_config(WIFI_IF_STA, &empty);
        state = YK_WIFI_UNCONFIGURED;
    }
    cJSON *wifi = cJSON_CreateObject();
    yk_wifi_describe(wifi);
    char *text = cJSON_PrintUnformatted(wifi);
    printf("YK_WIFI %s\n", text);
    cJSON_free(text);
    cJSON_Delete(wifi);
    return 0;
}

esp_err_t yk_wifi_register_console(void) {
    const esp_console_cmd_t cmd = {
        .command = "wifi", .help = "wifi status | scan | forget", .func = wifi_cmd};
    return esp_console_cmd_register(&cmd);
}

void yk_wifi_drop_pending(void) {
    memset(&pending, 0, sizeof(pending));
    pending_set = false;
}

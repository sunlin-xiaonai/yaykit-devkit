#include "yaykit_link.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_srp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "mbedtls/sha256.h"
#include "protocomm.h"
#include "protocomm_ble.h"
#include "protocomm_security.h"
#include "protocomm_security2.h"
#include "sdkconfig.h"
#include "wifi_provisioning/wifi_config.h"
#include "wifi_provisioning/wifi_scan.h"
#include "yaykit_slots.h"
#include "yk_mem.h"
#include "yk_line.h"
#include "yk_link_priv.h"
#include "yk_srp_salt.h"

static const char *TAG = "yk_link";

volatile bool yk_link_keep_running;  // yk_link_priv.h

#define YK_USERNAME "wifiprov"
#define YK_SALT_LEN 16
#define YK_HIDDEN_ARGS_MAX 384     // {"ssid":32 bytes,"password":64 bytes} with room for JSON escapes
#define YK_CMD_ARGS_MAX 1024       // a command's JSON on the line: launcher.install with a store URL is ~500
#define YK_HIDDEN_ARGS_WAIT_MS 10000
// NimBLE's host task runs on this core. Stopping NimBLE from the other core can crash it: its event queue is
// emptied and refilled under a spinlock that only holds this core, so the host task can take an event in between
// (assert in npl_freertos_eventq_remove). The stop always runs here.
#ifdef CONFIG_BT_NIMBLE_PINNED_TO_CORE
#define YK_NIMBLE_CORE CONFIG_BT_NIMBLE_PINNED_TO_CORE
#else
#define YK_NIMBLE_CORE 0
#endif

// 59414b49-542d-4c69-6e6b-2d5631000000 ("YAKIT-Link-V1"), least significant byte first.
static const uint8_t service_uuid[16] = {0x00, 0x00, 0x00, 0x31, 0x56, 0x2d, 0x6b, 0x6e,
                                         0x69, 0x4c, 0x2d, 0x54, 0x49, 0x4b, 0x41, 0x59};
// Standard provisioning UUIDs first, then Yaykit's. Wi-Fi endpoints only exist when configured.
static protocomm_ble_name_uuid_t endpoints[] = {
    {"prov-session", 0xFF51}, {"proto-ver", 0xFF53}, {"yk-info", 0xFF60},
    {"yk-cmd", 0xFF61},       {"prov-scan", 0xFF50},  {"prov-config", 0xFF52},
};

static yk_link_config_t cfg;
static char device_id[16];
static SemaphoreHandle_t lock;
static protocomm_t *pc;
static bool active, connected;
static char ble_name[12], code[9], qr[192];
static char *salt, *verifier;
static protocomm_security2_params_t sec2;
static esp_timer_handle_t idle_timer;
static volatile bool held;  // yk_link_ble_hold(): no idle stop

static void emit(yk_link_event_t event) {
    if (cfg.on_event) cfg.on_event(event, cfg.ctx);
}

static void touch(void) {
    if (!idle_timer || !active) return;
    esp_timer_stop(idle_timer);
    if (!held) esp_timer_start_once(idle_timer, (uint64_t)cfg.idle_seconds * 1000000ULL);
}

void yk_link_ble_hold(bool hold) {
    if (held == hold) return;
    held = hold;
    ESP_LOGI(TAG, "BLE %s", hold ? "held on" : "may idle again");
    touch();
}

static void stop_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "BLE idle for %d s; stopping", cfg.idle_seconds);
    yk_link_ble_stop();
    vTaskDelete(NULL);
}

static void on_idle(void *arg) {
    (void)arg;
    // Stopping NimBLE blocks; do it outside the timer task, on NimBLE's core.
    xTaskCreatePinnedToCore(stop_task, "yk_stop", 4096, NULL, 5, NULL, YK_NIMBLE_CORE);
}

static void on_transport(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        connected = id == PROTOCOMM_TRANSPORT_BLE_CONNECTED;
        touch();
        emit(connected ? YK_LINK_BLE_CONNECTED : YK_LINK_BLE_DISCONNECTED);
    } else if (base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) ESP_LOGI(TAG, "secure session ready");
        else ESP_LOGW(TAG, "secure session rejected (%ld)", (long)id);
    }
}

/** Why the chip last started, for diagnostics (power-on, panic, watchdog, brownout...). */
static const char *reset_reason(void) {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
    }
}

const char *yk_link_device_id(void) { return device_id; }
bool yk_link_ble_active(void) { return active; }
bool yk_link_ble_connected(void) { return active && connected; }
const char *yk_link_ble_name(void) { return active ? ble_name : ""; }
const char *yk_link_pairing_code(void) { return active ? code : ""; }
const char *yk_link_pairing_qr(void) { return active ? qr : ""; }

char *yk_link_info_json(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddNumberToObject(root, "proto", 1);

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    cJSON_AddStringToObject(device, "id", device_id);
    cJSON_AddStringToObject(device, "board", cfg.board);
    cJSON_AddStringToObject(device, "chip", "ESP32-S3");
    cJSON_AddNumberToObject(device, "flash", flash);
    cJSON_AddNumberToObject(device, "psram", esp_psram_is_initialized() ? esp_psram_get_size() : 0);

    cJSON *app = cJSON_AddObjectToObject(root, "app");
    cJSON_AddStringToObject(app, "slug", cfg.app_slug);
    cJSON_AddStringToObject(app, "version", cfg.app_version);
    cJSON_AddStringToObject(app, "layout", cfg.layout);
    cJSON *slots = cJSON_AddArrayToObject(root, "slots");
    // Only the yaykit-v1 layout has app slots.
    if (!strcmp(cfg.layout, "yaykit-v1")) yk_slots_to_json(slots);
    if (cfg.describe_recovery) {
        cJSON *recovery = cJSON_CreateObject();
        if (recovery && cfg.describe_recovery(recovery)) cJSON_AddItemToObject(root, "recovery", recovery);
        else cJSON_Delete(recovery);
    }

    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "internalFree", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    // Since the app was ready: the 40 KB line; from boot: the figure before 10-24, while the two are compared (yk_mem.h).
    cJSON_AddNumberToObject(heap, "internalMinSinceReady", yk_mem_min_since_ready());
    cJSON_AddNumberToObject(heap, "internalMinEver", yk_mem_min_ever());
    cJSON_AddNumberToObject(heap, "internalLargest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "psramFree", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (cfg.describe_heap) cfg.describe_heap(heap);

    cJSON *ble = cJSON_AddObjectToObject(root, "ble");
    cJSON_AddStringToObject(ble, "state", !active ? "off" : connected ? "connected" : "advertising");
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    if (cfg.describe_wifi) cfg.describe_wifi(wifi);
    else cJSON_AddStringToObject(wifi, "state", cfg.wifi ? "unknown" : "off");
    if (cfg.describe_account) cfg.describe_account(cJSON_AddObjectToObject(root, "account"));
    if (cfg.capabilities) {
        cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
        for (const char *const *c = cfg.capabilities; *c; c++) cJSON_AddItemToArray(caps, cJSON_CreateString(*c));
    }
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(root, "resetReason", reset_reason());

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

/** Wipes every string in a parsed request before it is freed: command arguments may carry a Wi-Fi password. */
static void scrub(cJSON *item) {
    for (; item; item = item->next) {
        if (cJSON_IsString(item) && item->valuestring) yk_line_wipe(item->valuestring, strlen(item->valuestring));
        scrub(item->child);
    }
}

/** Runs a command and returns {"ok":true,"result":{...}} or {"ok":false,"error":"CODE"}. */
static char *run_command(const char *name, const cJSON *args) {
    cJSON *out = cJSON_CreateObject();
    cJSON *result = cJSON_CreateObject();
    const char *error = "UNKNOWN_COMMAND";
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (!name || !*name) error = "BAD_REQUEST";
    else if (cfg.on_command) err = cfg.on_command(name, args, result, &error, cfg.ctx);
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    if (err == ESP_OK) cJSON_AddItemToObject(out, "result", result);
    else {
        cJSON_Delete(result);
        cJSON_AddStringToObject(out, "error", error ? error : "COMMAND_FAILED");
    }
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return text;
}

// A BLE attribute carries at most 512 bytes and Security 2 adds a 16-byte tag; protocomm rejects anything
// longer, and the client's message counter then no longer matches. Longer replies go out in parts: the
// first is "\x1e<total length>\n" plus the first bytes, and the client fetches the rest with
// {"name":"link.more"} on yk-cmd before sending anything else. Requests arrive one at a time.
#define BLE_PART 448
static char *pending;
static size_t pending_len, pending_sent;

static void drop_pending(void) {
    free(pending);
    pending = NULL;
    pending_len = pending_sent = 0;
}

/** Hands `text` (malloc'd) to protocomm, splitting it when it is longer than one part. */
static esp_err_t ble_reply(char *text, uint8_t **out, ssize_t *out_len) {
    drop_pending();
    if (!text) return ESP_ERR_NO_MEM;
    size_t len = strlen(text);
    if (len <= BLE_PART) {
        *out = (uint8_t *)text;
        *out_len = len;
        return ESP_OK;
    }
    char header[16];
    int header_len = snprintf(header, sizeof(header), "\x1e%u\n", (unsigned)len);
    size_t first = BLE_PART - header_len;
    uint8_t *part = malloc(BLE_PART);
    if (!part) {
        free(text);
        return ESP_ERR_NO_MEM;
    }
    memcpy(part, header, header_len);
    memcpy(part + header_len, text, first);
    pending = text;
    pending_len = len;
    pending_sent = first;
    *out = part;
    *out_len = BLE_PART;
    return ESP_OK;
}

/** The next part of a long reply, or an error reply when none is waiting. */
static esp_err_t ble_more(uint8_t **out, ssize_t *out_len) {
    if (!pending) return ble_reply(strdup("{\"ok\":false,\"error\":\"NO_MORE\"}"), out, out_len);
    size_t n = pending_len - pending_sent < BLE_PART ? pending_len - pending_sent : BLE_PART;
    uint8_t *part = malloc(n);
    if (!part) return ESP_ERR_NO_MEM;
    memcpy(part, pending + pending_sent, n);
    pending_sent += n;
    if (pending_sent == pending_len) drop_pending();
    *out = part;
    *out_len = n;
    return ESP_OK;
}

static esp_err_t info_endpoint(uint32_t session, const uint8_t *in, ssize_t in_len, uint8_t **out, ssize_t *out_len,
                               void *priv) {
    (void)session, (void)in, (void)in_len, (void)priv;
    touch();
    return ble_reply(yk_link_info_json(), out, out_len);
}

static esp_err_t command_endpoint(uint32_t session, const uint8_t *in, ssize_t in_len, uint8_t **out,
                                  ssize_t *out_len, void *priv) {
    (void)session, (void)priv;
    touch();
    cJSON *request = in && in_len > 0 ? cJSON_ParseWithLength((const char *)in, in_len) : NULL;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    if (cJSON_IsString(name) && !strcmp(name->valuestring, "link.more")) {
        cJSON_Delete(request);
        return ble_more(out, out_len);
    }
    char *text = run_command(cJSON_IsString(name) ? name->valuestring : NULL,
                             cJSON_GetObjectItemCaseSensitive(request, "args"));
    scrub(request);
    cJSON_Delete(request);
    return ble_reply(text, out, out_len);
}

static void release_session_material(void) {
    free(salt);
    free(verifier);
    salt = verifier = NULL;
}

esp_err_t yk_link_ble_start(void) {
    if (yk_link_keep_running) return ESP_ERR_INVALID_STATE;  // the long mode has NimBLE (yk_link_keep.h)
    xSemaphoreTake(lock, portMAX_DELAY);
    if (active) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    snprintf(code, sizeof(code), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    snprintf(ble_name, sizeof(ble_name), "YK-%.4s", device_id + strlen(device_id) - 4);
    int verifier_len = 0;
    salt = malloc(YK_SRP_SALT_LEN);
    esp_err_t err = salt ? yk_srp_salt_verifier(YK_USERNAME, code, (uint8_t *)salt, &verifier, &verifier_len)
                         : ESP_ERR_NO_MEM;
    // For matching a rejected session with what the phone computed (a short verifier means a leading zero byte).
    ESP_LOGI(TAG, "pairing code of %u digits, salt %d bytes (first %02x), verifier %d bytes (%s)", (unsigned)strlen(code),
             YK_SALT_LEN, salt ? (uint8_t)salt[0] : 0, verifier_len, esp_err_to_name(err));
    if (err == ESP_OK) {
        sec2 = (protocomm_security2_params_t){
            .salt = salt, .salt_len = YK_SALT_LEN, .verifier = verifier, .verifier_len = (uint16_t)verifier_len};
        pc = protocomm_new();
        err = pc ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = protocomm_set_security(pc, "prov-session", &protocomm_security2, &sec2);
    // Clients pick the AES-GCM nonce scheme from sec_patch_ver (patch 1 counts every message),
    // so report what protocomm actually runs instead of a fixed number.
    int sec_ver = 2;
    uint8_t sec_patch = 0;
    if (err == ESP_OK) err = protocomm_get_sec_version(pc, &sec_ver, &sec_patch);
    bool wifi = cfg.wifi_scan_handlers && cfg.wifi_config_handlers;
    char version[192];
    snprintf(version, sizeof(version),
             "{\"prov\":{\"ver\":\"v1.1\",\"sec_ver\":%d,\"sec_patch_ver\":%u,\"cap\":[%s]},"
             "\"yk\":{\"proto\":1,\"app\":\"%s\",\"part\":%d}}",
             sec_ver, (unsigned)sec_patch, wifi ? "\"wifi_scan\"" : "", cfg.app_slug, BLE_PART);
    if (err == ESP_OK) err = protocomm_set_version(pc, "proto-ver", version);
    if (err == ESP_OK) err = protocomm_add_endpoint(pc, "yk-info", info_endpoint, NULL);
    if (err == ESP_OK) err = protocomm_add_endpoint(pc, "yk-cmd", command_endpoint, NULL);
    if (err == ESP_OK && wifi)
        err = protocomm_add_endpoint(pc, "prov-scan", wifi_prov_scan_handler, cfg.wifi_scan_handlers);
    if (err == ESP_OK && wifi)
        err = protocomm_add_endpoint(pc, "prov-config", wifi_prov_config_data_handler, cfg.wifi_config_handlers);
    if (err == ESP_OK) {
        protocomm_ble_config_t ble = {
            .nu_lookup_count = sizeof(endpoints) / sizeof(endpoints[0]) - (wifi ? 0 : 2),
            .nu_lookup = endpoints,
        };
        strlcpy(ble.device_name, ble_name, sizeof(ble.device_name));
        memcpy(ble.service_uuid, service_uuid, sizeof(service_uuid));
        err = protocomm_ble_start(pc, &ble);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
        if (pc) protocomm_delete(pc);
        pc = NULL;
        release_session_material();
        xSemaphoreGive(lock);
        emit(YK_LINK_BLE_STOPPED);  // whoever waits for the link hears it is not coming
        return err;
    }
    snprintf(qr, sizeof(qr),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"username\":\"" YK_USERNAME
             "\",\"pop\":\"%s\",\"transport\":\"ble\",\"security\":2}",
             ble_name, code);
    active = true;
    connected = false;
    touch();
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "advertising %s", ble_name);
    emit(YK_LINK_BLE_ADVERTISING);
    return ESP_OK;
}

static esp_err_t ble_stop_here(void);

static void stop_on_core(void *arg) {
    ble_stop_here();
    xSemaphoreGive((SemaphoreHandle_t)arg);
    vTaskDelete(NULL);
}

esp_err_t yk_link_ble_stop(void) {
    // From any task: the stop runs in a task pinned to NimBLE's core, unless this one is (an unpinned task may
    // move to the other core at any time).
    if (xTaskGetCoreID(NULL) != YK_NIMBLE_CORE) {
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        if (done && xTaskCreatePinnedToCore(stop_on_core, "yk_stop", 4096, done, uxTaskPriorityGet(NULL), NULL,
                                            YK_NIMBLE_CORE) == pdPASS) {
            xSemaphoreTake(done, portMAX_DELAY);
            vSemaphoreDelete(done);
            return ESP_OK;
        }
        if (done) vSemaphoreDelete(done);
        ESP_LOGW(TAG, "stopping BLE without a task on core %d", YK_NIMBLE_CORE);
    }
    return ble_stop_here();
}

static esp_err_t ble_stop_here(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!active) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    esp_timer_stop(idle_timer);
    protocomm_ble_stop(pc);
    protocomm_delete(pc);
    pc = NULL;
    drop_pending();
    active = connected = false;
    release_session_material();
    memset(code, 0, sizeof(code));
    memset(qr, 0, sizeof(qr));
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "BLE stopped");
    emit(YK_LINK_BLE_STOPPED);
    return ESP_OK;
}

/**
 * `yk cmd <name> -`: prints YK_ARGS, then reads the JSON arguments from the next line itself, so linenoise never
 * echoes it or keeps it in the history. NULL on success, else the error code to print.
 */
static const char *read_hidden_args(char *json, size_t cap) {
    puts("YK_ARGS");
    fflush(stdout);
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    yk_line_t line;
    yk_line_init(&line, json, cap);
    yk_line_result_t got = YK_LINE_MORE;
    int64_t deadline = esp_timer_get_time() + (int64_t)YK_HIDDEN_ARGS_WAIT_MS * 1000;
    while (got == YK_LINE_MORE && esp_timer_get_time() < deadline) {
        char c;
        if (read(fd, &c, 1) == 1) got = yk_line_feed(&line, c);
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
    fcntl(fd, F_SETFL, flags);
    if (got == YK_LINE_DONE) return NULL;
    yk_line_wipe(json, cap);
    return got == YK_LINE_TOO_LONG ? "TOO_LONG" : "ARGS_TIMEOUT";
}

static int console_yk(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "info")) {
        char *text = yk_link_info_json();
        printf("YK_INFO %s\n", text ? text : "{}");
        cJSON_free(text);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "cmd")) {
        // On the heap: the console task's stack is not the place for a kilobyte.
        char *json = calloc(1, YK_CMD_ARGS_MAX);
        if (!json) {
            puts("YK_ERR NO_MEMORY");
            return 1;
        }
        bool hidden = argc == 4 && !strcmp(argv[3], "-");
        bool too_long = false;
        if (hidden) {
            const char *error = read_hidden_args(json, YK_HIDDEN_ARGS_MAX);  // secrets: the short buffer, as before
            if (error) {
                free(json);
                printf("YK_ERR %s\n", error);
                return 1;
            }
        } else {
            // Arguments may have been split on spaces; join them back into one JSON text.
            for (int i = 3; i < argc && !too_long; i++) {
                if (i > 3) strlcat(json, " ", YK_CMD_ARGS_MAX);
                too_long = strlcat(json, argv[i], YK_CMD_ARGS_MAX) >= YK_CMD_ARGS_MAX;
            }
        }
        bool given = *json;
        cJSON *args = given && !too_long ? cJSON_Parse(json) : NULL;
        yk_line_wipe(json, YK_CMD_ARGS_MAX);
        free(json);
        if (given && !args) {
            puts(too_long ? "YK_ERR TOO_LONG" : "YK_ERR BAD_JSON");
            return 1;
        }
        if (!hidden && cJSON_GetObjectItemCaseSensitive(args, "password")) {
            // The console has already echoed this line; drop it from the history and refuse it.
            linenoiseHistoryFree();
            scrub(args);
            cJSON_Delete(args);
            puts("YK_ERR SECRET_IN_LINE");
            return 1;
        }
        char *text = run_command(argv[2], args);
        scrub(args);
        cJSON_Delete(args);
        cJSON *reply = text ? cJSON_Parse(text) : NULL;
        bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok"));
        if (ok) {
            char *result = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(reply, "result"));
            printf("YK_OK %s\n", result ? result : "{}");
            cJSON_free(result);
        } else {
            const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
            printf("YK_ERR %s\n", cJSON_IsString(error) ? error->valuestring : "COMMAND_FAILED");
        }
        cJSON_Delete(reply);
        cJSON_free(text);
        return ok ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "ble") && !strcmp(argv[2], "srptest")) {
        // Development: yk_srp_verifier() against esp_srp_gen_salt_verifier() on esp_srp's own salts. They agree
        // except where esp_srp's salt began with 0 (about 1 in 256): the case yk_srp_salt_verifier() avoids.
        const int n = argc > 3 ? atoi(argv[3]) : 20;
        int same = 0, differ = 0;
        for (int i = 0; i < n; i++) {
            char *s = NULL, *v1 = NULL, *v2 = NULL;
            int l1 = 0, l2 = 0;
            if (esp_srp_gen_salt_verifier(YK_USERNAME, strlen(YK_USERNAME), "12345678", 8, &s, YK_SALT_LEN, &v1, &l1) ==
                    ESP_OK &&
                yk_srp_verifier(YK_USERNAME, "12345678", (const uint8_t *)s, &v2, &l2) == ESP_OK) {
                if (l1 == l2 && !memcmp(v1, v2, (size_t)l1)) same++;
                else differ++;
            }
            free(s), free(v1), free(v2);
        }
        printf("YK_SRP n=%d same=%d differ=%d\n", n, same, differ);
        return differ ? 1 : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "ble")) {
        esp_err_t err = ESP_OK;
        if (!strcmp(argv[2], "on")) err = yk_link_ble_start();
        else if (!strcmp(argv[2], "off")) err = yk_link_ble_stop();
        if (err != ESP_OK) {
            printf("YK_ERR %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("YK_BLE state=%s name=%s code=%s qr=%s\n", !active ? "off" : connected ? "connected" : "advertising",
               yk_link_ble_name(), yk_link_pairing_code(), yk_link_pairing_qr());
        return 0;
    }
    puts("YK_ERR USAGE yk info | yk cmd <name> [json] | yk ble on|off|status");
    return 1;
}

esp_err_t yk_link_register_console(void) {
    const esp_console_cmd_t command = {
        .command = "yk",
        .help = "Yaykit Link: yk info | yk cmd <name> [json] | yk ble on|off|status",
        .func = console_yk,
    };
    return esp_console_cmd_register(&command);
}

esp_err_t yk_link_init(const yk_link_config_t *config) {
    if (!config || !config->board || !config->app_slug || !config->app_version || !config->layout)
        return ESP_ERR_INVALID_ARG;
    cfg = *config;
    if (cfg.idle_seconds <= 0) cfg.idle_seconds = 90;
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;

    uint8_t mac[6], digest[32];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    uint8_t seed[12];
    memcpy(seed, mac, 6);
    memcpy(seed + 6, "yaykit", 6);
    mbedtls_sha256(seed, sizeof(seed), digest, 0);
    strcpy(device_id, "yk_");
    for (int i = 0; i < 6; i++) sprintf(device_id + 3 + i * 2, "%02x", digest[i]);

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, on_transport, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, on_transport, NULL));
    const esp_timer_create_args_t timer = {.callback = on_idle, .name = "yk_idle"};
    return esp_timer_create(&timer, &idle_timer);
}

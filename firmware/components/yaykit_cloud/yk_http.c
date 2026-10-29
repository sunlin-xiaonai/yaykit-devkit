#include "yk_http.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "yaykit_link.h"
#include "yaykit_wifi.h"

// A file registration lists up to 16 signed part URLs (~700 bytes each).
#define BODY_MAX (32 * 1024)

static const char *TAG = "yk_http";
static const char *app_slug = "", *app_version = "";

void yk_http_set_app(const char *app, const char *version) {
    app_slug = app ? app : "";
    app_version = version ? version : "";
}

esp_err_t yk_http_prepare(const char **code) {
    // BLE (~100 KB of internal RAM) and a TLS session do not fit together; the phone has had its turn.
    if (yk_link_ble_active()) yk_link_ble_stop();
    if (!yk_wifi_wait_connected(20000)) {
        if (code) *code = "NO_WIFI";
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_http_client_handle_t yk_http_client(const char *path, esp_http_client_method_t method, const char *bearer,
                                        const char *content_type, int timeout_ms) {
    char url[512];
    snprintf(url, sizeof(url), "%s%s", CONFIG_YK_CLOUD_ORIGIN, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1536,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    esp_http_client_set_header(client, "X-Yaykit-App", app_slug);
    esp_http_client_set_header(client, "X-Yaykit-Firmware", app_version);
    esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
    if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
    if (bearer) {
        char auth[640];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    return client;
}

void yk_http_read_body(esp_http_client_handle_t client, yk_http_response_t *out) {
    char *buf = heap_caps_malloc(BODY_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    size_t len = 0;
    int n;
    while (len < BODY_MAX && (n = esp_http_client_read(client, buf + len, BODY_MAX - len)) > 0) len += n;
    buf[len] = 0;
    out->body = buf;
    out->len = len;
}

esp_err_t yk_http_json(esp_http_client_method_t method, const char *path, const char *bearer, const char *json,
                       yk_http_response_t *out) {
    return yk_http_send(method, path, bearer, json ? "application/json" : NULL, (const uint8_t *)json,
                        json ? strlen(json) : 0, 15000, out);
}

esp_err_t yk_http_send(esp_http_client_method_t method, const char *path, const char *bearer, const char *content_type,
                       const uint8_t *body, size_t body_len, int timeout_ms, yk_http_response_t *out) {
    memset(out, 0, sizeof(*out));
    esp_http_client_handle_t client = yk_http_client(path, method, bearer, content_type, timeout_ms);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    for (size_t sent = 0; err == ESP_OK && sent < body_len;) {
        int n = esp_http_client_write(client, (const char *)body + sent, (int)(body_len - sent > 4096 ? 4096 : body_len - sent));
        if (n <= 0) err = ESP_FAIL;
        else sent += (size_t)n;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        out->status = esp_http_client_get_status_code(client);
        yk_http_read_body(client, out);
    } else {
        ESP_LOGW(TAG, "%s failed: %s", path, esp_err_to_name(err));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

void yk_http_free(yk_http_response_t *response) {
    free(response->body);
    response->body = NULL;
    response->len = 0;
}

const char *yk_http_error_code(const yk_http_response_t *response) {
    static const char *const known[] = {
        "TOKEN_EXPIRED",         "DEVICE_NOT_ACTIVATED", "OPERATION_NOT_ALLOWED", "CREDITS_EXHAUSTED",
        "RATE_LIMITED",          "VOICE_BUSY",           "SERVICE_BUSY",          "CONTENT_BLOCKED",
        "DEVICE_OWNED_BY_OTHER", "CLAIM_INVALID",        "SIGNATURE_INVALID",     "APP_NOT_ENABLED",
        "CHALLENGE_INVALID",
    };
    const char *code = "SERVER";
    cJSON *json = response->body ? cJSON_Parse(response->body) : NULL;
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
    if (cJSON_IsString(error)) {
        if (!strncmp(error->valuestring, "AUDIO_", 6)) code = "AUDIO";
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
            if (!strcmp(error->valuestring, known[i])) code = known[i];
    }
    cJSON_Delete(json);
    return code;
}

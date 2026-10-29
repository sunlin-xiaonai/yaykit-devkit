// Internal HTTPS helpers of yaykit_cloud.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"

typedef struct {
    int status;
    char *body;  // NUL-terminated, PSRAM; NULL when empty
    size_t len;
} yk_http_response_t;

/** App identity sent with every request (X-Yaykit-App / X-Yaykit-Firmware). */
void yk_http_set_app(const char *app, const char *version);
/** Bluetooth off (HTTPS needs its internal RAM) and Wi-Fi up. Sets *code to NO_WIFI on failure. */
esp_err_t yk_http_prepare(const char **code);
/** A client for origin + path with the common headers; NULL when out of memory. */
esp_http_client_handle_t yk_http_client(const char *path, esp_http_client_method_t method, const char *bearer,
                                        const char *content_type, int timeout_ms);
/** Small JSON request (body may be NULL). Fills `out`; ESP_FAIL only when no response arrived. */
esp_err_t yk_http_json(esp_http_client_method_t method, const char *path, const char *bearer, const char *json,
                       yk_http_response_t *out);
/** Any request with a body of `content_type` (may be NULL); fills `out`. ESP_FAIL only when no response arrived. */
esp_err_t yk_http_send(esp_http_client_method_t method, const char *path, const char *bearer, const char *content_type,
                       const uint8_t *body, size_t body_len, int timeout_ms, yk_http_response_t *out);
/** Reads the rest of a response body (at most 32 KB) into `out`. */
void yk_http_read_body(esp_http_client_handle_t client, yk_http_response_t *out);
void yk_http_free(yk_http_response_t *response);
/** The API's {"error":"CODE"} as one of the stable codes in yaykit_cloud.h / yaykit_account.h, else "SERVER". */
const char *yk_http_error_code(const yk_http_response_t *response);

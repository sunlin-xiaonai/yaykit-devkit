// Host tests only (test_me.c): the client's types; test_me.c answers the calls (none is made on the /me path).
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#define ESP_ERR_HTTP_EAGAIN 0x7007
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE } esp_http_client_method_t;
typedef struct esp_http_client *esp_http_client_handle_t;
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    bool keep_alive_enable;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len);
int esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

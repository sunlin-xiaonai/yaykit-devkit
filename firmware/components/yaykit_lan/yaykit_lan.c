// Yaykit LAN (yaykit_lan.h). Grown from the selfie lab's sl_net.c (exp/selfie-lab, c527e06c): esp_http_server's
// WebSocket on its own task (stack in PSRAM), TCP_NODELAY so the small answers do not wait for Nagle.
#include "yaykit_lan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "yk_lan_logic.h"

static const char *TAG = "yk_lan";

static yk_lan_config_t s_cfg;
static httpd_handle_t s_server;
static SemaphoreHandle_t s_lock;  // the token
static char s_token[YK_LAN_TOKEN_LEN + 1];
static volatile int s_client = -1;  // the socket that said hello

bool yk_lan_running(void) { return s_server != NULL; }
bool yk_lan_client(void) { return s_client >= 0; }

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

void yk_lan_new_token(char out[YK_LAN_TOKEN_LEN + 1]) {
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    yk_lan_token_text(r, s_token);
    memcpy(out, s_token, sizeof(s_token));
    if (s_lock) xSemaphoreGive(s_lock);
    wipe(r, sizeof(r));
}

void yk_lan_close_client(void) {
    const int fd = s_client;
    if (fd >= 0 && s_server) httpd_sess_trigger_close(s_server, fd);
}

void yk_lan_forget_token(void) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    wipe(s_token, sizeof(s_token));
    if (s_lock) xSemaphoreGive(s_lock);
    yk_lan_close_client();
}

static esp_err_t send_now(httpd_req_t *req, const char *text) {
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text)};
    return httpd_ws_send_frame(req, &f);
}

typedef struct {
    int fd;
    size_t len;
    char text[];
} out_t;

static void send_work(void *arg) {
    out_t *o = arg;
    if (o->fd == s_client && s_server) {
        httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)o->text, .len = o->len};
        httpd_ws_send_frame_async(s_server, o->fd, &f);
    }
    free(o);
}

esp_err_t yk_lan_send(const char *text) {
    const int fd = s_client;
    if (fd < 0 || !s_server) return ESP_ERR_INVALID_STATE;
    const size_t len = strlen(text);
    out_t *o = malloc(sizeof(*o) + len + 1);
    if (!o) return ESP_ERR_NO_MEM;
    o->fd = fd;
    o->len = len;
    memcpy(o->text, text, len + 1);
    const esp_err_t err = httpd_queue_work(s_server, send_work, o);
    if (err != ESP_OK) free(o);
    return err;
}

static esp_err_t on_text(httpd_req_t *req, int fd, size_t len) {
    if (len > YK_LAN_TEXT_MAX) return ESP_FAIL;
    char buf[YK_LAN_TEXT_MAX + 1];
    httpd_ws_frame_t f = {.payload = (uint8_t *)buf};
    esp_err_t err = httpd_ws_recv_frame(req, &f, len);
    if (err != ESP_OK) return err;
    buf[f.len] = 0;
    if (fd == s_client) {
        if (s_cfg.on_text) s_cfg.on_text(buf, f.len, s_cfg.ctx);
        return ESP_OK;
    }
    // Not in yet: only hello.
    char name[48];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const yk_lan_hello_t hello = yk_lan_hello(buf, s_token, name, sizeof(name));
    xSemaphoreGive(s_lock);
    wipe(buf, sizeof(buf));  // the token
    if (hello != YK_LAN_HELLO_OK) {
        ESP_LOGW(TAG, "a client was refused (%s)", hello == YK_LAN_HELLO_BAD ? "token" : "no hello");
        send_now(req, "{\"t\":\"hello.err\",\"code\":\"TOKEN\"}");
        return ESP_FAIL;  // closes it
    }
    const int old = s_client;
    s_client = fd;
    if (old >= 0 && old != fd) httpd_sess_trigger_close(s_server, old);
    send_now(req, "{\"t\":\"hello.ok\"}");
    ESP_LOGI(TAG, "client in");
    if (s_cfg.on_hello) s_cfg.on_hello(name, s_cfg.ctx);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK;  // the handshake
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);  // the length first
    if (err != ESP_OK) return err;
    const int fd = httpd_req_to_sockfd(req);
    if (f.type == HTTPD_WS_TYPE_TEXT) return on_text(req, fd, f.len);
    if (f.type != HTTPD_WS_TYPE_BINARY) return ESP_OK;  // pings are answered by the server itself
    if (fd != s_client || f.len == 0 || f.len > s_cfg.max_binary) return ESP_FAIL;
    uint8_t *data = heap_caps_malloc(f.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return ESP_ERR_NO_MEM;
    f.payload = data;
    if ((err = httpd_ws_recv_frame(req, &f, f.len)) != ESP_OK) {
        heap_caps_free(data);
        return err;
    }
    if (s_cfg.on_binary) s_cfg.on_binary(data, f.len, s_cfg.ctx);
    else heap_caps_free(data);
    return ESP_OK;
}

static esp_err_t on_open(httpd_handle_t hd, int fd) {
    (void)hd;
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

static void on_close(httpd_handle_t hd, int fd) {
    (void)hd;
    if (fd == s_client) {
        s_client = -1;
        ESP_LOGI(TAG, "client gone");
        if (s_cfg.on_close) s_cfg.on_close(s_cfg.ctx);
    }
    close(fd);
}

esp_err_t yk_lan_start(const yk_lan_config_t *cfg) {
    if (s_server) return ESP_OK;
    if (!cfg || !cfg->path || !cfg->port || !cfg->max_binary) return ESP_ERR_INVALID_ARG;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_cfg = *cfg;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = cfg->port;
    c.ctrl_port = cfg->port + 1;
    c.max_open_sockets = 3;
    c.lru_purge_enable = true;
    c.stack_size = 6144;
    c.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // internal RAM is the scarce one
    c.open_fn = on_open;
    c.close_fn = on_close;
    esp_err_t err = httpd_start(&s_server, &c);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }
    const httpd_uri_t uri = {.uri = cfg->path, .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    err = httpd_register_uri_handler(s_server, &uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }
    ESP_LOGI(TAG, "listening on %u%s", cfg->port, cfg->path);
    return ESP_OK;
}

void yk_lan_stop(void) {
    if (!s_server) return;
    httpd_handle_t h = s_server;
    httpd_stop(h);  // closes every socket (on_close runs)
    s_server = NULL;
    s_client = -1;
}

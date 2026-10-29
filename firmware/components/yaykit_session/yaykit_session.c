#include "yaykit_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

static const char *TAG = "yk_session";

#define TOKEN_MAX 1024
#define FRAME_MAX (YK_PROTO_HEADER + 1276)
#define TEXT_MAX 16384  // the longest text frame taken (stt text is at most 2000 characters)
#define CONNECT_WAIT_MS 15000

// BIT_PEER_CLOSE: the server's close frame came (yk_session_close waits for it); BIT_GONE: the client said the
// connection went (BIT_DOWN is the task's, which clears it).
enum { BIT_UP = 1, BIT_DOWN = 2, BIT_KICK = 4, BIT_REAUTH = 8, BIT_PEER_CLOSE = 16, BIT_GONE = 32 };

static struct {
    yk_session_config_t cfg;
    bool usb;
    TaskHandle_t task;
    EventGroupHandle_t events;
    SemaphoreHandle_t send_lock;
    esp_websocket_client_handle_t ws;
    volatile yk_session_state_t state;
    uint8_t seq;
    char *text;  // text frame being put together (PSRAM)
    size_t text_len;
    uint8_t *frame;  // the microphone's outgoing frame (one sender), PSRAM
    int64_t connect_t0;
    volatile int attempt;  // failed connections since the last hello.ok
    volatile bool hold;    // yk_session_hold(true): closed until released
    volatile bool usb_peer;  // the USB bridge's side is up (`ys c` … `ys d`)
    bool closing;            // yk_session_close sent the close frame: nothing goes after it (under send_lock)
    yk_session_stats_t st;
} S;

const char *yk_session_state_name(yk_session_state_t s) {
    switch (s) {
    case YK_SESSION_IDLE: return "idle";
    case YK_SESSION_CONNECTING: return "connecting";
    case YK_SESSION_UP: return "up";
    case YK_SESSION_DOWN: return "down";
    }
    return "?";
}

static void set_state(yk_session_state_t s) {
    if (S.state == s) return;
    printf("YAY_NET session=%s\n", yk_session_state_name(s));
    S.state = s;
    if (S.cfg.on_state) S.cfg.on_state(s, S.cfg.ctx);
}

yk_session_state_t yk_session_state(void) { return S.state; }

// ---------------------------------------------------------------- receiving

static void got_text(const char *json, size_t len) {
    S.st.recv_text++;
    yk_srv_msg_t m;
    const yk_srv_type_t type = yk_proto_parse(json, len, &m);
    if (type == YK_SRV_HELLO_OK) S.attempt = 0;  // only now is the connection good: a server that takes the socket
                                                  // and closes it again must not be hammered every second
    if (type == YK_SRV_INVALID) {
        S.st.invalid++;
        ESP_LOGW(TAG, "ignored a message that does not fit the protocol: %.*s", len > 120 ? 120 : (int)len, json);
    } else if (type == YK_SRV_REAUTH) {
        // A new token between turns (4.1): fetched in the session's own task (HTTPS needs its stack).
        printf("YAY_NET reauth asked bytes=%u\n", (unsigned)len);
        xEventGroupSetBits(S.events, BIT_REAUTH);
    } else if (S.cfg.on_message) {
        S.cfg.on_message(&m, S.cfg.ctx);
    }
    yk_proto_free(&m);
}

static void got_binary(const uint8_t *frame, size_t len) {
    int stream, line, seq;
    const uint8_t *packet;
    size_t bytes;
    if (!yk_proto_unframe(frame, len, &stream, &line, &seq, &packet, &bytes) || stream != YK_PROTO_STREAM_TTS) {
        S.st.invalid++;
        return;
    }
    S.st.recv_audio++;
    if (S.cfg.on_audio) S.cfg.on_audio(line, seq, packet, bytes, S.cfg.ctx);
}

// ---------------------------------------------------------------- sending

static void usb_out(char kind, const uint8_t *data, size_t len) {
    size_t n = 0;
    mbedtls_base64_encode(NULL, 0, &n, data, len);
    char *b64 = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!b64) return;
    if (mbedtls_base64_encode((unsigned char *)b64, n + 1, &n, data, len) == 0) {
        b64[n] = 0;
        printf("YS> %c %s\n", kind, b64);  // one printf: the line is not split by other tasks' output
    }
    heap_caps_free(b64);
}

/** hello is the one frame that goes out before the session is up. */
static esp_err_t send_raw(bool text, const uint8_t *data, size_t len, bool hello) {
    if (S.state != YK_SESSION_UP && !(hello && S.state == YK_SESSION_CONNECTING)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(S.send_lock, portMAX_DELAY);
    if (S.usb) {
        usb_out(text ? 't' : 'b', data, len);
    } else if (S.ws && !S.closing) {
        const int n = text ? esp_websocket_client_send_text(S.ws, (const char *)data, (int)len, pdMS_TO_TICKS(2000))
                           : esp_websocket_client_send_bin(S.ws, (const char *)data, (int)len, pdMS_TO_TICKS(2000));
        if (n < 0) err = ESP_FAIL;
    } else {
        err = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(S.send_lock);
    return err;
}

esp_err_t yk_session_send(char *json) {
    if (!json) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = send_raw(true, (const uint8_t *)json, strlen(json), false);
    if (err == ESP_OK) S.st.sent_text++;
    free(json);
    return err;
}

esp_err_t yk_session_send_audio(const uint8_t *packet, size_t bytes) {
    if (bytes > FRAME_MAX - YK_PROTO_HEADER) return ESP_ERR_INVALID_SIZE;
    if (!S.frame) return ESP_ERR_INVALID_STATE;
    const size_t len = yk_proto_frame(S.frame, YK_PROTO_STREAM_MIC, 0, S.seq++, packet, bytes);  // one sender
    const esp_err_t err = send_raw(false, S.frame, len, false);
    if (err == ESP_OK) S.st.sent_audio++;
    return err;
}

void yk_session_new_turn(void) { S.seq = 0; }

/** Internal RAM at each step of connecting, numbers only: where the lowest at the start comes from. */
static void stage(const char *name) {
    printf("YAY_NET stage=%s ms=%u internal_free=%u internal_min=%u\n", name,
           (unsigned)((esp_timer_get_time() - S.connect_t0) / 1000),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

/** Connected: say hello first. */
static void say_hello(void) {
    char *hello = S.cfg.hello ? S.cfg.hello(S.cfg.ctx) : NULL;
    if (!S.usb) stage("hello_made");
    const esp_err_t err = hello ? send_raw(true, (const uint8_t *)hello, strlen(hello), true) : ESP_ERR_INVALID_ARG;
    free(hello);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hello not sent");
        xEventGroupSetBits(S.events, BIT_DOWN);
        return;
    }
    S.st.sent_text++;
    S.st.connects++;
    S.st.connect_ms = (uint32_t)((esp_timer_get_time() - S.connect_t0) / 1000);
    set_state(YK_SESSION_UP);
}

// ---------------------------------------------------------------- WebSocket

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg, (void)base;
    const esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        xEventGroupSetBits(S.events, BIT_UP);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        xEventGroupSetBits(S.events, BIT_DOWN | BIT_GONE);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x2 && e->payload_offset == 0 && e->data_len == e->payload_len) {
            got_binary((const uint8_t *)e->data_ptr, (size_t)e->data_len);
        } else if (e->op_code == 0x1 || (e->op_code == 0x0 && S.text_len)) {
            // A text frame can arrive in pieces (payload_offset); put it together before reading it.
            if (e->payload_offset == 0 && e->op_code == 0x1) S.text_len = 0;
            if (S.text && S.text_len + (size_t)e->data_len < TEXT_MAX) {
                memcpy(S.text + S.text_len, e->data_ptr, (size_t)e->data_len);
                S.text_len += (size_t)e->data_len;
            }
            if (e->payload_offset + e->data_len >= e->payload_len && e->fin) {
                got_text(S.text, S.text_len);
                S.text_len = 0;
            }
        } else if (e->op_code == 0x8) {
            if (e->data_len >= 2) {
                const int code = ((uint8_t)e->data_ptr[0] << 8) | (uint8_t)e->data_ptr[1];
                printf("YAY_NET closed code=%d\n", code);  // 4001: the token ran out; the next connection gets one
            }
            xEventGroupSetBits(S.events, BIT_PEER_CLOSE);
        }
        break;
    default:
        break;
    }
}

static void ws_connect(void) {
    static char *headers;
    if (!headers) headers = heap_caps_malloc(TOKEN_MAX + 128, MALLOC_CAP_SPIRAM);
    char *token = heap_caps_calloc(1, TOKEN_MAX, MALLOC_CAP_SPIRAM);
    if (!headers) {
        free(token);
        xEventGroupSetBits(S.events, BIT_DOWN);
        return;
    }
    stage("begin");
    const bool have = token && S.cfg.token && S.cfg.token(token, TOKEN_MAX, false) == ESP_OK && token[0];
    stage("token");
    snprintf(headers, TOKEN_MAX + 128, "%s%s%sX-Yaykit-Firmware: %s\r\n", have ? "Authorization: Bearer " : "",
             have ? token : "", have ? "\r\n" : "", S.cfg.firmware ? S.cfg.firmware : "");
    free(token);
    const esp_websocket_client_config_t wc = {
        .uri = S.cfg.url,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 45,  // nothing back for 45 s: drop and reconnect (4.1)
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .buffer_size = 2048,
        .task_stack = 6144,
        .task_prio = 5,
    };
    S.ws = esp_websocket_client_init(&wc);
    if (!S.ws) {
        xEventGroupSetBits(S.events, BIT_DOWN);
        return;
    }
    esp_websocket_register_events(S.ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(S.ws) != ESP_OK) xEventGroupSetBits(S.events, BIT_DOWN);
    stage("ws_start");
}

static void ws_close(void) {
    if (!S.ws) return;
    xSemaphoreTake(S.send_lock, portMAX_DELAY);
    esp_websocket_client_handle_t ws = S.ws;
    S.ws = NULL;
    xSemaphoreGive(S.send_lock);
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
}

/** session.reauth: a fresh token (not the cached one, which may still have minutes left), sent as `auth`. Not
 *  replying is fine too: the server closes with 4001 when the old one runs out and the next connection fetches a
 *  new one. Logged with lengths only: never the token or the message. */
static void reauth(void) {
    const int64_t t0 = esp_timer_get_time();
    char *token = heap_caps_calloc(1, TOKEN_MAX, MALLOC_CAP_SPIRAM);
    const bool got = token && S.cfg.token && S.cfg.token(token, TOKEN_MAX, true) == ESP_OK && token[0];
    const unsigned fetch_ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    size_t token_len = got ? strlen(token) : 0, bytes = 0;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (got) {
        char *auth = yk_proto_auth(token);
        if (auth) {
            bytes = strlen(auth);
            err = send_raw(true, (const uint8_t *)auth, bytes, false);
            if (err == ESP_OK) S.st.sent_text++;
        }
        free(auth);
    }
    free(token);
    printf("YAY_NET auth %s token_len=%u bytes=%u fetch_ms=%u internal_min=%u\n",
           !got ? "no_token" : err == ESP_OK ? "sent" : "not_sent", (unsigned)token_len, (unsigned)bytes, fetch_ms,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

// ---------------------------------------------------------------- the task

static void session_task(void *arg) {
    (void)arg;
    for (;;) {
        while (S.hold) xEventGroupWaitBits(S.events, BIT_KICK, pdTRUE, pdFALSE, portMAX_DELAY);
        xEventGroupClearBits(S.events, BIT_UP | BIT_DOWN);
        if (S.usb && S.usb_peer) xEventGroupSetBits(S.events, BIT_UP);  // a `ys c` that came during the back-off
        if (S.hold) continue;  // held just now: no connection while the phone link needs the memory
        S.closing = false;     // a new connection (S.ws is NULL here: nothing sends)
        S.connect_t0 = esp_timer_get_time();
        set_state(YK_SESSION_CONNECTING);
        if (!S.usb) ws_connect();  // the USB bridge says `ys c` when its side is up
        EventBits_t bits = xEventGroupWaitBits(S.events, BIT_UP | BIT_DOWN, pdTRUE, pdFALSE,
                                               S.usb ? portMAX_DELAY : pdMS_TO_TICKS(CONNECT_WAIT_MS));
        if (bits & BIT_UP) {
            if (!S.usb) stage("connected");
            say_hello();
            if (!S.usb) stage("hello_sent");
            if (S.state == YK_SESSION_UP) {
                for (;;) {
                    const EventBits_t got = xEventGroupWaitBits(S.events, BIT_DOWN | BIT_REAUTH, pdTRUE, pdFALSE, portMAX_DELAY);
                    if (got & BIT_REAUTH) reauth();
                    if (got & BIT_DOWN) break;
                }
                S.st.drops++;
            }
        }
        if (!S.usb) ws_close();
        set_state(YK_SESSION_DOWN);
        if (S.hold) continue;  // closed on purpose: no back-off, it reconnects as soon as it is released
        const int wait_ms = yk_proto_backoff_ms(S.attempt, esp_random());
        S.attempt++;
        printf("YAY_NET retry_ms=%d\n", wait_ms);
        // A kick (Wi-Fi came back) cuts the wait short.
        xEventGroupWaitBits(S.events, BIT_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t yk_session_start(const yk_session_config_t *cfg) {
    if (S.task) return ESP_OK;
    if (!cfg || !cfg->url) return ESP_ERR_INVALID_ARG;
    S.cfg = *cfg;
    S.usb = !strcmp(cfg->url, YK_SESSION_USB);
#if !CONFIG_YAY_DEV
    if (S.usb) return ESP_ERR_NOT_SUPPORTED;
#endif
    S.hold = cfg->held && !S.usb;  // the task waits for yk_session_hold(false) before anything
    if (S.hold) printf("YAY_NET session_hold=1 at=start\n");
    S.events = xEventGroupCreate();
    S.send_lock = xSemaphoreCreateMutex();
    S.text = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    S.frame = heap_caps_malloc(FRAME_MAX, MALLOC_CAP_SPIRAM);
    if (!S.events || !S.send_lock || !S.text || !S.frame) return ESP_ERR_NO_MEM;
    // 8 KB of internal stack: the token comes over HTTPS (yaykit_cloud), here.
    if (xTaskCreatePinnedToCore(session_task, "yk_session", 8192, NULL, 5, &S.task, 0) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "session to %s", cfg->url);
    return ESP_OK;
}

void yk_session_kick(void) {
    if (S.events) xEventGroupSetBits(S.events, BIT_KICK);
}

esp_err_t yk_session_close(uint32_t timeout_ms) {
    if (!S.events) return ESP_ERR_INVALID_STATE;
    if (S.usb) return ESP_ERR_NOT_SUPPORTED;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(S.send_lock, portMAX_DELAY);
    if (S.state == YK_SESSION_UP && S.ws && !S.closing) {
        static const uint8_t normal[2] = {0x03, 0xE8};  // 1000, a normal closure
        xEventGroupClearBits(S.events, BIT_PEER_CLOSE | BIT_GONE);
        S.hold = true;  // not again until yk_session_hold(false)
        S.closing = true;
        // Under send_lock, after every frame sent before it, on the same TCP stream.
        err = esp_websocket_client_send_with_opcode(S.ws, WS_TRANSPORT_OPCODES_CLOSE, normal, sizeof(normal),
                                                    pdMS_TO_TICKS(timeout_ms)) < 0
                  ? ESP_FAIL
                  : ESP_OK;
    }
    xSemaphoreGive(S.send_lock);
    if (err == ESP_OK) {
        const int64_t spent_ms = (esp_timer_get_time() - t0) / 1000;
        const uint32_t left_ms = spent_ms < timeout_ms ? timeout_ms - (uint32_t)spent_ms : 0;
        const EventBits_t got = xEventGroupWaitBits(S.events, BIT_PEER_CLOSE | BIT_GONE, pdFALSE, pdFALSE,
                                                    pdMS_TO_TICKS(left_ms));
        err = got & BIT_PEER_CLOSE ? ESP_OK : got & BIT_GONE ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }
    // The task closes the client, as when it drops (the client itself still waits for the server's TCP end).
    if (S.closing) xEventGroupSetBits(S.events, BIT_DOWN);
    printf("YAY_NET close=%s ms=%lld\n",
           err == ESP_OK ? "replied" : err == ESP_ERR_TIMEOUT ? "timeout" : err == ESP_FAIL ? "not_sent" : "no_session",
           (long long)((esp_timer_get_time() - t0) / 1000));
    return err;
}

void yk_session_reconnect(void) {
    if (S.events) xEventGroupSetBits(S.events, BIT_DOWN);
}

bool yk_session_hold(bool hold) {
    if (!S.events || S.usb) return false;
    if (S.hold == hold) return true;
    printf("YAY_NET session_hold=%d\n", hold);
    S.hold = hold;
    xEventGroupSetBits(S.events, hold ? BIT_DOWN : BIT_KICK);
    return true;
}

bool yk_session_held(void) { return S.events && !S.usb && S.hold; }

void yk_session_stats(yk_session_stats_t *out) { *out = S.st; }

// ---------------------------------------------------------------- console

static int session_cmd(int argc, char **argv) {
    (void)argc, (void)argv;
    printf("SESSION_STAT state=%s url=%s connects=%u drops=%u sent_text=%u sent_audio=%u recv_text=%u recv_audio=%u "
           "invalid=%u connect_ms=%u\n",
           yk_session_state_name(S.state), S.cfg.url ? S.cfg.url : "", (unsigned)S.st.connects, (unsigned)S.st.drops,
           (unsigned)S.st.sent_text, (unsigned)S.st.sent_audio, (unsigned)S.st.recv_text, (unsigned)S.st.recv_audio,
           (unsigned)S.st.invalid, (unsigned)S.st.connect_ms);
    return 0;
}

#if CONFIG_YAY_DEV
/** The USB bridge's side: `ys c` (its WebSocket is up), `ys d` (gone), `ys t|b <base64>` (a frame from the server). */
static int ys_cmd(int argc, char **argv) {
    if (!S.usb || argc < 2) return 1;
    if (!strcmp(argv[1], "c")) {
        S.usb_peer = true;
        xEventGroupSetBits(S.events, BIT_UP);
        return 0;
    }
    if (!strcmp(argv[1], "d")) {
        S.usb_peer = false;
        xEventGroupSetBits(S.events, BIT_DOWN);
        return 0;
    }
    if (argc < 3) return 1;
    const size_t in = strlen(argv[2]);
    uint8_t *buf = heap_caps_malloc(in, MALLOC_CAP_SPIRAM);
    size_t n = 0;
    if (!buf || mbedtls_base64_decode(buf, in, &n, (const unsigned char *)argv[2], in) != 0) {
        heap_caps_free(buf);
        S.st.invalid++;
        return 1;
    }
    if (argv[1][0] == 't') got_text((const char *)buf, n);
    else got_binary(buf, n);
    heap_caps_free(buf);
    return 0;
}
#endif

esp_err_t yk_session_register_console(void) {
    const esp_console_cmd_t stat = {.command = "session", .help = "session: connection state and counts", .func = session_cmd};
    esp_err_t err = esp_console_cmd_register(&stat);
#if CONFIG_YAY_DEV
    if (err == ESP_OK) {
        const esp_console_cmd_t ys = {.command = "ys", .help = "development: the USB bridge's frames", .func = ys_cmd};
        err = esp_console_cmd_register(&ys);
    }
#endif
    return err;
}

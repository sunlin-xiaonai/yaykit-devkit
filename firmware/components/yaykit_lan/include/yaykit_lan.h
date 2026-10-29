// Yaykit LAN: one WebSocket client on the round screen's own network (docs/design/selfie.md 3: 自拍屏's viewfinder
// at ws://192.168.4.1:8787/selfie). A new component; nothing else uses it.
//
// - One client at a time. Its first text message must be hello with the token the app handed to the phone over an
//   encrypted link (Bluetooth): {"t":"hello","token":"<32 hex>","name":"<the phone's name>"}. The answer is
//   {"t":"hello.ok"} or {"t":"hello.err","code":"TOKEN"} (and the socket closes). A second client that says hello
//   right takes over; the first is closed.
// - Text messages after hello go to on_text as they came (JSON, at most YK_LAN_TEXT_MAX bytes); binary messages to
//   on_binary in a PSRAM buffer the app frees (heap_caps_free), at most cfg.max_binary bytes. Nothing from a socket
//   that has not said hello.
// - yk_lan_send() queues a text message to the client from any task.
// - The token is the app's to make new (every time the hotspot opens) and to forget; it is never logged.
//
// The server's task stack is in PSRAM: callbacks must not write flash (post to a task of the app's).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_LAN_TOKEN_LEN 32
#define YK_LAN_TEXT_MAX 512

typedef struct {
    uint16_t port;          // 8787
    const char *path;       // "/selfie"
    size_t max_binary;      // the largest binary message taken (bytes)
    /** The client said hello with the right token; `name` as it gave it ("" without one). */
    void (*on_hello)(const char *name, void *ctx);
    /** A text message after hello (hello itself not), NUL-terminated. */
    void (*on_text)(const char *text, size_t len, void *ctx);
    /** A binary message after hello: `data` is the app's now (heap_caps_free it). */
    void (*on_binary)(uint8_t *data, size_t len, void *ctx);
    /** The client that said hello is gone. */
    void (*on_close)(void *ctx);
    void *ctx;
} yk_lan_config_t;

/** Starts the server (any interface; the hotspot's is 192.168.4.1). Again while running: ESP_OK. */
esp_err_t yk_lan_start(const yk_lan_config_t *cfg);
/** Stops it and closes the client. */
void yk_lan_stop(void);
bool yk_lan_running(void);
/** A new token for the next hello (YK_LAN_TOKEN_LEN hex characters and a NUL into `out`). The old one stops
 *  working; a client already in stays. */
void yk_lan_new_token(char out[YK_LAN_TOKEN_LEN + 1]);
/** No hello is taken any more, and the client is closed. */
void yk_lan_forget_token(void);
/** A client said hello and is still there. */
bool yk_lan_client(void);
/** Closes the client (it may say hello again while the token holds). */
void yk_lan_close_client(void);
/** A text message to the client, queued (any task). ESP_ERR_INVALID_STATE without a client. */
esp_err_t yk_lan_send(const char *text);

#ifdef __cplusplus
}
#endif

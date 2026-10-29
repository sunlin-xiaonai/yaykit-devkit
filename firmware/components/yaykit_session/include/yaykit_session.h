// The main firmware's one connection to the cloud (docs/design/yay-device.md 4.1, 6.7): a WebSocket over TLS
// that carries JSON text frames and one Opus packet per binary frame. It only connects, sends, receives and
// reconnects (1, 2, 4, 8, 15, 30 s, each ±25 %, back to 1 s only after a hello.ok); what the messages mean is the
// state machine's business.
//
// Development builds can talk to tools/yay_dev.py's bridge over the USB console instead ("usb" as the URL), so
// the whole conversation runs against yaykit/scripts/mock-voice-server.ts before the device has Wi-Fi.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "yk_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_SESSION_URL "wss://device.yaykit.io/device/v1/session"
#define YK_SESSION_USB "usb"

typedef enum {
    YK_SESSION_IDLE,        // not started, or waiting for the network
    YK_SESSION_CONNECTING,
    YK_SESSION_UP,          // hello sent
    YK_SESSION_DOWN,        // lost; retrying after a back-off
} yk_session_state_t;

typedef struct {
    const char *url;       // YK_SESSION_URL, a development server, or YK_SESSION_USB
    const char *firmware;  // "yay/0.1.0" (X-Yaykit-Firmware)
    /** The bearer token (yaykit_cloud's yk_account_token()); NULL sends none. Called before every connection
     *  (fresh false: a cached token still good for a minute will do) and after session.reauth (fresh true: the
     *  server wants a new one, so call yk_account_token_expired() first; answered with `auth`), in the session's
     *  task, which has 8 KB of internal stack. */
    esp_err_t (*token)(char *out, size_t size, bool fresh);
    /** The hello for this connection (yk_proto_hello()); the session frees it. */
    char *(*hello)(void *ctx);
    /** These three run in the session's task (or the WebSocket client's): keep them short. session.reauth is
     *  handled here and never reaches on_message. */
    void (*on_state)(yk_session_state_t state, void *ctx);
    void (*on_message)(const yk_srv_msg_t *msg, void *ctx);
    void (*on_audio)(int line, int seq, const uint8_t *packet, size_t bytes, void *ctx);
    void *ctx;
    /** Started held closed, as if yk_session_hold(true) came at once: nothing connects (no token, no WebSocket) until
     *  yk_session_hold(false). yk_session_hold() itself does nothing before the start. Not on the USB bridge. */
    bool held;
} yk_session_config_t;

esp_err_t yk_session_start(const yk_session_config_t *cfg);
yk_session_state_t yk_session_state(void);
const char *yk_session_state_name(yk_session_state_t state);

/** Sends a JSON text frame and frees it (NULL is ignored). ESP_ERR_INVALID_STATE when not up. */
esp_err_t yk_session_send(char *json);
/** One microphone packet; the frame sequence number counts by itself (yk_session_new_turn() restarts it). */
esp_err_t yk_session_send_audio(const uint8_t *packet, size_t bytes);
void yk_session_new_turn(void);

/** Try again now (the network came back). */
void yk_session_kick(void);
/** Close and reconnect (a new token after session.reauth is sent with `auth` instead; this is for 4001). */
void yk_session_reconnect(void);
/** Closes the connection as WebSocket closes it, then holds the session closed (as yk_session_hold(true)): a close
 *  frame (1000) after every frame sent before it, then the server's close waited for, `timeout_ms` in all. For leaving
 *  before a restart: the restart drops the connection without a word, and what TLS had not got out yet is lost with
 *  it (Language Buddy's practice.end, 10-25). The server closes back only once it has read everything before it (one
 *  stream, in order), so ESP_OK says the last messages got there. ESP_ERR_TIMEOUT: sent, nothing back in time;
 *  ESP_ERR_INVALID_STATE: no connection up, or it went before the server's close; ESP_FAIL: the frame could not be
 *  sent; ESP_ERR_NOT_SUPPORTED: the USB bridge. Nothing is sent after it; yk_session_hold(false) connects again.
 *  Not from the session's own callbacks. */
esp_err_t yk_session_close(uint32_t timeout_ms);
/** Hold the session closed (true) and let it reconnect (false): the phone link and an install need the memory TLS
 *  takes (docs/design/yay-device.md 6.3: pairing, installing and talking exclude each other). The USB bridge is never
 *  held (no TLS there, and the bridge connects only once). */
bool yk_session_hold(bool hold);  // false when it does not apply (the USB bridge, not started)
/** Held closed on purpose (yk_session_hold(true)): a session that is not up then is not a network fault. */
bool yk_session_held(void);

typedef struct {
    uint32_t connects, drops;
    uint32_t sent_text, sent_audio, recv_text, recv_audio, invalid;
    uint32_t connect_ms;  // last connection: start to hello sent
} yk_session_stats_t;
void yk_session_stats(yk_session_stats_t *out);

/** `session stat` and, in development builds, `ys <c|d|t|b> [base64]` for the USB bridge. */
esp_err_t yk_session_register_console(void);

#ifdef __cplusplus
}
#endif

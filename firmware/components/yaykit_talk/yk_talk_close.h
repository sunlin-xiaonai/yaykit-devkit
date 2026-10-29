// Closing before a restart (yk_talk_close, #580 10-25): the uplink first (practice.end and what came before it handed
// to the session), then the WebSocket close after it on the same connection, then the server's close back. That
// close comes only once the server has read everything before it, so it is the one sign the last messages got there:
// a restart alone drops the connection without a word, and what TLS had not got out yet goes with it. Plain C (host
// tested): the caller brings the steps and the clock.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "yk_talk_flush.h"

typedef enum {
    YK_CLOSE_REPLIED,  // the server closed back: it read all before it
    YK_CLOSE_TIMEOUT,  // the uplink not through, or no close back, in time
    YK_CLOSE_GONE,     // no connection to close, or it went first
} yk_close_result_t;

typedef struct {
    bool (*up)(void *ctx);                                         // the session is up
    yk_flush_result_t (*flush)(uint32_t timeout_ms, void *ctx);    // yk_flush_wait
    yk_close_result_t (*close)(uint32_t timeout_ms, void *ctx);    // yk_session_close: the frame and the wait
    int64_t (*now_ms)(void *ctx);
    void *ctx;
} yk_close_env_t;

/** `timeout_ms` for all of it. The uplink not through in time: no close (it would go before what is still queued). */
yk_close_result_t yk_close_run(const yk_close_env_t *env, uint32_t timeout_ms);
const char *yk_close_result_name(yk_close_result_t r);

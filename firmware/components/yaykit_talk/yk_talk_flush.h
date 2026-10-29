// Waiting for the uplink (yk_talk_flush): every command given to the microphones' task is numbered as it is queued and
// marked through once the task got to it, in order. A wait takes the number given last and returns once the task is
// there, or at its time. Plain C (host tested): the caller brings the lock, the clock and the sleep.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t posted;  // commands queued, the last one's number
    uint32_t done;    // the last command the task got through
    uint32_t failed;  // messages it could not send (no session): counted, never reset
} yk_flush_t;

typedef enum {
    YK_FLUSH_SENT,     // through, and every message among them went out
    YK_FLUSH_FAILED,   // through, but a message among them could not be sent
    YK_FLUSH_TIMEOUT,  // not through in time
} yk_flush_result_t;

/** A command queued: its number (under the caller's lock). */
uint32_t yk_flush_post(yk_flush_t *f);
/** The task got through command `n`; `sent` false for a message it could not send (under the caller's lock). */
void yk_flush_through(yk_flush_t *f, uint32_t n, bool sent);

typedef struct {
    yk_flush_t (*read)(void *ctx);         // a copy, taken under the lock
    int64_t (*now_ms)(void *ctx);
    void (*sleep_ms)(uint32_t ms, void *ctx);
    void *ctx;
} yk_flush_env_t;

#define YK_FLUSH_POLL_MS 5

/** Waits until the task is through everything queued before this call, at most `timeout_ms`. Nothing queued: at
 *  once. */
yk_flush_result_t yk_flush_wait(const yk_flush_env_t *env, uint32_t timeout_ms);
const char *yk_flush_result_name(yk_flush_result_t r);

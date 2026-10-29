// Incremental parser for the device API's server-sent events ("event: x\ndata: {...}\n\n").
// Pure C, no allocation: put the struct in PSRAM on the device. Lines longer than YK_SSE_LINE_MAX are dropped.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A /tts audio event carries a whole Opus packet (up to 3,828 bytes, ~5,100 base64 characters) on one line.
#define YK_SSE_LINE_MAX 8192

typedef void (*yk_sse_event_fn)(const char *event, const char *data, void *ctx);

typedef struct {
    char line[YK_SSE_LINE_MAX];
    size_t line_len;
    bool overflow;
    char event[32];
    char data[YK_SSE_LINE_MAX];
    size_t data_len;
    bool has_data;
    yk_sse_event_fn on_event;
    void *ctx;
} yk_sse_t;

void yk_sse_init(yk_sse_t *parser, yk_sse_event_fn on_event, void *ctx);
void yk_sse_feed(yk_sse_t *parser, const char *bytes, size_t len);
/** Dispatches an event still pending when the stream ends without a blank line. */
void yk_sse_finish(yk_sse_t *parser);

/** One read of the stream for yk_sse_pump(): bytes (> 0), YK_SSE_AGAIN when none came within the read's short
 *  timeout, 0 at the end, or an error (< 0). */
#define YK_SSE_AGAIN (-1000000)
typedef int (*yk_sse_read_fn)(char *buf, int size, void *ctx);
/** Feeds each read to the parser the moment it returns, so an event reaches on_event as soon as its blank line has
 *  come (not once a buffer is full or the stream ends), until the end, an error, or idle_ms without a byte by
 *  now_ms(); then yk_sse_finish(). Returns the last read: 0 at the end, YK_SSE_AGAIN when it went idle. */
int yk_sse_pump(yk_sse_t *parser, yk_sse_read_fn read, void *ctx, char *buf, int size, int idle_ms,
                int64_t (*now_ms)(void));

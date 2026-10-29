#include "yk_sse.h"

#include <string.h>

void yk_sse_init(yk_sse_t *p, yk_sse_event_fn on_event, void *ctx) {
    memset(p, 0, sizeof(*p));
    p->on_event = on_event;
    p->ctx = ctx;
}

static void dispatch(yk_sse_t *p) {
    if (p->has_data && p->on_event) {
        p->data[p->data_len] = 0;
        p->on_event(p->event[0] ? p->event : "message", p->data, p->ctx);
    }
    p->event[0] = 0;
    p->data_len = 0;
    p->has_data = false;
}

static const char *field_value(const char *line, const char *name) {
    size_t n = strlen(name);
    if (strncmp(line, name, n) || line[n] != ':') return NULL;
    const char *value = line + n + 1;
    return *value == ' ' ? value + 1 : value;
}

static void line_done(yk_sse_t *p) {
    if (p->overflow) {
        p->overflow = false;
        p->line_len = 0;
        return;
    }
    if (p->line_len && p->line[p->line_len - 1] == '\r') p->line_len--;
    p->line[p->line_len] = 0;
    p->line_len = 0;
    if (!p->line[0]) {
        dispatch(p);
        return;
    }
    if (p->line[0] == ':') return;  // comment / keep-alive
    const char *value;
    if ((value = field_value(p->line, "event"))) {
        strncpy(p->event, value, sizeof(p->event) - 1);
        p->event[sizeof(p->event) - 1] = 0;
    } else if ((value = field_value(p->line, "data"))) {
        size_t len = strlen(value), sep = p->has_data ? 1 : 0;
        if (p->data_len + sep + len < sizeof(p->data)) {
            if (sep) p->data[p->data_len++] = '\n';
            memcpy(p->data + p->data_len, value, len);
            p->data_len += len;
            p->has_data = true;
        }
    }
}

void yk_sse_feed(yk_sse_t *p, const char *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == '\n') {
            line_done(p);
        } else if (p->line_len + 1 < sizeof(p->line)) {
            p->line[p->line_len++] = bytes[i];
        } else {
            p->overflow = true;
        }
    }
}

void yk_sse_finish(yk_sse_t *p) {
    if (p->line_len) line_done(p);
    dispatch(p);
}

int yk_sse_pump(yk_sse_t *p, yk_sse_read_fn read, void *ctx, char *buf, int size, int idle_ms, int64_t (*now_ms)(void)) {
    int64_t last = now_ms();
    int n;
    for (;;) {
        n = read(buf, size, ctx);
        if (n > 0) {
            yk_sse_feed(p, buf, (size_t)n);
            last = now_ms();
        } else if (n != YK_SSE_AGAIN || now_ms() - last >= idle_ms) {
            break;
        }
    }
    yk_sse_finish(p);
    return n;
}

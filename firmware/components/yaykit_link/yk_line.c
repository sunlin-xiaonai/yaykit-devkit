#include "yk_line.h"

void yk_line_wipe(void *p, size_t len) {
    volatile unsigned char *bytes = p;
    while (len--) *bytes++ = 0;
}

void yk_line_init(yk_line_t *line, char *buf, size_t cap) {
    line->buf = buf;
    line->cap = cap;
    line->len = 0;
    line->overflow = false;
    if (cap) buf[0] = 0;
}

yk_line_result_t yk_line_feed(yk_line_t *line, char c) {
    if (c == '\r' || c == '\n') {
        if (!line->len && !line->overflow) return YK_LINE_MORE;
        if (line->overflow) {
            yk_line_wipe(line->buf, line->cap);
            return YK_LINE_TOO_LONG;
        }
        line->buf[line->len] = 0;
        return YK_LINE_DONE;
    }
    if ((unsigned char)c < 0x20 || c == 0x7f) return YK_LINE_MORE;
    if (line->overflow || line->len + 1 >= line->cap) {
        line->overflow = true;
        return YK_LINE_MORE;
    }
    line->buf[line->len++] = c;
    return YK_LINE_MORE;
}

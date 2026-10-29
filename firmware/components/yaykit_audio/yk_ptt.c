#include "yk_ptt.h"

#include <string.h>

size_t yk_ptt_bytes(uint32_t rate, int seconds) { return YK_PTT_HEADER + (size_t)rate * (size_t)seconds * 2; }

void yk_ptt_init(yk_ptt_t *r, uint8_t *buf, size_t bytes, uint32_t rate) {
    r->buf = buf;
    r->cap = bytes > YK_PTT_HEADER ? (bytes - YK_PTT_HEADER) / 2 : 0;
    r->rate = rate;
    yk_ptt_reset(r);
}

void yk_ptt_reset(yk_ptt_t *r) {
    r->n = 0;
    r->speech_frames = 0;
}

bool yk_ptt_add(yk_ptt_t *r, const int16_t *pcm, size_t n, bool speech) {
    const size_t room = r->cap - r->n, k = n < room ? n : room;
    memcpy(r->buf + YK_PTT_HEADER + 2 * r->n, pcm, 2 * k);  // little-endian on the device and the host
    r->n += k;
    if (speech && k) r->speech_frames++;
    return r->n < r->cap;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF, p[1] = (v >> 8) & 0xFF, p[2] = (v >> 16) & 0xFF, p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF, p[1] = v >> 8; }

size_t yk_ptt_finish(yk_ptt_t *r) {
    if (!r->n) return 0;
    uint8_t *h = r->buf;
    const uint32_t data = (uint32_t)(r->n * 2);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);  // PCM
    put16(h + 22, 1);  // mono
    put32(h + 24, r->rate);
    put32(h + 28, r->rate * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    return YK_PTT_HEADER + data;
}

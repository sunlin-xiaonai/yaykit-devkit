#include "yk_speak_dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI  // not in strict C11 (glibc)
#define M_PI 3.14159265358979323846
#endif

void yk_ring_init(yk_ring_t *r, int16_t *buf, size_t cap) {
    r->buf = buf;
    r->cap = cap;
    r->head = r->tail = 0;
}

size_t yk_ring_avail(const yk_ring_t *r) { return r->head - r->tail; }
size_t yk_ring_space(const yk_ring_t *r) { return r->cap - (r->head - r->tail); }

size_t yk_ring_write(yk_ring_t *r, const int16_t *x, size_t n) {
    const size_t space = yk_ring_space(r);
    if (n > space) n = space;
    for (size_t i = 0; i < n; i++) r->buf[(r->head + i) & (r->cap - 1)] = x[i];
    r->head += n;
    return n;
}

size_t yk_ring_read(yk_ring_t *r, int16_t *out, size_t n) {
    const size_t avail = yk_ring_avail(r);
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; i++) out[i] = r->buf[(r->tail + i) & (r->cap - 1)];
    r->tail += n;
    return n;
}

void yk_ring_clear(yk_ring_t *r) { r->tail = r->head; }

void yk_fade(int16_t *x, size_t n, float g0, float g1) {
    if (!n) return;
    for (size_t i = 0; i < n; i++) {
        const float g = g0 + (g1 - g0) * (float)i / (float)n;
        x[i] = (int16_t)lrintf((float)x[i] * g);
    }
}

uint8_t yk_level16(const int16_t *x, size_t n) {
    if (!n) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)x[i] * x[i];
    const double rms = sqrt(sum / n) / 32768.0;
    if (rms <= 0) return 0;
    const double db = 20 * log10(rms);
    if (db <= -60) return 0;
    if (db >= -6) return 100;
    return (uint8_t)lrint((db + 60) * 100.0 / 54.0);
}

/** One note with a 5 ms attack and an exponential decay to about -40 dB at its end. */
static size_t note(int16_t *out, size_t max, int rate, float hz, int ms, float amp) {
    size_t n = (size_t)rate * ms / 1000;
    if (n > max) n = max;
    const size_t attack = (size_t)rate / 200;
    for (size_t i = 0; i < n; i++) {
        const float t = (float)i / rate;
        const float env = (i < attack ? (float)i / attack : 1.0f) * expf(-4.6f * (float)i / n);
        out[i] = (int16_t)lrintf(amp * env * sinf(2 * (float)M_PI * hz * t) * 32767);
    }
    return n;
}

size_t yk_chime_synth(yk_chime_t kind, int rate, int16_t *out, size_t max) {
    // E5 -> A5 to open the microphone, A5 -> E5 to close it: short and quiet, no louder than speech.
    const float first = kind == YK_CHIME_START ? 659.3f : 880.0f, second = kind == YK_CHIME_START ? 880.0f : 659.3f;
    size_t n = note(out, max, rate, first, 70, 0.25f);
    n += note(out + n, max - n, rate, second, 70, 0.25f);
    return n;
}

int yk_volume_to_codec(int level) {
    if (level <= 0) return 0;
    if (level > 10) level = 10;
    // The ES8311's scale is about dB-linear: the audible top part, 60..92, in ten steps (as yaykit_chime does).
    return 60 + level * (92 - 60) / 10;
}

bool yk_speak_may_reclaim(bool codec_open, bool playing, bool mic_open) { return !codec_open && !playing && !mic_open; }

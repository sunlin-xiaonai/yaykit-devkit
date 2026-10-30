#include "pcm_conv.h"

void pcm_conv_init(pcm_conv_t *c, uint32_t in_rate, uint32_t out_rate) {
    c->in_rate = in_rate;
    c->out_rate = out_rate;
    c->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    c->pos = 0;
    c->prev = 0;
    c->primed = 0;
}

size_t pcm_conv_max_out(const pcm_conv_t *c, size_t frames) {
    return (size_t)(((uint64_t)frames * c->out_rate) / c->in_rate) + 2;
}

static int16_t mono_at(const int16_t *in, size_t i, int channels) {
    if (channels == 2) return (int16_t)(((int32_t)in[2 * i] + in[2 * i + 1]) / 2);
    return in[i];
}

size_t pcm_conv_run(pcm_conv_t *c, const int16_t *in, size_t frames, int channels, int16_t *out, size_t out_cap) {
    if (!frames) return 0;
    if (!c->primed) {
        c->prev = mono_at(in, 0, channels);
        c->primed = 1;
    }
    // The block seen as s[0] = prev, s[1..frames] = this block's samples; pos indexes s.
    size_t n = 0;
    const uint32_t end = (uint32_t)frames << 16;
    while (c->pos < end && n < out_cap) {
        const size_t k = c->pos >> 16;
        const int32_t frac = (int32_t)(c->pos & 0xFFFF);
        const int32_t a = k ? mono_at(in, k - 1, channels) : c->prev;
        const int32_t b = mono_at(in, k, channels);
        out[n++] = (int16_t)(a + (((b - a) * frac) >> 16));
        c->pos += c->step;
    }
    if (c->pos < end) c->pos = end;  // out was full: drop the rest rather than drift
    c->pos -= end;
    c->prev = mono_at(in, frames - 1, channels);
    return n;
}

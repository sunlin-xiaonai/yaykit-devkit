// Decoded radio audio to what the speaker takes: interleaved 16-bit PCM (mono or stereo, any rate) down to mono
// at the speaker's rate, with linear interpolation. Plain C, no ESP-IDF: host tested (test/run.sh). The state
// carries across blocks, so a stream converted block by block is the same as converted in one go.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t in_rate, out_rate;
    uint32_t step;  // input samples per output sample, Q16
    uint32_t pos;   // where the next output sample sits, Q16, counted from `prev`
    int16_t prev;   // the last mono input sample of the previous block
    int primed;     // prev holds a real sample
} pcm_conv_t;

void pcm_conv_init(pcm_conv_t *c, uint32_t in_rate, uint32_t out_rate);
/** At most this many output samples for `frames` input frames. */
size_t pcm_conv_max_out(const pcm_conv_t *c, size_t frames);
/** Converts `frames` frames of `channels` (1 or 2) interleaved samples; returns how many mono samples it wrote
 *  (never more than out_cap: the rest of the block is dropped, so size out with pcm_conv_max_out). */
size_t pcm_conv_run(pcm_conv_t *c, const int16_t *in, size_t frames, int channels, int16_t *out, size_t out_cap);

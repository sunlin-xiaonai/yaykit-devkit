// The effects mixer of yaykit_speak (P13), pure C (host tested with test/run.sh): voices placed to the sample on
// the speaker's clock, pitched clips, gain ramps, the 6 dB duck under an utterance, a -3 dBFS limiter.
// Single-threaded: the device side calls it under its own mutex (yaykit_speak.c); synth callbacks run inside
// yk_mix_render().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "yaykit_speak.h"
#include "yk_adpcm.h"

#define YK_MIX_MAX_BLOCK 480        // samples: 20 ms at 24 kHz
#define YK_MIX_DUCK 0.5012f         // -6 dB
#define YK_MIX_CEILING 0.7079f      // -3 dBFS: the small speaker's headroom (round-screen spec 10)
#define YK_MIX_GROUPS 256

typedef struct {
    int id;  // 0: free
    // a clip (PCM16 / ADPCM) or a synth voice (render)
    yk_clip_format_t format;
    const uint8_t *data;
    size_t bytes;
    uint32_t samples;
    uint32_t next;  // index of the next sample to fetch from the clip
    yk_adpcm_state_t adpcm;
    float pitch, frac, s0, s1;  // pitched read: between s0 (at frac 0) and s1
    yk_sfx_render_fn render;
    void *ctx;
    uint64_t start;  // speaker sample it begins at
    float gain, gain_to, gain_step;  // per sample, towards gain_to
    uint32_t fade_left;  // > 0: fading out to silence, then freed
    float fade_step;
    bool no_duck, loop, primed, ended;
} yk_mix_voice_t;

typedef struct {
    uint32_t rate;
    yk_mix_voice_t v[YK_SFX_VOICES];
    int next_id;
    uint64_t group_at[YK_MIX_GROUPS];  // speaker sample + 1 of the group's last start (0 = never)
    float duck;      // the effects' gain now: 1, or YK_MIX_DUCK under an utterance (ramped)
    float lim_gain;  // the limiter's gain now
    float mix[YK_MIX_MAX_BLOCK], voice[YK_MIX_MAX_BLOCK];
    // statistics
    uint32_t started, dropped, max_active, limited;
} yk_mix_t;

void yk_mix_init(yk_mix_t *m, uint32_t rate);

/** Adds a clip (clip != NULL) or a synth voice (render != NULL) that begins at speaker sample `start`; `now` is the
 *  first sample of the next block to be rendered (a start before it plays at once). Returns its id (> 0) or
 *  YK_SFX_BUSY (no free voice, or its group started less than min_gap_ms before) or YK_SFX_BAD. */
int yk_mix_add(yk_mix_t *m, const yk_clip_t *clip, yk_sfx_render_fn render, void *ctx, const yk_sfx_opts_t *opts,
               uint64_t start, uint64_t now);
/** Fades a voice out over `fade` samples (at least 1) and frees it; id 0 = every voice. */
void yk_mix_stop(yk_mix_t *m, int id, uint32_t fade);
/** Ramps a voice's gain over `ramp` samples. */
void yk_mix_gain(yk_mix_t *m, int id, float gain, uint32_t ramp);
bool yk_mix_playing(const yk_mix_t *m, int id);
int yk_mix_active(const yk_mix_t *m);

/** Mixes one block of n (<= YK_MIX_MAX_BLOCK) samples: the utterance (`utter`, NULL when none is playing) at full
 *  level, the voices ducked under it, the limiter, to int16 `out`. sample0 / heard_us0: the block's first sample
 *  on the speaker's clock and when it is heard. Returns the voices that played in it. */
int yk_mix_render(yk_mix_t *m, const int16_t *utter, size_t n, uint64_t sample0, int64_t heard_us0, int16_t *out);

// Host tests for the effects mixer (yk_mix) and the ADPCM codec (yk_adpcm). Run: bash test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "yk_adpcm.h"
#include "yk_mix.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

static yk_mix_t M;  // ~6 KB, not on the stack

static void test_adpcm(void) {
    enum { N = 4801 };
    static int16_t pcm[N], back[N];
    static uint8_t enc[N];
    // A 440 Hz tone with a decay, at 24 kHz: what the clips are (Party Play's cheers and drums are louder and noisier).
    for (int i = 0; i < N; i++) pcm[i] = (int16_t)(20000 * sinf(2 * (float)M_PI * 440 * i / 24000) * expf(-i / 3000.0f));
    const size_t bytes = yk_adpcm_encode(pcm, N, enc);
    CHECK(bytes == yk_adpcm_bytes(N));
    CHECK(bytes == 4 + 2400);
    CHECK(yk_adpcm_bytes(1) == 4 && yk_adpcm_bytes(2) == 5 && yk_adpcm_bytes(3) == 5 && yk_adpcm_bytes(0) == 0);
    CHECK(yk_adpcm_decode(enc, bytes, N, back, N) == N);
    CHECK(back[0] == pcm[0]);
    double sig = 0, err = 0;
    for (int i = 0; i < N; i++) {
        sig += (double)pcm[i] * pcm[i];
        err += (double)(pcm[i] - back[i]) * (pcm[i] - back[i]);
    }
    const double snr = 10 * log10(sig / (err + 1));
    printf("  adpcm: %zu bytes for %d samples, SNR %.1f dB\n", bytes, N, snr);
    CHECK(snr > 20);
    // A short or broken header is refused, never read past.
    yk_adpcm_state_t st;
    int16_t first;
    CHECK(yk_adpcm_begin(enc, 3, &st, &first) == -1);
    uint8_t bad[5] = {0, 0, 99, 0, 0};
    CHECK(yk_adpcm_begin(bad, 5, &st, &first) == -1);
    // Decoding stops at the end of the data even when `total` claims more.
    CHECK(yk_adpcm_decode(enc, 10, N, back, N) == 1 + 2 * 6);
    // Full-scale steps do not wrap around.
    int16_t edge[64];
    for (int i = 0; i < 64; i++) edge[i] = i % 8 < 4 ? 32767 : -32768;
    uint8_t e[64];
    int16_t eb[64];
    yk_adpcm_encode(edge, 64, e);
    yk_adpcm_decode(e, yk_adpcm_bytes(64), 64, eb, 64);
    for (int i = 1; i < 64; i++) CHECK(eb[i] >= -32768 && eb[i] <= 32767);
}

static int16_t clip_pcm[2400];
static yk_clip_t pcm_clip(int16_t level, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) clip_pcm[i] = level;
    return (yk_clip_t){.format = YK_CLIP_PCM16, .data = (const uint8_t *)clip_pcm, .bytes = n * 2, .samples = n};
}

static void test_passthrough(void) {
    // With no effects the utterance is copied bit for bit (the main firmware's voice is unchanged by the mixer).
    yk_mix_init(&M, 16000);
    int16_t u[320], out[320];
    for (int i = 0; i < 320; i++) u[i] = (int16_t)(i * 97 - 15000);
    u[7] = 32767, u[8] = -32768;
    CHECK(yk_mix_render(&M, u, 320, 0, 0, out) == 0);
    CHECK(!memcmp(u, out, sizeof(u)));
    CHECK(yk_mix_render(&M, NULL, 320, 320, 0, out) == 0);
    int nz = 0;
    for (int i = 0; i < 320; i++) nz += out[i] != 0;
    CHECK(nz == 0);
}

static void test_schedule(void) {
    // A clip placed at a future sample starts exactly there, even mid-block.
    yk_mix_init(&M, 24000);
    yk_clip_t c = pcm_clip(8000, 100);
    const int id = yk_mix_add(&M, &c, NULL, NULL, NULL, 1000 + 37, 1000);
    CHECK(id > 0);
    CHECK(yk_mix_playing(&M, id) && yk_mix_active(&M) == 1);
    int16_t out[120];
    yk_mix_render(&M, NULL, 120, 1000, 0, out);
    CHECK(out[36] == 0 && out[37] == 8000 && out[119] == 8000);
    yk_mix_render(&M, NULL, 120, 1120, 0, out);
    // 100 samples from 1037: the last at 1136 = index 16 of this block.
    CHECK(out[16] == 8000 && out[17] == 0);
    CHECK(!yk_mix_playing(&M, id) && yk_mix_active(&M) == 0);
    // A start in the past plays at once.
    const int id2 = yk_mix_add(&M, &c, NULL, NULL, NULL, 5, 2000);
    yk_mix_render(&M, NULL, 120, 2000, 0, out);
    CHECK(id2 > 0 && out[0] == 8000);
}

static void test_group_gap_and_voices(void) {
    yk_mix_init(&M, 16000);
    yk_clip_t c = pcm_clip(1000, 2000);
    const yk_sfx_opts_t hit = {.group = 3, .min_gap_ms = 18};
    // 18 ms at 16 kHz = 288 samples: a second hit 200 samples later is dropped, 300 later plays.
    CHECK(yk_mix_add(&M, &c, NULL, NULL, &hit, 0, 0) > 0);
    CHECK(yk_mix_add(&M, &c, NULL, NULL, &hit, 200, 0) == YK_SFX_BUSY);
    CHECK(yk_mix_add(&M, &c, NULL, NULL, &hit, 300, 0) > 0);
    // Another group is not held up.
    const yk_sfx_opts_t other = {.group = 4, .min_gap_ms = 18};
    CHECK(yk_mix_add(&M, &c, NULL, NULL, &other, 200, 0) > 0);
    // At most YK_SFX_VOICES: the next is refused, counted.
    while (yk_mix_active(&M) < YK_SFX_VOICES) CHECK(yk_mix_add(&M, &c, NULL, NULL, NULL, 0, 0) > 0);
    const uint32_t dropped = M.dropped;
    CHECK(yk_mix_add(&M, &c, NULL, NULL, NULL, 0, 0) == YK_SFX_BUSY);
    CHECK(M.dropped == dropped + 1 && M.max_active == YK_SFX_VOICES);
    // Bad input.
    CHECK(yk_mix_add(&M, NULL, NULL, NULL, NULL, 0, 0) == YK_SFX_BAD);
    yk_clip_t short_clip = {.format = YK_CLIP_PCM16, .data = (const uint8_t *)clip_pcm, .bytes = 10, .samples = 100};
    CHECK(yk_mix_add(&M, &short_clip, NULL, NULL, NULL, 0, 0) == YK_SFX_BAD);
    yk_clip_t opus = {.format = YK_CLIP_OPUS, .data = (const uint8_t *)clip_pcm, .bytes = 100, .samples = 100};
    CHECK(yk_mix_add(&M, &opus, NULL, NULL, NULL, 0, 0) == YK_SFX_BAD);
}

static void test_duck_and_limiter(void) {
    yk_mix_init(&M, 16000);
    yk_clip_t c = pcm_clip(10000, 2400);
    const yk_sfx_opts_t loop = {.loop = true};
    const int id = yk_mix_add(&M, &c, NULL, NULL, &loop, 0, 0);
    int16_t u[320], out[320];
    memset(u, 0, sizeof(u));
    yk_mix_render(&M, NULL, 320, 0, 0, out);
    CHECK(out[300] == 10000);
    // An utterance starts (silent here so only the effect is heard): within 30 ms the effect is 6 dB down.
    for (int b = 0; b < 3; b++) yk_mix_render(&M, u, 320, 320 + 320 * b, 0, out);
    CHECK(abs(out[319] - 5012) <= 2);
    // It ends: back to full.
    for (int b = 0; b < 3; b++) yk_mix_render(&M, NULL, 320, 1280 + 320 * b, 0, out);
    CHECK(out[319] == 10000);
    // A voice with no_duck stays at full level under an utterance.
    yk_mix_stop(&M, id, 1);
    yk_mix_render(&M, NULL, 320, 2240, 0, out);
    CHECK(yk_mix_active(&M) == 0);
    const yk_sfx_opts_t nd = {.no_duck = true, .loop = true};
    yk_mix_add(&M, &c, NULL, NULL, &nd, 0, 0);
    for (int b = 0; b < 3; b++) yk_mix_render(&M, u, 320, 2560 + 320 * b, 0, out);
    CHECK(out[319] == 10000);
    // Four loud voices: the sum would be 1.22 of full scale; the limiter holds it at -3 dBFS, no wrap.
    yk_mix_init(&M, 16000);
    yk_clip_t loud = pcm_clip(10000, 2400);
    for (int i = 0; i < 4; i++) yk_mix_add(&M, &loud, NULL, NULL, &loop, 0, 0);
    int peak = 0;
    for (int b = 0; b < 4; b++) {
        yk_mix_render(&M, NULL, 320, 320 * b, 0, out);
        for (int i = 0; i < 320; i++) peak = abs(out[i]) > peak ? abs(out[i]) : peak;
    }
    CHECK(peak <= (int)(YK_MIX_CEILING * 32768) + 1 && peak > 22000);
    CHECK(M.limited > 0);
}

static void test_fade_and_gain(void) {
    yk_mix_init(&M, 16000);
    yk_clip_t c = pcm_clip(16000, 2400);
    const int id = yk_mix_add(&M, &c, NULL, NULL, NULL, 0, 0);
    int16_t out[320];
    yk_mix_render(&M, NULL, 320, 0, 0, out);
    yk_mix_gain(&M, id, 0.5f, 160);
    yk_mix_render(&M, NULL, 320, 320, 0, out);
    CHECK(out[0] < 16000 && out[0] > 15000 && abs(out[200] - 8000) <= 2);
    // A stop fades over the given samples, never a click: monotone down to 0, then the voice is free.
    yk_mix_stop(&M, id, 32);
    yk_mix_render(&M, NULL, 320, 640, 0, out);
    for (int i = 1; i < 40; i++) CHECK(out[i] <= out[i - 1]);
    CHECK(out[31] <= 300 && out[40] == 0);
    CHECK(!yk_mix_playing(&M, id));
    // Stopping everything at once.
    yk_mix_add(&M, &c, NULL, NULL, NULL, 0, 960);
    yk_mix_add(&M, &c, NULL, NULL, NULL, 0, 960);
    yk_mix_stop(&M, 0, 10);
    yk_mix_render(&M, NULL, 320, 960, 0, out);
    CHECK(yk_mix_active(&M) == 0);
}

typedef struct {
    int calls;
    uint64_t first_sample;
    int64_t first_heard;
    int blocks_left;
} synth_ctx_t;

static bool synth(float *out, size_t n, const yk_sfx_block_t *blk, void *ctx) {
    synth_ctx_t *s = ctx;
    if (!s->calls++) s->first_sample = blk->sample, s->first_heard = blk->heard_us;
    for (size_t i = 0; i < n; i++) out[i] = 0.25f;
    return --s->blocks_left > 0;
}

static void test_synth_voice(void) {
    yk_mix_init(&M, 24000);
    synth_ctx_t s = {.blocks_left = 3};
    // Starts at sample 130 of the block that begins at 120, heard at 1 s: the callback is told its first sample is
    // 130, heard 10 samples (416 us) after the block.
    CHECK(yk_mix_add(&M, NULL, synth, &s, NULL, 130, 120) > 0);
    int16_t out[120];
    CHECK(yk_mix_render(&M, NULL, 120, 120, 1000000, out) == 1);
    CHECK(s.first_sample == 130 && s.first_heard == 1000000 + 10 * 1000000 / 24000);
    CHECK(out[9] == 0 && out[10] == 8192);
    yk_mix_render(&M, NULL, 120, 240, 0, out);
    yk_mix_render(&M, NULL, 120, 360, 0, out);
    // The third call returned false: that block still plays, then the voice is gone.
    CHECK(out[119] == 8192 && s.calls == 3 && yk_mix_active(&M) == 0);
}

static void test_pitch(void) {
    // A ramp clip at twice the speed covers it in half the samples; at half speed in twice as many.
    yk_mix_init(&M, 16000);
    for (int i = 0; i < 200; i++) clip_pcm[i] = (int16_t)(i * 100);
    yk_clip_t c = {.format = YK_CLIP_PCM16, .data = (const uint8_t *)clip_pcm, .bytes = 400, .samples = 200};
    const yk_sfx_opts_t fast = {.pitch = 2};
    yk_mix_add(&M, &c, NULL, NULL, &fast, 0, 0);
    int16_t out[480];
    yk_mix_render(&M, NULL, 480, 0, 0, out);
    CHECK(out[0] == 0 && out[1] == 200 && out[50] == 10000);
    int last = 0;
    for (int i = 0; i < 480; i++) if (out[i]) last = i;
    CHECK(last >= 99 && last <= 100);
    yk_mix_init(&M, 16000);
    const yk_sfx_opts_t slow = {.pitch = 0.5f};
    yk_mix_add(&M, &c, NULL, NULL, &slow, 0, 0);
    yk_mix_render(&M, NULL, 480, 0, 0, out);
    CHECK(out[2] == 100 && out[3] == 150);
    last = 0;
    for (int i = 0; i < 480; i++) if (out[i]) last = i;
    CHECK(last >= 398 && last <= 401);
}

static void test_adpcm_voice(void) {
    // An ADPCM clip through the mixer equals the codec's own decode.
    enum { N = 1000 };
    static int16_t pcm[N], ref[N];
    static uint8_t enc[N];
    for (int i = 0; i < N; i++) pcm[i] = (int16_t)(9000 * sinf(2 * (float)M_PI * 700 * i / 16000));
    const size_t bytes = yk_adpcm_encode(pcm, N, enc);
    yk_adpcm_decode(enc, bytes, N, ref, N);
    yk_clip_t c = {.format = YK_CLIP_IMA_ADPCM, .data = enc, .bytes = bytes, .samples = N};
    yk_mix_init(&M, 16000);
    CHECK(yk_mix_add(&M, &c, NULL, NULL, NULL, 0, 0) > 0);
    int16_t out[320];
    int same = 0;
    for (int b = 0; b * 320 < N; b++) {
        yk_mix_render(&M, NULL, 320, (uint64_t)b * 320, 0, out);
        for (int i = 0; i < 320 && b * 320 + i < N; i++) same += abs(out[i] - ref[b * 320 + i]) <= 1;
    }
    CHECK(same == N);
}

static void test_many_hits(void) {
    // Party Play's dice: 100 hits in quick succession never exceed the voices, never clip, never NaN.
    yk_mix_init(&M, 24000);
    yk_clip_t c = pcm_clip(20000, 1200);
    const yk_sfx_opts_t hit = {.group = 1, .min_gap_ms = 18, .gain = 0.8f};
    int16_t out[120];
    uint64_t t = 0;
    int started = 0, peak = 0;
    for (int k = 0; k < 2000; k++, t += 120) {
        if (k % 3 == 0) started += yk_mix_add(&M, &c, NULL, NULL, &hit, t + (k * 37) % 120, t) > 0;
        yk_mix_render(&M, NULL, 120, t, 0, out);
        CHECK(yk_mix_active(&M) <= YK_SFX_VOICES);
        for (int i = 0; i < 120; i++) peak = abs(out[i]) > peak ? abs(out[i]) : peak;
    }
    CHECK(started > 100 && peak <= (int)(YK_MIX_CEILING * 32768) + 1);
}

int main(void) {
    test_adpcm();
    test_passthrough();
    test_schedule();
    test_group_gap_and_voices();
    test_duck_and_limiter();
    test_fade_and_gain();
    test_synth_voice();
    test_pitch();
    test_adpcm_voice();
    test_many_hits();
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}

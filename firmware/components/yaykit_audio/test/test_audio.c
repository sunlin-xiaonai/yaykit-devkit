// Host tests for the front end's pure C. Run: bash firmware/components/yaykit_audio/test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI  // not in strict C11 (glibc)
#define M_PI 3.14159265358979323846
#endif

#include "../yk_dsp.h"
#include "../yk_fe.h"
#include "../yk_srmodels.h"
#include "../yk_onset.h"

static int failures, checks;
#define CHECK(cond)                                                \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            failures++;                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);  \
        }                                                          \
    } while (0)

#define PI 3.14159265358979
#define FRAME 480

static unsigned rng = 12345;
static float white(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float)rng / 4294967296.0f * 2 - 1;
}

/** Speech-like test talker: low-passed noise, 0.3 s on, 0.2 s off. */
static void talker(float *s, size_t n, float rate, float amp) {
    float lp = 0;
    for (size_t i = 0; i < n; i++) {
        lp += 0.25f * (white() - lp);
        bool on = fmod(i / rate, 0.5) < 0.3;
        s[i] = on ? amp * lp : 0;
    }
}

static float db(float v) { return 20 * log10f(v); }

static float rms(const float *x, size_t n) {
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i];
    return (float)sqrt(e / n);
}

/** Resamples a whole signal in the given chunk sizes (cycled). Returns the output count. */
static size_t resample(const float *in, size_t n, float *out, const size_t *chunks, size_t nchunks) {
    yk_resampler_t *r = malloc(sizeof(*r));
    yk_resampler_init_24k_to_16k(r);
    size_t at = 0, m = 0, c = 0;
    while (at < n) {
        size_t k = chunks[c++ % nchunks];
        if (k > n - at) k = n - at;
        m += yk_resampler_run(r, in + at, k, out + m);
        at += k;
    }
    free(r);
    return m;
}

/** Steady-state RMS of a 24 kHz sine after resampling to 16 kHz. */
static float sine_through(float hz) {
    enum { N = 24000 };
    static float in[N], out[N];
    for (int i = 0; i < N; i++) in[i] = 0.5f * sinf((float)(2 * PI * hz * i / 24000));
    size_t chunk = FRAME, m = resample(in, N, out, &chunk, 1);
    return rms(out + 400, m - 800) / (0.5f / sqrtf(2));
}

static void test_resampler(void) {
    enum { N = 24000 };
    static float in[N], a[N], b[N];
    CHECK(fabsf(db(sine_through(1000))) < 0.05f);
    CHECK(fabsf(db(sine_through(6500))) < 0.1f);
    CHECK(fabsf(db(sine_through(7000))) < 0.2f);
    CHECK(db(sine_through(8200)) < -55);  // would alias to 7.8 kHz
    CHECK(db(sine_through(9000)) < -58);
    CHECK(db(sine_through(11000)) < -58);

    for (int i = 0; i < N; i++) in[i] = white() * 0.5f;
    size_t whole = YK_RS_MAX_IN, mixed[] = {1, 7, 480, 333, 2, 960, 13};
    size_t ma = resample(in, N, a, &whole, 1);
    size_t mb = resample(in, N, b, mixed, 7);
    CHECK(ma == mb);
    CHECK(ma >= 15999 && ma <= 16001);
    float worst = 0;
    for (size_t i = 0; i < ma; i++) worst = fmaxf(worst, fabsf(a[i] - b[i]));
    CHECK(worst < 1e-6f);  // chunking never changes the result
}

/** |gain| from the talker to the output over speech after `skip` samples: 1 is ideal, 0 is a null. */
static float projection(const float *out, const float *s, size_t n, size_t skip) {
    double sum = 0, ss = 0;
    for (size_t i = skip; i < n; i++) {
        sum += (double)out[i] * s[i];
        ss += (double)s[i] * s[i];
    }
    return (float)fabs(sum / ss);
}

enum { CN = 24000 * 2 };
static float cs[CN], ca[CN], cb[CN], cout[CN];

static float steer(float deg, float sign_b, float noise) {
    rng = 777;
    talker(cs, CN, 24000, 0.3f);
    float th = (float)(deg * PI / 180);
    for (int i = 0; i < CN; i++) {
        ca[i] = cosf(th) * cs[i] + noise * white();
        cb[i] = sign_b * sinf(th) * cs[i] + noise * white();
    }
    yk_combiner_t c;
    yk_combiner_init(&c, 5, 0.5f);
    for (int i = 0; i + FRAME <= CN; i += FRAME) yk_combiner_run(&c, ca + i, cb + i, cout + i, FRAME);
    return projection(cout, cs, CN, 12000);
}

static void test_combiner(void) {
    const float angles[] = {0, 30, 45, 90, 135, 160, 200, 270, 315};
    for (size_t k = 0; k < sizeof(angles) / sizeof(angles[0]); k++) {
        CHECK(steer(angles[k], 1, 0.003f) > 0.97f);
        CHECK(steer(angles[k], -1, 0.003f) > 0.97f);  // polarity of either pair does not matter
    }
    // The fixed mix the push-to-talk recorder uses today nulls a talker at 135°.
    rng = 777;
    talker(cs, CN, 24000, 0.3f);
    for (int i = 0; i < CN; i++) cout[i] = (cosf(0.75f * (float)PI) * cs[i] + sinf(0.75f * (float)PI) * cs[i]) / 2;
    CHECK(projection(cout, cs, CN, 0) < 0.01f);

    // Talker moves from 0° to 90° at 1 s: followed within 0.2 s, never more than max_step a frame.
    rng = 99;
    talker(cs, CN, 24000, 0.3f);
    yk_combiner_t c;
    yk_combiner_init(&c, 5, 0.5f);
    float worst_step = 0, prev = 0;
    for (int i = 0; i < CN; i++) {
        float th = i < CN / 2 ? 0 : (float)(PI / 2);
        ca[i] = cosf(th) * cs[i] + 0.003f * white();
        cb[i] = sinf(th) * cs[i] + 0.003f * white();
    }
    for (int i = 0; i + FRAME <= CN; i += FRAME) {
        yk_combiner_run(&c, ca + i, cb + i, cout + i, FRAME);
        float d = fabsf(c.theta - prev);
        d = fminf(d, 2 * (float)PI - d);
        worst_step = fmaxf(worst_step, d);
        prev = c.theta;
    }
    CHECK(worst_step <= 0.5f + 1e-4f);
    CHECK(projection(cout + CN / 2, cs + CN / 2, CN / 2, 4800) > 0.95f);

    // Diffuse noise alone has no direction: the weights hold.
    yk_combiner_init(&c, 5, 0.5f);
    for (int i = 0; i < CN; i++) {
        ca[i] = 0.05f * white();
        cb[i] = 0.05f * white();
    }
    for (int i = 0; i + FRAME <= CN; i += FRAME) yk_combiner_run(&c, ca + i, cb + i, cout + i, FRAME);
    CHECK(fabsf(c.theta) < 0.3f);
}

static void test_agc_limiter_gate(void) {
    float x[320];
    yk_agc_t g;
    yk_agc_init(&g, 0.02f, -20, -6, 18, 3, 10);
    // Quiet talker at -40 dBFS RMS: gain rises 3 dB/s up to the 18 dB ceiling, and holds in pauses.
    for (int f = 0; f < 50; f++) {
        for (int i = 0; i < 320; i++) x[i] = 0.01f * sinf((float)(2 * PI * 300 * i / 16000)) * sqrtf(2);
        yk_agc_run(&g, x, 320, true);
    }
    CHECK(fabsf(g.gain_db - 3) < 0.01f);
    float held = g.gain_db;
    for (int f = 0; f < 50; f++) {
        for (int i = 0; i < 320; i++) x[i] = 0.0003f * white();
        yk_agc_run(&g, x, 320, false);
    }
    CHECK(g.gain_db == held);
    for (int f = 0; f < 500; f++) {
        for (int i = 0; i < 320; i++) x[i] = 0.01f * sinf((float)(2 * PI * 300 * i / 16000)) * sqrtf(2);
        yk_agc_run(&g, x, 320, true);
    }
    CHECK(g.gain_db == 18);
    // A loud talker brings it down 10 dB/s, no lower than -6 dB.
    for (int f = 0; f < 500; f++) {
        for (int i = 0; i < 320; i++) x[i] = 0.5f * sinf((float)(2 * PI * 300 * i / 16000)) * sqrtf(2);
        yk_agc_run(&g, x, 320, true);
    }
    CHECK(g.gain_db == -6);

    yk_limiter_t l;
    yk_limiter_init(&l, 16000, -1, 100);
    float peak = 0;
    for (int f = 0; f < 20; f++) {
        for (int i = 0; i < 320; i++) x[i] = 2 * sinf((float)(2 * PI * 440 * (f * 320 + i) / 16000));
        yk_limiter_run(&l, x, 320);
        for (int i = 0; i < 320; i++) peak = fmaxf(peak, fabsf(x[i]));
    }
    CHECK(peak <= powf(10, -1 / 20.0f) + 1e-6f);

    yk_gate_t gate = {0};
    bool any = false;
    for (int f = 0; f < 20; f++) {
        for (int i = 0; i < 320; i++) x[i] = 0.001f * white();
        any |= yk_gate_speech(&gate, x, 320);
    }
    CHECK(!any);
    for (int i = 0; i < 320; i++) x[i] = 0.1f * white();
    CHECK(yk_gate_speech(&gate, x, 320));

    float silent[4] = {0}, full[2] = {1, -0.2f}, mid[1] = {0.0316f};
    CHECK(yk_level(silent, 4) == 0);
    CHECK(yk_level(full, 2) == 100);
    CHECK(yk_level(mid, 1) >= 49 && yk_level(mid, 1) <= 51);

    int16_t pcm[3];
    float over[3] = {1.5f, -1.5f, 0.5f};
    yk_to_pcm16(over, pcm, 3);
    CHECK(pcm[0] == 32767 && pcm[1] == -32767 && pcm[2] == 16384);
}

/** The whole pipeline on synthetic TDM: a talker at 135° (where today's fixed mix is silent), slot2 wired
 *  inverted, a loud loopback in slot1 that must not leak. */
static void test_pipeline(void) {
    enum { SECONDS = 4, N = 24000 * SECONDS };
    static int16_t tdm[N * 4];
    static float s[N], out[N];
    static int16_t pcm[N];
    rng = 4242;
    talker(s, N, 24000, 0.25f);
    float th = (float)(0.75 * PI);
    for (int i = 0; i < N; i++) {
        float a = cosf(th) * s[i] + 0.002f * white(), b = -sinf(th) * s[i] + 0.002f * white();
        tdm[i * 4 + 0] = (int16_t)lrintf(a * 32767);
        tdm[i * 4 + 1] = (int16_t)lrintf(0.9f * sinf((float)(2 * PI * 1000 * i / 24000)) * 32767);
        tdm[i * 4 + 2] = (int16_t)lrintf(b * 32767);
        tdm[i * 4 + 3] = 0;
    }
    yk_fe_config_t cfg;
    yk_fe_config_for(YK_AUDIO_RECORD, &cfg);
    yk_fe_t *fe = malloc(sizeof(*fe));
    yk_fe_init(fe, &YK_MICS_TIKPAL_R1, &cfg);
    size_t m = 0;
    for (int i = 0; i + FRAME <= N; i += FRAME) {
        size_t k = yk_fe_capture(fe, tdm + i * 4, FRAME, out + m);
        yk_fe_finish(fe, out + m, k, -1, pcm + m);
        m += k;
    }
    CHECK(m >= 16000 * SECONDS - 2 && m <= 16000 * SECONDS + 1);
    // Talker level in the last second's speech (0.3 s bursts): the talker's own RMS is about -21 dBFS at
    // the input; it must come through at roughly that level, not nulled.
    float on = rms(out + 16000 * 3, 16000 * 3 / 10);
    CHECK(db(on) > -26 && db(on) < -16);
    // No trace of the 1 kHz loopback in a pause.
    float pause = rms(out + 16000 * 3 + 16000 * 4 / 10, 16000 / 20);
    CHECK(db(pause) < -45);
    free(fe);
}

/** The main firmware runs I2S at 16 kHz: same talker and loopback, no resampler, sample for sample. */
static void test_pipeline_16k(void) {
    enum { SECONDS = 4, N = 16000 * SECONDS, FRAME16 = 320 };
    static int16_t tdm[N * 4];
    static float s[N], out[N];
    static int16_t pcm[N];
    rng = 4242;
    talker(s, N, 16000, 0.25f);
    float th = (float)(0.75 * PI);
    for (int i = 0; i < N; i++) {
        float a = cosf(th) * s[i] + 0.002f * white(), b = -sinf(th) * s[i] + 0.002f * white();
        tdm[i * 4 + 0] = (int16_t)lrintf(a * 32767);
        tdm[i * 4 + 1] = (int16_t)lrintf(0.9f * sinf((float)(2 * PI * 1000 * i / 16000)) * 32767);
        tdm[i * 4 + 2] = (int16_t)lrintf(b * 32767);
        tdm[i * 4 + 3] = 0;
    }
    yk_fe_config_t cfg;
    yk_fe_config_for(YK_AUDIO_PUSH_TO_TALK, &cfg);
    yk_fe_t *fe = malloc(sizeof(*fe));
    yk_fe_init(fe, &YK_MICS_TIKPAL_R1_16K, &cfg);
    size_t m = 0;
    for (int i = 0; i + FRAME16 <= N; i += FRAME16) {
        size_t k = yk_fe_capture(fe, tdm + i * 4, FRAME16, out + m);
        CHECK(k == FRAME16);
        yk_fe_finish(fe, out + m, k, -1, pcm + m);
        m += k;
    }
    CHECK(m == N);
    float on = rms(out + 16000 * 3, 16000 * 3 / 10);
    CHECK(db(on) > -26 && db(on) < -16);
    float pause = rms(out + 16000 * 3 + 16000 * 4 / 10, 16000 / 20);
    CHECK(db(pause) < -45);
    free(fe);
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

/** A packed model file as movemodel.py writes it: two models (nsnet2 with 2 files, vadnet with 1). */
static void test_srmodels(void) {
    static uint8_t f[4096];
    memset(f, 0, sizeof(f));
    const size_t header = 4 + (36 + 2 * 40) + (36 + 1 * 40);
    put32(f, 2);
    size_t at = 4;
    memcpy(f + at, "nsnet2", 6), put32(f + at + 32, 2), at += 36;
    memcpy(f + at, "nsnet2_data", 11), put32(f + at + 32, (uint32_t)header), put32(f + at + 36, 1000), at += 40;
    memcpy(f + at, "_MODEL_INFO_", 12), put32(f + at + 32, (uint32_t)header + 1000), put32(f + at + 36, 20), at += 40;
    memcpy(f + at, "vadnet1_medium", 14), put32(f + at + 32, 1), at += 36;
    memcpy(f + at, "vadnet_data", 11), put32(f + at + 32, (uint32_t)header + 1020), put32(f + at + 36, 500), at += 40;
    CHECK(at == header);
    CHECK(yk_srmodels_header_size(f, sizeof(f)) == header);
    CHECK(yk_srmodels_header_size(f, header - 1) == 0);  // not all of it read yet
    CHECK(yk_srmodels_size(f, sizeof(f), 1u << 20) == header + 1520);
    CHECK(yk_srmodels_size(f, sizeof(f), header + 1519) == 0);  // runs past the partition
    uint8_t erased[64];
    memset(erased, 0xff, sizeof(erased));
    CHECK(yk_srmodels_size(erased, sizeof(erased), 1u << 20) == 0);  // an empty partition
    put32(f, 0);
    CHECK(yk_srmodels_size(f, sizeof(f), 1u << 20) == 0);
}

/** Speech-like test clip at 16 kHz: 200 ms of silence, then three 200 ms "syllables" (a 150 Hz buzz with
 *  harmonics under a smooth envelope) with 80 ms gaps. */
static size_t make_clip(int16_t *x, size_t max) {
    size_t n = 0;
    unsigned seed = 7;
    for (; n < 3200 && n < max; n++) x[n] = 0;
    for (int syl = 0; syl < 3; syl++) {
        for (int i = 0; i < 3200 && n < max; i++, n++) {
            const double env = sin(M_PI * i / 3200.0) * (syl == 1 ? 0.6 : 1.0);
            double v = 0;
            for (int h = 1; h <= 8; h++) v += sin(2 * M_PI * 150 * h * n / 16000.0 + h) / h;
            seed = seed * 1103515245u + 12345u;
            v += ((seed >> 16) % 2000 - 1000) / 4000.0;
            x[n] = (int16_t)(8000 * env * v);
        }
        for (int i = 0; i < 1280 && n < max; i++, n++) x[n] = 0;
    }
    return n;
}

static void test_onset(void) {
    static int16_t ref[24000], cap[40000];
    const size_t nr = make_clip(ref, 24000);
    float env[200];
    CHECK(yk_env_db(ref, nr, 16000, env, 200) == nr / 160);
    CHECK(env[0] < 1 && env[25] > 60);  // silence, then speech
    // The microphones heard it 300 ms in, at half the level, over a quiet room.
    unsigned seed = 3;
    const size_t nc = 40000;
    for (size_t i = 0; i < nc; i++) {
        seed = seed * 1103515245u + 12345u;
        const int noise = (int)((seed >> 16) % 41) - 20;
        cap[i] = (int16_t)(noise + (i >= 4800 && i - 4800 < nr ? ref[i - 4800] / 2 : 0));
    }
    yk_onset_t o;
    CHECK(yk_onset_match(ref, nr, cap, nc, 16000, &o));
    CHECK(o.lag_ms >= 290 && o.lag_ms <= 310);
    CHECK(o.onset_ms >= o.lag_ms + 190 && o.onset_ms <= o.lag_ms + 220);  // speech starts 200 ms into the clip
    CHECK(o.corr > 0.9f);
    CHECK(o.early_ratio > 0.8f && o.early_ratio < 1.25f);
    // A gate that opens late: the first 150 ms of speech come through 20 dB down. That is a lost first word.
    for (size_t i = 4800 + 3200; i < 4800 + 3200 + 2400; i++) cap[i] = (int16_t)(cap[i] / 10);
    CHECK(yk_onset_match(ref, nr, cap, nc, 16000, &o));
    CHECK(o.lag_ms >= 290 && o.lag_ms <= 310);
    CHECK(o.early_ratio < 0.3f);
    // Nothing to find.
    static int16_t quiet[8000];
    CHECK(!yk_onset_match(quiet, 8000, cap, nc, 16000, &o));
    CHECK(!yk_onset_match(ref, nr, cap, nr / 2, 16000, &o));
}

int main(void) {
    test_onset();
    test_resampler();
    test_combiner();
    test_agc_limiter_gate();
    test_pipeline();
    test_pipeline_16k();
    test_srmodels();
    printf("yaykit_audio host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

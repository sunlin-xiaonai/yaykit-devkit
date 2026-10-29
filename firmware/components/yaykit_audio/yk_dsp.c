#include "yk_dsp.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979f
#define PI_D 3.14159265358979323846

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void biquad_set(yk_biquad_t *f, float b0, float b1, float b2, float a0, float a1, float a2) {
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
    f->z1 = f->z2 = 0;
}

void yk_biquad_highpass(yk_biquad_t *f, float rate, float hz, float q) {
    float w = 2 * PI * hz / rate, c = cosf(w), alpha = sinf(w) / (2 * q);
    biquad_set(f, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
}

void yk_biquad_lowshelf(yk_biquad_t *f, float rate, float hz, float gain_db) {
    float A = powf(10, gain_db / 40), w = 2 * PI * hz / rate, c = cosf(w);
    float beta = 2 * sqrtf(A) * sinf(w) / 2 * sqrtf(2);  // 2·√A·alpha with slope 1
    biquad_set(f, A * ((A + 1) - (A - 1) * c + beta), 2 * A * ((A - 1) - (A + 1) * c),
               A * ((A + 1) - (A - 1) * c - beta), (A + 1) + (A - 1) * c + beta, -2 * ((A - 1) + (A + 1) * c),
               (A + 1) + (A - 1) * c - beta);
}

void yk_biquad_run(yk_biquad_t *f, float *x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float in = x[i], out = f->b0 * in + f->z1;
        f->z1 = f->b1 * in - f->a1 * out + f->z2;
        f->z2 = f->b2 * in - f->a2 * out;
        x[i] = out;
    }
}

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= (x / 2 / k) * (x / 2 / k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

void yk_resampler_init_24k_to_16k(yk_resampler_t *r) {
    memset(r, 0, sizeof(*r));
    // Designed at the 48 kHz up-sampled rate: cutoff halfway between 7 and 8 kHz, Kaiser beta for 60 dB.
    r->up = 2;
    r->down = 3;
    r->taps = 176;
    const double rate = 48000, fc = 7500, beta = 5.653, mid = (r->taps - 1) / 2.0;
    for (int n = 0; n < r->taps; n++) {
        double x = 2 * fc / rate * (n - mid), sinc = x == 0 ? 1 : sin(PI_D * x) / (PI_D * x);
        double k = (n - mid) / mid, window = bessel_i0(beta * sqrt(1 - k * k)) / bessel_i0(beta);
        r->h[n] = (float)(r->up * 2 * fc / rate * sinc * window);
    }
    // History of zeros so the first output already has a full filter span behind it.
    r->held = (r->taps + r->up - 1) / r->up;
    r->t = r->held * r->up;
}

size_t yk_resampler_run(yk_resampler_t *r, const float *in, size_t n, float *out) {
    if (n > YK_RS_MAX_IN) n = YK_RS_MAX_IN;
    memcpy(r->buf + r->held, in, n * sizeof(float));
    r->held += (int)n;
    size_t m = 0;
    while (r->t / r->up <= r->held - 1) {
        float acc = 0;
        for (int k = r->t % r->up; k < r->taps && k <= r->t; k += r->up) acc += r->h[k] * r->buf[(r->t - k) / r->up];
        out[m++] = acc;
        r->t += r->down;
    }
    // Drop inputs that fall outside the filter span of every later output.
    int span = r->t - r->taps + 1;
    if (span > 0) {
        int drop = (span + r->up - 1) / r->up;
        if (drop > r->held) drop = r->held;
        memmove(r->buf, r->buf + drop, (size_t)(r->held - drop) * sizeof(float));
        r->held -= drop;
        r->t -= drop * r->up;
    }
    return m;
}

void yk_combiner_init(yk_combiner_t *c, float tau_frames, float max_step) {
    memset(c, 0, sizeof(*c));
    c->alpha = tau_frames <= 1 ? 1 : 1 - expf(-1 / tau_frames);
    c->max_step = max_step;
}

void yk_combiner_run(yk_combiner_t *c, const float *a, const float *b, float *out, size_t n) {
    if (!n) return;
    float caa = 0, cbb = 0, cab = 0;
    for (size_t i = 0; i < n; i++) {
        caa += a[i] * a[i];
        cbb += b[i] * b[i];
        cab += a[i] * b[i];
    }
    caa /= n;
    cbb /= n;
    cab /= n;
    float e = caa + cbb;
    if (!c->have_noise || e < c->noise) c->noise = e;
    else c->noise *= 1.01f;
    c->have_noise = true;

    // Before the first estimate any audible frame bootstraps it, so a short utterance starting at once still
    // gets steered; after that only frames 6 dB above the noise floor update the covariance.
    bool update = c->have_cov ? e > 4 * fmaxf(c->noise, 1e-10f) : e > 1e-6f;
    float target = c->theta;
    if (update) {
        if (!c->have_cov) {
            c->raa = caa;
            c->rbb = cbb;
            c->rab = cab;
            c->have_cov = true;
        } else {
            c->raa += c->alpha * (caa - c->raa);
            c->rbb += c->alpha * (cbb - c->rbb);
            c->rab += c->alpha * (cab - c->rab);
        }
        float p = c->raa, r = c->rbb, q = c->rab, d = sqrtf(0.25f * (p - r) * (p - r) + q * q);
        if (p + r > 0 && 2 * d > 0.2f * (p + r)) {  // a clear direction, not just diffuse noise
            float phi = 0.5f * atan2f(2 * q, p - r), diff = phi - c->theta;
            diff -= PI * roundf(diff / PI);  // the axis, not its sign: never flip the output's polarity
            target = c->theta + diff;
        }
    }
    float theta1 = c->theta + clampf(target - c->theta, -c->max_step, c->max_step);
    float wa0 = cosf(c->theta), wb0 = sinf(c->theta), wa1 = cosf(theta1), wb1 = sinf(theta1);
    for (size_t i = 0; i < n; i++) {
        float g = (float)(i + 1) / n, av = a[i], bv = b[i];
        out[i] = (wa0 + (wa1 - wa0) * g) * av + (wb0 + (wb1 - wb0) * g) * bv;
    }
    c->theta = theta1 - 2 * PI * roundf(theta1 / (2 * PI));
}

void yk_agc_init(yk_agc_t *g, float frame_seconds, float target_dbfs, float min_db, float max_db,
                 float up_db_per_s, float down_db_per_s) {
    g->gain_db = 0;
    g->target_dbfs = target_dbfs;
    g->min_db = min_db;
    g->max_db = max_db;
    g->up_db = up_db_per_s * frame_seconds;
    g->down_db = down_db_per_s * frame_seconds;
}

void yk_agc_run(yk_agc_t *g, float *x, size_t n, bool speech) {
    if (!n) return;
    float from = g->gain_db;
    if (speech) {
        float ms = 0;
        for (size_t i = 0; i < n; i++) ms += x[i] * x[i];
        float level = 10 * log10f(ms / n + 1e-12f);
        float want = clampf(g->target_dbfs - level, g->min_db, g->max_db);
        g->gain_db = from + clampf(want - from, -g->down_db, g->up_db);
    }
    float g0 = powf(10, from / 20), g1 = powf(10, g->gain_db / 20);
    for (size_t i = 0; i < n; i++) x[i] *= g0 + (g1 - g0) * (float)(i + 1) / n;
}

void yk_limiter_init(yk_limiter_t *l, float rate, float threshold_dbfs, float release_ms) {
    l->threshold = powf(10, threshold_dbfs / 20);
    l->release = expf(-1000 / (rate * release_ms));
    l->env = 0;
}

void yk_limiter_run(yk_limiter_t *l, float *x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float v = fabsf(x[i]);
        l->env = v > l->env * l->release ? v : l->env * l->release;
        if (l->env > l->threshold) x[i] *= l->threshold / l->env;
    }
}

bool yk_gate_speech(yk_gate_t *g, const float *x, size_t n) {
    if (!n) return false;
    float e = 0;
    for (size_t i = 0; i < n; i++) e += x[i] * x[i];
    e /= n;
    bool speech = g->primed && e > 1e-8f && e > 4 * g->noise;
    if (!g->primed || e < g->noise) g->noise = e;
    else g->noise *= 1.01f;
    g->primed = true;
    return speech;
}

uint8_t yk_level(const float *x, size_t n) {
    float peak = 0;
    for (size_t i = 0; i < n; i++) peak = fmaxf(peak, fabsf(x[i]));
    if (peak <= 0.001f) return 0;  // below -60 dBFS
    float db = 20 * log10f(peak);
    return (uint8_t)clampf((db + 60) * 100 / 60, 0, 100);
}

void yk_from_pcm16(const int16_t *in, size_t stride, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i * stride] / 32768.0f;
}

void yk_to_pcm16(const float *in, int16_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = (int16_t)lrintf(clampf(in[i], -1, 1) * 32767);
}

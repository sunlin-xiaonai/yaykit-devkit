// Signal-processing blocks of the shared audio front end. Pure C on float samples in [-1, 1] (host tested);
// see docs/architecture/audio.md. The ESP32-S3 has a single-precision FPU, so float is the working type.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Biquad in transposed direct form II. */
typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} yk_biquad_t;

/** Second-order high-pass (RBJ cookbook); q = 0.7071 is Butterworth. */
void yk_biquad_highpass(yk_biquad_t *f, float rate, float hz, float q);
/** Low shelf (RBJ cookbook, slope 1). gain_db = 0 passes the signal unchanged. */
void yk_biquad_lowshelf(yk_biquad_t *f, float rate, float hz, float gain_db);
void yk_biquad_run(yk_biquad_t *f, float *x, size_t n);

/** Rational resampler: a Kaiser-windowed sinc evaluated as a polyphase FIR, streaming in any chunk size up to
 *  YK_RS_MAX_IN. Output is delayed by (taps - 1) / 2 samples of the up-sampled rate. */
#define YK_RS_MAX_TAPS 192
#define YK_RS_MAX_IN   960
typedef struct {
    int up, down, taps;
    float h[YK_RS_MAX_TAPS];
    float buf[YK_RS_MAX_TAPS + YK_RS_MAX_IN];
    int held;  // samples in buf
    int t;     // up-sampled time of the next output, relative to buf[0]
} yk_resampler_t;

/** 24 kHz -> 16 kHz: flat to 7 kHz, at least 60 dB down from 8 kHz (the new Nyquist). */
void yk_resampler_init_24k_to_16k(yk_resampler_t *r);
/** Returns the number of outputs written (at most n * up / down + 1). n must not exceed YK_RS_MAX_IN. */
size_t yk_resampler_run(yk_resampler_t *r, const float *in, size_t n, float *out);

/** Steers two first-order differential (figure-8) channels at the talker: the output is w·[a, b] with w the
 *  principal direction of their smoothed 2×2 covariance. For a single talker that is the highest-SNR linear
 *  combination, and it does not depend on either channel's polarity. Updates only on frames well above the
 *  noise floor and holds when the field has no clear direction. */
typedef struct {
    float raa, rbb, rab;  // smoothed covariance
    float theta;          // current weight angle: w = (cos, sin)
    float noise;          // noise floor of the frame energy
    float alpha;          // covariance smoothing per frame
    float max_step;       // largest weight rotation per frame, radians
    bool have_noise, have_cov;
} yk_combiner_t;

/** tau_frames: covariance time constant in frames; max_step: radians per frame. */
void yk_combiner_init(yk_combiner_t *c, float tau_frames, float max_step);
/** One frame (call with a fixed frame length, e.g. 20 ms). out may alias a or b. */
void yk_combiner_run(yk_combiner_t *c, const float *a, const float *b, float *out, size_t n);

/** Slow automatic gain: moves toward target_dbfs only on speech frames and holds in pauses, so background
 *  noise is not pumped up between sentences. Gain changes are ramped across the frame. */
typedef struct {
    float gain_db, target_dbfs, min_db, max_db, up_db, down_db;  // up/down: dB per frame
} yk_agc_t;

void yk_agc_init(yk_agc_t *g, float frame_seconds, float target_dbfs, float min_db, float max_db,
                 float up_db_per_s, float down_db_per_s);
void yk_agc_run(yk_agc_t *g, float *x, size_t n, bool speech);

/** Peak limiter with instant attack: |output| never exceeds the threshold. */
typedef struct {
    float threshold, release, env;
} yk_limiter_t;

void yk_limiter_init(yk_limiter_t *l, float rate, float threshold_dbfs, float release_ms);
void yk_limiter_run(yk_limiter_t *l, float *x, size_t n);

/** Energy gate for when no voice-activity model runs: tracks the noise floor of frame energy (falls at once,
 *  rises about 2 dB/s at 20 ms frames) and calls a frame speech when it is 6 dB above it. */
typedef struct {
    float noise;
    bool primed;
} yk_gate_t;

bool yk_gate_speech(yk_gate_t *g, const float *x, size_t n);

/** Level meter 0..100 over -60..0 dBFS peak. */
uint8_t yk_level(const float *x, size_t n);

void yk_from_pcm16(const int16_t *in, size_t stride, float *out, size_t n);
void yk_to_pcm16(const float *in, int16_t *out, size_t n);

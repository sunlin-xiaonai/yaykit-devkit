// The front end's pure-C pipeline: board microphone map, per-mode settings and the two processing stages
// around the noise-suppression model (host tested). See docs/architecture/audio.md.
//
//   capture: TDM frames -> pick the two microphone signals -> high-pass -> steer and combine
//            -> optional low shelf -> 16 kHz mono
//   [device: ESP-SR nsnet2 noise suppression and voice activity, which re-chunk to their own frame size]
//   finish:  automatic gain (speech frames only) -> limiter -> PCM16 and level
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "yk_dsp.h"

/** Which TDM slots carry microphone signals on a board, and how. Moves to the board package once Yaykit
 *  maintains it; a board revision only changes this table. */
typedef struct {
    int rate;        // TDM sample rate the driver runs at
    int slots;       // TDM slots per frame
    int mic_a, mic_b;  // the two microphone signals
    float sign_a, sign_b;
    int reference;   // speaker loopback slot for echo cancellation, or -1
} yk_mics_t;

/** Tikpal R1: slot0 = MIC4 − MIC1 and slot2 = MIC3 − MIC2 (diagonal differentials, 41.8 mm), slot1 = speaker
 *  loopback, slot3 unconnected; 24 kHz. Slot2's polarity was measured inverted against slot0 (Pom, 2026-10-10);
 *  the combiner does not depend on it, so it only matters for code that mixes the slots directly. */
extern const yk_mics_t YK_MICS_TIKPAL_R1;
/** The same board with I2S at 16 kHz (the main firmware): the front end skips the resampler. */
extern const yk_mics_t YK_MICS_TIKPAL_R1_16K;

typedef enum {
    YK_AUDIO_RECORD,        // long recordings, no playback: steady steering, slow gain
    YK_AUDIO_CONVERSE,      // speaking while the device talks: echo cancellation (device side), faster gain
    YK_AUDIO_PUSH_TO_TALK,  // a few seconds of speech for recognition: fast steering
} yk_audio_mode_t;

#define YK_FE_RATE 16000
/** Input frame: 20 ms at the TDM rate. Output: 20 ms at 16 kHz. */
#define YK_FE_MAX_IN  480
#define YK_FE_MAX_OUT 330

typedef struct {
    float highpass_hz;
    float steer_tau_frames, steer_max_step;  // combiner time constant (frames) and rotation limit (rad/frame)
    float shelf_hz, shelf_db;                // low-frequency make-up for the differential pairs; 0 dB = off
    float agc_target_dbfs, agc_min_db, agc_max_db, agc_up_db_s, agc_down_db_s;
    float limit_dbfs, limit_release_ms;
} yk_fe_config_t;

/** Settings for a mode. The values are a starting point until the baseline recordings tune them. */
void yk_fe_config_for(yk_audio_mode_t mode, yk_fe_config_t *cfg);

typedef struct {
    yk_mics_t mics;
    yk_fe_config_t cfg;
    yk_biquad_t hp_a, hp_b, shelf;
    yk_combiner_t steer;
    yk_resampler_t rs;
    yk_agc_t agc;
    yk_limiter_t limiter;
    yk_gate_t gate;
    float a[YK_FE_MAX_IN], b[YK_FE_MAX_IN];
} yk_fe_t;

void yk_fe_init(yk_fe_t *fe, const yk_mics_t *mics, const yk_fe_config_t *cfg);

/** First stage: `frames` interleaved TDM frames (at most YK_FE_MAX_IN, one 20 ms frame per call keeps the
 *  steering's time constant in step) to 16 kHz mono floats; at mics.rate 16000 without resampling. Returns how
 *  many were written to out. */
size_t yk_fe_capture(yk_fe_t *fe, const int16_t *tdm, size_t frames, float *out);

/** Second stage, after noise suppression: gain and limiter in place, then PCM16. `speech` is the voice-activity
 *  decision for these samples; pass a negative value to use the built-in energy gate. Returns the level 0..100. */
uint8_t yk_fe_finish(yk_fe_t *fe, float *x, size_t n, int speech, int16_t *pcm);

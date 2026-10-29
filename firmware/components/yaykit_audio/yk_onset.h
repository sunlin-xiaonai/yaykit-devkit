// Pure C check for the first-word test (host tested with test/run.sh): where a known clip shows up in a
// recording, and whether its first 150 ms survived the front end at the same level as the rest.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int onset_ms;       // the clip's speech onset, in the recording's time
    int lag_ms;         // where the clip starts in the recording
    float corr;         // envelope correlation at that lag, -1..1
    float early_ratio;  // gain of the first 150 ms of speech relative to the gain of all of it (1 = untouched)
} yk_onset_t;

/** 10 ms energy envelope of x in dB (10 log10(mean square + 1)). Returns the blocks written, at most max. */
size_t yk_env_db(const int16_t *x, size_t n, int rate, float *env, size_t max);

/** Finds ref (the clip played) in cap (what the microphones made of it), same rate. False when ref has no
 *  speech or cap is shorter than ref. */
bool yk_onset_match(const int16_t *ref, size_t nref, const int16_t *cap, size_t ncap, int rate, yk_onset_t *out);

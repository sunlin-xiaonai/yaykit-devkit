#include "yk_onset.h"

#include <math.h>
#include <stdlib.h>

#define EARLY 15        // blocks: the first 150 ms of speech
#define SPEECH_DB 20.0f // a block within 20 dB of the loudest counts as speech

size_t yk_env_db(const int16_t *x, size_t n, int rate, float *env, size_t max) {
    const size_t block = (size_t)rate / 100;
    size_t k = 0;
    for (; k < max && (k + 1) * block <= n; k++) {
        double sum = 0;
        for (size_t i = 0; i < block; i++) sum += (double)x[k * block + i] * x[k * block + i];
        env[k] = 10.0f * log10f((float)(sum / block) + 1.0f);
    }
    return k;
}

static double lin(float db) { return pow(10.0, db / 10.0); }

bool yk_onset_match(const int16_t *ref, size_t nref, const int16_t *cap, size_t ncap, int rate, yk_onset_t *out) {
    const size_t block = (size_t)rate / 100;
    const size_t nr = nref / block, nc = ncap / block;
    if (!nr || nc < nr) return false;
    float *er = malloc(nr * sizeof(float)), *ec = malloc(nc * sizeof(float));
    bool ok = false;
    if (!er || !ec) goto done;
    yk_env_db(ref, nref, rate, er, nr);
    yk_env_db(cap, ncap, rate, ec, nc);
    float top = er[0];
    for (size_t k = 1; k < nr; k++) top = er[k] > top ? er[k] : top;
    size_t on = 0, last = 0;
    while (on < nr && er[on] < top - SPEECH_DB) on++;
    for (size_t k = 0; k < nr; k++)
        if (er[k] >= top - SPEECH_DB) last = k;
    if (on >= nr || top < 20) goto done;  // silence
    // The lag where the two envelopes move together best (Pearson over the clip's length).
    double mr = 0, sr = 0;
    for (size_t k = 0; k < nr; k++) mr += er[k];
    mr /= nr;
    for (size_t k = 0; k < nr; k++) sr += (er[k] - mr) * (er[k] - mr);
    double best = -2;
    size_t lag = 0;
    for (size_t l = 0; l + nr <= nc; l++) {
        double mc = 0, sc = 0, sx = 0;
        for (size_t k = 0; k < nr; k++) mc += ec[l + k];
        mc /= nr;
        for (size_t k = 0; k < nr; k++) {
            sc += (ec[l + k] - mc) * (ec[l + k] - mc);
            sx += (er[k] - mr) * (ec[l + k] - mc);
        }
        const double c = sr > 0 && sc > 0 ? sx / sqrt(sr * sc) : 0;
        if (c > best) best = c, lag = l;
    }
    // Gain over the first 150 ms of speech against the gain over all of it.
    double re = 0, ce = 0, ra = 0, ca = 0;
    for (size_t k = on; k <= last; k++) {
        ra += lin(er[k]), ca += lin(ec[lag + k]);
        if (k < on + EARLY) re += lin(er[k]), ce += lin(ec[lag + k]);
    }
    out->onset_ms = (int)((lag + on) * 10);
    out->lag_ms = (int)(lag * 10);
    out->corr = (float)best;
    out->early_ratio = re > 0 && ra > 0 && ca > 0 ? (float)((ce / re) / (ca / ra)) : 0;
    ok = true;
done:
    free(er);
    free(ec);
    return ok;
}

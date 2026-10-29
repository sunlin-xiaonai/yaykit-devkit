#include "yk_fe.h"

#include <string.h>

const yk_mics_t YK_MICS_TIKPAL_R1 = {
    .rate = 24000,
    .slots = 4,
    .mic_a = 0,
    .mic_b = 2,
    .sign_a = 1,
    .sign_b = -1,
    .reference = 1,
};

const yk_mics_t YK_MICS_TIKPAL_R1_16K = {
    .rate = 16000,
    .slots = 4,
    .mic_a = 0,
    .mic_b = 2,
    .sign_a = 1,
    .sign_b = -1,
    .reference = 1,
};

void yk_fe_config_for(yk_audio_mode_t mode, yk_fe_config_t *cfg) {
    // Shared: rumble and handling noise below 100 Hz carry no speech; peaks stay under -1 dBFS.
    *cfg = (yk_fe_config_t){
        .highpass_hz = 100,
        .shelf_hz = 300,
        .shelf_db = 0,
        .limit_dbfs = -1,
        .limit_release_ms = 100,
    };
    switch (mode) {
    case YK_AUDIO_RECORD:
        // A meeting moves between talkers: follow within about 0.1 s, but never swing more than ~30° a frame.
        cfg->steer_tau_frames = 5;
        cfg->steer_max_step = 0.5f;
        cfg->agc_target_dbfs = -20;
        cfg->agc_min_db = -6;
        cfg->agc_max_db = 18;
        cfg->agc_up_db_s = 3;
        cfg->agc_down_db_s = 10;
        break;
    case YK_AUDIO_CONVERSE:
        cfg->steer_tau_frames = 3;
        cfg->steer_max_step = 0.6f;
        cfg->agc_target_dbfs = -18;
        cfg->agc_min_db = -6;
        cfg->agc_max_db = 24;
        cfg->agc_up_db_s = 8;
        cfg->agc_down_db_s = 20;
        break;
    case YK_AUDIO_PUSH_TO_TALK:
    default:
        cfg->steer_tau_frames = 2;
        cfg->steer_max_step = 0.8f;
        cfg->agc_target_dbfs = -18;
        cfg->agc_min_db = -6;
        cfg->agc_max_db = 24;
        cfg->agc_up_db_s = 12;
        cfg->agc_down_db_s = 20;
        break;
    }
}

void yk_fe_init(yk_fe_t *fe, const yk_mics_t *mics, const yk_fe_config_t *cfg) {
    memset(fe, 0, sizeof(*fe));
    fe->mics = *mics;
    fe->cfg = *cfg;
    yk_biquad_highpass(&fe->hp_a, (float)mics->rate, cfg->highpass_hz, 0.7071f);
    yk_biquad_highpass(&fe->hp_b, (float)mics->rate, cfg->highpass_hz, 0.7071f);
    yk_biquad_lowshelf(&fe->shelf, (float)mics->rate, cfg->shelf_hz, cfg->shelf_db);
    yk_combiner_init(&fe->steer, cfg->steer_tau_frames, cfg->steer_max_step);
    yk_resampler_init_24k_to_16k(&fe->rs);
    yk_agc_init(&fe->agc, 0.02f, cfg->agc_target_dbfs, cfg->agc_min_db, cfg->agc_max_db, cfg->agc_up_db_s,
                cfg->agc_down_db_s);
    yk_limiter_init(&fe->limiter, YK_FE_RATE, cfg->limit_dbfs, cfg->limit_release_ms);
}

size_t yk_fe_capture(yk_fe_t *fe, const int16_t *tdm, size_t frames, float *out) {
    if (frames > YK_FE_MAX_IN) frames = YK_FE_MAX_IN;
    yk_from_pcm16(tdm + fe->mics.mic_a, (size_t)fe->mics.slots, fe->a, frames);
    yk_from_pcm16(tdm + fe->mics.mic_b, (size_t)fe->mics.slots, fe->b, frames);
    for (size_t i = 0; i < frames; i++) {
        fe->a[i] *= fe->mics.sign_a;
        fe->b[i] *= fe->mics.sign_b;
    }
    yk_biquad_run(&fe->hp_a, fe->a, frames);
    yk_biquad_run(&fe->hp_b, fe->b, frames);
    yk_combiner_run(&fe->steer, fe->a, fe->b, fe->a, frames);
    if (fe->cfg.shelf_db != 0) yk_biquad_run(&fe->shelf, fe->a, frames);
    if (fe->mics.rate == YK_FE_RATE) {  // the driver already runs at 16 kHz: nothing to resample
        memcpy(out, fe->a, frames * sizeof(float));
        return frames;
    }
    return yk_resampler_run(&fe->rs, fe->a, frames, out);
}

uint8_t yk_fe_finish(yk_fe_t *fe, float *x, size_t n, int speech, int16_t *pcm) {
    bool talking = speech < 0 ? yk_gate_speech(&fe->gate, x, n) : speech > 0;
    yk_agc_run(&fe->agc, x, n, talking);
    yk_limiter_run(&fe->limiter, x, n);
    if (pcm) yk_to_pcm16(x, pcm, n);
    return yk_level(x, n);
}

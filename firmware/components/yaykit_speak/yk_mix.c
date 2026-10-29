#include "yk_mix.h"

#include <math.h>
#include <string.h>

#define DUCK_MS 30      // the duck follows an utterance's start and end this fast
#define RELEASE_MS 80   // the limiter lets go this slowly

void yk_mix_init(yk_mix_t *m, uint32_t rate) {
    memset(m, 0, sizeof(*m));
    m->rate = rate;
    m->next_id = 1;
    m->duck = 1;
    m->lim_gain = 1;
}

static yk_mix_voice_t *find(yk_mix_t *m, int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < YK_SFX_VOICES; i++)
        if (m->v[i].id == id) return &m->v[i];
    return NULL;
}

int yk_mix_active(const yk_mix_t *m) {
    int n = 0;
    for (int i = 0; i < YK_SFX_VOICES; i++) n += m->v[i].id != 0;
    return n;
}

bool yk_mix_playing(const yk_mix_t *m, int id) {
    if (id <= 0) return false;
    for (int i = 0; i < YK_SFX_VOICES; i++)
        if (m->v[i].id == id) return true;
    return false;
}

int yk_mix_add(yk_mix_t *m, const yk_clip_t *clip, yk_sfx_render_fn render, void *ctx, const yk_sfx_opts_t *opts,
               uint64_t start, uint64_t now) {
    static const yk_sfx_opts_t none = {0};
    const yk_sfx_opts_t *o = opts ? opts : &none;
    if (!clip == !render) return YK_SFX_BAD;
    if (clip) {
        if (!clip->data || !clip->samples) return YK_SFX_BAD;
        if (clip->format == YK_CLIP_PCM16 && clip->bytes < (size_t)clip->samples * 2) return YK_SFX_BAD;
        if (clip->format == YK_CLIP_IMA_ADPCM && clip->bytes < yk_adpcm_bytes(clip->samples)) return YK_SFX_BAD;
        if (clip->format != YK_CLIP_PCM16 && clip->format != YK_CLIP_IMA_ADPCM) return YK_SFX_BAD;
    }
    if (o->gain < 0 || o->pitch < 0) return YK_SFX_BAD;
    if (start < now) start = now;
    if (o->group) {
        const uint64_t last = m->group_at[o->group];
        const uint64_t gap = (uint64_t)o->min_gap_ms * m->rate / 1000;
        if (last && start + 1 < last + gap) {
            m->dropped++;
            return YK_SFX_BUSY;
        }
    }
    yk_mix_voice_t *v = NULL;
    for (int i = 0; i < YK_SFX_VOICES && !v; i++)
        if (!m->v[i].id) v = &m->v[i];
    if (!v) {
        m->dropped++;
        return YK_SFX_BUSY;
    }
    memset(v, 0, sizeof(*v));
    if (clip) {
        v->format = clip->format;
        v->data = clip->data;
        v->bytes = clip->bytes;
        v->samples = clip->samples;
    }
    v->render = render;
    v->ctx = ctx;
    v->start = start;
    v->gain = v->gain_to = o->gain > 0 ? o->gain : 1;
    v->pitch = o->pitch > 0 ? (o->pitch < 0.5f ? 0.5f : o->pitch > 2 ? 2 : o->pitch) : 1;
    v->no_duck = o->no_duck;
    v->loop = o->loop && clip;
    if (o->group) m->group_at[o->group] = start + 1;
    v->id = m->next_id++;
    if (m->next_id <= 0) m->next_id = 1;
    m->started++;
    const int active = yk_mix_active(m);
    if ((uint32_t)active > m->max_active) m->max_active = (uint32_t)active;
    return v->id;
}

static void stop_voice(yk_mix_voice_t *v, uint32_t fade) {
    if (!v->id) return;
    if (!fade) fade = 1;
    if (v->fade_left && v->fade_left <= fade) return;  // already going out at least as fast
    v->fade_left = fade;
    v->fade_step = v->gain / (float)fade;
    v->gain_to = v->gain;
    v->gain_step = 0;
}

void yk_mix_stop(yk_mix_t *m, int id, uint32_t fade) {
    if (!id) {
        for (int i = 0; i < YK_SFX_VOICES; i++) stop_voice(&m->v[i], fade);
        return;
    }
    yk_mix_voice_t *v = find(m, id);
    if (v) stop_voice(v, fade);
}

void yk_mix_gain(yk_mix_t *m, int id, float gain, uint32_t ramp) {
    yk_mix_voice_t *v = find(m, id);
    if (!v || v->fade_left) return;
    if (gain < 0) gain = 0;
    v->gain_to = gain;
    v->gain_step = ramp ? (gain - v->gain) / (float)ramp : 0;
    if (!ramp) v->gain = gain;
}

/** The clip's sample `i` from a sequential decode (ADPCM keeps a running state; PCM reads directly). Returns false
 *  past the end. */
static bool fetch(yk_mix_voice_t *v, float *x) {
    if (v->next >= v->samples) {
        if (!v->loop) return false;
        v->next = 0;
    }
    const uint32_t i = v->next++;
    if (v->format == YK_CLIP_PCM16) {
        const uint8_t *p = v->data + 2 * (size_t)i;
        *x = (int16_t)(uint16_t)(p[0] | (p[1] << 8)) / 32768.0f;
        return true;
    }
    if (i == 0) {
        int16_t first;
        yk_adpcm_begin(v->data, v->bytes, &v->adpcm, &first);
        *x = first / 32768.0f;
        return true;
    }
    const uint8_t b = v->data[YK_ADPCM_HEADER + (i - 1) / 2];
    *x = yk_adpcm_step(&v->adpcm, (i - 1) % 2 ? b >> 4 : b & 15) / 32768.0f;
    return true;
}

/** Up to n samples of a clip into out; returns how many before it ended. */
static size_t clip_render(yk_mix_voice_t *v, float *out, size_t n) {
    if (v->pitch == 1) {
        for (size_t k = 0; k < n; k++)
            if (!fetch(v, &out[k])) return k;
        return n;
    }
    if (!v->primed) {
        v->s0 = v->s1 = 0;
        if (!fetch(v, &v->s0)) return 0;
        if (!fetch(v, &v->s1)) v->s1 = 0, v->ended = true;
        v->frac = 0;
        v->primed = true;
    }
    for (size_t k = 0; k < n; k++) {
        out[k] = v->s0 + (v->s1 - v->s0) * v->frac;
        v->frac += v->pitch;
        while (v->frac >= 1) {
            v->frac -= 1;
            v->s0 = v->s1;
            if (v->ended) return k + 1;  // past the last sample (and its glide to silence)
            if (!fetch(v, &v->s1)) v->s1 = 0, v->ended = true;
        }
    }
    return n;
}

int yk_mix_render(yk_mix_t *m, const int16_t *utter, size_t n, uint64_t sample0, int64_t heard_us0, int16_t *out) {
    if (n > YK_MIX_MAX_BLOCK) n = YK_MIX_MAX_BLOCK;
    float *mix = m->mix;
    memset(mix, 0, n * sizeof(float));
    // The duck: towards -6 dB while an utterance plays, back to 1 after, over DUCK_MS.
    const float duck_to = utter ? YK_MIX_DUCK : 1;
    const float duck_step = (1 - YK_MIX_DUCK) / (float)(m->rate * DUCK_MS / 1000);
    int played = 0;
    if (!yk_mix_active(m) && m->lim_gain >= 1) {
        // Nothing but the utterance: it passes untouched, as before the mixer (the main firmware's voice).
        if (utter) memcpy(out, utter, n * sizeof(int16_t));
        else memset(out, 0, n * sizeof(int16_t));
        m->duck = duck_to;
        return 0;
    }
    for (int i = 0; i < YK_SFX_VOICES; i++) {
        yk_mix_voice_t *v = &m->v[i];
        if (!v->id) continue;
        if (v->start >= sample0 + n) continue;  // not yet
        const size_t off = v->start > sample0 ? (size_t)(v->start - sample0) : 0;
        const size_t want = n - off;
        float *buf = m->voice;
        size_t got;
        bool done = false;
        if (v->render) {
            const yk_sfx_block_t blk = {
                .rate = m->rate,
                .sample = sample0 + off,
                .heard_us = heard_us0 + (int64_t)off * 1000000 / m->rate,
                .pitch = v->pitch,
            };
            memset(buf, 0, want * sizeof(float));
            done = !v->render(buf, want, &blk, v->ctx);
            got = want;
        } else {
            got = clip_render(v, buf, want);
            done = got < want;
        }
        played++;
        float duck = m->duck;
        for (size_t k = 0; k < got; k++) {
            if (v->fade_left) {
                v->gain -= v->fade_step;
                if (v->gain < 0) v->gain = 0;
                if (--v->fade_left == 0) {
                    got = k + 1;
                    done = true;
                }
            } else if (v->gain != v->gain_to) {
                v->gain += v->gain_step;
                if ((v->gain_step > 0 && v->gain > v->gain_to) || (v->gain_step < 0 && v->gain < v->gain_to) ||
                    v->gain_step == 0)
                    v->gain = v->gain_to;
            }
            if (!v->no_duck) duck += duck < duck_to ? duck_step : duck > duck_to ? -duck_step : 0;
            const float d = v->no_duck ? 1 : (duck < YK_MIX_DUCK ? YK_MIX_DUCK : duck > 1 ? 1 : duck);
            mix[off + k] += buf[k] * v->gain * d;
        }
        if (done) memset(v, 0, sizeof(*v));
    }
    // The duck moves once per block for everyone (the per-voice walk above is the same ramp).
    const float dmove = duck_step * (float)n;
    if (m->duck < duck_to) m->duck = m->duck + dmove > duck_to ? duck_to : m->duck + dmove;
    else if (m->duck > duck_to) m->duck = m->duck - dmove < duck_to ? duck_to : m->duck - dmove;
    // The utterance at full level, then the limiter at -3 dBFS: instant attack, RELEASE_MS release.
    const float rel = 1.0f / (float)(m->rate * RELEASE_MS / 1000);
    bool limited = false;
    for (size_t k = 0; k < n; k++) {
        float x = mix[k] + (utter ? utter[k] / 32768.0f : 0);
        const float a = fabsf(x);
        if (m->lim_gain < 1) {
            m->lim_gain += rel;
            if (m->lim_gain > 1) m->lim_gain = 1;
        }
        if (a * m->lim_gain > YK_MIX_CEILING) {
            m->lim_gain = YK_MIX_CEILING / a;
            limited = true;
        }
        x *= m->lim_gain;
        const float s = x * 32768.0f;
        out[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : lrintf(s));
    }
    if (limited) m->limited++;
    return played;
}

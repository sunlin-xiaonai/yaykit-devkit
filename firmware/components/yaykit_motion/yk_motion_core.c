#include "yk_motion_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI_F 3.14159265f
#define RAD (PI_F / 180.0f)
#define TAU_S 0.3f            // the accelerometer pulls gravity back with this time constant ...
#define NEAR_1G 0.15f         // ... only while |a| is within this of 1 g
#define PEAK_GAP_MS 60        // shake peaks closer than this are one (a knock on the table rings faster)
#define HOLDER_EVERY_MS 25
#define UP_TAU_S 0.05f        // the raw up component's low-pass (face down / up without the gyroscope)
#define RESEED_COS 0.5f       // the estimate and the accelerometer more than 60 degrees apart ...
#define RESEED_MS 250         // ... for this long, near 1 g: start the estimate again from the accelerometer

void yk_motion_default_thresholds(yk_motion_thresholds_t *th) {
    *th = (yk_motion_thresholds_t){
        .flat_deg = 25, .upright_deg = 60, .orient_ms = 500,
        .still_g = 0.04f, .still_dps = 4, .still_ms = 300,
        .lift_g = 0.15f, .lift_dps = 25, .lift_ms = 80, .lift_tilt_deg = 20, .lift_gap_ms = 2000,
        .shake_peak_g = 0.8f, .shake_peaks = 3, .shake_window_ms = 1000, .shake_end_ms = 400,
        .down_g = 0.75f, .down_probe_ms = 250, .down_ms = 600, .up_g = 0.30f, .up_ms = 400,
        .moving_dps = 15, .moving_g = 0.08f, .moving_window_ms = 1000,
        .holder_min_deg = 17, .holder_max_deg = 70,
    };
}

void yk_motion_default_cal(yk_motion_cal_t *cal) {
    // The device frame is x right, y up, z out of the glass. On the test device the sensor's z faces the back
    // (z = -1 g glass up, Breathe's measurement); turning it over about x keeps the frame right-handed.
    *cal = (yk_motion_cal_t){.axis = {1, -2, -3}};
}

void ykm_map(const yk_motion_cal_t *cal, const float in[3], float out[3]) {
    for (int i = 0; i < 3; i++) {
        const int a = cal->axis[i];
        const int k = (a < 0 ? -a : a) - 1;
        const float v = k >= 0 && k < 3 ? in[k] : 0;
        out[i] = a < 0 ? -v : v;
    }
}

static float norm3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/** Clockwise from the top, 0..360, of a direction (x right, y up). */
static float angle_of(float x, float y) {
    float a = atan2f(x, y) / RAD;
    if (a < 0) a += 360;
    if (a >= 360) a -= 360;
    return a;
}

void ykm_init(ykm_core_t *c, const yk_motion_thresholds_t *th, const yk_motion_cal_t *cal) {
    memset(c, 0, sizeof(*c));
    if (th) c->th = *th;
    else yk_motion_default_thresholds(&c->th);
    if (cal) c->cal = *cal;
    else yk_motion_default_cal(&c->cal);
}

void ykm_set_thresholds(ykm_core_t *c, const yk_motion_thresholds_t *th) { c->th = *th; }

/** One sample into the current second; when it is over and was steady, what |a| reads at rest and the gyroscope's
 *  bias are this second's means. */
static void steady_feed(ykm_core_t *c, float fn, const float raw_w[3], int64_t t) {
    if (!c->steady.n) c->steady.t0_ms = t;
    const int n = ++c->steady.n;
    float d = fn - c->steady.mean_a;
    c->steady.mean_a += d / (float)n;
    c->steady.m2_a += d * (fn - c->steady.mean_a);
    for (int i = 0; i < 3; i++) {
        d = raw_w[i] - c->steady.mean_w[i];
        c->steady.mean_w[i] += d / (float)n;
        c->steady.m2_w[i] += d * (raw_w[i] - c->steady.mean_w[i]);
    }
    if (t - c->steady.t0_ms < YKM_STEADY_MS) return;
    bool steady = n >= 10 && c->steady.m2_a / (float)n < YKM_STEADY_G * YKM_STEADY_G &&
                  c->steady.mean_a > YKM_REST_G_MIN && c->steady.mean_a < YKM_REST_G_MAX;
    for (int i = 0; i < 3; i++) steady = steady && c->steady.m2_w[i] / (float)n < YKM_STEADY_DPS * YKM_STEADY_DPS;
    if (steady) {
        c->rest_g_len = c->steady.mean_a;
        for (int i = 0; i < 3; i++) c->cal.gyro_bias[i] = c->steady.mean_w[i];
        c->settled = true;
    }
    memset(&c->steady, 0, sizeof(c->steady));
}

static int emit(yk_motion_event_t *ev, int n, int max, yk_motion_kind_t kind, int64_t t_ms) {
    if (n >= max) return n;
    ev[n] = (yk_motion_event_t){.kind = kind, .t_ms = t_ms};
    return n + 1;
}

/** Device frame to the screen's (y down); gyroscope as a pseudo-vector (sign flips with the reflection), so that
 *  gyro z > 0 turns clockwise as seen on the screen. */
static void to_screen(const float d[3], float s[3]) { s[0] = d[0], s[1] = -d[1], s[2] = d[2]; }
static void gyro_to_screen(const float d[3], float s[3]) { s[0] = -d[0], s[1] = d[1], s[2] = -d[2]; }

static float tilt_of(const float g[3]) {
    float up_z = -g[2];
    if (up_z > 1) up_z = 1;
    if (up_z < -1) up_z = -1;
    return acosf(up_z) / RAD;
}

static float shake_strength(const ykm_core_t *c, int64_t now_ms) {
    float sum = 0;
    int n = 0;
    for (int i = 0; i < YKM_RMS_LEN; i++)
        if (c->rms_t[i] && now_ms - c->rms_t[i] < 250) sum += c->rms_buf[i], n++;
    if (!n) return 0;
    const float rms = sqrtf(sum / (float)n);
    float s = (rms - 0.5f) / 2.5f;
    return s < 0 ? 0 : s > 1 ? 1 : s;
}

bool ykm_holder(ykm_core_t *c, int64_t end_ms, int window_ms, float *angle, float *conf) {
    float sx = 0, sy = 0;
    int all = 0, valid = 0;
    for (int i = 0; i < c->hist_n; i++) {
        const ykm_holder_t *h = &c->hist[i];
        if (h->t_ms > end_ms || h->t_ms <= end_ms - window_ms) continue;
        all++;
        if (h->x == 0 && h->y == 0) continue;
        valid++;
        sx += h->x, sy += h->y;
    }
    if (!valid) {
        if (!c->holder_known) return false;
        c->holder_conf *= 0.5f;
        *angle = c->holder_angle;
        *conf = c->holder_conf;
        return true;
    }
    const float r = sqrtf(sx * sx + sy * sy) / (float)valid;  // 1: every sample the same way
    c->holder_angle = angle_of(sx, sy);
    c->holder_conf = r * (float)valid / (float)all;
    c->holder_known = true;
    *angle = c->holder_angle;
    *conf = c->holder_conf;
    return true;
}

int ykm_feed(ykm_core_t *c, const float acc[3], const float gyr[3], int64_t t_us, yk_motion_event_t *ev, int max) {
    const yk_motion_thresholds_t *th = &c->th;
    const int64_t t = t_us / 1000;
    int n = 0;
    float f[3], w[3];
    ykm_map(&c->cal, acc, f);
    ykm_map(&c->cal, gyr, w);
    for (int i = 0; i < 3; i++) f[i] -= c->cal.acc_bias[i], w[i] -= c->cal.gyro_bias[i];
    const float fn = norm3(f);
    if (!c->started) {
        c->started = true;
        const float k = fn > 0.1f ? -1.0f / fn : 0;
        for (int i = 0; i < 3; i++) c->g[i] = fn > 0.1f ? f[i] * k : (i == 2 ? -1.0f : 0);
        c->t_us = t_us;
        c->last_motion_ms = t - 100000;
        c->last_lift_ms = t - 100000;
        c->rest_since_ms = t;
        c->quiet_since_ms = t;
        memcpy(c->rest_g, c->g, sizeof(c->g));
        c->up_raw = f[2];
    }
    float dt = (float)(t_us - c->t_us) / 1e6f;
    if (dt <= 0 || dt > 0.1f) dt = dt <= 0 ? 0 : 0.1f;
    c->t_us = t_us;

    // Gravity: turned by the gyroscope (a fixed direction seen from a turning body: dg = -w x g dt), pulled
    // towards the accelerometer's -f while that reads about 1 g.
    const float wr[3] = {w[0] * RAD, w[1] * RAD, w[2] * RAD};
    float *g = c->g;
    const float cx = wr[1] * g[2] - wr[2] * g[1], cy = wr[2] * g[0] - wr[0] * g[2], cz = wr[0] * g[1] - wr[1] * g[0];
    g[0] -= cx * dt, g[1] -= cy * dt, g[2] -= cz * dt;
    if (fn > 0.1f && fabsf(fn - 1) < NEAR_1G) {
        const float target[3] = {-f[0] / fn, -f[1] / fn, -f[2] / fn};
        // Pulling straight towards a target half a turn away does not turn the estimate at all (it only shrinks
        // and is normalised back): without the gyroscope a quick flip would never be followed. When the two stay
        // far apart while the device is about still, start again from the accelerometer.
        if (dot3(g, target) < RESEED_COS) c->disagree_ms += (int)(dt * 1000 + 0.5f);
        else c->disagree_ms = 0;
        if (c->disagree_ms >= RESEED_MS) {
            memcpy(g, target, sizeof(target));
            c->disagree_ms = 0;
        } else {
            const float a = dt / (TAU_S + dt);
            for (int i = 0; i < 3; i++) g[i] += (target[i] - g[i]) * a;
        }
    } else {
        c->disagree_ms = 0;
    }
    c->up_raw += (f[2] - c->up_raw) * (dt / (UP_TAU_S + dt));
    const float gn = norm3(g);
    if (gn > 0.01f)
        for (int i = 0; i < 3; i++) g[i] /= gn;
    float lin[3] = {f[0] + g[0], f[1] + g[1], f[2] + g[2]};
    const float lin_n = norm3(lin), w_n = norm3(w);

    c->s.t_us = t_us;
    to_screen(lin, c->s.lin);
    to_screen(g, c->s.gravity);
    gyro_to_screen(w, c->s.gyro);

    // Rest (strict) and moving (loose, over a window). Rest against this device's own |a| once a steady second has
    // shown it (with the gyroscope's bias from then); before that against 1 g, a little wider.
    {
        float raw_w[3];
        ykm_map(&c->cal, gyr, raw_w);
        steady_feed(c, fn, raw_w, t);
    }
    const bool rest_now = c->settled ? fabsf(fn - c->rest_g_len) < th->still_g && w_n < th->still_dps
                                     : fabsf(fn - 1) < th->still_g + YKM_UNSETTLED_G && w_n < th->still_dps;
    if (!rest_now) c->rest_since_ms = 0;
    else if (!c->rest_since_ms) c->rest_since_ms = t;
    const bool was_still = c->still;
    // (The gyroscope's bias is followed only in steady seconds: following it whenever rest counted, as before, let
    // a steady hand within still_dps drag it along.)
    c->still = c->rest_since_ms && t - c->rest_since_ms >= th->still_ms;
    if (w_n > th->moving_dps || lin_n > th->moving_g) c->last_motion_ms = t;
    const bool moving = t - c->last_motion_ms < th->moving_window_ms;
    if (moving != c->moving) {
        c->moving = moving;
        n = emit(ev, n, max, moving ? YK_MOTION_MOVING : YK_MOTION_STILL, t);
    }

    // Face down / up: the estimate follows a turn at once with the gyroscope; without one (all zeros: a feed of
    // accelerometer readings only) it lags, and the accelerometer's own up component is quicker.
    if (gyr[0] != 0 || gyr[1] != 0 || gyr[2] != 0) c->gyro_alive_ms = t;
    const bool gyro = c->gyro_alive_ms && t - c->gyro_alive_ms < 1000;
    const float tilt = tilt_of(g), up_z = gyro ? -g[2] : c->up_raw;

    // The holder: gravity's direction across the glass, while the screen is tilted towards someone.
    if (t - c->hist_last_ms >= HOLDER_EVERY_MS) {
        c->hist_last_ms = t;
        const float px = g[0], py = g[1], pn = sqrtf(px * px + py * py);
        const bool counts = tilt >= th->holder_min_deg && tilt <= th->holder_max_deg && pn > 0.01f;
        c->hist[c->hist_pos] = (ykm_holder_t){.t_ms = t, .x = counts ? px / pn : 0, .y = counts ? py / pn : 0};
        c->hist_pos = (c->hist_pos + 1) % YKM_HISTORY;
        if (c->hist_n < YKM_HISTORY) c->hist_n++;
    }

    // Pick-up: from rest, movement held for lift_ms, or the tilt changed by lift_tilt_deg; not on the way to
    // face down, not twice within lift_gap_ms.
    if (c->still) {
        memcpy(c->rest_g, g, sizeof(c->rest_g));
        c->lift_cond_since_ms = 0;
    }
    const bool armed = !c->was_moved && t - c->last_lift_ms >= th->lift_gap_ms && up_z > -0.5f;
    if (armed && !c->still) {
        const bool strong = lin_n > th->lift_g || w_n > th->lift_dps;
        if (!strong) c->lift_cond_since_ms = 0;
        else if (!c->lift_cond_since_ms) c->lift_cond_since_ms = t;
        float cosd = dot3(g, c->rest_g);
        if (cosd > 1) cosd = 1;
        const bool tilted = acosf(cosd) / RAD > th->lift_tilt_deg;
        if (tilted || (c->lift_cond_since_ms && t - c->lift_cond_since_ms >= th->lift_ms)) {
            c->last_lift_ms = t;
            c->was_moved = true;
            c->lift_cond_since_ms = 0;
            n = emit(ev, n, max, YK_MOTION_LIFT, t);
        }
    }

    // Shake: alternating peaks of linear acceleration.
    {
        const int slot = c->rms_pos;
        c->rms_buf[slot] = lin_n * lin_n;
        c->rms_t[slot] = t ? t : 1;
        c->rms_pos = (slot + 1) % YKM_RMS_LEN;
        if (lin_n > th->shake_peak_g) {
            if (!c->in_peak || lin_n > c->peak_best) {
                c->peak_best = lin_n;
                c->peak_t_ms = t;
                for (int i = 0; i < 3; i++) c->peak_dir[i] = lin[i] / lin_n;
            }
            c->in_peak = true;
        } else if (c->in_peak && lin_n < th->shake_peak_g * 0.5f) {
            c->in_peak = false;
            if (c->npeaks && c->peak_t_ms - c->peaks[c->npeaks - 1].t_ms < PEAK_GAP_MS &&
                dot3(c->peak_dir, c->peaks[c->npeaks - 1].d) > 0) {
                // the same push, ringing: keep the first
            } else {
                if (c->npeaks == YKM_SHAKE_PEAKS) {
                    memmove(c->peaks, c->peaks + 1, sizeof(c->peaks[0]) * (YKM_SHAKE_PEAKS - 1));
                    c->npeaks--;
                }
                c->peaks[c->npeaks].t_ms = c->peak_t_ms;
                memcpy(c->peaks[c->npeaks].d, c->peak_dir, sizeof(c->peak_dir));
                c->npeaks++;
                c->last_peak_ms = c->peak_t_ms;
            }
        }
        // the run of alternating peaks, newest back, within the window
        int run = c->npeaks ? 1 : 0;
        for (int i = c->npeaks - 1; i > 0; i--) {
            if (c->peaks[c->npeaks - 1].t_ms - c->peaks[i - 1].t_ms > th->shake_window_ms) break;
            if (c->peaks[i].t_ms - c->peaks[i - 1].t_ms < PEAK_GAP_MS) break;
            if (dot3(c->peaks[i].d, c->peaks[i - 1].d) >= 0) break;
            run++;
        }
        if (!c->shaking && run >= th->shake_peaks && t - c->last_peak_ms < th->shake_window_ms) {
            c->shaking = true;
            c->was_moved = true;
            if (n < max) {
                ev[n] = (yk_motion_event_t){.kind = YK_MOTION_SHAKE_START, .t_ms = t, .strength = shake_strength(c, t)};
                n++;
            }
        } else if (c->shaking && t - c->last_peak_ms > th->shake_end_ms && !c->in_peak) {
            c->shaking = false;
            c->npeaks = 0;
            n = emit(ev, n, max, YK_MOTION_SHAKE_END, t);
        }
    }

    // Put down: at rest again after a pick-up or a shake; the holder from the half second before it came to rest.
    if (c->still && !was_still && c->was_moved) {
        c->was_moved = false;
        float a = 0, conf = 0;
        if (!ykm_holder(c, c->rest_since_ms, 500, &a, &conf)) a = 0, conf = 0;
        if (n < max) {
            ev[n] = (yk_motion_event_t){.kind = YK_MOTION_PUT_DOWN, .t_ms = t, .angle = a, .confidence = conf};
            n++;
        }
    }

    // Flat / upright (and which way up).
    int cand = 0;
    float up_angle = angle_of(-g[0], -g[1]);
    if (tilt < th->flat_deg) cand = 1;
    else if (tilt > th->upright_deg && tilt < 180 - th->upright_deg) cand = 2 + ((int)lroundf(up_angle / 90.0f) % 4);
    if (cand != c->orient_cand) {
        c->orient_cand = cand;
        c->orient_since_ms = t;
    }
    if (cand && cand != c->orient && t - c->orient_since_ms >= th->orient_ms) {
        c->orient = cand;
        if (n < max) {
            ev[n] = (yk_motion_event_t){.kind = cand == 1 ? YK_MOTION_FLAT : YK_MOTION_UPRIGHT, .t_ms = t,
                                        .angle = cand == 1 ? 0 : up_angle};
            n++;
        }
    }

    // Face down: a probe, then for real; turned back over.
    if (c->down == 0) {
        if (up_z < -th->down_g) {
            if (!c->down_since_ms) c->down_since_ms = t;
            if (t - c->down_since_ms >= th->down_probe_ms) {
                c->down = 1;
                c->up_since_ms = 0;
                n = emit(ev, n, max, YK_MOTION_FACE_DOWN_PROBE, t);
            }
        } else {
            c->down_since_ms = 0;
        }
    }
    if (c->down >= 1) {
        if (c->down == 1 && up_z < -th->down_g && t - c->down_since_ms >= th->down_ms) {
            c->down = 2;
            n = emit(ev, n, max, YK_MOTION_FACE_DOWN, t);
        }
        if (up_z > -th->up_g) {
            if (!c->up_since_ms) c->up_since_ms = t;
            if (t - c->up_since_ms >= th->up_ms) {
                c->down = 0;
                c->down_since_ms = 0;
                n = emit(ev, n, max, YK_MOTION_FACE_UP, t);
            }
        } else {
            c->up_since_ms = 0;
        }
    }
    return n;
}

void ykm_state(const ykm_core_t *c, yk_motion_state_t *out) {
    memset(out, 0, sizeof(*out));
    out->now = c->s;
    out->tilt_deg = tilt_of(c->g);
    out->up_angle = angle_of(-c->g[0], -c->g[1]);
    out->flat = c->orient == 1;
    out->upright = c->orient >= 2;
    out->face_down = c->down == 2;
    out->still = c->still;
    out->moving = c->moving;
    out->shaking = c->shaking;
    out->shake_strength = shake_strength(c, c->t_us / 1000);
    out->holder_angle = c->holder_angle;
    out->holder_confidence = c->holder_conf;
}

bool ykm_csv_parse(const char *line, int64_t *t_us, float acc[3], float gyr[3]) {
    long long t;
    char tail;
    if (sscanf(line, "%lld,%f,%f,%f,%f,%f,%f%c", &t, &acc[0], &acc[1], &acc[2], &gyr[0], &gyr[1], &gyr[2], &tail) <
        7)
        return false;
    *t_us = t;
    return true;
}

int ykm_csv_format(char *out, size_t size, int64_t t_us, const float acc[3], const float gyr[3]) {
    return snprintf(out, size, "%lld,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f", (long long)t_us, acc[0], acc[1], acc[2], gyr[0],
                    gyr[1], gyr[2]);
}

const char *ykm_kind_name(yk_motion_kind_t kind) {
    static const char *const names[YK_MOTION_KIND_COUNT] = {
        "none", "flat", "upright", "lift", "put_down", "shake_start", "shake_end", "face_down_probe", "face_down",
        "face_up", "moving", "still",
    };
    return (unsigned)kind < YK_MOTION_KIND_COUNT ? names[kind] : "?";
}

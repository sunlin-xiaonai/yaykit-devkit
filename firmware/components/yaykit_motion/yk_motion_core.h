// Pure C part of yaykit_motion (host tested with test/run.sh, replaying recorded or synthesised CSV): the gravity
// estimate (a complementary filter: the gyroscope turns it, the accelerometer pulls it back while near 1 g), linear
// acceleration, rest and movement, the events, the holder's direction. Single-threaded: the motion task owns it.
//
// Rest is measured against the device's own reading at rest, not against an ideal 1 g and a zero gyroscope: one
// test device reads 0.963 g lying flat, another has a 9.5 dps gyroscope bias. Every second in which the readings
// hardly vary (a "steady" second: |a| within YKM_STEADY_G and each gyroscope axis within YKM_STEADY_DPS, standard
// deviations) sets what |a| reads at rest and the gyroscope's bias. A hand holding the device varies far more than
// that, and what such a second would set (the length of gravity, the bias) barely depends on how it is held. Until
// the first steady second, rest is |a| within still_g + YKM_UNSETTLED_G of 1 g. Learned afresh at every start (a
// second lying still), not kept in NVS.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "yaykit_motion.h"

#define YKM_HISTORY 64       // holder samples, one per 25 ms: 1.6 s
#define YKM_SHAKE_PEAKS 8
#define YKM_RMS_LEN 64       // linear acceleration magnitudes for the 250 ms RMS (at up to 256 Hz)
#define YKM_STEADY_MS 1000   // a steady second ...
#define YKM_STEADY_G 0.003f  // ... |a| varies less than this (standard deviation; 0.001 at rest on the test devices)
#define YKM_STEADY_DPS 0.4f  // ... and each gyroscope axis less than this (0.2 at rest)
#define YKM_REST_G_MIN 0.85f // what |a| reads at rest is taken only within these (a sensor further off is broken)
#define YKM_REST_G_MAX 1.15f
#define YKM_UNSETTLED_G 0.04f  // before the first steady second: rest is |a| within still_g + this of 1 g

typedef struct {
    int64_t t_ms;
    float x, y;  // unit vector of gravity's in-screen direction, weighted 0 when it does not count
} ykm_holder_t;

typedef struct {
    yk_motion_thresholds_t th;
    yk_motion_cal_t cal;
    bool started;
    int64_t t_us;                  // last sample
    yk_motion_sample_t s;          // last processed sample
    float g[3];                    // gravity estimate (unit, down)
    float up_raw;                  // the accelerometer's up component low-passed (~50 ms): face down / up use it
                                   // when there is no gyroscope (the estimate then lags a turn)
    int64_t gyro_alive_ms;         // the last sample whose gyroscope reading was not exactly zero
    int disagree_ms;               // near 1 g but the accelerometer more than 60 degrees from the estimate: after
                                   // a while the estimate is re-seeded (no gyroscope, or a turn too fast to follow)
    // the device's own rest (learned in steady seconds; see above)
    bool settled;                  // a steady second was seen: rest_g_len and cal.gyro_bias are this device's
    float rest_g_len;              // |a| at rest, g
    struct {
        int64_t t0_ms;
        int n;
        float mean_a, m2_a, mean_w[3], m2_w[3];  // Welford: |a| and the gyroscope (before its bias), device axes
    } steady;
    // rest and movement
    int64_t rest_since_ms, quiet_since_ms, last_motion_ms;
    bool still, moving, was_moved;  // was_moved: moved since the last rest (a put-down is due)
    float rest_g[3];               // gravity when it came to rest (the lift's tilt reference)
    int64_t lift_cond_since_ms, last_lift_ms;
    // orientation
    int orient;                    // 0 none, 1 flat, 2 upright: the last reported
    int orient_cand;
    int64_t orient_since_ms;
    // face down
    int down;                      // 0 up, 1 probed, 2 face down
    int64_t down_since_ms, up_since_ms;
    // shake
    struct {
        int64_t t_ms;
        float d[3];
    } peaks[YKM_SHAKE_PEAKS];
    int npeaks;
    bool in_peak;
    float peak_best, peak_dir[3];
    int64_t peak_t_ms;
    bool shaking;
    int64_t last_peak_ms;
    float rms_buf[YKM_RMS_LEN];
    int64_t rms_t[YKM_RMS_LEN];
    int rms_pos;
    // the holder
    ykm_holder_t hist[YKM_HISTORY];
    int hist_pos, hist_n;
    int64_t hist_last_ms;
    float holder_angle, holder_conf;
    bool holder_known;
} ykm_core_t;

void ykm_init(ykm_core_t *c, const yk_motion_thresholds_t *th, const yk_motion_cal_t *cal);
void ykm_set_thresholds(ykm_core_t *c, const yk_motion_thresholds_t *th);

/** One raw sample, sensor axes, in g and dps, at t_us. Writes up to `max` events; returns how many. */
int ykm_feed(ykm_core_t *c, const float acc[3], const float gyr[3], int64_t t_us, yk_motion_event_t *ev, int max);

void ykm_state(const ykm_core_t *c, yk_motion_state_t *out);

/** The holder's direction over the window_ms ending at end_ms (yk_motion_holder()). */
bool ykm_holder(ykm_core_t *c, int64_t end_ms, int window_ms, float *angle, float *conf);

/** Sensor axes to screen axes with the calibration's mapping (no bias). */
void ykm_map(const yk_motion_cal_t *cal, const float in[3], float out[3]);

/** CSV of raw samples, one per line: "t_us,ax,ay,az,gx,gy,gz" (g and dps, sensor axes). A header line or anything
 *  else that is not seven numbers returns false. */
bool ykm_csv_parse(const char *line, int64_t *t_us, float acc[3], float gyr[3]);
int ykm_csv_format(char *out, size_t size, int64_t t_us, const float acc[3], const float gyr[3]);
#define YKM_CSV_HEADER "t_us,ax,ay,az,gx,gy,gz"

const char *ykm_kind_name(yk_motion_kind_t kind);

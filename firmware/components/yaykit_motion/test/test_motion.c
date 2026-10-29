// Host tests for yaykit_motion's core: synthesised movements (the device turned and shaken in a simulation, read
// as the sensor would), CSV, and every recording in test/motion/ (README there: the file name says what it must
// and must not report). Run: bash test/run.sh
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yk_motion_core.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

#define PI_F 3.14159265f
#define RATE 224
static int rate_hz = RATE;  // samples a second in the simulation
static bool no_gyro;        // feed the accelerometer only (gyroscope all zeros), as Breathe can

// ---------------------------------------------------------------- the simulation

static ykm_core_t C;
static yk_motion_event_t EV[256];
static int nev;
static int64_t T;  // us
static FILE *csv;  // when set, every sample is also written here
static unsigned rng = 12345;

static float noise(float amp) {
    rng = rng * 1103515245u + 12345u;
    return amp * (((rng >> 8) & 0xFFFF) / 32768.0f - 1);
}

/** The device frame (x right, y up, z out of the glass) back to the sensor's, for the default calibration. */
static void to_sensor(const float d[3], float s[3]) {
    yk_motion_cal_t cal;
    yk_motion_default_cal(&cal);
    for (int i = 0; i < 3; i++) {
        const int a = cal.axis[i], k = (a < 0 ? -a : a) - 1;
        s[k] = a < 0 ? -d[i] : d[i];
    }
}

/** One sample: `up` = the world's up in the device frame (unit), `lin` = linear acceleration (g), `w` = rotation
 *  (dps), device frame. */
static void sample(const float up[3], const float lin[3], const float w[3]) {
    float f[3], fs[3], ws[3];
    for (int i = 0; i < 3; i++) f[i] = lin[i] + up[i] + noise(0.004f);
    float wn[3] = {w[0] + noise(0.3f), w[1] + noise(0.3f), w[2] + noise(0.3f)};
    if (no_gyro) wn[0] = wn[1] = wn[2] = 0;
    to_sensor(f, fs);
    to_sensor(wn, ws);
    if (csv) {
        char line[128];
        ykm_csv_format(line, sizeof(line), T, fs, ws);
        fprintf(csv, "%s\n", line);
    }
    yk_motion_event_t ev[8];
    const int n = ykm_feed(&C, fs, ws, T, ev, 8);
    for (int i = 0; i < n && nev < 256; i++) EV[nev++] = ev[i];
    T += 1000000 / rate_hz;
}

/** Tilted by theta about x (degrees; positive raises the top edge) and phi about y (positive raises the left
 *  edge... see rot_y). */
static void up_tilt_x(float theta_deg, float up[3]) {
    const float t = theta_deg * PI_F / 180;
    up[0] = 0, up[1] = sinf(t), up[2] = cosf(t);
}

static void rest(int ms, float theta_deg) {
    float up[3];
    up_tilt_x(theta_deg, up);
    const float z[3] = {0};
    for (int i = 0; i < ms * rate_hz / 1000; i++) sample(up, z, z);
}

/** Turns about x from a to b degrees in ms (ω_x = dθ/dt). */
static void turn_x(float a, float b, int ms) {
    const int n = ms * rate_hz / 1000;
    const float w[3] = {(b - a) * 1000.0f / (float)ms, 0, 0};
    const float z[3] = {0};
    for (int i = 0; i < n; i++) {
        float up[3];
        up_tilt_x(a + (b - a) * (float)i / (float)n, up);
        sample(up, z, w);
    }
}

/** Turns about y from a to b degrees (up = (sin φ, 0, cos φ) ... ω_y = -dφ/dt). */
static void turn_y(float a, float b, int ms) {
    const int n = ms * rate_hz / 1000;
    const float w[3] = {0, -(b - a) * 1000.0f / (float)ms, 0};
    const float z[3] = {0};
    for (int i = 0; i < n; i++) {
        const float p = (a + (b - a) * (float)i / (float)n) * PI_F / 180;
        const float up[3] = {sinf(p), 0, cosf(p)};
        sample(up, z, w);
    }
}

static void hold_y(float phi, int ms) {
    const float p = phi * PI_F / 180;
    const float up[3] = {sinf(p), 0, cosf(p)};
    const float z[3] = {0};
    for (int i = 0; i < ms * rate_hz / 1000; i++) sample(up, z, z);
}

/** Held at theta about x with a hand's tremor (peaks of tremor_g, and of 100 dps per g). */
static void hold(int ms, float theta_deg, float tremor_g) {
    float up[3];
    up_tilt_x(theta_deg, up);
    for (int i = 0; i < ms * rate_hz / 1000; i++) {
        const float lin[3] = {noise(tremor_g), noise(tremor_g), noise(tremor_g)};
        const float w[3] = {noise(100 * tremor_g), noise(100 * tremor_g), noise(100 * tremor_g)};
        sample(up, lin, w);
    }
}

/** Shaken along x at hz, amplitude g, held at theta. */
static void shake(int ms, float hz, float amp, float theta_deg) {
    float up[3];
    up_tilt_x(theta_deg, up);
    for (int i = 0; i < ms * rate_hz / 1000; i++) {
        const float s = sinf(2 * PI_F * hz * (float)i / rate_hz);
        const float lin[3] = {amp * s, 0.1f * s, 0};
        const float w[3] = {noise(20), noise(20), 40 * s};
        sample(up, lin, w);
    }
}

static void reset(void) {
    rate_hz = RATE;
    no_gyro = false;
    ykm_init(&C, NULL, NULL);
    nev = 0;
    T = 1000000;
}

static int count(yk_motion_kind_t k) {
    int n = 0;
    for (int i = 0; i < nev; i++) n += EV[i].kind == k;
    return n;
}

static const yk_motion_event_t *first(yk_motion_kind_t k) {
    for (int i = 0; i < nev; i++)
        if (EV[i].kind == k) return &EV[i];
    return NULL;
}

static float angle_diff(float a, float b) {
    float d = fmodf(fabsf(a - b), 360);
    return d > 180 ? 360 - d : d;
}

// ---------------------------------------------------------------- scenarios

static void test_flat_rest(void) {
    reset();
    rest(2000, 0);
    CHECK(count(YK_MOTION_FLAT) == 1);
    CHECK(first(YK_MOTION_FLAT) && first(YK_MOTION_FLAT)->t_ms >= 1500 && first(YK_MOTION_FLAT)->t_ms <= 1600);
    CHECK(count(YK_MOTION_LIFT) == 0 && count(YK_MOTION_SHAKE_START) == 0 && count(YK_MOTION_MOVING) == 0);
    yk_motion_state_t st;
    ykm_state(&C, &st);
    CHECK(st.flat && st.still && !st.moving && st.tilt_deg < 3);
    CHECK(fabsf(st.now.gravity[2] + 1) < 0.01f);  // pointing down, out of the back
}

static void test_lift_look_put_down(void) {
    // Picked up and looked at (the top edge raised 40 degrees: the screen leans towards someone at the bottom),
    // then laid down again.
    reset();
    rest(1000, 0);
    const int64_t t_lift = T / 1000;
    turn_x(0, 40, 400);
    hold(1500, 40, 0.10f);
    yk_motion_state_t st;
    ykm_state(&C, &st);
    CHECK(fabsf(st.tilt_deg - 40) < 3);
    CHECK(st.moving);
    turn_x(40, 0, 300);
    rest(1000, 0);
    CHECK(count(YK_MOTION_LIFT) == 1);
    CHECK(first(YK_MOTION_LIFT) && first(YK_MOTION_LIFT)->t_ms - t_lift <= 150);  // within the first moments
    CHECK(count(YK_MOTION_SHAKE_START) == 0);
    CHECK(count(YK_MOTION_PUT_DOWN) == 1);
    const yk_motion_event_t *pd = first(YK_MOTION_PUT_DOWN);
    CHECK(pd && angle_diff(pd->angle, 180) < 10 && pd->confidence > 0.5f);
    CHECK(count(YK_MOTION_MOVING) == 1 && count(YK_MOTION_STILL) == 1);
    // Picking it up again within 2 s is not a second pick-up; after that it is.
    nev = 0;
    turn_x(0, 30, 300);
    turn_x(30, 0, 300);
    rest(400, 0);
    CHECK(count(YK_MOTION_LIFT) == 0 || count(YK_MOTION_LIFT) == 1);
}

static void test_holder_direction(void) {
    // Tilted towards the right edge (the left raised): the holder is at 90 degrees.
    reset();
    rest(1000, 0);
    turn_y(0, -35, 300);
    hold_y(-35, 1000);
    float a, conf;
    CHECK(ykm_holder(&C, T / 1000, 500, &a, &conf));
    CHECK(angle_diff(a, 90) < 8 && conf > 0.8f);
    // Level for a long time: the last direction, less sure.
    turn_y(-35, 0, 300);
    rest(1500, 0);
    float a2, conf2;
    CHECK(ykm_holder(&C, T / 1000, 500, &a2, &conf2));
    CHECK(angle_diff(a2, a) < 1 && conf2 < conf);
    // Never tilted: unknown.
    reset();
    rest(1500, 0);
    CHECK(!ykm_holder(&C, T / 1000, 500, &a, &conf));
}

static void test_shake(void) {
    reset();
    rest(800, 0);
    turn_x(0, 30, 300);
    const int64_t t0 = T / 1000;
    shake(2000, 5, 1.6f, 30);
    const int64_t t_end = T / 1000;
    hold(800, 30, 0.02f);
    CHECK(count(YK_MOTION_SHAKE_START) == 1);
    const yk_motion_event_t *s = first(YK_MOTION_SHAKE_START);
    CHECK(s && s->t_ms - t0 <= 300);
    CHECK(s && s->strength > 0.1f && s->strength <= 1);
    const yk_motion_event_t *e = first(YK_MOTION_SHAKE_END);
    CHECK(e && e->t_ms > t_end && e->t_ms - t_end <= 600);
    // A harder shake is stronger.
    reset();
    rest(800, 0);
    shake(1000, 5, 1.0f, 0);
    yk_motion_state_t soft;
    ykm_state(&C, &soft);
    reset();
    rest(800, 0);
    shake(1000, 5, 3.0f, 0);
    yk_motion_state_t hard;
    ykm_state(&C, &hard);
    CHECK(soft.shaking && hard.shaking && hard.shake_strength > soft.shake_strength + 0.2f);
    // After a shake, coming to rest is a put-down (the dice settle).
    rest(1000, 0);
    CHECK(count(YK_MOTION_PUT_DOWN) >= 1);
}

static void test_no_false_shake(void) {
    // A knock on the table: one sharp push and fast ringing.
    reset();
    rest(1000, 0);
    float up[3];
    up_tilt_x(0, up);
    for (int i = 0; i < 12; i++) {
        const float r = i < 2 ? 1.5f : (i % 2 ? -1.0f : 1.0f) * expf(-(float)i / 6);
        const float lin[3] = {0, 0, r};
        const float w[3] = {0, 0, 0};
        sample(up, lin, w);
    }
    rest(1000, 0);
    CHECK(count(YK_MOTION_SHAKE_START) == 0);
    CHECK(count(YK_MOTION_LIFT) == 0);
    // Handed to someone: one firm move one way, a stop, held out.
    reset();
    rest(800, 0);
    turn_x(0, 30, 300);
    up_tilt_x(30, up);
    for (int i = 0; i < RATE / 2; i++) {
        const float s = sinf(2 * PI_F * (float)i / (RATE / 2));
        const float lin[3] = {0, 0.5f * s, 0.2f * s};
        const float w[3] = {10 * s, 0, 0};
        sample(up, lin, w);
    }
    hold(1000, 30, 0.04f);
    CHECK(count(YK_MOTION_SHAKE_START) == 0);
    // Walking with it in hand: steps at 2 Hz, 0.3 g.
    reset();
    rest(500, 0);
    turn_x(0, 35, 300);
    for (int i = 0; i < RATE * 3; i++) {
        const float s = sinf(2 * PI_F * 2 * (float)i / rate_hz);
        const float lin[3] = {0.05f * s, 0.3f * s, 0.1f * s};
        const float w[3] = {8 * s, 4 * s, 0};
        sample(up, lin, w);
    }
    CHECK(count(YK_MOTION_SHAKE_START) == 0);
}

static void test_face_down(void) {
    reset();
    rest(1000, 0);
    const int64_t t0 = T / 1000;
    turn_y(0, 180, 500);
    hold_y(180, 1500);
    const yk_motion_event_t *probe = first(YK_MOTION_FACE_DOWN_PROBE), *down = first(YK_MOTION_FACE_DOWN);
    CHECK(probe && down && down->t_ms - probe->t_ms >= 300 && down->t_ms - probe->t_ms <= 400);
    // Glass down (up < -0.75 g, past 139 degrees) at t0 + 385 ms: the probe 250 ms later; quiet within 1 s of
    // lying there.
    if (probe) printf("  face down: probe +%lld ms, quiet +%lld ms from the start of the turn\n",
                      (long long)(probe->t_ms - t0), down ? (long long)(down->t_ms - t0) : -1LL);
    CHECK(probe && probe->t_ms - t0 >= 620 && probe->t_ms - t0 <= 680);
    CHECK(down && down->t_ms - (t0 + 500) <= 1000);
    CHECK(count(YK_MOTION_FACE_UP) == 0);
    yk_motion_state_t st;
    ykm_state(&C, &st);
    CHECK(st.face_down && st.tilt_deg > 170);
    const int64_t t_back = T / 1000;
    turn_y(180, 0, 500);
    rest(800, 0);
    const yk_motion_event_t *up = first(YK_MOTION_FACE_UP);
    CHECK(up && up->t_ms - t_back >= 400 && up->t_ms - t_back <= 900);
    // A probe that is turned back at once: no FACE_DOWN.
    reset();
    rest(1000, 0);
    turn_y(0, 180, 300);
    hold_y(180, 350);
    turn_y(180, 0, 300);
    rest(800, 0);
    CHECK(count(YK_MOTION_FACE_DOWN_PROBE) == 1 && count(YK_MOTION_FACE_DOWN) == 0 && count(YK_MOTION_FACE_UP) == 1);
    // Breathe's thresholds can be changed: 1 s instead of 600 ms.
    yk_motion_thresholds_t th;
    yk_motion_default_thresholds(&th);
    th.down_ms = 1000;
    reset();
    ykm_set_thresholds(&C, &th);
    rest(1000, 0);
    turn_y(0, 180, 300);
    hold_y(180, 800);
    CHECK(count(YK_MOTION_FACE_DOWN_PROBE) == 1 && count(YK_MOTION_FACE_DOWN) == 0);
    hold_y(180, 500);
    CHECK(count(YK_MOTION_FACE_DOWN) == 1);
}

static void test_face_down_without_gyro(void) {
    // Breathe's case: 25 Hz, accelerometer only, turned over in 0.4 s. Quiet about 600 ms after lying there, not
    // 1.1 s as with the gravity estimate alone.
    reset();
    rate_hz = 25;
    no_gyro = true;
    rest(1000, 0);
    turn_y(0, 180, 400);
    const int64_t t_end = T / 1000;
    hold_y(180, 1500);
    const yk_motion_event_t *probe = first(YK_MOTION_FACE_DOWN_PROBE), *down = first(YK_MOTION_FACE_DOWN);
    if (down) printf("  face down without gyro: probe %+lld ms, quiet %+lld ms from the end of the turn\n",
                     probe ? (long long)(probe->t_ms - t_end) : -1LL, (long long)(down->t_ms - t_end));
    CHECK(probe && down && down->t_ms - t_end <= 750);
    turn_y(180, 0, 400);
    rest(1000, 0);
    CHECK(count(YK_MOTION_FACE_UP) == 1);
    // A flip in no time at all (the reading jumps from glass up to glass down): seen as face down, and the gravity
    // estimate catches up instead of staying where it was.
    reset();
    rate_hz = 25;
    no_gyro = true;
    rest(1000, 0);
    const int64_t t_flip = T / 1000;
    hold_y(180, 1500);
    down = first(YK_MOTION_FACE_DOWN);
    CHECK(down && down->t_ms - t_flip <= 800);
    yk_motion_state_t st;
    ykm_state(&C, &st);
    CHECK(st.tilt_deg > 170 && st.face_down);
    // The same jump with the gyroscope on but reporting nothing of it: the estimate re-seeds after 250 ms.
    reset();
    rest(1000, 0);
    const int64_t t_jump = T / 1000;
    hold_y(180, 1500);
    down = first(YK_MOTION_FACE_DOWN);
    CHECK(down && down->t_ms - t_jump <= 1000);
    ykm_state(&C, &st);
    CHECK(st.tilt_deg > 170);
    // Shaking near 1 g does not re-seed the estimate wrongly: still flat after a hard shake.
    reset();
    rest(800, 0);
    shake(1500, 5, 2.5f, 0);
    rest(800, 0);
    ykm_state(&C, &st);
    CHECK(st.tilt_deg < 5 && count(YK_MOTION_FACE_DOWN_PROBE) == 0);
}

static void test_upright(void) {
    reset();
    rest(1000, 0);
    turn_x(0, 90, 400);
    hold(800, 90, 0.01f);
    const yk_motion_event_t *u = first(YK_MOTION_UPRIGHT);
    CHECK(u && angle_diff(u->angle, 0) < 5);  // standing the right way up
    yk_motion_state_t st;
    ykm_state(&C, &st);
    CHECK(st.upright && !st.flat && fabsf(st.tilt_deg - 90) < 3);
    // Upside down (the top edge at the bottom): 180.
    reset();
    rest(1000, 0);
    turn_x(0, -90, 400);
    hold(800, -90, 0.01f);
    u = first(YK_MOTION_UPRIGHT);
    CHECK(u && angle_diff(u->angle, 180) < 5);
    // On its right edge (the left edge up): up is 270.
    reset();
    rest(1000, 0);
    turn_y(0, -90, 400);
    hold_y(-90, 800);
    u = first(YK_MOTION_UPRIGHT);
    CHECK(u && angle_diff(u->angle, 270) < 5);
    // Back flat: FLAT again.
    nev = 0;
    turn_y(-90, 0, 400);
    rest(800, 0);
    CHECK(count(YK_MOTION_FLAT) == 1);
}

static void test_stream_axes(void) {
    // Screen coordinates: tilted with the top up, gravity points to the bottom of the screen (+y) and out of the back.
    reset();
    rest(300, 0);
    turn_x(0, 30, 200);
    rest(600, 30);
    yk_motion_state_t st;
    ykm_state(&C, &st);
    CHECK(st.now.gravity[1] > 0.45f && st.now.gravity[2] < -0.8f && fabsf(st.now.gravity[0]) < 0.05f);
    CHECK(fabsf(st.now.lin[0]) < 0.05f && fabsf(st.now.lin[1]) < 0.05f && fabsf(st.now.lin[2]) < 0.05f);
    // A push to the right of the screen is +x.
    float up[3];
    up_tilt_x(30, up);
    const float lin[3] = {0.5f, 0, 0}, w[3] = {0};
    for (int i = 0; i < 10; i++) sample(up, lin, w);
    ykm_state(&C, &st);
    CHECK(st.now.lin[0] > 0.4f && fabsf(st.now.lin[1]) < 0.1f);
    // Turning clockwise as seen on the screen is gyro z > 0 (the device frame's z turns counter-clockwise).
    const float spin[3] = {0, 0, -90};
    sample(up, (const float[3]){0}, spin);
    ykm_state(&C, &st);
    CHECK(st.now.gyro[2] > 80);
}

static void test_csv(void) {
    char line[128];
    const float a[3] = {0.0123f, -0.9876f, 1.25f}, g[3] = {1.5f, -250.25f, 0};
    ykm_csv_format(line, sizeof(line), 1234567, a, g);
    CHECK(!strcmp(line, "1234567,0.0123,-0.9876,1.2500,1.50,-250.25,0.00"));
    int64_t t;
    float a2[3], g2[3];
    CHECK(ykm_csv_parse(line, &t, a2, g2));
    CHECK(t == 1234567 && fabsf(a2[1] + 0.9876f) < 1e-4f && fabsf(g2[1] + 250.25f) < 1e-3f);
    CHECK(!ykm_csv_parse(YKM_CSV_HEADER, &t, a2, g2));
    CHECK(!ykm_csv_parse("1,2,3", &t, a2, g2));
    CHECK(!ykm_csv_parse("", &t, a2, g2));
    CHECK(!strcmp(ykm_kind_name(YK_MOTION_SHAKE_START), "shake_start") && !strcmp(ykm_kind_name(99), "?"));
}

/** What a replay saw of rest: when it first counted as still (ms from the first sample, -1 never), how often it
 *  stopped being still after that, and the same for the rule before the device's own rest was learned (|a| within
 *  still_g of 1 g, the gyroscope's reading without its bias, still_ms of both): `old_still_ms` how long that rule
 *  called it still, `old_drops` how often it stopped. */
static struct {
    int64_t first_still_ms;
    int drops;
    int64_t back_ms;      // still again after the last drop
    int64_t max_run_ms;   // the longest stretch of still that ended before the recording did
    int64_t final_ms;     // when the stretch of still the recording ends in began (-1: it ends not still)
    int64_t settled_ms;   // when the device's own rest was first learned (-1 never)
    int64_t old_still_ms;
    int old_drops;
} RP;

/** Replays one CSV; returns the number of samples. */
static int replay(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    reset();
    memset(&RP, 0, sizeof(RP));
    RP.first_still_ms = RP.settled_ms = RP.final_ms = -1;
    char line[256];
    int n = 0;
    int64_t t0 = -1, old_since = -1, old_prev = -1, run_since = -1;
    bool was_still = false, old_still = false;
    while (fgets(line, sizeof(line), f)) {
        int64_t t;
        float a[3], g[3];
        if (!ykm_csv_parse(line, &t, a, g)) continue;
        if (t0 < 0) t0 = t;
        yk_motion_event_t ev[8];
        const int k = ykm_feed(&C, a, g, t, ev, 8);
        for (int i = 0; i < k && nev < 256; i++) EV[nev++] = ev[i];
        n++;
        const int64_t ms = (t - t0) / 1000;
        if (C.still && RP.first_still_ms < 0) RP.first_still_ms = ms;
        if (C.settled && RP.settled_ms < 0) RP.settled_ms = ms;
        if (was_still && !C.still) {
            RP.drops++;
            if (ms - run_since > RP.max_run_ms) RP.max_run_ms = ms - run_since;
        }
        if (!was_still && C.still) {
            run_since = ms;
            if (RP.drops) RP.back_ms = ms;
        }
        was_still = C.still;
        const float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), wn = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        const bool old_rest = fabsf(an - 1) < C.th.still_g && wn < C.th.still_dps;
        if (!old_rest) old_since = -1;
        else if (old_since < 0) old_since = ms;
        const bool now_still = old_since >= 0 && ms - old_since >= C.th.still_ms;
        if (old_still && !now_still) RP.old_drops++;
        if (now_still && old_prev >= 0) RP.old_still_ms += ms - old_prev;
        old_still = now_still;
        old_prev = ms;
    }
    fclose(f);
    RP.final_ms = was_still ? run_since : -1;
    return n;
}


static void test_csv_replay_matches_live(void) {
    // A shake written as CSV and read back gives the same events as live.
    const char *path = getenv("YKM_TMP");
    if (!path) return;
    reset();
    csv = fopen(path, "w");
    CHECK(csv != NULL);
    if (!csv) return;
    fprintf(csv, "%s\n", YKM_CSV_HEADER);
    rest(600, 0);
    shake(1200, 5, 1.8f, 0);
    rest(800, 0);
    fclose(csv);
    csv = NULL;
    const int live_n = nev;
    yk_motion_kind_t live[64];
    for (int i = 0; i < live_n && i < 64; i++) live[i] = EV[i].kind;
    CHECK(replay(path) > 500);
    CHECK(nev == live_n);
    for (int i = 0; i < nev && i < 64; i++) CHECK(EV[i].kind == live[i]);
    CHECK(count(YK_MOTION_SHAKE_START) == 1);
}

// ---------------------------------------------------------------- rest against the device's own reading

/** Rest that reads off 1 g (0.955 g) with a gyroscope bias past still_dps (9 dps on one axis): the old rule never
 *  called it still. Now: still once a steady second has shown both (the first second, then still_ms), and still for
 *  good. Starting up held and then laid down learns nothing while held. */
static void test_rest_off_1g(void) {
    reset();
    const float up[3] = {0, 0, 0.955f}, z[3] = {0}, bias[3] = {9, 1, -2};
    int64_t first = -1;
    int drops = 0;
    bool was = false;
    for (int i = 0; i < 10 * RATE; i++) {
        sample(up, z, bias);
        if (C.still && first < 0) first = T / 1000;
        if (was && !C.still) drops++;
        was = C.still;
    }
    CHECK(first >= 0 && first - 1000 <= 1400 && drops == 0);
    CHECK(C.settled && fabsf(C.rest_g_len - 0.955f) < 0.002f);
    CHECK(fabsf(C.cal.gyro_bias[0] - 9) < 0.2f);  // device axes: x as the device's
    // Before the first steady second, 1 g a little wider: 0.963 g counts at once (still_ms), 0.90 g does not.
    reset();
    const float up1[3] = {0, 0, 0.963f};
    for (int i = 0; i < RATE / 2; i++) sample(up1, z, z);
    CHECK(C.still && !C.settled);
    reset();
    const float up2[3] = {0, 0, 0.90f};
    for (int i = 0; i < RATE / 2; i++) sample(up2, z, z);
    CHECK(!C.still);
    for (int i = 0; i < 2 * RATE; i++) sample(up2, z, z);
    CHECK(C.still && C.settled);  // its own rest once learned
}

/** Recordings in test/motion: <what>-<n>.csv. The README lists what each kind must and must not report. */
static void test_recordings(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    int files = 0;
    while ((e = readdir(d))) {
        const size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".csv")) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        const int n = replay(path);
        files++;
        const char *name = e->d_name;
        int ok = n > 0;
#define PREFIX(p) (!strncmp(name, p, strlen(p)))
        if (PREFIX("shake-light") || PREFIX("shake-hard") || PREFIX("shake-walking")) ok &= count(YK_MOTION_SHAKE_START) >= 1;
        if (PREFIX("look") || PREFIX("handoff") || PREFIX("knock") || PREFIX("flat") || PREFIX("put-down"))
            ok &= count(YK_MOTION_SHAKE_START) == 0;
        if (PREFIX("look") || PREFIX("handoff")) ok &= count(YK_MOTION_LIFT) >= 1;
        if (PREFIX("knock") || PREFIX("flat")) ok &= count(YK_MOTION_LIFT) == 0;
        if (PREFIX("knock")) {
            // Knocks on the table next to it at rest: still throughout, its own rest learned all the same.
            ok &= RP.first_still_ms >= 0 && RP.first_still_ms <= 1500 && RP.drops == 0 && C.settled;
        }
        if (PREFIX("held")) {
            // In a hand (the recording may end laid down): still only in short stretches while held (Party Play
            // sleeps after a minute of it), and nothing of the device's rest learned from the hand: only once it
            // lies still at the end, if it does.
            ok &= RP.max_run_ms < 3000;
            ok &= RP.settled_ms < 0 || (RP.final_ms >= 0 && RP.settled_ms >= RP.final_ms);
        }
        if (PREFIX("held-then-put-down")) ok &= RP.final_ms >= 0 && C.settled;
        if (PREFIX("face-down")) ok &= count(YK_MOTION_FACE_DOWN) >= 1;
        if (PREFIX("upright")) ok &= count(YK_MOTION_UPRIGHT) >= 1;
        if (PREFIX("pass-around")) {
            // "moving" never stops for more than a second while it goes round the table
            ok &= count(YK_MOTION_STILL) <= 1;
        }
        if (PREFIX("rest-") || PREFIX("boot-")) {
            // Left alone (from its start, for boot-): still within 1.5 s, and still to the end; the device's own rest
            // learned. Neither a pick-up nor a shake.
            ok &= RP.first_still_ms >= 0 && RP.first_still_ms <= 1500 && RP.drops == 0 && C.settled;
            ok &= count(YK_MOTION_LIFT) == 0 && count(YK_MOTION_SHAKE_START) == 0;
        }
        if (PREFIX("rest-")) {
            // Recorded where the old rule (against 1 g, the gyroscope's reading as it is) called it held: it stopped
            // being still at least once, or never was.
            ok &= RP.old_drops > 0 || RP.old_still_ms == 0;
        }
#undef PREFIX
        printf("  %s: %d samples, %d events; still from %lld ms, %d drops, longest stretch before the end %lld ms, "
               "final stretch from %lld ms (old rule: still %lld ms, %d drops); rest learned at %lld ms: %.4f g, "
               "gyroscope bias %.2f %.2f %.2f dps%s\n",
               name, n, nev, (long long)RP.first_still_ms, RP.drops, (long long)RP.max_run_ms, (long long)RP.final_ms,
               (long long)RP.old_still_ms, RP.old_drops, (long long)RP.settled_ms, C.rest_g_len, C.cal.gyro_bias[0],
               C.cal.gyro_bias[1], C.cal.gyro_bias[2], ok ? "" : "  <- FAIL");
        CHECK(ok);
    }
    closedir(d);
    printf("  %d recordings\n", files);
}

int main(int argc, char **argv) {
    test_flat_rest();
    test_lift_look_put_down();
    test_holder_direction();
    test_shake();
    test_no_false_shake();
    test_face_down();
    test_face_down_without_gyro();
    test_upright();
    test_stream_axes();
    test_csv();
    test_csv_replay_matches_live();
    test_rest_off_1g();
    if (argc > 1) test_recordings(argv[1]);
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}

// Host tests for yk_speak_dsp: the playback ring, fades, level, cue tones, volume.
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI  // not in strict C11 (glibc)
#define M_PI 3.14159265358979323846
#endif

#include "yk_speak_dsp.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

static void test_ring(void) {
    int16_t buf[8], out[8];
    yk_ring_t r;
    yk_ring_init(&r, buf, 8);
    CHECK(yk_ring_avail(&r) == 0 && yk_ring_space(&r) == 8);
    const int16_t a[] = {1, 2, 3, 4, 5, 6};
    CHECK(yk_ring_write(&r, a, 6) == 6);
    CHECK(yk_ring_read(&r, out, 4) == 4 && out[0] == 1 && out[3] == 4);
    // Wraps around the end of the buffer, keeps order.
    const int16_t b[] = {7, 8, 9, 10, 11, 12};
    CHECK(yk_ring_write(&r, b, 6) == 6 && yk_ring_avail(&r) == 8);
    CHECK(yk_ring_write(&r, b, 1) == 0);  // full: nothing written
    CHECK(yk_ring_read(&r, out, 8) == 8 && out[0] == 5 && out[1] == 6 && out[2] == 7 && out[7] == 12);
    CHECK(yk_ring_read(&r, out, 1) == 0);  // empty
    // Partial writes and reads.
    yk_ring_write(&r, a, 6);
    CHECK(yk_ring_write(&r, b, 6) == 2);
    CHECK(yk_ring_read(&r, out, 20) == 8 && out[6] == 7 && out[7] == 8);
    yk_ring_write(&r, a, 3);
    yk_ring_clear(&r);
    CHECK(yk_ring_avail(&r) == 0 && yk_ring_space(&r) == 8);
    // The free-running counts can pass SIZE_MAX without trouble.
    r.head = r.tail = (size_t)-3;
    CHECK(yk_ring_write(&r, a, 6) == 6 && yk_ring_avail(&r) == 6);
    CHECK(yk_ring_read(&r, out, 6) == 6 && out[0] == 1 && out[5] == 6);
}

static void test_fade(void) {
    int16_t x[100];
    for (int i = 0; i < 100; i++) x[i] = 10000;
    yk_fade(x, 100, 0, 1);
    CHECK(x[0] == 0 && x[50] == 5000 && x[99] > 9800 && x[99] < 10000);
    for (int i = 0; i < 100; i++) x[i] = -10000;
    yk_fade(x, 100, 1, 0);
    CHECK(x[0] == -10000 && x[99] > -200 && x[99] <= 0);
    yk_fade(x, 0, 0, 1);  // nothing to do
}

static void test_level(void) {
    int16_t x[320];
    memset(x, 0, sizeof(x));
    CHECK(yk_level16(x, 320) == 0);
    CHECK(yk_level16(x, 0) == 0);
    for (int i = 0; i < 320; i++) x[i] = (int16_t)(32767 * 0.5 * sin(i * 0.3));  // about -9 dBFS RMS
    const uint8_t loud = yk_level16(x, 320);
    CHECK(loud > 90 && loud < 100);
    for (int i = 0; i < 320; i++) x[i] = (int16_t)(32767 * 0.005 * sin(i * 0.3));  // about -49 dBFS
    const uint8_t quiet = yk_level16(x, 320);
    CHECK(quiet > 10 && quiet < 30);
    for (int i = 0; i < 320; i++) x[i] = i % 2 ? 32767 : -32768;
    CHECK(yk_level16(x, 320) == 100);
}

/** Strongest frequency in x by a coarse DFT scan, Hz. */
static float peak_hz(const int16_t *x, size_t n, int rate) {
    float best = 0, best_hz = 0;
    for (float hz = 400; hz < 1200; hz += 5) {
        double re = 0, im = 0;
        for (size_t i = 0; i < n; i++) re += x[i] * cos(2 * M_PI * hz * i / rate), im += x[i] * sin(2 * M_PI * hz * i / rate);
        const float p = (float)(re * re + im * im);
        if (p > best) best = p, best_hz = hz;
    }
    return best_hz;
}

static void test_chime(void) {
    static int16_t x[4000];
    const size_t n = yk_chime_synth(YK_CHIME_START, 16000, x, 4000);
    CHECK(n == 2240);  // two 70 ms notes
    int16_t peak = 0;
    for (size_t i = 0; i < n; i++) peak = (int16_t)(x[i] > peak ? x[i] : -x[i] > peak ? -x[i] : peak);
    CHECK(peak > 4000 && peak <= 8192);  // quiet: -12 dBFS at most (about -15 after the attack)
    CHECK(x[0] == 0);                    // starts from silence (5 ms attack)
    CHECK(fabsf(peak_hz(x, 1120, 16000) - 659) < 10 && fabsf(peak_hz(x + 1120, 1120, 16000) - 880) < 10);  // rising
    yk_chime_synth(YK_CHIME_END, 16000, x, 4000);
    CHECK(fabsf(peak_hz(x, 1120, 16000) - 880) < 10 && fabsf(peak_hz(x + 1120, 1120, 16000) - 659) < 10);  // falling
    CHECK(yk_chime_synth(YK_CHIME_START, 16000, x, 1000) == 1000);  // cut to fit
    // Each note decays to near silence before the next starts: no click between them.
    yk_chime_synth(YK_CHIME_START, 16000, x, 4000);
    int16_t tail = 0;
    for (size_t i = 1100; i < 1120; i++) tail = (int16_t)(x[i] > tail ? x[i] : -x[i] > tail ? -x[i] : tail);
    CHECK(tail < 200);
}

static void test_volume(void) {
    CHECK(yk_volume_to_codec(0) == 0);
    CHECK(yk_volume_to_codec(-3) == 0);
    CHECK(yk_volume_to_codec(1) == 63);
    CHECK(yk_volume_to_codec(10) == 92);
    CHECK(yk_volume_to_codec(14) == 92);
    for (int v = 1; v < 10; v++) CHECK(yk_volume_to_codec(v + 1) > yk_volume_to_codec(v));
}

static void test_reclaim(void) {
    CHECK(yk_speak_may_reclaim(false, false, false));  // a tap's microphones closed, the speaker idle
    CHECK(!yk_speak_may_reclaim(false, false, true));  // the microphones open again (a double click, a quick hold)
    CHECK(!yk_speak_may_reclaim(true, false, false));  // the codec open: its format is the speaker's already
    CHECK(!yk_speak_may_reclaim(false, true, false));  // something to play: its own open does it
    CHECK(!yk_speak_may_reclaim(true, true, true));
}

int main(void) {
    test_ring();
    test_fade();
    test_level();
    test_chime();
    test_volume();
    test_reclaim();
    printf("yaykit_speak host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

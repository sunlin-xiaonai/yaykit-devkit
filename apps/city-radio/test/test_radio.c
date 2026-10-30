// Host tests for City Radio's plain C parts: the station list and the PCM conversion. Run: bash test/run.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/pcm_conv.h"
#include "../main/stations.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                  \
    } while (0)

static void test_stations(void) {
    const int n = stations_count();
    CHECK(n >= 2);
    CHECK(station_step(n - 1, 1) == 0);
    CHECK(station_step(0, -1) == n - 1);
    CHECK(station_at(n) == station_at(0));
    CHECK(station_at(-1) == station_at(n - 1));
    for (int i = 0; i < n; i++) {
        const station_t *s = station_at(i);
        CHECK(s->city && s->name && !strncmp(s->url, "http://", 7));
        CHECK(s->mhz10 >= FM_MIN_10 && s->mhz10 <= FM_MAX_10);
        if (i) CHECK(s->mhz10 > station_at(i - 1)->mhz10);  // low to high: next = to the right on the dial
    }
    char text[96];
    const station_t st = {"北京", "北京交通广播", 1039, "http://x"};
    station_freq_text(&st, text, sizeof(text));
    CHECK(!strcmp(text, "103.9"));
    CHECK(dial_permille(FM_MIN_10) == 0 && dial_permille(FM_MAX_10) == 1000);
    CHECK(dial_permille(800) == 0 && dial_permille(1200) == 1000);
    CHECK(dial_permille((FM_MIN_10 + FM_MAX_10) / 2) >= 495 && dial_permille((FM_MIN_10 + FM_MAX_10) / 2) <= 500);
    listen_sentence("北京交通广播", 25 * 60 + 10, text, sizeof(text));
    CHECK(!strcmp(text, "听了北京交通广播 25 分钟"));
    listen_sentence("北京交通广播", 89, text, sizeof(text));
    CHECK(!strcmp(text, "听了北京交通广播 1 分钟"));
    listen_sentence("北京交通广播", 20, text, sizeof(text));
    CHECK(!strcmp(text, "听了一会儿北京交通广播"));
}

/** Converts `frames` stereo frames of a ramp in blocks of `block`; returns the output count. */
static size_t convert_ramp(uint32_t in_rate, size_t frames, size_t block, int16_t *out, size_t cap) {
    pcm_conv_t c;
    pcm_conv_init(&c, in_rate, 24000);
    int16_t *in = malloc(frames * 2 * sizeof(int16_t));
    for (size_t i = 0; i < frames; i++) in[2 * i] = in[2 * i + 1] = (int16_t)(i % 20000);
    size_t n = 0;
    for (size_t at = 0; at < frames; at += block) {
        const size_t len = frames - at < block ? frames - at : block;
        n += pcm_conv_run(&c, in + 2 * at, len, 2, out + n, cap - n);
    }
    free(in);
    return n;
}

static void test_pcm(void) {
    enum { CAP = 30000 };
    static int16_t whole[CAP], blocks[CAP];
    // 48 kHz -> 24 kHz: exactly half, every other input sample (one input sample late: the first output is the
    // primed sample).
    size_t n = convert_ramp(48000, 48000, 48000, whole, CAP);
    CHECK(n == 24000);
    CHECK(whole[0] == 0 && whole[1] == 1 && whole[100] == 199);
    // 44.1 kHz -> 24 kHz: a second in is a second out (within a sample).
    n = convert_ramp(44100, 44100, 44100, whole, CAP);
    CHECK(n >= 23999 && n <= 24001);
    // Block by block (an MP3 frame is 1152) gives the same samples as all at once.
    const size_t m = convert_ramp(44100, 44100, 1152, blocks, CAP);
    CHECK(m == n && !memcmp(whole, blocks, n * sizeof(int16_t)));
    // Stereo is averaged to mono; a constant stays constant.
    pcm_conv_t c;
    pcm_conv_init(&c, 44100, 24000);
    int16_t lr[2 * 441], out[300];
    for (int i = 0; i < 441; i++) lr[2 * i] = 1000, lr[2 * i + 1] = -200;
    n = pcm_conv_run(&c, lr, 441, 2, out, 300);
    CHECK(n >= 239 && n <= 241);
    int same = 1;
    for (size_t i = 0; i < n; i++) same &= out[i] == 400;
    CHECK(same);
    CHECK(pcm_conv_max_out(&c, 1152) >= 1152 * 24000 / 44100 + 1);
    // A full output buffer drops the rest of the block but stays in step for the next one.
    pcm_conv_init(&c, 48000, 24000);
    n = pcm_conv_run(&c, lr, 441, 2, out, 10);
    CHECK(n == 10);
    n = pcm_conv_run(&c, lr, 441, 2, out, 300);
    CHECK(n >= 220 && n <= 221);
}

int main(void) {
    test_stations();
    test_pcm();
    printf("city-radio host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

// Host tests for yk_ptt, the push-to-talk recording's buffer. Run: bash test/run.sh
#include <stdio.h>
#include <string.h>

#include "../yk_ptt.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int main(void) {
    static uint8_t buf[44 + 2 * 1000];
    CHECK(yk_ptt_bytes(16000, 10) == 44 + 320000);
    yk_ptt_t r;
    yk_ptt_init(&r, buf, sizeof(buf), 16000);
    CHECK(r.cap == 1000);
    CHECK(yk_ptt_finish(&r) == 0);  // nothing recorded
    int16_t frame[256];
    for (int i = 0; i < 256; i++) frame[i] = (int16_t)(i * 100 - 12800);
    CHECK(yk_ptt_add(&r, frame, 256, false));
    CHECK(yk_ptt_add(&r, frame, 256, true));
    CHECK(yk_ptt_add(&r, frame, 256, true));
    CHECK(!yk_ptt_add(&r, frame, 256, false));  // full at 1000: the rest is dropped
    CHECK(r.n == 1000 && r.speech_frames == 2);
    const size_t len = yk_ptt_finish(&r);
    CHECK(len == 44 + 2000);
    CHECK(!memcmp(buf, "RIFF", 4) && get32(buf + 4) == 36 + 2000 && !memcmp(buf + 8, "WAVEfmt ", 8));
    CHECK(get32(buf + 24) == 16000 && get32(buf + 28) == 32000 && buf[22] == 1 && buf[34] == 16);
    CHECK(!memcmp(buf + 36, "data", 4) && get32(buf + 40) == 2000);
    int16_t s;
    memcpy(&s, buf + 44 + 2 * 3, 2);
    CHECK(s == frame[3]);
    memcpy(&s, buf + 44 + 2 * (768 + 231), 2);  // the last sample kept is frame[231] of the fourth frame
    CHECK(s == frame[231]);
    // Reset starts over.
    yk_ptt_reset(&r);
    CHECK(r.n == 0 && r.speech_frames == 0 && yk_ptt_finish(&r) == 0);
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}

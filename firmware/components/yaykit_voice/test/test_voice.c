// Host tests for yk_wav.c. Run: bash firmware/components/yaykit_voice/test/run.sh
#include <stdio.h>
#include <string.h>

#include "../yk_wav.h"

static int failures, checks;
#define CHECK(cond)                                                \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            failures++;                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);  \
        }                                                          \
    } while (0)

static unsigned u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned u16(const uint8_t *p) { return p[0] | p[1] << 8; }

int main(void) {
    // 1.5 s at 24 kHz, the device's codec rate: what services/device/src/audio.ts reads back.
    uint8_t h[YK_WAV_HEADER_BYTES];
    yk_wav_header(h, 24000, 72000);
    CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
    CHECK(u32(h + 4) == 36 + 72000);
    CHECK(u32(h + 16) == 16 && u16(h + 20) == 1 && u16(h + 22) == 1);
    CHECK(u32(h + 24) == 24000 && u32(h + 28) == 48000 && u16(h + 32) == 2 && u16(h + 34) == 16);
    CHECK(u32(h + 40) == 72000);

    int16_t quiet[4] = {0, 10, -10, 0}, loud[3] = {0, -32768, 100};
    CHECK(yk_wav_level(quiet, 4) == 0);
    CHECK(yk_wav_level(loud, 3) == 100);
    int16_t speech[2] = {4096, -100};
    CHECK(yk_wav_level(speech, 2) == 50);
    printf("yaykit_voice host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

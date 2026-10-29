#include "yk_wav.h"

#include <string.h>

static void le32(uint8_t *p, uint32_t v) {
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
    p[2] = (v >> 16) & 255;
    p[3] = (v >> 24) & 255;
}

static void le16(uint8_t *p, uint16_t v) {
    p[0] = v & 255;
    p[1] = (v >> 8) & 255;
}

void yk_wav_header(uint8_t out[YK_WAV_HEADER_BYTES], uint32_t sample_rate, uint32_t data_bytes) {
    memcpy(out, "RIFF", 4);
    le32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    le32(out + 16, 16);              // fmt chunk size
    le16(out + 20, 1);               // PCM
    le16(out + 22, 1);               // mono
    le32(out + 24, sample_rate);
    le32(out + 28, sample_rate * 2); // byte rate
    le16(out + 32, 2);               // block align
    le16(out + 34, 16);              // bits per sample
    memcpy(out + 36, "data", 4);
    le32(out + 40, data_bytes);
}

uint8_t yk_wav_level(const int16_t *samples, size_t count) {
    int peak = 0;
    for (size_t i = 0; i < count; i++) {
        int v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > peak) peak = v;
    }
    // Speech near the device peaks around a quarter of full scale at the recorder's gain.
    int level = peak * 100 / 8192;
    return (uint8_t)(level > 100 ? 100 : level);
}

#include "yk_adpcm.h"

static const int16_t STEPS[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,    31,
    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,
    157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,   544,   598,   658,
    724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

size_t yk_adpcm_bytes(uint32_t samples) { return samples ? YK_ADPCM_HEADER + (samples - 1 + 1) / 2 : 0; }

int16_t yk_adpcm_step(yk_adpcm_state_t *st, uint8_t code) {
    const int step = STEPS[st->index];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    st->predictor += code & 8 ? -diff : diff;
    if (st->predictor > 32767) st->predictor = 32767;
    if (st->predictor < -32768) st->predictor = -32768;
    st->index += INDEX[code & 15];
    if (st->index < 0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return (int16_t)st->predictor;
}

/** The code that brings the predictor closest to `x` (the standard IMA quantiser), applied to the state. */
static uint8_t encode_one(yk_adpcm_state_t *st, int16_t x) {
    const int step = STEPS[st->index];
    int diff = x - st->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) code |= 4, diff -= step;
    if (diff >= step >> 1) code |= 2, diff -= step >> 1;
    if (diff >= step >> 2) code |= 1;
    yk_adpcm_step(st, code);  // the decoder's view, so errors do not add up
    return code;
}

size_t yk_adpcm_encode(const int16_t *pcm, uint32_t n, uint8_t *out) {
    if (!n) return 0;
    yk_adpcm_state_t st = {.predictor = pcm[0], .index = 0};
    // A start index that suits the opening: the first difference's size.
    if (n > 1) {
        int d = pcm[1] - pcm[0];
        if (d < 0) d = -d;
        while (st.index < 88 && STEPS[st.index] < d) st.index++;
    }
    out[0] = (uint8_t)(pcm[0] & 0xFF);
    out[1] = (uint8_t)((uint16_t)pcm[0] >> 8);
    out[2] = (uint8_t)st.index;
    out[3] = 0;
    size_t k = YK_ADPCM_HEADER;
    for (uint32_t i = 1; i < n; i++) {
        const uint8_t code = encode_one(&st, pcm[i]);
        if ((i - 1) % 2 == 0) out[k] = code;
        else out[k++] |= (uint8_t)(code << 4);
    }
    if ((n - 1) % 2) k++;
    return k;
}

int yk_adpcm_begin(const uint8_t *clip, size_t bytes, yk_adpcm_state_t *st, int16_t *first) {
    if (!clip || bytes < YK_ADPCM_HEADER || clip[2] > 88) return -1;
    *first = (int16_t)(uint16_t)(clip[0] | (clip[1] << 8));
    st->predictor = *first;
    st->index = clip[2];
    return 0;
}

size_t yk_adpcm_decode(const uint8_t *clip, size_t bytes, uint32_t total, int16_t *out, size_t n) {
    yk_adpcm_state_t st;
    int16_t first;
    if (!total || yk_adpcm_begin(clip, bytes, &st, &first)) return 0;
    if (n > total) n = total;
    if (!n) return 0;
    out[0] = first;
    for (size_t i = 1; i < n; i++) {
        const size_t at = YK_ADPCM_HEADER + (i - 1) / 2;
        if (at >= bytes) return i;
        const uint8_t b = clip[at];
        out[i] = yk_adpcm_step(&st, (i - 1) % 2 ? b >> 4 : b & 15);
    }
    return n;
}

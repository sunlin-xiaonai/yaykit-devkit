// IMA ADPCM for packed sound effects (P13): 4 bits a sample, a quarter of PCM16 (Party Play's clips: 8 KB a second
// at 16 kHz, 12 KB at 24 kHz). Plain C, no ESP-IDF: the device decodes while mixing, the tools encode at build
// time (yaykit_speak/tools/adpcm-encode.c, or this header from any host program).
//
// Layout of a clip (YK_CLIP_IMA_ADPCM): one block for the whole sound.
//   bytes 0..1  first sample, int16 little endian (played as is)
//   byte  2     step index 0..88
//   byte  3     0
//   then 4-bit codes for samples 1, 2, 3, ...: the low nibble of each byte first.
// So `samples` samples take 4 + ceil((samples - 1) / 2) bytes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YK_ADPCM_HEADER 4

typedef struct {
    int32_t predictor;
    int32_t index;
} yk_adpcm_state_t;

/** Bytes a clip of `samples` samples takes. */
size_t yk_adpcm_bytes(uint32_t samples);

/** Encodes `n` samples into `out` (yk_adpcm_bytes(n) of room). Returns the bytes written (0 for n == 0). */
size_t yk_adpcm_encode(const int16_t *pcm, uint32_t n, uint8_t *out);

/** Reads the header: the state before sample 1 and the first sample. Returns 0, or -1 when it is too short. */
int yk_adpcm_begin(const uint8_t *clip, size_t bytes, yk_adpcm_state_t *st, int16_t *first);

/** One 4-bit code to the next sample. */
int16_t yk_adpcm_step(yk_adpcm_state_t *st, uint8_t code);

/** Decodes samples [from, from + n) of a clip of `total` samples sequentially from the start of the codes, for
 *  tests and tools (the mixer keeps a running state instead). Returns the samples written. */
size_t yk_adpcm_decode(const uint8_t *clip, size_t bytes, uint32_t total, int16_t *out, size_t n);

#ifdef __cplusplus
}
#endif

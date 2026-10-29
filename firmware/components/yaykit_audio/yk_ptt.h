// The push-to-talk recording's buffer (yk_audio_rec_*), pure C (host tested): 16-bit mono samples after a WAV
// header, filled frame by frame from the capture task, finished with the header written.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YK_PTT_HEADER 44

typedef struct {
    uint8_t *buf;     // YK_PTT_HEADER + 2 * cap bytes
    size_t cap;       // samples
    size_t n;         // samples so far
    uint32_t rate;
    uint32_t speech_frames;
} yk_ptt_t;

/** Bytes a buffer for `seconds` at `rate` needs. */
size_t yk_ptt_bytes(uint32_t rate, int seconds);
void yk_ptt_init(yk_ptt_t *r, uint8_t *buf, size_t bytes, uint32_t rate);
void yk_ptt_reset(yk_ptt_t *r);
/** Appends what fits; returns false once full (the recording stops there). */
bool yk_ptt_add(yk_ptt_t *r, const int16_t *pcm, size_t n, bool speech);
/** Writes the header; returns the WAV's length in bytes (0 when empty). */
size_t yk_ptt_finish(yk_ptt_t *r);

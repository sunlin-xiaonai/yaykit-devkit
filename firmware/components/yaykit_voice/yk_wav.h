// WAV container for 16-bit PCM. Pure C (host tested).
#pragma once

#include <stddef.h>
#include <stdint.h>

#define YK_WAV_HEADER_BYTES 44

/** Writes the 44-byte RIFF/WAVE header for 16-bit PCM mono at `sample_rate` with `data_bytes` of samples. */
void yk_wav_header(uint8_t out[YK_WAV_HEADER_BYTES], uint32_t sample_rate, uint32_t data_bytes);

/** Loudness of 16-bit samples as 0..100 (peak-based, for a level meter). */
uint8_t yk_wav_level(const int16_t *samples, size_t count);

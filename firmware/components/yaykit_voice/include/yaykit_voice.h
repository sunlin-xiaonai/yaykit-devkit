// Legacy (extension platform P2): new apps record with yaykit_audio at 16 kHz (yk_audio_rec_*, or its frames);
// this stays only for Homework Buddy until it moves over (second batch).
//
// Push-to-talk recording for cloud speech recognition. Both microphones (TDM slots 0 and 2; slot 1 is the
// speaker echo reference) are mixed to mono at the codec's 24 kHz and kept as a WAV in PSRAM. Nothing is
// written to flash or the TF card. Uses yaykit_board's audio at 24 kHz (yk_voice_init fails if it already
// runs at 16 kHz); mute speaker sounds while recording.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Brings up the codecs (idempotent) and reserves the PSRAM buffer for `max_seconds` of speech. */
esp_err_t yk_voice_init(int max_seconds);

/** Starts recording; stops by itself at the maximum length. */
esp_err_t yk_voice_start(void);
/** Asks the recording to stop and returns at once. */
void yk_voice_stop(void);
bool yk_voice_recording(void);
/** Microphone level of the last 20 ms, 0..100. */
uint8_t yk_voice_level(void);

/** Waits until the recording has stopped and returns the WAV (header + samples), or NULL if the microphone
 *  failed. Valid until the next yk_voice_start(). */
const uint8_t *yk_voice_wav(size_t *len, float *seconds);

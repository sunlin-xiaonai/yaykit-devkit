// The shared audio front end, device side (docs/architecture/audio.md, docs/design/yay-device.md 6.6): one
// capture task reads the ES7210's 4-slot TDM at 16 kHz, combines the two microphone pairs (yk_fe), runs ESP-SR's
// nsnet2 noise suppression and vadnet voice activity, then gain and limiter, and hands 16 kHz mono PCM to the
// firmware. The models live in the `models` partition above 16 MB (not mappable), so they are read into PSRAM at
// start. A ring keeps what was captured since yk_audio_start(), up to 0.5 s, for the first word.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "yk_fe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_AUDIO_RATE        16000
#define YK_AUDIO_PREROLL_MS  500

/** Processed audio, 16 kHz mono, in chunks of the noise model's size (16 ms for nsnet2). From the capture task. */
typedef void (*yk_audio_frame_fn)(const int16_t *pcm, size_t samples, bool speech, uint8_t level, void *ctx);

typedef struct {
    yk_audio_mode_t mode;       // YK_AUDIO_PUSH_TO_TALK for talking to Yay
    bool noise_suppression;     // nsnet2; off = the combined microphones as they are
    bool voice_activity;        // vadnet; off = the front end's energy gate
    float mic_gain_db;          // ES7210 gain; 0 = 30 dB
    yk_audio_frame_fn on_frame;
    void *ctx;
} yk_audio_config_t;

/** Starts the board's audio at 16 kHz, loads the models into PSRAM, creates the capture task (idle). */
esp_err_t yk_audio_init(void);

/** Opens the microphones; frames start arriving. ESP_ERR_INVALID_STATE while running. */
esp_err_t yk_audio_start(const yk_audio_config_t *cfg);
/** Closes the microphones once the frame in progress is done. Returns when they are closed. */
void yk_audio_stop(void);
bool yk_audio_running(void);

/** What was captured since yk_audio_start(), at most the last 0.5 s, oldest first. Returns the samples copied. */
size_t yk_audio_preroll(int16_t *out, size_t max_samples);

/** Level of the last chunk, 0..100. */
uint8_t yk_audio_level(void);

typedef struct {
    bool models;              // nsnet2 and vadnet are available
    uint32_t models_bytes, models_load_ms;
    uint32_t ns_chunk, vad_chunk;  // samples per model call
    uint32_t frames;          // 20 ms reads since start
    uint32_t late;            // reads that took over 40 ms (the I2S buffer is 160 ms deep)
    uint32_t fe_us, ns_us, vad_us, finish_us;  // average per 20 ms of audio
    uint32_t busy_max_us;     // the longest 20 ms worth of processing
    uint32_t open_ms;         // opening the microphones in the last start
} yk_audio_stats_t;
void yk_audio_stats(yk_audio_stats_t *out);

/** `audio stat | rec <ms> [raw|plain|nsonly] | opus <complexity> [bitrate]`: recordings are printed with
 *  yk_audio_print_wav() (tools/yay_dev.py rec). */
esp_err_t yk_audio_register_console(void);

/** Prints PCM as a WAV file: AUDIO_WAV_BEGIN, numbered base64 lines "AW <seq> <data>", AUDIO_WAV_END. */
void yk_audio_print_wav(const int16_t *pcm, size_t samples, int channels, uint32_t rate);

// ---------------------------------------------------------------- push-to-talk recording (P2)
// One utterance kept whole in PSRAM as a WAV, for one-shot recognition (/asr): what yaykit_voice did at 24 kHz
// without noise suppression, here at 16 kHz through the front end. New apps record with this (or stream with
// on_frame); yaykit_voice stays only for Homework Buddy until it moves over. Keep the start cue out of the recording
// as the main firmware does: yk_speak_chime(YK_CHIME_START) returns once heard, then yk_audio_rec_start().

/** Reserves the buffer for max_seconds (32 KB a second) and starts the board's audio at 16 kHz (yk_audio_init()). */
esp_err_t yk_audio_rec_init(int max_seconds);
/** Opens the microphones (push-to-talk settings, noise suppression and voice activity on) and records from the
 *  first frame, preroll included; stops by itself at the maximum. ESP_ERR_INVALID_STATE while the microphones
 *  are open for something else. */
esp_err_t yk_audio_rec_start(void);
/** Stops; returns once the microphones are closed. */
void yk_audio_rec_stop(void);
bool yk_audio_rec_running(void);
/** The last recording as a WAV (16 kHz mono 16-bit), valid until the next start. speech: the voice activity heard
 *  speech in it (when not, an app need not send it: no points for silence). NULL when nothing was recorded. */
const uint8_t *yk_audio_rec_wav(size_t *len, float *seconds, bool *speech);

// ---------------------------------------------------------------- Opus (60 ms, 16 kHz mono)

typedef struct yk_opus_enc yk_opus_enc_t;
typedef void (*yk_opus_packet_fn)(const uint8_t *packet, size_t bytes, void *ctx);

/** complexity 0..10 (the design uses 1), bitrate in bit/s (0 = 16000), VBR, VOIP. */
yk_opus_enc_t *yk_opus_enc_create(int complexity, int bitrate);
/** Buffers samples and emits one packet per 960 samples. Returns the packets emitted, or -1 on an encoder error. */
int yk_opus_enc_feed(yk_opus_enc_t *enc, const int16_t *pcm, size_t samples, yk_opus_packet_fn on_packet, void *ctx);
/** Drops buffered samples (between turns). */
void yk_opus_enc_reset(yk_opus_enc_t *enc);
/** Microseconds the last packet took to encode; bytes of all packets so far. */
uint32_t yk_opus_enc_last_us(const yk_opus_enc_t *enc);
size_t yk_opus_enc_bytes(const yk_opus_enc_t *enc);
void yk_opus_enc_destroy(yk_opus_enc_t *enc);

#ifdef __cplusplus
}
#endif

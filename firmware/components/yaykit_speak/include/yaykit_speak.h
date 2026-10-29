// Playback for Yay firmware (docs/design/yay-device.md 6.6, firmware guide 5, extension platform P13): mono through
// the ES8311 at the board's rate (16 kHz with yaykit_audio, 24 kHz for apps that never record).
// One task owns the speaker and the amplifier (GPIO43; no app switches it). It plays two things at once:
//  - the utterance: Opus packets (libopus, esp_audio_codec) or PCM into a 2 s PSRAM ring, starting once 120 ms are
//    buffered (or the utterance has ended), with 10 ms fades. Yay's voice, live (a voice session) or offline (an
//    Opus clip packed into the app: yk_speak_clip());
//  - sound effects (yk_sfx_*): up to YK_SFX_VOICES voices mixed every block: short clips (PCM, IMA ADPCM or Opus,
//    packed into the app) and voices the app renders itself (a callback per block: Party Play's collisions,
//    Breathe's whole breath synth). While an utterance plays, effects are 6 dB down. Effects can start at a
//    given future moment (the dice land, the wheel stops), and the speaker's clock says what is being heard now.
// While a live voice session holds the speaker (yk_speak_set_live()), effects are refused: the microphones are
// open and Yay's reply must not carry them. Offline lines and effects do mix.
// The amplifier is mute for YK_SPEAK_PA_WARM_MS after it is switched on, so playback waits that out with silence;
// call yk_speak_prepare() as soon as sound is likely (key down, key release) and nobody waits, or
// yk_speak_hold_amp(true) for a game where sounds come any moment. It stays on 3 s after the last sound. The level
// of what is being heard drives the mouth and the rim. v1 has no echo cancellation: the main firmware does not
// listen while it speaks.
//
// Latency: the default (20 ms blocks, 4 blocks ahead, the board's 8 x 20 ms DMA) puts ~100 ms between a call and
// the sound. Low latency (yk_speak_config_t.block_ms = 5 and a 4 x 5 ms DMA): ~25 ms, within P13's 50 ms.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "yk_speak_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_SPEAK_RATE 16000  // the default rate; yk_speak_rate() is the running one
#define YK_SPEAK_PA_WARM_MS 300  // measured: the amplifier is mute this long after it is switched on

/** After the board's audio runs at 16 kHz (audio_init_rate(16000); yk_audio_init() does it). Creates the task.
 *  Same as yk_speak_init_cfg() with every field 0. */
esp_err_t yk_speak_init(void);

typedef struct {
    uint32_t rate;     // 16000 or 24000; 0 = the board's running rate, else 16000. Starts the board's audio when it
                       // is not running yet (with block_ms-sized DMA frames, dma_frames of them)
    uint8_t block_ms;  // the mixer's block and how often it wakes: 20 (0, default) or 5 (low latency)
    uint8_t ahead;     // blocks written ahead of the speaker: 0 = 4 (default and low latency alike)
    uint8_t dma_frames;  // when this call starts the board's audio: 0 = 8 at 20 ms, 4 at 5 ms (Breathe: 12)
} yk_speak_config_t;
/** Creates the task with these settings (ESP_ERR_INVALID_STATE for a rate other than the board's running one). */
esp_err_t yk_speak_init_cfg(const yk_speak_config_t *cfg);
/** The rate everything is played at (16000 or 24000), 0 before init. PCM and ADPCM clips must be at this rate;
 *  Opus decodes to it. */
uint32_t yk_speak_rate(void);

/** A new utterance: whatever is still playing stops (10 ms fade) and the buffers are cleared. */
esp_err_t yk_speak_begin(void);
/** One Opus packet (mono, any rate libopus takes: it decodes to yk_speak_rate(); any frame length up to 120 ms). Copies it and returns; blocks while 2 s are
 *  already waiting (ESP_ERR_TIMEOUT after 3 s). */
esp_err_t yk_speak_opus(const uint8_t *packet, size_t bytes);
/** The same, waiting at most timeout_ms for room (0: not at all; ESP_ERR_TIMEOUT when full). */
esp_err_t yk_speak_opus_wait(const uint8_t *packet, size_t bytes, int timeout_ms);
/** Mono PCM at yk_speak_rate(), same rules as yk_speak_opus(). Use one or the other within an utterance. */
esp_err_t yk_speak_pcm(const int16_t *pcm, size_t samples);
/** No more audio for this utterance: what is buffered plays out. */
void yk_speak_end(void);
/** Stops now (10 ms fade); returns once the speaker is quiet. */
void yk_speak_stop(void);
/** The same without waiting: the speaker fades out within about 100 ms (what the I2S buffer still holds) while the
 *  caller goes on (a press must open the microphones at once). The next yk_speak_begin() waits for it. */
void yk_speak_cancel(void);
/** The microphones, opening while the speaker is closed, put the shared I2S (full duplex: its transmit side is the
 *  clock) in their own format, with larger DMA buffers: about 10 KB more internal RAM until the speaker next opens.
 *  After the microphones closed with nothing to play (a press that did not talk): the speaker's own format again,
 *  quietly (the amplifier stays off). Any task; done in the speaker's when it is idle, skipped while it plays and
 *  while the microphones record (yk_speak_mic). */
void yk_speak_reclaim(void);
/** Whoever calls yk_speak_reclaim() says when its microphones record: true before they open, false once they are
 *  closed. While true, no reclaim touches the codec (it would put the shared I2S's clock in the speaker's format
 *  under the recording); true waits for a reclaim going on to finish (about 20 ms). */
void yk_speak_mic(bool recording);
/** Switches the amplifier on now (it needs YK_SPEAK_PA_WARM_MS), for 3 s unless something plays. */
void yk_speak_prepare(void);
/** Something is buffered or playing. */
bool yk_speak_busy(void);
/** Waits until the utterance has played out (or timeout_ms). True when done. */
bool yk_speak_wait(int timeout_ms);

/** Level of what is being heard now, 0..100 (follows the I2S buffer's delay). */
uint8_t yk_speak_level(void);

/** The short cue before the microphone opens or after it closes. Cuts off what is playing; returns once heard. */
esp_err_t yk_speak_chime(yk_chime_t kind);

/** Volume 0..10 (0 = muted). Default: the device's shared volume, board_pref_volume() (P10), read at init.
 *  Sets the speaker only; to keep a change for every firmware, also board_pref_set_volume(). */
/** Deep sleep (the battery's, P25), called last before it: the amplifier (GPIO43) off and held off through the
 *  sleep. A digital pad: the S3 lets go of it at the wake by itself. Nothing when the speaker never started. */
void yk_speak_sleep(void);
void yk_speak_volume(int level);
int yk_speak_get_volume(void);

// ---------------------------------------------------------------- offline lines, the amplifier, live sessions

/** A clip packed into the app (yk_clip_t below). */
typedef struct yk_clip yk_clip_t;

/** Plays an Opus clip as the utterance (an offline line of Yay's): like yk_speak_begin() + its packets +
 *  yk_speak_end(), read straight from flash. Effects go 6 dB down while it plays. Returns at once. */
esp_err_t yk_speak_clip(const yk_clip_t *clip);

/** Keeps the amplifier on while `on` (in a game: a sound can come any moment and must not wait 300 ms); off lets
 *  it go 3 s after the last sound. */
void yk_speak_hold_amp(bool on);

/** A live voice session holds the speaker (yaykit_talk, the main firmware's conversation): effects fade out in
 *  10 ms and yk_sfx_play_*() return YK_SFX_BUSY until it is released. Utterances play as before. */
void yk_speak_set_live(bool on);

// ---------------------------------------------------------------- the speaker's clock

typedef struct {
    uint64_t sample;  // samples heard since the speaker opened (at yk_speak_rate()); resets when it reopens
    int64_t at_us;    // esp_timer time that sample comes out of the speaker
    bool running;     // the speaker is open; false: sample and at_us are 0 and nothing is playing
} yk_speak_clock_t;
/** What is being heard now. For drawing in step with a sound: the picture for a sound started at t shows at t. */
void yk_speak_clock(yk_speak_clock_t *out);
/** How long a sound started now takes to be heard (the blocks and DMA ahead plus, while the amplifier is off, its
 *  warm-up), ms. For scheduling with `at_us`: a sound at now + latency is placed to the sample. */
int yk_speak_latency_ms(void);

// ---------------------------------------------------------------- sound effects (P13)

#define YK_SFX_VOICES 8   // at most this many effects at once (Party Play uses 6; Breathe's synth is one)
#define YK_SFX_BUSY (-1)  // no voice free, a live session holds the speaker, or not initialised
#define YK_SFX_BAD (-2)   // a bad clip or argument

/** A clip packed into the app, in flash (or anywhere that stays valid while it plays). */
typedef enum {
    YK_CLIP_PCM16,      // mono int16, little endian, at yk_speak_rate(); an effect
    YK_CLIP_IMA_ADPCM,  // mono IMA ADPCM at yk_speak_rate(), 4 bits a sample (yk_adpcm.h: layout and encoder); an effect
    YK_CLIP_OPUS,       // mono Opus packets, each a 2-byte little-endian length then the packet (any rate): a line
                        // for yk_speak_clip(), not an effect (one Opus decoder, the utterance's)
} yk_clip_format_t;

struct yk_clip {
    yk_clip_format_t format;
    const uint8_t *data;
    size_t bytes;
    uint32_t samples;  // decoded length at yk_speak_rate() (PCM and ADPCM: exact; Opus: may be 0 = unknown)
};

typedef int yk_sfx_id_t;  // > 0 while it plays; YK_SFX_BUSY or YK_SFX_BAD when it did not start

typedef struct {
    float gain;           // 0..1 before the volume; 0 = 1 (full). Changeable while it plays: yk_sfx_set_gain()
    float pitch;          // clips: playback-rate multiplier 0.5..2 (0 = 1); synth voices get it in their block
    int64_t at_us;        // esp_timer time to be heard; 0 or in the past = as soon as possible
    uint8_t group;        // 1..255: a sound of the same group started within `min_gap_ms` is dropped (0 = no group)
    uint16_t min_gap_ms;  // Party Play's dice: 18 ms, so a burst of hits does not blur
    bool no_duck;         // stays at full level under an utterance (Breathe's synth, a game's own timer)
    bool loop;            // clips: from the start again at the end, until yk_sfx_stop()
} yk_sfx_opts_t;

/** One block of a synth voice. Called on the speaker task every block (5 or 20 ms): render `n` mono samples into
 *  `out` (float, -1..1; overwrite, the mixer adds them). Must be quick (the whole mix has the block's time, shared
 *  by all voices), must not block, lock or allocate. Return false when finished (this block is still played). */
typedef struct {
    uint32_t rate;      // yk_speak_rate()
    uint64_t sample;    // the block's first sample on the speaker's clock
    int64_t heard_us;   // esp_timer time that first sample is heard
    float pitch;        // from the options
} yk_sfx_block_t;
typedef bool (*yk_sfx_render_fn)(float *out, size_t n, const yk_sfx_block_t *blk, void *ctx);

/** Starts a PCM16 or ADPCM clip; `opts` may be NULL (defaults). Returns at once. */
yk_sfx_id_t yk_sfx_play_clip(const yk_clip_t *clip, const yk_sfx_opts_t *opts);
/** Starts a voice the app renders (ctx is the app's until the callback returned false or yk_sfx_stop() returned). */
yk_sfx_id_t yk_sfx_play_synth(yk_sfx_render_fn render, void *ctx, const yk_sfx_opts_t *opts);
/** Fades it out over fade_ms (0: 2 ms, no click) and frees the voice. Returns once the callback is no longer
 *  called. An id that has finished is ignored. */
void yk_sfx_stop(yk_sfx_id_t id, int fade_ms);
void yk_sfx_stop_all(int fade_ms);
/** Changes the gain of a playing voice, ramped over ramp_ms. */
void yk_sfx_set_gain(yk_sfx_id_t id, float gain, int ramp_ms);
bool yk_sfx_playing(yk_sfx_id_t id);
/** Voices playing now. */
int yk_sfx_active(void);

typedef struct {
    uint32_t utterances;
    uint32_t first_ms;       // yk_speak_begin() to the first sample handed to I2S, last utterance
    uint32_t warm_ms;        // of that, waiting for the amplifier (0 when it was already up)
    uint32_t open_ms;        // opening the codec, last time
    uint32_t packets, decode_errors;
    uint32_t decode_us, decode_max_us;  // per packet, average and worst
    uint32_t underruns;      // 20 ms periods with nothing to play in the middle of an utterance
    uint32_t played_ms;
    uint32_t stack_free;     // the task's stack never used, bytes
    // added with the mixer (P13)
    uint32_t rate, block_ms;
    uint32_t sfx_started, sfx_dropped;  // dropped: no free voice, the group's gap, a live session
    uint32_t sfx_max;        // most voices at once
    uint32_t mix_us, mix_max_us;  // mixing one block (all voices, limiter), average and worst
    uint32_t late_blocks;    // blocks written after the speaker had run dry (a gap was heard)
    uint32_t limited;        // blocks the limiter turned down (the mix went over -3 dBFS)
} yk_speak_stats_t;
void yk_speak_stats(yk_speak_stats_t *out);

/** `speak stat | tone <hz> <ms> | chime start|end | vol <0-10> | prepare | stop | sfx <n> <ms>` (sfx: n voices of
 *  test tones at once, to measure the mix). */
esp_err_t yk_speak_register_console(void);

#ifdef __cplusplus
}
#endif

// yaykit_talk: the main firmware's push-to-talk conversation for extension firmwares (extension-platform.md P1,
// docs/delivery/2026-10-24-yaykit-talk.md). Hold the side key to talk, release, the reply is shown and played, a
// press stops it: as in Yay. The app only draws.
//
// Two layers:
// - the session (yk_talk_start…): the voice session in the app's mode ("practice", "translate"), the turn state
//   machine (yk_talk_sm.h), the device's settings by voice (volume.set, brightness.set, screen_off.set: carried out
//   here and answered with action.done, internal-commands.md 5.4);
// - below it, usable without a session (homework-buddy's 问字 may use only these, with its own HTTP): the
//   microphones into a preroll ring and out as Opus packets and/or PCM (yk_talk_mic_*), and the reply's playback
//   (yk_talk_play_*). The press rules come from yaykit_key: DOWN opens, HOLD (0.3 s) streams, UP stops.
//
// Memory: about 23 KB of internal RAM (the session's 8 KB task, the WebSocket's 6 KB and 2 × 2 KB buffers, this loop's
// 5 KB); the rest in PSRAM. `YK_TALK mem internal=… internal_min=… largest=… why=…` at the start, at hello.ok, as each
// turn listens and when it ends (release builds too). internal_min is the lowest since the app was ready
// (yaykit_link's yk_mem.h: the figure the 40 KB line is read on). One TLS connection at a time: hold the session
// (yk_talk_hold) around the app's own HTTPS.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "yk_key_types.h"
#include "yk_proto.h"
#include "yk_talk_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- the session

typedef enum {
    YK_TALK_EV_STATE,    // `state`: what to draw (在听 / 在想 / 回复 / 没连上)
    YK_TALK_EV_PARTIAL,  // `text`: the draft while held (stt.partial)
    YK_TALK_EV_HEARD,    // `text`, `lang`: what was heard (stt.final; "" = not heard)
    YK_TALK_EV_LINE,     // `line`, `text`, `lang`, `fix`: a sentence of the reply (its speech plays by itself)
    YK_TALK_EV_END,      // `end`, `muted`, `credits`: the turn is over
    YK_TALK_EV_SETTING,  // `kind` ("volume", "brightness", "screen_off"), `value`: set by voice or from the phone, done
                         // already (a volume set at rest has had its cue); the rim's ticks are the app's to show
    YK_TALK_EV_MESSAGE,  // `msg`: hello.ok; practice.turn, only its open turn's (not one stopped or cancelled), after
                         // the lines that came before it. Valid during the call only (copy what you need)
} yk_talk_event_type_t;

typedef struct {
    yk_talk_event_type_t type;
    yk_talk_state_t state;   // always: the state now
    const char *turn;        // always: the turn it is about ("" before the first)
    yk_talk_end_t end;
    const char *text;        // "" when none
    const char *lang;        // "" when not given (translate: which language it was)
    bool fix;                // practice: the line is the correction card, not a subtitle
    int line;
    const char *muted;       // END: "", "credits" or "speech" (subtitles only)
    int credits;             // END: remaining after the turn, -1 when not known
    const char *kind;        // SETTING
    int value;               // SETTING: volume, brightness 0..10; seconds
    const yk_srv_msg_t *msg; // MESSAGE
} yk_talk_event_t;

/** All in yaykit_talk's task (5 KB of internal stack), in order: copy what you need and take board_lock() yourself to
 *  draw. A press during the reply has stopped the speaker and opened the microphones before the END comes. */
typedef void (*yk_talk_event_fn)(const yk_talk_event_t *ev, void *ctx);

typedef struct {
    const char *slug, *version;   // the app's own: "language-buddy", "0.1.0" (X-Yaykit-Firmware, the hello's fw)
    const char *mode;             // "practice", "translate": hello.mode, one of the manifest's voice.session.modes
    const char *language, *level; // practice: "en", "beginner"
    const char *pair;             // translate: "zh-ja"
    const char *locale;           // the owner's language; NULL: yaykit_account's (production), else "zh"
    int max_hold_s;               // the manifest's voice.session.maxHoldSeconds; 0: 30
    bool no_chimes;               // the app plays its own cues
    // NULL: wss://device.yaykit.io/device/v1/session, with the app's token (call yk_account_init() with its slug
    // first). Development builds only (CONFIG_YAY_DEV): another server (ws://<Mac>:8787/device/v1/session for
    // yaykit/scripts/mock-voice-server.ts) or "usb" (tools/yay_dev.py bridge): no token is asked for, nothing
    // touches the production cloud. Release builds always use the production session.
    const char *url;
    yk_talk_event_fn on_event;
    void *ctx;
} yk_talk_config_t;

/** Once, after yk_wifi_start(), yk_audio_init() and yk_speak_init(): connects and keeps connected (yaykit_session's
 *  back-off). The strings are copied. */
esp_err_t yk_talk_start(const yk_talk_config_t *cfg);
/** Holds the session closed (true: whatever goes on stops, as a network drop) and lets it reconnect (false), around
 *  the app's own HTTPS or the phone link (one TLS at a time). Holding waits up to 3 s for the TLS to close: true once
 *  it is (then the app's own connection may start); false when it is not held (the USB bridge, not started) or was
 *  still closing after 3 s. Letting go returns at once. Not from yaykit_talk's own task (an on_event). */
bool yk_talk_hold(bool hold);
/** The side key, from the app's yk_app_on_key handler (yk_app_config_t.talks = true) or yk_key_subscribe(): DOWN,
 *  HOLD, UP (true: taken) and TAP, DOUBLE (false: the app's; they only close microphones a lost UP left open). */
bool yk_talk_key(const yk_key_event_t *ev);
/** A tap on the screen while held: listen.cancel. */
void yk_talk_cancel(void);
/** Stops the reply: playback.stop, quiet at once. */
void yk_talk_stop_reply(void);
/** A mode's own message (yk_proto_pair_set, yk_proto_practice_next…): sent after everything given before it, in the
 *  uplink's task. Takes the JSON and frees it. ESP_ERR_INVALID_STATE before yk_talk_start(). */
esp_err_t yk_talk_send(char *json);
/** Waits until everything given before it (yk_talk_send, a turn's own messages) went through the uplink, at most
 *  `timeout_ms`: ESP_OK all sent; ESP_FAIL through, but one could not be sent (no session); ESP_ERR_TIMEOUT not
 *  through in time; ESP_ERR_INVALID_STATE before yk_talk_start(). Nothing pending: at once. For leaving, whose restart
 *  would otherwise cut practice.end off before it went out (yk_app_on_leave has YK_APP_LEAVE_MS). */
esp_err_t yk_talk_flush(uint32_t timeout_ms);
/** For leaving before a restart (#580, 10-25): everything given before it through the uplink (yk_talk_flush), then the
 *  connection closed after it as WebSocket closes (yk_session_close), `timeout_ms` in all. True once the server closed
 *  back: it closes only after reading all that came before, so the last message (practice.end) got there. False: no
 *  close back in time, the uplink not through (then nothing is closed: it would go before what is still queued), or
 *  no connection; leave all the same. `flush=sent` alone only says the message was handed to TLS: a restart right
 *  after can still lose it. The session stays closed afterwards (yk_session_hold(false) connects again). */
bool yk_talk_close(uint32_t timeout_ms);
/** The leaving going on was asked from the phone (app.home, internal commands step 2), not by the ring: for the app's
 *  wrap-up (yk_app_on_leave), which can say so (the language buddy's practice.end why "home"). */
bool yk_talk_going_home(void);
/** The next hello's setup (a language or pair chosen in the app); the live one goes with yk_talk_send(). Called before
 *  yk_talk_start too: what it set stays, the start's config does not override it (the app read that config earlier). */
void yk_talk_set(const char *language, const char *level, const char *pair);
/** The language and level the session's last hello carried, "" before the first: what the server takes for this
 *  session until a practice.set. An app whose language may have changed meanwhile compares them at hello.ok. */
void yk_talk_hello_setup(char *language, size_t language_size, char *level, size_t level_size);
/** The app changed volume, brightness or the screen-off time itself: `prefs` goes up 2 s after the last change.
 *  brightness.set by voice changes the shared setting and a lit panel (on battery at 15 % or less: 60 % at most,
 *  as Yay); a dark panel stays dark: lighting the screen to show it is the app's (YK_TALK_EV_SETTING). */
void yk_talk_prefs_changed(void);
yk_talk_state_t yk_talk_state(void);
/** `talk` (state, the turn's numbers) and yaykit_session's `session stat` and, in development builds, `ys …` for
 *  the USB bridge. After esp_console_init(). */
esp_err_t yk_talk_register_console(void);

// ---------------------------------------------------------------- below the session: the microphones

typedef enum { YK_TALK_MIC_STOPPED, YK_TALK_MIC_CANCELLED, YK_TALK_MIC_DROPPED } yk_talk_mic_end_t;

/** Where a stream goes, in the microphones' task (32 KB of PSRAM stack): keep each call short. */
typedef struct {
    /** Before the first audio: `preroll_ms` recorded since yk_talk_mic_open() (at most 1000; older dropped).
     *  False: nothing of this stream is wanted after all. NULL: always. */
    bool (*begin)(const char *tag, int preroll_ms, void *ctx);
    /** 16 kHz mono as recorded, 960 samples (60 ms) a call, the preroll first. NULL: not wanted. */
    void (*pcm)(const int16_t *pcm, size_t samples, void *ctx);
    /** Opus, 60 ms a packet (16 kHz, complexity 1, about 16 kbit/s). NULL: not encoded. */
    void (*packet)(const uint8_t *opus, size_t bytes, void *ctx);
    /** After the last of it (stopped: the last piece padded to 60 ms), or cancelled, or dropped. */
    void (*end)(const char *tag, yk_talk_mic_end_t how, void *ctx);
    void *ctx;
} yk_talk_sink_t;

/** After yk_audio_init() and yk_speak_init(). Idempotent (yk_talk_start() calls it). */
esp_err_t yk_talk_mic_init(void);
/** Key down: the microphones on into a fresh 2 s ring (the stream before may still be going out of its own). */
void yk_talk_mic_open(void);
/** Held: what was recorded since the key went down, then the live audio, goes to `sink` (copied; `tag` ≤ 16). */
void yk_talk_mic_stream(const char *tag, const yk_talk_sink_t *sink);
/** Released: the microphones off; the rest goes out, then sink.end(STOPPED). */
void yk_talk_mic_stop(void);
/** A tap while held: nothing more but sink.end(CANCELLED). */
void yk_talk_mic_cancel(void);
/** A short press, an error, the network: the recording dropped, sink.end(DROPPED) if it was streaming. */
void yk_talk_mic_discard(void);
/** 0..100: what the microphones hear (the rim while listening); 0 when closed. */
uint8_t yk_talk_mic_level(void);

// ---------------------------------------------------------------- below the session: playback

/** Idempotent (yk_talk_start() calls it). */
esp_err_t yk_talk_play_init(void);
/** A new utterance: anything still queued is an older one's and is dropped. */
void yk_talk_play_begin(void);
/** One Opus packet of it (≤ 400 bytes): queued, never waits (5.8 s are kept; past that it is dropped: false). */
bool yk_talk_play_opus(const uint8_t *opus, size_t bytes);
/** Its end, after the packets queued. */
void yk_talk_play_end(void);
/** Quiet now (a 10 ms fade, not waited for), anything queued dropped. */
void yk_talk_play_stop(void);
/** The listening (YK_CHIME_START) or released (YK_CHIME_END) cue, as Yay plays them; returns at once. */
void yk_talk_play_cue(bool start);
/** After yk_talk_play_end(): it has all played out. */
bool yk_talk_play_done(void);

#ifdef __cplusplus
}
#endif

// yaykit_talk's turn state machine: the conversation layer of the main firmware's yay_sm.c, without its pages,
// countdowns and installs (docs/design/yay-device.md 6.4, extension-platform.md P1). Plain C: it only takes
// inputs and gives effects (open the microphones, send listen.start, stop the speaker…), host tested with
// test/run.sh. yaykit_talk.c carries the effects out.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YK_TALK_TURN_LEN 16
#define YK_TALK_PREROLL_MAX_MS 1000  // listen.start's preroll at most (the contract): a slow hold drops the oldest
#define YK_TALK_HOLD_S 30            // the longest hold when the app's manifest says nothing (voice.session)
#define YK_TALK_REPLY_WAIT_MS 15000  // released and nothing back: 有点忙
#define YK_TALK_REPLY_STALL_MS 30000 // replying and no new message: the turn is over
#define YK_TALK_FX_MAX 8

typedef enum {
    YK_TALK_OFFLINE,    // no session (no Wi-Fi, connecting, refused)
    YK_TALK_IDLE,       // connected, waiting for a press
    YK_TALK_LISTENING,  // held 0.3 s: the microphones stream to the cloud
    YK_TALK_THINKING,   // released: waiting for what was heard and the reply
    YK_TALK_REPLYING,   // the reply's sentences and speech are coming or playing
} yk_talk_state_t;

/** How a turn ended (YK_TALK_EV_END): the app shows its own notice, if any; the state is IDLE (or OFFLINE) again. */
typedef enum {
    YK_TALK_DONE,        // replied (or a reply stalled 30 s)
    YK_TALK_NOT_HEARD,   // an empty stt.final, NOT_UNDERSTOOD, AUDIO_TOO_SHORT / _LONG
    YK_TALK_BUSY,        // 15 s and nothing back, or any other error on the turn
    YK_TALK_BLOCKED,     // CONTENT_BLOCKED
    YK_TALK_NO_CREDITS,  // CREDITS_EXHAUSTED
    YK_TALK_STOPPED,     // stopped here: a press during the reply, a tap while held, yk_talk_stop_reply()
    YK_TALK_NO_NET,      // the session went (or a hold with none): TOKEN_EXPIRED, SESSION_REPLACED, …
} yk_talk_end_t;

const char *yk_talk_state_name(yk_talk_state_t state);
const char *yk_talk_end_name(yk_talk_end_t end);

typedef enum {
    YK_TSM_KEY_DOWN,
    YK_TSM_KEY_HOLD,     // held 0.3 s (yaykit_key's HOLD)
    YK_TSM_KEY_UP,
    YK_TSM_KEY_SHORT,    // a TAP or DOUBLE: a short press, in case its UP never came (the app ring took it). now_ms:
                         // yaykit_key's at_ms, when that press ended; a delayed TAP of the press before this one
                         // (delay_tap without drop_tap_before_hold) is older than this down, and changes nothing
    YK_TSM_CANCEL,       // a tap on the screen while held
    YK_TSM_STOP_REPLY,   // the app stops the reply
    YK_TSM_NET_UP,       // hello.ok
    YK_TSM_NET_DOWN,
    YK_TSM_STT_PARTIAL,  // turn
    YK_TSM_STT_FINAL,    // turn, text ("" = not heard)
    YK_TSM_REPLY_LINE,   // turn
    YK_TSM_REPLY_END,    // turn, muted
    YK_TSM_ERROR,        // turn ("" when none), code
    YK_TSM_SPEAK_DONE,   // everything given to the speaker has played
    YK_TSM_TICK,
} yk_tsm_input_type_t;

typedef struct {
    yk_tsm_input_type_t type;
    int64_t now_ms;  // the key's: yaykit_key's at_ms (the same clock: esp_timer, ms)
    const char *turn, *text, *code, *muted;
} yk_tsm_input_t;

typedef enum {
    YK_TSM_MIC_OPEN,       // key down: microphones on into a fresh ring, the amplifier warming
    YK_TSM_MIC_DISCARD,    // microphones off, the recording dropped, nothing sent
    YK_TSM_LISTEN_START,   // `turn`: listen.start with the preroll, then the live audio
    YK_TSM_LISTEN_STOP,    // `turn`: the rest of the audio, then listen.stop
    YK_TSM_LISTEN_CANCEL,  // `turn`: listen.cancel
    YK_TSM_PLAYBACK_STOP,  // `turn`: playback.stop (this turn's, or a server's turn not taken)
    YK_TSM_SPEAK_BEGIN,    // the reply's first sentence: a new utterance
    YK_TSM_SPEAK_END,      // reply.end: after what is queued; YK_TSM_SPEAK_DONE follows once played
    YK_TSM_SPEAK_STOP,     // quiet now, anything queued dropped
    YK_TSM_CHIME_START,    // the listening cue
    YK_TSM_CHIME_END,      // the released cue
    YK_TSM_PARTIAL,        // show the input's text: the draft while held
    YK_TSM_HEARD,          // show the input's text: what was heard
    YK_TSM_LINE,           // show the input's text: a sentence of the reply
    YK_TSM_ENDED,          // `end`: the turn is over (YK_TALK_EV_END)
} yk_tsm_fx_type_t;

typedef struct {
    yk_tsm_fx_type_t type;
    char turn[YK_TALK_TURN_LEN + 1];
    yk_talk_end_t end;
} yk_tsm_fx_t;

typedef struct {
    yk_talk_state_t state;
    bool online;
    bool pressed, holding;  // the key is down; and held past 0.3 s
    bool turn_open;         // this turn still has messages to come (reply.end or an error closes it)
    bool reply_done;        // reply.end came
    bool speaking;          // the reply's speech was begun and has not played out
    bool muted;             // reply.end said muted: subtitles only
    char turn[YK_TALK_TURN_LEN + 1];
    int64_t down_ms, released_ms, activity_ms;
    int max_hold_ms;
    uint32_t rand_state;
    yk_tsm_fx_t fx[YK_TALK_FX_MAX];
    int fx_count;
} yk_tsm_t;

/** max_hold_s: the manifest's voice.session.maxHoldSeconds (0: YK_TALK_HOLD_S); seed: any number, for the turn ids. */
void yk_tsm_init(yk_tsm_t *sm, int max_hold_s, uint32_t seed);
/** Takes one input; sm->fx[0..fx_count) are the effects, in order. True when the state changed. */
bool yk_tsm_feed(yk_tsm_t *sm, const yk_tsm_input_t *in);
/** The preroll listen.start says for `recorded_ms` (capped at YK_TALK_PREROLL_MAX_MS); *drop_ms: the oldest part
 *  to drop first (NULL: not wanted). */
int yk_tsm_preroll(int recorded_ms, int *drop_ms);
/** A server's turn ("s-…"), which it started itself. */
bool yk_tsm_server_turn(const char *turn);
/** A mode's own message for `turn` (practice.turn) goes to the app: this turn's, still open (not one stopped or
 *  cancelled since, nor an older one). */
bool yk_tsm_takes_message(const yk_tsm_t *sm, const char *turn);
/** As much of `in` as fits in `size` (terminated), cut before a UTF-8 character rather than inside one. Returns the
 *  bytes copied. */
size_t yk_talk_copy_cut(char *out, size_t size, const char *in);

#ifdef __cplusplus
}
#endif

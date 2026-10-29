#include "yk_talk_sm.h"

#include <stdio.h>
#include <string.h>

static const char *const STATE_NAMES[] = {"offline", "idle", "listening", "thinking", "replying"};
static const char *const END_NAMES[] = {"done", "not_heard", "busy", "blocked", "no_credits", "stopped", "no_net"};

const char *yk_talk_state_name(yk_talk_state_t state) {
    return (unsigned)state < sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]) ? STATE_NAMES[state] : "?";
}

const char *yk_talk_end_name(yk_talk_end_t end) {
    return (unsigned)end < sizeof(END_NAMES) / sizeof(END_NAMES[0]) ? END_NAMES[end] : "?";
}

static void copy(char *out, size_t size, const char *in) {
    size_t n = 0;
    for (; in && in[n] && n + 1 < size; n++) out[n] = in[n];
    out[n] = 0;
}

static yk_tsm_fx_t *fx(yk_tsm_t *sm, yk_tsm_fx_type_t type) {
    static yk_tsm_fx_t spare;  // never more than YK_TALK_FX_MAX in one input; a bug should not write past the end
    yk_tsm_fx_t *f = sm->fx_count < YK_TALK_FX_MAX ? &sm->fx[sm->fx_count++] : &spare;
    memset(f, 0, sizeof(*f));
    f->type = type;
    copy(f->turn, sizeof(f->turn), sm->turn);
    return f;
}

static bool has_fx(const yk_tsm_t *sm, yk_tsm_fx_type_t type) {
    for (int i = 0; i < sm->fx_count; i++)
        if (sm->fx[i].type == type) return true;
    return false;
}

static uint32_t next_rand(yk_tsm_t *sm) {
    sm->rand_state = sm->rand_state * 1103515245u + 12345u;
    return sm->rand_state >> 8;
}

/** 16 letters and digits: a device turn (yay-device.md 4.2). */
static void new_turn(yk_tsm_t *sm) {
    static const char set[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < YK_TALK_TURN_LEN; i++) sm->turn[i] = set[next_rand(sm) % 36];
    sm->turn[YK_TALK_TURN_LEN] = 0;
}

bool yk_tsm_server_turn(const char *turn) { return turn && turn[0] == 's' && turn[1] == '-'; }

static bool same_turn(const yk_tsm_t *sm, const char *turn) { return turn && sm->turn[0] && !strcmp(sm->turn, turn); }

bool yk_tsm_takes_message(const yk_tsm_t *sm, const char *turn) { return sm->turn_open && same_turn(sm, turn); }

size_t yk_talk_copy_cut(char *out, size_t size, const char *in) {
    if (!size) return 0;
    size_t n = in ? strlen(in) : 0;
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) n--;  // in[n] would start the next character
    }
    if (n) memcpy(out, in, n);
    out[n] = 0;
    return n;
}

void yk_tsm_init(yk_tsm_t *sm, int max_hold_s, uint32_t seed) {
    memset(sm, 0, sizeof(*sm));
    sm->state = YK_TALK_OFFLINE;  // until hello.ok
    sm->max_hold_ms = (max_hold_s > 0 ? max_hold_s : YK_TALK_HOLD_S) * 1000;
    sm->rand_state = seed ? seed : 1;
}

int yk_tsm_preroll(int recorded_ms, int *drop_ms) {
    const int have = recorded_ms < 0 ? 0 : recorded_ms;
    const int preroll = have > YK_TALK_PREROLL_MAX_MS ? YK_TALK_PREROLL_MAX_MS : have;
    if (drop_ms) *drop_ms = have - preroll;
    return preroll;
}

static bool go(yk_tsm_t *sm, yk_talk_state_t state) {
    if (sm->state == state) return false;
    sm->state = state;
    return true;
}

/** The turn is over: the app hears why, the state is the resting one. */
static bool ended(yk_tsm_t *sm, yk_talk_end_t end) {
    fx(sm, YK_TSM_ENDED)->end = end;
    sm->turn_open = false;
    go(sm, sm->online ? YK_TALK_IDLE : YK_TALK_OFFLINE);
    return true;
}

/** Stops whatever reply is playing or still coming (yay_sm's stop_reply). */
static void stop_reply(yk_tsm_t *sm) {
    if (sm->turn_open && !sm->reply_done) fx(sm, YK_TSM_PLAYBACK_STOP);
    if (sm->speaking) fx(sm, YK_TSM_SPEAK_STOP);
    sm->speaking = false;
    sm->turn_open = false;
}

static void release(yk_tsm_t *sm, int64_t now) {
    fx(sm, YK_TSM_LISTEN_STOP);
    fx(sm, YK_TSM_CHIME_END);
    sm->released_ms = now;
    go(sm, YK_TALK_THINKING);
}

static bool on_error(yk_tsm_t *sm, const yk_tsm_input_t *in) {
    const bool on_turn = in->turn && in->turn[0];
    if (on_turn && !same_turn(sm, in->turn)) return false;  // an old turn's
    const char *code = in->code ? in->code : "";
    const bool session_gone =
        !strcmp(code, "TOKEN_EXPIRED") || !strcmp(code, "SESSION_REPLACED") || !strcmp(code, "DEVICE_NOT_ACTIVATED");
    if (!on_turn && !session_gone) return false;  // turnless and nothing to do with this turn
    const bool active = sm->turn_open || sm->state == YK_TALK_LISTENING || sm->state == YK_TALK_THINKING ||
                        sm->state == YK_TALK_REPLYING;
    if (sm->speaking) fx(sm, YK_TSM_SPEAK_STOP);
    sm->speaking = false;
    sm->turn_open = false;
    if (!active) return false;
    if (session_gone) return ended(sm, YK_TALK_NO_NET);  // the session reconnects (or the device is no longer added)
    if (!strcmp(code, "CREDITS_EXHAUSTED")) return ended(sm, YK_TALK_NO_CREDITS);
    if (!strcmp(code, "NOT_UNDERSTOOD") || !strcmp(code, "AUDIO_TOO_SHORT") || !strcmp(code, "AUDIO_TOO_LONG"))
        return ended(sm, YK_TALK_NOT_HEARD);
    if (!strcmp(code, "CONTENT_BLOCKED")) return ended(sm, YK_TALK_BLOCKED);
    return ended(sm, YK_TALK_BUSY);  // VOICE_BUSY, SERVICE_BUSY, RATE_LIMITED, and any code this firmware does not know
}

static bool feed(yk_tsm_t *sm, const yk_tsm_input_t *in) {
    const int64_t now = in->now_ms;
    bool changed = false;
    switch (in->type) {
    case YK_TSM_KEY_DOWN:
        sm->pressed = true;
        sm->holding = false;
        sm->down_ms = now;
        // Thinking or replying: stopped at once (no echo cancellation: the reply would be in the recording).
        if (sm->speaking || sm->state == YK_TALK_REPLYING || sm->state == YK_TALK_THINKING) {
            stop_reply(sm);
            changed |= ended(sm, YK_TALK_STOPPED);
        }
        if (sm->online) fx(sm, YK_TSM_MIC_OPEN);
        break;

    case YK_TSM_KEY_HOLD:
        if (!sm->pressed) break;
        sm->holding = true;
        if (!sm->online) {
            fx(sm, YK_TSM_ENDED)->end = YK_TALK_NO_NET;  // held with no session: the app says so
            break;
        }
        new_turn(sm);
        sm->turn_open = true;
        sm->reply_done = sm->muted = false;
        fx(sm, YK_TSM_CHIME_START);  // the cue first: listen.start sets off the preroll's encoding
        fx(sm, YK_TSM_LISTEN_START);
        changed |= go(sm, YK_TALK_LISTENING);
        break;

    case YK_TSM_KEY_UP:
        if (sm->state == YK_TALK_LISTENING) {
            release(sm, now);
            changed = true;
        } else if (sm->pressed && !sm->holding && sm->online) {
            fx(sm, YK_TSM_MIC_DISCARD);  // a short press: what it recorded goes now
        }
        sm->pressed = sm->holding = false;
        break;

    case YK_TSM_KEY_SHORT:
        // The press before this one (its TAP held back for a double click): this press goes on.
        if (sm->pressed && now < sm->down_ms) break;
        // The UP of a press the app ring took may never come: nothing stays open after a short press.
        if (sm->pressed && !sm->holding && sm->online) fx(sm, YK_TSM_MIC_DISCARD);
        if (sm->state != YK_TALK_LISTENING) sm->pressed = sm->holding = false;
        break;

    case YK_TSM_CANCEL:
        if (sm->state != YK_TALK_LISTENING) break;
        fx(sm, YK_TSM_LISTEN_CANCEL);
        changed |= ended(sm, YK_TALK_STOPPED);
        break;

    case YK_TSM_STOP_REPLY:
        if (!sm->speaking && sm->state != YK_TALK_THINKING && sm->state != YK_TALK_REPLYING) break;
        stop_reply(sm);
        changed |= ended(sm, YK_TALK_STOPPED);
        break;

    case YK_TSM_NET_UP:
        sm->online = true;
        if (sm->state == YK_TALK_OFFLINE) changed |= go(sm, YK_TALK_IDLE);
        break;

    case YK_TSM_NET_DOWN: {
        const bool active = sm->turn_open || sm->state != YK_TALK_IDLE;
        sm->online = false;
        if (sm->state == YK_TALK_LISTENING || sm->pressed) fx(sm, YK_TSM_MIC_DISCARD);
        if (sm->speaking) fx(sm, YK_TSM_SPEAK_STOP);
        sm->speaking = false;
        if (active && sm->state != YK_TALK_OFFLINE) changed |= ended(sm, YK_TALK_NO_NET);
        else changed |= go(sm, YK_TALK_OFFLINE);
        sm->turn_open = false;
        break;
    }

    case YK_TSM_STT_PARTIAL:
        if (sm->state == YK_TALK_LISTENING && same_turn(sm, in->turn)) fx(sm, YK_TSM_PARTIAL);
        break;

    case YK_TSM_STT_FINAL:
        if (!same_turn(sm, in->turn) || !sm->turn_open) break;
        fx(sm, YK_TSM_HEARD);
        if (!in->text || !in->text[0]) changed |= ended(sm, YK_TALK_NOT_HEARD);  // its reply.end follows, ignored
        break;

    case YK_TSM_REPLY_LINE:
        if (yk_tsm_server_turn(in->turn) && !same_turn(sm, in->turn)) {
            // A turn the server started (an opening, a new topic): taken only while nothing else goes on.
            if (sm->pressed || sm->speaking || sm->state != YK_TALK_IDLE) {
                copy(fx(sm, YK_TSM_PLAYBACK_STOP)->turn, YK_TALK_TURN_LEN + 1, in->turn);
                break;
            }
            copy(sm->turn, sizeof(sm->turn), in->turn);
            sm->turn_open = true;
            sm->reply_done = sm->muted = false;
        }
        if (!same_turn(sm, in->turn) || !sm->turn_open) break;
        if (!sm->speaking) {
            fx(sm, YK_TSM_SPEAK_BEGIN);
            sm->speaking = true;
        }
        fx(sm, YK_TSM_LINE);
        sm->activity_ms = now;
        changed |= go(sm, YK_TALK_REPLYING);
        break;

    case YK_TSM_REPLY_END:
        if (!same_turn(sm, in->turn) || !sm->turn_open) break;
        sm->turn_open = false;
        sm->reply_done = true;
        sm->activity_ms = now;
        sm->muted = in->muted && in->muted[0];
        if (sm->speaking) fx(sm, YK_TSM_SPEAK_END);  // YK_TSM_SPEAK_DONE follows once it has played out
        else changed |= ended(sm, YK_TALK_DONE);       // nothing to play: subtitles only, or nothing said
        break;

    case YK_TSM_ERROR:
        changed |= on_error(sm, in);
        break;

    case YK_TSM_SPEAK_DONE:
        if (!sm->speaking) break;
        sm->speaking = false;
        if (sm->state == YK_TALK_REPLYING && sm->reply_done) changed |= ended(sm, YK_TALK_DONE);
        break;

    case YK_TSM_TICK:
        if (sm->state == YK_TALK_LISTENING && now - sm->down_ms >= sm->max_hold_ms) {
            release(sm, now);  // held as long as it may: as if released
            changed = true;
        } else if (sm->state == YK_TALK_THINKING && now - sm->released_ms >= YK_TALK_REPLY_WAIT_MS) {
            stop_reply(sm);
            changed |= ended(sm, YK_TALK_BUSY);
        } else if (sm->state == YK_TALK_REPLYING && now - sm->activity_ms >= YK_TALK_REPLY_STALL_MS) {
            // No next sentence and no reply.end, or a speaker that never says it is done: never stuck here.
            stop_reply(sm);
            changed |= ended(sm, YK_TALK_DONE);
        }
        break;
    }
    return changed;
}

bool yk_tsm_feed(yk_tsm_t *sm, const yk_tsm_input_t *in) {
    sm->fx_count = 0;
    const yk_talk_state_t before = sm->state;
    bool changed = feed(sm, in);
    // Leaving listening by any way but release or cancel (an error, the network…) closes the microphones: nothing
    // may keep streaming to the cloud.
    if (before == YK_TALK_LISTENING && sm->state != YK_TALK_LISTENING && !has_fx(sm, YK_TSM_LISTEN_STOP) &&
        !has_fx(sm, YK_TSM_LISTEN_CANCEL) && !has_fx(sm, YK_TSM_MIC_DISCARD))
        fx(sm, YK_TSM_MIC_DISCARD);
    return changed || sm->state != before;
}

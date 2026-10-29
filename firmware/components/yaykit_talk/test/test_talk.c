// Host tests for yaykit_talk's plain C parts: the turn state machine (the timelines of yay-device.md 4.4–4.6 and
// 6.4, as the main firmware's yay_sm.c has them), the settings by voice (#264's arithmetic), and the hello, against
// the platform's own examples (packages/contracts/fixtures/device.json).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "yk_proto.h"
#include "yk_talk_close.h"
#include "yk_talk_flush.h"
#include "yk_talk_hello.h"
#include "yk_talk_msg.h"
#include "yk_talk_setting.h"
#include "yk_talk_setup.h"
#include "yk_talk_sm.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

static yk_tsm_t sm;
static int64_t t;

static bool in(yk_tsm_input_type_t type, const char *turn, const char *text, const char *code) {
    const yk_tsm_input_t i = {.type = type, .now_ms = t, .turn = turn, .text = text, .code = code};
    return yk_tsm_feed(&sm, &i);
}
static bool feed(yk_tsm_input_type_t type) { return in(type, NULL, NULL, NULL); }
static bool has(yk_tsm_fx_type_t type) {
    for (int i = 0; i < sm.fx_count; i++)
        if (sm.fx[i].type == type) return true;
    return false;
}
static const yk_tsm_fx_t *get(yk_tsm_fx_type_t type) {
    for (int i = 0; i < sm.fx_count; i++)
        if (sm.fx[i].type == type) return &sm.fx[i];
    return NULL;
}
static const char *fx_turn(yk_tsm_fx_type_t type) { return get(type) ? get(type)->turn : ""; }
static bool ended_with(yk_talk_end_t end) { return get(YK_TSM_ENDED) && get(YK_TSM_ENDED)->end == end; }

/** Connected, at rest. */
static void start(void) {
    yk_tsm_init(&sm, 0, 7);
    t = 1000;
    feed(YK_TSM_NET_UP);
}

/** Down, held 0.3 s: listening; the turn's id. */
static const char *hold(void) {
    feed(YK_TSM_KEY_DOWN);
    t += 300;
    feed(YK_TSM_KEY_HOLD);
    return sm.turn;
}

static void test_turn(void) {
    yk_tsm_init(&sm, 0, 7);
    CHECK(sm.state == YK_TALK_OFFLINE);
    CHECK(feed(YK_TSM_NET_UP) && sm.state == YK_TALK_IDLE);
    // Down: the microphones open (and the amplifier warms), nothing sent yet.
    feed(YK_TSM_KEY_DOWN);
    CHECK(has(YK_TSM_MIC_OPEN) && sm.fx_count == 1 && sm.state == YK_TALK_IDLE);
    // Held 0.3 s: the cue, listen.start with a fresh turn, listening.
    t += 300;
    CHECK(feed(YK_TSM_KEY_HOLD) && sm.state == YK_TALK_LISTENING);
    CHECK(sm.fx_count == 2 && sm.fx[0].type == YK_TSM_CHIME_START && sm.fx[1].type == YK_TSM_LISTEN_START);
    char turn[17];
    snprintf(turn, sizeof(turn), "%s", sm.turn);
    CHECK(strlen(turn) == 16 && !strcmp(fx_turn(YK_TSM_LISTEN_START), turn));
    // The draft while held; another turn's is not shown.
    in(YK_TSM_STT_PARTIAL, turn, "明天", NULL);
    CHECK(has(YK_TSM_PARTIAL));
    in(YK_TSM_STT_PARTIAL, "abcdefghijklmnop", "别的", NULL);
    CHECK(!has(YK_TSM_PARTIAL));
    // Released: the rest and listen.stop, the cue, thinking.
    t += 2000;
    CHECK(feed(YK_TSM_KEY_UP) && sm.state == YK_TALK_THINKING);
    CHECK(has(YK_TSM_LISTEN_STOP) && has(YK_TSM_CHIME_END) && !has(YK_TSM_MIC_DISCARD));
    in(YK_TSM_STT_FINAL, turn, "明天会下雨吗", NULL);
    CHECK(has(YK_TSM_HEARD) && sm.state == YK_TALK_THINKING);
    // The first sentence begins an utterance; the next only adds a line.
    t += 500;
    CHECK(in(YK_TSM_REPLY_LINE, turn, "明天下午有小雨。", NULL) && sm.state == YK_TALK_REPLYING);
    CHECK(has(YK_TSM_SPEAK_BEGIN) && has(YK_TSM_LINE));
    in(YK_TSM_REPLY_LINE, turn, "记得带伞。", NULL);
    CHECK(!has(YK_TSM_SPEAK_BEGIN) && has(YK_TSM_LINE));
    // reply.end: the speaker finishes what it has; then done.
    in(YK_TSM_REPLY_END, turn, NULL, NULL);
    CHECK(has(YK_TSM_SPEAK_END) && !has(YK_TSM_ENDED) && sm.state == YK_TALK_REPLYING);
    CHECK(feed(YK_TSM_SPEAK_DONE) && sm.state == YK_TALK_IDLE && ended_with(YK_TALK_DONE));
}

static void test_presses(void) {
    // A short press: opened, then dropped at the release; nothing sent.
    start();
    feed(YK_TSM_KEY_DOWN);
    t += 120;
    feed(YK_TSM_KEY_UP);
    CHECK(has(YK_TSM_MIC_DISCARD) && sm.state == YK_TALK_IDLE && !sm.pressed);
    // A short press whose UP the app ring took: the TAP or DOUBLE closes the microphones all the same.
    start();
    feed(YK_TSM_KEY_DOWN);
    feed(YK_TSM_KEY_SHORT);
    CHECK(has(YK_TSM_MIC_DISCARD) && !sm.pressed);
    // A tap on the screen while held: listen.cancel, stopped; its reply.end is not a second end.
    start();
    const char *turn = hold();
    char id[17];
    snprintf(id, sizeof(id), "%s", turn);
    CHECK(feed(YK_TSM_CANCEL) && has(YK_TSM_LISTEN_CANCEL) && ended_with(YK_TALK_STOPPED) && sm.state == YK_TALK_IDLE);
    CHECK(!has(YK_TSM_MIC_DISCARD));
    t += 100;
    feed(YK_TSM_KEY_UP);
    CHECK(!has(YK_TSM_LISTEN_STOP));
    in(YK_TSM_REPLY_END, id, NULL, NULL);
    CHECK(!has(YK_TSM_ENDED));
    // Held as long as the manifest allows: as if released.
    yk_tsm_init(&sm, 10, 7);
    feed(YK_TSM_NET_UP);
    hold();
    t += 9600;
    feed(YK_TSM_TICK);
    CHECK(sm.state == YK_TALK_LISTENING);
    t += 200;
    feed(YK_TSM_TICK);
    CHECK(sm.state == YK_TALK_THINKING && has(YK_TSM_LISTEN_STOP));
    feed(YK_TSM_KEY_UP);  // let go later: nothing more
    CHECK(sm.fx_count == 0 && sm.state == YK_TALK_THINKING);
    // 0: the default 30 s.
    start();
    hold();
    t += 29000;
    feed(YK_TSM_TICK);
    CHECK(sm.state == YK_TALK_LISTENING);
    t += 1000;
    feed(YK_TSM_TICK);
    CHECK(sm.state == YK_TALK_THINKING);
}

static void test_barge_in(void) {
    // A press while Yay speaks: playback.stop (the turn still sends), the speaker quiet, then the microphones.
    start();
    char turn[17];
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_LINE, turn, "第一句。", NULL);
    feed(YK_TSM_KEY_DOWN);
    CHECK(sm.fx_count == 4 && sm.fx[0].type == YK_TSM_PLAYBACK_STOP && sm.fx[1].type == YK_TSM_SPEAK_STOP);
    CHECK(sm.fx[2].type == YK_TSM_ENDED && sm.fx[2].end == YK_TALK_STOPPED && sm.fx[3].type == YK_TSM_MIC_OPEN);
    CHECK(!strcmp(fx_turn(YK_TSM_PLAYBACK_STOP), turn) && sm.state == YK_TALK_IDLE);
    // What still comes for that turn is not played.
    in(YK_TSM_REPLY_LINE, turn, "第二句。", NULL);
    CHECK(!has(YK_TSM_SPEAK_BEGIN) && !has(YK_TSM_LINE) && sm.state == YK_TALK_IDLE);
    // After reply.end (still playing): the speaker stops, no playback.stop (the server is done).
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_LINE, turn, "第一句。", NULL);
    in(YK_TSM_REPLY_END, turn, NULL, NULL);
    feed(YK_TSM_KEY_DOWN);
    CHECK(!has(YK_TSM_PLAYBACK_STOP) && has(YK_TSM_SPEAK_STOP) && has(YK_TSM_MIC_OPEN) && ended_with(YK_TALK_STOPPED));
    // Thinking: playback.stop, nothing to quieten.
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    feed(YK_TSM_KEY_DOWN);
    CHECK(has(YK_TSM_PLAYBACK_STOP) && !has(YK_TSM_SPEAK_STOP) && ended_with(YK_TALK_STOPPED));
    // The app stops it (a button of its own).
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_LINE, turn, "第一句。", NULL);
    CHECK(feed(YK_TSM_STOP_REPLY) && has(YK_TSM_PLAYBACK_STOP) && has(YK_TSM_SPEAK_STOP) && ended_with(YK_TALK_STOPPED));
    CHECK(!feed(YK_TSM_STOP_REPLY) && sm.fx_count == 0);  // nothing to stop now
}

static void test_server_turns(void) {
    // At rest: a turn of the server's own (the practice opening) is taken and played.
    start();
    CHECK(in(YK_TSM_REPLY_LINE, "s-k2m8q4w9z1x7c3", "我们今天聊聊旅行吧。", NULL) && sm.state == YK_TALK_REPLYING);
    CHECK(has(YK_TSM_SPEAK_BEGIN) && has(YK_TSM_LINE) && !strcmp(sm.turn, "s-k2m8q4w9z1x7c3"));
    in(YK_TSM_REPLY_END, "s-k2m8q4w9z1x7c3", NULL, NULL);
    feed(YK_TSM_SPEAK_DONE);
    CHECK(ended_with(YK_TALK_DONE) && sm.state == YK_TALK_IDLE);
    // Held, thinking, speaking: not taken, playback.stop for it.
    start();
    char turn[17];
    snprintf(turn, sizeof(turn), "%s", hold());
    in(YK_TSM_REPLY_LINE, "s-aaaaaaaaaaaaaa", "换个话题。", NULL);
    CHECK(has(YK_TSM_PLAYBACK_STOP) && !strcmp(fx_turn(YK_TSM_PLAYBACK_STOP), "s-aaaaaaaaaaaaaa"));
    CHECK(sm.state == YK_TALK_LISTENING && !strcmp(sm.turn, turn));
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_LINE, "s-aaaaaaaaaaaaaa", "换个话题。", NULL);
    CHECK(has(YK_TSM_PLAYBACK_STOP) && sm.state == YK_TALK_THINKING);
    // The key down (not yet held): the user is about to talk.
    start();
    feed(YK_TSM_KEY_DOWN);
    in(YK_TSM_REPLY_LINE, "s-aaaaaaaaaaaaaa", "换个话题。", NULL);
    CHECK(has(YK_TSM_PLAYBACK_STOP) && !has(YK_TSM_SPEAK_BEGIN));
    // Offline: nothing taken.
    yk_tsm_init(&sm, 0, 7);
    in(YK_TSM_REPLY_LINE, "s-aaaaaaaaaaaaaa", "换个话题。", NULL);
    CHECK(has(YK_TSM_PLAYBACK_STOP) && sm.state == YK_TALK_OFFLINE);
}

static void test_ends(void) {
    struct {
        const char *code;
        yk_talk_end_t end;
    } codes[] = {{"CREDITS_EXHAUSTED", YK_TALK_NO_CREDITS}, {"NOT_UNDERSTOOD", YK_TALK_NOT_HEARD},
                 {"AUDIO_TOO_SHORT", YK_TALK_NOT_HEARD},    {"CONTENT_BLOCKED", YK_TALK_BLOCKED},
                 {"VOICE_BUSY", YK_TALK_BUSY},              {"SOMETHING_NEW_123", YK_TALK_BUSY}};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        start();
        char turn[17];
        snprintf(turn, sizeof(turn), "%s", hold());
        feed(YK_TSM_KEY_UP);
        CHECK(in(YK_TSM_ERROR, turn, NULL, codes[i].code) && ended_with(codes[i].end) && sm.state == YK_TALK_IDLE);
    }
    // An error for an older turn: nothing.
    start();
    hold();
    feed(YK_TSM_KEY_UP);
    CHECK(!in(YK_TSM_ERROR, "abcdefghijklmnop", NULL, "VOICE_BUSY") && sm.state == YK_TALK_THINKING);
    // An error while held closes the microphones.
    start();
    char turn[17];
    snprintf(turn, sizeof(turn), "%s", hold());
    in(YK_TSM_ERROR, turn, NULL, "SERVICE_BUSY");
    CHECK(has(YK_TSM_MIC_DISCARD) && ended_with(YK_TALK_BUSY));
    // Turnless: the session went (the session layer reconnects); others are nobody's business here.
    start();
    hold();
    feed(YK_TSM_KEY_UP);
    CHECK(!in(YK_TSM_ERROR, "", NULL, "RATE_LIMITED") && sm.state == YK_TALK_THINKING);
    CHECK(in(YK_TSM_ERROR, "", NULL, "SESSION_REPLACED") && ended_with(YK_TALK_NO_NET));
    start();
    CHECK(!in(YK_TSM_ERROR, "", NULL, "SESSION_REPLACED") && !has(YK_TSM_ENDED));  // at rest: nothing to end
    // Not heard: an empty stt.final ends it; the reply.end after it is not a second end.
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    CHECK(in(YK_TSM_STT_FINAL, turn, "", NULL) && ended_with(YK_TALK_NOT_HEARD) && sm.state == YK_TALK_IDLE);
    in(YK_TSM_REPLY_END, turn, NULL, NULL);
    CHECK(!has(YK_TSM_ENDED));
    // Muted (subtitles only): the lines show, nothing plays, reply.end ends it.
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_LINE, turn, "明天有雨。", NULL);
    const yk_tsm_input_t end = {.type = YK_TSM_REPLY_END, .now_ms = t, .turn = turn, .muted = "credits"};
    yk_tsm_feed(&sm, &end);
    CHECK(sm.muted && has(YK_TSM_SPEAK_END) && !has(YK_TSM_ENDED));  // no audio came: the end plays out at once
    CHECK(feed(YK_TSM_SPEAK_DONE) && ended_with(YK_TALK_DONE) && sm.state == YK_TALK_IDLE);
    // 15 s and nothing back: busy, playback.stop.
    start();
    hold();
    feed(YK_TSM_KEY_UP);
    t += YK_TALK_REPLY_WAIT_MS - 1;
    feed(YK_TSM_TICK);
    CHECK(sm.state == YK_TALK_THINKING);
    t += 1;
    feed(YK_TSM_TICK);
    CHECK(ended_with(YK_TALK_BUSY) && has(YK_TSM_PLAYBACK_STOP));
    // Replying and nothing new for 30 s: over.
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_LINE, turn, "第一句。", NULL);
    t += YK_TALK_REPLY_STALL_MS;
    feed(YK_TSM_TICK);
    CHECK(ended_with(YK_TALK_DONE) && has(YK_TSM_SPEAK_STOP) && sm.state == YK_TALK_IDLE);
}

static void test_network(void) {
    // Offline: a press opens nothing; a hold says there is no session.
    yk_tsm_init(&sm, 0, 7);
    feed(YK_TSM_KEY_DOWN);
    CHECK(!has(YK_TSM_MIC_OPEN));
    t += 300;
    feed(YK_TSM_KEY_HOLD);
    CHECK(ended_with(YK_TALK_NO_NET) && !has(YK_TSM_LISTEN_START) && sm.state == YK_TALK_OFFLINE);
    feed(YK_TSM_KEY_UP);
    CHECK(!has(YK_TSM_MIC_DISCARD));
    // The network goes while held: the microphones close, nothing more is sent.
    start();
    hold();
    CHECK(feed(YK_TSM_NET_DOWN) && has(YK_TSM_MIC_DISCARD) && ended_with(YK_TALK_NO_NET) && sm.state == YK_TALK_OFFLINE);
    feed(YK_TSM_KEY_UP);
    CHECK(!has(YK_TSM_LISTEN_STOP));
    // ... while speaking: quiet.
    start();
    char turn[17];
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_LINE, turn, "第一句。", NULL);
    feed(YK_TSM_NET_DOWN);
    CHECK(has(YK_TSM_SPEAK_STOP) && ended_with(YK_TALK_NO_NET));
    // ... at rest: offline, no end; back: idle.
    start();
    CHECK(feed(YK_TSM_NET_DOWN) && !has(YK_TSM_ENDED) && sm.state == YK_TALK_OFFLINE);
    CHECK(feed(YK_TSM_NET_UP) && sm.state == YK_TALK_IDLE);
}

static void test_preroll(void) {
    int drop = -1;
    CHECK(yk_tsm_preroll(340, &drop) == 340 && drop == 0);
    CHECK(yk_tsm_preroll(1500, &drop) == 1000 && drop == 500);
    CHECK(yk_tsm_preroll(-5, &drop) == 0 && drop == 0);
    CHECK(yk_tsm_server_turn("s-abc") && !yk_tsm_server_turn("abcdefghijklmnop") && !yk_tsm_server_turn(NULL));
    // Turn ids differ from turn to turn.
    start();
    char a[17];
    snprintf(a, sizeof(a), "%s", hold());
    feed(YK_TSM_KEY_UP);
    feed(YK_TSM_KEY_DOWN);
    t += 300;
    feed(YK_TSM_KEY_HOLD);
    CHECK(strcmp(a, sm.turn) != 0);
}

static void test_settings(void) {
    int unmute = -1;
    CHECK(yk_talk_volume_after(6, -1, 1, -1, &unmute, 6) == 7);
    CHECK(yk_talk_volume_after(10, -1, 2, -1, &unmute, 6) == 10);
    CHECK(yk_talk_volume_after(1, -1, -3, -1, &unmute, 6) == 0);
    CHECK(yk_talk_volume_after(6, 4, 0, -1, &unmute, 6) == 4);
    // Mute keeps the level before; 取消静音 goes back to it, and leaves an unmuted volume as it is.
    CHECK(yk_talk_volume_after(7, -1, 0, 1, &unmute, 6) == 0 && unmute == 7);
    CHECK(yk_talk_volume_after(0, -1, 0, 1, &unmute, 6) == 0 && unmute == 7);  // muted twice: still 7 after
    CHECK(yk_talk_volume_after(0, -1, 0, 0, &unmute, 6) == 7);
    CHECK(yk_talk_volume_after(5, -1, 0, 0, &unmute, 6) == 5);
    unmute = -1;
    CHECK(yk_talk_volume_after(0, -1, 0, 0, &unmute, 6) == 6);  // muted before a restart: the default
    // Brightness: level 0 is 5 %, and reads back as 0.
    CHECK(yk_talk_brightness_percent(0) == 5 && yk_talk_brightness_level(5) == 0);
    CHECK(yk_talk_brightness_percent(8) == 80 && yk_talk_brightness_level(80) == 8);
    CHECK(yk_talk_brightness_level(100) == 10 && yk_talk_brightness_level(15) == 2);
    CHECK(yk_talk_brightness_after(80, -1, -2) == 6 && yk_talk_brightness_after(20, -1, -3) == 0);
    CHECK(yk_talk_brightness_after(5, -1, 1) == 1 && yk_talk_brightness_after(80, 3, 0) == 3);
    CHECK(yk_talk_brightness_after(80, -1, 0) == -1);  // "auto": answered UNSUPPORTED
    CHECK(yk_talk_screen_off_s(0) == 0 && yk_talk_screen_off_s(10) == 15 && yk_talk_screen_off_s(60) == 60);
    CHECK(yk_talk_screen_off_s(7200) == 3600);
}

/** A tap, then within the double-click time a hold (review of #495): with delay_tap and no drop_tap_before_hold the
 *  first press's TAP comes between the second's DOWN and HOLD, stamped with its own release. The hold still talks. */
static void test_tap_then_hold(void) {
    start();
    const yk_tsm_input_t seq[] = {
        {.type = YK_TSM_KEY_DOWN, .now_ms = 1000}, {.type = YK_TSM_KEY_UP, .now_ms = 1100},
        {.type = YK_TSM_KEY_DOWN, .now_ms = 1300}, {.type = YK_TSM_KEY_SHORT, .now_ms = 1100},
        {.type = YK_TSM_KEY_HOLD, .now_ms = 1600},
    };
    bool discarded_second = false;
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        yk_tsm_feed(&sm, &seq[i]);
        if (i >= 2 && has(YK_TSM_MIC_DISCARD)) discarded_second = true;
    }
    CHECK(!discarded_second && sm.state == YK_TALK_LISTENING && has(YK_TSM_LISTEN_START) && sm.pressed);
    // A TAP or DOUBLE of this press itself (its UP lost to the app ring) still closes the microphones.
    start();
    const yk_tsm_input_t down = {.type = YK_TSM_KEY_DOWN, .now_ms = 2000}, tap = {.type = YK_TSM_KEY_SHORT, .now_ms = 2080};
    yk_tsm_feed(&sm, &down);
    yk_tsm_feed(&sm, &tap);
    CHECK(has(YK_TSM_MIC_DISCARD) && !sm.pressed);
}

/** Texts cut before a character, never inside one (review of #495: a 255-byte cut halved a character). */
static void test_copy_cut(void) {
    char out[8];
    CHECK(yk_talk_copy_cut(out, sizeof(out), "明天下雨") == 6 && !strcmp(out, "明天"));  // 3 bytes each: 7 would split
    CHECK(yk_talk_copy_cut(out, sizeof(out), "abc") == 3 && !strcmp(out, "abc"));
    CHECK(yk_talk_copy_cut(out, sizeof(out), "abcdefgh") == 7 && !strcmp(out, "abcdefg"));
    CHECK(yk_talk_copy_cut(out, 5, "aé明") == 3 && !strcmp(out, "aé"));  // 1 + 2 bytes; 明's 3 would not fit in 4
    CHECK(yk_talk_copy_cut(out, sizeof(out), NULL) == 0 && out[0] == 0);
    CHECK(yk_talk_copy_cut(out, 1, "明") == 0 && out[0] == 0);
    // 120 Chinese characters (reply.line's longest): 360 bytes, whole in 512.
    char line[361], big[512];
    for (int i = 0; i < 120; i++) memcpy(line + i * 3, "好", 3);
    line[360] = 0;
    CHECK(yk_talk_copy_cut(big, sizeof(big), line) == 360 && !strcmp(big, line));
}

/** A mode's own message (practice.turn) goes to the app only for the turn still open. */
static void test_takes_message(void) {
    start();
    char turn[17];
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    CHECK(yk_tsm_takes_message(&sm, turn));
    CHECK(!yk_tsm_takes_message(&sm, "abcdefghijklmnop") && !yk_tsm_takes_message(&sm, NULL));
    in(YK_TSM_REPLY_LINE, turn, "Good.", NULL);
    CHECK(yk_tsm_takes_message(&sm, turn));  // the second part comes before reply.end
    feed(YK_TSM_KEY_DOWN);                   // stopped: what still comes for it is not shown
    CHECK(!yk_tsm_takes_message(&sm, turn));
    // Cancelled while held.
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_CANCEL);
    CHECK(!yk_tsm_takes_message(&sm, turn));
    // Over: reply.end closed it.
    start();
    snprintf(turn, sizeof(turn), "%s", hold());
    feed(YK_TSM_KEY_UP);
    in(YK_TSM_REPLY_END, turn, NULL, NULL);
    CHECK(!yk_tsm_takes_message(&sm, turn));
    // A server's own turn, taken: its messages are this turn's.
    start();
    in(YK_TSM_REPLY_LINE, "s-k2m8q4w9z1x7c3", "Let's talk about travel.", NULL);
    CHECK(yk_tsm_takes_message(&sm, "s-k2m8q4w9z1x7c3"));
}

static int allocs;
static void *counting_alloc(size_t n) {
    allocs++;
    return malloc(n);
}

/** The queue's copy of a practice.turn has its own strings: the session frees its message right after. */
static void test_msg_copy(const cJSON *cases) {
    const cJSON *c;
    const char *json = NULL;
    char *text = NULL;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(cases, "valid")) {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(c, "message");
        const cJSON *fix = cJSON_GetObjectItemCaseSensitive(m, "fix");
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "type"));
        if (type && !strcmp(type, "practice.turn") && fix) {
            text = cJSON_PrintUnformatted(m);
            json = text;
            break;
        }
    }
    CHECK(json != NULL);
    if (!json) return;
    yk_srv_msg_t m, copy;
    CHECK(yk_proto_parse(json, strlen(json), &m) == YK_SRV_PRACTICE_TURN && m.said && m.better);
    allocs = 0;
    CHECK(yk_talk_msg_copy(&copy, &m, counting_alloc) && allocs >= 3);
    CHECK(copy.said != m.said && !strcmp(copy.said, m.said) && !strcmp(copy.better, m.better));
    CHECK(!strcmp(copy.turn, m.turn) && !strcmp(copy.verdict, m.verdict) && copy.span_count == m.span_count);
    CHECK(!copy.params && !copy.url && !copy.ids);
    yk_proto_free(&m);  // the session's goes; the copy stays whole
    CHECK(!strcmp(copy.better, "たこ焼きを二つください"));
    yk_proto_free(&copy);
    cJSON_free(text);
}

static void test_brightness_lit(void) {
    CHECK(yk_talk_brightness_lit(80, 50, false) == 80);
    CHECK(yk_talk_brightness_lit(80, 15, false) == 60 && yk_talk_brightness_lit(80, 10, false) == 60);
    CHECK(yk_talk_brightness_lit(40, 10, false) == 40);  // under the cap already
    CHECK(yk_talk_brightness_lit(80, 10, true) == 80);   // plugged in
    CHECK(yk_talk_brightness_lit(80, -1, false) == 80);  // not read
}

static const cJSON *item(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }

/** The hello's mode fields, as the platform's own examples have them for the same app and setup. */
static void test_hello(const cJSON *cases) {
    const cJSON *practice = NULL, *translate = NULL, *c;
    cJSON_ArrayForEach(c, item(cases, "valid")) {
        const cJSON *m = item(c, "message");
        const char *type = cJSON_GetStringValue(item(m, "type"));
        if (!type || strcmp(type, "hello")) continue;
        const char *mode = cJSON_GetStringValue(item(m, "mode"));
        if (mode && !strcmp(mode, "practice")) practice = m;
        if (mode && !strcmp(mode, "translate")) translate = m;
    }
    CHECK(practice && translate);
    if (!practice || !translate) return;
    const cJSON *setup = item(practice, "practice");
    yk_talk_hello_in_t h = {.fw = "language-buddy/0.1.0", .locale = "zh", .battery_pct = 71, .rssi = -60, .volume = 6,
                            .brightness_pct = 80, .screen_off_s = 30, .mode = "practice",
                            .language = cJSON_GetStringValue(item(setup, "language")),
                            .level = cJSON_GetStringValue(item(setup, "level"))};
    char *json = yk_talk_hello(&h);
    cJSON *got = json ? cJSON_Parse(json) : NULL;
    CHECK(got != NULL);
    if (got) {
        CHECK(!strcmp(cJSON_GetStringValue(item(got, "mode")), "practice"));
        CHECK(cJSON_Compare(item(got, "practice"), setup, 1));
        CHECK(cJSON_IsArray(item(got, "slots")) && cJSON_GetArraySize(item(got, "slots")) == 0);  // no installs here
        CHECK(!item(got, "pair"));
        CHECK(cJSON_GetNumberValue(item(item(got, "prefs"), "brightness")) == 8);
        // What yaykit_talk carries out by itself, back to Yay too (internal commands, step 2).
        const cJSON *caps = item(got, "caps");
        CHECK(cJSON_GetArraySize(caps) == 6);
        CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetArrayItem(caps, 5)), "app.home"));
        CHECK(!strcmp(cJSON_GetStringValue(item(got, "fw")), "language-buddy/0.1.0"));
        CHECK(cJSON_GetNumberValue(item(item(got, "audio"), "outRate")) == 16000);
    }
    cJSON_Delete(got);
    cJSON_free(json);
    h = (yk_talk_hello_in_t){.fw = "translator/0.1.0", .battery_pct = -1, .rssi = 1, .volume = 6, .brightness_pct = 80,
                             .screen_off_s = 0, .mode = "translate", .pair = cJSON_GetStringValue(item(translate, "pair"))};
    json = yk_talk_hello(&h);
    got = json ? cJSON_Parse(json) : NULL;
    CHECK(got != NULL);
    if (got) {
        CHECK(cJSON_Compare(item(got, "pair"), item(translate, "pair"), 1) && !item(got, "practice"));
        CHECK(!strcmp(cJSON_GetStringValue(item(got, "locale")), "zh"));                      // not known: zh
        CHECK(cJSON_GetNumberValue(item(item(got, "battery"), "pct")) == 100);                // not read: 100
        CHECK(cJSON_GetNumberValue(item(item(got, "wifi"), "rssi")) == -127);                 // not connected
        CHECK(cJSON_GetNumberValue(item(item(got, "prefs"), "screenOff")) == 0);
    }
    cJSON_Delete(got);
    cJSON_free(json);
    // A mode without its setup is refused here, before the platform closes the session for it.
    h.pair = NULL;
    CHECK(yk_talk_hello(&h) == NULL);
    h = (yk_talk_hello_in_t){.fw = "language-buddy/0.1.0", .mode = "practice", .language = "en"};
    CHECK(yk_talk_hello(&h) == NULL);
    h.level = "expert";
    CHECK(yk_talk_hello(&h) == NULL);  // yk_proto: not a level
    h = (yk_talk_hello_in_t){.fw = "yay/0.1.0", .mode = "ask"};
    CHECK(yk_talk_hello(&h) == NULL);  // not a session mode (yet)
}

// yk_talk_flush: a simulated uplink that gets through one command per sleep, or none while it is stuck.
typedef struct {
    yk_flush_t f;
    int64_t now;
    bool stuck;
    bool fail_next;     // the next message it gets to can not be sent
    bool post_on_sleep;  // another task queues one more while it waits
    int sleeps;
} uplink_t;

static yk_flush_t up_read(void *ctx) { return ((uplink_t *)ctx)->f; }
static int64_t up_now(void *ctx) { return ((uplink_t *)ctx)->now; }
static void up_sleep(uint32_t ms, void *ctx) {
    uplink_t *u = ctx;
    u->now += ms;
    u->sleeps++;
    if (u->post_on_sleep) yk_flush_post(&u->f), u->post_on_sleep = false;
    if (!u->stuck && (int32_t)(u->f.done - u->f.posted) < 0) {
        yk_flush_through(&u->f, u->f.done + 1, !u->fail_next);
        u->fail_next = false;
    }
}

static void test_flush(void) {
    uplink_t u = {0};
    const yk_flush_env_t env = {.read = up_read, .now_ms = up_now, .sleep_ms = up_sleep, .ctx = &u};
    // Nothing queued: at once, no sleep.
    CHECK(yk_flush_wait(&env, 300) == YK_FLUSH_SENT && u.sleeps == 0 && u.now == 0);
    // practice.end and two before it: back once the third is through.
    for (int i = 0; i < 3; i++) CHECK(yk_flush_post(&u.f) == (uint32_t)i + 1);
    CHECK(yk_flush_wait(&env, 300) == YK_FLUSH_SENT && u.f.done == 3 && u.now == 3 * YK_FLUSH_POLL_MS);
    // The session gone: through, but said.
    yk_flush_post(&u.f);
    u.fail_next = true;
    CHECK(yk_flush_wait(&env, 300) == YK_FLUSH_FAILED && u.f.failed == 1);
    // A failure before the call is not this one's.
    yk_flush_post(&u.f);
    CHECK(yk_flush_wait(&env, 300) == YK_FLUSH_SENT);
    // Stuck (the network stalls): back at its time, not later.
    yk_flush_post(&u.f);
    u.stuck = true;
    const int64_t t0 = u.now;
    CHECK(yk_flush_wait(&env, 300) == YK_FLUSH_TIMEOUT && u.now - t0 >= 300 && u.now - t0 < 300 + YK_FLUSH_POLL_MS);
    // What another task queues while it waits is not waited for.
    u.stuck = false;
    u.f.done = u.f.posted;
    yk_flush_post(&u.f);
    u.post_on_sleep = true;
    CHECK(yk_flush_wait(&env, 300) == YK_FLUSH_SENT && u.f.done == u.f.posted - 1);
    // Numbers that wrap.
    u = (uplink_t){.f = {.posted = 0xFFFFFFFEu, .done = 0xFFFFFFFEu}};
    yk_flush_post(&u.f);
    yk_flush_post(&u.f);  // 0
    CHECK(u.f.posted == 0 && yk_flush_wait(&env, 300) == YK_FLUSH_SENT && u.f.done == 0);
    CHECK(!strcmp(yk_flush_result_name(YK_FLUSH_TIMEOUT), "timeout"));
}

/** A connection for yk_close_run: the uplink takes flush_ms, the server closes back after reply_ms (-1 never), the
 *  connection drops gone_ms into the wait (-1 not); close() keeps to its budget as yk_session_close does. */
typedef struct {
    bool up;
    int flush_ms;
    yk_flush_result_t flush;
    int reply_ms, gone_ms;
    int64_t now;
    int flushes, closes;
    uint32_t close_budget;
} conn_t;

static bool c_up(void *ctx) { return ((conn_t *)ctx)->up; }
static int64_t c_now(void *ctx) { return ((conn_t *)ctx)->now; }
static yk_flush_result_t c_flush(uint32_t timeout_ms, void *ctx) {
    conn_t *c = ctx;
    c->flushes++;
    if (c->flush == YK_FLUSH_TIMEOUT || c->flush_ms > (int)timeout_ms) {
        c->now += timeout_ms;
        return YK_FLUSH_TIMEOUT;
    }
    c->now += c->flush_ms;
    return c->flush;
}
static yk_close_result_t c_close(uint32_t timeout_ms, void *ctx) {
    conn_t *c = ctx;
    c->closes++;
    c->close_budget = timeout_ms;
    const bool gone = c->gone_ms >= 0 && c->gone_ms <= (int)timeout_ms && (c->reply_ms < 0 || c->gone_ms < c->reply_ms);
    if (gone) {
        c->now += c->gone_ms;
        return YK_CLOSE_GONE;
    }
    if (c->reply_ms >= 0 && c->reply_ms <= (int)timeout_ms) {
        c->now += c->reply_ms;
        return YK_CLOSE_REPLIED;
    }
    c->now += timeout_ms;
    return YK_CLOSE_TIMEOUT;
}

/** Leaving with the connection closed (#580, 10-25): practice.end flushed, the close after it, the server's back. */
static void test_close(void) {
    conn_t c = {.up = true, .flush_ms = 4, .flush = YK_FLUSH_SENT, .reply_ms = 120, .gone_ms = -1};
    const yk_close_env_t env = {.up = c_up, .flush = c_flush, .close = c_close, .now_ms = c_now, .ctx = &c};
    // Closed back: the server read practice.end. The close gets what the flush left of the 500 ms.
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_REPLIED && c.flushes == 1 && c.closes == 1 && c.close_budget == 496 &&
          c.now == 124);
    // No close back (the release session's 08:23, where nothing came either): back at 500 ms, not later.
    c = (conn_t){.up = true, .flush_ms = 4, .flush = YK_FLUSH_SENT, .reply_ms = -1, .gone_ms = -1};
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_TIMEOUT && c.closes == 1 && c.now == 500);
    // Back too late is the same.
    c = (conn_t){.up = true, .flush_ms = 4, .flush = YK_FLUSH_SENT, .reply_ms = 600, .gone_ms = -1};
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_TIMEOUT && c.now == 500);
    // The connection already gone (offline, never up): at once, nothing flushed or sent.
    c = (conn_t){.up = false, .reply_ms = 120, .gone_ms = -1};
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_GONE && c.flushes == 0 && c.closes == 0 && c.now == 0);
    // It drops while waiting for the close: gone, at once.
    c = (conn_t){.up = true, .flush_ms = 4, .flush = YK_FLUSH_SENT, .reply_ms = -1, .gone_ms = 30};
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_GONE && c.now == 34);
    // It went before practice.end got out (a message found no session): nothing to close.
    c = (conn_t){.up = true, .flush_ms = 4, .flush = YK_FLUSH_FAILED, .reply_ms = 120, .gone_ms = -1};
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_GONE && c.closes == 0);
    // The uplink not through: no close, which would go out before practice.end.
    c = (conn_t){.up = true, .flush = YK_FLUSH_TIMEOUT, .reply_ms = 120, .gone_ms = -1};
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_TIMEOUT && c.closes == 0 && c.now == 500);
    // Through just at the end: nothing left to wait with.
    c = (conn_t){.up = true, .flush_ms = 500, .flush = YK_FLUSH_SENT, .reply_ms = 1, .gone_ms = -1};
    CHECK(yk_close_run(&env, 500) == YK_CLOSE_TIMEOUT && c.closes == 0 && c.now == 500);
    CHECK(!strcmp(yk_close_result_name(YK_CLOSE_REPLIED), "replied") &&
          !strcmp(yk_close_result_name(YK_CLOSE_GONE), "gone"));
}

/** The next hello's setup (#509, 10-25): a language chosen before yk_talk_start stays; the hello carries it. */
static void test_setup(void) {
    yk_talk_setup_t s = {0};
    // As it went wrong: the start's config read "ja", the app chose "en" before the session started.
    yk_talk_setup_choose(&s, "en", NULL, NULL);
    yk_talk_setup_start(&s, "ja", "everyday", NULL);
    CHECK(!strcmp(s.language, "en") && !strcmp(s.level, "everyday") && !s.pair[0]);
    yk_talk_hello_in_t h = {.fw = "language-buddy/0.1.0", .locale = "zh", .battery_pct = 71, .rssi = -60, .volume = 6,
                            .brightness_pct = 80, .screen_off_s = 30, .mode = "practice", .language = s.language,
                            .level = s.level};
    char *json = yk_talk_hello(&h);
    cJSON *o = json ? cJSON_Parse(json) : NULL;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "practice");
    CHECK(p && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "language")), "en"));
    cJSON_Delete(o);
    cJSON_free(json);
    // Nothing chosen: the config's. Chosen after the start: the next hello's.
    s = (yk_talk_setup_t){0};
    yk_talk_setup_start(&s, "ja", "everyday", NULL);
    CHECK(!strcmp(s.language, "ja"));
    yk_talk_setup_choose(&s, "es", NULL, NULL);
    CHECK(!strcmp(s.language, "es") && !strcmp(s.level, "everyday"));
    // Translate's pair the same way; NULL leaves a field; too long is cut, not overrun.
    s = (yk_talk_setup_t){0};
    yk_talk_setup_choose(&s, NULL, NULL, "zh-ja");
    yk_talk_setup_start(&s, NULL, NULL, "zh-en");
    CHECK(!strcmp(s.pair, "zh-ja") && !s.language[0] && !s.chose_language);
    yk_talk_setup_choose(&s, "abcdefghijk", NULL, NULL);
    CHECK(strlen(s.language) == sizeof(s.language) - 1);
}

int main(int argc, char **argv) {
    test_turn();
    test_presses();
    test_barge_in();
    test_server_turns();
    test_ends();
    test_network();
    test_preroll();
    test_settings();
    test_tap_then_hold();
    test_copy_cut();
    test_takes_message();
    test_brightness_lit();
    test_flush();
    test_close();
    test_setup();
    cJSON *cases = NULL;
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *text = malloc((size_t)n + 1);
            if (text && fread(text, 1, (size_t)n, f) == (size_t)n) {
                text[n] = 0;
                cases = cJSON_Parse(text);
            }
            free(text);
            fclose(f);
        }
    }
    CHECK(cases != NULL);
    if (cases) test_hello(cases);
    if (cases) test_msg_copy(cases);
    cJSON_Delete(cases);
    printf("yaykit_talk host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

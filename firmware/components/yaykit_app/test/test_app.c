// Host tests for the pure parts of yaykit_app. Run: bash firmware/components/yaykit_app/test/run.sh
#include <stdio.h>
#include <string.h>

#include "../yk_app_logic.h"
#include "yk_app_recovery.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                  \
    } while (0)

static void test_home(void) {
    CHECK(yk_home_slot("yay", "valid") == 0);
    CHECK(yk_home_slot("yay", "new") == 0);
    CHECK(yk_home_slot("yay", "pending") == 0);
    CHECK(yk_home_slot("yay", "unknown") == 0);
    CHECK(yk_home_slot("yay", "invalid") == -1);  // rolled back: the recovery has to fix it
    CHECK(yk_home_slot("yay", "aborted") == -1);
    CHECK(yk_home_slot("", "empty") == -1);
    CHECK(yk_home_slot("party-play", "valid") == -1);  // slot 1 holds something else: not home
    CHECK(yk_home_slot(NULL, NULL) == -1);

    CHECK(yk_recovery_forward(false, "yay", "valid"));
    CHECK(!yk_recovery_forward(true, "yay", "valid"));  // someone asked for the recovery
    CHECK(!yk_recovery_forward(false, "party-play", "valid"));
}

static void test_launch_accept(void) {
    const long long t = 1792310400;  // 2026-10-17
    CHECK(yk_launch_accept("language-buddy", t, "language-buddy", t + 3));
    CHECK(yk_launch_accept("language-buddy", t, "language-buddy", t + 60));
    CHECK(!yk_launch_accept("language-buddy", t, "language-buddy", t + 61));  // stale: ignore it
    CHECK(!yk_launch_accept("breathe", t, "language-buddy", t + 1));         // for another app
    CHECK(yk_launch_accept("breathe", t, "breathe", t - 3));                 // small skew
    CHECK(!yk_launch_accept("breathe", t, "breathe", t - 30));
    CHECK(yk_launch_accept("breathe", 0, "breathe", t));  // unknown times are not judged
    CHECK(yk_launch_accept("breathe", t, "breathe", 100));
    CHECK(!yk_launch_accept(NULL, t, "breathe", t));
}

static void test_escape(void) {
    char out[64];
    CHECK(yk_json_escape(out, sizeof(out), "a\"b\\c\nd\x01") == 16 && !strcmp(out, "a\\\"b\\\\c\\nd\\u0001"));
    CHECK(yk_json_escape(out, sizeof(out), "练得怎么样？") == strlen("练得怎么样？") && !strcmp(out, "练得怎么样？"));
    char small[5];
    CHECK(yk_json_escape(small, sizeof(small), "abcdefg") == 7 && !strcmp(small, "abcd"));  // cut, terminated
    CHECK(yk_json_escape(out, sizeof(out), NULL) == 0 && out[0] == 0);
}

static void test_records(void) {
    char out[1100];
    int n = yk_launch_format(out, sizeof(out), "yay", "language-buddy", "practice", "{\"language\":\"en\"}",
                             1792310400, "9f2c4a1e");
    CHECK(n > 0 && !strcmp(out, "{\"v\":1,\"from\":\"yay\",\"slug\":\"language-buddy\",\"intent\":\"practice\","
                                "\"params\":{\"language\":\"en\"},\"at\":1792310400,\"nonce\":\"9f2c4a1e\"}"));
    CHECK((size_t)n == strlen(out));
    n = yk_launch_format(out, sizeof(out), "yay", "breathe", NULL, NULL, 1, "n");
    CHECK(n > 0 && strstr(out, "\"params\":{}") && strstr(out, "\"intent\":\"\""));
    CHECK(yk_launch_format(out, sizeof(out), "yay", "breathe", "", "[1,2]", 1, "n") == -1);  // not an object
    CHECK(yk_launch_format(out, sizeof(out), "yay", NULL, "", NULL, 1, "n") == -1);

    // Over 1 KB in all is refused, even when the buffer is bigger.
    char big[1200];
    memset(big, 'a', sizeof(big));
    big[0] = '{', big[1] = '"', big[1000] = '"', big[1001] = ':', big[1002] = '1', big[1003] = '}', big[1004] = 0;
    CHECK(yk_launch_format(out, sizeof(out), "yay", "breathe", "", big, 1, "n") == -1);
    char tiny[40];
    CHECK(yk_launch_format(tiny, sizeof(tiny), "yay", "language-buddy", "practice", NULL, 1, "n") == -1);

    n = yk_result_format(out, sizeof(out), "language-buddy", "练了 8 分钟\"英语\"", "{\"minutes\":8}", 1792310880);
    CHECK(n > 0 && !strcmp(out, "{\"v\":1,\"slug\":\"language-buddy\",\"summary\":\"练了 8 分钟\\\"英语\\\"\","
                                "\"data\":{\"minutes\":8},\"at\":1792310880}"));
    n = yk_result_format(out, sizeof(out), "breathe", NULL, NULL, 5);
    CHECK(n > 0 && !strcmp(out, "{\"v\":1,\"slug\":\"breathe\",\"data\":{},\"at\":5}"));
}

static void test_banner(void) {
    char out[40];
    yk_banner_name(out, sizeof(out), "party-play");
    CHECK(!strcmp(out, "YAYKIT_PARTY_PLAY_READY"));
    yk_banner_name(out, sizeof(out), "yay");
    CHECK(!strcmp(out, "YAYKIT_YAY_READY"));
}

static void test_guard(void) {
    uint8_t n = 99;
    // Normal resets (power on, esp_restart, USB) start the count again.
    CHECK(yk_boot_guard(0, YK_RESET_NORMAL, true, &n) == YK_GUARD_RUN && n == 0);
    CHECK(yk_boot_guard(2, YK_RESET_NORMAL, false, &n) == YK_GUARD_RUN && n == 0);
    // One or two crashes in a row: try again.
    CHECK(yk_boot_guard(0, YK_RESET_CRASH, true, &n) == YK_GUARD_RUN && n == 1);
    CHECK(yk_boot_guard(1, YK_RESET_CRASH, false, &n) == YK_GUARD_RUN && n == 2);
    // The third: leave. The main firmware goes to the recovery, an app back to Yay.
    CHECK(yk_boot_guard(2, YK_RESET_CRASH, true, &n) == YK_GUARD_RECOVERY && n == 3);
    CHECK(yk_boot_guard(2, YK_RESET_CRASH, false, &n) == YK_GUARD_HOME && n == 3);
    CHECK(yk_boot_guard(7, YK_RESET_CRASH, true, &n) == YK_GUARD_RECOVERY && n == 8);  // stale count from an old build
    CHECK(yk_boot_guard(255, YK_RESET_CRASH, false, &n) == YK_GUARD_HOME && n == 255);  // saturates
    CHECK(yk_boot_guard(1, YK_RESET_CRASH, true, NULL) == YK_GUARD_RUN);
    // A whole loop: crash, crash, leave; a good start in between clears it (the device writes 0).
    uint8_t stored = 0;
    int runs = 0;
    for (int boot = 0; boot < 5; boot++) {
        if (yk_boot_guard(stored, YK_RESET_CRASH, true, &stored) != YK_GUARD_RUN) break;
        runs++;
    }
    CHECK(runs == YK_CRASHLOOP_LIMIT - 1 && stored == YK_CRASHLOOP_LIMIT);
    // Brownouts (a battery running low) are not the app's crashes: however many, it keeps starting.
    stored = 0;
    for (int boot = 0; boot < 10; boot++) CHECK(yk_boot_guard(stored, YK_RESET_POWER, false, &stored) == YK_GUARD_RUN);
    CHECK(stored == 0);
    // Nor do they wipe out real crashes before them: crash, crash, brownout, crash still leaves.
    CHECK(yk_boot_guard(0, YK_RESET_CRASH, false, &n) == YK_GUARD_RUN && n == 1);
    CHECK(yk_boot_guard(n, YK_RESET_CRASH, false, &n) == YK_GUARD_RUN && n == 2);
    CHECK(yk_boot_guard(n, YK_RESET_POWER, false, &n) == YK_GUARD_RUN && n == 2);
    CHECK(yk_boot_guard(n, YK_RESET_CRASH, false, &n) == YK_GUARD_HOME && n == 3);
}

static void test_names(void) {
    CHECK(!strcmp(yk_app_display_name("party-play"), "聚会玩"));
    CHECK(!strcmp(yk_app_display_name("yay"), "Yay"));
    CHECK(!strcmp(yk_app_display_name("language-buddy"), "口语陪练"));
    CHECK(!strcmp(yk_app_display_name("weather-pal"), "weather-pal"));  // not known here: the slug
    CHECK(!strcmp(yk_app_display_name(NULL), ""));
    // Added rows (10-24, Yay 0.1.3's install page): the two the table lacked; the old ones as they were.
    CHECK(!strcmp(yk_app_display_name("recording-card"), "录音卡") && !strcmp(yk_app_display_name("translator"), "面对面翻译"));
    CHECK(!strcmp(yk_app_display_name("breathe"), "呼吸之间") && !strcmp(yk_app_display_name("template"), "扩展模板"));
    // In English (each app.json's name); NULL when the table does not know it.
    CHECK(!strcmp(yk_app_table_name("translator", true), "Face to Face") &&
          !strcmp(yk_app_table_name("recording-card", true), "Recording Card"));
    CHECK(!strcmp(yk_app_table_name("party-play", true), "Party Play") && !strcmp(yk_app_table_name("party-play", false), "聚会玩"));
    CHECK(!yk_app_table_name("weather-pal", false) && !yk_app_table_name("weather-pal", true) && !yk_app_table_name(NULL, true));
    // On screen: the store's name, else the table's, else the slug, in either language.
    CHECK(!strcmp(yk_app_label("recording-card", "录音卡片", false), "录音卡片"));          // the catalog has it
    CHECK(!strcmp(yk_app_label("recording-card", "Voice Notes", true), "Voice Notes"));
    CHECK(!strcmp(yk_app_label("recording-card", NULL, false), "录音卡"));               // not in the catalog: the table
    CHECK(!strcmp(yk_app_label("translator", "", true), "Face to Face"));
    CHECK(!strcmp(yk_app_label("weather-pal", NULL, false), "weather-pal"));           // neither: the slug
    CHECK(!strcmp(yk_app_label("weather-pal", "", true), "weather-pal"));
    CHECK(!strcmp(yk_app_label(NULL, NULL, false), ""));
}

static void test_summary(void) {
    yk_summary_t s;
    char out[512];
    // The design's example: 8 rounds, new phrases in rounds 2, 5 and 7, recorded.
    yk_summary_init(&s);
    CHECK(yk_summary_stat(&s, "8", "轮") && yk_summary_stat(&s, "3", "新说法"));
    const int rounds[] = {2, 5, 7};
    yk_summary_beads(&s, 8, rounds, 3);
    s.recorded = 1;
    CHECK(yk_summary_format(out, sizeof(out), &s) > 0);
    CHECK(!strcmp(out, "{\"stats\":[{\"v\":\"8\",\"l\":\"轮\"},{\"v\":\"3\",\"l\":\"新说法\"}],"
                       "\"beads\":{\"total\":8,\"marked\":[2,5,7]},\"recorded\":true}"));
    // No more than 3 numbers; a value is needed.
    CHECK(yk_summary_stat(&s, "12", "分钟") && !yk_summary_stat(&s, "1", "多余") && s.stat_count == 3);
    yk_summary_init(&s);
    CHECK(!yk_summary_stat(&s, "", "空") && !yk_summary_stat(&s, NULL, "空") && s.stat_count == 0);
    // Nothing set: an empty stats list, no beads, no recorded.
    CHECK(yk_summary_format(out, sizeof(out), &s) > 0 && !strcmp(out, "{\"stats\":[]}"));
    s.recorded = 0;
    CHECK(yk_summary_format(out, sizeof(out), &s) > 0 && !strcmp(out, "{\"stats\":[],\"recorded\":false}"));
    // Over 20: 20 beads, item i of n on floor(i * 20 / n).
    CHECK(yk_summary_bead(0, 30) == 0 && yk_summary_bead(29, 30) == 19 && yk_summary_bead(14, 30) == 9);
    CHECK(yk_summary_bead(19, 20) == 19 && yk_summary_bead(5, 8) == 5);
    const int many[] = {0, 1, 29, 30, -1};  // 0 and 1 share bead 0; 30 and -1 are not items
    yk_summary_beads(&s, 30, many, 5);
    CHECK(s.beads == 20 && s.marked == ((1u << 0) | (1u << 19)));
    yk_summary_beads(&s, 0, NULL, 0);
    CHECK(s.beads == 0 && s.marked == 0);
    // A label that does not fit is cut between characters, never inside one.
    yk_summary_init(&s);
    CHECK(yk_summary_stat(&s, "1", "一二三四五六七八九十"));  // 30 bytes into 24: 7 characters
    CHECK(!strcmp(s.stats[0].l, "一二三四五六七"));
    // Too small a buffer: -1.
    CHECK(yk_summary_format(out, 8, &s) == -1);
    // Fresh for 30 minutes; a clock not known at either end cannot tell.
    const int64_t now = 1792400000;
    CHECK(yk_result_fresh(now - 1800, now) && !yk_result_fresh(now - 1801, now));
    CHECK(yk_result_fresh(1000, now) && yk_result_fresh(now - 7200, 1000));
}

static void test_outbox(void) {
    CHECK(yk_record_kind_ok("breathe.session") && yk_record_kind_ok("abc"));
    CHECK(!yk_record_kind_ok("ab") && !yk_record_kind_ok("Breathe") && !yk_record_kind_ok("9a") &&
          !yk_record_kind_ok("a-b.c") && !yk_record_kind_ok(NULL));
    char key[8];
    yk_record_key(0, key);
    CHECK(!strcmp(key, "rec0"));
    yk_record_key(17, key);
    CHECK(!strcmp(key, "rec1"));  // the ring: 16 entries
    yk_record_key(4294967295u, key);
    CHECK(!strcmp(key, "rec15"));
    char out[700];
    const int64_t at = 1792400000123LL;  // 2026-10-19T08:53:20.123Z
    CHECK(yk_record_format(out, sizeof(out), "breathe", "yk_0a1b2c3d4e5f", 7, "breathe.session",
                           "{\"minutes\":5,\"breaths\":42}", at) > 0);
    CHECK(!strcmp(out, "{\"app\":\"breathe\",\"id\":\"yk_0a1b2c3d4e5f:breathe:7\",\"kind\":\"breathe.session\","
                       "\"occurredAt\":\"2026-10-19T08:53:20.123Z\",\"clockTrusted\":true,"
                       "\"data\":{\"minutes\":5,\"breaths\":42}}"));
    // A clock never set: still a time, not trusted.
    CHECK(yk_record_format(out, sizeof(out), "breathe", "yk_0a1b2c3d4e5f", 8, "breathe.session", NULL, 5000) > 0);
    CHECK(strstr(out, "\"occurredAt\":\"1970-01-01T00:00:05.000Z\",\"clockTrusted\":false") && strstr(out, "\"data\":{}"));
    // Refused: a bad kind, data that is not an object or too long, a buffer too small.
    CHECK(yk_record_format(out, sizeof(out), "breathe", "yk_0a1b2c3d4e5f", 9, "Bad", NULL, at) == -1);
    CHECK(yk_record_format(out, sizeof(out), "breathe", "yk_0a1b2c3d4e5f", 9, "breathe.session", "[1]", at) == -1);
    char big[600];
    memset(big, 'x', sizeof(big));
    memcpy(big, "{\"a\":\"", 6);
    big[sizeof(big) - 3] = '"', big[sizeof(big) - 2] = '}', big[sizeof(big) - 1] = 0;
    CHECK(yk_record_format(out, sizeof(out), "breathe", "yk_0a1b2c3d4e5f", 9, "breathe.session", big, at) == -1);
    CHECK(yk_record_format(out, 40, "breathe", "yk_0a1b2c3d4e5f", 9, "breathe.session", NULL, at) == -1);
}

enum { DOWN = 1, HOLD, UP, TAP, DOUBLE };

/** The side key with the app ring (P23): which presses are the app's. */
static void test_keys(void) {
    yk_keys_t k;
    yk_keys_init(&k);
    // Taps and holds are the app's; a double click opens the ring.
    CHECK(yk_keys_route(&k, DOWN, 1000, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, UP, 1080, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, TAP, 1080, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, DOWN, 1200, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, UP, 1280, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, DOUBLE, 1280, false) == YK_KEYS_RING_OPEN && k.ring && !yk_keys_idle(&k));
    // A tap while it is open: closes it, and nothing of that press reaches the app.
    CHECK(yk_keys_route(&k, DOWN, 3000, false) == YK_KEYS_RING_CLOSE && !k.ring);
    CHECK(yk_keys_route(&k, UP, 3080, false) == YK_KEYS_EAT);
    CHECK(!yk_keys_idle(&k));
    CHECK(yk_keys_route(&k, TAP, 3080, false) == YK_KEYS_EAT && yk_keys_idle(&k));
    // A double click while it is open: the first press closes it; without delay_tap its TAP came already, so the
    // second press is the app's, but the double click does not open it again.
    yk_keys_route(&k, DOUBLE, 5000, false);
    CHECK(k.ring);
    CHECK(yk_keys_route(&k, DOWN, 6000, false) == YK_KEYS_RING_CLOSE);
    yk_keys_route(&k, UP, 6080, false);
    yk_keys_route(&k, TAP, 6080, false);
    CHECK(yk_keys_route(&k, DOWN, 6200, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, UP, 6280, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, DOUBLE, 6280, false) == YK_KEYS_EAT && !k.ring);
    // ... with delay_tap no TAP comes between: the whole double click is eaten.
    yk_keys_route(&k, DOUBLE, 9000, false);
    CHECK(yk_keys_route(&k, DOWN, 10000, false) == YK_KEYS_RING_CLOSE);
    CHECK(yk_keys_route(&k, UP, 10080, false) == YK_KEYS_EAT);
    CHECK(yk_keys_route(&k, DOWN, 10200, false) == YK_KEYS_EAT);
    CHECK(yk_keys_route(&k, UP, 10280, false) == YK_KEYS_EAT);
    CHECK(yk_keys_route(&k, DOUBLE, 10280, false) == YK_KEYS_EAT && yk_keys_idle(&k) && !k.ring);
    // A double click well after: the ring again.
    CHECK(yk_keys_route(&k, DOUBLE, 20000, false) == YK_KEYS_RING_OPEN);
    // Held while it is open: closes it; the hold and its release are the system's.
    CHECK(yk_keys_route(&k, DOWN, 21000, false) == YK_KEYS_RING_CLOSE);
    CHECK(yk_keys_route(&k, HOLD, 21300, false) == YK_KEYS_EAT);
    CHECK(yk_keys_route(&k, UP, 22000, false) == YK_KEYS_EAT && yk_keys_idle(&k));
    // It closed by itself (a tap on it): the next press is the app's.
    yk_keys_route(&k, DOUBLE, 30000, false);
    yk_keys_closed(&k);
    CHECK(yk_keys_idle(&k) && yk_keys_route(&k, DOWN, 31000, false) == YK_KEYS_APP);
    yk_keys_route(&k, UP, 31080, false);
    yk_keys_route(&k, TAP, 31080, false);
    // An app that takes the double click itself: no ring.
    CHECK(yk_keys_route(&k, DOUBLE, 40000, true) == YK_KEYS_APP && !k.ring);
    // A hold the app did not use, in an app that does not talk: 「回到 Yay 说？」; one it used, or a talking app: no.
    yk_keys_init(&k);
    CHECK(yk_keys_route(&k, DOWN, 1000, false) == YK_KEYS_APP);
    CHECK(yk_keys_route(&k, HOLD, 1300, false) == YK_KEYS_APP);
    CHECK(!yk_keys_ask(&k, true, false) && !yk_keys_ask(&k, false, true) && !k.ask);
    CHECK(yk_keys_ask(&k, false, false) && k.ask);
    CHECK(yk_keys_route(&k, UP, 2000, false) == YK_KEYS_EAT);  // the release of the hold that asked
    // A press says stay: the card goes, that press is the system's.
    CHECK(yk_keys_route(&k, DOWN, 3000, false) == YK_KEYS_ASK_CLOSE && !k.ask);
    CHECK(yk_keys_route(&k, UP, 3080, false) == YK_KEYS_EAT);
    CHECK(yk_keys_route(&k, TAP, 3080, false) == YK_KEYS_EAT && yk_keys_idle(&k));
    // A hold while the ring is up asks nothing (it closes the ring).
    yk_keys_route(&k, DOUBLE, 9000, false);
    yk_keys_route(&k, DOWN, 9500, false);
    yk_keys_route(&k, HOLD, 9800, false);
    CHECK(!yk_keys_ask(&k, false, false) && !k.ask);
}

// ---------------------------------------------------------------- what the app gets (#371's review)

#include "yk_key_sm.h"

typedef struct {
    int64_t at;
    bool down;
} edge_t;

/** Presses (key edges at these times) through the real key state machine with `cfg`, then yk_keys_route: the events
 *  the app gets ("down up tap…") and what the system did ("ring", "close"…). */
static int64_t s_clock;  // each press() starts 10 s after the last one began

static void press(const yk_key_config_t *cfg, yk_keys_t *k, const edge_t *edges, int n, char *app, size_t app_size,
                  char *sys, size_t sys_size) {
    s_clock += 10000;
    static const char *const names[] = {"?", "down", "hold", "up", "tap", "double"};
    static const char *const did[] = {"", "", "ring", "close", "stay"};  // APP, EAT, RING_OPEN, RING_CLOSE, ASK_CLOSE
    yk_key_sm_t sm;
    yk_key_sm_init(&sm, cfg);
    app[0] = sys[0] = 0;
    for (int64_t now = s_clock + edges[0].at; now <= s_clock + edges[n - 1].at + 1500; now += 5) {
        yk_key_event_t ev[YK_KEY_SM_MAX_EVENTS];
        int c = 0;
        for (int i = 0; i < n; i++)
            if (s_clock + edges[i].at == now) c += yk_key_sm_edge(&sm, edges[i].down, now, ev + c);
        c += yk_key_sm_tick(&sm, now, ev + c);
        for (int i = 0; i < c; i++) {
            const yk_keys_do_t d = yk_keys_route(k, ev[i].type, ev[i].at_ms, false);
            if (d == YK_KEYS_APP) {
                strncat(app, names[ev[i].type], app_size - strlen(app) - 1);
                strncat(app, " ", app_size - strlen(app) - 1);
            } else if (did[d][0]) {
                strncat(sys, did[d], sys_size - strlen(sys) - 1);
                strncat(sys, " ", sys_size - strlen(sys) - 1);
            }
        }
    }
}

static void test_app_events(void) {
    const edge_t dbl[] = {{1000, true}, {1080, false}, {1200, true}, {1280, false}};
    const edge_t tap[] = {{1000, true}, {1080, false}};
    char app[64], sys[64];
    yk_keys_t k;
    // Without delay_tap the first press of a double click is a TAP the app acts on (Party Play back to its menu,
    // Breathe paused, the template counting) before the ring opens: why the apps on yk_app_on_key use delay_tap.
    const yk_key_config_t plain = {0}, delayed = {.delay_tap = true};
    yk_keys_init(&k);
    press(&plain, &k, dbl, 4, app, sizeof(app), sys, sizeof(sys));
    CHECK(!strcmp(app, "down up tap down up ") && !strcmp(sys, "ring "));
    // With delay_tap: the app sees the two presses go down and up, never a tap; the ring opens.
    yk_keys_init(&k);
    press(&delayed, &k, dbl, 4, app, sizeof(app), sys, sizeof(sys));
    CHECK(!strcmp(app, "down up down up ") && !strcmp(sys, "ring "));
    // The ring open: a tap closes it, the app gets nothing of it.
    press(&delayed, &k, tap, 2, app, sizeof(app), sys, sizeof(sys));
    CHECK(!strcmp(app, "") && !strcmp(sys, "close ") && yk_keys_idle(&k));
    // A tap with the ring closed: the app's, once no double click can follow.
    press(&delayed, &k, tap, 2, app, sizeof(app), sys, sizeof(sys));
    CHECK(!strcmp(app, "down up tap ") && !strcmp(sys, ""));
    // The ring open, a double click: closed by its first press, nothing for the app, not opened again.
    press(&delayed, &k, dbl, 4, app, sizeof(app), sys, sizeof(sys));
    CHECK(!strcmp(sys, "ring "));
    press(&delayed, &k, dbl, 4, app, sizeof(app), sys, sizeof(sys));
    CHECK(!strcmp(app, "") && !strcmp(sys, "close ") && !k.ring && yk_keys_idle(&k));
}

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void test_update_record(void) {
    yk_update_t u = {.state = YK_UPDATE_PENDING, .version = "1.0.1", .sha256 = SHA_A, .from = "1.0.0"}, back;
    char buf[160];
    CHECK(yk_update_format(buf, sizeof(buf), &u) > 0);
    CHECK(!strcmp(buf, "1|yay|1.0.1|" SHA_A "|pending|0|1.0.0"));
    CHECK(yk_update_parse(buf, &back) && back.state == YK_UPDATE_PENDING && !strcmp(back.version, "1.0.1") &&
          !strcmp(back.sha256, SHA_A) && !strcmp(back.from, "1.0.0") && back.tries == 0);
    u.state = YK_UPDATE_TRIAL, u.tries = 2, u.from[0] = 0;  // `from` may be empty
    CHECK(yk_update_format(buf, sizeof(buf), &u) > 0 && yk_update_parse(buf, &back) && back.state == YK_UPDATE_TRIAL &&
          back.tries == 2 && !back.from[0]);
    CHECK(yk_update_format(buf, 20, &u) == -1);  // does not fit
    // Not written by this firmware: dropped.
    CHECK(!yk_update_parse(NULL, &back));
    CHECK(!yk_update_parse("", &back));
    CHECK(!yk_update_parse("2|yay|1.0.1|" SHA_A "|pending|0|", &back));          // another format
    CHECK(!yk_update_parse("1|breathe|1.0.1|" SHA_A "|pending|0|", &back));      // only Yay updates this way
    CHECK(!yk_update_parse("1|yay|1.0.1|" SHA_A "|done|0|", &back));             // unknown state
    CHECK(!yk_update_parse("1|yay|1.0.1|" SHA_A "|pending|256|", &back));        // tries out of range
    CHECK(!yk_update_parse("1|yay|1.0.1|" SHA_A "|pending||", &back));           // no tries
    CHECK(!yk_update_parse("1|yay||" SHA_A "|pending|0|", &back));               // no version
    CHECK(!yk_update_parse("1|yay|1.0.1|AAAA|pending|0|", &back));               // not a SHA-256
    CHECK(!yk_update_parse("1|yay|1.0.1|" SHA_A "|pending|0", &back));           // six fields
    CHECK(!yk_update_parse("1|yay|1.0.1|" SHA_A "|pending|0||x", &back));        // eight
    CHECK(!yk_update_parse("1|yay|1 0|" SHA_A "|pending|0|", &back));            // a space in the version
    // A version that would break the record is never written.
    yk_update_t bad = u;
    snprintf(bad.version, sizeof(bad.version), "1|0");
    CHECK(yk_update_format(buf, sizeof(buf), &bad) == -1);
    bad = u, bad.sha256[0] = 'G';
    CHECK(yk_update_format(buf, sizeof(buf), &bad) == -1);
    bad = u, bad.state = YK_UPDATE_NONE;
    CHECK(yk_update_format(buf, sizeof(buf), &bad) == -1);
}

static yk_update_in_t upd(yk_update_state_t state, uint8_t tries, bool home_usable, bool crashed, bool new_cached,
                          bool previous) {
    yk_update_in_t in = {.has = state != YK_UPDATE_NONE, .home_usable = home_usable, .home_crashed = crashed,
                         .new_cached = new_cached, .previous_cached = previous};
    in.rec.state = state;
    in.rec.tries = tries;
    return in;
}

static void test_update_next(void) {
    yk_update_in_t in;
    // No update: on as before; slot 1 broken or empty (a write cut short) with a previous version: it goes back.
    in = upd(YK_UPDATE_NONE, 0, true, false, false, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_GO);
    in = upd(YK_UPDATE_NONE, 0, false, false, false, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_RESTORE);
    in = upd(YK_UPDATE_NONE, 0, false, false, false, false);
    CHECK(yk_update_next(&in) == YK_UPDATE_GO);  // nothing to restore: the recovery page
    in = upd(YK_UPDATE_NONE, 0, true, true, false, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_GO);  // Yay crashing with no update going: the crash page, as before
    // Asked for: back up the Yay that runs first, then write.
    in = upd(YK_UPDATE_PENDING, 0, true, false, true, false);
    CHECK(yk_update_next(&in) == YK_UPDATE_BACKUP);
    in = upd(YK_UPDATE_PENDING, 0, true, false, false, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_DROP);  // the image is not in the cache: nothing changes
    in = upd(YK_UPDATE_PENDING, 0, false, false, true, false);
    CHECK(yk_update_next(&in) == YK_UPDATE_WRITE);  // no usable Yay to keep: straight in
    in = upd(YK_UPDATE_PENDING, YK_UPDATE_TRIES, false, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_ROLLBACK);
    // Backed up: write; the power went while writing (slot 1 empty): again, up to YK_UPDATE_TRIES writes.
    in = upd(YK_UPDATE_BACKED_UP, 0, true, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_WRITE);
    in = upd(YK_UPDATE_BACKED_UP, 1, false, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_WRITE);
    in = upd(YK_UPDATE_BACKED_UP, YK_UPDATE_TRIES, false, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_ROLLBACK);
    // Refused before slot 1 was touched (a bad signature), twice: the Yay there still works, nothing is rewritten.
    in = upd(YK_UPDATE_BACKED_UP, YK_UPDATE_TRIES, true, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_REJECT);
    in = upd(YK_UPDATE_BACKED_UP, 1, true, false, false, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_REJECT);
    // A bad hash or image halfway: the install dropped the cached copy and slot 1 is empty: back to the previous one.
    in = upd(YK_UPDATE_BACKED_UP, 1, false, false, false, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_ROLLBACK);
    in = upd(YK_UPDATE_BACKED_UP, 1, false, false, false, false);
    CHECK(yk_update_next(&in) == YK_UPDATE_DROP);  // nothing to go back to: the recovery page
    // The new one starts: on to it. It crashed three times at start, or slot 1 broke: back to the previous one.
    in = upd(YK_UPDATE_TRIAL, 1, true, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_GO);
    in = upd(YK_UPDATE_TRIAL, 1, true, true, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_ROLLBACK);
    in = upd(YK_UPDATE_TRIAL, 1, false, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_ROLLBACK);
    in = upd(YK_UPDATE_TRIAL, 1, true, true, true, false);
    CHECK(yk_update_next(&in) == YK_UPDATE_DROP);  // crashing, nothing to go back to: the crash page
    // Confirmed (record gone), it crashes three times much later: the crash page, not the previous version.
    in = upd(YK_UPDATE_NONE, 0, true, true, false, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_GO);
    CHECK(!strcmp(yk_update_step_name(YK_UPDATE_REJECT), "reject"));
    CHECK(!strcmp(yk_update_step_name(YK_UPDATE_ROLLBACK), "rollback") &&
          !strcmp(yk_update_state_name(YK_UPDATE_BACKED_UP), "backed_up"));
}

static void test_update_confirm(void) {
    yk_update_t u = {.state = YK_UPDATE_TRIAL, .version = "1.0.1", .sha256 = SHA_A};
    // On trial, the main firmware in slot 1: confirmed a stable minute after yk_app_ready(), online or not (hello.ok
    // only sooner). A crash before then leaves it on trial: three of them and the recovery goes back.
    CHECK(yk_update_confirm_after(&u, true) == YK_UPDATE_CONFIRM_MS && YK_UPDATE_CONFIRM_MS == 60000);
    CHECK(YK_UPDATE_CONFIRM_MS > YK_CRASHLOOP_STABLE_MS);
    CHECK(yk_update_confirm_after(&u, false) == -1);  // an app, or Yay not in slot 1: not its update
    CHECK(yk_update_confirm_after(NULL, true) == -1);  // no record
    u.state = YK_UPDATE_PENDING;
    CHECK(yk_update_confirm_after(&u, true) == -1);  // the Yay that asked: the recovery has not written the new one
    u.state = YK_UPDATE_BACKED_UP;
    CHECK(yk_update_confirm_after(&u, true) == -1);
    // The cache keeps the new image and the previous version from the request until the update is kept.
    CHECK(yk_update_holds_cache(&u));
    u.state = YK_UPDATE_PENDING;
    CHECK(yk_update_holds_cache(&u));
    u.state = YK_UPDATE_TRIAL;
    CHECK(yk_update_holds_cache(&u));
    u.state = YK_UPDATE_NONE;
    CHECK(!yk_update_holds_cache(&u) && !yk_update_holds_cache(NULL));
}

/** A tap on an extension's app ring (app-ecosystem.md 4.5): 「更多」 goes home with {"then":"more"}. */
static void test_ring_tapped(void) {
    char p[96];
    CHECK(yk_ring_tapped(NULL, "breathe", false, p, sizeof(p)) == YK_RING_STAY && p[0] == 0);
    CHECK(yk_ring_tapped("", "breathe", false, p, sizeof(p)) == YK_RING_STAY);
    CHECK(yk_ring_tapped("breathe", "breathe", true, p, sizeof(p)) == YK_RING_STAY);  // the app in use
    CHECK(yk_ring_tapped("yay", "breathe", false, p, sizeof(p)) == YK_RING_HOME && p[0] == 0);
    CHECK(yk_ring_tapped(YK_RING_SETTINGS, "breathe", false, p, sizeof(p)) == YK_RING_HOME_WITH);
    CHECK(!strcmp(p, "{\"then\":\"settings\"}"));
    CHECK(yk_ring_tapped(YK_RING_MORE, "breathe", false, p, sizeof(p)) == YK_RING_HOME_WITH);
    CHECK(!strcmp(p, "{\"then\":\"more\"}"));
    CHECK(yk_ring_tapped("party-play", "breathe", true, p, sizeof(p)) == YK_RING_OPEN && p[0] == 0);
    CHECK(yk_ring_tapped("translator", "breathe", false, p, sizeof(p)) == YK_RING_HOME_WITH);
    CHECK(!strcmp(p, "{\"then\":\"install\",\"slug\":\"translator\"}"));
    CHECK(!strcmp(YK_RING_MORE, "+more"));
    CHECK(!strcmp(YK_RING_SETTINGS, "+settings"));  // yaykit_ui's YK_APPS_SETTINGS
}

/** The new recovery (P3): the battery rule, which steps it guards, the automatic mark. */
static void test_update_p3(void) {
    // On battery under 30 %: no; at 30, on external power at any level, or not known: yes.
    CHECK(!yk_update_battery_ok(29, false));
    CHECK(!yk_update_battery_ok(0, false));
    CHECK(yk_update_battery_ok(30, false));
    CHECK(yk_update_battery_ok(5, true));
    CHECK(yk_update_battery_ok(-1, false));
    // Guarded: the backup, and the first write over a slot 1 that works; not a write after a power cut, nor going back.
    yk_update_in_t in = upd(YK_UPDATE_PENDING, 0, true, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_BACKUP && yk_update_step_guarded(YK_UPDATE_BACKUP, &in));
    in = upd(YK_UPDATE_BACKED_UP, 0, true, false, true, true);
    CHECK(yk_update_next(&in) == YK_UPDATE_WRITE && yk_update_step_guarded(YK_UPDATE_WRITE, &in));
    in = upd(YK_UPDATE_BACKED_UP, 1, false, false, true, true);  // cut short halfway: slot 1 may be broken
    CHECK(yk_update_next(&in) == YK_UPDATE_WRITE && !yk_update_step_guarded(YK_UPDATE_WRITE, &in));
    in = upd(YK_UPDATE_BACKED_UP, 1, true, false, true, true);  // written once already
    CHECK(!yk_update_step_guarded(YK_UPDATE_WRITE, &in));
    in = upd(YK_UPDATE_PENDING, 0, false, false, true, true);  // no usable Yay to keep: straight in, not guarded
    CHECK(yk_update_next(&in) == YK_UPDATE_WRITE && !yk_update_step_guarded(YK_UPDATE_WRITE, &in));
    CHECK(!yk_update_step_guarded(YK_UPDATE_RESTORE, &in) && !yk_update_step_guarded(YK_UPDATE_ROLLBACK, &in));
    CHECK(!yk_update_step_guarded(YK_UPDATE_GO, &in) && !yk_update_step_guarded(YK_UPDATE_REJECT, &in));
    CHECK(!yk_update_step_guarded(YK_UPDATE_WRITE, NULL));
    // Automatic only when the mark holds this record's sha256.
    const yk_update_t u = {.state = YK_UPDATE_PENDING, .version = "1.0.1", .sha256 = SHA_A, .from = "1.0.0"};
    CHECK(yk_update_auto_matches(SHA_A, &u));
    CHECK(!yk_update_auto_matches(SHA_B, &u));  // left from another update
    CHECK(!yk_update_auto_matches("", &u) && !yk_update_auto_matches(NULL, &u) && !yk_update_auto_matches(SHA_A, NULL));
    CHECK(!yk_update_auto_matches("1", &u));
    // The record stays seven fields, which every recovery since 0.1.0 reads: the mark never goes into it.
    char buf[160];
    yk_update_t back;
    CHECK(yk_update_format(buf, sizeof(buf), &u) > 0 && yk_update_parse(buf, &back));
    int bars = 0;
    for (const char *p = buf; *p; p++) bars += *p == '|';
    CHECK(bars == 6);
}

static void test_recovery(void) {
    // The six released recoveries: each one's ELF key and length give its version, sha256 and bytes
    static const struct {
        uint8_t elf[YK_RECOVERY_ELF_KEY];
        uint32_t size;
        const char *version, *sha_start;
    } released[] = {
        {{0xa4, 0x6d, 0xf9, 0xf5, 0x5a, 0x54, 0x47, 0xce}, 1609904, "0.1.0", "e38d0934112584ad"},
        {{0x1c, 0x9b, 0x03, 0x2d, 0xfc, 0x9c, 0xf8, 0x09}, 1609680, "0.1.1", "0734ed73f13d0a81"},
        {{0x6a, 0x58, 0xc5, 0x49, 0x94, 0xcc, 0x75, 0x05}, 1609680, "0.1.2", "6f61d5a71a3f18f4"},
        {{0x95, 0x5e, 0x05, 0xde, 0x33, 0x6b, 0x16, 0xa8}, 1616384, "0.1.3", "e96787bd89c3bc2b"},
        {{0xbe, 0x93, 0x0f, 0x2d, 0xf9, 0x8a, 0x47, 0xed}, 1616592, "0.1.4", "51b7736e02ffcd7c"},
        {{0x29, 0x76, 0xa4, 0xb3, 0xf5, 0x47, 0xe7, 0xea}, 1616624, "0.1.5", "417b48262d11e981"},
    };
    yk_recovery_t r;
    for (size_t i = 0; i < sizeof(released) / sizeof(released[0]); i++) {
        yk_recovery_identify(released[i].elf, "0.1.0", released[i].size, &r);
        CHECK(r.from == YK_RECOVERY_TABLE);
        CHECK(!strcmp(r.version, released[i].version));
        CHECK(strlen(r.sha256) == 64 && !strncmp(r.sha256, released[i].sha_start, 16));
        CHECK(r.size == released[i].size);
    }
    // Same ELF key, another length: not that release (changed after the build), hashed
    yk_recovery_identify(released[3].elf, "0.1.0", 1616400, &r);
    CHECK(r.from == YK_RECOVERY_PENDING && !strcmp(r.version, YK_RECOVERY_UNKNOWN) && !r.sha256[0] && r.size == 1616400);
    // Not in the table: the description's version unless it is the "0.1.0" every release says
    const uint8_t other[YK_RECOVERY_ELF_KEY] = {1, 2, 3, 4, 5, 6, 7, 8};
    yk_recovery_identify(other, "0.1.0", 1700000, &r);
    CHECK(r.from == YK_RECOVERY_PENDING && !strcmp(r.version, YK_RECOVERY_UNKNOWN));
    yk_recovery_identify(other, "0.1.6", 1700000, &r);  // P3: the recovery's version follows Yay's package
    CHECK(r.from == YK_RECOVERY_PENDING && !strcmp(r.version, "0.1.6") && !r.sha256[0]);
    yk_recovery_identify(other, "", 1700000, &r);
    CHECK(!strcmp(r.version, YK_RECOVERY_UNKNOWN));
    yk_recovery_identify(other, NULL, 1700000, &r);
    CHECK(!strcmp(r.version, YK_RECOVERY_UNKNOWN));
    yk_recovery_identify(other, "0.1.6-a-very-long-tag", 1700000, &r);  // does not fit: unknown, not cut
    CHECK(!strcmp(r.version, YK_RECOVERY_UNKNOWN));
    CHECK(!strcmp(yk_recovery_from_name(YK_RECOVERY_TABLE), "table"));
    CHECK(!strcmp(yk_recovery_from_name(YK_RECOVERY_NONE), "none"));

    // Hashed, kept in NVS, read again on the next start
    const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char rec[128];
    yk_recovery_identify(other, "0.1.0", 1700000, &r);
    CHECK(yk_recovery_record(rec, sizeof(rec), &r) == -1);  // nothing hashed yet
    CHECK(!yk_recovery_hashed(&r, "0123"));
    CHECK(!yk_recovery_hashed(&r, "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"));  // upper case
    CHECK(r.from == YK_RECOVERY_PENDING && !r.sha256[0]);
    CHECK(yk_recovery_hashed(&r, sha) && r.from == YK_RECOVERY_HASHED && !strcmp(r.sha256, sha));
    CHECK(yk_recovery_record(rec, sizeof(rec), &r) > 0);
    CHECK(!strcmp(rec, "1|0102030405060708|1700000|0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    CHECK(yk_recovery_record(rec, 40, &r) == -1);  // does not fit
    yk_recovery_t next;
    yk_recovery_identify(other, "0.1.0", 1700000, &next);
    CHECK(yk_recovery_record(rec, sizeof(rec), &r) > 0 && yk_recovery_from_record(rec, &next));
    CHECK(next.from == YK_RECOVERY_NVS && !strcmp(next.sha256, sha) && !strcmp(next.version, YK_RECOVERY_UNKNOWN));
    // A record of another recovery (replaced over the web USB): hashed again
    yk_recovery_identify(other, "0.1.0", 1700016, &next);
    CHECK(!yk_recovery_from_record(rec, &next) && next.from == YK_RECOVERY_PENDING && !next.sha256[0]);
    const uint8_t third[YK_RECOVERY_ELF_KEY] = {1, 2, 3, 4, 5, 6, 7, 9};
    yk_recovery_identify(third, "0.1.0", 1700000, &next);
    CHECK(!yk_recovery_from_record(rec, &next) && next.from == YK_RECOVERY_PENDING);
    // Not well formed
    yk_recovery_identify(other, "0.1.0", 1700000, &next);
    CHECK(!yk_recovery_from_record(NULL, &next));
    CHECK(!yk_recovery_from_record("", &next));
    CHECK(!yk_recovery_from_record("2|0102030405060708|1700000|" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &next));
    CHECK(!yk_recovery_from_record("1|0102030405060708|1700000|0123", &next));
    CHECK(!yk_recovery_from_record("1|0102030405060708||0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &next));
    CHECK(!yk_recovery_from_record("1|0102030405060708|1700000x|0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", &next));
    CHECK(!yk_recovery_from_record("1|0102030405060708|1700000|0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0", &next));
    CHECK(next.from == YK_RECOVERY_PENDING && !next.sha256[0]);
}

int main(void) {
    test_keys();
    test_ring_tapped();
    test_app_events();
    test_summary();
    test_outbox();
    test_home();
    test_guard();
    test_names();
    test_launch_accept();
    test_escape();
    test_records();
    test_banner();
    test_update_record();
    test_update_next();
    test_update_p3();
    test_update_confirm();
    test_recovery();
    printf("yaykit_app host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

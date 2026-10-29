// Host tests for the side-key state machine. Run: bash firmware/components/yaykit_key/test/run.sh
#include <stdio.h>
#include <string.h>

#include "../yk_key_sm.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                  \
    } while (0)

// Collected events as a string like "down hold up" with held times where they matter: "up:120 tap:120".
static char got[512];
static yk_key_sm_t sm;

static const char *name(yk_key_event_type_t t) {
    switch (t) {
    case YK_KEY_DOWN: return "down";
    case YK_KEY_HOLD: return "hold";
    case YK_KEY_UP: return "up";
    case YK_KEY_TAP: return "tap";
    case YK_KEY_DOUBLE: return "double";
    }
    return "?";
}

static void add(const yk_key_event_t *ev, int n) {
    for (int i = 0; i < n; i++) {
        char one[48];
        if (ev[i].type == YK_KEY_DOWN || ev[i].type == YK_KEY_HOLD)
            snprintf(one, sizeof(one), "%s@%lld ", name(ev[i].type), (long long)ev[i].at_ms);
        else
            snprintf(one, sizeof(one), "%s:%d ", name(ev[i].type), (int)ev[i].held_ms);
        strncat(got, one, sizeof(got) - strlen(got) - 1);
    }
}

static void edge(int pressed, int64_t t) {
    yk_key_event_t ev[YK_KEY_SM_MAX_EVENTS];
    add(ev, yk_key_sm_edge(&sm, pressed, t, ev));
}

// Runs every tick the driver would run up to t (at each deadline), like the key task does.
static void until(int64_t t) {
    yk_key_event_t ev[YK_KEY_SM_MAX_EVENTS];
    for (int64_t d = yk_key_sm_deadline(&sm); d >= 0 && d <= t; d = yk_key_sm_deadline(&sm)) {
        int n = yk_key_sm_tick(&sm, d, ev);
        add(ev, n);
        if (n == 0 && yk_key_sm_deadline(&sm) == d) break;  // nothing changed: avoid looping
    }
}

static void start(const yk_key_config_t *cfg) {
    yk_key_sm_init(&sm, cfg);
    got[0] = 0;
}

static int is(const char *want) {
    if (strcmp(got, want) == 0) return 1;
    printf("  got  \"%s\"\n  want \"%s\"\n", got, want);
    return 0;
}

static void test_tap_and_hold(void) {
    start(NULL);
    edge(1, 1000), until(1120), edge(0, 1120), until(3000);
    CHECK(is("down@1000 up:120 tap:120 "));

    start(NULL);
    edge(1, 1000), until(1299);
    CHECK(is("down@1000 "));
    until(1300);
    CHECK(is("down@1000 hold@1300 "));
    until(2500), edge(0, 2500);
    CHECK(is("down@1000 hold@1300 up:1500 "));
    CHECK(yk_key_sm_deadline(&sm) == -1);
}

static void test_hold_without_tick(void) {
    // The driver missed the 300 ms deadline (busy): the release still reports the hold, before UP.
    start(NULL);
    edge(1, 0), edge(0, 450);
    CHECK(is("down@0 hold@300 up:450 "));
}

static void test_double(void) {
    start(NULL);
    edge(1, 0), edge(0, 90), until(300), edge(1, 300), edge(0, 380), until(2000);
    CHECK(is("down@0 up:90 tap:90 down@300 up:80 double:80 "));

    // Too slow: the second press starts 451 ms after the first release, so two taps.
    start(NULL);
    edge(1, 0), edge(0, 100), until(551), edge(1, 551), edge(0, 640), until(2000);
    CHECK(is("down@0 up:100 tap:100 down@551 up:89 tap:89 "));

    // A hold as the second press is not a double click, and a third quick press starts afresh.
    start(NULL);
    edge(1, 0), edge(0, 100), edge(1, 200), until(500), edge(0, 900), edge(1, 1000), edge(0, 1050), until(3000);
    CHECK(is("down@0 up:100 tap:100 down@200 hold@500 up:700 down@1000 up:50 tap:50 "));

    // Three quick taps: a double, then a plain tap.
    start(NULL);
    edge(1, 0), edge(0, 60), edge(1, 150), edge(0, 210), edge(1, 300), edge(0, 360), until(2000);
    CHECK(is("down@0 up:60 tap:60 down@150 up:60 double:60 down@300 up:60 tap:60 "));
}

static void test_no_double(void) {
    yk_key_config_t cfg = {.no_double = true};
    start(&cfg);
    edge(1, 0), edge(0, 60), edge(1, 150), edge(0, 210), until(2000);
    CHECK(is("down@0 up:60 tap:60 down@150 up:60 tap:60 "));
    CHECK(yk_key_sm_deadline(&sm) == -1);
}

static void test_delay_tap(void) {
    yk_key_config_t cfg = {.delay_tap = true};
    // A single tap arrives once the window has closed: 450 ms after its release.
    start(&cfg);
    edge(1, 0), edge(0, 100), until(550);
    CHECK(is("down@0 up:100 "));
    until(551);
    CHECK(is("down@0 up:100 tap:100 "));

    // A double click sends no taps at all.
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 300), edge(0, 380), until(2000);
    CHECK(is("down@0 up:100 down@300 up:80 double:80 "));

    // The window closes while the second press is still down: wait for it. It becomes a hold, so the
    // first tap is final and comes before the hold.
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 400), until(2000), edge(0, 2000);
    CHECK(is("down@0 up:100 down@400 tap:100 hold@700 up:1600 "));

    // ... or it is short, so the pair is a double click even though it ended after the window.
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 500), until(600), edge(0, 600);
    CHECK(is("down@0 up:100 down@500 up:100 double:100 "));
}

static void test_drop_tap_before_hold(void) {
    // The main firmware (P21): a tap, then at once a hold, is talking only; the tap is never sent.
    yk_key_config_t cfg = {.delay_tap = true, .drop_tap_before_hold = true};
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 300), until(2000), edge(0, 2000);
    CHECK(is("down@0 up:100 down@300 hold@600 up:1700 "));
    CHECK(yk_key_sm_deadline(&sm) == -1);

    // The window closes while the second press is still down, and it becomes a hold: still no tap.
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 400), until(2000), edge(0, 2000);
    CHECK(is("down@0 up:100 down@400 hold@700 up:1600 "));

    // Released before the hold even ran (a busy driver): the hold is reported at the release, and the tap is not.
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 300), edge(0, 700);
    CHECK(is("down@0 up:100 down@300 hold@600 up:400 "));

    // A press after the window is a press of its own: the tap was final before it (sent at the edge).
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 700), until(1000), edge(0, 1100);
    CHECK(is("down@0 up:100 tap:100 down@700 hold@1000 up:400 "));

    // Single taps and double clicks are as with delay_tap alone.
    start(&cfg);
    edge(1, 0), edge(0, 100), until(551);
    CHECK(is("down@0 up:100 tap:100 "));
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 300), edge(0, 380), until(2000);
    CHECK(is("down@0 up:100 down@300 up:80 double:80 "));
    start(&cfg);
    edge(1, 0), edge(0, 100), edge(1, 500), until(600), edge(0, 600);
    CHECK(is("down@0 up:100 down@500 up:100 double:100 "));

    // A hold, a tap, a hold: each hold talks; the tap between them waits for its window and is dropped too.
    start(&cfg);
    edge(1, 0), until(400), edge(0, 400), edge(1, 500), edge(0, 560), edge(1, 700), until(1200), edge(0, 1200);
    CHECK(is("down@0 hold@300 up:400 down@500 up:60 down@700 hold@1000 up:500 "));

    // Without delay_tap the tap went at its release already: nothing to drop.
    yk_key_config_t plain = {.drop_tap_before_hold = true};
    start(&plain);
    edge(1, 0), edge(0, 100), edge(1, 300), until(2000), edge(0, 2000);
    CHECK(is("down@0 up:100 tap:100 down@300 hold@600 up:1700 "));
}

static void test_custom_times_and_repeats(void) {
    yk_key_config_t cfg = {.hold_ms = 500, .double_ms = 200};
    start(&cfg);
    // Repeated edges are ignored; 499 ms is still a tap with hold_ms 500; a 151 ms gap is within 200 ms.
    edge(1, 0), edge(1, 10), until(499), edge(0, 499), edge(0, 520), edge(1, 650), edge(0, 700), until(2000);
    CHECK(is("down@0 up:499 tap:499 down@650 up:50 double:50 "));
    start(&cfg);
    edge(1, 0), edge(0, 100), until(301), edge(1, 301), edge(0, 350), until(2000);
    CHECK(is("down@0 up:100 tap:100 down@301 up:49 tap:49 "));
}

int main(void) {
    test_tap_and_hold();
    test_hold_without_tick();
    test_double();
    test_no_double();
    test_delay_tap();
    test_drop_tap_before_hold();
    test_custom_times_and_repeats();
    printf("yaykit_key host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

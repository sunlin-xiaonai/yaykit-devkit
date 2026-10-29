// Host tests for the pure C parts of yaykit_cloud. Run: bash firmware/components/yaykit_cloud/test/run.sh
#include <stdio.h>
#include <string.h>

#include "../yk_codec.h"
#include "../yk_sse.h"
#include "yk_locale.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                  \
    } while (0)

static void test_base64url(void) {
    char out[64];
    // Same values as Buffer.from(x).toString('base64url') in Node.
    CHECK(yk_b64url_encode((const uint8_t *)"", 0, out, sizeof(out)) == 0 && !strcmp(out, ""));
    CHECK(yk_b64url_encode((const uint8_t *)"f", 1, out, sizeof(out)) == 2 && !strcmp(out, "Zg"));
    CHECK(yk_b64url_encode((const uint8_t *)"fo", 2, out, sizeof(out)) == 3 && !strcmp(out, "Zm8"));
    CHECK(yk_b64url_encode((const uint8_t *)"foo", 3, out, sizeof(out)) == 4 && !strcmp(out, "Zm9v"));
    const uint8_t bytes[] = {0xfb, 0xff, 0xbf, 0x04};
    CHECK(yk_b64url_encode(bytes, 4, out, sizeof(out)) == 6 && !strcmp(out, "-_-_BA"));
    char tiny[4];
    CHECK(yk_b64url_encode((const uint8_t *)"foo", 3, tiny, sizeof(tiny)) == 0);
    // A 65-byte P-256 point encodes to 87 characters, as the server expects.
    uint8_t point[65] = {4};
    char key[100];
    CHECK(yk_b64url_encode(point, sizeof(point), key, sizeof(key)) == 87);
}

static void test_messages(void) {
    char out[160];
    CHECK(yk_activation_message("yk_00000000beef", "ykc_abc", out, sizeof(out)) > 0);
    CHECK(!strcmp(out, "yaykit-activate:v1:yk_00000000beef:ykc_abc"));
    CHECK(yk_token_message("yk_00000000beef", "homework-buddy", "1791.mac", out, sizeof(out)) > 0);
    CHECK(!strcmp(out, "yaykit-token:v1:yk_00000000beef:homework-buddy:1791.mac"));
    char tiny[10];
    CHECK(yk_token_message("yk_00000000beef", "a", "b", tiny, sizeof(tiny)) == 0);
    CHECK(yk_claim_valid("ykc_0123456789abcdefABCDEF-_"));
    CHECK(!yk_claim_valid("ykc_short"));
    CHECK(!yk_claim_valid("abc_0123456789abcdefABCDEF"));
    CHECK(!yk_claim_valid("ykc_0123456789abcdef/ABCDEF"));
    CHECK(!yk_claim_valid(NULL));
}

static char events[8][32], datas[8][256];
static size_t lengths[8];
static int count;
static void on_event(const char *event, const char *data, void *ctx) {
    (void)ctx;
    if (count < 8) {
        snprintf(events[count], sizeof(events[count]), "%s", event);
        snprintf(datas[count], sizeof(datas[count]), "%s", data);
        lengths[count] = strlen(data);
    }
    count++;
}

static yk_sse_t parser;
static void test_sse(void) {
    // What Hono's streamSSE writes for a lookup, fed one byte at a time.
    const char *stream =
        "event: heard\ndata: {\"text\":\"老鹰的鹰怎么写？\"}\n\n"
        "event: answer\ndata: {\"found\":true,\"char\":\"鹰\"}\n\n"
        "event: done\ndata: {\"credits\":{\"charged\":2}}\n\n";
    count = 0;
    yk_sse_init(&parser, on_event, NULL);
    for (size_t i = 0; i < strlen(stream); i++) yk_sse_feed(&parser, stream + i, 1);
    yk_sse_finish(&parser);
    CHECK(count == 3);
    CHECK(!strcmp(events[0], "heard") && !strcmp(datas[0], "{\"text\":\"老鹰的鹰怎么写？\"}"));
    CHECK(!strcmp(events[1], "answer") && strstr(datas[1], "鹰"));
    CHECK(!strcmp(events[2], "done"));

    // CRLF line ends, comments, an event without a name, multi-line data, and a final event without a blank line.
    count = 0;
    yk_sse_init(&parser, on_event, NULL);
    const char *other = ": keep-alive\r\ndata: a\r\ndata: b\r\n\r\nevent: error\r\ndata: {\"error\":\"X\"}";
    yk_sse_feed(&parser, other, strlen(other));
    CHECK(count == 1 && !strcmp(events[0], "message") && !strcmp(datas[0], "a\nb"));
    yk_sse_finish(&parser);
    CHECK(count == 2 && !strcmp(events[1], "error") && !strcmp(datas[1], "{\"error\":\"X\"}"));

    // A line longer than the buffer is dropped without breaking the next event.
    count = 0;
    yk_sse_init(&parser, on_event, NULL);
    static char huge[YK_SSE_LINE_MAX + 100];
    memset(huge, 'x', sizeof(huge));
    memcpy(huge, "data: ", 6);
    yk_sse_feed(&parser, huge, sizeof(huge));
    const char *after = "\n\nevent: done\ndata: ok\n\n";
    yk_sse_feed(&parser, after, strlen(after));
    CHECK(count == 1 && !strcmp(events[0], "done") && !strcmp(datas[0], "ok"));

    // A /tts audio event with a whole Opus packet (3,828 bytes: 5,104 base64 characters) is one line, and it fits.
    count = 0;
    yk_sse_init(&parser, on_event, NULL);
    static char audio[5200];
    const char *head = "event: audio\ndata: {\"seq\":0,\"data\":\"";
    size_t at = strlen(head);
    memcpy(audio, head, at);
    memset(audio + at, 'A', 5104);
    at += 5104;
    memcpy(audio + at, "\"}\n\n", 4);
    at += 4;
    for (size_t i = 0; i < at; i += 700) yk_sse_feed(&parser, audio + i, at - i < 700 ? at - i : 700);
    CHECK(count == 1 && !strcmp(events[0], "audio") && lengths[0] == strlen("{\"seq\":0,\"data\":\"\"}") + 5104);
}

// A stream as the device reads it with a short timeout: a piece, nothing yet (NULL), the end (""), a broken link (BROKEN).
static const char BROKEN[] = "broken";
static const char *const *script;
static int reads, at_read[8];
static int64_t clock_ms;
static int64_t fake_now(void) { return clock_ms; }
static int fake_read(char *buf, int size, void *ctx) {
    (void)ctx;
    const char *piece = script[reads++];
    if (piece == BROKEN) return -1;
    if (!piece) {
        clock_ms += 200;  // the read's timeout went by
        return YK_SSE_AGAIN;
    }
    const int n = (int)strlen(piece) < size ? (int)strlen(piece) : size;
    memcpy(buf, piece, (size_t)n);
    return n;
}
static void on_timed(const char *event, const char *data, void *ctx) {
    if (count < 8) at_read[count] = reads;
    on_event(event, data, ctx);
}

static void test_sse_pump(void) {
    // A lookup's events in batches (heard, then the model's answer 1.5 s later): each reaches on_event as its read
    // returns, not all at the end (they are well under the 1 KB buffer; 10-24 they all came at once).
    static const char *const lookup[] = {
        "event: heard\ndata: {\"text\":\"鹰怎么写\"}\n\n", NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        "event: answer\ndata: {\"found\":true}\n\nevent: do", "ne\ndata: {}\n\n", ""};
    char buf[1024];
    script = lookup, reads = 0, clock_ms = 0, count = 0;
    yk_sse_init(&parser, on_timed, NULL);
    CHECK(yk_sse_pump(&parser, fake_read, NULL, buf, sizeof(buf), 30000, fake_now) == 0);
    CHECK(count == 3 && !strcmp(events[0], "heard") && !strcmp(events[1], "answer") && !strcmp(events[2], "done"));
    CHECK(at_read[0] == 1 && at_read[1] == 9 && at_read[2] == 10);  // heard right away, before the waits
    CHECK(reads == 11);

    // Nothing for the idle time: it gives up (as the client's 30 s timeout did), with what is pending dispatched.
    static const char *const stalled[200] = {"data: half"};  // the rest: NULL, nothing comes
    script = stalled, reads = 0, clock_ms = 0, count = 0;
    yk_sse_init(&parser, on_timed, NULL);
    CHECK(yk_sse_pump(&parser, fake_read, NULL, buf, sizeof(buf), 30000, fake_now) == YK_SSE_AGAIN);
    CHECK(reads == 151 && clock_ms == 30000);
    CHECK(count == 1 && !strcmp(datas[0], "half"));

    // A broken link ends it at once, waiting or not.
    static const char *const broken[] = {"event: heard\ndata: {}\n\n", NULL, BROKEN};
    script = broken, reads = 0, clock_ms = 0, count = 0;
    yk_sse_init(&parser, on_timed, NULL);
    CHECK(yk_sse_pump(&parser, fake_read, NULL, buf, sizeof(buf), 30000, fake_now) == -1);
    CHECK(count == 1 && reads == 3);
}

static void test_token(void) {
    char a[11], b[11];
    yk_token_key("yay", a);
    yk_token_key("party-play", b);
    CHECK(strlen(a) == 10 && !strncmp(a, "tk", 2) && strcmp(a, b));
    yk_token_key("yay", b);
    CHECK(!strcmp(a, b));  // the same app, the same key
    const int64_t now = 1792400000;  // 2026-10-20
    CHECK(yk_token_left_s(now, now + 900) == 900);
    CHECK(yk_token_left_s(now, now + 121) == 121 && yk_token_left_s(now, now + 120) == 0);  // about to end
    CHECK(yk_token_left_s(now, now - 5) == 0);
    CHECK(yk_token_left_s(1000, 1900) == 0);  // the clock never set: 1970
    CHECK(yk_token_left_s(now, now + 3 * 24 * 3600) == 0);  // a clock that jumped back: not trusted
}

/** The owner's language (#545): kept as said, read by its prefix; Chinese or English on screen and in the catalog. */
static void test_locale(void) {
    static const char *const ok[] = {"zh", "zh-Hans", "en", "ja", "es-419", "pt-BR", "fr"};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) CHECK(yk_locale_ok(ok[i]));
    static const char *const bad[] = {"", "x", "zh-Hans-CN", "es-4190x", "zh_Hans", "1en", "-en", "en ", "中文", "e\"n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!yk_locale_ok(bad[i]));
    CHECK(!yk_locale_ok(NULL));
    // Chinese by its prefix; everything else English (ja, es-419 and those not known yet too)
    CHECK(!yk_locale_english("zh") && !yk_locale_english("zh-Hans") && !yk_locale_english("ZH-hant"));
    CHECK(yk_locale_english("en") && yk_locale_english("ja") && yk_locale_english("es-419") && yk_locale_english("fr"));
    CHECK(!yk_locale_english("") && !yk_locale_english(NULL));  // nothing kept: the device's default, Chinese
    CHECK(!strcmp(yk_locale_catalog("zh-Hans"), "zh") && !strcmp(yk_locale_catalog("zh"), "zh"));
    CHECK(!strcmp(yk_locale_catalog("en"), "en") && !strcmp(yk_locale_catalog("ja"), "en") &&
          !strcmp(yk_locale_catalog("es-419"), "en") && !strcmp(yk_locale_catalog(NULL), "zh"));
    // Taken when it is one to keep and another than kept (zh → zh-Hans too: the same language, kept as said)
    char kept[YK_LOCALE_MAX] = "zh";
    CHECK(yk_locale_take(kept, "zh-Hans") && !strcmp(kept, "zh-Hans"));
    CHECK(!yk_locale_take(kept, "zh-Hans") && !strcmp(kept, "zh-Hans"));  // the same: nothing
    CHECK(yk_locale_take(kept, "en") && !strcmp(kept, "en"));
    CHECK(!yk_locale_take(kept, "x") && !yk_locale_take(kept, "zh-Hans-CN") && !yk_locale_take(kept, NULL) &&
          !yk_locale_take(kept, "") && !strcmp(kept, "en"));  // bad values change nothing
    CHECK(yk_locale_take(kept, "es-419") && !strcmp(kept, "es-419"));
}

int main(void) {
    test_locale();
    test_base64url();
    test_token();
    test_messages();
    test_sse();
    test_sse_pump();
    printf("yaykit_cloud host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

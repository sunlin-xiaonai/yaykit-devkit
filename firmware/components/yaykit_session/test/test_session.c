// Host tests for yk_proto: every example in packages/contracts/fixtures/device.json, the platform's own.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "yk_proto.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)
#define CHECK_CASE(cond, name)                                             \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, name); \
        }                                                                  \
    } while (0)

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) {
        free(s);
        s = NULL;
    }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double num(const cJSON *o, const char *key, double fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

/** The message as the schema reads it: hello's audio rates and the slots' lastUsed take their defaults; a slot's
 *  missing slug or version is null. */
static void normalize(cJSON *m) {
    if (strcmp(str(m, "type"), "hello")) return;
    cJSON *audio = cJSON_GetObjectItemCaseSensitive(m, "audio");
    if (!cJSON_GetObjectItemCaseSensitive(audio, "inRate")) cJSON_AddNumberToObject(audio, "inRate", 16000);
    if (!cJSON_GetObjectItemCaseSensitive(audio, "outRate")) cJSON_AddNumberToObject(audio, "outRate", 16000);
    cJSON *slot;
    cJSON_ArrayForEach(slot, cJSON_GetObjectItemCaseSensitive(m, "slots")) {
        if (!cJSON_GetObjectItemCaseSensitive(slot, "lastUsed")) cJSON_AddNumberToObject(slot, "lastUsed", 0);
        if (!cJSON_GetObjectItemCaseSensitive(slot, "slug")) cJSON_AddNullToObject(slot, "slug");
        if (!cJSON_GetObjectItemCaseSensitive(slot, "version")) cJSON_AddNullToObject(slot, "version");
    }
}

/** A screensaver object (prefs.screensaver, action.done's) as the firmware holds it; NULL when there is none. */
static const yk_proto_saver_t *saver_of(const cJSON *v, yk_proto_saver_t *out) {
    if (!v) return NULL;
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(v, "image");
    *out = (yk_proto_saver_t){.on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "on")),
                              .after_s = (int)num(v, "after", -1),
                              .duration_s = (int)num(v, "duration", -1),
                              .image_id = str(image, "id"),
                              .image_sha256 = str(image, "sha256")};
    return out;
}

/** Builds the device message the way the firmware does, from the fixture's fields. NULL when the builder refuses. */
static char *build(const cJSON *m) {
    yk_proto_saver_t saver;
    const char *type = str(m, "type");
    if (!strcmp(type, "hello")) {
        const cJSON *practice = cJSON_GetObjectItemCaseSensitive(m, "practice");
        yk_proto_hello_t h = {.fw = str(m, "fw"), .board = str(m, "board"), .locale = str(m, "locale"),
                              .tz = str(m, "tz"), .mode = str(m, "mode"), .pair = str(m, "pair"),
                              .practice_language = str(practice, "language"), .practice_level = str(practice, "level")};
        const cJSON *battery = cJSON_GetObjectItemCaseSensitive(m, "battery"), *audio = cJSON_GetObjectItemCaseSensitive(m, "audio");
        h.battery_pct = (int)num(battery, "pct", -1);
        h.charging = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(battery, "charging"));
        h.rssi = (int)num(cJSON_GetObjectItemCaseSensitive(m, "wifi"), "rssi", 1);
        h.out_rate = (int)num(audio, "outRate", 16000);
        const char *codec = str(audio, "codec");
        if (!codec || strcmp(codec, "opus")) return NULL;  // the firmware only speaks Opus
        const cJSON *slot;
        cJSON_ArrayForEach(slot, cJSON_GetObjectItemCaseSensitive(m, "slots")) {
            yk_proto_slot_t *s = &h.slots[h.slot_count++];
            s->slot = (int)num(slot, "slot", -1);
            s->slug = str(slot, "slug");
            s->version = str(slot, "version");
            s->state = str(slot, "state");
            s->last_used_ms = (int64_t)num(slot, "lastUsed", 0);
        }
        const cJSON *prefs = cJSON_GetObjectItemCaseSensitive(m, "prefs"), *caps = cJSON_GetObjectItemCaseSensitive(m, "caps");
        if (prefs) {
            h.has_prefs = true;
            h.volume = (int)num(prefs, "volume", -1);
            h.brightness = (int)num(prefs, "brightness", -1);
            h.screen_off_s = (int)num(prefs, "screenOff", -1);
            h.saver = saver_of(cJSON_GetObjectItemCaseSensitive(prefs, "screensaver"), &saver);
        }
        const char *cap_list[33];
        if (caps) {
            const cJSON *cap;
            cJSON_ArrayForEach(cap, caps) if (h.cap_count < 33) cap_list[h.cap_count++] = cJSON_IsString(cap) ? cap->valuestring : NULL;
            h.caps = cap_list;
        }
        return yk_proto_hello(&h);
    }
    if (!strcmp(type, "listen.start"))
        return yk_proto_listen_start_ex(str(m, "turn"), (int)num(m, "preroll", -1), str(m, "side"),
                                        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "held")));
    if (!strcmp(type, "pair.set")) return yk_proto_pair_set(str(m, "pair"));
    if (!strcmp(type, "translate.text")) return yk_proto_translate_text(str(m, "turn"), str(m, "text"), str(m, "from"));
    if (!strcmp(type, "practice.open")) return yk_proto_practice_open(str(m, "why"));
    if (!strcmp(type, "practice.set")) return yk_proto_practice_set(str(m, "language"), str(m, "level"));
    if (!strcmp(type, "practice.next")) return yk_proto_practice_next();
    if (!strcmp(type, "practice.end")) return yk_proto_practice_end(str(m, "why"));
    if (!strcmp(type, "app.records")) {
        // As Yay passes them on: each record as the outbox keeps it (yk_record_format, with "app").
        const char *records[YK_PROTO_RECORDS_MAX + 1];
        int n = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(m, "records")) {
            if (n > YK_PROTO_RECORDS_MAX) break;
            cJSON *kept = cJSON_Duplicate(r, 1);
            cJSON_AddStringToObject(kept, "app", str(m, "slug") ? str(m, "slug") : "");
            records[n++] = cJSON_PrintUnformatted(kept);
            cJSON_Delete(kept);
        }
        char *out = yk_proto_app_records(str(m, "slug"), records, n);
        for (int i = 0; i < n; i++) cJSON_free((void *)records[i]);
        return out;
    }
    if (!strcmp(type, "action.done")) {
        const bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "ok"));
        // kind is only sent again for an install from an earlier session, value only for a setting: never both.
        if (str(m, "kind")) return yk_proto_action_done_kind(str(m, "id"), ok, str(m, "error"), str(m, "kind"));
        // screensaver.set's: the screensaver Yay uses after it
        const cJSON *uses = cJSON_GetObjectItemCaseSensitive(m, "screensaver");
        if (uses)
            return yk_proto_action_done_saver(str(m, "id"), ok, str(m, "error"), (int)num(m, "value", -1),
                                              saver_of(uses, &saver));
        return yk_proto_action_done_value(str(m, "id"), ok, str(m, "error"), (int)num(m, "value", -1));
    }
    if (!strcmp(type, "prefs"))
        return yk_proto_prefs_saver((int)num(m, "volume", -1), (int)num(m, "brightness", -1), (int)num(m, "screenOff", -1),
                                    saver_of(cJSON_GetObjectItemCaseSensitive(m, "screensaver"), &saver));
    if (!strcmp(type, "app.result")) {
        char *data = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(m, "data"));
        char *out = yk_proto_app_result(str(m, "slug"), data);
        cJSON_free(data);
        return out;
    }
    if (!strcmp(type, "status")) {
        const cJSON *battery = cJSON_GetObjectItemCaseSensitive(m, "battery");
        return yk_proto_status((int)num(battery, "pct", -1), cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(battery, "charging")),
                               (int)num(cJSON_GetObjectItemCaseSensitive(m, "wifi"), "rssi", 1),
                               (uint32_t)num(m, "heapMin", 0));
    }
    if (!strcmp(type, "auth")) return yk_proto_auth(str(m, "token"));
    return yk_proto_turn_message(type, str(m, "turn"));  // listen.stop, listen.cancel, playback.stop (or refused)
}

static void test_device(const cJSON *cases, bool valid) {
    const cJSON *c;
    cJSON_ArrayForEach(c, cases) {
        if (strcmp(str(c, "from"), "device")) continue;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(c, "message");
        const char *name = str(c, "name") ? str(c, "name") : str(m, "type");
        char *built = build(m);
        if (!valid) {
            CHECK_CASE(built == NULL, name);
        } else {
            CHECK_CASE(built != NULL, name);
            cJSON *want = cJSON_Duplicate(m, 1), *got = built ? cJSON_Parse(built) : NULL;
            normalize(want);
            CHECK_CASE(got && cJSON_Compare(got, want, 1), name);
            if (got && !cJSON_Compare(got, want, 1)) {
                char *w = cJSON_PrintUnformatted(want);
                printf("  want %s\n  got  %s\n", w, built);
                cJSON_free(w);
            }
            cJSON_Delete(want);
            cJSON_Delete(got);
        }
        cJSON_free(built);
    }
}

static void test_server(const cJSON *cases, bool valid) {
    const cJSON *c;
    cJSON_ArrayForEach(c, cases) {
        if (strcmp(str(c, "from"), "server")) continue;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(c, "message");
        const char *name = str(c, "name") ? str(c, "name") : str(m, "type");
        char *json = cJSON_PrintUnformatted(m);
        yk_srv_msg_t msg;
        const yk_srv_type_t t = yk_proto_parse(json, strlen(json), &msg);
        // Device policy (§13.3): what a newer server adds is taken, not dropped; the schema calls an unknown error
        // code invalid, the device still ends the turn with it.
        const bool lenient = !strcmp(name, "an unknown error code");
        // Older servers sent brightness "auto": read, and answered UNSUPPORTED (no light sensor).
        const bool auto_brightness = !strcmp(name, "automatic brightness (no light sensor)");
        if (lenient) {
            CHECK_CASE(t == YK_SRV_ERROR && !strcmp(msg.code, "OOPS"), name);
        } else if (auto_brightness) {
            CHECK_CASE(t == YK_SRV_ACTION && msg.kind == YK_ACT_BRIGHTNESS && msg.level == -1 && msg.step == 0, name);
        } else if (valid) {
            CHECK_CASE(t != YK_SRV_INVALID && !strcmp(yk_proto_type_name(t), str(m, "type")), name);
            CHECK_CASE(msg.text != NULL, name);
            const char *turn = str(m, "turn");
            CHECK_CASE(!turn || !strcmp(msg.turn, turn), name);
            if (t == YK_SRV_HELLO_OK) {  // the account's language as the fixture has it (#545), "" without one
                const char *loc = str(cJSON_GetObjectItemCaseSensitive(m, "owner"), "locale");
                CHECK_CASE(!strcmp(msg.owner_locale, loc ? loc : ""), name);
            }
        } else {
            CHECK_CASE(t == YK_SRV_INVALID, name);
        }
        yk_proto_free(&msg);
        cJSON_free(json);
    }
}

static yk_srv_type_t parse(const char *json, yk_srv_msg_t *m) { return yk_proto_parse(json, strlen(json), m); }

static void test_fields(void) {
    yk_srv_msg_t m;
    CHECK(parse("{\"type\":\"hello.ok\",\"session\":\"s1\",\"time\":1792310400000,\"owner\":{\"nickname\":\"小鱼\"},"
                "\"credits\":{\"remaining\":846,\"resetAt\":\"2026-10-18T16:00:00.000Z\"},\"dot\":true}", &m) == YK_SRV_HELLO_OK);
    CHECK(m.time_ms == 1792310400000LL && !strcmp(m.nickname, "小鱼") && m.has_credits && m.credits_remaining == 846 &&
          m.dot);
    CHECK(m.next_reminder == -1);  // no nextReminderAt (platforms before it): not known, never "none"
    yk_proto_free(&m);
    CHECK(!m.owner_locale[0]);  // no owner.locale (servers before #545): none
    // nextReminderAt (N): a time, null (none), or anything else: not known, the hello.ok kept all the same
    static const struct {
        const char *json;
        int8_t want;
        int64_t ms;
    } reminders[] = {
        {"1792330200000", 1, 1792330200000LL}, {"null", 0, 0}, {"0", -1, 0}, {"-5", -1, 0}, {"1.5", -1, 0},
        {"\"soon\"", -1, 0}, {"{}", -1, 0},
    };
    for (size_t i = 0; i < sizeof(reminders) / sizeof(reminders[0]); i++) {
        char hello[400];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello.ok\",\"session\":\"s1\",\"time\":1792310400000,\"owner\":{\"nickname\":\"小鱼\"},"
                 "\"credits\":{\"remaining\":846,\"resetAt\":\"2026-10-18T16:00:00.000Z\"},\"dot\":false,"
                 "\"nextReminderAt\":%s}",
                 reminders[i].json);
        CHECK(parse(hello, &m) == YK_SRV_HELLO_OK);
        CHECK(m.next_reminder == reminders[i].want && (reminders[i].want != 1 || m.next_reminder_ms == reminders[i].ms));
        yk_proto_free(&m);
    }
    // owner.locale (#545): kept as said; a value a device would not keep is left out, the hello.ok kept all the same
    static const struct {
        const char *json, *want;
    } locales[] = {
        {"\"en\"", "en"},       {"\"zh-Hans\"", "zh-Hans"}, {"\"ja\"", "ja"},   {"\"es-419\"", "es-419"},
        {"\"x\"", ""},          {"\"zh-Hans-CN\"", ""},    {"\"\"", ""},      {"5", ""},
        {"null", ""},             {"{\"a\":1}", ""},          {"\"e n\"", ""},
    };
    for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]); i++) {
        char hello[400];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello.ok\",\"session\":\"s1\",\"time\":1792310400000,\"owner\":{\"nickname\":\"小鱼\","
                 "\"locale\":%s},\"credits\":{\"remaining\":846,\"resetAt\":\"2026-10-18T16:00:00.000Z\"},\"dot\":false}",
                 locales[i].json);
        CHECK(parse(hello, &m) == YK_SRV_HELLO_OK);
        CHECK(!strcmp(m.owner_locale, locales[i].want) && !strcmp(m.nickname, "小鱼"));
        yk_proto_free(&m);
    }
    CHECK(parse("{\"type\":\"stt.final\",\"turn\":\"t7f3a9c21d0e4b55\",\"text\":\"\"}", &m) == YK_SRV_STT_FINAL);
    CHECK(m.text && m.text[0] == 0);  // not understood
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"reply.end\",\"turn\":\"t7f3a9c21d0e4b55\",\"credits\":{\"used\":3.1,\"remaining\":0},"
                "\"muted\":\"credits\"}", &m) == YK_SRV_REPLY_END);
    CHECK(m.has_credits && m.credits_used > 3.09 && m.credits_used < 3.11 && m.credits_remaining == 0 &&
          !strcmp(m.muted, "credits"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"reply.line\",\"turn\":\"s-k2m8q4w9z1x7c3\",\"line\":3,\"text\":\"好\"}", &m) == YK_SRV_REPLY_LINE);
    CHECK(m.line == 3 && yk_proto_server_turn(m.turn) && !strcmp(m.text, "好"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a1\",\"kind\":\"app.open\",\"slug\":\"language-buddy\","
                "\"params\":{\"level\":\"beginner\"}}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_APP_OPEN && !strcmp(m.slug, "language-buddy") && !strcmp(m.params, "{\"level\":\"beginner\"}"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a4\",\"kind\":\"brightness.set\",\"level\":\"auto\"}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_BRIGHTNESS && m.level == -1 && m.step == 0);
    yk_proto_free(&m);
    // Internal commands: a level, a step, mute, the screen-off time.
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"volume.set\",\"level\":6}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_VOLUME && m.level == 6 && m.step == 0 && m.mute == -1);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"volume.set\",\"step\":-2}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_VOLUME && m.level == -1 && m.step == -2 && m.mute == -1);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"volume.set\",\"mute\":false}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_VOLUME && m.level == -1 && m.step == 0 && m.mute == 0);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"brightness.set\",\"step\":3}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_BRIGHTNESS && m.level == -1 && m.step == 3);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"screen_off.set\",\"seconds\":0}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_SCREEN_OFF && m.seconds == 0);
    yk_proto_free(&m);
    // Back to Yay, asked from the phone: a command's id (a UUID) is the action's.
    CHECK(parse("{\"type\":\"action\",\"id\":\"7d4c2f9e-3b1a-4c8d-9e2f-1a3b5c7d9e0f\",\"kind\":\"app.home\"}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_APP_HOME && !strcmp(m.action_id, "7d4c2f9e-3b1a-4c8d-9e2f-1a3b5c7d9e0f"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"volume.set\",\"level\":\"auto\"}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"volume.set\",\"mute\":1}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"volume.set\",\"step\":1,\"mute\":true}", &m) == YK_SRV_INVALID);
    // A newer kind with fields of its own is still answered.
    CHECK(parse("{\"type\":\"action\",\"id\":\"a5\",\"kind\":\"night.set\",\"level\":\"dim\"}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_UNKNOWN);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"ui.state\",\"state\":\"working\",\"label\":\"在看邮件\",\"progress\":0.33,\"taskId\":\"t2\"}", &m) ==
          YK_SRV_UI_STATE);
    CHECK(!strcmp(m.state, "working") && m.progress > 0.32f && m.progress < 0.34f && !strcmp(m.task_id, "t2"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"ui.state\",\"state\":\"done\",\"label\":\"做好了\",\"progress\":null,\"taskId\":null}", &m) ==
          YK_SRV_UI_STATE);
    CHECK(m.progress < 0 && m.task_id[0] == 0);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"error\",\"turn\":\"t7f3a9c21d0e4b55\",\"code\":\"RATE_LIMITED\",\"retryAfter\":30}", &m) ==
          YK_SRV_ERROR);
    CHECK(!strcmp(m.code, "RATE_LIMITED") && m.retry_after == 30);
    yk_proto_free(&m);
    // Not messages at all, or broken ones.
    CHECK(parse("not json", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"reply.line\",\"turn\":\"t7f3a9c21d0e4b55\",\"line\":256,\"text\":\"x\"}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"reply.line\",\"turn\":\"t7f3a9c21d0e4b55\",\"line\":0,\"text\":\"\"}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"ui.state\",\"state\":\"working\",\"label\":\"x\",\"progress\":0.5}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"hello.ok\",\"session\":\"s\",\"time\":1,\"owner\":{\"nickname\":\"\"},"
                "\"credits\":{\"remaining\":1,\"resetAt\":\"2026-10-18 16:00\"},\"dot\":false}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"session.reauth\"}", &m) == YK_SRV_REAUTH);
    yk_proto_free(&m);
    // A newer server: a new muted value reads as speech, a new action kind is still answered.
    CHECK(parse("{\"type\":\"reply.end\",\"turn\":\"t7f3a9c21d0e4b55\",\"credits\":{\"used\":1,\"remaining\":5},"
                "\"muted\":\"quiet-hours\"}", &m) == YK_SRV_REPLY_END);
    CHECK(!strcmp(m.muted, "speech"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a7\",\"kind\":\"camera.snap\"}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_UNKNOWN && !strcmp(m.action_id, "a7"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"kind\":\"camera.snap\"}", &m) == YK_SRV_INVALID);  // no id: nothing to answer
    CHECK(parse("{\"type\":\"error\",\"turn\":\"t7f3a9c21d0e4b55\",\"code\":\"\"}", &m) == YK_SRV_INVALID);
}

static void test_frames(const cJSON *frames) {
    const cJSON *c;
    cJSON_ArrayForEach(c, frames) {
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(c, "frame");
        const char *hex = str(f, "packet"), *want = str(c, "bytes");
        uint8_t packet[64], out[80];
        size_t n = strlen(hex) / 2;
        for (size_t i = 0; i < n; i++) sscanf(hex + 2 * i, "%2hhx", &packet[i]);
        const size_t len = yk_proto_frame(out, (int)num(f, "stream", 0), (int)num(f, "line", 0), (int)num(f, "seq", 0), packet, n);
        char got[200] = "";
        for (size_t i = 0; i < len; i++) sprintf(got + 2 * i, "%02x", out[i]);
        CHECK_CASE(!strcmp(got, want), str(c, "name"));
        int stream, line, seq;
        const uint8_t *p;
        size_t bytes;
        CHECK(yk_proto_unframe(out, len, &stream, &line, &seq, &p, &bytes) && stream == (int)num(f, "stream", 0) &&
              line == (int)num(f, "line", 0) && seq == (int)num(f, "seq", 0) && bytes == n && !memcmp(p, packet, n));
    }
    const uint8_t bad_version[] = {2, 1, 0, 0, 9}, bad_stream[] = {1, 3, 0, 0, 9}, empty[] = {1, 1, 0, 0};
    int s, l, q;
    const uint8_t *p;
    size_t b;
    CHECK(!yk_proto_unframe(bad_version, 5, &s, &l, &q, &p, &b));
    CHECK(!yk_proto_unframe(bad_stream, 5, &s, &l, &q, &p, &b));
    CHECK(!yk_proto_unframe(empty, 4, &s, &l, &q, &p, &b));
}

/** The modes (extension-platform.md P1) and the records Yay carries (P19), beyond the fixtures. */
static void test_modes(void) {
    CHECK(yk_proto_pair_ok("zh-ja") && !yk_proto_pair_ok("zh-zh") && !yk_proto_pair_ok("zh-ko") &&
          !yk_proto_pair_ok("zhja") && !yk_proto_pair_ok("zh-jap") && !yk_proto_pair_ok(NULL));
    yk_proto_hello_t h = {.fw = "language-buddy/0.1.0", .board = "tikpal-r1", .locale = "zh", .tz = "Asia/Shanghai",
                          .battery_pct = 80, .rssi = -50, .out_rate = 16000, .mode = "practice",
                          .practice_language = "en"};
    CHECK(yk_proto_hello(&h) == NULL);  // a language without a level
    h.practice_level = "everyday";
    char *s = yk_proto_hello(&h);
    CHECK(s && strstr(s, "\"mode\":\"practice\",\"practice\":{\"language\":\"en\",\"level\":\"everyday\"}"));
    cJSON_free(s);
    h.mode = "karaoke";
    CHECK(yk_proto_hello(&h) == NULL);
    h.mode = NULL, h.practice_language = h.practice_level = NULL;
    s = yk_proto_hello(&h);
    CHECK(s && !strstr(s, "mode"));  // chat, as before the modes
    cJSON_free(s);
    CHECK(yk_proto_listen_start("t7f3a9c21d0e4b55", 300, "left") == NULL);
    s = yk_proto_listen_start_ex("t7f3a9c21d0e4b55", 300, NULL, true);  // recorded while connecting (#834's example)
    CHECK(s && !strcmp(s, "{\"type\":\"listen.start\",\"turn\":\"t7f3a9c21d0e4b55\",\"preroll\":300,\"held\":true}"));
    cJSON_free(s);
    s = yk_proto_listen_start("t7f3a9c21d0e4b55", 300, NULL);
    CHECK(s && !strstr(s, "held"));  // live, as before
    cJSON_free(s);
    CHECK(yk_proto_translate_text("t7f3a9c21d0e4b55", "", "zh") == NULL);
    CHECK(yk_proto_practice_end("bored") == NULL);
    s = yk_proto_practice_end("home");  // the phone asked it back to Yay (app.home)
    CHECK(s && strstr(s, "\"why\":\"home\""));
    cJSON_free(s);

    // Records as the outbox keeps them: "app" is left behind, the rest passes as it is.
    const char *rec[2] = {"{\"app\":\"breathe\",\"id\":\"yk_0a1b2c3d4e5f:breathe:7\",\"kind\":\"breathe.session\","
                          "\"occurredAt\":\"2026-10-19T08:53:20.123Z\",\"clockTrusted\":true,\"data\":{\"minutes\":5}}",
                          "{\"app\":\"breathe\",\"id\":\"yk_0a1b2c3d4e5f:breathe:8\",\"kind\":\"breathe.session\","
                          "\"occurredAt\":\"1970-01-01T00:00:12.000Z\",\"clockTrusted\":false,\"data\":{}}"};
    s = yk_proto_app_records("breathe", rec, 2);
    CHECK(s && !strcmp(s, "{\"type\":\"app.records\",\"slug\":\"breathe\",\"records\":[{\"id\":\"yk_0a1b2c3d4e5f:breathe:7\","
                          "\"kind\":\"breathe.session\",\"occurredAt\":\"2026-10-19T08:53:20.123Z\",\"clockTrusted\":true,"
                          "\"data\":{\"minutes\":5}},{\"id\":\"yk_0a1b2c3d4e5f:breathe:8\",\"kind\":\"breathe.session\","
                          "\"occurredAt\":\"1970-01-01T00:00:12.000Z\",\"clockTrusted\":false,\"data\":{}}]}"));
    cJSON_free(s);
    const char *bad[] = {"{\"id\":\"a b c d\",\"kind\":\"breathe.session\",\"occurredAt\":\"2026-10-19T08:53:20Z\",\"data\":{}}",
                         "{\"id\":\"abcd\",\"kind\":\"Breathe\",\"occurredAt\":\"2026-10-19T08:53:20Z\",\"data\":{}}",
                         "{\"id\":\"abcd\",\"kind\":\"breathe.session\",\"occurredAt\":\"2026-10-19 08:53\",\"data\":{}}",
                         "{\"id\":\"abcd\",\"kind\":\"breathe.session\",\"occurredAt\":\"2026-10-19T08:53:20Z\",\"data\":[]}",
                         "not json"};
    for (int i = 0; i < 5; i++) CHECK(yk_proto_app_records("breathe", &bad[i], 1) == NULL);
    CHECK(yk_proto_app_records("breathe", rec, 0) == NULL && yk_proto_app_records("breathe", rec, 51) == NULL);

    yk_srv_msg_t m;
    CHECK(parse("{\"type\":\"hello.ok\",\"session\":\"s1\",\"time\":1792310400000,\"owner\":{\"nickname\":\"\"},\"credits\":"
                "{\"remaining\":846,\"resetAt\":\"2026-10-18T16:00:00.000Z\"},\"dot\":false,\"practice\":{\"turnPoints\":11.5}}",
                &m) == YK_SRV_HELLO_OK);
    CHECK(m.has_practice && m.turn_points == 11.5 && m.week_days == -1 && m.week_turns == -1);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"practice.turn\",\"turn\":\"t7f3a9c21d0e4b55\",\"verdict\":\"fix\",\"fix\":{\"said\":\"たこ焼きが二つください\","
                "\"better\":\"たこ焼きを二つください\",\"spans\":[[4,5],[6,8]],\"note\":\"要东西用「を」\"},\"hint\":\"お茶をください。\","
                "\"newWords\":[{\"word\":\"お茶\",\"meaning\":\"茶\"}]}", &m) == YK_SRV_PRACTICE_TURN);
    CHECK(!strcmp(m.verdict, "fix") && !strcmp(m.better, "たこ焼きを二つください") && !strcmp(m.note, "要东西用「を」") &&
          m.span_count == 2 && m.spans[1][0] == 6 && m.spans[1][1] == 8 && !strcmp(m.text, "お茶をください。") &&
          m.word_count == 1 && !strcmp(m.words[0], "お茶") && !strcmp(m.meanings[0], "茶"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"practice.turn\",\"turn\":\"t7f3a9c21d0e4b55\",\"verdict\":\"good\"}", &m) == YK_SRV_PRACTICE_TURN);
    CHECK(!m.said && !m.better && m.span_count == 0 && m.word_count == 0 && m.text && !m.text[0]);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"practice.turn\",\"turn\":\"t7f3a9c21d0e4b55\",\"verdict\":\"fix\",\"fix\":{\"said\":\"a\",\"better\":\"b\","
                "\"spans\":[[0]],\"note\":\"\"}}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"practice.turn\",\"turn\":\"t7f3a9c21d0e4b55\",\"verdict\":\"good\",\"newWords\":[{\"word\":\"a\","
                "\"meaning\":\"b\"},{\"word\":\"c\",\"meaning\":\"d\"},{\"word\":\"e\",\"meaning\":\"f\"}]}", &m) == YK_SRV_INVALID);
    // reply.line: 120 characters for a translated sentence; a kind or language this firmware does not know is not
    // refused (a subtitle; no language).
    char line[400];
    snprintf(line, sizeof(line), "{\"type\":\"reply.line\",\"turn\":\"t7f3a9c21d0e4b55\",\"line\":0,\"text\":\"%0120d\","
             "\"kind\":\"aside\",\"lang\":\"ko\"}", 0);
    CHECK(parse(line, &m) == YK_SRV_REPLY_LINE && !m.fix_line && !m.lang[0] && strlen(m.text) == 120);
    yk_proto_free(&m);
    snprintf(line, sizeof(line), "{\"type\":\"reply.line\",\"turn\":\"t7f3a9c21d0e4b55\",\"line\":0,\"text\":\"%0121d\"}", 0);
    CHECK(parse(line, &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"reply.line\",\"turn\":\"t7f3a9c21d0e4b55\",\"line\":1,\"kind\":\"fix\",\"lang\":\"en\",\"text\":\"Two, please.\"}",
                &m) == YK_SRV_REPLY_LINE && m.fix_line && !strcmp(m.lang, "en"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"stt.final\",\"turn\":\"t7f3a9c21d0e4b55\",\"text\":\"안녕\",\"lang\":\"other\"}", &m) == YK_SRV_STT_FINAL &&
          !strcmp(m.lang, "other"));
    yk_proto_free(&m);
    // app.records.ok: the batch's ids the platform is done with.
    CHECK(parse("{\"type\":\"app.records.ok\",\"slug\":\"breathe\",\"ids\":[\"yk_0a1b2c3d4e5f:breathe:7\","
                "\"yk_0a1b2c3d4e5f:breathe:8\"]}", &m) == YK_SRV_APP_RECORDS_OK);
    CHECK(!strcmp(m.slug, "breathe") && m.id_count == 2 && !strcmp(m.ids[1], "yk_0a1b2c3d4e5f:breathe:8"));
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"app.records.ok\",\"slug\":\"breathe\",\"ids\":[]}", &m) == YK_SRV_APP_RECORDS_OK &&
          m.id_count == 0);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"app.records.ok\",\"slug\":\"breathe\",\"ids\":[\"a b\"]}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"app.records.ok\",\"slug\":\"Breathe\",\"ids\":[\"abcd\"]}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"app.records.ok\",\"slug\":\"breathe\"}", &m) == YK_SRV_INVALID);
}

/** practice.turn in two parts (the contract's fixtures): merged by turn, the later fields added, none replaced. */
static void test_practice_parts(const cJSON *valid) {
    const char *first = NULL, *second = NULL;
    const cJSON *c;
    cJSON_ArrayForEach(c, valid) {
        const char *name = str(c, "name");
        if (!name || strncmp(name, "practice: in two parts", 22)) continue;
        char *json = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(c, "message"));
        if (strstr(name, "1 of 2")) first = json;
        else second = json;
    }
    CHECK(first && second);
    if (!first || !second) return;
    yk_srv_msg_t a, b;
    CHECK(parse(first, &a) == YK_SRV_PRACTICE_TURN && parse(second, &b) == YK_SRV_PRACTICE_TURN);
    CHECK(yk_proto_practice_merge(&a, &b));
    CHECK(!strcmp(a.verdict, "fix") && !strcmp(a.better, "I went to school yesterday.") && a.span_count == 1 &&
          a.spans[0][0] == 2 && a.spans[0][1] == 6 && !strcmp(a.note, "过去的事用过去式") && a.word_count == 1 &&
          !strcmp(a.words[0], "photosynthesis") && !strcmp(a.text, "I learned about plants."));
    // The other way round (the hint first): nothing that came first is replaced.
    yk_srv_msg_t x, y;
    parse(second, &x);
    parse(first, &y);
    CHECK(yk_proto_practice_merge(&x, &y) && !strcmp(x.text, "I learned about plants.") && x.better && x.word_count == 1);
    // Another turn's part is not merged; it is freed all the same.
    yk_srv_msg_t z;
    CHECK(parse("{\"type\":\"practice.turn\",\"turn\":\"aaaaaaaaaaaaaaaa\",\"verdict\":\"good\",\"hint\":\"Hi\"}", &z) ==
          YK_SRV_PRACTICE_TURN);
    CHECK(!yk_proto_practice_merge(&a, &z) && !strcmp(a.text, "I learned about plants.") && !z.text);
    yk_proto_free(&a);
    yk_proto_free(&x);
    cJSON_free((void *)first);
    cJSON_free((void *)second);
}

static void test_saver(void) {
    yk_srv_msg_t m;
    // Everything, a catalog image: what to change, and the image to fetch.
    CHECK(parse("{\"type\":\"action\",\"id\":\"a11\",\"kind\":\"screensaver.set\",\"on\":true,\"after\":60,"
                "\"duration\":600,\"image\":{\"id\":\"default-03\",\"url\":\"https://x.dev/1/default-03-466.jpg\","
                "\"sha256\":\"3a9f1c7e5b2d8a4f0c6e9b1d7a3f5c8e2b0d4a6f9c1e7b3d5a8f0c2e6b4d9a1f\",\"size\":61210}}",
                &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_SAVER_SET && m.saver_on == 1 && m.saver_after == 60 && m.saver_duration == 600 &&
          !strcmp(m.image_id, "default-03") && !strcmp(m.url, "https://x.dev/1/default-03-466.jpg") && m.size == 61210 &&
          !strcmp(m.sha256, "3a9f1c7e5b2d8a4f0c6e9b1d7a3f5c8e2b0d4a6f9c1e7b3d5a8f0c2e6b4d9a1f"));
    yk_proto_free(&m);
    // Only the times: nothing said of on or the image.
    CHECK(parse("{\"type\":\"action\",\"id\":\"a14\",\"kind\":\"screensaver.set\",\"after\":300,\"duration\":1800}",
                &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_SAVER_SET && m.saver_on == -1 && m.saver_after == 300 && m.saver_duration == 1800 &&
          !m.image_id[0] && !m.url);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a15\",\"kind\":\"screensaver.set\",\"on\":false}", &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_SAVER_SET && m.saver_on == 0 && !m.saver_after && !m.saver_duration);
    yk_proto_free(&m);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a13\",\"kind\":\"screensaver.preview\",\"url\":\"https://x.dev/p.jpg\","
                "\"sha256\":\"3a9f1c7e5b2d8a4f0c6e9b1d7a3f5c8e2b0d4a6f9c1e7b3d5a8f0c2e6b4d9a1f\",\"size\":98304,\"seconds\":10}",
                &m) == YK_SRV_ACTION);
    CHECK(m.kind == YK_ACT_SAVER_PREVIEW && m.seconds == 10 && m.size == 98304 && !strcmp(m.url, "https://x.dev/p.jpg"));
    yk_proto_free(&m);
    // A photo's id in capitals is still a UUID; an upper-case sha256, an on that is not a boolean, are not.
    CHECK(yk_proto_saver_image_ok("5D0C1B7E-8A3F-4E2B-9C61-2F7A4B8D3E90") && yk_proto_saver_image_ok("default-07"));
    CHECK(!yk_proto_saver_image_ok("default-7") && !yk_proto_saver_image_ok("default-ab") && !yk_proto_saver_image_ok(NULL) &&
          !yk_proto_saver_image_ok("5d0c1b7e-8a3f-4e2b-9c61-2f7a4b8d3e9") && !yk_proto_saver_image_ok("5d0c1b7e08a3f-4e2b-9c61-2f7a4b8d3e90"));
    CHECK(parse("{\"type\":\"action\",\"id\":\"a13\",\"kind\":\"screensaver.preview\",\"url\":\"https://x.dev/p.jpg\","
                "\"sha256\":\"3A9F1C7E5B2D8A4F0C6E9B1D7A3F5C8E2B0D4A6F9C1E7B3D5A8F0C2E6B4D9A1F\",\"size\":1,\"seconds\":10}",
                &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a15\",\"kind\":\"screensaver.set\",\"on\":1}", &m) == YK_SRV_INVALID);
    CHECK(parse("{\"type\":\"action\",\"id\":\"a15\",\"kind\":\"screensaver.preview\",\"url\":\"https://x.dev/p.jpg\","
                "\"sha256\":\"3a9f1c7e5b2d8a4f0c6e9b1d7a3f5c8e2b0d4a6f9c1e7b3d5a8f0c2e6b4d9a1f\",\"size\":0,\"seconds\":10}",
                &m) == YK_SRV_INVALID);
    // What Yay says it uses: a hello without the screensaver stays as before; a broken one is refused.
    const yk_proto_saver_t uses = {.on = true, .after_s = 60, .duration_s = 600, .image_id = "default-07",
                                   .image_sha256 = "834370e26ca77ce574e970b30ace0a6ce67a475061196bd389e0fbc7f04a7129"};
    char *p = yk_proto_prefs_saver(6, 8, 30, &uses);
    CHECK(p && strstr(p, "\"screensaver\":{\"on\":true,\"after\":60,\"duration\":600,\"image\":{\"id\":\"default-07\""));
    cJSON_free(p);
    p = yk_proto_prefs(6, 8, 30);
    CHECK(p && !strstr(p, "screensaver"));
    cJSON_free(p);
    yk_proto_saver_t bad = uses;
    bad.image_sha256 = "834370E26CA77CE574E970B30ACE0A6CE67A475061196BD389E0FBC7F04A7129";
    CHECK(!yk_proto_prefs_saver(6, 8, 30, &bad) && !yk_proto_action_done_saver("a1", true, NULL, -1, &bad));
    bad = uses;
    bad.after_s = 10;
    CHECK(!yk_proto_prefs_saver(6, 8, 30, &bad));
    p = yk_proto_action_done_saver("a1", false, "BAD_IMAGE", -1, &uses);
    CHECK(p && strstr(p, "\"error\":\"BAD_IMAGE\"") && strstr(p, "\"screensaver\":{") && !strstr(p, "value"));
    cJSON_free(p);
    // screen_off.set while the screensaver is on: the new time before it, and the screensaver as it is now.
    p = yk_proto_action_done_saver("a5", true, NULL, 300, &uses);
    CHECK(p && strstr(p, "\"value\":300") && strstr(p, "\"screensaver\":{\"on\":true"));
    cJSON_free(p);
    p = yk_proto_action_done_saver("a1", false, "BUSY", -1, NULL);  // a preview's: no screensaver
    CHECK(p && !strstr(p, "screensaver"));
    cJSON_free(p);
}

static void test_turns(void) {
    uint8_t r[16];
    char t[YK_PROTO_TURN_LEN + 1];
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 16; i++) r[i] = (uint8_t)(round * 37 + i * 101);
        yk_proto_new_turn(t, r);
        CHECK(yk_proto_turn_ok(t) && !yk_proto_server_turn(t));
    }
    CHECK(yk_proto_turn_ok("s-k2m8q4w9z1x7c3") && yk_proto_server_turn("s-k2m8q4w9z1x7c3"));
    CHECK(!yk_proto_turn_ok("t7f3a9c21d0e4b5") && !yk_proto_turn_ok("t7f3a9c21d0e4b5!") && !yk_proto_turn_ok(NULL));
    CHECK(yk_proto_slug_ok("language-buddy") && !yk_proto_slug_ok("Yay") && !yk_proto_slug_ok("a") &&
          !yk_proto_slug_ok("1abc"));
}

static void test_backoff(void) {
    static const int steps[] = {1000, 2000, 4000, 8000, 15000, 30000, 30000, 30000};
    for (int a = 0; a < 8; a++) {
        int lo = 1 << 30, hi = 0;
        for (uint32_t r = 0; r < 4000; r += 7) {
            const int ms = yk_proto_backoff_ms(a, r * 2654435761u);
            lo = ms < lo ? ms : lo;
            hi = ms > hi ? ms : hi;
        }
        // Within ±25 % of the step, and spread over most of it (not one fixed value).
        CHECK(lo >= steps[a] * 3 / 4 && hi <= steps[a] * 5 / 4);
        CHECK(hi - lo >= steps[a] * 2 / 5);
    }
    CHECK(yk_proto_backoff_ms(-1, 0) == 750 && yk_proto_backoff_ms(1000, 0) == 22500);
}

int main(int argc, char **argv) {
    char *text = argc > 1 ? read_file(argv[1]) : NULL;
    cJSON *fixtures = text ? cJSON_Parse(text) : NULL;
    CHECK(fixtures != NULL);
    if (fixtures) {
        test_device(cJSON_GetObjectItemCaseSensitive(fixtures, "valid"), true);
        test_device(cJSON_GetObjectItemCaseSensitive(fixtures, "invalid"), false);
        test_server(cJSON_GetObjectItemCaseSensitive(fixtures, "valid"), true);
        test_server(cJSON_GetObjectItemCaseSensitive(fixtures, "invalid"), false);
        test_frames(cJSON_GetObjectItemCaseSensitive(fixtures, "frames"));
        test_practice_parts(cJSON_GetObjectItemCaseSensitive(fixtures, "valid"));
        // The screensaver's own (docs/design/screensaver.md): Yay's hello and prefs with it, its actions, the answers.
        const cJSON *saver = cJSON_GetObjectItemCaseSensitive(fixtures, "screensaver");
        CHECK(saver != NULL);
        test_device(cJSON_GetObjectItemCaseSensitive(saver, "valid"), true);
        test_device(cJSON_GetObjectItemCaseSensitive(saver, "invalid"), false);
        test_server(cJSON_GetObjectItemCaseSensitive(saver, "valid"), true);
        test_server(cJSON_GetObjectItemCaseSensitive(saver, "invalid"), false);
    }
    test_saver();
    test_fields();
    test_modes();
    test_turns();
    test_backoff();
    cJSON_Delete(fixtures);
    free(text);
    printf("yaykit_session host tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

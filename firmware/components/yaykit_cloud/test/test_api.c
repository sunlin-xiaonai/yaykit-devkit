// Host tests of yk_api.c against the platform's contract fixtures (packages/contracts/fixtures/device-api.json):
// every valid answer must read, and every request the client builds must equal the fixture's request.
// Run: bash firmware/components/yaykit_cloud/test/run.sh (needs ESP-IDF's cJSON, from IDF_PATH).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../yk_api.h"

static int failures, checks;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            failures++;                                                                 \
            printf("FAIL %s:%d %s (%s)\n", __FILE__, __LINE__, #cond, current);         \
        }                                                                               \
    } while (0)

static const char *current = "";

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}
static const cJSON *at(const cJSON *o, const char *name) { return cJSON_GetObjectItemCaseSensitive(o, name); }
static const char *str(const cJSON *o, const char *name) {
    const cJSON *s = at(o, name);
    return cJSON_IsString(s) ? s->valuestring : NULL;
}
static double num(const cJSON *o, const char *name, double fallback) {
    const cJSON *n = at(o, name);
    return cJSON_IsNumber(n) ? n->valuedouble : fallback;
}
static bool same(const char *a, const char *b) { return (!a && !b) || (a && b && !strcmp(a, b)); }
/** Whether the JSON text `built` equals the fixture value. */
static bool equal(const char *built, const cJSON *expected) {
    cJSON *b = cJSON_Parse(built);
    bool ok = b && cJSON_Compare(b, expected, true);
    if (!ok) printf("  built    %s\n  expected %s\n", built, cJSON_PrintUnformatted(expected));
    cJSON_Delete(b);
    return ok;
}

static void me(const cJSON *v) {
    char *json = cJSON_PrintUnformatted(v);
    yk_cloud_me_t out;
    cJSON *caps = NULL, *settings = NULL;
    CHECK(yk_api_me(json, &out, &caps, &settings));
    const cJSON *credits = at(v, "credits"), *owner = at(v, "owner");
    CHECK(out.api_version == 1);
    CHECK(out.remaining == (int)num(credits, "remaining", -1));
    CHECK(out.daily_limit == (int)num(credits, "dailyLimit", -1));
    CHECK(same(out.reset_at, str(credits, "resetAt")));
    CHECK(same(out.timezone, str(owner, "timezone")));
    CHECK(out.storage_limit == (int64_t)num(at(v, "storage"), "limitBytes", -1));
    CHECK(out.settings_version == (int)num(v, "settingsVersion", -1));
    CHECK(out.records_enabled == cJSON_IsTrue(at(at(v, "records"), "enabled")));
    CHECK(same(out.min_firmware, str(v, "minFirmware") ? str(v, "minFirmware") : ""));
    // Every capability granted is there, with its limits; one this firmware does not know is simply present.
    const cJSON *cap;
    cJSON_ArrayForEach(cap, at(v, "capabilities")) CHECK(yk_api_has(caps, cap->string));
    CHECK(!yk_api_has(caps, "nothing.here"));
    const cJSON *asr = at(at(v, "capabilities"), "asr");
    if (asr) {
        CHECK(yk_api_limit(caps, "asr", "maxSeconds", 0) == (int)num(asr, "maxSeconds", -1));
        CHECK(yk_api_allows(caps, "asr", "languages", cJSON_GetArrayItem(at(asr, "languages"), 0)->valuestring));
        CHECK(!yk_api_allows(caps, "asr", "languages", "xx"));
    }
    const cJSON *files = at(at(v, "capabilities"), "files");
    if (files) {
        CHECK(yk_api_allows(caps, "files", "contentTypes", "audio/ogg"));
        CHECK(yk_api_limit(caps, "files", "maxBytes", 0) == (int)num(files, "maxBytes", -1));
    }
    CHECK(yk_api_limit(caps, "nothing", "maxSeconds", 7) == 7);
    CHECK(cJSON_GetArraySize(settings) == cJSON_GetArraySize(at(v, "settings")));
    cJSON_Delete(caps);
    cJSON_Delete(settings);
    cJSON_free(json);
}

static void asr(const cJSON *v) {
    char *json = cJSON_PrintUnformatted(v);
    yk_cloud_asr_t out;
    CHECK(yk_api_asr(json, &out));
    CHECK(same(out.text, str(v, "text")));
    CHECK(same(out.language, str(v, "language")));
    CHECK(out.credits.charged == (int)num(at(v, "credits"), "charged", -1));
    CHECK(out.credits.remaining == (int)num(at(v, "credits"), "remaining", -1));
    cJSON_free(json);
}

static void chat_request(const cJSON *v) {
    yk_cloud_message_t messages[8];
    size_t count = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, at(v, "messages")) {
        if (count < 8) messages[count++] = (yk_cloud_message_t){str(m, "role"), str(m, "content")};
    }
    const char *names[8], *values[8];
    size_t vars = 0;
    const cJSON *var;
    cJSON_ArrayForEach(var, at(v, "vars")) {
        if (vars < 8) names[vars] = var->string, values[vars++] = var->valuestring;
    }
    yk_cloud_chat_t request = {.tier = str(v, "tier"),
                               .messages = messages,
                               .count = count,
                               .max_tokens = (int)num(v, "maxTokens", 0),
                               .json = cJSON_IsTrue(at(v, "json")),
                               .template_name = str(v, "template"),
                               .var_names = names,
                               .var_values = values,
                               .var_count = vars};
    char *body = yk_api_chat_body(&request);
    CHECK(equal(body, v));
    cJSON_free(body);
}

static char deltas[64];
static void on_delta(const char *t, void *ctx) {
    (void)ctx;
    strncat(deltas, t, sizeof(deltas) - strlen(deltas) - 1);
}
static void chat_events(const char *schema, const cJSON *v) {
    yk_cloud_chat_done_t done;
    const char *code = NULL;
    if (!strcmp(schema, "chatEvents.delta")) {
        deltas[0] = 0;
        CHECK(yk_api_chat_event("delta", v, on_delta, NULL, &done, &code));
        CHECK(same(deltas, str(v, "text")));
    } else {
        CHECK(yk_api_chat_event("done", v, on_delta, NULL, &done, &code));
        CHECK(done.input_tokens == (int)num(at(v, "usage"), "inputTokens", -1));
        CHECK(done.credits.charged == (int)num(at(v, "credits"), "charged", -1));
    }
    // Events this firmware does not know are skipped; an error ends the stream with a code to keep.
    CHECK(yk_api_chat_event("heard", v, on_delta, NULL, &done, &code));
    cJSON *error = cJSON_Parse("{\"error\":\"CREDITS_EXHAUSTED\"}");
    CHECK(!yk_api_chat_event("error", error, on_delta, NULL, &done, &code) && !strcmp(code, "CREDITS_EXHAUSTED"));
    cJSON_Delete(error);
}

static void tts_request(const cJSON *v) {
    yk_cloud_tts_t options = {.voice = str(v, "voice"),
                              .format = str(v, "format"),
                              .sample_rate = (int)num(v, "sampleRate", 0)};
    char *body = yk_api_tts_body(str(v, "text"), &options);
    cJSON *expected = cJSON_Duplicate(v, true);
    // The format is always sent (Opus unless asked).
    if (!at(expected, "format")) cJSON_AddStringToObject(expected, "format", "opus");
    CHECK(equal(body, expected));
    cJSON_Delete(expected);
    cJSON_free(body);
}

static uint8_t heard[16];
static size_t heard_len;
static void on_audio(int seq, const uint8_t *data, size_t len, void *ctx) {
    (void)seq, (void)ctx;
    heard_len = len < sizeof(heard) ? len : sizeof(heard);
    memcpy(heard, data, heard_len);
}
static void tts_events(const char *schema, const cJSON *v) {
    yk_cloud_charge_t credits = {0};
    const char *code = NULL;
    if (!strcmp(schema, "ttsEvents.audio")) {
        heard_len = 0;
        uint8_t expected[16];
        size_t n = yk_api_b64_decode(str(v, "data"), expected, sizeof(expected));
        // Decoded in place: the fixture's string is overwritten, so it is read first.
        CHECK(yk_api_tts_event("audio", v, on_audio, NULL, &credits, &code));
        CHECK(n > 0 && heard_len == n && !memcmp(heard, expected, n));
    } else {
        CHECK(yk_api_tts_event("done", v, on_audio, NULL, &credits, &code));
        CHECK(credits.charged == (int)num(at(v, "credits"), "charged", -1));
    }
}

static void test_base64(void) {
    uint8_t out[16];
    current = "base64";
    CHECK(yk_api_b64_decode("T2dnUw==", out, sizeof(out)) == 4 && !memcmp(out, "OggS", 4));
    CHECK(yk_api_b64_decode("Zm9vYg==", out, sizeof(out)) == 4 && !memcmp(out, "foob", 4));
    CHECK(yk_api_b64_decode("Zm9vYmE=", out, sizeof(out)) == 5 && !memcmp(out, "fooba", 5));
    CHECK(yk_api_b64_decode("Zm9vYmFy", out, sizeof(out)) == 6 && !memcmp(out, "foobar", 6));
    CHECK(yk_api_b64_decode("Zm9v!mFy", out, sizeof(out)) == 0);
    CHECK(yk_api_b64_decode("Zm9", out, sizeof(out)) == 0);
    CHECK(yk_api_b64_decode("Zm9vYmFy", out, 3) == 0);
    // In place, as /tts audio is decoded.
    char same[] = "Zm9vYmFy";
    CHECK(yk_api_b64_decode(same, (uint8_t *)same, strlen(same)) == 6 && !memcmp(same, "foobar", 6));
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const uint8_t *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < len ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = 0;
}
static uint8_t whole[3828];
static size_t whole_len;
static int whole_seq;
static void on_whole(int seq, const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    whole_seq = seq;
    whole_len = len;
    if (len <= sizeof(whole)) memcpy(whole, data, len);
}

static void test_tts_stream(void) {
    // The largest Opus packet (3,828 bytes) arrives whole: decoded over the event's own string, no stack buffer.
    current = "a whole Opus packet";
    static uint8_t packet[3828];
    static char text[5200];
    for (size_t i = 0; i < sizeof(packet); i++) packet[i] = (uint8_t)(i * 7 + 3);
    b64_encode(packet, sizeof(packet), text);
    CHECK(strlen(text) == 5104);
    cJSON *event = cJSON_CreateObject();
    cJSON_AddNumberToObject(event, "seq", 41);
    cJSON_AddStringToObject(event, "data", text);
    yk_cloud_charge_t credits = {0};
    const char *code = NULL;
    CHECK(yk_api_tts_event("audio", event, on_whole, NULL, &credits, &code));
    CHECK(whole_seq == 41 && whole_len == sizeof(packet) && !memcmp(whole, packet, sizeof(packet)));
    cJSON_Delete(event);

    // How a chat or speech stream ended: whole only with its done event.
    current = "stream end";
    CHECK(yk_api_stream_end(true, NULL) == NULL);
    CHECK(!strcmp(yk_api_stream_end(false, NULL), "NETWORK"));
    CHECK(!strcmp(yk_api_stream_end(false, "CREDITS_EXHAUSTED"), "CREDITS_EXHAUSTED"));
    CHECK(!strcmp(yk_api_stream_end(true, "PROVIDER_FAILED"), "PROVIDER_FAILED"));
}

static void deliver_request(const cJSON *v) {
    yk_cloud_delivery_info_t info = {.id = str(v, "id"),
                                     .target = str(v, "target"),
                                     .file_id = str(v, "fileId"),
                                     .text = str(v, "text"),
                                     .title = str(v, "title"),
                                     .notion_parent = str(at(v, "notion"), "parentId")};
    char *body = yk_api_deliver_body(&info);
    CHECK(equal(body, v));
    cJSON_free(body);
}
static void delivery_answer(const cJSON *v) {
    char *json = cJSON_PrintUnformatted(v);
    yk_cloud_delivery_t out;
    CHECK(yk_api_delivery(json, &out));
    const cJSON *d = at(v, "delivery");
    CHECK(same(out.id, str(d, "id")) && same(out.state, str(d, "state")) && same(out.target, str(d, "target")));
    CHECK(out.tries == (int)num(d, "tries", -1));
    cJSON_free(json);
}

static void file_register(const cJSON *v) {
    const cJSON *recording = at(v, "recording"), *transcribe = at(v, "transcribe");
    yk_cloud_file_info_t info = {
        .id = str(v, "id"),
        .kind = str(v, "kind"),
        .content_type = str(v, "contentType"),
        .size = (int64_t)num(v, "size", 0),
        .sha256 = str(v, "sha256"),
        .duration_ms = (int64_t)num(v, "durationMs", -1),
        .speech_ms = (int64_t)num(v, "speechMs", -1),
        .recorded_at = str(v, "recordedAt"),
        .clock_trusted = cJSON_IsTrue(at(v, "clockTrusted")),
        .retention = str(v, "retention"),
        .recording_id = str(recording, "id"),
        .segment = (int)num(recording, "segment", 0),
        .transcribe = cJSON_IsTrue(at(transcribe, "enabled")),
        .language = str(transcribe, "language"),
    };
    char *body = yk_api_file_body(&info);
    CHECK(equal(body, v));
    cJSON_free(body);
}

static void file_answer(const cJSON *v) {
    char *json = cJSON_PrintUnformatted(v);
    yk_cloud_file_t out;
    CHECK(yk_api_file(json, &out));
    const cJSON *file = at(v, "file"), *upload = at(v, "upload"), *transcript = at(file, "transcript");
    CHECK(same(out.id, str(file, "id")));
    CHECK(same(out.state, str(file, "state")));
    CHECK(out.size == (int64_t)num(file, "size", -1));
    CHECK(out.received == (int64_t)num(file, "receivedBytes", -1));
    CHECK(same(out.transcript, cJSON_IsObject(transcript) ? str(transcript, "state") : ""));
    CHECK(out.has_upload == cJSON_IsObject(upload));
    if (cJSON_IsObject(upload)) {
        CHECK(out.nparts == cJSON_GetArraySize(at(upload, "parts")));
        CHECK(out.received_parts == cJSON_GetArraySize(at(upload, "receivedParts")));
        for (int i = 0; i < out.nparts; i++) {
            const cJSON *p = cJSON_GetArrayItem(at(upload, "parts"), i);
            CHECK(out.parts[i].number == (int)num(p, "number", -1));
            CHECK(out.parts[i].offset == (int64_t)num(p, "offset", -1));
            CHECK(out.parts[i].bytes == (int64_t)num(p, "bytes", -1));
            CHECK(same(out.parts[i].url, str(p, "url")));
            CHECK(yk_api_https(out.parts[i].url));
        }
    }
    CHECK(out.has_credits == cJSON_IsObject(at(v, "credits")));
    yk_cloud_file_free(&out);
    CHECK(out.nparts == 0);
    cJSON_free(json);
}

static void data_item(const cJSON *v) {
    yk_cloud_data_item_t out;
    CHECK(yk_api_data_item(v, &out));
    CHECK(same(out.key, str(v, "key")));
    CHECK(out.version == (int)num(v, "version", -1));
    CHECK(cJSON_Compare(out.value, at(v, "value"), true));
    yk_cloud_data_free(&out);
}

static void data_put(const cJSON *v) {
    char *body = yk_api_data_body(at(v, "value"), (int)num(v, "version", -1));
    CHECK(equal(body, v));
    cJSON_free(body);
}

static int items;
static void on_item(const char *id, const char *kind, const cJSON *data, void *ctx) {
    (void)id, (void)kind, (void)data, (void)ctx;
    items++;
}
static void inbox(const cJSON *v) {
    char *json = cJSON_PrintUnformatted(v), now[40];
    items = 0;
    CHECK(yk_api_inbox(json, on_item, NULL, now, sizeof(now)));
    CHECK(items == cJSON_GetArraySize(at(v, "items")));
    CHECK(same(now, str(v, "now")));
    cJSON_free(json);
}

static void refusal(const cJSON *v) {
    char *json = cJSON_PrintUnformatted(v);
    const char *code = yk_api_error_code(json);
    // Every code the server sends is one the firmware knows.
    CHECK(strcmp(code, "SERVER") != 0 && !strcmp(code, str(v, "error")));
    const cJSON *detail;
    cJSON_ArrayForEach(detail, v) {
        if (cJSON_IsNumber(detail)) CHECK(yk_api_error_number(json, detail->string, -1) == (int)detail->valuedouble);
    }
    cJSON_free(json);
}

static void test_escape(void) {
    char out[64];
    current = "escape";
    CHECK(yk_api_escape("2026-10-19T08:00:00+08:00", out, sizeof(out)) && !strcmp(out, "2026-10-19T08%3A00%3A00%2B08%3A00"));
    CHECK(yk_api_escape("a b/c", out, sizeof(out)) && !strcmp(out, "a%20b%2Fc"));
    char tiny[4];
    CHECK(yk_api_escape("abcd", tiny, sizeof(tiny)) == 0);
    CHECK(!strcmp(yk_api_code("NOT_A_CODE"), "SERVER"));
    CHECK(!strcmp(yk_api_code("TEXT_EMPTY"), "TEXT_EMPTY"));
    CHECK(!strcmp(yk_api_code("DELIVERIES_WAITING"), "DELIVERIES_WAITING") && !strcmp(yk_api_code("RETRY_TOO_SOON"), "RETRY_TOO_SOON"));
    CHECK(yk_api_https("https://bucket.fly.storage.tigris.dev/devices/u/f?partNumber=1"));
    CHECK(!yk_api_https("http://bucket.fly.storage.tigris.dev/devices/u/f"));
    CHECK(!yk_api_https("file:///etc/passwd"));
    CHECK(!yk_api_https("https://"));
    CHECK(!yk_api_https("https:///path"));
    CHECK(!yk_api_https("https://host/a\r\nHost: other"));
    CHECK(!yk_api_https(NULL));
    CHECK(!strcmp(yk_api_code(NULL), "SERVER"));
}

int main(int argc, char **argv) {
    char *text = slurp(argc > 1 ? argv[1] : "../../../../../packages/contracts/fixtures/device-api.json");
    if (!text) {
        printf("no fixtures\n");
        return 1;
    }
    cJSON *fixtures = cJSON_Parse(text);
    const cJSON *c;
    int read = 0;
    cJSON_ArrayForEach(c, at(fixtures, "valid")) {
        const char *schema = str(c, "schema");
        const cJSON *v = at(c, "value");
        current = str(c, "name") ? str(c, "name") : schema;
        read++;
        if (!strcmp(schema, "meResponse")) me(v);
        else if (!strcmp(schema, "asrResponse")) asr(v);
        else if (!strcmp(schema, "chatRequest")) chat_request(v);
        else if (!strncmp(schema, "chatEvents.", 11)) chat_events(schema, v);
        else if (!strcmp(schema, "ttsRequest")) tts_request(v);
        else if (!strncmp(schema, "ttsEvents.", 10)) tts_events(schema, v);
        else if (!strcmp(schema, "deliverRequest")) deliver_request(v);
        else if (!strcmp(schema, "deliveryAnswer")) delivery_answer(v);
        else if (!strcmp(schema, "fileRegister")) file_register(v);
        else if (!strcmp(schema, "fileRegistered") || !strcmp(schema, "fileAnswer")) file_answer(v);
        else if (!strcmp(schema, "dataItem")) data_item(v);
        else if (!strcmp(schema, "dataPut")) data_put(v);
        else if (!strcmp(schema, "inbox")) inbox(v);
        else if (!strcmp(schema, "deviceError")) refusal(v);
        else read--;
    }
    test_escape();
    test_base64();
    test_tts_stream();
    printf("%d checks over %d fixtures, %d failures\n", checks, read, failures);
    cJSON_Delete(fixtures);
    free(text);
    return failures ? 1 : 0;
}

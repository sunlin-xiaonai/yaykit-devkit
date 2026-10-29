#include "yaykit_cloud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "yaykit_account.h"
#include "yk_api.h"
#include "yk_http.h"
#include "yk_locale.h"
#include "yk_sse.h"

static const char *TAG = "yk_cloud";
/** This app's capabilities and settings from the last /me (used from the app's own task). */
static cJSON *s_capabilities, *s_settings;

const char *yk_cloud_origin(void) { return CONFIG_YK_CLOUD_ORIGIN; }

/** After a refused call: forget an expired token or a binding the cloud no longer knows. */
static bool handle_refusal(int status, const char *code) {
    if (status == 401 && !strcmp(code, "TOKEN_EXPIRED")) {
        yk_account_token_expired();
        return true;  // worth one retry with a new token
    }
    if (status == 401 && !strcmp(code, "DEVICE_NOT_ACTIVATED")) yk_account_unbound_by_server();
    return false;
}

esp_err_t yk_cloud_me(yk_cloud_me_t *out, const char **code) { return yk_cloud_me_ex(out, true, code); }

esp_err_t yk_cloud_me_ex(yk_cloud_me_t *out, bool keep_owner, const char **code) {
    memset(out, 0, sizeof(*out));
    for (int attempt = 0; attempt < 2; attempt++) {
        char token[512];
        if (yk_account_token(token, sizeof(token), code) != ESP_OK) return ESP_FAIL;
        yk_http_response_t res;
        if (yk_http_json(HTTP_METHOD_GET, "/device/v1/me", token, NULL, &res) != ESP_OK) {
            *code = "NETWORK";
            return ESP_FAIL;
        }
        if (res.status == 200) {
            cJSON *capabilities = NULL, *settings = NULL;
            if (yk_api_me(res.body, out, &capabilities, &settings)) {
                cJSON_Delete(s_capabilities);
                cJSON_Delete(s_settings);
                s_capabilities = capabilities;
                s_settings = settings;
            }
            if (keep_owner) {
                cJSON *json = cJSON_Parse(res.body ? res.body : "");
                yk_account_owner_seen(cJSON_GetObjectItemCaseSensitive(json, "owner"));
                cJSON_Delete(json);
            }
            yk_http_free(&res);
            return ESP_OK;
        }
        *code = yk_http_error_code(&res);
        int status = res.status;
        yk_http_free(&res);
        if (!handle_refusal(status, *code)) return ESP_FAIL;
    }
    return ESP_FAIL;
}

typedef struct {
    yk_cloud_event_fn on_event;
    void *ctx;
    int events;
    bool precise;  // level 2: refusals as the API names them
} stream_t;

static void on_sse(const char *event, const char *data, void *ctx) {
    stream_t *s = ctx;
    cJSON *json = cJSON_Parse(data);
    s->events++;
    if (s->on_event) s->on_event(event, json, s->ctx);
    cJSON_Delete(json);
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/** What has come of the stream: esp_http_client_read() returns it when the client's (short) timeout passes. */
static int read_some(char *buf, int size, void *ctx) {
    const int n = esp_http_client_read((esp_http_client_handle_t)ctx, buf, size);
    return n == -ESP_ERR_HTTP_EAGAIN ? YK_SSE_AGAIN : n;
}

/** One streaming POST. Returns the HTTP status (0 when no response arrived). */
static int stream_post(const char *path, const char *token, const char *content_type, const uint8_t *body, size_t len,
                       stream_t *stream, const char **code) {
    // A lookup is speech recognition plus a model call; a slow cross-border hop can take ~12 s.
    esp_http_client_handle_t client = yk_http_client(path, HTTP_METHOD_POST, token, content_type, 30000);
    if (!client) {
        *code = "NETWORK";
        return 0;
    }
    int status = 0;
    esp_err_t err = esp_http_client_open(client, (int)len);
    for (size_t sent = 0; err == ESP_OK && sent < len;) {
        int n = esp_http_client_write(client, (const char *)body + sent, (int)(len - sent > 4096 ? 4096 : len - sent));
        if (n <= 0) err = ESP_FAIL;
        else sent += (size_t)n;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0) status = esp_http_client_get_status_code(client);
    if (status == 200) {
        yk_sse_t *parser = heap_caps_malloc(sizeof(yk_sse_t), MALLOC_CAP_SPIRAM);
        char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        if (parser && buf) {
            // esp_http_client_read() waits until its buffer is full or the stream ends, so a lookup's few events
            // (well under 1 KB) all came at the end and 「你说的是」 never showed first. Each read now hands over
            // what came within 200 ms; the stream still ends after 30 s without a byte, as the client's timeout did.
            esp_http_client_set_timeout_ms(client, 200);
            yk_sse_init(parser, on_sse, stream);
            yk_sse_pump(parser, read_some, client, buf, 1024, 30000, now_ms);
        } else {
            *code = "NETWORK";
        }
        free(parser);
        free(buf);
    } else if (status) {
        yk_http_response_t res = {.status = status};
        yk_http_read_body(client, &res);
        *code = stream->precise ? yk_api_error_code(res.body) : yk_http_error_code(&res);
        yk_http_free(&res);
        ESP_LOGW(TAG, "%s refused: HTTP %d %s", path, status, *code);
    } else {
        *code = "NETWORK";
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

esp_err_t yk_cloud_hanzi_lookup(const uint8_t *wav, size_t len, yk_cloud_event_fn on_event, void *ctx,
                                const char **code) {
    stream_t stream = {.on_event = on_event, .ctx = ctx};
    for (int attempt = 0; attempt < 2; attempt++) {
        char token[512];
        if (yk_account_token(token, sizeof(token), code) != ESP_OK) return ESP_FAIL;
        if (yk_http_prepare(code) != ESP_OK) return ESP_FAIL;
        int status = stream_post("/device/v1/hanzi/lookup", token, "audio/wav", wav, len, &stream, code);
        if (status == 200) return stream.events ? ESP_OK : (*code = "NETWORK", ESP_FAIL);
        if (!handle_refusal(status, *code)) return ESP_FAIL;
    }
    return ESP_FAIL;
}

// ---- Level 2: the device API v1 (docs/design/yay-device.md §13.4) ----

bool yk_cloud_has(const char *capability) { return yk_api_has(s_capabilities, capability); }
int yk_cloud_limit(const char *capability, const char *key, int fallback) {
    return yk_api_limit(s_capabilities, capability, key, fallback);
}
bool yk_cloud_allows(const char *capability, const char *list, const char *value) {
    return yk_api_allows(s_capabilities, capability, list, value);
}
bool yk_cloud_setting(const char *key, char *out, size_t size) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(s_settings, key);
    if (!value || !size) return false;
    if (cJSON_IsString(value)) {
        snprintf(out, size, "%s", value->valuestring);
        return true;
    }
    char *json = cJSON_PrintUnformatted(value);
    if (!json) return false;
    snprintf(out, size, "%s", json);
    cJSON_free(json);
    return true;
}

/**
 * One call with the app's token, tried again once with a new token when it expired. ESP_OK for a 2xx answer;
 * `res` holds the body either way (yk_http_free), and `*code` the refusal as the API names it.
 */
static esp_err_t call(esp_http_client_method_t method, const char *path, const char *content_type, const uint8_t *body,
                      size_t len, int timeout_ms, yk_http_response_t *res, const char **code) {
    memset(res, 0, sizeof(*res));
    for (int attempt = 0; attempt < 2; attempt++) {
        char token[512];
        if (yk_account_token(token, sizeof(token), code) != ESP_OK) return ESP_FAIL;
        if (yk_http_prepare(code) != ESP_OK) return ESP_FAIL;
        if (yk_http_send(method, path, token, content_type, body, len, timeout_ms, res) != ESP_OK) {
            *code = "NETWORK";
            return ESP_FAIL;
        }
        if (res->status >= 200 && res->status < 300) return ESP_OK;
        *code = yk_api_error_code(res->body);
        ESP_LOGW(TAG, "%s refused: HTTP %d %s", path, res->status, *code);
        if (attempt || !handle_refusal(res->status, *code)) return ESP_FAIL;
        yk_http_free(res);
    }
    return ESP_FAIL;
}
static esp_err_t call_json(esp_http_client_method_t method, const char *path, const char *json, int timeout_ms,
                           yk_http_response_t *res, const char **code) {
    return call(method, path, json ? "application/json" : NULL, (const uint8_t *)json, json ? strlen(json) : 0,
                timeout_ms, res, code);
}

esp_err_t yk_cloud_asr(const uint8_t *audio, size_t len, yk_cloud_format_t format, const char *language,
                       yk_cloud_asr_t *out, const char **code) {
    char path[64], lang[16];
    snprintf(path, sizeof(path), "/device/v1/asr%s%s", language ? "?language=" : "",
             language && yk_api_escape(language, lang, sizeof(lang)) ? lang : "");
    yk_http_response_t res;
    esp_err_t err = call(HTTP_METHOD_POST, path, format == YK_CLOUD_OGG_OPUS ? "audio/ogg" : "audio/wav", audio, len,
                         30000, &res, code);
    if (err == ESP_OK && !yk_api_asr(res.body, out)) {
        *code = "SERVER";
        err = ESP_FAIL;
    }
    yk_http_free(&res);
    return err;
}

/** A streaming POST with the app's token, tried again once when the token expired. */
static esp_err_t stream_call(const char *path, const char *content_type, const uint8_t *body, size_t len, stream_t *s,
                        const char **code) {
    s->precise = true;
    for (int attempt = 0; attempt < 2; attempt++) {
        char token[512];
        if (yk_account_token(token, sizeof(token), code) != ESP_OK) return ESP_FAIL;
        if (yk_http_prepare(code) != ESP_OK) return ESP_FAIL;
        int status = stream_post(path, token, content_type, body, len, s, code);
        if (status == 200) return s->events ? ESP_OK : (*code = "NETWORK", ESP_FAIL);
        if (!handle_refusal(status, *code)) return ESP_FAIL;
    }
    return ESP_FAIL;
}

esp_err_t yk_cloud_scene(const char *path, const char *content_type, const uint8_t *body, size_t len,
                         yk_cloud_event_fn on_event, void *ctx, const char **code) {
    char full[96];
    snprintf(full, sizeof(full), "/device/v1/%s", path);
    stream_t s = {.on_event = on_event, .ctx = ctx};
    return stream_call(full, content_type, body, len, &s, code);
}

typedef struct {
    yk_cloud_delta_fn on_delta;
    void *ctx;
    yk_cloud_chat_done_t *done;
    bool finished;
    const char *error;
} chat_t;
static void on_chat(const char *event, const cJSON *data, void *ctx) {
    chat_t *c = ctx;
    const char *code = NULL;
    if (!strcmp(event, "done")) c->finished = true;
    if (!yk_api_chat_event(event, data, c->on_delta, c->ctx, c->done, &code)) c->error = code;
}

esp_err_t yk_cloud_chat(const yk_cloud_chat_t *request, yk_cloud_delta_fn on_delta, void *ctx, yk_cloud_chat_done_t *done,
                        const char **code) {
    char *body = yk_api_chat_body(request);
    if (!body) {
        *code = "SERVER";
        return ESP_FAIL;
    }
    chat_t c = {.on_delta = on_delta, .ctx = ctx, .done = done};
    stream_t s = {.on_event = on_chat, .ctx = &c};
    esp_err_t err = stream_call("/device/v1/llm/chat", "application/json", (const uint8_t *)body, strlen(body), &s, code);
    cJSON_free(body);
    const char *end = err == ESP_OK ? yk_api_stream_end(c.finished, c.error) : NULL;
    if (end) {
        *code = end;
        err = ESP_FAIL;
    }
    return err;
}

typedef struct {
    yk_cloud_audio_fn on_audio;
    void *ctx;
    yk_cloud_charge_t *credits;
    bool finished;
    const char *error;
} tts_t;
static void on_tts(const char *event, const cJSON *data, void *ctx) {
    tts_t *t = ctx;
    const char *code = NULL;
    if (!strcmp(event, "done")) t->finished = true;
    if (!yk_api_tts_event(event, data, t->on_audio, t->ctx, t->credits, &code)) t->error = code;
}

esp_err_t yk_cloud_tts(const char *text, const yk_cloud_tts_t *options, yk_cloud_audio_fn on_audio, void *ctx,
                       yk_cloud_charge_t *credits, const char **code) {
    char *body = yk_api_tts_body(text, options);
    if (!body) {
        *code = "SERVER";
        return ESP_FAIL;
    }
    tts_t t = {.on_audio = on_audio, .ctx = ctx, .credits = credits};
    stream_t s = {.on_event = on_tts, .ctx = &t};
    esp_err_t err = stream_call("/device/v1/tts", "application/json", (const uint8_t *)body, strlen(body), &s, code);
    cJSON_free(body);
    const char *end = err == ESP_OK ? yk_api_stream_end(t.finished, t.error) : NULL;
    if (end) {
        *code = end;
        err = ESP_FAIL;
    }
    return err;
}

/** A {file, upload?} answer into `out`; ESP_FAIL with SERVER when it is not one. */
static esp_err_t file_answer(esp_err_t err, yk_http_response_t *res, yk_cloud_file_t *out, const char **code) {
    if (err == ESP_OK && !yk_api_file(res->body, out)) {
        *code = "SERVER";
        err = ESP_FAIL;
    }
    yk_http_free(res);
    return err;
}

esp_err_t yk_cloud_file_register(const yk_cloud_file_info_t *info, yk_cloud_file_t *out, const char **code) {
    char *body = yk_api_file_body(info);
    yk_http_response_t res;
    esp_err_t err = call_json(HTTP_METHOD_POST, "/device/v1/files", body, 15000, &res, code);
    cJSON_free(body);
    return file_answer(err, &res, out, code);
}

esp_err_t yk_cloud_file_upload(const yk_cloud_part_t *part, yk_cloud_read_fn read, void *ctx, const char **code) {
    // Only ever HTTPS: the URL comes from our platform, and the device sends the recording nowhere else.
    if (!yk_api_https(part->url)) {
        *code = "UPLOAD_REFUSED";
        return ESP_FAIL;
    }
    if (yk_http_prepare(code) != ESP_OK) return ESP_FAIL;
    // Straight to storage with the signed URL: no token, and exactly the length it was signed for.
    esp_http_client_config_t cfg = {
        .url = part->url,
        .method = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!client || !buf) {
        if (client) esp_http_client_cleanup(client);
        free(buf);
        *code = "NETWORK";
        return ESP_FAIL;
    }
    int status = 0;
    esp_err_t err = esp_http_client_open(client, (int)part->bytes);
    for (int64_t sent = 0; err == ESP_OK && sent < part->bytes;) {
        size_t want = part->bytes - sent > 4096 ? 4096 : (size_t)(part->bytes - sent);
        int n = read(ctx, part->offset + sent, buf, want);
        if (n <= 0 || esp_http_client_write(client, (const char *)buf, n) != n) err = ESP_FAIL;
        else sent += n;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0) status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
    if (status >= 200 && status < 300) return ESP_OK;
    *code = status == 403 ? "UPLOAD_REFUSED" : "NETWORK";
    ESP_LOGW(TAG, "part %d not sent: HTTP %d", part->number, status);
    return ESP_FAIL;
}

static esp_err_t file_call(esp_http_client_method_t method, const char *id, const char *action, const char *json,
                           int timeout_ms, yk_cloud_file_t *out, const char **code) {
    char path[160];
    snprintf(path, sizeof(path), "/device/v1/files/%s%s", id, action);
    yk_http_response_t res;
    esp_err_t err = call_json(method, path, json, timeout_ms, &res, code);
    if (!out) {
        yk_http_free(&res);
        return err;
    }
    return file_answer(err, &res, out, code);
}
esp_err_t yk_cloud_file_complete(const char *id, yk_cloud_file_t *out, const char **code) {
    // The platform reads the whole file back to check it: hundreds of megabytes for a long recording.
    return file_call(HTTP_METHOD_POST, id, "/complete", "{}", 130000, out, code);
}
esp_err_t yk_cloud_file_status(const char *id, yk_cloud_file_t *out, const char **code) {
    return file_call(HTTP_METHOD_GET, id, "", NULL, 15000, out, code);
}
esp_err_t yk_cloud_file_transcribe(const char *id, const char *language, yk_cloud_file_t *out, const char **code) {
    char body[48];
    if (language) snprintf(body, sizeof(body), "{\"language\":\"%.8s\"}", language);
    else snprintf(body, sizeof(body), "{}");
    return file_call(HTTP_METHOD_POST, id, "/transcribe", body, 15000, out, code);
}
esp_err_t yk_cloud_file_delete(const char *id, const char **code) {
    return file_call(HTTP_METHOD_DELETE, id, "", NULL, 15000, NULL, code);
}

static esp_err_t data_answer(esp_err_t err, yk_http_response_t *res, yk_cloud_data_item_t *out, const char **code) {
    if (err == ESP_OK && out) {
        cJSON *json = cJSON_Parse(res->body ? res->body : "");
        if (!yk_api_data_item(json, out)) {
            *code = "SERVER";
            err = ESP_FAIL;
        }
        cJSON_Delete(json);
    }
    yk_http_free(res);
    return err;
}
esp_err_t yk_cloud_data_get(const char *collection, const char *key, yk_cloud_data_item_t *out, const char **code) {
    char path[224];
    snprintf(path, sizeof(path), "/device/v1/data/%s/%s", collection, key);
    yk_http_response_t res;
    return data_answer(call_json(HTTP_METHOD_GET, path, NULL, 15000, &res, code), &res, out, code);
}
esp_err_t yk_cloud_data_put(const char *collection, const char *key, const cJSON *value, int version,
                            yk_cloud_data_item_t *out, int *current, const char **code) {
    char path[224];
    snprintf(path, sizeof(path), "/device/v1/data/%s/%s", collection, key);
    char *body = yk_api_data_body(value, version);
    yk_http_response_t res;
    esp_err_t err = call_json(HTTP_METHOD_PUT, path, body, 15000, &res, code);
    cJSON_free(body);
    if (err != ESP_OK && current && !strcmp(*code, "VERSION_CONFLICT")) *current = yk_api_error_number(res.body, "version", -1);
    return data_answer(err, &res, out, code);
}
esp_err_t yk_cloud_data_del(const char *collection, const char *key, int version, const char **code) {
    char path[240];
    if (version >= 0) snprintf(path, sizeof(path), "/device/v1/data/%s/%s?version=%d", collection, key, version);
    else snprintf(path, sizeof(path), "/device/v1/data/%s/%s", collection, key);
    yk_http_response_t res;
    return data_answer(call_json(HTTP_METHOD_DELETE, path, NULL, 15000, &res, code), &res, NULL, code);
}
esp_err_t yk_cloud_data_list(const char *collection, const char *cursor, yk_cloud_data_fn on_item, void *ctx, char *next,
                             size_t next_size, const char **code) {
    char path[480], escaped[400];
    if (cursor && *cursor && yk_api_escape(cursor, escaped, sizeof(escaped)))
        snprintf(path, sizeof(path), "/device/v1/data/%s?cursor=%s", collection, escaped);
    else snprintf(path, sizeof(path), "/device/v1/data/%s", collection);
    if (next && next_size) next[0] = 0;
    yk_http_response_t res;
    esp_err_t err = call_json(HTTP_METHOD_GET, path, NULL, 15000, &res, code);
    if (err == ESP_OK) {
        cJSON *json = cJSON_Parse(res.body ? res.body : "");
        const cJSON *entry;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(json, "items")) {
            yk_cloud_data_item_t item;
            if (!yk_api_data_item(entry, &item)) continue;
            if (on_item) on_item(&item, ctx);
            yk_cloud_data_free(&item);
        }
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(json, "next");
        if (next && next_size && cJSON_IsString(n)) snprintf(next, next_size, "%s", n->valuestring);
        cJSON_Delete(json);
    }
    yk_http_free(&res);
    return err;
}

static esp_err_t delivery_answer(esp_err_t err, yk_http_response_t *res, yk_cloud_delivery_t *out, const char **code) {
    if (err == ESP_OK && out && !yk_api_delivery(res->body, out)) {
        *code = "SERVER";
        err = ESP_FAIL;
    }
    yk_http_free(res);
    return err;
}
esp_err_t yk_cloud_deliver(const yk_cloud_delivery_info_t *info, yk_cloud_delivery_t *out, const char **code) {
    char *body = yk_api_deliver_body(info);
    yk_http_response_t res;
    esp_err_t err = call_json(HTTP_METHOD_POST, "/device/v1/deliver", body, 15000, &res, code);
    cJSON_free(body);
    return delivery_answer(err, &res, out, code);
}
esp_err_t yk_cloud_delivery_status(const char *id, yk_cloud_delivery_t *out, const char **code) {
    char path[128];
    snprintf(path, sizeof(path), "/device/v1/deliveries/%s", id);
    yk_http_response_t res;
    return delivery_answer(call_json(HTTP_METHOD_GET, path, NULL, 15000, &res, code), &res, out, code);
}
esp_err_t yk_cloud_delivery_retry(const char *id, yk_cloud_delivery_t *out, const char **code) {
    char path[128];
    snprintf(path, sizeof(path), "/device/v1/deliveries/%s/retry", id);
    yk_http_response_t res;
    return delivery_answer(call_json(HTTP_METHOD_POST, path, "{}", 15000, &res, code), &res, out, code);
}

esp_err_t yk_cloud_records_push(const cJSON *records, int *accepted, const char **code) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "records", cJSON_Duplicate(records, true));
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    yk_http_response_t res;
    esp_err_t err = call_json(HTTP_METHOD_POST, "/device/v1/records", json, 15000, &res, code);
    cJSON_free(json);
    if (err == ESP_OK && accepted) {
        cJSON *answer = cJSON_Parse(res.body ? res.body : "");
        *accepted = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer, "accepted"));
        cJSON_Delete(answer);
    }
    yk_http_free(&res);
    return err;
}

esp_err_t yk_cloud_inbox(const char *since, yk_cloud_inbox_fn on_item, void *ctx, char *now, size_t now_size,
                         const char **code) {
    char path[128], escaped[96];
    if (since && *since && yk_api_escape(since, escaped, sizeof(escaped)))
        snprintf(path, sizeof(path), "/device/v1/inbox?since=%s", escaped);
    else snprintf(path, sizeof(path), "/device/v1/inbox");
    yk_http_response_t res;
    esp_err_t err = call_json(HTTP_METHOD_GET, path, NULL, 15000, &res, code);
    if (err == ESP_OK && !yk_api_inbox(res.body, on_item, ctx, now, now_size)) {
        *code = "SERVER";
        err = ESP_FAIL;
    }
    yk_http_free(&res);
    return err;
}

esp_err_t yk_cloud_apps(const char *layout, const char *lang, char **json, const char **code) {
    *json = NULL;
    char path[96];
    snprintf(path, sizeof(path), "/device/v1/apps?layout=%s&lang=%s", layout ? layout : "yaykit-v1",
             yk_locale_catalog(lang));  // "zh…" Chinese, any other English (#545)
    yk_http_response_t res;
    const esp_err_t err = call(HTTP_METHOD_GET, path, NULL, NULL, 0, 15000, &res, code);
    if (err == ESP_OK) {
        *json = res.body;  // the caller's now
        res.body = NULL;
    }
    yk_http_free(&res);
    return err;
}

/** A slug or a version as a query value as it is: letters, digits, '.', '-', '_' only (nothing to escape). */
static bool query_safe(const char *s) {
    if (!s || !*s || strlen(s) > 40) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' ||
              *s == '-' || *s == '_'))
            return false;
    return true;
}

esp_err_t yk_cloud_updates(const char *slug, const char *version, const char *layout, char **json, const char **code) {
    *json = NULL;
    if (!query_safe(slug) || !query_safe(version) || (layout && !query_safe(layout))) {
        *code = "BAD_REQUEST";
        return ESP_ERR_INVALID_ARG;
    }
    char path[160];
    snprintf(path, sizeof(path), "/device/v1/updates?slug=%s&version=%s&layout=%s", slug, version,
             layout ? layout : "yaykit-v1");
    yk_http_response_t res;
    const esp_err_t err = call(HTTP_METHOD_GET, path, NULL, NULL, 0, 15000, &res, code);
    if (err == ESP_OK) {
        *json = res.body;  // the caller's now
        res.body = NULL;
    }
    yk_http_free(&res);
    return err;
}

esp_err_t yk_cloud_get(const char *url, uint8_t *buf, size_t size, size_t *len, int timeout_ms, const char **code) {
    *len = 0;
    if (!url || strncmp(url, "https://", 8) || !buf) {
        *code = "BAD_URL";
        return ESP_ERR_INVALID_ARG;
    }
    if (yk_http_prepare(code) != ESP_OK) return ESP_FAIL;
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        *code = "NO_MEM";
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) err = ESP_FAIL;
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    if (err != ESP_OK) {
        *code = "NETWORK";
    } else if (status != 200) {
        *code = "SERVER";
        err = ESP_FAIL;
    } else {
        int n = 0;
        while (*len < size && (n = esp_http_client_read(c, (char *)buf + *len, (int)(size - *len))) > 0) *len += (size_t)n;
        char more;
        if (n < 0) {
            *code = "NETWORK";
            err = ESP_FAIL;
        } else if (*len == size && esp_http_client_read(c, &more, 1) > 0) {
            *code = "TOO_BIG";
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "get failed: HTTP %d %s", status, *code);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t yk_cloud_status(const cJSON *report, const char **code) {
    char *json = cJSON_PrintUnformatted(report);
    yk_http_response_t res;
    esp_err_t err = call_json(HTTP_METHOD_POST, "/device/v1/status", json, 15000, &res, code);
    cJSON_free(json);
    yk_http_free(&res);
    return err;
}

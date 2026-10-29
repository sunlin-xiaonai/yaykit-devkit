#include "yk_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Readers take what they know and skip the rest; builders write only what the device API expects
// (packages/contracts/src/device-api.ts). Shared by the device and test/test_api.c.

const char *yk_api_code(const char *error) {
    // The device API's codes (§13), as stable strings the firmware can compare and keep.
    static const char *const known[] = {
        "TOKEN_EXPIRED",          "DEVICE_NOT_ACTIVATED",  "OPERATION_NOT_ALLOWED", "CREDITS_EXHAUSTED",
        "ACCOUNT_FROZEN",         "RATE_LIMITED",          "VOICE_BUSY",            "SERVICE_BUSY",
        "CONTENT_BLOCKED",        "PROVIDER_FAILED",       "PROVIDER_UNAVAILABLE",  "VALIDATION_ERROR",
        "NOT_FOUND",              "AUDIO_TOO_LONG",        "AUDIO_TOO_SHORT",       "AUDIO_NOT_WAV",
        "AUDIO_NOT_OGG",          "AUDIO_MALFORMED",       "AUDIO_FORMAT_UNSUPPORTED", "AUDIO_WAV_REQUIRED",
        "LANGUAGE_UNSUPPORTED",   "TOO_MANY_MESSAGES",     "JSON_NOT_ALLOWED",      "FILE_INCOMPLETE",
        "FILE_SIZE_MISMATCH",     "FILE_HASH_MISMATCH",    "FILE_CONFLICT",         "FILE_GONE",
        "FILE_BUSY",              "FILE_NOT_FOUND",        "FILE_NOT_STORED",       "FILE_TOO_LARGE",
        "FILE_KIND_NOT_ALLOWED",  "CONTENT_TYPE_NOT_ALLOWED", "RETENTION_NOT_ALLOWED", "DURATION_REQUIRED",
        "STORAGE_LIMIT",          "TRANSCRIPT_BUSY",       "TRANSCRIPT_NOT_READY",  "VERSION_CONFLICT",
        "DATA_TOO_LARGE",         "DATA_NOT_FOUND",        "COLLECTION_FULL",       "COLLECTION_NOT_FOUND",
        "RECORDS_DISABLED",       "RECORD_KIND_NOT_ALLOWED", "SETTING_UNKNOWN",     "SETTINGS_TOO_LARGE",
        "APP_MISMATCH",           "BOARD_REQUIRED",        "TEMPLATE_NOT_ALLOWED",  "TEMPLATE_VARS_INVALID",
        "TEXT_TOO_LONG",          "FORMAT_UNSUPPORTED",    "VOICE_UNSUPPORTED",     "SAMPLE_RATE_UNSUPPORTED",
        "TARGET_NOT_ALLOWED",     "TARGET_NOT_CONNECTED",  "NOTION_PARENT_REQUIRED", "NOTHING_TO_DELIVER",
        "OWNER_EMAIL_MISSING",    "DELIVERY_NOT_FOUND",    "DELIVERY_BUSY",         "RETRY_NEEDS_TEXT",
        "CONNECTION_INACTIVE",    "CONNECTOR_SCOPE_REQUIRED", "TEXT_EMPTY",           "DELIVERIES_WAITING",
        "RETRY_TOO_SOON",         "RETRY_LIMIT",
    };
    if (error)
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
            if (!strcmp(error, known[i])) return known[i];
    return "SERVER";
}

const char *yk_api_error_code(const char *json) {
    cJSON *refusal = cJSON_Parse(json ? json : "");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(refusal, "error");
    const char *code = yk_api_code(cJSON_IsString(error) ? error->valuestring : NULL);
    cJSON_Delete(refusal);
    return code;
}

/** A copy of `s` (strdup is POSIX, not C11). */
static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static const cJSON *item(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}
static int number(const cJSON *object, const char *name, int fallback) {
    const cJSON *n = item(object, name);
    return cJSON_IsNumber(n) ? (int)n->valuedouble : fallback;
}
static int64_t big(const cJSON *object, const char *name, int64_t fallback) {
    const cJSON *n = item(object, name);
    return cJSON_IsNumber(n) ? (int64_t)n->valuedouble : fallback;
}
static void text(const cJSON *object, const char *name, char *out, size_t size) {
    const cJSON *s = item(object, name);
    if (size) out[0] = 0;
    if (cJSON_IsString(s) && s->valuestring) snprintf(out, size, "%s", s->valuestring);
}

bool yk_api_me(const char *json, yk_cloud_me_t *out, cJSON **capabilities, cJSON **settings) {
    memset(out, 0, sizeof(*out));
    if (capabilities) *capabilities = NULL;
    if (settings) *settings = NULL;
    cJSON *me = cJSON_Parse(json ? json : "");
    if (!cJSON_IsObject(me)) {
        cJSON_Delete(me);
        return false;
    }
    const cJSON *credits = item(me, "credits");
    out->remaining = number(credits, "remaining", 0);
    out->daily_limit = number(credits, "dailyLimit", 0);
    out->daily_remaining = number(credits, "dailyRemaining", 0);
    text(credits, "resetAt", out->reset_at, sizeof(out->reset_at));
    const cJSON *owner = item(me, "owner");
    text(owner, "nickname", out->nickname, sizeof(out->nickname));
    text(owner, "locale", out->locale, sizeof(out->locale));
    text(owner, "timezone", out->timezone, sizeof(out->timezone));
    out->api_version = number(me, "apiVersion", 0);
    const cJSON *storage = item(me, "storage");
    out->storage_used = big(storage, "usedBytes", 0);
    out->storage_limit = big(storage, "limitBytes", 0);
    out->settings_version = number(me, "settingsVersion", 0);
    out->records_enabled = cJSON_IsTrue(item(item(me, "records"), "enabled"));
    text(me, "minFirmware", out->min_firmware, sizeof(out->min_firmware));
    if (capabilities) {
        const cJSON *caps = item(me, "capabilities");
        *capabilities = cJSON_IsObject(caps) ? cJSON_Duplicate(caps, true) : cJSON_CreateObject();
    }
    if (settings) {
        const cJSON *set = item(me, "settings");
        *settings = cJSON_IsObject(set) ? cJSON_Duplicate(set, true) : cJSON_CreateObject();
    }
    cJSON_Delete(me);
    return true;
}

bool yk_api_has(const cJSON *capabilities, const char *capability) {
    return cJSON_IsObject(item(capabilities, capability));
}

int yk_api_limit(const cJSON *capabilities, const char *capability, const char *key, int fallback) {
    return number(item(capabilities, capability), key, fallback);
}

bool yk_api_allows(const cJSON *capabilities, const char *capability, const char *list, const char *value) {
    const cJSON *values = item(item(capabilities, capability), list), *v;
    if (!cJSON_IsArray(values) || !value) return false;
    cJSON_ArrayForEach(v, values) {
        if (cJSON_IsString(v) && !strcmp(v->valuestring, value)) return true;
    }
    return false;
}

void yk_api_charge(const cJSON *credits, yk_cloud_charge_t *out) {
    memset(out, 0, sizeof(*out));
    out->charged = number(credits, "charged", 0);
    out->remaining = number(credits, "remaining", 0);
    out->daily_limit = number(credits, "dailyLimit", 0);
    out->daily_remaining = number(credits, "dailyRemaining", 0);
    text(credits, "resetAt", out->reset_at, sizeof(out->reset_at));
}

bool yk_api_asr(const char *json, yk_cloud_asr_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *heard = cJSON_Parse(json ? json : "");
    if (!cJSON_IsObject(heard) || !cJSON_IsString(item(heard, "text"))) {
        cJSON_Delete(heard);
        return false;
    }
    text(heard, "text", out->text, sizeof(out->text));
    const cJSON *seconds = item(heard, "seconds");
    out->seconds = cJSON_IsNumber(seconds) ? (float)seconds->valuedouble : 0;
    text(heard, "language", out->language, sizeof(out->language));
    yk_api_charge(item(heard, "credits"), &out->credits);
    cJSON_Delete(heard);
    return true;
}

char *yk_api_chat_body(const yk_cloud_chat_t *request) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "tier", request->tier ? request->tier : "fast");
    if (request->template_name) {
        // A template brings its own prompt; only its variables go.
        cJSON_AddStringToObject(body, "template", request->template_name);
        if (request->var_count) {
            cJSON *vars = cJSON_AddObjectToObject(body, "vars");
            for (size_t i = 0; i < request->var_count; i++)
                cJSON_AddStringToObject(vars, request->var_names[i], request->var_values[i]);
        }
    } else {
        cJSON *messages = cJSON_AddArrayToObject(body, "messages");
        for (size_t i = 0; i < request->count; i++) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", request->messages[i].role);
            cJSON_AddStringToObject(m, "content", request->messages[i].content);
            cJSON_AddItemToArray(messages, m);
        }
    }
    if (request->max_tokens > 0) cJSON_AddNumberToObject(body, "maxTokens", request->max_tokens);
    if (request->json) cJSON_AddBoolToObject(body, "json", true);
    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

bool yk_api_chat_event(const char *event, const cJSON *data, yk_cloud_delta_fn on_delta, void *ctx,
                       yk_cloud_chat_done_t *done, const char **code) {
    if (!strcmp(event, "delta")) {
        const cJSON *t = item(data, "text");
        if (on_delta && cJSON_IsString(t)) on_delta(t->valuestring, ctx);
    } else if (!strcmp(event, "done")) {
        if (done) {
            memset(done, 0, sizeof(*done));
            const cJSON *usage = item(data, "usage");
            done->input_tokens = number(usage, "inputTokens", 0);
            done->output_tokens = number(usage, "outputTokens", 0);
            yk_api_charge(item(data, "credits"), &done->credits);
        }
    } else if (!strcmp(event, "error")) {
        const cJSON *e = item(data, "error");
        if (code) *code = yk_api_code(cJSON_IsString(e) ? e->valuestring : NULL);
        return false;
    }
    // Events this firmware does not know are skipped.
    return true;
}

char *yk_api_tts_body(const char *text, const yk_cloud_tts_t *options) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "text", text);
    if (options && options->voice) cJSON_AddStringToObject(body, "voice", options->voice);
    cJSON_AddStringToObject(body, "format", options && options->format ? options->format : "opus");
    if (options && options->sample_rate) cJSON_AddNumberToObject(body, "sampleRate", options->sample_rate);
    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

size_t yk_api_b64_decode(const char *in, uint8_t *out, size_t size) {
    size_t n = 0, len = strlen(in);
    if (len % 4) return 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A'
                : c >= 'a' && c <= 'z' ? c - 'a' + 26
                : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '+' ? 62
                : c == '/' ? 63
                : c == '=' && i >= len - 2 ? -2
                : -1;
        if (v == -1) return 0;
        if (v == -2) break;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= size) return 0;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

bool yk_api_tts_event(const char *event, const cJSON *data, yk_cloud_audio_fn on_audio, void *ctx,
                      yk_cloud_charge_t *credits, const char **code) {
    if (!strcmp(event, "audio")) {
        const cJSON *b64 = item(data, "data");
        // Decoded over its own string (the bytes are shorter than their base64): no buffer on the stack,
        // whatever the piece's size (an Opus packet is at most 3,828 bytes; 60 ms of 24 kHz PCM is 2,880).
        if (cJSON_IsString(b64)) {
            uint8_t *bytes = (uint8_t *)b64->valuestring;
            size_t len = yk_api_b64_decode(b64->valuestring, bytes, strlen(b64->valuestring));
            if (len && on_audio) on_audio(number(data, "seq", 0), bytes, len, ctx);
        }
    } else if (!strcmp(event, "done")) {
        if (credits) yk_api_charge(item(data, "credits"), credits);
    } else if (!strcmp(event, "error")) {
        const cJSON *e = item(data, "error");
        if (code) *code = yk_api_code(cJSON_IsString(e) ? e->valuestring : NULL);
        return false;
    }
    return true;
}

const char *yk_api_stream_end(bool done, const char *error) {
    if (error) return error;
    // The connection stopped before the done event: the answer (or the speech) is not whole.
    return done ? NULL : "NETWORK";
}

char *yk_api_file_body(const yk_cloud_file_info_t *info) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", info->id);
    cJSON_AddStringToObject(body, "kind", info->kind);
    cJSON_AddStringToObject(body, "contentType", info->content_type);
    cJSON_AddNumberToObject(body, "size", (double)info->size);
    cJSON_AddStringToObject(body, "sha256", info->sha256);
    if (info->duration_ms >= 0) cJSON_AddNumberToObject(body, "durationMs", (double)info->duration_ms);
    if (info->speech_ms >= 0) cJSON_AddNumberToObject(body, "speechMs", (double)info->speech_ms);
    if (info->recorded_at) {
        cJSON_AddStringToObject(body, "recordedAt", info->recorded_at);
        cJSON_AddBoolToObject(body, "clockTrusted", info->clock_trusted);
    }
    if (info->retention) cJSON_AddStringToObject(body, "retention", info->retention);
    if (info->recording_id) {
        cJSON *r = cJSON_AddObjectToObject(body, "recording");
        cJSON_AddStringToObject(r, "id", info->recording_id);
        cJSON_AddNumberToObject(r, "segment", info->segment > 0 ? info->segment : 1);
    }
    if (info->transcribe) {
        cJSON *t = cJSON_AddObjectToObject(body, "transcribe");
        cJSON_AddBoolToObject(t, "enabled", true);
        if (info->language) cJSON_AddStringToObject(t, "language", info->language);
    }
    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

void yk_cloud_file_free(yk_cloud_file_t *file) {
    for (int i = 0; i < file->nparts; i++) {
        free(file->parts[i].url);
        file->parts[i].url = NULL;
    }
    file->nparts = 0;
}

bool yk_api_file(const char *json, yk_cloud_file_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *answer = cJSON_Parse(json ? json : "");
    const cJSON *file = item(answer, "file");
    if (!cJSON_IsObject(file)) {
        cJSON_Delete(answer);
        return false;
    }
    text(file, "id", out->id, sizeof(out->id));
    text(file, "state", out->state, sizeof(out->state));
    out->size = big(file, "size", 0);
    out->received = big(file, "receivedBytes", 0);
    const cJSON *transcript = item(file, "transcript");
    if (cJSON_IsObject(transcript)) {
        text(transcript, "state", out->transcript, sizeof(out->transcript));
        text(transcript, "error", out->transcript_error, sizeof(out->transcript_error));
        out->charged = number(transcript, "charged", 0);
    }
    const cJSON *upload = item(answer, "upload");
    if (cJSON_IsObject(upload)) {
        out->has_upload = true;
        out->part_bytes = number(upload, "partBytes", 0);
        out->part_count = number(upload, "partCount", 0);
        out->received_parts = cJSON_GetArraySize(item(upload, "receivedParts"));
        text(upload, "expiresAt", out->expires_at, sizeof(out->expires_at));
        const cJSON *part;
        cJSON_ArrayForEach(part, item(upload, "parts")) {
            const cJSON *url = item(part, "url");
            if (out->nparts >= YK_CLOUD_PARTS || !cJSON_IsString(url)) continue;
            yk_cloud_part_t *p = &out->parts[out->nparts];
            p->number = number(part, "number", 0);
            p->offset = big(part, "offset", 0);
            p->bytes = big(part, "bytes", 0);
            p->url = copy(url->valuestring);
            if (p->url) out->nparts++;
        }
    }
    const cJSON *credits = item(answer, "credits");
    if (cJSON_IsObject(credits)) {
        out->has_credits = true;
        yk_api_charge(credits, &out->credits);
    }
    cJSON_Delete(answer);
    return true;
}

char *yk_api_deliver_body(const yk_cloud_delivery_info_t *info) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", info->id);
    cJSON_AddStringToObject(body, "target", info->target);
    if (info->file_id) cJSON_AddStringToObject(body, "fileId", info->file_id);
    if (info->text) cJSON_AddStringToObject(body, "text", info->text);
    if (info->title) cJSON_AddStringToObject(body, "title", info->title);
    if (info->notion_parent) {
        cJSON *notion = cJSON_AddObjectToObject(body, "notion");
        cJSON_AddStringToObject(notion, "parentId", info->notion_parent);
    }
    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

bool yk_api_delivery(const char *json, yk_cloud_delivery_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *answer = cJSON_Parse(json ? json : "");
    const cJSON *d = item(answer, "delivery");
    bool ok = cJSON_IsObject(d);
    if (ok) {
        text(d, "id", out->id, sizeof(out->id));
        text(d, "target", out->target, sizeof(out->target));
        text(d, "state", out->state, sizeof(out->state));
        text(d, "error", out->error, sizeof(out->error));
        text(d, "externalUrl", out->external_url, sizeof(out->external_url));
        out->tries = number(d, "tries", 1);
    }
    cJSON_Delete(answer);
    return ok;
}

void yk_cloud_data_free(yk_cloud_data_item_t *data) {
    cJSON_Delete(data->value);
    data->value = NULL;
}

bool yk_api_data_item(const cJSON *data, yk_cloud_data_item_t *out) {
    memset(out, 0, sizeof(*out));
    const cJSON *value = item(data, "value");
    if (!cJSON_IsObject(data) || !cJSON_IsString(item(data, "key")) || !cJSON_IsObject(value)) return false;
    text(data, "key", out->key, sizeof(out->key));
    out->version = number(data, "version", 0);
    text(data, "updatedAt", out->updated_at, sizeof(out->updated_at));
    out->value = cJSON_Duplicate(value, true);
    return out->value != NULL;
}

char *yk_api_data_body(const cJSON *value, int version) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "value", cJSON_Duplicate(value, true));
    if (version >= 0) cJSON_AddNumberToObject(body, "version", version);
    char *out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

bool yk_api_inbox(const char *json, yk_cloud_inbox_fn on_item, void *ctx, char *now, size_t now_size) {
    cJSON *box = cJSON_Parse(json ? json : "");
    if (!cJSON_IsObject(box) || !cJSON_IsArray(item(box, "items"))) {
        cJSON_Delete(box);
        return false;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item(box, "items")) {
        const cJSON *id = item(entry, "id"), *kind = item(entry, "kind");
        if (on_item && cJSON_IsString(id) && cJSON_IsString(kind))
            on_item(id->valuestring, kind->valuestring, item(entry, "data"), ctx);
    }
    if (now) text(box, "now", now, now_size);
    cJSON_Delete(box);
    return true;
}

int yk_api_error_number(const char *json, const char *key, int fallback) {
    cJSON *refusal = cJSON_Parse(json ? json : "");
    int n = number(refusal, key, fallback);
    cJSON_Delete(refusal);
    return n;
}

bool yk_api_https(const char *url) {
    if (!url || strncmp(url, "https://", 8) != 0) return false;
    const char *host = url + 8;
    // A host of at least one character before the path, and nothing that could end the header line.
    if (!*host || *host == '/' || *host == ':') return false;
    for (const char *c = url; *c; c++)
        if (*c == '\r' || *c == '\n' || *c == ' ') return false;
    return true;
}

size_t yk_api_escape(const char *value, char *out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
        bool plain = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
                     *c == '_' || *c == '.' || *c == '~';
        if (n + (plain ? 1 : 3) >= size) return 0;
        if (plain) {
            out[n++] = (char)*c;
        } else {
            out[n++] = '%';
            out[n++] = hex[*c >> 4];
            out[n++] = hex[*c & 15];
        }
    }
    if (n >= size) return 0;
    out[n] = 0;
    return n;
}

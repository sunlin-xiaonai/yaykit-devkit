#include "yk_proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

// ---------------------------------------------------------------- small checks (the zod schemas in device.ts)

static bool alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool yk_proto_turn_ok(const char *t) {
    if (!t) return false;
    const size_t n = strlen(t);
    if (n != YK_PROTO_TURN_LEN) return false;
    const size_t from = (t[0] == 's' && t[1] == '-') ? 2 : 0;
    for (size_t i = from; i < n; i++)
        if (!alnum(t[i])) return false;
    return true;
}

bool yk_proto_server_turn(const char *t) { return t && t[0] == 's' && t[1] == '-'; }

void yk_proto_new_turn(char out[YK_PROTO_TURN_LEN + 1], const uint8_t random[16]) {
    static const char set[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < YK_PROTO_TURN_LEN; i++) out[i] = set[random[i] % 62];
    if (out[0] == 's' && out[1] == '-') out[1] = 'x';  // cannot happen ('-' is not in the set), kept explicit
    out[YK_PROTO_TURN_LEN] = 0;
}

bool yk_proto_slug_ok(const char *s) {
    if (!s) return false;
    const size_t n = strlen(s);
    if (n < 2 || n > 31 || s[0] < 'a' || s[0] > 'z') return false;
    for (size_t i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return false;
    return true;
}

/** Characters (code points; the same as UTF-16 units for Chinese) in UTF-8 text. */
static size_t chars(const char *s) {
    size_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }

static bool str_in(const cJSON *v, size_t min, size_t max) {
    if (!cJSON_IsString(v) || !v->valuestring) return false;
    const size_t n = chars(v->valuestring);
    return n >= min && n <= max;
}

static bool is_int(const cJSON *v, double min, double max) {
    return cJSON_IsNumber(v) && v->valuedouble >= min && v->valuedouble <= max && floor(v->valuedouble) == v->valuedouble;
}

static bool one_of(const char *s, const char *const *set) {
    for (; *set; set++)
        if (!strcmp(s, *set)) return true;
    return false;
}

static bool enum_str(const cJSON *v, const char *const *set) { return cJSON_IsString(v) && one_of(v->valuestring, set); }

/** z.iso.datetime(): 2026-10-18T16:00:00Z, fractions allowed, no offset. */
static bool datetime_ok(const cJSON *v) {
    if (!cJSON_IsString(v)) return false;
    const char *s = v->valuestring;
    static const char shape[] = "dddd-dd-ddTdd:dd:dd";
    for (int i = 0; shape[i]; i++) {
        if (shape[i] == 'd' ? !(s[i] >= '0' && s[i] <= '9') : s[i] != shape[i]) return false;
    }
    s += sizeof(shape) - 1;
    if (*s == '.') {
        s++;
        if (!(*s >= '0' && *s <= '9')) return false;
        while (*s >= '0' && *s <= '9') s++;
    }
    return s[0] == 'Z' && s[1] == 0;
}

static void copy(char *out, size_t size, const cJSON *v) {
    snprintf(out, size, "%s", cJSON_IsString(v) && v->valuestring ? v->valuestring : "");
}

static char *dup(const cJSON *v) {
    const char *s = cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
    const size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static const char *const YAY_STATES[] = {"ready", "listening", "thinking", "working", "approval", "done",
                                         "reminder", "help", "resting", "speaking", "ack", NULL};
static const char *const SLOT_STATES[] = {"empty", "valid", "pending", "invalid", "aborted", "new", "unknown", NULL};
static const char *const MODES[] = {"chat", "practice", "translate", NULL};
static const char *const TALK_LANGUAGES[] = {"zh", "en", "ja", "es", NULL};
static const char *const LEVELS[] = {"beginner", "everyday", "fluent", NULL};

bool yk_proto_talk_language_ok(const char *lang) { return lang && one_of(lang, TALK_LANGUAGES); }

bool yk_proto_pair_ok(const char *pair) {
    if (!pair || strlen(pair) != 5 || pair[2] != '-' || !strncmp(pair, pair + 3, 2)) return false;
    const char mine[3] = {pair[0], pair[1], 0};
    return yk_proto_talk_language_ok(mine) && yk_proto_talk_language_ok(pair + 3);
}

/** ^[A-Za-z0-9._:-]{4,80}$ (a record id) or ^[a-z][a-z0-9.]{2,40}$ (a record or action kind). */
static bool record_word(const char *s, bool id) {
    const size_t n = s ? strlen(s) : 0;
    if (id ? (n < 4 || n > 80) : (n < 3 || n > 41 || s[0] < 'a' || s[0] > 'z')) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        const bool ok = id ? alnum(c) || c == '.' || c == '_' || c == ':' || c == '-'
                           : (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.';
        if (!ok) return false;
    }
    return true;
}

// ---------------------------------------------------------------- device to cloud

static char *finish(cJSON *o) {
    char *s = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return s;
}

static bool text_ok(const char *s, size_t min, size_t max) {
    if (!s) return false;
    const size_t n = chars(s);
    return n >= min && n <= max;
}

static bool screen_off_ok(int s) { return s == 0 || (s >= 15 && s <= 3600); }
static bool prefs_ok(int volume, int brightness, int screen_off_s) {
    return volume >= 0 && volume <= 10 && brightness >= 0 && brightness <= 10 && screen_off_ok(screen_off_s);
}
static void add_prefs(cJSON *o, int volume, int brightness, int screen_off_s) {
    cJSON_AddNumberToObject(o, "volume", volume);
    cJSON_AddNumberToObject(o, "brightness", brightness);
    cJSON_AddNumberToObject(o, "screenOff", screen_off_s);
}

static bool hex_digits(const char *s, size_t n, bool upper) {
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (upper && s[i] >= 'A' && s[i] <= 'F')))
            return false;
    return true;
}
/** ^[0-9a-f]{64}$ */
static bool sha256_ok(const char *s) { return s && strlen(s) == 64 && hex_digits(s, 64, false); }

bool yk_proto_saver_image_ok(const char *id) {
    if (!id) return false;
    const size_t n = strlen(id);
    if (n == 10) return !strncmp(id, "default-", 8) && id[8] >= '0' && id[8] <= '9' && id[9] >= '0' && id[9] <= '9';
    if (n != 36) return false;
    for (size_t i = 0; i < n; i++) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? id[i] != '-' : !hex_digits(id + i, 1, true)) return false;
    }
    return true;
}

static bool saver_ok(const yk_proto_saver_t *v) {
    return v && v->after_s >= 15 && v->after_s <= 3600 && v->duration_s >= 60 && v->duration_s <= 7200 &&
           yk_proto_saver_image_ok(v->image_id) && sha256_ok(v->image_sha256);
}
static void add_saver(cJSON *o, const yk_proto_saver_t *v) {
    cJSON *s = cJSON_AddObjectToObject(o, "screensaver");
    cJSON_AddBoolToObject(s, "on", v->on);
    cJSON_AddNumberToObject(s, "after", v->after_s);
    cJSON_AddNumberToObject(s, "duration", v->duration_s);
    cJSON *image = cJSON_AddObjectToObject(s, "image");
    cJSON_AddStringToObject(image, "id", v->image_id);
    cJSON_AddStringToObject(image, "sha256", v->image_sha256);
}
/** ^[a-z][a-z_.]{2,39}$ */
static bool cap_ok(const char *c) {
    const size_t n = c ? strlen(c) : 0;
    if (n < 3 || n > 40 || c[0] < 'a' || c[0] > 'z') return false;
    for (size_t i = 1; i < n; i++)
        if (!((c[i] >= 'a' && c[i] <= 'z') || c[i] == '_' || c[i] == '.')) return false;
    return true;
}

char *yk_proto_hello(const yk_proto_hello_t *h) {
    if (!h || !text_ok(h->fw, 1, 40) || !text_ok(h->board, 1, 40) || !text_ok(h->locale, 2, 20) ||
        !text_ok(h->tz, 1, 64) || h->slot_count < 0 || h->slot_count > 4 || h->battery_pct < 0 ||
        h->battery_pct > 100 || h->rssi < -127 || h->rssi > 0 ||
        (h->out_rate != 16000 && h->out_rate != 24000 && h->out_rate != 48000) ||
        (h->mode && !one_of(h->mode, MODES)) || (h->pair && !yk_proto_pair_ok(h->pair)) ||
        (!h->practice_language != !h->practice_level) ||
        (h->practice_language && (!yk_proto_talk_language_ok(h->practice_language) || !one_of(h->practice_level, LEVELS))))
        return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "hello");
    cJSON_AddStringToObject(o, "fw", h->fw);
    cJSON_AddStringToObject(o, "board", h->board);
    cJSON *slots = cJSON_AddArrayToObject(o, "slots");
    for (int i = 0; i < h->slot_count; i++) {
        const yk_proto_slot_t *s = &h->slots[i];
        const bool has_slug = s->slug && s->slug[0];
        if (s->slot < 1 || s->slot > 3 || !s->state || !one_of(s->state, SLOT_STATES) ||
            (has_slug && !yk_proto_slug_ok(s->slug)) || (s->version && strlen(s->version) > 31) || s->last_used_ms < 0) {
            cJSON_Delete(o);
            return NULL;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "slot", s->slot);
        if (has_slug) cJSON_AddStringToObject(e, "slug", s->slug);
        else cJSON_AddNullToObject(e, "slug");
        if (s->version && s->version[0]) cJSON_AddStringToObject(e, "version", s->version);
        else cJSON_AddNullToObject(e, "version");
        cJSON_AddStringToObject(e, "state", s->state);
        cJSON_AddNumberToObject(e, "lastUsed", (double)s->last_used_ms);
        cJSON_AddItemToArray(slots, e);
    }
    cJSON *battery = cJSON_AddObjectToObject(o, "battery");
    cJSON_AddNumberToObject(battery, "pct", h->battery_pct);
    cJSON_AddBoolToObject(battery, "charging", h->charging);
    cJSON_AddNumberToObject(cJSON_AddObjectToObject(o, "wifi"), "rssi", h->rssi);
    cJSON_AddStringToObject(o, "locale", h->locale);
    cJSON_AddStringToObject(o, "tz", h->tz);
    cJSON *audio = cJSON_AddObjectToObject(o, "audio");
    cJSON_AddNumberToObject(audio, "inRate", 16000);
    cJSON_AddNumberToObject(audio, "outRate", h->out_rate);
    cJSON_AddStringToObject(audio, "codec", "opus");
    if (h->mode) cJSON_AddStringToObject(o, "mode", h->mode);
    if (h->practice_language) {
        cJSON *practice = cJSON_AddObjectToObject(o, "practice");
        cJSON_AddStringToObject(practice, "language", h->practice_language);
        cJSON_AddStringToObject(practice, "level", h->practice_level);
    }
    if (h->pair) cJSON_AddStringToObject(o, "pair", h->pair);
    if (h->has_prefs) {
        if (!prefs_ok(h->volume, h->brightness, h->screen_off_s) || (h->saver && !saver_ok(h->saver))) {
            cJSON_Delete(o);
            return NULL;
        }
        cJSON *prefs = cJSON_AddObjectToObject(o, "prefs");
        add_prefs(prefs, h->volume, h->brightness, h->screen_off_s);
        if (h->saver) add_saver(prefs, h->saver);
    }
    if (h->caps) {
        if (h->cap_count < 0 || h->cap_count > 32) {
            cJSON_Delete(o);
            return NULL;
        }
        cJSON *caps = cJSON_AddArrayToObject(o, "caps");
        for (int i = 0; i < h->cap_count; i++) {
            if (!cap_ok(h->caps[i])) {
                cJSON_Delete(o);
                return NULL;
            }
            cJSON_AddItemToArray(caps, cJSON_CreateString(h->caps[i]));
        }
    }
    return finish(o);
}

char *yk_proto_listen_start(const char *turn, int preroll_ms, const char *side) {
    return yk_proto_listen_start_ex(turn, preroll_ms, side, false);
}

char *yk_proto_listen_start_ex(const char *turn, int preroll_ms, const char *side, bool held) {
    static const char *const sides[] = {"mine", "theirs", NULL};
    if (!yk_proto_turn_ok(turn) || preroll_ms < 0 || preroll_ms > 1000 || (side && !one_of(side, sides))) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "listen.start");
    cJSON_AddStringToObject(o, "turn", turn);
    cJSON_AddNumberToObject(o, "preroll", preroll_ms);
    if (side) cJSON_AddStringToObject(o, "side", side);
    if (held) cJSON_AddTrueToObject(o, "held");
    return finish(o);
}

char *yk_proto_turn_message(const char *type, const char *turn) {
    static const char *const types[] = {"listen.stop", "listen.cancel", "playback.stop", NULL};
    if (!type || !one_of(type, types) || !yk_proto_turn_ok(turn)) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddStringToObject(o, "turn", turn);
    return finish(o);
}

// kind: sent again in a later session, where the id is not known; value: a setting's value after it (< 0: none).
static char *action_done(const char *id, bool ok, const char *error, const char *kind, int value) {
    if (!text_ok(id, 1, 40) || (error && !text_ok(error, 0, 60)) || (kind && !record_word(kind, false)) ||
        value > 3600)
        return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "action.done");
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddBoolToObject(o, "ok", ok);
    if (error) cJSON_AddStringToObject(o, "error", error);
    if (kind) cJSON_AddStringToObject(o, "kind", kind);
    if (value >= 0) cJSON_AddNumberToObject(o, "value", value);
    return finish(o);
}

char *yk_proto_action_done(const char *id, bool ok, const char *error) {
    return action_done(id, ok, error, NULL, -1);
}

char *yk_proto_action_done_kind(const char *id, bool ok, const char *error, const char *kind) {
    return action_done(id, ok, error, kind, -1);
}

char *yk_proto_action_done_value(const char *id, bool ok, const char *error, int value) {
    return action_done(id, ok, error, NULL, value);
}

char *yk_proto_action_done_saver(const char *id, bool ok, const char *error, int value, const yk_proto_saver_t *saver) {
    if (!saver) return action_done(id, ok, error, NULL, value);
    if (!saver_ok(saver)) return NULL;
    char *done = action_done(id, ok, error, NULL, value);
    cJSON *o = done ? cJSON_Parse(done) : NULL;
    cJSON_free(done);
    if (!o) return NULL;
    add_saver(o, saver);
    return finish(o);
}

char *yk_proto_prefs(int volume, int brightness, int screen_off_s) {
    return yk_proto_prefs_saver(volume, brightness, screen_off_s, NULL);
}

char *yk_proto_prefs_saver(int volume, int brightness, int screen_off_s, const yk_proto_saver_t *saver) {
    if (!prefs_ok(volume, brightness, screen_off_s) || (saver && !saver_ok(saver))) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "prefs");
    add_prefs(o, volume, brightness, screen_off_s);
    if (saver) add_saver(o, saver);
    return finish(o);
}

char *yk_proto_app_result(const char *slug, const char *data_json) {
    if (!yk_proto_slug_ok(slug)) return NULL;
    cJSON *data = data_json ? cJSON_Parse(data_json) : cJSON_CreateObject();
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "app.result");
    cJSON_AddStringToObject(o, "slug", slug);
    cJSON_AddItemToObject(o, "data", data);
    return finish(o);
}

char *yk_proto_status(int battery_pct, bool charging, int rssi, uint32_t heap_min) {
    if (battery_pct < 0 || battery_pct > 100 || rssi < -127 || rssi > 0) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "status");
    cJSON *battery = cJSON_AddObjectToObject(o, "battery");
    cJSON_AddNumberToObject(battery, "pct", battery_pct);
    cJSON_AddBoolToObject(battery, "charging", charging);
    cJSON_AddNumberToObject(cJSON_AddObjectToObject(o, "wifi"), "rssi", rssi);
    cJSON_AddNumberToObject(o, "heapMin", heap_min);
    return finish(o);
}

char *yk_proto_auth(const char *token) {
    if (!token || !token[0] || strlen(token) > 4096) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "auth");
    cJSON_AddStringToObject(o, "token", token);
    return finish(o);
}

/** {"type":type, key:value}: the one-field messages of the modes. */
static char *one_field(const char *type, const char *key, const char *value) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    if (key) cJSON_AddStringToObject(o, key, value);
    return finish(o);
}

char *yk_proto_pair_set(const char *pair) { return yk_proto_pair_ok(pair) ? one_field("pair.set", "pair", pair) : NULL; }

char *yk_proto_translate_text(const char *turn, const char *text, const char *from) {
    if (!yk_proto_turn_ok(turn) || !text_ok(text, 1, 500) || !yk_proto_talk_language_ok(from)) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "translate.text");
    cJSON_AddStringToObject(o, "turn", turn);
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddStringToObject(o, "from", from);
    return finish(o);
}

char *yk_proto_practice_open(const char *why) {
    static const char *const whys[] = {"start", "settings", "resume", NULL};
    return why && one_of(why, whys) ? one_field("practice.open", "why", why) : NULL;
}

char *yk_proto_practice_set(const char *language, const char *level) {
    if (!yk_proto_talk_language_ok(language) || !level || !one_of(level, LEVELS)) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "practice.set");
    cJSON_AddStringToObject(o, "language", language);
    cJSON_AddStringToObject(o, "level", level);
    return finish(o);
}

char *yk_proto_practice_next(void) { return one_field("practice.next", NULL, NULL); }

char *yk_proto_practice_end(const char *why) {
    static const char *const whys[] = {"double", "facedown", "idle", "home", NULL};
    return why && one_of(why, whys) ? one_field("practice.end", "why", why) : NULL;
}

/** One record as POST /records takes it, from the outbox's JSON; NULL when it breaks the schema. */
static cJSON *carried_record(const char *json) {
    cJSON *in = json ? cJSON_Parse(json) : NULL;
    const cJSON *id = get(in, "id"), *kind = get(in, "kind"), *at = get(in, "occurredAt"),
                *trusted = get(in, "clockTrusted"), *data = get(in, "data");
    cJSON *out = NULL;
    if (cJSON_IsString(id) && record_word(id->valuestring, true) && cJSON_IsString(kind) &&
        record_word(kind->valuestring, false) && datetime_ok(at) && (!trusted || cJSON_IsBool(trusted)) &&
        cJSON_IsObject(data)) {
        out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "id", id->valuestring);
        cJSON_AddStringToObject(out, "kind", kind->valuestring);
        cJSON_AddStringToObject(out, "occurredAt", at->valuestring);
        if (trusted) cJSON_AddBoolToObject(out, "clockTrusted", cJSON_IsTrue(trusted));
        cJSON_AddItemToObject(out, "data", cJSON_Duplicate(data, true));
    }
    cJSON_Delete(in);
    return out;
}

char *yk_proto_app_records(const char *slug, const char *const *records, int count) {
    if (!yk_proto_slug_ok(slug) || !records || count < 1 || count > YK_PROTO_RECORDS_MAX) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "app.records");
    cJSON_AddStringToObject(o, "slug", slug);
    cJSON *list = cJSON_AddArrayToObject(o, "records");
    for (int i = 0; i < count; i++) {
        cJSON *r = carried_record(records[i]);
        if (!r) {
            cJSON_Delete(o);
            return NULL;
        }
        cJSON_AddItemToArray(list, r);
    }
    return finish(o);
}

// ---------------------------------------------------------------- cloud to device

static bool turn_field(const cJSON *o, yk_srv_msg_t *m, bool required) {
    const cJSON *t = get(o, "turn");
    if (!t) return !required;
    if (!cJSON_IsString(t) || !yk_proto_turn_ok(t->valuestring)) return false;
    copy(m->turn, sizeof(m->turn), t);
    return true;
}

static bool credits_field(const cJSON *c, yk_srv_msg_t *m) {
    const cJSON *used = get(c, "used"), *left = get(c, "remaining");
    if (!cJSON_IsObject(c) || !cJSON_IsNumber(used) || used->valuedouble < 0 || !is_int(left, 0, 2147483647))
        return false;
    m->has_credits = true;
    m->credits_used = used->valuedouble;
    m->credits_remaining = (int)left->valuedouble;
    return true;
}

static bool nullable_str(const cJSON *v, size_t max, char *out, size_t size) {
    if (cJSON_IsNull(v)) {
        out[0] = 0;
        return true;
    }
    if (!str_in(v, 0, max)) return false;
    copy(out, size, v);
    return true;
}

/** stt.final, reply.line: a language this firmware does not know (a newer server) is left out, not refused. */
static void lang_field(const cJSON *v, yk_srv_msg_t *m) {
    if (cJSON_IsString(v) && (yk_proto_talk_language_ok(v->valuestring) || !strcmp(v->valuestring, "other")))
        copy(m->lang, sizeof(m->lang), v);
}

/** practice.turn fix {said, better, spans, note}. */
static bool practice_fix(const cJSON *fix, yk_srv_msg_t *m) {
    const cJSON *said = get(fix, "said"), *better = get(fix, "better"), *spans = get(fix, "spans"),
                *note = get(fix, "note");
    if (!cJSON_IsObject(fix) || !str_in(said, 1, 300) || !str_in(better, 1, 300) || !cJSON_IsArray(spans) ||
        cJSON_GetArraySize(spans) > 8 || !str_in(note, 0, 24))
        return false;
    const cJSON *span;
    cJSON_ArrayForEach(span, spans) {
        const cJSON *from = cJSON_GetArrayItem(span, 0), *to = cJSON_GetArrayItem(span, 1);
        if (!cJSON_IsArray(span) || cJSON_GetArraySize(span) != 2 || !is_int(from, 0, 65535) || !is_int(to, 1, 65535))
            return false;
        m->spans[m->span_count][0] = (uint16_t)from->valuedouble;
        m->spans[m->span_count][1] = (uint16_t)to->valuedouble;
        m->span_count++;
    }
    m->said = dup(said);
    m->better = dup(better);
    m->note = dup(note);
    return m->said && m->better && m->note;
}

/** practice.turn newWords [{word, meaning}], two at most. */
static bool new_words(const cJSON *words, yk_srv_msg_t *m) {
    if (!cJSON_IsArray(words) || cJSON_GetArraySize(words) > 2) return false;
    const cJSON *w;
    cJSON_ArrayForEach(w, words) {
        const cJSON *word = get(w, "word"), *meaning = get(w, "meaning");
        if (!cJSON_IsObject(w) || !str_in(word, 1, 40) || !str_in(meaning, 1, 40)) return false;
        const int i = m->word_count++;
        m->words[i] = dup(word);
        m->meanings[i] = dup(meaning);
        if (!m->words[i] || !m->meanings[i]) return false;
    }
    return true;
}

/** A screensaver image to download: url (https), sha256 and size (1..98304) into m (url owned). */
static bool saver_image(const cJSON *o, yk_srv_msg_t *m) {
    const cJSON *url = get(o, "url"), *sha = get(o, "sha256"), *size = get(o, "size");
    if (!cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) || strlen(url->valuestring) < 9 ||
        !cJSON_IsString(sha) || !sha256_ok(sha->valuestring) || !is_int(size, 1, YK_PROTO_SAVER_IMAGE_MAX))
        return false;
    copy(m->sha256, sizeof(m->sha256), sha);
    m->size = (uint32_t)size->valuedouble;
    free(m->url);
    m->url = dup(url);
    return m->url != NULL;
}

/** screensaver.set: at least one of on, after (30, 60, 300), duration (300, 600, 1800) and image {id, url, sha256,
 *  size}. */
static bool parse_saver_set(const cJSON *o, yk_srv_msg_t *m) {
    const cJSON *on = get(o, "on"), *after = get(o, "after"), *duration = get(o, "duration"), *image = get(o, "image");
    m->saver_on = -1;
    if (on) {
        if (!cJSON_IsBool(on)) return false;
        m->saver_on = cJSON_IsTrue(on) ? 1 : 0;
    }
    if (after) {
        const int s = is_int(after, 1, 3600) ? (int)after->valuedouble : 0;
        if (s != 30 && s != 60 && s != 300) return false;
        m->saver_after = s;
    }
    if (duration) {
        const int s = is_int(duration, 1, 7200) ? (int)duration->valuedouble : 0;
        if (s != 300 && s != 600 && s != 1800) return false;
        m->saver_duration = s;
    }
    if (image) {
        const cJSON *id = get(image, "id");
        if (!cJSON_IsObject(image) || !cJSON_IsString(id) || !yk_proto_saver_image_ok(id->valuestring) ||
            !saver_image(image, m))
            return false;
        copy(m->image_id, sizeof(m->image_id), id);
    }
    if (!on && !after && !duration && !image) return false;  // something to change
    m->kind = YK_ACT_SAVER_SET;
    return true;
}

static bool parse_action(const cJSON *o, yk_srv_msg_t *m) {
    const cJSON *id = get(o, "id"), *kind = get(o, "kind");
    if (!str_in(id, 1, 40) || !cJSON_IsString(kind)) return false;
    copy(m->action_id, sizeof(m->action_id), id);
    const char *k = kind->valuestring;
    if (!strcmp(k, "app.open")) {
        const cJSON *slug = get(o, "slug"), *params = get(o, "params");
        if (!cJSON_IsString(slug) || !yk_proto_slug_ok(slug->valuestring) || !cJSON_IsObject(params)) return false;
        m->kind = YK_ACT_APP_OPEN;
        copy(m->slug, sizeof(m->slug), slug);
        m->params = cJSON_PrintUnformatted(params);
        return m->params != NULL;
    }
    if (!strcmp(k, "app.install")) {
        const cJSON *slug = get(o, "slug"), *name = get(o, "name"), *version = get(o, "version"),
                    *size = get(o, "size"), *sha = get(o, "sha256"), *url = get(o, "url"), *sig = get(o, "signature"),
                    *replace = get(o, "replace");
        if (!cJSON_IsString(slug) || !yk_proto_slug_ok(slug->valuestring) || !str_in(name, 1, 40) ||
            !str_in(version, 1, 31) || !is_int(size, 1, 4294967295.0) || !cJSON_IsString(sha) ||
            strlen(sha->valuestring) != 64 || !cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) ||
            strlen(url->valuestring) < 9 || !str_in(sig, 1, 200) ||
            (replace && (!cJSON_IsString(replace) || !yk_proto_slug_ok(replace->valuestring))))
            return false;
        for (const char *c = sha->valuestring; *c; c++)
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return false;
        m->kind = YK_ACT_APP_INSTALL;
        copy(m->slug, sizeof(m->slug), slug);
        copy(m->name, sizeof(m->name), name);
        copy(m->version, sizeof(m->version), version);
        m->size = (uint32_t)size->valuedouble;
        copy(m->sha256, sizeof(m->sha256), sha);
        m->url = dup(url);
        m->signature = dup(sig);
        if (replace) copy(m->replace, sizeof(m->replace), replace);
        return m->url && m->signature;
    }
    const cJSON *level = get(o, "level"), *step = get(o, "step"), *mute = get(o, "mute");
    m->level = -1;
    m->step = 0;
    m->mute = -1;
    const bool setting = !strcmp(k, "volume.set") || !strcmp(k, "brightness.set");
    if (setting) {
        // Exactly one of level, step and (volume) mute.
        if (level && !is_int(level, 0, 10) && !(cJSON_IsString(level) && !strcmp(level->valuestring, "auto"))) return false;
        if (step && (!is_int(step, -3, 3) || step->valuedouble == 0)) return false;
        if (is_int(level, 0, 10)) m->level = (int)level->valuedouble;
        if (step) m->step = (int)step->valuedouble;
    }
    if (!strcmp(k, "volume.set")) {
        if (mute && !cJSON_IsBool(mute)) return false;
        if (mute) m->mute = cJSON_IsTrue(mute) ? 1 : 0;
        if (!!level + !!step + !!mute != 1 || cJSON_IsString(level)) return false;  // "auto" is brightness's
        m->kind = YK_ACT_VOLUME;
        return true;
    }
    if (!strcmp(k, "brightness.set")) {
        // "auto" (older servers) reads as no level and no step: answered UNSUPPORTED, there is no light sensor.
        if (!!level + !!step != 1) return false;
        m->kind = YK_ACT_BRIGHTNESS;
        return true;
    }
    if (!strcmp(k, "app.home")) {
        m->kind = YK_ACT_APP_HOME;
        return true;
    }
    if (!strcmp(k, "screen_off.set")) {
        const cJSON *seconds = get(o, "seconds");
        if (!is_int(seconds, 0, 3600) || !screen_off_ok((int)seconds->valuedouble)) return false;
        m->kind = YK_ACT_SCREEN_OFF;
        m->seconds = (int)seconds->valuedouble;
        return true;
    }
    if (!strcmp(k, "screensaver.set")) return parse_saver_set(o, m);
    if (!strcmp(k, "screensaver.preview")) {
        const cJSON *seconds = get(o, "seconds");
        if (!saver_image(o, m) || !is_int(seconds, 3, 30)) return false;
        m->kind = YK_ACT_SAVER_PREVIEW;
        m->seconds = (int)seconds->valuedouble;
        return true;
    }
    m->kind = YK_ACT_UNKNOWN;  // a newer kind: the id is enough to say no
    return true;
}

/** A language the device keeps (as yaykit_cloud's yk_locale_ok): 2 to 7 characters, a letter first, then letters,
 *  digits and '-'. */
static bool locale_ok(const char *s) {
    const size_t n = strlen(s);
    if (n < 2 || n > 7) return false;
    for (size_t i = 0; i < n; i++) {
        const bool letter = (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z');
        if (!letter && (i == 0 || !((s[i] >= '0' && s[i] <= '9') || s[i] == '-'))) return false;
    }
    return true;
}

static bool parse_into(const cJSON *o, yk_srv_msg_t *m) {
    const cJSON *type = get(o, "type");
    if (!cJSON_IsObject(o) || !cJSON_IsString(type)) return false;
    const char *t = type->valuestring;
    if (!strcmp(t, "hello.ok")) {
        const cJSON *session = get(o, "session"), *time = get(o, "time"), *owner = get(o, "owner"),
                    *credits = get(o, "credits"), *dot = get(o, "dot");
        const cJSON *nick = owner ? get(owner, "nickname") : NULL, *left = credits ? get(credits, "remaining") : NULL,
                    *reset = credits ? get(credits, "resetAt") : NULL;
        if (!str_in(session, 1, 64) || !is_int(time, 1, 9007199254740991.0) || !cJSON_IsObject(owner) ||
            !str_in(nick, 0, 40) || !cJSON_IsObject(credits) || !is_int(left, 0, 2147483647) || !datetime_ok(reset) ||
            !cJSON_IsBool(dot))
            return false;
        m->type = YK_SRV_HELLO_OK;
        copy(m->session, sizeof(m->session), session);
        m->time_ms = (int64_t)time->valuedouble;
        copy(m->nickname, sizeof(m->nickname), nick);
        // The account's language (#545): optional, and a value the device would not keep is left out, never the
        // hello.ok with it.
        const cJSON *loc = get(owner, "locale");
        if (cJSON_IsString(loc) && locale_ok(loc->valuestring)) copy(m->owner_locale, sizeof(m->owner_locale), loc);
        m->has_credits = true;  // credits is required here: an app reading has_credits gets it (#580)
        m->credits_remaining = (int)left->valuedouble;
        copy(m->reset_at, sizeof(m->reset_at), reset);
        m->dot = cJSON_IsTrue(dot);
        // nextReminderAt: optional; a value that is not a time leaves it not known, never the hello.ok with it.
        const cJSON *next = get(o, "nextReminderAt");
        m->next_reminder = -1;
        if (cJSON_IsNull(next)) {
            m->next_reminder = 0;
        } else if (is_int(next, 1, 9007199254740991.0)) {
            m->next_reminder = 1;
            m->next_reminder_ms = (int64_t)next->valuedouble;
        }
        m->week_days = m->week_turns = -1;
        const cJSON *practice = get(o, "practice");
        if (practice) {
            const cJSON *points = get(practice, "turnPoints"), *week = get(practice, "week");
            const cJSON *days = week ? get(week, "days") : NULL, *turns = week ? get(week, "turns") : NULL;
            if (!cJSON_IsObject(practice) || !cJSON_IsNumber(points) || points->valuedouble < 0 ||
                (week && (!cJSON_IsObject(week) || !is_int(days, 0, 7) || !is_int(turns, 0, 2147483647))))
                return false;
            m->has_practice = true;
            m->turn_points = points->valuedouble;
            if (week) m->week_days = (int)days->valuedouble, m->week_turns = (int)turns->valuedouble;
        }
        return true;
    }
    if (!strcmp(t, "stt.partial") || !strcmp(t, "stt.final")) {
        const cJSON *text = get(o, "text");
        if (!turn_field(o, m, true) || !str_in(text, 0, 2000)) return false;
        m->type = t[4] == 'p' ? YK_SRV_STT_PARTIAL : YK_SRV_STT_FINAL;
        lang_field(get(o, "lang"), m);
        m->text = dup(text);
        return m->text != NULL;
    }
    if (!strcmp(t, "reply.line")) {
        const cJSON *line = get(o, "line"), *text = get(o, "text"), *kind = get(o, "kind");
        if (!turn_field(o, m, true) || !is_int(line, 0, 255) || !str_in(text, 1, 120) || (kind && !cJSON_IsString(kind)))
            return false;
        m->type = YK_SRV_REPLY_LINE;
        m->line = (int)line->valuedouble;
        // A kind this firmware does not know is a subtitle, as for the devices before kinds.
        m->fix_line = kind && !strcmp(kind->valuestring, "fix");
        lang_field(get(o, "lang"), m);
        m->text = dup(text);
        return m->text != NULL;
    }
    if (!strcmp(t, "app.records.ok")) {
        const cJSON *slug = get(o, "slug"), *ids = get(o, "ids");
        if (!cJSON_IsString(slug) || !yk_proto_slug_ok(slug->valuestring) || !cJSON_IsArray(ids) ||
            cJSON_GetArraySize(ids) > YK_PROTO_RECORDS_MAX)
            return false;
        m->type = YK_SRV_APP_RECORDS_OK;
        copy(m->slug, sizeof(m->slug), slug);
        m->ids = calloc((size_t)cJSON_GetArraySize(ids) + 1, sizeof(char *));
        if (!m->ids) return false;
        const cJSON *id;
        cJSON_ArrayForEach(id, ids) {
            if (!cJSON_IsString(id) || !record_word(id->valuestring, true)) return false;
            if (!(m->ids[m->id_count++] = dup(id))) return false;
        }
        return true;
    }
    if (!strcmp(t, "practice.turn")) {
        static const char *const verdicts[] = {"fix", "good", "none", "blocked", NULL};
        const cJSON *verdict = get(o, "verdict"), *fix = get(o, "fix"), *hint = get(o, "hint"),
                    *words = get(o, "newWords");
        if (!turn_field(o, m, true) || !enum_str(verdict, verdicts) || (fix && !practice_fix(fix, m)) ||
            (hint && !str_in(hint, 0, 120)) || (words && !new_words(words, m)))
            return false;
        m->type = YK_SRV_PRACTICE_TURN;
        copy(m->verdict, sizeof(m->verdict), verdict);
        m->text = dup(hint);
        return m->text != NULL;
    }
    if (!strcmp(t, "reply.end")) {
        static const char *const muted[] = {"credits", "speech", NULL};
        const cJSON *mute = get(o, "muted");
        if (!turn_field(o, m, true) || !credits_field(get(o, "credits"), m) || (mute && !cJSON_IsString(mute)))
            return false;
        m->type = YK_SRV_REPLY_END;
        if (mute) snprintf(m->muted, sizeof(m->muted), "%s", enum_str(mute, muted) ? mute->valuestring : "speech");
        return true;
    }
    if (!strcmp(t, "ui.state")) {
        const cJSON *state = get(o, "state"), *label = get(o, "label"), *progress = get(o, "progress"),
                    *task = get(o, "taskId");
        if (!enum_str(state, YAY_STATES) || !str_in(label, 0, 40) || !progress ||
            !(cJSON_IsNull(progress) || (cJSON_IsNumber(progress) && progress->valuedouble >= 0 && progress->valuedouble <= 1)) ||
            !task || !nullable_str(task, 64, m->task_id, sizeof(m->task_id)))
            return false;
        m->type = YK_SRV_UI_STATE;
        copy(m->state, sizeof(m->state), state);
        m->text = dup(label);
        m->progress = cJSON_IsNull(progress) ? -1.0f : (float)progress->valuedouble;
        return m->text != NULL;
    }
    if (!strcmp(t, "action")) {
        if (!parse_action(o, m)) return false;
        m->type = YK_SRV_ACTION;
        return true;
    }
    if (!strcmp(t, "inbox")) {
        static const char *const kinds[] = {"dot", "approval", "reminder", "done", NULL};
        const cJSON *kind = get(o, "kind"), *text = get(o, "text"), *task = get(o, "taskId");
        if (!enum_str(kind, kinds) || !str_in(text, 0, 120) || !task ||
            !nullable_str(task, 64, m->task_id, sizeof(m->task_id)))
            return false;
        m->type = YK_SRV_INBOX;
        copy(m->inbox_kind, sizeof(m->inbox_kind), kind);
        m->text = dup(text);
        return m->text != NULL;
    }
    if (!strcmp(t, "session.reauth")) {
        m->type = YK_SRV_REAUTH;
        return true;
    }
    if (!strcmp(t, "error")) {
        const cJSON *code = get(o, "code"), *retry = get(o, "retryAfter"), *reset = get(o, "resetAt"),
                    *credits = get(o, "credits");
        // A code this firmware does not know still ends the turn (shown as a general error).
        if (!turn_field(o, m, false) || !str_in(code, 1, 40) || (retry && !is_int(retry, 1, 2147483647)) ||
            (reset && !datetime_ok(reset)) || (credits && !credits_field(credits, m)))
            return false;
        m->type = YK_SRV_ERROR;
        copy(m->code, sizeof(m->code), code);
        if (retry) m->retry_after = (int)retry->valuedouble;
        if (reset) copy(m->reset_at, sizeof(m->reset_at), reset);
        return true;
    }
    return false;
}

yk_srv_type_t yk_proto_parse(const char *json, size_t len, yk_srv_msg_t *out) {
    memset(out, 0, sizeof(*out));
    out->progress = -1.0f;
    cJSON *o = json ? cJSON_ParseWithLength(json, len) : NULL;
    const bool ok = o && parse_into(o, out);
    cJSON_Delete(o);
    if (!ok) {
        yk_proto_free(out);
        return YK_SRV_INVALID;
    }
    if (!out->text) {
        out->text = malloc(1);
        if (!out->text) {
            yk_proto_free(out);
            return YK_SRV_INVALID;
        }
        out->text[0] = 0;
    }
    return out->type;
}

void yk_proto_free(yk_srv_msg_t *m) {
    if (!m) return;
    free(m->text);
    cJSON_free(m->params);
    free(m->url);
    free(m->signature);
    free(m->said);
    free(m->better);
    free(m->note);
    for (int i = 0; i < 2; i++) free(m->words[i]), free(m->meanings[i]);
    for (int i = 0; m->ids && i < m->id_count; i++) free(m->ids[i]);
    free(m->ids);
    memset(m, 0, sizeof(*m));
    m->progress = -1.0f;
}

/** Takes *from into *to when *to has nothing (both owned strings). */
static void take(char **to, char **from) {
    if (*to && (*to)[0]) return;
    free(*to);
    *to = *from;
    *from = NULL;
}

bool yk_proto_practice_merge(yk_srv_msg_t *into, yk_srv_msg_t *more) {
    const bool same = into->type == YK_SRV_PRACTICE_TURN && more->type == YK_SRV_PRACTICE_TURN &&
                      !strcmp(into->turn, more->turn);
    if (same) {
        if (!into->verdict[0]) memcpy(into->verdict, more->verdict, sizeof(into->verdict));
        if (!into->better && more->better) {  // the correction comes whole: said, better, spans and note together
            take(&into->said, &more->said);
            take(&into->better, &more->better);
            take(&into->note, &more->note);
            memcpy(into->spans, more->spans, sizeof(into->spans));
            into->span_count = more->span_count;
        }
        if (!into->word_count) {
            for (int i = 0; i < more->word_count; i++) {
                into->words[i] = more->words[i], into->meanings[i] = more->meanings[i];
                more->words[i] = more->meanings[i] = NULL;
            }
            into->word_count = more->word_count;
        }
        take(&into->text, &more->text);  // the hint
    }
    yk_proto_free(more);
    return same;
}

const char *yk_proto_type_name(yk_srv_type_t type) {
    static const char *const names[] = {"invalid", "hello.ok", "stt.partial", "stt.final", "reply.line", "reply.end",
                                        "ui.state", "action", "inbox", "session.reauth", "error", "practice.turn",
                                        "app.records.ok"};
    return (unsigned)type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

// ---------------------------------------------------------------- reconnecting

int yk_proto_backoff_ms(int attempt, uint32_t rnd) {
    static const int steps_s[] = {1, 2, 4, 8, 15, 30};
    const int last = (int)(sizeof(steps_s) / sizeof(steps_s[0])) - 1;
    const int base = steps_s[attempt < 0 ? 0 : attempt > last ? last : attempt] * 1000;
    return base * 3 / 4 + (int)(rnd % (uint32_t)(base / 2 + 1));
}

// ---------------------------------------------------------------- audio frames

size_t yk_proto_frame(uint8_t *out, int stream, int line, int seq, const uint8_t *packet, size_t bytes) {
    out[0] = YK_PROTO_FRAME_VERSION;
    out[1] = (uint8_t)stream;
    out[2] = (uint8_t)(line & 0xff);
    out[3] = (uint8_t)(seq & 0xff);
    if (bytes) memcpy(out + YK_PROTO_HEADER, packet, bytes);
    return YK_PROTO_HEADER + bytes;
}

bool yk_proto_unframe(const uint8_t *f, size_t len, int *stream, int *line, int *seq, const uint8_t **packet,
                      size_t *bytes) {
    if (len <= YK_PROTO_HEADER || f[0] != YK_PROTO_FRAME_VERSION) return false;
    if (f[1] != YK_PROTO_STREAM_MIC && f[1] != YK_PROTO_STREAM_TTS) return false;
    *stream = f[1];
    *line = f[2];
    *seq = f[3];
    *packet = f + YK_PROTO_HEADER;
    *bytes = len - YK_PROTO_HEADER;
    return true;
}

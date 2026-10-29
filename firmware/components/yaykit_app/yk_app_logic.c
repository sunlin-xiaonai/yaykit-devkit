#define _POSIX_C_SOURCE 200809L  // gmtime_r
#include "yk_app_logic.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int yk_home_slot(const char *slot0_slug, const char *slot0_state) {
    if (!slot0_slug || !slot0_state || strcmp(slot0_slug, "yay")) return -1;
    // valid: confirmed; new: just written; pending: started once, not yet confirmed; unknown: no OTA record.
    static const char *usable[] = {"valid", "new", "pending", "unknown"};
    for (size_t i = 0; i < sizeof(usable) / sizeof(usable[0]); i++)
        if (!strcmp(slot0_state, usable[i])) return 0;
    return -1;  // empty, invalid (rolled back), aborted
}

bool yk_recovery_forward(bool recovery_flag, const char *slot0_slug, const char *slot0_state) {
    return !recovery_flag && yk_home_slot(slot0_slug, slot0_state) == 0;
}

bool yk_launch_accept(const char *record_slug, int64_t record_at_s, const char *my_slug, int64_t now_s) {
    if (!record_slug || !my_slug || strcmp(record_slug, my_slug)) return false;
    if (record_at_s < YK_APP_TIME_VALID_S || now_s < YK_APP_TIME_VALID_S) return true;
    const int64_t age = now_s - record_at_s;
    return age >= -5 && age <= YK_APP_LAUNCH_MAX_AGE_S;  // a few seconds of clock skew are fine
}

yk_ring_go_t yk_ring_tapped(const char *slug, const char *me, bool in_slot, char *params, size_t size) {
    if (params && size) params[0] = 0;
    if (!slug || !slug[0] || (me && !strcmp(slug, me))) return YK_RING_STAY;
    if (!strcmp(slug, "yay")) return YK_RING_HOME;  // YK_APP_HOME_SLUG
    if (!strcmp(slug, YK_RING_MORE)) {
        snprintf(params, size, "{\"then\":\"more\"}");
        return YK_RING_HOME_WITH;
    }
    if (!strcmp(slug, YK_RING_SETTINGS)) {
        snprintf(params, size, "{\"then\":\"settings\"}");
        return YK_RING_HOME_WITH;
    }
    if (in_slot) return YK_RING_OPEN;
    char esc[80];
    yk_json_escape(esc, sizeof(esc), slug);
    snprintf(params, size, "{\"then\":\"install\",\"slug\":\"%s\"}", esc);
    return YK_RING_HOME_WITH;
}

size_t yk_json_escape(char *out, size_t size, const char *in) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        char tmp[7];
        const char *piece = tmp;
        size_t len;
        switch (*p) {
        case '"': piece = "\\\"", len = 2; break;
        case '\\': piece = "\\\\", len = 2; break;
        case '\n': piece = "\\n", len = 2; break;
        case '\r': piece = "\\r", len = 2; break;
        case '\t': piece = "\\t", len = 2; break;
        default:
            if (*p < 0x20) {
                len = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            } else {
                tmp[0] = (char)*p;
                len = 1;
            }
        }
        for (size_t i = 0; i < len; i++, n++)
            if (size && n < size - 1) out[n] = piece[i];
    }
    if (size) out[n < size ? n : size - 1] = 0;
    return n;
}

/** Appends the escaped string in quotes; false when it no longer fits. */
static bool put_string(char *out, size_t size, size_t *at, const char *s) {
    if (*at + 1 >= size) return false;
    out[(*at)++] = '"';
    const size_t need = yk_json_escape(out + *at, size - *at, s);
    if (*at + need + 2 > size) return false;
    *at += need;
    out[(*at)++] = '"';
    out[*at] = 0;
    return true;
}

static bool put_raw(char *out, size_t size, size_t *at, const char *s) {
    const size_t len = strlen(s);
    if (*at + len + 1 > size) return false;
    memcpy(out + *at, s, len + 1);
    *at += len;
    return true;
}

/** A JSON object as written, or "{}": only the outer braces are checked, the reader parses it properly. */
static const char *object_or_empty(const char *json) {
    if (!json) return "{}";
    while (*json == ' ' || *json == '\n' || *json == '\t' || *json == '\r') json++;
    return *json == '{' ? json : NULL;
}

int yk_launch_format(char *out, size_t size, const char *from, const char *slug, const char *intent,
                     const char *params_json, int64_t at_s, const char *nonce) {
    const char *params = object_or_empty(params_json);
    if (!params || !slug) return -1;
    if (size > YK_APP_RECORD_MAX + 1) size = YK_APP_RECORD_MAX + 1;
    size_t at = 0;
    char num[24];
    snprintf(num, sizeof(num), "%lld", (long long)at_s);
    const bool ok = put_raw(out, size, &at, "{\"v\":1,\"from\":") && put_string(out, size, &at, from ? from : "") &&
                    put_raw(out, size, &at, ",\"slug\":") && put_string(out, size, &at, slug) &&
                    put_raw(out, size, &at, ",\"intent\":") && put_string(out, size, &at, intent ? intent : "") &&
                    put_raw(out, size, &at, ",\"params\":") && put_raw(out, size, &at, params) &&
                    put_raw(out, size, &at, ",\"at\":") && put_raw(out, size, &at, num) &&
                    put_raw(out, size, &at, ",\"nonce\":") && put_string(out, size, &at, nonce ? nonce : "") &&
                    put_raw(out, size, &at, "}");
    return ok ? (int)at : -1;
}

int yk_result_format(char *out, size_t size, const char *slug, const char *summary, const char *data_json,
                     int64_t at_s) {
    const char *data = object_or_empty(data_json);
    if (!data || !slug) return -1;
    if (size > YK_APP_RECORD_MAX + 1) size = YK_APP_RECORD_MAX + 1;
    size_t at = 0;
    char num[24];
    snprintf(num, sizeof(num), "%lld", (long long)at_s);
    bool ok = put_raw(out, size, &at, "{\"v\":1,\"slug\":") && put_string(out, size, &at, slug);
    if (ok && summary) ok = put_raw(out, size, &at, ",\"summary\":") && put_string(out, size, &at, summary);
    ok = ok && put_raw(out, size, &at, ",\"data\":") && put_raw(out, size, &at, data) &&
         put_raw(out, size, &at, ",\"at\":") && put_raw(out, size, &at, num) && put_raw(out, size, &at, "}");
    return ok ? (int)at : -1;
}

// ---------------------------------------------------------------- the summary

/** Copies as much as fits, cut at a character boundary (never inside a UTF-8 sequence). */
static void cut_copy(char *out, size_t size, const char *in) {
    if (!in) in = "";
    size_t n = strlen(in);
    if (n > size - 1) {
        n = size - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) n--;  // in[n] continues a character: cut before it
    }
    memcpy(out, in, n);
    out[n] = 0;
}

void yk_summary_init(yk_summary_t *s) {
    memset(s, 0, sizeof(*s));
    s->recorded = -1;
}

bool yk_summary_stat(yk_summary_t *s, const char *value, const char *label) {
    if (s->stat_count >= YK_SUMMARY_STATS || !value || !value[0]) return false;
    cut_copy(s->stats[s->stat_count].v, sizeof(s->stats[0].v), value);
    cut_copy(s->stats[s->stat_count].l, sizeof(s->stats[0].l), label);
    s->stat_count++;
    return true;
}

int yk_summary_bead(int i, int n) {
    if (n <= YK_SUMMARY_BEADS) return i;
    return (int)((int64_t)i * YK_SUMMARY_BEADS / n);
}

void yk_summary_beads(yk_summary_t *s, int n, const int *marked, int marked_count) {
    s->beads = n <= 0 ? 0 : n > YK_SUMMARY_BEADS ? YK_SUMMARY_BEADS : n;
    s->marked = 0;
    for (int k = 0; marked && k < marked_count; k++)
        if (marked[k] >= 0 && marked[k] < n) s->marked |= 1u << yk_summary_bead(marked[k], n);
}

int yk_summary_format(char *out, size_t size, const yk_summary_t *s) {
    size_t at = 0;
    bool ok = put_raw(out, size, &at, "{\"stats\":[");
    for (int i = 0; ok && i < s->stat_count; i++)
        ok = put_raw(out, size, &at, i ? ",{\"v\":" : "{\"v\":") && put_string(out, size, &at, s->stats[i].v) &&
             put_raw(out, size, &at, ",\"l\":") && put_string(out, size, &at, s->stats[i].l) &&
             put_raw(out, size, &at, "}");
    ok = ok && put_raw(out, size, &at, "]");
    if (ok && s->beads > 0) {
        char num[16];
        snprintf(num, sizeof(num), "%d", s->beads);
        ok = put_raw(out, size, &at, ",\"beads\":{\"total\":") && put_raw(out, size, &at, num) &&
             put_raw(out, size, &at, ",\"marked\":[");
        bool first = true;
        for (int b = 0; ok && b < s->beads; b++) {
            if (!(s->marked & (1u << b))) continue;
            snprintf(num, sizeof(num), first ? "%d" : ",%d", b);
            ok = put_raw(out, size, &at, num);
            first = false;
        }
        ok = ok && put_raw(out, size, &at, "]}");
    }
    if (ok && s->recorded >= 0) ok = put_raw(out, size, &at, s->recorded ? ",\"recorded\":true" : ",\"recorded\":false");
    ok = ok && put_raw(out, size, &at, "}");
    return ok ? (int)at : -1;
}

bool yk_result_fresh(int64_t at_s, int64_t now_s) {
    if (at_s < YK_APP_TIME_VALID_S || now_s < YK_APP_TIME_VALID_S) return true;
    return now_s - at_s <= YK_RESULT_FRESH_S;
}

// ---------------------------------------------------------------- the record outbox

bool yk_record_kind_ok(const char *kind) {
    if (!kind || kind[0] < 'a' || kind[0] > 'z') return false;
    size_t n = 1;
    for (; kind[n]; n++)
        if (!((kind[n] >= 'a' && kind[n] <= 'z') || (kind[n] >= '0' && kind[n] <= '9') || kind[n] == '.')) return false;
    return n >= 3 && n <= 41;
}

void yk_record_key(uint32_t seq, char out[8]) { snprintf(out, 8, "rec%u", (unsigned)(seq % YK_RECORD_SLOTS)); }

int yk_record_format(char *out, size_t size, const char *app, const char *device_id, uint32_t seq, const char *kind,
                     const char *data_json, int64_t at_ms) {
    const char *data = object_or_empty(data_json);
    if (!data || !app || !device_id || !yk_record_kind_ok(kind) || strlen(data) > YK_RECORD_DATA_MAX) return -1;
    char id[96], when[80];  // when: room for any year the compiler can imagine
    const int idn = snprintf(id, sizeof(id), "%s:%s:%u", device_id, app, (unsigned)seq);
    if (idn < 4 || idn > 80) return -1;
    const time_t secs = (time_t)(at_ms / 1000);
    struct tm tm;
    gmtime_r(&secs, &tm);
    snprintf(when, sizeof(when), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(at_ms % 1000));
    size_t at = 0;
    const bool trusted = at_ms / 1000 >= YK_APP_TIME_VALID_S;
    const bool ok = put_raw(out, size, &at, "{\"app\":") && put_string(out, size, &at, app) &&
                    put_raw(out, size, &at, ",\"id\":") && put_string(out, size, &at, id) &&
                    put_raw(out, size, &at, ",\"kind\":") && put_string(out, size, &at, kind) &&
                    put_raw(out, size, &at, ",\"occurredAt\":") && put_string(out, size, &at, when) &&
                    put_raw(out, size, &at, trusted ? ",\"clockTrusted\":true" : ",\"clockTrusted\":false") &&
                    put_raw(out, size, &at, ",\"data\":") && put_raw(out, size, &at, data) && put_raw(out, size, &at, "}");
    return ok ? (int)at : -1;
}

void yk_banner_name(char *out, size_t size, const char *slug) {
    size_t n = (size_t)snprintf(out, size, "YAYKIT_");
    for (const char *p = slug ? slug : ""; *p && n + 1 < size; p++, n++)
        out[n] = *p == '-' ? '_' : (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
    if (n < size) out[n] = 0;
    strncat(out, "_READY", size - strlen(out) - 1);
}

yk_guard_action_t yk_boot_guard(uint8_t stored, yk_reset_t reset, bool is_home, uint8_t *count) {
    const uint8_t n = reset == YK_RESET_NORMAL ? 0 : reset == YK_RESET_POWER ? stored
                    : stored < UINT8_MAX ? stored + 1 : UINT8_MAX;
    if (count) *count = n;
    if (n < YK_CRASHLOOP_LIMIT) return YK_GUARD_RUN;
    return is_home ? YK_GUARD_RECOVERY : YK_GUARD_HOME;
}

// The built-in names, Chinese and English (each firmware's app.json; the template's Chinese one as it always was).
static const struct {
    const char *slug, *zh, *en;
} NAMES[] = {
    {"yay", "Yay", "Yay"},
    {"party-play", "聚会玩", "Party Play"},
    {"language-buddy", "口语陪练", "Language Buddy"},
    {"homework-buddy", "作业伙伴", "Homework Buddy"},
    {"breathe", "呼吸之间", "Breathe"},
    {"template", "扩展模板", "Counter Template"},
    {"tikpal", "Tikpal", "Tikpal"},
    {"recording-card", "录音卡", "Recording Card"},
    {"translator", "面对面翻译", "Face to Face"},
};

const char *yk_app_table_name(const char *slug, bool english) {
    for (size_t i = 0; slug && i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (!strcmp(slug, NAMES[i].slug)) return english ? NAMES[i].en : NAMES[i].zh;
    return NULL;
}

const char *yk_app_display_name(const char *slug) {
    if (!slug) return "";
    const char *name = yk_app_table_name(slug, false);
    return name ? name : slug;
}

const char *yk_app_label(const char *slug, const char *catalog_name, bool english) {
    if (catalog_name && catalog_name[0]) return catalog_name;
    if (!slug) return "";
    const char *name = yk_app_table_name(slug, english);
    return name ? name : slug;
}

// The side key's numbers (yaykit_key's yk_key_types.h), kept here so this stays plain C.
enum { KEY_DOWN = 1, KEY_HOLD, KEY_UP, KEY_TAP, KEY_DOUBLE };

void yk_keys_init(yk_keys_t *k) {
    memset(k, 0, sizeof(*k));
    k->closed_ms = -1;
}

yk_keys_do_t yk_keys_route(yk_keys_t *k, int type, int64_t at_ms, bool own_double) {
    if (k->eat) {
        // The press that closed the ring or the card: its hold, release, tap or double click are no one else's.
        if (type == KEY_HOLD) k->held = true;
        if (type == KEY_TAP || type == KEY_DOUBLE || (type == KEY_UP && k->held)) k->eat = k->held = false;
        return YK_KEYS_EAT;
    }
    if (k->ring || k->ask) {
        if (type != KEY_DOWN) return YK_KEYS_EAT;  // the end of the press that asked
        k->eat = true;
        k->held = false;
        if (k->ring) {
            k->ring = false;
            k->closed_ms = at_ms;
            return YK_KEYS_RING_CLOSE;
        }
        k->ask = false;
        return YK_KEYS_ASK_CLOSE;
    }
    if (type == KEY_DOUBLE && !own_double) {
        // Without delay_tap the first press's TAP ended the eating; its double click is still that press's.
        if (k->closed_ms >= 0 && at_ms - k->closed_ms < YK_KEYS_REOPEN_MS) return YK_KEYS_EAT;
        k->ring = true;
        return YK_KEYS_RING_OPEN;
    }
    return YK_KEYS_APP;
}

bool yk_keys_ask(yk_keys_t *k, bool used, bool talks) {
    if (used || talks || k->ring || k->ask || k->eat) return false;
    k->ask = true;
    return true;
}

void yk_keys_closed(yk_keys_t *k) { k->ring = k->ask = false; }

bool yk_keys_idle(const yk_keys_t *k) { return !k->ring && !k->ask && !k->eat; }

// ---------------------------------------------------------------- the main firmware's update (yk_app_update.h)

static const char *const UPDATE_STATES[] = {"none", "pending", "backed_up", "trial"};

const char *yk_update_state_name(yk_update_state_t state) {
    return (unsigned)state < sizeof(UPDATE_STATES) / sizeof(UPDATE_STATES[0]) ? UPDATE_STATES[state] : "?";
}

const char *yk_update_step_name(yk_update_step_t step) {
    static const char *const names[] = {"go", "backup", "write", "restore", "rollback", "drop", "reject"};
    return (unsigned)step < sizeof(names) / sizeof(names[0]) ? names[step] : "?";
}

/** A version as a record field: 1..31 printable characters, no '|' or space. `empty_ok` for `from`. */
static bool update_field_ok(const char *s, bool empty_ok) {
    const size_t n = strlen(s);
    if (n > 31 || (!n && !empty_ok)) return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] <= ' ' || s[i] > '~' || s[i] == '|') return false;
    return true;
}

static bool update_sha_ok(const char *s) {
    if (strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

int yk_update_format(char *out, size_t size, const yk_update_t *u) {
    if (!out || !u || u->state == YK_UPDATE_NONE || (unsigned)u->state > YK_UPDATE_TRIAL ||
        !update_field_ok(u->version, false) || !update_sha_ok(u->sha256) || !update_field_ok(u->from, true))
        return -1;
    const int n = snprintf(out, size, "1|yay|%s|%s|%s|%u|%s", u->version, u->sha256, UPDATE_STATES[u->state],
                           (unsigned)u->tries, u->from);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

bool yk_update_parse(const char *in, yk_update_t *out) {
    memset(out, 0, sizeof(*out));
    if (!in) return false;
    // Seven fields; copy each (the last, `from`, may be empty).
    char f[7][72];
    size_t k = 0, len = 0;
    for (const char *p = in;; p++) {
        if (*p == '|' || !*p) {
            if (k >= 7) return false;
            f[k++][len] = 0;
            len = 0;
            if (!*p) break;
            continue;
        }
        if (k >= 7 || len + 1 >= sizeof(f[0])) return false;
        f[k][len++] = *p;
    }
    if (k != 7 || strcmp(f[0], "1") || strcmp(f[1], "yay")) return false;
    yk_update_state_t state = YK_UPDATE_NONE;
    for (int s = YK_UPDATE_PENDING; s <= YK_UPDATE_TRIAL; s++)
        if (!strcmp(f[4], UPDATE_STATES[s])) state = (yk_update_state_t)s;
    unsigned tries = 0;
    const size_t tl = strlen(f[5]);
    for (size_t i = 0; i < tl; i++) {
        if (f[5][i] < '0' || f[5][i] > '9' || tl > 3) return false;
        tries = tries * 10 + (unsigned)(f[5][i] - '0');
    }
    if (state == YK_UPDATE_NONE || !tl || tries > 255 || !update_field_ok(f[2], false) || !update_sha_ok(f[3]) ||
        !update_field_ok(f[6], true))
        return false;
    out->state = state;
    memcpy(out->version, f[2], strlen(f[2]) + 1);  // lengths checked above (31, 64, 31)
    memcpy(out->sha256, f[3], 65);
    memcpy(out->from, f[6], strlen(f[6]) + 1);
    out->tries = (uint8_t)tries;
    return true;
}

yk_update_step_t yk_update_next(const yk_update_in_t *in) {
    // What to go back to, when the update does not take.
    const yk_update_step_t back = in->previous_cached ? YK_UPDATE_ROLLBACK : YK_UPDATE_DROP;
    if (!in->has) return !in->home_usable && in->previous_cached ? YK_UPDATE_RESTORE : YK_UPDATE_GO;
    switch (in->rec.state) {
    case YK_UPDATE_PENDING:
        if (!in->new_cached) return YK_UPDATE_DROP;  // nothing written yet: the Yay there stays
        if (in->home_usable) return YK_UPDATE_BACKUP;
        // Slot 1 holds no usable Yay to keep (a write cut short before): the new one straight in.
        return in->rec.tries >= YK_UPDATE_TRIES ? back : YK_UPDATE_WRITE;
    case YK_UPDATE_BACKED_UP:
        // Gone from the cache (a bad copy), or written too often without taking: back to the previous one, unless
        // the Yay there still works (refused before its slot was touched, as a bad signature is).
        if (!in->new_cached || in->rec.tries >= YK_UPDATE_TRIES) return in->home_usable ? YK_UPDATE_REJECT : back;
        return YK_UPDATE_WRITE;
    case YK_UPDATE_TRIAL:
        if (in->home_crashed || !in->home_usable) return back;
        return YK_UPDATE_GO;  // starting it: confirmed after a stable minute (yk_update_confirm_after)
    default:
        return YK_UPDATE_DROP;
    }
}

int32_t yk_update_confirm_after(const yk_update_t *u, bool home) {
    return home && u && u->state == YK_UPDATE_TRIAL ? YK_UPDATE_CONFIRM_MS : -1;
}

bool yk_update_battery_ok(int percent, bool usb) { return usb || percent < 0 || percent >= YK_UPDATE_LOW_BATTERY; }

bool yk_update_step_guarded(yk_update_step_t step, const yk_update_in_t *in) {
    if (step == YK_UPDATE_BACKUP) return true;
    return step == YK_UPDATE_WRITE && in && in->has && in->home_usable && in->rec.tries == 0;
}

bool yk_update_auto_matches(const char *auto_sha, const yk_update_t *u) {
    return auto_sha && u && update_sha_ok(auto_sha) && !strcmp(auto_sha, u->sha256);
}

bool yk_update_holds_cache(const yk_update_t *u) {
    return u && (u->state == YK_UPDATE_PENDING || u->state == YK_UPDATE_BACKED_UP || u->state == YK_UPDATE_TRIAL);
}


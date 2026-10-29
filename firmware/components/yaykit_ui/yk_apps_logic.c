// Pure C parts of the app ring, the app catalog and the hint arrow (yk_apps_logic.h). No LVGL, no ESP-IDF: host
// tested with test/run.sh.
#include "yk_apps_logic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "yk_round.h"

// ---------------------------------------------------------------- the ring's geometry

void yk_apps_slots(int n, int slots[YK_APPS_MAX]) {
    static const int sets[6][5] = {
        {0}, {0}, {1, 4}, {0, 2, 3}, {1, 2, 3, 4}, {0, 1, 2, 3, 4},
    };
    if (n < 0) n = 0;
    if (n > 5) n = 5;  // the 5-place ring (without 「设置」)
    for (int i = 0; i < YK_APPS_MAX; i++) slots[i] = i < n ? sets[n][i] : -1;
}

static bool box_in_circle(const yk_box_t *b, int r) {
    const float rr = (float)r * (float)r;
    const int xs[2] = {b->x, b->x + b->w}, ys[2] = {b->y, b->y + b->h};
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            const float dx = (float)(xs[i] - YK_CX), dy = (float)(ys[j] - YK_CY);
            if (dx * dx + dy * dy > rr) return false;
        }
    return true;
}

static void slot_centre(int slot, int16_t *cx, int16_t *cy) {
    const bool six = slot >= YK_APPS_SIX;
    const double a = (six ? (slot - YK_APPS_SIX) * 60.0 : slot * 72.0) * 3.14159265358979 / 180.0;  // from 12, clockwise
    const double r = six ? YK_APPS_R6 : YK_APPS_R;
    *cx = (int16_t)lround(YK_CX + r * sin(a));
    *cy = (int16_t)lround(YK_CY - r * cos(a));
}

int yk_apps_name_room(int slot, int name_h) {
    int16_t cx, cy;
    slot_centre(slot, &cx, &cy);
    // How wide the circle is across the glyphs' farther edge.
    const int top = cy + YK_APPS_SAT_R + YK_APPS_NAME_GAP + YK_APPS_INK_TOP - YK_CY;
    const int bottom = cy + YK_APPS_SAT_R + YK_APPS_NAME_GAP + name_h - YK_APPS_INK_BOTTOM - YK_CY;
    const int far = abs(top) > abs(bottom) ? abs(top) : abs(bottom);
    if (far >= YK_R_CONTENT) return 0;
    const int half = (int)floor(sqrt((double)YK_R_CONTENT * YK_R_CONTENT - (double)far * far));
    int lo = YK_CX - half, hi = YK_CX + half;
    // The lower two share the bottom of the circle: each keeps to its side. On the 6-place ring a disc stands at the
    // bottom between them: they keep beside it (and its in-use ring), not under it.
    const int apart = slot >= YK_APPS_SIX ? YK_APPS_SAT_R + YK_APPS_RING_W + 2 : YK_APPS_NAME_APART / 2;
    if (cy > YK_CY && cx > YK_CX) lo = YK_CX + apart;
    if (cy > YK_CY && cx < YK_CX) hi = YK_CX - apart;
    // The widest w whose middle can sit within the disc's width (|middle - cx| < SAT_R) inside lo..hi.
    int best = 0;
    for (int w = 1; w <= YK_APPS_NAME_MAX_W && w <= hi - lo; w++) {
        const int mid = cx < lo + w / 2 ? lo + w / 2 : cx > hi - (w - w / 2) ? hi - (w - w / 2) : cx;
        if (abs(mid - cx) < YK_APPS_SAT_R) best = w;
    }
    return best;
}

static bool is(const yk_apps_item_t *item, const char *slug) { return item->slug && !strcmp(item->slug, slug); }

void yk_apps_item_slots(const yk_apps_item_t *items, int n, int slots[YK_APPS_MAX]) {
    if (n < 0) n = 0;
    if (n > YK_APPS_MAX) n = YK_APPS_MAX;
    if (n && items && is(&items[n - 1], YK_APPS_SETTINGS)) {
        const bool more = n >= 2 && is(&items[n - 2], YK_APPS_MORE);
        const int apps = n - 1 - more, room = more ? 4 : 5;
        for (int i = 0; i < YK_APPS_MAX; i++)
            slots[i] = i < apps && i < room ? YK_APPS_SIX + i : -1;
        if (more) slots[n - 2] = YK_APPS_MORE_PLACE6;
        slots[n - 1] = YK_APPS_SETTINGS_PLACE;
        return;
    }
    if (n > 5) n = 5;
    if (!n || !items || !items[n - 1].slug || strcmp(items[n - 1].slug, YK_APPS_MORE)) {
        yk_apps_slots(n, slots);
        return;
    }
    for (int i = 0; i < YK_APPS_MAX; i++) slots[i] = i < n - 1 ? i : i == n - 1 ? YK_APPS_MORE_PLACE : -1;
}

void yk_apps_layout(int n, const int name_w[], int name_h, int label_w, yk_apps_layout_t *out) {
    int slots[YK_APPS_MAX];
    yk_apps_slots(n, slots);
    yk_apps_layout_at(n, slots, name_w, name_h, label_w, out);
}

void yk_apps_layout_at(int n, const int slots[], const int name_w[], int name_h, int label_w, yk_apps_layout_t *out) {
    memset(out, 0, sizeof(*out));
    out->n = n < 0 ? 0 : n > YK_APPS_MAX ? YK_APPS_MAX : n;
    if (label_w > YK_APPS_LABEL_MAX_W) label_w = YK_APPS_LABEL_MAX_W;
    out->label = (yk_box_t){(int16_t)(YK_CX - label_w / 2), (int16_t)(YK_CY + YK_APPS_LABEL_DY), (int16_t)label_w,
                            (int16_t)name_h};
    for (int i = 0; i < out->n; i++) {
        yk_apps_sat_t *s = &out->sat[i];
        s->slot = slots[i];
        slot_centre(s->slot, &s->cx, &s->cy);
        const int room = yk_apps_name_room(s->slot, name_h);
        int w = name_w ? name_w[i] : 0;
        if (w > room) w = room;
        s->name = (yk_box_t){(int16_t)(s->cx - w / 2), (int16_t)(s->cy + YK_APPS_SAT_R + YK_APPS_NAME_GAP), (int16_t)w,
                             (int16_t)name_h};
        // Towards the middle until the glyphs are in the circle (the lower two: about 25 px).
        const int dir = s->cx > YK_CX ? -1 : 1;
        for (int k = 0; k < YK_APPS_R; k++) {
            const yk_box_t ink = {s->name.x, (int16_t)(s->name.y + YK_APPS_INK_TOP), s->name.w,
                                  (int16_t)(s->name.h - YK_APPS_INK_TOP - YK_APPS_INK_BOTTOM)};
            if (box_in_circle(&ink, YK_R_CONTENT) || s->name.x + s->name.w / 2 == YK_CX) break;
            s->name.x = (int16_t)(s->name.x + dir);
            s->name_shift++;
        }
    }
}

// ---------------------------------------------------------------- motion

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float ease_out(float p) { return 1 - (1 - p) * (1 - p) * (1 - p); }
static float ease_in(float p) { return p * p; }

static yk_apps_pose_t pose_at(float x0, float y0, float x1, float y1, float k, float s0, float s1, float o0,
                              float o1) {
    return (yk_apps_pose_t){
        .cx = (int16_t)lroundf(x0 + (x1 - x0) * k),
        .cy = (int16_t)lroundf(y0 + (y1 - y0) * k),
        .scale = (uint16_t)lroundf(256 * (s0 + (s1 - s0) * k)),
        .opa = (uint8_t)lroundf(255 * (o0 + (o1 - o0) * k)),
    };
}

bool yk_apps_frame(const yk_apps_layout_t *L, int t_ms, bool closing, yk_apps_pose_t pose[YK_APPS_MAX + 1]) {
    const float t = (float)(t_ms < 0 ? 0 : t_ms);
    if (closing) {
        const float k = ease_in(clamp01(t / YK_APPS_CLOSE_MS));
        pose[0] = pose_at(YK_CX, YK_CY, YK_CX, YK_CY, k, 1.0f, 0.8f, 1, 0);
        for (int i = 0; i < L->n; i++)
            pose[i + 1] = pose_at(L->sat[i].cx, L->sat[i].cy, YK_CX, YK_CY, k, 1.0f, 0.2f, 1, 0);
        return t >= YK_APPS_CLOSE_MS;
    }
    pose[0] = pose_at(YK_CX, YK_CY, YK_CX, YK_CY, ease_out(clamp01(t / YK_APPS_CENTER_IN_MS)), 0.6f, 1.0f, 0, 1);
    int end = YK_APPS_CENTER_IN_MS;
    for (int i = 0; i < L->n; i++) {
        const int start = i * YK_APPS_STAGGER_MS;
        const float k = ease_out(clamp01((t - start) / YK_APPS_FLY_MS));
        pose[i + 1] = pose_at(YK_CX, YK_CY, L->sat[i].cx, L->sat[i].cy, k, 0.2f, 1.0f, 0, 1);
        if (start + YK_APPS_FLY_MS > end) end = start + YK_APPS_FLY_MS;
    }
    return t >= end;
}

static bool in_box(const yk_box_t *b, int x, int y, int pad) {
    return x >= b->x - pad && x < b->x + b->w + pad && y >= b->y - pad && y < b->y + b->h + pad;
}

int yk_apps_hit(const yk_apps_layout_t *L, int x, int y) {
    for (int i = 0; i < L->n; i++) {
        const int dx = x - L->sat[i].cx, dy = y - L->sat[i].cy;
        if (dx * dx + dy * dy <= YK_APPS_HIT_R * YK_APPS_HIT_R || in_box(&L->sat[i].name, x, y, 4)) return i + 1;
    }
    const int dx = x - YK_CX, dy = y - YK_CY;
    if (dx * dx + dy * dy <= YK_APPS_CENTER_HIT_R * YK_APPS_CENTER_HIT_R || in_box(&L->label, x, y, 4)) return 0;
    return -1;
}

int yk_apps_pick(const yk_apps_slot_t *installed, int n, const yk_catalog_t *cat, yk_apps_item_t *out, int max) {
    if (max > YK_APPS_MAX) max = YK_APPS_MAX;
    int count = 0;
    // The installed ones, the most recently used first (a stable selection: equal times keep the slot order).
    bool taken[8] = {false};
    if (n > 8) n = 8;
    for (int round = 0; round < n && count < max; round++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (taken[i] || !installed[i].slug || !installed[i].slug[0] || !strcmp(installed[i].slug, YK_APPS_HOME)) continue;
            if (best < 0 || installed[i].used_ms > installed[best].used_ms) best = i;
        }
        if (best < 0) break;
        taken[best] = true;
        bool dup = false;
        for (int k = 0; k < count; k++) dup |= !strcmp(out[k].slug, installed[best].slug);
        if (dup) continue;
        const yk_catalog_entry_t *e = cat ? yk_catalog_find(cat, installed[best].slug) : NULL;
        out[count++] = (yk_apps_item_t){.slug = installed[best].slug, .name = e ? e->name : NULL, .installed = true};
    }
    // Then the catalog's apps that are not installed, in its order.
    for (int i = 0; cat && i < cat->count && count < max; i++) {
        const yk_catalog_entry_t *e = &cat->app[i];
        if (!e->slug[0] || !strcmp(e->slug, YK_APPS_HOME)) continue;
        bool have = false;
        for (int k = 0; k < count; k++) have |= !strcmp(out[k].slug, e->slug);
        if (!have) out[count++] = (yk_apps_item_t){.slug = e->slug, .name = e->name, .installed = false};
    }
    return count;
}

/** Adds `slug` unless it is Yay, empty or there already; false when out is full. */
static bool ring_add(yk_apps_item_t *out, int *count, int max, const char *slug, const char *name, bool installed,
                     bool cached) {
    if (!slug || !slug[0] || !strcmp(slug, YK_APPS_HOME) || !strcmp(slug, YK_APPS_MORE) || !strcmp(slug, YK_APPS_SETTINGS))
        return true;
    for (int k = 0; k < *count; k++)
        if (!strcmp(out[k].slug, slug)) return true;
    if (*count >= max) return false;
    out[(*count)++] = (yk_apps_item_t){.slug = slug, .name = name, .installed = installed, .cached = cached};
    return true;
}

int yk_apps_pick_ring(const yk_apps_slot_t *installed, int n, const char *const cached[], int nc,
                      const yk_catalog_t *cat, yk_apps_item_t *out, int max) {
    if (max > YK_APPS_MAX) max = YK_APPS_MAX;
    if (max < 1) return 0;
    const bool settings = max >= 2;  // 「设置」 after 「更多」 when both fit (settings-u.md 5)
    const int fixed = settings ? 2 : 1, apps = max - fixed < YK_APPS_RING_APPS ? max - fixed : YK_APPS_RING_APPS;
    // The ones in a slot, as yk_apps_pick() takes them (the most recently used first).
    int count = yk_apps_pick(installed, n, NULL, out, apps);
    for (int k = 0; cat && k < count; k++) {
        const yk_catalog_entry_t *e = yk_catalog_find(cat, out[k].slug);
        if (e) out[k].name = e->name;
    }
    for (int i = 0; cached && i < nc; i++) {
        const yk_catalog_entry_t *e = cat ? yk_catalog_find(cat, cached[i]) : NULL;
        if (!ring_add(out, &count, apps, cached[i], e ? e->name : NULL, false, true)) break;
    }
    for (int i = 0; cat && i < cat->count; i++)
        if (!ring_add(out, &count, apps, cat->app[i].slug, cat->app[i].name, false, false)) break;
    out[count++] = (yk_apps_item_t){.slug = YK_APPS_MORE};
    if (settings) out[count++] = (yk_apps_item_t){.slug = YK_APPS_SETTINGS};
    return count;
}

const yk_catalog_entry_t *yk_catalog_find(const yk_catalog_t *cat, const char *slug) {
    for (int i = 0; cat && slug && i < cat->count && i < YK_CATALOG_MAX; i++)
        if (!strcmp(cat->app[i].slug, slug)) return &cat->app[i];
    return NULL;
}

int yk_text_fit(const char *text, yk_glyph_width_fn width, void *ctx, int max_w, char *out, size_t size) {
    if (!size) return 0;
    out[0] = 0;
    if (!text) return 0;
    static const char ELLIPSIS[] = "\xE2\x80\xA6";  // …
    const int ell = width(0x2026, ctx);
    int total = 0, len, fit_bytes = 0, fit_w = 0;
    const char *p = text;
    for (; *p; p += len) {
        const int w = width(yk_utf8_next(p, &len), ctx);
        if (total + w <= max_w - ell) fit_bytes = (int)(p - text) + len, fit_w = total + w;
        total += w;
    }
    if (total <= max_w) {
        // all of it
        const size_t n = strlen(text) < size - 1 ? strlen(text) : size - 1;
        memcpy(out, text, n);
        out[n] = 0;
        return total;
    }
    if ((size_t)fit_bytes + sizeof(ELLIPSIS) > size) return 0;
    memcpy(out, text, (size_t)fit_bytes);
    memcpy(out + fit_bytes, ELLIPSIS, sizeof(ELLIPSIS));
    return fit_w + ell;
}

void yk_first_char(const char *name, char out[5]) {
    out[0] = 0;
    if (!name || !name[0]) return;
    int len;
    yk_utf8_next(name, &len);
    if (len > 4) len = 4;
    memcpy(out, name, (size_t)len);
    out[len] = 0;
    if (len == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}

// ---------------------------------------------------------------- the catalog's bytes

uint32_t yk_catalog_crc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

size_t yk_catalog_icon_bytes(uint8_t format) {
    switch (format) {
    case YK_ICON_A8: return YK_CATALOG_ICON * YK_CATALOG_ICON;
    case YK_ICON_ARGB8888: return YK_CATALOG_ICON * YK_CATALOG_ICON * 4;
    default: return 0;
    }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v), put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

/** A string field: copied, NUL-padded; false when it does not fit with its NUL. */
static bool put_str(uint8_t *p, const char *s, size_t field) {
    const size_t n = s ? strnlen(s, field) : 0;
    if (n >= field) return false;
    memset(p, 0, field);
    if (n) memcpy(p, s, n);
    return true;
}

static bool get_str(char *dst, const uint8_t *p, size_t field) {
    if (!memchr(p, 0, field)) return false;
    memcpy(dst, p, field);
    return true;
}

// Entry layout (128 bytes): slug 0..31, name 32..79, version 80..95, size 96, format 100 (+3 reserved), colour 104,
// icon offset 108, icon CRC 112, channel 116 (10-24; 0 stable, 1 preview: zero in older catalogs), the icon's sha256
// 117..124 (its first 8 bytes, Yay 0.1.5; zero in older catalogs and with no icon), reserved 125..127.
enum { E_SLUG = 0, E_NAME = 32, E_VERSION = 80, E_SIZE = 96, E_FORMAT = 100, E_COLOR = 104, E_OFFSET = 108, E_CRC = 112,
       E_CHANNEL = 116, E_ICON_SHA = 117 };
// Header (64 bytes): magic 0, version 4, count 6, seq 8, table length 12, table CRC 16, written_ms 20, lang 28..35,
// reserved 36..59, header CRC 60 (of bytes 0..59).
enum { H_MAGIC = 0, H_VERSION = 4, H_COUNT = 6, H_SEQ = 8, H_LEN = 12, H_CRC = 16, H_WRITTEN = 20, H_LANG = 28, H_SELF = 60 };

size_t yk_catalog_encode(const yk_catalog_t *cat, const uint8_t *const icons[], uint8_t *out, size_t size) {
    if (!cat || cat->count < 0 || cat->count > YK_CATALOG_MAX || size < YK_CATALOG_TABLE_AT) return 0;
    const uint32_t table_len = (uint32_t)cat->count * YK_CATALOG_ENTRY;
    uint32_t at = YK_CATALOG_TABLE_AT + table_len;
    if (at > size) return 0;
    memset(out, 0, at);
    for (int i = 0; i < cat->count; i++) {
        const yk_catalog_entry_t *e = &cat->app[i];
        uint8_t *t = out + YK_CATALOG_TABLE_AT + i * YK_CATALOG_ENTRY;
        if (!e->slug[0] || !put_str(t + E_SLUG, e->slug, 32) || !put_str(t + E_NAME, e->name, 48) ||
            !put_str(t + E_VERSION, e->version, 16))
            return 0;
        put32(t + E_SIZE, e->size);
        const size_t bytes = yk_catalog_icon_bytes(e->icon_format);
        if (e->icon_format != YK_ICON_NONE && (!bytes || !icons || !icons[i])) return 0;
        t[E_FORMAT] = bytes ? e->icon_format : YK_ICON_NONE;
        put32(t + E_COLOR, e->icon_color & 0xFFFFFF);
        t[E_CHANNEL] = e->channel == YK_CHANNEL_PREVIEW ? YK_CHANNEL_PREVIEW : YK_CHANNEL_STABLE;
        if (bytes) {
            at = (at + 3) & ~3u;
            if (at + bytes > size || at + bytes > YK_CATALOG_REGION) return 0;
            memcpy(out + at, icons[i], bytes);
            put32(t + E_OFFSET, at);
            put32(t + E_CRC, yk_catalog_crc32(0, icons[i], bytes));
            memcpy(t + E_ICON_SHA, e->icon_sha, YK_CATALOG_ICON_SHA);
            at += (uint32_t)bytes;
        }
    }
    uint8_t *h = out;
    put32(h + H_MAGIC, YK_CATALOG_MAGIC);
    put16(h + H_VERSION, YK_CATALOG_VERSION);
    put16(h + H_COUNT, (uint16_t)cat->count);
    put32(h + H_SEQ, cat->seq);
    put32(h + H_LEN, table_len);
    put32(h + H_CRC, yk_catalog_crc32(0, out + YK_CATALOG_TABLE_AT, table_len));
    put32(h + H_WRITTEN, (uint32_t)(uint64_t)cat->written_ms);
    put32(h + H_WRITTEN + 4, (uint32_t)((uint64_t)cat->written_ms >> 32));
    put_str(h + H_LANG, cat->lang[0] ? cat->lang : "", 8);
    put32(h + H_SELF, yk_catalog_crc32(0, h, H_SELF));
    return at;
}

bool yk_catalog_decode_header(const uint8_t *h, yk_catalog_t *out, uint32_t *table_len, uint32_t *table_crc) {
    memset(out, 0, sizeof(*out));
    if (get32(h + H_MAGIC) != YK_CATALOG_MAGIC || get16(h + H_VERSION) != YK_CATALOG_VERSION) return false;
    if (get32(h + H_SELF) != yk_catalog_crc32(0, h, H_SELF)) return false;
    const int count = get16(h + H_COUNT);
    const uint32_t len = get32(h + H_LEN);
    if (count > YK_CATALOG_MAX || len != (uint32_t)count * YK_CATALOG_ENTRY) return false;
    if (!get_str(out->lang, h + H_LANG, 8)) return false;
    out->count = count;
    out->seq = get32(h + H_SEQ);
    out->written_ms = (int64_t)((uint64_t)get32(h + H_WRITTEN) | ((uint64_t)get32(h + H_WRITTEN + 4) << 32));
    *table_len = len;
    *table_crc = get32(h + H_CRC);
    return true;
}

bool yk_catalog_decode_table(const uint8_t *t, uint32_t len, uint32_t crc, yk_catalog_t *out) {
    if (len != (uint32_t)out->count * YK_CATALOG_ENTRY || yk_catalog_crc32(0, t, len) != crc) return false;
    for (int i = 0; i < out->count; i++) {
        const uint8_t *p = t + i * YK_CATALOG_ENTRY;
        yk_catalog_entry_t *e = &out->app[i];
        memset(e, 0, sizeof(*e));
        if (!get_str(e->slug, p + E_SLUG, 32) || !get_str(e->name, p + E_NAME, 48) ||
            !get_str(e->version, p + E_VERSION, 16) || !e->slug[0])
            return false;
        e->size = get32(p + E_SIZE);
        e->icon_format = p[E_FORMAT];
        e->icon_color = get32(p + E_COLOR);
        e->icon_offset = get32(p + E_OFFSET);
        e->icon_crc = get32(p + E_CRC);
        e->channel = p[E_CHANNEL] == YK_CHANNEL_PREVIEW ? YK_CHANNEL_PREVIEW : YK_CHANNEL_STABLE;  // a later value: stable
        memcpy(e->icon_sha, p + E_ICON_SHA, YK_CATALOG_ICON_SHA);
        const size_t bytes = yk_catalog_icon_bytes(e->icon_format);
        // An icon in a format this reader does not know: the first character instead (formats are only added).
        if (!bytes) e->icon_format = YK_ICON_NONE;
        else if (e->icon_offset < YK_CATALOG_TABLE_AT + len || e->icon_offset + bytes > YK_CATALOG_REGION) return false;
    }
    return true;
}

static uint16_t rgb565(uint32_t c) {
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xFF) >> 3));
}

void yk_icon_rgb565a8(uint8_t format, const uint8_t *src, uint32_t color, uint8_t opa, uint8_t *dst) {
    const int n = YK_CATALOG_ICON * YK_CATALOG_ICON;
    uint8_t *alpha = dst + 2 * n;
    for (int i = 0; i < n; i++) {
        uint32_t c = 0;
        int a = 0;
        if (format == YK_ICON_A8) {
            c = color;
            a = src[i];
        } else if (format == YK_ICON_ARGB8888) {
            const uint8_t *p = src + 4 * i;
            c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            a = p[3];
        }
        const uint16_t v = rgb565(c);
        dst[2 * i] = (uint8_t)v, dst[2 * i + 1] = (uint8_t)(v >> 8);  // LVGL's RGB565: little-endian
        alpha[i] = (uint8_t)(a * opa / 255);
    }
}

// ---------------------------------------------------------------- the hint arrow

void yk_hint_geometry(float key_deg, int text_w, yk_hint_geo_t *g) {
    const float a = key_deg * 3.14159265f / 180.0f;
    g->ux = sinf(a), g->uy = -cosf(a);
    const float px = -g->uy, py = g->ux;  // across the arrow
    g->tip_x = YK_CX + g->ux * YK_HINT_TIP_R;
    g->tip_y = YK_CY + g->uy * YK_HINT_TIP_R;
    const float bx = g->tip_x - g->ux * YK_HINT_V_DEPTH, by = g->tip_y - g->uy * YK_HINT_V_DEPTH;
    g->a1_x = bx - px * YK_HINT_V_HALF, g->a1_y = by - py * YK_HINT_V_HALF;
    g->a2_x = bx + px * YK_HINT_V_HALF, g->a2_y = by + py * YK_HINT_V_HALF;
    // The pill: its side towards the key at YK_HINT_PILL_R, pulled in until its corners are in the content circle.
    const int w = text_w + 2 * YK_HINT_PILL_PAD, h = YK_HINT_PILL_H;
    const float reach = fabsf(g->ux) * w / 2.0f + fabsf(g->uy) * h / 2.0f;
    for (float d = YK_HINT_PILL_R - reach; d > -YK_R_CONTENT; d -= 1) {
        const float cx = YK_CX + g->ux * d, cy = YK_CY + g->uy * d;
        g->pill = (yk_box_t){(int16_t)lroundf(cx - w / 2.0f), (int16_t)lroundf(cy - h / 2.0f), (int16_t)w, (int16_t)h};
        if (box_in_circle(&g->pill, YK_R_CONTENT - 2)) break;
    }
}

uint32_t yk_hint_app_bit(const char *slug) {
    if (!slug || !slug[0] || !strcmp(slug, YK_APPS_HOME)) return 0;
    uint32_t h = 2166136261u;  // FNV-1a
    for (const char *p = slug; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return 1u << (h % 32);
}

bool yk_hint_rule(const yk_hint_state_t *s, yk_hint_kind_t kind) {
    switch (kind) {
    case YK_HINT_TALK: return s->talked < YK_HINT_CAP && s->talk_shown < YK_HINT_CAP;
    case YK_HINT_TAP: return !s->tap_shown;
    case YK_HINT_APPS: return s->app_bit && !s->ring && !(s->seen & s->app_bit);
    case YK_HINT_BACK: return true;
    default: return false;
    }
}

bool yk_hint_apply(yk_hint_state_t *s, yk_hint_event_t ev) {
    switch (ev) {
    case YK_HINT_EV_WAKE: s->tap_shown = false; return false;
    case YK_HINT_EV_TALKED: return s->talked < YK_HINT_CAP ? (s->talked++, true) : false;
    case YK_HINT_EV_RING_OPENED: return !s->ring ? (s->ring = 1, true) : false;  // once is all the rules need
    default: return false;
    }
}

bool yk_hint_shown(yk_hint_state_t *s, yk_hint_kind_t kind) {
    switch (kind) {
    case YK_HINT_TALK: return s->talk_shown < YK_HINT_CAP ? (s->talk_shown++, true) : false;
    case YK_HINT_TAP: s->tap_shown = true; return false;
    case YK_HINT_APPS:
        if (!s->app_bit || (s->seen & s->app_bit)) return false;
        s->seen |= s->app_bit;
        return true;
    default: return false;
    }
}

const char *yk_hint_text(yk_hint_kind_t kind, bool english) {
    switch (kind) {
    case YK_HINT_TALK:
    case YK_HINT_BACK: return english ? "Hold here to talk" : "按住这里说话";
    case YK_HINT_TAP: return english ? "Hold the key to talk" : "按侧键说话";
    case YK_HINT_APPS: return english ? "Double-press: apps, home" : "双击：应用和回家";
    default: return "";
    }
}

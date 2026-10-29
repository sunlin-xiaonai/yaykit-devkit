// The app ring (yk_apps.h): one opaque full-screen object on top of the screen draws the discs, icons, marks and
// names itself (LV_EVENT_DRAW_MAIN), from the layout and poses of yk_apps_logic.c; Yay in the middle is yk_yay. A
// frame of the motion repaints only the boxes of what moved; LVGL skips whatever the paper covers.
#include "yk_apps.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cjk_font.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "yk_apps_logic.h"
#include "yk_hint.h"
#include "yk_round.h"
#include "yk_ui_priv.h"
#include "yk_yay.h"

static const char *TAG = "yk_apps";

#define FRAME_MS 16       // while it moves
#define IDLE_CHECK_MS 250 // while it waits

typedef struct {
    char slug[32];
    char name[64];               // cut to YK_APPS_NAME_MAX_W
    char first[5];               // stands in for a missing icon
    bool installed, current;
    bool cached;                 // in the package cache (not in a slot): a hollow sage ring, not dimmed
    bool more;                   // 「更多」: three dots
    bool settings;               // 「设置」: a gear
    const lv_image_dsc_t *icon;  // the caller's, or `own`
    lv_image_dsc_t own;          // from the catalog: RGB565A8 in PSRAM, already dimmed when not installed
} sat_t;

static struct {
    lv_obj_t *root;
    yk_yay_t *yay;
    lv_timer_t *timer;
    yk_apps_layout_t L;
    yk_apps_pose_t pose[YK_APPS_MAX + 1];
    lv_area_t drawn[YK_APPS_MAX + 1];  // what each element covered last frame
    sat_t sat[YK_APPS_MAX];
    char label[24];
    bool home;
    const lv_font_t *font, *first_font;
    int line_h;
    yk_apps_cb_t cb;
    void *ctx;
    uint32_t t0, last_input;
    bool closing, moving, report;
    bool has_result;
    char result[32];
    int64_t opened_us;
    int frames;
} s;

static yk_apps_stats_t s_stats;

// ---------------------------------------------------------------- where things are at a pose

static float k_of(const yk_apps_pose_t *p) { return p->scale / 256.0f; }

/** Text cannot be scaled cheaply: a name fades in over the last part of its disc's growth instead. */
static lv_opa_t text_opa(const yk_apps_pose_t *p, float k0) {
    const float k = k_of(p), f = k <= k0 ? 0 : k >= 1 ? 1 : (k - k0) / (1 - k0);
    return (lv_opa_t)lroundf(p->opa * f);
}

/** A box laid out for the element at full size, carried along with it: its offset from the element's centre is
 *  scaled, its size is not (text does not scale). 4 px wider each side so LVGL never wraps a measured line. */
static lv_area_t carried(const yk_box_t *b, int ox, int oy, const yk_apps_pose_t *p) {
    const float k = k_of(p);
    const int x = p->cx + (int)lroundf((b->x - ox) * k), y = p->cy + (int)lroundf((b->y - oy) * k);
    return (lv_area_t){x - 4, y, x + b->w + 3, y + b->h - 1};
}

static void join(lv_area_t *a, const lv_area_t *b) {
    a->x1 = LV_MIN(a->x1, b->x1), a->y1 = LV_MIN(a->y1, b->y1);
    a->x2 = LV_MAX(a->x2, b->x2), a->y2 = LV_MAX(a->y2, b->y2);
}

static lv_area_t disc_area(int cx, int cy, float r) {
    const int R = (int)ceilf(r) + 1;
    return (lv_area_t){cx - R, cy - R, cx + R, cy + R};
}

/** Everything element e (0: the middle, 1..n: an app) covers at pose p. */
static lv_area_t element_area(int e, const yk_apps_pose_t *p) {
    const float k = k_of(p);
    if (e == 0) {
        lv_area_t a = disc_area(p->cx, p->cy, (YK_APPS_CENTER_R + YK_APPS_RING_W) * k);
        const lv_area_t l = carried(&s.L.label, YK_CX, YK_CY, p);
        join(&a, &l);
        return a;
    }
    const yk_apps_sat_t *st = &s.L.sat[e - 1];
    lv_area_t a = disc_area(p->cx, p->cy, (YK_APPS_SAT_R + YK_APPS_RING_W) * k);
    // the icon's square (its corners are clear, but they are drawn)
    const lv_area_t icon = disc_area(p->cx, p->cy, YK_CATALOG_ICON / 2 * k);
    join(&a, &icon);
    const lv_area_t badge = disc_area(p->cx + (int)lroundf(YK_APPS_BADGE_D * k), p->cy + (int)lroundf(YK_APPS_BADGE_D * k),
                                      YK_APPS_BADGE_R * k);
    join(&a, &badge);
    const lv_area_t name = carried(&st->name, st->cx, st->cy, p);
    join(&a, &name);
    return a;
}

// ---------------------------------------------------------------- drawing

void yk_draw_disc(lv_layer_t *layer, int cx, int cy, float r, uint32_t color, lv_opa_t opa) {
    const int R = (int)lroundf(r);
    if (R <= 0) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = LV_RADIUS_CIRCLE;
    const lv_area_t a = {cx - R, cy - R, cx + R - 1, cy + R - 1};
    lv_draw_rect(layer, &d, &a);
}

void yk_draw_in_use(lv_layer_t *layer, int cx, int cy, float r, float k, lv_opa_t opa) {
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx;
    a.center.y = cy;
    a.radius = (uint16_t)lroundf((r + 2) * k);
    a.width = LV_MAX(1, (int32_t)lroundf(YK_APPS_RING_W * k));
    a.start_angle = 0;
    a.end_angle = 360;
    a.color = lv_color_hex(YK_SAGE);
    a.opa = opa;
    lv_draw_arc(layer, &a);
}

void yk_draw_text(lv_layer_t *layer, const char *str, const lv_font_t *font, uint32_t color, lv_opa_t opa,
                  const lv_area_t *a) {
    if (!font || !str[0]) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = str;  // the caller's, as long as what it draws
    d.font = font;
    d.color = lv_color_hex(color);
    d.opa = opa;
    d.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(layer, &d, a);
}

static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, float w, lv_opa_t opa) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1, d.p1.y = y1, d.p2.x = x2, d.p2.y = y2;
    d.width = LV_MAX(1, (int32_t)lroundf(w));
    d.color = lv_color_hex(YK_PAPER);
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

void yk_draw_download_mark(lv_layer_t *layer, int cx, int cy, float k, lv_opa_t opa) {
    const float bx = cx + YK_APPS_BADGE_D * k, by = cy + YK_APPS_BADGE_D * k;
    yk_draw_disc(layer, (int)lroundf(bx), (int)lroundf(by), YK_APPS_BADGE_R * k, YK_SAGE, opa);
    if (k < 0.5f) return;
    line(layer, bx, by - 5 * k, bx, by + 3 * k, 2.4f * k, opa);
    line(layer, bx - 4 * k, by, bx, by + 4 * k, 2.4f * k, opa);
    line(layer, bx, by + 4 * k, bx + 4 * k, by, 2.4f * k, opa);
}

void yk_draw_cached_mark(lv_layer_t *layer, int cx, int cy, float k, lv_opa_t opa) {
    const int mx = cx + (int)lroundf(YK_APPS_CACHE_D * k), my = cy + (int)lroundf(YK_APPS_CACHE_D * k);
    yk_draw_disc(layer, mx, my, YK_APPS_CACHE_R * k, YK_SAGE, opa);
    yk_draw_disc(layer, mx, my, (YK_APPS_CACHE_R - YK_APPS_CACHE_W) * k, YK_PAPER, opa);
}

/** 「设置」's gear (the canvas's 24-unit icon at 36 px): a ring, a hub and eight spokes, on the disc. */
static void draw_gear(lv_layer_t *layer, int cx, int cy, float k, lv_opa_t opa) {
    const float u = YK_APPS_GEAR_UNIT * k, w = 2 * u;
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx;
    a.center.y = cy;
    a.width = LV_MAX(1, (int32_t)lroundf(w));
    a.start_angle = 0;
    a.end_angle = 360;
    a.color = lv_color_hex(YK_INK);
    a.opa = opa;
    a.radius = (uint16_t)lroundf(6.5f * u + w / 2);
    lv_draw_arc(layer, &a);
    a.radius = (uint16_t)lroundf(3 * u + w / 2);
    lv_draw_arc(layer, &a);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = a.width;
    d.color = lv_color_hex(YK_INK);
    d.opa = opa;
    d.round_start = d.round_end = 1;
    for (int i = 0; i < 8; i++) {
        const float t = (float)i * 3.14159265f / 4, dx = sinf(t), dy = -cosf(t);
        d.p1.x = cx + 6.8f * u * dx, d.p1.y = cy + 6.8f * u * dy;
        d.p2.x = cx + 9.2f * u * dx, d.p2.y = cy + 9.2f * u * dy;
        lv_draw_line(layer, &d);
    }
}

static void draw_sat(lv_layer_t *layer, int i) {
    const yk_apps_pose_t *p = &s.pose[i + 1];
    if (p->opa <= LV_OPA_MIN) return;
    const sat_t *t = &s.sat[i];
    const float k = k_of(p);
    const bool local = t->installed || t->cached;
    yk_draw_disc(layer, p->cx, p->cy, YK_APPS_SAT_R * k, YK_PANEL, p->opa);
    if (t->more) {
        for (int d = -1; d <= 1; d++) {
            const int x = p->cx + (int)lroundf(d * YK_APPS_DOT_GAP * k);
            yk_draw_disc(layer, x, p->cy, YK_APPS_DOT_R * k, YK_INK, p->opa);
        }
    } else if (t->settings) {
        draw_gear(layer, p->cx, p->cy, k, p->opa);
    } else if (t->icon) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = t->icon;
        d.scale_x = d.scale_y = p->scale;
        d.pivot.x = YK_CATALOG_ICON / 2, d.pivot.y = YK_CATALOG_ICON / 2;
        // the catalog's icons are dimmed already; the caller's are dimmed here
        d.opa = local || t->icon == &t->own ? p->opa : (lv_opa_t)(p->opa * YK_APPS_DIM_OPA / 255);
        const lv_area_t a = {p->cx - YK_CATALOG_ICON / 2, p->cy - YK_CATALOG_ICON / 2, p->cx + YK_CATALOG_ICON / 2 - 1,
                             p->cy + YK_CATALOG_ICON / 2 - 1};
        lv_draw_image(layer, &d, &a);
    } else if (t->first[0] && s.first_font) {
        const int h = lv_font_get_line_height(s.first_font);
        const lv_area_t a = {p->cx - YK_APPS_SAT_R, p->cy - h / 2, p->cx + YK_APPS_SAT_R - 1, p->cy - h / 2 + h - 1};
        yk_draw_text(layer, t->first, s.first_font, local ? YK_INK : yk_rim_mix(YK_INK, YK_PANEL, YK_APPS_DIM_OPA),
             text_opa(p, 0.6f), &a);
    }
    if (t->current) yk_draw_in_use(layer, p->cx, p->cy, YK_APPS_SAT_R, k, p->opa);
    const bool own = t->more || t->settings;  // the ring's own places: no marks, never dimmed
    if (t->cached && !t->installed && !own) yk_draw_cached_mark(layer, p->cx, p->cy, k, p->opa);
    else if (!local && !own) yk_draw_download_mark(layer, p->cx, p->cy, k, p->opa);
    const lv_area_t na = carried(&s.L.sat[i].name, s.L.sat[i].cx, s.L.sat[i].cy, p);
    const uint32_t color = t->current ? YK_SAGE_TEXT : local || own ? YK_INK : YK_INK2;
    yk_draw_text(layer, t->name, s.font, color, text_opa(p, 0.6f), &na);
}

static void draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    for (int i = 0; i < s.L.n; i++) draw_sat(layer, i);
    const yk_apps_pose_t *p = &s.pose[0];
    if (p->opa <= LV_OPA_MIN) return;
    const float k = k_of(p);
    yk_draw_disc(layer, p->cx, p->cy, YK_APPS_CENTER_R * k, YK_PANEL, p->opa);
    if (s.home) yk_draw_in_use(layer, p->cx, p->cy, YK_APPS_CENTER_R, k, p->opa);
    const lv_area_t la = carried(&s.L.label, YK_CX, YK_CY, p);
    yk_draw_text(layer, s.label, s.font, s.home ? YK_SAGE_TEXT : YK_INK, text_opa(p, 0.8f), &la);
    // Yay is a child: drawn over this
}

// ---------------------------------------------------------------- motion

/** Takes the poses: repaints what moved, sizes and fades Yay. */
static void apply(const yk_apps_pose_t pose[YK_APPS_MAX + 1]) {
    for (int e = 0; e <= s.L.n; e++) {
        if (!memcmp(&pose[e], &s.pose[e], sizeof(pose[e]))) continue;
        s.pose[e] = pose[e];
        const lv_area_t a = element_area(e, &pose[e]);
        lv_obj_invalidate_area(s.root, &s.drawn[e]);
        lv_obj_invalidate_area(s.root, &a);
        s.drawn[e] = a;
    }
    if (s.yay) {
        const float k = k_of(&s.pose[0]);
        yk_yay_size(s.yay, (int)lroundf(YK_APPS_YAY * k));
        yk_yay_move(s.yay, s.pose[0].cx, s.pose[0].cy + (int)lroundf(YK_APPS_YAY_DY * k));
        lv_obj_set_style_opa(yk_yay_obj(s.yay), s.pose[0].opa, 0);
    }
}

static void finish(void);

static void start_close(bool report, const char *result) {
    if (s.closing) return;
    s.closing = true;
    s.moving = true;
    s.report = report;
    s.has_result = result != NULL;
    if (result) snprintf(s.result, sizeof(s.result), "%s", result);
    s.t0 = lv_tick_get();
    lv_timer_set_period(s.timer, FRAME_MS);
    lv_timer_reset(s.timer);
}

static void tick(lv_timer_t *t) {
    if (!s.root) return;
    // Stay on top of what the screen gets meanwhile.
    if (lv_obj_get_index(s.root) != (int32_t)lv_obj_get_child_count(lv_obj_get_parent(s.root)) - 1)
        lv_obj_move_to_index(s.root, -1);
    if (s.moving) {
        yk_apps_pose_t pose[YK_APPS_MAX + 1];
        const bool done = yk_apps_frame(&s.L, (int)lv_tick_elaps(s.t0), s.closing, pose);
        apply(pose);
        if (!s.closing) s.frames++;
        if (!done) return;
        s.moving = false;
        if (s.closing) {
            finish();
            return;
        }
        s_stats.open_ms = (int)((esp_timer_get_time() - s.opened_us) / 1000);
        s_stats.frames = s.frames;
        lv_timer_set_period(t, IDLE_CHECK_MS);
    } else if (lv_tick_elaps(s.last_input) >= YK_APPS_IDLE_MS) {
        start_close(true, NULL);
    }
}

static void on_press(lv_event_t *e) {
    (void)e;
    s.last_input = lv_tick_get();
}

static void on_click(lv_event_t *e) {
    (void)e;
    if (s.closing) return;
    lv_point_t p = {0};
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);
    const int hit = yk_apps_hit(&s.L, p.x, p.y);
    start_close(true, hit < 0 ? NULL : hit == 0 ? YK_APPS_HOME : s.sat[hit - 1].slug);
}

/** The root is going (closed, or its screen deleted): everything else goes with it. */
static void on_delete(lv_event_t *e) {
    (void)e;
    if (s.timer) lv_timer_delete(s.timer);
    if (s.yay) yk_yay_delete(s.yay);
    for (int i = 0; i < YK_APPS_MAX; i++) heap_caps_free((void *)s.sat[i].own.data);
    memset(&s, 0, sizeof(s));
}

static void finish(void) {
    const bool report = s.report && s.cb;
    const yk_apps_cb_t cb = s.cb;
    void *ctx = s.ctx;
    char result[sizeof(s.result)];
    const bool has = s.has_result;
    memcpy(result, s.result, sizeof(result));
    lv_obj_delete(s.root);  // on_delete clears `s`
    if (report) cb(has ? result : NULL, ctx);
}

// ---------------------------------------------------------------- open, close

static int glyph_w(uint32_t cp, void *ctx) { return lv_font_get_glyph_width((const lv_font_t *)ctx, cp, 0); }

/** The entry's icon as RGB565A8 in PSRAM (raw: a scratch buffer big enough for any format). */
static const lv_image_dsc_t *catalog_icon(sat_t *t, const yk_catalog_entry_t *e, uint8_t *raw) {
    const size_t bytes = yk_catalog_icon_bytes(e->icon_format);
    if (!bytes || !raw || yk_catalog_icon(e, raw, bytes) != ESP_OK) return NULL;
    uint8_t *px = heap_caps_malloc(YK_ICON_RGB565A8_BYTES, MALLOC_CAP_SPIRAM);
    if (!px) return NULL;
    yk_icon_rgb565a8(e->icon_format, raw, e->icon_color, t->installed || t->cached ? LV_OPA_COVER : YK_APPS_DIM_OPA, px);
    t->own = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .w = YK_CATALOG_ICON,
                   .h = YK_CATALOG_ICON, .stride = YK_CATALOG_ICON * 2},
        .data_size = YK_ICON_RGB565A8_BYTES,
        .data = px,
    };
    return &t->own;
}

static void teardown(void) {
    if (s.root) lv_obj_delete(s.root);  // on_delete clears `s`
}

bool yk_apps_open(lv_obj_t *parent, const yk_apps_item_t *items, int n, const char *current, yk_apps_cb_t cb,
                  void *ctx) {
    if (!parent) return false;
    teardown();
    const int64_t t_start = esp_timer_get_time();
    memset(&s_stats, 0, sizeof(s_stats));
    if (n < 0 || !items) n = 0;
    if (n > YK_APPS_MAX) n = YK_APPS_MAX;
    const bool en = yk_ui_is_english();
    s.cb = cb;
    s.ctx = ctx;
    s.font = cjk_font_get(YK_APPS_NAME_SIZE);
    s.first_font = cjk_font_get(YK_APPS_FIRST_SIZE);
    s.line_h = s.font ? lv_font_get_line_height(s.font) : YK_APPS_NAME_SIZE + 6;
    s.home = current && !strcmp(current, YK_APPS_HOME);

    // The catalog (names and icons), read now and let go when the icons are made.
    yk_catalog_t *cat = heap_caps_malloc(sizeof(*cat), MALLOC_CAP_SPIRAM);
    const bool have_cat = cat && yk_catalog_read(cat) == ESP_OK;
    uint8_t *raw = NULL;
    const int64_t t_icons = esp_timer_get_time();
    int name_w[YK_APPS_MAX] = {0}, slots[YK_APPS_MAX];
    yk_apps_item_slots(items, n, slots);
    for (int i = 0; i < n; i++) {
        sat_t *t = &s.sat[i];
        snprintf(t->slug, sizeof(t->slug), "%s", items[i].slug ? items[i].slug : "");
        t->more = !strcmp(t->slug, YK_APPS_MORE);
        t->settings = !strcmp(t->slug, YK_APPS_SETTINGS);
        t->installed = items[i].installed;
        t->cached = items[i].cached;
        t->current = current && !strcmp(t->slug, current);
        const yk_catalog_entry_t *e = have_cat && !t->more && !t->settings ? yk_catalog_find(cat, t->slug) : NULL;
        const char *name = items[i].name ? items[i].name
                         : t->more        ? (en ? "More" : "更多")
                         : t->settings    ? (en ? "Settings" : "设置")
                         : e && e->name[0] ? e->name
                                           : t->slug;
        const int room = yk_apps_name_room(slots[i], s.line_h);
        if (s.font) name_w[i] = yk_text_fit(name, glyph_w, (void *)s.font, room, t->name, sizeof(t->name));
        else snprintf(t->name, sizeof(t->name), "%s", name);
        if (!t->more && !t->settings) yk_first_char(name, t->first);
        t->icon = t->more || t->settings ? NULL : items[i].icon;
        if (!t->icon && e && e->icon_format != YK_ICON_NONE) {
            if (!raw) raw = heap_caps_malloc(yk_catalog_icon_bytes(YK_ICON_ARGB8888), MALLOC_CAP_SPIRAM);
            t->icon = catalog_icon(t, e, raw);
            if (t->icon) s_stats.icons_cached++;
        }
    }
    heap_caps_free(raw);
    heap_caps_free(cat);
    s_stats.icons_ms = (int)((esp_timer_get_time() - t_icons) / 1000);
    const char *label = s.home ? "Yay" : en ? "Back to Yay" : "回到 Yay";
    int label_w = YK_APPS_LABEL_MAX_W;
    if (s.font) label_w = yk_text_fit(label, glyph_w, (void *)s.font, YK_APPS_LABEL_MAX_W, s.label, sizeof(s.label));
    else snprintf(s.label, sizeof(s.label), "%s", label);
    yk_apps_layout_at(n, slots, name_w, s.line_h, label_w, &s.L);

    // The paper over everything, one object; Yay in the middle.
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, YK_CX * 2, YK_CY * 2);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(YK_PAPER), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_gesture_bubble(root, false);  // a swipe on the ring is not the page's
    lv_obj_set_clickable(root, true);
    lv_obj_add_event_cb(root, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(root, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, NULL);
    s.root = root;
    yk_apps_frame(&s.L, 0, false, s.pose);
    for (int e = 0; e <= s.L.n; e++) s.drawn[e] = element_area(e, &s.pose[e]);
    // Transparent around Yay: blended with the disc's colour, its square's corners would stand out past the disc
    // (r 56 against a 100 px square) and over the sage ring (seen on the device, 10-22).
    s.yay = yk_yay_create(root, (int)lroundf(YK_APPS_YAY * k_of(&s.pose[0])), YK_CX, YK_CY + YK_APPS_YAY_DY,
                          YK_YAY_TRANSPARENT);
    if (s.yay) lv_obj_set_style_opa(yk_yay_obj(s.yay), s.pose[0].opa, 0);
    s.timer = lv_timer_create(tick, FRAME_MS, NULL);
    s.t0 = s.last_input = lv_tick_get();
    s.moving = true;
    s.opened_us = t_start;
    s_stats.setup_ms = (int)((esp_timer_get_time() - t_start) / 1000);
    ESP_LOGI(TAG, "open: %d apps, catalog %s, %d icons from it, setup %d ms", n, have_cat ? "yes" : "none",
             s_stats.icons_cached, s_stats.setup_ms);
    yk_hint_hide();
    yk_hint_note(YK_HINT_EV_RING_OPENED);
    return true;
}

void yk_apps_close(bool animated) {
    if (!s.root) return;
    s.report = false;  // whatever was tapped before, the caller closed it
    if (animated) start_close(false, NULL);
    else teardown();
}

bool yk_apps_is_open(void) { return s.root != NULL; }

static bool ask_tap(void);

bool yk_apps_tap(const char *slug) {
    if (ask_tap()) return true;
    if (!s.root || s.closing) return false;
    const char *hit = NULL;
    if (slug && !strcmp(slug, YK_APPS_HOME)) hit = YK_APPS_HOME;
    for (int i = 0; slug && !hit && i < s.L.n; i++)
        if (!strcmp(s.sat[i].slug, slug)) hit = s.sat[i].slug;
    s.last_input = lv_tick_get();
    start_close(true, hit);
    return true;
}

// ---------------------------------------------------------------- 「回到 Yay 说？」

#define ASK_CARD 0x1F2521  // the canvas's card colour (yay_pages' CARD)

static struct {
    lv_obj_t *root;
    lv_timer_t *timer;
    yk_apps_cb_t cb;
    void *ctx;
} s_ask;

static lv_obj_t *ask_label(lv_obj_t *parent, int y, int px, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_width(l, 290);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    const lv_font_t *f = cjk_font_get(px);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_label_set_text(l, text);
    return l;
}

/** Gone: the timer and the objects (the screen deleted takes the objects; this takes the timer). */
static void ask_deleted(lv_event_t *e) {
    (void)e;
    if (s_ask.timer) lv_timer_delete(s_ask.timer);
    memset(&s_ask, 0, sizeof(s_ask));
}

static void ask_end(const char *result) {
    const yk_apps_cb_t cb = s_ask.cb;
    void *ctx = s_ask.ctx;
    if (s_ask.root) lv_obj_delete(s_ask.root);  // ask_deleted clears s_ask
    if (cb) cb(result, ctx);
}

static void ask_tapped(lv_event_t *e) {
    (void)e;
    ask_end(YK_APPS_HOME);
}

static void ask_idle(lv_timer_t *t) {
    (void)t;
    ask_end(NULL);
}

bool yk_apps_ask(lv_obj_t *parent, yk_apps_cb_t cb, void *ctx) {
    if (!parent) return false;
    yk_apps_ask_close();
    const bool en = yk_ui_is_english();
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, YK_CX * 2, YK_CY * 2);
    lv_obj_set_style_bg_color(root, lv_color_hex(YK_PAPER), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_gesture_bubble(root, false);
    lv_obj_set_clickable(root, true);
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 88, 184);  // the prototype's 290 x 104, radius 26
    lv_obj_set_size(card, 290, 104);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(ASK_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_clickable(card, false);  // the tap is the paper's, anywhere
    ask_label(card, 22, 22, YK_INK, en ? "Talk in Yay?" : "回到 Yay 说？");
    ask_label(card, 62, YK_SIZE_MIN, YK_INK3, en ? "Tap to go · press to stay" : "点屏幕回去 · 短按留下");
    lv_obj_add_event_cb(root, ask_tapped, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(root, ask_deleted, LV_EVENT_DELETE, NULL);
    s_ask.root = root;
    s_ask.cb = cb;
    s_ask.ctx = ctx;
    s_ask.timer = lv_timer_create(ask_idle, YK_APPS_IDLE_MS, NULL);
    lv_timer_set_repeat_count(s_ask.timer, 1);
    lv_timer_set_auto_delete(s_ask.timer, false);
    yk_hint_hide();
    ESP_LOGI(TAG, "ask: back to Yay to talk?");
    return true;
}

void yk_apps_ask_close(void) {
    if (!s_ask.root) return;
    s_ask.cb = NULL;
    lv_obj_delete(s_ask.root);
}

bool yk_apps_ask_is_open(void) { return s_ask.root != NULL; }

/** yk_apps_tap() on the card: a tap anywhere on it. */
static bool ask_tap(void) {
    if (!s_ask.root) return false;
    ask_end(YK_APPS_HOME);
    return true;
}

static lv_obj_t *leaving_label(lv_obj_t *parent, int y, int px, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, YK_CX - YK_R_CONTENT, y);
    lv_obj_set_width(l, YK_R_CONTENT * 2);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);  // one line
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    const lv_font_t *f = cjk_font_get(px);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *s_leaving;  // the black screen, until the restart (or yk_apps_leaving_clear)

void yk_apps_leaving_clear(void) {
    if (s_leaving) lv_obj_delete(s_leaving);
    s_leaving = NULL;
}

void yk_apps_leaving(const char *slug, const char *name) {
    const bool home = !slug || !strcmp(slug, YK_APPS_HOME), en = yk_ui_is_english();
    char found[sizeof(((yk_catalog_entry_t *)0)->name)] = "";
    if (!home && !name) {
        yk_catalog_t *cat = heap_caps_malloc(sizeof(*cat), MALLOC_CAP_SPIRAM);
        const yk_catalog_entry_t *e = cat && yk_catalog_read(cat) == ESP_OK ? yk_catalog_find(cat, slug) : NULL;
        if (e) snprintf(found, sizeof(found), "%s", e->name);
        heap_caps_free(cat);
        name = found[0] ? found : slug;
    }
    yk_apps_leaving_clear();
    lv_obj_t *o = s_leaving = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, YK_CX * 2, YK_CY * 2);
    lv_obj_set_style_bg_color(o, lv_color_hex(YK_NIGHT), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, true);  // nothing under it takes a touch now
    // The prototype's baselines (214 and 252) for 16 and 24 px lines.
    leaving_label(o, 196, YK_SIZE_MIN, YK_INK2, home ? (en ? "Back to" : "正在回到") : (en ? "Opening" : "正在打开"));
    leaving_label(o, 226, YK_SIZE_HEADING, YK_INK, home ? "Yay" : name);
    ESP_LOGI(TAG, "leaving for %s", home ? YK_APPS_HOME : slug);
}

void yk_apps_stats(yk_apps_stats_t *out) {
    if (out) *out = s_stats;
}

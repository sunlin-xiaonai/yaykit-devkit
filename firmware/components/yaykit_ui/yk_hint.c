// The hint arrow (yk_hint.h): a pill with the line and a small object drawing the V, both on top of the screen. The
// V nudges by moving its own small object (a small repaint each frame); after 4 s both go. The rules and the
// geometry are pure (yk_apps_logic.c); the counts live in NVS namespace yk_hint.
#include "yk_hint.h"

#include <math.h>
#include <string.h>

#include "cjk_font.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "yk_apps.h"
#include "yk_apps_logic.h"
#include "yk_round.h"
#include "yk_ui_priv.h"

static const char *TAG = "yk_hint";

#define NS "yk_hint"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static yk_hint_state_t s_st;
static bool s_loaded;

static struct {
    lv_obj_t *pill, *arrow;
    lv_timer_t *timer;
    yk_hint_geo_t g;
    int32_t bx, by;  // the arrow object's place before the nudge
} s;

// ---------------------------------------------------------------- the counts

/** Reads the saved counts once (from any task; NVS outside the spinlock). */
static void load(void) {
    if (s_loaded) return;
    uint8_t talked = 0, ring = 0, shown = 0;
    uint32_t seen = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "talk", &talked);
        nvs_get_u8(h, "ring", &ring);
        nvs_get_u8(h, "shown", &shown);
        nvs_get_u32(h, "seen", &seen);
        nvs_close(h);
    }
    taskENTER_CRITICAL(&s_mux);
    if (!s_loaded) {
        s_st.talked = talked, s_st.ring = ring, s_st.talk_shown = shown, s_st.seen = seen;
        s_loaded = true;
    }
    taskEXIT_CRITICAL(&s_mux);
}

/** Writes what changed (each count only up to its cap: a few writes in a device's life). */
static void save(const yk_hint_state_t *was, const yk_hint_state_t *now) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (now->talked != was->talked) err = nvs_set_u8(h, "talk", now->talked);
        if (err == ESP_OK && now->ring != was->ring) err = nvs_set_u8(h, "ring", now->ring);
        if (err == ESP_OK && now->talk_shown != was->talk_shown) err = nvs_set_u8(h, "shown", now->talk_shown);
        if (err == ESP_OK && now->seen != was->seen) err = nvs_set_u32(h, "seen", now->seen);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "counts not saved: %s", esp_err_to_name(err));
}

esp_err_t yk_hint_init(const char *slug) {
    load();
    taskENTER_CRITICAL(&s_mux);
    s_st.app_bit = yk_hint_app_bit(slug);
    taskEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool yk_hint_should(yk_hint_kind_t kind) {
    load();
    if (yk_apps_is_open()) return false;
    taskENTER_CRITICAL(&s_mux);
    const bool yes = yk_hint_rule(&s_st, kind);
    taskEXIT_CRITICAL(&s_mux);
    return yes;
}

void yk_hint_note(yk_hint_event_t event) {
    load();
    taskENTER_CRITICAL(&s_mux);
    const yk_hint_state_t was = s_st;
    const bool changed = yk_hint_apply(&s_st, event);
    const yk_hint_state_t now = s_st;
    taskEXIT_CRITICAL(&s_mux);
    if (changed) save(&was, &now);
}

void yk_hint_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);  // this namespace only
        nvs_commit(h);
        nvs_close(h);
    }
    taskENTER_CRITICAL(&s_mux);
    const uint32_t bit = s_st.app_bit;
    memset(&s_st, 0, sizeof(s_st));
    s_st.app_bit = bit;
    s_loaded = true;
    taskEXIT_CRITICAL(&s_mux);
}

// ---------------------------------------------------------------- the arrow and the pill

static void arrow_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(o, &c);
    const float dx = (float)(c.x1 - s.bx), dy = (float)(c.y1 - s.by);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = YK_HINT_STROKE;
    d.color = lv_color_hex(YK_CORAL);
    d.opa = lv_obj_get_style_opa_recursive(o, LV_PART_MAIN);
    d.round_start = d.round_end = 1;
    d.p1.x = s.g.a1_x + dx, d.p1.y = s.g.a1_y + dy;
    d.p2.x = s.g.tip_x + dx, d.p2.y = s.g.tip_y + dy;
    lv_draw_line(layer, &d);
    d.p1.x = s.g.a2_x + dx, d.p1.y = s.g.a2_y + dy;
    lv_draw_line(layer, &d);
}

static void nudge_exec(void *obj, int32_t v) {
    lv_obj_set_pos(obj, s.bx + (int32_t)lroundf(s.g.ux * v), s.by + (int32_t)lroundf(s.g.uy * v));
}

static void opa_exec(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

static void fade_in(lv_obj_t *o) {
    lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, YK_HINT_FADE_MS);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_start(&a);
}

/** Either object going (hidden, or its screen deleted) lets go of it; with both gone the timer goes too. */
static void on_delete(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_anim_delete(o, NULL);
    if (o == s.pill) s.pill = NULL;
    if (o == s.arrow) s.arrow = NULL;
    if (!s.pill && !s.arrow && s.timer) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
}

static void expire(lv_timer_t *t) {
    (void)t;
    yk_hint_hide();
}

static int glyph_w(uint32_t cp, void *ctx) { return lv_font_get_glyph_width((const lv_font_t *)ctx, cp, 0); }

bool yk_hint_show(lv_obj_t *parent, yk_hint_kind_t kind, const char *text) {
    if (!parent || kind >= YK_HINT_KIND_COUNT || yk_apps_is_open()) return false;
    yk_hint_hide();
    load();
    const lv_font_t *font = cjk_font_get(YK_HINT_TEXT_SIZE);
    char line[96];
    const char *want = text ? text : yk_hint_text(kind, yk_ui_is_english());
    const int text_w = font ? yk_text_fit(want, glyph_w, (void *)font, 300, line, sizeof(line)) : 200;
    if (!font) snprintf(line, sizeof(line), "%s", want);
    yk_hint_geometry(YK_KEY_ANGLE_DEG, text_w, &s.g);

    // The pill: panel, the line in ink.
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_scrollable(pill, false);
    lv_obj_set_clickable(pill, false);
    lv_obj_set_pos(pill, s.g.pill.x, s.g.pill.y);
    lv_obj_set_size(pill, s.g.pill.w, s.g.pill.h);
    lv_obj_set_style_bg_color(pill, lv_color_hex(YK_PANEL), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *l = lv_label_create(pill);
    lv_label_set_text(l, line);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(YK_INK), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(pill, on_delete, LV_EVENT_DELETE, NULL);

    // The V: its own small object, moved for the nudge.
    const float xs[3] = {s.g.a1_x, s.g.tip_x, s.g.a2_x}, ys[3] = {s.g.a1_y, s.g.tip_y, s.g.a2_y};
    float x1 = xs[0], y1 = ys[0], x2 = xs[0], y2 = ys[0];
    for (int i = 1; i < 3; i++) x1 = fminf(x1, xs[i]), y1 = fminf(y1, ys[i]), x2 = fmaxf(x2, xs[i]), y2 = fmaxf(y2, ys[i]);
    const int pad = YK_HINT_STROKE;
    s.bx = (int32_t)floorf(x1) - pad, s.by = (int32_t)floorf(y1) - pad;
    lv_obj_t *arrow = lv_obj_create(parent);
    lv_obj_remove_style_all(arrow);
    lv_obj_set_scrollable(arrow, false);
    lv_obj_set_clickable(arrow, false);
    lv_obj_set_pos(arrow, s.bx, s.by);
    lv_obj_set_size(arrow, (int32_t)ceilf(x2) + pad - s.bx + 1, (int32_t)ceilf(y2) + pad - s.by + 1);
    lv_obj_add_event_cb(arrow, arrow_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(arrow, on_delete, LV_EVENT_DELETE, NULL);
    s.pill = pill, s.arrow = arrow;

    fade_in(pill);
    fade_in(arrow);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arrow);
    lv_anim_set_values(&a, 0, YK_HINT_NUDGE);
    lv_anim_set_duration(&a, YK_HINT_NUDGE_MS);
    lv_anim_set_reverse_duration(&a, YK_HINT_NUDGE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, nudge_exec);
    lv_anim_start(&a);
    s.timer = lv_timer_create(expire, YK_HINT_SHOW_MS, NULL);

    taskENTER_CRITICAL(&s_mux);
    const yk_hint_state_t was = s_st;
    const bool changed = yk_hint_shown(&s_st, kind);
    const yk_hint_state_t now = s_st;
    taskEXIT_CRITICAL(&s_mux);
    if (changed) save(&was, &now);
    return true;
}

void yk_hint_hide(void) {
    // on_delete clears the pointers and the timer
    if (s.arrow) lv_obj_delete(s.arrow);
    if (s.pill) lv_obj_delete(s.pill);
    if (s.timer) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
}

bool yk_hint_is_shown(void) { return s.pill || s.arrow; }

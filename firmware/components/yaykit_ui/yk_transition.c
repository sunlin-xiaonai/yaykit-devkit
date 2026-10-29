// Into and out of an app (yk_transition.h): the splash ("铺满帧") drawn from its description at any scale and
// opacity, so Yay's growing icon and the app's first frame are the same picture.
#include "yk_transition.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "yk_round.h"

/** Party Play's mark: a die (cream, three paper pips) and a few pieces of confetti, over a coral disc. */
static void party_mark(lv_layer_t *layer, int32_t cx, int32_t cy, float k) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    // confetti first, under the die
    static const struct {
        int16_t x, y, w, h;
        uint32_t c;
    } bits[] = {{-120, -96, 18, 8, YK_SAGE}, {104, -112, 8, 18, YK_CREAM}, {128, 40, 18, 8, YK_AMBER},
                {-136, 64, 8, 18, 0xB98AA6},  {-40, 132, 18, 8, YK_SAGE}, {56, -150, 18, 8, YK_AMBER}};
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        d.bg_color = lv_color_hex(bits[i].c);
        d.radius = (int32_t)(3 * k);
        const lv_area_t a = {cx + (int32_t)(bits[i].x * k), cy + (int32_t)(bits[i].y * k),
                             cx + (int32_t)((bits[i].x + bits[i].w) * k), cy + (int32_t)((bits[i].y + bits[i].h) * k)};
        lv_draw_rect(layer, &d, &a);
    }
    const int32_t half = (int32_t)(72 * k);
    d.bg_color = lv_color_hex(YK_CREAM);
    d.radius = (int32_t)(26 * k);
    const lv_area_t die = {cx - half, cy - half, cx + half, cy + half};
    lv_draw_rect(layer, &d, &die);
    d.bg_color = lv_color_hex(YK_PAPER);
    d.radius = LV_RADIUS_CIRCLE;
    const int32_t r = (int32_t)(14 * k), off = (int32_t)(38 * k);
    for (int i = -1; i <= 1; i++) {
        const lv_area_t pip = {cx + i * off - r, cy + i * off - r, cx + i * off + r, cy + i * off + r};
        lv_draw_rect(layer, &d, &pip);
    }
}

static const yk_splash_t SPLASHES[] = {
    // Breathe's UI document: a cream orb r 120 and a sage ring r 214 on black (its night screens are black).
    {.slug = "breathe", .bg = YK_NIGHT, .disc = YK_CREAM, .disc_r = 120, .ring = YK_SAGE, .ring_r = 214, .ring_w = 4},
    // Party Play's: a coral disc, a die and confetti (the prototype's appIcon).
    {.slug = "party-play", .bg = YK_CORAL, .draw = party_mark},
};
// Until an app's UI document names its splash: the paper with a sage ring.
static const yk_splash_t PLAIN = {.slug = "", .bg = YK_PAPER, .ring = YK_SAGE, .ring_r = 214, .ring_w = 4};

const yk_splash_t *yk_splash_for(const char *slug) {
    for (size_t i = 0; slug && i < sizeof(SPLASHES) / sizeof(SPLASHES[0]); i++)
        if (!strcmp(SPLASHES[i].slug, slug)) return &SPLASHES[i];
    return &PLAIN;
}

typedef struct {
    const yk_splash_t *s;
    float scale;
    lv_opa_t opa;
} splash_state_t;

static void circle(lv_layer_t *layer, int32_t r, uint32_t color, lv_opa_t opa) {
    if (r <= 0) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = LV_RADIUS_CIRCLE;
    const lv_area_t a = {YK_CX - r, YK_CY - r, YK_CX + r - 1, YK_CY + r - 1};
    lv_draw_rect(layer, &d, &a);
}

static void splash_draw(lv_event_t *e) {
    splash_state_t *st = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const yk_splash_t *s = st->s;
    const float k = st->scale;
    if (k <= 0.01f || st->opa == LV_OPA_TRANSP) return;
    circle(layer, (int32_t)lroundf(YK_R_EDGE * k), s->bg, st->opa);
    if (s->disc_r) circle(layer, (int32_t)lroundf(s->disc_r * k), s->disc, st->opa);
    if (s->ring_w) {
        lv_draw_arc_dsc_t a;
        lv_draw_arc_dsc_init(&a);
        a.center.x = YK_CX;
        a.center.y = YK_CY;
        a.radius = (uint16_t)lroundf(s->ring_r * k);
        a.width = (int32_t)lroundf(s->ring_w * k) > 0 ? (int32_t)lroundf(s->ring_w * k) : 1;
        a.start_angle = 0;
        a.end_angle = 360;
        a.color = lv_color_hex(s->ring);
        a.opa = st->opa;
        lv_draw_arc(layer, &a);
    }
    if (s->draw && st->opa == LV_OPA_COVER) s->draw(layer, YK_CX, YK_CY, k);
}

static void splash_free(lv_event_t *e) { free(lv_event_get_user_data(e)); }

lv_obj_t *yk_splash_create(lv_obj_t *parent, const yk_splash_t *splash) {
    splash_state_t *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->s = splash ? splash : &PLAIN;
    st->scale = 1;
    st->opa = LV_OPA_COVER;
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_size(o, YK_CX * 2, YK_CY * 2);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_user_data(o, st);
    lv_obj_add_event_cb(o, splash_draw, LV_EVENT_DRAW_MAIN, st);
    lv_obj_add_event_cb(o, splash_free, LV_EVENT_DELETE, st);
    return o;
}

/** Repaints the square around the larger of two scales. */
static void invalidate(lv_obj_t *o, float a, float b) {
    const int32_t r = (int32_t)ceilf(YK_R_EDGE * (a > b ? a : b)) + 2;
    const lv_area_t box = {YK_CX - r, YK_CY - r, YK_CX + r, YK_CY + r};
    lv_obj_invalidate_area(o, &box);
}

void yk_splash_set_scale(lv_obj_t *splash, float scale) {
    splash_state_t *st = splash ? lv_obj_get_user_data(splash) : NULL;
    if (!st || st->scale == scale) return;
    const float was = st->scale;
    st->scale = scale < 0 ? 0 : scale;
    invalidate(splash, was, st->scale);
}

static void set_opa(lv_obj_t *splash, lv_opa_t opa) {
    splash_state_t *st = lv_obj_get_user_data(splash);
    if (!st || st->opa == opa) return;
    st->opa = opa;
    invalidate(splash, st->scale, st->scale);
}

static const yk_splash_t *s_first;  // the splash the first frame showed

lv_obj_t *yk_ui_first_frame(const char *slug, int brightness) {
    const yk_splash_t *s = yk_splash_for(slug);
    s_first = s;
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_hex(s->bg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    yk_splash_create(screen, s);
    lv_screen_load(screen);
    lv_refr_now(NULL);  // the frame is on the panel before the panel lights up
    board_brightness_set(brightness >= 0 ? brightness : board_pref_brightness());
    return screen;
}

/* The enter and leave animations: one splash on the top layer, scale and opacity from `a` to `b`. */
typedef struct {
    float s0, s1;
    int o0, o1;
} morph_t;

static void morph_exec(lv_anim_t *a, int32_t v) {
    lv_obj_t *o = a->var;
    const morph_t *m = lv_anim_get_user_data(a);
    const float t = (float)v / 1024.0f;
    yk_splash_set_scale(o, m->s0 + (m->s1 - m->s0) * t);
    set_opa(o, (lv_opa_t)(m->o0 + (int)((float)(m->o1 - m->o0) * t)));
}

static void morph_deleted(lv_anim_t *a) { free(lv_anim_get_user_data(a)); }

static void enter_done(lv_anim_t *a) {
    lv_obj_delete_async(a->var);
    morph_deleted(a);
}

static void morph(lv_obj_t *o, morph_t m, int ms, bool delete_after) {
    morph_t *mm = malloc(sizeof(*mm));
    if (!mm) {
        if (delete_after) lv_obj_delete(o);
        return;
    }
    *mm = m;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, (uint32_t)(ms > 0 ? ms : 1));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_custom_exec_cb(&a, morph_exec);
    lv_anim_set_user_data(&a, mm);
    lv_anim_set_completed_cb(&a, delete_after ? enter_done : morph_deleted);
    lv_anim_start(&a);
}

void yk_ui_enter(lv_obj_t *home, int ms) {
    lv_obj_t *was = lv_screen_active();
    const yk_splash_t *s = s_first ? s_first : &PLAIN;
    lv_obj_t *top = yk_splash_create(lv_layer_top(), s);
    lv_screen_load(home);
    if (was && was != home) lv_obj_delete_async(was);
    morph(top, (morph_t){.s0 = 1, .s1 = 0.8f, .o0 = LV_OPA_COVER, .o1 = LV_OPA_TRANSP}, ms, true);
}

void yk_ui_leave(const char *slug, int ms) {
    lv_obj_t *top = yk_splash_create(lv_layer_top(), yk_splash_for(slug));
    yk_splash_set_scale(top, 0.3f);
    set_opa(top, LV_OPA_TRANSP);
    morph(top, (morph_t){.s0 = 0.3f, .s1 = 1, .o0 = LV_OPA_TRANSP, .o1 = LV_OPA_COVER}, ms, false);
}

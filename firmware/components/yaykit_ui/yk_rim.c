// The rim (yk_rim.h): one full-screen object drawing its arcs in LV_EVENT_DRAW_MAIN. What it shows is
// yk_rim_picture() (yk_ui_logic.c); a change repaints only the 45-degree boxes whose fingerprint changed, so a moving
// arc or a width step costs a few small areas, never the ring's whole bounding box.
#include "yk_rim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "yk_round.h"
#include "yk_ui_logic.h"

#define FRAME_MS 33           // animations
#define LISTEN_RESTYLE_MS 250 // the voice restyles the ring at most 4 times a second (each a repaint of the band)

struct yk_rim {
    lv_obj_t *obj;
    lv_timer_t *timer;
    yk_rim_in_t in;
    uint32_t since;  // lv_tick of the mode's start
    yk_rim_pic_t pic;
    uint32_t sig[YK_RIM_SEGS];
    int width;       // the widest arc now drawn (the boxes cover it)
    int repaints;
    uint32_t level_at;
    int level_step;
    // what ALERT flashes over and returns to
    yk_rim_mode_t back_mode;
    yk_rim_opts_t back_opts;
    int back_value;
    uint32_t back_since;
};

static int widest(const yk_rim_pic_t *p) {
    int w = 0;
    for (int i = 0; i < p->n; i++) w = p->arc[i].width > w ? p->arc[i].width : w;
    return w;
}

static void draw_cb(lv_event_t *e) {
    yk_rim_t *rim = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_arc_dsc_t d;
    for (int i = 0; i < rim->pic.n; i++) {
        const yk_rim_arc_t *a = &rim->pic.arc[i];
        lv_draw_arc_dsc_init(&d);
        d.center.x = YK_CX;
        d.center.y = YK_CY;
        d.radius = YK_RIM_R;
        d.width = a->width;
        d.color = lv_color_hex(a->color);
        d.opa = LV_OPA_COVER;
        d.rounded = a->round;
        // LVGL's angles start at 3 o'clock; the rim's at 12, both clockwise. LVGL takes 0 <= start < 360 and an end
        // up to a turn past it (it wraps).
        const float start = fmodf(a->a0 + 270, 360);
        d.start_angle = start;
        d.end_angle = start + (a->a1 - a->a0);
        if (a->a1 - a->a0 >= 360) d.start_angle = 0, d.end_angle = 360;
        lv_draw_arc(layer, &d);
    }
}

/** Recomputes the picture and repaints the boxes that changed. */
static void refresh(yk_rim_t *rim) {
    rim->in.t_ms = (int64_t)lv_tick_elaps(rim->since);
    // Into the rim itself: what changed comes from the signatures and the width kept, not the old picture. Not on the
    // caller's stack (~1 KB: Yay sets the rim from its 5 KB loop, 0.1.4).
    yk_rim_picture(&rim->in, &rim->pic);
    uint32_t sig[YK_RIM_SEGS];
    yk_rim_signatures(&rim->pic, sig);
    const int w_new = widest(&rim->pic), w = w_new > rim->width ? w_new : rim->width;
    for (int k = 0; k < YK_RIM_SEGS; k++) {
        if (sig[k] == rim->sig[k]) continue;
        lv_area_t a;
        yk_rim_seg_box(k, YK_RIM_R, w + 2, &a.x1, &a.y1, &a.x2, &a.y2);
        lv_obj_invalidate_area(rim->obj, &a);
        rim->repaints++;
    }
    memcpy(rim->sig, sig, sizeof(sig));
    rim->width = w_new;
}

static bool animated(yk_rim_mode_t m) {
    return m == YK_RIM_CONNECTING || m == YK_RIM_THINKING || m == YK_RIM_DONE || m == YK_RIM_ALERT;
}

static void tick(lv_timer_t *t) {
    yk_rim_t *rim = lv_timer_get_user_data(t);
    if (yk_rim_finished(&rim->in)) {
        if (rim->in.mode == YK_RIM_ALERT && rim->back_mode != YK_RIM_ALERT) {
            rim->in.mode = rim->back_mode;
            rim->in.opts = rim->back_opts;
            rim->in.value = rim->back_value;
            rim->since = rim->back_since;
        } else {
            rim->in.mode = YK_RIM_OFF;
        }
        if (!animated(rim->in.mode)) lv_timer_pause(t);
    }
    refresh(rim);
}

yk_rim_t *yk_rim_create(lv_obj_t *parent) {
    yk_rim_t *rim = calloc(1, sizeof(*rim));
    if (!rim) return NULL;
    rim->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(rim->obj);
    lv_obj_set_size(rim->obj, YK_CX * 2, YK_CY * 2);
    lv_obj_set_pos(rim->obj, 0, 0);
    lv_obj_set_clickable(rim->obj, false);
    lv_obj_set_scrollable(rim->obj, false);
    lv_obj_add_event_cb(rim->obj, draw_cb, LV_EVENT_DRAW_MAIN, rim);
    lv_obj_move_to_index(rim->obj, 0);  // behind everything else on the screen
    rim->in.bg = YK_PAPER;
    rim->timer = lv_timer_create(tick, FRAME_MS, rim);
    lv_timer_pause(rim->timer);
    yk_rim_picture(&rim->in, &rim->pic);
    yk_rim_signatures(&rim->pic, rim->sig);
    return rim;
}

void yk_rim_delete(yk_rim_t *rim) {
    if (!rim) return;
    lv_timer_delete(rim->timer);
    lv_obj_delete(rim->obj);
    free(rim);
}

lv_obj_t *yk_rim_obj(yk_rim_t *rim) { return rim ? rim->obj : NULL; }

void yk_rim_set(yk_rim_t *rim, yk_rim_mode_t mode, const yk_rim_opts_t *opts) {
    if (!rim) return;
    const yk_rim_opts_t none = {0};
    if (!opts) opts = &none;
    if (mode == rim->in.mode && !memcmp(opts, &rim->in.opts, sizeof(*opts))) return;  // keeps its animation going
    if (mode == YK_RIM_ALERT && rim->in.mode != YK_RIM_ALERT) {
        rim->back_mode = rim->in.mode;
        rim->back_opts = rim->in.opts;
        rim->back_value = rim->in.value;
        rim->back_since = rim->since;
    }
    if (mode != rim->in.mode) {
        rim->since = lv_tick_get();
        if (mode != YK_RIM_ALERT) rim->in.value = 0;
    }
    rim->in.mode = mode;
    rim->in.opts = *opts;
    if (animated(mode)) lv_timer_resume(rim->timer);
    else lv_timer_pause(rim->timer);
    rim->level_step = -1;
    refresh(rim);
}

yk_rim_mode_t yk_rim_mode(const yk_rim_t *rim) { return rim ? rim->in.mode : YK_RIM_OFF; }

void yk_rim_value(yk_rim_t *rim, int value) {
    if (!rim || rim->in.value == value) return;
    rim->in.value = value;
    if (rim->in.mode != YK_RIM_ALERT) refresh(rim);
}

void yk_rim_level(yk_rim_t *rim, int level) {
    if (!rim) return;
    rim->in.level = level;
    if (rim->in.mode != YK_RIM_LISTEN) return;
    const int step = yk_rim_listen_step(level);
    if (step == rim->level_step || lv_tick_elaps(rim->level_at) < LISTEN_RESTYLE_MS) return;
    rim->level_step = step;
    rim->level_at = lv_tick_get();
    refresh(rim);
}

void yk_rim_background(yk_rim_t *rim, uint32_t color) {
    if (!rim || rim->in.bg == color) return;
    rim->in.bg = color;
    refresh(rim);
}

int yk_rim_repaints(yk_rim_t *rim) {
    if (!rim) return 0;
    const int n = rim->repaints;
    rim->repaints = 0;
    return n;
}

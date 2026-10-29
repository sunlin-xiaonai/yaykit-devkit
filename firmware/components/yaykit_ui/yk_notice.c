// The same notices in every app (yk_notice.h): a line that fades, Yay's pose, the rim. The table and the network
// rule are pure (yk_ui_logic.c).
#include "yk_notice.h"

#include <stdlib.h>

#include "cjk_font.h"
#include "yk_round.h"

#define FADE_MS 150

struct yk_notice_line {
    lv_obj_t *label;
    yk_notice_t shown;
};

static void opa_exec(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

yk_notice_line_t *yk_notice_create(lv_obj_t *parent, int y) {
    yk_notice_line_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->label = lv_label_create(parent);
    lv_label_set_text(n->label, "");
    const int cy = y ? y : 360;
    // The width the circle allows at this height (spec 2): never wider than 400.
    lv_obj_set_width(n->label, 400);
    lv_obj_set_pos(n->label, YK_CX - 200, cy - YK_SIZE_STATUS);
    lv_obj_set_style_text_align(n->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(n->label, lv_color_hex(YK_INK2), 0);
    const lv_font_t *f = cjk_font_get(YK_SIZE_STATUS);
    if (f) lv_obj_set_style_text_font(n->label, f, 0);
    lv_obj_set_style_opa(n->label, LV_OPA_TRANSP, 0);
    return n;
}

yk_notice_t yk_notice_current(const yk_notice_line_t *line) { return line ? line->shown : YK_NOTICE_NONE; }

static void fade(lv_obj_t *o, lv_opa_t to) {
    lv_anim_delete(o, opa_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_values(&a, lv_obj_get_style_opa(o, 0), to);
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_start(&a);
}

void yk_notice_show(yk_notice_line_t *line, yk_notice_t notice, yk_yay_t *yay, yk_rim_t *rim, const char *override,
                    bool english) {
    const yk_notice_spec_t *s = yk_notice_spec(notice);
    const yk_notice_t was = line ? line->shown : YK_NOTICE_NONE;
    if (line) line->shown = notice;
    // The rim: turning while connecting, one amber flash for a low battery; a notice that no longer connects
    // leaves the rim off.
    if (rim) {
        if (s->rim != YK_RIM_OFF) yk_rim_set(rim, s->rim, NULL);
        else if (yk_rim_mode(rim) == YK_RIM_CONNECTING && was == YK_NOTICE_CONNECTING) yk_rim_set(rim, YK_RIM_OFF, NULL);
    }
    if (notice == YK_NOTICE_NONE) {
        if (line) fade(line->label, LV_OPA_TRANSP);
        return;
    }
    if (yay && notice != YK_NOTICE_BATTERY_LOW) yk_yay_pose(yay, s->pose, FADE_MS);
    if (!line) return;
    const char *text = override ? override : (english ? s->en : s->zh);
    if (!text || !text[0]) {
        fade(line->label, LV_OPA_TRANSP);
        return;
    }
    lv_label_set_text(line->label, text);
    fade(line->label, LV_OPA_COVER);
}

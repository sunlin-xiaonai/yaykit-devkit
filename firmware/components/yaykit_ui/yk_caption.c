// Subtitles (yk_caption.h): yk_caption_layout() (yk_ui_logic.c) breaks the sentence into lines as wide as the circle
// allows at each line's distance from the centre; the lines go into a centred label with explicit breaks. Two labels
// take turns so a new sentence crossfades over the old one.
#include "yk_caption.h"

#include <stdlib.h>
#include <string.h>

#include "cjk_font.h"
#include "yk_round.h"
#include "yk_ui_logic.h"

#define FADE_MS YK_CAPTION_FADE_MS
#define MAX_TEXT 512

struct yk_caption {
    lv_obj_t *box;
    lv_obj_t *label[2];
    int cur;
    yk_caption_opts_t o;
    char lang[8];
    char buf[MAX_TEXT + 16];
};

static int glyph_width(uint32_t cp, void *ctx) { return lv_font_get_glyph_width(ctx, cp, 0); }

static const lv_font_t *font(const yk_caption_t *c, int px) { return cjk_font_get_lang(px, c->lang); }

static void style_label(yk_caption_t *c, lv_obj_t *l, const lv_font_t *f) {
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_line_space(l, c->o.line_gap, 0);
}

yk_caption_t *yk_caption_create(lv_obj_t *parent, const yk_caption_opts_t *opts) {
    yk_caption_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (opts) c->o = *opts;
    if (!c->o.circle_x && !c->o.circle_y) c->o.circle_x = YK_CX, c->o.circle_y = YK_CY;
    // each left at 0 is the circle's (an app often gives only the height)
    if (!c->o.center_x) c->o.center_x = c->o.circle_x;
    if (!c->o.center_y) c->o.center_y = c->o.circle_y;
    if (!c->o.radius) c->o.radius = YK_R_CONTENT;
    if (!c->o.max_lines) c->o.max_lines = 3;
    if (!c->o.size) c->o.size = YK_SIZE_SUBTITLE;
    if (!c->o.small_size) c->o.small_size = YK_SIZE_SUBTITLE_SMALL;
    if (!c->o.color) c->o.color = YK_INK;
    if (!c->o.line_gap) c->o.line_gap = 8;
    snprintf(c->lang, sizeof(c->lang), "%s", c->o.lang ? c->o.lang : "zh");
    c->o.lang = c->lang;
    const int w = c->o.radius * 2;
    c->box = lv_obj_create(parent);
    lv_obj_remove_style_all(c->box);
    lv_obj_set_clickable(c->box, false);
    lv_obj_set_scrollable(c->box, false);
    lv_obj_set_size(c->box, w, LV_SIZE_CONTENT);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *l = lv_label_create(c->box);
        lv_label_set_text(l, "");
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(c->o.color), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_opa(l, LV_OPA_TRANSP, 0);
        style_label(c, l, font(c, c->o.size));
        c->label[i] = l;
    }
    lv_obj_set_pos(c->box, c->o.center_x - w / 2, c->o.center_y);
    return c;
}

void yk_caption_delete(yk_caption_t *c) {
    if (!c) return;
    lv_anim_delete(c->label[0], NULL);
    lv_anim_delete(c->label[1], NULL);
    lv_obj_delete(c->box);
    free(c);
}

lv_obj_t *yk_caption_obj(yk_caption_t *c) { return c ? c->box : NULL; }

void yk_caption_color(yk_caption_t *c, uint32_t color) {
    if (!c) return;
    c->o.color = color;
    for (int i = 0; i < 2; i++) lv_obj_set_style_text_color(c->label[i], lv_color_hex(color), 0);
}

/** Lays the text out at px; writes the broken text into c->buf. Returns the lines, or -1. */
static int lay(yk_caption_t *c, const char *text, int px, const lv_font_t **used) {
    const lv_font_t *f = font(c, px);
    *used = f;
    if (!f) return -1;
    const int line_h = lv_font_get_line_height(f) + c->o.line_gap;
    yk_caption_line_t lines[8];
    const int n = yk_caption_layout(text, glyph_width, (void *)f, line_h, c->o.center_y - c->o.circle_y, c->o.radius,
                                    c->o.max_lines, c->lang, lines);
    if (n <= 0) return n;
    size_t k = 0;
    for (int i = 0; i < n && k + lines[i].len + 2 < sizeof(c->buf); i++) {
        memcpy(c->buf + k, text + lines[i].start, lines[i].len);
        k += lines[i].len;
        if (i + 1 < n) c->buf[k++] = '\n';
    }
    c->buf[k] = 0;
    return n;
}

/** The sentence laid out at the size, then the small size, then cut with an ellipsis. */
static int fit(yk_caption_t *c, const char *text, const lv_font_t **used) {
    int n = lay(c, text, c->o.size, used);
    if (n >= 0) return n;
    n = lay(c, text, c->o.small_size, used);
    if (n >= 0) return n;
    // Too long even small: the longest start that fits with "…".
    static char cut[MAX_TEXT + 4];
    int lo = 0, hi = (int)strlen(text), best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        while (mid > 0 && ((uint8_t)text[mid] & 0xC0) == 0x80) mid--;  // a whole UTF-8 character
        memcpy(cut, text, (size_t)mid);
        strcpy(cut + mid, "…");
        if (lay(c, cut, c->o.small_size, used) >= 0) best = mid, lo = mid + 1;
        else hi = mid - 1;
        if (mid == 0) break;
    }
    memcpy(cut, text, (size_t)best);
    strcpy(cut + best, "…");
    return lay(c, cut, c->o.small_size, used);
}

static void opa_exec(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

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

/** Puts the laid-out text into label l, the block centred on center_y. */
static void place(yk_caption_t *c, lv_obj_t *l, const lv_font_t *f, int lines) {
    style_label(c, l, f);
    lv_label_set_text(l, c->buf);
    const int line_h = lv_font_get_line_height(f) + c->o.line_gap;
    lv_obj_set_y(l, 0);
    lv_obj_set_pos(c->box, c->o.center_x - c->o.radius, c->o.center_y - lines * line_h / 2);
}

static int set(yk_caption_t *c, const char *text, bool animate) {
    if (!c) return 0;
    if (!text || !text[0]) {
        fade(c->label[c->cur], LV_OPA_TRANSP);
        return 0;
    }
    char src[MAX_TEXT + 1];
    snprintf(src, sizeof(src), "%s", text);
    const lv_font_t *f = NULL;
    const int n = fit(c, src, &f);
    if (n <= 0 || !f) return 0;
    if (!animate) {
        place(c, c->label[c->cur], f, n);
        lv_obj_set_style_opa(c->label[c->cur], LV_OPA_COVER, 0);
        return n;
    }
    fade(c->label[c->cur], LV_OPA_TRANSP);
    c->cur = !c->cur;
    place(c, c->label[c->cur], f, n);
    lv_obj_set_style_opa(c->label[c->cur], LV_OPA_TRANSP, 0);
    fade(c->label[c->cur], LV_OPA_COVER);
    return n;
}

int yk_caption_set(yk_caption_t *c, const char *text) { return set(c, text, true); }

int yk_caption_update(yk_caption_t *c, const char *text) { return set(c, text, false); }

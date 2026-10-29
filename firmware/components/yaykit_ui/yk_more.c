// 「我的应用」 (yk_more.h): one drawing object over the screen. The honeycomb's place, sizes and touches are pure
// (yk_more_logic.c); this draws the apps the band shows (each skipped when the area being drawn does not reach it),
// follows a drag, and moves an app to the middle: the nearest when a drag lets go, the one tapped.
// Autofocus (docs/delivery/2026-10-26-more-autofocus.md), Apple Watch's way: each app's size, light and a halo go by
// how far it is from the middle on the screen, every frame of a drag; the nearest one is the focus as it is dragged
// (its name below follows). At rest it looks as before. Eight sizes of first characters and icons (the four at rest
// for every app, the four between them only for those near the middle).
//
// A frame only lays masks (A8, coloured as they are drawn, as party-play does): the discs by diameter, made here and
// shared by the apps and the marks; each app's first character at each of its four sizes and the three lines of
// words, drawn once onto a hidden canvas. The masks are blended straight into the band being drawn: through
// lv_draw_image each cost ~80 us a call, ~150 calls a frame. Drawing them as shapes and text every frame took ~65 ms
// a frame on the device; a frame of the whole band with nothing on it takes ~26 ms, nearly all of it sending (24 lines
// at a time). So what a move or a drag redraws is each app's box, where it was and where it is now: the paper between
// them is not sent again.
#include "yk_more.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjk_font.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "yk_apps_logic.h"
#include "yk_more_logic.h"
#include "yk_round.h"
#include "yk_ui_priv.h"

static const char *TAG = "yk_more";

#define FRAME_MS 16
#define DEV_DRAG_MS 300  // yk_more_drag(): as quick a drag as a finger's
#define DISC_N 56        // disc masks by diameter / 2: even diameters up to 110
#define GLYPHS YK_MORE_GLYPHS  // the first character's sizes (yk_more_size_class, yk_more_glyph_px)
#define PREP_US 8000     // an idle tick makes first characters for this long at most (all made in about a second)
#define BOXES_MAX 24     // more apps shown than this: the band is redrawn in strips (LVGL keeps 32 areas a frame)
#ifdef CONFIG_YK_BOARD_LCD_BUFFER_LINES
#define STRIP CONFIG_YK_BOARD_LCD_BUFFER_LINES  // what the display draws at a time
#else
#define STRIP 24
#endif
// The band the apps are drawn in: their centres' band and the biggest one's radius around it.
#define BAND_TOP (YK_MORE_TOP - YK_MORE_SIZE_0 / 2 - 4)
#define BAND_BOTTOM (YK_MORE_BOTTOM + YK_MORE_SIZE_0 / 2 + 4)

static const int GLYPH_PX[GLYPHS] = {36, 32, 28, 24, 22, 20, 18, 16};  // fonts Yay already has (no new size)
#define PANEL_LIT 0x3C4439  // autofocus: the middle one's disc, a shade lighter than YK_PANEL
_Static_assert(GLYPHS == YK_MORE_GLYPHS, "an icon size for each first character's");

/** A coverage mask (A8) cropped to its ink, and where it goes: its top left from where it is placed. */
typedef struct {
    lv_draw_buf_t *buf;
    int16_t dx, dy;
} mask_t;

typedef struct {
    char slug[32];
    char name[48];
    char first[5];
    yk_more_where_t where;
    bool current, preview;
    uint32_t bytes;        // to download: its size and how long it would take (yk_more_state_line)
    int eta_s;
    mask_t glyph[GLYPHS];  // its first character at each size: made when first wanted, or while nothing moves
    uint8_t tried;         // bit g: glyph[g] (or ico[g]) made, or it could not be (no font, no memory): not tried again
    const uint8_t *icon;   // the caller's A8 64 x 64 (NULL: the first character), and its four sizes, made as the
    uint32_t icon_color;   // first characters are (yk_more_icon_px)
    mask_t ico[GLYPHS];
    lv_area_t box, next;   // its box as it is drawn now (boxed: shown), and as it will be (is)
    bool boxed, is;
    int16_t cx, cy;        // this frame (worked out once a frame, not for each band of it)
    float size;
    lv_opa_t opa;
    float lit;             // 1 in the middle, to 0 half a step away (yk_more_lit): the halo and the lighter disc
    bool shown;
} app_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *canvas;            // hidden: the first characters and the lines are drawn onto it once, as masks
    lv_timer_t *timer;
    app_t *app;                  // PSRAM, YK_MORE_MAX (so is all that is big: internal RAM is Wi-Fi's and TLS's)
    int n, focus;
    int from;                    // the middle one before a move: the sizes go from its to focus's
    float k;                     // how far the move is (yk_more_ease); 1 at rest
    float ox, oy;                // the honeycomb's origin from the middle
    float ax, ay, bx, by;        // a move: from, to
    uint32_t t0;
    bool moving;
    bool pressed, dragging, dragged;
    lv_point_t press_at;
    float press_ox, press_oy;
    bool dev_drag;               // yk_more_drag(): a finger's drag played by the timer
    int dev_dx, dev_dy;
    bool shaking;                // the middle one, coming soon, tapped: it shakes a little (since t0)
    bool prep;                   // first characters still to make while nothing moves
    lv_draw_buf_t **disc;        // PSRAM, DISC_N: A8 discs by diameter / 2, made when first drawn ...
    uint8_t **rows;              // ... and for each row of each, where its left edge and its solid middle begin
    lv_draw_buf_t **arrow;       // PSRAM, DISC_N: the download mark's arrow, by its app's diameter / 2
    float *icon_tmp;             // PSRAM: yk_more_icon_scale's scratch, and its picture (made when the first is)
    uint8_t *icon_px;
    mask_t line[3];              // the title, the middle one's name, where it is
    const lv_font_t *glyph_font[GLYPHS];
    const lv_font_t *title_font, *name_font, *state_font;
    char title[40], name[96], state[64];
    yk_more_cb_t cb;
    void *ctx;
    int64_t opened_us;
    bool first_frame;
    int64_t refr_us;
} s;

static yk_more_stats_t s_stats;
static uint8_t s_halo_opa = YK_MORE_HALO_OPA;  // yk_more_dev_halo() changes it for the comparison pictures
static int64_t s_frame_sum_us;
static int s_bands, s_px, s_draw_us;            // this frame's
static int64_t s_bands_sum, s_px_sum, s_draw_sum;  // the moving frames

static int glyph_w(uint32_t cp, void *ctx) { return lv_font_get_glyph_width((const lv_font_t *)ctx, cp, 0); }


// ---------------------------------------------------------------- masks

/** A disc's coverage, `diameter` rounded to an even number of pixels (its centre on a pixel corner). */
static const lv_draw_buf_t *disc_mask(float diameter) {
    int h = (int)lroundf(diameter / 2);
    if (h < 1) h = 1;
    if (h >= DISC_N) h = DISC_N - 1;
    if (s.disc[h]) return s.disc[h];
    const int d = 2 * h;
    lv_draw_buf_t *b = lv_draw_buf_create(d, d, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!b) return NULL;
    const uint32_t st = b->header.stride;
    for (int y = 0; y < h; y++)  // a quarter, mirrored
        for (int x = 0; x < h; x++) {
            const float e = h + 0.5f - hypotf(x + 0.5f, y + 0.5f);
            const uint8_t v = e <= 0 ? 0 : e >= 1 ? 255 : (uint8_t)(e * 255);
            b->data[(h + y) * st + h + x] = b->data[(h + y) * st + h - 1 - x] = v;
            b->data[(h - 1 - y) * st + h + x] = b->data[(h - 1 - y) * st + h - 1 - x] = v;
        }
    // Row by row: the first pixel with any coverage, the first fully covered one (h: none).
    uint8_t *r = heap_caps_malloc(2 * d, MALLOC_CAP_SPIRAM);
    if (r) {
        for (int y = 0; y < d; y++) {
            int e = 0, f;
            while (e < h && !b->data[y * st + e]) e++;
            for (f = e; f < h && b->data[y * st + f] < 255; f++) {}
            r[2 * y] = (uint8_t)e, r[2 * y + 1] = (uint8_t)f;
        }
        s.rows[h] = r;
    }
    return s.disc[h] = b;
}

/** Distance from (px, py) to the segment a-b. */
static float to_segment(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, ll = vx * vx + vy * vy;
    float t = ll > 0 ? ((px - ax) * vx + (py - ay) * vy) / ll : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypotf(px - ax - t * vx, py - ay - t * vy);
}

/** The download mark's arrow for an app of diameter 2h (yk_draw_download_mark's three round-ended lines), centred
 *  on the mark: its box is 2E square, the mark's centre on its middle pixel corner. */
static const lv_draw_buf_t *arrow_mask(int h) {
    if (h < 1 || h >= DISC_N) return NULL;
    if (s.arrow[h]) return s.arrow[h];
    const float k = (float)h / YK_APPS_SAT_R, w = 2.4f * k / 2;
    const int E = (int)ceilf(5 * k + w + 1);
    lv_draw_buf_t *b = lv_draw_buf_create(2 * E, 2 * E, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!b) return NULL;
    const uint32_t st = b->header.stride;
    for (int y = 0; y < 2 * E; y++)
        for (int x = 0; x < 2 * E; x++) {
            const float px = x + 0.5f - E, py = y + 0.5f - E;
            float d = to_segment(px, py, 0, -5 * k, 0, 3 * k);
            d = fminf(d, to_segment(px, py, -4 * k, 0, 0, 4 * k));
            d = fminf(d, to_segment(px, py, 0, 4 * k, 4 * k, 0));
            const float e = w + 0.5f - d;
            b->data[y * st + x] = e <= 0 ? 0 : e >= 1 ? 255 : (uint8_t)(e * 255);
        }
    return s.arrow[h] = b;
}

/** `str` in `font` onto the canvas, centred in a box `w` wide and a line tall, kept as a mask cropped to its ink;
 *  dx, dy: the crop's top left from the box's top middle. False with nothing to show (or no memory). Not while the
 *  screen is being drawn (from a timer or an input event). */
static bool text_mask(const char *str, const lv_font_t *font, int w, mask_t *out) {
    if (out->buf) lv_draw_buf_destroy(out->buf);
    *out = (mask_t){0};
    if (!font || !str || !str[0] || !s.canvas) return false;
    const int h = lv_font_get_line_height(font);
    lv_draw_buf_t *tmp = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!tmp) return false;
    lv_draw_buf_clear(tmp, NULL);
    lv_canvas_set_draw_buf(s.canvas, tmp);
    lv_layer_t layer;
    lv_canvas_init_layer(s.canvas, &layer);
    const lv_area_t a = {0, 0, w - 1, h - 1};
    yk_draw_text(&layer, str, font, 0xFFFFFF, LV_OPA_COVER, &a);
    lv_canvas_finish_layer(s.canvas, &layer);
    const uint32_t st = tmp->header.stride;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (tmp->data[y * st + x]) x0 = LV_MIN(x0, x), y0 = LV_MIN(y0, y), x1 = LV_MAX(x1, x), y1 = LV_MAX(y1, y);
    lv_draw_buf_t *b = x1 >= 0 ? lv_draw_buf_create(x1 - x0 + 1, y1 - y0 + 1, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO) : NULL;
    if (b) {
        for (int y = y0; y <= y1; y++) memcpy(b->data + (y - y0) * b->header.stride, tmp->data + y * st + x0, x1 - x0 + 1);
        *out = (mask_t){.buf = b, .dx = (int16_t)(x0 - w / 2), .dy = (int16_t)y0};
    }
    lv_draw_buf_destroy(tmp);
    return b != NULL;
}

/** App i's first character at size g, made once. */
static const mask_t *glyph_mask(int i, int g, bool make) {
    app_t *a = &s.app[i];
    if (a->glyph[g].buf || (a->tried & (1 << g)) || !make) return a->glyph[g].buf ? &a->glyph[g] : NULL;
    a->tried |= 1 << g;
    text_mask(a->first, s.glyph_font[g], 2 * GLYPH_PX[g], &a->glyph[g]);
    return a->glyph[g].buf ? &a->glyph[g] : NULL;
}

/** App i's icon at size g (yk_more_icon_px), cropped to its ink: dx, dy its top left from the disc's centre. */
static bool icon_mask(const uint8_t *icon, int g, mask_t *out) {
    *out = (mask_t){0};
    if (!s.icon_tmp) {
        s.icon_tmp = heap_caps_malloc(YK_MORE_ICON_TMP * sizeof(float), MALLOC_CAP_SPIRAM);
        s.icon_px = heap_caps_malloc(128 * 128, MALLOC_CAP_SPIRAM);
        if (!s.icon_tmp || !s.icon_px) {
            heap_caps_free(s.icon_tmp), heap_caps_free(s.icon_px);
            s.icon_tmp = NULL, s.icon_px = NULL;
            return false;
        }
    }
    const int n = yk_more_icon_px(g);
    yk_more_icon_scale(icon, n, s.icon_px, s.icon_tmp);
    int x0 = n, y0 = n, x1 = -1, y1 = -1;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (s.icon_px[y * n + x]) x0 = LV_MIN(x0, x), y0 = LV_MIN(y0, y), x1 = LV_MAX(x1, x), y1 = LV_MAX(y1, y);
    lv_draw_buf_t *b = x1 >= 0 ? lv_draw_buf_create(x1 - x0 + 1, y1 - y0 + 1, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO) : NULL;
    if (!b) return false;
    for (int y = y0; y <= y1; y++) memcpy(b->data + (y - y0) * b->header.stride, s.icon_px + y * n + x0, x1 - x0 + 1);
    *out = (mask_t){.buf = b, .dx = (int16_t)(x0 - n / 2), .dy = (int16_t)(y0 - n / 2)};
    return true;
}

/** What goes on app i's disc at size g: its icon, else its first character; made once. */
static const mask_t *face_mask(int i, int g, bool make) {
    app_t *a = &s.app[i];
    if (!a->icon) return glyph_mask(i, g, make);
    if (a->ico[g].buf || (a->tried & (1 << g)) || !make) return a->ico[g].buf ? &a->ico[g] : NULL;
    a->tried |= 1 << g;
    if (icon_mask(a->icon, g, &a->ico[g])) return &a->ico[g];
    a->icon = NULL;  // no memory for it: the first character from now on, at every size
    a->tried = 0;
    return glyph_mask(i, g, make);
}

/** While nothing moves: the icons and first characters not made yet, nearest the middle one first (each app's size
 *  now first, then its others), PREP_US at most. False once all are made. */
static bool prep_step(void) {
    const int64_t until = esp_timer_get_time() + PREP_US;
    int order[YK_MORE_MAX];
    float d[YK_MORE_MAX];
    for (int i = 0; i < s.n; i++) {
        d[i] = yk_more_apart(i, s.focus);
        int j = i;
        for (; j > 0 && d[order[j - 1]] > d[i]; j--) order[j] = order[j - 1];
        order[j] = i;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < s.n; k++) {
            const int i = order[k], now = yk_more_size_class(yk_more_size(d[i]));
            for (int g = pass ? 0 : now; g <= (pass ? GLYPHS - 1 : now); g++) {
                if (s.app[i].tried & (1 << g)) continue;
                // The in-between sizes only for those near the middle: the others pass them only on the way there.
                if (pass && !yk_more_class_at_rest(g) && d[i] > YK_MORE_NEAR) continue;
                if (esp_timer_get_time() >= until) return true;
                face_mask(i, g, true);
            }
        }
    return false;
}

/** a and b's overlap into out; false when they do not. */
static bool overlap(lv_area_t *out, const lv_area_t *a, const lv_area_t *b) {
    out->x1 = LV_MAX(a->x1, b->x1), out->y1 = LV_MAX(a->y1, b->y1);
    out->x2 = LV_MIN(a->x2, b->x2), out->y2 = LV_MIN(a->y2, b->y2);
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

static bool clipped(const lv_layer_t *layer, const lv_area_t *a) {
    const lv_area_t *clip = &layer->_clip_area;
    return a->x2 < clip->x1 || a->x1 > clip->x2 || a->y2 < clip->y1 || a->y1 > clip->y2;
}

/** A mask (an lv_draw_buf_t or lv_image_dsc_t, w x h) with its top left at (x0, y0), in colour c; nothing where the
 *  area being drawn is not. */
static void blit_src(lv_layer_t *layer, const void *src, int w, int h, int x0, int y0, uint32_t c, lv_opa_t opa) {
    const lv_area_t a = {x0, y0, x0 + w - 1, y0 + h - 1};
    if (!src || opa <= LV_OPA_MIN || clipped(layer, &a)) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = src;
    d.opa = opa;
    d.recolor = lv_color_hex(c);  // an A8 image is drawn in its recolour
    lv_draw_image(layer, &d, &a);
}

/** fg over bg at alpha a (0-255), RGB565: red and blue with 6 bits of alpha, green with 8. */
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint32_t a) {
    uint32_t rb = bg & 0xF81F, g = bg & 0x07E0;
    rb += (((fg & 0xF81F) - rb) * (a >> 2)) >> 6;
    g += (((fg & 0x07E0) - g) * a) >> 8;
    return (uint16_t)((rb & 0xF81F) | (g & 0x07E0));
}

/** The same, blended straight into the band being drawn when it is the display's (RGB565): its earlier draw tasks
 *  are done by now (LVGL without an OS draws each as it is added), and LVGL's image drawing is what costs. */
static void blit(lv_layer_t *layer, const lv_draw_buf_t *b, int x0, int y0, uint32_t c, lv_opa_t opa) {
    if (!b || opa <= LV_OPA_MIN) return;
    lv_draw_buf_t *db = layer->draw_buf;
    if (!db || db->header.cf != LV_COLOR_FORMAT_RGB565) {  // a snapshot, say
        blit_src(layer, b, (int)b->header.w, (int)b->header.h, x0, y0, c, opa);
        return;
    }
    const lv_area_t a = {x0, y0, x0 + (int)b->header.w - 1, y0 + (int)b->header.h - 1};
    lv_area_t in;
    if (!overlap(&in, &a, &layer->_clip_area) || !overlap(&in, &in, &layer->buf_area)) return;
    const uint16_t col = lv_color_to_u16(lv_color_hex(c));
    const bool full = opa >= LV_OPA_MAX;
    const int w = lv_area_get_width(&in);
    const uint32_t mst = b->header.stride, dst = db->header.stride;
    for (int y = in.y1; y <= in.y2; y++) {
        uint16_t *d = (uint16_t *)(db->data + (y - layer->buf_area.y1) * dst) + (in.x1 - layer->buf_area.x1);
        const uint8_t *m = b->data + (y - y0) * mst + (in.x1 - x0);
        for (int x = 0; x < w; x++) {
            uint32_t v = m[x];
            if (!v) continue;
            if (!full) v = (v * opa + 128) >> 8;
            d[x] = v >= LV_OPA_MAX ? col : blend565(col, d[x], v);
        }
    }
}

/** Pixels x0..x1 of row d (the band's, clipped to [lo, hi]) in col. */
static inline void fill_row(uint16_t *d, int x0, int x1, int lo, int hi, uint16_t col) {
    x0 = LV_MAX(x0, lo), x1 = LV_MIN(x1, hi);
    for (int x = x0; x <= x1; x++) d[x] = col;
}

/** A disc centred on (cx, cy). Into the display's band: each row's solid middle filled, its two edges blended from
 *  the mask (a big disc's whole mask from PSRAM cost ~0.5 ms a frame). Only paper is under a disc (they do not
 *  overlap), so a dimmed one's middle is one colour too. */
static void disc(lv_layer_t *layer, int cx, int cy, float diameter, uint32_t c, lv_opa_t opa) {
    const lv_draw_buf_t *b = disc_mask(diameter);
    if (!b || opa <= LV_OPA_MIN) return;
    const int h = (int)b->header.w / 2;
    lv_draw_buf_t *db = layer->draw_buf;
    const uint8_t *r = s.rows[h];
    if (!db || db->header.cf != LV_COLOR_FORMAT_RGB565 || !r) {
        blit(layer, b, cx - h, cy - h, c, opa);
        return;
    }
    const lv_area_t a = {cx - h, cy - h, cx + h - 1, cy + h - 1};
    lv_area_t in;
    if (!overlap(&in, &a, &layer->_clip_area) || !overlap(&in, &in, &layer->buf_area)) return;
    const uint16_t col = lv_color_to_u16(lv_color_hex(c));
    const bool full = opa >= LV_OPA_MAX;
    const uint16_t solid = full ? col : blend565(col, lv_color_to_u16(lv_color_hex(YK_PAPER)), opa);
    const uint32_t mst = b->header.stride, dst = db->header.stride;
    const int bx = layer->buf_area.x1, lo = in.x1 - bx, hi = in.x2 - bx, left = cx - h - bx;
    for (int y = in.y1; y <= in.y2; y++) {
        uint16_t *d = (uint16_t *)(db->data + (y - layer->buf_area.y1) * dst);
        const int my = y - (cy - h), e = r[2 * my], f = r[2 * my + 1];
        if (e >= h) continue;
        const uint8_t *m = b->data + my * mst;
        for (int x = e; x < f; x++) {  // the edges, mirrored
            const uint32_t v = full ? m[x] : (m[x] * opa + 128) >> 8;
            const int xl = left + x, xr = left + 2 * h - 1 - x;
            if (xl >= lo && xl <= hi) d[xl] = blend565(col, d[xl], v);
            if (xr >= lo && xr <= hi) d[xr] = blend565(col, d[xr], v);
        }
        if (f < h) fill_row(d, left + f, left + 2 * h - 1 - f, lo, hi, solid);
    }
}

// ---------------------------------------------------------------- drawing

static bool reaches(const lv_area_t *clip, int cx, int cy, int r) {
    return cx + r >= clip->x1 && cx - r <= clip->x2 && cy + r >= clip->y1 && cy - r <= clip->y2;
}

/** How far an app of radius r draws from its centre: its marks stay inside r (the download mark's edge a pixel past
 *  it); the ring of the one in use reaches 1.13 r. */
static int reach(float r, bool current) { return (int)ceilf(current ? r * 1.15f : r) + 2 + YK_MORE_HALO_W; }

/** An app's size, opacity and light where it is on the screen now: by its distance from the middle (autofocus), so
 *  every frame of a drag changes them; at rest the same as by the honeycomb. */
static float size_at(float x, float y, lv_opa_t *opa, float *lit) {
    const float d = yk_more_from_middle(x, y);
    *opa = yk_more_opa(d);
    if (lit) *lit = yk_more_lit(d);
    return yk_more_size(d);
}

/** How far app i is shaken to the side now. */
static float shake_dx(int i) { return s.shaking && i == s.focus ? yk_more_shake((int)lv_tick_elaps(s.t0)) : 0; }

/** Where each app is this frame, and how big: once, before its first band. */
static void frame_at(void) {
    for (int i = 0; i < s.n; i++) {
        float x, y;
        yk_more_at(yk_more_place(i), s.ox, s.oy, &x, &y);
        s.app[i].size = size_at(x, y, &s.app[i].opa, &s.app[i].lit);
        x += shake_dx(i);
        s.app[i].shown = yk_more_shown(x, y);
        s.app[i].cx = (int16_t)lroundf(x), s.app[i].cy = (int16_t)lroundf(y);
    }
}

static void draw_app(lv_layer_t *layer, int i) {
    const app_t *a = &s.app[i];
    lv_opa_t opa = s.app[i].opa;
    const float size = s.app[i].size, r = size / 2, k = r / YK_APPS_SAT_R;
    const int cx = s.app[i].cx, cy = s.app[i].cy;
    if (!reaches(&layer->_clip_area, cx, cy, reach(r, a->current))) return;
    const bool middle = i == s.focus;
    const bool cloud = a->where == YK_MORE_CLOUD && !a->current;
    const bool dim = cloud && !middle;  // the middle one as it is (the design's)
    const bool soon = a->where == YK_MORE_SOON;
    if (soon) opa = (lv_opa_t)(opa * YK_MORE_CLOUD_OPA / 255);  // all of it at 42 %, in the middle too
    // Autofocus: the one near the middle lit, a soft ink halo round it and its disc a shade lighter, more the nearer.
    if (a->lit > 0.02f) disc(layer, cx, cy, size + 2 * YK_MORE_HALO_W, YK_INK, (lv_opa_t)(opa * a->lit * s_halo_opa / 255));
    disc(layer, cx, cy, size, a->lit > 0.02f ? yk_rim_mix(PANEL_LIT, YK_PANEL, (lv_opa_t)(255 * a->lit)) : YK_PANEL, opa);
    const uint32_t ink = dim ? yk_rim_mix(YK_INK, YK_PANEL, YK_MORE_CLOUD_OPA) : YK_INK;
    // Its size's mask, or the nearest one made (a size between two not made yet: made while nothing moves).
    int g = yk_more_size_class(size);
    if (!face_mask(i, g, false)) {
        s.prep = true;
        if (s.timer) lv_timer_resume(s.timer);
        for (int step = 1; step < GLYPHS; step++) {
            if (g - step >= 0 && face_mask(i, g - step, false)) { g -= step; break; }
            if (g + step < GLYPHS && face_mask(i, g + step, false)) { g += step; break; }
        }
    }
    const mask_t *ic = a->icon ? face_mask(i, g, false) : NULL;
    const mask_t *m = ic ? NULL : glyph_mask(i, g, false);
    const lv_font_t *f = s.glyph_font[g];
    if (ic) {  // in its colour, dimmed as the first character is
        const uint32_t c = dim ? yk_rim_mix(a->icon_color, YK_PANEL, YK_MORE_CLOUD_OPA) : a->icon_color;
        blit(layer, ic->buf, cx + ic->dx, cy + ic->dy, c, opa);
    } else if (m) {
        blit(layer, m->buf, cx + m->dx, cy - lv_font_get_line_height(f) / 2 + m->dy, ink, opa);
    } else if (f && a->first[0]) {  // not made yet: as text, once (the timer makes it)
        const int h = lv_font_get_line_height(f);
        const lv_area_t box = {cx - (int)r, cy - h / 2, cx + (int)r - 1, cy - h / 2 + h - 1};
        yk_draw_text(layer, a->first, f, ink, opa, &box);  // (an icon not made yet: its first character this frame)
        s.prep = true;
        if (s.timer) lv_timer_resume(s.timer);
    }
    if (a->current) yk_draw_in_use(layer, cx, cy, YK_APPS_SAT_R, k, opa);
    if (size <= 50 || soon) return;  // the marks only on the bigger ones (the design's); none on one coming soon
    if (a->where == YK_MORE_CACHE && !a->current) {
        // a hollow sage ring: sage, and paper inside
        const int mx = cx + (int)lroundf(YK_APPS_CACHE_D * k), my = cy + (int)lroundf(YK_APPS_CACHE_D * k);
        disc(layer, mx, my, 2 * YK_APPS_CACHE_R * k, YK_SAGE, opa);
        disc(layer, mx, my, 2 * (YK_APPS_CACHE_R - YK_APPS_CACHE_W) * k, YK_PAPER, opa);
    } else if (cloud) {
        // a sage dot with a paper arrow pointing down
        const int bx = cx + (int)lroundf(YK_APPS_BADGE_D * k), by = cy + (int)lroundf(YK_APPS_BADGE_D * k);
        disc(layer, bx, by, 2 * YK_APPS_BADGE_R * k, YK_SAGE, opa);
        const lv_draw_buf_t *b = k >= 0.5f ? arrow_mask((int)lroundf(r)) : NULL;
        if (b) blit(layer, b, bx - (int)b->header.w / 2, by - (int)b->header.h / 2, YK_PAPER, opa);
    }
}

static void draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    const int64_t t0 = esp_timer_get_time();
    s_bands++;
    s_px += lv_area_get_size(&layer->_clip_area);
    int shown = 0;
    for (int i = 0; i < s.n; i++) {
        if (!s.app[i].shown) continue;
        shown++;
        draw_app(layer, i);
    }
    if (shown > s_stats.shown_max) s_stats.shown_max = shown;
    // Grey for what is not on the device yet (to download, coming soon), sage for what is
    const bool cloud = s.n && (s.app[s.focus].where == YK_MORE_CLOUD || s.app[s.focus].where == YK_MORE_SOON) &&
                       !s.app[s.focus].current;
    const struct {
        const lv_font_t *font;
        int y;
        uint32_t color;
    } lines[3] = {
        {s.title_font, YK_MORE_TITLE_Y, YK_INK2},
        {s.name_font, YK_MORE_NAME_Y, YK_INK},
        {s.state_font, YK_MORE_STATE_Y, cloud ? YK_INK2 : YK_SAGE_TEXT},
    };
    for (int i = 0; i < 3; i++)
        if (s.line[i].buf) blit(layer, s.line[i].buf, YK_CX + s.line[i].dx, lines[i].y + s.line[i].dy, lines[i].color, LV_OPA_COVER);
    s_draw_us += (int)(esp_timer_get_time() - t0);
}

/** The band the apps are drawn in, in strips the display's height, each as wide as the glass is there. */
static void invalidate_strips(void) {
    for (int y = BAND_TOP; y <= BAND_BOTTOM; y += STRIP) {
        const int y2 = LV_MIN(y + STRIP - 1, BAND_BOTTOM);
        const int near = y <= YK_CY && y2 >= YK_CY ? 0 : LV_MIN(abs(y - YK_CY), abs(y2 - YK_CY));
        const int half = near >= YK_CX ? 0 : (int)ceilf(sqrtf((float)(YK_CX * YK_CX - near * near)));
        const lv_area_t a = {YK_CX - half, y, YK_CX + half - 1, y2};
        lv_obj_invalidate_area(s.root, &a);
    }
}

/** App i's box as it would be drawn now; false when it is not shown. */
static bool app_box(int i, lv_area_t *out) {
    float x, y;
    yk_more_at(yk_more_place(i), s.ox, s.oy, &x, &y);
    if (!yk_more_shown(x, y)) return false;
    lv_opa_t opa;
    const float size = size_at(x, y, &opa, NULL);
    x += shake_dx(i);
    const int cx = (int)lroundf(x), cy = (int)lroundf(y), h = reach(size / 2, s.app[i].current);
    *out = (lv_area_t){cx - h, cy - h, cx + h, cy + h};
    return true;
}

/** Everything a move or a drag changed since the last frame: each app's box where it was drawn and where it is now
 *  (one area: an app moves a few pixels a frame); all of it in strips when too many apps are shown for LVGL's areas. */
static void invalidate_band(void) {
    int shown = 0;
    for (int i = 0; i < s.n; i++) {
        app_t *p = &s.app[i];
        p->is = app_box(i, &p->next);
        shown += p->is || p->boxed;
    }
    for (int i = 0; i < s.n; i++) {
        app_t *p = &s.app[i];
        if (shown <= BOXES_MAX && (p->is || p->boxed)) {
            lv_area_t a = p->is ? p->next : p->box;
            if (p->is && p->boxed) {
                a.x1 = LV_MIN(a.x1, s.app[i].box.x1), a.y1 = LV_MIN(a.y1, s.app[i].box.y1);
                a.x2 = LV_MAX(a.x2, s.app[i].box.x2), a.y2 = LV_MAX(a.y2, s.app[i].box.y2);
            }
            lv_obj_invalidate_area(s.root, &a);
        }
        p->boxed = p->is;
        if (p->is) p->box = p->next;
    }
    if (shown > BOXES_MAX) invalidate_strips();
}

/** The two lines below: the middle one's name and where it is (made into masks here: not while drawing). */
static void set_lines(void) {
    const bool en = yk_ui_is_english();
    const app_t *a = s.n ? &s.app[s.focus] : NULL;
    const char *name = a ? a->name : en ? "No apps yet" : "还没有应用";
    char line[64] = "";
    if (a) yk_more_state_line(a->where, a->current, a->preview, a->bytes, a->eta_s, en, line, sizeof(line));
    const char *state = line;
    if (s.name_font) {
        const int room = yk_more_line_room(YK_MORE_NAME_Y, lv_font_get_line_height(s.name_font));
        yk_text_fit(name, glyph_w, (void *)s.name_font, room, s.name, sizeof(s.name));
    } else {
        snprintf(s.name, sizeof(s.name), "%s", name);
    }
    if (s.state_font) {
        const int room = yk_more_line_room(YK_MORE_STATE_Y, lv_font_get_line_height(s.state_font));
        yk_text_fit(state, glyph_w, (void *)s.state_font, room, s.state, sizeof(s.state));
    } else {
        snprintf(s.state, sizeof(s.state), "%s", state);
    }
    // Redrawn where the words were and where they are.
    static const int Y[3] = {YK_MORE_TITLE_Y, YK_MORE_NAME_Y, YK_MORE_STATE_Y};
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            text_mask(s.name, s.name_font, YK_MORE_LINE_W, &s.line[1]);
            text_mask(s.state, s.state_font, YK_MORE_LINE_W, &s.line[2]);
        }
        for (int i = 1; i < 3; i++) {
            const mask_t *m = &s.line[i];
            if (!m->buf) continue;
            const lv_area_t a = {YK_CX + m->dx, Y[i] + m->dy, YK_CX + m->dx + (int)m->buf->header.w - 1,
                                 Y[i] + m->dy + (int)m->buf->header.h - 1};
            lv_obj_invalidate_area(s.root, &a);
        }
    }
}

// ---------------------------------------------------------------- motion and touches

/** Moves app i to the middle (from wherever the honeycomb is), its sizes coming with it. */
static void move_to(int i) {
    s.shaking = false;
    s.from = s.focus;
    s.focus = i;
    s.k = s.from == i ? 1 : 0;
    s.ax = s.ox, s.ay = s.oy;
    yk_more_offset_for(i, &s.bx, &s.by);
    s.t0 = lv_tick_get();
    s.moving = true;
    // The first characters the move shows (each app's sizes from the last middle one's to the new one's), before its
    // first frame: drawn as text, one would cost a frame twice the time. The idle ticks have made most of them.
    for (int j = 0; j < s.n; j++) {
        float x0, y0, x1, y1;
        yk_more_at(yk_more_place(j), s.ax, s.ay, &x0, &y0);
        yk_more_at(yk_more_place(j), s.bx, s.by, &x1, &y1);
        if (!yk_more_shown(x0, y0) && !yk_more_shown(x1, y1)) continue;
        const int g0 = yk_more_size_class(yk_more_size(yk_more_apart(j, s.from)));
        const int g1 = yk_more_size_class(yk_more_size(yk_more_apart(j, i)));
        for (int g = LV_MIN(g0, g1); g <= LV_MAX(g0, g1); g++) face_mask(j, g, true);
    }
    s.prep = true;
    lv_timer_resume(s.timer);
    set_lines();
}

/** Autofocus: the nearest to the middle is the focus while the honeycomb moves; its name and where it is follow. */
static void follow_focus(void) {
    const int f = yk_more_nearest(s.n, s.ox, s.oy);
    if (f == s.focus) return;
    s.focus = s.from = f;
    s.k = 1;
    set_lines();
}

static void tick(lv_timer_t *t) {
    if (s.shaking) {
        if (lv_tick_elaps(s.t0) >= YK_MORE_SHAKE_MS) s.shaking = false;  // its last frame: back in place
        invalidate_band();
        return;
    }
    if (s.dev_drag) {
        const int ms = (int)lv_tick_elaps(s.t0);
        const float f = ms >= DEV_DRAG_MS ? 1 : (float)ms / DEV_DRAG_MS;
        float ox = s.press_ox + s.dev_dx * f, oy = s.press_oy + s.dev_dy * f;
        yk_more_clamp(s.n, &ox, &oy);
        s.ox = ox, s.oy = oy;
        follow_focus();
        invalidate_band();
        if (f >= 1) {  // let go
            s.dev_drag = s.dragging = false;
            move_to(yk_more_nearest(s.n, s.ox, s.oy));
        }
        return;
    }
    if (!s.moving) {
        if (!s.pressed && s.prep) s.prep = prep_step();
        if (!s.prep) lv_timer_pause(t);
        return;
    }
    const int ms = (int)lv_tick_elaps(s.t0);
    const bool done = yk_more_move(s.ax, s.ay, s.bx, s.by, ms, &s.ox, &s.oy);
    if (s.from != s.focus) s.k = yk_more_ease(ms);
    invalidate_band();
    if (done) s.moving = false, s.k = 1, s.from = s.focus;
}

static void on_press(lv_event_t *e) {
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_indev_get_point(in, &s.press_at);
    s.pressed = true;
    s.dragging = s.dragged = false;
    if (s.shaking) {  // back in place under the finger
        s.shaking = false;
        invalidate_band();
    }
    if (s.moving) {  // a finger takes it where it is, with the sizes it was going to
        s.moving = false;
        s.k = 1, s.from = s.focus;
        invalidate_band();
    }
    s.press_ox = s.ox, s.press_oy = s.oy;
}

static void on_pressing(lv_event_t *e) {
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in || !s.pressed) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    const int dx = p.x - s.press_at.x, dy = p.y - s.press_at.y;
    // A drag from LVGL's own threshold on (as a scroll starts): a tap below it.
    if (!s.dragging && abs(dx) <= LV_INDEV_DEF_SCROLL_LIMIT && abs(dy) <= LV_INDEV_DEF_SCROLL_LIMIT) return;
    s.dragging = true;
    float ox = s.press_ox + (float)dx, oy = s.press_oy + (float)dy;
    yk_more_clamp(s.n, &ox, &oy);
    if (ox == s.ox && oy == s.oy) return;
    s.ox = ox, s.oy = oy;
    follow_focus();
    invalidate_band();
}

static void on_release(lv_event_t *e) {
    (void)e;
    s.pressed = false;
    s.dragged = s.dragging;
    // The nearest to the middle comes there (the same one: back into place, the honeycomb's bounce); no flinging on.
    if (s.dragging) move_to(yk_more_nearest(s.n, s.ox, s.oy));
    s.dragging = false;
}

static void tapped(int i) {
    if (i < 0) return;
    // Not the middle one: on the device, opened at once; to download or coming soon, to the middle first so that its
    // line below (要下载 · 约 40 秒, 即将推出) is seen (the delivery 2.6 C).
    if (i != s.focus && !yk_more_opens_at_once(s.app[i].where)) {
        move_to(i);
        return;
    }
    if (s.app[i].where == YK_MORE_SOON) {  // not out yet: it only shakes a little (no install, no page)
        if (!s.moving) {
            s.shaking = true;
            s.t0 = lv_tick_get();
            lv_timer_resume(s.timer);
        }
        return;
    }
    if (s.cb) s.cb(i, s.ctx);
}

static void on_click(lv_event_t *e) {
    (void)e;
    if (s.dragged) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    tapped(yk_more_hit(s.n, s.focus, s.ox, s.oy, p.x, p.y));
}

/** The frames while it moves (yay perf has the rest): render and send, between the display's two events. */
static void on_refr(lv_event_t *e) {
    if (!s.root) return;
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s.refr_us = esp_timer_get_time();
        s_bands = s_px = s_draw_us = 0;
        frame_at();
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (!s.first_frame) {
        s.first_frame = true;
        s_stats.first_frame_ms = (int)((now - s.opened_us) / 1000);
        return;
    }
    if (!s.refr_us || (!s.moving && !s.dragging)) return;
    const int ms = (int)((now - s.refr_us) / 1000);
    s_stats.frames++;
    s_frame_sum_us += now - s.refr_us;
    if (ms > s_stats.frame_max_ms) s_stats.frame_max_ms = ms;
    int *part = s.dragging ? &s_stats.drag_max_ms : &s_stats.move_max_ms;
    if (ms > *part) *part = ms;
    s_stats.frame_avg_ms = (int)(s_frame_sum_us / s_stats.frames / 1000);
    s_bands_sum += s_bands, s_px_sum += s_px, s_draw_sum += s_draw_us;
    s_stats.bands = (int)(s_bands_sum / s_stats.frames);
    s_stats.kpx = (int)(s_px_sum / s_stats.frames / 1000);
    s_stats.draw_ms = (int)(s_draw_sum / s_stats.frames / 1000);
}

static void on_delete(lv_event_t *e) {
    (void)e;
    lv_display_t *d = lv_display_get_default();
    if (d) {
        lv_display_remove_event_cb_with_user_data(d, on_refr, NULL);
    }
    if (s.timer) lv_timer_delete(s.timer);
    if (s.canvas) lv_obj_delete(s.canvas);  // before the masks it last drew onto
    for (int i = 0; s.app && i < s.n; i++)
        for (int g = 0; g < GLYPHS; g++) {
            if (s.app[i].glyph[g].buf) lv_draw_buf_destroy(s.app[i].glyph[g].buf);
            if (s.app[i].ico[g].buf) lv_draw_buf_destroy(s.app[i].ico[g].buf);
        }
    heap_caps_free(s.icon_tmp);
    heap_caps_free(s.icon_px);
    for (int h = 0; s.disc && h < DISC_N; h++) {
        if (s.disc[h]) lv_draw_buf_destroy(s.disc[h]);
        if (s.arrow[h]) lv_draw_buf_destroy(s.arrow[h]);
        heap_caps_free(s.rows[h]);
    }
    heap_caps_free(s.disc);
    for (int i = 0; i < 3; i++)
        if (s.line[i].buf) lv_draw_buf_destroy(s.line[i].buf);
    heap_caps_free(s.app);
    memset(&s, 0, sizeof(s));
}

// ---------------------------------------------------------------- open, close

bool yk_more_open(lv_obj_t *parent, const yk_more_item_t *items, int n, yk_more_cb_t cb, void *ctx) {
    if (!parent) return false;
    yk_more_close();
    const int64_t t_start = esp_timer_get_time();
    memset(&s_stats, 0, sizeof(s_stats));
    s_frame_sum_us = s_bands_sum = s_px_sum = s_draw_sum = 0;
    if (n < 0 || !items) n = 0;
    if (n > YK_MORE_MAX) n = YK_MORE_MAX;
    s.app = heap_caps_calloc(YK_MORE_MAX, sizeof(app_t), MALLOC_CAP_SPIRAM);
    s.disc = heap_caps_calloc(3 * DISC_N, sizeof(void *), MALLOC_CAP_SPIRAM);
    if (!s.app || !s.disc) {
        heap_caps_free(s.app), heap_caps_free(s.disc);
        memset(&s, 0, sizeof(s));
        return false;
    }
    s.arrow = s.disc + DISC_N;
    s.rows = (uint8_t **)(s.disc + 2 * DISC_N);
    for (int i = 0; i < n; i++) {
        app_t *a = &s.app[i];
        snprintf(a->slug, sizeof(a->slug), "%s", items[i].slug ? items[i].slug : "");
        snprintf(a->name, sizeof(a->name), "%s", items[i].name && items[i].name[0] ? items[i].name : a->slug);
        yk_first_char(items[i].glyph && items[i].glyph[0] ? items[i].glyph : a->name, a->first);
        a->where = items[i].where;
        a->current = items[i].current;
        a->preview = items[i].preview;
        a->bytes = items[i].size;
        a->eta_s = items[i].eta_s;
        a->icon = items[i].icon;
        a->icon_color = items[i].icon_color & 0xFFFFFF;
    }
    s.n = n;
    s.cb = cb;
    s.ctx = ctx;
    for (int g = 0; g < GLYPHS; g++) s.glyph_font[g] = cjk_font_get(GLYPH_PX[g]);
    s.title_font = cjk_font_get(18);
    s.name_font = cjk_font_get(22);
    s.state_font = cjk_font_get(16);
    const bool en = yk_ui_is_english();
    char title[40];
    snprintf(title, sizeof(title), en ? "My apps · %d" : "我的应用 · %d", n);
    if (s.title_font)
        yk_text_fit(title, glyph_w, (void *)s.title_font,
                    yk_more_line_room(YK_MORE_TITLE_Y, lv_font_get_line_height(s.title_font)), s.title, sizeof(s.title));
    else
        snprintf(s.title, sizeof(s.title), "%s", title);

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, YK_CX * 2, YK_CY * 2);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(YK_PAPER), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_gesture_bubble(root, false);  // a drag here is not the page's swipe
    lv_obj_set_clickable(root, true);
    lv_obj_add_event_cb(root, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(root, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, on_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, on_release, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, on_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, NULL);
    s.root = root;
    s.canvas = lv_canvas_create(root);
    lv_obj_set_hidden(s.canvas, true);
    s.focus = s.from = 0;
    s.k = 1;
    yk_more_offset_for(0, &s.ox, &s.oy);
    for (int i = 0; i < n; i++) s.app[i].boxed = app_box(i, &s.app[i].box);  // drawn with the whole page, first
    frame_at();  // (a snapshot draws it without a refresh of the display)
    s.timer = lv_timer_create(tick, FRAME_MS, NULL);
    lv_timer_pause(s.timer);
    lv_display_t *d = lv_display_get_default();
    if (d) {
        lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_READY, NULL);
    }
    text_mask(s.title, s.title_font, YK_MORE_LINE_W, &s.line[0]);
    // Every disc an app passes through as it moves (made while drawing, a big one would cost a frame ~1 ms).
    for (int d = YK_MORE_SIZE_FAR; d <= YK_MORE_SIZE_0; d += 2) disc_mask((float)d);
    // The first characters of those shown now; the rest while nothing moves.
    for (int i = 0; i < n; i++) {
        float x, y;
        yk_more_at(yk_more_place(i), s.ox, s.oy, &x, &y);
        if (yk_more_shown(x, y)) face_mask(i, yk_more_size_class(yk_more_size(yk_more_apart(i, 0))), true);
    }
    s.prep = n > 0;
    if (s.prep) lv_timer_resume(s.timer);
    set_lines();
    s.opened_us = t_start;
    s_stats.setup_ms = (int)((esp_timer_get_time() - t_start) / 1000);
    ESP_LOGI(TAG, "open: %d apps, setup %d ms", n, s_stats.setup_ms);
    return true;
}

void yk_more_close(void) {
    if (s.root) lv_obj_delete(s.root);  // on_delete clears `s`
}

bool yk_more_is_open(void) { return s.root != NULL; }

int yk_more_focus(void) { return s.root ? s.focus : -1; }

bool yk_more_tap(int index) {
    if (!s.root || index < 0 || index >= s.n) return false;
    float x, y;
    yk_more_at(yk_more_place(index), s.ox, s.oy, &x, &y);
    if (!yk_more_shown(x, y)) return false;
    tapped(index);
    return true;
}

bool yk_more_drag(int dx, int dy) {
    if (!s.root || s.pressed) return false;
    s.moving = s.shaking = false;
    s.k = 1, s.from = s.focus;
    s.press_ox = s.ox, s.press_oy = s.oy;
    s.dev_dx = dx, s.dev_dy = dy;
    s.dev_drag = s.dragging = true;
    s.t0 = lv_tick_get();
    lv_timer_resume(s.timer);
    return true;
}

bool yk_more_hold(int dx, int dy) {
    if (!s.root) return false;
    if (!s.dragging) {
        s.moving = s.shaking = s.dev_drag = false;
        s.press_ox = s.ox, s.press_oy = s.oy;
        s.dragging = true;
    }
    float ox = s.press_ox + (float)dx, oy = s.press_oy + (float)dy;
    yk_more_clamp(s.n, &ox, &oy);
    s.ox = ox, s.oy = oy;
    follow_focus();
    invalidate_band();
    return true;
}

bool yk_more_let_go(void) {
    if (!s.root || !s.dragging || s.pressed) return false;
    s.dragging = false;
    move_to(yk_more_nearest(s.n, s.ox, s.oy));
    return true;
}

void yk_more_stats(yk_more_stats_t *out) { *out = s_stats; }

void yk_more_dev_halo(int opa) {
    s_halo_opa = (uint8_t)(opa < 0 ? YK_MORE_HALO_OPA : opa > 255 ? 255 : opa);
    if (s.root) lv_obj_invalidate(s.root);
}

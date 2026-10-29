// Pure C part of 「我的应用」 (yk_more_logic.h). No LVGL, no ESP-IDF: host tested with test/run.sh.
#include "yk_more_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yk_apps_logic.h"
#include "yk_round.h"

// Ring k's corners, anticlockwise from the right (screen: y down), and the way from each corner to the next.
static const int8_t CORNER[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
static const int8_t SIDE[6][2] = {{0, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 0}, {1, -1}};

yk_hex_t yk_more_place(int i) {
    if (i <= 0) return (yk_hex_t){0, 0};
    int k = 1;
    while (i > 3 * k * (k + 1)) k++;  // rings 0..k hold 1 + 3k(k+1)
    int j = i - 1 - 3 * k * (k - 1);  // 0..6k-1 along ring k
    const int side = j / k, step = j % k;
    return (yk_hex_t){(int16_t)(CORNER[side][0] * k + SIDE[side][0] * step),
                      (int16_t)(CORNER[side][1] * k + SIDE[side][1] * step)};
}

void yk_more_at(yk_hex_t h, float ox, float oy, float *x, float *y) {
    *x = YK_MORE_CX + ox + YK_MORE_STEP * ((float)h.q + (float)h.r / 2.0f);
    *y = YK_MORE_CY + oy + YK_MORE_ROW * (float)h.r;
}

float yk_more_apart(int i, int j) {
    const yk_hex_t a = yk_more_place(i), b = yk_more_place(j);
    const float dq = (float)(a.q - b.q), dr = (float)(a.r - b.r);
    return hypotf(YK_MORE_STEP * (dq + dr / 2.0f), YK_MORE_ROW * dr);
}

float yk_more_size(float d) {
    static const float D[4] = {0, YK_MORE_STEP, YK_MORE_STEP * 1.7320508f, YK_MORE_STEP * 2};
    static const float S[4] = {YK_MORE_SIZE_0, YK_MORE_SIZE_1, YK_MORE_SIZE_2, YK_MORE_SIZE_FAR};
    if (d <= 0) return S[0];
    for (int i = 1; i < 4; i++)
        if (d < D[i]) return S[i - 1] + (S[i] - S[i - 1]) * (d - D[i - 1]) / (D[i] - D[i - 1]);
    return YK_MORE_SIZE_FAR;
}

uint8_t yk_more_opa(float d) {
    if (d <= YK_MORE_DIM_FROM) return 255;
    if (d >= YK_MORE_DIM_TO) return YK_MORE_DIM_OPA;
    const float k = (d - YK_MORE_DIM_FROM) / (YK_MORE_DIM_TO - YK_MORE_DIM_FROM);
    return (uint8_t)lroundf(255 - (255 - YK_MORE_DIM_OPA) * k);
}

float yk_more_from_middle(float x, float y) {
    const float dx = x - YK_MORE_CX, dy = y - YK_MORE_CY;
    return sqrtf(dx * dx + dy * dy);
}

float yk_more_lit(float d) {
    if (d >= YK_MORE_LIT_R) return 0;
    const float t = 1 - (d < 0 ? 0 : d) / YK_MORE_LIT_R;
    return t * t * (3 - 2 * t);
}

bool yk_more_shown(float x, float y) {
    const float dx = x - YK_CX, dy = y - YK_CY;
    return y >= YK_MORE_TOP && y <= YK_MORE_BOTTOM && dx * dx + dy * dy <= (float)YK_MORE_SHOW_R * YK_MORE_SHOW_R;
}

void yk_more_offset_for(int i, float *ox, float *oy) {
    float x, y;
    yk_more_at(yk_more_place(i), 0, 0, &x, &y);
    *ox = YK_MORE_CX - x;
    *oy = YK_MORE_CY - y;
}

int yk_more_nearest(int n, float ox, float oy) {
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < n; i++) {
        float x, y;
        yk_more_at(yk_more_place(i), ox, oy, &x, &y);
        const float d = (x - YK_MORE_CX) * (x - YK_MORE_CX) + (y - YK_MORE_CY) * (y - YK_MORE_CY);
        if (d < best_d) best_d = d, best = i;
    }
    return best;
}

void yk_more_clamp(int n, float *ox, float *oy) {
    float far = 0;
    for (int i = 0; i < n; i++) {
        float x, y;
        yk_more_at(yk_more_place(i), 0, 0, &x, &y);
        const float d = hypotf(x - YK_MORE_CX, y - YK_MORE_CY);
        if (d > far) far = d;
    }
    const float reach = far + YK_MORE_STEP / 2, d = hypotf(*ox, *oy);
    if (d > reach && d > 0) *ox *= reach / d, *oy *= reach / d;
}

int yk_more_hit(int n, int focus, float ox, float oy, int x, int y) {
    int best = -1;
    float best_d = 1e30f;
    for (int i = 0; i < n; i++) {
        float cx, cy;
        yk_more_at(yk_more_place(i), ox, oy, &cx, &cy);
        if (!yk_more_shown(cx, cy)) continue;
        float r = yk_more_size(yk_more_apart(i, focus)) / 2;
        if (r < YK_MORE_HIT_MIN) r = YK_MORE_HIT_MIN;
        const float d = hypotf((float)x - cx, (float)y - cy);
        if (d <= r && d < best_d) best_d = d, best = i;
    }
    return best;
}

float yk_more_ease(int t_ms) {
    if (t_ms <= 0) return 0;
    if (t_ms >= YK_MORE_MOVE_MS) return 1;
    // In and out along a sine (the prototype's CSS `ease` is close): at its fastest a frame (~30 ms) goes 18 % of the
    // way. Easing out at once went a third of it in the first frame, CSS's ease a quarter: the frame with the most to
    // redraw, over 33 ms.
    return (1 - cosf(3.14159265f * (float)t_ms / YK_MORE_MOVE_MS)) / 2;
}

bool yk_more_move(float ax, float ay, float bx, float by, int t_ms, float *x, float *y) {
    const float k = yk_more_ease(t_ms);
    *x = ax + (bx - ax) * k;
    *y = ay + (by - ay) * k;
    return t_ms >= YK_MORE_MOVE_MS;
}

int yk_more_line_room(int y, int line_h) {
    const int top = y + 4 - YK_CY, bottom = y + line_h - 1 - YK_CY;
    const int far = abs(top) > abs(bottom) ? abs(top) : abs(bottom);
    if (far >= YK_R_CONTENT) return 0;
    const int w = 2 * (int)floorf(sqrtf((float)(YK_R_CONTENT * YK_R_CONTENT - far * far)));
    return w < YK_MORE_LINE_W ? w : YK_MORE_LINE_W;
}

static const float CLASS_DISC[YK_MORE_GLYPHS] = {100, 86, 72, 63, 54, 49, 44, 40};

int yk_more_size_class(float size) {
    int best = 0;
    for (int c = 1; c < YK_MORE_GLYPHS; c++)
        if (fabsf(size - CLASS_DISC[c]) < fabsf(size - CLASS_DISC[best])) best = c;
    return best;
}

bool yk_more_class_at_rest(int c) { return c == 0 || c == 2 || c == 4 || c == YK_MORE_GLYPHS - 1; }

int yk_more_glyph_px(float size) {
    static const int PX[YK_MORE_GLYPHS] = {36, 32, 28, 24, 22, 20, 18, 16};
    return PX[yk_more_size_class(size)];
}

bool yk_more_opens_at_once(yk_more_where_t where) { return where == YK_MORE_SLOT || where == YK_MORE_CACHE; }

/** Whether `slug` can be added: not Yay, 「更多」, empty, there already, nor out full. */
static bool pick_room(const yk_more_item_t *out, int count, int max, const char *slug) {
    if (count >= max || !slug || !slug[0] || !strcmp(slug, YK_APPS_HOME) || !strcmp(slug, YK_APPS_MORE)) return false;
    for (int k = 0; k < count; k++)
        if (!strcmp(out[k].slug, slug)) return false;
    return true;
}

static void pick_add(yk_more_item_t *out, int *count, int max, const char *slug, yk_more_where_t where,
                     const yk_catalog_t *cat) {
    if (!pick_room(out, *count, max, slug)) return;
    const yk_catalog_entry_t *e = cat ? yk_catalog_find(cat, slug) : NULL;
    out[(*count)++] = (yk_more_item_t){.slug = slug, .name = e && e->name[0] ? e->name : NULL, .where = where,
                                       .preview = e && e->channel == YK_CHANNEL_PREVIEW};
}

int yk_more_pick(const yk_apps_slot_t *installed, int n, const char *const cached[], int nc, const yk_catalog_t *cat,
                 const yk_more_item_t *extra, int ne, yk_more_item_t *out, int max) {
    if (max > YK_MORE_MAX) max = YK_MORE_MAX;
    int count = 0;
    yk_apps_item_t slot[YK_APPS_MAX];
    const int ns = yk_apps_pick(installed, n, NULL, slot, YK_APPS_MAX);  // the most recently used first
    for (int i = 0; i < ns; i++) pick_add(out, &count, max, slot[i].slug, YK_MORE_SLOT, cat);
    for (int i = 0; cached && i < nc; i++) pick_add(out, &count, max, cached[i], YK_MORE_CACHE, cat);
    for (int i = 0; cat && i < cat->count && i < YK_CATALOG_MAX; i++)
        pick_add(out, &count, max, cat->app[i].slug, YK_MORE_CLOUD, cat);
    for (int i = 0; extra && i < ne; i++)  // the planned ones the store does not list yet, as given
        if (pick_room(out, count, max, extra[i].slug)) out[count++] = extra[i];
    return count;
}

const char *yk_more_state_text(yk_more_where_t where, bool current, bool preview, bool english) {
    if (current) return english ? "In use" : "正在用";
    if (where == YK_MORE_SOON) return english ? "Coming soon" : "即将推出";
    if (preview) {
        switch (where) {
        case YK_MORE_SLOT: return english ? "Preview · tap to open" : "预览版 · 点一下打开";
        case YK_MORE_CACHE: return english ? "Preview · downloaded" : "预览版 · 下载过 · 几秒就好";
        default: return english ? "Preview · ~20 s download" : "预览版 · 要下载 · 约二十秒";
        }
    }
    switch (where) {
    case YK_MORE_SLOT: return english ? "On Yay · tap to open" : "在圆屏上 · 点一下打开";
    case YK_MORE_CACHE: return english ? "Downloaded · seconds" : "下载过 · 几秒就好";
    default: return english ? "To download · ~20 s" : "要下载 · 大约二十秒";
    }
}

const char *yk_more_eta_text(int eta_s, bool english, char *out, size_t size) {
    if (!size) return out;
    if (eta_s <= 10) {
        snprintf(out, size, english ? "~10 s" : "约 10 秒");
    } else if (eta_s <= 55) {
        const int tens = (eta_s + 9) / 10 * 10;  // up to the next ten; 51–55 still 50 (from 56 a minute)
        snprintf(out, size, english ? "~%d s" : "约 %d 秒", tens > 50 ? 50 : tens);
    } else if (eta_s < 90) {
        snprintf(out, size, english ? "~1 min" : "约 1 分钟");
    } else if (eta_s < 570) {
        snprintf(out, size, english ? "~%d min" : "约 %d 分钟", (eta_s + 30) / 60);  // the nearest minute
    } else {
        snprintf(out, size, english ? "10+ min" : "10 分钟以上");
    }
    return out;
}

const char *yk_more_size_text(uint32_t bytes, char *out, size_t size) {
    if (!size) return out;
    const uint32_t kb = (bytes + 500) / 1000;
    if (kb < 1000) {
        snprintf(out, size, "%u KB", (unsigned)(kb ? kb : 1));
    } else {
        const uint32_t tenths = (bytes + 50000) / 100000;
        snprintf(out, size, "%u.%u MB", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
    }
    return out;
}

const char *yk_more_state_line(yk_more_where_t where, bool current, bool preview, uint32_t bytes, int eta_s,
                               bool english, char *out, size_t size) {
    if (!size) return out;
    if (!current && where == YK_MORE_CACHE) {  // only the device's own copying (Yay 0.1.5's delivery 3): no network
        char what[24];
        // Its size not known; or a preview's 10 分钟以上, 228 px (copying never takes that long: 137 MB)
        if (eta_s <= 0 || (preview && eta_s >= 570))
            snprintf(out, size, "%s", preview ? (english ? "Preview · downloaded" : "预览版 · 下载过")
                                              : (english ? "Downloaded" : "下载过"));
        else if (preview)  // Preview · downloaded · ~20 s is 239 px of the 222
            snprintf(out, size, english ? "Preview · %s" : "预览版 · 下载过 · %s", yk_more_eta_text(eta_s, english, what, sizeof(what)));
        else
            snprintf(out, size, english ? "Downloaded · %s" : "下载过 · %s", yk_more_eta_text(eta_s, english, what, sizeof(what)));
        return out;
    }
    if (current || where != YK_MORE_CLOUD) {
        snprintf(out, size, "%s", yk_more_state_text(where, current, preview, english));
        return out;
    }
    if (eta_s <= 0 && !bytes) {  // no list and no catalog: not even its size
        snprintf(out, size, "%s", preview ? (english ? "Preview · to download" : "预览版 · 要下载")
                                          : (english ? "To download" : "要下载"));
        return out;
    }
    char what[24];
    if (eta_s > 0)
        yk_more_eta_text(eta_s, english, what, sizeof(what));
    else
        yk_more_size_text(bytes, what, sizeof(what));
    if (preview)
        snprintf(out, size, english ? "Preview · %s download" : "预览版 · %s", what);
    else
        snprintf(out, size, english ? "To download · %s" : "要下载 · %s", what);
    return out;
}

float yk_more_shake(int t_ms) {
    if (t_ms <= 0 || t_ms >= YK_MORE_SHAKE_MS) return 0;
    const float k = (float)t_ms / YK_MORE_SHAKE_MS;
    return YK_MORE_SHAKE_PX * sinf(2 * 3.14159265f * 2 * k) * (1 - k);  // twice each way, dying down
}

int yk_more_icon_px(int size_class) {
    static const int PX[YK_MORE_GLYPHS] = {84, 72, 61, 53, 45, 41, 37, 34};
    return PX[size_class < 0 ? 0 : size_class >= YK_MORE_GLYPHS ? YK_MORE_GLYPHS - 1 : size_class];
}

/** One line of `sn` values (every `ss`-th of src) as `dn` (every `ds`-th of dst): the area each covers, or bilinear. */
static void resample(const float *src, int sn, int ss, float *dst, int dn, int ds) {
    const float k = (float)sn / (float)dn;
    for (int i = 0; i < dn; i++) {
        float v = 0;
        if (dn <= sn) {  // each one the mean of [i k, (i + 1) k)
            const float a = i * k, b = a + k;
            for (int j = (int)a; j < sn && j < b; j++) {
                const float lo = j > a ? (float)j : a, hi = j + 1 < b ? (float)(j + 1) : b;
                v += src[j * ss] * (hi - lo);
            }
            v /= k;
        } else {  // between the two nearest, by their centres
            float p = (i + 0.5f) * k - 0.5f;
            if (p < 0) p = 0;
            if (p > sn - 1) p = (float)(sn - 1);
            const int j = (int)p;
            const float f = p - j;
            v = src[j * ss] * (1 - f) + (j + 1 < sn ? src[(j + 1) * ss] : src[j * ss]) * f;
        }
        dst[i * ds] = v;
    }
}

void yk_more_icon_scale(const uint8_t *src, int n, uint8_t *dst, float *tmp) {
    if (n < 1 || n > 128) return;
    float *line = tmp + 64 * 128, *col = line, *out = line + 128;  // a row of 64, then a column of 64 and its n
    for (int y = 0; y < 64; y++) {  // across: 64 rows of n
        for (int x = 0; x < 64; x++) line[x] = src[y * 64 + x];
        resample(line, 64, 1, tmp + y * n, n, 1);
    }
    for (int x = 0; x < n; x++) {  // down: n columns of n
        for (int y = 0; y < 64; y++) col[y] = tmp[y * n + x];
        resample(col, 64, 1, out, n, 1);
        for (int y = 0; y < n; y++) {
            const float v = out[y] + 0.5f;
            dst[y * n + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
}

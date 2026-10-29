#include "yk_ui_logic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "yk_round.h"

// ---------------------------------------------------------------- the rim

uint32_t yk_rim_mix(uint32_t a, uint32_t b, int opa) {
    if (opa >= 255) return a;
    if (opa <= 0) return b;
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (uint32_t)((ca * opa + cb * (255 - opa) + 127) / 255) << sh;
    }
    return out;
}

int yk_rim_listen_step(int level) { return level >= 80 ? 4 : level < 0 ? 0 : level / 20; }

static void push(yk_rim_pic_t *p, float a0, float a1, uint32_t color, int width, bool round) {
    if (p->n >= YK_RIM_MAX_ARCS || a1 <= a0) return;
    p->arc[p->n++] = (yk_rim_arc_t){.a0 = a0, .a1 = a1, .color = color, .width = (uint8_t)width, .round = round};
}

/** An arc from a0 for len degrees, wrapped into 0..360 (split at the top when it crosses it). */
static void add_arc(yk_rim_pic_t *p, float a0, float len, uint32_t color, int width, bool round) {
    if (len <= 0) return;
    if (len >= 360) {
        push(p, 0, 360, color, width, false);
        return;
    }
    a0 = fmodf(a0, 360);
    if (a0 < 0) a0 += 360;
    const float a1 = a0 + len;
    if (a1 <= 360) {
        push(p, a0, a1, color, width, round);
    } else {
        push(p, a0, 360, color, width, round);
        push(p, 0, a1 - 360, color, width, round);
    }
}

static uint32_t mode_color(yk_rim_mode_t m) {
    switch (m) {
    case YK_RIM_CONNECTING: return YK_INK3;
    case YK_RIM_COUNTDOWN:
    case YK_RIM_HOLD: return YK_CORAL;
    case YK_RIM_ALERT: return YK_AMBER;
    default: return YK_SAGE;
    }
}

/** 0..1..0 over a period. */
static float triangle(int64_t t, int period) {
    const float ph = (float)(t % period) / (float)period * 2;
    return ph < 1 ? ph : 2 - ph;
}

bool yk_rim_finished(const yk_rim_in_t *in) {
    if (in->mode == YK_RIM_DONE) return in->t_ms >= YK_RIM_DONE_CLOSE_MS + YK_RIM_DONE_FADE_MS;
    if (in->mode == YK_RIM_ALERT) return in->t_ms >= YK_RIM_ALERT_MS;
    return false;
}

void yk_rim_picture(const yk_rim_in_t *in, yk_rim_pic_t *out) {
    out->n = 0;
    const yk_rim_opts_t *o = &in->opts;
    const int w = o->width > 0 ? o->width : YK_RIM_W;
    const float from = o->from_deg, span = o->span_deg > 0 && o->span_deg < 360 ? o->span_deg : 360;
    const bool whole = span >= 360;
    const uint32_t bg = in->bg;
    uint32_t color = o->color ? o->color : mode_color(in->mode);
    if (o->opa > 0 && o->opa < 255) color = yk_rim_mix(color, bg, o->opa);
    const int value = in->value < 0 ? 0 : in->value;
    switch (in->mode) {
    case YK_RIM_OFF:
    case YK_RIM_MODE_COUNT:
        return;
    case YK_RIM_LISTEN: {
        const int step = yk_rim_listen_step(in->level);
        const int opa = 110 + step * 36;
        add_arc(out, from, span, yk_rim_mix(color, bg, opa), w - 2 + step, !whole);
        return;
    }
    case YK_RIM_CONNECTING: {
        float start;
        if (whole) start = from + (float)(in->t_ms % YK_RIM_CONNECTING_PERIOD_MS) * 360.0f / YK_RIM_CONNECTING_PERIOD_MS;
        else start = from + triangle(in->t_ms, 2 * YK_RIM_CONNECTING_PERIOD_MS) * (span - YK_RIM_SHORT_ARC);
        add_arc(out, start, YK_RIM_SHORT_ARC, color, w, true);
        return;
    }
    case YK_RIM_THINKING: {
        // back and forth around the top of the part in use, 60 degrees each way
        const float centre = whole ? from : from + span / 2;
        const float reach = whole ? 60 : (span - YK_RIM_SHORT_ARC) / 2 < 60 ? (span - YK_RIM_SHORT_ARC) / 2 : 60;
        const float pos = centre + (triangle(in->t_ms, YK_RIM_THINKING_PERIOD_MS) * 2 - 1) * reach;
        add_arc(out, pos - YK_RIM_SHORT_ARC / 2.0f, YK_RIM_SHORT_ARC, color, w, true);
        return;
    }
    case YK_RIM_COUNTDOWN:
    case YK_RIM_PROGRESS:
    case YK_RIM_HOLD: {
        const float len = span * (float)(value > 1000 ? 1000 : value) / 1000.0f;
        add_arc(out, from, len, color, w, !(whole && value >= 1000));
        return;
    }
    case YK_RIM_DIAL: {
        int n = o->ticks > 0 ? o->ticks : 10;
        if (n > YK_RIM_MAX_ARCS / 2) n = YK_RIM_MAX_ARCS / 2;
        const float step = span / (float)(whole ? n : n), half = step * 0.3f < 1.5f ? step * 0.3f : 1.5f;
        for (int i = 0; i < n; i++) {
            const float c = from + step * ((float)i + 0.5f);
            add_arc(out, c - half, 2 * half, i < value ? color : YK_PANEL, w, false);
        }
        return;
    }
    case YK_RIM_DONE: {
        if (in->t_ms < YK_RIM_DONE_CLOSE_MS) {
            // from half a ring, or from the progress that was there (value: permille)
            const float s = value > 0 && value < 1000 ? (float)value / 1000.0f : 0.5f;
            const float k = s + (1 - s) * (float)in->t_ms / YK_RIM_DONE_CLOSE_MS;
            add_arc(out, from, span * k, color, w, true);
            return;
        }
        const int64_t f = in->t_ms - YK_RIM_DONE_CLOSE_MS;
        if (f >= YK_RIM_DONE_FADE_MS) return;
        const int opa = 255 - (int)(f * 255 / YK_RIM_DONE_FADE_MS);
        // quantised to 16 steps: each is one repaint of the ring, not one a frame
        add_arc(out, from, span, yk_rim_mix(color, bg, opa & ~15), w, !whole);
        return;
    }
    case YK_RIM_ALERT: {
        if (in->t_ms >= YK_RIM_ALERT_MS) return;
        const int opa = (int)(triangle(in->t_ms, YK_RIM_ALERT_MS) * 255);
        add_arc(out, from, span, yk_rim_mix(color, bg, opa & ~31), w, !whole);
        return;
    }
    }
}

static uint32_t fnv(uint32_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void yk_rim_signatures(const yk_rim_pic_t *pic, uint32_t sig[YK_RIM_SEGS]) {
    for (int k = 0; k < YK_RIM_SEGS; k++) {
        // A little past each edge: a round end or the antialiasing near the edge shows in the next box too.
        const float s0 = 45.0f * (float)k - 3, s1 = 45.0f * (float)(k + 1) + 3;
        uint32_t h = 2166136261u;
        for (int i = 0; i < pic->n; i++) {
            const yk_rim_arc_t *a = &pic->arc[i];
            // the segment may wrap below 0 or past 360: test the arc shifted by a turn as well
            for (int turn = -1; turn <= 1; turn++) {
                const float a0 = a->a0 + 360.0f * (float)turn, a1 = a->a1 + 360.0f * (float)turn;
                const float c0 = a0 > s0 ? a0 : s0, c1 = a1 < s1 ? a1 : s1;
                if (c1 <= c0) continue;
                const int32_t q[5] = {(int32_t)lroundf(c0 * 4), (int32_t)lroundf(c1 * 4), (int32_t)a->color, a->width,
                                      a->round};
                h = fnv(h, q, sizeof(q));
            }
        }
        sig[k] = h;
    }
}

void yk_rim_seg_box(int seg, int r_out, int width, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2) {
    const int r_in = r_out - width - 2, r_o = r_out + 2;
    int32_t bx1 = INT32_MAX, by1 = INT32_MAX, bx2 = INT32_MIN, by2 = INT32_MIN;
    // The boxes meet at multiples of 45 degrees, so each 0/90/180/270 bulge is an end of one: the ends are enough.
    for (int e = 0; e < 2; e++) {
        const float rad = 45.0f * (float)(seg + e) * 3.14159265f / 180.0f, sn = sinf(rad), cs = cosf(rad);
        for (int r = r_in; r <= r_o; r += r_o - r_in) {
            const int32_t x = YK_CX + (int32_t)lroundf((float)r * sn), y = YK_CY - (int32_t)lroundf((float)r * cs);
            bx1 = x < bx1 ? x : bx1, by1 = y < by1 ? y : by1, bx2 = x > bx2 ? x : bx2, by2 = y > by2 ? y : by2;
        }
    }
    *x1 = bx1, *y1 = by1, *x2 = bx2, *y2 = by2;
}

// ---------------------------------------------------------------- subtitles

uint32_t yk_utf8_next(const char *p, int *len) {
    const uint8_t c = (uint8_t)p[0];
    if (c < 0x80) return *len = 1, c;
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) return *len = 2, ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
        return *len = 3, ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
        return *len = 4, ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) |
                             (p[3] & 0x3F);
    return *len = 1, 0xFFFD;
}

int yk_circle_width(int dy_top, int dy_bottom, int radius) {
    int d = abs(dy_top) > abs(dy_bottom) ? abs(dy_top) : abs(dy_bottom);
    if (dy_top < 0 && dy_bottom > 0) d = abs(dy_top) > dy_bottom ? abs(dy_top) : dy_bottom;
    if (d >= radius) return 0;
    return (int)(2 * sqrt((double)radius * radius - (double)d * d));
}

/** Never at the start of a line. */
static bool no_start(uint32_t cp, bool ja) {
    static const uint32_t closing[] = {0xFF0C, 0x3002, 0x3001, 0xFF1B, 0xFF1A, 0xFF1F, 0xFF01, 0xFF09, 0x300D, 0x300F,
                                       0x3011, 0x3009, 0x300B, 0x201D, 0x2019, 0x2026, 0x2025, ',', '.', ';', ':', '?',
                                       '!', ')', ']', '}', 0xFF0E, 0x30FB, 0x301C, 0};
    for (int i = 0; closing[i]; i++)
        if (cp == closing[i]) return true;
    if (ja) {
        static const uint32_t small[] = {0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308E,
                                         0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE,
                                         0x30F5, 0x30F6, 0x30FC, 0};
        for (int i = 0; small[i]; i++)
            if (cp == small[i]) return true;
    }
    return false;
}

/** Never at the end of a line (opening brackets and quotes). */
static bool no_end(uint32_t cp) {
    return cp == 0xFF08 || cp == 0x300C || cp == 0x300E || cp == 0x3010 || cp == 0x3008 || cp == 0x300A ||
           cp == 0x201C || cp == 0x2018 || cp == '(' || cp == '[' || cp == 0xBF || cp == 0xA1;  // ¿ ¡ open Spanish
}

/** Letters and digits that make a word (Latin, Latin-1, Latin Extended): kept together. */
static bool wordish(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '\'' ||
           cp == '-' || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7);
}

typedef struct {
    int start, len, width;
    bool space;  // a run of spaces: a break, dropped at the ends of lines
} unit_t;

#define MAX_UNITS 256

/** The text cut into units that are never split across lines (unless one alone is too wide). */
static int units_of(const char *text, yk_glyph_width_fn width, void *ctx, bool ja, unit_t *u) {
    int n = 0, i = 0;
    while (text[i] && n < MAX_UNITS) {
        int len;
        uint32_t cp = yk_utf8_next(text + i, &len);
        unit_t cur = {.start = i, .len = len, .width = width(cp, ctx), .space = cp == ' '};
        i += len;
        if (cur.space) {
            while (text[i] == ' ') cur.width += width(' ', ctx), cur.len++, i++;
        } else if (wordish(cp)) {
            for (;;) {
                const uint32_t nx = yk_utf8_next(text + i, &len);
                if (!text[i] || !wordish(nx)) break;
                cur.width += width(nx, ctx), cur.len += len, i += len;
            }
        }
        // what may not start a line joins the unit before; an opening mark joins the unit after
        bool joined = false;
        if (n && !cur.space && !u[n - 1].space && no_start(cp, ja)) {
            u[n - 1].len += cur.len, u[n - 1].width += cur.width;
            joined = true;
        }
        if (!joined) u[n++] = cur;
        while (n && text[i] && no_end(cp) && !u[n - 1].space) {
            const uint32_t nx = yk_utf8_next(text + i, &len);
            if (nx == ' ') break;
            u[n - 1].len += len, u[n - 1].width += width(nx, ctx);
            i += len;
            cp = nx;
            if (wordish(nx)) {
                for (;;) {
                    const uint32_t nn = yk_utf8_next(text + i, &len);
                    if (!text[i] || !wordish(nn)) break;
                    u[n - 1].len += len, u[n - 1].width += width(nn, ctx), i += len;
                }
            }
        }
    }
    return n;
}

/** Fills n_lines lines with the units; false when they do not all fit. */
static bool fill(const char *text, const unit_t *u, int nu, yk_glyph_width_fn width, void *ctx, const int *avail,
                 int n_lines, yk_caption_line_t *lines) {
    int line = 0, k = 0;
    while (k < nu && u[k].space) k++;
    if (k == nu) return false;
    lines[0] = (yk_caption_line_t){.start = u[k].start, .avail = avail[0]};
    while (k < nu) {
        const unit_t *x = &u[k];
        yk_caption_line_t *L = &lines[line];
        if (x->space) {
            // a space counts only once something follows it on the same line
            int j = k + 1;
            if (j < nu && L->width + x->width + u[j].width <= L->avail) {
                L->width += x->width;
                L->len = u[j].start - L->start;
                k++;
                continue;
            }
            k++;
            if (j >= nu) break;
            if (++line >= n_lines) return false;
            lines[line] = (yk_caption_line_t){.start = u[j].start, .avail = avail[line]};
            continue;
        }
        if (L->width + x->width <= L->avail) {
            L->width += x->width;
            L->len = x->start + x->len - L->start;
            k++;
            continue;
        }
        if (L->len) {
            // on the next line (which may be wider: the middle of the circle)
            if (++line >= n_lines) return false;
            lines[line] = (yk_caption_line_t){.start = x->start, .avail = avail[line]};
            continue;
        }
        // too wide even alone on a line: break it by characters (a long word or a run of joined marks)
        for (int i = x->start, end = x->start + x->len; i < end;) {
            int len;
            const uint32_t cp = yk_utf8_next(text + i, &len);
            const int cw = width(cp, ctx);
            if (L->width + cw > L->avail && L->len) {
                if (++line >= n_lines) return false;
                lines[line] = (yk_caption_line_t){.start = i, .avail = avail[line]};
                L = &lines[line];
            }
            L->width += cw;
            L->len = i + len - L->start;
            i += len;
        }
        k++;
    }
    for (int i = line + 1; i < n_lines; i++) lines[i] = (yk_caption_line_t){0};
    return true;
}

int yk_caption_layout(const char *text, yk_glyph_width_fn width, void *ctx, int line_h, int center_dy, int radius,
                      int max_lines, const char *lang, yk_caption_line_t *lines) {
    if (!text || !*text || max_lines <= 0) return 0;
    const bool ja = lang && !strncmp(lang, "ja", 2);
    static unit_t u[MAX_UNITS];
    const int nu = units_of(text, width, ctx, ja, u);
    if (!nu) return 0;
    int avail[8];
    if (max_lines > 8) max_lines = 8;
    for (int n = 1; n <= max_lines; n++) {
        const int top = center_dy - n * line_h / 2;
        for (int i = 0; i < n; i++) avail[i] = yk_circle_width(top + i * line_h, top + (i + 1) * line_h, radius);
        if (fill(text, u, nu, width, ctx, avail, n, lines)) {
            int used = 0;
            for (int i = 0; i < n; i++) {
                if (!lines[i].len && !lines[i].width) break;
                lines[i].y = top + i * line_h - center_dy;
                used++;
            }
            return used;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- notices

static const yk_notice_spec_t NOTICES[YK_NOTICE_COUNT] = {
    [YK_NOTICE_NONE] = {YK_YAY_READY, YK_RIM_OFF, NULL, NULL, false},
    [YK_NOTICE_CONNECTING] = {YK_YAY_READY, YK_RIM_CONNECTING, NULL, NULL, false},
    [YK_NOTICE_OFFLINE] = {YK_YAY_READY, YK_RIM_OFF, "回 Yay 设置网络", "Set up Wi-Fi in Yay", false},
    [YK_NOTICE_NOT_ADDED] = {YK_YAY_READY, YK_RIM_OFF, "先在 Yay 里添加", "Add me in Yay first", false},
    [YK_NOTICE_NO_CREDITS] = {YK_YAY_READY, YK_RIM_OFF, "今天的点数用完了", "Out of points today", false},
    [YK_NOTICE_NOT_HEARD] = {YK_YAY_HELP, YK_RIM_OFF, "再说一次？", "Say it again?", false},
    [YK_NOTICE_BUSY] = {YK_YAY_HELP, YK_RIM_OFF, "等一下再试", "Try again soon", true},
    [YK_NOTICE_BLOCKED] = {YK_YAY_HELP, YK_RIM_OFF, "换个话题吧", "Let's try something else", false},
    [YK_NOTICE_BATTERY_LOW] = {YK_YAY_READY, YK_RIM_ALERT, NULL, NULL, false},
};

const yk_notice_spec_t *yk_notice_spec(yk_notice_t notice) {
    return &NOTICES[(unsigned)notice < YK_NOTICE_COUNT ? notice : YK_NOTICE_NONE];
}

yk_notice_t yk_notice_net(yk_notice_net_t *net, bool connected, int64_t now_ms) {
    if (connected) {
        net->trying_since_ms = 0;
        return YK_NOTICE_NONE;
    }
    if (!net->trying_since_ms) net->trying_since_ms = now_ms ? now_ms : 1;
    return now_ms - net->trying_since_ms >= YK_NOTICE_OFFLINE_AFTER_MS ? YK_NOTICE_OFFLINE : YK_NOTICE_CONNECTING;
}

// ---------------------------------------------------------------- Yay

static const char *const POSES[YK_YAY_POSE_COUNT] = {
    "ready", "ack", "listening", "thinking", "working", "approval", "speaking", "done", "reminder", "help", "resting",
    "greet", "working-laptop", "thinking-glass",
};

const char *yk_yay_pose_name(yk_yay_pose_t pose) { return (unsigned)pose < YK_YAY_POSE_COUNT ? POSES[pose] : NULL; }

yk_yay_pose_t yk_yay_pose_from_name(const char *name) {
    for (int i = 0; name && i < YK_YAY_POSE_COUNT; i++)
        if (!strcmp(POSES[i], name)) return (yk_yay_pose_t)i;
    return YK_YAY_POSE_COUNT;
}

bool yk_yay_pick(yk_yay_pose_t pose, int size, bool (*has)(yk_yay_pose_t, int), yk_yay_pose_t *use_pose,
                 int *use_size) {
    static const int SIZES[] = {YK_YAY_CORNER, YK_YAY_COMPANION, YK_YAY_HERO};
    if ((unsigned)pose >= YK_YAY_POSE_COUNT) return false;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (has(pose, size)) {
            *use_pose = pose, *use_size = size;
            return true;
        }
        for (int i = 0; i < 3; i++)  // the next larger
            if (SIZES[i] > size && has(pose, SIZES[i])) {
                *use_pose = pose, *use_size = SIZES[i];
                return true;
            }
        for (int i = 2; i >= 0; i--)  // the largest smaller
            if (SIZES[i] < size && has(pose, SIZES[i])) {
                *use_pose = pose, *use_size = SIZES[i];
                return true;
            }
        if (pose != YK_YAY_THINKING_GLASS) break;
        pose = YK_YAY_THINKING;
    }
    return false;
}

void yk_yay_palette(const uint8_t pal[1024], uint32_t paper, uint16_t rgb[256], uint8_t alpha[256]) {
    const bool clear = paper == YK_YAY_TRANSPARENT;
    const unsigned pr = (paper >> 16) & 0xFF, pg = (paper >> 8) & 0xFF, pb = paper & 0xFF;
    for (int i = 0; i < 256; i++) {
        unsigned b = pal[4 * i], g = pal[4 * i + 1], r = pal[4 * i + 2];
        const unsigned a = pal[4 * i + 3];
        if (!clear) {
            r = (r * a + pr * (255 - a) + 127) / 255;
            g = (g * a + pg * (255 - a) + 127) / 255;
            b = (b * a + pb * (255 - a) + 127) / 255;
        }
        rgb[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        alpha[i] = clear ? (uint8_t)a : 255;
    }
}

yk_yay_mouth_t yk_yay_mouth_step(yk_yay_mouth_t now, uint8_t level) {
    if (level >= 45) return YK_YAY_MOUTH_OPEN;
    if (now == YK_YAY_MOUTH_OPEN && level >= 35) return YK_YAY_MOUTH_OPEN;
    if (level >= 18) return YK_YAY_MOUTH_SLIGHT;
    if (now >= YK_YAY_MOUTH_SLIGHT && level >= 10) return YK_YAY_MOUTH_SLIGHT;
    return YK_YAY_MOUTH_CLOSED;
}

int yk_yay_blink_gap_ms(uint32_t *seed) {
    uint32_t x = *seed ? *seed : 0x9E3779B9u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    *seed = x;
    return 4000 + (int)(x % 2001);
}

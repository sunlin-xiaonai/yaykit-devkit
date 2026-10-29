// Host tests for yaykit_ui v2's pure parts. Run: bash test/run.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yk_round.h"
#include "yk_ui_logic.h"

static int failures, checks;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                  \
    } while (0)

static yk_rim_pic_t P;

static float covered(const yk_rim_pic_t *p) {
    float s = 0;
    for (int i = 0; i < p->n; i++) s += p->arc[i].a1 - p->arc[i].a0;
    return s;
}

static int changed(const uint32_t a[8], const uint32_t b[8]) {
    int n = 0;
    for (int i = 0; i < 8; i++) n += a[i] != b[i];
    return n;
}

static void test_rim_states(void) {
    yk_rim_in_t in = {.bg = YK_PAPER};
    in.mode = YK_RIM_OFF;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 0);
    // Countdown: 25 % left = the arc from the top to 3 o'clock, coral, 6 px, round ends.
    in.mode = YK_RIM_COUNTDOWN;
    in.value = 250;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 1 && P.arc[0].a0 == 0 && fabsf(P.arc[0].a1 - 90) < 0.01f);
    CHECK(P.arc[0].color == YK_CORAL && P.arc[0].width == YK_RIM_W && P.arc[0].round);
    // Progress is sage; a hero dial 14 px.
    in.mode = YK_RIM_PROGRESS;
    in.opts.width = 14;
    yk_rim_picture(&in, &P);
    CHECK(P.arc[0].color == YK_SAGE && P.arc[0].width == 14);
    in.opts.width = 0;
    // Listening: the whole ring, wider and brighter with the voice (4..8 px).
    in.mode = YK_RIM_LISTEN;
    in.level = 0;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 1 && covered(&P) == 360 && P.arc[0].width == 4 && !P.arc[0].round);
    in.level = 95;
    yk_rim_picture(&in, &P);
    CHECK(P.arc[0].width == 8);
    CHECK(yk_rim_listen_step(0) == 0 && yk_rim_listen_step(39) == 1 && yk_rim_listen_step(80) == 4);
    // Split screen: only the bottom half (from 3 to 9 o'clock).
    in.opts.from_deg = 90, in.opts.span_deg = 180;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 1 && P.arc[0].a0 == 90 && P.arc[0].a1 == 270 && P.arc[0].round);
    // The top half wraps over 0 and is split there.
    in.opts.from_deg = 270;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 2 && fabsf(covered(&P) - 180) < 0.01f);
    in.opts.from_deg = 0, in.opts.span_deg = 0;
    // Connecting: one 40-degree ink3 arc, turning a full circle in 3 s.
    in.mode = YK_RIM_CONNECTING;
    in.t_ms = 0;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 1 && P.arc[0].color == YK_INK3 && fabsf(covered(&P) - 40) < 0.01f && P.arc[0].a0 == 0);
    in.t_ms = 750;
    yk_rim_picture(&in, &P);
    CHECK(fabsf(P.arc[0].a0 - 90) < 0.5f);
    in.t_ms = 2900;  // crossing the top: split, still 40 degrees
    yk_rim_picture(&in, &P);
    CHECK(P.n == 2 && fabsf(covered(&P) - 40) < 0.01f);
    // Thinking: a sage arc sweeping 60 degrees each way around the top.
    in.mode = YK_RIM_THINKING;
    float lo = 999, hi = -999;
    for (int t = 0; t < 1600; t += 20) {
        in.t_ms = t;
        yk_rim_picture(&in, &P);
        CHECK(fabsf(covered(&P) - 40) < 0.01f && P.arc[0].color == YK_SAGE);
        float mid = P.n == 1 ? (P.arc[0].a0 + P.arc[0].a1) / 2 : (P.arc[0].a0 - 360 + P.arc[1].a1) / 2;
        if (mid > 180) mid -= 360;
        lo = mid < lo ? mid : lo, hi = mid > hi ? mid : hi;
    }
    CHECK(lo < -55 && hi > 55 && lo > -61 && hi < 61);
    // Dial: 10 ticks, the first 4 lit (sage), the rest panel.
    in.mode = YK_RIM_DIAL;
    in.value = 4;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 10);
    int lit = 0;
    for (int i = 0; i < P.n; i++) lit += P.arc[i].color == YK_SAGE;
    CHECK(lit == 4);
    // Done closes to a full ring then fades to nothing and is finished.
    in.mode = YK_RIM_DONE;
    in.t_ms = 299;
    yk_rim_picture(&in, &P);
    CHECK(covered(&P) > 355);
    in.t_ms = 600;
    yk_rim_picture(&in, &P);
    CHECK(covered(&P) == 360 && P.arc[0].color != YK_SAGE && !yk_rim_finished(&in));
    in.t_ms = 800;
    yk_rim_picture(&in, &P);
    CHECK(P.n == 0 && yk_rim_finished(&in));
    // From a progress that was there (850 permille), not back to half a ring first.
    in.value = 850;
    in.t_ms = 0;
    yk_rim_picture(&in, &P);
    CHECK(covered(&P) >= 305 && covered(&P) <= 307);
    in.value = 0;
    yk_rim_picture(&in, &P);
    CHECK(covered(&P) >= 179 && covered(&P) <= 181);
    // A dimmed colour is mixed with the paper (opaque, between the two).
    CHECK(yk_rim_mix(0xFFFFFF, 0x000000, 128) == 0x808080);
    CHECK(yk_rim_mix(YK_SAGE, YK_PAPER, 255) == YK_SAGE && yk_rim_mix(YK_SAGE, YK_PAPER, 0) == YK_PAPER);
}

static void test_rim_repaints(void) {
    // The point of the 8 boxes: a countdown that moves a little repaints one box, not the ring.
    yk_rim_in_t in = {.bg = YK_PAPER, .mode = YK_RIM_COUNTDOWN, .value = 400};
    uint32_t a[8], b[8];
    yk_rim_picture(&in, &P);
    yk_rim_signatures(&P, a);
    in.value = 390;  // 144 -> 140.4 degrees: inside box 3 (135..180)
    yk_rim_picture(&in, &P);
    yk_rim_signatures(&P, b);
    CHECK(changed(a, b) == 1 && a[3] != b[3]);
    // The same state twice: nothing.
    yk_rim_signatures(&P, a);
    CHECK(changed(a, b) == 0);
    // An end right at a box's edge repaints both boxes there (its round end and antialiasing reach over).
    in.value = 500;
    yk_rim_picture(&in, &P);
    yk_rim_signatures(&P, a);
    in.value = 490;
    yk_rim_picture(&in, &P);
    yk_rim_signatures(&P, b);
    CHECK(changed(a, b) == 2 && a[3] != b[3] && a[4] != b[4]);
    // Connecting turns 6 degrees a frame (at 30 fps): the boxes around its two ends, at most 3.
    in.mode = YK_RIM_CONNECTING;
    int worst = 0;
    for (int t = 0; t < 3000; t += 33) {
        in.t_ms = t;
        yk_rim_picture(&in, &P);
        yk_rim_signatures(&P, b);
        if (t) worst = changed(a, b) > worst ? changed(a, b) : worst;
        memcpy(a, b, sizeof(a));
    }
    CHECK(worst <= 3);
    // A half ring only ever touches the 4 (or 5, at its round ends) boxes of its half.
    in.mode = YK_RIM_LISTEN;
    in.opts.from_deg = 90, in.opts.span_deg = 180;
    in.level = 0;
    yk_rim_picture(&in, &P);
    yk_rim_signatures(&P, a);
    in.level = 90;
    yk_rim_picture(&in, &P);
    yk_rim_signatures(&P, b);
    CHECK(changed(a, b) <= 6 && a[0] == b[0] && a[7] == b[7]);
    // Box geometry: box 0 runs from the top to 45 degrees; boxes stay on the screen; the band is inside them.
    int32_t x1, y1, x2, y2;
    yk_rim_seg_box(0, YK_RIM_R, 6, &x1, &y1, &x2, &y2);
    CHECK(y1 == YK_CY - YK_RIM_R - 2 && x1 == YK_CX);
    CHECK(x2 == YK_CX + (int)lroundf((YK_RIM_R + 2) * sinf(3.14159265f / 4)));
    for (int k = 0; k < 8; k++) {
        yk_rim_seg_box(k, YK_RIM_R, 16, &x1, &y1, &x2, &y2);
        CHECK(x1 >= 0 && y1 >= 0 && x2 <= 465 && y2 <= 465 && x2 > x1 && y2 > y1);
        CHECK((x2 - x1 + 1) * (y2 - y1 + 1) < 466 * 466 / 6);  // each a small part of the screen
    }
}

// Glyph widths for the tests: CJK and full-width 24 px, ASCII 12, space 6.
static int width(uint32_t cp, void *ctx) {
    (void)ctx;
    return cp == ' ' ? 6 : cp < 0x80 ? 12 : cp < 0x2000 ? 13 : 24;
}

static char line_text[8][256];
static int layout(const char *text, int center_dy, int max_lines, const char *lang, yk_caption_line_t *lines) {
    const int n = yk_caption_layout(text, width, NULL, 32, center_dy, 200, max_lines, lang, lines);
    for (int i = 0; i < n && i < 8; i++) {
        snprintf(line_text[i], sizeof(line_text[i]), "%.*s", lines[i].len, text + lines[i].start);
    }
    return n;
}

static void test_caption(void) {
    // The width the circle allows (spec 2's table: 400, 381, 346, 285, 210 at 0, 60, 100, 140, 170).
    CHECK(yk_circle_width(0, 0, 200) == 400);
    CHECK(abs(yk_circle_width(60, 60, 200) - 381) <= 1);
    CHECK(abs(yk_circle_width(-100, -100, 200) - 346) <= 1);
    CHECK(abs(yk_circle_width(140, 140, 200) - 285) <= 1);
    CHECK(abs(yk_circle_width(170, 170, 200) - 210) <= 1);
    CHECK(yk_circle_width(-16, 16, 200) == yk_circle_width(16, 16, 200));
    CHECK(yk_circle_width(200, 220, 200) == 0);
    yk_caption_line_t L[8];
    // Short: one line.
    CHECK(layout("你好", 0, 3, "zh", L) == 1 && !strcmp(line_text[0], "你好") && L[0].width == 48);
    // 20 characters (480 px) do not fit one line of 400: two lines in the middle of the circle.
    const char *zh = "今天天气很好我们一起去公园散步然后回家吃饭";
    int n = layout(zh, 0, 3, "zh", L);
    CHECK(n == 2);
    for (int i = 0; i < n; i++) CHECK(L[i].width <= L[i].avail);
    // Lower on the screen the lines are narrower, so the same text needs more of them.
    int low = layout(zh, 120, 3, "zh", L);
    CHECK(low >= n);
    for (int i = 0; i < low; i++) CHECK(L[i].width <= L[i].avail && L[i].avail < 360);
    // Closing punctuation never starts a line: it stays with the character before.
    char buf[256] = "";
    for (int i = 0; i < 16; i++) strcat(buf, "字");
    strcat(buf, "。下一句");  // 16 characters fill 384 px; the full stop would start line 2
    n = layout(buf, 0, 3, "zh", L);
    CHECK(n == 2);
    CHECK(strncmp(line_text[1], "。", 3) != 0);
    // Japanese: no small kana or long vowel mark at a line's start.
    char ja[256] = "";
    for (int i = 0; i < 16; i++) strcat(ja, "あ");
    strcat(ja, "ゃーよ");
    n = layout(ja, 0, 3, "ja", L);
    CHECK(n == 2);
    CHECK(strncmp(line_text[1], "ゃ", 3) != 0 && strncmp(line_text[1], "ー", 3) != 0);
    // English breaks at spaces, never inside a word, and no space at either end of a line.
    const char *en = "The quick brown fox jumps over the lazy dog near the river bank";
    n = layout(en, 0, 3, "en", L);
    CHECK(n == 2);
    for (int i = 0; i < n; i++) {
        CHECK(line_text[i][0] != ' ' && line_text[i][strlen(line_text[i]) - 1] != ' ');
        CHECK(L[i].width <= L[i].avail);
    }
    // Every word is whole: joining the lines with spaces gives the text back.
    char joined[256];
    snprintf(joined, sizeof(joined), "%s %s", line_text[0], line_text[1]);
    CHECK(!strcmp(joined, en));
    // Spanish with accents and ¿: "¿" stays with its word.
    n = layout("¿Dónde está la estación de tren más cercana, por favor?", 0, 3, "es", L);
    CHECK(n >= 1);
    for (int i = 0; i < n; i++) {
        const size_t len = strlen(line_text[i]);
        CHECK(!(len >= 2 && !strcmp(line_text[i] + len - 2, "¿")));
    }
    // Too long for three lines: -1 (the caption then tries 20 px, then cuts).
    char longtext[600] = "";
    for (int i = 0; i < 60; i++) strcat(longtext, "长");
    CHECK(layout(longtext, 0, 3, "zh", L) == -1);
    // A word wider than a line is broken by characters rather than lost.
    n = layout("Pneumonoultramicroscopicsilicovolcanoconiosis", 0, 3, "en", L);
    CHECK(n == 2 && L[0].width <= L[0].avail);
    // Empty.
    CHECK(layout("", 0, 3, "zh", L) == 0 && layout("   ", 0, 3, "en", L) <= 0);
    // The lines sit centred around the block's centre.
    n = layout(zh, 50, 3, "zh", L);
    CHECK(n == 2 && L[0].y == -32 && L[1].y == 0);
    // UTF-8: broken sequences are one replacement character, never read past.
    int len;
    CHECK(yk_utf8_next("\xE4\xBD", &len) == 0xFFFD && len == 1);
    CHECK(yk_utf8_next("\xC3\xB1", &len) == 0xF1 && len == 2);
}

static void test_notices(void) {
    const yk_notice_spec_t *s = yk_notice_spec(YK_NOTICE_NOT_HEARD);
    CHECK(s->pose == YK_YAY_HELP && !strcmp(s->zh, "再说一次？") && !strcmp(s->en, "Say it again?"));
    CHECK(yk_notice_spec(YK_NOTICE_CONNECTING)->zh == NULL && yk_notice_spec(YK_NOTICE_CONNECTING)->rim == YK_RIM_CONNECTING);
    CHECK(yk_notice_spec(YK_NOTICE_BUSY)->retry_once);
    CHECK(yk_notice_spec(YK_NOTICE_BATTERY_LOW)->rim == YK_RIM_ALERT && !yk_notice_spec(YK_NOTICE_BATTERY_LOW)->zh);
    CHECK(yk_notice_spec(99) == yk_notice_spec(YK_NOTICE_NONE));
    // Spec 1 and 9: a notice is at most 8 characters (Chinese).
    for (int k = 0; k < YK_NOTICE_COUNT; k++) {
        const char *zh = yk_notice_spec(k)->zh;
        if (!zh) continue;
        int chars = 0, len;
        for (const char *p = zh; *p; p += len) {
            const uint32_t cp = yk_utf8_next(p, &len);
            chars += cp != ' ';
        }
        CHECK(chars <= YK_NOTICE_MAX_CHARS);
    }
    // Network: connecting at first, offline after 45 s of trying, gone when connected.
    yk_notice_net_t net = {0};
    CHECK(yk_notice_net(&net, false, 1000) == YK_NOTICE_CONNECTING);
    CHECK(yk_notice_net(&net, false, 45999) == YK_NOTICE_CONNECTING);
    CHECK(yk_notice_net(&net, false, 46000) == YK_NOTICE_OFFLINE);
    CHECK(yk_notice_net(&net, true, 47000) == YK_NOTICE_NONE);
    CHECK(yk_notice_net(&net, false, 50000) == YK_NOTICE_CONNECTING);
}

static bool art_has(yk_yay_pose_t pose, int size) {
    // what assets/yay holds today
    if (size == 176) return pose != YK_YAY_THINKING_GLASS;
    if (size == 264) return pose == YK_YAY_READY || pose == YK_YAY_RESTING || pose == YK_YAY_GREET;
    if (size == 132) return pose == YK_YAY_GREET;
    return false;
}

static void test_yay(void) {
    yk_yay_pose_t p;
    int s;
    CHECK(yk_yay_pick(YK_YAY_GREET, 132, art_has, &p, &s) && p == YK_YAY_GREET && s == 132);
    // No 132 done yet: the next larger, scaled down.
    CHECK(yk_yay_pick(YK_YAY_DONE, 132, art_has, &p, &s) && p == YK_YAY_DONE && s == 176);
    // No 264 help: the largest smaller, scaled up.
    CHECK(yk_yay_pick(YK_YAY_HELP, 264, art_has, &p, &s) && p == YK_YAY_HELP && s == 176);
    // thinking_glass is not exported yet: thinking.
    CHECK(yk_yay_pick(YK_YAY_THINKING_GLASS, 176, art_has, &p, &s) && p == YK_YAY_THINKING && s == 176);
    CHECK(!yk_yay_pick(YK_YAY_POSE_COUNT, 176, art_has, &p, &s));
    CHECK(!strcmp(yk_yay_pose_name(YK_YAY_WORKING_LAPTOP), "working-laptop"));
    CHECK(yk_yay_pose_from_name("thinking-glass") == YK_YAY_THINKING_GLASS);
    CHECK(yk_yay_pose_from_name("nope") == YK_YAY_POSE_COUNT && yk_yay_pose_name(YK_YAY_POSE_COUNT) == NULL);
    // Mouth with hysteresis: no flutter around one threshold.
    yk_yay_mouth_t m = YK_YAY_MOUTH_CLOSED;
    m = yk_yay_mouth_step(m, 50);
    CHECK(m == YK_YAY_MOUTH_OPEN);
    m = yk_yay_mouth_step(m, 40);
    CHECK(m == YK_YAY_MOUTH_OPEN);
    m = yk_yay_mouth_step(m, 30);
    CHECK(m == YK_YAY_MOUTH_SLIGHT);
    m = yk_yay_mouth_step(m, 12);
    CHECK(m == YK_YAY_MOUTH_SLIGHT);
    m = yk_yay_mouth_step(m, 5);
    CHECK(m == YK_YAY_MOUTH_CLOSED);
    m = yk_yay_mouth_step(m, 15);
    CHECK(m == YK_YAY_MOUTH_CLOSED);
    // Blinks every 4 to 6 seconds, not always the same gap.
    uint32_t seed = 7;
    int lo = 99999, hi = 0;
    for (int i = 0; i < 200; i++) {
        const int g = yk_yay_blink_gap_ms(&seed);
        lo = g < lo ? g : lo, hi = g > hi ? g : hi;
    }
    CHECK(lo >= 4000 && hi <= 6000 && hi - lo > 1000);
}

static void test_yay_palette(void) {
    // Three entries (B, G, R, A): fully clear, opaque red, half-transparent white.
    uint8_t pal[1024] = {0};
    pal[4 * 1 + 2] = 255, pal[4 * 1 + 3] = 255;
    pal[4 * 2 + 0] = pal[4 * 2 + 1] = pal[4 * 2 + 2] = 255, pal[4 * 2 + 3] = 128;
    uint16_t rgb[256];
    uint8_t a[256];
    // Over the paper: opaque everywhere, the clear entry is the paper itself, the half white between the two.
    yk_yay_palette(pal, YK_PAPER, rgb, a);
    const uint16_t paper565 = (uint16_t)(((0x17 & 0xF8) << 8) | ((0x1C & 0xFC) << 3) | (0x19 >> 3));
    CHECK(rgb[0] == paper565 && a[0] == 255);
    CHECK(rgb[1] == 0xF800 && a[1] == 255);
    CHECK(rgb[2] != 0xFFFF && rgb[2] != paper565 && a[2] == 255);
    // Black paper (Breathe's night screens) is a different blend.
    uint16_t night[256];
    yk_yay_palette(pal, YK_NIGHT, night, a);
    CHECK(night[0] == 0 && night[2] != rgb[2]);
    // Transparent: the colours themselves, the alpha kept.
    yk_yay_palette(pal, YK_YAY_TRANSPARENT, rgb, a);
    CHECK(a[0] == 0 && a[1] == 255 && a[2] == 128);
    CHECK(rgb[1] == 0xF800 && rgb[2] == 0xFFFF);
    // The transparent marker is not a colour any paper could be (0xRRGGBB never sets the top byte).
    CHECK(YK_YAY_TRANSPARENT > 0xFFFFFF);
}

int main(void) {
    test_rim_states();
    test_rim_repaints();
    test_caption();
    test_notices();
    test_yay();
    test_yay_palette();
    printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}

#include "tuner.h"

#include <math.h>
#include <stdio.h>

#include "stations.h"
#include "yaykit_ui.h"

// Concentric, like a radio's dial face: the city along the top of the circle, a thin inner ring around the
// frequency, and the FM dial along the bottom with its baseline, ticks growing inwards from it, a dot on the
// baseline per station and a coral needle across it. All inside the radius-200 content circle. LVGL measures angles
// clockwise from 3 o'clock; the dial is its bottom quarter, 45 .. 135 degrees, low frequencies on the left.
#define CITY_R 178        // the city's baseline along the top
#define RING_R 160        // the inner ring around the frequency
#define DIAL_R 188        // the dial's baseline
#define DIAL_FROM 45
#define DIAL_SPAN 90
#define DOT_D 10          // station dots, centred on the baseline
#define NEEDLE_IN 166
#define NEEDLE_OUT 198
#define SLIDE_MS 400

#define FM_Y 102
#define FREQ_Y 118
#define NAME_Y 250

#define BARS 11
#define BAR_W 6
#define BAR_GAP 7
#define BAR_MAX 36
#define BAR_MIN 4
#define BARS_Y 302        // the bars' centre line

static lv_obj_t *s_screen, *s_city, *s_freq, *s_name, *s_needle;
static lv_obj_t *s_bars[BARS];
static lv_point_precise_t s_needle_pts[2];
static int s_needle_pm = -1;  // where the needle is, permille of the dial
static float s_bar_h[BARS];
static uint32_t s_seed = 0x2545F491;

/** The dial angle (degrees, LVGL's) of a permille, low frequencies on the left. */
static float dial_deg(int pm) { return DIAL_FROM + DIAL_SPAN * (1000 - pm) / 1000.0f; }

static void point_at(float deg, float r, lv_point_precise_t *p) {
    const float a = deg * (float)M_PI / 180.0f;
    p->x = YK_CX + r * cosf(a);
    p->y = YK_CY + r * sinf(a);
}

static void needle_to(int pm) {
    s_needle_pm = pm;
    const float deg = dial_deg(pm);
    point_at(deg, NEEDLE_IN, &s_needle_pts[0]);
    point_at(deg, NEEDLE_OUT, &s_needle_pts[1]);
    lv_line_set_points(s_needle, s_needle_pts, 2);
}

static void needle_anim(void *var, int32_t v) {
    (void)var;
    needle_to((int)v);
}

static lv_obj_t *circle(lv_obj_t *parent, int d, int cx, int cy) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_clickable(o, false);
    return o;
}

void tuner_create(lv_obj_t *screen) {
    s_screen = screen;
    lv_obj_t *ring = circle(screen, 2 * RING_R, YK_CX, YK_CY);
    lv_obj_set_style_border_color(ring, lv_color_hex(YK_PANEL), 0);
    lv_obj_set_style_border_width(ring, 2, 0);

    // The dial: the baseline arc, ticks inwards from it, a longer one every fourth.
    lv_obj_t *scale = lv_scale_create(screen);
    lv_obj_set_size(scale, 2 * DIAL_R, 2 * DIAL_R);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(scale, DIAL_FROM);
    lv_scale_set_angle_range(scale, DIAL_SPAN);
    lv_scale_set_range(scale, 0, 1000);
    lv_scale_set_total_tick_count(scale, 41);
    lv_scale_set_major_tick_every(scale, 4);
    lv_scale_set_label_show(scale, false);
    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(scale, lv_color_hex(YK_INK3), LV_PART_MAIN);
    lv_obj_set_style_arc_width(scale, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(scale, lv_color_hex(YK_INK3), LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(YK_INK2), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 12, LV_PART_INDICATOR);
    lv_obj_set_clickable(scale, false);

    // A dot per station, centred on the baseline.
    for (int i = 0; i < stations_count(); i++) {
        lv_point_precise_t p;
        point_at(dial_deg(dial_permille(station_at(i)->mhz10)), DIAL_R - 1, &p);
        lv_obj_t *dot = circle(screen, DOT_D, (int)lroundf(p.x), (int)lroundf(p.y));
        lv_obj_set_style_bg_color(dot, lv_color_hex(YK_SAGE), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    s_needle = lv_line_create(screen);
    lv_obj_set_style_line_color(s_needle, lv_color_hex(YK_CORAL), 0);
    lv_obj_set_style_line_width(s_needle, 5, 0);
    lv_obj_set_style_line_rounded(s_needle, true, 0);
    lv_obj_set_clickable(s_needle, false);

    yk_ui_label(screen, "FM", 0, FM_Y, 466, YK_SIZE_MIN, YK_INK3);
    s_freq = yk_ui_label(screen, "", 0, FREQ_Y, 466, YK_SIZE_HERO_MIN, YK_INK);
    s_name = yk_ui_label(screen, "", 0, NAME_Y, 466, YK_SIZE_HEADING, YK_INK2);

    const int x0 = YK_CX - (BARS * BAR_W + (BARS - 1) * BAR_GAP) / 2;
    for (int i = 0; i < BARS; i++) {
        lv_obj_t *bar = lv_obj_create(screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_radius(bar, BAR_W / 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(YK_INK3), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_pos(bar, x0 + i * (BAR_W + BAR_GAP), BARS_Y - BAR_MIN / 2);
        lv_obj_set_size(bar, BAR_W, BAR_MIN);
        lv_obj_set_clickable(bar, false);
        s_bars[i] = bar;
        s_bar_h[i] = BAR_MIN;
    }
}

void tuner_show_station(int index, bool slide) {
    const station_t *st = station_at(index);
    char text[16];
    station_freq_text(st, text, sizeof(text));
    lv_label_set_text(s_freq, text);
    lv_label_set_text(s_name, st->name);
    // The city along the top of the circle (one label per character: short names only).
    if (s_city) lv_obj_delete(s_city);
    s_city = yk_ui_arc_label(s_screen, st->city, YK_SIZE_STATUS, YK_SAGE_TEXT, 0, CITY_R, false);
    const int target = dial_permille(st->mhz10);
    lv_anim_delete(NULL, needle_anim);
    if (!slide || s_needle_pm < 0) {
        needle_to(target);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, needle_anim);
    lv_anim_set_values(&a, s_needle_pm, target);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void tuner_level(int level, bool live) {
    static bool was_live;
    for (int i = 0; i < BARS; i++) {
        float target = BAR_MIN;
        if (live) {
            // Taller in the middle, each bar wobbling on its own, all following the speaker's level.
            s_seed = s_seed * 1664525u + 1013904223u;
            const float shape = 1.0f - fabsf(i - (BARS - 1) / 2.0f) / BARS;
            const float wobble = 0.55f + 0.45f * (float)(s_seed >> 24) / 255.0f;
            target = BAR_MIN + (BAR_MAX - BAR_MIN) * (level / 100.0f) * shape * wobble;
        }
        // Up fast, down slowly.
        s_bar_h[i] += (target - s_bar_h[i]) * (target > s_bar_h[i] ? 0.7f : 0.25f);
        const int h = (int)(s_bar_h[i] + 0.5f);
        if (lv_obj_get_height(s_bars[i]) != h) {
            lv_obj_set_height(s_bars[i], h);
            lv_obj_set_y(s_bars[i], BARS_Y - h / 2);
        }
    }
    if (live != was_live) {
        was_live = live;
        for (int i = 0; i < BARS; i++)
            lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(live ? YK_SAGE : YK_INK3), 0);
    }
}

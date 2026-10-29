// Yay in an app (yk_yay.h). The art is yaykit/firmware/assets/yay: I8 images with a 256-colour palette (alpha not
// premultiplied) and the face's boxes. Each pose is blended once over the paper it sits on into opaque RGB565 in
// PSRAM (drawing it is then a copy; the main firmware measured ~40 ms of a 45 ms frame for the same pose with
// alpha), or for YK_YAY_TRANSPARENT kept with its alpha (RGB565A8), and kept in a small cache. Blinking, looking and the mouth are drawn over the art: the drawn eye or mouth is
// covered with the skin colour (grown by YAY_ART_COVER_PAD) and a new one drawn in YAY_ART_INK.
#include "yk_yay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "yay_art.h"
#include "yk_round.h"
#include "yk_ui_logic.h"

static const char *TAG = "yk_yay";

#define CACHE 8
#define BLINK_MS 130
#define LOOK_PX 3  // how far the eyes move to look somewhere (at 176 px)

static struct {
    const yay_art_t *art;
    uint32_t paper;
    lv_image_dsc_t dsc;
    uint32_t used;  // lv_tick of the last use (the oldest goes)
} s_cache[CACHE];

struct yk_yay {
    lv_obj_t *box;          // size x size, centred on (cx, cy)
    lv_obj_t *img[2];       // the pose and, while crossfading, the one before
    int cur;
    lv_obj_t *eye_cover[2], *eye[2], *mouth_cover, *mouth;
    yk_yay_pose_t pose;
    const yay_art_t *art;   // the art drawn (maybe another size, scaled)
    int size, cx, cy;
    uint32_t paper;
    float scale;            // size / art size
    float rot;              // degrees
    bool auto_blink, blinking, breathing;
    lv_timer_t *timer;
    uint32_t blink_seed, next_blink;
    int look_dx, look_dy;   // eye offset in art pixels; 0, 0 = ahead
    yk_yay_mouth_t mouth_set;  // fixed shape, or POSE
    yk_yay_mouth_t mouth_level;
    bool mouth_from_level;
};

static const yay_art_t *art_for(yk_yay_pose_t pose, int size) {
    const char *name = yk_yay_pose_name(pose);
    return name ? yay_art_find(name, (uint16_t)size) : NULL;
}

bool yk_yay_has(yk_yay_pose_t pose, int size) { return art_for(pose, size) != NULL; }

/** The pose blended over `paper` as RGB565, from the cache or made now. */
static const lv_image_dsc_t *decoded(const yay_art_t *art, uint32_t paper) {
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < CACHE; i++) {
        if (s_cache[i].art == art && s_cache[i].paper == paper) {
            s_cache[i].used = lv_tick_get();
            return &s_cache[i].dsc;
        }
        if (!s_cache[i].art && free_slot < 0) free_slot = i;
        if (s_cache[i].used < s_cache[oldest].used) oldest = i;
    }
    const int slot = free_slot >= 0 ? free_slot : oldest;
    const lv_image_dsc_t *src = art->image;
    const uint32_t w = src->header.w, h = src->header.h;
    const bool clear = paper == YK_YAY_TRANSPARENT;
    const size_t n = (size_t)w * h, bytes = n * (clear ? 3 : 2);
    uint8_t *mem = (uint8_t *)s_cache[slot].dsc.data;
    if (!s_cache[slot].art || s_cache[slot].dsc.data_size != bytes) {
        heap_caps_free(mem);
        mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!mem) {
            s_cache[slot].art = NULL;
            s_cache[slot].dsc.data = NULL;
            ESP_LOGE(TAG, "no PSRAM for a %ux%u pose", (unsigned)w, (unsigned)h);
            return NULL;
        }
    }
    // I8: 256 palette entries of B, G, R, A, then one index a pixel. Opaque: RGB565 over the paper. Transparent:
    // RGB565A8, the colour plane then the alpha plane (LVGL's software renderer draws it, turned and scaled too).
    const uint8_t *idx = src->data + 1024;
    uint16_t rgb[256];
    uint8_t alpha[256];
    yk_yay_palette(src->data, paper, rgb, alpha);
    uint16_t *px = (uint16_t *)mem;
    for (size_t i = 0; i < n; i++) px[i] = rgb[idx[i]];
    if (clear)
        for (size_t i = 0; i < n; i++) mem[2 * n + i] = alpha[idx[i]];
    s_cache[slot].art = art;
    s_cache[slot].paper = paper;
    s_cache[slot].used = lv_tick_get();
    s_cache[slot].dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = clear ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565,
                   .w = w, .h = h, .stride = w * 2},
        .data_size = bytes,
        .data = mem,
    };
    return &s_cache[slot].dsc;
}

static lv_obj_t *rect(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_hidden(o, true);
    return o;
}

/** An art box (image pixels) to the box object's coordinates: scaled, turned 180 when upside down. */
static void place(yk_yay_t *y, lv_obj_t *o, float x, float yy, float w, float h, int radius) {
    float k = y->scale, X = x * k, Y = yy * k, W = w * k, H = h * k;
    if (fabsf(y->rot - 180) < 0.5f) X = y->size - X - W, Y = y->size - Y - H;
    lv_obj_set_pos(o, (int32_t)lroundf(X), (int32_t)lroundf(Y));
    lv_obj_set_size(o, (int32_t)lroundf(W) > 0 ? (int32_t)lroundf(W) : 1, (int32_t)lroundf(H) > 0 ? (int32_t)lroundf(H) : 1);
    lv_obj_set_style_radius(o, (int32_t)lroundf(radius * k), 0);
}

/** The face drawn over the art: eyes (blinking or looking) and the mouth. Only upright or upside down. */
static void face(yk_yay_t *y) {
    const yay_art_t *a = y->art;
    const bool can = a && (fabsf(y->rot) < 0.5f || fabsf(y->rot - 180) < 0.5f);
    const bool eyes = can && a->eyes_open && (y->blinking || y->look_dx || y->look_dy);
    for (int i = 0; i < 2; i++) {
        if (!eyes) {
            lv_obj_set_hidden(y->eye_cover[i], true);
            lv_obj_set_hidden(y->eye[i], true);
            continue;
        }
        const yay_art_box_t *e = i ? &a->eye_right : &a->eye_left;
        const int p = YAY_ART_COVER_PAD;
        lv_obj_set_style_bg_color(y->eye_cover[i], lv_color_hex(a->skin), 0);
        place(y, y->eye_cover[i], e->x - p, e->y - p, e->w + 2 * p, e->h + 2 * p, a->eye_radius + p);
        lv_obj_set_hidden(y->eye_cover[i], false);
        lv_obj_set_style_bg_color(y->eye[i], lv_color_hex(YAY_ART_INK), 0);
        if (y->blinking) {
            const float h = e->h / 5.0f > 2 ? e->h / 5.0f : 2;  // a closed eye: a short thick line
            place(y, y->eye[i], e->x, e->y + e->h * 0.6f, e->w, h, (int)(h / 2));
        } else {
            place(y, y->eye[i], e->x + y->look_dx, e->y + y->look_dy, e->w, e->h, a->eye_radius);
        }
        lv_obj_set_hidden(y->eye[i], false);
    }
    const yk_yay_mouth_t m = y->mouth_from_level ? y->mouth_level : y->mouth_set;
    if (!can || m == YK_YAY_MOUTH_POSE) {
        lv_obj_set_hidden(y->mouth_cover, true);
        lv_obj_set_hidden(y->mouth, true);
        return;
    }
    const yay_art_box_t *mb = &a->mouth;
    const int p = YAY_ART_COVER_PAD;
    lv_obj_set_style_bg_color(y->mouth_cover, lv_color_hex(a->skin), 0);
    place(y, y->mouth_cover, mb->x - p, mb->y - p, mb->w + 2 * p, mb->h + 2 * p, p);
    lv_obj_set_hidden(y->mouth_cover, false);
    const yay_art_size_t sz = a->mouth_levels[m];
    const float mx = mb->x + mb->w / 2.0f - sz.w / 2.0f, my = mb->y + mb->h / 2.0f - sz.h / 2.0f;
    lv_obj_set_style_bg_color(y->mouth, lv_color_hex(YAY_ART_INK), 0);
    place(y, y->mouth, mx, my, sz.w, sz.h, sz.h / 2 + 1);
    lv_obj_set_hidden(y->mouth, false);
}

static void tick(lv_timer_t *t) {
    yk_yay_t *y = lv_timer_get_user_data(t);
    const uint32_t now = lv_tick_get();
    if (y->blinking && now - y->next_blink >= BLINK_MS) {
        y->blinking = false;
        y->next_blink = now + (uint32_t)yk_yay_blink_gap_ms(&y->blink_seed);
        face(y);
    } else if (!y->blinking && y->auto_blink && (int32_t)(now - y->next_blink) >= 0) {
        yk_yay_blink(y);
    }
}

static void layout(yk_yay_t *y) {
    lv_obj_set_size(y->box, y->size, y->size);
    lv_obj_set_pos(y->box, y->cx - y->size / 2, y->cy - y->size / 2);
}

yk_yay_t *yk_yay_create(lv_obj_t *parent, int size, int cx, int cy, uint32_t paper) {
    yk_yay_t *y = calloc(1, sizeof(*y));
    if (!y) return NULL;
    y->size = size, y->cx = cx, y->cy = cy, y->paper = paper;
    y->pose = YK_YAY_POSE_COUNT;
    y->mouth_set = YK_YAY_MOUTH_POSE;
    y->mouth_level = YK_YAY_MOUTH_CLOSED;
    y->box = lv_obj_create(parent);
    lv_obj_remove_style_all(y->box);
    lv_obj_set_clickable(y->box, false);
    lv_obj_set_scrollable(y->box, false);
    layout(y);
    for (int i = 0; i < 2; i++) {
        y->img[i] = lv_image_create(y->box);
        lv_obj_set_hidden(y->img[i], true);
    }
    for (int i = 0; i < 2; i++) y->eye_cover[i] = rect(y->box), y->eye[i] = rect(y->box);
    y->mouth_cover = rect(y->box);
    y->mouth = rect(y->box);
    y->blink_seed = (uint32_t)(uintptr_t)y ^ lv_tick_get();
    y->auto_blink = true;
    y->next_blink = lv_tick_get() + (uint32_t)yk_yay_blink_gap_ms(&y->blink_seed);
    y->timer = lv_timer_create(tick, 50, y);
    yk_yay_pose(y, YK_YAY_READY, 0);
    return y;
}

void yk_yay_delete(yk_yay_t *y) {
    if (!y) return;
    lv_timer_delete(y->timer);
    lv_anim_delete(y->box, NULL);
    lv_anim_delete(y->img[0], NULL);
    lv_anim_delete(y->img[1], NULL);
    lv_obj_delete(y->box);
    free(y);
}

lv_obj_t *yk_yay_obj(yk_yay_t *y) { return y ? y->box : NULL; }

static void fade_exec(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

static void fade_done(lv_anim_t *a) {
    yk_yay_t *y = lv_anim_get_user_data(a);
    lv_obj_set_hidden(y->img[!y->cur], true);
    lv_obj_set_style_opa(y->img[y->cur], LV_OPA_COVER, 0);
}

/** Shows y->art in the current image (scaled to the size, turned). */
static void show(yk_yay_t *y, lv_obj_t *img) {
    const lv_image_dsc_t *d = decoded(y->art, y->paper);
    if (!d) return;
    lv_image_set_src(img, d);
    y->scale = (float)y->size / (float)y->art->size;
    lv_obj_set_size(img, d->header.w, d->header.h);
    lv_obj_set_pos(img, (y->size - (int32_t)d->header.w) / 2, (y->size - (int32_t)d->header.h) / 2);
    lv_image_set_pivot(img, d->header.w / 2, d->header.h / 2);
    lv_image_set_scale(img, (uint32_t)lroundf(256 * y->scale));
    lv_image_set_rotation(img, (int32_t)lroundf(y->rot * 10));
    lv_obj_set_hidden(img, false);
}

static bool art_has_cb(yk_yay_pose_t pose, int size) { return art_for(pose, size) != NULL; }

void yk_yay_pose(yk_yay_t *y, yk_yay_pose_t pose, int fade_ms) {
    if (!y || pose == y->pose) return;
    yk_yay_pose_t use;
    int size;
    if (!yk_yay_pick(pose, y->size, art_has_cb, &use, &size)) return;
    y->pose = pose;
    y->art = art_for(use, size);
    if (fade_ms <= 0 || lv_obj_is_hidden(y->img[y->cur])) {
        lv_anim_delete(y->img[!y->cur], NULL);
        lv_obj_set_hidden(y->img[!y->cur], true);
        lv_obj_set_style_opa(y->img[y->cur], LV_OPA_COVER, 0);
        show(y, y->img[y->cur]);
    } else {
        // the new pose fades in over the old
        y->cur = !y->cur;
        lv_obj_move_to_index(y->img[y->cur], 1);
        lv_obj_set_style_opa(y->img[y->cur], LV_OPA_TRANSP, 0);
        show(y, y->img[y->cur]);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, y->img[y->cur]);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&a, (uint32_t)fade_ms);
        lv_anim_set_exec_cb(&a, fade_exec);
        lv_anim_set_user_data(&a, y);
        lv_anim_set_completed_cb(&a, fade_done);
        lv_anim_start(&a);
    }
    if (!y->art->eyes_open) y->blinking = false;
    face(y);
}

yk_yay_pose_t yk_yay_get_pose(const yk_yay_t *y) { return y ? y->pose : YK_YAY_POSE_COUNT; }

void yk_yay_size(yk_yay_t *y, int size) {
    if (!y || size == y->size) return;
    y->size = size;
    layout(y);
    const yk_yay_pose_t p = y->pose;
    y->pose = YK_YAY_POSE_COUNT;
    yk_yay_pose(y, p, 0);
}

void yk_yay_move(yk_yay_t *y, int cx, int cy) {
    if (!y) return;
    y->cx = cx, y->cy = cy;
    layout(y);
}

void yk_yay_auto_blink(yk_yay_t *y, bool on) {
    if (y) y->auto_blink = on;
}

void yk_yay_blink(yk_yay_t *y) {
    if (!y || !y->art || !y->art->eyes_open) return;
    y->blinking = true;
    y->next_blink = lv_tick_get();  // the blink's start; tick() opens the eyes BLINK_MS later
    face(y);
}

void yk_yay_look_at(yk_yay_t *y, int x, int yy) {
    if (!y || !y->art) return;
    // towards the point, a few art pixels at most
    float dx = (float)(x - y->cx), dy = (float)(yy - y->cy);
    if (fabsf(y->rot - 180) < 0.5f) dx = -dx, dy = -dy;
    const float d = sqrtf(dx * dx + dy * dy);
    const float reach = LOOK_PX * (float)y->art->size / 176.0f;
    y->look_dx = d > 1 ? (int)lroundf(dx / d * reach) : 0;
    y->look_dy = d > 1 ? (int)lroundf(dy / d * reach * 0.6f) : 0;
    face(y);
}

void yk_yay_look_ahead(yk_yay_t *y) {
    if (!y) return;
    y->look_dx = y->look_dy = 0;
    face(y);
}

void yk_yay_mouth(yk_yay_t *y, yk_yay_mouth_t mouth) {
    if (!y) return;
    y->mouth_from_level = false;
    y->mouth_set = mouth;
    face(y);
}

void yk_yay_mouth_level(yk_yay_t *y, uint8_t level) {
    if (!y) return;
    const yk_yay_mouth_t m = yk_yay_mouth_step(y->mouth_level, level);
    if (y->mouth_from_level && m == y->mouth_level) return;
    y->mouth_from_level = true;
    y->mouth_level = m;
    face(y);
}

static void breathe_exec(void *obj, int32_t v) { lv_obj_set_style_translate_y(obj, v, 0); }

void yk_yay_breathe(yk_yay_t *y, bool on) {
    if (!y || on == y->breathing) return;
    y->breathing = on;
    lv_anim_delete(y->box, breathe_exec);
    lv_obj_set_style_translate_y(y->box, 0, 0);
    if (!on) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, y->box);
    lv_anim_set_values(&a, 0, -3);
    lv_anim_set_duration(&a, 2500);
    lv_anim_set_reverse_duration(&a, 2500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, breathe_exec);
    lv_anim_start(&a);
}

static void rotate_exec(void *var, int32_t v) {
    yk_yay_t *y = var;
    y->rot = (float)v / 10.0f;
    lv_image_set_rotation(y->img[0], v);
    lv_image_set_rotation(y->img[1], v);
    face(y);
}

void yk_yay_rotate(yk_yay_t *y, float deg, int ms) {
    if (!y) return;
    deg = fmodf(deg, 360);
    if (deg < 0) deg += 360;
    lv_anim_delete(y, rotate_exec);
    if (ms <= 0) {
        rotate_exec(y, (int32_t)lroundf(deg * 10));
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, y);
    lv_anim_set_values(&a, (int32_t)lroundf(y->rot * 10), (int32_t)lroundf(deg * 10));
    lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, rotate_exec);
    lv_anim_start(&a);
}

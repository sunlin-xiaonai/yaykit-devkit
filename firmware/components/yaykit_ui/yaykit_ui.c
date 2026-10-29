#include "yaykit_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "cjk_font.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "yk_round.h"
#include "yk_ui_priv.h"
#include "yk_ui_logic.h"

static volatile bool capturing;
static bool english;

void yk_ui_set_english(bool on) { english = on; }

bool yk_ui_is_english(void) { return english; }

lv_obj_t *yk_ui_label(lv_obj_t *parent, const char *text, int x, int y, int width, int size, uint32_t color) {
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, width);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    const lv_font_t *font = cjk_font_get(size);
    if (font) lv_obj_set_style_text_font(o, font, 0);
    return o;
}

lv_obj_t *yk_ui_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, lv_event_cb_t cb,
                       void *user_data, uint32_t color) {
    lv_obj_t *o = lv_button_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, user_data);
    yk_ui_label(o, text, 0, (h - 27) / 2, w, 22, color == YK_CORAL ? YK_PAPER : YK_INK);
    return o;
}

static int snapshot_cmd(int argc, char **argv) {
    (void)argc, (void)argv;
    capturing = true;
    board_lock(-1);
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    board_unlock();
    if (!buf) {
        capturing = false;
        puts("SNAP_ERROR out of memory");
        return 1;
    }
    printf("SNAP_BEGIN %u %u %u rgb565\n", (unsigned)buf->header.w, (unsigned)buf->header.h, (unsigned)buf->header.stride);
    // Numbered lines, "SN <seq> <base64>": another task's log line can land between them, and the reader keeps only
    // lines in sequence (an unnumbered line from a log would be decoded as pixels and shift the rest).
    unsigned char out[80];
    size_t total = buf->header.stride * buf->header.h;
    for (size_t off = 0; off < total; off += 57) {
        size_t n = total - off < 57 ? total - off : 57, len;
        mbedtls_base64_encode(out, sizeof(out), &len, buf->data + off, n);
        out[len] = 0;
        printf("SN %u %s\n", (unsigned)(off / 57), (char *)out);
        if ((off / 57) % 256 == 255) vTaskDelay(1);
    }
    puts("SNAP_END");
    lv_draw_buf_destroy(buf);
    capturing = false;
    return 0;
}

void yk_ui_register_snapshot(void) {
    const esp_console_cmd_t cmd = {.command = "snap", .help = "capture the actual LVGL frame", .func = snapshot_cmd};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

bool yk_ui_capturing(void) { return capturing; }

// ---------------------------------------------------------------- v2 helpers (P11)

lv_obj_t *yk_ui_arc_label(lv_obj_t *parent, const char *text, int size, uint32_t color, float angle_deg, int radius,
                          bool ccw) {
    const lv_font_t *font = cjk_font_get(size);
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_clickable(box, false);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_size(box, YK_CX * 2, YK_CY * 2);
    lv_obj_set_pos(box, 0, 0);
    if (!font || !text) return box;
    // The characters' widths along the circle, centred on angle_deg.
    int widths[16], n = 0, total = 0, len;
    uint32_t cps[16];
    for (const char *p = text; *p && n < 16; p += len) {
        cps[n] = yk_utf8_next(p, &len);
        widths[n] = lv_font_get_glyph_width(font, cps[n], 0);
        total += widths[n++];
    }
    const float rad_per_px = 1.0f / (float)radius, dir = ccw ? -1.0f : 1.0f;
    float at = angle_deg * 3.14159265f / 180.0f - dir * total * rad_per_px / 2;
    // ccw (the bottom half): the baseline is on the circle, the characters' tops towards the centre.
    const int r_mid = ccw ? radius + size / 2 : radius - size / 2;
    for (int i = 0; i < n; i++) {
        const float mid = at + dir * widths[i] * rad_per_px / 2;
        char ch[5] = {0};
        // the code point back to UTF-8, one label each
        const uint32_t c = cps[i];
        if (c < 0x80) ch[0] = (char)c;
        else if (c < 0x800) ch[0] = (char)(0xC0 | (c >> 6)), ch[1] = (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000)
            ch[0] = (char)(0xE0 | (c >> 12)), ch[1] = (char)(0x80 | ((c >> 6) & 0x3F)), ch[2] = (char)(0x80 | (c & 0x3F));
        else
            ch[0] = (char)(0xF0 | (c >> 18)), ch[1] = (char)(0x80 | ((c >> 12) & 0x3F)),
            ch[2] = (char)(0x80 | ((c >> 6) & 0x3F)), ch[3] = (char)(0x80 | (c & 0x3F));
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, ch);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        const int w = widths[i] > 0 ? widths[i] : 1, h = lv_font_get_line_height(font);
        lv_obj_set_size(l, w, h);
        const float x = YK_CX + r_mid * sinf(mid), y = YK_CY - r_mid * cosf(mid);
        lv_obj_set_pos(l, (int32_t)lroundf(x - w / 2.0f), (int32_t)lroundf(y - h / 2.0f));
        lv_obj_set_style_transform_pivot_x(l, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(l, h / 2, 0);
        lv_obj_set_style_transform_rotation(l, (int32_t)lroundf((mid * 180.0f / 3.14159265f + (ccw ? 180 : 0)) * 10), 0);
        at += dir * w * rad_per_px;
    }
    return box;
}

lv_obj_t *yk_ui_turn_box(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_size(box, YK_CX * 2, YK_CY * 2);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_style_transform_pivot_x(box, YK_CX, 0);
    lv_obj_set_style_transform_pivot_y(box, YK_CY, 0);
    return box;
}

static void turn_exec(void *obj, int32_t v) { lv_obj_set_style_transform_rotation(obj, v, 0); }

void yk_ui_turn(lv_obj_t *box, float deg, int ms) {
    lv_anim_delete(box, turn_exec);
    const int32_t to = (int32_t)lroundf(deg * 10);
    if (ms <= 0) {
        turn_exec(box, to);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, box);
    lv_anim_set_values(&a, lv_obj_get_style_transform_rotation(box, 0), to);
    lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, turn_exec);
    lv_anim_start(&a);
}

#define SHEET_H 210

lv_obj_t *yk_ui_status_sheet(lv_obj_t *parent) {
    lv_obj_t *sheet = lv_obj_create(parent);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_scrollable(sheet, false);
    lv_obj_set_size(sheet, YK_CX * 2, SHEET_H);
    lv_obj_set_pos(sheet, 0, -SHEET_H);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(YK_PANEL), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_hidden(sheet, true);
    // four lines, filled by yk_ui_status_show(): clock; network; battery and volume; points
    for (int i = 0; i < 4; i++) yk_ui_label(sheet, "", 33, 52 + i * 34, 400, i ? YK_SIZE_STATUS : YK_SIZE_HEADING, i ? YK_INK2 : YK_INK);
    return sheet;
}

static void sheet_y(void *obj, int32_t v) { lv_obj_set_y(obj, v); }

static void sheet_hidden(lv_anim_t *a) {
    if (lv_obj_get_y(a->var) <= -SHEET_H) lv_obj_set_hidden(a->var, true);
}

void yk_ui_status_show(lv_obj_t *sheet, const yk_status_t *st, bool show) {
    if (!sheet) return;
    if (st) {
        char line[64];
        lv_obj_t *l;
        int k = 0;
        if ((l = lv_obj_get_child(sheet, k++))) lv_label_set_text(l, st->clock ? st->clock : "");
        if ((l = lv_obj_get_child(sheet, k++))) {
            static const char *const zh[] = {"Wi-Fi 关", "正在连", "已连网"}, *const en[] = {"Wi-Fi off", "Connecting", "Online"};
            lv_label_set_text(l, st->wifi >= 0 && st->wifi <= 2 ? (english ? en : zh)[st->wifi] : "");
        }
        if ((l = lv_obj_get_child(sheet, k++))) {
            line[0] = 0;
            if (st->battery >= 0)
                snprintf(line, sizeof(line), english ? "Battery %d%%%s" : "电量 %d%%%s", st->battery,
                         st->charging ? (english ? ", charging" : "，在充电") : "");
            if (st->volume >= 0)
                snprintf(line + strlen(line), sizeof(line) - strlen(line), english ? "%sVolume %d" : "%s音量 %d",
                         line[0] ? "  ·  " : "", st->volume);
            lv_label_set_text(l, line);
        }
        if ((l = lv_obj_get_child(sheet, k++))) {
            line[0] = 0;
            if (st->credits >= 0) snprintf(line, sizeof(line), english ? "%d points left today" : "今天还有 %d 点", st->credits);
            lv_label_set_text(l, line);
        }
    }
    lv_anim_delete(sheet, sheet_y);
    if (show) lv_obj_set_hidden(sheet, false);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sheet);
    lv_anim_set_values(&a, lv_obj_get_y(sheet), show ? 0 : -SHEET_H);
    lv_anim_set_duration(&a, YK_PAGE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, sheet_y);
    lv_anim_set_completed_cb(&a, sheet_hidden);
    lv_anim_start(&a);
}

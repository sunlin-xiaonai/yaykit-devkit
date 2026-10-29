#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "yk_theme.h"
// v2 (extension platform P11): the round-screen spec's pieces, each in its own header.
#include "yk_caption.h"     // subtitles laid out by the circle
#include "yk_notice.h"      // the same notices everywhere
#include "yk_rim.h"         // the rim, eight boxes
#include "yk_round.h"       // the spec as constants: colours, sizes, geometry
#include "yk_transition.h"  // the splash, into and out of an app
#include "yk_yay.h"         // Yay, alive
// P23/P24 (device-controls.md): the double click's app ring, the catalog it draws from, the arrow to the side key.
#include "yk_apps.h"        // the app ring
#include "yk_catalog.h"     // the app catalog cache (names, icons, sizes)
#include "yk_hint.h"        // the hint arrow and when it shows

#ifdef __cplusplus
extern "C" {
#endif

/** Centered CJK label (fonts from the shared `fonts` partition). */
lv_obj_t *yk_ui_label(lv_obj_t *parent, const char *text, int x, int y, int width, int size, uint32_t color);
/** Pill button with a centered label. */
lv_obj_t *yk_ui_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h, lv_event_cb_t cb,
                       void *user_data, uint32_t color);
/**
 * Draws the pairing content on `screen` below a title: QR code, 8-digit code and status
 * while BLE is on, or a button to turn it on. Caller owns the title and back button.
 * Only this function needs Yaykit Link's Bluetooth (yk_link_init with BLE) and CONFIG_LV_USE_QRCODE (without it: the
 * code alone); firmware that never calls it links neither.
 */
void yk_ui_pair_content(lv_obj_t *screen, bool starting, lv_event_cb_t on_turn_on, lv_event_cb_t on_turn_off);
/** English text in the shared pages (default Chinese). Call with the board lock held, before drawing. */
void yk_ui_set_english(bool on);
/** Registers `snap`: prints the real LVGL frame as base64 RGB565 between SNAP_BEGIN/SNAP_END. */
void yk_ui_register_snapshot(void);
/** True while `snap` is streaming, so periodic banners can stay quiet. */
bool yk_ui_capturing(void);

// ---------------------------------------------------------------- v2 helpers (P11)

/** A short label (at most 8 characters) along the circle, centred at `angle_deg` (clockwise from the top) with its
 *  baseline on `radius`, reading clockwise (or counter-clockwise when `ccw`, for the bottom half to read upright).
 *  One small label per character: for short labels only (spec 2). */
lv_obj_t *yk_ui_arc_label(lv_obj_t *parent, const char *text, int size, uint32_t color, float angle_deg, int radius,
                          bool ccw);

/** A full-screen container whose content turns about the screen's centre (Translator's far half, Party Play's
 *  results facing a player). Its children are laid out unturned in 466 x 466; yk_ui_turn() turns them (ms 0: at
 *  once; 300 is the spec's). Turning redraws the box's content area: keep what is inside small. */
lv_obj_t *yk_ui_turn_box(lv_obj_t *parent);
void yk_ui_turn(lv_obj_t *box, float deg, int ms);

/** The pull-down status sheet (spec 6): what the app shows of the device. -1 / NULL hides an item (Party Play
 *  shows only the battery and the volume; nothing about the network in an offline app). */
typedef struct {
    const char *clock;  // "12:30"
    int wifi;           // -1 hidden, 0 off, 1 connecting, 2 connected
    int battery;        // percent, -1 hidden
    bool charging;
    int volume;         // 0..10, -1 hidden
    int credits;        // today's points left, -1 hidden
} yk_status_t;
/** Creates the sheet in `parent` (hidden). */
lv_obj_t *yk_ui_status_sheet(lv_obj_t *parent);
/** Fills it and slides it down (show) or up (hide) over YK_PAGE_MS. */
void yk_ui_status_show(lv_obj_t *sheet, const yk_status_t *st, bool show);

#ifdef __cplusplus
}
#endif
